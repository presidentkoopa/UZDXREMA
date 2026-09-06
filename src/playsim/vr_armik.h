/*
** vr_armik.h
**
** VR body avatar + native two-bone arm IK -- ported whole from the DXR fork
** (playsim/p_user.cpp VR_UpdateArmIK and its helpers, r_data/models.cpp body
** fit + matrix publish, d_player.h state, vmthunks SetArmIKEnabled).
**
** The avatar is whichever actor player_t::vr_body_actor names; when nothing
** has set it, the player pawn itself, exactly as the original did. Any mod
** may point it at its own body actor with Actor.SetVRBodyActor.
**
**---------------------------------------------------------------------------
*/
#pragma once

#include "matrix.h"
#include "name.h"
#include "vectors.h"

struct player_t;
class AActor;
class FModel;

// The actor the renderer treats as the local VR body and the IK drives.
// player->vr_body_actor when set, else player->mo. Never null for a live player.
AActor* VR_BodyActor(player_t* player);

// Two-bone shoulder/elbow solve onto the controllers; writes the avatar's
// modelData->proceduralPose.
void VR_UpdateArmIK(player_t* player);
// Runs VR_UpdateArmIK at most once per rendered frame. Called by the renderer for the body
// actor right after it has published the body's object matrix for that frame.
void VR_UpdateArmIKFrame(player_t* player, uint64_t frameStamp);

// r_data/models.cpp: where a joint of `a`'s model is drawn this frame -- the world (GL layout)
// position of (joint bind position + offsetModel, both in the model's file space) and the
// model's object matrix. False if the actor has no model frame or no such joint.
bool VR_ModelJointWorld(AActor* a, FName joint, const FVector3& offsetModel, FVector3& outPosGL, VSMatrix& outObjToWorld);
// The inverse: a Doom world point expressed as an offset from a joint, in the model's file space.
bool VR_ModelWorldToJointOffset(AActor* a, FName joint, const DVector3& worldDoom, FVector3& outOffsetModel);
// The heading an attached actor inherits from its parent (the VR body's decoupled facing, or the
// parent's own yaw).
double VR_ActorFacing(AActor* a);

// Decouple the drawn body facing from the HMD-slaved pawn yaw (50-deg dead zone).
// Called from P_PlayerThink after VR_UpdateArmIK, same order as the original.
void VR_UpdateBodyFacing(player_t* player);


// Allocates an empty DActorModelData on the avatar if it has none and returns
// the rigged IQM bound to it by its static MODELDEF (or nullptr).
FModel* VR_EnsureAvatarModelDataAndGetModel(AActor* mo);

// p_actionfunctions.cpp -- the file-static EnsureModelData, exported.
void P_EnsureActorModelData(AActor* mobj);

// Published by the renderer (r_data/models.cpp) for the playsim IK: the live
// body-fit scale and the EXACT finalized objectToWorldMatrix the body was drawn
// with this frame. Written on the render thread, read on the playsim thread; at
// worst one frame stale. Valid flag guards the pre-first-render frame.
extern float    g_xr_vrBodyRenderScale;
extern VSMatrix g_xr_vrBodyObjectToWorld;
extern bool     g_xr_vrBodyObjToWorldValid;
extern int      g_xr_vrRenderProcHits;
