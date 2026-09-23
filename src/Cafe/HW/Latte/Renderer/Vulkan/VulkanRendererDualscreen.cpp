#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

#include "DualscreenSession.h"
#include "util/helpers/helpers.h"

#include <chrono>

// Handing the Wii U's two screens to Dualscreen, which puts them on two
// machines. See src/dualscreen/README.md for what is on the other end.
//
// This is deliberately not Cemu's screenshot path, which sits at the same call
// site and does the same copy. That one submits the command buffer and then
// waits on it, because a screenshot happens when somebody presses F12 and a
// stall nobody notices is the right trade. This happens sixty times a second,
// where the same stall would be most of the frame.
//
// So the copy is recorded into whichever command buffer is already being
// filled, and the bytes are read on a later frame, once that buffer has
// finished for its own reasons. Nothing here ever waits. A frame that is not
// ready yet is simply not sent, and a slot still in flight when its turn comes
// round again means this end is behind -- so that frame is dropped, which is
// what the shell expects: it keeps showing the last one it was given.
//
// That covers the GPU side. The CPU side of a readback -- DualscreenDrain(),
// unpacking whatever format the source was in -- turned out to have the same
// problem for the opposite reason: on a discrete GPU, reading this buffer
// from the CPU at all costs real time (measured upward of half a millisecond
// a call), where it is close to free on an APU sharing memory with its GPU.
// A title needing a real per-pixel unpack (packed_a2b10g10r10 -- Wind Waker
// HD among them) used to pay that on every frame -- measured on a discrete
// GPU as an entire CPU core kept continuously busy, which is well past "a
// slow readback" and squarely a reason Cemu's real-time audio thread could
// end up starved of its own share of the machine. HandleDualscreenFrame now
// does that conversion on the GPU instead, with a blit into
// m_dualscreenConvert (see its own comment) immediately before the readback
// copy, whenever the device supports it -- which on any GPU worth calling
// discrete, it does. m_dualscreenThread and its CPU-side unpack are still
// here as the fallback for whatever it does not: an unsupported device, or
// this title's screens arriving in some format the blit path itself does
// not handle. A slow readback there still only costs Dualscreen a frame of
// latency, the same as the ring buffer already tolerates, rather than
// costing Cemu a frame of pacing.

namespace
{
	// Temporary instrumentation, kept across the move to a background
	// thread to confirm it actually worked: HandleDualscreenFrame ("tap")
	// should now cost close to nothing on the GPU thread, with the readback
	// cost showing up in PixelConvert on m_dualscreenThread instead, where a
	// slow moment no longer competes with the game for the same thread.
	// Remove once that's confirmed.
	struct ScopedCallTiming
	{
		const char* label;
		std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
		std::chrono::steady_clock::time_point& windowStart;
		double& totalMs;
		int& calls;
		ScopedCallTiming(const char* label_, std::chrono::steady_clock::time_point& windowStart_,
						 double& totalMs_, int& calls_)
			: label(label_), windowStart(windowStart_), totalMs(totalMs_), calls(calls_)
		{
		}
		~ScopedCallTiming()
		{
			const auto now = std::chrono::steady_clock::now();
			totalMs += std::chrono::duration<double, std::milli>(now - started).count();
			++calls;
			const double windowSeconds = std::chrono::duration<double>(now - windowStart).count();
			if (windowSeconds >= 1.0)
			{
				cemuLog_log(LogType::Force,
							"Dualscreen: {} cost {:.1f}ms over {} calls in the last {:.2f}s", label,
							totalMs, calls, windowSeconds);
				windowStart = now;
				totalMs = 0.0;
				calls = 0;
			}
		}
	};
}

void VulkanRenderer::DualscreenReleaseBuffers()
{
	// m_dualscreenThread reads a slot's mapped memory between it being marked
	// pending and it clearing that flag again, with no lock around the
	// memory itself -- only around the flag. Unmapping out from under a read
	// still in flight is a real hazard, so this waits for every slot to be
	// clear first. Normally instant: called either with the thread already
	// joined (renderer teardown) or with a session that has already stopped,
	// which is what stops anything from setting a slot pending again.
	// Bounded all the same, since "normally" is not "always".
	for (int guard = 0; guard < 1000; ++guard)
	{
		bool anyPending = false;
		for (auto& ring : m_dualscreenReadback)
			for (auto& slot : ring)
				anyPending = anyPending || slot.pending.load(std::memory_order_acquire);
		if (!anyPending)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	for (auto& ring : m_dualscreenReadback)
	{
		for (auto& slot : ring)
		{
			if (slot.mapped)
			{
				vkUnmapMemory(m_logicalDevice, slot.memory);
				slot.mapped = nullptr;
			}
			if (slot.buffer != VK_NULL_HANDLE)
				memoryManager->DeleteBuffer(slot.buffer, slot.memory);
			slot.buffer = VK_NULL_HANDLE;
			slot.memory = VK_NULL_HANDLE;
			slot.capacity = 0;
			slot.width = 0;
			slot.height = 0;
			slot.srgb = false;
			slot.packed_a2b10g10r10 = false;
			slot.commandBufferId = 0;
			slot.serial = 0;
			slot.pending.store(false, std::memory_order_relaxed);
		}
	}
	m_dualscreenNextSlot = {0, 0};
	m_dualscreenHasBuffers = false;

	// Nothing else touches these -- unlike the readback buffers above, no
	// wait is needed before freeing them.
	for (auto& convert : m_dualscreenConvert)
	{
		if (convert.image != VK_NULL_HANDLE)
			vkDestroyImage(m_logicalDevice, convert.image, nullptr);
		if (convert.memory != VK_NULL_HANDLE)
			vkFreeMemory(m_logicalDevice, convert.memory, nullptr);
		convert = {};
	}
}

bool VulkanRenderer::DualscreenBlitSupported()
{
	if (m_dualscreenBlitSupported.has_value())
		return *m_dualscreenBlitSupported;

	// Everything this needs blit conversion between: the packed format some
	// titles' scan buffers use as a blit source, and both the UNORM and SRGB
	// variants of RGBA8 as either side of a gamma-only conversion. Checked
	// once and cached rather than every frame -- what a device supports does
	// not change mid-session.
	auto supportsSrc = [this](VkFormat format) {
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
		return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0;
	};
	auto supportsDst = [this](VkFormat format) {
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
		return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
	};
	const bool supported = supportsSrc(VK_FORMAT_A2B10G10R10_UNORM_PACK32) &&
						   supportsSrc(VK_FORMAT_R8G8B8A8_UNORM) && supportsDst(VK_FORMAT_R8G8B8A8_UNORM) &&
						   supportsSrc(VK_FORMAT_R8G8B8A8_SRGB) && supportsDst(VK_FORMAT_R8G8B8A8_SRGB);
	m_dualscreenBlitSupported = supported;
	if (!supported)
		cemuLog_log(LogType::Force,
					"Dualscreen: this device cannot blit-convert a packed or gamma-mismatched "
					"scan buffer, so that conversion will cost CPU time instead of GPU time");
	return supported;
}

bool VulkanRenderer::DualscreenLinearBlitSupported()
{
	if (m_dualscreenLinearBlitSupported.has_value())
		return *m_dualscreenLinearBlitSupported;

	// vkCmdBlitImage with VK_FILTER_LINEAR requires the *source* format to
	// support filtering, which is a separate feature bit from being blittable
	// at all. Same three source formats DualscreenBlitSupported() checks, and
	// cached the same way for the same reason.
	auto supportsLinear = [this](VkFormat format) {
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &props);
		return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
	};
	const bool supported = supportsLinear(VK_FORMAT_A2B10G10R10_UNORM_PACK32) &&
						   supportsLinear(VK_FORMAT_R8G8B8A8_UNORM) &&
						   supportsLinear(VK_FORMAT_R8G8B8A8_SRGB);
	m_dualscreenLinearBlitSupported = supported;
	if (!supported)
		cemuLog_log(LogType::Force,
					"Dualscreen: this device cannot filter while blitting, so screens are handed "
					"over at the size they were rendered and reduced on the other end instead");
	return supported;
}

bool VulkanRenderer::EnsureDualscreenConvertImage(size_t screenIndex, int width, int height, bool srgb)
{
	auto& convert = m_dualscreenConvert[screenIndex];
	if (convert.image != VK_NULL_HANDLE && convert.width == width && convert.height == height &&
		convert.srgb == srgb)
		return true;

	if (convert.image != VK_NULL_HANDLE)
		vkDestroyImage(m_logicalDevice, convert.image, nullptr);
	if (convert.memory != VK_NULL_HANDLE)
		vkFreeMemory(m_logicalDevice, convert.memory, nullptr);
	convert = {};

	VkImageCreateInfo imageInfo{};
	imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageInfo.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
	imageInfo.extent = {(uint32)width, (uint32)height, 1};
	imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
	imageInfo.arrayLayers = 1;
	imageInfo.mipLevels = 1;
	imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	imageInfo.imageType = VK_IMAGE_TYPE_2D;
	imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;

	if (vkCreateImage(m_logicalDevice, &imageInfo, nullptr, &convert.image) != VK_SUCCESS)
	{
		convert = {};
		return false;
	}

	VkMemoryRequirements memRequirements;
	vkGetImageMemoryRequirements(m_logicalDevice, convert.image, &memRequirements);

	VkMemoryAllocateInfo allocInfo{};
	allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocInfo.allocationSize = memRequirements.size;
	uint32 memIndex;
	if (!memoryManager->FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
									   memIndex))
	{
		vkDestroyImage(m_logicalDevice, convert.image, nullptr);
		convert = {};
		return false;
	}
	allocInfo.memoryTypeIndex = memIndex;

	if (vkAllocateMemory(m_logicalDevice, &allocInfo, nullptr, &convert.memory) != VK_SUCCESS)
	{
		vkDestroyImage(m_logicalDevice, convert.image, nullptr);
		convert = {};
		return false;
	}

	vkBindImageMemory(m_logicalDevice, convert.image, convert.memory, 0);
	convert.width = width;
	convert.height = height;
	convert.srgb = srgb;
	convert.everBlitted = false;
	return true;
}

void VulkanRenderer::DualscreenDrain(size_t screenIndex)
{
	auto& ring = m_dualscreenReadback[screenIndex];
	const ds::ScreenId screen = (screenIndex == 0) ? ds::ScreenId::Top : ds::ScreenId::Bottom;
	// Oldest first, starting where the next capture will go: slots are filled
	// in ring order, so their command buffers finish in that order too. The
	// first one that has not finished means none after it has either.
	const bool dstUsesSRGB =
		(screenIndex == 0) ? LatteGPUState.tvBufferUsesSRGB : LatteGPUState.drcBufferUsesSRGB;
	for (size_t step = 0; step < ring.size(); ++step)
	{
		auto& slot = ring[(m_dualscreenNextSlot[screenIndex] + step) % ring.size()];
		if (!slot.pending.load(std::memory_order_acquire))
			continue;
		if (!HasCommandBufferFinished(slot.commandBufferId))
			break;

		const size_t count = (size_t)slot.width * slot.height;
		// The gamma question is decided the same way Cemu's own output shader
		// decides it (RendererOuputShader.cpp: outputShader() samples the raw
		// texture and only conditionally applies sRGBEncode -- there is no
		// decode the other way, since nothing here is ever sampled through an
		// sRGB-tagged view). slot.srgb records whether these particular bytes
		// were already gamma-encoded before this readback saw them: true for a
		// source stored in an _SRGB-tagged format, false otherwise, packed
		// 10:10:10:2 included, since Vulkan has no sRGB-tagged variant of it
		// for a sampler to have auto-decoded.
		const bool toRGB = slot.srgb && !dstUsesSRGB;
		const bool toSRGB = !slot.srgb && dstUsesSRGB;
		// Byte order R,G,B,0xff throughout -- what a real GPU-filled RGBA8
		// texture already is, and what Dualscreen now expects a Wii U's
		// screens to arrive as (see external_screen_source.cpp's
		// upload_rgba8888()). The two conversions below still need doing
		// (10-bit unpacking, gamma), but a title that needs neither -- the
		// overwhelming common case -- no longer pays for a CPU pass over
		// every pixel at all: measured at ~700ms of *every second* on a
		// discrete GPU, where reading this readback buffer from the CPU
		// turned out to be far slower than from an APU's shared memory.
		// slot.mapped is already in the wire format then, so it goes
		// straight to PublishScreen with no intermediate copy.
		const uint32* publishFrom;
		if (!slot.packed_a2b10g10r10 && !toRGB && !toSRGB)
		{
			publishFrom = (const uint32*)slot.mapped;
		}
		else
		{
			static std::chrono::steady_clock::time_point s_pixelWindowStart = std::chrono::steady_clock::now();
			static double s_pixelTotalMs = 0.0;
			static int s_pixelCalls = 0;
			ScopedCallTiming pixelTiming("PixelConvert", s_pixelWindowStart, s_pixelTotalMs, s_pixelCalls);
			m_dualscreenPixels.resize(count);
			if (slot.packed_a2b10g10r10)
			{
				const uint32* source = (const uint32*)slot.mapped;
				for (size_t index = 0; index < count; ++index)
				{
					const uint32 packed = source[index];
					uint8 r = (uint8)((packed & 0x3FFu) >> 2);
					uint8 g = (uint8)(((packed >> 10) & 0x3FFu) >> 2);
					uint8 b = (uint8)(((packed >> 20) & 0x3FFu) >> 2);
					if (toSRGB)
					{
						r = RGBComponentToSRGB(r);
						g = RGBComponentToSRGB(g);
						b = RGBComponentToSRGB(b);
					}
					m_dualscreenPixels[index] =
						(uint32)r | ((uint32)g << 8) | ((uint32)b << 16) | (0xFFu << 24);
				}
			}
			else
			{
				const uint8* source = (const uint8*)slot.mapped;
				for (size_t index = 0; index < count; ++index)
				{
					uint8 r = source[index * 4 + 0];
					uint8 g = source[index * 4 + 1];
					uint8 b = source[index * 4 + 2];
					if (toRGB)
					{
						r = SRGBComponentToRGB(r);
						g = SRGBComponentToRGB(g);
						b = SRGBComponentToRGB(b);
					}
					else if (toSRGB)
					{
						r = RGBComponentToSRGB(r);
						g = RGBComponentToSRGB(g);
						b = RGBComponentToSRGB(b);
					}
					m_dualscreenPixels[index] =
						(uint32)r | ((uint32)g << 8) | ((uint32)b << 16) | (0xFFu << 24);
				}
			}
			publishFrom = m_dualscreenPixels.data();
		}
		DualscreenSession::instance().PublishScreen(screen, slot.serial, publishFrom, slot.width,
													slot.height);
		// Release, pairing with the acquire load the GPU thread does before
		// it reuses this slot: everything this call touched above must be
		// visibly done before that thread can see the slot as free again.
		slot.pending.store(false, std::memory_order_release);
	}
}

void VulkanRenderer::DualscreenThreadMain()
{
	SetThreadName("DualscreenConv");
	while (!m_dualscreenThreadShutdown.load(std::memory_order_acquire))
	{
		if (!DualscreenSession::instance().IsRunning())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			continue;
		}
		DualscreenDrain(0);
		DualscreenDrain(1);
		// A short, fixed poll rather than anything tied to Cemu's own frame
		// pacing -- this thread does not know that pacing and should not
		// need to. A slot's own command-buffer-finished check inside
		// DualscreenDrain is what actually decides whether there was
		// anything to do this pass.
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void VulkanRenderer::HandleDualscreenFrame(LatteTextureView* texView, bool padView)
{
	auto& session = DualscreenSession::instance();
	if (!session.IsRunning())
	{
		// Nothing is listening, or the shell has gone. Anything still mapped
		// belonged to a session that is over.
		if (m_dualscreenHasBuffers)
			DualscreenReleaseBuffers();
		return;
	}

	static std::chrono::steady_clock::time_point s_tapWindowStart = std::chrono::steady_clock::now();
	static double s_tapTotalMs = 0.0;
	static int s_tapCalls = 0;
	ScopedCallTiming tapTiming("tap", s_tapWindowStart, s_tapTotalMs, s_tapCalls);

	const size_t screenIndex = padView ? 1 : 0;
	// Draining -- reading whatever finished since last time -- is
	// m_dualscreenThread's job now, off this thread entirely. All this does
	// is start the next capture, which is cheap: recording a copy and a
	// couple of barriers, never waiting on anything.

	auto texViewVk = (LatteTextureViewVk*)texView;
	if (texViewVk->firstMip != 0)
		return; // the same thing the screenshot path refuses, for the same reason

	auto baseImageTex = texViewVk->GetBaseImage();
	sint32 width = 0;
	sint32 height = 0;
	baseImageTex->GetEffectiveSize(width, height, 0);
	if (width <= 0 || height <= 0)
		return;

	const auto format = baseImageTex->GetFormat();
	const bool isRGBA8 = (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB);
	// Some titles (Wind Waker HD among them) render through a 10-bit-per-channel
	// scan buffer rather than RGBA8. Same four bytes per pixel, so it needs no
	// extra machinery here -- only DualscreenDrain reads the bits differently.
	const bool isPacked1010102 = (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
	if (!isRGBA8 && !isPacked1010102)
	{
		// Anything else would need the blit-to-a-temporary-image dance the
		// screenshot path does, which is a lot of machinery for a case that has
		// not come up yet. Said once, because if it does come up it is a
		// property of the title rather than of a frame.
		if (!m_dualscreenFormatWarned)
		{
			m_dualscreenFormatWarned = true;
			cemuLog_log(LogType::Force,
						"Dualscreen: this title's screens are in a format the handover does not "
						"convert yet (Vulkan format {}), so they are not being sent",
						(int)format);
		}
		return;
	}

	// How big this screen is actually worth handing over: whatever the shell
	// said its own display can show, never larger than what was rendered.
	// A graphics pack rendering at 4K into a 1280x800 handheld is sixteen
	// times the pixels, and every one of them would otherwise be read out of
	// video memory, copied through shared memory, and then scaled back down
	// on the shell's CPU. The blit below already resizes for nothing; the
	// only reason it did not until now is that nobody told this end what
	// size to aim for. See DualscreenSession::CaptureSize().
	sint32 captureWidth = width;
	sint32 captureHeight = height;
	{
		int fittedWidth = width;
		int fittedHeight = height;
		session.CaptureSize(width, height, fittedWidth, fittedHeight);
		captureWidth = (sint32)fittedWidth;
		captureHeight = (sint32)fittedHeight;
	}
	// Only worth claiming when the blit can actually filter -- reducing with
	// NEAREST would alias so badly that handing the whole picture over and
	// letting the shell scale it properly is the better trade.
	const bool needsResize = (captureWidth != width || captureHeight != height) &&
							 DualscreenLinearBlitSupported();
	if (!needsResize)
	{
		// Back to the rendered size, so that a blit taken for one of the
		// conversion reasons below does not quietly resize with NEAREST after
		// the filtered path was ruled out.
		captureWidth = width;
		captureHeight = height;
	}

	// Whether this frame needs converting before DualscreenDrain can hand it
	// to PublishScreen as-is, and if so whether that conversion happens here
	// on the GPU or is left to DualscreenDrain's own CPU-side unpack -- see
	// m_dualscreenConvert's own comment for why the GPU path exists at all.
	// Two things can require it: a packed scan buffer (isPacked1010102), or
	// this screen's current gamma setting no longer matching what this
	// texture is tagged as -- DualscreenDrain already handles the latter on
	// its own for whatever this leaves unconverted, so a device that cannot
	// blit still gets a correct picture, just not the free one.
	const bool sourceIsSRGB = (format == VK_FORMAT_R8G8B8A8_SRGB);
	const bool dstUsesSRGB = padView ? LatteGPUState.drcBufferUsesSRGB : LatteGPUState.tvBufferUsesSRGB;
	const bool needsConversion = isPacked1010102 || (sourceIsSRGB != dstUsesSRGB);
	// Resizing is the second reason to go through the blit, and unlike the
	// conversions above it is not something DualscreenDrain can fall back to
	// doing on the CPU -- a device that cannot blit simply hands the whole
	// rendered picture over and lets the shell scale it, exactly as before.
	const bool useBlit = (needsConversion || needsResize) && DualscreenBlitSupported() &&
						 EnsureDualscreenConvertImage(screenIndex, captureWidth, captureHeight,
													  dstUsesSRGB);
	// What actually ends up in the buffer: the reduced size only if the blit
	// that would produce it is really happening.
	const sint32 outWidth = useBlit ? captureWidth : width;
	const sint32 outHeight = useBlit ? captureHeight : height;

	auto& slot = m_dualscreenReadback[screenIndex][m_dualscreenNextSlot[screenIndex]];
	if (slot.pending.load(std::memory_order_acquire))
		return; // still in flight: this end is behind, so this frame is dropped

	// Sized for what the copy will actually write, which is the reduced
	// picture only when the blit that produces it is really happening.
	const size_t needed = (size_t)outWidth * outHeight * 4;
	if (slot.capacity < needed)
	{
		if (slot.mapped)
		{
			vkUnmapMemory(m_logicalDevice, slot.memory);
			slot.mapped = nullptr;
		}
		if (slot.buffer != VK_NULL_HANDLE)
			memoryManager->DeleteBuffer(slot.buffer, slot.memory);
		slot.capacity = 0;
		if (!memoryManager->CreateBuffer(needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
										 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
											 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
											 VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
										 slot.buffer, slot.memory))
		{
			cemuLog_log(LogType::Force, "Dualscreen: could not allocate a readback buffer");
			return;
		}
		vkMapMemory(m_logicalDevice, slot.memory, 0, VK_WHOLE_SIZE, 0, &slot.mapped);
		slot.capacity = needed;
		m_dualscreenHasBuffers = true;
	}

	// Out of whatever render pass the game has open, before anything below is
	// recorded. vkCmdCopyImageToBuffer and vkCmdBlitImage are not allowed
	// inside one, and neither is a layout transition on an image that may be
	// one of its attachments -- which the screen being handed over often is,
	// since a game can render straight into its scan buffer. Every copy Cemu
	// records itself starts with this line (texture_copyImageSubData,
	// DrawBackbufferQuad); this one did not, because it runs *before* the
	// backbuffer copy that would have ended the pass, and on the screen this
	// machine is not showing there is no backbuffer copy at all.
	//
	// Nothing checks for this: a driver records the commands anyway, into a
	// command buffer that is then in no valid state, and what goes wrong
	// after that is anybody's guess.
	//
	// (This was first put in as the fix for Nintendo Land crashing on
	// starting Luigi's Mansion and Takamaru's Ninja Castle. It was not: the
	// same crash happened with no shell attached at all, which rules this
	// code out. That one was upstream's -- see the texture copy fixes picked
	// from upstream alongside it. This stays because the rule above holds
	// regardless.)
	draw_endRenderPass();

	auto textureVk = baseImageTex->GetImageObj();
	textureVk->flagForCurrentCommandBuffer();

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	// Tightly packed, which is what makes the stride the width on the far side.
	region.bufferRowLength = outWidth;
	region.bufferImageHeight = outHeight;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	// m_dualscreenConvert is always a fresh single-layer image (see its own
	// comment), unlike the source texture this screen's slices can come from.
	region.imageSubresource.baseArrayLayer = useBlit ? 0 : texViewVk->firstSlice;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = 0;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {(uint32)outWidth, (uint32)outHeight, 1};

	if (useBlit)
	{
		auto& convert = m_dualscreenConvert[screenIndex];

		VkImageSubresourceLayers srcRange{};
		srcRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		srcRange.mipLevel = 0;
		srcRange.baseArrayLayer = texViewVk->firstSlice;
		srcRange.layerCount = 1;
		// Same reasoning as the non-blit path's own barrier below, just
		// feeding the blit instead of the buffer copy.
		barrier_image<IMAGE_WRITE | TRANSFER_WRITE, TRANSFER_READ>(baseImageTex, srcRange,
																	VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		VkImageSubresourceRange dstRange{};
		dstRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		dstRange.baseMipLevel = 0;
		dstRange.levelCount = 1;
		dstRange.baseArrayLayer = 0;
		dstRange.layerCount = 1;
		// UNDEFINED the very first time this image is used, TRANSFER_SRC_OPTIMAL
		// every time after -- what the end of this same block always leaves it in.
		barrier_image<TRANSFER_READ, TRANSFER_WRITE>(
			convert.image, dstRange,
			convert.everBlitted ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkImageBlit blitRegion{};
		blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.srcSubresource.mipLevel = 0;
		blitRegion.srcSubresource.baseArrayLayer = texViewVk->firstSlice;
		blitRegion.srcSubresource.layerCount = 1;
		blitRegion.srcOffsets[1] = {width, height, 1};
		blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blitRegion.dstSubresource.mipLevel = 0;
		blitRegion.dstSubresource.baseArrayLayer = 0;
		blitRegion.dstSubresource.layerCount = 1;
		blitRegion.dstOffsets[1] = {captureWidth, captureHeight, 1};
		// LINEAR only when the two sizes actually differ. A same-size blit is
		// a format conversion with nothing for a filter to do, and NEAREST
		// there is both free and exactly right; a reduction with NEAREST
		// would alias badly, which is the one thing a downscale must not do.
		vkCmdBlitImage(m_state.currentCommandBuffer, textureVk->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   convert.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blitRegion,
					   needsResize ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
		convert.everBlitted = true;

		// Source back to its resting layout -- the blit above was its only use
		// this frame, same as the non-blit path's copy below.
		barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(baseImageTex, srcRange,
																	baseImageTex->GetDefaultLayout());
		// Destination to TRANSFER_SRC_OPTIMAL for the readback copy just below
		// -- and what it stays in afterward, ready for next frame's blit above
		// without another UNDEFINED-rooted transition.
		barrier_image<TRANSFER_WRITE, TRANSFER_READ>(convert.image, dstRange,
													 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
													 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, convert.image,
							   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &region);
	}
	else
	{
		// TRANSFER_SRC_OPTIMAL, not GENERAL: this texture's resting layout
		// (GetDefaultLayout(), below) is already the least-constrained one Cemu
		// has -- GENERAL itself, or VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT
		// on hardware that supports it, chosen there because a Wii U render
		// target moves between being written and being sampled with no layout
		// transition Cemu wants to pay for on every such switch. Sending it
		// through GENERAL for a plain read-only copy source is a step backwards
		// from either: GENERAL disables the compression a real render target
		// otherwise keeps, and the feedback-loop layout is already sample-safe
		// without it. TextureReadbackVk.cpp's readback -- Cemu's own screenshot
		// path, doing the same copy -- already uses TRANSFER_SRC_OPTIMAL for
		// exactly this reason.
		barrier_image<IMAGE_WRITE | TRANSFER_WRITE, TRANSFER_READ>(baseImageTex, region.imageSubresource,
																   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, textureVk->m_image,
							   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer, 1, &region);
		barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(baseImageTex,
																   region.imageSubresource,
																   baseImageTex->GetDefaultLayout());
	}

	slot.width = outWidth;
	slot.height = outHeight;
	if (useBlit)
	{
		// Already converted on the GPU above -- DualscreenDrain's fast,
		// no-CPU-work path applies exactly as it would for a plain RGBA8
		// frame that never needed converting at all.
		slot.packed_a2b10g10r10 = false;
		slot.srgb = dstUsesSRGB;
	}
	else
	{
		slot.packed_a2b10g10r10 = isPacked1010102;
		// No sRGB-tagged variant of A2B10G10R10_UNORM_PACK32 exists in Vulkan, so a
		// sampler could never auto-decode one on read the way it would an
		// _SRGB-tagged RGBA8 texture -- these bytes are exactly as raw as a plain
		// R8G8B8A8_UNORM source, never pre-encoded.
		slot.srgb = isPacked1010102 ? false : sourceIsSRGB;
	}
	// Both screens of one emulated frame share this, which is what stops the
	// two devices showing halves of different moments.
	slot.serial = LatteGPUState.frameCounter;
	// The buffer this copy was recorded into. Once it has finished for its own
	// reasons, the bytes are there -- no fence of our own, and no waiting.
	slot.commandBufferId = GetCurrentCommandBufferId();
	// Release, pairing with the acquire load m_dualscreenThread does before
	// touching any of the fields above: everything this call wrote to the
	// slot must be visible to that thread once it sees this.
	slot.pending.store(true, std::memory_order_release);
	m_dualscreenNextSlot[screenIndex] =
		(m_dualscreenNextSlot[screenIndex] + 1) % kDualscreenReadbackSlots;
}
