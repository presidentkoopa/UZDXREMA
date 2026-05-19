#include "vk_openxrdevice.h"

#include "common/rendering/stereo3d/openxr/oxr_loader.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/textures/vk_imagetransition.h"
#include "vulkan/textures/vk_renderbuffers.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "zvulkan/vulkanbuilders.h"
#include "zvulkan/vulkancompatibledevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "d_player.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "doomdef.h"
#include "c_console.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "common/rendering/hwrenderer/data/hw_viewpointbuffer.h"
#include "v_draw.h"

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
void QzDoom_setUseScreenLayer(bool use);

EXTERN_CVAR(Float, vr_ipd);
EXTERN_CVAR(Float, vr_vunits_per_meter);
EXTERN_CVAR(Float, vr_height_adjust);
EXTERN_CVAR(Float, vr_openxr_fov_adjust_deg);
EXTERN_CVAR(Float, vr_openxr_eye_shift_scale);
EXTERN_CVAR(Bool, vr_debug_projection_compare);
EXTERN_CVAR(Bool, vr_openxr_debug_sizes);
EXTERN_CVAR(Bool, vr_openxr_debug_present);
EXTERN_CVAR(Int, screenblocks);
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
EXTERN_CVAR(Int, vr_desktop_view);
EXTERN_CVAR(Bool, vr_swap_eyes);
EXTERN_CVAR(Bool, vr_automap_use_hud);
EXTERN_CVAR(Int, vr_overlayscreen);
EXTERN_CVAR(Bool, vr_overlayscreen_always);
EXTERN_CVAR(Float, vr_overlayscreen_size);
EXTERN_CVAR(Float, vr_overlayscreen_dist);
EXTERN_CVAR(Float, vr_overlayscreen_vpos);
EXTERN_CVAR(Int, vr_overlayscreen_bg);
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

static bool HasOpenXRExtension(const char* extensionName)
{
	uint32_t extensionCount = 0;
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr)) || extensionCount == 0)
		return false;

	std::vector<XrExtensionProperties> extensions(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data())))
		return false;

	for (const auto& extension : extensions)
	{
		if (strcmp(extension.extensionName, extensionName) == 0)
			return true;
	}

	return false;
}

static XrColorSpaceFB SelectPreferredColorSpace(const std::vector<XrColorSpaceFB>& supportedColorSpaces)
{
	const XrColorSpaceFB preferredOrder[] = {
		// Match the OpenVR/OpenGL handoff as closely as possible: submit the
		// engine's already-presented LDR output without asking the runtime to
		// reinterpret it into a managed display color space first.
		XR_COLOR_SPACE_UNMANAGED_FB,
		XR_COLOR_SPACE_REC709_FB,
		XR_COLOR_SPACE_RIFT_S_FB,
		XR_COLOR_SPACE_QUEST_FB,
		XR_COLOR_SPACE_P3_FB,
		XR_COLOR_SPACE_REC2020_FB,
	};

	for (XrColorSpaceFB preferred : preferredOrder)
	{
		if (std::find(supportedColorSpaces.begin(), supportedColorSpaces.end(), preferred) != supportedColorSpaces.end())
			return preferred;
	}

	return supportedColorSpaces.empty() ? XR_COLOR_SPACE_UNMANAGED_FB : supportedColorSpaces.front();
}

static float DEG2RAD(float deg)
{
	return deg * (float)(M_PI / 180.0);
}

static XrVector3f RotateVector(const XrQuaternionf& q, const XrVector3f& v)
{
	XrVector3f result;

	const float qx = q.x;
	const float qy = q.y;
	const float qz = q.z;
	const float qw = q.w;

	const float tx = 2.0f * (qy * v.z - qz * v.y);
	const float ty = 2.0f * (qz * v.x - qx * v.z);
	const float tz = 2.0f * (qx * v.y - qy * v.x);

	result.x = v.x + qw * tx + (qy * tz - qz * ty);
	result.y = v.y + qw * ty + (qz * tx - qx * tz);
	result.z = v.z + qw * tz + (qx * ty - qy * tx);
	return result;
}

static XrVector3f AddVector(const XrVector3f& a, const XrVector3f& b)
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static XrVector3f SubtractVector(const XrVector3f& a, const XrVector3f& b)
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}

static XrVector3f ScaleVector(const XrVector3f& v, float scale)
{
	return { v.x * scale, v.y * scale, v.z * scale };
}

static XrQuaternionf MultiplyQuaternion(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
}

static XrQuaternionf ConjugateQuaternion(const XrQuaternionf& q)
{
	return { -q.x, -q.y, -q.z, q.w };
}

static XrQuaternionf MakeAxisAngleQuaternion(const XrVector3f& axis, float angleRadians)
{
	const float halfAngle = angleRadians * 0.5f;
	const float s = std::sin(halfAngle);
	const float c = std::cos(halfAngle);
	return { axis.x * s, axis.y * s, axis.z * s, c };
}

static int mAngleFromRadians(double radians)
{
	return (int)std::round(radians * 65536.0 / (2.0 * M_PI));
}

static float GetRawHmdHeightInMapUnit()
{
	const double pixelstretch = level.info ? level.info->pixelstretch : 1.2;
	return (float)(((double)hmdPosition[1] + (double)vr_height_adjust) * (double)vr_vunits_per_meter / pixelstretch);
}

static float GetHmdAdjustedHeightInMapUnit(bool applyLocalAnchor, float localHeightAnchor)
{
	const float rawHeight = GetRawHmdHeightInMapUnit();
	return applyLocalAnchor ? (rawHeight + localHeightAnchor) : rawHeight;
}

static float GetHmdAdjustedHeightInMapUnit()
{
	return GetHmdAdjustedHeightInMapUnit(false, 0.0f);
}

static float GetDoomPlayerHeightWithoutCrouch(const player_t* player)
{
	static float cachedHeight = 0.0f;
	if (cachedHeight == 0.0f && player != nullptr)
	{
		cachedHeight = player->DefaultViewHeight();
	}
	return cachedHeight != 0.0f ? cachedHeight : GetHmdAdjustedHeightInMapUnit();
}

static float GetViewpointYaw()
{
	if (cinemamode)
		return (float)r_viewpoint.Angles.Yaw.Degrees();
	return doomYaw;
}

struct XrSafeSourceRect
{
	IntRect rect = { 0, 0, 0, 0 };
	float scaleX = 1.0f;
	float scaleY = -1.0f;
	float offsetX = 0.0f;
	float offsetY = 1.0f;
	bool usedFallback = false;
	bool wasClamped = false;
};

static void GetStableOpenXRVirtualScreenSize(uint32_t& width, uint32_t& height)
{
	// Keep the OpenXR menu/console surface independent from the live framebuffer
	// size. The OpenVR reference path uses a stable virtual-screen surface, and
	// resizing this overlay to arbitrary desktop dimensions was triggering the
	// freeze you observed once the resolution got large.
	width = 960;
	height = 720;
}

static XrSafeSourceRect GetSafeXrSourceRect(VulkanRenderDevice* vkfb)
{
	XrSafeSourceRect result;
	auto* buffers = vkfb ? vkfb->GetBuffers() : nullptr;
	const int srcBufferW = buffers ? buffers->GetWidth() : 0;
	const int srcBufferH = buffers ? buffers->GetHeight() : 0;
	const IntRect requestedRect = vkfb ? vkfb->mSceneViewport : IntRect{ 0, 0, 0, 0 };

	auto useFullBufferFallback = [&]()
	{
		result.rect.left = 0;
		result.rect.top = 0;
		result.rect.width = std::max(1, srcBufferW);
		result.rect.height = std::max(1, srcBufferH);
		result.usedFallback = true;
	};

	if (srcBufferW <= 0 || srcBufferH <= 0 || requestedRect.width <= 0 || requestedRect.height <= 0)
	{
		useFullBufferFallback();
	}
	else
	{
		const int requestedLeft = requestedRect.left;
		const int requestedTop = requestedRect.top;
		const int requestedRight = requestedRect.left + requestedRect.width;
		const int requestedBottom = requestedRect.top + requestedRect.height;

		const int clampedLeft = std::clamp(requestedLeft, 0, srcBufferW);
		const int clampedTop = std::clamp(requestedTop, 0, srcBufferH);
		const int clampedRight = std::clamp(requestedRight, 0, srcBufferW);
		const int clampedBottom = std::clamp(requestedBottom, 0, srcBufferH);

		if (clampedRight <= clampedLeft || clampedBottom <= clampedTop)
		{
			useFullBufferFallback();
		}
		else
		{
			result.rect.left = clampedLeft;
			result.rect.top = clampedTop;
			result.rect.width = clampedRight - clampedLeft;
			result.rect.height = clampedBottom - clampedTop;
			result.wasClamped = clampedLeft != requestedLeft || clampedTop != requestedTop ||
				clampedRight != requestedRight || clampedBottom != requestedBottom;
		}
	}

	if (srcBufferW > 0 && srcBufferH > 0)
	{
		result.scaleX = result.rect.width / (float)srcBufferW;
		result.scaleY = -result.rect.height / (float)srcBufferH;
		result.offsetX = result.rect.left / (float)srcBufferW;
		result.offsetY = (result.rect.top + result.rect.height) / (float)srcBufferH;
	}

	return result;
}

static void AngleVectors(const float angles[3], float* forward, float* right, float* up)
{
	const float pitch = (float)(angles[0] * (M_PI / 180.0f));
	const float yaw = (float)(angles[1] * (M_PI / 180.0f));
	const float roll = (float)(angles[2] * (M_PI / 180.0f));

	const float sp = std::sin(pitch);
	const float cp = std::cos(pitch);
	const float sy = std::sin(yaw);
	const float cy = std::cos(yaw);
	const float sr = std::sin(roll);
	const float cr = std::cos(roll);

	if (forward != nullptr)
	{
		forward[0] = cp * cy;
		forward[1] = cp * sy;
		forward[2] = -sp;
	}

	if (right != nullptr)
	{
		right[0] = (-sr * sp * cy) + (cr * sy);
		right[1] = (-sr * sp * sy) - (cr * cy);
		right[2] = -sr * cp;
	}

	if (up != nullptr)
	{
		up[0] = (cr * sp * cy) + (sr * sy);
		up[1] = (cr * sp * sy) - (sr * cy);
		up[2] = cr * cp;
	}
}

static VSMatrix BuildOpenXREyeProjection(const XrFovf& fov, float nearZ, float farZ, int eye)
{
	(void)eye;

	const float fovAdjust = DEG2RAD(clamp<float>(vr_openxr_fov_adjust_deg, -30.0f, 30.0f));
	const XrFovf adjustedFov = {
		std::max(fov.angleLeft - fovAdjust, (float)(-0.5 * M_PI + 0.001)),
		std::min(fov.angleRight + fovAdjust, (float)(0.5 * M_PI - 0.001)),
		std::min(fov.angleUp + fovAdjust, (float)(0.5 * M_PI - 0.001)),
		std::max(fov.angleDown - fovAdjust, (float)(-0.5 * M_PI + 0.001))
	};

	const float tanLeft = std::tan(adjustedFov.angleLeft);
	const float tanRight = std::tan(adjustedFov.angleRight);
	const float tanUp = std::tan(adjustedFov.angleUp);
	const float tanDown = std::tan(adjustedFov.angleDown);
	const float tanWidth = tanRight - tanLeft;
	const float tanHeight = tanUp - tanDown;

	FLOATTYPE m[16];
	memset(m, 0, sizeof(m));

	// Use the OpenXR SDK's asymmetric frustum layout, but keep the engine's
	// existing projection convention. Feeding raw Vulkan clip-space here flips
	// the 3D scene because the renderer's matrix path still expects the
	// OpenGL-style Y/depth form at this stage.
	m[0] = 2.0f / tanWidth;
	m[5] = 2.0f / tanHeight;
	// Doom's existing view/projection path expects the horizontal asymmetric
	// center term with the opposite sign from the raw OpenXR helper output.
	// Using the SDK sign here makes the scene diverge/cross-eye while the rest
	// of the layer pipeline remains correct.
	m[8] = -(tanRight + tanLeft) / tanWidth;
	m[9] = (tanUp + tanDown) / tanHeight;
	m[10] = -(farZ + nearZ) / (farZ - nearZ);
	m[11] = -1.0f;
	m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);

	VSMatrix matrix;
	matrix.loadMatrix(m);
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

static const float overlayBG[6][3] = {
	{ 0.0f, 0.0f, 0.0f },
	{ 0.11f, 0.0f, 0.01f },
	{ 0.0f, 0.11f, 0.02f },
	{ 0.0f, 0.02f, 0.11f },
	{ 0.0f, 0.11f, 0.10f },
	{ 0.10f, 0.10f, 0.10f }
};

static XrVector3f GetVirtualScreenBackgroundColor()
{
	const int idx = clamp<int>(vr_overlayscreen_bg, 0, 5);
	return { overlayBG[idx][0], overlayBG[idx][1], overlayBG[idx][2] };
}

}

static int64_t SelectSwapchainFormat(const std::vector<int64_t>& runtimeFormats, VkFormat preferredFormat)
{
	auto hasFormat = [&](int64_t format) -> bool
	{
		return std::find(runtimeFormats.begin(), runtimeFormats.end(), format) != runtimeFormats.end();
	};

	const VkFormat preferredSrgb =
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? VK_FORMAT_B8G8R8A8_SRGB :
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB :
		VK_FORMAT_UNDEFINED;
	const VkFormat preferredUnorm =
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_UNORM :
		VK_FORMAT_UNDEFINED;

	const int64_t preferred[] = {
		(int64_t)preferredSrgb,
		(preferredSrgb == VK_FORMAT_B8G8R8A8_SRGB) ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB,
		(preferredSrgb == VK_FORMAT_R8G8B8A8_SRGB) ? (int64_t)VK_FORMAT_B8G8R8A8_SRGB : (int64_t)VK_FORMAT_R8G8B8A8_SRGB,
		(int64_t)preferredUnorm,
		(preferredUnorm == VK_FORMAT_B8G8R8A8_UNORM) ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM,
		(preferredUnorm == VK_FORMAT_R8G8B8A8_UNORM) ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_UNORM
	};

	for (int64_t format : preferred)
	{
		if (format != VK_FORMAT_UNDEFINED && hasFormat(format))
			return format;
	}

	return runtimeFormats.empty() ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : runtimeFormats[0];
}

static bool IsSRGBSwapchainFormat(VkFormat format)
{
	return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
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
	const float hmdHeight = GetHmdAdjustedHeightInMapUnit(mode.xrUsingStageSpace ? false : mode.xrHasLocalHeightAnchor, mode.xrLocalHeightAnchor);
	const float playerHeight = (r_viewpoint.camera && r_viewpoint.camera->player) ? GetDoomPlayerHeightWithoutCrouch(r_viewpoint.camera->player) : hmdHeight;

	DVector3 shift;
	if (eye >= 0)
	{
		// Isolation step: remove OpenXR per-eye camera translation entirely and
		// let the asymmetric projection matrix carry scene stereo by itself.
		// If comfort changes here, the remaining issue is in eye translation;
		// if not, the frustum math is still the real problem.
		shift.X = 0.0;
		shift.Y = 0.0;
		shift.Z = 0.0;
		shift.Z += hmdHeight - playerHeight;
	}
	else
	{
		float angles[3];
		angles[0] = vp.HWAngles.Pitch.Degrees();
		angles[1] = GetViewpointYaw();
		angles[2] = vp.HWAngles.Roll.Degrees();

		float v_forward[3], v_right[3], v_up[3];
		AngleVectors(angles, v_forward, v_right, v_up);

		const float stereoSeparation = (vr_ipd * 0.5f) * vr_vunits_per_meter * (eye == 0 ? -1.0f : 1.0f);
		shift.X = v_right[0] * stereoSeparation;
		shift.Y = v_right[1] * stereoSeparation;
		shift.Z = v_right[2] * stereoSeparation;
		shift.Z += hmdHeight - playerHeight;
	}

	if (vr_debug_projection_compare)
	{
		static bool loggedShift[2] = { false, false };
		if (eye >= 0 && eye < 2 && !loggedShift[eye])
		{
			Printf("VR_PROJ OpenXR eye=%d shift=(%.3f, %.3f, %.3f) hmdPos=(%.3f, %.3f, %.3f) hmdHeight=%.3f playerHeight=%.3f stage=%d\n",
				eye, (double)shift.X, (double)shift.Y, (double)shift.Z,
				(double)hmdPosition[0], (double)hmdPosition[1], (double)hmdPosition[2],
				hmdHeight, playerHeight, mode.xrUsingStageSpace ? 1 : 0);
			loggedShift[eye] = true;
		}
	}

	return shift;
}

void VKOpenXRDeviceEyePose::SetUp() const
{
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

		static bool loggedEye[2] = { false, false };
		if (vr_debug_projection_compare && eye >= 0 && eye < 2 && !loggedEye[eye])
		{
			const float tanLeft = std::tan(currentFov.angleLeft);
			const float tanRight = std::tan(currentFov.angleRight);
			const float tanUp = std::tan(currentFov.angleUp);
			const float tanDown = std::tan(currentFov.angleDown);
			const float horizontalFovDeg = (float)((currentFov.angleRight - currentFov.angleLeft) * (180.0 / M_PI));
			const float verticalFovDeg = (float)((currentFov.angleUp - currentFov.angleDown) * (180.0 / M_PI));
			Printf("VR_PROJ OpenXR eye=%d fovL=%.3f fovR=%.3f fovU=%.3f fovD=%.3f hFov=%.3f vFov=%.3f\n",
				eye,
				currentFov.angleLeft * (180.0f / (float)M_PI),
				currentFov.angleRight * (180.0f / (float)M_PI),
				currentFov.angleUp * (180.0f / (float)M_PI),
				currentFov.angleDown * (180.0f / (float)M_PI),
				horizontalFovDeg,
				verticalFovDeg);
			Printf("VR_PROJ OpenXR eye=%d tan=[%.6f %.6f %.6f %.6f] proj=[%.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f | %.6f %.6f %.6f %.6f]\n",
				eye,
				tanLeft, tanRight, tanUp, tanDown,
				projection.get()[0], projection.get()[1], projection.get()[2], projection.get()[3],
				projection.get()[4], projection.get()[5], projection.get()[6], projection.get()[7],
				projection.get()[8], projection.get()[9], projection.get()[10], projection.get()[11],
				projection.get()[12], projection.get()[13], projection.get()[14], projection.get()[15]);
			loggedEye[eye] = true;
		}
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
	if (r_viewpoint.ViewLevel == nullptr)
		return;
	if (VR_ShouldDrawMountedHud())
		return;
	VSMatrix hudProj = GetHUDProjection();
	const float hudStereo = (automapactive && !vr_automap_use_hud) ? (float)vr_automap_stereo : (float)vr_hud_stereo;
	const float orthoWidth = 2.0f;
	const float stereoSeparation = (eye == 0 ? 1.0f : -1.0f) * vr_ipd * hudStereo * 0.1f * orthoWidth;

	auto* di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
	if (di)
	{
		di->VPUniforms.mViewMatrix.translate(stereoSeparation, 0.0f, 0.0f);
		di->VPUniforms.mProjectionMatrix = hudProj;
		di->ProjectionMatrix2 = hudProj;
		di->VPUniforms.CalcDependencies();
		if (screen->mViewpoints)
		{
			ApplyVPUniforms(di);
		}
		di->EndDrawInfo();
	}
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
	}

	VSMatrix finalMatrix = projection;
	di->VPUniforms.mProjectionMatrix = finalMatrix;
	di->ProjectionMatrix2 = finalMatrix;
	di->VPUniforms.CalcDependencies();
	if (screen->mViewpoints)
	{
		di->vpIndex = screen->mViewpoints->SetViewpoint(renderState, &di->VPUniforms);
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

VSMatrix VKOpenXRDeviceEyePose::GetHUDProjection() const
{
	VSMatrix hudProjection;
	hudProjection.loadIdentity();

	const float hudStereo = getHUDValue<FFloatCVarRef>(vr_automap_stereo, vr_hud_stereo);
	const float orthoWidth = 2.0f;
	const float stereoSeparation = (eye == 0 ? 1.0f : -1.0f) * vr_ipd * hudStereo * 0.1f * orthoWidth;

	hudProjection.scale(-vr_vunits_per_meter, vr_vunits_per_meter, -vr_vunits_per_meter);

	const double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	hudProjection.scale(1.0, (FLOATTYPE)pixelstretch, 1.0);

	if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_roll, vr_hud_fixed_roll))
	{
		hudProjection.rotate(-hmdorientation[2], 0, 0, 1);
	}

	hudProjection.rotate(getHUDValue<FFloatCVarRef>(vr_automap_rotate, vr_hud_rotate), 1, 0, 0);

	if (getHUDValue<FBoolCVarRef>(vr_automap_fixed_pitch, vr_hud_fixed_pitch))
	{
		hudProjection.rotate(-hmdorientation[0], 1, 0, 0);
	}

	hudProjection.translate(0.0f, 0.0f, getHUDValue<FFloatCVarRef>(vr_automap_distance, vr_hud_distance));
	const float hudScale = getHUDValue<FFloatCVarRef>(vr_automap_scale, vr_hud_scale);
	hudProjection.scale(-hudScale, hudScale, -hudScale);

	const float screenWidth = (float)screen->GetWidth();
	const float screenHeight = (float)screen->GetHeight();
	hudProjection.translate(-1.0f, 1.0f, 0.0f);
	hudProjection.scale(2.0f / screenWidth, -2.0f / screenHeight, -1.0f);

	VSMatrix projection;
	projection.loadIdentity();
	const float hudOrthoScale = 1.0f;
	projection.ortho(-hudOrthoScale, hudOrthoScale, -hudOrthoScale, hudOrthoScale, 0.5f, 65536.0f);

	VSMatrix finalProjection(projection);
	return finalProjection;
}

VSMatrix VKOpenXRDeviceMode::GetHUDProjection() const
{
	for (int i = 0; i < 2; ++i)
	{
		if (mEyes[i] != nullptr && mEyes[i]->isActive())
		{
			return mEyes[i]->GetHUDProjection();
		}
	}
	return GetHUDSpriteProjection();
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
	xrHasFBColorSpace = HasOpenXRExtension(XR_FB_COLOR_SPACE_EXTENSION_NAME);
	if (xrHasFBColorSpace)
	{
		extensions.push_back(XR_FB_COLOR_SPACE_EXTENSION_NAME);
	}
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

	if (xrHasFBColorSpace && xrEnumerateColorSpacesFB && xrSetColorSpaceFB)
	{
		uint32_t colorSpaceCount = 0;
		if (XR_SUCCEEDED(xrEnumerateColorSpacesFB(xrSession, 0, &colorSpaceCount, nullptr)) && colorSpaceCount > 0)
		{
			std::vector<XrColorSpaceFB> supportedColorSpaces(colorSpaceCount);
			if (XR_SUCCEEDED(xrEnumerateColorSpacesFB(xrSession, colorSpaceCount, &colorSpaceCount, supportedColorSpaces.data())))
			{
				const XrColorSpaceFB requestedColorSpace = SelectPreferredColorSpace(supportedColorSpaces);
				if (XR_SUCCEEDED(xrSetColorSpaceFB(xrSession, requestedColorSpace)))
				{
					Printf("OpenXR: requested FB color space %d from %u supported modes.\n", (int)requestedColorSpace, colorSpaceCount);
				}
			}
		}
	}

	XrReferenceSpaceCreateInfo spaceInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.poseInReferenceSpace = XrPosef{ {0,0,0,1}, {0,0,0} };
	xrUsingStageSpace = false;
	xrSpace = XR_NULL_HANDLE;

	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	if (XR_SUCCEEDED(xrCreateReferenceSpace(xrSession, &spaceInfo, &xrSpace)))
	{
		xrUsingStageSpace = true;
		Printf("OpenXR: using STAGE reference space.\n");
	}
	else
	{
		spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		if (XR_FAILED(xrCreateReferenceSpace(xrSession, &spaceInfo, &xrSpace)))
		{
			return fail();
		}
		Printf("OpenXR: using LOCAL reference space fallback.\n");
	}

	if (xrSpace == XR_NULL_HANDLE)
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
	Printf("OpenXR: preferred scene swapchain format=%d selected=%d srgb=%d runtimeFormats=%u.\n",
		(int)preferredFormat,
		(int)xrSwapchainFormat,
		IsSRGBSwapchainFormat((VkFormat)xrSwapchainFormat) ? 1 : 0,
		formatCount);

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

	xrSwapchainTextures.resize(imageCount * xrViewCount);
	for (uint32_t imageIndex = 0; imageIndex < imageCount; ++imageIndex)
	{
		for (uint32_t layer = 0; layer < xrViewCount; ++layer)
		{
			auto& texture = xrSwapchainTextures[imageIndex * xrViewCount + layer];
			texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
			texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrSwapchainImages[imageIndex].image, nullptr,
				(int)cfg.recommendedImageRectWidth, (int)cfg.recommendedImageRectHeight, 1, (int)xrViewCount);
			texture.View = ImageViewBuilder()
				.Image(texture.Image.get(), (VkFormat)xrSwapchainFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, (int)layer, 1, 1)
				.DebugName("OpenXRSwapchainLayerView")
				.Create(xrVkDevice.get());
		}
	}

	if (!xrPresentTextures.empty() && xrPresentTextures[0].Image != nullptr)
	{
		xrPresentWidth = (uint32_t)xrPresentTextures[0].Image->width;
		xrPresentHeight = (uint32_t)xrPresentTextures[0].Image->height;
	}

	return true;
}

bool VKOpenXRDeviceMode::CreatePresentTextures(VulkanRenderDevice* vkfb) const
{
	if (!vkfb || xrSwapchainFormat == VK_FORMAT_UNDEFINED || xrViewCount == 0)
		return false;

	const uint32_t width = xrViewConfigs.empty() ? 0 : xrViewConfigs[0].recommendedImageRectWidth;
	const uint32_t height = xrViewConfigs.empty() ? 0 : xrViewConfigs[0].recommendedImageRectHeight;
	if (width == 0 || height == 0)
		return false;

	if (xrPresentTextures.size() == xrViewCount && xrPresentWidth == width && xrPresentHeight == height)
		return true;

	if (!xrPresentTextures.empty())
		xrDeferredPresentTextures.emplace_back(std::move(xrPresentTextures));
	xrPresentTextures.clear();

	for (auto& texture : xrPresentTextures)
		texture.Reset(vkfb);

	xrPresentTextures.resize(xrViewCount);
	xrPresentWidth = width;
	xrPresentHeight = height;

	const VkFormat presentFormat = (VkFormat)xrSwapchainFormat;
	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		auto& texture = xrPresentTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = ImageBuilder()
			.Format(presentFormat)
			.Size(width, height)
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			.DebugName("OpenXRPresentTexture")
			.Create(vkfb->device.get());
		texture.View = ImageViewBuilder()
			.Image(texture.Image.get(), presentFormat)
			.DebugName("OpenXRPresentTextureView")
			.Create(vkfb->device.get());
	}

	if (!xrPresentTextures.empty() && xrPresentTextures[0].Image != nullptr)
	{
		xrPresentWidth = (uint32_t)xrPresentTextures[0].Image->width;
		xrPresentHeight = (uint32_t)xrPresentTextures[0].Image->height;
	}

	return true;
}

bool VKOpenXRDeviceMode::CreateVirtualScreenSwapchain(uint32_t width, uint32_t height) const
{
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrSwapchainFormat == VK_FORMAT_UNDEFINED || width == 0 || height == 0)
		return false;
	if (xrVirtualScreenSwapchain != XR_NULL_HANDLE &&
		xrVirtualScreenWidth == width &&
		xrVirtualScreenHeight == height &&
		!xrVirtualScreenTextures.empty())
	{
		return true;
	}

	if (!xrVirtualScreenTextures.empty())
		xrDeferredVirtualScreenTextures.emplace_back(std::move(xrVirtualScreenTextures));
	xrVirtualScreenTextures.clear();

	DestroyVirtualScreenSwapchain();

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = xrSwapchainFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = width;
	swapchainInfo.height = height;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrVirtualScreenSwapchain)))
	{
		Printf("OpenXR: failed to create virtual screen swapchain %ux%u.\n", width, height);
		xrVirtualScreenVisible = false;
		return false;
	}

	uint32_t imageCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenSwapchain, 0, &imageCount, nullptr)) || imageCount == 0)
	{
		DestroyVirtualScreenSwapchain();
		xrVirtualScreenVisible = false;
		return false;
	}

	xrVirtualScreenSwapchainImages.resize(imageCount);
	for (auto& image : xrVirtualScreenSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrVirtualScreenSwapchainImages.data()))))
	{
		DestroyVirtualScreenSwapchain();
		xrVirtualScreenVisible = false;
		return false;
	}
	xrVirtualScreenSwapchainImages.resize(imageCount);

	xrVirtualScreenTextures.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		auto& texture = xrVirtualScreenTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrVirtualScreenSwapchainImages[i].image, nullptr,
			(int)width, (int)height, 1, 1);
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), (VkFormat)xrSwapchainFormat)
			.DebugName("OpenXR.VirtualScreenView")
			.Create(xrVkDevice.get());
	}

	xrVirtualScreenWidth = width;
	xrVirtualScreenHeight = height;
	Printf("OpenXR: created virtual screen swapchain %ux%u with %u images.\n", width, height, imageCount);
	return true;
}

bool VKOpenXRDeviceMode::CreateVirtualScreenBackdropSwapchain(uint32_t width, uint32_t height) const
{
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrSwapchainFormat == VK_FORMAT_UNDEFINED || width == 0 || height == 0)
		return false;
	if (xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE &&
		xrVirtualScreenWidth == width &&
		xrVirtualScreenHeight == height &&
		!xrVirtualScreenBackdropTextures.empty())
	{
		return true;
	}

	if (!xrVirtualScreenBackdropTextures.empty())
		xrDeferredVirtualScreenBackdropTextures.emplace_back(std::move(xrVirtualScreenBackdropTextures));
	xrVirtualScreenBackdropTextures.clear();

	DestroyVirtualScreenBackdropSwapchain();

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	swapchainInfo.format = xrSwapchainFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = width;
	swapchainInfo.height = height;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrVirtualScreenBackdropSwapchain)))
	{
		Printf("OpenXR: failed to create virtual screen backdrop swapchain %ux%u.\n", width, height);
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	uint32_t imageCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenBackdropSwapchain, 0, &imageCount, nullptr)) || imageCount == 0)
	{
		DestroyVirtualScreenBackdropSwapchain();
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	xrVirtualScreenBackdropSwapchainImages.resize(imageCount);
	for (auto& image : xrVirtualScreenBackdropSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrVirtualScreenBackdropSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrVirtualScreenBackdropSwapchainImages.data()))))
	{
		DestroyVirtualScreenBackdropSwapchain();
		xrVirtualScreenBackdropVisible = false;
		return false;
	}
	xrVirtualScreenBackdropSwapchainImages.resize(imageCount);

	xrVirtualScreenBackdropTextures.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		auto& texture = xrVirtualScreenBackdropTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrVirtualScreenBackdropSwapchainImages[i].image, nullptr,
			(int)width, (int)height, 1, 1);
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), (VkFormat)xrSwapchainFormat)
			.DebugName("OpenXR.VirtualScreenBackdropView")
			.Create(xrVkDevice.get());
	}

	xrVirtualScreenBackdropVisible = true;
	return true;
}

void VKOpenXRDeviceMode::DestroyVirtualScreenSwapchain() const
{
	xrVirtualScreenVisible = false;
	xrVirtualScreenImageIndex = -1;
	xrVirtualScreenTextures.clear();
	xrVirtualScreenSwapchainImages.clear();
	if (xrVirtualScreenSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrVirtualScreenSwapchain);
		xrVirtualScreenSwapchain = XR_NULL_HANDLE;
	}
	xrVirtualScreenWidth = 0;
	xrVirtualScreenHeight = 0;
}

void VKOpenXRDeviceMode::DestroyVirtualScreenBackdropSwapchain() const
{
	xrVirtualScreenBackdropVisible = false;
	xrVirtualScreenBackdropImageIndex = -1;
	xrVirtualScreenBackdropTextures.clear();
	xrVirtualScreenBackdropSwapchainImages.clear();
	if (xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrVirtualScreenBackdropSwapchain);
		xrVirtualScreenBackdropSwapchain = XR_NULL_HANDLE;
	}
}

void VKOpenXRDeviceMode::DestroyOpenXR() const
{
	DestroyVirtualScreenSwapchain();
	DestroyVirtualScreenBackdropSwapchain();
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
	xrHasFBColorSpace = false;
	xrUsingStageSpace = false;
	xrHasLocalHeightAnchor = false;
	xrLocalHeightAnchor = 0.0f;
	xrPoseAction = XR_NULL_HANDLE;
	xrSelectAction = XR_NULL_HANDLE;
	xrMenuAction = XR_NULL_HANDLE;
	xrSwapchainImages.clear();
	xrSwapchainTextures.clear();
	xrPresentTextures.clear();
	xrDeferredPresentTextures.clear();
	xrViewConfigs.clear();
	xrViews.clear();
	xrProjectionViews.clear();
	xrViewCount = 0;
	xrCurrentImageIndex = -1;
	xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	xrPresentWidth = 0;
	xrPresentHeight = 0;
	xrFrameState = { XR_TYPE_FRAME_STATE };
	sceneWidth = 0;
	sceneHeight = 0;
	xrLoggedDesktopViewportMismatch = false;
	xrVkSubmitFence.reset();
	xrVkCommandBuffer.reset();
	xrVkCommandPool.reset();
	xrVkDevice.reset();
	xrVkInstance.reset();
	xrDeferredVirtualScreenTextures.clear();
	xrDeferredVirtualScreenBackdropTextures.clear();
}

void VKOpenXRDeviceMode::PurgeDeferredOpenXRResources() const
{
	xrDeferredPresentTextures.clear();
	xrDeferredVirtualScreenTextures.clear();
	xrDeferredVirtualScreenBackdropTextures.clear();
}

void VKOpenXRDeviceMode::SetUp() const
{
	PurgeDeferredOpenXRResources();
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

	if (gamestate == GS_LEVEL && menuactive == MENU_Off)
	{
		cachedScreenBlocks = screenblocks;
		screenblocks = 12;
		QzDoom_setUseScreenLayer(false);
	}
	else
	{
		QzDoom_setUseScreenLayer(true);
	}

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

	if (!xrUsingStageSpace && !xrHasLocalHeightAnchor && r_viewpoint.camera && r_viewpoint.camera->player)
	{
		xrLocalHeightAnchor = GetDoomPlayerHeightWithoutCrouch(r_viewpoint.camera->player) - GetRawHmdHeightInMapUnit();
		xrHasLocalHeightAnchor = true;
	}

	if (gamestate != GS_LEVEL || menuactive != MENU_Off || r_viewpoint.camera == nullptr || r_viewpoint.ViewLevel == nullptr)
		return;

	if (vr_debug_projection_compare)
	{
		const float pixelstretch = r_viewpoint.ViewLevel ? (float)r_viewpoint.ViewLevel->pixelstretch : 1.2f;
		const player_t* player = r_viewpoint.camera->player;
		const double playerHeight = player ? GetDoomPlayerHeightWithoutCrouch(player) : 0.0;
		const double rawHeight = GetRawHmdHeightInMapUnit();
		const double adjustedHeight = GetHmdAdjustedHeightInMapUnit(xrUsingStageSpace ? false : xrHasLocalHeightAnchor, xrLocalHeightAnchor);
		Printf("VR_PROJ OpenXR pose: hmdY=%.3f rawHmd=%.3f adjHmd=%.3f anchor=%.3f playerHeight=%.3f vupm=%.3f eyeShiftScale=%.3f pixelstretch=%.3f vpPos=(%.3f, %.3f, %.3f) yaw=%.3f pitch=%.3f roll=%.3f stage=%d\n",
			hmdPosition[1], rawHeight, adjustedHeight, (double)xrLocalHeightAnchor, playerHeight, (double)vr_vunits_per_meter, (double)vr_openxr_eye_shift_scale, pixelstretch,
			vp.Pos.X, vp.Pos.Y, vp.Pos.Z,
			vp.Angles.Yaw.Degrees(), vp.HWAngles.Pitch.Degrees(), vp.HWAngles.Roll.Degrees(),
			xrUsingStageSpace ? 1 : 0);
	}

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

void VKOpenXRDeviceMode::TearDown() const
{
	if (cachedScreenBlocks != 0 && gamestate == GS_LEVEL && menuactive == MENU_Off && !paused)
	{
		screenblocks = cachedScreenBlocks;
		cachedScreenBlocks = 0;
	}
}

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
	for (uint32_t i = 0; i < viewCount; ++i)
	{
	}
	if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 || (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0)
	{
	}

	updateHmdPose(r_viewpoint);
	xrVirtualScreenVisible = false;
	xrVirtualScreenBackdropVisible = false;
	xrVirtualScreenImageIndex = -1;
	xrVirtualScreenBackdropImageIndex = -1;
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

	const uint32_t recommendedW = xrViewConfigs.empty() ? 0 : xrViewConfigs[0].recommendedImageRectWidth;
	const uint32_t recommendedH = xrViewConfigs.empty() ? 0 : xrViewConfigs[0].recommendedImageRectHeight;
	const uint32_t dstW = xrPresentWidth != 0 ? xrPresentWidth : recommendedW;
	const uint32_t dstH = xrPresentHeight != 0 ? xrPresentHeight : recommendedH;
	if (dstW == 0 || dstH == 0)
	{
		Printf("OpenXR frame %llu: blit skipped - invalid XR destination dimensions present=%ux%u recommended=%ux%u.\n",
			(unsigned long long)xrFrameCounter, dstW, dstH, recommendedW, recommendedH);
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrSwapchain, &releaseInfo);

		XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
		endInfo.displayTime = xrFrameState.predictedDisplayTime;
		endInfo.environmentBlendMode = environmentBlendMode;
		endInfo.layerCount = 0;
		endInfo.layers = nullptr;
		Printf("OpenXR frame %llu: Before xrEndFrame (invalid XR destination dimensions).\n", (unsigned long long)xrFrameCounter);
		XrResult endResult = xrEndFrame(xrSession, &endInfo);
		Printf("OpenXR frame %llu: After xrEndFrame (%d).\n", (unsigned long long)xrFrameCounter, (int)endResult);
		xrFrameInProgress = false;
		return XR_SUCCEEDED(endResult);
	}

	if (vr_openxr_debug_sizes)
	{
		Printf("XR_SIZES frame=%llu desktopVP=%dx%d screenVP=%dx%d sceneVP=%dx%d fb=%dx%d present=%ux%u recommended=%ux%u\n",
			(unsigned long long)xrFrameCounter,
			vkfb->mOutputLetterbox.width, vkfb->mOutputLetterbox.height,
			vkfb->mScreenViewport.width, vkfb->mScreenViewport.height,
			vkfb->mSceneViewport.width, vkfb->mSceneViewport.height,
			buffers->GetWidth(), buffers->GetHeight(),
			dstW, dstH,
			recommendedW, recommendedH);
	}

	if (!xrLoggedDesktopViewportMismatch)
	{
		const bool desktopDiffers = (vkfb->mOutputLetterbox.width != (int)dstW) || (vkfb->mOutputLetterbox.height != (int)dstH);
		const bool screenDiffers = (vkfb->mScreenViewport.width != (int)dstW) || (vkfb->mScreenViewport.height != (int)dstH);
		if (desktopDiffers || screenDiffers)
		{
			Printf("OpenXR: desktop viewport differs from XR sizing desktopVP=%dx%d screenVP=%dx%d sceneVP=%dx%d present=%ux%u recommended=%ux%u (XR blits stay on XR sizes)\n",
				vkfb->mOutputLetterbox.width, vkfb->mOutputLetterbox.height,
				vkfb->mScreenViewport.width, vkfb->mScreenViewport.height,
				vkfb->mSceneViewport.width, vkfb->mSceneViewport.height,
				dstW, dstH,
				recommendedW, recommendedH);
			xrLoggedDesktopViewportMismatch = true;
		}
	}
	Printf("OpenXR frame %llu: about to draw XR layers to swapchain image %u.\n",
		(unsigned long long)xrFrameCounter, imageIndex);

	vkResetCommandPool(xrVkDevice->device, xrVkCommandPool->pool, 0);
	xrVkCommandBuffer->begin();

	VkImage dstImage = xrSwapchainImages[imageIndex].image;
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
	vkCmdPipelineBarrier(
		xrVkCommandBuffer->buffer,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

	for (uint32_t layer = 0; layer < xrViewCount; ++layer)
	{
		// Intentional eye-order correction for this Vulkan/OpenXR bridge.
		const uint32_t sourceEye = xrViewCount - 1 - layer;
		if (sourceEye >= xrPresentTextures.size() || xrPresentTextures[sourceEye].Image == nullptr)
		{
			Printf("OpenXR frame %llu: Skipping eye %u - missing prepared eye image %u\n",
				(unsigned long long)xrFrameCounter, layer, sourceEye);
			continue;
		}

		auto& preparedEyeImage = xrPresentTextures[sourceEye];
		const auto* preparedEyeTexture = preparedEyeImage.Image.get();
		const int32_t srcW = preparedEyeTexture ? preparedEyeTexture->width : 0;
		const int32_t srcH = preparedEyeTexture ? preparedEyeTexture->height : 0;
		if (srcW <= 0 || srcH <= 0)
		{
			Printf("OpenXR frame %llu: Skipping eye %u - invalid prepared eye image %u extent=%dx%d\n",
				(unsigned long long)xrFrameCounter, layer, sourceEye, srcW, srcH);
			continue;
		}

		if (vr_openxr_debug_present)
		{
			Printf("XR_PRESENT_MAP eye=%u sourceEye=%u srcExtent=%dx%d dstExtent=%ux%u\n",
				layer, sourceEye, srcW, srcH, dstW, dstH);
		}

		VkImageTransition()
			.AddImage(&preparedEyeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(xrVkCommandBuffer.get());

		VkImageBlit blitRegion{};
		blitRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		blitRegion.srcOffsets[0] = { 0, 0, 0 };
		blitRegion.srcOffsets[1] = { srcW, srcH, 1 };
		blitRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1 };
		blitRegion.dstOffsets[0] = { 0, 0, 0 };
		blitRegion.dstOffsets[1] = { (int32_t)dstW, (int32_t)dstH, 1 };
		vkCmdBlitImage(
			xrVkCommandBuffer->buffer,
			preparedEyeImage.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blitRegion, VK_FILTER_LINEAR);

		VkImageTransition()
			.AddImage(&preparedEyeImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(xrVkCommandBuffer.get());
	}

	VkImageMemoryBarrier dstRestoreBarrier{};
	dstRestoreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstRestoreBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstRestoreBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	dstRestoreBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstRestoreBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
	dstRestoreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstRestoreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstRestoreBarrier.image = dstImage;
	dstRestoreBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, xrViewCount };
	vkCmdPipelineBarrier(
		xrVkCommandBuffer->buffer,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstRestoreBarrier);

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
		const uint32_t viewIndex = i;
		const uint32_t arrayIndex = i;
		xrProjectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		xrProjectionViews[i].pose = xrViews[viewIndex].pose;
		xrProjectionViews[i].fov = xrViews[viewIndex].fov;
		xrProjectionViews[i].subImage.swapchain = xrSwapchain;
		xrProjectionViews[i].subImage.imageArrayIndex = arrayIndex;
		xrProjectionViews[i].subImage.imageRect.offset = { 0, 0 };
		xrProjectionViews[i].subImage.imageRect.extent = { (int32_t)xrViewConfigs[i].recommendedImageRectWidth, (int32_t)xrViewConfigs[i].recommendedImageRectHeight };
	}

	XrCompositionLayerQuad backdropLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad quadLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	bool submitVirtualScreen = xrVirtualScreenVisible && xrVirtualScreenSwapchain != XR_NULL_HANDLE && xrVirtualScreenImageIndex >= 0;
	bool submitBackdrop = submitVirtualScreen && xrVirtualScreenBackdropVisible && xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE && xrVirtualScreenBackdropImageIndex >= 0;

	const XrCompositionLayerBaseHeader* layers[3];
	int layerIndex = 0;
	// Always keep the projection layer alive. The virtual-screen path is an
	// overlay layer, not a replacement for the headset scene. Replacing the
	// projection layer made the whole HMD depend on the menu quad pass and could
	// drop straight into SteamVR's waiting screen if that overlay path failed.
	layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&layer;

	if (submitVirtualScreen)
	{
		if (submitBackdrop)
		{
			backdropLayer = xrVirtualScreenBackdropLayer;
			backdropLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			backdropLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			backdropLayer.subImage.swapchain = xrVirtualScreenBackdropSwapchain;
			backdropLayer.subImage.imageArrayIndex = 0;
			layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&backdropLayer;
		}

		quadLayer = xrVirtualScreenLayer;
		quadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		quadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		quadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quadLayer.subImage.swapchain = xrVirtualScreenSwapchain;
		quadLayer.subImage.imageArrayIndex = 0;
		layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&quadLayer;
	}
	XrFrameEndInfo endInfo{ XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = xrFrameState.predictedDisplayTime;
	endInfo.environmentBlendMode = environmentBlendMode;
	endInfo.layerCount = layerIndex;
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
	if (screen == nullptr) return;
	if (!mInVRSceneRender) return;

	// Preserve the normal VR viewport scaling contract here instead of forcing
	// the scene to the XR recommended eye size. The XR swapchain itself already
	// carries the eye-image dimensions, and rewriting the framebuffer viewport
	// here can distort/zoom the scene and break 2D overlay composition when the
	// desktop resolution or scaling changes.
	VRMode::AdjustViewport(screen);
}

bool VKOpenXRDeviceMode::IsRenderingVirtualScreen() const
{
	return mInVirtualScreenRender;
}

void VKOpenXRDeviceMode::updateVirtualScreenLayer() const
{
	if (xrViewCount == 0 || xrViews.size() < xrViewCount)
		return;

	XrVector3f center{ 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < xrViewCount; ++i)
	{
		center.x += xrViews[i].pose.position.x;
		center.y += xrViews[i].pose.position.y;
		center.z += xrViews[i].pose.position.z;
	}
	center.x /= xrViewCount;
	center.y /= xrViewCount;
	center.z /= xrViewCount;

	const XrQuaternionf headOrientation = xrViews[0].pose.orientation;
	const XrVector3f forward = RotateVector(headOrientation, { 0.0f, 0.0f, -1.0f });
	const XrVector3f up = RotateVector(headOrientation, { 0.0f, 1.0f, 0.0f });

	const float distance = std::max(0.25f, 2.5f + vr_overlayscreen_dist);
	const float screenWidth = std::max(0.1f, 1.0f + vr_overlayscreen_size);
	const float aspect = (xrVirtualScreenHeight > 0) ? (float)xrVirtualScreenWidth / (float)xrVirtualScreenHeight : 1.0f;
	const float screenHeight = std::max(0.1f, screenWidth / std::max(aspect, 0.01f));
	const XrQuaternionf flipRotation = MakeAxisAngleQuaternion({ 0.0f, 0.0f, 1.0f }, (float)M_PI);

	// Keep the quad aligned with the headset and apply only the in-plane
	// correction that the original overlay path used.
	xrVirtualScreenPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
	xrVirtualScreenPose.position = AddVector(center, ScaleVector(forward, distance));
	xrVirtualScreenPose.position = AddVector(xrVirtualScreenPose.position, ScaleVector(up, vr_overlayscreen_vpos));

	xrVirtualScreenLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	xrVirtualScreenLayer.layerFlags = 0;
	xrVirtualScreenLayer.space = xrSpace;
	xrVirtualScreenLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	xrVirtualScreenLayer.pose = xrVirtualScreenPose;
	xrVirtualScreenLayer.size = { screenWidth, screenHeight };
	xrVirtualScreenLayer.subImage.swapchain = xrVirtualScreenSwapchain;
	xrVirtualScreenLayer.subImage.imageArrayIndex = 0;
	xrVirtualScreenLayer.subImage.imageRect.offset = { 0, 0 };
	xrVirtualScreenLayer.subImage.imageRect.extent = { (int32_t)xrVirtualScreenWidth, (int32_t)xrVirtualScreenHeight };

	xrVirtualScreenBackdropPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
	xrVirtualScreenBackdropPose.position = AddVector(center, ScaleVector(forward, distance + 0.15f));
	xrVirtualScreenBackdropLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	xrVirtualScreenBackdropLayer.layerFlags = 0;
	xrVirtualScreenBackdropLayer.space = xrSpace;
	xrVirtualScreenBackdropLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	xrVirtualScreenBackdropLayer.pose = xrVirtualScreenBackdropPose;
	xrVirtualScreenBackdropLayer.size = { screenWidth * 6.0f, screenHeight * 6.0f };
	xrVirtualScreenBackdropLayer.subImage.swapchain = xrVirtualScreenBackdropSwapchain;
	xrVirtualScreenBackdropLayer.subImage.imageArrayIndex = 0;
	xrVirtualScreenBackdropLayer.subImage.imageRect.offset = { 0, 0 };
	xrVirtualScreenBackdropLayer.subImage.imageRect.extent = { (int32_t)xrVirtualScreenWidth, (int32_t)xrVirtualScreenHeight };

}

bool VKOpenXRDeviceMode::ShouldRenderVirtualScreen() const
{
	return (gamestate != GS_LEVEL || menuactive != MENU_Off || cinemamode || ConsoleState != c_up) && (vr_overlayscreen || vr_overlayscreen_always);
}

bool VKOpenXRDeviceMode::RenderVirtualScreen() const
{
	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (!vkfb || !xrFrameInProgress || xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVkCommandBuffer == nullptr)
	{
		xrVirtualScreenVisible = false;
		return false;
	}
	if (!ShouldRenderVirtualScreen())
	{
		xrVirtualScreenVisible = false;
		return false;
	}

	const bool forceOverlay = gamestate != GS_LEVEL || menuactive != MENU_Off || cinemamode || ConsoleState != c_up;
	const bool allowBlankOverlay = vr_overlayscreen_always || cinemamode || gamestate != GS_LEVEL;
	if (twod == nullptr || (twod->DrawCount() == 0 && !allowBlankOverlay && !forceOverlay))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenImageIndex = -1;
		return false;
	}

	uint32_t screenWidth = 0;
	uint32_t screenHeight = 0;
	GetStableOpenXRVirtualScreenSize(screenWidth, screenHeight);
	if (!CreateVirtualScreenSwapchain(screenWidth, screenHeight))
	{
		xrVirtualScreenVisible = false;
		return false;
	}
	if (!CreateVirtualScreenBackdropSwapchain(screenWidth, screenHeight))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}
	if (xrVirtualScreenSwapchain == XR_NULL_HANDLE || xrVirtualScreenTextures.empty())
	{
		xrVirtualScreenVisible = false;
		return false;
	}
	if (xrVirtualScreenBackdropSwapchain == XR_NULL_HANDLE || xrVirtualScreenBackdropTextures.empty())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t imageIndex = 0;
	XrResult xrResult = xrAcquireSwapchainImage(xrVirtualScreenSwapchain, &acquireInfo, &imageIndex);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen acquire failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		return false;
	}

	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 100 * 1000 * 1000;
	xrResult = xrWaitSwapchainImage(xrVirtualScreenSwapchain, &waitInfo);
	if (XR_FAILED(xrResult))
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrVirtualScreenSwapchain, &releaseInfo);
		Printf("OpenXR: virtual screen wait failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		return false;
	}

	xrVirtualScreenImageIndex = (int)imageIndex;
	auto& target = xrVirtualScreenTextures[imageIndex];
	const bool useSceneBackdrop = gamestate == GS_LEVEL && menuactive != MENU_Off && !cinemamode;

	if (vr_openxr_debug_sizes)
	{
		Printf("XR_VSCREEN visible=%d swapchain=%ux%u draw=%ux%u backdrop=%d\n",
			1,
			xrVirtualScreenWidth, xrVirtualScreenHeight,
			screenWidth, screenHeight,
			useSceneBackdrop ? 1 : 0);
	}

	if (useSceneBackdrop)
	{
		vkfb->GetPostprocess()->BlitCurrentToImage(&target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		VkImageTransition()
			.AddImage(&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(vkfb->GetCommands()->GetDrawCommands());
	}
	else
	{
		VkImageTransition()
			.AddImage(&target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(vkfb->GetCommands()->GetDrawCommands());
	}

	float savedClear[4];
	memcpy(savedClear, screen->mSceneClearColor, sizeof(savedClear));
	if (useSceneBackdrop)
	{
		screen->mSceneClearColor[0] = 0.0f;
		screen->mSceneClearColor[1] = 0.0f;
		screen->mSceneClearColor[2] = 0.0f;
		screen->mSceneClearColor[3] = 0.0f;
	}
	else
	{
		const XrVector3f bg = GetVirtualScreenBackgroundColor();
		screen->mSceneClearColor[0] = bg.x;
		screen->mSceneClearColor[1] = bg.y;
		screen->mSceneClearColor[2] = bg.z;
		screen->mSceneClearColor[3] = 1.0f;
	}

	auto* renderState = vkfb->GetRenderState();
	const IntRect savedScreenViewport = vkfb->mScreenViewport;
	const IntRect savedSceneViewport = vkfb->mSceneViewport;
	const IntRect savedOutputLetterbox = vkfb->mOutputLetterbox;
	const int savedGameScreenWidth = vkfb->mGameScreenWidth;
	const int savedGameScreenHeight = vkfb->mGameScreenHeight;
	vkfb->mScreenViewport = { 0, 0, (int)screenWidth, (int)screenHeight };
	vkfb->mSceneViewport = vkfb->mScreenViewport;
	vkfb->mOutputLetterbox = vkfb->mScreenViewport;
	vkfb->mGameScreenWidth = (int)screenWidth;
	vkfb->mGameScreenHeight = (int)screenHeight;
	renderState->SetRenderTarget(&target, nullptr, (int)screenWidth, (int)screenHeight, (VkFormat)xrSwapchainFormat, VK_SAMPLE_COUNT_1_BIT);
	// Render the virtual-screen texture as a regular 2D target. The VR layer
	// compositor will handle the actual head-locked presentation.
	screen->mViewpoints->Set2D(*renderState, (int)screenWidth, (int)screenHeight);
	mInVirtualScreenRender = true;
	if (!useSceneBackdrop)
	{
		renderState->Clear(CT_Color);
	}
	screen->Draw2D(true);
	screen->Draw2D(false);
	mInVirtualScreenRender = false;
	renderState->EndRenderPass();
	vkfb->mScreenViewport = savedScreenViewport;
	vkfb->mSceneViewport = savedSceneViewport;
	vkfb->mOutputLetterbox = savedOutputLetterbox;
	vkfb->mGameScreenWidth = savedGameScreenWidth;
	vkfb->mGameScreenHeight = savedGameScreenHeight;

	memcpy(screen->mSceneClearColor, savedClear, sizeof(savedClear));

	auto* cmdbuffer = vkfb->GetCommands()->GetDrawCommands();
	auto& bounce = vkfb->GetBuffers()->PipelineImage[0];
	const int32_t targetWidth = target.Image != nullptr ? target.Image->width : 0;
	const int32_t targetHeight = target.Image != nullptr ? target.Image->height : 0;
	const int32_t bounceWidth = bounce.Image != nullptr ? bounce.Image->width : 0;
	const int32_t bounceHeight = bounce.Image != nullptr ? bounce.Image->height : 0;
	VkImageTransition()
		.AddImage(&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(&bounce, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(cmdbuffer);
	VkImageBlit blit = {};
	blit.srcOffsets[0] = { 0, 0, 0 };
	blit.srcOffsets[1] = { targetWidth, targetHeight, 1 };
	blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blit.dstOffsets[0] = { bounceWidth, 0, 0 };
	blit.dstOffsets[1] = { 0, bounceHeight, 1 };
	blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	cmdbuffer->blitImage(target.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		bounce.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blit, VK_FILTER_NEAREST);
	VkImageTransition()
		.AddImage(&bounce, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.AddImage(&target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(cmdbuffer);
	VkImageBlit blitBack = {};
	blitBack.srcOffsets[0] = { 0, 0, 0 };
	blitBack.srcOffsets[1] = { bounceWidth, bounceHeight, 1 };
	blitBack.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	blitBack.dstOffsets[0] = { 0, 0, 0 };
	blitBack.dstOffsets[1] = { targetWidth, targetHeight, 1 };
	blitBack.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	cmdbuffer->blitImage(bounce.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		target.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &blitBack, VK_FILTER_NEAREST);
	VkImageTransition()
		.AddImage(&target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.AddImage(&bounce, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmdbuffer);

	XrSwapchainImageAcquireInfo backdropAcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t backdropIndex = 0;
	xrResult = xrAcquireSwapchainImage(xrVirtualScreenBackdropSwapchain, &backdropAcquireInfo, &backdropIndex);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen backdrop acquire failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	XrSwapchainImageWaitInfo backdropWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	backdropWaitInfo.timeout = 100 * 1000 * 1000;
	xrResult = xrWaitSwapchainImage(xrVirtualScreenBackdropSwapchain, &backdropWaitInfo);
	if (XR_FAILED(xrResult))
	{
		XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(xrVirtualScreenBackdropSwapchain, &releaseInfo);
		Printf("OpenXR: virtual screen backdrop wait failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	xrVirtualScreenBackdropImageIndex = (int)backdropIndex;
	auto& backdropTarget = xrVirtualScreenBackdropTextures[backdropIndex];

	VkImageTransition()
		.AddImage(&backdropTarget, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
		.Execute(vkfb->GetCommands()->GetDrawCommands());

	const XrVector3f bgColor = GetVirtualScreenBackgroundColor();
	float savedBackdropClear[4];
	memcpy(savedBackdropClear, screen->mSceneClearColor, sizeof(savedBackdropClear));
	screen->mSceneClearColor[0] = bgColor.x;
	screen->mSceneClearColor[1] = bgColor.y;
	screen->mSceneClearColor[2] = bgColor.z;
	screen->mSceneClearColor[3] = 1.0f;

	auto* backdropState = vkfb->GetRenderState();
	backdropState->SetRenderTarget(&backdropTarget, nullptr, (int)screenWidth, (int)screenHeight, (VkFormat)xrSwapchainFormat, VK_SAMPLE_COUNT_1_BIT);
	backdropState->Clear(CT_Color);
	backdropState->EndRenderPass();

	memcpy(screen->mSceneClearColor, savedBackdropClear, sizeof(savedBackdropClear));

	VkImageTransition()
		.AddImage(&backdropTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(vkfb->GetCommands()->GetDrawCommands());

	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrResult = xrReleaseSwapchainImage(xrVirtualScreenBackdropSwapchain, &releaseInfo);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen backdrop release failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrResult = xrReleaseSwapchainImage(xrVirtualScreenSwapchain, &releaseInfo);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen release failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		return false;
	}

	updateVirtualScreenLayer();
	xrVirtualScreenVisible = true;
	xrVirtualScreenBackdropVisible = true;
	return true;
}

void VKOpenXRDeviceMode::FinalizeEyeImage(VulkanRenderDevice* vkfb, int eyeIndex) const
{
	if (!vkfb || eyeIndex < 0 || xrSession == XR_NULL_HANDLE || xrSwapchainFormat == VK_FORMAT_UNDEFINED)
		return;
	if ((uint32_t)eyeIndex >= xrViewCount)
		return;
	if (!CreatePresentTextures(vkfb))
		return;

	auto* postprocess = vkfb->GetPostprocess();
	if (!postprocess)
		return;
	const int pipelineImageIndex = postprocess->GetCurrentPipelineImage();
	const XrSafeSourceRect sourceRect = GetSafeXrSourceRect(vkfb);
	static bool xrLoggedPresentFormat = false;
	if (!xrLoggedPresentFormat)
	{
		Printf("OpenXR: finalize eye copy uses xrSwapchainFormat=%d srgb=%d presentTextureFormat=%d.\n",
			(int)xrSwapchainFormat,
			IsSRGBSwapchainFormat((VkFormat)xrSwapchainFormat) ? 1 : 0,
			(int)xrSwapchainFormat);
		xrLoggedPresentFormat = true;
	}

	if (vr_debug_projection_compare || vr_openxr_debug_present || vr_openxr_debug_sizes)
	{
		auto* buffers = vkfb->GetBuffers();
		const auto* sourceTexture = (buffers && pipelineImageIndex >= 0 && pipelineImageIndex < 2)
			? buffers->PipelineImage[pipelineImageIndex].Image.get()
			: nullptr;
		const auto* targetTexture = (eyeIndex >= 0 && eyeIndex < (int)xrPresentTextures.size())
			? xrPresentTextures[eyeIndex].Image.get()
			: nullptr;
		const int sourceWidth = sourceTexture ? sourceTexture->width : -1;
		const int sourceHeight = sourceTexture ? sourceTexture->height : -1;
		const int targetWidth = targetTexture ? targetTexture->width : (int)xrPresentWidth;
		const int targetHeight = targetTexture ? targetTexture->height : (int)xrPresentHeight;
		Printf("XR_PRESENT_MAP finalize eye=%d sourceImage=%d srcExtent=%dx%d dstExtent=%ux%u fb=%dx%d present=%ux%u\n",
			eyeIndex,
			postprocess->GetCurrentPipelineImage(),
			sourceWidth,
			sourceHeight,
			targetWidth,
			targetHeight,
			buffers ? buffers->GetWidth() : -1,
			buffers ? buffers->GetHeight() : -1,
			xrPresentWidth,
			xrPresentHeight);

		Printf("XR_PRESENT_MAP finalize eye=%d srcBuffer=%dx%d srcRect=%d,%d %dx%d scale=(%.4f,%.4f) offset=(%.4f,%.4f) dstEye=%dx%d fallback=%d clamped=%d\n",
			eyeIndex,
			buffers ? buffers->GetWidth() : -1,
			buffers ? buffers->GetHeight() : -1,
			sourceRect.rect.left,
			sourceRect.rect.top,
			sourceRect.rect.width,
			sourceRect.rect.height,
			(double)sourceRect.scaleX,
			(double)sourceRect.scaleY,
			(double)sourceRect.offsetX,
			(double)sourceRect.offsetY,
			targetWidth,
			targetHeight,
			sourceRect.usedFallback ? 1 : 0,
			sourceRect.wasClamped ? 1 : 0);
	}

	static bool xrLoggedSourceRectAdjustment = false;
	if (!xrLoggedSourceRectAdjustment && (sourceRect.usedFallback || sourceRect.wasClamped) && (vr_openxr_debug_present || vr_openxr_debug_sizes))
	{
		auto* buffers = vkfb->GetBuffers();
		Printf("OpenXR: adjusted XR finalize source rect srcBuffer=%dx%d requested=%d,%d %dx%d chosen=%d,%d %dx%d fallback=%d clamped=%d\n",
			buffers ? buffers->GetWidth() : -1,
			buffers ? buffers->GetHeight() : -1,
			vkfb->mSceneViewport.left,
			vkfb->mSceneViewport.top,
			vkfb->mSceneViewport.width,
			vkfb->mSceneViewport.height,
			sourceRect.rect.left,
			sourceRect.rect.top,
			sourceRect.rect.width,
			sourceRect.rect.height,
			sourceRect.usedFallback ? 1 : 0,
			sourceRect.wasClamped ? 1 : 0);
		xrLoggedSourceRectAdjustment = true;
	}

	postprocess->SetCurrentPipelineImage(pipelineImageIndex);

	IntRect targetBox;
	targetBox.left = 0;
	targetBox.top = 0;
	targetBox.width = (int)xrPresentWidth;
	targetBox.height = (int)xrPresentHeight;

	postprocess->DrawPresentTextureToImage(
		&xrPresentTextures[eyeIndex],
		(VkFormat)xrSwapchainFormat,
		targetBox,
		true,
		false,
		sourceRect.scaleX,
		sourceRect.scaleY,
		sourceRect.offsetX,
		sourceRect.offsetY,
		vkfb->GetCommands()->GetDrawCommands());
}

bool VKOpenXRDeviceMode::RenderDesktopMirror(VulkanRenderDevice* fb, VulkanImage* dstImage) const
{
	if (!fb || !dstImage)
		return false;

	auto* cmdbuffer = fb->GetCommands()->GetDrawCommands();
	const bool sideBySide = vr_desktop_view != 1 && vr_desktop_view != 2;
	const int leftSourceIndex = vr_swap_eyes ? 1 : 0;
	const int rightSourceIndex = vr_swap_eyes ? 0 : 1;

	if (xrPresentTextures.empty() || xrPresentTextures[leftSourceIndex].Image == nullptr || (sideBySide && xrPresentTextures[rightSourceIndex].Image == nullptr))
		return false;

	const bool hasBackdrop = xrVirtualScreenBackdropVisible &&
		xrVirtualScreenBackdropImageIndex >= 0 &&
		xrVirtualScreenBackdropImageIndex < (int)xrVirtualScreenBackdropTextures.size();

	VkTextureImage* leftEyeSource = &xrPresentTextures[leftSourceIndex];
	VkTextureImage* rightEyeSource = &xrPresentTextures[rightSourceIndex];
	VkImageTransition()
		.AddImage(leftEyeSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
		.Execute(cmdbuffer);
	if ((sideBySide || vr_desktop_view == 2) && rightEyeSource != leftEyeSource)
	{
		VkImageTransition()
			.AddImage(rightEyeSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
	}

	if (hasBackdrop)
	{
		auto& backdropSource = xrVirtualScreenBackdropTextures[xrVirtualScreenBackdropImageIndex];
		VkImageTransition()
			.AddImage(&backdropSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
	}

	VkImageMemoryBarrier dstBarrier = {};
	dstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	dstBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	dstBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	dstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	dstBarrier.image = dstImage->image;
	dstBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	cmdbuffer->pipelineBarrier(
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, nullptr, 0, nullptr, 1, &dstBarrier);

	const IntRect mirrorBox = fb->mOutputLetterbox;
	auto blitImage = [&](VkTextureImage* source, const IntRect& rect)
	{
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { 0, 0, 0 };
		blit.srcOffsets[1] = { (int32_t)source->Image->width, (int32_t)source->Image->height, 1 };
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
			dstImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_LINEAR);
	};
	auto blitOverlayImage = [&](VkTextureImage* source, const IntRect& rect)
	{
		VkImageBlit blit = {};
		blit.srcOffsets[0] = { (int32_t)source->Image->width, (int32_t)source->Image->height, 0 };
		blit.srcOffsets[1] = { 0, 0, 1 };
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
			dstImage->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &blit, VK_FILTER_LINEAR);
	};

	if (vr_desktop_view == 1)
	{
		blitImage(leftEyeSource, mirrorBox);
	}
	else if (vr_desktop_view == 2)
	{
		blitImage(rightEyeSource, mirrorBox);
	}
	else
	{
		IntRect leftHalf = mirrorBox;
		leftHalf.width = mirrorBox.width / 2;
		IntRect rightHalf = mirrorBox;
		rightHalf.width = mirrorBox.width - leftHalf.width;
		rightHalf.left += leftHalf.width;

		blitImage(leftEyeSource, leftHalf);
		blitImage(rightEyeSource, rightHalf);
	}

	if (hasBackdrop)
	{
		auto& backdropSource = xrVirtualScreenBackdropTextures[xrVirtualScreenBackdropImageIndex];
		VkImageTransition()
			.AddImage(&backdropSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
		blitOverlayImage(&backdropSource, mirrorBox);
	}
	if (xrVirtualScreenVisible && xrVirtualScreenImageIndex >= 0 && xrVirtualScreenImageIndex < (int)xrVirtualScreenTextures.size())
	{
		auto& contentSource = xrVirtualScreenTextures[xrVirtualScreenImageIndex];
		VkImageTransition()
			.AddImage(&contentSource, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmdbuffer);
		blitOverlayImage(&contentSource, mirrorBox);
	}

	if (hasBackdrop)
	{
		auto& backdropSource = xrVirtualScreenBackdropTextures[xrVirtualScreenBackdropImageIndex];
		VkImageTransition()
			.AddImage(&backdropSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}
	if (xrVirtualScreenVisible && xrVirtualScreenImageIndex >= 0 && xrVirtualScreenImageIndex < (int)xrVirtualScreenTextures.size())
	{
		auto& contentSource = xrVirtualScreenTextures[xrVirtualScreenImageIndex];
		VkImageTransition()
			.AddImage(&contentSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}
	VkImageTransition()
		.AddImage(leftEyeSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(cmdbuffer);
	if ((sideBySide || vr_desktop_view == 2) && rightEyeSource != leftEyeSource)
	{
		VkImageTransition()
			.AddImage(rightEyeSource, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmdbuffer);
	}

	return true;
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
