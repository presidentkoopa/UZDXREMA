/*
** models.cpp
**
** General model handling code
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "filesystem.h"
#include "cmdlib.h"
#include "sc_man.h"
#include "m_crc32.h"
#include "c_console.h"
#include "g_game.h"
#include "doomstat.h"
#include "g_level.h"
#include "r_state.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "r_utility.h"
#include "models.h"
#include "model_kvx.h"
#include "i_time.h"
#include "texturemanager.h"
#include "modelrenderer.h"
#include "actor.h"
#include "actorinlines.h"
#include "v_video.h"
#include "hw_bonebuffer.h"
#include "hw_vrmodes.h"


#ifdef _MSC_VER
#pragma warning(disable:4244) // warning C4244: conversion from 'double' to 'float', possible loss of data
#endif

CVAR(Bool, gl_interpolate_model_frames, true, CVAR_ARCHIVE)

// RS FORK -- HUD BONE ANCHORING.
//
// Where a psprite's model resolves each of its requested bones, so a later
// layer can be drawn there. Keyed by (layer id, bone name) and stamped with a
// frame counter: a target that stops being drawn must not leave an attachment
// frozen at its last known position, so a stale stamp reads as "no anchor" and
// the layer falls back to its own placement.
// mat is what an anchored layer is drawn with. offset is the same bone
// expressed as a displacement from the weapon's ORIGIN, in the model's own
// axes, already multiplied by the scale the model is being drawn at -- which is
// the form script can use.
//
// Script cannot see any of this otherwise, and the cost of that has been high:
// every grab point in the mods is a hand-guessed distance ("the pump is about
// sixteen units forward"), which is wrong the moment a weapon is rescaled or
// repositioned, and wrong in a way that presents as the grab silently never
// firing. The engine has always known exactly where the pump is.
struct HudAnchorEntry { VSMatrix mat; DVector3 offset; uint64_t frame; };
static TMap<uint64_t, HudAnchorEntry> g_hudAnchors;
static uint64_t g_hudAnchorFrame = 0;

// The transform of the model currently being drawn, so a bone matrix -- which
// is model-local -- can be combined into something the anchored layer can use
// directly.
static VSMatrix g_hudAnchorSource;

static inline uint64_t HudAnchorKey(int layer, FName bone)
{
	return (uint64_t(uint32_t(layer)) << 32) | uint32_t(bone.GetIndex());
}

void HudAnchor_BeginFrame()
{
	g_hudAnchorFrame++;

	// The shared table above guards staleness with the frame stamp, but the
	// copies written onto the psprites themselves have no such guard, and a
	// latched AnchorBoneLive is worse than none: a grab test would keep firing at
	// a weapon that is no longer drawn, at wherever it was last seen. Cleared
	// here, so only a bone actually published this frame reads as live.
	player_t *player = &players[consoleplayer];
	for (DPSprite *q = player->psprites; q != nullptr; q = q->GetNext())
	{
		q->AnchorBoneLive = false;
	}
}

bool HudAnchor_Get(int layer, FName bone, VSMatrix &out)
{
	auto *e = g_hudAnchors.CheckKey(HudAnchorKey(layer, bone));
	if (!e || e->frame != g_hudAnchorFrame) return false;
	out = e->mat;
	return true;
}

// Where a bone sits relative to the weapon's origin, in the model's own axes
// and in map units. Zero if that bone was not drawn this frame -- a stale
// answer is worse than no answer, because a grab test cannot tell the two
// apart and would keep firing at a weapon that is no longer on screen.
//
// The requests drive what gets published: a bone nobody has asked for is never
// stored, so a mod must anchor something to a bone (or ask for it) before this
// returns anything for it.
bool HudAnchor_GetOffset(int layer, FName bone, DVector3 &out)
{
	auto *e = g_hudAnchors.CheckKey(HudAnchorKey(layer, bone));
	if (!e || e->frame != g_hudAnchorFrame) { out = DVector3(0, 0, 0); return false; }
	out = e->offset;
	return true;
}

// Publish whichever bones another layer has asked this one for. Driven by the
// requests rather than storing every bone, because a rigged weapon has dozens
// and almost none of them are ever anchored to.
static void HudAnchor_Store(const DPSprite *psp, FModel *mdl, const TArray<VSMatrix> &bones)
{
	if (!psp || !psp->Owner || !mdl) return;

	for (DPSprite *q = psp->Owner->psprites; q != nullptr; q = q->GetNext())
	{
		if (q->AnchorLayer != psp->GetID() || q->AnchorBone == NAME_None) continue;

		int j = mdl->FindJoint(q->AnchorBone);
		if (j < 0 || (unsigned)j >= bones.Size()) continue;

		HudAnchorEntry e;
		e.mat = g_hudAnchorSource;
		e.mat.multMatrix(bones[j]);
		e.frame = g_hudAnchorFrame;

		// The bone's own translation is its position in MODEL space, and the
		// model origin is where the weapon sits, so that translation is already
		// the offset script wants -- it only needs the scale the model is drawn
		// at, which is the length of a basis column of the model's transform.
		//
		// Taken from the matrix rather than recomputed from the cvars and the
		// MODELDEF: several things multiply into that scale and reading it back
		// off the result cannot drift out of step with them.
		{
			// THE BIND POSE MATTERS. bones[j] is a SKINNING matrix: it maps
			// bind-pose space to posed space, so reading its translation column
			// gives where the MODEL ORIGIN lands, not where the bone is. At
			// rest every skinning matrix is identity, so every bone reported
			// the same point -- the model origin -- and a magazine anchored to
			// the magwell spawned wherever the model origin happened to sit
			// (on the T77, right at the trigger).
			//
			// The bone's real position is that matrix applied to the joint's
			// BIND position, which is what GetJointPosition returns (an
			// absolute, parent-accumulated position, see models_iqm.cpp).
			const FVector3 bindPos = mdl->GetJointPosition(j);
			const FLOATTYPE bp[4] = { (FLOATTYPE)bindPos.X, (FLOATTYPE)bindPos.Y, (FLOATTYPE)bindPos.Z, (FLOATTYPE)1.0 };

			// Copy: bones is a const reference and multMatrixPoint is non-const.
			VSMatrix boneMat = bones[j];
			FLOATTYPE posed[4];
			boneMat.multMatrixPoint(bp, posed);

			const FLOATTYPE *sm = g_hudAnchorSource.get();
			const double sc = sqrt(sm[0]*sm[0] + sm[1]*sm[1] + sm[2]*sm[2]);
			e.offset = DVector3(posed[0] * sc, posed[1] * sc, posed[2] * sc);

			// Straight onto the psprite that asked. Script reads it from there,
			// so nothing on the game side ever touches this table.
			q->AnchorBonePos = e.offset;
			q->AnchorBoneLive = true;

			// The same bone as a world point, in the frame AttackPos and
			// OffhandPos are taken from. e.mat carries the weapon's full
			// object-to-world transform combined with the bone, and it is
			// applied to the joint's BIND position for the reason above -- not
			// read off the translation column, which would give the model
			// origin. The Y/Z swap is the usual convention: the matrix is
			// Y-up, the playsim is Z-up.
			FLOATTYPE world[4];
			e.mat.multMatrixPoint(bp, world);
			q->AnchorBoneWorld = DVector3(world[0], world[2], world[1]);

			// AND THE SAME CORRECTION ON THE MATRIX ITSELF.
			//
			// e.mat is what actually PLACES an anchored layer -- HudAnchor_Get
			// hands it straight to the renderer, which orthonormalises the
			// basis (stripping scale) and keeps the translation as-is. Its
			// translation column has the identical skinning-matrix problem
			// described above: at rest it is the MODEL ORIGIN, the same point
			// for every bone. So a hand anchored to a pistol's grip bone and a
			// hand anchored to its slide both landed at the model's origin --
			// which on the T77 is nowhere near either, and reads in the headset
			// as hands floating off the gun entirely.
			//
			// Fixing e.offset/AnchorBoneWorld earlier only fixed what SCRIPT
			// reads. This fixes what the RENDERER draws, which is a separate
			// consumer of the same wrong number -- and it fixes every anchored
			// hand on every weapon, not just this one, since nothing about it
			// is T77-specific.
			//
			// Rotation is deliberately untouched: only the translation was
			// ever wrong, and orientation does not depend on which point of
			// the bone is used.
			{
				FLOATTYPE fixed[16];
				memcpy(fixed, e.mat.get(), sizeof(fixed));
				fixed[12] = world[0];
				fixed[13] = world[1];
				fixed[14] = world[2];
				e.mat.loadMatrix(fixed);
			}

			// The same matrix's ROTATION, decomposed to the playsim's
			// yaw/pitch/roll. Read here rather than in script because the
			// matrix is right here and ZScript's Quat cannot rotate a vector,
			// so script has no honest way to derive this itself.
			//
			// Basis columns, with the same Y-up -> Z-up swap the translation
			// above uses: the matrix's X axis is forward, its Z axis is the
			// playsim's Y, and its Y axis is the playsim's Z. Scale is divided
			// out first -- the model is drawn scaled and a scaled basis would
			// give wrong angles.
			{
				// The rotation still comes from the matrix's basis columns --
				// only the TRANSLATION needed the bind-pose correction above,
				// since orientation does not depend on which point is used.
				const FLOATTYPE *wm = e.mat.get();

				DVector3 fwd  (wm[0], wm[2], wm[1]);
				DVector3 side (wm[8], wm[10], wm[9]);
				DVector3 up   (wm[4], wm[6], wm[5]);

				const double flen = fwd.Length();
				const double slen = side.Length();
				const double ulen = up.Length();
				if (flen > 0) fwd /= flen;
				if (slen > 0) side /= slen;
				if (ulen > 0) up /= ulen;

				const double yaw   = atan2(fwd.Y, fwd.X);
				const double pitch = asin(clamp(-fwd.Z, -1.0, 1.0));
				const double roll  = atan2(side.Z, up.Z);

				q->AnchorBoneAngles = DVector3(
					yaw   * (180.0 / M_PI),
					pitch * (180.0 / M_PI),
					roll  * (180.0 / M_PI));
			}
		}
		g_hudAnchors.Insert(HudAnchorKey(psp->GetID(), q->AnchorBone), e);
	}
}

// RS FORK -- pose pipeline diagnostics. Traces an explicitly addressed model
// frame from the psprite through to the bone calculation, which is otherwise
// invisible: a pose that never applies looks exactly like a pose that was
// never set. Prints only on change, so a held pose reports once.
CVAR(Bool, vr_pose_debug, false, 0)
// RS FORK -- MODEL DIAGNOSTICS.
//
// Every check here cost a headset session to find by eye, and every one of them
// presents as something misleading: a model with no frames renders as missing
// textures, a packed alpha channel renders as a half-transparent gun. None of
// them look like what they are, which is exactly why they are worth reporting.
//
// Checked as a model is first drawn, and reported once each.
CVAR(Bool, vr_validate, false, 0)
CVAR(Bool, vr_spatialreport, false, 0)

static TMap<uint64_t, bool> g_validateSeen;

static bool ValidateOnce(const void *key, int slot)
{
	uint64_t k = (uint64_t)(intptr_t)key * 16 + slot;
	if (g_validateSeen.CheckKey(k)) return false;
	g_validateSeen.Insert(k, true);
	return true;
}

static void ValidateHudModel(const FSpriteModelFrame *smf, FModel *mdl, const DPSprite *psp, unsigned smf_flags)
{
	if (!vr_validate || !mdl || !smf) return;

	const char *who = (psp && psp->Caller != nullptr)
		? psp->Caller->GetClass()->TypeName.GetChars()
		: "unknown";

	// A bone-weighted mesh with no pose has nothing to evaluate its weights
	// against, so most of it collapses. It reads as missing geometry or missing
	// textures, never as an animation problem.
	if (mdl->NumJoints() > 0 && mdl->NumFrames() == 0 && ValidateOnce(smf, 0))
	{
		Printf(TEXTCOLOR_ORANGE "[MODEL] %s: %d bones but ZERO frames. A skinned model with no pose collapses; "
			"it looks like missing geometry or missing textures. Export a bind pose.\n",
			who, mdl->NumJoints());
	}

	// Packed PBR maps carry roughness or gloss in alpha, not opacity. The model
	// is alpha-tested against that channel and most of it is discarded.
	if (!(smf_flags & MDL_IGNORESKINALPHA) && ValidateOnce(smf, 1))
	{
		FGameTexture *tex = nullptr;
		if (smf->skinIDs.Size() > 0 && smf->skinIDs[0].isValid())
			tex = TexMan.GetGameTexture(smf->skinIDs[0], true);
		else if (smf->surfaceskinIDs.Size() > 0 && smf->surfaceskinIDs[0].isValid())
			tex = TexMan.GetGameTexture(smf->surfaceskinIDs[0], true);

		if (tex && tex->GetTranslucency())
		{
			Printf(TEXTCOLOR_ORANGE "[MODEL] %s: skin has an alpha channel and IgnoreSkinAlpha is not set. "
				"If that alpha is packed data rather than opacity, most of the model is alpha-tested away "
				"and reads as half transparent.\n", who);
		}
	}
}

EXTERN_CVAR(Bool, r_drawvoxels)

// [BB] BODY-AXIS CORRECTION FOR HELD VOXELS.
//
// A voxel pack's AngleOffset corrects which way the mesh FACES. That is not
// the same thing as putting its long axis on +X, which is what the pitch and
// roll rotations assume. When the two disagree by a quarter turn, a wrist roll
// comes out as a fore/aft tilt -- the mesh is being rolled about an axis that
// runs across it rather than along it.
//
// Which way a given pack is off is not knowable from the data; it depends on
// how its author authored the voxels. So this is a dial, not a constant. It
// wraps the pitch/roll pair only, leaving yaw and the mesh's resting facing
// alone, and applies ONLY to actors with VoxelOverride set -- scenery voxels
// standing on a floor never see it.
// Negative = derive it from the pack's own angleoffset, which is right for
// every pack examined so far. 0 and up override with a literal quarter turn.
//
// RENAMED from vr_voxel_bodyyaw, 2026-08-29, and the rename IS the fix. That
// cvar shipped once with a default of 0, which archived a 0 into the config of
// anyone who ran that build. Changing the default afterwards did nothing for
// them -- an archived value always beats a new default -- so the correction sat
// switched off and read as a broken feature across three separate test runs,
// with the log faithfully reporting bodyyaw=0.0 every time. A new name has
// nothing archived against it, so the default finally applies.
//
// Generalises: changing the default of a CVAR_ARCHIVE cvar only ever affects
// someone who has never run a build that wrote one.
CVAR(Float, vr_voxel_rollaxis, -1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Held-voxel orientation trace. On by default while this is being worked out;
// it only ever prints for an actor that is actually in a hand, and only once a
// second, so it is quiet unless something is held.
CVAR(Bool, vr_voxel_debug, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Int, vr_control_scheme)
EXTERN_CVAR(Float, vr_weaponScale)
EXTERN_CVAR(Float, vr_3dweaponOffsetX);
EXTERN_CVAR(Float, vr_3dweaponOffsetY);
EXTERN_CVAR(Float, vr_3dweaponOffsetZ);
EXTERN_CVAR(Float, vr_hand_ofs_x);
EXTERN_CVAR(Float, vr_hand_ofs_y);
EXTERN_CVAR(Float, vr_hand_ofs_z);
EXTERN_CVAR(Float, vr_hand_yaw);
EXTERN_CVAR(Float, vr_hand_pitch);
EXTERN_CVAR(Float, vr_hand_roll);
EXTERN_CVAR(Float, vr_offhand_ofs_x);
EXTERN_CVAR(Float, vr_offhand_ofs_y);
EXTERN_CVAR(Float, vr_offhand_ofs_z);
EXTERN_CVAR(Float, vr_offhand_yaw);
EXTERN_CVAR(Float, vr_offhand_pitch);
EXTERN_CVAR(Float, vr_offhand_roll);

// ======================= [XR] VR body avatar (playsim/vr_armik.cpp) =======================
#include "vr_armik.h"
EXTERN_CVAR(Int, vr_mode)   // [XR] real VR-on check; vrmode->IsVR() lies (returns 0) in the render path

CVAR(Float, vr_body_size, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)   // [XR] life size; the body's own units are map units
CVAR(Float, vr_body_z,     0.0f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] Body position in its own facing frame: forward/back and left/right of the pawn, map units.
CVAR(Float, vr_body_forward, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] Body thickness (left-right and front-back) on top of the size: 1.0 = the rig's own proportions.
CVAR(Float, vr_body_width,   1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_side,    0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_yaw,   90.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool,  vr_body_autofit,  true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_headroom, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_neck_height, 63.64f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] NECK-TO-HMD placement (what FRIK's setBodyUnderHMD does). The body keeps whatever scale it has --
// so its arm length, i.e. the player's reach, is untouched -- and is slid vertically so its neck stump
// lands at the live eye height. Seated play is the case this exists for: the eye is far below a standing
// neck, and scaling the body down to meet it (vr_body_autofit) shrinks the arms below the player's real
// reach, which the arm-IK then has to stretch. vr_body_neck_eye_gap is how far the eyes sit above the
// neck stump on the rig, in model units (marine: ~3.5). Composes with autofit and vr_body_z.
CVAR(Bool,  vr_body_neck_to_hmd,  true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_body_neck_eye_gap, 3.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// [XR] HEAD PIVOT. Your eyes are not your neck: they sit above and in front of the joint the head
// turns on, so nodding swings them through an arc while the neck stays where it is. Placing the
// body from the raw eye height therefore moved the whole avatar every time the player looked down
// -- and the old defence, sampling the height only while the view was near level, only narrowed
// the window it happened in.
//
// So reconstruct the neck joint from the headset POSE instead of its position alone: back along the
// head's own forward axis, down its own up axis. That point is invariant to head rotation, which is
// the property the placement actually wanted. The two distances are the player's own anatomy, in
// map units (about 34 to the metre): 8cm back, 10cm down.
CVAR(Bool,  vr_body_head_pivot, true,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
EXTERN_CVAR(Bool, vr_body_crouch)   // playsim/vr_armik.cpp -- see the standing-height note below
CVAR(Float, vr_body_neck_back,  2.7f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // eye -> neck, along head-forward
CVAR(Float, vr_body_neck_drop,  3.4f,  CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // eye -> neck, along head-up

// [XR] The live body-fit scale actually applied to the local avatar this frame (autofit smoothed
// value, or the manual vr_body_size when autofit is off). Published so the playsim arm-IK
// (vr_armik.cpp VR_UpdateArmIK) can divide the world hand target by the SAME scale the renderer used,
// converting the target from rendered-body space into the unscaled baseframe the IK solves in.
// Written on the render thread, read on the playsim thread: a plain float, at worst one frame stale,
// and the value is heavily smoothed, so no sync is needed.
float g_xr_vrBodyRenderScale = 0.70f;

// [XR] The EXACT finalized objectToWorldMatrix used to draw the local VR body this frame, published so
// the playsim arm-IK (VR_UpdateArmIK) can INVERT the renderer's OWN transform instead of
// hand-rebuilding world->model-local math. baseframe-space -> GL-world is F = objectToWorldMatrix*swapYZ
// (boneData==I at bind), so the IK does target_baseframe = swapYZ * objectToWorldMatrix^-1 * controller_GL.
// This captures the drawn yaw (vr_body_facing/vr_body_yaw), vr_body_z, AND the Y/Z-swapped bodyScale
// factors exactly, so the manual un-yaw/axis-remap/feet-subtract/scale-divide all become obsolete.
// Written on the render thread, read on the playsim thread: at worst one frame stale, the pose is smooth,
// same lock-free contract as g_xr_vrBodyRenderScale above. Valid flag guards the pre-first-render frame.
VSMatrix g_xr_vrBodyObjectToWorld;
bool     g_xr_vrBodyObjToWorldValid = false;
// [XR] Diagnostic: how many times the renderer consumed a procedural pose. Read by the playsim probe.
int      g_xr_vrRenderProcHits = 0;

// [XR] Is this actor the local player's VR body? The designated body actor when a mod set one,
// else the console player's pawn -- the original test was `actor == players[consoleplayer].mo`.
static inline bool VR_IsBodyActor(const AActor* actor)
{
	return (int)vr_mode != 0 && actor != nullptr && actor == VR_BodyActor(&players[consoleplayer]);
}
// ==========================================================================================

extern TDeletingArray<FVoxel *> Voxels;
extern TDeletingArray<FVoxelDef *> VoxelDefs;

// [RS FORK] Upstream added the 'ticFrac' parameter (the animation clock now comes
// from AActor::GetModelTimer() + ticFrac instead of Level->totaltime + I_GetTimeFrac()).
// The fork's trailing 'psp' parameter is kept, defaulted so upstream's psp-less
// callers (RenderModel here, AActor::CalcBones in p_mobj.cpp) still compile.
void RenderFrameModels(FModelRenderer* renderer, FLevelLocals* Level, const FSpriteModelFrame *smf, const FState* curState, int curTics, double ticFrac, FTranslationID translation, AActor* actor, const DPSprite* psp = nullptr);

void RenderModel(FModelRenderer *renderer, float x, float y, float z, FSpriteModelFrame *smf, AActor *actor, double ticFrac)
{
	int smf_flags = smf->getFlags(actor->modelData);
	FTranslationID translation = NO_TRANSLATION;
	if (!(smf_flags & MDL_IGNORETRANSLATION))
		translation = actor->Translation;

	VSMatrix objectToWorldMatrix = smf->ObjectToWorldMatrix(actor, x, y, z, ticFrac);

	// [XR] Publish the finalized VR-body transform so the arm-IK can invert F = objectToWorldMatrix*swapYZ.
	// ObjectToWorldMatrix is complete here (every translate/rotate/scale, pixel stretch included, is baked
	// in) and it is exactly the matrix the GPU uses as the model matrix -- so its inverse is exact.
	if (VR_IsBodyActor(actor))
	{
		g_xr_vrBodyObjectToWorld = objectToWorldMatrix; g_xr_vrBodyObjToWorldValid = true;
		// [XR] Solve the arms for THIS frame, against this exact matrix and the controllers as they are
		// right now -- the same pose the weapon is drawn with -- before the body's bones are read.
		VR_UpdateArmIKFrame(&players[consoleplayer], screen->FrameTime);
	}

	const DVector2 scale = actor->InterpolatedScale(ticFrac);
	float scaleFactorX = scale.X * smf->xscale;
	float scaleFactorY = scale.X * smf->yscale;
	float scaleFactorZ = scale.Y * smf->zscale;
	float orientation = scaleFactorX * scaleFactorY * scaleFactorZ;

	renderer->BeginDrawModel(actor->RenderStyle, smf_flags, objectToWorldMatrix, orientation < 0);
	RenderFrameModels(renderer, actor->Level, smf, actor->state, actor->tics, ticFrac, translation, actor);
	renderer->EndDrawModel(actor->RenderStyle, smf_flags);
}

// VR_WORLDACTOROFFSET -- where a world actor's MODEL actually is, in the world.
//
// THE GAP THIS FILLS.
//
// TransformByNamedBone answers "where is this bone" in MODEL space. It applies
// the bone matrix and stops. It never sees the object-to-world matrix -- the
// actor's position, the MODELDEF scale and offsets and angle corrections, or,
// for a followed model, the entire controller transform loaded by
// GetWeaponTransform. So script could ask where MARKER_grip was and get an
// answer in a space with no relation to the room.
//
// That is why every attempt at seating a world model has come down to a human
// finding an offset by eye on a slider. There was no way to ask.
//
// With this, seating is arithmetic and not taste:
//
//     grip = gun.ModelPointToWorld(gun.TransformByNamedBone('MARKER_grip', ...))
//     palm = hand.ModelPointToWorld(hand.TransformByNamedBone('HANDPALM_joint', ...))
//     gun.SetOrigin(gun.Pos + (palm - grip), false)
//
// and the firing line is the returned forward axis -- the direction the barrel
// is actually drawn pointing, not a reconstruction from Euler angles.
//
// Returns position, forward, up. Forward and up are unit vectors in world space,
// taken from the matrix's own basis, so they carry every correction the model
// received including ones nothing in script knows about.
static void ModelWorldTransform(AActor *self, double mx, double my, double mz,
	DVector3 &posOut, DVector3 &fwdOut, DVector3 &upOut)
{
	posOut = DVector3(0, 0, 0);
	fwdOut = DVector3(1, 0, 0);
	upOut  = DVector3(0, 0, 1);
	if (self == nullptr) return;

	// The frame the renderer would pick for this actor right now. Decoupled
	// actors resolve through BaseSpriteModelFrames, which is why an actor
	// without BaseFrame answers nothing here -- the same reason it draws nothing.
	FSpriteModelFrame *smf = FindModelFrame(self, self->sprite, self->frame, false);
	if (smf == nullptr) return;

	const double ticFrac = I_GetTimeFrac();
	VSMatrix m = smf->ObjectToWorldMatrix(self,
		(float)self->X(), (float)self->Y(), (float)self->Z(), ticFrac);

	// Column-major, the way VSMatrix stores it: [0..2] is axis X, [4..6] axis Y,
	// [8..10] axis Z, [12..14] the translation.
	const FLOATTYPE *v = m.get();
	auto xf = [&](double a, double b, double c) {
		return DVector3(
			v[0]*a + v[4]*b + v[8]*c  + v[12],
			v[1]*a + v[5]*b + v[9]*c  + v[13],
			v[2]*a + v[6]*b + v[10]*c + v[14]);
	};
	posOut = xf(mx, my, mz);

	// Axes as differences from the transformed origin, so translation cancels
	// and any scale baked into the matrix normalises away.
	const DVector3 org = xf(0, 0, 0);
	DVector3 fx = xf(1, 0, 0) - org;
	DVector3 fy = xf(0, 1, 0) - org;
	if (fx.Length() > 1e-9) fwdOut = fx / fx.Length();
	if (fy.Length() > 1e-9) upOut  = fy / fy.Length();
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, ModelPointToWorld, ModelWorldTransform)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(mx);
	PARAM_FLOAT(my);
	PARAM_FLOAT(mz);
	DVector3 pos, fwd, up;
	ModelWorldTransform(self, mx, my, mz, pos, fwd, up);
	if (numret > 2) ret[2].SetVector(up);
	if (numret > 1) ret[1].SetVector(fwd);
	if (numret > 0) ret[0].SetVector(pos);
	return numret;
}

// [XR] Where a joint of an actor's model is drawn this frame. Same matrix ModelPointToWorld uses; the
// joint's bind position (file space) goes through the file->drawn swap before the object matrix.
bool VR_ModelJointWorld(AActor* a, FName joint, const FVector3& offsetModel, FVector3& outPosGL, VSMatrix& outObjToWorld)
{
	if (a == nullptr) return false;
	FSpriteModelFrame* smf = FindModelFrame(a, a->sprite, a->frame, false);
	if (smf == nullptr || smf->modelIDs.Size() == 0 || smf->modelIDs[0] < 0 || (unsigned)smf->modelIDs[0] >= Models.Size()) return false;
	FModel* mdl = Models[smf->modelIDs[0]];
	if (mdl == nullptr) return false;
	const int j = (joint == NAME_None) ? -1 : mdl->FindJointByNameCI(joint);
	if (j < 0 && joint != NAME_None) return false;
	FVector3 p = ((j >= 0) ? mdl->GetJointPosition(j) : FVector3(0.f, 0.f, 0.f)) + offsetModel;   // file space
	outObjToWorld = smf->ObjectToWorldMatrix(a, (float)a->X(), (float)a->Y(), (float)a->Z(), I_GetTimeFrac());
	const FLOATTYPE* v = outObjToWorld.get();
	// drawn model space is the file space with Y and Z swapped
	const double mx = p.X, my = p.Z, mz = p.Y;
	outPosGL = FVector3(
		(float)(v[0]*mx + v[4]*my + v[8]*mz  + v[12]),
		(float)(v[1]*mx + v[5]*my + v[9]*mz  + v[13]),
		(float)(v[2]*mx + v[6]*my + v[10]*mz + v[14]));
	return true;
}

bool VR_ModelWorldToJointOffset(AActor* a, FName joint, const DVector3& worldDoom, FVector3& outOffsetModel)
{
	FVector3 jointGL; VSMatrix m;
	if (!VR_ModelJointWorld(a, joint, FVector3(0.f, 0.f, 0.f), jointGL, m)) return false;
	VSMatrix inv;
	if (!m.inverseMatrix(inv)) return false;
	FLOATTYPE p[4] = { (FLOATTYPE)worldDoom.X, (FLOATTYPE)worldDoom.Z, (FLOATTYPE)worldDoom.Y, (FLOATTYPE)1 };
	FLOATTYPE o[4];
	inv.multMatrixPoint(p, o);
	// drawn model space -> file space (Y/Z swap), minus the joint's file-space position
	FSpriteModelFrame* smf = FindModelFrame(a, a->sprite, a->frame, false);
	FModel* mdl = (smf && smf->modelIDs.Size() > 0 && smf->modelIDs[0] >= 0) ? Models[smf->modelIDs[0]] : nullptr;
	const int j = mdl ? mdl->FindJointByNameCI(joint) : -1;
	FVector3 jp = (j >= 0) ? mdl->GetJointPosition(j) : FVector3(0.f, 0.f, 0.f);
	outOffsetModel = FVector3((float)o[0], (float)o[2], (float)o[1]) - jp;
	return true;
}

double VR_ActorFacing(AActor* a)
{
	if (a == nullptr) return 0.0;
	const player_t* bp = &players[consoleplayer];
	if ((int)vr_mode != 0 && a == VR_BodyActor(&players[consoleplayer]) && bp->vr_body_facing_valid)
		return (double)bp->vr_body_facing_yaw;
	return a->Angles.Yaw.Degrees();
}

// PLACEMENT CVARS ARE `user` CVARS, AND FindCVar CANNOT READ THOSE.
//
// FindCVar hands back the raw FBaseCVar. For a CVAR_USERINFO cvar that object
// is not where the value lives: c_cvars.cpp's own GetCVar exists to say so and
// redirects through callbacks->GetUserCVar(playernum, name) for precisely this
// case. Read the raw one and you get 0.
//
// Which is the worst answer available, because 0 is a legal-looking number.
// Every guard downstream is `if (v > 0)`, so a scale silently stays 1 and an
// offset silently stays 0 -- the model draws exactly as if every slider were
// centred, moving a slider does nothing, and nothing is logged to say the value
// never arrived. It is why the drawn reach volume came out a fixed sphere
// rather than the oval its three separate semi-axes describe, and why the
// offsets could not move it.
//
// Returns false only when the cvar genuinely does not exist, so a caller keeps
// its own default instead of being handed a zero.
static bool GetPlacementCVar(const char *name, float &out)
{
	FBaseCVar *cv = GetCVar(consoleplayer, name);
	if (cv == nullptr) return false;
	out = (float)cv->GetGenericRep(CVAR_Float).Float;
	return true;
}

VSMatrix FSpriteModelFrame::ObjectToWorldMatrix(AActor * actor, float x, float y, float z, double ticFrac)
{
	int smf_flags = getFlags(actor->modelData);

	// [BB] A VOXEL ASKED FOR BY HAND TURNS WITH THE HAND.
	//
	// An actor with VoxelOverride set has been switched to its voxel for a
	// reason -- something is holding it, and a held thing has to answer the
	// wrist. But pitch and roll are opt-in per model definition, and no voxel
	// pack in the wild sets them: they were authored for scenery standing on a
	// floor, where the only meaningful rotation is yaw. The pack this was
	// written against declares AngleOffset on all 74 entries and
	// UseActorPitch/UseActorRoll on none of them, which is typical.
	//
	// Forcing both here rather than asking authors to re-tag their packs, and
	// doing it on a LOCAL copy of the flags rather than on the shared
	// FSpriteModelFrame, so nothing leaks to the same voxel drawn elsewhere in
	// the level. Costs one OR on actors that have the field set and nothing at
	// all on those that do not.
	// MDL_VOXELBODYAXIS rides along so the matrix overload below -- which is
	// handed flags and no actor -- knows this one is held.
	if (actor->VoxelOverride) smf_flags |= MDL_USEACTORPITCH | MDL_USEACTORROLL | MDL_VOXELBODYAXIS;

	// The same opt-in without the voxel, for an actor wearing a model it does
	// not own -- a holstered weapon above all. See the field note in actor.h.
	// No MDL_VOXELBODYAXIS here: the body-axis correction undoes a VOXEL pack's
	// angleoffset, and a borrowed MODELDEF's offsets are already the ones its
	// own weapon is drawn with.
	if (actor->ForceModelAngles) smf_flags |= MDL_USEACTORPITCH | MDL_USEACTORROLL;

	// [XR] RENDER ATTACHMENT: this actor is drawn where a joint of its parent's model is, this
	// frame, from the parent's live matrix. Position comes from the joint (+ offset); heading is the
	// parent's facing plus the attachment's own angles. See the field note in actor.h.
	bool attached = false;
	DRotator attachAngles;
	{
		AActor* par = actor->RenderAttachParent;
		if (par != nullptr && !(par->ObjectFlags & OF_EuthanizeMe) && par != actor)
		{
			FVector3 gl; VSMatrix pm;
			if (VR_ModelJointWorld(par, actor->RenderAttachBone, actor->RenderAttachOffset, gl, pm))
			{
				x = gl.X; y = gl.Z; z = gl.Y;   // GL (x, up, y) -> Doom (x, y, z)
				attachAngles = DRotator(actor->RenderAttachAngles.Pitch, DAngle::fromDeg(VR_ActorFacing(par)) + actor->RenderAttachAngles.Yaw, actor->RenderAttachAngles.Roll);
				attached = true;
				smf_flags |= MDL_USEACTORPITCH | MDL_USEACTORROLL;
			}
		}
	}

	// [BB] HELD-VOXEL DIAGNOSTIC.
	//
	// Which quarter turn a pack is off by is not something anyone should have
	// to find by feel in a headset, and it is not guessable from the pack
	// either -- it depends on how its author laid the voxels out. But it IS
	// derivable from the three model offsets against the three actor angles,
	// and both of those are right here.
	//
	// Throttled to once a second per actor rather than once per draw: this runs
	// on the render path, which is called per eye, so an unthrottled Printf
	// would be two lines a frame and would itself cost frametime.
	if (actor->VoxelOverride && vr_voxel_debug)
	{
		static const AActor *lastActor = nullptr;
		static int lastTic = -1000;
		if (actor != lastActor || (gametic - lastTic) > TICRATE)
		{
			lastActor = actor;
			lastTic = gametic;
			Printf("[RSVOX] %s  modeloffsets angle=%.1f pitch=%.1f roll=%.1f  |  actor yaw=%.1f pitch=%.1f roll=%.1f  |  bodyyaw=%.1f  usepitch=%d useroll=%d pivotz=%.1f\n",
				actor->GetClass()->TypeName.GetChars(),
				angleoffset, pitchoffset, rolloffset,
				actor->Angles.Yaw.Degrees(), actor->Angles.Pitch.Degrees(), actor->Angles.Roll.Degrees(),
				// The DERIVED body yaw, not the raw cvar. The cvar is a
				// sentinel when negative and printing it raw said "bodyyaw=-1"
				// while the renderer was using 90 -- a trace that reports the
				// input rather than the decision is worse than none.
				(vr_voxel_rollaxis < 0.f) ? angleoffset : (float)vr_voxel_rollaxis,
				!!(smf_flags & MDL_USEACTORPITCH), !!(smf_flags & MDL_USEACTORROLL),
				// The PIVOT ACTUALLY USED, in map units. This used to print
				// MDL_USEROTATIONCENTER, which is a MODELDEF flag no voxel pack
				// sets -- so it read 0 on every line while the per-actor pivot
				// added alongside it was working fine. It described the old
				// mechanism, not the live one.
				float(actor->Height * 0.5));
		}
	}

	// Setup transformation.
	DRotator angles;

	if (attached)
		angles = attachAngles;
	else if (actor->renderflags & RF_INTERPOLATEANGLES) // [Nash] use interpolated angles
		angles = actor->InterpolatedAngles(ticFrac);
	else
		angles = actor->Angles;

	float angle = angles.Yaw.Degrees();
	float pitch = 0;
	float roll = 0;

	// [BB] Workaround for the missing pitch information.
	if ((smf_flags & MDL_PITCHFROMMOMENTUM))
	{
		const double x = actor->Vel.X;
		const double y = actor->Vel.Y;
		const double z = actor->Vel.Z;

		if (actor->Vel.LengthSquared() > EQUAL_EPSILON)
		{
			// [BB] Calculate the pitch using spherical coordinates.
			if (z || x || y) pitch = float(atan(z / sqrt(x*x + y*y)) / M_PI * 180);

			// Correcting pitch if model is moving backwards
			if (fabs(x) > EQUAL_EPSILON || fabs(y) > EQUAL_EPSILON)
			{
				if ((x * cos(angle * M_PI / 180) + y * sin(angle * M_PI / 180)) / sqrt(x * x + y * y) < 0) pitch *= -1;
			}
			else pitch = fabs(pitch);
		}
	}

	// Added MDL_USEACTORPITCH and MDL_USEACTORROLL flags processing.
	// If both flags MDL_USEACTORPITCH and MDL_PITCHFROMMOMENTUM are set, the pitch sums up the actor pitch and the velocity vector pitch.
	if (smf_flags & MDL_USEACTORPITCH)
	{
		double d = angles.Pitch.Degrees();
		if (smf_flags & MDL_BADROTATION) pitch += d;
		else pitch -= d;
	}
	if (smf_flags & MDL_USEACTORROLL) roll += angles.Roll.Degrees();

	// [Nash] take SpriteRotation into account
	angle += actor->SpriteRotation.Degrees();

	// [XR] Local VR body avatar: shrink ONLY the player's own body model, anchored at its feet (the
	// mesh origin -- the scale below is applied around model space 0,0,0 which for the marine is
	// between the feet). This drops the head below the HMD while the feet stay planted. Every other
	// actor renders unchanged. vr_body_z adds a vertical nudge for fine-tuning.
	const bool isVRBody = VR_IsBodyActor(actor);
	float bodyScale = 1.f;
	double bodyZ = 0.0, bodyOffX = 0.0, bodyOffY = 0.0;
	if (isVRBody)
	{
		bodyScale = vr_body_size;   // manual fallback
		const player_t* bodyPlayer = &players[consoleplayer];

		// CenterEyePos.Z - actor->Z() == the live HMD eye height above the floor in map units
		// (OpenXR floor-relative tracking, already * vr_vunits_per_meter). Smooth it so head-bob
		// doesn't pulse the body -- only the slow standing height tracks. Shared by both fits below.
		double eyeAboveFeet = r_viewpoint.CenterEyePos.Z - actor->Z();

		// [XR] HEAD PIVOT: replace the eye height with the NECK JOINT's height, reconstructed from the
		// headset's full pose (see the cvar decl). The head's own axes come out of GetHmdTransform in
		// GL layout -- up = +colY, forward = -colZ -- and their Y components are the world-Z ones, so
		//   neckZ = eyeZ - up.z * drop - fwd.z * back
		// which is constant through any amount of nodding. Feeding it in as "eye height minus the
		// neck->eye gap" keeps every line below unchanged, gap included.
		bool haveNeckPivot = false;
		if (vr_body_head_pivot)
		{
			auto hmdMode = VRMode::GetVRModeCached(true);
			VSMatrix hmdXf;
			if (hmdMode != nullptr && hmdMode->IsVR() && hmdMode->GetHmdTransform(&hmdXf))
			{
				const FLOATTYPE* hm = hmdXf.get();
				const double upZ = (double)hm[5], fwdZ = -(double)hm[9];
				const double neckZ = (double)hm[13] - upZ * (double)vr_body_neck_drop - fwdZ * (double)vr_body_neck_back;
				const double neckAboveFeet = neckZ - actor->Z();
				if (neckAboveFeet > 1.0)
				{
					// the block below places (neck + gap) at this value, so hand it the neck plus the gap
					eyeAboveFeet = neckAboveFeet + (double)vr_body_neck_eye_gap;
					haveNeckPivot = true;
				}
			}
		}
		static double smoothedEye = 0.0;

		// [XR] STANDING HEIGHT, and why crouch needs it.
		//
		// Neck-to-HMD slides the whole avatar down to keep its neck under the headset. That is right
		// for a player who is short, or seated, or has just changed their height -- and it is exactly
		// wrong for a player who has bent their knees, because it lowers the FEET through the floor
		// instead of bending anything. The two features were therefore cancelling: by the time the IK
		// looked for a crouch, the placement had already absorbed every unit of it, so the drop it
		// measured was always zero and the body stayed rigid however deep the player went.
		//
		// So separate the two questions. This is a high-water mark of standing height: it follows
		// upward at once (the player straightened, or is genuinely taller than we thought) and downward
		// only very slowly (a sustained lower posture is a new standing height, a squat behind cover is
		// not). The body is placed at THIS height, feet planted; the difference between it and the live
		// height is the crouch, handed to the IK to spend on the hips and the knees.
		static double standingEye = 0.0;
		// [XR] The eyes sit ahead of the neck pivot, so pitching the head moves them up and down by a
		// few units -- and the body followed. Sample the standing height only while the view is near
		// level, and follow it slowly; a nod then changes nothing, a real posture change still tracks.
		// [XR] With the neck pivot above there is nothing left for the pitch gate to defend against, and
		// the value can follow properly instead of crawling: a real crouch tracks in about a second
		// rather than ten. Without it, the old gate-and-crawl stands.
		const double viewPitch = (bodyPlayer->mo != nullptr) ? fabs(bodyPlayer->mo->Angles.Pitch.Degrees()) : 0.0;
		if (eyeAboveFeet > 1.0 && (haveNeckPivot || viewPitch < 12.0 || smoothedEye <= 0.0))
		{
			const double follow = haveNeckPivot ? 0.15 : 0.01;
			if (smoothedEye <= 0.0) smoothedEye = eyeAboveFeet;
			else                    smoothedEye += (eyeAboveFeet - smoothedEye) * follow;

			if (standingEye <= 0.0)           standingEye = smoothedEye;
			else if (smoothedEye > standingEye) standingEye = smoothedEye;                       // straightened: at once
			else                                standingEye += (smoothedEye - standingEye) * 0.002; // sank: very slowly
		}
		// Publish the crouch for the IK, and hold the body at standing height while it is non-zero.
		{
			player_t* pl = &players[consoleplayer];
			double crouchDrop = 0.0;
			if (vr_body_crouch && standingEye > 1.0 && smoothedEye > 1.0)
				crouchDrop = standingEye - smoothedEye;
			if (crouchDrop < 0.0) crouchDrop = 0.0;
			pl->vr_body_crouch_drop = (float)crouchDrop;
			if (crouchDrop > 0.0) smoothedEye = standingEye;   // the legs take it from here, not the placement
		}
		// [XR] Neck height: read off the rig by the IK when the mod named a "neck" role, else the cvar.
		const double neckH = (bodyPlayer->vr_body_neck_z > 1.0f) ? (double)bodyPlayer->vr_body_neck_z
		                   : ((vr_body_neck_height > 1.0f) ? (double)vr_body_neck_height : 63.6);

		// [XR] Eye-height autofit and neck-to-HMD contradict each other (one scales the body to the eye,
		// the other slides it there); neck-to-HMD wins, since scaling to a seated eye height halves the arms.
		if (vr_body_autofit && !vr_body_neck_to_hmd)
		{
			// Scale the marine so its NECK-STUMP (bip_neck, model-Z vr_body_neck_height) lands at the HMD
			// eye height and the feet (model origin) stay on the floor -- a true neck->HMD / feet->floor
			// fit. The whole body, arms included, scales by this SAME factor, so the scaled arm reach
			// matches the player's real reach (up to the marine's own arm-to-height proportion; the
			// arm-IK stretches the last bit). This replaces the old actor->Height reference, which is the
			// Doom HITBOX (not the mesh) and mis-sized the body so the arms fell short of the controllers.
			if (smoothedEye > 1.0)
				bodyScale = (float)clamp((smoothedEye - (double)vr_body_headroom) / neckH, 0.25, 1.9);
		}
		// [XR] Publish the exact scale we're about to apply so the playsim arm-IK divides the hand
		// target by the SAME factor (see g_xr_vrBodyRenderScale decl above). Do this even when the
		// scale is 1.0 so a stale value never lingers.
		if (bodyScale > 0.05f && bodyScale < 8.0f) g_xr_vrBodyRenderScale = bodyScale;
		if (!(bodyScale > 0.f)) bodyScale = 1.f;

		// [XR] +vr_body_z raises/lowers the local VR body only.
		bodyZ = (double)vr_body_z;

		// [XR] Forward/back and left/right, in the body's facing frame (the decoupled heading when
		// valid, else the pawn's). Folded into the same matrix the IK inverts, so the hands stay exact.
		{
			const double facing = bodyPlayer->vr_body_facing_valid ? (double)bodyPlayer->vr_body_facing_yaw : (double)angles.Yaw.Degrees();
			const double fx = cos(facing * M_PI / 180.0), fy = sin(facing * M_PI / 180.0);
			bodyOffX = fx * (double)vr_body_forward + fy * (double)vr_body_side;
			bodyOffY = fy * (double)vr_body_forward - fx * (double)vr_body_side;
		}

		// [XR] NECK-TO-HMD: keep the scale, slide the body so the neck stump (plus the rig's neck->eye
		// gap) sits at the live eye height. With autofit on as well the two agree and this adds ~0;
		// with autofit off (seated play at scale 1.0) this is what puts the shoulders where yours are.
		if (vr_body_neck_to_hmd && smoothedEye > 1.0 && bodyScale > 0.f)
		{
			bodyZ += smoothedEye - (neckH + (double)vr_body_neck_eye_gap) * (double)bodyScale;
		}

		// [XR] correct the local VR body's facing (marine mesh authored ~90 off). Body only.
		angle += vr_body_yaw;

		// [XR] Decouple the body facing from the HMD: the pawn yaw follows the headset, so without this the
		// whole torso spins when you turn your head ("no neck"). If P_PlayerThink has a valid decoupled
		// body yaw, render the body at THAT heading (plus the mesh-correction + sprite rotation) instead of
		// the raw HMD-slaved pawn yaw. Pawn Angles.Yaw is untouched, so gameplay + arm-IK targets are as-is.
		if (bodyPlayer->vr_body_facing_valid)
		{
			angle = bodyPlayer->vr_body_facing_yaw + vr_body_yaw + actor->SpriteRotation.Degrees();
		}
	}

	double tic = actor->GetModelTimer();

	if (!WorldPaused(true) && !actor->isFrozen())
	{
		tic += ticFrac;
	}

	// TURN IT ABOUT ITS MIDDLE, NOT ITS FEET.
	//
	// An actor's origin sits on the floor between its feet, and every rotation
	// below is applied about that origin. For scenery standing in a room that is
	// exactly right -- a barrel turns on the spot. For a barrel in your hand it
	// is not: the thing you are holding swings through an arc the length of its
	// own height, which reads as the object pivoting about a point somewhere
	// below it rather than turning where you are holding it.
	//
	// Half the height is the honest approximation. The real answer is where the
	// hand actually gripped it, which nothing here knows; the midpoint is right
	// for the upright cylinders this mostly picks up and wrong by less than half
	// a height for everything else.
	const float bodyPivotZ = actor->VoxelOverride ? float(actor->Height * 0.5) : 0.f;

	// [XR] The body fit rides in on the actor scale (the matrix overload multiplies it into the
	// MODELDEF scale exactly where the original scaled scaleFactorX/Y/Z) and vr_body_z on the
	// translation. Both are identity for everything that is not the local VR body.
	DVector2 actorScale = actor->InterpolatedScale(ticFrac);
	if (isVRBody)
	{
		// X = horizontal (both map axes), Y = vertical in an actor scale. Width slims or thickens the
		// body without touching its height; the IK inverts this same matrix, so the hands stay put.
		actorScale.X *= (double)bodyScale * clamp((double)vr_body_width, 0.3, 2.0);
		actorScale.Y *= (double)bodyScale;
	}

	return ObjectToWorldMatrix(actor->Level, DVector3(x + bodyOffX, y + bodyOffY, z + bodyZ), DRotator(DAngle::fromDeg(pitch), DAngle::fromDeg(angle), DAngle::fromDeg(roll)), actorScale, smf_flags, tic, bodyPivotZ, actor->FollowBodyMode, actor->FollowBodyOfs);
}

VSMatrix FSpriteModelFrame::ObjectToWorldMatrix(FLevelLocals *Level, DVector3 translation, DRotator rotation, DVector2 scaling, unsigned int flags, double tic, float bodyPivotZ, int followBodyMode, DVector3 followBodyOfs)
{
	double rotateOffset = 0;

	if (flags & MDL_ROTATING)
	{
		if (rotationSpeed > 0.0000000001 || rotationSpeed < -0.0000000001)
		{
			double turns = (tic) / (200.0 / rotationSpeed);
			turns -= floor(turns);
			rotateOffset = turns * 360.0;
		}
		else
		{
			rotateOffset = 0.0;
		}
	}

	// y scale for a sprite means height, i.e. z in the world!
	float scaleFactorX = scaling.X * xscale;
	float scaleFactorY = scaling.X * yscale;
	float scaleFactorZ = scaling.Y * zscale;

	VSMatrix objectToWorldMatrix;
	objectToWorldMatrix.loadIdentity();

	// MDL_FOLLOWMAINHAND / MDL_FOLLOWOFFHAND -- see the flag comment in models.h.
	//
	// Deliberately GetWeaponTransform and not a reconstruction of it. Two prior
	// attempts to rebuild this engine's rotation basis by hand each passed their
	// own self-consistency check and each still landed every prop 4.55 units off,
	// because neither accounted for RenderModel negating pitch before rotating.
	// Replaying the engine's own transform is the only approach in this tree's
	// history that ever worked, so this calls the exact function the working HUD
	// path calls and takes the matrix whole -- no decomposition, no Euler round
	// trip, nothing to get the axis order wrong in.
	// AActor::FollowBodyMode -- the same idea one step out from the hand: the
	// player's own frame, read at draw rate, with the actor's seat inside it.
	// Taken whole from GetHmdTransform for the reason stated below about
	// GetWeaponTransform -- rebuilding this basis by hand is what cost the two
	// earlier attempts, and the body frame is built the same way the hand one
	// is precisely so the two agree.
	//
	// The seat is applied in the body's frame BEFORE any of the model's own
	// offsets, so MODELDEF Offset and the placement sliders keep meaning what
	// they mean everywhere else: adjustments relative to where the thing sits.
	bool followedHand = false;
	if (followBodyMode > 0)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode != nullptr && vrmode->IsVR() && vrmode->GetHmdTransform(&objectToWorldMatrix))
		{
			objectToWorldMatrix.translate((float)followBodyOfs.X,
				(float)followBodyOfs.Z, (float)followBodyOfs.Y);
			followedHand = true;
		}
		else
		{
			// Not in VR, or no pose this frame. Fall back to ordinary world
			// placement rather than drawing everything at the origin.
			objectToWorldMatrix.loadIdentity();
		}
	}

	const int followHand = (flags & MDL_FOLLOWMAINHAND) ? VR_MAINHAND
		: ((flags & MDL_FOLLOWOFFHAND) ? VR_OFFHAND : -1);
	if (!followedHand && followHand >= 0)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode != nullptr && vrmode->IsVR() &&
			vrmode->GetWeaponTransform(&objectToWorldMatrix, followHand, !(flags & MDL_NOAUTOREVERSE)))
		{
			followedHand = true;

		}
		else
		{
			// Not in VR, or the pose is unavailable this frame. Fall back to the
			// ordinary world placement rather than drawing at the origin.
			objectToWorldMatrix.loadIdentity();
		}
	}

	if (followedHand)
	{
		// The controller supplies orientation, so the actor's own Angles must not
		// be applied on top. Zeroing them here rather than branching around the
		// rotation block below leaves that block's structure untouched -- a
		// rotate() of zero degrees is a no-op -- so MDL_ROTATING and the rotation
		// -centre paths keep behaving exactly as they always have.
		rotation.Yaw = rotation.Pitch = rotation.Roll = DAngle::fromDeg(0.);
	}
	else
	{
		// Model space => World space
		objectToWorldMatrix.translate(translation.X, translation.Z, translation.Y);
	}

	// consider the pixel stretching. For non-voxels this must be factored out here
	float stretch = 1.f;

	// [MK] distortions might happen depending on when the pixel stretch is compensated for
	// so we make the "undistorted" behavior opt-in
	if ((flags & MDL_CORRECTPIXELSTRETCH) && modelIDs.Size() > 0)
	{
		stretch = (modelIDs[0] >= 0 ? Models[modelIDs[0]]->getAspectFactor(Level->info->pixelstretch) : 1.f) / Level->info->pixelstretch;
		objectToWorldMatrix.scale(1, stretch, 1);
	}

	// Zero for everything that is not a held voxel, so the common path is one
	// compare and the rotate calls below fold away.
	// NEGATIVE MEANS DERIVE IT, and that is the default.
	//
	// Step 5 below spins the mesh by -angleoffset before any of this runs. A
	// pack that declares 90 therefore leaves the mesh's long axis on Z while
	// roll still turns about X -- a quarter turn out, which is a wrist roll
	// coming out as a fore/aft tilt. Undoing exactly the offset the pack
	// declared puts the body axes back where pitch and roll expect them, so the
	// right number is not a matter of taste and nobody should have to find it
	// by feel. Confirmed against the barrel: angleoffset=90, tilt on roll.
	//
	// The override stays because a pack whose voxels are authored nose-up
	// rather than nose-along could need something else, and there is no way to
	// tell that from the data either.
	float voxBodyYaw = 0.f;
	if (flags & MDL_VOXELBODYAXIS)
		// NEGATED, confirmed in a headset 2026-08-29: at +angleoffset the barrel
		// rolled the right way about the right axis and in the WRONG DIRECTION.
		//
		// Which is the expected result of getting the wrap backwards. The pair
		// below rotates the mesh into a frame, applies pitch and roll, and
		// rotates back out; sending +angleoffset winds it the same way step 5
		// already wound the mesh instead of unwinding it, so the body axes land
		// a quarter turn past where they started rather than back at it.
		voxBodyYaw = (vr_voxel_rollaxis < 0.f) ? -angleoffset : (float)vr_voxel_rollaxis;

	bool rotating_xzy = (flags & MDL_ROTATING) && (flags & MDL_FIXROTATING);
	bool rotating_xyz = (flags & MDL_ROTATING) && !(flags & MDL_FIXROTATING);

	// Applying model transformations:
	// 1) Applying actor angle, pitch and roll to the model
	if (flags & MDL_USEROTATIONCENTER)
	{
		objectToWorldMatrix.translate(rotationCenterX, rotationCenterZ/stretch, rotationCenterY);

		objectToWorldMatrix.rotate(-rotation.Yaw.Degrees(), 0, 1, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(-voxBodyYaw, 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Pitch.Degrees(), 0, 0, 1);
		objectToWorldMatrix.rotate(-rotation.Roll.Degrees(), 1, 0, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(voxBodyYaw, 0, 1, 0);

		// 2) Applying Doomsday like rotation of the weapon pickup models
		// The rotation angle is based on the elapsed time.
		if(rotating_xzy)
		{
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
		}

		objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterZ/stretch, -rotationCenterY);

		if(rotating_xyz)
		{
			objectToWorldMatrix.translate(rotationCenterX, rotationCenterY/stretch, rotationCenterZ);
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
			objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterY/stretch, -rotationCenterZ);
		}
	}
	else
	{
		// Same shape as the USEROTATIONCENTER branch above -- lift the pivot to
		// the origin, turn, put it back -- but the height comes from the ACTOR
		// rather than from a MODELDEF, because no voxel pack declares one and a
		// held object needs one regardless. Zero for everything else, and the
		// two translates fold away.
		if (bodyPivotZ != 0.f) objectToWorldMatrix.translate(0, bodyPivotZ / stretch, 0);

		objectToWorldMatrix.rotate(-rotation.Yaw.Degrees(), 0, 1, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(-voxBodyYaw, 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Pitch.Degrees(), 0, 0, 1);
		objectToWorldMatrix.rotate(-rotation.Roll.Degrees(), 1, 0, 0);
		if (voxBodyYaw != 0.f) objectToWorldMatrix.rotate(voxBodyYaw, 0, 1, 0);

		if (bodyPivotZ != 0.f) objectToWorldMatrix.translate(0, -bodyPivotZ / stretch, 0);

		// 2) Applying Doomsday like rotation of the weapon pickup models
		// The rotation angle is based on the elapsed time.
		if(rotating_xzy)
		{
			objectToWorldMatrix.translate(rotationCenterX, rotationCenterZ/stretch, rotationCenterY);
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
			objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterZ/stretch, -rotationCenterY);
		}
		else if(rotating_xyz)
		{
			objectToWorldMatrix.translate(rotationCenterX, rotationCenterY/stretch, rotationCenterZ);
			objectToWorldMatrix.rotate(rotateOffset, xrotate, yrotate, zrotate);
			objectToWorldMatrix.translate(-rotationCenterX, -rotationCenterY/stretch, -rotationCenterZ);
		}
	}

	// PlacementCVars on the WORLD path.
	//
	// This used to exist only in RenderHUDModel, which made the feature exactly
	// backwards: a physically held gun IS a world actor, so the one case that
	// most needs live tuning was the one case the sliders could not reach, and
	// moving them did nothing at all with nothing in the log to say why.
	// Commit 026d2a8a80 fixed that and the wholesale revert took it back out.
	//
	// Summed into the SAME translate and rotate calls as the MODELDEF values,
	// never applied afterwards. Rotations do not commute: a yaw applied after the
	// model's own pitch and roll turns about an already-rotated axis and is NOT
	// the same number added to angleoffset. Because they fold in here, a value
	// found by eye transfers into the MODELDEF verbatim and the slider returns to
	// zero with nothing moving.
	float wPlaceOfs[3] = { 0.0f, 0.0f, 0.0f };
	float wPlaceRot[3] = { 0.0f, 0.0f, 0.0f };
	float wPlaceScale = 1.0f;
	// PER-AXIS scale, on top of the uniform one. Needed by anything whose three
	// dimensions are genuinely different numbers -- a drawn collision box, most
	// obviously, whose whole value is being the same three numbers the solver
	// was handed. Axes are stated in ACTOR terms, matching _ofs_x/_y/_z:
	// x = forward, y = sideways, z = up.
	float wPlaceAxis[3] = { 1.0f, 1.0f, 1.0f };

	// The live pivot, so the number can be found on a slider before being folded
	// into the MODELDEF. Every other placement value here works that way and a
	// pivot is the hardest of them to guess, because being wrong shows up as a
	// wobble during rotation rather than as a static misplacement.
	float wPlacePiv[3] = { 0.0f, 0.0f, 0.0f };
	if (placementCVars != NAME_None)
	{
		static const char *sufOfs[3] = { "_ofs_x", "_ofs_y", "_ofs_z" };
		static const char *sufRot[3] = { "_yaw", "_pitch", "_roll" };
		static const char *sufPiv[3] = { "_piv_x", "_piv_y", "_piv_z" };
		FString nm;
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", placementCVars.GetChars(), sufOfs[i]);
			GetPlacementCVar(nm.GetChars(), wPlaceOfs[i]);
			nm.Format("%s%s", placementCVars.GetChars(), sufRot[i]);
			GetPlacementCVar(nm.GetChars(), wPlaceRot[i]);
			nm.Format("%s%s", placementCVars.GetChars(), sufPiv[i]);
			GetPlacementCVar(nm.GetChars(), wPlacePiv[i]);
		}
		// Defaults to 1, NOT the 0 an absent cvar reads as -- a missing slider
		// must leave the model alone, not collapse it to a point.
		nm.Format("%s_scale", placementCVars.GetChars());
		{
			float sc = 0.0f;
			if (GetPlacementCVar(nm.GetChars(), sc) && sc > 0.0f) wPlaceScale = sc;
		}
		static const char *sufAxis[3] = { "_scale_x", "_scale_y", "_scale_z" };
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", placementCVars.GetChars(), sufAxis[i]);
			{
				float sc = 0.0f;
				if (GetPlacementCVar(nm.GetChars(), sc) && sc > 0.0f) wPlaceAxis[i] = sc;
			}
		}
	}

	// 3) Scaling model.
	objectToWorldMatrix.scale(scaleFactorX * wPlaceScale * wPlaceAxis[0],
		scaleFactorZ * wPlaceScale * wPlaceAxis[2],
		scaleFactorY * wPlaceScale * wPlaceAxis[1]);

	// 4) Aplying model offsets (model offsets do not depend on model scalings).
	objectToWorldMatrix.translate((xoffset + wPlaceOfs[0]) / xscale,
		(zoffset + wPlaceOfs[2]) / (zscale*stretch),
		(yoffset + wPlaceOfs[1]) / yscale);

	// 5) Applying model rotations.
	objectToWorldMatrix.rotate(-(angleoffset + wPlaceRot[0]), 0, 1, 0);
	objectToWorldMatrix.rotate(pitchoffset + wPlaceRot[1], 0, 0, 1);
	objectToWorldMatrix.rotate(-(rolloffset + wPlaceRot[2]), 1, 0, 0);

	// 6) PIVOT -- the point the model turns about, in the model's OWN space.
	//
	// LAST IN CODE ORDER MEANS FIRST ON THE VERTEX, and that is the entire point.
	// VSMatrix::translate/rotate/scale post-multiply (M = M * op, matrix.cpp:177),
	// so the operation written last here is applied to the vertex first. Putting
	// the subtraction here gives v' = R * (v - p): the mesh is moved onto its
	// intended turning point BEFORE being rotated.
	//
	// Written after step 5 rather than before it for exactly that reason. Move
	// these three lines above the rotations and they become another Offset --
	// which is to say, they stop working, silently, while still looking correct.
	//
	// Same axis order and the same division by scale as step 4, so a pivot and an
	// offset are stated in the same units and can be read against each other.
	//
	// The compare is not an optimisation: the common case is a model with no pivot
	// at all, and three float compares are cheaper than a matrix multiply on every
	// drawn model in the level.
	if (pivotx != 0.f || pivoty != 0.f || pivotz != 0.f)
	{
		objectToWorldMatrix.translate(-(pivotx + wPlacePiv[0]) / xscale,
			-(pivotz + wPlacePiv[2]) / (zscale*stretch),
			-(pivoty + wPlacePiv[1]) / yscale);
	}
	else if (wPlacePiv[0] != 0.f || wPlacePiv[1] != 0.f || wPlacePiv[2] != 0.f)
	{
		// Cvar-only pivot, so the sliders can find the number before it is folded
		// into the MODELDEF -- the same workflow the offset and rotation sliders
		// already support.
		objectToWorldMatrix.translate(-wPlacePiv[0] / xscale,
			-wPlacePiv[2] / (zscale*stretch),
			-wPlacePiv[1] / yscale);
	}

	if (!(flags & MDL_CORRECTPIXELSTRETCH) && modelIDs.Size() > 0)
	{
		stretch = (modelIDs[0] >= 0 ? Models[modelIDs[0]]->getAspectFactor(Level->info->pixelstretch) : 1.f) / Level->info->pixelstretch;
		objectToWorldMatrix.scale(1, stretch, 1);
	}

	return objectToWorldMatrix;
}

void RenderHUDModel(FModelRenderer *renderer, DPSprite *psp, FVector3 translation, FVector3 rotation, FVector3 rotation_pivot, FSpriteModelFrame *smf, double ticFrac)
{
	AActor * playermo = players[consoleplayer].camera;

	int smf_flags = smf->getFlags(psp->Caller->modelData);

	// [BB] No model found for this sprite, so we can't render anything.
	if (smf == nullptr)
		return;

	// The model position and orientation has to be drawn independently from the position of the player,
	// but we need to position it correctly in the world for light to work properly.
	VSMatrix objectToWorldMatrix = renderer->GetViewToWorldMatrix();

	// [BB] Which controller this psprite rides on.
	//
	// The caller test comes first and is the original one: both hands' muzzle
	// flashes share PSP_FLASH, so a flash layer's ID says nothing about its
	// side and only the caller identifies it.
	//
	// The ID test is the addition. Hand selection used to be derived purely
	// from the player's weapon slots, so any psprite whose caller was not
	// literally player->OffhandWeapon -- i.e. anything that is not a weapon,
	// such as a hand model -- silently fell through to the mainhand pose, with
	// no way to say otherwise from script. Layers at or above
	// PSP_OFFHANDWEAPON now name the offhand explicitly. That range includes
	// PSP_OFFHANDWEAPON itself, so this also keeps the offhand weapon on the
	// correct hand during the frames where its slot is momentarily null.
	int hand = (psp->GetCaller() == playermo->player->OffhandWeapon
		|| psp->GetID() >= PSP_OFFHANDWEAPON) ? 1 : 0;
	auto vrmode = VRMode::GetVRModeCached(true);

	// MDL_NOAUTOREVERSE: the model supplies its own left and right variants, so
	// the non-dominant-hand mirror would flip an already-correct mesh.
	// The HUD-model unit conversion, remembered rather than only applied.
	//
	// This 0.01 is not part of positioning the model at the controller -- it is
	// the conversion from the model's own units into the units the rest of this
	// function works in, and EVERY hud model needs it, anchored or not. The
	// anchoring block below replaces the whole matrix (loadMatrix), which
	// silently discarded it and drew anchored models at 100x size.
	//
	// It went unnoticed on weapons because a weapon cancels it: the T77 carries
	// MODELDEF Scale 100, and 0.01 * 100 = 1. The VR hands carry Scale 1.0, so
	// they have nothing to cancel with and take the full factor of 100 -- which
	// is precisely the "absolutely massive" hands, and why only the ANCHORED
	// ones were affected while a hand holding a magazine (deliberately
	// unanchored by the hands mod) stayed correct.
	float hudUnitScale = 1.0f;
	if (vrmode->GetWeaponTransform(&objectToWorldMatrix, hand, !(smf_flags & MDL_NOAUTOREVERSE)))
	{
		float scale = 0.01f;
		objectToWorldMatrix.scale(scale, scale, scale);
		objectToWorldMatrix.translate(0, 5, 30);
		hudUnitScale = scale;
	}
	else if (vrmode->IsVR())
	{
		DVector3 pos = playermo->Pos();
		objectToWorldMatrix.translate(pos.X, pos.Z + 40, pos.Y);
		objectToWorldMatrix.rotate(-playermo->Angles.Yaw.Degrees() - 90, 0, 1, 0);
	}

	float fovscale = 1.0f;
	if (smf->viewModelFOV <= 0.0f)
	{
		if (smf->viewModelFOV < 0.0f)
			fovscale = 1.0f / fabs(smf->viewModelFOV);

		// [Nash] Optional scale weapon FOV
		if (smf_flags & MDL_SCALEWEAPONFOV)
		{
			float newScale = tan(players[consoleplayer].DesiredFOV * (0.5f * M_PI / 180.f));
			newScale = 1.f + (newScale - 1.f) * cl_scaleweaponfov;
			fovscale *= newScale;
		}
	}
	else if (players[consoleplayer].DesiredFOV != smf->viewModelFOV)
	{
		fovscale = tan(players[consoleplayer].DesiredFOV * (0.5f * M_PI / 180.f)) / tan(smf->viewModelFOV * (0.5f * M_PI / 180.f));
	}

	// [BB] The psprite's own scale reaches the MODEL path, not just the sprite
	// one.
	//
	// psp->scale was read only by the 2D weapon-sprite code, so a mod that
	// shrank a psprite saw nothing happen to a weapon drawn as a model -- and
	// there was no other way to resize one from script at all.
	//
	// It defaults to (0,0) and the sprite path already treats that as "unset"
	// (hw_weapon.cpp tests scale.isZero()), so this is a free channel: zero
	// leaves every existing model exactly as it was, and no content that does
	// not opt in can be affected.
	//
	// X alone, applied uniformly. A model scaled unevenly on two axes shears
	// rather than resizes, and "half size" is one number in every caller's head.
	float pspScale = 1.0f;
	if (!psp->scale.isZero()) pspScale = (float)psp->scale.X;

	// Scaling model (y scale for a sprite means height, i.e. z in the world!).
	objectToWorldMatrix.scale(smf->xscale * pspScale, smf->zscale * pspScale, (smf->yscale / fovscale) * pspScale);

	// Aplying model offsets (model offsets do not depend on model scalings).
	//
	// MDL_USEHANDOFFSETS adds the live vr_hand_ofs_* CVARs into the same
	// translate rather than a second one. Summing inside the single call is what
	// makes a CVAR value and a MODELDEF value genuinely interchangeable, so a
	// number found on a slider can be folded into MODELDEF and the slider zeroed
	// with nothing moving.
	// RS FORK -- MOD-OWNED PLACEMENT, read live from CVARs the MOD declares.
	//
	// Looked up by name every frame rather than resolved once at parse time,
	// because MODELDEF is parsed before a mod's CVARINFO is guaranteed to have
	// run, and a cached null would be permanent. Six hash lookups for a handful
	// of drawn models is not worth optimising away.
	float placeOfs[3] = { 0.0f, 0.0f, 0.0f };
	float placeRot[3] = { 0.0f, 0.0f, 0.0f };
	float placeScale = 1.0f;
	// Per-axis, same as the world path -- kept in step so a prefix behaves the
	// same whichever path draws it. A model tuned on one and moved to the other
	// silently losing an axis is the kind of asymmetry that costs a session.
	float placeAxis[3] = { 1.0f, 1.0f, 1.0f };
	// Live pivot, matching the world path so a prefix behaves identically on both.
	float placePiv[3] = { 0.0f, 0.0f, 0.0f };
	if (smf->placementCVars != NAME_None)
	{
		static const char *sufOfs[3] = { "_ofs_x", "_ofs_y", "_ofs_z" };
		static const char *sufRot[3] = { "_yaw", "_pitch", "_roll" };
		static const char *sufPiv[3] = { "_piv_x", "_piv_y", "_piv_z" };
		FString nm;
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufOfs[i]);
			GetPlacementCVar(nm.GetChars(), placeOfs[i]);
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufRot[i]);
			GetPlacementCVar(nm.GetChars(), placeRot[i]);
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufPiv[i]);
			GetPlacementCVar(nm.GetChars(), placePiv[i]);
		}

		// Scale defaults to 1, NOT to the 0 an absent CVAR would read as -- a
		// missing or zeroed slider must leave the model alone, not collapse it
		// to a point.
		nm.Format("%s_scale", smf->placementCVars.GetChars());
		{
			float sv = 0.0f;
			if (GetPlacementCVar(nm.GetChars(), sv) && sv > 0.0f) placeScale = sv;
		}
		static const char *sufAxis[3] = { "_scale_x", "_scale_y", "_scale_z" };
		for (int i = 0; i < 3; ++i)
		{
			nm.Format("%s%s", smf->placementCVars.GetChars(), sufAxis[i]);
			{
				float v = 0.0f;
				if (GetPlacementCVar(nm.GetChars(), v) && v > 0.0f) placeAxis[i] = v;
			}
		}
	}

	// RS FORK -- HUD BONE ANCHORING, applied.
	//
	// Everything above positioned this model at a controller. If it is anchored
	// to a bone instead, that work is discarded here and the bone's transform
	// becomes the base. Deliberately placed AFTER the controller maths rather
	// than replacing it: the offsets, rotations and scale below then apply
	// relative to the bone, so MODELDEF still fine-tunes the fit exactly as it
	// does for an unanchored model, and one code path serves both.
	bool isAnchored = false;
	if (psp->AnchorLayer >= 0 && psp->AnchorBone != NAME_None)
	{
		VSMatrix anchored;
		if (HudAnchor_Get(psp->AnchorLayer, psp->AnchorBone, anchored))
		{
			// Position and orientation only -- NEVER scale.
			//
			// A bone matrix carries the entire chain that produced it, and that
			// chain includes whatever scale the target model was authored at.
			// Adopting it wholesale multiplies THIS model by the other one's
			// size, which is never what anchoring means: a magazine placed in a
			// hand should be magazine-sized, not hand-times-magazine sized.
			//
			// It bites hard because the numbers involved are not small. The hand
			// rig carries a 0.01 at its root joint, so a magazine anchored to a
			// knuckle came out a hundredth of its size -- far past what any
			// scale slider could climb back out of, and looking for all the
			// world like a model exported wrong.
			//
			// So the basis is orthonormalised: each of the three axes is scaled
			// back to unit length, which strips scale while leaving rotation and
			// translation exactly as they were.
			FLOATTYPE m[16];
			memcpy(m, anchored.get(), sizeof(m));
			for (int c = 0; c < 3; ++c)
			{
				FLOATTYPE *col = &m[c * 4];
				FLOATTYPE len = (FLOATTYPE)sqrt(col[0]*col[0] + col[1]*col[1] + col[2]*col[2]);
				if (len > (FLOATTYPE)1e-8)
				{
					col[0] /= len; col[1] /= len; col[2] /= len;
				}
			}
			objectToWorldMatrix.loadMatrix(m);

			// AND PUT THIS MODEL'S OWN SCALE BACK.
			//
			// loadMatrix REPLACES the matrix outright, which throws away the
			// MODELDEF scale applied further up (the `objectToWorldMatrix.scale`
			// on smf->xscale/zscale/yscale). Anchoring is only supposed to
			// discard the target-relative POSITIONING done above it -- not the
			// model's own size.
			//
			// It is not only magnitude. The VR hands are ONE mesh mirrored by a
			// negative X scale (RS_HandIdleMain carries `Scale -1.0 1.0 1.0`),
			// so dropping this silently un-mirrors the main hand: an anchored
			// hand came out the wrong way round as well as the wrong size.
			//
			// Deliberately the identical expression used above, not a
			// recalculation -- the two must not be able to drift apart.
			objectToWorldMatrix.scale(smf->xscale * pspScale, smf->zscale * pspScale, (smf->yscale / fovscale) * pspScale);

			// ...and the hud-model unit conversion the controller branch
			// applied, which loadMatrix above also threw away. See the note
			// where hudUnitScale is set: without this an anchored model is
			// drawn 100x too large.
			objectToWorldMatrix.scale(hudUnitScale, hudUnitScale, hudUnitScale);

			isAnchored = true;
		}
	}

	const bool useHandOfs = !!(smf_flags & MDL_USEHANDOFFSETS);
	// Which set of sliders this model listens to. The two hands are one mesh
	// mirrored by a negative X scale, so a shared value pushes them in opposite
	// directions and no single number can place both -- they each need their own.
	// hand was resolved above from the psprite, so it is already known here.
	const bool isOffhand = (hand == 1);
	const float handOfsX = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_x : vr_hand_ofs_x) : 0.0f;
	const float handOfsY = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_y : vr_hand_ofs_y) : 0.0f;
	const float handOfsZ = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_z : vr_hand_ofs_z) : 0.0f;

	// Seat offset. Only meaningful for an anchored layer -- for anything else
	// the bone frame it is expressed in does not exist -- and summed in here
	// rather than applied separately, for the same non-commuting reason the
	// rotation block below spells out.
	const float seatX = isAnchored ? (float)psp->AnchorOfs.X : 0.0f;
	const float seatY = isAnchored ? (float)psp->AnchorOfs.Y : 0.0f;
	const float seatZ = isAnchored ? (float)psp->AnchorOfs.Z : 0.0f;

	objectToWorldMatrix.translate((smf->xoffset + handOfsX + placeOfs[0] + seatX) / smf->xscale,
		(smf->zoffset + handOfsZ + placeOfs[2] + seatZ) / smf->zscale,
		(smf->yoffset + handOfsY + placeOfs[1] + seatY) / smf->yscale);

	// Everything in this block places the model relative to the PLAYER: the
	// global weapon offset, the bob, the aim rotation, the viewmodel axis fix.
	//
	// An anchored model must skip all of it. The bone matrix it started from was
	// captured after its target had already been through these same steps, so
	// applying them again would add the weapon's position and rotation a second
	// time and throw the attachment well clear of the bone it is meant to sit on.
	// What survives below is only the model's OWN offsets, rotations and scale,
	// which is exactly what should still fine-tune the fit.
	if (!isAnchored)
	{
		// Applying player custom offsets
		objectToWorldMatrix.translate(-vr_3dweaponOffsetX, vr_3dweaponOffsetY, vr_3dweaponOffsetZ);

		// [BB] Weapon bob, very similar to the normal Doom weapon bob.
		objectToWorldMatrix.translate(rotation_pivot.X, rotation_pivot.Y, rotation_pivot.Z);

		objectToWorldMatrix.rotate(rotation.X, 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Y, 1, 0, 0);
		objectToWorldMatrix.rotate(rotation.Z, 0, 0, 1);

		objectToWorldMatrix.translate(-rotation_pivot.X, -rotation_pivot.Y, -rotation_pivot.Z);

		objectToWorldMatrix.translate(translation.X, translation.Y, translation.Z);

		// [BB] For some reason the jDoom models need to be rotated.
		objectToWorldMatrix.rotate(90.f, 0, 1, 0);
	}

	// Applying angleoffset, pitchoffset, rolloffset.
	const float handYaw   = useHandOfs ? (float)(isOffhand ? vr_offhand_yaw   : vr_hand_yaw)   : 0.0f;
	const float handPitch = useHandOfs ? (float)(isOffhand ? vr_offhand_pitch : vr_hand_pitch) : 0.0f;
	const float handRoll  = useHandOfs ? (float)(isOffhand ? vr_offhand_roll  : vr_hand_roll)  : 0.0f;

	// Summed into the same three calls: rotations do not commute, so a seat
	// angle only equals a MODELDEF value if it is added here rather than
	// applied afterwards. These are solved against a bone's own orientation,
	// so they belong in the same frame the MODELDEF offsets establish.
	const float seatYaw   = isAnchored ? (float)psp->AnchorAngles.X : 0.0f;
	const float seatPitch = isAnchored ? (float)psp->AnchorAngles.Y : 0.0f;
	const float seatRoll  = isAnchored ? (float)psp->AnchorAngles.Z : 0.0f;

	objectToWorldMatrix.rotate(-(smf->angleoffset + placeRot[0] + seatYaw), 0, 1, 0);
	objectToWorldMatrix.rotate(smf->pitchoffset + placeRot[1] + seatPitch, 0, 0, 1);
	objectToWorldMatrix.rotate(-(smf->rolloffset + placeRot[2] + seatRoll), 1, 0, 0);

	// PIVOT, the same field the world path uses. See the note in model.h.
	//
	// AFTER the rotations in code order, therefore BEFORE them on the vertex --
	// VSMatrix post-multiplies, so the last operation written is the first
	// applied. That ordering IS the feature: written above the rotations these
	// three lines would silently become a second Offset.
	//
	// Present on both paths deliberately. A model tuned on one and moved to the
	// other silently losing a correction is exactly the asymmetry the placement
	// comment above warns about, and a pivot is the worst one to lose, because
	// being wrong shows up as a wobble while turning rather than as a static
	// misplacement anyone would spot immediately.
	if (smf->pivotx != 0.f || smf->pivoty != 0.f || smf->pivotz != 0.f ||
		placePiv[0] != 0.f || placePiv[1] != 0.f || placePiv[2] != 0.f)
	{
		objectToWorldMatrix.translate(-(smf->pivotx + placePiv[0]),
			-(smf->pivotz + placePiv[2]),
			-(smf->pivoty + placePiv[1]));
	}

	// THE HAND SLIDERS ARE APPLIED HERE, IN THE MODEL'S OWN ORIENTED FRAME,
	// and NOT summed into the three calls above. They used to be summed, on
	// the reasoning that a slider value should mean the same thing as the
	// MODELDEF keyword of the same name. That equivalence is real, but it is
	// what broke the sliders outright on any model carrying a 90 degree
	// PitchOffset -- which the VR hands do.
	//
	// These rotations are intrinsic: each turns about the axis the previous
	// ones left behind. Summing put the model's baked pitch BETWEEN the yaw
	// and the roll, and a 90 degree turn about Z maps the X axis onto Y -- so
	// by the time roll was applied, its axis had been rotated onto the exact
	// axis yaw had already used. Two sliders, one axis: moving either one
	// rolled the hand, and nothing at all turned it. Textbook gimbal lock,
	// and not a fault in either slider.
	//
	// Applied after the model is oriented, the three are orthogonal again in
	// the frame the player actually sees: roll turns the hand about its own
	// long axis -- a cylinder roll, which is what the word means -- and yaw
	// turns it about an axis genuinely across that, because the baked pitch is
	// now behind all three rather than in the middle of them.
	//
	// THE EQUIVALENCE IS NOT LOST, it moved. HandAngleOffset/HandPitchOffset/
	// HandRollOffset are summed in right here, in this frame and this order,
	// so a value dialled in on a slider can be written into the matching
	// MODELDEF keyword and mean EXACTLY the same thing. That is what makes a
	// tuning pass permanent instead of something every user has to redo --
	// dial it in live, then bake it, and the slider goes back to zero having
	// changed nothing.
	//
	// What is NOT interchangeable is these three against angleoffset/
	// pitchoffset/rolloffset, and that is the entire reason they are separate
	// keywords rather than more of the same: those orient the model, these
	// orient the hand holding it, and folding one into the other is what put
	// a baked 90 degree pitch between the yaw and the roll in the first place.
	objectToWorldMatrix.rotate(-(smf->handangleoffset + handYaw), 0, 1, 0);
	objectToWorldMatrix.rotate(smf->handpitchoffset + handPitch, 0, 0, 1);
	objectToWorldMatrix.rotate(-(smf->handrolloffset + handRoll), 1, 0, 0);

	//Scale weapon
	// placeScale is the mod's own live slider, multiplied onto the global one so
	// a per-weapon size can be found without disturbing every other weapon.
	objectToWorldMatrix.scale(vr_weaponScale * placeScale * placeAxis[0],
		vr_weaponScale * placeScale * placeAxis[2],
		vr_weaponScale * placeScale * placeAxis[1]);

	float orientation = smf->xscale * smf->yscale * smf->zscale;

	// Where this layer actually ended up, in numbers.
	//
	// "Tiny and far away" and "correctly sized but mispositioned" look identical
	// through a headset and are entirely different bugs. The transform says
	// which: position is the last column, scale is the length of the first.
	// Resolved defensively: a frame's model id is -1 when it has no model, and
	// Models[-1] is an out-of-bounds read -- a silent crash at the first weapon
	// draw, which is to say the instant a map starts.
	FModel *validateModel = nullptr;
	if (smf->modelIDs.Size() > 0)
	{
		const int vid = smf->modelIDs[0];
		if (vid >= 0 && vid < Models.SSize()) validateModel = Models[vid];
	}
	ValidateHudModel(smf, validateModel, psp, smf_flags);

	if (vr_spatialreport && psp)
	{
		// PER LAYER, not one shared timer.
		//
		// This used to be a single `static uint64_t lastReport`, which made the
		// report structurally unable to say anything about most of the scene:
		// psprites are drawn in ascending layer order, so the WEAPON (layer 1)
		// consumed the once-a-second slot every time and no other layer was
		// ever printed. Hands (900000/1900000) never appeared at all, and their
		// absence read as "not being drawn" when it only meant "never got the
		// slot" -- the exact wrong conclusion to hand someone debugging a
		// missing model.
		static TMap<int, uint64_t> lastReportByLayer;
		const int reportLayer = psp->GetID();
		uint64_t *slot = lastReportByLayer.CheckKey(reportLayer);
		const uint64_t last = slot ? *slot : 0;
		if (screen && (screen->FrameTime - last) > 1000)
		{
			lastReportByLayer.Insert(reportLayer, screen->FrameTime);
			const FLOATTYPE *m = objectToWorldMatrix.get();
			float sx = (float)sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
			Printf("[SPATIAL] layer %-8d %-22s pos(%.1f %.1f %.1f) scale %.3f frame %d %s\n",
				psp->GetID(),
				(psp->Caller != nullptr) ? psp->Caller->GetClass()->TypeName.GetChars() : "unknown",
				(float)m[12], (float)m[13], (float)m[14], sx,
				psp->ModelFrame,
				isAnchored ? "ANCHORED" : "");
		}
	}

	// Kept for HudAnchor_Store: a bone matrix is model-local, so publishing a
	// usable anchor needs the transform that puts this model in the world.
	g_hudAnchorSource = objectToWorldMatrix;

	renderer->BeginDrawHUDModel(playermo->RenderStyle, objectToWorldMatrix, orientation < 0, smf_flags);
	auto trans = psp->GetTranslation();
	if ((psp->Flags & PSPF_PLAYERTRANSLATED)) trans = psp->Owner->mo->Translation;

	RenderFrameModels(renderer, playermo->Level, smf, psp->GetState(), psp->GetTics(), ticFrac, trans, psp->Caller, psp);
	renderer->EndDrawHUDModel(playermo->RenderStyle, smf_flags);
}

double getCurrentFrame(const ModelAnim &anim, double tic, bool *looped)
{
	if(anim.framerate <= 0) return anim.startFrame;

	double frame = ((tic - anim.startTic) / GameTicRate) * anim.framerate; // position in frames

	double duration = double(anim.lastFrame) - anim.startFrame;

	if((anim.flags & MODELANIM_LOOP) && frame >= duration)
	{
		if(looped) *looped = true;
		frame = frame - duration;
		return fmod(frame, anim.lastFrame - anim.loopFrame) + anim.loopFrame;
	}
	else
	{
		return min(frame, duration) + anim.startFrame;
	}
}

void calcFrame(const ModelAnim &anim, double tic, ModelAnimFrameInterp &inter)
{
	bool looped = false;

	double frame = getCurrentFrame(anim, tic, &looped);

	inter.frame1 = int(floor(frame));

	inter.inter = frame - inter.frame1;

	inter.frame2 = int(ceil(frame));

	int startFrame = (looped ? anim.loopFrame : anim.startFrame);

	if(inter.frame1 < startFrame) inter.frame1 = anim.lastFrame;
	if(inter.frame2 > anim.lastFrame) inter.frame2 = startFrame;
}

void calcFrames(const ModelAnim &curAnim, double tic, ModelAnimFrameInterp &to, float &inter)
{
	if(curAnim.startTic > tic)
	{
		inter = (tic - (curAnim.startTic - curAnim.switchOffset)) / curAnim.switchOffset;

		calcFrame(curAnim, curAnim.startTic, to);
	}
	else
	{
		inter = -1.0f;
		calcFrame(curAnim, tic, to);
	}
}

CalcModelFrameInfo CalcModelFrame(FLevelLocals *Level, const FSpriteModelFrame *smf, const FState *curState, const int curTics, DActorModelData* data, AActor* actor, bool is_decoupled, double tic, double ticFrac, const DPSprite* psp)
{
	// [BB] Frame interpolation: Find the FSpriteModelFrame smfNext which follows after smf in the animation
	// and the scalar value inter ( element of [0,1) ), both necessary to determine the interpolated frame.

	int smf_flags = smf->getFlags(data);

	const FSpriteModelFrame * smfNext = nullptr;
	float inter = 0.;

	ModelAnimFrameInterp decoupled_frame;
	ModelAnimFrame * decoupled_frame_prev = nullptr;

	// if prev_frame == -1: interpolate(main_frame, next_frame, inter), else: interpolate(interpolate(main_prev_frame, main_frame, inter_main), interpolate(next_prev_frame, next_frame, inter_next), inter)
	// 4-way interpolation is needed to interpolate animation switches between animations that aren't 35hz

	if(is_decoupled)
	{
		smfNext = smf = &BaseSpriteModelFrames[(data != nullptr && data->modelDef != nullptr) ? data->modelDef : actor->GetClass()];
		if(data && !(data->anims.curAnim.flags & MODELANIM_NONE))
		{
			calcFrames(data->anims.curAnim, tic, decoupled_frame, inter);
			decoupled_frame_prev = &data->anims.prevAnim;
		}
	}
	else if (gl_interpolate_model_frames && !(smf_flags & MDL_NOINTERPOLATION))
	{
		FState *nextState = curState->GetNextState();
		if (curState != nextState && nextState)
		{
			// [BB] To interpolate at more than 35 fps we take tic fractions into account.
			float ticFraction = 0.;
			// [BB] In case the tic counter is frozen we have to leave ticFraction at zero.
			if (!WorldPaused(true) && !Level->isFrozen())
			{
				ticFraction = ticFrac;
			}

			inter = static_cast<double>(curState->Tics - curTics + ticFraction) / static_cast<double>(curState->Tics);

			// [BB] For some actors (e.g. ZPoisonShroom) spr->actor->tics can be bigger than curState->Tics.
			// In this case inter is negative and we need to set it to zero.
			if (curState->Tics < curTics)
				inter = 0.;
			else
			{
				// [BB] Workaround for actors that use the same frame twice in a row.
				// Most of the standard Doom monsters do this in their see state.
				if ((smf_flags & MDL_INTERPOLATEDOUBLEDFRAMES))
				{
					const FState *prevState = curState - 1;
					if ((curState->sprite == prevState->sprite) && (curState->Frame == prevState->Frame))
					{
						inter /= 2.;
						inter += 0.5;
					}
					if (nextState && ((curState->sprite == nextState->sprite) && (curState->Frame == nextState->Frame)))
					{
						inter /= 2.;
						nextState = nextState->GetNextState();
					}
				}
				if (nextState && inter != 0.0)
					smfNext = FindModelFrame(actor, nextState->sprite, nextState->Frame, false);
			}
		}
	}

	// RS FORK -- EXPLICIT INTERPOLATION.
	// Everything above derives 'inter' from state tics and only tweens across a
	// state transition. That is useless when one of OUR model animations is
	// being played across THEIR state timings -- the blend would restart on
	// every state change and sit at zero in between, so the model would snap
	// from pose to pose. When the psprite supplies a lerp we take it verbatim.
	//
	// smfNext must be non-null or RenderModelFrame's nextFrame test fails and
	// 'inter' is discarded. Same definition, different frame number, so smf
	// itself is the correct "next" here.
	//
	// Deliberately placed AFTER the gl_interpolate_model_frames /
	// MDL_NOINTERPOLATION branch so an explicit blend is not silently dropped
	// when a user turns that CVar off.
	if (psp && psp->ModelFrameLerp >= 0.f)
	{
		float f = psp->ModelFrameLerp;
		if (f > 1.f) f = 1.f;
		inter   = f;
		smfNext = smf;
	}
	// RS FORK -- the same, for a WORLD ACTOR. A world-actor hand has no psprite
	// to carry the blend, and without this every pose change is a single-tic
	// jump between rigged shapes, which reads as the hand teleporting.
	else if (actor && actor->ModelFrameLerp >= 0.f)
	{
		float f = actor->ModelFrameLerp;
		if (f > 1.f) f = 1.f;
		inter   = f;
		smfNext = smf;
	}

	// RS FORK -- PER-PART FRAME ADDRESSING needs a blend TARGET even when no
	// scalar lerp was set (p_pspr.h).
	//
	// 'inter' itself is NOT touched here: a per-part blend is per part, so the
	// factor is applied inside the draw loop in RenderFrameModels, where the
	// part index exists. What must be settled before the loop is smfNext --
	// RenderModelFrame discards 'inter' entirely when there is no next frame to
	// blend toward, so without this a per-part lerp would be silently dropped
	// on any layer whose scalar lerp is inactive, which is every layer using
	// the new path. Same definition, different frame number, so smf is the
	// correct "next" here for the same reason the two branches above say so.
	if (psp && !smfNext && psp->AnyModelPartActive())
	{
		smfNext = smf;
	}

	// RS FORK -- NATIVE STATE REMAP interpolation (FORK_CHANGES.md, "Native
	// state remap"). When the weapon carries a state->frame table, the
	// psprite's own state IS the animation clock, and intra-state progress
	// comes straight from its tic countdown plus the render fraction --
	// true display-rate smoothness, computed where the display rate lives.
	// Runs after every other inter derivation so the table, when present,
	// is authoritative; unmapped states simply keep whatever resolved above.
	if (psp && data && data->stateRemap.CountUsed() > 0 && curState != nullptr)
	{
		if (data->stateRemap.CheckKey(intptr_t(curState)) && curState->Tics > 0)
		{
			// [RS FORK] Upstream 5.0.0 hands the render fraction down as 'ticFrac'
			// and folded the console/menu/freeze test into WorldPaused(), which
			// already returns false for MENU_OnNoPause -- so the fork's original
			// "keep animating under a non-pausing menu" semantics are preserved.
			float ticFraction = 0.f;
			if (!WorldPaused(true) && !Level->isFrozen())
			{
				ticFraction = ticFrac;
			}
			float f = (float(curState->Tics - curTics) + ticFraction) / float(curState->Tics);
			if (f < 0.f) f = 0.f;
			if (f > 1.f) f = 1.f;
			inter   = f;
			smfNext = smf;
		}
	}

	unsigned modelsamount = smf->modelsAmount;
	//[SM] - if we added any models for the frame to also render, then we also need to update modelsAmount for this smf
	if (data != nullptr)
	{
		if (data->models.Size() > modelsamount)
			modelsamount = data->models.Size();
	}

	return
	{
		actor,          // RS fork -- so the overrides pass can read ModelFrame
		smf_flags,
		smfNext,
		inter,
		is_decoupled,
		decoupled_frame,
		decoupled_frame_prev,
		modelsamount
	};
}

bool CalcModelOverrides(int i, const FSpriteModelFrame *smf, DActorModelData* data, const CalcModelFrameInfo &info, ModelDrawInfo &out, bool is_decoupled, const DPSprite* psp)
{
	//reset drawinfo
	out.modelid = -1;
	out.animationid = -1;
	out.modelframe = -1;
	out.modelframenext = -1;
	out.modelframe_explicit = false;
	out.skinid.SetNull();
	out.surfaceskinids.Clear();

	// RS FORK -- PER-PART HIDE (p_pspr.h). Returning false is how this function
	// already says "do not draw model index i", and the caller's loop skips
	// RenderModelFrame for it.
	//
	// FIRST, immediately after the reset and before anything is resolved: a
	// part that is not drawn has no model id, frame or skin worth computing,
	// and leaving drawinfo at its reset values is the honest state for one.
	//
	// This is what "the magazine is out of the gun" is. The alternative the old
	// mod used -- point the part at a frame index that does not exist and let
	// the renderer reject it -- draws nothing only by accident and needs a
	// junk frame to aim at.
	//
	// CAVEAT FOR RIGGED MODELS: RenderModelFrame threads boneStartingPosition
	// and evaluatedSingle across iterations, so skipping a part of an IQM whose
	// bones are evaluated once for the whole stack can leave later parts
	// reading a bone offset that was never written. MD3 has no bones and is the
	// case this exists for; hiding a part of a skinned model is untested.
	if (psp && i >= 0 && i < DPSprite::RS_MODEL_PARTS && psp->ModelPartHidden[i])
	{
		return false;
	}

	if (data)
	{
		//modelID
		if (data->models.SSize() > i && data->models[i].modelID >= 0)
		{
			out.modelid = data->models[i].modelID;
		}
		else if(data->models.SSize() > i && data->models[i].modelID == -2)
		{
			return false;
		}
		else if(smf->modelsAmount > i)
		{
			out.modelid = smf->modelIDs[i];
		}

		//animationID
		if (data->animationIDs.SSize() > i && data->animationIDs[i] >= 0)
		{
			out.animationid = data->animationIDs[i];
		}
		else if(smf->modelsAmount > i)
		{
			out.animationid = smf->animationIDs[i];
		}
		if(!is_decoupled)
		{
			//modelFrame
			if (data->modelFrameGenerators.SSize() > i
				&& (unsigned)data->modelFrameGenerators[i] < info.modelsamount
				&& smf->modelframes[data->modelFrameGenerators[i]] >= 0
				) {
				out.modelframe = smf->modelframes[data->modelFrameGenerators[i]];

				if (info.smfNext)
				{
					if(info.smfNext->modelframes[data->modelFrameGenerators[i]] >= 0)
					{
						out.modelframenext = info.smfNext->modelframes[data->modelFrameGenerators[i]];
					}
					else
					{
						out.modelframenext = info.smfNext->modelframes[i];
					}
				}
			}
			else if(smf->modelsAmount > i)
			{
				out.modelframe = smf->modelframes[i];
				if (info.smfNext) out.modelframenext = info.smfNext->modelframes[i];
			}
		}

		//skinID
		if (data->skinIDs.SSize() > i && data->skinIDs[i].isValid())
		{
			out.skinid = data->skinIDs[i];
		}
		else if(smf->modelsAmount > i)
		{
			out.skinid = smf->skinIDs[i];
		}

		//surfaceSkinIDs
		if(data->models.SSize() > i && data->models[i].surfaceSkinIDs.SSize() > 0)
		{
			unsigned sz1 = smf->surfaceskinIDs.Size();
			unsigned sz2 = data->models[i].surfaceSkinIDs.Size();
			unsigned start = i * MD3_MAX_SURFACES;

			out.surfaceskinids = data->models[i].surfaceSkinIDs;
			out.surfaceskinids.Resize(MD3_MAX_SURFACES);

			for (unsigned surface = 0; surface < MD3_MAX_SURFACES; surface++)
			{
				if (sz2 > surface && (data->models[i].surfaceSkinIDs[surface].isValid()))
				{
					continue;
				}
				if((surface + start) < sz1)
				{
					out.surfaceskinids[surface] = smf->surfaceskinIDs[surface + start];
				}
				else
				{
					out.surfaceskinids[surface].SetNull();
				}
			}
		}
	}
	else
	{
		out.modelid = smf->modelIDs[i];
		out.animationid = smf->animationIDs[i];
		out.modelframe = smf->modelframes[i];
		if (info.smfNext) out.modelframenext = info.smfNext->modelframes[i];
		out.skinid = smf->skinIDs[i];
	}

	// RS FORK -- DIRECT MODEL FRAME ADDRESSING.
	// Every branch above resolved the frame through the sprite: a letter index
	// capped at MAX_SPRITE_FRAMES (29) fed through MODELDEF's FrameIndex table.
	// Our weapon meshes run past 70 frames, so most of their animation had no
	// letter to name it with and could not be reached at all.
	//
	// The psprite's ModelFrame replaces the NUMBER only. Everything else the
	// smf carries -- scale, offsets, angle/pitch/roll, skins, flags -- still
	// comes from the sprite lookup, which is why FindModelFrame must still
	// resolve upstream of here.
	//
	// Applied to every model index. A donor with several models is showing
	// frames of one animation, so they advance together. Where a caller DOES
	// need the parts to diverge -- a gun whose slide moves while its magazine
	// is gone -- the per-part arrays below override this for their own index;
	// see p_pspr.h.
	//
	// Out of range is not clamped on purpose: FMD3Model::RenderFrame rejects
	// (unsigned)frameno >= Frames.Size() and draws nothing, which is a visible
	// failure. Silently clamping to the last frame would hide the bug.
	if (psp && psp->ModelFrame >= 0)
	{
		out.modelframe     = psp->ModelFrame;
		out.modelframenext = (psp->ModelFrameNext >= 0) ? psp->ModelFrameNext : psp->ModelFrame;
		out.modelframe_explicit = true;
	}
	// RS FORK -- the same, for a WORLD ACTOR. Checked second so a psprite still
	// wins on the HUD path; the two never both apply to one draw.
	else if (info.actor && info.actor->ModelFrame >= 0)
	{
		out.modelframe     = info.actor->ModelFrame;
		out.modelframenext = (info.actor->ModelFrameNext >= 0)
			? info.actor->ModelFrameNext : info.actor->ModelFrame;
		out.modelframe_explicit = true;
	}

	// RS FORK -- PER-PART FRAME, and it wins (p_pspr.h).
	//
	// LAST, so it beats both branches above for its own index and only its own
	// index. That ordering is the whole contract: a caller may set the scalar
	// as a base pose for the stack and then move one part off it, and a caller
	// that sets no per-part value -- ModelSwapper, every existing weapon -- is
	// bit-for-bit unaffected because the array is all -1.
	//
	// Out of range is not clamped, for the reason given above: FMD3Model::
	// RenderFrame rejects an impossible frame and draws nothing, which is a
	// visible failure rather than a silent wrong pose.
	if (psp && i >= 0 && i < DPSprite::RS_MODEL_PARTS && psp->ModelFramePart[i] >= 0)
	{
		out.modelframe     = psp->ModelFramePart[i];
		out.modelframenext = (psp->ModelFrameNextPart[i] >= 0)
			? psp->ModelFrameNextPart[i] : psp->ModelFramePart[i];
		out.modelframe_explicit = true;
	}

	if (vr_pose_debug && psp)
	{
		// Separate slots per hand so the two do not mask each other's changes.
		const bool offhand = (psp->GetID() >= PSP_OFFHANDWEAPON);
		static int lastMain = -0x7fffffff, lastOff = -0x7fffffff;
		int &last = offhand ? lastOff : lastMain;
		int key = (psp->ModelFrame * 4) + (out.modelframe_explicit ? 1 : 0) + (out.modelframe << 12);
		if (key != last)
		{
			last = key;
			Printf("[POSE/in ] %s layer=%d psp.ModelFrame=%d -> drawinfo.modelframe=%d next=%d explicit=%s\n",
				offhand ? "OFF " : "MAIN", psp->GetID(), psp->ModelFrame,
				out.modelframe, out.modelframenext,
				out.modelframe_explicit ? "yes" : "NO");
		}
	}

	// RS FORK -- NATIVE STATE REMAP frame resolution (FORK_CHANGES.md,
	// "Native state remap"). The table maps the psprite's CURRENT state
	// directly to mesh frames, registered once at bind time from ZScript.
	// Placed last on purpose: when a table exists it beats both the sprite
	// lookup and the legacy per-tick ModelFrame fields (which can linger,
	// serialized, from older builds). Unmapped states fall through to
	// whatever resolved above -- the pinned anchor's rest pose -- so a
	// state the walk couldn't see reads as a pause, never as garbage.
	if (psp && data && data->stateRemap.CountUsed() > 0)
	{
		FState *st = psp->GetState();
		if (st != nullptr)
		{
			int64_t *v = data->stateRemap.CheckKey(intptr_t(st));
			if (v != nullptr)
			{
				out.modelframe     = int(uint32_t(*v >> 32));
				out.modelframenext = int(uint32_t(*v & 0xffffffff));
				out.modelframe_explicit = true;
			}
		}
	}

	return (out.modelid >= 0 && out.modelid < Models.SSize());
}


const TArray<VSMatrix> * ProcessModelFrame(FModel * animation, bool nextFrame, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, BoneInfo *out)
{
	const TArray<TRS>* animationData = nullptr;

	if (modelData && modelData->useProceduralPose && modelData->proceduralPose.Size() > 0)
	{
		// [XR] Procedurally supplied per-bone pose (the VR body's arm IK, playsim/vr_armik.cpp, or
		// ZScript SetModelBonePose) overrides any baked animation. CalculateBonesIQM already branches
		// on (animationData ? *animationData : TRSData): one frame's worth of TRS, frame index 0.
		animationData = &modelData->proceduralPose;
		g_xr_vrRenderProcHits++;
		{ static int s_vrRenderDbg = 0; if (s_vrRenderDbg < 20) { s_vrRenderDbg++; Printf("[VRIK_RENDER] useProc=1 poseSize=%d is_decoupled=%d modelframe=%d\n", (int)modelData->proceduralPose.Size(), (int)is_decoupled, drawinfo.modelframe); } }
	}
	else if (drawinfo.animationid >= 0)
	{
		animation = Models[drawinfo.animationid];
		animationData = animation->AttachAnimationData();
	}

	const TArray<VSMatrix> *boneData = nullptr;

	if (vr_pose_debug && is_decoupled)
	{
		static int last = -0x7fffffff;
		const int branch = (frameinfo.decoupled_frame.frame1 >= 0) ? 0
		                 : (drawinfo.modelframe_explicit ? 1 : 2);
		int key = branch + (drawinfo.modelframe << 4);
		if (key != last)
		{
			last = key;
			static const char *names[3] = { "ANIM (SetAnimation wins)", "STATIC (our pose)", "REST (pose discarded)" };
			Printf("[POSE/out] decoupled -> %s  frame=%d next=%d  decoupled_frame1=%d\n",
				names[branch], drawinfo.modelframe, drawinfo.modelframenext,
				frameinfo.decoupled_frame.frame1);
		}
	}

	if(is_decoupled)
	{
		if(frameinfo.decoupled_frame.frame1 >= 0)
		{
			boneData = animation->CalculateBones(
				frameinfo.decoupled_frame_prev ? *frameinfo.decoupled_frame_prev : nullptr,
				frameinfo.decoupled_frame,
				frameinfo.inter,
				animationData,
				(modelData && modelData->modelBoneOverrides.SSize() > i)
				? &modelData->modelBoneOverrides[i]
				: nullptr,
				out,
				tic);
		}
		else if(drawinfo.modelframe_explicit)
		{
			// RS FORK -- STATIC POSE ON THE DECOUPLED PATH.
			//
			// A +DECOUPLEDANIMATIONS model has only two states here: playing an
			// animation (above), or the rest pose (below). Neither consults
			// drawinfo.modelframe, which is read only on the non-decoupled
			// branch -- so a decoupled model could not be pinned to a single
			// authored frame at all.
			//
			// That is exactly what hand posing needs. The poses are baked as
			// frames of one clip and ZScript picks one per tic from controller
			// input; there is no animation to play, just a pose to hold. Doing
			// it through SetAnimation instead would mean running a clip at zero
			// framerate to keep it still, i.e. fighting the animation clock to
			// get a static result.
			//
			// Same construction as the non-decoupled branch below, so an
			// explicitly addressed frame means the same thing on both paths.
			// Bone overrides still compose on top, so a posed hand can still be
			// adjusted procedurally afterwards.
			boneData = animation->CalculateBones(
				nullptr,
				{
					nextFrame ? frameinfo.inter : -1.0f,
					drawinfo.modelframe,
					drawinfo.modelframenext
				},
				-1.0f,
				animationData,
				(modelData && modelData->modelBoneOverrides.SSize() > i)
				? &modelData->modelBoneOverrides[i]
				: nullptr,
				out,
				tic);
		}
		else
		{
			boneData = animation->CalculateBonesOnlyOffsets(
				(modelData && modelData->modelBoneOverrides.SSize() > i)
				? &modelData->modelBoneOverrides[i]
				: nullptr,
				out,
				tic);
		}
	}
	else
	{
		boneData = animation->CalculateBones(
			nullptr,
			{
				nextFrame ? frameinfo.inter : -1.0f,
				drawinfo.modelframe,
				drawinfo.modelframenext
			},
			-1.0f,
			animationData,
			(modelData && modelData->modelBoneOverrides.SSize() > i)
			? &modelData->modelBoneOverrides[i]
			: nullptr,
			out,
			tic);
	}

	return boneData;
}

static inline void RenderModelFrame(FModelRenderer *renderer, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, FTranslationID translation, int &boneStartingPosition, bool &evaluatedSingle, const DPSprite *psp = nullptr)
{
	FModel * mdl = Models[drawinfo.modelid];
	auto tex = drawinfo.skinid.isValid() ? TexMan.GetGameTexture(drawinfo.skinid, true) : nullptr;
	mdl->BuildVertexBuffer(renderer);

	auto ssidp = drawinfo.surfaceskinids.Size() > 0
		? drawinfo.surfaceskinids.Data()
		: (((i * MD3_MAX_SURFACES) < smf->surfaceskinIDs.SSize()) ? &smf->surfaceskinIDs[i * MD3_MAX_SURFACES] : nullptr);

	bool nextFrame = frameinfo.smfNext && drawinfo.modelframe != drawinfo.modelframenext;

	// [Jay] while per-model animations aren't done, DECOUPLEDANIMATIONS does the same as MODELSAREATTACHMENTS
	if(!evaluatedSingle)
	{  // [Jay] TODO per-model decoupled animations
		const TArray<VSMatrix> *boneData = ProcessModelFrame(mdl, nextFrame, i, smf, modelData, frameinfo, drawinfo, is_decoupled, tic, nullptr);

		if(frameinfo.smf_flags & MDL_MODELSAREATTACHMENTS || is_decoupled)
		{
			if(!boneData && is_decoupled)
			{
				boneData = mdl->CalculateBonesOnlyOffsets((modelData && modelData->modelBoneOverrides.SSize() > i)? &modelData->modelBoneOverrides[i] : nullptr, nullptr, tic);
			}

			// [RS FORK] Upstream's new CalculateBonesOnlyOffsets path only covers
			// the decoupled case. Keep the fork's base-pose fallback so a
			// MDL_MODELSAREATTACHMENTS model that is NOT decoupled still gets a
			// bone set uploaded instead of rendering unskinned.
			if(!boneData)
			{
				boneData = mdl->GetBasePose();
			}

			boneStartingPosition = boneData ? screen->mBones->UploadBones(*boneData) : -1;
			evaluatedSingle = true;

		}

		// Publish this model's bones for anything anchored to this layer.
		//
		// Outside the branch above on purpose. That branch only runs for
		// attachment sets and decoupled models, and anchoring has no reason to
		// require either -- a plain weapon model resolves perfectly good bones and
		// something should be able to hang off them. Keeping the store inside it
		// meant a non-decoupled weapon silently published nothing, so anchoring to
		// it did nothing and looked like a script bug.
		if (psp && boneData) HudAnchor_Store(psp, mdl, *boneData);
	}

	mdl->RenderFrame(renderer, tex, drawinfo.modelframe, nextFrame ? drawinfo.modelframenext : drawinfo.modelframe, nextFrame ? frameinfo.inter : -1.f, translation, ssidp, boneStartingPosition);
}

void RenderFrameModels(FModelRenderer *renderer, FLevelLocals *Level, const FSpriteModelFrame *smf, const FState *curState, int curTics, double ticFrac, FTranslationID translation, AActor* actor, const DPSprite* psp)
{
	double tic = actor->GetModelTimer();
	if (!WorldPaused(true) && !actor->isFrozen())
	{
		tic += ticFrac;
	}

	bool is_decoupled = (actor->flags9 & MF9_DECOUPLEDANIMATIONS);

	DActorModelData* modelData = actor ? actor->modelData.ForceGet() : nullptr;

	// [XR] Diagnostic: what the renderer actually sees on the VR body, independent of any branch below.
	if (actor && VR_IsBodyActor(actor))
	{
		static int s_rfm = 0;
		if ((s_rfm++ % 140) == 0)
		{
			Printf("[VRIK_RFM] actor=%p md=%p useProc=%d pose=%u models=%u smfModels=%u decoupled=%d hits=%d\n",
				actor, modelData, modelData ? (int)modelData->useProceduralPose : -1,
				modelData ? modelData->proceduralPose.Size() : 0u, modelData ? modelData->models.Size() : 0u,
				smf->modelsAmount, (int)is_decoupled, g_xr_vrRenderProcHits);
		}
	}

	CalcModelFrameInfo frameinfo = CalcModelFrame(Level, smf, curState, curTics, modelData, actor, is_decoupled, tic, ticFrac, psp);
	ModelDrawInfo drawinfo;

	int boneStartingPosition = -1;
	bool evaluatedSingle = false;

	// RS FORK -- PER-PART BLEND (p_pspr.h).
	//
	// The blend factor lives in frameinfo, which is computed once for the whole
	// stack, while a per-part blend is by definition per part. So the base is
	// captured here and frameinfo.inter is re-seeded from it at the top of every
	// iteration before any per-part value replaces it.
	//
	// RE-SEEDING IS THE LOAD-BEARING HALF. Writing the part's factor straight
	// into frameinfo would leak it into every LATER part that has no factor of
	// its own -- so racking a slide would smear the blend across the frame,
	// the magazine and the hands, which is precisely the coupling this whole
	// mechanism exists to remove.
	//
	// The blend TARGET (smfNext) was settled in CalcModelFrame; without it
	// RenderModelFrame discards inter and nothing below has any effect.
	const float baseInter = frameinfo.inter;
	const bool  anyPart   = (psp && psp->AnyModelPartActive());

	for (unsigned i = 0; i < frameinfo.modelsamount; i++)
	{
		if (anyPart)
		{
			frameinfo.inter = baseInter;
			if (i < (unsigned)DPSprite::RS_MODEL_PARTS && psp->ModelFrameLerpPart[i] >= 0.f)
			{
				float f = psp->ModelFrameLerpPart[i];
				if (f > 1.f) f = 1.f;
				frameinfo.inter = f;
			}
		}

		if (CalcModelOverrides(i, smf, modelData, frameinfo, drawinfo, is_decoupled, psp))
		{
			RenderModelFrame(renderer, i, smf, modelData, frameinfo, drawinfo, is_decoupled, tic, translation, boneStartingPosition, evaluatedSingle, psp);
		}
	}
}


static TArray<int> SpriteModelHash;
//TArray<FStateModelFrame> StateModelFrames;

//===========================================================================
//
// InitModels
//
//===========================================================================

void ParseModelDefLump(int Lump);

void InitModels()
{
	Models.DeleteAndClear();
	SpriteModelFrames.Clear();
	SpriteModelHash.Clear();

	// First, create models for each voxel
	for (unsigned i = 0; i < Voxels.Size(); i++)
	{
		FVoxelModel *md = new FVoxelModel(Voxels[i], false);
		Voxels[i]->VoxelIndex = Models.Push(md);
	}
	// now create GL model frames for the voxeldefs
	for (unsigned i = 0; i < VoxelDefs.Size(); i++)
	{
		FVoxelModel *md = (FVoxelModel*)Models[VoxelDefs[i]->Voxel->VoxelIndex];
		FSpriteModelFrame smf;
		memset((void*)&smf, 0, sizeof(smf));
		smf.isVoxel = true;
		smf.modelsAmount = 1;
		smf.modelframes.Alloc(1);
		smf.modelframes[0] = -1;
		smf.modelIDs.Alloc(1);
		smf.modelIDs[0] = VoxelDefs[i]->Voxel->VoxelIndex;
		smf.skinIDs.Alloc(1);
		smf.skinIDs[0] = md->GetPaletteTexture();
		smf.animationIDs.Alloc(1);
		smf.animationIDs[0] = -1;
		smf.xscale = smf.yscale = smf.zscale = VoxelDefs[i]->Scale;
		smf.angleoffset = VoxelDefs[i]->AngleOffset.Degrees();
		smf.xoffset = VoxelDefs[i]->xoffset;
		smf.yoffset = VoxelDefs[i]->yoffset;
		smf.zoffset = VoxelDefs[i]->zoffset;
		// this helps catching uninitialized data.
		assert(VoxelDefs[i]->PitchFromMomentum == true || VoxelDefs[i]->PitchFromMomentum == false);
		if (VoxelDefs[i]->PitchFromMomentum) smf.flags |= MDL_PITCHFROMMOMENTUM;
		if (VoxelDefs[i]->UseActorPitch) smf.flags |= MDL_USEACTORPITCH;
		if (VoxelDefs[i]->UseActorRoll) smf.flags |= MDL_USEACTORROLL;
		if (VoxelDefs[i]->PlacedSpin != 0)
		{
			smf.yrotate = 1.f;
			smf.rotationSpeed = VoxelDefs[i]->PlacedSpin / 55.55f;
			smf.flags |= MDL_ROTATING;
		}
		VoxelDefs[i]->VoxeldefIndex = SpriteModelFrames.Push(smf);
		if (VoxelDefs[i]->PlacedSpin != VoxelDefs[i]->DroppedSpin)
		{
			if (VoxelDefs[i]->DroppedSpin != 0)
			{
				smf.yrotate = 1.f;
				smf.rotationSpeed = VoxelDefs[i]->DroppedSpin / 55.55f;
				smf.flags |= MDL_ROTATING;
			}
			else
			{
				smf.yrotate = 0;
				smf.rotationSpeed = 0;
				smf.flags &= ~MDL_ROTATING;
			}
			SpriteModelFrames.Push(smf);
		}
	}

	int Lump;
	int lastLump = 0;
	while ((Lump = fileSystem.FindLump("MODELDEF", &lastLump)) != -1)
	{
		ParseModelDefLump(Lump);
	}

	// create a hash table for quick access
	SpriteModelHash.Resize(SpriteModelFrames.Size ());
	memset(SpriteModelHash.Data(), 0xff, SpriteModelFrames.Size () * sizeof(int));

	for (unsigned int i = 0; i < SpriteModelFrames.Size (); i++)
	{
		int j = ModelFrameHash(&SpriteModelFrames[i]) % SpriteModelFrames.Size ();

		SpriteModelFrames[i].hashnext = SpriteModelHash[j];
		SpriteModelHash[j]=i;
	}
}

void ParseModelDefLump(int Lump)
{
	FScanner sc(Lump);
	while (sc.GetString())
	{
		if (sc.Compare("model"))
		{
			int index, surface;
			FString path = "";
			sc.MustGetString();

			FSpriteModelFrame smf;
			memset((void*)&smf, 0, sizeof(smf));
			smf.xscale=smf.yscale=smf.zscale=1.f;

			auto type = PClass::FindClass(sc.String);
			if (!type || type->Defaults == nullptr)
			{
				sc.ScriptError("MODELDEF: Unknown actor type '%s'\n", sc.String);
			}
			smf.type = type;
			FScanner::SavedPos scPos = sc.SavePos();
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("model"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					if (index < 0)
					{
						sc.ScriptError("Model index must be 0 or greater in %s", type->TypeName.GetChars());
					}
					smf.modelsAmount = index + 1;
				}
			}
			//Make sure modelsAmount is at least equal to MIN_MODELS(4) to ensure compatibility with old mods
			if (smf.modelsAmount < MIN_MODELS)
			{
				smf.modelsAmount = MIN_MODELS;
			}

			const auto initArray = [](auto& array, const unsigned count, const auto value)
			{
				array.Alloc(count);
				std::fill(array.begin(), array.end(), value);
			};

			initArray(smf.modelIDs, smf.modelsAmount, -1);
			initArray(smf.skinIDs, smf.modelsAmount, FNullTextureID());
			initArray(smf.surfaceskinIDs, smf.modelsAmount * MD3_MAX_SURFACES, FNullTextureID());
			initArray(smf.animationIDs, smf.modelsAmount, -1);
			initArray(smf.modelframes, smf.modelsAmount, 0);

			sc.RestorePos(scPos);
			sc.MustGetStringName("{");
			while (!sc.CheckString("}"))
			{
				sc.MustGetString();
				if (sc.Compare("path"))
				{
					sc.MustGetString();
					FixPathSeperator(sc.String);
					path = sc.String;
					if (path[(int)path.Len()-1]!='/') path+='/';
				}
				else if (sc.Compare("model"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					if (index < 0)
					{
						sc.ScriptError("Model index must be 0 or greater in %s", type->TypeName.GetChars());
					}
					else if (index >= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					sc.MustGetString();
					FixPathSeperator(sc.String);
					smf.modelIDs[index] = FindModel(path.GetChars(), sc.String);
					if (smf.modelIDs[index] == -1)
					{
						Printf("%s: model not found in %s\n", sc.String, path.GetChars());
					}
				}
				else if (sc.Compare("animation"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					if (index < 0)
					{
						sc.ScriptError("Animation index must be 0 or greater in %s", type->TypeName.GetChars());
					}
					else if (index >= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					sc.MustGetString();
					FixPathSeperator(sc.String);
					smf.animationIDs[index] = FindModel(path.GetChars(), sc.String);
					if (smf.animationIDs[index] == -1)
					{
						Printf("%s: animation model not found in %s\n", sc.String, path.GetChars());
					}
				}
				else if (sc.Compare("scale"))
				{
					sc.MustGetFloat();
					smf.xscale = sc.Float;
					sc.MustGetFloat();
					smf.yscale = sc.Float;
					sc.MustGetFloat();
					smf.zscale = sc.Float;
				}
				// [BB] Added zoffset reading.
				// Now it must be considered deprecated.
				else if (sc.Compare("zoffset"))
				{
					sc.MustGetFloat();
					smf.zoffset=sc.Float;
				}
				// Offset reading.
				else if (sc.Compare("offset"))
				{
					sc.MustGetFloat();
					smf.xoffset = sc.Float;
					sc.MustGetFloat();
					smf.yoffset = sc.Float;
					sc.MustGetFloat();
					smf.zoffset = sc.Float;
				}
				// [BB] PivotOffset -- the point the model TURNS ABOUT, in its own
				// space. Same three axes and the same units as Offset above, and
				// deliberately spelled to sit next to it, because the two are
				// constantly confused: Offset moves the model, PivotOffset moves
				// what it rotates around. See the field note in model.h.
				else if (sc.Compare("pivotoffset"))
				{
					sc.MustGetFloat();
					smf.pivotx = sc.Float;
					sc.MustGetFloat();
					smf.pivoty = sc.Float;
					sc.MustGetFloat();
					smf.pivotz = sc.Float;
				}
				// angleoffset, pitchoffset and rolloffset reading.
				else if (sc.Compare("angleoffset"))
				{
					sc.MustGetFloat();
					smf.angleoffset = sc.Float;
				}
				else if (sc.Compare("placementcvars"))
				{
					// One prefix, six CVARs by convention. Declaring them is the
					// mod's job -- a missing CVAR reads as zero, so a half-finished
					// set degrades to the MODELDEF values instead of failing.
					sc.MustGetString();
					smf.placementCVars = sc.String;
				}
				else if (sc.Compare("pitchoffset"))
				{
					sc.MustGetFloat();
					smf.pitchoffset = sc.Float;
				}
				else if (sc.Compare("rolloffset"))
				{
					sc.MustGetFloat();
					smf.rolloffset = sc.Float;
				}
				// RS FORK -- the bakeable twin of the vr_hand_* sliders. Same
				// frame, same order, same sign, so a tuned slider value can be
				// written here verbatim and mean exactly what it did live.
				// See FSpriteModelFrame in model.h for why these cannot simply
				// be folded into angleoffset/pitchoffset/rolloffset.
				else if (sc.Compare("handangleoffset"))
				{
					sc.MustGetFloat();
					smf.handangleoffset = sc.Float;
				}
				else if (sc.Compare("handpitchoffset"))
				{
					sc.MustGetFloat();
					smf.handpitchoffset = sc.Float;
				}
				else if (sc.Compare("handrolloffset"))
				{
					sc.MustGetFloat();
					smf.handrolloffset = sc.Float;
				}
				// [BB] Added model flags reading.
				else if (sc.Compare("ignoretranslation"))
				{
					smf.flags |= MDL_IGNORETRANSLATION;
				}
				else if (sc.Compare("followmainhand"))
				{
					smf.flags |= MDL_FOLLOWMAINHAND;
				}
				else if (sc.Compare("followoffhand"))
				{
					smf.flags |= MDL_FOLLOWOFFHAND;
				}
				else if (sc.Compare("pitchfrommomentum"))
				{
					smf.flags |= MDL_PITCHFROMMOMENTUM;
				}
				else if (sc.Compare("inheritactorpitch"))
				{
					smf.flags |= MDL_USEACTORPITCH | MDL_BADROTATION;
				}
				else if (sc.Compare("inheritactorroll"))
				{
					smf.flags |= MDL_USEACTORROLL;
				}
				else if (sc.Compare("useactorpitch"))
				{
					smf.flags |= MDL_USEACTORPITCH;
				}
				else if (sc.Compare("useactorroll"))
				{
					smf.flags |= MDL_USEACTORROLL;
				}
				else if (sc.Compare("noperpixellighting"))
				{
					smf.flags |= MDL_NOPERPIXELLIGHTING;
				}
				else if (sc.Compare("scaleweaponfov"))
				{
					smf.flags |= MDL_SCALEWEAPONFOV;
				}
				else if (sc.Compare("modelsareattachments"))
				{
					smf.flags |= MDL_MODELSAREATTACHMENTS;
				}
				else if (sc.Compare("viewmodelfov"))
				{
					sc.MustGetFloat();
					smf.viewModelFOV = sc.Float;
					if (smf.viewModelFOV > 0.0f)
						smf.viewModelFOV = min<float>(smf.viewModelFOV, 175.0f);
				}
				else if (sc.Compare("rotating"))
				{
					smf.flags |= MDL_ROTATING;
					smf.xrotate = 0.;
					smf.yrotate = 1.;
					smf.zrotate = 0.;
					smf.rotationCenterX = 0.;
					smf.rotationCenterY = 0.;
					smf.rotationCenterZ = 0.;
					smf.rotationSpeed = 1.;
				}
				else if (sc.Compare("fix-rotating"))
				{
					smf.flags |= MDL_FIXROTATING;
				}
				else if (sc.Compare("rotation-speed"))
				{
					sc.MustGetFloat();
					smf.rotationSpeed = sc.Float;
				}
				else if (sc.Compare("rotation-vector"))
				{
					sc.MustGetFloat();
					smf.xrotate = sc.Float;
					sc.MustGetFloat();
					smf.yrotate = sc.Float;
					sc.MustGetFloat();
					smf.zrotate = sc.Float;
				}
				else if (sc.Compare("rotation-center"))
				{
					sc.MustGetFloat();
					smf.rotationCenterX = sc.Float;
					sc.MustGetFloat();
					smf.rotationCenterY = sc.Float;
					sc.MustGetFloat();
					smf.rotationCenterZ = sc.Float;
				}
				else if (sc.Compare("interpolatedoubledframes"))
				{
					smf.flags |= MDL_INTERPOLATEDOUBLEDFRAMES;
				}
				else if (sc.Compare("nointerpolation"))
				{
					smf.flags |= MDL_NOINTERPOLATION;
				}
				else if (sc.Compare("skin"))
				{
					sc.MustGetNumber();
					index=sc.Number;
					if (index<0 || index>= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					sc.MustGetString();
					FixPathSeperator(sc.String);
					if (sc.Compare(""))
					{
						smf.skinIDs[index]=FNullTextureID();
					}
					else
					{
						smf.skinIDs[index] = LoadSkin(path.GetChars(), sc.String);
						if (!smf.skinIDs[index].isValid())
						{
							Printf("Skin '%s' not found in '%s'\n",
								sc.String, type->TypeName.GetChars());
						}
					}
				}
				else if (sc.Compare("surfaceskin"))
				{
					sc.MustGetNumber();
					index = sc.Number;
					sc.MustGetNumber();
					surface = sc.Number;

					if (index<0 || index >= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}

					if (surface<0 || surface >= MD3_MAX_SURFACES)
					{
						sc.ScriptError("Invalid MD3 Surface %d in %s", MD3_MAX_SURFACES, type->TypeName.GetChars());
					}

					sc.MustGetString();
					FixPathSeperator(sc.String);
					int ssIndex = surface + index * MD3_MAX_SURFACES;
					if (sc.Compare(""))
					{
						smf.surfaceskinIDs[ssIndex] = FNullTextureID();
					}
					else
					{
						smf.surfaceskinIDs[ssIndex] = LoadSkin(path.GetChars(), sc.String);
						if (!smf.surfaceskinIDs[ssIndex].isValid())
						{
							Printf("Surface Skin '%s' not found in '%s'\n",
								sc.String, type->TypeName.GetChars());
						}
					}
				}
				else if (sc.Compare("baseframe"))
				{
					FSpriteModelFrame *smfp = &BaseSpriteModelFrames.Insert(type, smf);
					for(int modelID : smf.modelIDs)
					{
						if(modelID >= 0)
							Models[modelID]->baseFrame = smfp;
					}
					GetDefaultByType(type)->hasmodel = true;
				}
				else if (sc.Compare("frameindex") || sc.Compare("frame"))
				{
					bool isframe=!!sc.Compare("frame");

					sc.MustGetString();
					smf.sprite = -1;
					for (int i = 0; i < (int)sprites.Size (); ++i)
					{
						if (strnicmp (sprites[i].name, sc.String, 4) == 0)
						{
							if (sprites[i].numframes==0)
							{
								//sc.ScriptError("Sprite %s has no frames", sc.String);
							}
							smf.sprite = i;
							break;
						}
					}
					if (smf.sprite==-1)
					{
						sc.ScriptError("Unknown sprite %s in model definition for %s", sc.String, type->TypeName.GetChars());
					}

					sc.MustGetString();
					FString framechars = sc.String;

					sc.MustGetNumber();
					index=sc.Number;
					if (index<0 || index>= smf.modelsAmount)
					{
						sc.ScriptError("Too many models in %s", type->TypeName.GetChars());
					}
					if (isframe)
					{
						sc.MustGetString();
						if (smf.modelIDs[index] >= 0)
						{
							FModel *model = Models[smf.modelIDs[index]];
							if (smf.animationIDs[index] >= 0)
							{
								model = Models[smf.animationIDs[index]];
							}
							smf.modelframes[index] = model->FindFrame(sc.String);
							if (smf.modelframes[index]==-1) sc.ScriptError("Unknown frame '%s' in %s", sc.String, type->TypeName.GetChars());
						}
						else smf.modelframes[index] = -1;
					}
					else
					{
						sc.MustGetNumber();
						smf.modelframes[index] = sc.Number;
					}

					for (int i = 0; i < static_cast<int>(framechars.Len()) && framechars[i] > 0; i++)
					{
						char map[29]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
						int c = toupper(framechars[i])-'A';

						if (c<0 || c>=29)
						{
							sc.ScriptError("Invalid frame character %c found", c+'A');
						}
						if (map[c]) continue;
						smf.frame=c;
						SpriteModelFrames.Push(smf);
						GetDefaultByType(type)->hasmodel = true;
						map[c]=1;
					}
				}
				else if (sc.Compare("dontcullbackfaces"))
				{
					smf.flags |= MDL_DONTCULLBACKFACES;
				}
				else if (sc.Compare("userotationcenter"))
				{
					smf.flags |= MDL_USEROTATIONCENTER;
					smf.rotationCenterX = 0.;
					smf.rotationCenterY = 0.;
					smf.rotationCenterZ = 0.;
				}
				else if (sc.Compare("correctpixelstretch"))
				{
					smf.flags |= MDL_CORRECTPIXELSTRETCH;
				}
				else if (sc.Compare("forcecullbackfaces"))
				{
					smf.flags |= MDL_FORCECULLBACKFACES;
				}
				else if (sc.Compare("usehandoffsets"))
				{
					smf.flags |= MDL_USEHANDOFFSETS;
				}
				else if (sc.Compare("ignoreskinalpha"))
				{
					smf.flags |= MDL_IGNORESKINALPHA;
				}
				else if (sc.Compare("noautoreverse"))
				{
					smf.flags |= MDL_NOAUTOREVERSE;
				}
				else
				{
					sc.ScriptMessage("Unrecognized string \"%s\"", sc.String);
				}
			}
		}
		else if (sc.Compare("#include"))
		{
			sc.MustGetString();
			// This is not using sc.Open because it can print a more useful error message when done here
			int includelump = fileSystem.CheckNumForFullName(sc.String, true);
			if (includelump == -1)
			{
				if (strcmp(sc.String, "sentinel.modl") != 0) // Gene Tech mod has a broken #include statement
					sc.ScriptError("Lump '%s' not found", sc.String);
			}
			else
			{
				ParseModelDefLump(includelump);
			}
		}
	}
}

//===========================================================================
//
// FindModelFrame
//
//===========================================================================

//===========================================================================
//
// [BB] FindVoxelFrame
//
// The voxel half of the lookup, lifted out of FindModelFrameRaw so the
// per-actor override below can reach it without duplicating the walk or the
// dropped-spin rule. Deliberately does NOT consult r_drawvoxels: the two
// callers disagree about that on purpose -- the ordinary path is gated by the
// cvar, the per-actor override is not.
//
// Voxels are keyed on the SPRITE FRAME, not on a class, which is the whole
// reason a per-actor opt-in has to live outside this function.
//
//===========================================================================

FSpriteModelFrame * FindVoxelFrame(int sprite, int frame, bool dropped)
{
	if (sprite < 0 || sprite >= (int)sprites.Size()) return nullptr;

	spritedef_t *sprdef = &sprites[sprite];
	if (frame >= sprdef->numframes) return nullptr;

	spriteframe_t *sprframe = &SpriteFrames[sprdef->spriteframes + frame];
	if (sprframe->Voxel == nullptr) return nullptr;

	int index = sprframe->Voxel->VoxeldefIndex;
	if (dropped && sprframe->Voxel->DroppedSpin != sprframe->Voxel->PlacedSpin) index++;
	return &SpriteModelFrames[index];
}

FSpriteModelFrame * FindModelFrameRaw(const AActor * actorDefaults, const PClass * ti, int sprite, int frame, bool dropped)
{
	if(actorDefaults->hasmodel)
	{
		FSpriteModelFrame smf;

		memset((void*)&smf, 0, sizeof(smf));
		smf.type = ti;
		smf.sprite = sprite;
		smf.frame = frame;

		int hash = SpriteModelHash[ModelFrameHash(&smf) % SpriteModelFrames.Size()];

		while (hash>=0)
		{
			FSpriteModelFrame * smff = &SpriteModelFrames[hash];
			if (smff->type == ti && smff->sprite == sprite && smff->frame == frame) return smff;
			hash = smff->hashnext;
		}
	}

	// Check for voxel replacements
	if (r_drawvoxels)
	{
		FSpriteModelFrame *vox = FindVoxelFrame(sprite, frame, dropped);
		if (vox != nullptr) return vox;
	}

	return nullptr;
}

FSpriteModelFrame * FindModelFrame(const PClass * ti, int sprite, int frame, bool dropped)
{
	auto def = GetDefaultByType(ti);

	if (def->hasmodel)
	{
		if(def->flags9 & MF9_DECOUPLEDANIMATIONS)
		{
			FSpriteModelFrame * smf = BaseSpriteModelFrames.CheckKey(ti);
			if(smf) return smf;
		}
	}

	return FindModelFrameRaw(def, ti, sprite, frame, dropped);
}

FSpriteModelFrame * FindModelFrame(const PClass * ti, bool is_decoupled, int sprite, int frame, bool dropped)
{
	if(!ti) return nullptr;

	if(is_decoupled)
	{
		return BaseSpriteModelFrames.CheckKey(ti);
	}
	else
	{
		return FindModelFrameRaw(GetDefaultByType(ti), ti, sprite, frame, dropped);
	}
}

FSpriteModelFrame * FindModelFrame(AActor * thing, int sprite, int frame, bool dropped)
{
	if(!thing) return nullptr;

	// [BB] PER-ACTOR VOXEL OVERRIDE.
	//
	// Voxels are otherwise all-or-nothing: the lookup keys on a sprite frame
	// and is gated by one global cvar, so loading a voxel pack turns EVERY
	// actor that has one into a voxel, everywhere, with no way to ask for it
	// on a single object. VoxelOverride is that way.
	//
	// This is the hook for "a thing you are physically holding becomes a real
	// 3D object". A billboard cannot be turned over in your hand -- it always
	// faces you -- so a grabbed item wants to be a voxel for exactly as long
	// as it is held, and a sprite again the moment it is dropped. Set the
	// field on grab, clear it on release.
	//
	// TWO DELIBERATE DIFFERENCES from the ordinary path, both of which are the
	// point of the feature rather than oversights:
	//
	//   IT IGNORES r_drawvoxels. That cvar means "draw voxels for everything",
	//   and the case this exists for is a pack loaded with it switched OFF.
	//   Gating the override on it would make the feature unreachable in the
	//   exact configuration it was built for.
	//
	//   IT OUTRANKS A MODEL. Ordinarily a model wins and the voxel is only a
	//   fallback (see FindModelFrameRaw). Here the caller has explicitly asked
	//   for the voxel on this one actor, so it takes precedence -- otherwise
	//   anything carrying a MODELDEF could never be overridden, which includes
	//   most of what a mod would want to pick up.
	//
	// Falls through when the actor has no voxel for its current frame, so
	// setting the field on something without one costs a null check and
	// changes nothing.
	if (thing->VoxelOverride)
	{
		FSpriteModelFrame *vox = FindVoxelFrame(sprite, frame, dropped);

		// [BB] REPORT THE MISS, NOT JUST THE HIT.
		//
		// The first cut of this trace lived in ObjectToWorldMatrix, which only
		// ever runs on something that has ALREADY resolved to a voxel -- so the
		// one outcome worth knowing about, "asked for a voxel and there is not
		// one", printed nothing at all and read exactly like the trace being
		// broken. This is the decision itself: what was asked for, by which
		// sprite and frame, and whether the pack answered.
		if (vr_voxel_debug)
		{
			static const AActor *lastActor = nullptr;
			static int lastTic = -1000;
			if (thing != lastActor || (gametic - lastTic) > TICRATE)
			{
				lastActor = thing;
				lastTic = gametic;
				char sprname[5] = { 0 };
				if (sprite >= 0 && sprite < (int)sprites.Size())
					memcpy(sprname, sprites[sprite].name, 4);
				Printf("[RSVOX] %s  sprite=%s frame=%d dropped=%d  ->  %s\n",
					thing->GetClass()->TypeName.GetChars(),
					sprname, frame, (int)dropped,
					vox ? "VOXEL FOUND" : "no voxel for this frame");
			}
		}

		if (vox != nullptr) return vox;
	}

	return FindModelFrame((thing->modelData != nullptr && thing->modelData->modelDef != nullptr) ? thing->modelData->modelDef : thing->GetClass(), (thing->flags9 & MF9_DECOUPLEDANIMATIONS), sprite, frame, dropped);
}

//===========================================================================
//
// IsHUDModelForPlayerAvailable
//
//===========================================================================

bool IsHUDModelForPlayerAvailable (player_t * player)
{
	if (player == nullptr || player->psprites == nullptr)
		return false;

	// [MK] check that at least one psprite uses models
	for (DPSprite *psp = player->psprites; psp != nullptr && psp->GetID() < PSP_TARGETCENTER; psp = psp->GetNext())
	{
		if ( FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false) != nullptr ) return true;
	}
	return false;
}


unsigned int FSpriteModelFrame::getFlags(class DActorModelData * defs) const
{
	return (defs && defs->flags & MODELDATA_OVERRIDE_FLAGS)? (flags | defs->overrideFlagsSet) & ~(defs->overrideFlagsClear) : flags;
}
