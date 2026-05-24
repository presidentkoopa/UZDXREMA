#pragma once

#include "hw_vrmodes.h"
#include "vulkan/system/vk_renderdevice.h"
#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "zvulkan/vulkanobjects.h"

#include <vector>
#include <memory>

class VkTextureImage;

namespace s3d {

class VKOpenXRDeviceEyePose : public VREyeInfo
{
public:
	friend class VKOpenXRDeviceMode;

	VKOpenXRDeviceEyePose(int eye);
	virtual ~VKOpenXRDeviceEyePose() override;
	virtual VSMatrix GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio, bool iso_ortho) const override;
	virtual VSMatrix GetHUDProjection() const override;
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
	virtual VSMatrix GetHUDProjection() const override;
	virtual void Present() const override;
	virtual void PollXREvents() const override;
	virtual bool BeginXRFrame() const override;
	virtual bool AcquireXRSwapchain() const override;
	virtual bool SubmitFrame() const override;
	virtual void AdjustViewport(DFrameBuffer* screen) const override;
	virtual void AdjustPlayerSprites(FRenderState& state, int hand = 0) const override;
	virtual void UnAdjustPlayerSprites(FRenderState& state) const override;
	virtual void DrawMountedHud(HWDrawInfo* di, FRenderState& state) const override;
	virtual bool IsRenderingVirtualScreen() const override;
	virtual bool RenderVirtualScreen() const override;
	virtual void FinalizeEyeImage(VulkanRenderDevice* fb, int eyeIndex) const override;
	virtual bool RenderDesktopMirror(VulkanRenderDevice* fb, VulkanImage* dstImage) const override;
	bool GetRecommendedRenderSize(int& outWidth, int& outHeight) const;
	
	virtual bool GetHandTransform(int hand, VSMatrix* out) const override;
	virtual bool RenderPlayerSpritesInScene() const { return true; }
	virtual bool GetTeleportLocation(DVector3 &out) const override;
	virtual void Vibrate(float duration, int channel, float intensity) const override;

    // Vulkan specific multiview setup
    void InitializeMultiview() const;

protected:

	void updateHmdPose(FRenderViewpoint& vp) const;
	void UpdateControllerState() const;
	void ProcessHaptics() const;
	void StopHaptics() const;
	bool InitializeOpenXR() const;
	bool CreateSwapchain() const;
	bool CreatePresentTextures(VulkanRenderDevice* fb) const;
	bool CreateVirtualScreenSwapchain(uint32_t width, uint32_t height) const;
	bool CreateVirtualScreenBackdropSwapchain(uint32_t width, uint32_t height) const;
	void DestroyVirtualScreenSwapchain() const;
	void DestroyVirtualScreenBackdropSwapchain() const;
	void DestroyOpenXR() const;

	void updateVirtualScreenLayer() const;
	bool ShouldRenderVirtualScreen() const;
	void PurgeDeferredOpenXRResources() const;

	std::unique_ptr<VKOpenXRDeviceEyePose> mEyes[2];

	mutable bool isSetup;
	mutable bool isOpenXRReady = false;
	mutable bool isSessionRunning = false;
	mutable bool isSessionReadyToBegin = false;
	mutable bool mInVRSceneRender = false;
	mutable bool mInVirtualScreenRender = false;
	mutable uint32_t sceneWidth = 0;
	mutable uint32_t sceneHeight = 0;
	mutable int cachedScreenBlocks = 0;
	mutable XrInstance xrInstance = XR_NULL_HANDLE;
	mutable XrSystemId xrSystemId = XR_NULL_SYSTEM_ID;
	mutable XrSession xrSession = XR_NULL_HANDLE;
	mutable XrSpace xrSpace = XR_NULL_HANDLE;
	mutable bool xrUsingStageSpace = false;
	mutable bool xrHasLocalHeightAnchor = false;
	mutable float xrLocalHeightAnchor = 0.0f;
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
	mutable XrAction xrGripAction = XR_NULL_HANDLE;
	mutable XrAction xrThumbClickAction = XR_NULL_HANDLE;
	mutable XrAction xrThumbstickAction = XR_NULL_HANDLE;
	mutable XrAction xrTrackpadAction = XR_NULL_HANDLE;
	mutable XrAction xrAAction = XR_NULL_HANDLE;
	mutable XrAction xrBAction = XR_NULL_HANDLE;
	mutable XrAction xrXAction = XR_NULL_HANDLE;
	mutable XrAction xrYAction = XR_NULL_HANDLE;
	mutable XrAction xrPrimaryAction = XR_NULL_HANDLE;
	mutable XrAction xrSecondaryAction = XR_NULL_HANDLE;
	mutable XrPath xrLeftHandPath = XR_NULL_PATH;
	mutable XrPath xrRightHandPath = XR_NULL_PATH;
	mutable XrPosef xrHandPoses[2] = { { {0,0,0,1}, {0,0,0} }, { {0,0,0,1}, {0,0,0} } };
	mutable bool xrHandPoseValid[2] = { false, false };
	mutable bool xrLastSelectState[2] = { false, false };
	mutable bool xrLastMenuState[2] = { false, false };
	mutable bool xrLastGripState[2] = { false, false };
	mutable bool xrLastThumbClickState[2] = { false, false };
	mutable bool xrLastTrackpadClickState[2] = { false, false };
	mutable bool xrLastAState[2] = { false, false };
	mutable bool xrLastBState[2] = { false, false };
	mutable bool xrLastXState[2] = { false, false };
	mutable bool xrLastYState[2] = { false, false };
	mutable bool xrLastPrimaryState[2] = { false, false };
	mutable bool xrLastSecondaryState[2] = { false, false };
	mutable XrVector2f xrLastThumbstickState[2] = { {0.0f, 0.0f}, {0.0f, 0.0f} };
	mutable XrVector2f xrLastTrackpadState[2] = { {0.0f, 0.0f}, {0.0f, 0.0f} };
	mutable bool xrLastMenuReturnState = false;
	mutable bool xrLastMenuBackState = false;
	mutable bool xrLastMenuBackspaceState = false;
	mutable bool xrLoggedWeaponState = false;
	mutable XrAction xrHapticAction = XR_NULL_HANDLE;
	mutable double xrHapticDuration[2] = { 0.0, 0.0 };
	mutable float xrHapticIntensity[2] = { 0.0f, 0.0f };
	mutable bool xrHapticActive[2] = { false, false };
	mutable DVector3 m_TeleportLocation = DVector3(0.0, 0.0, 0.0);
	mutable int m_TeleportTarget = 0;

	mutable std::vector<XrViewConfigurationView> xrViewConfigs;
	mutable std::vector<XrView> xrViews;
	mutable std::vector<XrCompositionLayerProjectionView> xrProjectionViews;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrSwapchainImages;
	mutable std::vector<VkTextureImage> xrSwapchainTextures;
	mutable std::vector<VkTextureImage> xrPresentTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredPresentTextures;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrVirtualScreenSwapchainImages;
	mutable std::vector<XrSwapchainImageVulkanKHR> xrVirtualScreenBackdropSwapchainImages;
	mutable std::vector<VkTextureImage> xrVirtualScreenTextures;
	mutable std::vector<VkTextureImage> xrVirtualScreenBackdropTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredVirtualScreenTextures;
	mutable std::vector<std::vector<VkTextureImage>> xrDeferredVirtualScreenBackdropTextures;
	mutable uint32_t xrViewCount = 0;
	mutable int xrCurrentImageIndex = -1;
	mutable int xrVirtualScreenImageIndex = -1;
	mutable int xrVirtualScreenBackdropImageIndex = -1;
	mutable uint32_t xrVirtualScreenWidth = 0;
	mutable uint32_t xrVirtualScreenHeight = 0;
	mutable uint32_t xrPresentWidth = 0;
	mutable uint32_t xrPresentHeight = 0;
	mutable int64_t xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	mutable XrSwapchain xrVirtualScreenSwapchain = XR_NULL_HANDLE;
	mutable XrSwapchain xrVirtualScreenBackdropSwapchain = XR_NULL_HANDLE;
	mutable XrCompositionLayerQuad xrVirtualScreenLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	mutable XrCompositionLayerQuad xrVirtualScreenBackdropLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	mutable XrCompositionLayerEquirectKHR xrVirtualScreenBackdropEquirectLayer{ XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR };
	mutable XrPosef xrVirtualScreenPose{};
	mutable XrPosef xrVirtualScreenBackdropPose{};
	mutable XrFrameState xrFrameState = { XR_TYPE_FRAME_STATE };
	mutable bool xrFrameInProgress = false;
	mutable bool xrVirtualScreenVisible = false;
	mutable bool xrVirtualScreenBackdropVisible = false;
	mutable bool xrLoggedDesktopViewportMismatch = false;
	mutable bool mSetUpInProgress = false;
	mutable uint64_t xrFrameCounter = 0;
	mutable bool xrHasFBColorSpace = false;
	mutable bool xrHasEquirectBackdrop = false;
    
private:
	typedef VRMode super;
};

} /* namespace s3d */
