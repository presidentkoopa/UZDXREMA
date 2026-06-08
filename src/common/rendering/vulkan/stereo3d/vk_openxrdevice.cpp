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
#include "QzDoom/VrCommon.h"
#include "d_player.h"
#include "g_game.h"
#include "g_levellocals.h"
#include "doomdef.h"
#include "c_console.h"
#include "d_eventbase.h"
#include "d_gui.h"
#include "menu.h"
#include "i_time.h"
#include "p_trace.h"
#include "p_linetracedata.h"
#include "p_local.h"
#include "LSMatrix.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "common/rendering/hwrenderer/data/hw_viewpointbuffer.h"
#include "v_draw.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <thread>
#include <chrono>
#include <vector>

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
extern float snapTurn;
extern vec3_t positionDeltaThisFrame;
extern float remote_movementSideways;
extern float remote_movementForward;
extern float positional_movementSideways;
extern float positional_movementForward;
extern bool resetDoomYaw;
extern bool resetPreviousPitch;
extern bool ready_teleport;
extern bool trigger_teleport;
extern bool automapactive;
extern bool cinemamode;
bool VR_UseScreenLayer();
void VR_SetHMDOrientation(float pitch, float yaw, float roll);
void VR_SetHMDPosition(float x, float y, float z);
double P_XYMovement(AActor* mo, DVector2 scroll);
void QzDoom_setUseScreenLayer(bool use);

EXTERN_CVAR(Float, vr_ipd);
EXTERN_CVAR(Float, vr_vunits_per_meter);
EXTERN_CVAR(Float, vr_height_adjust);
EXTERN_CVAR(Float, vr_openxr_fov_adjust_deg);
EXTERN_CVAR(Float, vr_openxr_eye_shift_scale);
EXTERN_CVAR(Float, vr_openxr_render_scale);
EXTERN_CVAR(Bool, vr_debug_projection_compare);
EXTERN_CVAR(Bool, vr_openxr_debug_sizes);
EXTERN_CVAR(Bool, vr_openxr_debug_present);
EXTERN_CVAR(Bool, vr_openxr_debug_weapon);
EXTERN_CVAR(Float, vr_snapTurn);
EXTERN_CVAR(Bool, vr_move_use_offhand);
EXTERN_CVAR(Bool, vr_switch_sticks);
EXTERN_CVAR(Bool, vr_secondary_button_mappings);
EXTERN_CVAR(Bool, vr_teleport);
EXTERN_CVAR(Float, vr_weaponRotate);
EXTERN_CVAR(Float, vr_weaponScale);
EXTERN_CVAR(Bool, vr_enable_haptics);
EXTERN_CVAR(Float, vr_2dweaponScale);
EXTERN_CVAR(Float, vr_2dweaponOffsetX);
EXTERN_CVAR(Float, vr_2dweaponOffsetY);
EXTERN_CVAR(Float, vr_2dweaponOffsetZ);
EXTERN_CVAR(Int, screenblocks);
EXTERN_CVAR(Int, vid_defwidth);
EXTERN_CVAR(Int, vid_defheight);
EXTERN_CVAR(Float, vr_automap_stereo);
EXTERN_CVAR(Float, vr_hud_stereo);
EXTERN_CVAR(Float, vr_automap_rotate);
EXTERN_CVAR(Float, vr_hud_rotate);
EXTERN_CVAR(Float, vr_automap_distance);
EXTERN_CVAR(Float, vr_hud_distance);
EXTERN_CVAR(Float, vr_automap_scale);
EXTERN_CVAR(Float, vr_hud_scale);
EXTERN_CVAR(Bool, vr_hud_mount);
EXTERN_CVAR(Int, vr_hud_mount_pos);
EXTERN_CVAR(Float, vr_hud_mount_scale);
EXTERN_CVAR(Bool, vr_automap_fixed_roll);
EXTERN_CVAR(Bool, vr_hud_fixed_roll);
EXTERN_CVAR(Bool, vr_automap_fixed_pitch);
EXTERN_CVAR(Bool, vr_hud_fixed_pitch);
EXTERN_CVAR(Bool, vr_automap_mount);
EXTERN_CVAR(Int, vr_automap_mount_pos);
EXTERN_CVAR(Float, vr_automap_mount_scale);
EXTERN_CVAR(Int, vr_automap_border);
EXTERN_CVAR(Color, vr_automap_border_color);
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
EXTERN_CVAR(Bool, vr_two_handed_weapons);
CVAR(Bool, vr_menu_pointer, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Color, vr_menu_pointer_color, 0xffffff, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Bool, vr_mouse_in_menu, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

namespace s3d {

namespace
{
constexpr XrViewConfigurationType viewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr XrEnvironmentBlendMode environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
XrSessionState xrSessionState = XR_SESSION_STATE_UNKNOWN;

using PFN_xrGetVulkanGraphicsRequirementsKHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
using PFN_xrGetVulkanGraphicsDeviceKHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, VkInstance, VkPhysicalDevice*);
using PFN_xrGetVulkanGraphicsRequirements2KHR_t = XrResult (XRAPI_PTR *)(XrInstance, XrSystemId, XrGraphicsRequirementsVulkanKHR*);
using PFN_xrGetVulkanGraphicsDevice2KHR_t = XrResult (XRAPI_PTR *)(XrInstance, const XrVulkanGraphicsDeviceGetInfoKHR*, VkPhysicalDevice*);

PFN_xrGetVulkanGraphicsRequirementsKHR_t xrGetVulkanGraphicsRequirementsKHR_inst = nullptr;
PFN_xrGetVulkanGraphicsDeviceKHR_t xrGetVulkanGraphicsDeviceKHR_inst = nullptr;
PFN_xrGetVulkanGraphicsRequirements2KHR_t xrGetVulkanGraphicsRequirements2KHR_inst = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR_t xrGetVulkanGraphicsDevice2KHR_inst = nullptr;

static const std::vector<XrExtensionProperties>& GetOpenXRExtensions()
{
	static std::vector<XrExtensionProperties> cachedExtensions;
	static bool cacheInitialized = false;
	if (cacheInitialized)
		return cachedExtensions;

	cacheInitialized = true;
	uint32_t extensionCount = 0;
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr)) || extensionCount == 0)
		return cachedExtensions;

	cachedExtensions.assign(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
	if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, cachedExtensions.data())))
	{
		cachedExtensions.clear();
		return cachedExtensions;
	}

	if (cachedExtensions.size() != extensionCount)
		cachedExtensions.resize(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });

	return cachedExtensions;
}

static bool IsGameplaySceneActive()
{
	return gamestate == GS_LEVEL && menuactive == MENU_Off && !paused && ConsoleState == c_up;
}

static bool HasOpenXRExtension(const char* extensionName)
{
	const auto& extensions = GetOpenXRExtensions();

	for (const auto& extension : extensions)
	{
		if (strcmp(extension.extensionName, extensionName) == 0)
			return true;
	}

	return false;
}

static bool ShouldRetryCreateSession(XrResult xrResult)
{
	// Retry only for transient runtime failures
	return xrResult == XR_ERROR_RUNTIME_FAILURE;
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

static XrVector3f NormalizeVector(const XrVector3f& v)
{
	const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (length <= 0.0f)
	{
		return { 0.0f, 0.0f, 0.0f };
	}
	return { v.x / length, v.y / length, v.z / length };
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

static float DotVector(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static XrVector3f CrossVector(const XrVector3f& a, const XrVector3f& b)
{
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
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

static XrQuaternionf QuaternionFromBasis(const XrVector3f& xAxis, const XrVector3f& yAxis, const XrVector3f& zAxis)
{
	const float m00 = xAxis.x, m01 = yAxis.x, m02 = zAxis.x;
	const float m10 = xAxis.y, m11 = yAxis.y, m12 = zAxis.y;
	const float m20 = xAxis.z, m21 = yAxis.z, m22 = zAxis.z;

	const float trace = m00 + m11 + m22;
	XrQuaternionf q{};
	if (trace > 0.0f)
	{
		const float s = std::sqrt(trace + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m21 - m12) / s;
		q.y = (m02 - m20) / s;
		q.z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		q.w = (m21 - m12) / s;
		q.x = 0.25f * s;
		q.y = (m01 + m10) / s;
		q.z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		q.w = (m02 - m20) / s;
		q.x = (m01 + m10) / s;
		q.y = 0.25f * s;
		q.z = (m12 + m21) / s;
	}
	else
	{
		const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		q.w = (m10 - m01) / s;
		q.x = (m02 + m20) / s;
		q.y = (m12 + m21) / s;
		q.z = 0.25f * s;
	}
	return q;
}

static XrQuaternionf MakeAxisAngleQuaternion(const XrVector3f& axis, float angleRadians)
{
	const float halfAngle = angleRadians * 0.5f;
	const float s = std::sin(halfAngle);
	const float c = std::cos(halfAngle);
	return { axis.x * s, axis.y * s, axis.z * s, c };
}

static XrVector3f OpenVREulerAnglesFromQuaternion(const XrQuaternionf& quat)
{
	double q0 = quat.w;
	// Permute axes to match OpenVR's yaw/pitch/roll convention.
	double q2 = quat.x;
	double q3 = quat.y;
	double q1 = quat.z;

	double roll = std::atan2(2.0 * (q0 * q1 + q2 * q3), 1.0 - 2.0 * (q1 * q1 + q2 * q2));
	double pitch = std::asin(2.0 * (q0 * q2 - q3 * q1));
	double yaw = std::atan2(2.0 * (q0 * q3 + q1 * q2), 1.0 - 2.0 * (q2 * q2 + q3 * q3));

	return { (float)yaw, (float)pitch, (float)roll };
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
	// Keep the virtual screen live with the current game resolution, but fold
	// it into a stable 4:3 surface so the controller ray and menu pixels stay
	// in the same coordinate space while the game is running.
	constexpr uint32_t kFallbackW = 960;
	constexpr uint32_t kFallbackH = 720;
	constexpr uint32_t kMinW = 640;
	constexpr uint32_t kMinH = 360;
	constexpr uint32_t kMaxW = 2048;
	constexpr uint32_t kMaxH = 2048;
	constexpr uint64_t kMaxPixels = 2048ull * 1536ull;

	uint32_t sourceW = (uint32_t)std::max(0, DisplayWidth);
	uint32_t sourceH = (uint32_t)std::max(0, DisplayHeight);
	if (sourceW == 0 || sourceH == 0)
	{
		sourceW = (uint32_t)std::max(0, (int)vid_defwidth);
		sourceH = (uint32_t)std::max(0, (int)vid_defheight);
	}
	if (sourceW == 0 || sourceH == 0)
	{
		sourceW = kFallbackW;
		sourceH = kFallbackH;
	}

	sourceW = std::clamp(sourceW, kMinW, kMaxW);
	sourceH = std::clamp(sourceH, kMinH, kMaxH);

	uint32_t targetW = sourceW;
	uint32_t targetH = sourceH;
	if ((uint64_t)sourceW * 3ull <= (uint64_t)sourceH * 4ull)
	{
		targetH = (uint32_t)std::floor((double)sourceW * 3.0 / 4.0);
	}
	else
	{
		targetW = (uint32_t)std::floor((double)sourceH * 4.0 / 3.0);
	}

	targetW = std::clamp(targetW, kMinW, kMaxW);
	targetH = std::clamp(targetH, kMinH, kMaxH);

	uint64_t pixels = (uint64_t)targetW * (uint64_t)targetH;
	if (pixels > kMaxPixels)
	{
		const double scale = std::sqrt((double)kMaxPixels / (double)pixels);
		targetW = std::max<uint32_t>(kMinW, (uint32_t)std::floor((double)targetW * scale));
		targetH = std::max<uint32_t>(kMinH, (uint32_t)std::floor((double)targetH * scale));
	}
	targetW &= ~1u;
	targetH &= ~1u;
	if (targetW == 0 || targetH == 0)
	{
		targetW = kFallbackW;
		targetH = kFallbackH;
	}

	width = targetW;
	height = targetH;
}

static void GetOpenXRVirtualScreenMeters(uint32_t renderW, uint32_t renderH, float& widthMeters, float& heightMeters)
{
	const float baseWidthMeters = std::max(0.1f, (1.0f + vr_overlayscreen_size) * 1.2f);
	if (renderW == 0 || renderH == 0)
	{
		GetStableOpenXRVirtualScreenSize(renderW, renderH);
	}

	widthMeters = baseWidthMeters;
	heightMeters = std::max(0.1f, baseWidthMeters * ((float)renderH / (float)std::max(renderW, 1u)) * 1.05f);
}

static float YawDegFromForward(const XrVector3f& forwardIn)
{
	XrVector3f forward = forwardIn;
	forward.y = 0.0f;
	forward = NormalizeVector(forward);
	if (DotVector(forward, forward) <= 0.000001f)
		return 0.0f;
	return std::atan2(forward.x, forward.z) * (180.0f / (float)M_PI);
}

static float ShortestAngleDeltaDeg(float a, float b)
{
	float d = std::fmod(a - b, 360.0f);
	if (d > 180.0f) d -= 360.0f;
	if (d < -180.0f) d += 360.0f;
	return d;
}

static XrSafeSourceRect GetSafeXrSourceRect(VulkanRenderDevice* vkfb)
{
	XrSafeSourceRect result;
	auto* buffers = vkfb ? vkfb->GetBuffers() : nullptr;
	const int srcBufferW = buffers ? buffers->GetWidth() : 0;
	const int srcBufferH = buffers ? buffers->GetHeight() : 0;
	IntRect requestedRect = vkfb ? vkfb->mSceneViewport : IntRect{ 0, 0, 0, 0 };
	const bool overlayUIActive = menuactive != MENU_Off || ConsoleState != c_up || cinemamode;
	const auto& mode = (const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance();

	if (mode.ShouldUseRecommendedRenderSizeThisFrame() && !overlayUIActive && srcBufferW > 0 && srcBufferH > 0)
	{
		requestedRect.left = 0;
		requestedRect.top = 0;
		requestedRect.width = srcBufferW;
		requestedRect.height = srcBufferH;
	}

	auto useFullBufferFallback = [&]()
	{
		result.rect.left = 0;
		result.rect.top = 0;
		result.rect.width = std::max(1, srcBufferW);
		result.rect.height = std::max(1, srcBufferH);
		result.usedFallback = true;
	};

	if (srcBufferW <= 0 || srcBufferH <= 0 || requestedRect.width <= 0 || requestedRect.height <= 0 || overlayUIActive)
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
	const float offsetZ = nearZ;

	FLOATTYPE m[16];
	memset(m, 0, sizeof(m));

	// This renderer stage consumes an OpenGL-style [-1,1] Z projection even
	// when the final submitted image goes through a Vulkan/OpenXR bridge.
	m[0] = 2.0f / tanWidth;
	m[5] = 2.0f / tanHeight;
	m[8] = (tanRight + tanLeft) / tanWidth;
	m[9] = (tanUp + tanDown) / tanHeight;
	m[10] = -(farZ + offsetZ) / (farZ - nearZ);
	m[11] = -1.0f;
	m[14] = -(farZ * (nearZ + offsetZ)) / (farZ - nearZ);

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

static XrQuaternionf QuaternionFromAxisAngle(float x, float y, float z, float radians)
{
	const float halfAngle = radians * 0.5f;
	const float sine = sinf(halfAngle);
	return XrQuaternionf{ x * sine, y * sine, z * sine, cosf(halfAngle) };
}

static double NormalizeAngle(double angle)
{
	angle = fmod(angle, 360.0);
	angle = fmod(angle + 360.0, 360.0);
	if (angle > 180.0)
	{
		angle -= 360.0;
	}
	return angle;
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

static XrVector3f GetVirtualScreenBackdropColor()
{
	const XrVector3f base = GetVirtualScreenBackgroundColor();
	constexpr float kBackdropDimScale = 0.30f;
	return { base.x * kBackdropDimScale, base.y * kBackdropDimScale, base.z * kBackdropDimScale };
}

static bool IsRightHandedVrControls()
{
	return vr_control_scheme < 10;
}

static int GetMainHandIndex()
{
	return IsRightHandedVrControls() ? 1 : 0;
}

static int GetOffHandIndex()
{
	return IsRightHandedVrControls() ? 0 : 1;
}

static int HandKeyOffset(int hand)
{
	return hand * (KEY_PAD_RSHOULDER - KEY_PAD_LSHOULDER);
}

static int HandAxisKeyOffset(int hand)
{
	return hand * (KEY_PAD_RTHUMB_LEFT - KEY_PAD_LTHUMB_LEFT);
}

struct OpenXRHandInputState
{
	bool select = false;
	bool grip = false;
	bool thumbClick = false;
	bool menu = false;
	bool a = false;
	bool b = false;
	bool x = false;
	bool y = false;
	XrVector2f thumbstick = { 0.0f, 0.0f };
	XrVector2f trackpad = { 0.0f, 0.0f };
};

static void PostControllerKeyTransition(bool oldState, bool newState, int key)
{
	if (oldState == newState)
		return;

	event_t ev = {};
	ev.data1 = key;
	ev.type = newState ? EV_KeyDown : EV_KeyUp;
	D_PostEvent(&ev);
}

static void PostGuiMouseEvent(EGUIEvent type, int x, int y)
{
	event_t ev = {};
	ev.type = EV_GUI_Event;
	ev.subtype = type;
	ev.data1 = x;
	ev.data2 = y;
	D_PostEvent(&ev);
}

static void PostGuiWheelEvent(EGUIEvent type, int x, int y, int modifiers = 0)
{
	event_t ev = {};
	ev.type = EV_GUI_Event;
	ev.subtype = type;
	ev.data1 = x;
	ev.data2 = y;
	ev.data3 = (int16_t)modifiers;
	D_PostEvent(&ev);
}

static void PostControllerAxisTransitions(const XrVector2f& oldValue, const XrVector2f& newValue, int leftKey, int rightKey, int downKey, int upKey)
{
	// Keep X/Y virtual-button zones mutually exclusive so diagonal thumbstick
	// movement cannot trigger both turn (left/right) and up/down binds together.
	// For right-stick vertical binds, make up/down significantly harder to
	// trigger than left/right to protect turning from accidental weapon changes.
	const bool strictRightVertical =
		(downKey == KEY_JOYAXIS4MINUS && upKey == KEY_JOYAXIS4PLUS) ||
		(downKey == KEY_JOYAXIS8MINUS && upKey == KEY_JOYAXIS8PLUS);
	const float xDeadZone = 0.25f;
	const float yDeadZone = strictRightVertical ? 0.72f : 0.25f;
	const float horizontalDominanceMargin = strictRightVertical ? 0.00f : 0.15f;
	const float verticalDominanceMargin = strictRightVertical ? 0.30f : 0.15f;
	auto resolveCardinalStates = [&](const XrVector2f& value, bool& left, bool& right, bool& down, bool& up)
	{
		const float absX = fabsf(value.x);
		const float absY = fabsf(value.y);
		const bool xActive = absX > xDeadZone;
		const bool yActive = absY > yDeadZone;
		const bool xDominant = xActive && (!yActive || (absX - absY) >= horizontalDominanceMargin);
		const bool yDominant = yActive && (!xActive || (absY - absX) >= verticalDominanceMargin);

		left = xDominant && value.x < 0.0f;
		right = xDominant && value.x > 0.0f;
		down = yDominant && value.y < 0.0f;
		up = yDominant && value.y > 0.0f;
	};

	bool oldLeft = false, oldRight = false, oldDown = false, oldUp = false;
	bool newLeft = false, newRight = false, newDown = false, newUp = false;
	resolveCardinalStates(oldValue, oldLeft, oldRight, oldDown, oldUp);
	resolveCardinalStates(newValue, newLeft, newRight, newDown, newUp);

	PostControllerKeyTransition(oldLeft, newLeft, leftKey);
	PostControllerKeyTransition(oldRight, newRight, rightKey);
	PostControllerKeyTransition(oldDown, newDown, downKey);
	PostControllerKeyTransition(oldUp, newUp, upKey);
}

static void PostRemappedControllerAxisTransitions(
	const XrVector2f& oldValue,
	const XrVector2f& newValue,
	bool oldModifier,
	bool newModifier,
	int baseLeftKey,
	int baseRightKey,
	int baseDownKey,
	int baseUpKey,
	int modifiedLeftKey,
	int modifiedRightKey,
	int modifiedDownKey,
	int modifiedUpKey)
{
	// Keep X/Y virtual-button zones mutually exclusive so diagonal thumbstick
	// movement cannot trigger both turn (left/right) and up/down binds together.
	// For right-stick vertical binds, make up/down significantly harder to
	// trigger than left/right to protect turning from accidental weapon changes.
	const bool strictRightVertical =
		(baseDownKey == KEY_JOYAXIS4MINUS && baseUpKey == KEY_JOYAXIS4PLUS) ||
		(modifiedDownKey == KEY_JOYAXIS8MINUS && modifiedUpKey == KEY_JOYAXIS8PLUS);
	const float xDeadZone = 0.25f;
	const float yDeadZone = strictRightVertical ? 0.72f : 0.25f;
	const float horizontalDominanceMargin = strictRightVertical ? 0.00f : 0.15f;
	const float verticalDominanceMargin = strictRightVertical ? 0.30f : 0.15f;
	auto resolveCardinalStates = [&](const XrVector2f& value, bool& left, bool& right, bool& down, bool& up)
	{
		const float absX = fabsf(value.x);
		const float absY = fabsf(value.y);
		const bool xActive = absX > xDeadZone;
		const bool yActive = absY > yDeadZone;
		const bool xDominant = xActive && (!yActive || (absX - absY) >= horizontalDominanceMargin);
		const bool yDominant = yActive && (!xActive || (absY - absX) >= verticalDominanceMargin);

		left = xDominant && value.x < 0.0f;
		right = xDominant && value.x > 0.0f;
		down = yDominant && value.y < 0.0f;
		up = yDominant && value.y > 0.0f;
	};

	bool oldLeft = false, oldRight = false, oldDown = false, oldUp = false;
	bool newLeft = false, newRight = false, newDown = false, newUp = false;
	resolveCardinalStates(oldValue, oldLeft, oldRight, oldDown, oldUp);
	resolveCardinalStates(newValue, newLeft, newRight, newDown, newUp);

	auto emitTransition = [&](bool oldState, bool newState, int oldBaseKey, int newBaseKey)
	{
		if (oldBaseKey == newBaseKey)
		{
			PostControllerKeyTransition(oldState, newState, oldBaseKey);
			return;
		}

		// If the grip modifier flips while the stick is still held, switch the
		// virtual key cleanly instead of leaving the old layer latched.
		if (oldState)
			PostControllerKeyTransition(true, false, oldBaseKey);
		if (newState)
			PostControllerKeyTransition(false, true, newBaseKey);
	};

	const int oldLeftKey = oldModifier ? modifiedLeftKey : baseLeftKey;
	const int oldRightKey = oldModifier ? modifiedRightKey : baseRightKey;
	const int oldDownKey = oldModifier ? modifiedDownKey : baseDownKey;
	const int oldUpKey = oldModifier ? modifiedUpKey : baseUpKey;
	const int newLeftKey = newModifier ? modifiedLeftKey : baseLeftKey;
	const int newRightKey = newModifier ? modifiedRightKey : baseRightKey;
	const int newDownKey = newModifier ? modifiedDownKey : baseDownKey;
	const int newUpKey = newModifier ? modifiedUpKey : baseUpKey;

	emitTransition(oldLeft, newLeft, oldLeftKey, newLeftKey);
	emitTransition(oldRight, newRight, oldRightKey, newRightKey);
	emitTransition(oldDown, newDown, oldDownKey, newDownKey);
	emitTransition(oldUp, newUp, oldUpKey, newUpKey);
}

static float NonLinearFilter(float value)
{
	const float absValue = fabsf(value);
	const float squared = absValue * absValue;
	return (value < 0.0f) ? -squared : squared;
}

static float OpenVRLength2D(float x, float y)
{
	return sqrtf(x * x + y * y);
}

static float OpenVRNonLinearFilter(float in)
{
	constexpr float NLF_DEADZONE = 0.1f;
	constexpr float NLF_POWER = 2.2f;

	float val = 0.0f;
	if (in > NLF_DEADZONE)
	{
		val = in > 1.0f ? 1.0f : in;
		val -= NLF_DEADZONE;
		val /= (1.0f - NLF_DEADZONE);
		val = powf(val, NLF_POWER);
	}
	else if (in < -NLF_DEADZONE)
	{
		val = in < -1.0f ? -1.0f : in;
		val += NLF_DEADZONE;
		val /= (1.0f - NLF_DEADZONE);
		val = -powf(fabsf(val), NLF_POWER);
	}

	return val;
}

static void PostRemappedControllerKeyTransition(bool oldPressed, bool newPressed, bool oldModifier, bool newModifier, int baseKey, int modifiedKey)
{
	const int oldKey = oldModifier ? modifiedKey : baseKey;
	const int newKey = newModifier ? modifiedKey : baseKey;

	if (oldKey == newKey)
	{
		PostControllerKeyTransition(oldPressed, newPressed, newKey);
		return;
	}

	if (oldPressed)
		PostControllerKeyTransition(true, false, oldKey);
	if (newPressed)
		PostControllerKeyTransition(false, true, newKey);
}

static bool GetActionBoolean(XrSession session, XrAction action, XrPath subactionPath)
{
	if (session == XR_NULL_HANDLE || action == XR_NULL_HANDLE)
		return false;

	XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;

	XrActionStateBoolean state{ XR_TYPE_ACTION_STATE_BOOLEAN };
	if (XR_FAILED(xrGetActionStateBoolean(session, &getInfo, &state)))
		return false;
	return state.currentState != XR_FALSE;
}

static XrVector2f GetActionVector2f(XrSession session, XrAction action, XrPath subactionPath)
{
	if (session == XR_NULL_HANDLE || action == XR_NULL_HANDLE)
		return { 0.0f, 0.0f };

	XrActionStateGetInfo getInfo{ XR_TYPE_ACTION_STATE_GET_INFO };
	getInfo.action = action;
	getInfo.subactionPath = subactionPath;

	XrActionStateVector2f state{ XR_TYPE_ACTION_STATE_VECTOR2F };
	if (XR_FAILED(xrGetActionStateVector2f(session, &getInfo, &state)))
		return { 0.0f, 0.0f };
	return state.currentState;
}

static FString PathToString(XrInstance instance, XrPath path)
{
	if (instance == XR_NULL_HANDLE || path == XR_NULL_PATH)
		return FString("<null>");

	uint32_t bufferLength = 0;
	if (XR_FAILED(xrPathToString(instance, path, 0, &bufferLength, nullptr)) || bufferLength == 0)
		return FString("<invalid>");

	std::vector<char> buffer(bufferLength);
	if (XR_FAILED(xrPathToString(instance, path, bufferLength, &bufferLength, buffer.data())))
		return FString("<invalid>");

	return FString(buffer.data());
}

static bool IsCurrentInteractionProfile(XrInstance instance, XrSession session, XrPath handPath, const char* profilePathSuffix)
{
	if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE || handPath == XR_NULL_PATH || profilePathSuffix == nullptr)
		return false;

	XrInteractionProfileState profileState{ XR_TYPE_INTERACTION_PROFILE_STATE };
	if (XR_FAILED(xrGetCurrentInteractionProfile(session, handPath, &profileState)) || profileState.interactionProfile == XR_NULL_PATH)
		return false;

	uint32_t bufferLength = 0;
	if (XR_FAILED(xrPathToString(instance, profileState.interactionProfile, 0, &bufferLength, nullptr)) || bufferLength == 0)
		return false;

	std::vector<char> buffer(bufferLength);
	if (XR_FAILED(xrPathToString(instance, profileState.interactionProfile, bufferLength, &bufferLength, buffer.data())))
		return false;

	return strstr(buffer.data(), profilePathSuffix) != nullptr;
}

static void LogBoundSourcesForAction(XrInstance instance, XrSession session, XrAction action, const char* label)
{
	if (instance == XR_NULL_HANDLE || session == XR_NULL_HANDLE || action == XR_NULL_HANDLE || label == nullptr)
		return;
	if (!xrEnumerateBoundSourcesForAction)
		return;

	XrBoundSourcesForActionEnumerateInfo enumerateInfo{ XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
	enumerateInfo.action = action;

	uint32_t sourceCount = 0;
	if (XR_FAILED(xrEnumerateBoundSourcesForAction(session, &enumerateInfo, 0, &sourceCount, nullptr)) || sourceCount == 0)
	{
		Printf("OpenXR: bound sources [%s] unavailable.\n", label);
		return;
	}

	std::vector<XrPath> sources(sourceCount, XR_NULL_PATH);
	if (XR_FAILED(xrEnumerateBoundSourcesForAction(session, &enumerateInfo, sourceCount, &sourceCount, sources.data())))
	{
		Printf("OpenXR: bound sources [%s] query failed.\n", label);
		return;
	}

	FString list;
	for (uint32_t i = 0; i < sourceCount; ++i)
	{
		const FString source = PathToString(instance, sources[i]);
		if (i != 0)
			list += ", ";
		list += source.GetChars();
	}

	Printf("OpenXR: bound sources [%s] = %s\n", label, list.GetChars());
}

static void SuggestBindingsForProfile(XrInstance instance, XrPath profilePath, const char* profileName, const std::vector<XrActionSuggestedBinding>& bindings)
{
	if (instance == XR_NULL_HANDLE || profilePath == XR_NULL_PATH || bindings.empty())
		return;

	XrInteractionProfileSuggestedBinding suggested{ XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = profilePath;
	suggested.suggestedBindings = bindings.data();
	suggested.countSuggestedBindings = (uint32_t)bindings.size();
	const XrResult result = xrSuggestInteractionProfileBindings(instance, &suggested);
	Printf("OpenXR: profile %s suggestion count=%u result=%d\n",
		profileName != nullptr ? profileName : "<unknown>",
		(unsigned)bindings.size(),
		(int)result);
	if (XR_FAILED(result))
	{
		Printf("OpenXR: xrSuggestInteractionProfileBindings failed for profile %s result=%d\n",
			profileName != nullptr ? profileName : "<unknown>",
			(int)result);
	}
}

static void AddBinding(std::vector<XrActionSuggestedBinding>& bindings, XrAction action, XrPath path)
{
	if (action != XR_NULL_HANDLE && path != XR_NULL_PATH)
	{
		bindings.push_back({ action, path });
	}
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

static int64_t SelectFlatSwapchainFormat(const std::vector<int64_t>& runtimeFormats, VkFormat preferredFormat)
{
	auto hasFormat = [&](int64_t format) -> bool
	{
		return std::find(runtimeFormats.begin(), runtimeFormats.end(), format) != runtimeFormats.end();
	};

	const VkFormat preferredUnorm =
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_UNORM :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_UNORM :
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_UNORM :
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? VK_FORMAT_B8G8R8A8_UNORM :
		VK_FORMAT_UNDEFINED;
	const VkFormat preferredSrgb =
		(preferredFormat == VK_FORMAT_R8G8B8A8_UNORM) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_UNORM) ? VK_FORMAT_B8G8R8A8_SRGB :
		(preferredFormat == VK_FORMAT_R8G8B8A8_SRGB) ? VK_FORMAT_R8G8B8A8_SRGB :
		(preferredFormat == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB :
		VK_FORMAT_UNDEFINED;

	const int64_t preferred[] = {
		(int64_t)preferredUnorm,
		(preferredUnorm == VK_FORMAT_B8G8R8A8_UNORM) ? (int64_t)VK_FORMAT_R8G8B8A8_UNORM : (int64_t)VK_FORMAT_B8G8R8A8_UNORM,
		(preferredUnorm == VK_FORMAT_R8G8B8A8_UNORM) ? (int64_t)VK_FORMAT_B8G8R8A8_UNORM : (int64_t)VK_FORMAT_R8G8B8A8_UNORM,
		(int64_t)preferredSrgb,
		(preferredSrgb == VK_FORMAT_B8G8R8A8_SRGB) ? (int64_t)VK_FORMAT_R8G8B8A8_SRGB : (int64_t)VK_FORMAT_B8G8R8A8_SRGB,
		(preferredSrgb == VK_FORMAT_R8G8B8A8_SRGB) ? (int64_t)VK_FORMAT_B8G8R8A8_SRGB : (int64_t)VK_FORMAT_R8G8B8A8_SRGB
	};

	for (int64_t format : preferred)
	{
		if (format != VK_FORMAT_UNDEFINED && hasFormat(format))
			return format;
	}

	return SelectSwapchainFormat(runtimeFormats, preferredFormat);
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

DAngle VKOpenXRDeviceEyePose::GetRenderFov(DAngle fallback) const
{
	const float fovAdjust = DEG2RAD(clamp<float>(vr_openxr_fov_adjust_deg, -30.0f, 30.0f));
	const float horizontalFov = (currentFov.angleRight - currentFov.angleLeft) + 2.0f * fovAdjust;
	const float verticalFov = (currentFov.angleUp - currentFov.angleDown) + 2.0f * fovAdjust;
	const float renderFovDegrees = std::max(horizontalFov, verticalFov) * (180.0f / (float)M_PI);
	return renderFovDegrees > 0.0f ? DAngle::fromDeg(renderFovDegrees) : fallback;
}

DVector3 VKOpenXRDeviceEyePose::GetViewShift(FRenderViewpoint& vp) const
{
	auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
	const float hmdHeight = GetHmdAdjustedHeightInMapUnit(mode.xrUsingStageSpace ? false : mode.xrHasLocalHeightAnchor, mode.xrLocalHeightAnchor);
	const player_t* player = &players[consoleplayer];
	const float playerHeight = (player && player->mo) ? GetDoomPlayerHeightWithoutCrouch(player) : hmdHeight;
	DVector3 shift = { 0.0, 0.0, hmdHeight - playerHeight };

	if (eye >= 0 && (size_t)eye < mode.xrViews.size())
	{
		const XrQuaternionf headOrientation = mode.xrViews[0].pose.orientation;
		const XrVector3f headRight = NormalizeVector(RotateVector(headOrientation, { 1.0f, 0.0f, 0.0f }));
		const XrVector3f eyeOffsetMeters = {
			currentEyePose.position.x - hmdPosition[0],
			currentEyePose.position.y - hmdPosition[1],
			currentEyePose.position.z - hmdPosition[2]
		};

		const double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
		const double stereoShift = DotVector(eyeOffsetMeters, headRight) * vr_vunits_per_meter * vr_openxr_eye_shift_scale * pixelstretch;
		const double yaw = DEG2RAD(vp.HWAngles.Yaw.Degrees());
		shift.X += -cos(yaw) * stereoShift;
		shift.Y += sin(yaw) * stereoShift;
	}

	if (vr_debug_projection_compare)
	{
		static bool loggedShift[2] = { false, false };
		if (eye >= 0 && eye < 2 && !loggedShift[eye])
		{
			Printf("VR_PROJ OpenXR eye=%d shift=(%.3f, %.3f, %.3f) eyePos=(%.4f, %.4f, %.4f) hmdPos=(%.4f, %.4f, %.4f) hmdHeight=%.3f playerHeight=%.3f stage=%d\n",
				eye, (double)shift.X, (double)shift.Y, (double)shift.Z,
				(double)currentEyePose.position.x, (double)currentEyePose.position.y, (double)currentEyePose.position.z,
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
	mode.mInVRSceneRender = mode.ShouldUseRecommendedRenderSizeThisFrame();
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
	{
		static bool loggedMountedHudBypass = false;
		if (!loggedMountedHudBypass && (vr_openxr_debug_present || vr_debug_projection_compare))
		{
			Printf("OpenXR HUD: skipping camera HUD projection because mounted HUD path is active.\n");
			loggedMountedHudBypass = true;
		}
		return;
	}
	VSMatrix hudProj = GetHUDProjection();
	static bool loggedCameraHudPath = false;
	if (!loggedCameraHudPath && (vr_openxr_debug_present || vr_debug_projection_compare))
	{
		Printf("OpenXR HUD: using camera-mounted HUD projection path (mounted HUD inactive).\n");
		loggedCameraHudPath = true;
	}

	auto* di = HWDrawInfo::StartDrawInfo(r_viewpoint.ViewLevel, nullptr, r_viewpoint, nullptr);
	if (di)
	{
		di->VPUniforms.mViewMatrix.loadIdentity();
		di->VPUniforms.mProjectionMatrix = hudProj;
		ApplyVPUniforms(di);
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

	di->VPUniforms.mViewMatrix.loadIdentity();
	di->VPUniforms.mProjectionMatrix.loadIdentity();
	di->VPUniforms.mProjectionMatrix.translate(-1, 1, 0);
	di->VPUniforms.mProjectionMatrix.scale(2.0 / SCREENWIDTH, -2.0 / SCREENHEIGHT, -1.0);
	di->ProjectionMatrix2 = di->VPUniforms.mProjectionMatrix;
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

bool VKOpenXRDeviceMode::GetRecommendedRenderSize(int& outWidth, int& outHeight) const
{
	int width = 0;
	int height = 0;

	if (sceneWidth != 0 && sceneHeight != 0)
	{
		width = (int)sceneWidth;
		height = (int)sceneHeight;
	}
	else if (!xrViewConfigs.empty() && xrViewConfigs[0].recommendedImageRectWidth != 0 && xrViewConfigs[0].recommendedImageRectHeight != 0)
	{
		width = (int)xrViewConfigs[0].recommendedImageRectWidth;
		height = (int)xrViewConfigs[0].recommendedImageRectHeight;
	}
	else
	{
		outWidth = 0;
		outHeight = 0;
		return false;
	}

	const float renderScale = clamp<float>(vr_openxr_render_scale, 0.25f, 4.0f);
	outWidth = std::max(1, (int)std::lround(width * renderScale));
	outHeight = std::max(1, (int)std::lround(height * renderScale));
	return true;
}

bool VKOpenXRDeviceMode::ShouldUseRecommendedRenderSizeThisFrame() const
{
	return mFrameRenderMode == FrameRenderMode::GameplayEyes;
}

bool VKOpenXRDeviceMode::ShouldUseScreenLayerForCurrentFrame() const
{
	return mFrameRenderMode != FrameRenderMode::GameplayEyes;
}

static void ApplyOpenXRGameplayViewport(DFrameBuffer* screen, int width, int height)
{
	screen->mSceneViewport.left = 0;
	screen->mSceneViewport.top = 0;
	screen->mSceneViewport.width = width;
	screen->mSceneViewport.height = height;
	screen->mScreenViewport.left = 0;
	screen->mScreenViewport.top = 0;
	screen->mScreenViewport.width = width;
	screen->mScreenViewport.height = height;
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
	const float stereoSeparation =
		(vr_ipd * 0.5f) * vr_vunits_per_meter * hudStereo * (eye == 1 ? -1.0f : 1.0f);
	hudProjection.translate(stereoSeparation, 0.0f, 0.0f);

	hudProjection.scale(-vr_vunits_per_meter, vr_vunits_per_meter, -vr_vunits_per_meter);

	const double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;
	hudProjection.scale(1.0, (FLOATTYPE)pixelstretch, 1.0);

	if (eye >= 0)
	{
		const auto& mode = const_cast<VKOpenXRDeviceMode&>((const VKOpenXRDeviceMode&)VKOpenXRDeviceMode::getInstance());
		if ((size_t)eye < mode.xrViews.size())
		{
			const XrQuaternionf headOrientation = mode.xrViews[0].pose.orientation;
			const XrVector3f eyeOffsetMeters = {
				currentEyePose.position.x - hmdPosition[0],
				currentEyePose.position.y - hmdPosition[1],
				currentEyePose.position.z - hmdPosition[2]
			};
			const XrVector3f localEyeOffset = RotateVector(ConjugateQuaternion(headOrientation), eyeOffsetMeters);
			// Match OpenVR's eye-to-head transform contribution so HUD depth, size, and stereo
			hudProjection.translate(localEyeOffset.x, localEyeOffset.y, localEyeOffset.z);
		}
	}

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

	float screenWidth = (float)SCREENWIDTH;
	float screenHeight = (float)SCREENHEIGHT;
	hudProjection.translate(-1.0f, 1.0f, 0.0f);
	hudProjection.scale(2.0f / screenWidth, -2.0f / screenHeight, -1.0f);

	// Match OpenVR/GL OpenXR behavior: compose eye projection with the HUD
	// transform so the HUD is camera-anchored in front of the user.
	VSMatrix finalProjection(projection);
	finalProjection.multMatrix(hudProjection);

	if (vr_openxr_debug_present || vr_debug_projection_compare)
	{
		static bool loggedHudProjection[2] = { false, false };
		if (eye >= 0 && eye < 2 && !loggedHudProjection[eye])
		{
			const FLOATTYPE* m = finalProjection.get();
			Printf("OpenXR HUD eye=%d stereo=%.3f dist=%.3f scale=%.3f projRow0=[%.5f %.5f %.5f %.5f] projRow1=[%.5f %.5f %.5f %.5f]\n",
				eye,
				(double)hudStereo,
				(double)getHUDValue<FFloatCVarRef>(vr_automap_distance, vr_hud_distance),
				(double)getHUDValue<FFloatCVarRef>(vr_automap_scale, vr_hud_scale),
				(double)m[0], (double)m[4], (double)m[8], (double)m[12],
				(double)m[1], (double)m[5], (double)m[9], (double)m[13]);
			loggedHudProjection[eye] = true;
		}
	}

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
		xrSelectAction != XR_NULL_HANDLE || xrMenuAction != XR_NULL_HANDLE || xrGripAction != XR_NULL_HANDLE ||
		xrThumbClickAction != XR_NULL_HANDLE || xrThumbstickAction != XR_NULL_HANDLE ||
		xrPrimaryAction != XR_NULL_HANDLE || xrSecondaryAction != XR_NULL_HANDLE || xrVkInstance != nullptr ||
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
	// Some runtimes expose the loader DLL but do not support Vulkan OpenXR.
	const bool hasVulkanEnable = HasOpenXRExtension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	const bool hasVulkanEnable2 = HasOpenXRExtension(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	if (!hasVulkanEnable && !hasVulkanEnable2)
	{
		Printf("OpenXR: runtime does not advertise %s or %s, skipping OpenXR initialization.\n",
			XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
		return fail();
	}


	std::vector<const char*> extensions;
	if (hasVulkanEnable)
		extensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
	if (hasVulkanEnable2)
		extensions.push_back(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
	xrHasEquirectBackdrop = HasOpenXRExtension(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
	if (xrHasEquirectBackdrop)
	{
		extensions.push_back(XR_KHR_COMPOSITION_LAYER_EQUIRECT_EXTENSION_NAME);
	}
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
	loadProc("xrGetVulkanGraphicsRequirements2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsRequirements2KHR_inst));
	loadProc("xrGetVulkanGraphicsDevice2KHR", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsDevice2KHR_inst));

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
	if (xrGetVulkanGraphicsRequirements2KHR_inst)
	{
		if (XR_FAILED(xrGetVulkanGraphicsRequirements2KHR_inst(xrInstance, xrSystemId, &graphicsRequirements)))
		{
			return fail();
		}
	}
	else if (xrGetVulkanGraphicsRequirementsKHR_inst)
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
	if (xrGetVulkanGraphicsDevice2KHR_inst)
	{
		XrVulkanGraphicsDeviceGetInfoKHR getInfo{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
		getInfo.systemId = xrSystemId;
		getInfo.vulkanInstance = xrVkInstance->Instance;
		if (XR_FAILED(xrGetVulkanGraphicsDevice2KHR_inst(xrInstance, &getInfo, &xrPhysicalDevice)))
		{
			return fail();
		}
		if (xrVkDevice->PhysicalDevice.Device != xrPhysicalDevice)
		{
			return fail();
		}
	}
	else if (xrGetVulkanGraphicsDeviceKHR_inst)
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
	if (XR_FAILED(xrResult) && ShouldRetryCreateSession(xrResult))
	{
		xrResult = xrCreateSession(xrInstance, &sessionInfo, &xrSession);
	}
	if (XR_FAILED(xrResult))
	{
		return fail();
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

	xrStringToPath(xrInstance, "/user/hand/left", &xrLeftHandPath);
	xrStringToPath(xrInstance, "/user/hand/right", &xrRightHandPath);
	const XrPath subactionPaths[2] = { xrLeftHandPath, xrRightHandPath };

	auto createAction = [&](const char* name, const char* localizedName, XrActionType type, XrAction& out)
	{
		XrActionCreateInfo actionInfo{ XR_TYPE_ACTION_CREATE_INFO };
		actionInfo.actionType = type;
		strncpy(actionInfo.actionName, name, sizeof(actionInfo.actionName) - 1);
		strncpy(actionInfo.localizedActionName, localizedName, sizeof(actionInfo.localizedActionName) - 1);
		actionInfo.countSubactionPaths = 2;
		actionInfo.subactionPaths = subactionPaths;
		xrCreateAction(xrActionSet, &actionInfo, &out);
	};

	createAction("hand_pose", "Hand Pose", XR_ACTION_TYPE_POSE_INPUT, xrPoseAction);
	createAction("select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT, xrSelectAction);
	createAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, xrMenuAction);
	createAction("grip", "Grip", XR_ACTION_TYPE_BOOLEAN_INPUT, xrGripAction);
	createAction("thumb_click", "Thumb Click", XR_ACTION_TYPE_BOOLEAN_INPUT, xrThumbClickAction);
	createAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, xrThumbstickAction);
	createAction("trackpad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT, xrTrackpadAction);
	createAction("button_a", "Button A", XR_ACTION_TYPE_BOOLEAN_INPUT, xrAAction);
	createAction("button_b", "Button B", XR_ACTION_TYPE_BOOLEAN_INPUT, xrBAction);
	createAction("button_x", "Button X", XR_ACTION_TYPE_BOOLEAN_INPUT, xrXAction);
	createAction("button_y", "Button Y", XR_ACTION_TYPE_BOOLEAN_INPUT, xrYAction);
	createAction("primary", "Primary", XR_ACTION_TYPE_BOOLEAN_INPUT, xrPrimaryAction);
	createAction("secondary", "Secondary", XR_ACTION_TYPE_BOOLEAN_INPUT, xrSecondaryAction);
	createAction("haptic", "Haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, xrHapticAction);

	XrPath leftTriggerClickPath = XR_NULL_PATH;
	XrPath rightTriggerClickPath = XR_NULL_PATH;
	XrPath leftTriggerValuePath = XR_NULL_PATH;
	XrPath rightTriggerValuePath = XR_NULL_PATH;
	XrPath leftSqueezeClickPath = XR_NULL_PATH;
	XrPath rightSqueezeClickPath = XR_NULL_PATH;
	XrPath leftSqueezeValuePath = XR_NULL_PATH;
	XrPath rightSqueezeValuePath = XR_NULL_PATH;
	XrPath leftThumbClickPath = XR_NULL_PATH;
	XrPath rightThumbClickPath = XR_NULL_PATH;
	XrPath leftThumbstickPath = XR_NULL_PATH;
	XrPath rightThumbstickPath = XR_NULL_PATH;
	XrPath leftTrackpadPath = XR_NULL_PATH;
	XrPath rightTrackpadPath = XR_NULL_PATH;
	XrPath leftTrackpadClickPath = XR_NULL_PATH;
	XrPath rightTrackpadClickPath = XR_NULL_PATH;
	XrPath leftTrackpadTouchPath = XR_NULL_PATH;
	XrPath rightTrackpadTouchPath = XR_NULL_PATH;
	XrPath leftHapticPath = XR_NULL_PATH;
	XrPath rightHapticPath = XR_NULL_PATH;
	XrPath leftMenuClickPath = XR_NULL_PATH;
	XrPath rightMenuClickPath = XR_NULL_PATH;
	XrPath leftSelectClickPath = XR_NULL_PATH;
	XrPath rightSelectClickPath = XR_NULL_PATH;
	XrPath leftGripPosePath = XR_NULL_PATH;
	XrPath rightGripPosePath = XR_NULL_PATH;
	XrPath leftAimPosePath = XR_NULL_PATH;
	XrPath rightAimPosePath = XR_NULL_PATH;
	XrPath leftXClickPath = XR_NULL_PATH;
	XrPath leftYClickPath = XR_NULL_PATH;
	XrPath leftPrimaryClickPath = XR_NULL_PATH;
	XrPath rightPrimaryClickPath = XR_NULL_PATH;
	XrPath leftSecondaryClickPath = XR_NULL_PATH;
	XrPath rightSecondaryClickPath = XR_NULL_PATH;

	xrStringToPath(xrInstance, "/user/hand/left/input/trigger/click", &leftTriggerClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trigger/click", &rightTriggerClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trigger/value", &leftTriggerValuePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trigger/value", &rightTriggerValuePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/squeeze/click", &leftSqueezeClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/squeeze/click", &rightSqueezeClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/squeeze/value", &leftSqueezeValuePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/squeeze/value", &rightSqueezeValuePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/thumbstick/click", &leftThumbClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/thumbstick/click", &rightThumbClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/thumbstick", &leftThumbstickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/thumbstick", &rightThumbstickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trackpad", &leftTrackpadPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trackpad", &rightTrackpadPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trackpad/click", &leftTrackpadClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trackpad/click", &rightTrackpadClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/trackpad/touch", &leftTrackpadTouchPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/trackpad/touch", &rightTrackpadTouchPath);
	xrStringToPath(xrInstance, "/user/hand/left/output/haptic", &leftHapticPath);
	xrStringToPath(xrInstance, "/user/hand/right/output/haptic", &rightHapticPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/menu/click", &leftMenuClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/menu/click", &rightMenuClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/select/click", &leftSelectClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/select/click", &rightSelectClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/grip/pose", &leftGripPosePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/grip/pose", &rightGripPosePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/aim/pose", &leftAimPosePath);
	xrStringToPath(xrInstance, "/user/hand/right/input/aim/pose", &rightAimPosePath);
	xrStringToPath(xrInstance, "/user/hand/left/input/x/click", &leftXClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/y/click", &leftYClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/a/click", &leftPrimaryClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/a/click", &rightPrimaryClickPath);
	xrStringToPath(xrInstance, "/user/hand/left/input/b/click", &leftSecondaryClickPath);
	xrStringToPath(xrInstance, "/user/hand/right/input/b/click", &rightSecondaryClickPath);

	XrPath simpleProfile = XR_NULL_PATH;
	XrPath viveProfile = XR_NULL_PATH;
	XrPath touchProfile = XR_NULL_PATH;
	XrPath indexProfile = XR_NULL_PATH;
	XrPath wmrProfile = XR_NULL_PATH;
	xrStringToPath(xrInstance, "/interaction_profiles/khr/simple_controller", &simpleProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/htc/vive_controller", &viveProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/oculus/touch_controller", &touchProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/valve/index_controller", &indexProfile);
	xrStringToPath(xrInstance, "/interaction_profiles/microsoft/motion_controller", &wmrProfile);

	std::vector<XrActionSuggestedBinding> simpleBindings;
	AddBinding(simpleBindings, xrSelectAction, leftSelectClickPath);
	AddBinding(simpleBindings, xrSelectAction, rightSelectClickPath);
	AddBinding(simpleBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(simpleBindings, xrMenuAction, rightMenuClickPath);
	AddBinding(simpleBindings, xrPoseAction, leftAimPosePath);
	AddBinding(simpleBindings, xrPoseAction, rightAimPosePath);
	AddBinding(simpleBindings, xrHapticAction, leftHapticPath);
	AddBinding(simpleBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, simpleProfile, "KHR simple", simpleBindings);

	std::vector<XrActionSuggestedBinding> viveBindings;
	AddBinding(viveBindings, xrSelectAction, leftTriggerClickPath);
	AddBinding(viveBindings, xrSelectAction, rightTriggerClickPath);
	AddBinding(viveBindings, xrGripAction, leftSqueezeClickPath);
	AddBinding(viveBindings, xrGripAction, rightSqueezeClickPath);
	AddBinding(viveBindings, xrTrackpadAction, leftTrackpadPath);
	AddBinding(viveBindings, xrTrackpadAction, rightTrackpadPath);
	AddBinding(viveBindings, xrThumbClickAction, leftTrackpadClickPath);
	AddBinding(viveBindings, xrThumbClickAction, rightTrackpadClickPath);
	AddBinding(viveBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(viveBindings, xrMenuAction, rightMenuClickPath);
	AddBinding(viveBindings, xrPoseAction, leftAimPosePath);
	AddBinding(viveBindings, xrPoseAction, rightAimPosePath);
	AddBinding(viveBindings, xrHapticAction, leftHapticPath);
	AddBinding(viveBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, viveProfile, "Vive", viveBindings);

	std::vector<XrActionSuggestedBinding> touchBindings;
	AddBinding(touchBindings, xrSelectAction, leftTriggerValuePath);
	AddBinding(touchBindings, xrSelectAction, rightTriggerValuePath);
	AddBinding(touchBindings, xrGripAction, leftSqueezeValuePath);
	AddBinding(touchBindings, xrGripAction, rightSqueezeValuePath);
	AddBinding(touchBindings, xrThumbClickAction, leftThumbClickPath);
	AddBinding(touchBindings, xrThumbClickAction, rightThumbClickPath);
	AddBinding(touchBindings, xrThumbstickAction, leftThumbstickPath);
	AddBinding(touchBindings, xrThumbstickAction, rightThumbstickPath);
	AddBinding(touchBindings, xrXAction, leftXClickPath);
	AddBinding(touchBindings, xrYAction, leftYClickPath);
	AddBinding(touchBindings, xrAAction, rightPrimaryClickPath);
	AddBinding(touchBindings, xrBAction, rightSecondaryClickPath);
	AddBinding(touchBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(touchBindings, xrPoseAction, leftAimPosePath);
	AddBinding(touchBindings, xrPoseAction, rightAimPosePath);
	AddBinding(touchBindings, xrHapticAction, leftHapticPath);
	AddBinding(touchBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, touchProfile, "Oculus Touch", touchBindings);

	std::vector<XrActionSuggestedBinding> indexBindings;
	AddBinding(indexBindings, xrSelectAction, leftTriggerValuePath);
	AddBinding(indexBindings, xrSelectAction, rightTriggerValuePath);
	AddBinding(indexBindings, xrGripAction, leftSqueezeValuePath);
	AddBinding(indexBindings, xrGripAction, rightSqueezeValuePath);
	AddBinding(indexBindings, xrThumbClickAction, leftThumbClickPath);
	AddBinding(indexBindings, xrThumbClickAction, rightThumbClickPath);
	AddBinding(indexBindings, xrThumbstickAction, leftThumbstickPath);
	AddBinding(indexBindings, xrThumbstickAction, rightThumbstickPath);
	AddBinding(indexBindings, xrXAction, leftXClickPath);
	AddBinding(indexBindings, xrYAction, leftYClickPath);
	AddBinding(indexBindings, xrAAction, rightPrimaryClickPath);
	AddBinding(indexBindings, xrBAction, rightSecondaryClickPath);
	AddBinding(indexBindings, xrPoseAction, leftAimPosePath);
	AddBinding(indexBindings, xrPoseAction, rightAimPosePath);
	AddBinding(indexBindings, xrHapticAction, leftHapticPath);
	AddBinding(indexBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, indexProfile, "Valve Index", indexBindings);

	std::vector<XrActionSuggestedBinding> wmrBindings;
	AddBinding(wmrBindings, xrSelectAction, leftTriggerValuePath);
	AddBinding(wmrBindings, xrSelectAction, rightTriggerValuePath);
	AddBinding(wmrBindings, xrGripAction, leftSqueezeClickPath);
	AddBinding(wmrBindings, xrGripAction, rightSqueezeClickPath);
	AddBinding(wmrBindings, xrThumbClickAction, leftThumbClickPath);
	AddBinding(wmrBindings, xrThumbClickAction, rightThumbClickPath);
	AddBinding(wmrBindings, xrThumbstickAction, leftThumbstickPath);
	AddBinding(wmrBindings, xrThumbstickAction, rightThumbstickPath);
	AddBinding(wmrBindings, xrMenuAction, leftMenuClickPath);
	AddBinding(wmrBindings, xrMenuAction, rightMenuClickPath);
	AddBinding(wmrBindings, xrPoseAction, leftAimPosePath);
	AddBinding(wmrBindings, xrPoseAction, rightAimPosePath);
	AddBinding(wmrBindings, xrHapticAction, leftHapticPath);
	AddBinding(wmrBindings, xrHapticAction, rightHapticPath);
	SuggestBindingsForProfile(xrInstance, wmrProfile, "WMR", wmrBindings);

	XrSessionActionSetsAttachInfo attachInfo{ XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attachInfo.countActionSets = 1;
	attachInfo.actionSets = &xrActionSet;
	xrAttachSessionActionSets(xrSession, &attachInfo);

	for (int i = 0; i < 2; ++i)
	{
		XrActionSpaceCreateInfo actionSpaceInfo{ XR_TYPE_ACTION_SPACE_CREATE_INFO };
		actionSpaceInfo.action = xrPoseAction;
		actionSpaceInfo.subactionPath = subactionPaths[i]; 
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
	xrVirtualScreenSwapchainFormat = SelectFlatSwapchainFormat(runtimeFormats, preferredFormat);
	Printf("OpenXR: preferred scene swapchain format=%d selected=%d srgb=%d runtimeFormats=%u.\n",
		(int)preferredFormat,
		(int)xrSwapchainFormat,
		IsSRGBSwapchainFormat((VkFormat)xrSwapchainFormat) ? 1 : 0,
		formatCount);
	Printf("OpenXR: preferred flat swapchain format=%d selected=%d srgb=%d.\n",
		(int)preferredFormat,
		(int)xrVirtualScreenSwapchainFormat,
		IsSRGBSwapchainFormat((VkFormat)xrVirtualScreenSwapchainFormat) ? 1 : 0);

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
	if (!xrMirrorPresentTextures.empty())
		xrDeferredMirrorPresentTextures.emplace_back(std::move(xrMirrorPresentTextures));
	xrMirrorPresentTextures.clear();

	for (auto& texture : xrPresentTextures)
		texture.Reset(vkfb);
	for (auto& texture : xrMirrorPresentTextures)
		texture.Reset(vkfb);

	xrPresentTextures.resize(xrViewCount);
	xrMirrorPresentTextures.resize(xrViewCount);
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

		auto& mirrorTexture = xrMirrorPresentTextures[i];
		mirrorTexture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		mirrorTexture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		mirrorTexture.Image = ImageBuilder()
			.Format(presentFormat)
			.Size(width, height)
			.Usage(VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
			.DebugName("OpenXRMirrorPresentTexture")
			.Create(vkfb->device.get());
		mirrorTexture.View = ImageViewBuilder()
			.Image(mirrorTexture.Image.get(), presentFormat)
			.DebugName("OpenXRMirrorPresentTextureView")
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
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVirtualScreenSwapchainFormat == VK_FORMAT_UNDEFINED || width == 0 || height == 0)
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
	swapchainInfo.format = xrVirtualScreenSwapchainFormat;
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
			.Image(texture.Image.get(), (VkFormat)xrVirtualScreenSwapchainFormat)
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
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVirtualScreenSwapchainFormat == VK_FORMAT_UNDEFINED || width == 0 || height == 0)
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
	swapchainInfo.format = xrVirtualScreenSwapchainFormat;
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
			.Image(texture.Image.get(), (VkFormat)xrVirtualScreenSwapchainFormat)
			.DebugName("OpenXR.VirtualScreenBackdropView")
			.Create(xrVkDevice.get());
	}

	xrVirtualScreenBackdropVisible = true;
	return true;
}

bool VKOpenXRDeviceMode::CreateMenuPointerBeamSwapchain() const
{
	constexpr uint32_t beamW = 8;
	constexpr uint32_t beamH = 8;
	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrSwapchainFormat == VK_FORMAT_UNDEFINED)
		return false;
	if (xrMenuPointerBeamSwapchain != XR_NULL_HANDLE && !xrMenuPointerBeamTextures.empty())
		return true;

	if (!xrMenuPointerBeamTextures.empty())
		xrDeferredMenuPointerBeamTextures.emplace_back(std::move(xrMenuPointerBeamTextures));
	xrMenuPointerBeamTextures.clear();

	DestroyMenuPointerBeamSwapchain();

	XrSwapchainCreateInfo swapchainInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	swapchainInfo.format = xrSwapchainFormat;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = beamW;
	swapchainInfo.height = beamH;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	if (XR_FAILED(xrCreateSwapchain(xrSession, &swapchainInfo, &xrMenuPointerBeamSwapchain)))
		return false;

	uint32_t imageCount = 0;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrMenuPointerBeamSwapchain, 0, &imageCount, nullptr)) || imageCount == 0)
	{
		DestroyMenuPointerBeamSwapchain();
		return false;
	}

	xrMenuPointerBeamSwapchainImages.resize(imageCount);
	for (auto& image : xrMenuPointerBeamSwapchainImages)
		image.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	if (XR_FAILED(xrEnumerateSwapchainImages(xrMenuPointerBeamSwapchain, imageCount, &imageCount,
		reinterpret_cast<XrSwapchainImageBaseHeader*>(xrMenuPointerBeamSwapchainImages.data()))))
	{
		DestroyMenuPointerBeamSwapchain();
		return false;
	}
	xrMenuPointerBeamSwapchainImages.resize(imageCount);

	xrMenuPointerBeamTextures.resize(imageCount);
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		auto& texture = xrMenuPointerBeamTextures[i];
		texture.Layout = VK_IMAGE_LAYOUT_UNDEFINED;
		texture.AspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		texture.Image = std::make_unique<VulkanImage>(xrVkDevice.get(), xrMenuPointerBeamSwapchainImages[i].image, nullptr,
			(int)beamW, (int)beamH, 1, 1);
		texture.View = ImageViewBuilder()
			.Type(VK_IMAGE_VIEW_TYPE_2D)
			.Image(texture.Image.get(), (VkFormat)xrSwapchainFormat)
			.DebugName("OpenXR.MenuPointerBeamView")
			.Create(xrVkDevice.get());
	}
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

void VKOpenXRDeviceMode::DestroyMenuPointerBeamSwapchain() const
{
	xrMenuPointerBeamImageIndex = -1;
	xrMenuPointerBeamTextures.clear();
	xrMenuPointerBeamSwapchainImages.clear();
	if (xrMenuPointerBeamSwapchain != XR_NULL_HANDLE)
	{
		xrDestroySwapchain(xrMenuPointerBeamSwapchain);
		xrMenuPointerBeamSwapchain = XR_NULL_HANDLE;
	}
}

void VKOpenXRDeviceMode::DestroyOpenXR() const
{
	StopHaptics();
	DestroyVirtualScreenSwapchain();
	DestroyVirtualScreenBackdropSwapchain();
	DestroyMenuPointerBeamSwapchain();
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
		if (xrHapticAction != XR_NULL_HANDLE)
		{
			xrDestroyAction(xrHapticAction);
			xrHapticAction = XR_NULL_HANDLE;
		}
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
	xrHasEquirectBackdrop = false;
	xrHasFBColorSpace = false;
	xrUsingStageSpace = false;
	xrHasLocalHeightAnchor = false;
	xrLocalHeightAnchor = 0.0f;
	xrPoseAction = XR_NULL_HANDLE;
	xrSelectAction = XR_NULL_HANDLE;
	xrMenuAction = XR_NULL_HANDLE;
	xrGripAction = XR_NULL_HANDLE;
	xrThumbClickAction = XR_NULL_HANDLE;
	xrThumbstickAction = XR_NULL_HANDLE;
	xrAAction = XR_NULL_HANDLE;
	xrBAction = XR_NULL_HANDLE;
	xrXAction = XR_NULL_HANDLE;
	xrYAction = XR_NULL_HANDLE;
	xrPrimaryAction = XR_NULL_HANDLE;
	xrSecondaryAction = XR_NULL_HANDLE;
	xrLeftHandPath = XR_NULL_PATH;
	xrRightHandPath = XR_NULL_PATH;
	xrHandPoseValid[0] = false;
	xrHandPoseValid[1] = false;
	xrLastSelectState[0] = xrLastSelectState[1] = false;
	xrLastMenuState[0] = xrLastMenuState[1] = false;
	xrLastGripState[0] = xrLastGripState[1] = false;
	xrLastThumbClickState[0] = xrLastThumbClickState[1] = false;
	xrLastTrackpadClickState[0] = xrLastTrackpadClickState[1] = false;
	xrLastAState[0] = xrLastAState[1] = false;
	xrLastBState[0] = xrLastBState[1] = false;
	xrLastXState[0] = xrLastXState[1] = false;
	xrLastYState[0] = xrLastYState[1] = false;
	xrLastPrimaryState[0] = xrLastPrimaryState[1] = false;
	xrLastSecondaryState[0] = xrLastSecondaryState[1] = false;
	xrLastThumbstickState[0] = { 0.0f, 0.0f };
	xrLastThumbstickState[1] = { 0.0f, 0.0f };
	xrLastTrackpadState[0] = { 0.0f, 0.0f };
	xrLastTrackpadState[1] = { 0.0f, 0.0f };
	xrLastMenuReturnState = false;
	xrLastMenuBackState = false;
	xrLastMenuBackspaceState = false;
	xrLoggedWeaponState = false;
	StopHaptics();
	xrHapticAction = XR_NULL_HANDLE;
	xrHapticDuration[0] = xrHapticDuration[1] = 0.0;
	xrHapticIntensity[0] = xrHapticIntensity[1] = 0.0f;
	xrHapticActive[0] = xrHapticActive[1] = false;
	m_TeleportTarget = 0;
	m_TeleportLocation = DVector3(0, 0, 0);
	xrSwapchainImages.clear();
	xrSwapchainTextures.clear();
	xrPresentTextures.clear();
	xrMirrorPresentTextures.clear();
	xrDeferredPresentTextures.clear();
	xrDeferredMirrorPresentTextures.clear();
	xrViewConfigs.clear();
	xrViews.clear();
	xrProjectionViews.clear();
	xrViewCount = 0;
	xrCurrentImageIndex = -1;
	xrSwapchainFormat = VK_FORMAT_UNDEFINED;
	xrVirtualScreenSwapchainFormat = VK_FORMAT_UNDEFINED;
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
	xrDeferredMenuPointerBeamTextures.clear();
}

void VKOpenXRDeviceMode::PurgeDeferredOpenXRResources() const
{
	xrDeferredPresentTextures.clear();
	xrDeferredMirrorPresentTextures.clear();
	xrDeferredVirtualScreenTextures.clear();
	xrDeferredVirtualScreenBackdropTextures.clear();
	xrDeferredMenuPointerBeamTextures.clear();
}

VKOpenXRDeviceMode::FrameRenderMode VKOpenXRDeviceMode::DetermineFrameRenderMode() const
{
	const bool forceVirtualScreen = gamestate == GS_LEVEL && menuactive == MENU_Off && (cinemamode || vr_overlayscreen_always);
	return (IsGameplaySceneActive() && !forceVirtualScreen) ? FrameRenderMode::GameplayEyes : FrameRenderMode::VirtualScreen;
}

void VKOpenXRDeviceMode::ApplyFrameRenderMode(FrameRenderMode mode) const
{
	mFrameRenderMode = mode;

	if (mode == FrameRenderMode::GameplayEyes)
	{
		QzDoom_setUseScreenLayer(false);
	}
	else
	{
		QzDoom_setUseScreenLayer(true);
	}
}

void VKOpenXRDeviceMode::SetUp() const
{
	ApplyFrameRenderMode(DetermineFrameRenderMode());

	super::SetUp();
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
		isSetup = true;
	}

	if (xrSession == XR_NULL_HANDLE) return;
	PollXREvents();
	ApplyFrameRenderMode(DetermineFrameRenderMode());

	UpdateControllerState();

	player_t* player = &players[consoleplayer];
	if (gamestate == GS_LEVEL && resetDoomYaw && player != nullptr && player->mo != nullptr)
	{
		doomYaw = (float)player->mo->Angles.Yaw.Degrees();
		resetDoomYaw = false;
	}
	else if (gamestate != GS_LEVEL || menuactive != MENU_Off
		|| ConsoleState == c_down || ConsoleState == c_falling
		|| (player && player->playerstate == PST_DEAD)
		|| (player && player->resetDoomYaw)
		|| paused)
	{
		resetDoomYaw = true;
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
				StopHaptics();
				xrEndSession(xrSession);
				isSessionRunning = false;
				isSessionReadyToBegin = false;
			}
			else if (ev->state == XR_SESSION_STATE_LOSS_PENDING || ev->state == XR_SESSION_STATE_EXITING)
			{
				StopHaptics();
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
	VR_SetHMDPosition(hmdPosition[0], hmdPosition[1], hmdPosition[2]);
	VR_SetHMDOrientation(hmdorientation[0], hmdorientation[1], hmdorientation[2]);
	positional_movementSideways = 0.0f;
	positional_movementForward = 0.0f;

	if (gamestate == GS_LEVEL && menuactive == MENU_Off && !paused)
	{
		const float rotation = GetViewpointYaw() - hmdorientation[1];
		DVector2 rotated = DVector2(positionDeltaThisFrame[0], positionDeltaThisFrame[2]).Rotated(DAngle::fromDeg(-rotation));
		positional_movementSideways = rotated.Y;
		positional_movementForward = rotated.X;
	}

	if (!xrUsingStageSpace && !xrHasLocalHeightAnchor)
	{
		const player_t* player = &players[consoleplayer];
		if (player != nullptr && player->mo != nullptr)
		{
			xrLocalHeightAnchor = GetDoomPlayerHeightWithoutCrouch(player) - GetRawHmdHeightInMapUnit();
			xrHasLocalHeightAnchor = true;
		}
	}

	if (gamestate != GS_LEVEL || menuactive != MENU_Off || r_viewpoint.camera == nullptr || r_viewpoint.ViewLevel == nullptr)
		return;

	if (vr_debug_projection_compare)
	{
		const float pixelstretch = r_viewpoint.ViewLevel ? (float)r_viewpoint.ViewLevel->pixelstretch : 1.2f;
		const player_t* player = &players[consoleplayer];
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
	const float currentHmdYaw = hmdorientation[1] + snapTurn;
	const bool lockGameplayViewToScreenLayer = ShouldUseScreenLayerForCurrentFrame();
	if (!havePreviousYaw)
	{
		previousHmdYaw = currentHmdYaw;
		doomYaw = r_viewpoint.Angles.Yaw.Degrees();
		havePreviousYaw = true;
	}
	float hmdYawDeltaDegrees = currentHmdYaw - previousHmdYaw;
	if (!lockGameplayViewToScreenLayer)
	{
		G_AddViewAngle(mAngleFromRadians((float)DEG2RAD(-hmdYawDeltaDegrees)));
	}
	previousHmdYaw = currentHmdYaw;

	if (!lockGameplayViewToScreenLayer && gamestate == GS_LEVEL && menuactive == MENU_Off)
	{
		doomYaw += hmdYawDeltaDegrees;
		vp.HWAngles.Roll = FAngle::fromDeg(-r);
		vp.HWAngles.Pitch = FAngle::fromDeg(-p);
		double viewYaw = GetViewpointYaw();
		while (viewYaw <= -180.0) viewYaw += 360.0;
		while (viewYaw > 180.0) viewYaw -= 360.0;
		vp.Angles.Yaw = DAngle::fromDeg(viewYaw);
	}
}

void VKOpenXRDeviceMode::UpdateControllerState() const
{
	if (xrSession == XR_NULL_HANDLE || xrSpace == XR_NULL_HANDLE || xrActionSet == XR_NULL_HANDLE)
		return;
	if (!isSessionRunning)
		return;

	if (xrFrameState.predictedDisplayTime == 0)
		return;

	XrActiveActionSet activeActionSet{};
	activeActionSet.actionSet = xrActionSet;

	XrActionsSyncInfo syncInfo{ XR_TYPE_ACTIONS_SYNC_INFO };
	syncInfo.countActiveActionSets = 1;
	syncInfo.activeActionSets = &activeActionSet;
	const XrResult syncResult = xrSyncActions(xrSession, &syncInfo);
	if (XR_FAILED(syncResult))
	{
		Printf("OpenXR: xrSyncActions failed result=%d\n", (int)syncResult);
	}

	const bool menuMode = menuactive != MENU_Off;
	const bool gameplayMode = gamestate == GS_LEVEL && !menuMode && !paused;
	const int mainHand = GetMainHandIndex();
	const int offHand = GetOffHandIndex();
	const int movementHand = *vr_switch_sticks ? mainHand : offHand;
	const int turnHand = *vr_switch_sticks ? offHand : mainHand;
	const bool useTrackpad = IsCurrentInteractionProfile(xrInstance, xrSession, xrRightHandPath, "vive_controller");
	static bool loggedProfile = false;
	if (!loggedProfile)
	{
		Printf("OpenXR: controller profile %s, locomotion=%s\n",
			useTrackpad ? "vive/trackpad" : "thumbstick",
			"left-move/right-turn");
		loggedProfile = true;
	}

	OpenXRHandInputState handInput[2];
	for (int hand = 0; hand < 2; ++hand)
	{
		OpenXRHandInputState& input = handInput[hand];
		const XrPath handPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
		input.select = GetActionBoolean(xrSession, xrSelectAction, handPath);
		input.grip = GetActionBoolean(xrSession, xrGripAction, handPath);
		input.thumbClick = GetActionBoolean(xrSession, xrThumbClickAction, handPath);
		input.menu = GetActionBoolean(xrSession, xrMenuAction, handPath);
		input.a = GetActionBoolean(xrSession, xrAAction, handPath);
		input.b = GetActionBoolean(xrSession, xrBAction, handPath);
		input.x = GetActionBoolean(xrSession, xrXAction, handPath);
		input.y = GetActionBoolean(xrSession, xrYAction, handPath);
		input.trackpad = GetActionVector2f(xrSession, xrTrackpadAction, handPath);
		input.thumbstick = GetActionVector2f(xrSession, xrThumbstickAction, handPath);
	}

	static bool lastMenuMode = false;
	const bool menuModeChanged = (lastMenuMode != menuMode);
	lastMenuMode = menuMode;

	auto syncHandState = [&](int hand)
	{
		const OpenXRHandInputState& input = handInput[hand];
		xrLastSelectState[hand] = input.select;
		xrLastMenuState[hand] = input.menu;
		xrLastGripState[hand] = input.grip;
		xrLastThumbClickState[hand] = input.thumbClick;
		xrLastTrackpadClickState[hand] = input.thumbClick;
		xrLastAState[hand] = input.a;
		xrLastBState[hand] = input.b;
		xrLastXState[hand] = input.x;
		xrLastYState[hand] = input.y;
		xrLastPrimaryState[hand] = input.a;
		xrLastSecondaryState[hand] = input.b;
		xrLastThumbstickState[hand] = input.thumbstick;
		xrLastTrackpadState[hand] = input.trackpad;
	};

	if (menuModeChanged)
	{
		syncHandState(0);
		syncHandState(1);
	}

	const bool dominantGripModifierNew = *vr_secondary_button_mappings && handInput[mainHand].grip;
	const bool dominantGripModifierOld = *vr_secondary_button_mappings && xrLastGripState[mainHand];
	static float analogTurnRateDegPerSec = 0.0f;
	static uint64_t lastAnalogTurnTime = 0;

		if (gameplayMode)
		{
			static bool turnRightLatched[2] = { false, false };
			static bool turnLeftLatched[2] = { false, false };

			if (dominantGripModifierNew)
			{
				analogTurnRateDegPerSec = 0.0f;
				lastAnalogTurnTime = 0;
				turnRightLatched[mainHand] = false;
				turnLeftLatched[mainHand] = false;
			}
			else
			{
				const XrVector2f rightTurnState = useTrackpad
					? handInput[turnHand].trackpad
					: handInput[turnHand].thumbstick;
				const float turnX = rightTurnState.x;
				const uint64_t currentTime = I_msTime();
				if (lastAnalogTurnTime == 0)
				{
					lastAnalogTurnTime = currentTime;
				}
				float deltaSeconds = float(currentTime - lastAnalogTurnTime) * 0.001f;
				if (deltaSeconds > 0.1f)
				{
					deltaSeconds = 0.1f;
				}
				lastAnalogTurnTime = currentTime;

				if (*vr_snapTurn <= 10.0f)
				{
					snapTurn += VR_ApplyAnalogSmoothTurn(turnX, 210.0f, deltaSeconds, VR_GetAnalogTurnResponseScale(*vr_snapTurn), analogTurnRateDegPerSec);
					if (fabsf(turnX) > 0.05f)
					{
						turnRightLatched[mainHand] = false;
						turnLeftLatched[mainHand] = false;
					}
					else
					{
						analogTurnRateDegPerSec = 0.0f;
					}
				}
				else
				{
					analogTurnRateDegPerSec = 0.0f;
					if (turnX > 0.60f)
					{
						if (!turnRightLatched[mainHand])
						{
							snapTurn -= *vr_snapTurn;
							resetDoomYaw = true;
							turnRightLatched[mainHand] = true;
						}
					}
					else if (turnX < 0.40f)
					{
						turnRightLatched[mainHand] = false;
					}

					if (turnX < -0.60f)
					{
						if (!turnLeftLatched[mainHand])
						{
							snapTurn += *vr_snapTurn;
							resetDoomYaw = true;
							turnLeftLatched[mainHand] = true;
						}
					}
					else if (turnX > -0.40f)
					{
						turnLeftLatched[mainHand] = false;
					}
				}
			}
		}
		else
		{
			analogTurnRateDegPerSec = 0.0f;
			lastAnalogTurnTime = 0;
		}

	auto updateHandPose = [&](int hand, float* offset, float* angles)
	{
		if (xrHandSpaces[hand] == XR_NULL_HANDLE)
		{
			xrHandPoseValid[hand] = false;
			return false;
		}
		XrSpaceLocation location{ XR_TYPE_SPACE_LOCATION };
		if (XR_FAILED(xrLocateSpace(xrHandSpaces[hand], xrSpace, xrFrameState.predictedDisplayTime, &location)))
		{
			xrHandPoseValid[hand] = false;
			return false;
		}

		const bool valid = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
			(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
		xrHandPoseValid[hand] = valid;
		if (!valid)
			return false;

		xrHandPoses[hand] = location.pose;

		offset[0] = location.pose.position.x - hmdPosition[0];
		offset[1] = location.pose.position.y - hmdPosition[1];
		offset[2] = location.pose.position.z - hmdPosition[2];

		const float yawRotation = GetViewpointYaw() - hmdorientation[1];
		DVector2 rotated = DVector2(offset[0], offset[2]).Rotated(-yawRotation);
		offset[0] = rotated.Y;
		offset[2] = rotated.X;

		// Apply vr_weaponRotate as a pure pitch-domain adjustment
		const XrVector3f euler = OpenVREulerAnglesFromQuaternion(location.pose.orientation);
		angles[YAW] = (float)(euler.x * (180.0 / M_PI));
		// Match OpenVR pitch-adjust direction: negative vr_weaponRotate should tilt the weapon downward
		constexpr float openxrWeaponPitchBiasDeg = -40.0f;
		angles[PITCH] = -(float)(euler.y * (180.0 / M_PI)) - (vr_weaponRotate * 2.0f) + openxrWeaponPitchBiasDeg;
		// Apply extra 180-degree roll flip to match the expected OpenVR-facing baseline
		angles[ROLL] = (float)NormalizeAngle(-(float)(euler.z * (180.0 / M_PI)));
		return true;
	};

	const bool mainHandValid = updateHandPose(mainHand, weaponoffset, weaponangles);
	const bool offHandValid = updateHandPose(offHand, offhandoffset, offhandangles);
	if (mainHandValid && offHandValid)
	{
		const float dx = xrHandPoses[mainHand].position.x - xrHandPoses[offHand].position.x;
		const float dy = xrHandPoses[mainHand].position.y - xrHandPoses[offHand].position.y;
		const float dz = xrHandPoses[mainHand].position.z - xrHandPoses[offHand].position.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		const bool offhandGrip = handInput[offHand].grip;
		if (vr_two_handed_weapons)
		{
			if (offhandGrip && distance < 0.50f)
				weaponStabilised = true;
			else if (!offhandGrip)
				weaponStabilised = false;
		}
		else
		{
			weaponStabilised = false;
		}

		if (weaponStabilised)
		{
			const float z = xrHandPoses[offHand].position.z - xrHandPoses[mainHand].position.z;
			const float x = xrHandPoses[offHand].position.x - xrHandPoses[mainHand].position.x;
			const float y = xrHandPoses[offHand].position.y - xrHandPoses[mainHand].position.y;
			const float zxDist = std::sqrt(x * x + z * z);
			if (zxDist != 0.0f && z != 0.0f)
			{
				weaponangles[0] = -(float)(atanf(y / zxDist) * (180.0 / M_PI));
				weaponangles[1] = -(float)(atan2f(x, -z) * (180.0 / M_PI));
			}
		}
	}
	else
	{
		weaponStabilised = false;
	}

	if (vr_openxr_debug_weapon)
	{
		static bool lastLoggedWeaponStabilised = false;
		static bool haveLastLoggedWeaponStabilised = false;
		const float dx = xrHandPoses[mainHand].position.x - xrHandPoses[offHand].position.x;
		const float dy = xrHandPoses[mainHand].position.y - xrHandPoses[offHand].position.y;
		const float dz = xrHandPoses[mainHand].position.z - xrHandPoses[offHand].position.z;
		const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
		if (!haveLastLoggedWeaponStabilised || lastLoggedWeaponStabilised != weaponStabilised)
		{
			Printf("VR_WEAPON OpenXR stab=%d dist=%.3f main=%d off=%d grip=%d valid=%d/%d\n",
				weaponStabilised ? 1 : 0,
				distance,
				mainHand,
				offHand,
				handInput[offHand].grip ? 1 : 0,
				mainHandValid ? 1 : 0,
				offHandValid ? 1 : 0);
			lastLoggedWeaponStabilised = weaponStabilised;
			haveLastLoggedWeaponStabilised = true;
		}
	}

	if (menuModeChanged)
	{
		ProcessHaptics();
		return;
	}

	if (gameplayMode)
	{
		const bool suppressLocomotion = dominantGripModifierNew;
		remote_movementSideways = 0.0f;
		remote_movementForward = 0.0f;

		auto applyCurve = [](float value)
		{
			const float deadZone = 0.15f;
			const float absValue = fabsf(value);
			if (absValue < deadZone)
				return 0.0f;

			float scaled = (absValue - deadZone) / (1.0f - deadZone);
			scaled *= scaled;
			return (value < 0.0f) ? -scaled : scaled;
		};

		if (!suppressLocomotion)
		{
			const XrVector2f leftMovementState = useTrackpad
				? handInput[movementHand].trackpad
				: handInput[movementHand].thumbstick;
			float moveDist = OpenVRLength2D(leftMovementState.x, leftMovementState.y);
			const float nlf = OpenVRNonLinearFilter(moveDist);
			moveDist = (moveDist > 1.0f) ? moveDist : 1.0f;
			float moveX = nlf * (leftMovementState.x / moveDist);
			float moveY = nlf * (leftMovementState.y / moveDist);
			const bool playerMoving = (fabsf(moveX) + fabsf(moveY)) > 0.05f;
			moveX = playerMoving ? moveX : 0.0f;
			moveY = playerMoving ? moveY : 0.0f;

			// Keep OpenXR locomotion in line with the OpenVR path's effective speed.
			// OpenVR movement is accumulated through both joystick axes and VR_GetMove.
			// OpenXR currently feeds VR_GetMove only.
			constexpr float kOpenXrLocomotionCompatScale = 2.0f;
			moveX *= kOpenXrLocomotionCompatScale;
			moveY *= kOpenXrLocomotionCompatScale;
			remote_movementSideways = moveX;
			remote_movementForward = moveY;

			if (*vr_teleport)
			{
				if (moveY > 0.7f && !ready_teleport)
				{
					ready_teleport = true;
				}
				else if (moveY < 0.7f && ready_teleport)
				{
					ready_teleport = false;
					trigger_teleport = true;
				}
			}
		}
		else
		{
			remote_movementSideways = 0.0f;
			remote_movementForward = 0.0f;
		}

	}
	else
	{
		remote_movementSideways = 0.0f;
		remote_movementForward = 0.0f;
	}

	auto emitGameplayHandButtons = [&](int hand, bool emitAxes)
	{
		const bool dominantHand = (hand == mainHand);
		const bool modifierOld = dominantGripModifierOld;
		const bool modifierNew = dominantGripModifierNew;
		const int handOffset = HandKeyOffset(hand);
		const int axisOffset = HandAxisKeyOffset(hand);
		const int oppositeAxisOffset = HandAxisKeyOffset(1 - hand);
		const int gripKey = KEY_PAD_LSHOULDER + handOffset;
		const bool suppressGripButton = dominantHand && *vr_secondary_button_mappings;
		const bool face1Pressed = hand == 1 ? handInput[hand].a : handInput[hand].x;
		const bool face2Pressed = hand == 1 ? handInput[hand].b : handInput[hand].y;
		const bool face1Old = hand == 1 ? xrLastAState[hand] : xrLastXState[hand];
		const bool face2Old = hand == 1 ? xrLastBState[hand] : xrLastYState[hand];
		const int face1BaseKey = hand == 1 ? KEY_PAD_A : KEY_PAD_X;
		const int face2BaseKey = hand == 1 ? KEY_PAD_B : KEY_PAD_Y;
		const int face1AltKey = dominantHand ? KEY_PAD_LTHUMB : KEY_PGDN;
		const int face2AltKey = dominantHand ? KEY_BACKSPACE : KEY_PGUP;
		const int triggerBaseKey = dominantHand ? KEY_PAD_RTRIGGER : KEY_LSHIFT;
		const int triggerAltKey = dominantHand ? KEY_PAD_LTRIGGER : KEY_LALT;
		const int thumbBaseKey = dominantHand ? KEY_ENTER : KEY_SPACE;
		const int thumbAltKey = dominantHand ? KEY_TAB : KEY_HOME;
		const int gripAltKey = KEY_PAD_DPAD_UP;

		// When virtual menu mouse is active on the right hand, the trigger drives
		// GUI left-click events. Suppress trigger-as-key to avoid menu key-path
		// conflicts (notably messagebox yes/no confirmations).
		const bool suppressSelectAsKey = menuMode && menuactive != MENU_WaitKey && hand == 1 && *vr_menu_pointer && (*vr_mouse_in_menu || handInput[1].grip);
		if (suppressSelectAsKey)
		{
			const int oldSelectKey = modifierOld ? triggerAltKey : triggerBaseKey;
			PostControllerKeyTransition(xrLastSelectState[hand], false, oldSelectKey);
		}
		else
		{
			PostRemappedControllerKeyTransition(xrLastSelectState[hand], handInput[hand].select, modifierOld, modifierNew, triggerBaseKey, triggerAltKey);
		}
		if (suppressGripButton)
		{
			// While the dominant hand grip is acting as a modifier, do not emit it
			// as a separate button. This keeps grip+B style combos from also firing
			// the standalone grip binding and matches the OpenVR shift-layer intent.
			PostControllerKeyTransition(xrLastGripState[hand], false, gripKey);
		}
		else if (!dominantHand && *vr_secondary_button_mappings)
		{
			PostRemappedControllerKeyTransition(xrLastGripState[hand], handInput[hand].grip, modifierOld, modifierNew, gripKey, gripAltKey);
		}
		else
		{
			PostControllerKeyTransition(xrLastGripState[hand], handInput[hand].grip, gripKey);
		}
		PostRemappedControllerKeyTransition(xrLastThumbClickState[hand], handInput[hand].thumbClick, modifierOld, modifierNew, thumbBaseKey, thumbAltKey);
		const int thumbClickKey = (hand == 1) ? KEY_PAD_RTHUMB : KEY_PAD_LTHUMB;
		// The combo layer already emits the mapped thumb-click action, so keep the
		// raw button from leaking through while the dominant grip is acting as a modifier.
		if (!(*vr_secondary_button_mappings && dominantGripModifierNew))
		{
			PostControllerKeyTransition(xrLastThumbClickState[hand], handInput[hand].thumbClick, thumbClickKey);
		}
		if (dominantHand)
		{
			PostRemappedControllerKeyTransition(face1Old, face1Pressed, dominantGripModifierOld, dominantGripModifierNew, face1BaseKey, face1AltKey);
			PostRemappedControllerKeyTransition(face2Old, face2Pressed, dominantGripModifierOld, dominantGripModifierNew, face2BaseKey, face2AltKey);
		}
		else if (*vr_secondary_button_mappings)
		{
			PostRemappedControllerKeyTransition(face1Old, face1Pressed, modifierOld, modifierNew, face1BaseKey, KEY_PGDN);
			PostRemappedControllerKeyTransition(face2Old, face2Pressed, modifierOld, modifierNew, face2BaseKey, KEY_PGUP);
		}
		else
		{
			PostControllerKeyTransition(face1Old, face1Pressed, face1BaseKey);
			PostControllerKeyTransition(face2Old, face2Pressed, face2BaseKey);
		}

		if (emitAxes)
		{
			const XrVector2f& lastAxisState = useTrackpad ? xrLastTrackpadState[hand] : xrLastThumbstickState[hand];
			const XrVector2f& newAxisState = useTrackpad ? handInput[hand].trackpad : handInput[hand].thumbstick;
			const int baseLeftKey = hand == 1 ? KEY_JOYAXIS3MINUS : KEY_JOYAXIS1MINUS;
			const int baseRightKey = hand == 1 ? KEY_JOYAXIS3PLUS : KEY_JOYAXIS1PLUS;
			const int baseDownKey = hand == 1 ? KEY_JOYAXIS4MINUS : KEY_JOYAXIS2MINUS;
			const int baseUpKey = hand == 1 ? KEY_JOYAXIS4PLUS : KEY_JOYAXIS2PLUS;
			const int shiftedLeftKey = hand == 1 ? KEY_JOYAXIS7MINUS : KEY_JOYAXIS5MINUS;
			const int shiftedRightKey = hand == 1 ? KEY_JOYAXIS7PLUS : KEY_JOYAXIS5PLUS;
			const int shiftedDownKey = hand == 1 ? KEY_JOYAXIS8MINUS : KEY_JOYAXIS6MINUS;
			const int shiftedUpKey = hand == 1 ? KEY_JOYAXIS8PLUS : KEY_JOYAXIS6PLUS;

			PostRemappedControllerAxisTransitions(lastAxisState, newAxisState, modifierOld, modifierNew,
				baseLeftKey,
				baseRightKey,
				baseDownKey,
				baseUpKey,
				shiftedLeftKey,
				shiftedRightKey,
				shiftedDownKey,
				shiftedUpKey);
		}

		syncHandState(hand);
	};

	xrMenuPointerActive = false;
	xrMenuPointerHasHit = false;
	xrMenuPointerBeamVisible = false;
	xrMenuPointerBeamLength = 0.0f;
	menu_allow_mouse_override = false;
	const bool keybindCaptureMode = menuactive == MENU_WaitKey;

	const int pointerHand = 1; // Always use the right controller for virtual menu mouse.
	const bool vrMouseEnabled = !keybindCaptureMode && (*vr_mouse_in_menu || handInput[pointerHand].grip);
	const bool canUseMenuPointer = menuMode &&
		!keybindCaptureMode &&
		*vr_menu_pointer &&
		vrMouseEnabled &&
		pointerHand >= 0 && pointerHand < 2 &&
		xrHandPoseValid[pointerHand] &&
		xrVirtualScreenWidth > 0 &&
		xrVirtualScreenHeight > 0 &&
		xrViewCount > 0 &&
		xrViews.size() >= xrViewCount;

	if (canUseMenuPointer)
	{
		float screenWidth = 0.0f;
		float screenHeight = 0.0f;
		GetOpenXRVirtualScreenMeters(xrVirtualScreenWidth, xrVirtualScreenHeight, screenWidth, screenHeight);
		const XrQuaternionf screenOrientation = xrVirtualScreenPose.orientation;
		const XrVector3f planeOrigin = xrVirtualScreenPose.position;

		const XrVector3f planeNormal = NormalizeVector(RotateVector(screenOrientation, { 0.0f, 0.0f, 1.0f }));
		const XrVector3f planeRight = NormalizeVector(RotateVector(screenOrientation, { 1.0f, 0.0f, 0.0f }));
		const XrVector3f planeUp = NormalizeVector(RotateVector(screenOrientation, { 0.0f, 1.0f, 0.0f }));
		const XrVector3f rayOrigin = xrHandPoses[pointerHand].position;
		const XrQuaternionf pointerAlignRotation = QuaternionFromAxisAngle(0.0f, 0.0f, 1.0f, -(vr_weaponRotate * 2.0f) * (float)(M_PI / 180.0));
		const XrQuaternionf pointerOrientation = MultiplyQuaternion(xrHandPoses[pointerHand].orientation, pointerAlignRotation);

		struct RayHit
		{
			bool valid = false;
			float t = 0.0f;
			float unclampedU = 0.0f;
			float unclampedV = 0.0f;
			float localX = 0.0f;
			float localY = 0.0f;
			float overflow = 0.0f;
			XrVector3f hitPoint = { 0.0f, 0.0f, 0.0f };
		};

		auto testRayAxis = [&](const XrVector3f& localAxis) -> RayHit
		{
			RayHit out;
			const XrVector3f rayDir = NormalizeVector(RotateVector(pointerOrientation, localAxis));
			const float denom = DotVector(rayDir, planeNormal);
			if (fabsf(denom) <= 0.0001f)
			{
				return out;
			}

			const float t = DotVector(SubtractVector(planeOrigin, rayOrigin), planeNormal) / denom;
			if (t <= 0.0f)
			{
				return out;
			}

			const XrVector3f hitPoint = AddVector(rayOrigin, ScaleVector(rayDir, t));
			const XrVector3f hitDelta = SubtractVector(hitPoint, planeOrigin);
			out.hitPoint = hitPoint;
			const float localX = DotVector(hitDelta, planeRight);
			const float localY = DotVector(hitDelta, planeUp);
			const float halfW = screenWidth * 0.5f;
			const float halfH = screenHeight * 0.5f;
			out.localX = localX;
			out.localY = localY;
			out.unclampedU = (localX + halfW) / std::max(screenWidth, 0.0001f);
			out.unclampedV = (halfH - localY) / std::max(screenHeight, 0.0001f);
			out.overflow =
				(out.unclampedU < 0.0f ? -out.unclampedU : 0.0f) +
				(out.unclampedU > 1.0f ? out.unclampedU - 1.0f : 0.0f) +
				(out.unclampedV < 0.0f ? -out.unclampedV : 0.0f) +
				(out.unclampedV > 1.0f ? out.unclampedV - 1.0f : 0.0f);
			out.t = t;
			out.valid = true;
			return out;
		};

		const XrVector3f rayAxis = { 0.0f, 0.0f, -1.0f };
		const RayHit bestHit = testRayAxis(rayAxis);

		if (bestHit.valid)
		{
			const bool inside = bestHit.overflow <= 0.0001f;
			const float mappedU = 1.0f - bestHit.unclampedU;
			const float mappedV = 1.0f - bestHit.unclampedV;
			float u = clamp<float>(mappedU, 0.0f, 1.0f);
			float v = clamp<float>(mappedV, 0.0f, 1.0f);
			const int mouseX = clamp<int>((int)std::lround(u * (float)(xrVirtualScreenWidth - 1)), 0, (int)xrVirtualScreenWidth - 1);
			const int mouseY = clamp<int>((int)std::lround(v * (float)(xrVirtualScreenHeight - 1)), 0, (int)xrVirtualScreenHeight - 1);
			if (vr_openxr_debug_sizes)
			{
				static int pointerDebugTicker = 0;
				if ((pointerDebugTicker++ % 35) == 0)
				{
					const int guiW = screen ? screen->GetWidth() : 0;
					const int guiH = screen ? screen->GetHeight() : 0;
					Printf("XR_MENU_PTR axis=%d t=%.3f local=(%.3f,%.3f) half=(%.3f,%.3f) uvRaw=(%.3f,%.3f) uvMap=(%.3f,%.3f) ovf=%.3f virt=%ux%u vis=(%d,%d) gui=%dx%d guiPos=(%d,%d)\n",
						0,
						bestHit.t,
						bestHit.localX, bestHit.localY,
						screenWidth * 0.5f, screenHeight * 0.5f,
						bestHit.unclampedU, bestHit.unclampedV,
						u, v,
						bestHit.overflow,
						xrVirtualScreenWidth, xrVirtualScreenHeight,
						mouseX, mouseY,
						guiW, guiH,
						mouseX, mouseY);
				}
			}

			// Build a world-space quad layer representing a laser beam from
			// controller pose to the virtual-screen hit point.
			const XrVector3f beamStart = rayOrigin;
			const XrVector3f beamEnd = bestHit.hitPoint;
			const XrVector3f beamVec = SubtractVector(beamEnd, beamStart);
			const float beamLength = std::sqrt(DotVector(beamVec, beamVec));
			if (beamLength > 0.01f)
			{
				const XrVector3f beamDir = ScaleVector(beamVec, 1.0f / beamLength);
				const XrVector3f beamCenter = AddVector(beamStart, ScaleVector(beamDir, beamLength * 0.5f));
				XrVector3f viewDir = NormalizeVector(SubtractVector(xrViews[0].pose.position, beamCenter));
				XrVector3f xAxis = NormalizeVector(CrossVector(viewDir, beamDir));
				if (DotVector(xAxis, xAxis) < 0.0001f)
				{
					xAxis = NormalizeVector(CrossVector({ 0.0f, 1.0f, 0.0f }, beamDir));
					if (DotVector(xAxis, xAxis) < 0.0001f)
						xAxis = NormalizeVector(CrossVector({ 1.0f, 0.0f, 0.0f }, beamDir));
				}
				const XrVector3f xBeamAxis = beamDir;
				const XrVector3f yBeamAxis = xAxis;
				const XrVector3f zAxis = NormalizeVector(CrossVector(xBeamAxis, yBeamAxis));
				xrMenuPointerBeamPose.position = beamCenter;
				xrMenuPointerBeamPose.orientation = QuaternionFromBasis(xBeamAxis, yBeamAxis, zAxis);
				xrMenuPointerBeamLength = beamLength;
				xrMenuPointerBeamVisible = true;
			}

			const bool dragHoldActive = xrMenuPointerLastLeftDown || xrMenuPointerLastRightDown;
			const bool allowClampedInteraction = inside || dragHoldActive;
			if (allowClampedInteraction)
			{
				xrMenuPointerActive = true;
				xrMenuPointerHasHit = true;
				xrMenuPointerX = (float)mouseX;
				xrMenuPointerY = (float)mouseY;
				menu_allow_mouse_override = true;

				if (!xrMenuPointerHadPos || mouseX != xrMenuPointerLastX || mouseY != xrMenuPointerLastY)
				{
					PostGuiMouseEvent(EV_GUI_MouseMove, mouseX, mouseY);
					xrMenuPointerLastX = mouseX;
					xrMenuPointerLastY = mouseY;
				}
				xrMenuPointerHadPos = true;

				if (handInput[pointerHand].select != xrMenuPointerLastLeftDown)
				{
					PostGuiMouseEvent(handInput[pointerHand].select ? EV_GUI_LButtonDown : EV_GUI_LButtonUp, xrMenuPointerLastX, xrMenuPointerLastY);
					xrMenuPointerLastLeftDown = handInput[pointerHand].select;
				}
				// If vr_mouse_in_menu is disabled, grip acts as a temporary "hold to use mouse"
				// modifier, so do not also emit right-click from the same button.
				const bool emitRightClick = *vr_mouse_in_menu;
				if (emitRightClick && handInput[pointerHand].grip != xrMenuPointerLastRightDown)
				{
					PostGuiMouseEvent(handInput[pointerHand].grip ? EV_GUI_RButtonDown : EV_GUI_RButtonUp, xrMenuPointerLastX, xrMenuPointerLastY);
					xrMenuPointerLastRightDown = handInput[pointerHand].grip;
				}
				else if (!emitRightClick)
				{
					xrMenuPointerLastRightDown = false;
				}
			}
		}

		// Allow scrolling long menus with right-thumbstick vertical movement,
		// even if the current ray cast misses the panel.
		{
			constexpr float wheelDeadZone = 0.55f;
			static int wheelRepeatCooldown = 0;
			if (wheelRepeatCooldown > 0) wheelRepeatCooldown--;
			const float wheelY = handInput[pointerHand].thumbstick.y;
			if (wheelRepeatCooldown == 0)
			{
				if (wheelY >= wheelDeadZone)
				{
					PostGuiWheelEvent(EV_GUI_WheelUp, xrMenuPointerLastX, xrMenuPointerLastY);
					wheelRepeatCooldown = 8;
				}
				else if (wheelY <= -wheelDeadZone)
				{
					PostGuiWheelEvent(EV_GUI_WheelDown, xrMenuPointerLastX, xrMenuPointerLastY);
					wheelRepeatCooldown = 8;
				}
			}
		}
	}
	if (!menuMode || !xrMenuPointerActive)
	{
		if (xrMenuPointerLastLeftDown || xrMenuPointerLastRightDown)
		{
			const int releaseX = xrMenuPointerHadPos ? xrMenuPointerLastX : 0;
			const int releaseY = xrMenuPointerHadPos ? xrMenuPointerLastY : 0;
			if (xrMenuPointerLastLeftDown)
			{
				PostGuiMouseEvent(EV_GUI_LButtonUp, releaseX, releaseY);
			}
			if (xrMenuPointerLastRightDown)
			{
				PostGuiMouseEvent(EV_GUI_RButtonUp, releaseX, releaseY);
			}
		}
		xrMenuPointerLastLeftDown = false;
		xrMenuPointerLastRightDown = false;
		if (!menuMode)
		{
			xrMenuPointerHadPos = false;
		}
	}

	if (menuMode)
	{
		emitGameplayHandButtons(0, true);
		emitGameplayHandButtons(1, menuactive == MENU_WaitKey);
		return;
	}

	emitGameplayHandButtons(0, true);
	emitGameplayHandButtons(1, true);
	if (gameplayMode)
	{
		player_t* player = &players[consoleplayer];
		if (player && player->mo)
		{
			const float hmdHeight = GetHmdAdjustedHeightInMapUnit(xrUsingStageSpace ? false : xrHasLocalHeightAnchor, xrLocalHeightAnchor);
			if (!vr_crouch_use_button)
			{
				const double defaultViewHeight = player->DefaultViewHeight();
				if (defaultViewHeight > 0.0)
				{
					player->crouching = 10;
					player->crouchfactor = hmdHeight / defaultViewHeight;
				}
			}
			else if (player->crouching == 10)
			{
				player->Uncrouch();
			}

			LSMatrix44 mat;
			if (GetWeaponTransform(&mat, VR_MAINHAND))
			{
				player->mo->AttackPos.X = mat[3][0];
				player->mo->AttackPos.Y = mat[3][2];
				player->mo->AttackPos.Z = mat[3][1];
				player->mo->AttackPitch = DAngle::fromDeg(ShouldUseScreenLayerForCurrentFrame()
					? -weaponangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees()
					: -weaponangles[PITCH]);
				player->mo->AttackAngle = DAngle::fromDeg(-90 + GetViewpointYaw() + (weaponangles[YAW] - playerYaw));
				player->mo->AttackRoll = DAngle::fromDeg(weaponangles[ROLL]);
			}

			LSMatrix44 matOffhand;
			if (GetWeaponTransform(&matOffhand, VR_OFFHAND))
			{
				player->mo->OffhandPos.X = matOffhand[3][0];
				player->mo->OffhandPos.Y = matOffhand[3][2];
				player->mo->OffhandPos.Z = matOffhand[3][1];
				player->mo->OffhandPitch = DAngle::fromDeg(ShouldUseScreenLayerForCurrentFrame()
					? -offhandangles[PITCH] - r_viewpoint.Angles.Pitch.Degrees()
					: -offhandangles[PITCH]);
				player->mo->OffhandAngle = DAngle::fromDeg(-90 + GetViewpointYaw() + (offhandangles[YAW] - playerYaw));
				player->mo->OffhandRoll = DAngle::fromDeg(offhandangles[ROLL]);
			}

			if (vr_teleport && player->mo->health > 0)
			{
				DAngle yaw = DAngle::fromDeg(GetViewpointYaw() - hmdorientation[YAW] + offhandangles[YAW]);
				DAngle pitch = DAngle::fromDeg(offhandangles[PITCH]);
				const double pixelstretch = r_viewpoint.ViewLevel ? r_viewpoint.ViewLevel->pixelstretch : 1.2;

				if (ready_teleport)
				{
					FLineTraceData trace;
					if (P_LineTrace(player->mo, yaw, 8192, pitch, TRF_ABSOFFSET | TRF_BLOCKUSE | TRF_BLOCKSELF | TRF_SOLIDACTORS,
						((hmdPosition[1] + offhandoffset[1] + vr_height_adjust) * vr_vunits_per_meter) / pixelstretch,
						-(offhandoffset[2] * vr_vunits_per_meter),
						-(offhandoffset[0] * vr_vunits_per_meter), &trace))
					{
						m_TeleportTarget = trace.HitType;
						m_TeleportLocation = trace.HitLocation;
					}
					else
					{
						m_TeleportTarget = TRACE_HitNone;
						m_TeleportLocation = DVector3(0, 0, 0);
					}
				}
				else if (trigger_teleport && m_TeleportTarget == TRACE_HitFloor)
				{
					auto vel = player->mo->Vel;
					player->mo->Vel = DVector3(m_TeleportLocation.X - player->mo->X(),
						m_TeleportLocation.Y - player->mo->Y(), 0);
					bool wasOnGround = player->mo->Z() <= player->mo->floorz + 0.1;
					double oldZ = player->mo->Z();
					P_XYMovement(player->mo, DVector2(0, 0));

					if (player->mo->Z() >= oldZ && wasOnGround)
					{
						player->mo->SetZ(player->mo->floorz);
					}
					else
					{
						player->mo->SetZ(oldZ);
					}
					player->mo->Vel = vel;
				}

				trigger_teleport = false;
			}

			if (*vr_move_use_offhand && xrHandPoseValid[offHand])
			{
				const DAngle offhandYaw = DAngle::fromDeg(GetViewpointYaw() - hmdorientation[YAW] + offhandangles[YAW]);
				player->mo->ThrustAngleOffset = offhandYaw - player->mo->Angles.Yaw;
			}
			else
			{
				player->mo->ThrustAngleOffset = nullAngle;
			}

			auto vel = player->mo->Vel;
			player->mo->Vel = DVector3((DVector2(positional_movementSideways, positional_movementForward) * vr_vunits_per_meter), 0);
			bool wasOnGround = player->mo->Z() <= player->mo->floorz;
			float oldZ = player->mo->Z();
			P_XYMovement(player->mo, DVector2(0, 0));

			if (player->mo->Z() >= oldZ && wasOnGround)
			{
				player->mo->SetZ(player->mo->floorz);
			}
			else
			{
				player->mo->SetZ(oldZ);
			}
			player->mo->Vel = vel;

			if (vr_openxr_debug_weapon && !xrLoggedWeaponState)
			{
				Printf("VR_WEAPON OpenXR handedness=%s main=%d off=%d weapon=(%.3f,%.3f,%.3f|%.1f,%.1f,%.1f) offhand=(%.3f,%.3f,%.3f|%.1f,%.1f,%.1f) stab=%d teleport=%d target=%d\n",
					IsRightHandedVrControls() ? "right" : "left",
					GetMainHandIndex(), GetOffHandIndex(),
					weaponoffset[0], weaponoffset[1], weaponoffset[2],
					weaponangles[0], weaponangles[1], weaponangles[2],
					offhandoffset[0], offhandoffset[1], offhandoffset[2],
					offhandangles[0], offhandangles[1], offhandangles[2],
					weaponStabilised ? 1 : 0,
					ready_teleport ? 1 : 0,
					m_TeleportTarget);
				xrLoggedWeaponState = true;
			}
		}
	}
	static bool loggedBoundSources = false;
	ProcessHaptics();
	if (!loggedBoundSources)
	{
		auto getCurrentProfile = [&](XrPath handPath)
		{
			XrInteractionProfileState profileState{ XR_TYPE_INTERACTION_PROFILE_STATE };
			if (XR_FAILED(xrGetCurrentInteractionProfile(xrSession, handPath, &profileState)) || profileState.interactionProfile == XR_NULL_PATH)
				return FString("<unbound>");
			return PathToString(xrInstance, profileState.interactionProfile);
		};

		const FString leftProfile = getCurrentProfile(xrLeftHandPath);
		const FString rightProfile = getCurrentProfile(xrRightHandPath);
		if (!strcmp(leftProfile.GetChars(), "<unbound>") && !strcmp(rightProfile.GetChars(), "<unbound>"))
			return;

		loggedBoundSources = true;
		Printf("OpenXR: active interaction profiles left=%s right=%s\n",
			leftProfile.GetChars(),
			rightProfile.GetChars());
		LogBoundSourcesForAction(xrInstance, xrSession, xrSelectAction, "select");
		LogBoundSourcesForAction(xrInstance, xrSession, xrGripAction, "grip");
		LogBoundSourcesForAction(xrInstance, xrSession, xrMenuAction, "menu");
		LogBoundSourcesForAction(xrInstance, xrSession, xrAAction, "A");
		LogBoundSourcesForAction(xrInstance, xrSession, xrBAction, "B");
		LogBoundSourcesForAction(xrInstance, xrSession, xrXAction, "X");
		LogBoundSourcesForAction(xrInstance, xrSession, xrYAction, "Y");
	}
}

void VKOpenXRDeviceMode::TearDown() const
{
	StopHaptics();
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

	if (xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr)
		return false;

	if (xrSwapchain == XR_NULL_HANDLE && !CreateSwapchain())
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
		const uint32_t sourceEye = layer;
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
	XrCompositionLayerQuad beamLayer{ XR_TYPE_COMPOSITION_LAYER_QUAD };
	bool submitVirtualScreen = xrVirtualScreenVisible && xrVirtualScreenSwapchain != XR_NULL_HANDLE && xrVirtualScreenImageIndex >= 0;
	bool submitBackdrop = submitVirtualScreen && xrVirtualScreenBackdropVisible && xrVirtualScreenBackdropSwapchain != XR_NULL_HANDLE && xrVirtualScreenBackdropImageIndex >= 0;
	bool submitMenuPointerBeam = submitVirtualScreen &&
		xrMenuPointerBeamVisible &&
		xrMenuPointerBeamSwapchain != XR_NULL_HANDLE &&
		xrMenuPointerBeamImageIndex >= 0 &&
		!xrMenuPointerBeamTextures.empty();

	const XrCompositionLayerBaseHeader* layers[4];
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

		if (submitMenuPointerBeam)
		{
			beamLayer = xrMenuPointerBeamLayer;
			beamLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			beamLayer.layerFlags = 0;
			beamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			beamLayer.subImage.swapchain = xrMenuPointerBeamSwapchain;
			beamLayer.subImage.imageArrayIndex = 0;
			layers[layerIndex++] = (const XrCompositionLayerBaseHeader*)&beamLayer;
		}
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
	if (screen == nullptr)
		return;
	if (!mInVRSceneRender)
		return;

	int width = 0;
	int height = 0;
	if (!GetRecommendedRenderSize(width, height))
	{
		VRMode::AdjustViewport(screen);
		return;
	}

	ApplyOpenXRGameplayViewport(screen, width, height);
}

void VKOpenXRDeviceMode::AdjustPlayerSprites(FRenderState& state, int hand) const
{
	if (GetWeaponTransform(&state.mModelMatrix, hand))
	{
		const float scale = 0.00125f * vr_weaponScale * vr_2dweaponScale;
		state.mModelMatrix.scale(scale, -scale, scale);
		state.mModelMatrix.translate(-viewwidth / 2, -viewheight * 3 / 4, 0.0f);

		const float offsetFactor = 40.f;
		state.mModelMatrix.translate(vr_2dweaponOffsetX * offsetFactor, -vr_2dweaponOffsetY * offsetFactor, vr_2dweaponOffsetZ * offsetFactor);
	}
	state.EnableModelMatrix(true);
}

void VKOpenXRDeviceMode::UnAdjustPlayerSprites(FRenderState& state) const
{
	state.EnableModelMatrix(false);
}

void VKOpenXRDeviceMode::DrawMountedHud(HWDrawInfo* di, FRenderState& state) const
{
	if (!VR_ShouldDrawMountedHud() || di == nullptr)
	{
		return;
	}

	auto& surface = GetVRHudSurface();
	VSMatrix mountTransform;
	if (!VR_GetMountedHudTransform(mountTransform))
	{
		return;
	}

	const float mountScale = automapactive ? vr_automap_mount_scale : vr_hud_mount_scale;
	const float pixelUnit = mountScale * 0.002f;
	const float baseWidth = (float)surface.GetWidth() * pixelUnit;
	const float baseHeight = (float)surface.GetHeight() * pixelUnit;

	auto savedMatrix = state.mModelMatrix;
	state.mModelMatrix = mountTransform;
	state.EnableModelMatrix(true);
	di->DrawHudQuad(state, surface.GetGameTexture(),
		baseWidth,
		baseHeight,
		0.f, 0.f,
		(automapactive ? vr_automap_mount_pos : vr_hud_mount_pos) == 1,
		automapactive);
	if (automapactive && vr_automap_border > 0)
	{
		const PalEntry borderColor = (uint32_t)vr_automap_border_color;
		float borderPadPx = (float)vr_automap_border;
		if (borderPadPx < 1.0f) borderPadPx = 1.0f;
		if (borderPadPx > 5.0f) borderPadPx = 5.0f;
		borderPadPx *= 5.0f;
		const float borderPadX = pixelUnit * borderPadPx;
		const float borderPadY = pixelUnit * borderPadPx;
		di->DrawVRHudBorder(state, baseWidth + (borderPadX * 2.0f), borderPadY, borderColor, 0.f, (baseHeight * 0.5f) + (borderPadY * 0.5f));
		di->DrawVRHudBorder(state, baseWidth + (borderPadX * 2.0f), borderPadY, borderColor, 0.f, -((baseHeight * 0.5f) + (borderPadY * 0.5f)));
		di->DrawVRHudBorder(state, borderPadX, baseHeight, borderColor, -((baseWidth * 0.5f) + (borderPadX * 0.5f)), 0.f);
		di->DrawVRHudBorder(state, borderPadX, baseHeight, borderColor, (baseWidth * 0.5f) + (borderPadX * 0.5f), 0.f);
	}
	state.mModelMatrix = savedMatrix;
	state.EnableModelMatrix(false);
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
	const int effectiveOverlayMode = (vr_overlayscreen == 0) ? 2 : vr_overlayscreen;

	const XrQuaternionf headOrientation = xrViews[0].pose.orientation;
	const XrVector3f headForward = RotateVector(headOrientation, { 0.0f, 0.0f, -1.0f });
	const XrVector3f headUp = RotateVector(headOrientation, { 0.0f, 1.0f, 0.0f });
	const float headYawDeg = YawDegFromForward(headForward);
	if (xrHasPrevHeadSampleForRecenter)
	{
		// Do not treat normal head turning as recenter. Only react to large
		// vertical/discontinuous jumps that indicate runtime origin reset.
		const float heightDelta = fabsf(center.y - xrPrevHeadCenterForRecenter.y);
		const XrVector3f deltaPos = SubtractVector(center, xrPrevHeadCenterForRecenter);
		const float deltaLen = std::sqrt(std::max(0.0f, DotVector(deltaPos, deltaPos)));
		if ((effectiveOverlayMode == 1 || effectiveOverlayMode == 2) && (heightDelta > 0.35f || deltaLen > 0.80f))
		{
			xrStationaryAnchorValid = false;
		}
	}
	xrPrevHeadCenterForRecenter = center;
	xrPrevHeadYawDegForRecenter = headYawDeg;
	xrHasPrevHeadSampleForRecenter = true;

	const float distance = std::max(0.25f, 2.5f + vr_overlayscreen_dist);
	float screenWidth = 0.0f;
	float screenHeight = 0.0f;
	GetOpenXRVirtualScreenMeters(xrVirtualScreenWidth, xrVirtualScreenHeight, screenWidth, screenHeight);
	const XrQuaternionf flipRotation = MakeAxisAngleQuaternion({ 0.0f, 0.0f, 1.0f }, (float)M_PI);
	const XrVector3f worldUp = { 0.0f, 1.0f, 0.0f };
	const XrVector3f yawForward = NormalizeVector({ headForward.x, 0.0f, headForward.z });
	const double now = I_msTimeF();

	auto BuildYawUprightPose = [&](const XrVector3f& anchorCenter, const XrVector3f& forwardIn) -> XrPosef
	{
		XrVector3f forward = NormalizeVector({ forwardIn.x, 0.0f, forwardIn.z });
		if (DotVector(forward, forward) < 0.0001f)
			forward = { 0.0f, 0.0f, -1.0f };
		const XrVector3f normal = ScaleVector(forward, -1.0f);
		XrVector3f right = NormalizeVector(CrossVector(worldUp, normal));
		if (DotVector(right, right) < 0.0001f)
			right = { 1.0f, 0.0f, 0.0f };
		const XrVector3f up = NormalizeVector(CrossVector(normal, right));
		const XrVector3f pos = AddVector(AddVector(anchorCenter, ScaleVector(forward, distance)), ScaleVector(worldUp, vr_overlayscreen_vpos));
		XrPosef pose{};
		pose.orientation = QuaternionFromBasis(right, up, normal);
		pose.orientation = MultiplyQuaternion(pose.orientation, flipRotation);
		pose.position = pos;
		return pose;
	};

	switch (effectiveOverlayMode)
	{
	case 1: // Stationary
		if (!xrStationaryAnchorValid || xrStationaryAnchorMode != effectiveOverlayMode)
		{
			xrStationaryAnchorPose = BuildYawUprightPose(center, yawForward);
			xrStationaryAnchorValid = true;
			xrStationaryAnchorMode = effectiveOverlayMode;
		}
		xrVirtualScreenPose = xrStationaryAnchorPose;
		break;
	case 2: // Stationary (follow)
		if (!xrStationaryAnchorValid || xrStationaryAnchorMode != effectiveOverlayMode)
		{
			xrStationaryFollowCurrentPose = BuildYawUprightPose(center, yawForward);
			xrStationaryFollowTargetPose = xrStationaryFollowCurrentPose;
			xrStationaryFollowNextTargetTimeMs = now + 1000.0;
			xrStationaryFollowLastStepTimeMs = now;
			xrStationaryAnchorValid = true;
			xrStationaryAnchorMode = effectiveOverlayMode;
		}
		if (now >= xrStationaryFollowNextTargetTimeMs)
		{
			const XrPosef candidateTarget = BuildYawUprightPose(center, yawForward);
			const float oldYaw = YawDegFromForward(RotateVector(xrStationaryFollowTargetPose.orientation, { 0.0f, 0.0f, -1.0f }));
			const float newYaw = YawDegFromForward(RotateVector(candidateTarget.orientation, { 0.0f, 0.0f, -1.0f }));
			if (fabsf(ShortestAngleDeltaDeg(newYaw, oldYaw)) >= 15.0f)
				xrStationaryFollowTargetPose = candidateTarget;
			xrStationaryFollowNextTargetTimeMs = now + 1000.0;
		}
		{
			const float dt = (float)clamp((now - xrStationaryFollowLastStepTimeMs) / 1000.0, 0.0, 0.1);
			xrStationaryFollowLastStepTimeMs = now;
			const float step = clamp(dt * 1.1f, 0.0f, 1.0f);
			const float eased = 1.0f - powf(1.0f - step, 3.0f);
			xrStationaryFollowCurrentPose.position = AddVector(
				xrStationaryFollowCurrentPose.position,
				ScaleVector(SubtractVector(xrStationaryFollowTargetPose.position, xrStationaryFollowCurrentPose.position), eased));
			const XrVector3f currentForward = RotateVector(xrStationaryFollowCurrentPose.orientation, { 0.0f, 0.0f, -1.0f });
			const XrVector3f targetForward = RotateVector(xrStationaryFollowTargetPose.orientation, { 0.0f, 0.0f, -1.0f });
			const XrVector3f blendedForward = NormalizeVector(AddVector(ScaleVector(currentForward, 1.0f - eased), ScaleVector(targetForward, eased)));
			XrPosef orientedPose = BuildYawUprightPose(center, blendedForward);
			orientedPose.position = xrStationaryFollowCurrentPose.position;
			xrStationaryFollowCurrentPose = orientedPose;
			xrVirtualScreenPose = xrStationaryFollowCurrentPose;
		}
		break;
	case 4: // Follow Main Hand
	case 5: // Follow Offhand
	{
		const int hand = (effectiveOverlayMode == 4) ? 1 : 0;
		if (hand >= 0 && hand < 2 && xrHandPoseValid[hand])
		{
			const XrQuaternionf handOrientation = xrHandPoses[hand].orientation;
			const XrVector3f handForward = RotateVector(handOrientation, { 0.0f, 0.0f, -1.0f });
			// Keep controller-follow overlay upright (no roll), like OpenVR overlay behavior.
			xrVirtualScreenPose = BuildYawUprightPose(xrHandPoses[hand].position, handForward);
		}
		else
		{
			xrVirtualScreenPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
			xrVirtualScreenPose.position = AddVector(center, ScaleVector(headForward, distance));
			xrVirtualScreenPose.position = AddVector(xrVirtualScreenPose.position, ScaleVector(headUp, vr_overlayscreen_vpos));
		}
		break;
	}
	case 3: // Follow Head movement
	default:
		xrVirtualScreenPose.orientation = MultiplyQuaternion(headOrientation, flipRotation);
		xrVirtualScreenPose.position = AddVector(center, ScaleVector(headForward, distance));
		xrVirtualScreenPose.position = AddVector(xrVirtualScreenPose.position, ScaleVector(headUp, vr_overlayscreen_vpos));
		break;
	}

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
	xrVirtualScreenBackdropPose.position = AddVector(center, ScaleVector(headForward, distance + 0.15f));
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

	if (xrHasEquirectBackdrop)
	{
		xrVirtualScreenBackdropEquirectLayer.type = XR_TYPE_COMPOSITION_LAYER_EQUIRECT_KHR;
		xrVirtualScreenBackdropEquirectLayer.layerFlags = 0;
		xrVirtualScreenBackdropEquirectLayer.space = xrSpace;
		xrVirtualScreenBackdropEquirectLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		xrVirtualScreenBackdropEquirectLayer.pose = xrVirtualScreenBackdropPose;
		xrVirtualScreenBackdropEquirectLayer.radius = 0.0f;
		xrVirtualScreenBackdropEquirectLayer.scale = { 1.0f, 1.0f };
		xrVirtualScreenBackdropEquirectLayer.bias = { 0.0f, 0.0f };
		xrVirtualScreenBackdropEquirectLayer.subImage.swapchain = xrVirtualScreenBackdropSwapchain;
		xrVirtualScreenBackdropEquirectLayer.subImage.imageArrayIndex = 0;
		xrVirtualScreenBackdropEquirectLayer.subImage.imageRect.offset = { 0, 0 };
		xrVirtualScreenBackdropEquirectLayer.subImage.imageRect.extent = { (int32_t)xrVirtualScreenWidth, (int32_t)xrVirtualScreenHeight };
	}

}

bool VKOpenXRDeviceMode::ShouldRenderVirtualScreen() const
{
	const int effectiveOverlayMode = (vr_overlayscreen == 0) ? 2 : vr_overlayscreen;
	const bool overlayEnabled = (effectiveOverlayMode > 0) || vr_overlayscreen_always;
	return ShouldUseScreenLayerForCurrentFrame() &&
		(gamestate != GS_LEVEL || menuactive != MENU_Off || cinemamode || ConsoleState != c_up || vr_overlayscreen_always) &&
		overlayEnabled;
}

bool VKOpenXRDeviceMode::RenderVirtualScreen() const
{
	auto* vkfb = dynamic_cast<VulkanRenderDevice*>(screen);
	if (!vkfb || !xrFrameInProgress || xrSession == XR_NULL_HANDLE || xrVkDevice == nullptr || xrVkCommandBuffer == nullptr)
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (!ShouldRenderVirtualScreen())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	const int effectiveOverlayMode = (vr_overlayscreen == 0) ? 2 : vr_overlayscreen;
	if ((effectiveOverlayMode == 1 || effectiveOverlayMode == 2) && !xrVirtualScreenWasVisibleLastFrame)
	{
		xrStationaryAnchorValid = false;
	}
	xrVirtualScreenWasVisibleLastFrame = true;

	const bool forceOverlay = gamestate != GS_LEVEL || menuactive != MENU_Off || cinemamode || ConsoleState != c_up || vr_overlayscreen_always;
	const bool allowBlankOverlay = vr_overlayscreen_always || cinemamode || gamestate != GS_LEVEL;
	xrMenuPointerBeamImageIndex = -1;
	if (twod == nullptr || (twod->DrawCount() == 0 && !allowBlankOverlay && !forceOverlay))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenImageIndex = -1;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	uint32_t screenWidth = 0;
	uint32_t screenHeight = 0;
	GetStableOpenXRVirtualScreenSize(screenWidth, screenHeight);
	if (!CreateVirtualScreenSwapchain(screenWidth, screenHeight))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (!CreateVirtualScreenBackdropSwapchain(screenWidth, screenHeight))
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (!CreateMenuPointerBeamSwapchain())
	{
		xrMenuPointerBeamVisible = false;
	}
	if (xrVirtualScreenSwapchain == XR_NULL_HANDLE || xrVirtualScreenTextures.empty())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}
	if (xrVirtualScreenBackdropSwapchain == XR_NULL_HANDLE || xrVirtualScreenBackdropTextures.empty())
	{
		xrVirtualScreenVisible = false;
		xrVirtualScreenBackdropVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t imageIndex = 0;
	XrResult xrResult = xrAcquireSwapchainImage(xrVirtualScreenSwapchain, &acquireInfo, &imageIndex);
	if (XR_FAILED(xrResult))
	{
		Printf("OpenXR: virtual screen acquire failed (%d).\n", (int)xrResult);
		xrVirtualScreenVisible = false;
		xrVirtualScreenWasVisibleLastFrame = false;
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
		xrVirtualScreenWasVisibleLastFrame = false;
		return false;
	}

	xrVirtualScreenImageIndex = (int)imageIndex;
	auto& target = xrVirtualScreenTextures[imageIndex];
	const bool useSceneBackdrop = gamestate == GS_LEVEL;
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
	renderState->SetRenderTarget(&target, nullptr, (int)screenWidth, (int)screenHeight, (VkFormat)xrVirtualScreenSwapchainFormat, VK_SAMPLE_COUNT_1_BIT);
	// Render the virtual-screen texture as a regular 2D target. The VR layer
	// compositor will handle the actual head-locked presentation.
	screen->mViewpoints->Set2D(*renderState, (int)screenWidth, (int)screenHeight);
	mInVirtualScreenRender = true;
	if (!useSceneBackdrop)
	{
		renderState->Clear(CT_Color);
	}
	screen->Draw2D(true);
	if (xrMenuPointerActive && xrMenuPointerHasHit && twod != nullptr)
	{
		const float beamEndX = xrMenuPointerX;
		const float beamEndY = xrMenuPointerY;
		const PalEntry cursorColor = PalEntry(vr_menu_pointer_color);
		// twod line colors are packed as AABBGGRR.
		const uint32_t cursorLineColor =
			0xFF000000u |
			((uint32_t)cursorColor.b << 16) |
			((uint32_t)cursorColor.g << 8) |
			(uint32_t)cursorColor.r;
		const float cursorRadius = 8.0f;
		const int cursorSegments = 16;
		for (int i = 0; i < cursorSegments; ++i)
		{
			const float a0 = (float)(2.0 * M_PI * (double)i / (double)cursorSegments);
			const float a1 = (float)(2.0 * M_PI * (double)(i + 1) / (double)cursorSegments);
			const DVector2 p0(beamEndX + std::cos(a0) * cursorRadius, beamEndY + std::sin(a0) * cursorRadius);
			const DVector2 p1(beamEndX + std::cos(a1) * cursorRadius, beamEndY + std::sin(a1) * cursorRadius);
			twod->AddThickLine(p0, p1, 3.0, cursorLineColor, 255);
		}
		twod->AddColorOnlyQuad((int)(beamEndX - 3.0f), (int)(beamEndY - 3.0f), 6, 6, PalEntry(255, cursorColor.r, cursorColor.g, cursorColor.b));
	}
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
	auto* postprocess = vkfb->GetPostprocess();
	const int32_t targetWidth = target.Image != nullptr ? target.Image->width : 0;
	const int32_t targetHeight = target.Image != nullptr ? target.Image->height : 0;
	const int32_t bounceWidth = bounce.Image != nullptr ? bounce.Image->width : 0;
	const int32_t bounceHeight = bounce.Image != nullptr ? bounce.Image->height : 0;

	// Run the same OpenXR headset-facing present bias used by the eye images so
	// the virtual-screen quad can share the tuned gamma/contrast/brightness/
	// saturation response instead of relying on desktop-style flat output.
	if (postprocess != nullptr && targetWidth > 0 && targetHeight > 0 && bounceWidth > 0 && bounceHeight > 0)
	{
		VkImageTransition()
			.AddImage(&target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.AddImage(&bounce, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
			.Execute(cmdbuffer);

		VkImageBlit copyToPipeline = {};
		copyToPipeline.srcOffsets[0] = { 0, 0, 0 };
		copyToPipeline.srcOffsets[1] = { targetWidth, targetHeight, 1 };
		copyToPipeline.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copyToPipeline.dstOffsets[0] = { 0, 0, 0 };
		copyToPipeline.dstOffsets[1] = { bounceWidth, bounceHeight, 1 };
		copyToPipeline.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		cmdbuffer->blitImage(target.Image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			bounce.Image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			1, &copyToPipeline, VK_FILTER_NEAREST);

		VkImageTransition()
			.AddImage(&bounce, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false)
			.Execute(cmdbuffer);

		const int previousPipelineImage = postprocess->GetCurrentPipelineImage();
		postprocess->SetCurrentPipelineImage(0);
		IntRect fullTargetRect = { 0, 0, targetWidth, targetHeight };
		postprocess->DrawPresentTextureToImage(
			&target,
			(VkFormat)xrVirtualScreenSwapchainFormat,
			fullTargetRect,
			true,
			false,
			1.0f,
			1.0f,
			0.0f,
			0.0f,
			cmdbuffer,
			true);
		postprocess->SetCurrentPipelineImage(previousPipelineImage);
	}

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
		.AddImage(&backdropTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
		.Execute(vkfb->GetCommands()->GetDrawCommands());

	const XrVector3f backdropColor =
		(cinemamode || vr_overlayscreen_always || gamestate == GS_LEVEL)
		? GetVirtualScreenBackdropColor()
		: XrVector3f{ 0.0f, 0.0f, 0.0f };
	VkClearColorValue backdropColorValue = {};
	backdropColorValue.float32[0] = backdropColor.x;
	backdropColorValue.float32[1] = backdropColor.y;
	backdropColorValue.float32[2] = backdropColor.z;
	backdropColorValue.float32[3] = 1.0f;
	VkImageSubresourceRange backdropRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkfb->GetCommands()->GetDrawCommands()->clearColorImage(
		backdropTarget.Image->image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		&backdropColorValue,
		1,
		&backdropRange);

	VkImageTransition()
		.AddImage(&backdropTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
		.Execute(vkfb->GetCommands()->GetDrawCommands());

	if (xrMenuPointerBeamVisible && xrMenuPointerBeamSwapchain != XR_NULL_HANDLE && !xrMenuPointerBeamTextures.empty())
	{
		XrSwapchainImageAcquireInfo beamAcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t beamIndex = 0;
		xrResult = xrAcquireSwapchainImage(xrMenuPointerBeamSwapchain, &beamAcquireInfo, &beamIndex);
		if (XR_SUCCEEDED(xrResult))
		{
			XrSwapchainImageWaitInfo beamWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			beamWaitInfo.timeout = 100 * 1000 * 1000;
			xrResult = xrWaitSwapchainImage(xrMenuPointerBeamSwapchain, &beamWaitInfo);
			if (XR_SUCCEEDED(xrResult))
			{
				xrMenuPointerBeamImageIndex = (int)beamIndex;
				auto& beamTarget = xrMenuPointerBeamTextures[beamIndex];

				VkImageTransition()
					.AddImage(&beamTarget, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, false)
					.Execute(vkfb->GetCommands()->GetDrawCommands());

				const int beamColorRaw = (int)vr_menu_pointer_color;
				VkClearColorValue beamColorValue = {};
				beamColorValue.float32[0] = RPART(beamColorRaw) / 255.0f;
				beamColorValue.float32[1] = GPART(beamColorRaw) / 255.0f;
				beamColorValue.float32[2] = BPART(beamColorRaw) / 255.0f;
				beamColorValue.float32[3] = 1.0f;
				VkImageSubresourceRange beamRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				vkfb->GetCommands()->GetDrawCommands()->clearColorImage(
					beamTarget.Image->image,
					VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					&beamColorValue,
					1,
					&beamRange);

				VkImageTransition()
					.AddImage(&beamTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
					.Execute(vkfb->GetCommands()->GetDrawCommands());

				xrMenuPointerBeamLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
				xrMenuPointerBeamLayer.layerFlags = 0;
				xrMenuPointerBeamLayer.space = xrSpace;
				xrMenuPointerBeamLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				xrMenuPointerBeamLayer.pose = xrMenuPointerBeamPose;
				xrMenuPointerBeamLayer.size = { std::max(0.02f, xrMenuPointerBeamLength), 0.005f };
				xrMenuPointerBeamLayer.subImage.swapchain = xrMenuPointerBeamSwapchain;
				xrMenuPointerBeamLayer.subImage.imageArrayIndex = 0;
				xrMenuPointerBeamLayer.subImage.imageRect.offset = { 0, 0 };
				xrMenuPointerBeamLayer.subImage.imageRect.extent = { beamTarget.Image->width, beamTarget.Image->height };
			}

			XrSwapchainImageReleaseInfo beamReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(xrMenuPointerBeamSwapchain, &beamReleaseInfo);
		}
	}

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

	// 1) Unbiased image for desktop mirror parity.
	if (eyeIndex >= 0 && eyeIndex < (int)xrMirrorPresentTextures.size())
	{
		postprocess->DrawPresentTextureToImage(
			&xrMirrorPresentTextures[eyeIndex],
			(VkFormat)xrSwapchainFormat,
			targetBox,
			true,
			false,
			sourceRect.scaleX,
			sourceRect.scaleY,
			sourceRect.offsetX,
			sourceRect.offsetY,
			vkfb->GetCommands()->GetDrawCommands(),
			false);
	}

	// 2) XR-submitted image with OpenXR bias knobs applied.
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
		vkfb->GetCommands()->GetDrawCommands(),
		true);
}

bool VKOpenXRDeviceMode::RenderDesktopMirror(VulkanRenderDevice* fb, VulkanImage* dstImage) const
{
	if (!fb || !dstImage)
		return false;

	auto* cmdbuffer = fb->GetCommands()->GetDrawCommands();
	const bool sideBySide = vr_desktop_view != 1 && vr_desktop_view != 2;
	const int leftSourceIndex = vr_swap_eyes ? 1 : 0;
	const int rightSourceIndex = vr_swap_eyes ? 0 : 1;

	if (xrMirrorPresentTextures.empty() || xrMirrorPresentTextures[leftSourceIndex].Image == nullptr || (sideBySide && xrMirrorPresentTextures[rightSourceIndex].Image == nullptr))
		return false;

	const bool hasBackdrop = xrVirtualScreenBackdropVisible &&
		xrVirtualScreenBackdropImageIndex >= 0 &&
		xrVirtualScreenBackdropImageIndex < (int)xrVirtualScreenBackdropTextures.size();

	VkTextureImage* leftEyeSource = &xrMirrorPresentTextures[leftSourceIndex];
	VkTextureImage* rightEyeSource = &xrMirrorPresentTextures[rightSourceIndex];
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
	player_t* player = &players[consoleplayer];
	if (player)
	{
		const bool rightHanded = IsRightHandedVrControls();
		const bool useMainHandPose = (rightHanded && hand == 1) || (!rightHanded && hand == 0);
		float* offset = useMainHandPose ? weaponoffset : offhandoffset;
		float* angles = useMainHandPose ? weaponangles : offhandangles;

		mat->loadIdentity();
		mat->translate((float)r_viewpoint.CenterEyePos.X, (float)r_viewpoint.CenterEyePos.Z - GetDoomPlayerHeightWithoutCrouch(player), (float)r_viewpoint.CenterEyePos.Y);
		mat->scale((float)vr_vunits_per_meter, (float)vr_vunits_per_meter, (float)-vr_vunits_per_meter);

		mat->translate(-offset[0], (hmdPosition[1] + offset[1] + (float)vr_height_adjust) / (float)pixelstretch, offset[2]);
		mat->scale(1, 1 / (float)pixelstretch, 1);

		if (ShouldUseScreenLayerForCurrentFrame())
		{
			mat->rotate(-90 + r_viewpoint.Angles.Yaw.Degrees() + (angles[1] - playerYaw), 0, 1, 0);
			mat->rotate(-angles[0] - r_viewpoint.Angles.Pitch.Degrees(), 1, 0, 0);
		}
		else
		{
			mat->rotate(-90 + doomYaw + (angles[1] - hmdorientation[1]), 0, 1, 0);
			mat->rotate(-angles[0], 1, 0, 0);
		}
		mat->rotate(-angles[2], 0, 0, 1);
		return true;
	}
	return false;
}

bool VKOpenXRDeviceMode::GetTeleportLocation(DVector3 &out) const
{
	player_t* player = &players[consoleplayer];
	if (vr_teleport &&
		ready_teleport &&
		(player && player->mo->health > 0) &&
		m_TeleportTarget == TRACE_HitFloor)
	{
		out = m_TeleportLocation;
		return true;
	}

	return false;
}

void VKOpenXRDeviceMode::StopHaptics() const
{
	if (xrSession == XR_NULL_HANDLE || xrHapticAction == XR_NULL_HANDLE)
		return;

	for (int hand = 0; hand < 2; ++hand)
	{
		if (!xrHapticActive[hand])
			continue;

		XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		actionInfo.action = xrHapticAction;
		actionInfo.subactionPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
		xrStopHapticFeedback(xrSession, &actionInfo);
		xrHapticActive[hand] = false;
	}

	xrHapticDuration[0] = xrHapticDuration[1] = 0.0;
	xrHapticIntensity[0] = xrHapticIntensity[1] = 0.0f;
}

static XrDuration MakeOpenXRHapticDuration(double remainingMs)
{
	// XR_MIN_HAPTIC_DURATION is valid, but several runtimes are much more reliable when we
	// send a small finite pulse and re-issue it while the local timer is still active.
	constexpr double kMinPulseMs = 1.0;
	constexpr double kMaxPulseMs = 10.0;

	if (remainingMs < kMinPulseMs)
		remainingMs = kMinPulseMs;
	else if (remainingMs > kMaxPulseMs)
		remainingMs = kMaxPulseMs;

	return static_cast<XrDuration>(std::llround(remainingMs * 1000000.0));
}

void VKOpenXRDeviceMode::ProcessHaptics() const
{
	if (!vr_enable_haptics || xrSession == XR_NULL_HANDLE || xrHapticAction == XR_NULL_HANDLE || !isSessionRunning)
	{
		StopHaptics();
		return;
	}

	static auto lastUpdate = std::chrono::steady_clock::now();
	const auto now = std::chrono::steady_clock::now();
	const double elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();
	lastUpdate = now;

	for (int hand = 0; hand < 2; ++hand)
	{
		const bool active = xrHapticDuration[hand] != 0.0 && xrHapticIntensity[hand] > 0.0f;
		if (!active)
		{
			if (xrHapticActive[hand])
			{
				XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
				actionInfo.action = xrHapticAction;
				actionInfo.subactionPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
				xrStopHapticFeedback(xrSession, &actionInfo);
				xrHapticActive[hand] = false;
			}
			continue;
		}

		XrHapticVibration vibration{ XR_TYPE_HAPTIC_VIBRATION };
		vibration.amplitude = clamp<float>(xrHapticIntensity[hand], 0.0f, 1.0f);
		vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
		vibration.duration = MakeOpenXRHapticDuration(xrHapticDuration[hand]);

		XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		actionInfo.action = xrHapticAction;
		actionInfo.subactionPath = (hand == 0) ? xrLeftHandPath : xrRightHandPath;
		XrResult result = xrApplyHapticFeedback(xrSession, &actionInfo, (XrHapticBaseHeader*)&vibration);
		if (XR_SUCCEEDED(result))
		{
			xrHapticActive[hand] = true;
		}
		else if (xrHapticActive[hand])
		{
			xrStopHapticFeedback(xrSession, &actionInfo);
			xrHapticActive[hand] = false;
		}

		if (xrHapticDuration[hand] > 0.0)
		{
			xrHapticDuration[hand] -= elapsedMs;
			if (xrHapticDuration[hand] <= 0.0)
			{
				xrHapticDuration[hand] = 0.0;
			}
		}
	}
}

void VKOpenXRDeviceMode::Vibrate(float duration, int channel, float intensity) const
{
	if (channel < 0)
		channel = 0;
	if (channel > 1)
		channel = 1;

	if (!vr_enable_haptics)
		return;

	xrHapticDuration[channel] = duration;
	xrHapticIntensity[channel] = intensity;

	if (duration <= 0.0f || intensity <= 0.0f)
	{
		XrHapticActionInfo actionInfo{ XR_TYPE_HAPTIC_ACTION_INFO };
		actionInfo.action = xrHapticAction;
		actionInfo.subactionPath = (channel == 0) ? xrLeftHandPath : xrRightHandPath;
		if (xrSession != XR_NULL_HANDLE && xrHapticAction != XR_NULL_HANDLE)
			xrStopHapticFeedback(xrSession, &actionInfo);
		xrHapticActive[channel] = false;
		xrHapticDuration[channel] = 0.0;
		xrHapticIntensity[channel] = 0.0f;
		return;
	}

	ProcessHaptics();
}

void VKOpenXRDeviceMode::InitializeMultiview() const {}

} // namespace s3d
