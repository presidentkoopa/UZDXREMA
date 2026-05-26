
#pragma once

#include <functional>
#include <map>
#include <array>

#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "zvulkan/vulkanobjects.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_imagetransition.h"

class FString;

class VkPPShader;
class VkPPTexture;
class PipelineBarrier;
class VulkanRenderDevice;
class VulkanCommandBuffer;

class VkPostprocess
{
public:
	VkPostprocess(VulkanRenderDevice* fb);
	~VkPostprocess();

	void SetActiveRenderTarget();
	void PostProcessScene(int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D);

	void AmbientOccludeScene(float m5);
	void BlurScene(float gameinfobluramount);
	void ClearTonemapPalette();

	void UpdateShadowMap();

	void ImageTransitionScene(bool undefinedSrcLayout);

	void BlitSceneToPostprocess();
	void BlitCurrentToImage(VkTextureImage *image, VkImageLayout finallayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	void DrawPresentTexture(const IntRect &box, bool applyGamma, bool screenshot);
	void DrawPresentTextureToImage(VkTextureImage *image, VkFormat outputFormat, const IntRect &box, bool applyGamma, bool screenshot, float sourceScaleX, float sourceScaleY, float sourceOffsetX, float sourceOffsetY, VulkanCommandBuffer *cmdbuffer, bool applyOpenXrBias = true);

	int GetCurrentPipelineImage() const { return mCurrentPipelineImage; }
	void SetCurrentPipelineImage(int index);
	void NextEye(int eyeCount);

private:
	VulkanRenderDevice* fb = nullptr;

	int mCurrentPipelineImage = 0;

	friend class VkPPRenderState;
};
