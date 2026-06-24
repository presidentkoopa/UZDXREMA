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

#include <inttypes.h>
#include <limits>
#include <thread>

#include "v_video.h"
#include "r_videoscale.h"
#include "i_time.h"
#include "v_text.h"
#include "version.h"
#include "v_draw.h"

#include "hw_clock.h"
#include "hw_vrmodes.h"
#include "hw_cvars.h"
#include "hw_skydome.h"
#include "hwrenderer/data/hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hwrenderer/data/shaderuniforms.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"

#include "vk_renderdevice.h"
#include "vk_hwbuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_renderpass.h"
#include "vulkan/renderer/vk_descriptorset.h"
#include "vulkan/renderer/vk_streambuffer.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "vulkan/renderer/vk_raytrace.h"
#include "vulkan/shaders/vk_shader.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/textures/vk_samplers.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_texture.h"
#include "vulkan/textures/vk_framebuffer.h"
#include <zvulkan/vulkanswapchain.h>

extern bool cinemamode;
#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkansurface.h>
#include <zvulkan/vulkancompatibledevice.h>
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/system/vk_buffer.h"
#include "engineerrors.h"
#include "c_dispatch.h"
#include "common/rendering/stereo3d/openxr/oxr_loader.h"

FString JitCaptureStackTrace(int framesToSkip, bool includeNativeFrames, int maxFrames = -1);

EXTERN_CVAR(Int, gl_tonemap)
EXTERN_CVAR(Int, screenblocks)
EXTERN_CVAR(Bool, cl_capfps)
EXTERN_CVAR(Int, vr_mode)
EXTERN_CVAR(Bool, gl_texture_thread)
EXTERN_CVAR(Int, gl_texture_thread_workers)
EXTERN_CVAR(Int, gl_background_flush_count)

CVAR(Bool, vk_raytrace, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Physical device info
static std::vector<VulkanCompatibleDevice> SupportedDevices;
int vkversion;

CUSTOM_CVAR(Bool, vk_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CVAR(Bool, vk_debug_callstack, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, vk_device, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CCMD(vk_listdevices)
{
	for (size_t i = 0; i < SupportedDevices.size(); i++)
	{
		Printf("#%d - %s\n", (int)i, SupportedDevices[i].Device->Properties.Properties.deviceName);
	}
}

void VulkanError(const char* text)
{
	throw CVulkanError(text);
}

void VulkanPrintLog(const char* typestr, const std::string& msg)
{
	bool showcallstack = strstr(typestr, "error") != nullptr;

	if (showcallstack)
		Printf("\n");

	Printf(TEXTCOLOR_RED "[%s] ", typestr);
	Printf(TEXTCOLOR_WHITE "%s\n", msg.c_str());

	if (vk_debug_callstack && showcallstack)
	{
		FString callstack = JitCaptureStackTrace(0, true, 5);
		if (!callstack.IsEmpty())
			Printf("%s\n", callstack.GetChars());
	}
}

bool VkTexLoadThread::loadResource(VkTexLoadIn& input, VkTexLoadOut& output)
{
	if (input.texture == nullptr || input.hardwareTexture == nullptr)
	{
		return false;
	}

	auto texBuffer = input.texture->CreateTexBuffer(input.translation, input.scaleFlags | CTF_ProcessData);
	output.pixels = std::shared_ptr<uint8_t>(texBuffer.mBuffer, std::default_delete<uint8_t[]>());
	texBuffer.mBuffer = nullptr;
	output.width = texBuffer.mWidth;
	output.height = texBuffer.mHeight;
	output.scaleFlags = input.scaleFlags;
	output.hardwareTexture = input.hardwareTexture;
	return output.pixels != nullptr;
}

VulkanRenderDevice::VulkanRenderDevice(void *hMonitor, bool fullscreen, std::shared_ptr<VulkanSurface> surface) :
	Super(hMonitor, fullscreen) 
{
	VulkanDeviceBuilder builder;
	builder.OptionalRayQuery();
	builder.Surface(surface);
	builder.SelectDevice(vk_device);
	if (vr_mode == VR_OPENXR_MOBILE)
	{
		OpenXRVulkanBootstrapInfo xrInfo;
		if (QueryOpenXRVulkanBootstrapInfo(xrInfo))
		{
			for (const auto& ext : xrInfo.requiredDeviceExtensions)
				builder.RequireExtension(ext);
		}

		VkPhysicalDevice preferredDevice = VK_NULL_HANDLE;
		if (surface != nullptr && surface->Instance != nullptr && QueryOpenXRVulkanPreferredPhysicalDevice(surface->Instance->Instance, preferredDevice))
		{
			builder.PreferredPhysicalDevice(preferredDevice);
		}
	}
	SupportedDevices = builder.FindDevices(surface->Instance);
	device = builder.Create(surface->Instance);

	if (gl_texture_thread)
	{
		unsigned int threadCount = std::max(1u, std::thread::hardware_concurrency());
		threadCount = threadCount > 1 ? threadCount - 1 : 1;
		threadCount = std::min(threadCount, (unsigned int)gl_texture_thread_workers);
		bgTransferThreads.reserve(threadCount);
		for (unsigned int i = 0; i < threadCount; i++)
		{
			auto worker = std::make_unique<VkTexLoadThread>(&primaryTexQueue, &secondaryTexQueue, &outputTexQueue);
			worker->start();
			bgTransferThreads.push_back(std::move(worker));
		}
		bgTransferEnabled = !bgTransferThreads.empty();
	}
}

VulkanRenderDevice::~VulkanRenderDevice()
{
	StopBackgroundCache();
	vkDeviceWaitIdle(device->device); // make sure the GPU is no longer using any objects before RAII tears them down

	delete mVertexData;
	delete mSkyData;
	delete mViewpoints;
	delete mLights;
	delete mBones;
	mShadowMap.Reset();

	if (mDescriptorSetManager)
		mDescriptorSetManager->Deinit();
	if (mTextureManager)
		mTextureManager->Deinit();
	if (mBufferManager)
		mBufferManager->Deinit();
	if (mShaderManager)
		mShaderManager->Deinit();

	mCommands->DeleteFrameObjects();
}

void VulkanRenderDevice::InitializeState()
{
	static bool first = true;
	if (first)
	{
		PrintStartupLog();
		first = false;
	}

	// Use the same names here as OpenGL returns.
	switch (device->PhysicalDevice.Properties.Properties.vendorID)
	{
	case 0x1002: vendorstring = "ATI Technologies Inc.";     break;
	case 0x10DE: vendorstring = "NVIDIA Corporation";  break;
	case 0x8086: vendorstring = "Intel";   break;
	default:     vendorstring = "Unknown"; break;
	}

	hwcaps = RFL_SHADER_STORAGE_BUFFER | RFL_BUFFER_STORAGE;
	glslversion = 4.50f;
	uniformblockalignment = (unsigned int)device->PhysicalDevice.Properties.Properties.limits.minUniformBufferOffsetAlignment;
	maxuniformblock = device->PhysicalDevice.Properties.Properties.limits.maxUniformBufferRange;

	mCommands.reset(new VkCommandBufferManager(this));

	mSamplerManager.reset(new VkSamplerManager(this));
	mTextureManager.reset(new VkTextureManager(this));
	mFramebufferManager.reset(new VkFramebufferManager(this));
	mBufferManager.reset(new VkBufferManager(this));
	mBufferManager->Init();

	mScreenBuffers.reset(new VkRenderBuffers(this));
	mSaveBuffers.reset(new VkRenderBuffers(this));
	mActiveRenderBuffers = mScreenBuffers.get();

	mPostprocess.reset(new VkPostprocess(this));
	mDescriptorSetManager.reset(new VkDescriptorSetManager(this));
	mRenderPassManager.reset(new VkRenderPassManager(this));
	mRaytrace.reset(new VkRaytrace(this));

	mVertexData = new FFlatVertexBuffer(GetWidth(), GetHeight());
	mSkyData = new FSkyVertexBuffer;
	mViewpoints = new HWViewpointBuffer;
	mLights = new FLightBuffer();
	mBones = new BoneBuffer();

	mShaderManager.reset(new VkShaderManager(this));
	mDescriptorSetManager->Init();
#ifdef __APPLE__
	mRenderState.reset(new VkRenderStateMolten(this));
#else
	mRenderState.reset(new VkRenderState(this));
#endif
}

void VulkanRenderDevice::Update()
{
	twoD.Reset();
	Flush3D.Reset();

	Flush3D.Clock();

	const auto vrmode = VRMode::GetVRModeCached(true);
	auto* postprocess = GetPostprocess();
	if (postprocess)
	{
		if (vrmode != nullptr && vrmode->IsVR())
		{
			const int eyeCount = vrmode->mEyeCount > 0 ? vrmode->mEyeCount : 1;
			const bool suppressSceneEye2D = vrmode->ShouldUseScreenLayerForCurrentFrame() || cinemamode;
			const bool useGameplayEyeViewport = vrmode->ShouldUseRecommendedRenderSizeThisFrame() && !vrmode->IsRenderingVirtualScreen();
			const IntRect savedScreenViewport = mScreenViewport;
			VREyeComposite.Clock();
			for (int eye_ix = 0; eye_ix < eyeCount; ++eye_ix)
			{
				mCurrentEyeIndex = eye_ix;
				const auto eye = (eye_ix >= 0 && eye_ix < 2) ? vrmode->mEyes[eye_ix] : nullptr;
				postprocess->SetPipelineImagePair((eye_ix % 2) * 2, 2);
				postprocess->SetCurrentPipelineImage(mEyeFinalPipelineImage[eye_ix % 2]);
				if (useGameplayEyeViewport)
				{
					mScreenViewport = mSceneViewport;
				}
				postprocess->SetActiveRenderTarget();
				if (eye != nullptr)
				{
					eye->AdjustBlend(nullptr);
				}
				if (!suppressSceneEye2D && twod != nullptr && twod->HasCommandsForPass(true))
				{
					Draw2D(true);
				}
				if (!vrmode->ShouldUseScreenLayerForCurrentFrame() && !vrmode->IsRenderingVirtualScreen() && eye != nullptr)
				{
					eye->AdjustHud();
				}
				if (!suppressSceneEye2D && twod != nullptr && twod->HasCommandsForPass(false))
				{
					Draw2D(false);
				}
				vrmode->FinalizeEyeImage(this, eye_ix);
			}
			VREyeComposite.Unclock();
			mCurrentEyeIndex = 0;
			mScreenViewport = savedScreenViewport;
			vrmode->RenderVirtualScreen();
			twod->Clear();
		}
		else
		{
			postprocess->SetActiveRenderTarget();
			Draw2D();
			twod->Clear();
		}
	}

	mRenderState->EndRenderPass();
	mRenderState->EndFrame();

	Flush3D.Unclock();

	mCommands->WaitForCommands(true);
	mCommands->UpdateGpuStats();

	if (vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame)
	{
		vrmode->AcquireXRSwapchain();
	}
	mXRFrameBeganThisFrame = false;

	Super::Update();
}

bool VulkanRenderDevice::CompileNextShader()
{
	return mShaderManager->CompileNextShader();
}

void VulkanRenderDevice::RenderTextureView(FCanvasTexture* tex, std::function<void(IntRect &)> renderFunc)
{
	auto BaseLayer = static_cast<VkHardwareTexture*>(tex->GetHardwareTexture(0, 0));

	VkTextureImage *image = BaseLayer->GetImage(tex, 0, 0);
	VkTextureImage *depthStencil = BaseLayer->GetDepthStencil(tex);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	mRenderState->SetRenderTarget(image, depthStencil->GetFramebufferView(), image->Image->width, image->Image->height, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT);

	IntRect bounds;
	bounds.left = bounds.top = 0;
	bounds.width = min(tex->GetWidth(), image->Image->width);
	bounds.height = min(tex->GetHeight(), image->Image->height);

	renderFunc(bounds);

	mRenderState->EndRenderPass();

	VkImageTransition()
		.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(mCommands->GetDrawCommands());

	SetSceneRenderTarget(false);

	tex->SetUpdated(true);
}

void VulkanRenderDevice::PostProcessScene(bool swscene, int fixedcm, float flash, const std::function<void()> &afterBloomDrawEndScene2D)
{
	if (!swscene) mPostprocess->BlitSceneToPostprocess(); // Copy the resulting scene to the current post process texture
	mPostprocess->PostProcessScene(fixedcm, flash, afterBloomDrawEndScene2D);
	mEyeFinalPipelineImage[mCurrentEyeIndex % 2] = mPostprocess->GetCurrentPipelineImage();
}

const char* VulkanRenderDevice::DeviceName() const
{
	return device->PhysicalDevice.Properties.Properties.deviceName;
}

void VulkanRenderDevice::SetVSync(bool vsync)
{
	mVSync = vsync;
}

void VulkanRenderDevice::NewRefreshRate()
{
	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR())
	{
		vrmode->ApplyRefreshRate();
	}
}

void VulkanRenderDevice::PrecacheMaterial(FMaterial *mat, int translation)
{
	if (mat->Source()->GetUseType() == ETextureType::SWCanvas) return;

	MaterialLayerInfo* layer;

	auto systex = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation, &layer));
	systex->GetImage(layer->layerTexture, translation, layer->scaleFlags);

	int numLayers = mat->NumLayers();
	for (int i = 1; i < numLayers; i++)
	{
		auto syslayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		syslayer->GetImage(layer->layerTexture, 0, layer->scaleFlags);
	}
}

void VulkanRenderDevice::PrequeueMaterial(FMaterial *mat, int translation)
{
	BackgroundCacheMaterial(mat, FTranslationID::fromInt(translation), false, true);
}

bool VulkanRenderDevice::BackgroundCacheTextureMaterial(FGameTexture *tex, FTranslationID translation, int scaleFlags, bool makeSPI)
{
	if (!bgTransferEnabled || tex == nullptr || !tex->isValid())
	{
		return false;
	}

	patchQueue.deleteSearch([&](QueuedPatch& queued)
	{
		return queued.tex == tex &&
			queued.translation == translation.index() &&
			queued.scaleFlags == scaleFlags;
	});

	QueuedPatch patch;
	patch.tex = tex;
	patch.translation = translation.index();
	patch.scaleFlags = scaleFlags;
	patch.secondary = false;
	patchQueue.queue(std::move(patch));
	return true;
}

bool VulkanRenderDevice::BackgroundCacheMaterial(FMaterial *mat, FTranslationID translation, bool makeSPI, bool secondary)
{
	if (!bgTransferEnabled || mat == nullptr || mat->Source()->GetUseType() == ETextureType::SWCanvas)
	{
		return false;
	}

	MaterialLayerInfo* layer = nullptr;
	auto baseLayer = static_cast<VkHardwareTexture*>(mat->GetLayer(0, translation.index(), &layer));
	if (layer != nullptr && layer->layerTexture != nullptr)
	{
		if (!secondary && baseLayer->GetState() == IHardwareTexture::CACHING)
		{
			VkTexLoadIn input;
			if (secondaryTexQueue.dequeueSearch(input, [&](VkTexLoadIn& queued)
			{
				return queued.hardwareTexture == baseLayer;
			}))
			{
				baseLayer->SetHardwareState(IHardwareTexture::LOADING);
				primaryTexQueue.queue(std::move(input));
				for (auto& thread : bgTransferThreads) thread->wake();
			}
		}
		else if (baseLayer->GetState() == IHardwareTexture::NONE)
		{
			baseLayer->SetHardwareState(secondary ? IHardwareTexture::CACHING : IHardwareTexture::LOADING);
			VkTexLoadIn input;
			input.texture = layer->layerTexture;
			input.translation = translation.index();
			input.scaleFlags = layer->scaleFlags;
			input.hardwareTexture = baseLayer;
			if (secondary) secondaryTexQueue.queue(std::move(input));
			else primaryTexQueue.queue(std::move(input));
			for (auto& thread : bgTransferThreads) thread->wake();
		}
	}

	for (int i = 1; i < mat->NumLayers(); i++)
	{
		auto extraLayer = static_cast<VkHardwareTexture*>(mat->GetLayer(i, 0, &layer));
		if (layer != nullptr && layer->layerTexture != nullptr)
		{
			if (!secondary && extraLayer->GetState() == IHardwareTexture::CACHING)
			{
				VkTexLoadIn input;
				if (secondaryTexQueue.dequeueSearch(input, [&](VkTexLoadIn& queued)
				{
					return queued.hardwareTexture == extraLayer;
				}))
				{
					extraLayer->SetHardwareState(IHardwareTexture::LOADING);
					primaryTexQueue.queue(std::move(input));
					for (auto& thread : bgTransferThreads) thread->wake();
				}
			}
			else if (extraLayer->GetState() == IHardwareTexture::NONE)
			{
				extraLayer->SetHardwareState(secondary ? IHardwareTexture::CACHING : IHardwareTexture::LOADING);
				VkTexLoadIn input;
				input.texture = layer->layerTexture;
				input.translation = 0;
				input.scaleFlags = layer->scaleFlags;
				input.hardwareTexture = extraLayer;
				if (secondary) secondaryTexQueue.queue(std::move(input));
				else primaryTexQueue.queue(std::move(input));
				for (auto& thread : bgTransferThreads) thread->wake();
			}
		}
	}

	return true;
}

bool VulkanRenderDevice::CachingActive()
{
	if (!bgTransferEnabled)
	{
		return false;
	}

	if (primaryTexQueue.size() > 0 || secondaryTexQueue.size() > 0 || outputTexQueue.size() > 0 || patchQueue.size() > 0)
	{
		return true;
	}

	for (auto& thread : bgTransferThreads)
	{
		if (thread->isActive())
		{
			return true;
		}
	}
	return false;
}

float VulkanRenderDevice::CacheProgress()
{
	return (float)(primaryTexQueue.size() + secondaryTexQueue.size() + patchQueue.size());
}

void VulkanRenderDevice::StopBackgroundCache()
{
	for (auto& thread : bgTransferThreads)
	{
		thread->stop();
	}
	bgTransferThreads.clear();
	bgTransferEnabled = false;
}

void VulkanRenderDevice::FlushBackground()
{
	if (!bgTransferEnabled)
	{
		return;
	}

	UpdateBackgroundCache(true);
	UploadLoadedTextures(true);
}

void VulkanRenderDevice::UploadLoadedTextures(bool flush)
{
	int uploadBudget = flush ? std::numeric_limits<int>::max() : std::max(1, (int)gl_background_flush_count);

	VkTexLoadOut loaded;
	while (uploadBudget > 0 && outputTexQueue.dequeue(loaded))
	{
		if (loaded.hardwareTexture != nullptr && loaded.pixels != nullptr)
		{
			const bool indexed = !!(loaded.scaleFlags & CTF_Indexed);
			loaded.hardwareTexture->BackgroundCreateTexture(
				mCommands.get(),
				loaded.width,
				loaded.height,
				indexed ? 1 : 4,
				indexed ? VK_FORMAT_R8_UNORM : VK_FORMAT_B8G8R8A8_UNORM,
				loaded.pixels.get(),
				indexed ? 0 : -1,
				!indexed);
			loaded.hardwareTexture->CheckFinalTransition(mCommands->GetTransferCommands(), true);
			loaded.hardwareTexture->SetHardwareState(IHardwareTexture::READY);
		}
		uploadBudget--;
	}
}

void VulkanRenderDevice::UpdateBackgroundCache(bool flush)
{
	if (!bgTransferEnabled)
	{
		return;
	}

	QueuedPatch patch;
	while (patchQueue.dequeue(patch))
	{
		if (patch.tex != nullptr)
		{
			auto material = FMaterial::ValidateTexture(patch.tex, patch.scaleFlags, true);
			if (material != nullptr)
			{
				BackgroundCacheMaterial(material, FTranslationID::fromInt(patch.translation), false, patch.secondary);
			}
		}
	}

	if (flush)
	{
		while (CachingActive())
		{
			UploadLoadedTextures(true);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return;
	}

	UploadLoadedTextures(false);
}

IHardwareTexture *VulkanRenderDevice::CreateHardwareTexture(int numchannels)
{
	return new VkHardwareTexture(this, numchannels);
}

FMaterial* VulkanRenderDevice::CreateMaterial(FGameTexture* tex, int scaleflags)
{
	return new VkMaterial(this, tex, scaleflags);
}

IVertexBuffer *VulkanRenderDevice::CreateVertexBuffer()
{
	return GetBufferManager()->CreateVertexBuffer();
}

IIndexBuffer *VulkanRenderDevice::CreateIndexBuffer()
{
	return GetBufferManager()->CreateIndexBuffer();
}

IDataBuffer *VulkanRenderDevice::CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize)
{
	return GetBufferManager()->CreateDataBuffer(bindingpoint, ssbo, needsresize);
}

void VulkanRenderDevice::SetTextureFilterMode()
{
	if (mSamplerManager)
	{
		mDescriptorSetManager->ResetHWTextureSets();
		mSamplerManager->ResetHWSamplers();
	}
}

void VulkanRenderDevice::StartPrecaching()
{
	// Destroy the texture descriptors to avoid problems with potentially stale textures.
	mDescriptorSetManager->ResetHWTextureSets();
}

void VulkanRenderDevice::BlurScene(float amount)
{
	if (mPostprocess)
		mPostprocess->BlurScene(amount);
}

void VulkanRenderDevice::UpdatePalette()
{
	if (mPostprocess)
		mPostprocess->ClearTonemapPalette();
}

FTexture *VulkanRenderDevice::WipeStartScreen()
{
	SetViewportRects(nullptr);

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeStartScreen");

	return tex;
}

FTexture *VulkanRenderDevice::WipeEndScreen()
{
	GetPostprocess()->SetActiveRenderTarget();
	Draw2D();
	twod->Clear();

	auto tex = new FWrapperTexture(mScreenViewport.width, mScreenViewport.height, 1);
	auto systex = static_cast<VkHardwareTexture*>(tex->GetSystemTexture());

	systex->CreateWipeTexture(mScreenViewport.width, mScreenViewport.height, "WipeEndScreen");

	return tex;
}

void VulkanRenderDevice::CopyScreenToBuffer(int w, int h, uint8_t *data)
{
	VkTextureImage image;

	// Convert from rgba16f to rgba8 using the GPU:
	image.Image = ImageBuilder()
		.Format(VK_FORMAT_R8G8B8A8_UNORM)
		.Usage(VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
		.Size(w, h)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	GetPostprocess()->BlitCurrentToImage(&image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	// Staging buffer for download
	auto staging = BufferBuilder()
		.Size(w * h * 4)
		.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
		.DebugName("CopyScreenToBuffer")
		.Create(device.get());

	// Copy from image to buffer
	VkBufferImageCopy region = {};
	region.imageExtent.width = w;
	region.imageExtent.height = h;
	region.imageExtent.depth = 1;
	region.imageSubresource.layerCount = 1;
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	mCommands->GetDrawCommands()->copyImageToBuffer(image.Image->image, image.Layout, staging->buffer, 1, &region);

	// Submit command buffers and wait for device to finish the work
	mCommands->WaitForCommands(false);

	// Map and convert from rgba8 to rgb8
	uint8_t *dest = (uint8_t*)data;
	uint8_t *pixels = (uint8_t*)staging->Map(0, w * h * 4);
	int dindex = 0;
	for (int y = 0; y < h; y++)
	{
		int sindex = (h - y - 1) * w * 4;
		for (int x = 0; x < w; x++)
		{
			dest[dindex] = pixels[sindex];
			dest[dindex + 1] = pixels[sindex + 1];
			dest[dindex + 2] = pixels[sindex + 2];
			dindex += 3;
			sindex += 4;
		}
	}
	staging->Unmap();
}

void VulkanRenderDevice::SetActiveRenderTarget()
{
	mPostprocess->SetActiveRenderTarget();
}

void VulkanRenderDevice::FirstEye()
{
	if (mPostprocess)
	{
		mCurrentEyeIndex = 0;
		mPostprocess->SetPipelineImagePair(0, 2);
		mPostprocess->SetCurrentPipelineImage(0);
	}
}

void VulkanRenderDevice::NextEye(int eyecount)
{
	if (mPostprocess)
	{
		if (eyecount > 1)
		{
			mCurrentEyeIndex = (mCurrentEyeIndex + 1) % eyecount;
		}
		else
		{
			mCurrentEyeIndex = 0;
		}
		mPostprocess->SetPipelineImagePair((mCurrentEyeIndex % 2) * 2, 2);
		mPostprocess->SetCurrentPipelineImage((mCurrentEyeIndex % 2) * 2);
	}
}

TArray<uint8_t> VulkanRenderDevice::GetScreenshotBuffer(int &pitch, ESSType &color_type, float &gamma)
{
	int w = SCREENWIDTH;
	int h = SCREENHEIGHT;

	IntRect box;
	box.left = 0;
	box.top = 0;
	box.width = w;
	box.height = h;
	mPostprocess->DrawPresentTexture(box, true, true);

	TArray<uint8_t> ScreenshotBuffer(w * h * 3, true);
	CopyScreenToBuffer(w, h, ScreenshotBuffer.Data());

	pitch = w * 3;
	color_type = SS_RGB;
	gamma = 1.0f;
	return ScreenshotBuffer;
}

void VulkanRenderDevice::BeginFrame()
{
	const auto vrmode = VRMode::GetVRModeCached(true);
	mXRFrameBeganThisFrame = false;
	mCurrentEyeIndex = 0;
	mEyeFinalPipelineImage[0] = 0;
	mEyeFinalPipelineImage[1] = 2;
	if (vrmode != nullptr && vrmode->IsVR())
	{
		vrmode->SetUp();
		mXRFrameBeganThisFrame = vrmode->BeginXRFrame();
	}

	SetViewportRects(nullptr);
	int bufferScreenWidth = mScreenViewport.width;
	int bufferScreenHeight = mScreenViewport.height;
	int bufferSceneWidth = mSceneViewport.width;
	int bufferSceneHeight = mSceneViewport.height;
	if (vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame && vrmode->ShouldUseRecommendedRenderSizeThisFrame())
	{
		int recommendedWidth = 0;
		int recommendedHeight = 0;
		if (vrmode->GetRecommendedRenderSize(recommendedWidth, recommendedHeight))
		{
			bufferSceneWidth = recommendedWidth;
			bufferSceneHeight = recommendedHeight;
			bufferScreenWidth = recommendedWidth;
			bufferScreenHeight = recommendedHeight;
		}
	}
	if (bufferSceneWidth == 0 || bufferSceneHeight == 0)
	{
		bufferSceneWidth = bufferScreenWidth;
		bufferSceneHeight = bufferScreenHeight;
	}

	mViewpoints->Clear();
	UpdateBackgroundCache(false);
	mCommands->BeginFrame();
	mTextureManager->BeginFrame();
	int eyeLayerCount = 1;
	const bool useMultiviewScene = vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame && vrmode->ShouldUseMultiviewThisFrame();
	if (vrmode != nullptr && vrmode->IsVR() && mXRFrameBeganThisFrame && vrmode->ShouldUseMultiviewThisFrame())
	{
		eyeLayerCount = std::max(1, vrmode->GetMultiviewLayerCount());
	}
	const uint32_t multiviewMask = useMultiviewScene ? vrmode->GetMultiviewViewMask() : 0;
	mScreenBuffers->BeginFrame(
		bufferScreenWidth, bufferScreenHeight,
		bufferSceneWidth, bufferSceneHeight,
		eyeLayerCount, eyeLayerCount);

	const VkSampleCountFlagBits sceneSamples = mScreenBuffers->GetSceneSamples();
	mSaveBuffers->BeginFrame(SAVEPICWIDTH, SAVEPICHEIGHT, SAVEPICWIDTH, SAVEPICHEIGHT, 1, 1);
	mRenderState->BeginFrame();
	mDescriptorSetManager->BeginFrame();
}

void VulkanRenderDevice::SetViewportRects(IntRect *bounds)
{
	Super::SetViewportRects(bounds);

	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR() && vrmode->ShouldUseRecommendedRenderSizeThisFrame())
	{
		vrmode->AdjustViewport(this);
	}
}

void VulkanRenderDevice::InitLightmap(int LMTextureSize, int LMTextureCount, TArray<uint16_t>& LMTextureData)
{
	if (LMTextureData.Size() > 0)
	{
		GetTextureManager()->SetLightmap(LMTextureSize, LMTextureCount, LMTextureData);
		LMTextureData.Reset(); // We no longer need this, release the memory
	}
}

void VulkanRenderDevice::Draw2D(bool outside2D)
{
	::Draw2D(twod, *mRenderState, outside2D);
}

void VulkanRenderDevice::WaitForCommands(bool finish)
{
	mCommands->WaitForCommands(finish);
}

unsigned int VulkanRenderDevice::GetLightBufferBlockSize() const
{
	return mLights->GetBlockSize();
}

void VulkanRenderDevice::PrintStartupLog()
{
	const auto &props = device->PhysicalDevice.Properties.Properties;

	FString deviceType;
	switch (props.deviceType)
	{
	case VK_PHYSICAL_DEVICE_TYPE_OTHER: deviceType = "other"; break;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "integrated gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "discrete gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "virtual gpu"; break;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "cpu"; break;
	default: deviceType.Format("%d", (int)props.deviceType); break;
	}

	FString apiVersion, driverVersion;
	apiVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
	driverVersion.Format("%d.%d.%d", VK_VERSION_MAJOR(props.driverVersion), VK_VERSION_MINOR(props.driverVersion), VK_VERSION_PATCH(props.driverVersion));
	vkversion = VK_API_VERSION_MAJOR(props.apiVersion) * 100 + VK_API_VERSION_MINOR(props.apiVersion);

	Printf("Vulkan device: " TEXTCOLOR_ORANGE "%s\n", props.deviceName);
	Printf("Vulkan device type: %s\n", deviceType.GetChars());
	Printf("Vulkan version: %s (api) %s (driver)\n", apiVersion.GetChars(), driverVersion.GetChars());

	Printf(PRINT_LOG, "Vulkan extensions:");
	for (const VkExtensionProperties &p : device->PhysicalDevice.Extensions)
	{
		Printf(PRINT_LOG, " %s", p.extensionName);
	}
	Printf(PRINT_LOG, "\n");

	const auto &limits = props.limits;
	Printf("Max. texture size: %d\n", limits.maxImageDimension2D);
	Printf("Max. uniform buffer range: %d\n", limits.maxUniformBufferRange);
	Printf("Min. uniform buffer offset alignment: %" PRIu64 "\n", limits.minUniformBufferOffsetAlignment);
}

void VulkanRenderDevice::SetLevelMesh(hwrenderer::LevelMesh* mesh)
{
	mRaytrace->SetLevelMesh(mesh);
}

void VulkanRenderDevice::UpdateShadowMap()
{
	mPostprocess->UpdateShadowMap();
}

void VulkanRenderDevice::SetSaveBuffers(bool yes)
{
	if (yes) mActiveRenderBuffers = mSaveBuffers.get();
	else mActiveRenderBuffers = mScreenBuffers.get();
}

void VulkanRenderDevice::ImageTransitionScene(bool unknown)
{
	mPostprocess->ImageTransitionScene(unknown);
}

FRenderState* VulkanRenderDevice::RenderState()
{
	return mRenderState.get();
}

void VulkanRenderDevice::AmbientOccludeScene(float m5)
{
	mPostprocess->AmbientOccludeScene(m5);
}

void VulkanRenderDevice::SetSceneRenderTarget(bool useSSAO)
{
	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode != nullptr && vrmode->IsVR() && vrmode->ShouldUseMultiviewThisFrame() && GetBuffers()->GetSceneLayers() > 1)
	{
		mRenderState->SetRenderTarget(
			&GetBuffers()->SceneColor,
			GetBuffers()->SceneDepthStencil.GetFramebufferView(),
			GetBuffers()->GetWidth(),
			GetBuffers()->GetHeight(),
			VK_FORMAT_R16G16B16A16_SFLOAT,
			GetBuffers()->GetSceneSamples(),
			std::max(1, vrmode->GetMultiviewLayerCount()),
			vrmode->GetMultiviewViewMask(),
			0);
		return;
	}

	const int layerIndex = GetBuffers()->GetSceneLayers() > 1 ? GetCurrentEyeLayer() : 0;
	mRenderState->SetRenderTarget(&GetBuffers()->SceneColor, GetBuffers()->SceneDepthStencil.GetLayerView(layerIndex), GetBuffers()->GetWidth(), GetBuffers()->GetHeight(), VK_FORMAT_R16G16B16A16_SFLOAT, GetBuffers()->GetSceneSamples(), 1, 0, layerIndex);
}

bool VulkanRenderDevice::RaytracingEnabled()
{
	return vk_raytrace && device->SupportsExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
}

bool VulkanRenderDevice::ShouldUseCurrentEyeLayer(const PPTextureType& type, const VkTextureImage* image) const
{
	if (!image || !image->Image || image->Image->layerCount <= 1)
		return false;

	switch (type)
	{
	case PPTextureType::CurrentPipelineTexture:
	case PPTextureType::NextPipelineTexture:
	case PPTextureType::SceneColor:
	case PPTextureType::SceneNormal:
	case PPTextureType::SceneFog:
	case PPTextureType::SceneDepth:
		return true;
	default:
		return false;
	}
}
