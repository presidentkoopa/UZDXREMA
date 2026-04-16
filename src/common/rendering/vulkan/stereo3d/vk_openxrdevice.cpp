#include "vk_openxrdevice.h"

#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "zvulkan/vulkanbuilders.h"
#include "zvulkan/vulkancompatibledevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "d_player.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "doomdef.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "common/rendering/hwrenderer/data/hw_viewpointbuffer.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern float hmdorientation[3];
extern float hmdPosition[3];
extern float weaponoffset[3];
extern float weaponangles[3];
extern float offhandoffset[3];
extern float offhandangles[3];
extern float doomYaw;
extern float previousPitch;
extern float playerYaw;
extern bool resetDoomYaw;
extern bool resetPreviousPitch;
extern bool automapactive;
extern bool cinemamode;

EXTERN_CVAR(Float, vr_ipd);
EXTERN_CVAR(Float, vr_vunits_per_meter);
EXTERN_CVAR(Float, vr_height_adjust);
EXTERN_CVAR(Float, vr_automap_stereo);
EXTERN_CVAR(Float, vr_hud_stereo);
EXTERN_CVAR(Float, vr_automap_rotate);
EXTERN_CVAR(Float, vr_hud_rotate);
EXTERN_CVAR(Float, vr_automap_distance);
EXTERN_CVAR(Float, vr_hud_distance);
EXTERN_CVAR(Float, vr_automap_scale);
EXTERN_CVAR(Float, vr_hud_scale);
EXTERN_CVAR(Bool, vr_automap_fixed_roll);
EXTERN_CVAR(Bool, vr_hud_fixed_roll);
EXTERN_CVAR(Bool, vr_automap_fixed_pitch);
EXTERN_CVAR(Bool, vr_hud_fixed_pitch);
EXTERN_CVAR(Bool, vr_automap_use_hud);
EXTERN_CVAR(Int, vr_control_scheme);

namespace s3d {

namespace
{
constexpr XrViewConfigurationType viewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr XrEnvironmentBlendMode environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
XrSessionState xrSessionState = XR_SESSION_STATE_UNKNOWN;

using PFN_xrGetVulkanGraphicsRequirementsKHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
using PFN_xrGetVulkanGraphicsDeviceKHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, VkInstance, VkPhysicalDevice*);

PFN_xrGetVulkanGraphicsRequirementsKHR_t xrGetVulkanGraphicsRequirementsKHR_inst = nullptr;
PFN_xrGetVulkanGraphicsDeviceKHR_t xrGetVulkanGraphicsDeviceKHR_inst = nullptr;

static float DEG2RAD(float deg)
{
	return deg * (float)(M_PI / 180.0);
}

static int mAngleFromRadians(double radians)
{
	return (int)std::round(radians * 65536.0 / (2.0 * M_PI));
}

static VSMatrix BuildOpenXREyeProjection(const XrFovf& fov, float nearZ, float farZ, int eye)
{
	const float tanLeft = std::tan(fov.angleLeft);
	const float tanRight = std::tan(fov.angleRight);
	const float tanUp = std::tan(fov.angleUp);
	const float tanDown = std::tan(fov.angleDown);

	const float left = nearZ * tanLeft;
	const float right = nearZ * tanRight;
	const float bottom = nearZ * tanDown;
	const float top = nearZ * tanUp;
	const float tanWidth = tanRight - tanLeft;
	const float tanHeight = tanUp - tanDown;
	const float skewX = -(tanRight + tanLeft) / tanWidth;
	const float skewY = (tanUp + tanDown) / tanHeight;
	// The frustum already carries the asymmetry in skewX; keep the center shift small in NDC space.
	const float centerOffsetNDC = std::fabs(skewX) * 0.1f;
	const float eyeOffsetX = (eye == 0) ? -centerOffsetNDC : centerOffsetNDC;

	Printf("OpenXR: BuildOpenXREyeProjection fov[L=%.6f R=%.6f U=%.6f D=%.6f] tan[L=%.6f R=%.6f U=%.6f D=%.6f] near=%.6f far=%.6f\n",
		(double)fov.angleLeft, (double)fov.angleRight, (double)fov.angleUp, (double)fov.angleDown,
		(double)tanLeft, (double)tanRight, (double)tanUp, (double)tanDown,
		(double)nearZ, (double)farZ);
	Printf("OpenXR: BuildOpenXREyeProjection width=%.6f height=%.6f skewX=%.6f skewY=%.6f\n",
		(double)tanWidth, (double)tanHeight, (double)skewX, (double)skewY);
	Printf("OpenXR: BuildOpenXREyeProjection eye=%d skewX=%.6f centerOffsetNDC=%.6f eyeOffsetX=%.6f row3col0=%.6f\n",
		eye, (double)skewX, (double)centerOffsetNDC, (double)eyeOffsetX, (double)eyeOffsetX);

	FLOATTYPE m[16];
	memset(m, 0, sizeof(m));
	m[0 * 4 + 0] = 2.0f * nearZ / (right - left);
	m[1 * 4 + 1] = 2.0f * nearZ / (top - bottom);
	m[2 * 4 + 0] = skewX;
	m[2 * 4 + 1] = skewY;
	m[2 * 4 + 2] = - (farZ + nearZ) / (farZ - nearZ);
	m[2 * 4 + 3] = -1.0f;
	m[3 * 4 + 0] = eyeOffsetX;
	m[3 * 4 + 2] = - 2.0f * farZ * nearZ / (farZ - nearZ);
	m[3 * 4 + 3] = 0.0f;

	VSMatrix matrix;
	matrix.loadMatrix(m);
	Printf("OpenXR: BuildOpenXREyeProjection MATRIX DETAIL eye=%d\n", eye);
	Printf("  projection[0][0] (X scale) = %.6f\n", (double)m[0 * 4 + 0]);
	Printf("  projection[1][1] (Y scale) = %.6f\n", (double)m[1 * 4 + 1]);
	Printf("  projection[2][0] (X skew)  = %.6f\n", (double)m[2 * 4 + 0]);
	Printf("  projection[2][1] (Y skew)  = %.6f\n", (double)m[2 * 4 + 1]);
	Printf("  projection[2][2] (Z near)  = %.6f\n", (double)m[2 * 4 + 2]);
	Printf("  projection[2][3] (W)       = %.6f\n", (double)m[2 * 4 + 3]);
	Printf("  projection[3][0] (X trans) = %.6f\n", (double)m[3 * 4 + 0]);
	Printf("  projection[3][2] (Z trans) = %.6f\n", (double)m[3 * 4 + 2]);
	return matrix;
}

void QuaternionToEuler(const XrQuaternionf& q, float& pitch, float& yaw, float& roll)
{
	double q0 = q.w;
	// permute axes to match OpenVR's yaw/pitch/roll convention
	double q2 = q.x;
	double q3 = q.y;
	double q1 = q.z;

	double outRoll = std::atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2));
	double outPitch = std::asin(2.0 * (q0 * q2 - q3 * q1));
	double outYaw = -std::atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3));

	pitch = (float)(outPitch * (180.0 / M_PI));
	yaw = (float)(outYaw * (180.0 / M_PI));
	roll = (float)(outRoll * (180.0 / M_PI));
}

}

static int64_t SelectSwapchainFormat(const std::vector<int64_t>& runtimeFormats, VkFormat preferredFormat)
{
	auto hasFormat = [&](int64_t format) -> bool
	{
		return std::find(runtimeFormats.begin(), runtimeFormats.end(), format) != runtimeFormats.end();
	};

	const int64_t preferred[] = {
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? (int64_t)VK_FORMAT_B8G8R8A8_SRGB : (int64_t)preferredFormat,
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB,
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM,
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_SRGB,
		(int64_t)VK_FORMAT_R8G8B8A8_UNORM
	};

	for (int64_t format : preferred)
	{
		if (format != VK_FORMAT_UNDEFINED && hasFormat(format))
			return format;
	}

	return runtimeFormats.empty() ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : runtimeFormats[0];
}

static VREyeInfo* GetDummyOpenXREyes()
{
	static VREyeInfo eyes[2] = { VREyeInfo(0.0f, 1.0f), VREyeInfo(0.0f, 0.0f) };
	return eyes;
}

VKOpenXRDeviceEyePose::VKOpenXRDeviceEyePose(int eye) : VREyeInfo(0.0f, 1.0f), eye(eye)
{
}
VKOpenXRDeviceEyePose::~VKOpenXRDeviceEyePose() {}

VSMatrix VKOpenXRDeviceEyePose::GetProjection(FLOATTYPE fov, FLOATTYPE aspectRatio, FLOATTYPE fovRatio, bool iso_ortho) const
{
	Printf("OpenXR: GetProjection eye=%d fov=%.1f aspect=%.3f iso=%d rawFov[L=%.6f R=%.6f U=%.6f D=%.6f]\n",
		eye, (double)fov, (double)aspectRatio, (int)iso_ortho,
		(double)currentFov.angleLeft, (double)currentFov.angleRight,
		(double)currentFov.angleUp, (double)currentFov.angleDown);
	(void)fov;
	(void)aspectRatio;
	(void)fovRatio;
	(void)iso_ortho;

	const float nearZ = (float)screen->GetZNear();
	const float farZ = (float)screen->GetZFar();
	projection = BuildOpenXREyeProjection(currentFov, nearZ, farZ, eye);
	return projection;
}

DVector3 VKOpenXRDeviceEyePose::GetViewShift(FRenderViewpoint& vp) const
{
	auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());

	XrVector3f center = { 0, 0, 0 };
	if (mode.xrViewCount > 0)
	{
		for (uint32_t i = 0; i < mode.xrViewCount; ++i)
		{
			center.x += mode.xrViews[i].pose.position.x;
			center.y += mode.xrViews[i].pose.position.y;
			center.z += mode.xrViews[i].pose.position.z;
		}
		center.x /= mode.xrViewCount;
		center.y /= mode.xrViewCount;
		center.z /= mode.xrViewCount;
	}

	const float localX = currentEyePose.position.x - center.x;
	const float localY = currentEyePose.position.y - center.y;
	const float localZ = currentEyePose.position.z - center.z;

	// Convert OpenXR local offset to Doom local unrotated space.
	const float doomLocalX = -localZ * vr_vunits_per_meter;
	const float doomLocalY = -localX * vr_vunits_per_meter;
	const float doomLocalZ = localY * vr_vunits_per_meter;

	const float yaw = (float)(vp.Angles.Yaw.Degrees() * (M_PI / 180.0));
	const float sy = std::sin(yaw);
	const float cy = std::cos(yaw);

	DVector3 shift;
	shift.X = doomLocalX * cy - doomLocalY * sy;
	shift.Y = doomLocalX * sy + doomLocalY * cy;
	shift.Z = doomLocalZ;

	Printf("OpenXR: GetViewShift eye=%d center[x=%.6f y=%.6f z=%.6f] local[x=%.6f y=%.6f z=%.6f] shift[x=%.6f y=%.6f z=%.6f]\n",
		eye, (double)center.x, (double)center.y, (double)center.z,
		(double)localX, (double)localY, (double)localZ,
		shift.X, shift.Y, shift.Z);
	return shift;
}

void VKOpenXRDeviceEyePose::SetUp() const
{
	Printf("OpenXR: SetUp called for eye=%d gamestate=%d menuactive=%d\n",
		eye, (int)gamestate, (int)menuactive);
	volatile int breakpoint = eye;
	(void)breakpoint;
	static thread_local bool inSetUp = false;
	if (inSetUp)
	{
		return;
	}
	inSetUp = true;
	struct Guard
	{
		bool& flag;
		~Guard()
		{
			flag = false;
		}
	} guard{ inSetUp };

	auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
	if (eye >= 0 && (size_t)eye < mode.xrViews.size())
	{
		currentEyePose = mode.xrViews[(size_t)eye].pose;
		currentFov = mode.xrViews[(size_t)eye].fov;
		projection = BuildOpenXREyeProjection(currentFov, (float)screen->GetZNear(), (float)screen->GetZFar(), eye);
	}

	VREyeInfo::SetUp();
	mode.mInVRSceneRender = true;
}

void VKOpenXRDeviceEyePose::TearDown() const
{
	Printf("OpenXR: TearDown called for eye=%d\n", eye);
	VREyeInfo::TearDown();
	if (eye == 1)
	{
		auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
		mode.mInVRSceneRender = false;
	}
}

static void ApplyVPUniforms(HWDrawInfo* di)
{
	auto& renderState = *screen->RenderState();
	di->VPUniforms.CalcDependencies();
	if (screen->mViewpoints)
		di->vpIndex = screen->mViewpoints->SetViewpoint(renderState, &di->VPUniforms);
}

void VKOpenXRDeviceEyePose::AdjustHud() const
{
	Printf("OpenXR: AdjustHud eye=%d\n", eye);
	if (r_viewpoint.ViewLevel == nullptr)
		return;

	const VKOpenXRDeviceMode& mode = (const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance();
	if (mode.mEyeCount == 1) return;

	auto* di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
	di->VPUniforms.mViewMatrix.loadIdentity();
	di->VPUniforms.mProjectionMatrix = mode.getHUDProjection(eye);
	ApplyVPUniforms(di);
	di->EndDrawInfo();
}

void VKOpenXRDeviceEyePose::AdjustBlend(HWDrawInfo* di) const
{
	if (r_viewpoint.ViewLevel == nullptr)
		return;

	bool new_di = false;
	if (di == nullptr)
	{
		di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
		new_di = true;
	}

	auto& renderState = *screen->RenderState();
	const VSMatrix eyeProjection = BuildOpenXREyeProjection(currentFov, (float)screen->GetZNear(), (float)screen->GetZFar(), eye);
	di->VPUniforms.mProjectionMatrix = eyeProjection;
	di->ProjectionMatrix2 = eyeProjection;
	di->VPUniforms.CalcDependencies();
	if (screen->mViewpoints)
	{
		di->vpIndex = screen->mViewpoints->SetViewpoint(renderState, &di->VPUniforms);
		Printf("OpenXR: AdjustBlend eye=%d bound viewpoint index=%d with eyeProjection\n", eye, di->vpIndex);
	}
	else
	{
		Printf("OpenXR: AdjustBlend eye=%d viewpoint buffer unavailable\n", eye);
	}

	VSMatrix finalMatrix = projection;
	di->VPUniforms.mProjectionMatrix = finalMatrix;
	di->ProjectionMatrix2 = finalMatrix;
	di->VPUniforms.CalcDependencies();
	if (screen->mViewpoints)
	{
		di->vpIndex = screen->mViewpoints->SetViewpoint(renderState, &di->VPUniforms);
		Printf("OpenXR: AdjustBlend eye=%d rebound viewpoint index=%d with finalMatrix\n", eye, di->vpIndex);
	}
	{
		const FLOATTYPE* m = finalMatrix.get();
		Printf("OpenXR: AdjustBlend eye=%d finalMatrix[row3col0=%.6f] matrix=[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]\n",
			eye, (double)m[3 * 4 + 0],
			(double)m[0], (double)m[1], (double)m[2], (double)m[3],
			(double)m[4], (double)m[5], (double)m[6], (double)m[7],
			(double)m[8], (double)m[9], (double)m[10], (double)m[11],
			(double)m[12], (double)m[13], (double)m[14], (double)m[15]);
	}
	ApplyVPUniforms(di);

	if (new_di)
	{
		di->EndDrawInfo();
	}
}

VKOpenXRDeviceMode::VKOpenXRDeviceMode()
	: VRMode(2, 1.0f, 1.0f, 1.0f, GetDummyOpenXREyes())
{
	mEyes[0] = std::make_unique<VKOpenXRDeviceEyePose>(0);
	mEyes[1] = std::make_unique<VKOpenXRDeviceEyePose>(1);
	VRMode::mEyes[0] = mEyes[0].get();
	VRMode::mEyes[1] = mEyes[1].get();
	isSetup = false;
}

VKOpenXRDeviceMode::~VKOpenXRDeviceMode()
{
	DestroyOpenXR();
}

const VRMode& VKOpenXRDeviceMode::getInstance()
{
	static VKOpenXRDeviceMode instance;
	return instance;
}

template<class TYPE>
static TYPE& getHUDValue(TYPE& automap, TYPE& hud)
{
	return (automapactive && !vr_automap_use_hud) ? automap : hud;
}

VSMatrix VKOpenXRDeviceMode::getHUDProjection(int eye) const
{
	Printf("OpenXR: getHUDProjection called for eye %d\n", eye);
	VSMatrix new_projection;
	new_projection.loadIdentity();

	float stereo_separation = (vr_ipd * 0.5f) * vr_vunits_per_meter * getHUDValue<FFloatCVarRef>(vr_automap_stereo, vr_hud_stereo) * (eye == 1 ? -1.0f : 1.0f);
	new_projection.translate(stereo_separation, 0, 0);

	new_projection.scale(-vr_vunits_per_meter, vr_vunits_per_meter, -vr_vunits_per_meter);
	double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	new_projection.scale(1.0, pixelstretch, 1.0);

	if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_roll, vr_hud_fixed_roll))
	{
		new_projection.rotate(-hmdorientation[2], 0, 0, 1);
	}

	new_projection.rotate(getHUDValue<FFloatCVarRef>(vr_automap_rotate, vr_hud_rotate), 1, 0, 0);

	if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_pitch, vr_hud_fixed_pitch))
	{
		new_projection.rotate(-hmdorientation[0], 1, 0, 0);
	}

	double distance = getHUDValue<FFloatCVarRef>(vr_automap_distance, vr_hud_distance);
	new_projection.translate(0.0, 0.0, distance);
	double vr_scale = getHUDValue<FFloatCVarRef>(vr_automap_scale, vr_hud_scale);
	new_projection.scale(-vr_scale, vr_scale, -vr_scale);

	new_projection.translate(-1.0, 1.0, 0);
	new_projection.scale(2.0f / (float)SCREENWIDTH, -2.0f / (float)SCREENHEIGHT, -1.0f);

	VSMatrix proj = mEyes[eye]->projection;
	proj.multMatrix(new_projection);
	return proj;
}

bool VKOpenXRDeviceMode::InitializeOpenXR() const
{
	auto fail = [&]() -> bool
	{
		DestroyOpenXR();
		return false;
	};

	if (isOpenXRReady)
		return true;
	if (xrInstance != XR_NULL_HANDLE || xrSession != XR_NULL_HANDLE || xrSwapchain != XR_NULL_HANDLE ||
		xrSpace != XR_NULL_HANDLE || xrActionSet != XR_NULL_HANDLE || xrPoseAction != XR_NULL_HANDLE ||
		xrSelectAction != XR_NULL_HANDLE || xrMenuAction != XR_NULL_HANDLE || xrVkInstance != nullptr ||
		xrVkDevice != nullptr || xrVkCommandPool != nullptr || xrVkCommandBuffer != nullptr ||
		xrVkSubmitFence != nullptr || !xrSwapchainImages.empty() || !xrViewConfigs.empty() ||
		!xrViews.empty() || !xrProjectionViews.empty() || xrViewCount != 0 || sceneWidth != 0 || sceneHeight != 0)
	{
		DestroyOpenXR();
	}
	if (!IsOpenXRPresent())
	{
		return fail();
	}


	std::vector<const char*> extensions = {
		XR_KHR_VULKAN_ENABLE_EXTENSION_NAME
	};
	XrApplicationInfo appInfo{};
	appInfo.apiVersion = XR_API_VERSION_1_0;
	appInfo.applicationVersion = 1;
	appInfo.engineVersion = 1;
	strncpy(appInfo.applicationName, "DoomXR", sizeof(appInfo.applicationName) - 1);
	strncpy(appInfo.engineName, "DoomXR", sizeof(appInfo.engineName) - 1);

	XrInstanceCreateInfo instanceInfo{ XR_TYPE_INSTANCE_CREATE_INFO };
	instanceInfo.applicationInfo = appInfo;
	instanceInfo.enabledExtensionCount = (uint32_t)extensions.size();
	instanceInfo.enabledExtensionNames = extensions.data();
	XrResult xrResult = xrCreateInstance(&instanceInfo, &xrInstance);
	if (XR_FAILED(xrResult))
	{
		return fail();
	}

	auto loadProc = [&](const char* name, PFN_xrVoidFunction* out) -> bool
	{
		if (XR_FAILED(xrGetInstanceProcAddr(xrInstance, name, out)))
		{
			return false;
		}
		return *out != nullptr;
	};

	loadProc("xrGetVulkanGraphicsRequirementsKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsRequirementsKHR_inst));
	loadProc("xrGetVulkanGraphicsDeviceKHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsDeviceKHR_inst));

	XrSystemGetInfo systemInfo{ XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (XR_FAILED(xrGetSystem(xrInstance, &systemInfo, &xrSystemId)))
	{
		return fail();
	}

	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (!vkfb)
	{
		return fail();
	}

	XrGraphicsRequirementsVulkanKHR graphicsRequirements{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR };
	if (xrGetVulkanGraphicsRequirementsKHR_inst)
	{
		if (XR_FAILED(xrGetVulkanGraphicsRequirementsKHR_inst(xrInstance, xrSystemId, &graphicsRequirements)))
		{
			return fail();
		}
	}
	xrVkDevice = vkfb->device;
	xrVkInstance = xrVkDevice ? xrVkDevice->Instance : nullptr;
	if (!xrVkDevice || !xrVkInstance)
	{
		return fail();
	}

	XrGraphicsBindingVulkanKHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
	VkPhysicalDevice xrPhysicalDevice = VK_NULL_HANDLE;
	if (xrGetVulkanGraphicsDeviceKHR_inst)
	{
		if (XR_FAILED(xrGetVulkanGraphicsDeviceKHR_inst(xrInstance, xrSystemId, xrVkInstance->Instance, &xrPhysicalDevice)))
		{
			return fail();
		}
		if (xrVkDevice->PhysicalDevice.Device != xrPhysicalDevice)
		{
			return fail();
		}
	}

	xrVkCommandPool = CommandPoolBuilder()
		.QueueFamily(xrVkDevice->GraphicsFamily)
		.DebugName("OpenXRCommandPool")
		.Create(xrVkDevice.get());
	xrVkCommandBuffer = xrVkCommandPool->createBuffer();
	xrVkCommandBuffer->SetDebugName("OpenXRCommandBuffer");
	xrVkSubmitFence = std::make_unique<VulkanFence>(xrVkDevice.get());

	binding.instance = xrVkInstance->Instance;
	binding.physicalDevice = xrVkDevice->PhysicalDevice.Device;
	binding.device = xrVkDevice->device;
	binding.queueFamilyIndex = xrVkDevice->GraphicsFamily;
	binding.queueIndex = 0;

	XrSessionCreateInfo sessionInfo{ XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = xrSystemId;
	xrResult = xrCreateSession(xrInstance, &sessionInfo, &xrSession);
	if (XR_FAILED(xrResult))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		xrResult = xrCreateSession(xrInstance, &sessionInfo, &xrSession);
		if (XR_FAILED(xrResult))
		{
			return fail();
		}
	}

	XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace = XrPosef{ {0,0,0,1}, {0,0,0} };
	if (XR_FAILED(xrCreateReferenceSpace(xrSession, &spaceInfo, &xrSpace)))
	{
		return fail();
	}

	// Action setup
	XrActionSetCreateInfo actionSetInfo{ XR_TYPE_ACTION_SET_CREATE_INFO };
	strncpy(actionSetInfo.actionSetName, "gameplay", sizeof(actionSetInfo.actionSetName) - 1);
	strncpy(actionSetInfo.localizedActionSetName, "Gameplay", sizeof(actionSetInfo.localizedActionSetName) - 1);
	xrCreateActionSet(xrInstance, &actionSetInfo, &xrActionSet);

	auto createAction = [&](const char* name, const char* localizedName, XrActionType type, XrAction& out)
	{
		XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
		actionInfo.actionType = type;
		strncpy(actionInfo.actionName, name, sizeof(actionInfo.actionName) - 1);
		strncpy(actionInfo.localizedActionName, localizedName, sizeof(actionInfo.localizedActionName) - 1);
		xrCreateAction(xrActionSet, &actionInfo, &out);
	};

	createAction("hand_pose", "Hand Pose", XR_ACTION_TYPE_POSE_INPUT, xrPoseAction);
	createAction("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT, xrSelectAction);
	createAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, xrMenuAction);

	XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &xrActionSet;
	xrAttachSessionActionSets(xrSession, &attachInfo);

	for (int i = 0; i < 2; ++i)
	{
		XrActionSpaceCreateInfo actionSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		actionSpaceInfo.action = xrPoseAction;
		actionSpaceInfo.subactionPath = XR_NULL_PATH; 
		actionSpaceInfo.poseInActionSpace = XrPosef{ {0,0,0,1}, {0,0,0} };
		xrCreateActionSpace(xrSession, &actionSpaceInfo, &xrHandSpaces[i]);
	}

	uint32_t viewCount = 0;
	xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, viewType, 0, &viewCount, nullptr);
	xrViewCount = viewCount;
	xrViewConfigs.resize(viewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
	xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, viewType, viewCount, &viewCount, xrViewConfigs.data());
	xrViews.resize(viewCount, { XR_TYPE_VIEW });
	xrProjectionViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });

	sceneWidth = xrViewConfigs[0].recommendedImageRectWidth;
	sceneHeight = xrViewConfigs[0].recommendedImageRectHeight;

	isOpenXRReady = true;
	return true;
}

bool VKOpenXRDeviceMode::CreateSwapchain() const
{
	if (xrSwapchain != XR_NULL_HANDLE)
		return true;

	if (!isOpenXRReady)
		return false;

	const auto& cfg = xrViewConfigs[0];
	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	VkFormat preferredFormat = VK_FORMAT_B8G8R8A8_UNORM;
	if (vkfb && vkfb->GetFramebufferManager() && vkfb->GetFramebufferManager()->SwapChain)
		preferredFormat = vkfb->GetFramebufferManager()->SwapChain->Format().format;

	uint32_t formatCount = 0;
	std::vector<int64_t> runtimeFormats;
	if (XR_SUCCEEDED(xrEnumerateSwapchainFormats(xrSession, 0, &formatCount, nullptr)) && formatCount > 0)
	{
		runtimeFormats.resize(formatCount);
		xrEnumerateSwapchainFormats(xrSession, formatCount, &formatCount, runtimeFormats.data());
	}
	xrSwapchainFormat = SelectSwapchainFormat(runtimeFormats, preferredFormat);

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = xrSwapchainFormat;
	swapchainInfo.sampleCount = cfg.recommendedSwapchainSampleCount;
	swapchainInfo.width = cfg.recommendedImageRectWidth;
	swapchainInfo.height = cfg.recommendedImageRectHeight;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = xrViewCount;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrSwapchain)))
	{
		return false;
	}

	uint32_t imageCount = 0;
	xrEnumerateSwapchainImages(xrSwapchain, 0, &imageCount, nullptr);
	xrSwapchainImages.resize(imageCount);
	for (auto& image : xrSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	xrEnumerateSwapchainImages(xrSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrSwapchainImages.data()));

	return true;
}

void VKOpenXRDeviceMode::DestroyOpenXR() const
{
	if (xrSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrSwapchain);
		xrSwapchain = XR_NULL_HANDLE;
	}
	if (xrSpace != XR_NULL_HANDLE)
	{
		xrDestroySpace(xrSpace);
		xrSpace = XR_NULL_HANDLE;
	}
	for (auto& handSpace : xrHandSpaces)
	{
		if (handSpace != XR_NULL_HANDLE)
		{
			xrDestroySpace(handSpace);
			handSpace = XR_NULL_HANDLE;
		}
	}
	if (xrActionSet != XR_NULL_HANDLE)
	{
		xrDestroyActionSet(xrActionSet);
		xrActionSet = XR_NULL_HANDLE;
	}
	if (xrSession != XR_NULL_HANDLE)
	{
		xrDestroySession(xrSession);
		xrSession = XR_NULL_HANDLE;
	}
	if (xrInstance != XR_NULL_HANDLE)
	{
		xrDestroyInstance(xrInstance);
		xrInstance = XR_NULL_HANDLE;
	}
	isOpenXRReady = false;
	isSessionRunning = false;
	isSessionReadyToBegin = false;
	xrSessionState = XR_SESSION_STATE_UNKNOWN;
	xrFrameInProgress = false;
	isSetup = false;
	xrPoseAction = XR_NULL_HANDLE;
	xrSelectAction = XR_NULL_HANDLE;
	xrMenuAction = XR_NULL_HANDLE;
	xrSwapchainImages.clear();
	xrViewConfigs.clear();
	xrViews.clear();
	xrProjectionViews.clear();
	xrViewCount = 0;
	xrCurrentImageIndex = -1;
	xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	xrFrameState = { XR_TYPE_FRAME_STATE };
	sceneWidth = 0;
	sceneHeight = 0;
	xrVkSubmitFence.reset();
	xrVkCommandBuffer.reset();
	xrVkCommandPool.reset();
	xrVkDevice.reset();
	xrVkInstance.reset();
}

void VKOpenXRDeviceMode::SetUp() const
{
	struct Guard
	{
		bool& flag;
		bool entered;

		explicit Guard(bool& inFlag) : flag(inFlag), entered(false)
		{
			if (!flag)
			{
				flag = true;
				entered = true;
			}
		}

		explicit operator bool() const
		{
			return entered;
		}

		~Guard()
		{
			if (entered)
				flag = false;
		}
	} guard{ mSetUpInProgress };

	if (!guard)
		return;

	static int setupCallCount = 0;
	setupCallCount++;
	if (!isSetup)
	{
		if (!InitializeOpenXR()) return;
		if (!CreateSwapchain()) return;
		isSetup = true;
	}

	if (xrSession == XR_NULL_HANDLE) return;
	PollXREvents();

	if (isSessionRunning)
	{
		updateHmdPose(r_viewpoint);
	}
}

void VKOpenXRDeviceMode::PollXREvents() const
{
	if (xrInstance == XR_NULL_HANDLE)
		return;

	XrEventDataBuffer eventData{ XR_TYPE_EVENT_DATA_BUFFER };
	XrResult result = xrPollEvent(xrInstance, &eventData);
	if (result == XR_EVENT_UNAVAILABLE || !XR_SUCCEEDED(result))
		return;

	if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
	{
		auto* ev = reinterpret_cast<XrEventDataSessionStateChanged*>(&eventData);
		if (ev->session == xrSession)
		{
			xrSessionState = ev->state;
			if (ev->state == XR_SESSION_STATE_READY)
			{
				isSessionReadyToBegin = true;
			}
			else if (ev->state == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(xrSession);
				isSessionRunning = false;
				isSessionReadyToBegin = false;
			}
			else if (ev->state == XR_SESSION_STATE_LOSS_PENDING || ev->state == XR_SESSION_STATE_EXITING)
			{
				DestroyOpenXR();
			}
			else
			{
			}
		}
	}
}

void VKOpenXRDeviceMode::updateHmdPose(FRenderViewpoint& vp) const
{
	if (xrViews.empty()) return;

	XrVector3f pos = { 0, 0, 0 };
	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		pos.x += xrViews[i].pose.position.x;
		pos.y += xrViews[i].pose.position.y;
		pos.z += xrViews[i].pose.position.z;
	}
	pos.x /= xrViewCount;
	pos.y /= xrViewCount;
	pos.z /= xrViewCount;

	hmdPosition[0] = pos.x;
	hmdPosition[1] = pos.y;
	hmdPosition[2] = pos.z;

	float p, y, r;
	QuaternionToEuler(xrViews[0].pose.orientation, p, y, r);

	hmdorientation[0] = -p;
	hmdorientation[1] = -y;
	hmdorientation[2] = -r;

	if (gamestate != GS_LEVEL || menuactive != MENU_Off || r_viewpoint.camera == nullptr || r_viewpoint.ViewLevel == nullptr)
		return;

	static float previousHmdYaw = 0;
	static bool havePreviousYaw = false;
	const float currentHmdYaw = hmdorientation[1];
	if (!havePreviousYaw)
	{
		previousHmdYaw = currentHmdYaw;
		doomYaw = r_viewpoint.Angles.Yaw.Degrees();
		havePreviousYaw = true;
	}
	float hmdYawDeltaDegrees = currentHmdYaw - previousHmdYaw;
	G_AddViewAngle(mAngleFromRadians((float)DEG2RAD(-hmdYawDeltaDegrees)));
	previousHmdYaw = currentHmdYaw;

	if (gamestate == GS_LEVEL && menuactive == MENU_Off)
	{
		doomYaw += hmdYawDeltaDegrees;
		vp.HWAngles.Roll = FAngle::fromDeg(-r);
		vp.HWAngles.Pitch = FAngle::fromDeg(-p);
		double viewYaw = doomYaw;
		while (viewYaw <= -180.0) viewYaw += 360.0;
		while (viewYaw > 180.0) viewYaw -= 360.0;
		vp.Angles.Yaw = DAngle::fromDeg(viewYaw);
	}
}

void VKOpenXRDeviceMode::TearDown() const {}

bool VKOpenXRDeviceMode::SubmitFrame() const
{
	if (!BeginXRFrame())
		return false;
	return AcquireXRSwapchain();
}

bool VKOpenXRDeviceMode::BeginXRFrame() const
{
	const uint64_t nextFrame = xrFrameCounter + 1;
	++xrFrameCounter;

	if (xrSession == XR_NULL_HANDLE || xrSwapchain == XR_NULL_HANDLE || xrVkDevice == nullptr)
		return false;

	if (gamestate == GS_LEVEL && (r_viewpoint.camera == nullptr || r_viewpoint.ViewLevel == nullptr))
	{
		return false;
	}

	if (isSessionReadyToBegin && !isSessionRunning)
	{
		XrSessionBeginInfo beginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
		beginInfo.primaryViewConfigurationType = viewType;
		XrResult r = xrBeginSession(xrSession, &beginInfo);
		if (XR_SUCCEEDED(r))
			isSessionRunning = true;
		else
			return false;
		isSessionReadyToBegin = false;
	}

	if (!isSessionRunning)
		return false;

	if (xrFrameInProgress)
	{
		return false;
	}

	XrFrameWaitInfo waitInfo{ XR_TYPE_FRAME_WAIT_INFO };
	XrResult xrResult = xrWaitFrame(xrSession, &waitInfo, &xrFrameState);
	if (XR_FAILED(xrResult))
		return false;

	XrFrameBeginInfo beginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
	xrResult = xrBeginFrame(xrSession, &beginInfo);
	if (XR_FAILED(xrResult))
		return false;

	XrViewLocateInfo locateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = viewType;
	locateInfo.displayTime = xrFrameState.predictedDisplayTime;
	locateInfo.space = xrSpace;
	XrViewState viewState{ XR_TYPE_VIEW_STATE };
	uint32_t viewCount = xrViewCount;
	if (viewCount == 0 || xrViews.size() < viewCount || xrProjectionViews.size() < viewCount)
		return false;
	xrResult = xrLocateViews(xrSession, &locateInfo, &viewState, viewCount, &viewCount, xrViews.data());
	if (XR_FAILED(xrResult))
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		xrFrameInProgress = false;
		return false;
	}
	if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 || (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
	{
	}

	updateHmdPose(r_viewpoint);
	xrFrameInProgress = true;
	return true;
}

bool VKOpenXRDeviceMode::AcquireXRSwapchain() const
{

	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (!xrFrameInProgress)
	{
		return true;
	}

	xrFrameInProgress = false;

	if (!vkfb || xrSession == XR_NULL_HANDLE || xrSwapchain == XR_NULL_HANDLE || !isSessionRunning || xrVkDevice == nullptr || xrVkCommandBuffer == nullptr)
		return true;

	auto* framebufferManager = vkfb->GetFramebufferManager();
	auto* postprocess = vkfb->GetPostprocess();
	const bool shouldSubmitProjectionLayer = xrFrameState.shouldRender;
	const bool sessionVisibleOrFocused = xrSessionState == XR_SESSION_STATE_VISIBLE || xrSessionState == XR_SESSION_STATE_FOCUSED;

	if (!shouldSubmitProjectionLayer || !sessionVisibleOrFocused)
	{
		if (!sessionVisibleOrFocused)
			Printf("OpenXR frame %llu: submission skipped - session state=%d.\n", (unsigned long long)xrFrameCounter, (int)xrSessionState);
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (empty layer).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		Printf("OpenXR frame %llu: shouldRender=%d presentImageIndex=%d gamestate=%d xrEndFrame=%d.\n",
			(unsigned long long)xrFrameCounter,
			xrFrameState.shouldRender ? 1 : 0,
			framebufferManager ? framebufferManager->PresentImageIndex : -999,
			(int)gamestate,
			(int)endResult);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}

	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t imageIndex = 0;
	XrResult xrResult = xrAcquireSwapchainImage(xrSwapchain, &acquireInfo, &imageIndex);
	if (XR_FAILED(xrResult))
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (acquire failed).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		xrFrameInProgress = false;
		return false;
	}

	XrSwapchainImageWaitInfo imageWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	imageWaitInfo.timeout = 20 * 1000 * 1000; // 20 ms
	xrResult = xrWaitSwapchainImage(xrSwapchain, &imageWaitInfo);
	if (xrResult == XR_TIMEOUT_EXPIRED)
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (wait timeout).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}
	if (XR_FAILED(xrResult))
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (wait failed).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		xrFrameInProgress = false;
		return false;
	}

	auto* buffers = vkfb->GetBuffers();
	if (buffers == nullptr || postprocess == nullptr)
	{
		Printf("OpenXR frame %llu: blit SKIPPED reason: missing postprocess source.\n", (unsigned long long)xrFrameCounter);
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (missing source buffer).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		return XR_SUCCEEDED(endResult);
	}

	VkImage dstImage = xrSwapchainImages[imageIndex].image;
	int32_t srcW = (int32_t)vkfb->mScreenViewport.width;
	int32_t srcH = (int32_t)vkfb->mScreenViewport.height;
	int32_t dstW = (int32_t)xrViewConfigs[0].recommendedImageRectWidth;
	int32_t dstH = (int32_t)xrViewConfigs[0].recommendedImageRectHeight;
	if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
	{
		Printf("OpenXR frame %llu: blit skipped - invalid image dimensions src=%dx%d dst=%dx%d.\n",
			(unsigned long long)xrFrameCounter, srcW, srcH, dstW, dstH);
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (invalid dimensions).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}
	Printf("OpenXR frame %llu: about to blit XR layers to dstImage=%p\n",
		(unsigned long long)xrFrameCounter, (void*)dstImage);

	vkResetCommandPool(xrVkDevice->device, xrVkCommandPool->pool, 0);
	xrVkCommandBuffer->begin();

	VkImageMemoryBarrier dstBarrier{};
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcAccessMask = 0;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.image = dstImage;
	dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, xrViewCount };
	vkCmdPipelineBarrier(xrVkCommandBuffer->buffer,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

	for (uint32_t layer = 0; layer < xrViewCount; ++layer)
	{
		int eyePipelineImage = (int)layer;
		// Use Eye 0 for 2D-only states (boot/main menu) where 3D scene doesn't render.
		if (gamestate != GS_LEVEL)
		{
			eyePipelineImage = 0;
		}
		if (eyePipelineImage < 0 || eyePipelineImage >= VkRenderBuffers::NumPipelineImages || buffers->PipelineImage[eyePipelineImage].Image == nullptr)
		{
			Printf("OpenXR frame %llu: Skipping eye %u - invalid pipeline image %d\n",
				(unsigned long long)xrFrameCounter, layer, eyePipelineImage);
			continue;
		}

		VkTextureImage& eyeSourceImage = buffers->PipelineImage[eyePipelineImage];
		VkImage srcImage = eyeSourceImage.Image->image;

		VkImageMemoryBarrier srcBarrier{};
		srcBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		srcBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		srcBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		srcBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		srcBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcBarrier.image = srcImage;
		srcBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(xrVkCommandBuffer->buffer,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &srcBarrier);

		Printf("OpenXR frame %llu: eye %u blitting from pipeline image %d (%p)\n",
			(unsigned long long)xrFrameCounter, layer, eyePipelineImage, (void*)srcImage);

		VkImageBlit blitRegion{};
		blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blitRegion.srcOffsets[0] = { 0, srcH, 0 };
		blitRegion.srcOffsets[1] = { srcW, 0, 1 };
		blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
		blitRegion.dstOffsets[0] = { 0, 0, 0 };
		blitRegion.dstOffsets[1] = { dstW, dstH, 1 };
		vkCmdBlitImage(xrVkCommandBuffer->buffer,
			srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blitRegion, VK_FILTER_LINEAR);

		VkImageMemoryBarrier srcRestoreBarrier{};
		srcRestoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		srcRestoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		srcRestoreBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		srcRestoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		srcRestoreBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		srcRestoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcRestoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		srcRestoreBarrier.image = srcImage;
		srcRestoreBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(xrVkCommandBuffer->buffer,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			0, 0, nullptr, 0, nullptr, 1, &srcRestoreBarrier);

		eyeSourceImage.Layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	}

	VkImageMemoryBarrier dstFinalBarrier{};
	dstFinalBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstFinalBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstFinalBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	dstFinalBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstFinalBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	dstFinalBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstFinalBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstFinalBarrier.image = dstImage;
	dstFinalBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, xrViewCount };
	vkCmdPipelineBarrier(xrVkCommandBuffer->buffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstFinalBarrier);

	xrVkCommandBuffer->end();

	vkResetFences(xrVkDevice->device, 1, &xrVkSubmitFence->fence);
	VkCommandBuffer cmdBuf = xrVkCommandBuffer->buffer;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmdBuf;
	VkResult submitResult = vkQueueSubmit(xrVkDevice->GraphicsQueue, 1, &submitInfo, xrVkSubmitFence->fence);
	if (submitResult != VK_SUCCESS)
	{
		Printf("OpenXR frame %llu: blit submit failed.\n", (unsigned long long)xrFrameCounter);
		return false;
	}
	VkResult waitResult = vkWaitForFences(xrVkDevice->device, 1, &xrVkSubmitFence->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
	if (waitResult != VK_SUCCESS)
	{
		Printf("OpenXR frame %llu: blit fence wait failed.\n", (unsigned long long)xrFrameCounter);
		return false;
	}
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrResult = xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);
	Printf("OpenXR frame %llu: xrReleaseSwapchainImage=%d.\n", (unsigned long long)xrFrameCounter, (int)xrResult);
	if (XR_FAILED(xrResult))
	{
		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (release failed).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		xrFrameInProgress = false;
		return false;
	}

	XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	layer.space = xrSpace;
	layer.viewCount = xrViewCount;
	layer.views = xrProjectionViews.data();

	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		const uint32_t viewIndex = (i == 0) ? 1 : 0;
		const uint32_t arrayIndex = i;
		xrProjectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		xrProjectionViews[i].pose = xrViews[viewIndex].pose;
		xrProjectionViews[i].fov = xrViews[viewIndex].fov;
		xrProjectionViews[i].subImage.swapchain = xrSwapchain;
		xrProjectionViews[i].subImage.imageArrayIndex = arrayIndex;
		xrProjectionViews[i].subImage.imageRect.offset = { 0, 0 };
		xrProjectionViews[i].subImage.imageRect.extent = { (int32_t)xrViewConfigs[i].recommendedImageRectWidth, (int32_t)xrViewConfigs[i].recommendedImageRectHeight };
	}

	const XrCompositionLayerBaseHeader* layers[] = { (const XrCompositionLayerBaseHeader*)&layer };
	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = xrFrameState.predictedDisplayTime;
	endInfo.environmentBlendMode = environmentBlendMode;
	endInfo.layerCount = 1;
	endInfo.layers = layers;
	XrResult endResult = xrEndFrame(xrSession, &endInfo);
	xrFrameInProgress = false;
	return XR_SUCCEEDED(endResult);
}

void VKOpenXRDeviceMode::Present() const
{
	Printf("OpenXR: Present called\n");
}

void VKOpenXRDeviceMode::AdjustViewport(DFrameBuffer* screen) const
{
	Printf("OpenXR: AdjustViewport called screen=%p sceneWidth=%u sceneHeight=%u inVRScene=%d\n", (void*)screen, sceneWidth, sceneHeight, (int)mInVRSceneRender);
	if (screen == nullptr) return;
	if (sceneWidth == 0 || sceneHeight == 0) return;
	if (!mInVRSceneRender) return;
	screen->mSceneViewport.width = sceneWidth;
	screen->mSceneViewport.height = sceneHeight;
	screen->mSceneViewport.left = 0;
	screen->mSceneViewport.top = 0;
}

bool VKOpenXRDeviceMode::GetHandTransform(int hand, VSMatrix* mat) const
{
	double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;
	if (player)
	{
		mat->loadIdentity();
		mat->translate((float)r_viewpoint.CenterEyePos.X, (float)r_viewpoint.CenterEyePos.Z - (float)player->DefaultViewHeight(), (float)r_viewpoint.CenterEyePos.Y);
		mat->scale((float)vr_vunits_per_meter, (float)vr_vunits_per_meter, (float)-vr_vunits_per_meter);

		float* offset = (hand == 1) ? weaponoffset : offhandoffset;
		float* angles = (hand == 1) ? weaponangles : offhandangles;

		mat->translate(-offset[0], (hmdPosition[1] + offset[1] + (float)vr_height_adjust) / (float)pixelstretch, offset[2]);
		mat->scale(1, 1 / (float)pixelstretch, 1);

		mat->rotate(-90 + doomYaw + (angles[1] - hmdorientation[1]), 0, 1, 0);
		mat->rotate(angles[0], 1, 0, 0);
		mat->rotate(angles[2], 0, 0, 1);
		return true;
	}
	return false;
}

bool VKOpenXRDeviceMode::GetTeleportLocation(DVector3 &out) const { return false; }
void VKOpenXRDeviceMode::Vibrate(float duration, int channel, float intensity) const {}
void VKOpenXRDeviceMode::InitializeMultiview() const {}

} // namespace s3d
