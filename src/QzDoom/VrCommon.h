#if !defined(vrcommon_h)
#define vrcommon_h

#include "c_cvars.h"

typedef float vec_t;
typedef vec_t vec3_t[3];

#define PITCH 0
#define YAW 1
#define ROLL 2

#define VectorSet(v, x, y, z) ((v)[0]=(x),(v)[1]=(y),(v)[2]=(z))

EXTERN_CVAR(Int, vr_control_scheme)
EXTERN_CVAR(Bool, vr_move_use_offhand)
EXTERN_CVAR(Float, vr_weaponRotate);
EXTERN_CVAR(Float, vr_snapTurn);
EXTERN_CVAR(Float, vr_ipd);
EXTERN_CVAR(Float, vr_weaponScale);
EXTERN_CVAR(Bool, vr_teleport);
EXTERN_CVAR(Bool, vr_switch_sticks);
EXTERN_CVAR(Bool, vr_secondary_button_mappings);
EXTERN_CVAR(Bool, vr_two_handed_weapons);
EXTERN_CVAR(Bool, vr_crouch_use_button);

extern bool cinemamode;
extern float cinemamodeYaw;
extern float cinemamodePitch;

extern float playerYaw;
extern bool vrApplyingHmdYaw;
extern bool resetDoomYaw;
extern float doomYaw;
extern bool resetPreviousPitch;
extern float previousPitch;

extern vec3_t weaponangles;
extern vec3_t weaponoffset;

extern vec3_t offhandangles;
extern vec3_t offhandoffset;

extern bool player_moving;

extern bool ready_teleport;
extern bool trigger_teleport;

// Multiplayer-safe VR locomotion helpers.
void VR_QueueTeleportCommandBurst(float forwardUnits, float sideUnits);
void VR_ClearTeleportCommandBurst();
bool VR_ConsumeTeleportCommandStep(float maxUnitsPerTick, float* outForwardUnits, float* outSideUnits);
void VR_QueueMultiplayerTeleportTarget(float x, float y, float z);
void VR_QueueMultiplayerRoomscaleTeleportTarget(float x, float y, float z);
bool VR_ConsumeMultiplayerTeleportTarget(float* outX, float* outY, float* outZ, bool* outTelefrag);
void VR_ClearMultiplayerTeleportTarget();
void VR_AddMultiplayerRoomscaleWorldOffset(float xUnits, float yUnits);
bool VR_GetMultiplayerRoomscaleWorldOffset(float* outXUnits, float* outYUnits);
void VR_ClearMultiplayerRoomscaleWorldOffset();
void VR_ResetTransientNetSafeState();
void VR_SetMultiplayerCrouchHeight(float hmdHeightMapUnits);
void VR_ClearMultiplayerCrouchHeight();
bool VR_GetMultiplayerCrouchHeight(float* outHmdHeightMapUnits);

// Shared smooth-turn helper used by OpenVR/OpenXR.
float VR_GetAnalogTurnResponseScale(float smoothTurnSetting);
float VR_ApplyAnalogSmoothTurn(float turnAxis, float maxTurnRateDegPerSec, float deltaSeconds, float responseScale, float& currentTurnRateDegPerSec);

//Called from engine code
void QzDoom_setUseScreenLayer(bool use);
bool VR_UseScreenLayer();
void QzDoom_Restart();


#endif //vrcommon_h