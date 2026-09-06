/*
** vr_armik.cpp
**
** VR body avatar + native two-bone arm IK. Ported WHOLE from the DXR fork
** (playsim/p_user.cpp: IK_* helpers, IK_SolveTwoBoneArm, VR_UpdateArmIK, the
** body-facing decouple in P_PlayerThink; p_actionfunctions.cpp:
** VR_EnsureAvatarModelDataAndGetModel). The text below is that code, with the
** only changes being what UZDoom 5.0 forces, each marked [5.0]:
**
**   * TRS::rotation is an FQuaternion here, not an FVector4.
**   * FindModelFrame(AActor*, sprite, frame, dropped) lost its fifth argument.
**   * The avatar is VR_BodyActor(player) -- player->vr_body_actor when a mod
**     set one, else player->mo exactly as before -- instead of player->mo
**     hard-coded, so a body that is its own actor can be driven.
**   * VSMatrix::get() returns FLOATTYPE here (float unless VSMATRIX_DOUBLE).
**
** Everything else -- the exact-inverse frame, wall clamp, foregrip pin, palm
** seat, target nudge, stretch, wrist follow + smoothing + rate limit, offhand
** flip, finger curl, every probe and every cvar -- is as it was.
**
**---------------------------------------------------------------------------
*/

#include <cmath>
#include "vr_armik.h"
#include "basics.h"
#include "doomstat.h"
#include "d_player.h"
#include "actor.h"
#include "g_levellocals.h"
#include "hw_vrmodes.h"
#include "model.h"          // FModel, extern TDeletingArray<FModel*> Models -- VR_UpdateArmIK reads joint data
#include "models.h"         // FSpriteModelFrame / FindModelFrame
#include "c_cvars.h"
#include "printf.h"
#include "vectors.h"
#include "quaternion.h"
#include "TRS.h"
#include "matrix.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

EXTERN_CVAR(Int,   vr_control_scheme)
EXTERN_CVAR(Bool,  vr_arm_ik)
EXTERN_CVAR(Float, vr_ik_upperarm_len)
EXTERN_CVAR(Float, vr_ik_forearm_len)
EXTERN_CVAR(Bool,  vr_ik_hand_rot)
EXTERN_CVAR(Float, vr_ik_hand_pitch)
EXTERN_CVAR(Float, vr_ik_hand_yaw)
EXTERN_CVAR(Float, vr_ik_hand_roll)
EXTERN_CVAR(Float, vr_ik_hand_smooth)
EXTERN_CVAR(Float, vr_ik_hand_maxstep)
EXTERN_CVAR(Bool,  vr_hand_ik_clamp)

// [XR] Live-tunable CONSTANT nudge of the IK hand target in the model's own frame so the hand can be slid
// exactly onto the controller in-headset (same idea as per-weapon model offsets). Absorbs any fixed shift
// the frame math leaves (vr_body_z, feet-vs-mesh-origin, mesh authoring). 0,0,0 = no nudge. Dial in-console.
CVAR(Float, vr_ik_target_offx, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // +X = model LATERAL (+left / -right)
CVAR(Float, vr_ik_target_offy, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // +Y = model FORWARD (view direction)
CVAR(Float, vr_ik_target_offz, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // +Z = model UP
// [XR] PALM-SEAT correction. The two-bone IK places the marine WRIST joint on the controller/grip point, but
// the hand MESH is skinned FORWARD of the wrist -- so the palm/fingers (the part that wraps a gun grip) land a
// few units PAST the controller. This pulls the IK target back along model-FORWARD by that fixed wrist->palm
// length, so the PALM seats on the controller (and thus on the weapon's hs_grip, which sits at the model
// origin = the controller). Applied to BOTH hands (same mesh authoring) every tic. Default is a best-guess
// starting length; dial in headset until the palm sits in the controller sphere. Set 0 to disable (revert).
CVAR(Float, vr_ik_palm_seat, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)   // [XR] 0 = wrist joint exactly on the target
// [XR] HAND GRIP CURL. The arm IK poses the wrist but leaves the 15 finger joints per hand at their OPEN bind
// pose, so the hand never closes on a held gun. When a hand holds a weapon, curl its finger joints into a fist.
// Rig fact (marine_novr.iqm, verified from bind geometry): finger bones point down local -Z and spread along X,
// so flexion is rotation about local +X -- the axis is derived, NOT guessed. Only the SIGN may need a flip:
// if fingers bend BACKWARD, negate vr_hand_grip_curl / _thumb. Set vr_hand_grip 0 to disable (open hands).
CVAR(Bool,  vr_hand_grip,       true,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // curl fingers around a held weapon
CVAR(Float, vr_hand_grip_curl,  35.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // per-segment finger flex angle (deg; negate to flip)
CVAR(Float, vr_hand_grip_thumb, 15.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // thumb flex angle (deg; usually less than the fingers)
// [XR] Overall arm-length scale, wired into VR_UpdateArmIK's per-side bone lengths (both bones x this). 1.0 =
// the rig's own proportions; <1 shortens reach, >1 lengthens. Clamped 0.5..2.0 in the solver. Slider-tunable.
CVAR(Float, vr_ik_reach_scale,    1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] Reach limit: how far past their natural length the arm bones may be lengthened to meet a
// hand. Past this the arm points at the hand and stops. 2.5 was the original's cap and read as
// rubber arms; 1.25 covers the difference between a seated reach and the rig's proportions.
CVAR(Float, vr_ik_stretch_max,   1.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] Elbow direction: the pole the elbow bends toward, as weights on outward / down / back.
CVAR(Float, vr_ik_elbow_out,     1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_ik_elbow_down,    0.6f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_ik_elbow_back,    0.35f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] Per-limb sizes on top of the body size: uniform scale on each upper arm (carries the
// forearm and hand with it) and on each hand joint (gauntlet and fingers only). Reach follows
// the arm size; the hand size changes nothing about reach.
CVAR(Float, vr_body_arm_size,    1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_hand_size,   1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] How far the head may turn before the body follows it (degrees).
CVAR(Float, vr_body_facing_deadzone, 50.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] POSTURE. Lean: the headset's horizontal offset from where the rig's eyes sit is bent into
// the spine, so leaning forward or sideways bends the torso. Crouch: the headset's height below the
// standing sample lowers the hips and the legs bend to keep the feet where they were. Both need the
// rig roles spine_0.. (root to top), hips, and hip_r/knee_r/foot_r (+_l).
CVAR(Bool,  vr_body_lean,          false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_lean_gain,     1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)   // 1 = the neck follows the headset exactly
CVAR(Bool,  vr_body_crouch,        false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_crouch_deadzone, 2.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // units of headset drop ignored (head bob)
// [XR] FINGER TOUCH: controllers with capacitive sensing report whether the index finger rests on the
// trigger and the thumb on the face (pawn FingerTouchMain/Off, FINGERTOUCH_INDEX / _THUMB in
// vk_openxrdevice.h). A lifted finger's three joints open toward the bind pose by vr_hand_touch_open
// (0..1). Default off: a controller without touch sensing reports "lifted" forever.
CVAR(Bool,  vr_hand_touch_fingers,  false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hand_touch_open,      1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] FEET ON TERRAIN: each foot's rest point is lifted/dropped by the sector floor under it, relative
// to the pawn's own floor, and the leg re-solved (knee forward) so stairs and slopes stop the feet
// floating or sinking. Limited to +-vr_body_feet_max units; the leg never stretches; 3D floors are
// not sampled. Default off.
CVAR(Bool,  vr_body_feet_terrain,   false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_feet_max,       24.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] SHOULDERS. A reaching arm carries the collar bone toward the hand: this fraction of the
// collar->hand swing, capped at vr_ik_shoulder_max degrees. 0 = collar stays at bind.
CVAR(Float, vr_ik_shoulder_follow, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_ik_shoulder_max,    25.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] TWIST. Wrist roll is spread down the forearm's twist bones (and upper-arm roll down the
// upper arm's) by their position along the bone. 1 = full distribution, 0 = off.
CVAR(Float, vr_ik_twist,           1.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] How fast the body may turn to catch up with the head (degrees per second). 0 = instant (the
// original's snap).
CVAR(Float, vr_body_facing_rate, 120.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] Where the body's heading comes from: 0 = the head, with the dead zone; 1 = the hands (the
// direction from the headset to the midpoint of the two controllers, FRIK's method), which barely
// reacts to looking around. Falls back to the head when a controller is not tracked.
CVAR(Int,   vr_body_facing_mode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] EXTRA palm rotation applied to the LEFT / OFF hand ONLY (side 1), on top of the shared
// vr_ik_hand_pitch/yaw/roll. The left hand's bind palm is a MIRROR of the right, so the offset that
// aligns the right hand is ~180 off for the left. Dial these (try vr_ik_offhand_roll 180 first) until
// the offhand palm matches the right; 0,0,0 = same as the main hand.
CVAR(Float, vr_ik_offhand_pitch, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_ik_offhand_yaw,   0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_ik_offhand_roll,  0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] mesh-facing correction the RENDERER adds to the VR body (r_data/models.cpp, default 90).
// VR_UpdateArmIK must un-yaw the hand target by the SAME total yaw the renderer draws the body at
// (vr_body_facing_yaw + vr_body_yaw + SpriteRotation) so the IK frame == render frame.
EXTERN_CVAR(Float, vr_body_yaw)

// [XR] Body-fit scale used by the RENDERER (r_data/models.cpp) to shrink the local
// player's avatar about its feet (the mesh origin == actorPos). The IK solves in the UNSCALED
// baseframe, so the world hand target -- captured in that same rendered/scaled body space --
// has to be divided by this scale (about the feet pivot) to land in baseframe units. The
// live render autofit scale is exposed via g_xr_vrBodyRenderScale (models.cpp); vr_body_size
// is the manual/fallback. The [VRIK_TGT2] probe below reveals any residual gap.
EXTERN_CVAR(Float, vr_body_size)
EXTERN_CVAR(Bool,  vr_body_autofit)
EXTERN_CVAR(Float, vr_body_neck_eye_gap)   // models.cpp: eyes above the neck stump, rig units

//==========================================================================
//
// The actor the renderer draws as the local VR body and the IK drives.
//
//==========================================================================

AActor* VR_BodyActor(player_t* player)
{
	if (player == nullptr) return nullptr;
	AActor* body = player->vr_body_actor;
	if (body != nullptr && !(body->ObjectFlags & OF_EuthanizeMe)) return body;
	return player->mo;
}

//==========================================================================
//
// VR_EnsureAvatarModelDataAndGetModel (was p_actionfunctions.cpp)
//
//==========================================================================

FModel* VR_EnsureAvatarModelDataAndGetModel(AActor* mo)
{
	if (mo == nullptr) return nullptr;
	P_EnsureActorModelData(mo);   // allocates an empty modelData if the pawn has none (the pose target)
	FSpriteModelFrame* smf = FindModelFrame(mo, mo->sprite, mo->frame, false);   // [5.0] four-argument form
	FModel* result = nullptr;
	if (smf != nullptr)
	{
		for (unsigned i = 0; i < smf->modelIDs.Size(); i++)
		{
			int id = smf->modelIDs[i];
			if (id < 0 || (unsigned)id >= Models.Size()) continue;
			FModel* m = Models[id];
			if (m != nullptr && m->GetLoadState() == FModel::READY && m->GetJointCount() > 0) { result = m; break; }
		}
	}
	static int s_avmdDbg = 0;
	if (s_avmdDbg < 20)
	{
		s_avmdDbg++;
		Printf("[VRIK_MODEL] sprite=%d frame=%d smf=%p result=%p", (int)mo->sprite, (int)mo->frame, smf, result);
		if (smf != nullptr)
		{
			Printf(" nIDs=%u", smf->modelIDs.Size());
			for (unsigned i = 0; i < smf->modelIDs.Size(); i++)
			{
				int id = smf->modelIDs[i];
				Printf(" [%u]id=%d", i, id);
				if (id >= 0 && (unsigned)id < Models.Size())
				{
					FModel* m = Models[id];
					Printf(" state=%d joints=%d", m ? (int)m->GetLoadState() : -1, m ? m->GetJointCount() : -1);
				}
			}
		}
		Printf("\n");
	}
	return result;
}

//==========================================================================
//
// VR_UpdateArmIK -- native two-bone shoulder/elbow IK helpers
//
// VSMatrix (common/utility/matrix.h) exposes no translation/rotation accessors -- only
// get() returning the raw column-major float[16] (VSMatrix::translate() writes indices
// 12/13/14, matrix.cpp:142-144; index = col*4+row throughout, confirmed against
// VSMatrix::multQuaternion's own forward construction at matrix.cpp:101-114). The two
// helpers below pull the translation column and reconstruct the rotation quaternion
// straight out of that same layout -- there is nothing else in this codebase to call.
//
//==========================================================================

static FVector3 IK_MatTranslation(const VSMatrix& m)
{
	const FLOATTYPE* d = m.get();
	return FVector3((float)d[12], (float)d[13], (float)d[14]);
}

// Standard trace-based (Shepperd) matrix->quaternion extraction. This is the exact closed-
// form inverse of VSMatrix::multQuaternion's own forward construction (matrix.cpp:101-114):
// that code writes element(row,col) = mMatrix[col*4+row] as the textbook active-rotation
// matrix for quaternion (X,Y,Z,W); what follows is the standard inverse of that matrix, not
// a guessed convention. CALIBRATE: assumes every ancestor joint's bind Scale is ~(1,1,1) --
// baseframe[] bakes each ancestor's scale into the 3x3 submatrix multiplicatively as it
// accumulates (models_iqm.cpp:166-167), so a non-uniform ancestor scale would skew this
// extraction. The column-normalize below is a cheap defensive guard against that, not a
// real fix -- a real fix would need Gram-Schmidt re-orthonormalization, not worth it unless
// a render test shows skewed elbows on a rig that actually uses non-uniform bone scale.
static FQuaternion IK_MatRotation(const VSMatrix& m)
{
	const FLOATTYPE* d = m.get();

	FVector3 c0((float)d[0], (float)d[1], (float)d[2]);
	FVector3 c1((float)d[4], (float)d[5], (float)d[6]);
	FVector3 c2((float)d[8], (float)d[9], (float)d[10]);
	if (c0.LengthSquared() > 1.e-12f) c0.MakeUnit();
	if (c1.LengthSquared() > 1.e-12f) c1.MakeUnit();
	if (c2.LengthSquared() > 1.e-12f) c2.MakeUnit();

	float m00 = c0.X, m10 = c0.Y, m20 = c0.Z;
	float m01 = c1.X, m11 = c1.Y, m21 = c1.Z;
	float m02 = c2.X, m12 = c2.Y, m22 = c2.Z;

	float trace = m00 + m11 + m22;
	FQuaternion q(0.f, 0.f, 0.f, 1.f);
	if (trace > 0.f)
	{
		float s = sqrtf(trace + 1.f) * 2.f;
		q.W = 0.25f * s;
		q.X = (m21 - m12) / s;
		q.Y = (m02 - m20) / s;
		q.Z = (m10 - m01) / s;
	}
	else if (m00 > m11 && m00 > m22)
	{
		float s = sqrtf(1.f + m00 - m11 - m22) * 2.f;
		q.W = (m21 - m12) / s;
		q.X = 0.25f * s;
		q.Y = (m01 + m10) / s;
		q.Z = (m02 + m20) / s;
	}
	else if (m11 > m22)
	{
		float s = sqrtf(1.f + m11 - m00 - m22) * 2.f;
		q.W = (m02 - m20) / s;
		q.X = (m01 + m10) / s;
		q.Y = 0.25f * s;
		q.Z = (m12 + m21) / s;
	}
	else
	{
		float s = sqrtf(1.f + m22 - m00 - m11) * 2.f;
		q.W = (m10 - m01) / s;
		q.X = (m02 + m20) / s;
		q.Y = (m12 + m21) / s;
		q.Z = 0.25f * s;
	}
	q.MakeUnit();
	return q;
}

// Shortest-arc quaternion rotating unit vector a onto unit vector b. Same construction as
// the proven ZScript prototype (QuatFromTo, wadsrc/static/zscript/actors/doom/vr_whip.zs:
// 361-373), ported to FQuaternion/FVector3.
static FQuaternion IK_QuatFromTo(FVector3 a, FVector3 b)
{
	FVector3 axis = a ^ b;
	float al = (float)axis.Length();
	float d = (float)(a | b);
	if (al < 0.0001f)
	{
		if (d >= 0.f) return FQuaternion(0.f, 0.f, 0.f, 1.f); // identical -> identity
		// Opposite -> a 180 degree flip. Axis just needs to be perpendicular to a.
		FVector3 fallback = (fabsf(a.X) < fabsf(a.Y)) ? FVector3(1.f, 0.f, 0.f) : FVector3(0.f, 1.f, 0.f);
		FVector3 perp = a ^ fallback;
		if (perp.LengthSquared() < 1.e-8f) perp = a ^ FVector3(0.f, 0.f, 1.f);
		perp.MakeUnit();
		return FQuaternion::AxisAngle(perp, FAngle::fromDeg(180.0));
	}
	axis /= al;
	return FQuaternion::AxisAngle(axis, FAngle::fromRad(atan2f(al, d)));
}

// world -> model-local (baseframe/raw-joint) space -- see the COORDINATE FRAME note on
// VR_UpdateArmIK below. Two steps: (1) undo the actor's yaw in the Doom-world XY plane
// (Z passes through as "world up" for now), then (2) remap that Z-up actor-relative
// frame onto the model's raw joint-local axes, which this rig's IQM export uses Y as
// "up" for (see CalculateBonesIQM's own unconditional swapYZ sandwich, models_iqm.cpp
// ~line 700-761 -- it exists specifically because baseframe/inversebaseframe are NOT
// already in the same up-convention as the final Z-up render space). That swap is a
// literal (x,y,z)->(x,z,y) relabel, so step 2 here is the same relabel: local Y takes
// the world-up component, local Z takes the lateral (yaw-corrected off.Y) component.
// Local X (forward, from cosInvYaw/sinInvYaw) is untouched -- the swap never touches X.
// [XR] [[maybe_unused]]: the position path now inverts the renderer's objectToWorldMatrix directly
// (VR_UpdateArmIK), so this hand-rebuilt world->model-local helper is retained only as the documented
// reference for IK_ControllerModelRot's shared un-yaw algebra. Kept to avoid an unused-static warning.
[[maybe_unused]] static FVector3 IK_WorldToModelLocal(const DVector3& worldPos, const DVector3& actorPos, double cosInvYaw, double sinInvYaw)
{
	DVector3 off = worldPos - actorPos;
	double lx = off.X * cosInvYaw - off.Y * sinInvYaw; // forward
	double ly = off.X * sinInvYaw + off.Y * cosInvYaw; // lateral (Doom-world sense)
	// Y<->Z relabel into raw joint-local (Y-up) space: local Y = world up, local Z = lateral.
	return FVector3((float)lx, (float)off.Z, (float)ly);
}

// [XR] ROTATION analog of the POSITION path: pull the CONTROLLER's orientation out of the same
// GetWeaponTransform VSMatrix the position path reads, and land it in the EXACT baseframe the
// two-bone solve works in (the frame targetLocal[] lives in), so the wrist can be driven
// parent-relative off solve.lowerWorldRot.
//
// This now uses the SAME exact inverse the position path uses (Finv = swapYZ * objectToWorld^-1),
// applied to the controller's basis vectors as DIRECTIONS (w=0), so the wrist frame is consistent
// with the hand POSITION by construction -- NOT the old hand-rebuilt un-yaw/(Z,X,Y)-relabel/
// forward-flip approximation (which only un-yawed the pawn yaw, ignored the Y/Z-swapped bodyScale
// and the full drawn yaw baked into objectToWorldMatrix, and used an ad-hoc forward sign instead
// of swapYZ -> a frame mismatch that inverted the wrist). Steps, each mirroring the point path:
//   1. Read controller forward/up in raw GL layout (NO Doom remap): GL-forward = -colZ =
//      (-m[8],-m[9],-m[10]); GL-up = +colY = (m[4],m[5],m[6]) -- the same GL columns ptGL reads.
//   2. Transform each as a DIRECTION (w=0) through Finv (linear 3x3 only) -> baseframe space,
//      identically to how the point path lands the position; uniform bodyScale cancels on normalize.
//   3. Re-orthonormalize (fwd,up -> right = up^fwd, up = fwd^right) to strip any non-uniform
//      Y/Z-swapped-scale skew that Finv's non-orthonormal 3x3 leaves.
//   4. Load into a VSMatrix's rotation columns using the SAME column layout IK_MatRotation reads
//      (colX=[0..2], colY=[4..6], colZ=[8..10]) with GL-forward == -colZ, and extract the
//      quaternion with the proven IK_MatRotation. The returned quat is in the solve's baseframe
//      == solve.lowerWorldRot's frame, so localHand = lowerWorldRot^-1 * (ctrlModelRot*palmOffset)
//      is a valid same-frame composition and the wrist tracks correctly.
static bool IK_ControllerModelRot(const VSMatrix& handXf, VSMatrix& Finv, FQuaternion& outRot)
{
	const FLOATTYPE* m = handXf.get();
	// (1) controller basis in the SAME raw GL layout the POSITION path reads (do NOT pre-remap to
	// Doom): GL-forward = -colZ = (-m[8],-m[9],-m[10]); GL-up = +colY = (m[4],m[5],m[6]). These are
	// the object->world (GL) axes objectToWorldMatrix^-1 expects, mirroring ptGL={m[12],m[13],m[14]}.
	FLOATTYPE fwdGL[4] = { -m[8], -m[9], -m[10], (FLOATTYPE)0 }; // -colZ == controller forward, as a DIRECTION (w=0)
	FLOATTYPE upGL [4] = {  m[4],  m[5],  m[6],  (FLOATTYPE)0 }; // +colY == controller up,      as a DIRECTION (w=0)

	// (2) transform both basis vectors as DIRECTIONS through the EXACT Finv (= swapYZ * objectToWorld^-1),
	// identically to how the point path lands the POSITION -- so the wrist frame == the target frame.
	// w=0 applies only Finv's linear 3x3 part (multMatrixPoint = column-major M*vec, matrix.cpp:357-367);
	// uniform bodyScale cancels on the normalize below.
	FLOATTYPE fwdB4[4], upB4[4];
	Finv.multMatrixPoint(fwdGL, fwdB4);
	Finv.multMatrixPoint(upGL,  upB4);
	FVector3 f((float)fwdB4[0], (float)fwdB4[1], (float)fwdB4[2]); // baseframe forward
	FVector3 u((float)upB4[0],  (float)upB4[1],  (float)upB4[2]);  // baseframe up
	if (f.LengthSquared() < 1.e-8f || u.LengthSquared() < 1.e-8f) return false;
	f.MakeUnit();

	// (3) Finv's 3x3 is NON-orthonormal (Y/Z-swapped, possibly non-uniform bodyScale), so the transformed
	// fwd/up are not guaranteed orthogonal -> re-orthonormalize off forward to strip the skew.
	// HANDEDNESS: Finv contains swapYZ (det -1) so its linear part is ORIENTATION-REVERSING. A textbook
	// right-handed cross order (right = up^fwd) on Finv's outputs yields a LEFT-handed basis (det -1) --
	// a MIRROR that IK_MatRotation silently collapses to the nearest rotation and that NO palmOffset can
	// correct (a reflection is unreachable by any rotation), which is what left the wrist mirrored/inverted.
	// Reverse the cross order (right = fwd^up, up = right^fwd) to compensate for the det(-1) map so the
	// reconstructed basis is a PROPER rotation (verified det +1 across 200 random controller poses).
	FVector3 right = f ^ u;
	if (right.LengthSquared() < 1.e-8f) return false; // fwd/up parallel -> degenerate, keep bind
	right.MakeUnit();
	u = right ^ f;      // re-orthonormalize up (right-handed given the reversed cross above)
	u.MakeUnit();

	// (4) orthonormal basis + matrix -> quat. GL-forward == -colZ, so colZ = -f (matches IK_MatRotation's read).
	FVector3 colZ = -f;
	VSMatrix rm;
	rm.loadIdentity();
	FLOATTYPE* d = const_cast<FLOATTYPE*>(rm.get());   // [5.0] FLOATTYPE, not float
	d[0] = right.X; d[1] = right.Y; d[2]  = right.Z; // colX
	d[4] = u.X;     d[5] = u.Y;     d[6]  = u.Z;     // colY
	d[8] = colZ.X;  d[9] = colZ.Y;  d[10] = colZ.Z;  // colZ
	outRot = IK_MatRotation(rm);
	return true;
}

// [XR] The ROTATION of another hand model, brought into the body's baseframe. ax/ay/az are the
// GL-world directions of that model's +X/+Y/+Z axes as the renderer draws them (the object
// matrix columns). The renderer draws a model in Y-up space (file vertices go through swapYZ),
// so the model's file +Y is its drawn +Z and vice versa; expressed in the body's file-space
// baseframe that makes colX = S*ax, colY = S*az, colZ = S*ay with S = Finv's linear part. A
// model drawn with a negative scale (a mirrored hand) comes out left-handed; flipping colX
// recovers the rotation, which is the convention the alignment constants are computed in.
static bool IK_HandModelRot(const FVector3& ax, const FVector3& ay, const FVector3& az, VSMatrix& Finv, FQuaternion& outRot)
{
	FLOATTYPE in[3][4] = { { ax.X, ax.Y, ax.Z, 0 }, { az.X, az.Y, az.Z, 0 }, { ay.X, ay.Y, ay.Z, 0 } };
	FVector3 col[3];
	for (int i = 0; i < 3; i++)
	{
		FLOATTYPE o[4];
		Finv.multMatrixPoint(in[i], o);
		col[i] = FVector3((float)o[0], (float)o[1], (float)o[2]);
		if (col[i].LengthSquared() < 1.e-10f) return false;
		col[i].MakeUnit();
	}
	const float det = col[0] | (col[1] ^ col[2]);
	if (det < 0.f) col[0] = -col[0];
	// light re-orthonormalisation off X, then Y
	col[1] = col[1] - col[0] * (col[0] | col[1]);
	if (col[1].LengthSquared() < 1.e-10f) return false;
	col[1].MakeUnit();
	col[2] = col[0] ^ col[1];
	VSMatrix rm; rm.loadIdentity();
	FLOATTYPE* d = const_cast<FLOATTYPE*>(rm.get());
	d[0] = col[0].X; d[1] = col[0].Y; d[2]  = col[0].Z;
	d[4] = col[1].X; d[5] = col[1].Y; d[6]  = col[1].Z;
	d[8] = col[2].X; d[9] = col[2].Y; d[10] = col[2].Z;
	outRot = IK_MatRotation(rm);
	return true;
}

// [XR] Forward kinematics over the CURRENT pose: the model-space (baseframe-space) transform of a
// joint given the per-joint local TRS in `pose`. Walks to the root; joints in a chain are few.
static void IK_PoseWorld(FModel* model, const TArray<TRS>& pose, int joint, VSMatrix& out)
{
	int chain[128]; int n = 0;
	for (int j = joint; j >= 0 && n < 128; j = model->GetJointParent(j)) chain[n++] = j;
	out.loadIdentity();
	for (int k = n - 1; k >= 0; k--)
	{
		const TRS& t = pose[chain[k]];
		out.translate(t.translation.X, t.translation.Y, t.translation.Z);
		out.multQuaternion(t.rotation);
		out.scale(t.scaling.X, t.scaling.Y, t.scaling.Z);
	}
}

// The twist of q about unit axis a (swing-twist decomposition), as a unit quaternion.
static FQuaternion IK_TwistAbout(const FQuaternion& q, const FVector3& a)
{
	const float d = q.X * a.X + q.Y * a.Y + q.Z * a.Z;
	FQuaternion t(a.X * d, a.Y * d, a.Z * d, q.W);
	if (t.LengthSquared() < 1.e-8f) return FQuaternion(0.f, 0.f, 0.f, 1.f);
	t.MakeUnit();
	if (t.W < 0.f) { t.X = -t.X; t.Y = -t.Y; t.Z = -t.Z; t.W = -t.W; }
	return t;
}

// A fraction of a rotation: slerp from identity.
static FQuaternion IK_Fraction(const FQuaternion& q, float k)
{
	FQuaternion r = FQuaternion::SLerp(FQuaternion(0.f, 0.f, 0.f, 1.f), q, clamp(k, 0.f, 1.f));
	r.MakeUnit();
	return r;
}

// Two-bone (shoulder/elbow) IK solve for one arm, entirely in the model's own local/rest
// space. Returns the FULL desired model-space rotation for the upper-arm and lower-arm
// joints (i.e. the rotation component baseframe[joint] would have had, had the model been
// authored in this new pose) -- VR_UpdateArmIK still converts these into the LOCAL
// parent-relative rotation the engine actually wants.
struct FArmIKSolve
{
	FQuaternion upperWorldRot;
	FQuaternion lowerWorldRot;
	float stretch = 1.0f;  // [XR] >1 => arm stretched to reach a target beyond its natural span (written onto the upperArm bone scale)
};

static bool IK_SolveTwoBoneArm(
	const FVector3& shoulderPos, const FVector3& elbowBindPos, const FVector3& handBindPos,
	const FVector3& targetPos, const FVector3& poleDir,
	const FQuaternion& bindUpperWorldRot, const FQuaternion& bindLowerWorldRot,
	float upperLen, float forearmLen,
	FArmIKSolve& out)
{
	if (upperLen < 0.01f || forearmLen < 0.01f) return false;

	FVector3 bindUpperDir = elbowBindPos - shoulderPos;
	FVector3 bindLowerDir = handBindPos - elbowBindPos;
	if (bindUpperDir.LengthSquared() < 1.e-8f || bindLowerDir.LengthSquared() < 1.e-8f) return false;
	bindUpperDir.MakeUnit();
	bindLowerDir.MakeUnit();

	FVector3 toTarget = targetPos - shoulderPos;
	float rawReach = (float)toTarget.Length();
	if (rawReach < 0.0001f) return false; // target sits on the shoulder -- no aim direction
	FVector3 aimDir = toTarget / rawReach;

	// [XR] STRETCHY REACH: if the controller sits farther than the arm's natural span, scale BOTH bones so
	// the arm spans exactly rawReach -- the hand then lands ON the controller regardless of the marine's
	// (short) arm-to-height proportion. upperLen/forearmLen are by-value params, so overwriting them here
	// feeds the stretched lengths through the whole solve; out.stretch is written onto the upperArm bone's
	// pose scale in VR_UpdateArmIK so the MESH follows. == 1.0 (no change) whenever the target is in reach.
	{
		const float naturalArm = upperLen + forearmLen;
		out.stretch = (naturalArm > 0.01f && rawReach > naturalArm) ? (rawReach / naturalArm) : 1.0f;
		upperLen   *= out.stretch;
		forearmLen *= out.stretch;
	}

	// Reach-clamp: solve as if the target were at the nearest point still reachable by this
	// bone pair, along the SAME direction, instead of feeding law-of-cosines an out-of-domain
	// acos() argument when the real hand distance exceeds (or undershoots) what the arm can
	// physically span.
	float maxReach = (upperLen + forearmLen) * 0.999f;
	float minReach = fabsf(upperLen - forearmLen) * 1.001f + 0.01f;
	float reach = clamp(rawReach, minReach, maxReach);

	// Law of cosines: angle at the shoulder between aimDir (shoulder->target) and the
	// solved upper-arm direction.
	float cosShoulder = (upperLen * upperLen + reach * reach - forearmLen * forearmLen) / (2.f * upperLen * reach);
	cosShoulder = clamp(cosShoulder, -1.f, 1.f);
	float shoulderAngle = acosf(cosShoulder); // radians

	// Pole vector: the plane the elbow bends in. Project out the aimDir component so what's
	// left is purely perpendicular to the shoulder->target line ("which way the elbow points").
	FVector3 poleProj = poleDir - aimDir * (float)(poleDir | aimDir);
	float poleLen = (float)poleProj.Length();
	if (poleLen < 0.0001f)
	{
		poleProj = aimDir ^ FVector3(0.f, 0.f, 1.f);
		poleLen = (float)poleProj.Length();
		if (poleLen < 0.0001f)
		{
			poleProj = FVector3(1.f, 0.f, 0.f);
			poleLen = 1.f;
		}
	}
	poleProj /= poleLen;

	FVector3 rotAxis = aimDir ^ poleProj;
	float axisLen = (float)rotAxis.Length();
	if (axisLen < 0.0001f) return false; // aimDir parallel to the pole plane normal -- shouldn't happen post-projection
	rotAxis /= axisLen;

	FQuaternion shoulderSwing = FQuaternion::AxisAngle(rotAxis, FAngle::fromRad(shoulderAngle));
	FVector3 upperDirSolved = shoulderSwing * aimDir;
	upperDirSolved.MakeUnit();

	FVector3 elbowPos = shoulderPos + upperDirSolved * upperLen;
	FVector3 lowerDirSolved = targetPos - elbowPos;
	float lowerLen = (float)lowerDirSolved.Length();
	if (lowerLen > 0.0001f) lowerDirSolved /= lowerLen;
	else lowerDirSolved = bindLowerDir; // degenerate -- keep the bind direction rather than NaN

	FQuaternion deltaUpper = IK_QuatFromTo(bindUpperDir, upperDirSolved);
	FQuaternion deltaLower = IK_QuatFromTo(bindLowerDir, lowerDirSolved);

	out.upperWorldRot = deltaUpper * bindUpperWorldRot;
	out.lowerWorldRot = deltaLower * bindLowerWorldRot;
	out.upperWorldRot.MakeUnit();
	out.lowerWorldRot.MakeUnit();
	return true;
}

//==========================================================================
//
// [XR] Rig table lookups. A role ("upperarm_r", "index_1_l", "neck", ...) resolves through the
// mod-supplied table on player_t first; -1 when the role is not in the table or the joint is not
// on this model. Callers fall back to the built-in marine names after this.
//
//==========================================================================

static int VR_RoleJoint(player_t* player, FModel* model, const char* role)
{
	if (player == nullptr || model == nullptr) return -1;
	FName* bone = player->vr_body_bone_roles.CheckKey(FName(role));
	if (bone == nullptr || *bone == NAME_None) return -1;
	return model->FindJointByNameCI(*bone);
}

// Every joint below `root` (children, grandchildren, ...), root excluded. Used to find a hand's
// finger joints without naming them: everything that hangs off the hand joint poses with it.
static void VR_CollectDescendants(FModel* model, int root, TArray<int>& out)
{
	out.Clear();
	if (model == nullptr || root < 0) return;
	const int n = model->GetJointCount();
	for (int j = 0; j < n; j++)
	{
		for (int p = model->GetJointParent(j); p >= 0; p = model->GetJointParent(p))
		{
			if (p == root) { out.Push(j); break; }
		}
	}
}

//==========================================================================
//
// VR_UpdateArmIK
//
// Native two-bone shoulder/elbow IK for the avatar's IQM arm joints. Writes ONLY
// player->vr_ik_pose (one parent-local TRS per skeleton joint: bind values for every
// joint except the 4 solved arm joints, which get a new ROTATION only -- translate/
// scale are never touched, bones don't stretch). A separate glue point (not this
// function, per the design brief) copies vr_ik_pose into player->mo->modelData->
// proceduralPose and flips useProceduralPose for the render path (see
// AActor::SetModelBonePose/SetModelUseProceduralPose, scripting/vmthunks_actors.cpp).
//
// STRUCTURAL TEMPLATE: VR_UpdateGravityGloves (DXR p_user.cpp) for the guard
// block shape, and VR_UpdateHardpoints for the local-player-only gate rationale --
// GetWeaponTransform reads the single LOCAL OpenXR device with no player parameter,
// so driving ANY playsim-visible state off it for a non-console player_t on this
// machine is meaningless local-headset data misattributed to someone else.
// vr_ik_pose itself is also explicitly excluded from FSerializer/net (see the
// TRANSIENT/CLIENT-PRESENTATION-ONLY note in d_player.h).
//
// COORDINATE FRAME: FModel::GetBasePose() (model.h, overridden model_iqm.h) hands back
// baseframe -- a plain top-down accumulation of each joint's own RAW local bind TRS
// (models_iqm.cpp, the joint-read loop just above CalculateBonesIQM), i.e. MODEL-local/
// rest space with the actor's transform NOT applied and NO swapYZ baked in. The
// world-space hand targets from GetWeaponTransform must be converted into that SAME
// space in two steps: (1) subtract the actor's world position and undo the actor's yaw
// with a plain 2D rotation in the Doom-world XY plane, then (2) relabel the result onto
// the model's own raw joint-local axes via a Y<->Z swap (IK_WorldToModelLocal does both).
// That second step is required, NOT optional: CalculateBonesIQM (models_iqm.cpp)
// sandwiches baseframe/inversebaseframe between an UNCONDITIONAL swapYZ
// on every joint (root and child alike) specifically because raw joint-local space is
// NOT already in the same up-convention as the final Z-up render/world space -- this
// project's own prior verified finding is that this IQM rig's bone TRS is Y-up, matching
// the swapYZ evidence exactly. So: local Y (not local Z) carries "world up", and local Z
// carries the lateral component; local X (forward) is untouched by the relabel, since
// swapYZ never touches the X row/column. The pole-vector down/back constants below are
// expressed in this SAME post-swap local frame. CALIBRATE (the one thing that still
// needs an in-headset render test, not provable from source alone): this Y<->Z relabel
// direction -- if elbows splay sideways/forward instead of down-and-back on a real
// render, the fix is swapping which of (off.Z, ly) lands on local Y vs Z in
// IK_WorldToModelLocal, and mirroring that in downLocal/backLocal below. The SIGN of the
// yaw un-rotation itself is lower-risk and well-grounded: Angles.Yaw.ToVector() = (Cos,
// Sin) (vectors.h), the same forward convention already used for real gameplay traces
// (p_map.cpp UseRange trace, p_switch.cpp dlu.dx/dlu.dy), and algebraic substitution
// confirms invYaw=-Yaw maps world-forward onto local +X here -- still worth eyeballing
// in the same render test, but not the primary suspect if arms look wrong.
//
//==========================================================================

void VR_UpdateArmIKFrame(player_t* player, uint64_t frameStamp)
{
	static uint64_t s_lastFrame = ~0ull;
	if (frameStamp == s_lastFrame) return;   // once per frame, not once per eye
	s_lastFrame = frameStamp;
	VR_UpdateArmIK(player);
}

void VR_UpdateArmIK(player_t* player)
{
	if (!player || !player->mo) return;

	// [5.0] The body being posed: player->vr_body_actor when a mod designated one, else the pawn --
	// which is exactly what every player->mo below used to be. `body` replaces player->mo ONLY where
	// the original meant "the avatar" (its modelData, its drawn yaw); the console-player gate stays on
	// the pawn.
	AActor* body = VR_BodyActor(player);
	if (body == nullptr) return;

	{ static int s_ikEntry=0; if (s_ikEntry++<20) Printf("[VRIK_ENTRY] sprite=%d frame=%d vr_arm_ik=%d vr_ik_enabled=%d\n", (int)body->sprite, (int)body->frame, (int)vr_arm_ik, (int)player->vr_ik_enabled); }
	// [XR] Uncapped, once a second: the gate state and the body, so a run that never solves still says why.
	{
		static int s_gate = 0;
		if ((s_gate++ % 35) == 0)
		{
			const VRMode* vm = VRMode::GetVRModeCached(true);
			Printf("[VRIK_GATE] enable=%d enabled=%d console=%d isvr=%d body=%p md=%p valid=%d\n",
				(int)vr_arm_ik, (int)player->vr_ik_enabled, (int)(player == player->mo->Level->GetConsolePlayer()),
				vm ? (int)vm->IsVR() : -1, body, body->modelData.ForceGet(), (int)g_xr_vrBodyObjToWorldValid);
		}
	}

	// LOCAL PLAYER ONLY -- see the rationale block above.
	if (player != player->mo->Level->GetConsolePlayer())
	{
		{ static int s_gB=0; if (s_gB++<20) Printf("[VRIK_BAIL_B] player=%p console=%p\n", player, player->mo->Level->GetConsolePlayer()); }
		player->vr_ik_active = false;
		return;
	}

	if (!VRMode::GetVRModeCached(false))
	{
		{ static int s_gC=0; if (s_gC++<20) Printf("[VRIK_BAIL_C]\n"); }
		player->vr_ik_active = false;
		return;
	}

	// Two independent gates, per the design brief: the global feature cvar, and the
	// per-player runtime flag toggled by AActor::SetArmIKEnabled (vmthunks_actors.cpp).
	if (!vr_arm_ik || !player->vr_ik_enabled)
	{
		{ static int s_gD=0; if (s_gD++<20) Printf("[VRIK_BAIL_D] enable=%d enabled=%d\n", (int)vr_arm_ik, (int)player->vr_ik_enabled); }
		player->vr_ik_active = false;
		return;
	}

	// ---- locate the avatar's loaded IQM model ----
	// Scan every entry in modelData->models rather than assuming index 0 is the body --
	// GetJointCount() > 0 is itself the "this is a loaded IQM with a skeleton" signal, so
	// there is no need to RTTI/dynamic_cast to IQMModel at all (see model.h's GetJointCount
	// base-class comment).
	// The marine avatar is bound via its STATIC modeldef, so a plain player pawn never gets a
	// DActorModelData -- and the IK needs one to write the solved pose into (the renderer reads
	// modelData->proceduralPose). This creates it (models list left EMPTY -> render still uses the
	// static modeldef, body unchanged) and returns the rigged avatar model from that same modeldef.
	// THE FIX for "arms never move": before this, modelData was null every tic and the IK bailed here.
	FModel* model = VR_EnsureAvatarModelDataAndGetModel(body);
	DActorModelData* modelData = body->modelData;
	if (model == nullptr || modelData == nullptr)
	{
		player->vr_ik_active = false;
		return;
	}

	const TArray<VSMatrix>* baseframePtr = model->GetBasePose();
	if (baseframePtr == nullptr || baseframePtr->Size() == 0)
	{
		{ static int s_gF=0; if (s_gF++<20) Printf("[VRIK_BAIL_F] bf=%p size=%d\n", baseframePtr, baseframePtr?(int)baseframePtr->Size():-1); }
		player->vr_ik_active = false;
		return;
	}
	const TArray<VSMatrix>& baseframe = *baseframePtr;

	int jointCount = model->GetJointCount();
	if (jointCount <= 0 || (unsigned)jointCount > baseframe.Size())
	{
		{ static int s_gG=0; if (s_gG++<20) Printf("[VRIK_BAIL_G] jointCount=%d bfSize=%u\n", jointCount, (unsigned)baseframe.Size()); }
		player->vr_ik_active = false;
		return;
	}

	// ---- (re)size + fill the bind pose. Resize only on a genuine count mismatch (first
	// solve / model swap) to avoid per-tic heap churn; the fill loop itself runs every tic
	// regardless, since the 2 solved joints per side get overwritten below and everything
	// else must be re-seeded to bind in case a PREVIOUS tic's solve touched them. ----
	if (player->vr_ik_pose.Size() != (unsigned)jointCount)
	{
		player->vr_ik_pose.Resize(jointCount);
	}
	for (int i = 0; i < jointCount; i++)
	{
		model->GetJointBindTRS(i, player->vr_ik_pose[i]);
	}
	bool handPosed[2] = { false, false };   // [XR] set below once the chain is known

	// ---- resolve the arm-chain joint indices: name lookup first, numeric fallback ----
	// Resolved ONCE per loaded model and cached, not redone every tic -- FindJointByName is
	// an O(Joints.Size()) linear scan and FName(const char*) is a global name-table
	// insert-or-find on first use, neither of which needs to run 2*4 times per tic forever
	// for data that cannot change between tics for a given loaded model (mirrors the
	// one-time-cost discipline already used for warnedNameFallback/vr_ik_pose.Resize just
	// above). Re-resolved only when the avatar model itself changes.
	struct ArmChain { int collar, upperArm, lowerArm, hand; };
	static FModel* cachedModel = nullptr;
	static int cachedGen = -1;
	static ArmChain cachedChains[2];
	static bool cachedValid = false;
	static TArray<int> cachedHandKids[2];   // [XR] every joint below the hand joint: the fingers, whatever they are called
	static TArray<int> cachedHidden;        // [XR] joints the mod asked to collapse (head, from inside)
	static int cachedNeck = -1;             // [XR] "neck" role joint, for the body fit
	static TArray<int> cachedSpine;         // [XR] spine_0.. (root to top), for the lean
	static int cachedHips = -1;             // [XR] "hips" role, for the crouch
	static int cachedLegs[2][3] = { { -1, -1, -1 }, { -1, -1, -1 } };   // [XR] hip/knee/foot per side
	static TArray<int> cachedUpperTwist[2], cachedLowerTwist[2];         // [XR] twist helpers per arm bone

	if (model != cachedModel || player->vr_body_rig_gen != cachedGen)
	{
		static const char* const names[2][4] =
		{
			{ "bip_collar_R", "bip_upperArm_R", "bip_lowerArm_R", "bip_hand_R" },
			{ "bip_collar_L", "bip_upperArm_L", "bip_lowerArm_L", "bip_hand_L" }
		};
		static const int fallbackIdx[2][4] =
		{
			{ 22, 25, 29, 37 }, // right: collar, upperArm, lowerArm, hand
			{ 24, 27, 33, 42 }  // left:  collar, upperArm, lowerArm, hand
		};

		// [XR] The mod's rig table comes first: all four roles of a side, or none of them.
		static const char* const roles[2][4] =
		{
			{ "collar_r", "upperarm_r", "lowerarm_r", "hand_r" },
			{ "collar_l", "upperarm_l", "lowerarm_l", "hand_l" }
		};

		ArmChain resolved[2];
		bool usedFallback = false;
		for (int side = 0; side < 2; side++)
		{
			// All-or-nothing PER SIDE: never mix a name-resolved index with a hardcoded
			// fallback index on the same chain. A partially-matching rig (e.g. has
			// "bip_collar_R" but not "bip_upperArm_R") would otherwise silently splice in
			// the marine's hardcoded index for an unrelated joint on an unrelated model --
			// it would pass the later bounds check clean (still just an in-range index on
			// THIS model) and run the solve on the wrong bone with no diagnostic.
			int found[4];
			bool sideRoleOk = true;
			for (int j = 0; j < 4; j++)
			{
				found[j] = VR_RoleJoint(player, model, roles[side][j]);
				if (found[j] < 0) sideRoleOk = false;
			}
			bool sideNameOk = sideRoleOk;
			if (!sideRoleOk)
			{
				sideNameOk = true;
				for (int j = 0; j < 4; j++)
				{
					found[j] = model->FindJointByName(FName(names[side][j]));
					if (found[j] < 0) sideNameOk = false;
				}
			}
			int* dst = &resolved[side].collar;
			for (int j = 0; j < 4; j++)
			{
				dst[j] = sideNameOk ? found[j] : fallbackIdx[side][j];
			}
			if (!sideNameOk) usedFallback = true;
		}

		static bool warnedNameFallback = false;
		if (usedFallback && !warnedNameFallback)
		{
			Printf("VR_UpdateArmIK: joint name lookup failed for one or more arm bones -- "
				"falling back to hardcoded marine joint indices (specific to that rig).\n");
			warnedNameFallback = true;
		}

		// Bounds-validate every resolved index against THIS model's joint count -- the
		// hardcoded fallback indices are specific to the marine's rig and would be garbage
		// on a different avatar model that still happens to pass the IQM/joint-count gate
		// above. (Meaningful only for the fallback case: a genuine FindJointByName hit can
		// never be out of range on the same model it was just looked up on.)
		bool boundsOk = true;
		for (int side = 0; side < 2 && boundsOk; side++)
		{
			int idxs[4] = { resolved[side].collar, resolved[side].upperArm, resolved[side].lowerArm, resolved[side].hand };
			for (int j = 0; j < 4; j++)
			{
				if (idxs[j] < 0 || idxs[j] >= jointCount) { boundsOk = false; break; }
			}
		}

		cachedChains[0] = resolved[0];
		cachedChains[1] = resolved[1];
		cachedValid = boundsOk;
		cachedModel = model;
		cachedGen = player->vr_body_rig_gen;

		// [XR] Everything below each hand joint poses with the hand (pose clip seeding below).
		for (int side = 0; side < 2; side++)
			VR_CollectDescendants(model, boundsOk ? resolved[side].hand : -1, cachedHandKids[side]);

		// [XR] Joints the mod wants collapsed (the head, seen from inside).
		cachedHidden.Clear();
		for (unsigned h = 0; h < player->vr_body_hidden_bones.Size(); h++)
		{
			int hj = model->FindJointByNameCI(player->vr_body_hidden_bones[h]);
			if (hj >= 0 && hj < jointCount) cachedHidden.Push(hj);
		}

		// [XR] Neck height from the rig itself, for the renderer's body fit.
		cachedNeck = VR_RoleJoint(player, model, "neck");

		// [XR] Posture joints (all optional): spine root..top, hips, legs; and the twist helpers,
		// which are simply the children of each arm bone that are not the next arm bone.
		cachedSpine.Clear();
		for (int i = 0; i < 12; i++)
		{
			char role[16]; snprintf(role, sizeof role, "spine_%d", i);
			const int sj = VR_RoleJoint(player, model, role);
			if (sj < 0) break;
			cachedSpine.Push(sj);
		}
		cachedHips = VR_RoleJoint(player, model, "hips");
		{
			static const char* const legRoles[2][3] = { { "hip_r", "knee_r", "foot_r" }, { "hip_l", "knee_l", "foot_l" } };
			for (int side = 0; side < 2; side++)
			{
				cachedLegs[side][0] = VR_RoleJoint(player, model, legRoles[side][0]);
				cachedLegs[side][1] = VR_RoleJoint(player, model, legRoles[side][1]);
				cachedLegs[side][2] = VR_RoleJoint(player, model, legRoles[side][2]);
			}
		}
		for (int side = 0; side < 2; side++)
		{
			cachedUpperTwist[side].Clear(); cachedLowerTwist[side].Clear();
			if (!boundsOk) continue;
			for (int j = 0; j < jointCount; j++)
			{
				const int p = model->GetJointParent(j);
				if (p == resolved[side].upperArm && j != resolved[side].lowerArm) cachedUpperTwist[side].Push(j);
				if (p == resolved[side].lowerArm && j != resolved[side].hand)     cachedLowerTwist[side].Push(j);
			}
		}
		Printf("[VRIK_RIG2] spine=%u hips=%d legsR=%d/%d/%d legsL=%d/%d/%d twistU=%u/%u twistL=%u/%u\n",
			cachedSpine.Size(), cachedHips, cachedLegs[0][0], cachedLegs[0][1], cachedLegs[0][2], cachedLegs[1][0], cachedLegs[1][1], cachedLegs[1][2],
			cachedUpperTwist[0].Size(), cachedUpperTwist[1].Size(), cachedLowerTwist[0].Size(), cachedLowerTwist[1].Size());
		Printf("[VRIK_RIG] gen=%d chainR=%d/%d/%d/%d chainL=%d/%d/%d/%d valid=%d handKids=%u/%u hidden=%u neck=%d\n",
			cachedGen, resolved[0].collar, resolved[0].upperArm, resolved[0].lowerArm, resolved[0].hand,
			resolved[1].collar, resolved[1].upperArm, resolved[1].lowerArm, resolved[1].hand, (int)boundsOk,
			cachedHandKids[0].Size(), cachedHandKids[1].Size(), cachedHidden.Size(), cachedNeck);
	}
	player->vr_body_neck_z = (cachedNeck >= 0 && (unsigned)cachedNeck < baseframe.Size()) ? IK_MatTranslation(baseframe[cachedNeck]).Z : 0.f;

	if (!cachedValid)
	{
		{ static int s_gH=0; if (s_gH++<20) Printf("[VRIK_BAIL_H]\n"); }
		player->vr_ik_active = false;
		return;
	}
	ArmChain (&chains)[2] = cachedChains; // reference to the cached (or freshly re-resolved) chain array

	// [XR] HAND POSES FROM THE BODY'S OWN POSE CLIP. When the mod names a pose frame for a hand, every
	// joint below that hand joint takes its TRS from that frame (blended toward the next frame by lerp)
	// instead of the bind pose. The clip is baked frame-for-frame from the RS hands' clip, so the frame
	// the mod shows on its hand model is the frame that poses this body's fingers. The hand joint itself
	// is not touched: the wrist still follows the controller.
	{
		bool rightHanded2 = vr_control_scheme < 10;
		const int numFrames = model->NumFrames();
		for (int side = 0; side < 2; side++)
		{
			const int handEnum = (side == 0) ? (rightHanded2 ? VR_MAINHAND : VR_OFFHAND) : (rightHanded2 ? VR_OFFHAND : VR_MAINHAND);
			int f0 = player->vr_body_hand_pose_frame[handEnum];
			if (f0 < 0 || numFrames <= 0 || cachedHandKids[side].Size() == 0) continue;
			int f1 = player->vr_body_hand_pose_next[handEnum];
			float t = clamp(player->vr_body_hand_pose_lerp[handEnum], 0.f, 1.f);
			f0 = clamp(f0, 0, numFrames - 1);
			f1 = (f1 < 0) ? f0 : clamp(f1, 0, numFrames - 1);
			for (unsigned k = 0; k < cachedHandKids[side].Size(); k++)
			{
				const int j = cachedHandKids[side][k];
				TRS a = model->GetJointPose(j, f0);
				if (f1 != f0 && t > 0.f)
				{
					TRS b = model->GetJointPose(j, f1);
					a.translation = a.translation * (1.f - t) + b.translation * t;
					a.scaling     = a.scaling * (1.f - t) + b.scaling * t;
					a.rotation    = FQuaternion::SLerp(a.rotation, b.rotation, t);
					a.rotation.MakeUnit();
				}
				player->vr_ik_pose[j] = a;
			}
			handPosed[side] = true;
		}
	}

	// ---- world -> model-local space setup for the hand targets (see COORDINATE FRAME above) ----
	// [XR] actorPos (player->mo->Pos()) is no longer needed: the feet-relative subtract it fed
	// IK_WorldToModelLocal is now inverted for free inside objectToWorldMatrix (translate at models.cpp).
	// [XR] UN-YAW BASIS FIX (proven by [VRIK_TGT2] range-of-motion data + vk_openxrdevice.cpp): the
	// hand WORLD position from GetWeaponTransform is placed at world yaw = -90 + doomYaw + controllerRelYaw,
	// where doomYaw == player->mo->Angles.Yaw (the live pawn/HMD yaw). So the target must be un-yawed by the
	// PAWN yaw, NOT the decoupled vr_body_facing_yaw the renderer draws the body at. Un-yawing by the render
	// facing left a residual of (pawnYaw - vr_body_facing_yaw) whenever the head turns inside the 50-deg body
	// dead-zone -- which rotated the horizontal plane (the apparent "90-deg off / sign flip") and inflated
	// reach (worst ~+70% at a ~100-deg head/body gap). Because doomYaw is baked into the hand placement,
	// un-yawing by that SAME doomYaw cancels head rotation cleanly (no HMD leak). The vr_body_yaw(90) /
	// SpriteRotation mesh terms must NOT be added (the -90 baked into the hand placement is their counterpart
	// and already cancels; the baseframe shoulders are authored in the frame that nets to a plain -pawnYaw).
	// Also: vr_body_facing_yaw is updated AFTER this function in P_PlayerThink, so it lagged a tic -- another
	// reason the in-sync pawn yaw is the right basis. Across the full log this gives 10% of rows over reach-25
	// vs ~35% for the old render-facing basis.
	// [XR] HEAD-SWING FIX: un-yaw by the EXACT yaw the RENDERER draws the body at (r_data/models.cpp):
	//   valid facing -> vr_body_facing_yaw + vr_body_yaw + SpriteRotation   (DECOUPLED from the HMD)
	//   else         -> pawn Angles.Yaw   + vr_body_yaw + SpriteRotation
	// The pawn Angles.Yaw follows the headset, but the body is DRAWN at the decoupled facing. Un-yawing by
	// the pawn yaw (the old basis) rotated the model-local hand target by (headYaw - bodyFacingYaw) every
	// time the head turned inside the body dead-zone, so the extended arm swung opposite the head. Matching
	// the renderer's ACTUAL drawn yaw makes the IK frame track the DRAWN body -> head-turn no longer moves
	// the arm. (This replaces the earlier pure-pawnYaw basis, which was measured before the facing-decouple
	// and body-yaw/sprite terms were folded into the render path.)
	double renderBodyYaw = (player->vr_body_facing_valid ? (double)player->vr_body_facing_yaw
	                                                     : body->Angles.Yaw.Degrees())
	                     + (double)vr_body_yaw + body->SpriteRotation.Degrees();
	const DAngle   invYaw   = DAngle::fromDeg(-renderBodyYaw); // undo the renderer's drawn body yaw (models.cpp)
	// [XR] cosInvYaw/sinInvYaw are no longer consumed by the wrist path: IK_ControllerModelRot now
	// derives the controller orientation from the EXACT Finv (= swapYZ * objectToWorld^-1), the same
	// inverse the position path uses, instead of this pawn-yaw-only un-yaw. invYaw itself is still read
	// by the [VRIK_TGT2] diagnostic below; the cos/sin are retained [[maybe_unused]] as the documented
	// reference for IK_WorldToModelLocal's shared algebra.
	[[maybe_unused]] const double   cosInvYaw = invYaw.Cos();
	[[maybe_unused]] const double   sinInvYaw = invYaw.Sin();

	// Real per-hand tracked targets. NOTE: player->mo->AttackPos is deliberately NOT used
	// here for either hand, even though the design notes floated it for the main hand --
	// AttackPos is set from PosAtZ(shootz) (common/rendering/hwrenderer/data/hw_vrmodes.cpp,
	// VRMode::SetUp), i.e. its X/Y are pinned to the actor's OWN center and only Z tracks
	// the headset height; it's a hitscan ray origin, not a 3D hand position, and would
	// leave the arms unable to reach sideways at all. GetWeaponTransform(hand) is the real
	// per-hand tracked transform -- the same source VR_UpdateGravityGloves and
	// VR_UpdateHardpoints already use for both hands -- so it's used for
	// BOTH main and off hand here too.
	// [XR] Use the mode that is ACTUALLY VR. GetVRModeCached(false) returns the MONO mode
	// (hw_vrmodes.cpp -- !toscreen -> mode 0), whose GetHandTransform returns false -- which is
	// exactly why the arms never got hand targets (haveTarget=0,0 every tic, gate i). In the playsim
	// tic the (true) cache resolves to the real OpenXR mode, whose GetHandTransform is populated from
	// the live controller pose. Prefer whichever mode IsVR(); fall back to (false) so non-VR is safe.
	const VRMode* vrmode = VRMode::GetVRModeCached(true);
	if (!vrmode->IsVR()) vrmode = VRMode::GetVRModeCached(false);
	bool rightHanded = vr_control_scheme < 10; // same remap GetWeaponTransform itself applies internally
	int rightHandEnum = rightHanded ? VR_MAINHAND : VR_OFFHAND;
	int leftHandEnum  = rightHanded ? VR_OFFHAND  : VR_MAINHAND;

	VSMatrix rightXf, leftXf;
	bool haveTarget[2];
	bool haveCtrl[2];   // [XR] controller transform present (needed for the wrist orientation)
	haveCtrl[0] = vrmode->GetWeaponTransform(&rightXf, rightHandEnum);
	haveCtrl[1] = vrmode->GetWeaponTransform(&leftXf,  leftHandEnum);
	// [XR] An external hand target (Actor.SetVRBodyHandTarget) can stand in for a missing controller
	// position, and always replaces it when present -- the point another hand model is drawn at.
	haveTarget[0] = haveCtrl[0] || player->vr_body_hand_target_valid[rightHandEnum] || player->vr_body_hand_actor[rightHandEnum] != nullptr;
	haveTarget[1] = haveCtrl[1] || player->vr_body_hand_target_valid[leftHandEnum]  || player->vr_body_hand_actor[leftHandEnum]  != nullptr;
	if (!haveCtrl[0]) rightXf.loadIdentity();
	if (!haveCtrl[1]) leftXf.loadIdentity();

	// [XR hand-world collision] Clamp a hand's IK target to the wall when VR_UpdateHandCollision (runs
	// earlier the same tic) found it touching solid geometry -- keeps the rendered hand from
	// clipping through walls. Deliberately placed HERE, upstream of everything below (including the
	// hs_foregrip pin a few lines down): overriding rightXf/leftXf's translation before either matrix is
	// consumed means a clamped, non-foregripping hand flows into the rest of the solve for free, with no
	// second write site. CONTRACT: a foregripping OFF-hand stays pinned to the gun, never the wall --
	// excluded here so this and the foregrip pin can never fight over the same hand in the same tic.
	// m[12]=X, m[13]=Z(map-up), m[14]=Y -- same layout every GetWeaponTransform consumer
	// uses; const_cast<FLOATTYPE*>(xf.get()) matches the existing write-through pattern used below
	// (the rotation-matrix build) rather than adding a new mutator to VSMatrix.
	if (vr_hand_ik_clamp)
	{
		if (haveTarget[0] && player->vr_hand_touching_wall[rightHandEnum] &&
			!(rightHandEnum == VR_OFFHAND && player->vr_foregrip_engaged))
		{
			const DVector3& c = player->vr_hand_collision_clamp_pos[rightHandEnum];
			FLOATTYPE* rm = const_cast<FLOATTYPE*>(rightXf.get());
			rm[12] = (FLOATTYPE)c.X; rm[13] = (FLOATTYPE)c.Z; rm[14] = (FLOATTYPE)c.Y;
		}
		if (haveTarget[1] && player->vr_hand_touching_wall[leftHandEnum] &&
			!(leftHandEnum == VR_OFFHAND && player->vr_foregrip_engaged))
		{
			const DVector3& c = player->vr_hand_collision_clamp_pos[leftHandEnum];
			FLOATTYPE* lm = const_cast<FLOATTYPE*>(leftXf.get());
			lm[12] = (FLOATTYPE)c.X; lm[13] = (FLOATTYPE)c.Z; lm[14] = (FLOATTYPE)c.Y;
		}
	}

	{ static int s_ikMode=0; if (s_ikMode++<20) Printf("[VRIK_MODE] IsVR=%d haveTarget=%d,%d\n", (int)vrmode->IsVR(), (int)haveTarget[0], (int)haveTarget[1]); }
	if (!haveTarget[0] && !haveTarget[1])
	{
		{ static int s_gI=0; if (s_gI++<20) Printf("[VRIK_BAIL_I] haveT=%d,%d\n", (int)haveTarget[0], (int)haveTarget[1]); }
		player->vr_ik_active = false;
		return;
	}

	// [XR] EXACT F^-1: target_baseframe = swapYZ * objectToWorldMatrix^-1 * controller_world_GL.
	// This single inversion of the renderer's OWN published matrix (g_xr_vrBodyObjectToWorld, models.cpp)
	// REPLACES the whole hand-rebuilt world->model-local pipeline that used to live here -- the un-yaw
	// (IK_WorldToModelLocal), the (.Z,.X,.Y) skeleton relabel, the bodyFit divide, and the forward-Y flip.
	// The renderer draws  world = objectToWorldMatrix * boneData[i] * vertex,  boneData[i] == Identity at
	// bind, and the uploaded vertex is swapYZ * v_file while baseframe[] is built from the RAW file TRS with
	// no swap -- so the exact baseframe-space -> GL-world map is  F = objectToWorldMatrix * swapYZ,  and its
	// inverse lands the controller in the SAME baseframe space (Z-up, shoulder Z~59) the two-bone solver
	// reads shoulderPos = IK_MatTranslation(baseframe[upperArm]) from. objectToWorldMatrix already contains
	// the drawn yaw, vr_body_z, and the Y/Z-swapped bodyScale, so all of those are inverted for free: NO
	// manual remap / feet-subtract / scale-divide / forward-flip. (invYaw is retained ABOVE only for the
	// [VRIK_TGT2] diagnostic; the wrist path IK_ControllerModelRot now uses this SAME Finv, not invYaw.)
	// swapYZ = row-swap of Y and Z, its own inverse (models_iqm.cpp).
	static const FLOATTYPE kSwapYZ[16] = { 1,0,0,0,  0,0,1,0,  0,1,0,0,  0,0,0,1 };
	VSMatrix swapYZ; swapYZ.loadMatrix(kSwapYZ);
	VSMatrix objToWorldInv;
	if (!g_xr_vrBodyObjToWorldValid || !g_xr_vrBodyObjectToWorld.inverseMatrix(objToWorldInv))
	{
		// No published body matrix yet (pre-first-render) or a singular transform (e.g. bodyScale 0):
		// hold the bind pose this tic rather than solving to garbage.
		{ static int s_gM=0; if (s_gM++<20) Printf("[VRIK_BAIL_M] valid=%d\n", (int)g_xr_vrBodyObjToWorldValid); }
		player->vr_ik_active = false;
		return;
	}
	// F^-1 = swapYZ * objectToWorldMatrix^-1  (VSMatrix A.multMatrix(B) => A = A*B, so this composes swapYZ
	// on the LEFT of the inverse -- applied to the point AFTER the inverse, which is what F^-1 requires).
	VSMatrix Finv = swapYZ;
	Finv.multMatrix(objToWorldInv);

	// [XR] bodyFitScale is kept ONLY for the [VRIK_TGT2] diagnostic printf below; it no longer scales the
	// target (bodyScale is inverted inside objectToWorldMatrix). Remove with the probe once tracking is
	// confirmed.
	float bodyFitScale = g_xr_vrBodyRenderScale;
	if (!(bodyFitScale > 0.05f && bodyFitScale < 8.0f)) bodyFitScale = (float)vr_body_size;
	if (!(bodyFitScale > 0.05f && bodyFitScale < 8.0f)) bodyFitScale = 0.70f;

	FVector3 targetLocal[2]; // [0]=right,[1]=left, in baseframe space (X=lateral, Y=forward, Z=up)
	for (int s = 0; s < 2; s++)
	{
		if (!haveTarget[s]) continue;

		// [XR] HAND-TO-HOTSPOT PIN (IQM weapons). When the OFF hand is foregripping the weapon's authored
		// hs_foregrip bone, drive THAT hand's IK target from the foregrip WORLD point instead of the raw
		// controller. This lands the marine hand exactly ON the gun's foregrip and keeps it glued there as
		// two-hand aim swings the gun -- instead of floating at wherever the physical controller drifted.
		//   * Gated on player->vr_foregrip_engaged, which VR_CalculateTwoHanding (called ONE line before this
		//     in P_PlayerThink, same tic -> fresh) sets ONLY when VR_WeaponHotspotWorld returns a REAL authored
		//     hs_foregrip bone AND the off hand grips within vr_foregrip_radius AND the grip arbiter frees it.
		//     MD3 / unconverted weapons never author the bone -> never engage -> this path is byte-for-byte
		//     unchanged for them (no behavioral risk to the existing roster).
		//   * MAIN hand is intentionally NOT pinned: the gun rides the main-hand transform and hs_grip sits at
		//     the model origin (0,0,0), so the main hand already lands on the grip by construction.
		//   * vr_foregrip_world is Doom world (x, y, z-up); the point path below wants GL (x, up, z) -- i.e.
		//     GLx=Doom.x, GLy=Doom.z, GLz=Doom.y -- the exact inverse of VR_WeaponHotspotWorld's own
		//     handPos(m[12],m[14],m[13]) Doom read. Wrist ORIENTATION is left on the controller (unchanged):
		//     the off hand is physically at the grip when engaged, so its real orientation reads as a natural
		//     grip, and this avoids disturbing the verified IK_ControllerModelRot wrist path.
		const int   handEnum    = (s == 0) ? rightHandEnum : leftHandEnum;
		const bool  pinForegrip = player->vr_foregrip_engaged && (handEnum == VR_OFFHAND);
		// [XR] A hand MODEL ACTOR to follow: its live object matrix this frame gives the exact drawn
		// position of its palm joint (plus the mod's offset) -- render rate, zero lag.
		FVector3 handActorGL; VSMatrix handActorM; bool haveHandActor = false;
		{
			AActor* ha = player->vr_body_hand_actor[handEnum];
			if (!pinForegrip && ha != nullptr && !(ha->ObjectFlags & OF_EuthanizeMe) && player->vr_body_hand_palm_bone[handEnum] != NAME_None)
				haveHandActor = VR_ModelJointWorld(ha, player->vr_body_hand_palm_bone[handEnum], player->vr_body_hand_offset_model[handEnum], handActorGL, handActorM);
			if (haveHandActor)
			{
				// its drawn axes, for the wrist orientation below
				const FLOATTYPE* m = handActorM.get();
				player->vr_body_hand_frame_ax[handEnum] = FVector3((float)m[0], (float)m[1], (float)m[2]);
				player->vr_body_hand_frame_ay[handEnum] = FVector3((float)m[4], (float)m[5], (float)m[6]);
				player->vr_body_hand_frame_az[handEnum] = FVector3((float)m[8], (float)m[9], (float)m[10]);
				player->vr_body_hand_frame_valid[handEnum] = true;
				haveTarget[s] = true;
			}
		}
		const bool  extTarget   = !pinForegrip && !haveHandActor && player->vr_body_hand_target_valid[handEnum];

		FLOATTYPE ptGL[4];
		if (haveHandActor)
		{
			ptGL[0] = (FLOATTYPE)handActorGL.X; ptGL[1] = (FLOATTYPE)handActorGL.Y; ptGL[2] = (FLOATTYPE)handActorGL.Z; ptGL[3] = (FLOATTYPE)1;
		}
		else if (pinForegrip)
		{
			ptGL[0] = (FLOATTYPE)player->vr_foregrip_world[0]; // Doom.x   -> GL.x
			ptGL[1] = (FLOATTYPE)player->vr_foregrip_world[2]; // Doom.z-up-> GL.y
			ptGL[2] = (FLOATTYPE)player->vr_foregrip_world[1]; // Doom.y   -> GL.z
			ptGL[3] = (FLOATTYPE)1;
		}
		else if (extTarget)
		{
			// [XR] The mod told us where this hand IS (Doom world) -- e.g. the wrist of the hand model it
			// draws. Same Doom -> GL relabel as the foregrip point above.
			const DVector3& t = player->vr_body_hand_target_pos[handEnum];
			ptGL[0] = (FLOATTYPE)t.X; ptGL[1] = (FLOATTYPE)t.Z; ptGL[2] = (FLOATTYPE)t.Y; ptGL[3] = (FLOATTYPE)1;
		}
		else
		{
			// GetWeaponTransform columns m[12..14] are ALREADY in GL layout (GL-X=Doom.x, GL-Y=up=Doom.z,
			// GL-Z=Doom.y) -- exactly objectToWorldMatrix's output axes -- so feed them straight in, NO reorder.
			// (This is the same per-hand controller world pos the old m[12],m[14],m[13] Doom read used; here we
			// keep the raw GL columns because objectToWorldMatrix^-1 wants GL, not Doom. AttackPos is NOT used:
			// its X/Y are actor-center-pinned, per the note above, so it can't drive sideways reach.)
			const FLOATTYPE* m = (s == 0) ? rightXf.get() : leftXf.get();
			ptGL[0] = m[12]; ptGL[1] = m[13]; ptGL[2] = m[14]; ptGL[3] = (FLOATTYPE)1;
		}
		FLOATTYPE outBase[4];
		Finv.multMatrixPoint(ptGL, outBase);                 // swapYZ * objectToWorldMatrix^-1 * (ctrl | foregrip)_GL
		targetLocal[s] = FVector3((float)outBase[0], (float)outBase[1], (float)outBase[2]);

		if (pinForegrip)
		{
			static int s_pinLog = 0;
			if (s_pinLog < 40)
			{
				s_pinLog++;
				Printf("[VRIK_PIN] off-hand -> hs_foregrip world=(%.1f,%.1f,%.1f) local=(%.1f,%.1f,%.1f)\n",
					player->vr_foregrip_world[0], player->vr_foregrip_world[1], player->vr_foregrip_world[2],
					targetLocal[s].X, targetLocal[s].Y, targetLocal[s].Z);
			}
		}
	}

	// [XR] Live CONSTANT nudge so the hand can be dialed exactly onto the controller in-headset -- absorbs any
	// fixed shift the frame math leaves (vr_body_z, feet-vs-mesh-origin, authoring). Dial vr_ik_target_off* in
	// the console until the model hand sits in the sphere; the value is then baked into autoexec.
	if ((float)vr_ik_target_offx != 0.f || (float)vr_ik_target_offy != 0.f || (float)vr_ik_target_offz != 0.f)
		for (int s = 0; s < 2; s++)
			if (haveTarget[s])
				targetLocal[s] += FVector3((float)vr_ik_target_offx, (float)vr_ik_target_offy, (float)vr_ik_target_offz);

	// [XR] PALM-SEAT: pull each hand's target back along model-FORWARD (+Y) by the fixed wrist->palm length so
	// the palm (not the wrist) lands on the controller/grip. See vr_ik_palm_seat decl. A foregripping OFF hand
	// is EXCLUDED: it is already pinned to hs_foregrip's WORLD point (above), whose palm seating is handled by
	// that bone's own authored placement -- shifting it here would double-correct and slide it off the barrel.
	// [5.0] SIGN: the rig's toes prove -Y is FORWARD in this frame (marine and Slayer alike), so pulling
	// the target BACK is +Y. The original subtracted, which pushed the wrist forward. An external target
	// is exactly where the hand is and is never moved.
	if ((float)vr_ik_palm_seat != 0.f)
		for (int s = 0; s < 2; s++)
		{
			if (!haveTarget[s]) continue;
			const int handEnum = (s == 0) ? rightHandEnum : leftHandEnum;
			if (player->vr_foregrip_engaged && handEnum == VR_OFFHAND) continue;
			if (player->vr_body_hand_target_valid[handEnum]) continue;
			if (player->vr_body_hand_actor[handEnum] != nullptr) continue;
			targetLocal[s].Y += (float)vr_ik_palm_seat;
		}

	// ---- per-side bind-pose geometry, read straight from the model's own baseframe
	// (model-local space, no swapYZ / inversebaseframe -- see COORDINATE FRAME above) ----
	FVector3 shoulderPos[2], elbowBindPos[2], handBindPos[2];
	FQuaternion collarBindRot[2], upperBindRot[2], lowerBindRot[2];
	float upperLen[2], forearmLen[2];

	for (int side = 0; side < 2; side++)
	{
		shoulderPos[side]  = IK_MatTranslation(baseframe[chains[side].upperArm]);
		elbowBindPos[side] = IK_MatTranslation(baseframe[chains[side].lowerArm]);
		handBindPos[side]  = IK_MatTranslation(baseframe[chains[side].hand]);

		collarBindRot[side] = IK_MatRotation(baseframe[chains[side].collar]);
		upperBindRot[side]  = IK_MatRotation(baseframe[chains[side].upperArm]);
		lowerBindRot[side]  = IK_MatRotation(baseframe[chains[side].lowerArm]);

		float derivedUpper = (float)(elbowBindPos[side] - shoulderPos[side]).Length();
		float derivedFore  = (float)(handBindPos[side] - elbowBindPos[side]).Length();

		// Fall back to the cvars (default 0 => "prefer the model", per hw_vrmodes.cpp's own
		// vr_ik_upperarm_len/vr_ik_forearm_len comments) ONLY if the derived length is
		// degenerate; if the cvar is also unset, fall back to the bind-length constants
		// this session measured directly off this same rig (~11.05 / ~9.44 map units).
		upperLen[side]   = (derivedUpper > 0.1f) ? derivedUpper : ((float)vr_ik_upperarm_len > 0.1f ? (float)vr_ik_upperarm_len : 11.05f);
		forearmLen[side] = (derivedFore  > 0.1f) ? derivedFore  : ((float)vr_ik_forearm_len  > 0.1f ? (float)vr_ik_forearm_len  : 9.44f);

		// [XR] Live overall ARM-LENGTH scale (vr_ik_reach_scale, default 1.0). Scales BOTH bones together so the
		// hands reach nearer/farther from the shoulder without changing bend behaviour -- the tuning slider for
		// "my arms feel too short/long". Guarded to a sane range so a mis-set value can't zero or explode reach.
		float armScale = (float)vr_ik_reach_scale;
		if (armScale < 0.5f) armScale = 0.5f; else if (armScale > 2.0f) armScale = 2.0f;
		// [XR] the drawn arm is scaled by vr_body_arm_size (below), so the solve reaches with it
		float armSize = clamp((float)vr_body_arm_size, 0.5f, 2.0f);
		upperLen[side]   *= armScale * armSize;
		forearmLen[side] *= armScale * armSize;
	}

	// ===================================================================================================
	// [XR] POSTURE: lean (spine) and crouch (hips + legs) from the headset, then the arms' frames follow.
	// ===================================================================================================
	{
		// headset in baseframe space, and where the rig's eyes sit at rest
		FVector3 hmdBase(0.f, 0.f, 0.f); bool haveHmd = false;
		if (cachedNeck >= 0 && (unsigned)cachedNeck < baseframe.Size())
		{
			FLOATTYPE hg[4] = { (FLOATTYPE)player->mo->HmdPos.X, (FLOATTYPE)player->mo->HmdPos.Z, (FLOATTYPE)player->mo->HmdPos.Y, (FLOATTYPE)1 };
			FLOATTYPE hb[4]; Finv.multMatrixPoint(hg, hb);
			hmdBase = FVector3((float)hb[0], (float)hb[1], (float)hb[2]);
			haveHmd = true;
		}
		FVector3 eyeBind = haveHmd ? IK_MatTranslation(baseframe[cachedNeck]) + FVector3(0.f, 0.f, (float)vr_body_neck_eye_gap) : FVector3(0.f, 0.f, 0.f);
		FVector3 d = haveHmd ? (hmdBase - eyeBind) : FVector3(0.f, 0.f, 0.f);

		// [XR] [VRIK_POSTURE] what the posture block is actually being fed, once a second while either
		// feature is on: a lean that does nothing because the offset reads zero looks identical, in a
		// headset, to one that does nothing because the code is wrong.
		if ((vr_body_lean || vr_body_crouch) && screen != nullptr)
		{
			static uint64_t s_lastPost = 0;
			if (screen->FrameTime - s_lastPost > 1000)
			{
				s_lastPost = screen->FrameTime;
				Printf("[VRIK_POSTURE] hmd offset from rest eye=(%.1f,%.1f,%.1f) crouch=%.1f spine=%u hips=%d lean=%d crouch_on=%d\n",
					d.X, d.Y, d.Z, player->vr_body_crouch_drop, cachedSpine.Size(), cachedHips,
					(int)vr_body_lean, (int)vr_body_crouch);
			}
		}

		// ---- LEAN: bend the spine so the eyes follow the headset's horizontal offset
		if (vr_body_lean && haveHmd && cachedSpine.Size() > 0)
		{
			const int root = cachedSpine[0];
			const FVector3 pivot = IK_MatTranslation(baseframe[root]);
			FVector3 from = eyeBind - pivot;
			FVector3 to   = eyeBind + FVector3(d.X, d.Y, 0.f) * clamp((float)vr_body_lean_gain, 0.f, 2.f) - pivot;
			if (from.LengthSquared() > 1.e-4f && to.LengthSquared() > 1.e-4f)
			{
				from.MakeUnit(); to.MakeUnit();
				FQuaternion swing = IK_QuatFromTo(from, to);
				const float per = 1.0f / (float)cachedSpine.Size();
				for (unsigned k = 0; k < cachedSpine.Size(); k++)
				{
					const int j = cachedSpine[k];
					const FQuaternion partial = IK_Fraction(swing, per * (float)(k + 1));   // cumulative up the chain
					FQuaternion desiredWorld = partial * IK_MatRotation(baseframe[j]);
					const int p = model->GetJointParent(j);
					FQuaternion parentWorld(0.f, 0.f, 0.f, 1.f);
					if (p >= 0) { VSMatrix pm; IK_PoseWorld(model, player->vr_ik_pose, p, pm); parentWorld = IK_MatRotation(pm); }
					FQuaternion local = parentWorld.Inverse() * desiredWorld; local.MakeUnit();
					player->vr_ik_pose[j].rotation = local;
				}
			}
		}

		// ---- CROUCH + FEET ON TERRAIN: drop the hips by the headset's drop; put each foot on the floor
		//      under it; then re-solve the legs from the (moved) hips to the (moved) feet.
		{
			float drop = 0.f;
			if (vr_body_crouch && cachedHips >= 0)
			{
				// [XR] From the renderer's standing high-water mark, NOT from d.Z. d.Z is the headset's
				// offset from the rig's rest eye position, and the neck-to-HMD placement drives exactly
				// that to zero every frame -- so reading a crouch out of it measured the one quantity
				// guaranteed to be nothing. See vr_body_crouch_drop (d_player.h, written in models.cpp).
				drop = player->vr_body_crouch_drop;
				const float dz = clamp((float)vr_body_crouch_deadzone, 0.f, 20.f);
				drop = (drop > dz) ? (drop - dz) : 0.f;
			}
			// per-foot floor delta (baseframe units, +Z up), relative to the pawn's own floor
			float footDz[2] = { 0.f, 0.f }; bool anyTerrain = false;
			if (vr_body_feet_terrain && player->mo != nullptr && player->mo->Level != nullptr)
			{
				// baseframe -> GL world is F = objectToWorld * swapYZ (the inverse of Finv above)
				VSMatrix F = g_xr_vrBodyObjectToWorld; F.multMatrix(swapYZ);
				const float lim = clamp((float)vr_body_feet_max, 0.f, 128.f);
				for (int side = 0; side < 2; side++)
				{
					const int footJ = cachedLegs[side][2];
					if (footJ < 0 || (unsigned)footJ >= baseframe.Size()) continue;
					const FVector3 fb = IK_MatTranslation(baseframe[footJ]);
					FLOATTYPE pt[4] = { (FLOATTYPE)fb.X, (FLOATTYPE)fb.Y, (FLOATTYPE)fb.Z, (FLOATTYPE)1 }, g[4];
					F.multMatrixPoint(pt, g);                       // GL: x, up, y
					const DVector2 xy((double)g[0], (double)g[2]);  // Doom x, y
					sector_t* sec = player->mo->Level->PointInSector(xy);
					if (sec == nullptr) continue;
					const double dzWorld = sec->floorplane.ZatPoint(xy) - player->mo->floorz;   // + = the floor under this foot is higher
					// world up delta -> baseframe (a direction through Finv; GL up is +Y)
					FLOATTYPE wd[4] = { (FLOATTYPE)0, (FLOATTYPE)dzWorld, (FLOATTYPE)0, (FLOATTYPE)0 }, bd[4];
					Finv.multMatrixPoint(wd, bd);
					footDz[side] = clamp((float)bd[2], -lim, lim);
					if (fabsf(footDz[side]) > 0.01f) anyTerrain = true;
				}
			}
			if (drop > 0.f && cachedHips >= 0)
			{
				// world delta -> the hips joint's parent-local translation delta
				const int hp = model->GetJointParent(cachedHips);
				VSMatrix pm; pm.loadIdentity();
				if (hp >= 0) IK_PoseWorld(model, player->vr_ik_pose, hp, pm);
				VSMatrix pinv;
				if (pm.inverseMatrix(pinv))
				{
					FLOATTYPE wd[4] = { 0, 0, (FLOATTYPE)(-drop), 0 }, ld[4];
					pinv.multMatrixPoint(wd, ld);
					player->vr_ik_pose[cachedHips].translation += FVector3((float)ld[0], (float)ld[1], (float)ld[2]);
				}
			}
			if (drop > 0.f || anyTerrain)
			{
				// legs: two-bone solve from the (lowered) hips to each foot's rest point on its floor
				for (int side = 0; side < 2; side++)
				{
					const int hipJ = cachedLegs[side][0], kneeJ = cachedLegs[side][1], footJ = cachedLegs[side][2];
					if (hipJ < 0 || kneeJ < 0 || footJ < 0) continue;
					VSMatrix hm; IK_PoseWorld(model, player->vr_ik_pose, hipJ, hm);
					const FVector3 hipNow = IK_MatTranslation(hm);
					const FVector3 hipBind = IK_MatTranslation(baseframe[hipJ]);
					const FVector3 kneeBind = IK_MatTranslation(baseframe[kneeJ]);
					const FVector3 footBind = IK_MatTranslation(baseframe[footJ]);
					// the leg's bind geometry moved with the hips (translation only): shift it
					const FVector3 shift = hipNow - hipBind;
					const float lu = (float)(kneeBind - hipBind).Length(), ll = (float)(footBind - kneeBind).Length();
					FVector3 footTarget = footBind + FVector3(0.f, 0.f, footDz[side]);
					// legs never stretch: a floor too far below straightens the leg and the foot hangs
					{
						const FVector3 toFoot = footTarget - hipNow;
						const float maxLeg = (lu + ll) * 0.98f, len = (float)toFoot.Length();
						if (len > maxLeg && len > 0.001f) footTarget = hipNow + toFoot * (maxLeg / len);
					}
					FVector3 pole = FVector3(0.f, -1.f, 0.f);   // knees forward (-Y is forward)
					FArmIKSolve leg;
					if (IK_SolveTwoBoneArm(hipNow, kneeBind + shift, footBind + shift, footTarget, pole,
					                        IK_MatRotation(baseframe[hipJ]), IK_MatRotation(baseframe[kneeJ]), lu, ll, leg))
					{
						const int hipParent = model->GetJointParent(hipJ);
						FQuaternion hipParentWorld(0.f, 0.f, 0.f, 1.f);
						if (hipParent >= 0) { VSMatrix ppm; IK_PoseWorld(model, player->vr_ik_pose, hipParent, ppm); hipParentWorld = IK_MatRotation(ppm); }
						FQuaternion lUp = hipParentWorld.Inverse() * leg.upperWorldRot; lUp.MakeUnit();
						FQuaternion lLo = leg.upperWorldRot.Inverse() * leg.lowerWorldRot; lLo.MakeUnit();
						FQuaternion lFt = leg.lowerWorldRot.Inverse() * IK_MatRotation(baseframe[footJ]); lFt.MakeUnit();   // foot stays flat
						player->vr_ik_pose[hipJ].rotation  = lUp;
						player->vr_ik_pose[kneeJ].rotation = lLo;
						player->vr_ik_pose[footJ].rotation = lFt;
					}
				}
			}
		}

		// ---- SHOULDERS: the collar bone follows the hand a little
		const float follow = clamp((float)vr_ik_shoulder_follow, 0.f, 1.f);
		for (int side = 0; side < 2; side++)
		{
			if (!haveTarget[side] || follow <= 0.f) continue;
			const int cj = chains[side].collar;
			VSMatrix cm; IK_PoseWorld(model, player->vr_ik_pose, cj, cm);           // collar as posed so far (spine lean included)
			const FVector3 cpos = IK_MatTranslation(cm);
			const FQuaternion cworld = IK_MatRotation(cm);
			FVector3 u0 = shoulderPos[side] - IK_MatTranslation(baseframe[cj]);      // collar -> shoulder, at bind
			u0 = IK_MatRotation(cm) * (IK_MatRotation(baseframe[cj]).Inverse() * u0); // ... carried by the collar's current rotation
			FVector3 u1 = targetLocal[side] - cpos;
			if (u0.LengthSquared() < 1.e-6f || u1.LengthSquared() < 1.e-6f) continue;
			u0.MakeUnit(); u1.MakeUnit();
			FQuaternion swing = IK_QuatFromTo(u0, u1);
			// cap the swing angle
			const float ang = 2.f * acosf(clamp(fabsf(swing.W), 0.f, 1.f)) * (180.f / (float)M_PI);
			float k = follow;
			const float maxDeg = clamp((float)vr_ik_shoulder_max, 0.f, 90.f);
			if (ang * k > maxDeg && ang > 1.e-3f) k = maxDeg / ang;
			FQuaternion desiredWorld = IK_Fraction(swing, k) * cworld;
			const int p = model->GetJointParent(cj);
			FQuaternion parentWorld(0.f, 0.f, 0.f, 1.f);
			if (p >= 0) { VSMatrix pm; IK_PoseWorld(model, player->vr_ik_pose, p, pm); parentWorld = IK_MatRotation(pm); }
			FQuaternion local = parentWorld.Inverse() * desiredWorld; local.MakeUnit();
			player->vr_ik_pose[cj].rotation = local;
		}

		// ---- the arm's bind frame moves with whatever moved its collar: re-derive everything the solve reads
		for (int side = 0; side < 2; side++)
		{
			const int cj = chains[side].collar;
			VSMatrix cm; IK_PoseWorld(model, player->vr_ik_pose, cj, cm);
			VSMatrix cbInv;
			if (!baseframe[cj].inverseMatrix(cbInv)) continue;
			VSMatrix D = cm; D.multMatrix(cbInv);                                     // posed collar * bind collar^-1
			const FQuaternion dr = IK_MatRotation(D);
			auto xf = [&](const FVector3& v) { FLOATTYPE in[4] = { v.X, v.Y, v.Z, 1 }, o[4]; D.multMatrixPoint(in, o); return FVector3((float)o[0], (float)o[1], (float)o[2]); };
			shoulderPos[side]  = xf(shoulderPos[side]);
			elbowBindPos[side] = xf(elbowBindPos[side]);
			handBindPos[side]  = xf(handBindPos[side]);
			collarBindRot[side] = dr * collarBindRot[side]; collarBindRot[side].MakeUnit();
			upperBindRot[side]  = dr * upperBindRot[side];  upperBindRot[side].MakeUnit();
			lowerBindRot[side]  = dr * lowerBindRot[side];  lowerBindRot[side].MakeUnit();
		}
	}

	// [XR] [VRIK_TGT2] POST-CORRECTION probe (frame remap + body-fit scale applied). If reach ~= armLen
	// (~15-20 for a natural chest reach vs armLen 20.5), the target is now REACHABLE and the arms will
	// bend to it. If reach is still >> armLen, bodyFitScale is wrong: too-big reach -> bodyFitScale too
	// small (over-divided), too-small -> too large. Also prints the raw target so a HEIGHT-only offset
	// (Z far from ~45 while X/Y sane) is distinguishable from a uniform scale error. Remove once tracking.
	{
		// [XR] periodic (every 15 tics, up to 400 logs) so it captures a full RANGE OF MOTION, not just the
		// startup burst. Also logs the yaw state (fv=facing_valid / rYaw=renderYaw used / aYaw=raw pawn yaw)
		// so the arm-frame error AND any HMD-yaw fallback (fv=0 -> uses aYaw -> head leak) are both visible.
		static int s_ikTgt2Call = 0;
		static int s_ikTgt2 = 0;
		const bool doLog = ((s_ikTgt2Call++ % 15) == 0) && s_ikTgt2 < 400;
		for (int s = 0; s < 2 && doLog; s++)
		{
			if (!haveTarget[s]) continue;
			s_ikTgt2++;
			FVector3 d = targetLocal[s] - shoulderPos[s];
			Printf("[VRIK_TGT2] body=%p md=%p rhits=%d side=%d fv=%d invYaw=%.1f aYaw=%.1f bodyFit=%.3f tgt=(%.1f,%.1f,%.1f) shoulder=(%.1f,%.1f,%.1f) reach=%.1f armLen=%.1f\n",
				body, modelData, g_xr_vrRenderProcHits, s, (int)player->vr_body_facing_valid, invYaw.Degrees(), body->Angles.Yaw.Degrees(), bodyFitScale,
				targetLocal[s].X, targetLocal[s].Y, targetLocal[s].Z,
				shoulderPos[s].X, shoulderPos[s].Y, shoulderPos[s].Z,
				(float)d.Length(), upperLen[s] + forearmLen[s]);
		}
	}

	// [XR] TEMP probe: is the solve TARGET actually tracking your controller and reachable, or is it
	// collapsed (near the shoulder/origin -> arms stay at bind)? If tgt moves as you wave and reach ~=
	// armLen, the target is good and the solve math is the issue. If tgt is ~constant or reach is huge/
	// tiny, the world->model target conversion is wrong. Remove once arms track.
	{
		static int s_ikTgt = 0;
		if (s_ikTgt < 40 && haveTarget[0])
		{
			s_ikTgt++;
			FVector3 d = targetLocal[0] - shoulderPos[0];
			Printf("[VRIK_TGT] tgt=(%.1f,%.1f,%.1f) shoulder=(%.1f,%.1f,%.1f) handBind=(%.1f,%.1f,%.1f) reach=%.1f armLen=%.1f\n",
				targetLocal[0].X, targetLocal[0].Y, targetLocal[0].Z,
				shoulderPos[0].X, shoulderPos[0].Y, shoulderPos[0].Z,
				handBindPos[0].X, handBindPos[0].Y, handBindPos[0].Z,
				(float)d.Length(), upperLen[0] + forearmLen[0]);
		}
	}

	// Pole vector per side: outward + down + back, so elbows splay naturally instead of
	// clipping through the torso.
	//  - "outward" is derived from the two shoulders' own bind positions (points away from
	//    the body centerline regardless of which raw model axis happens to be left/right),
	//    not a hardcoded axis letter.
	//  - "back" is the actor's own facing undone by the SAME yaw rotation used above, which
	//    is mathematically always local (+1,0,0) after that conversion: Doom's yaw=0 forward
	//    is (cos,sin,0); undoing yaw always maps the actor's own forward onto local (1,0,0)
	//    (see IK_WorldToModelLocal above) -- so "back" = local (-1,0,0), a derived constant,
	//    not a modeling guess. The Y<->Z relabel never touches X, so this is unaffected by it.
	//  - "down" is local (0,-1,0) -- NOT (0,0,-1) -- per the Y<->Z relabel documented in the
	//    COORDINATE FRAME note above: local Y carries "world up" in this raw joint-local
	//    (Y-up) space, so "down" points along -Y here, not -Z.
	// The 1.0/0.6/0.35 weights are a reasoned starting shape (mostly outward, some sag,
	// slight back), not a balance decision -- these are the first thing to retune from a
	// render test if the elbows splay the wrong way (see the CALIBRATE note above).
	FVector3 lateralRtoL = shoulderPos[1] - shoulderPos[0];
	if (lateralRtoL.LengthSquared() < 1.e-6f) lateralRtoL = FVector3(0.f, 0.f, 1.f); // shoulders coincide -- arbitrary fallback (lateral now lives on local Z)
	lateralRtoL.MakeUnit();
	// [XR] pole vectors in the skeleton's Z-up baseframe frame (lateral=X, forward=Y, up=Z), matching
	// the target-remap above: "down" = -Z, "back" = -forward = -Y.  (Was (0,-1,0)/(-1,0,0) for the old
	// mismatched Y-up target frame -- see the FRAME FIX note where targetLocal is remapped.)
	const FVector3 downLocal(0.f, 0.f, -1.f);
	// [5.0] SIGN: -Y is forward (measured from the rig's toes), so back = +Y. The original's (0,-1,0)
	// pushed hanging elbows forward.
	const FVector3 backLocal(0.f, 1.f, 0.f);
	FVector3 poleDir[2];
	const float wOut = (float)vr_ik_elbow_out, wDown = (float)vr_ik_elbow_down, wBack = (float)vr_ik_elbow_back;
	poleDir[0] = lateralRtoL * -wOut + downLocal * wDown + backLocal * wBack; // right: outward = -lateralRtoL
	poleDir[1] = lateralRtoL *  wOut + downLocal * wDown + backLocal * wBack; // left:  outward = +lateralRtoL
	if (poleDir[0].LengthSquared() < 1.e-6f) poleDir[0] = lateralRtoL * -1.0f;
	if (poleDir[1].LengthSquared() < 1.e-6f) poleDir[1] = lateralRtoL;
	// [XR] The old poleDir horizontal negation (lockstep partner of the removed targetLocal 180) has been
	// REMOVED along with it. poleDir.X = lateralRtoL is a BODY-FIXED direction (built from the model's own
	// baseframe shoulder positions at shoulderPos[side] = IK_MatTranslation(baseframe[...upperArm]) above)
	// -- it is already in the render frame and rotates WITH the body, so it must NOT be yaw-flipped.
	// Flipping it was swapping every elbow to the INBOARD side ("forearm pivots to the wrong lateral
	// side"). With the target now solved in the render frame, the per-side outward signs at 0=-lateralRtoL
	// (right) / 1=+lateralRtoL (left) and back = -Y are already correct: the elbow splays outward and
	// bends down-and-back with no negation.
	poleDir[0].MakeUnit();
	poleDir[1].MakeUnit();

	// ---- solve + write both arms ----
	bool anySolved = false;
	for (int side = 0; side < 2; side++)
	{
		if (!haveTarget[side]) continue;

		FArmIKSolve solve;
		bool ok = IK_SolveTwoBoneArm(
			shoulderPos[side], elbowBindPos[side], handBindPos[side],
			targetLocal[side], poleDir[side],
			upperBindRot[side], lowerBindRot[side],
			upperLen[side], forearmLen[side],
			solve);
		if (!ok) continue;

		// Chain per point 4 of the design brief: upperArm's parent reference is the
		// COLLAR's bind rotation (its real parent in the render composition --
		// baseframe[Parent] inside CalculateBonesIQM); lowerArm's parent reference is the
		// UPPER ARM's OWN JUST-SOLVED world rotation, NOT its bind rotation, because the
		// upper arm no longer sits at its bind orientation once solved (matches the whip's
		// own parentWorldRot=worldRot chaining, vr_whip.zs DriveModelBones).
		FQuaternion localUpper = collarBindRot[side].Inverse() * solve.upperWorldRot;
		FQuaternion localLower = solve.upperWorldRot.Inverse() * solve.lowerWorldRot;
		localUpper.MakeUnit();
		localLower.MakeUnit();

		// Rotation only -- translate/scale were already seeded from GetJointBindTRS above
		// and must stay untouched (bones don't stretch).
		TRS& upperPose = player->vr_ik_pose[chains[side].upperArm];
		upperPose.rotation = localUpper;   // [5.0] TRS::rotation is an FQuaternion
		// [XR] STRETCHY: scale the upperArm bone by solve.stretch. Bone scale propagates down the chain
		// (forearm + hand inherit it), so BOTH arm segments lengthen and the mesh hand reaches the
		// controller even when the target is past the marine's natural arm span. 1.0 = untouched (in reach);
		// the pose is re-seeded to bind every tic above, so no stretch ever lingers.
		// [XR] STRETCH the arm bones so the hand PINS to the controller no matter the marine's arm-to-height
		// proportion. The frame is now an exact inverse (head no longer drags the shoulders), so the target is
		// correct; the only thing that can leave the hand short is the arm being physically too short, and the
		// stretch closes that gap. Allow a generous 2.5x (covers any human reach vs marine proportion) and only
		// guard against NaN / runaway (finite check) so a bad tic can't explode the arm. Solve.stretch is 1.0
		// for in-reach targets, so this is a no-op until you actually extend past the marine's natural span.
		TRS& lowerPose = player->vr_ik_pose[chains[side].lowerArm];
		lowerPose.rotation = localLower;   // [5.0] FQuaternion

		// [5.0] LENGTHEN, DO NOT INFLATE. The original wrote the stretch as a uniform SCALE on the upper
		// arm, and scale inherits down the chain: the forearm, hand and every finger grew with it --
		// the balloon arm. A bone's length is its child's local translation, so the same reach is had
		// by pushing the elbow and wrist joints out along their bones: nothing thickens, the blend at
		// the joints takes the stretch. The solver already solved with these lengthened bones.
		if (solve.stretch > 1.001f && solve.stretch < 100.0f)
		{
			const float cap = clamp((float)vr_ik_stretch_max, 1.0f, 2.5f);
			float s = (solve.stretch > cap) ? cap : solve.stretch;
			lowerPose.translation = lowerPose.translation * s;
			player->vr_ik_pose[chains[side].hand].translation = player->vr_ik_pose[chains[side].hand].translation * s;
		}

		// [XR] WRIST FOLLOWS CONTROLLER. Without this the hand bone stays at its bind rotation
		// (re-seeded from GetJointBindTRS every tic above) and the palm locks facing inward no
		// matter how the controller twists. The hand's parent in the bip chain is the lowerArm,
		// so -- exactly like localLower = upperWorldRot^-1 * lowerWorldRot above -- the local
		// (parent-relative) hand rotation is lowerArm's JUST-SOLVED model rot ^-1 times the
		// desired hand model rot. desiredHandModel is the controller orientation brought into
		// this same solve frame by IK_ControllerModelRot (same VSMatrix rightXf/leftXf as the
		// position target, same invYaw un-yaw + skeleton relabel). palmOffset is the fixed
		// bind-palm-vs-controller correction, exposed as vr_ik_hand_pitch/yaw/roll (deg) and
		// applied on the MODEL-space side so it rotates the hand about its own axes; dial it
		// in-headset. Guarded by vr_ik_hand_rot so it can be killed without touching reach/bend,
		// and it only touches .rotation (translate/scale stay at bind -- the wrist doesn't move,
		// so the reach the elbow just solved to is not disturbed).
		if (vr_ik_hand_rot)
		{
			const VSMatrix& handXf = (side == 0) ? rightXf : leftXf;
			const int handEnumW = (side == 0) ? rightHandEnum : leftHandEnum;
			FQuaternion ctrlModelRot;
			FQuaternion desiredFromModel;
			bool haveDesiredFromModel = false;
			// [XR] ORIENTATION LOCK: when the mod supplies the hand model's drawn axes and the palm
			// alignment, the hand bone copies that model's orientation exactly; the controller and the
			// palm-offset cvars are the fallback.
			if (player->vr_body_hand_frame_valid[handEnumW] && player->vr_body_hand_align_set[handEnumW])
			{
				FQuaternion qModel;
				if (IK_HandModelRot(player->vr_body_hand_frame_ax[handEnumW], player->vr_body_hand_frame_ay[handEnumW],
				                    player->vr_body_hand_frame_az[handEnumW], Finv, qModel))
				{
					FQuaternion handBindRot = IK_MatRotation(baseframe[chains[side].hand]);
					desiredFromModel = qModel * player->vr_body_hand_align[handEnumW] * handBindRot;
					desiredFromModel.MakeUnit();
					haveDesiredFromModel = true;
				}
			}
			// [XR] Pass the SAME exact Finv (= swapYZ * objectToWorld^-1) the POSITION path built at the
			// top of this function, so the wrist orientation lands in the SAME baseframe as
			// solve.lowerWorldRot -- consistent with the hand position by construction. (Was
			// cosInvYaw/sinInvYaw: the deleted pawn-yaw-only un-yaw + relabel + forward-flip approximation.)
			if (haveDesiredFromModel || (haveCtrl[side] && IK_ControllerModelRot(handXf, Finv, ctrlModelRot)))
			{
				FQuaternion palmOffset =
					FQuaternion::AxisAngle(FVector3(0.f, 0.f, 1.f), FAngle::fromDeg((double)vr_ik_hand_roll))  *
					FQuaternion::AxisAngle(FVector3(0.f, 1.f, 0.f), FAngle::fromDeg((double)vr_ik_hand_yaw))   *
					FQuaternion::AxisAngle(FVector3(1.f, 0.f, 0.f), FAngle::fromDeg((double)vr_ik_hand_pitch));
				// [XR] LEFT/offhand (side 1) bind palm is mirrored vs the right, so it gets its OWN extra
				// alignment (vr_ik_offhand_*) on top of the shared palmOffset; side 0 (right) = identity.
				FQuaternion offhandFlip = (side == 1)
					? FQuaternion::AxisAngle(FVector3(0.f, 0.f, 1.f), FAngle::fromDeg((double)vr_ik_offhand_roll))
					  * FQuaternion::AxisAngle(FVector3(0.f, 1.f, 0.f), FAngle::fromDeg((double)vr_ik_offhand_yaw))
					  * FQuaternion::AxisAngle(FVector3(1.f, 0.f, 0.f), FAngle::fromDeg((double)vr_ik_offhand_pitch))
					: FQuaternion(0.f, 0.f, 0.f, 1.f);
				FQuaternion desiredHandModel = haveDesiredFromModel ? desiredFromModel : (ctrlModelRot * palmOffset * offhandFlip);
				FQuaternion localHand = solve.lowerWorldRot.Inverse() * desiredHandModel;
				localHand.MakeUnit();

				// [XR] JITTER FIX: exponential smoothing of the wrist rotation. Raw controller
				// tracking is noisy, and if consecutive-tic wrist quats flip sign the renderer
				// interpolates them "the long way" -> violent jitter. SLerp(prev, target, a) fixes
				// BOTH: it damps the noise (a<1) AND enforces sign-continuity (it negates `target`
				// when dot<0), so my per-tic outputs are continuous and the render interp stays short.
				// vr_ik_hand_smooth: 1 = raw/instant, lower = smoother/laggier. Per-hand static state;
				// this is first-person local-player only, so a static [2] is safe/transient.
				static FQuaternion s_prevHand[2];
				static bool s_prevHandValid[2] = { false, false };
				if (s_prevHandValid[side])
				{
					// [XR] RATE LIMIT + smoothing. The euler round-trip (vk_openxrdevice.cpp) makes the
					// controller orientation gimbal-jump INSTANTANEOUSLY -- huge one-tic deltas a real
					// wrist can't produce. Measure the shortest angle prev->target (2*acos|dot|, |dot| for
					// quaternion double-cover), and if the smoothed step would exceed vr_ik_hand_maxstep
					// degrees this tic, clamp the SLerp fraction so the wrist rotates AT MOST that far --
					// violent spikes become bounded, human-speed motion and get averaged out, while normal
					// motion (small delta) passes at the vr_ik_hand_smooth rate. SLerp also fixes sign
					// continuity so the render never interpolates "the long way".
					float qd = clamp((float)(s_prevHand[side] | localHand), -1.0f, 1.0f);
					float angRad = 2.0f * acosf(fabsf(qd));                 // shortest prev->target angle
					float t = clamp((float)vr_ik_hand_smooth, 0.05f, 1.0f); // base smoothing fraction
					float maxStepRad = (float)(fabs((double)vr_ik_hand_maxstep) * (M_PI / 180.0));
					if (maxStepRad > 1.e-4f && angRad > 1.e-4f && angRad * t > maxStepRad)
						t = maxStepRad / angRad;                            // cap this tic's rotation
					localHand = FQuaternion::SLerp(s_prevHand[side], localHand, t);
				}
				localHand.MakeUnit();
				s_prevHand[side] = localHand;
				s_prevHandValid[side] = true;

				TRS& handPose = player->vr_ik_pose[chains[side].hand];
				handPose.rotation = localHand;   // [5.0] FQuaternion
			}
		}

		// [XR] TWIST DISTRIBUTION: spread the hand's roll about the forearm axis down the forearm's
		// twist bones, and the upper arm's roll about its own axis down the upper arm's, each by its
		// position along the bone (0 at the parent joint, 1 at the child joint).
		if ((float)vr_ik_twist > 0.f)
		{
			const float tw = clamp((float)vr_ik_twist, 0.f, 1.f);
			struct { int parentJ; int childJ; const TArray<int>* helpers; FQuaternion childLocal; } segs[2] = {
				{ chains[side].lowerArm, chains[side].hand,     &cachedLowerTwist[side], player->vr_ik_pose[chains[side].hand].rotation },
				{ chains[side].upperArm, chains[side].lowerArm, &cachedUpperTwist[side], player->vr_ik_pose[chains[side].lowerArm].rotation } };
			for (int sgi = 0; sgi < 2; sgi++)
			{
				auto& sg = segs[sgi];
				if (sg.helpers->Size() == 0) continue;
				// bone axis in the parent's local frame = the child's bind local translation
				FVector3 axis = model->GetJointBaseTRS(sg.childJ).translation;
				if (axis.LengthSquared() < 1.e-6f) continue;
				const float boneLen = (float)axis.Length();
				axis.MakeUnit();
				// child's rotation relative to its bind, twist only, about the bone axis
				FQuaternion childBind = model->GetJointBaseTRS(sg.childJ).rotation;
				FQuaternion rel = sg.childLocal * childBind.Inverse(); rel.MakeUnit();
				FQuaternion twist = IK_TwistAbout(rel, axis);
				for (unsigned h = 0; h < sg.helpers->Size(); h++)
				{
					const int hj = (*sg.helpers)[h];
					const FVector3 hpos = model->GetJointBaseTRS(hj).translation;
					const float t = clamp((float)(hpos | axis) / boneLen, 0.f, 1.f);
					if (t <= 0.01f) continue;
					FQuaternion frac = IK_Fraction(twist, t * tw);
					FQuaternion outq = frac * model->GetJointBaseTRS(hj).rotation; outq.MakeUnit();
					player->vr_ik_pose[hj].rotation = outq;
				}
			}
		}

		// [XR] Per-limb sizes. Uniform scale on the upper arm carries the whole arm; on the hand
		// joint only the gauntlet and fingers. The arm's lengths were solved with the same factor.
		{
			const float armSize  = clamp((float)vr_body_arm_size, 0.5f, 2.0f);
			const float handSize = clamp((float)vr_body_hand_size, 0.5f, 2.0f);
			if (armSize != 1.f)  upperPose.scaling = upperPose.scaling * armSize;
			if (handSize != 1.f) { TRS& hp = player->vr_ik_pose[chains[side].hand]; hp.scaling = hp.scaling * handSize; }
		}

		anySolved = true;
	}

	// ---- [XR] HAND GRIP CURL: close the fingers around a held weapon ------------------------------------
	// The solve loop above posed the wrist but left every finger joint at its OPEN bind rotation (seeded at
	// the top from GetJointBindTRS). Here we curl those joints into a fist for whichever hand holds a weapon.
	// proceduralPose is PARENT-LOCAL, so post-multiplying a finger joint's local bind rotation by a curl about
	// local +X flexes it relative to its parent -- independent of the wrist pose the IK just solved. Finger
	// names resolved ONCE per model and cached (same discipline as the arm chain), -1 = joint absent -> skipped.
	if ((vr_hand_grip || vr_hand_touch_fingers) && anySolved)
	{
		// [side][finger][segment] joint names. side 0=right,1=left. 4 fingers x 3 flex segments + thumb x 3.
		static const char* const fingerNames[2][5][3] =
		{
			{ // RIGHT
				{ "bip_index_0_R",  "bip_index_1_R",  "bip_index_2_R"  },
				{ "bip_middle_0_R", "bip_middle_1_R", "bip_middle_2_R" },
				{ "bip_ring_0_R",   "bip_ring_1_R",   "bip_ring_2_R"   },
				{ "bip_pinky_0_R",  "bip_pinky_1_R",  "bip_pinky_2_R"  },
				{ "bip_thumb_0_R",  "bip_thumb_1_R",  "bip_thumb_2_R"  },
			},
			{ // LEFT
				{ "bip_index_0_L",  "bip_index_1_L",  "bip_index_2_L"  },
				{ "bip_middle_0_L", "bip_middle_1_L", "bip_middle_2_L" },
				{ "bip_ring_0_L",   "bip_ring_1_L",   "bip_ring_2_L"   },
				{ "bip_pinky_0_L",  "bip_pinky_1_L",  "bip_pinky_2_L"  },
				{ "bip_thumb_0_L",  "bip_thumb_1_L",  "bip_thumb_2_L"  },
			},
		};
		static FModel* fingerCachedModel = nullptr;
		static int     fingerCachedGen = -1;
		static int     fingerIdx[2][5][3];
		if (model != fingerCachedModel || fingerCachedGen != player->vr_body_rig_gen)
		{
			// [XR] Role names "<finger>_<segment>_<side>" (index_0_r ...) resolve through the rig table
			// first, then the built-in marine names.
			static const char* const fingerRole[5] = { "index", "middle", "ring", "pinky", "thumb" };
			for (int s = 0; s < 2; s++)
				for (int fng = 0; fng < 5; fng++)
					for (int seg = 0; seg < 3; seg++)
					{
						char role[32];
						snprintf(role, sizeof role, "%s_%d_%c", fingerRole[fng], seg, s == 0 ? 'r' : 'l');
						int idx = VR_RoleJoint(player, model, role);
						fingerIdx[s][fng][seg] = (idx >= 0) ? idx : model->FindJointByNameCI(FName(fingerNames[s][fng][seg]));
					}
			fingerCachedModel = model;
			fingerCachedGen = player->vr_body_rig_gen;
		}

		// [XR] Flex axis in joint-local space: from the rig table (Actor.SetVRBodyGripAxis), default the
		// marine's +X (its finger bones run down -Z and spread along X).
		FVector3 flexAxis = player->vr_body_grip_axis;
		if (flexAxis.LengthSquared() < 1.e-6f) flexAxis = FVector3(1.f, 0.f, 0.f);
		flexAxis.MakeUnit();
		for (int side = 0; side < 2 && vr_hand_grip; side++)
		{
			if (handPosed[side]) continue;         // [XR] the pose clip already shaped this hand
			const int handEnum = (side == 0) ? rightHandEnum : leftHandEnum;
			AActor* heldWeapon = (handEnum == VR_MAINHAND) ? player->ReadyWeapon : player->OffhandWeapon;
			if (heldWeapon == nullptr) continue;   // empty hand stays open (no curl)

			for (int fng = 0; fng < 5; fng++)
			{
				const bool isThumb = (fng == 4);
				const float deg = isThumb ? (float)vr_hand_grip_thumb : (float)vr_hand_grip_curl;
				if (deg == 0.f) continue;
				const FQuaternion curl = FQuaternion::AxisAngle(flexAxis, FAngle::fromDeg((double)deg));
				for (int seg = 0; seg < 3; seg++)
				{
					const int ji = fingerIdx[side][fng][seg];
					if (ji < 0 || (unsigned)ji >= player->vr_ik_pose.Size()) continue;
					FQuaternion bind = player->vr_ik_pose[ji].rotation;   // [5.0] FQuaternion
					FQuaternion out = bind * curl;   // local post-multiply: flex in the joint's own frame
					out.MakeUnit();
					player->vr_ik_pose[ji].rotation = out;
				}
			}
		}

		// [XR] FINGER TOUCH: a finger lifted off its sensor opens toward the bind pose (see the cvar
		// decl). Runs after the pose clip and the curl, so it lifts a posed finger too.
		if (vr_hand_touch_fingers && player->mo != nullptr)
		{
			enum { kTouchThumb = 1 << 0, kTouchIndex = 1 << 1 };   // mirrors FINGERTOUCH_THUMB / _INDEX (vk_openxrdevice.h)
			const float open = clamp((float)vr_hand_touch_open, 0.f, 1.f);
			for (int side = 0; side < 2 && open > 0.f; side++)
			{
				const int handEnum = (side == 0) ? rightHandEnum : leftHandEnum;
				const int bits = (handEnum == VR_MAINHAND) ? player->mo->FingerTouchMain : player->mo->FingerTouchOff;
				// finger 0 = index (trigger), 4 = thumb (face buttons / stick); -1 = still touching, leave it
				const int lifted[2] = { (bits & kTouchIndex) ? -1 : 0, (bits & kTouchThumb) ? -1 : 4 };
				for (int L = 0; L < 2; L++)
				{
					const int fng = lifted[L];
					if (fng < 0) continue;
					for (int seg = 0; seg < 3; seg++)
					{
						const int ji = fingerIdx[side][fng][seg];
						if (ji < 0 || (unsigned)ji >= player->vr_ik_pose.Size()) continue;
						TRS bind; model->GetJointBindTRS(ji, bind);
						FQuaternion q = FQuaternion::SLerp(player->vr_ik_pose[ji].rotation, bind.rotation, open);
						q.MakeUnit();
						player->vr_ik_pose[ji].rotation = q;
					}
				}
			}
		}
	}

	// [XR] HIDDEN BONES: collapse the listed joints (and, through inheritance, everything under them)
	// to zero scale. This is how the head disappears from inside the body without removing a vertex.
	for (unsigned h = 0; h < cachedHidden.Size(); h++)
	{
		const int hj = cachedHidden[h];
		if (hj >= 0 && (unsigned)hj < player->vr_ik_pose.Size())
			player->vr_ik_pose[hj].scaling = FVector3(0.f, 0.f, 0.f);
	}

	player->vr_ik_active = anySolved;

	// [XR] TEMP probe (throttled): confirms the IK reached the solve and what it produced, so we can
	// see WHY the arms do/don't move without another blind round-trip. Remove once arms track.
	{
		static int s_ikDbg = 0;
		if (s_ikDbg < 20)
		{
			s_ikDbg++;
			Printf("[VRIK] joints=%d haveTarget=%d,%d anySolved=%d poseSize=%d\n",
				model ? model->GetJointCount() : -1, (int)haveTarget[0], (int)haveTarget[1],
				(int)anySolved, player->vr_ik_pose.Size());
		}
	}

	// ---- push the solved pose to the render path (the missing native glue) ----
	// vr_ik_pose is a private playsim buffer; the renderer ONLY reads
	// modelData->proceduralPose (r_data/models.cpp ProcessModelFrame). Copy it across and flip
	// useProceduralPose so ProcessModelFrame consumes the solved bones this frame. On a
	// tic with no valid solve, clear the flag so the avatar reverts to its bind pose.
	// modelData is guaranteed non-null here (early-return gate above). NOTE: the avatar
	// MODELDEF block must also carry the `modelsareattachments` keyword for these bones to
	// upload to the GPU (models.cpp RenderModelFrame) -- +DECOUPLEDANIMATIONS alone fails on a 0-anim IQM.
	if (anySolved)
	{
		modelData->proceduralPose = player->vr_ik_pose;   // TArray<TRS> copy; identical element type
		modelData->useProceduralPose = true;
	}
	else
	{
		modelData->useProceduralPose = false;
	}
}

//==========================================================================
//
// VR_UpdateBodyFacing (was inline in P_PlayerThink, directly after
// VR_UpdateArmIK)
//
// Decouple the body facing from the HMD: the pawn yaw follows the headset, so
// without this the whole torso spins when you turn your head ("no neck").
// Only the body MODEL render reads vr_body_facing_yaw (models.cpp, isVRBody).
//
//==========================================================================

// ===================================================================================================
void VR_UpdateBodyFacing(player_t* player)
{
	if (!player || !player->mo) return;

	const double headYaw = player->mo->Angles.Yaw.Degrees();
	if (!player->vr_body_facing_valid)
	{
		player->vr_body_facing_yaw = (float)headYaw;
		player->vr_body_facing_valid = true;
		return;
	}

	// Where the body wants to face this tic.
	double wantYaw = (double)player->vr_body_facing_yaw;
	bool haveWant = false;
	if ((int)vr_body_facing_mode == 1)
	{
		// From the hands: the headset -> controller-midpoint direction, in the Doom XY plane.
		const VRMode* vm = VRMode::GetVRModeCached(true);
		if (!vm->IsVR()) vm = VRMode::GetVRModeCached(false);
		VSMatrix a, b;
		if (vm->GetWeaponTransform(&a, VR_MAINHAND) && vm->GetWeaponTransform(&b, VR_OFFHAND))
		{
			const FLOATTYPE* ma = a.get(); const FLOATTYPE* mb = b.get();
			// GL layout: [12]=x, [14]=Doom y
			const double mx = 0.5 * (ma[12] + mb[12]) - player->mo->HmdPos.X;
			const double my = 0.5 * (ma[14] + mb[14]) - player->mo->HmdPos.Y;
			if (mx * mx + my * my > 4.0)   // hands at least 2 units out from the head
			{
				wantYaw = atan2(my, mx) * (180.0 / M_PI);
				haveWant = true;
			}
		}
	}
	if (!haveWant)
	{
		double diff = headYaw - (double)player->vr_body_facing_yaw;
		while (diff >  180.0) diff -= 360.0;
		while (diff < -180.0) diff += 360.0;
		const double deadzone = clamp((double)vr_body_facing_deadzone, 0.0, 180.0);   // deg the head can turn before the body starts to follow
		if (diff >  deadzone)      wantYaw = headYaw - deadzone;
		else if (diff < -deadzone) wantYaw = headYaw + deadzone;
	}

	// Turn toward it no faster than vr_body_facing_rate degrees per second (0 = snap).
	double step = wantYaw - (double)player->vr_body_facing_yaw;
	while (step >  180.0) step -= 360.0;
	while (step < -180.0) step += 360.0;
	const double rate = (double)vr_body_facing_rate;
	if (rate > 0.0)
	{
		const double maxStep = rate / (double)TICRATE;
		if (step >  maxStep) step =  maxStep;
		if (step < -maxStep) step = -maxStep;
	}
	double yaw = (double)player->vr_body_facing_yaw + step;
	while (yaw >  180.0) yaw -= 360.0;
	while (yaw < -180.0) yaw += 360.0;
	player->vr_body_facing_yaw = (float)yaw;
}
