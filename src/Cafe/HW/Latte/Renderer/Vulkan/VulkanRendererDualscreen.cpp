#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureVk.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/LatteTextureViewVk.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"

#include "DualscreenSession.h"

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
		m_dualscreenPixels.resize(count);
		if (slot.packed_a2b10g10r10)
		{
			// A2B10G10R10_UNORM_PACK32 has no sRGB-tagged twin to compare
			// against dstUsesSRGB the way RGBA8 does below -- there is nothing
			// else here that says what encoding these bits are in besides the
			// buffer's own flag, which is what chose dstUsesSRGB in the first
			// place. So there is no second opinion to reconcile: read the ten
			// bits down to eight and send them as they are.
			const uint32* source = (const uint32*)slot.mapped;
			for (size_t index = 0; index < count; ++index)
			{
				const uint32 packed = source[index];
				const uint8 r = (uint8)((packed & 0x3FFu) >> 2);
				const uint8 g = (uint8)(((packed >> 10) & 0x3FFu) >> 2);
				const uint8 b = (uint8)(((packed >> 20) & 0x3FFu) >> 2);
				m_dualscreenPixels[index] = ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
			}
		}
		else
		{
			const uint8* source = (const uint8*)slot.mapped;
			// RGBA8 in, XRGB8888 out. The gamma question is decided the same way
			// Cemu's own screenshot decides it, so what crosses the link matches
			// what is on the television here rather than being a second opinion.
			const bool toRGB = slot.srgb && !dstUsesSRGB;
			const bool toSRGB = !slot.srgb && dstUsesSRGB;
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
				m_dualscreenPixels[index] = ((uint32)r << 16) | ((uint32)g << 8) | (uint32)b;
			}
		}
		DualscreenSession::instance().PublishScreen(screen, slot.serial,
													m_dualscreenPixels.data(), slot.width,
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

	const size_t screenIndex = padView ? 1 : 0;
	// Whatever finished since last time goes over before anything new is
	// started, so a screen's frames arrive in the order they were drawn.
	DualscreenDrain(screenIndex);

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

	barrier_image<IMAGE_WRITE | TRANSFER_WRITE, TRANSFER_READ>(baseImageTex, region.imageSubresource,
															   VK_IMAGE_LAYOUT_GENERAL);
	vkCmdCopyImageToBuffer(m_state.currentCommandBuffer, textureVk->m_image,
						   VK_IMAGE_LAYOUT_GENERAL, slot.buffer, 1, &region);
	barrier_image<TRANSFER_READ, TRANSFER_WRITE | IMAGE_WRITE>(baseImageTex,
															   region.imageSubresource,
															   baseImageTex->GetDefaultLayout());

	slot.width = width;
	slot.height = height;
	slot.packed_a2b10g10r10 = isPacked1010102;
	if (isPacked1010102)
	{
		// No sRGB-tagged twin exists for this layout to read a hint off of, so
		// there is nothing to reconcile against dstUsesSRGB in DualscreenDrain
		// -- mark it equal, which is what tells that pass to leave the bits as
		// they are.
		const bool dstUsesSRGB =
			padView ? LatteGPUState.drcBufferUsesSRGB : LatteGPUState.tvBufferUsesSRGB;
		slot.srgb = dstUsesSRGB;
	}
	else
	{
		slot.srgb = (format == VK_FORMAT_R8G8B8A8_SRGB);
	}
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
