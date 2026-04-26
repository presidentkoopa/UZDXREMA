/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <zvulkan/vulkanobjects.h>
#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanswapchain.h>
#include "v_video.h"
#include "hw_vrmodes.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "hw_cvars.h"
#include "vk_framebuffer.h"

CVAR(Bool, vk_hdr, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vk_exclusivefullscreen, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
EXTERN_CVAR(Int, vr_desktop_view);
EXTERN_CVAR(Bool, vr_swap_eyes);

VkFramebufferManager::VkFramebufferManager(VulkanRenderDevice* fb) : fb(fb)
{
	SwapChain = VulkanSwapChainBuilder()
		.Create(fb->device.get());

	SwapChainImageAvailableSemaphore = SemaphoreBuilder()
		.DebugName("SwapChainImageAvailableSemaphore")
		.Create(fb->device.get());

	RenderFinishedSemaphore = SemaphoreBuilder()
		.DebugName("RenderFinishedSemaphore")
		.Create(fb->device.get());
}

VkFramebufferManager::~VkFramebufferManager()
{
}

void VkFramebufferManager::AcquireImage()
{
	bool exclusiveFullscreen = fb->IsFullscreen() && vk_exclusivefullscreen;
	if (SwapChain->Lost() || fb->GetClientWidth() != CurrentWidth || fb->GetClientHeight() != CurrentHeight || fb->GetVSync() != CurrentVSync || CurrentHdr != vk_hdr || CurrentExclusiveFullscreen != exclusiveFullscreen)
	{
		Framebuffers.clear();

		CurrentWidth = fb->GetClientWidth();
		CurrentHeight = fb->GetClientHeight();
		CurrentVSync = fb->GetVSync();
		CurrentHdr = vk_hdr;
		CurrentExclusiveFullscreen = exclusiveFullscreen;

		SwapChain->Create(CurrentWidth, CurrentHeight, CurrentVSync ? 2 : 3, CurrentVSync, CurrentHdr, CurrentExclusiveFullscreen);
	}

	PresentImageIndex = SwapChain->AcquireImage(SwapChainImageAvailableSemaphore.get());
	if (PresentImageIndex != -1)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode != nullptr && vrmode->IsVR() && vr_desktop_view != -1)
		{
			auto buffers = fb->GetBuffers();
			auto cmdbuffer = fb->GetCommands()->GetDrawCommands();
			auto dstImage = SwapChain->GetImage(PresentImageIndex);
			auto clearColorImage = &dstImage->image;
			auto leftSourceIndex = vr_swap_eyes ? 1 : 0;
			auto rightSourceIndex = vr_swap_eyes ? 0 : 1;
			auto leftSource = &buffers->PipelineImage[leftSourceIndex];
			auto rightSource = &buffers->PipelineImage[rightSourceIndex];
			bool sideBySide = vr_desktop_view != 1 && vr_desktop_view != 2;

			if (leftSource->Image == nullptr || (sideBySide && rightSource->Image == nullptr))
			{
				Printf("Vulkan present: missing eye source image, falling back to single-eye mirror.\n");
				fb->GetPostprocess()->DrawPresentTexture(fb->mOutputLetterbox, true, false);
			}
			else
			{
				Printf("Vulkan present: vr_desktop_view=%d leftEyeSource=%d rightEyeSource=%d sideBySide=%d\n",
					(int)vr_desktop_view, leftSourceIndex, rightSourceIndex, sideBySide ? 1 : 0);

				VkImageTransition()
					.AddImage(leftSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
					.Execute(cmdbuffer);
				if (sideBySide && rightSource != leftSource)
				{
					VkImageTransition()
						.AddImage(rightSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
						.Execute(cmdbuffer);
				}

				VkImageMemoryBarrier dstBarrier = {};
				dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				dstBarrier.srcAccessMask = 0;
				dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				dstBarrier.image = *clearColorImage;
				dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				cmdbuffer->pipelineBarrier(
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

				VkClearColorValue clearValue = {};
				VkImageSubresourceRange clearRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				cmdbuffer->clearColorImage(*clearColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearValue, 1, &clearRange);

				auto blitEye = [&](VkTextureImage* source, const IntRect& rect)
				{
					VkImageBlit blit = {};
					blit.srcOffsets[0] = { 0, (int32_t)source->Image->height, 0 };
					blit.srcOffsets[1] = { (int32_t)source->Image->width, 0, 1 };
					blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					blit.srcSubresource.mipLevel = 0;
					blit.srcSubresource.baseArrayLayer = 0;
					blit.srcSubresource.layerCount = 1;
					blit.dstOffsets[0] = { rect.left, rect.top, 0 };
					blit.dstOffsets[1] = { rect.left + rect.width, rect.top + rect.height, 1 };
					blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					blit.dstSubresource.mipLevel = 0;
					blit.dstSubresource.baseArrayLayer = 0;
					blit.dstSubresource.layerCount = 1;
					cmdbuffer->blitImage(
						source->Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						*clearColorImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						1, &blit, VK_FILTER_LINEAR);
				};

				IntRect mirrorBox = fb->mOutputLetterbox;
				if (vr_desktop_view == 1)
				{
					blitEye(leftSource, mirrorBox);
				}
				else if (vr_desktop_view == 2)
				{
					blitEye(rightSource, mirrorBox);
				}
				else
				{
					IntRect leftHalf = mirrorBox;
					leftHalf.width = mirrorBox.width / 2;
					IntRect rightHalf = mirrorBox;
					rightHalf.width = mirrorBox.width - leftHalf.width;
					rightHalf.left += leftHalf.width;

					blitEye(leftSource, leftHalf);
					blitEye(rightSource, rightHalf);
				}

				if (vrmode->RenderDesktopMirror(fb, dstImage))
				{
					// OpenXR virtual screen composed directly into the desktop mirror.
				}

				VkImageTransition()
					.AddImage(leftSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
					.Execute(cmdbuffer);
				if (sideBySide && rightSource != leftSource)
				{
					VkImageTransition()
						.AddImage(rightSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
						.Execute(cmdbuffer);
				}

				VkImageMemoryBarrier dstFinalBarrier = {};
				dstFinalBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				dstFinalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				dstFinalBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
				dstFinalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
				dstFinalBarrier.dstAccessMask = 0;
				dstFinalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				dstFinalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				dstFinalBarrier.image = *clearColorImage;
				dstFinalBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				cmdbuffer->pipelineBarrier(
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
					0, 0, nullptr, 0, nullptr, 1, &dstFinalBarrier);
			}
		}
		else
		{
			fb->GetPostprocess()->DrawPresentTexture(fb->mOutputLetterbox, true, false);
		}
	}
}

void VkFramebufferManager::QueuePresent()
{
	if (PresentImageIndex != -1)
		SwapChain->QueuePresent(PresentImageIndex, RenderFinishedSemaphore.get());
}
