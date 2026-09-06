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
bool resetPreviousHmdYaw;
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
static VRMultiplayerTeleportTarget vr_mp_pendingTeleportTarget;
static float vr_mp_roomscaleWorldOffsetX = 0.0f;
static float vr_mp_roomscaleWorldOffsetY = 0.0f;
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

VRMultiplayerTeleportTarget VR_MakeCanonicalMultiplayerTeleportTarget(double x, double y, double z, bool telefrag)
{
    VRMultiplayerTeleportTarget target;
    target.x = (int32_t)clamp((int64_t)llround(x), (int64_t)INT32_MIN, (int64_t)INT32_MAX);
    target.y = (int32_t)clamp((int64_t)llround(y), (int64_t)INT32_MIN, (int64_t)INT32_MAX);
    target.z = (int32_t)clamp((int64_t)llround(z), (int64_t)INT32_MIN, (int64_t)INT32_MAX);
    target.telefrag = telefrag;
    target.valid = true;
    return target;
}

void VR_QueueMultiplayerTeleportTarget(const VRMultiplayerTeleportTarget& target)
{
    vr_mp_pendingTeleportTarget = target;
    VR_ClearTeleportCommandBurst();
}

void VR_QueueMultiplayerRoomscaleTeleportTarget(const VRMultiplayerTeleportTarget& target)
{
    vr_mp_pendingTeleportTarget = target;
    vr_mp_pendingTeleportTarget.telefrag = false;
    VR_ClearTeleportCommandBurst();
}

bool VR_ConsumeMultiplayerTeleportTarget(VRMultiplayerTeleportTarget* outTarget)
{
    if (outTarget == nullptr || !vr_mp_pendingTeleportTarget.valid)
    {
        return false;
    }

    *outTarget = vr_mp_pendingTeleportTarget;
    vr_mp_pendingTeleportTarget = {};
    return true;
}

void VR_ClearMultiplayerTeleportTarget()
{
    vr_mp_pendingTeleportTarget = {};
}

void VR_AddMultiplayerRoomscaleWorldOffset(float xUnits, float yUnits)
{
    vr_mp_roomscaleWorldOffsetX += xUnits;
    vr_mp_roomscaleWorldOffsetY += yUnits;
}

bool VR_GetMultiplayerRoomscaleWorldOffset(float* outXUnits, float* outYUnits)
{
    if (outXUnits == nullptr || outYUnits == nullptr)
    {
        return false;
    }

    *outXUnits = vr_mp_roomscaleWorldOffsetX;
    *outYUnits = vr_mp_roomscaleWorldOffsetY;
    return fabsf(vr_mp_roomscaleWorldOffsetX) > 0.0001f || fabsf(vr_mp_roomscaleWorldOffsetY) > 0.0001f;
}

void VR_ClearMultiplayerRoomscaleWorldOffset()
{
    vr_mp_roomscaleWorldOffsetX = 0.0f;
    vr_mp_roomscaleWorldOffsetY = 0.0f;
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
    VR_ClearMultiplayerTeleportTarget();
    VR_ClearMultiplayerRoomscaleWorldOffset();
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

// [BB] THE PLAYSIM'S HAPTICS, CONNECTED.
//
// This function had an empty body. Every built-in haptic event in the game calls
// it -- firing a weapon, taking a bullet, standing in slime, picking something
// up, a door closing -- each one computing an intensity from its matching
// ext_haptic_level_* cvar and handing that number to nothing. Twenty-odd call
// sites across p_map.cpp, p_mobj.cpp, p_interaction.cpp, a_weapons.cpp,
// sbar_mugshot.cpp and t_func.cpp, all inert.
//
// That is why controllers never buzzed for anything the game itself did, and why
// the whole ext_haptic_level_* menu had no effect: the settings were real and
// wired, the consumer was not. hw_vrmodes.h:243 states the situation outright.
//
// VRMode::Vibrate is the path that actually reaches the runtime
// (VKOpenXRDeviceMode::Vibrate -> xrApplyHapticFeedback). It already gates on
// vr_enable_haptics and clamps its own arguments, so this only has to translate.
//
// TRANSLATION, NOT POLICY. Three things need converting and nothing else belongs
// here:
//
//   POSITION -> CHANNEL. Callers pass a PHYSICAL side: 1 left, 2 right. See
//     p_map.cpp:4847, which writes `rightHanded ? 2 : 1` for the main hand.
//     Vibrate takes a physical channel of 0 left, 1 right, so this is
//     position - 1. Position 0 means no side was given, which for a body event
//     like poison or a health station is honest rather than missing -- it goes to
//     both hands.
//
//   INTENSITY. Callers pass 100 * the event's ext_haptic_level_* value, so full
//     strength is nominally 100 and a player who turns one event up can exceed
//     it deliberately. Divided by 100 and left for Vibrate to clamp, so raising a
//     level above 1.0 still does something rather than being silently flattened
//     here.
//
//   DURATION, which no caller supplies. One length for everything would make a
//     pistol shot feel identical to walking into slime, so the event name picks
//     it. These are LENGTHS, not strengths: how long a thing lasts is a property
//     of the thing, while how hard it hits is the player's setting and stays
//     theirs.
//
// angle and yHeight are ignored. They address positional hardware -- a vest or a
// belt, where an event has a place on your body. A controller has no such axis,
// and inventing one from them would be a guess dressed as a feature.
CVAR(Float, vr_haptic_event_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

static float VR_HapticEventDurationMs(const char* event)
{
	if (event == nullptr) return 60.0f;

	// Ordered by how often each fires, so the common case compares first.
	if (!stricmp(event, "fire_weapon"))   return 55.0f;
	if (!stricmp(event, "bullet"))        return 70.0f;
	if (!stricmp(event, "shotgun"))       return 110.0f;
	if (!stricmp(event, "fireball"))      return 120.0f;
	if (!stricmp(event, "melee"))         return 100.0f;
	if (!stricmp(event, "pickup"))        return 35.0f;
	if (!stricmp(event, "pickup_weapon")) return 60.0f;
	if (!stricmp(event, "doorclose"))     return 45.0f;
	if (!stricmp(event, "healstation"))   return 30.0f;

	// The damage-over-time set. Longer and softer, because these repeat for as
	// long as you stand in the thing -- a short sharp tap on repeat is a stutter,
	// not a warning.
	if (!stricmp(event, "slime"))         return 140.0f;
	if (!stricmp(event, "fire"))          return 140.0f;
	if (!stricmp(event, "poison"))        return 140.0f;

	// Unknown event, and reachable: p_interaction.cpp:1945 passes a mod-defined
	// poison type by name. A middling default is better than silence, because an
	// unrecognised event is still a real thing that happened to the player.
	return 60.0f;
}

void VR_HapticEvent(const char* event, int position, int intensity, float angle, float yHeight )
{
	const VRMode* vrmode = VRMode::GetVRModeCached();
	if (vrmode == nullptr) return;

	const float amp = (intensity / 100.0f) * (float)vr_haptic_event_scale;
	if (amp <= 0.0f) return;

	const float ms = VR_HapticEventDurationMs(event);

	if (position == 1 || position == 2)
	{
		vrmode->Vibrate(ms, position - 1, amp);
	}
	else
	{
		vrmode->Vibrate(ms, 0, amp);
		vrmode->Vibrate(ms, 1, amp);
	}
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

bool VR_UseCinematicScreenLayer()
{
	return vr_overlayscreen && (cinemamode || vr_overlayscreen_always);
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

