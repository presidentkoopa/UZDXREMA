#include "doomtype.h"
#include "VrCommon.h"
#include "hw_vrmodes.h"
#include <cmath>

#if defined(_WIN32) && !defined(__ANDROID__)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

EXTERN_CVAR(Float, fov)
EXTERN_CVAR(Int, vr_overlayscreen);
EXTERN_CVAR(Bool, vr_overlayscreen_always);

//Define all variables here that were externs in the VrCommon.h
vec3_t weaponangles;
vec3_t weaponoffset;
vec3_t offhandangles;
vec3_t offhandoffset;
vec3_t worldPosition;
vec3_t hmdPosition;
vec3_t hmdorientation;
vec3_t positionDeltaThisFrame;

bool weaponStabilised;
bool vrApplyingHmdYaw;
bool resetDoomYaw;
bool resetPreviousPitch;
// bool shutdown;
bool ready_teleport;
bool trigger_teleport;
bool cinemamode;

float playerYaw;
float doomYaw;
float previousPitch;
float snapTurn;
float cinemamodeYaw;
float cinemamodePitch;
float remote_movementSideways;
float remote_movementForward;
float positional_movementSideways;
float positional_movementForward;
static float vr_mp_pendingTeleportForwardUnits;
static float vr_mp_pendingTeleportSideUnits;
static float vr_mp_crouchHeightMapUnits = -1.0f;
static bool vr_hasWorldPositionSample = false;

//This is now controlled by the engine
static bool useVirtualScreen = false;

/*
================================================================================

QuestZDoom Stuff

================================================================================
*/

int QzDoom_SetRefreshRate(int refreshRate)
{
    return 0;
}

void QzDoom_GetScreenRes(uint32_t *width, uint32_t *height)
{
}

float QzDoom_GetFOV()
{
	const auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode->IsVR()) return 90.;
	return fov;
}

void VR_QueueTeleportCommandBurst(float forwardUnits, float sideUnits)
{
    vr_mp_pendingTeleportForwardUnits = forwardUnits;
    vr_mp_pendingTeleportSideUnits = sideUnits;
}

void VR_ClearTeleportCommandBurst()
{
    vr_mp_pendingTeleportForwardUnits = 0.0f;
    vr_mp_pendingTeleportSideUnits = 0.0f;
}

bool VR_ConsumeTeleportCommandStep(float maxUnitsPerTick, float* outForwardUnits, float* outSideUnits)
{
    if (outForwardUnits == nullptr || outSideUnits == nullptr || maxUnitsPerTick <= 0.0f)
    {
        return false;
    }

    const float forward = vr_mp_pendingTeleportForwardUnits;
    const float side = vr_mp_pendingTeleportSideUnits;
    const float length = sqrtf((forward * forward) + (side * side));
    if (length <= 0.001f)
    {
        *outForwardUnits = 0.0f;
        *outSideUnits = 0.0f;
        VR_ClearTeleportCommandBurst();
        return false;
    }

    const float scale = length > maxUnitsPerTick ? (maxUnitsPerTick / length) : 1.0f;
    *outForwardUnits = forward * scale;
    *outSideUnits = side * scale;

    vr_mp_pendingTeleportForwardUnits -= *outForwardUnits;
    vr_mp_pendingTeleportSideUnits -= *outSideUnits;

    if (fabsf(vr_mp_pendingTeleportForwardUnits) <= 0.001f && fabsf(vr_mp_pendingTeleportSideUnits) <= 0.001f)
    {
        VR_ClearTeleportCommandBurst();
    }

    return true;
}

void VR_ResetTransientNetSafeState()
{
    VectorSet(worldPosition, 0.0f, 0.0f, 0.0f);
    VectorSet(positionDeltaThisFrame, 0.0f, 0.0f, 0.0f);

    remote_movementForward = 0.0f;
    remote_movementSideways = 0.0f;
    positional_movementForward = 0.0f;
    positional_movementSideways = 0.0f;

    ready_teleport = false;
    trigger_teleport = false;
    snapTurn = 0.0f;
    cinemamodeYaw = 0.0f;
    cinemamodePitch = 0.0f;

    VR_ClearTeleportCommandBurst();
    VR_ClearMultiplayerCrouchHeight();
    vr_hasWorldPositionSample = false;
}

void VR_SetMultiplayerCrouchHeight(float hmdHeightMapUnits)
{
    vr_mp_crouchHeightMapUnits = hmdHeightMapUnits;
}

void VR_ClearMultiplayerCrouchHeight()
{
    vr_mp_crouchHeightMapUnits = -1.0f;
}

bool VR_GetMultiplayerCrouchHeight(float* outHmdHeightMapUnits)
{
    if (outHmdHeightMapUnits == nullptr || vr_mp_crouchHeightMapUnits <= 0.0f)
    {
        return false;
    }

    *outHmdHeightMapUnits = vr_mp_crouchHeightMapUnits;
    return true;
}

void VR_HapticEvent(const char* event, int position, int intensity, float angle, float yHeight )
{
}

void QzDoom_Restart()
{
#if defined(__ANDROID__)
	return;
#elif defined(_WIN32)
	WCHAR path[MAX_PATH] = {};
	if (GetModuleFileNameW(GetModuleHandleW(nullptr), path, MAX_PATH) > 0)
	{
		ShellExecuteW(nullptr, L"open", path, GetCommandLineW(), nullptr, SW_SHOWNORMAL);
	}
#endif
}

void QzDoom_setUseScreenLayer(bool use)
{
	useVirtualScreen = use;
}

bool VR_UseScreenLayer()
{
	return vr_overlayscreen && (useVirtualScreen || cinemamode || vr_overlayscreen_always);
}

void VR_SetHMDOrientation(float pitch, float yaw, float roll)
{
	VectorSet(hmdorientation, pitch, yaw, roll);

	if (!VR_UseScreenLayer())
	{
		playerYaw = yaw;
	}
}

void VR_SetHMDPosition(float x, float y, float z )
{
 	VectorSet(hmdPosition, x, y, z);

	if (!vr_hasWorldPositionSample)
	{
		VectorSet(positionDeltaThisFrame, 0.0f, 0.0f, 0.0f);
		vr_hasWorldPositionSample = true;
	}
	else
	{
		positionDeltaThisFrame[0] = (worldPosition[0] - x);
		positionDeltaThisFrame[1] = (worldPosition[1] - y);
		positionDeltaThisFrame[2] = (worldPosition[2] - z);
	}

	worldPosition[0] = x;
	worldPosition[1] = y;
	worldPosition[2] = z;
}

static float DEG2RAD(float deg)
{
	return deg * float(M_PI / 180.0);
}

void VR_GetMove(float *joy_forward, float *joy_side, float *hmd_forward, float *hmd_side, float *up,
				float *yaw, float *pitch, float *roll)
{
    *joy_forward = remote_movementForward;
    *joy_side = remote_movementSideways;
    *hmd_forward = positional_movementForward;
    *hmd_side = positional_movementSideways;
    *up = 0.0f;
    *yaw = VR_UseScreenLayer() ? cinemamodeYaw : hmdorientation[YAW] + snapTurn;
	*pitch = VR_UseScreenLayer() ? cinemamodePitch : hmdorientation[PITCH];
	*roll = VR_UseScreenLayer() ? 0.0f : hmdorientation[ROLL];
}

