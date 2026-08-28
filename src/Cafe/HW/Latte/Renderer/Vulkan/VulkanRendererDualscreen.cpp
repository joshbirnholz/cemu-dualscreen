#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

#include "DualscreenSession.h"

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

namespace
{
	// Temporary instrumentation: DualscreenSession's own probe cleared
	// Poll()/PublishScreen()/SendAudio() of the ~750ms/s HandleDualscreenFrame
	// itself was shown to cost -- none of them add up to more than a couple of
	// percent of it. What is left inside that function is DualscreenDrain's
	// per-pixel conversion loop and the handful of new-capture calls after it
	// (buffer creation on first use, the barriers, the copy command itself).
	// These three probes split those apart to find out which. Remove once
	// that's answered.
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
			slot = DualscreenReadback();
		}
	}
	m_dualscreenNextSlot = {0, 0};
	m_dualscreenHasBuffers = false;
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
		if (!slot.pending)
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
		slot.pending = false;
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
	// Whatever finished since last time goes over before anything new is
	// started, so a screen's frames arrive in the order they were drawn.
	{
		static std::chrono::steady_clock::time_point s_drainWindowStart = std::chrono::steady_clock::now();
		static double s_drainTotalMs = 0.0;
		static int s_drainCalls = 0;
		ScopedCallTiming drainTiming("Drain", s_drainWindowStart, s_drainTotalMs, s_drainCalls);
		DualscreenDrain(screenIndex);
	}

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

	auto& slot = m_dualscreenReadback[screenIndex][m_dualscreenNextSlot[screenIndex]];
	if (slot.pending)
		return; // still in flight: this end is behind, so this frame is dropped

	const size_t needed = (size_t)width * height * 4;
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

	auto textureVk = baseImageTex->GetImageObj();
	textureVk->flagForCurrentCommandBuffer();

	VkBufferImageCopy region{};
	region.bufferOffset = 0;
	// Tightly packed, which is what makes the stride the width on the far side.
	region.bufferRowLength = width;
	region.bufferImageHeight = height;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.baseArrayLayer = texViewVk->firstSlice;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.mipLevel = 0;
	region.imageOffset = {0, 0, 0};
	region.imageExtent = {(uint32)width, (uint32)height, 1};

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

	slot.width = width;
	slot.height = height;
	slot.packed_a2b10g10r10 = isPacked1010102;
	// No sRGB-tagged variant of A2B10G10R10_UNORM_PACK32 exists in Vulkan, so a
	// sampler could never auto-decode one on read the way it would an
	// _SRGB-tagged RGBA8 texture -- these bytes are exactly as raw as a plain
	// R8G8B8A8_UNORM source, never pre-encoded.
	slot.srgb = isPacked1010102 ? false : (format == VK_FORMAT_R8G8B8A8_SRGB);
	// Both screens of one emulated frame share this, which is what stops the
	// two devices showing halves of different moments.
	slot.serial = LatteGPUState.frameCounter;
	// The buffer this copy was recorded into. Once it has finished for its own
	// reasons, the bytes are there -- no fence of our own, and no waiting.
	slot.commandBufferId = GetCurrentCommandBufferId();
	slot.pending = true;
	m_dualscreenNextSlot[screenIndex] =
		(m_dualscreenNextSlot[screenIndex] + 1) % kDualscreenReadbackSlots;

	// Reading whatever the shell has sent, and noticing it going away. Twice a
	// frame when both screens are drawn, which costs a syscall that finds
	// nothing.
	session.Poll();
}
