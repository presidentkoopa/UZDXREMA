#pragma once

#include "hw_vrmodes.h"
#include "vulkan/system/vk_renderdevice.h"
#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "zvulkan/vulkanobjects.h"

#include <vector>
#include <memory>

namespace s3d {

class VKOpenXRDeviceEyePose : public VREyeInfo
{
public:
	friend class VKOpenXRDeviceMode;

	VKOpenXRDeviceEyePose(int eye);
	virtual ~VKOpenXRDeviceEyePose() override;
	virtual VSMatrix GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio, bool iso_ortho) const override;
	DVector3 GetViewShift(FRenderViewpoint& vp) const override;
	virtual void SetUp() const override;
	virtual void TearDown() const override;
	virtual void AdjustHud() const override;
	virtual void AdjustBlend(HWDrawInfo* di) const override;

protected:
	int eye;
	mutable VSMatrix projection;
	mutable XrPosef currentEyePose{};
	mutable XrFovf currentFov{};
};

class VKOpenXRDeviceMode : public VRMode
{
public:
	friend class VKOpenXRDeviceEyePose;
	static const VRMode& getInstance();

	VKOpenXRDeviceMode();
	virtual ~VKOpenXRDeviceMode() override;
	
	virtual void SetUp() const override;
	virtual void TearDown() const override;
	virtual bool IsVR() const override { return true; }
	virtual void Present() const override;
	virtual void PollXREvents() const override;
	virtual bool BeginXRFrame() const override;
	virtual bool AcquireXRSwapchain() const override;
	virtual bool SubmitFrame() const override;
	virtual void AdjustViewport(DFrameBuffer* screen) const override;
	
	virtual bool GetHandTransform(int hand, VSMatrix* out) const override;
	virtual bool RenderPlayerSpritesInScene() const { return true; }
	virtual bool GetTeleportLocation(DVector3 &out) const override;
	virtual void Vibrate(float duration, int channel, float intensity) const override;

    // Vulkan specific multiview setup
    void InitializeMultiview() const;

protected:

	void updateHmdPose(FRenderViewpoint& vp) const;
	bool InitializeOpenXR() const;
	bool CreateSwapchain() const;
	void DestroyOpenXR() const;

	VSMatrix getHUDProjection(int eye) const;

	std::unique_ptr<VKOpenXRDeviceEyePose> mEyes[2];

	mutable bool isSetup;
	mutable bool isOpenXRReady = false;
	mutable bool isSessionRunning = false;
	mutable bool isSessionReadyToBegin = false;
	mutable bool mInVRSceneRender = false;
	mutable uint32_t sceneWidth = 0;
	mutable uint32_t sceneHeight = 0;
	mutable XrInstance xrInstance = XR_NULL_HANDLE;
	mutable XrSystemId xrSystemId = XR_NULL_SYSTEM_ID;
	mutable XrSession xrSession = XR_NULL_HANDLE;
	mutable XrSpace xrSpace = XR_NULL_HANDLE;
	mutable XrSwapchain xrSwapchain = XR_NULL_HANDLE;
	mutable std::shared_ptr<VulkanInstance> xrVkInstance;
	mutable std::shared_ptr<VulkanDevice> xrVkDevice;
	mutable std::unique_ptr<VulkanCommandPool> xrVkCommandPool;
	mutable std::unique_ptr<VulkanCommandBuffer> xrVkCommandBuffer;
	mutable std::unique_ptr<VulkanFence> xrVkSubmitFence;

	mutable XrActionSet xrActionSet = XR_NULL_HANDLE;
	mutable XrAction xrPoseAction = XR_NULL_HANDLE;
	mutable XrSpace xrHandSpaces[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	mutable XrAction xrSelectAction = XR_NULL_HANDLE;
	mutable XrAction xrMenuAction = XR_NULL_HANDLE;

	mutable std::vector<XrViewConfigurationView> xrViewConfigs;
	mutable std::vector<XrView> xrViews;
	mutable std::vector<XrCompositionLayerProjectionView> xrProjectionViews;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrSwapchainImages;
	mutable uint32_t xrViewCount = 0;
	mutable int xrCurrentImageIndex = -1;
	mutable int64_t xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	mutable XrFrameState xrFrameState = { XR_TYPE_FRAME_STATE };
	mutable bool xrFrameInProgress = false;
	mutable bool mSetUpInProgress = false;
	mutable uint64_t xrFrameCounter = 0;
    
private:
	typedef VRMode super;
};

} /* namespace s3d */
