#pragma once

#ifdef LoadImage
#undef LoadImage
#endif

#define SHADED_TEXTURE -1
#define DIRECT_PALETTE -2

#include "tarray.h"
#include "hw_ihwtexture.h"
#include <zvulkan/vulkanobjects.h>
#include "vk_imagetransition.h"
#include "hw_material.h"
#include <list>

struct FMaterialState;
class VulkanDescriptorSet;
class VulkanImage;
class VulkanImageView;
class VulkanBuffer;
class VulkanRenderDevice;
class FGameTexture;

class VkHardwareTexture : public IHardwareTexture
{
	friend class VkMaterial;
	friend class VulkanRenderDevice;
public:
	VkHardwareTexture(VulkanRenderDevice* fb, int numchannels);
	~VkHardwareTexture();

	void Reset();

	// Software renderer stuff
	void AllocateBuffer(int w, int h, int texelsize) override;
	uint8_t *MapBuffer() override;
	unsigned int CreateTexture(unsigned char * buffer, int w, int h, int texunit, bool mipmap, const char *name) override;
	void BackgroundCreateTexture(VkCommandBufferManager* bufManager, int w, int h, int pixelsize, VkFormat format, const void *pixels, int numMipLevels, bool createMips, int totalSize = -1);
	void CheckFinalTransition(VulkanCommandBuffer* cmd, bool background);
	void ReleaseLoadedFromQueue(VulkanCommandBuffer* cmd, int fromQueueFamily, int toQueueFamily);
	void AcquireLoadedFromQueue(VulkanCommandBuffer* cmd, int fromQueueFamily, int toQueueFamily);

	// Wipe screen
	void CreateWipeTexture(int w, int h, const char *name);

	VkTextureImage *GetImage(FTexture *tex, int translation, int flags);
	VkTextureImage *GetDepthStencil(FTexture *tex);

	VulkanRenderDevice* fb = nullptr;
	std::list<VkHardwareTexture*>::iterator it;

private:
	void CreateImage(FTexture *tex, int translation, int flags);

	void CreateTexture(int w, int h, int pixelsize, VkFormat format, const void *pixels, bool mipmap);
	void CreateTexture(VkCommandBufferManager *bufManager, VkTextureImage *img, int w, int h, int pixelsize, VkFormat format, const void *pixels, int mipmap, bool generateMipmaps = true, int totalSize = -1);
	void SwapToLoadedImage();
	static int GetMipLevels(int w, int h);

	VkTextureImage mImage;
	VkTextureImage mLoadedImage;
	int mTexelsize = 4;

	VkTextureImage mDepthStencil;

	uint8_t* mappedSWFB = nullptr;
};

class VkMaterial : public FMaterial
{
public:
	VkMaterial(VulkanRenderDevice* fb, FGameTexture* tex, int scaleflags);
	~VkMaterial();

	VulkanDescriptorSet* GetDescriptorSet(const FMaterialState& state);

	void DeleteDescriptors() override;

	VulkanRenderDevice* fb = nullptr;
	std::list<VkMaterial*>::iterator it;

private:
	struct DescriptorEntry
	{
		int clampmode;
		intptr_t remap;
		std::unique_ptr<VulkanDescriptorSet> descriptor;

		DescriptorEntry(int cm, intptr_t f, std::unique_ptr<VulkanDescriptorSet>&& d)
		{
			clampmode = cm;
			remap = f;
			descriptor = std::move(d);
		}
	};

	std::vector<DescriptorEntry> mDescriptorSets;
};
