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
EXTERN_CVAR(Bool, r_drawvoxels)
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

	const DVector2 scale = actor->InterpolatedScale(ticFrac);
	float scaleFactorX = scale.X * smf->xscale;
	float scaleFactorY = scale.X * smf->yscale;
	float scaleFactorZ = scale.Y * smf->zscale;
	float orientation = scaleFactorX * scaleFactorY * scaleFactorZ;

	renderer->BeginDrawModel(actor->RenderStyle, smf_flags, objectToWorldMatrix, orientation < 0);
	RenderFrameModels(renderer, actor->Level, smf, actor->state, actor->tics, ticFrac, translation, actor);
	renderer->EndDrawModel(actor->RenderStyle, smf_flags);
}

VSMatrix FSpriteModelFrame::ObjectToWorldMatrix(AActor * actor, float x, float y, float z, double ticFrac)
{
	int smf_flags = getFlags(actor->modelData);

	// Setup transformation.
	DRotator angles;

	if (actor->renderflags & RF_INTERPOLATEANGLES) // [Nash] use interpolated angles
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

	double tic = actor->GetModelTimer();

	if (!WorldPaused(true) && !actor->isFrozen())
	{
		tic += ticFrac;
	}

	return ObjectToWorldMatrix(actor->Level, DVector3(x, y, z), DRotator(DAngle::fromDeg(pitch), DAngle::fromDeg(angle), DAngle::fromDeg(roll)), actor->InterpolatedScale(ticFrac), smf_flags, tic);
}

VSMatrix FSpriteModelFrame::ObjectToWorldMatrix(FLevelLocals *Level, DVector3 translation, DRotator rotation, DVector2 scaling, unsigned int flags, double tic)
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

	// Model space => World space
	objectToWorldMatrix.translate(translation.X, translation.Z, translation.Y);

	// consider the pixel stretching. For non-voxels this must be factored out here
	float stretch = 1.f;

	// [MK] distortions might happen depending on when the pixel stretch is compensated for
	// so we make the "undistorted" behavior opt-in
	if ((flags & MDL_CORRECTPIXELSTRETCH) && modelIDs.Size() > 0)
	{
		stretch = (modelIDs[0] >= 0 ? Models[modelIDs[0]]->getAspectFactor(Level->info->pixelstretch) : 1.f) / Level->info->pixelstretch;
		objectToWorldMatrix.scale(1, stretch, 1);
	}

	bool rotating_xzy = (flags & MDL_ROTATING) && (flags & MDL_FIXROTATING);
	bool rotating_xyz = (flags & MDL_ROTATING) && !(flags & MDL_FIXROTATING);

	// Applying model transformations:
	// 1) Applying actor angle, pitch and roll to the model
	if (flags & MDL_USEROTATIONCENTER)
	{
		objectToWorldMatrix.translate(rotationCenterX, rotationCenterZ/stretch, rotationCenterY);

		objectToWorldMatrix.rotate(-rotation.Yaw.Degrees(), 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Pitch.Degrees(), 0, 0, 1);
		objectToWorldMatrix.rotate(-rotation.Roll.Degrees(), 1, 0, 0);

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
		objectToWorldMatrix.rotate(-rotation.Yaw.Degrees(), 0, 1, 0);
		objectToWorldMatrix.rotate(rotation.Pitch.Degrees(), 0, 0, 1);
		objectToWorldMatrix.rotate(-rotation.Roll.Degrees(), 1, 0, 0);

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

	// 3) Scaling model.
	objectToWorldMatrix.scale(scaleFactorX, scaleFactorZ, scaleFactorY);

	// 4) Aplying model offsets (model offsets do not depend on model scalings).
	objectToWorldMatrix.translate(xoffset / xscale, zoffset / (zscale*stretch), yoffset / yscale);

	// 5) Applying model rotations.
	objectToWorldMatrix.rotate(-angleoffset, 0, 1, 0);
	objectToWorldMatrix.rotate(pitchoffset, 0, 0, 1);
	objectToWorldMatrix.rotate(-rolloffset, 1, 0, 0);

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
	if (vrmode->GetWeaponTransform(&objectToWorldMatrix, hand, !(smf_flags & MDL_NOAUTOREVERSE)))
	{
		float scale = 0.01f;
		objectToWorldMatrix.scale(scale, scale, scale);
		objectToWorldMatrix.translate(0, 5, 30);
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
	const bool useHandOfs = !!(smf_flags & MDL_USEHANDOFFSETS);
	// Which set of sliders this model listens to. The two hands are one mesh
	// mirrored by a negative X scale, so a shared value pushes them in opposite
	// directions and no single number can place both -- they each need their own.
	// hand was resolved above from the psprite, so it is already known here.
	const bool isOffhand = (hand == 1);
	const float handOfsX = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_x : vr_hand_ofs_x) : 0.0f;
	const float handOfsY = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_y : vr_hand_ofs_y) : 0.0f;
	const float handOfsZ = useHandOfs ? (float)(isOffhand ? vr_offhand_ofs_z : vr_hand_ofs_z) : 0.0f;

	objectToWorldMatrix.translate((smf->xoffset + handOfsX) / smf->xscale,
		(smf->zoffset + handOfsZ) / smf->zscale,
		(smf->yoffset + handOfsY) / smf->yscale);

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

	// Applying angleoffset, pitchoffset, rolloffset.
	//
	// The live vr_hand_* rotations are summed into these three calls rather than
	// applied as three more afterwards. That distinction matters: rotations do
	// not commute, so a yaw applied after this pitch and roll turns about an
	// already-rotated axis and is NOT the same as the same number added to
	// angleoffset. Summing here is what makes the slider and the MODELDEF
	// keyword interchangeable.
	const float handYaw   = useHandOfs ? (float)(isOffhand ? vr_offhand_yaw   : vr_hand_yaw)   : 0.0f;
	const float handPitch = useHandOfs ? (float)(isOffhand ? vr_offhand_pitch : vr_hand_pitch) : 0.0f;
	const float handRoll  = useHandOfs ? (float)(isOffhand ? vr_offhand_roll  : vr_hand_roll)  : 0.0f;

	objectToWorldMatrix.rotate(-(smf->angleoffset + handYaw), 0, 1, 0);
	objectToWorldMatrix.rotate(smf->pitchoffset + handPitch, 0, 0, 1);
	objectToWorldMatrix.rotate(-(smf->rolloffset + handRoll), 1, 0, 0);

	//Scale weapon
	objectToWorldMatrix.scale(vr_weaponScale, vr_weaponScale, vr_weaponScale);

	float orientation = smf->xscale * smf->yscale * smf->zscale;

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
	out.skinid.SetNull();
	out.surfaceskinids.Clear();

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
	// frames of one animation, so they advance together; per-index divergence
	// would need an array and no donor needs it yet.
	//
	// Out of range is not clamped on purpose: FMD3Model::RenderFrame rejects
	// (unsigned)frameno >= Frames.Size() and draws nothing, which is a visible
	// failure. Silently clamping to the last frame would hide the bug.
	if (psp && psp->ModelFrame >= 0)
	{
		out.modelframe     = psp->ModelFrame;
		out.modelframenext = (psp->ModelFrameNext >= 0) ? psp->ModelFrameNext : psp->ModelFrame;
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
			}
		}
	}

	return (out.modelid >= 0 && out.modelid < Models.SSize());
}


const TArray<VSMatrix> * ProcessModelFrame(FModel * animation, bool nextFrame, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, BoneInfo *out)
{
	const TArray<TRS>* animationData = nullptr;

	if (drawinfo.animationid >= 0)
	{
		animation = Models[drawinfo.animationid];
		animationData = animation->AttachAnimationData();
	}

	const TArray<VSMatrix> *boneData = nullptr;

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

static inline void RenderModelFrame(FModelRenderer *renderer, int i, const FSpriteModelFrame *smf, DActorModelData* modelData, const CalcModelFrameInfo &frameinfo, ModelDrawInfo &drawinfo, bool is_decoupled, double tic, FTranslationID translation, int &boneStartingPosition, bool &evaluatedSingle)
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

	CalcModelFrameInfo frameinfo = CalcModelFrame(Level, smf, curState, curTics, modelData, actor, is_decoupled, tic, ticFrac, psp);
	ModelDrawInfo drawinfo;

	int boneStartingPosition = -1;
	bool evaluatedSingle = false;

	for (unsigned i = 0; i < frameinfo.modelsamount; i++)
	{
		if (CalcModelOverrides(i, smf, modelData, frameinfo, drawinfo, is_decoupled, psp))
		{
			RenderModelFrame(renderer, i, smf, modelData, frameinfo, drawinfo, is_decoupled, tic, translation, boneStartingPosition, evaluatedSingle);
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
				// angleoffset, pitchoffset and rolloffset reading.
				else if (sc.Compare("angleoffset"))
				{
					sc.MustGetFloat();
					smf.angleoffset = sc.Float;
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
				// [BB] Added model flags reading.
				else if (sc.Compare("ignoretranslation"))
				{
					smf.flags |= MDL_IGNORETRANSLATION;
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
		spritedef_t *sprdef = &sprites[sprite];
		if (frame < sprdef->numframes)
		{
			spriteframe_t *sprframe = &SpriteFrames[sprdef->spriteframes + frame];
			if (sprframe->Voxel != nullptr)
			{
				int index = sprframe->Voxel->VoxeldefIndex;
				if (dropped && sprframe->Voxel->DroppedSpin != sprframe->Voxel->PlacedSpin) index++;
				return &SpriteModelFrames[index];
			}
		}
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
