/*
** p_pspr.h
**
** Sprite animation.
**
**---------------------------------------------------------------------------
**
** Copyright 1993-1996 id Software
** Copyright 1994-1996 Raven Software
** Copyright 1999-2016 Marisa Heit
** Copyright 2002-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#ifndef __P_PSPR_H__
#define __P_PSPR_H__

#include "renderstyle.h"
#include "palettecontainer.h"
#include "common/utility/TSQueue.h"

// Basic data types.
// Needs fixed point, and BAM angles.

#define WEAPONBOTTOM			128.

#define WEAPONTOP				32.
#define WEAPON_FUDGE_Y			0.375
struct FTranslatedLineTarget;
struct FState;
class player_t;

//
// Overlay psprites are scaled shapes
// drawn directly on the view screen,
// coordinates are given for a 320*200 view screen.
//
enum PSPLayers
{
	PSP_STRIFEHANDS = -1,
	PSP_CALLERID = 0,
	PSP_WEAPON = 1,
	PSP_FLASH = 1000,
	PSP_OFFHANDWEAPON = 1000000,
	PSP_TARGETCENTER = INT_MAX - 2,
	PSP_TARGETLEFT,
	PSP_TARGETRIGHT,
};

enum PSPFlags
{
	PSPF_ADDWEAPON		= 1 << 0,
	PSPF_ADDBOB			= 1 << 1,
	PSPF_POWDOUBLE		= 1 << 2,
	PSPF_CVARFAST		= 1 << 3,
	PSPF_ALPHA			= 1 << 4,
	PSPF_RENDERSTYLE	= 1 << 5,
	PSPF_FLIP			= 1 << 6,
	PSPF_FORCEALPHA		= 1 << 7,
	PSPF_FORCESTYLE		= 1 << 8,
	PSPF_MIRROR			= 1 << 9,
	PSPF_PLAYERTRANSLATED = 1 << 10,
	PSPF_PIVOTPERCENT	= 1 << 11,
	PSPF_INTERPOLATE	= 1 << 12,
};

enum PSPAlign
{
	PSPA_TOP = 0,
	PSPA_CENTER,
	PSPA_BOTTOM,
	PSPA_LEFT = PSPA_TOP,
	PSPA_RIGHT = 2
};

enum EPSPBobType
{
	PSPB_None,
	PSPB_2D,
	PSPB_3D,
};

struct WeaponInterp
{
	FVector2 v[4];
};

struct FPlayerBob
{
	struct FWeaponBobInfo {
		int Tic2D = -1;
		FVector2 Bob2D = {};

		int Tic3D = -1;
		FVector3 Translation = {};
		FVector3 Rotation = {};

		void Clear2D()
		{
			Tic2D = -1;
			Bob2D = {};
		}

		void Clear3D()
		{
			Tic3D = -1;
			Translation = Rotation = {};
		}

		void Clear()
		{
			Clear2D();
			Clear3D();
		}
	};

	FWeaponBobInfo BobInfo = {}, PrevBobInfo = {};

	void SetBob2D(int tic, FVector2 bob)
	{
		BobInfo.Tic2D = tic;
		BobInfo.Bob2D = bob;
		if (PrevBobInfo.Tic2D < 0 || BobInfo.Tic2D - PrevBobInfo.Tic2D != 1)
			PrevBobInfo = BobInfo;
	}

	void SetBob3D(int tic, const FVector3& trans, const FVector3& rot)
	{
		BobInfo.Tic3D = tic;
		BobInfo.Translation = trans;
		BobInfo.Rotation = rot;
		if (PrevBobInfo.Tic3D < 0 || BobInfo.Tic3D - PrevBobInfo.Tic3D != 1)
			PrevBobInfo = BobInfo;
	}

	void UpdateInterpolation(bool sprite)
	{
		if (sprite)
			BobInfo.Clear3D();
		else
			BobInfo.Clear2D();

		PrevBobInfo = BobInfo;
	}

	void ResetInterpolation()
	{
		BobInfo.Clear();
		PrevBobInfo.Clear();
	}

	FVector2 Interpolate2D(double ticFrac) const
	{
		return PrevBobInfo.Bob2D * (1.0 - ticFrac) + BobInfo.Bob2D * ticFrac;
	}

	void Interpolate3D(FVector3& t, FVector3& r, double ticFrac) const
	{
		t = PrevBobInfo.Translation * (1.0 - ticFrac) + BobInfo.Translation * ticFrac;
		r = PrevBobInfo.Rotation * (1.0 - ticFrac) + BobInfo.Rotation * ticFrac;
	}
};

class DPSprite : public DObject
{
	DECLARE_CLASS (DPSprite, DObject)
	HAS_OBJECT_POINTERS
public:
	DPSprite(player_t *owner, AActor *caller, int id);

	static void NewTick();
	void SetState(FState *newstate, bool pending = false);

	int			GetID()							const { return ID; }
	int			GetSprite()						const { return Sprite; }
	int			GetFrame()						const { return Frame; }
	int			GetTics()						const {	return Tics; }
	FTranslationID	GetTranslation()					  { return Translation; }
	FState*		GetState()						const { return State; }
	DPSprite*	GetNext()							  { return Next; }
	AActor*		GetCaller()							  { return Caller; }
	void		SetCaller(AActor *newcaller)		  { Caller = newcaller; }
	void		ResetInterpolation()				  { oldx = x; oldy = y; Prev = Vert; InterpolateTic = false; }
	void OnDestroy() override;
	std::pair<FRenderStyle, float> GetRenderStyle(FRenderStyle ownerstyle, double owneralpha);
	float GetYAdjust(bool fullscreen);

	int HAlign, VAlign;		// Horizontal and vertical alignment
	DVector2 baseScale;		// Base scale (set by weapon); defaults to (1.0, 1.2) since that's Doom's native aspect ratio
	DAngle rotation;		// How much rotation to apply.
	DVector2 pivot;			// pivot points
	DVector2 scale;			// Dynamic scale (set by A_Overlay functions)
	double x, y, alpha;
	double oldx, oldy;
	bool InterpolateTic;	// One tic interpolation (WOF_INTERPOLATE)
	DVector2 Coord[4];		// Offsets
	WeaponInterp Prev;		// Interpolation
	WeaponInterp Vert;		// Current Position
	bool firstTic;
	int Tics;
	FTranslationID Translation;
	RingBuffer<int, 5> LastPatches;
	FTextureID LastPatch;
	int Flags;
	FRenderStyle Renderstyle;

	// RS FORK -- PER-PSPRITE MODEL TINT. Added 2026-08-08.
	// Tint multiplies the weapon's own texture; Glow is added on top.
	// Both apply to a 3D HUD MODEL, not just a sprite: hw_weapon.cpp
	// sets them on the draw state immediately before RenderHUDModel,
	// and neither hw_models.cpp nor modelrenderer.h touches object or
	// add colour, so the model inherits them.
	//
	// These live on the PSprite rather than the weapon actor so each
	// hand tints independently -- mainhand and offhand are separate
	// PSprite layers (PSP_WEAPON / PSP_OFFHANDWEAPON).
	//
	// Stock GZDoom could not do this at all: the only colour input was
	// the PLAYER's fillcolor, gated behind STYLEF_ColorIsFixed, which
	// forces a stencil style and flattens the model to a silhouette.
	// IN-CLASS INITIALISERS, NOT CONSTRUCTOR-BODY ONES, AND THAT IS LOAD
	// BEARING. DPSprite has a second, PRIVATE default constructor used by
	// savegame deserialisation (`DPSprite () {}` below) which runs none of
	// the public constructor's body. Set only there, these stayed
	// uninitialised on every load, and the serialiser leaves them alone
	// when reading a save written before they existed -- so an old save
	// resumed with whatever garbage was in that memory. Garbage near zero
	// multiplies the weapon by black.
	PalEntry Tint = 0xffffffff;   // multiply, 0xffffffff = untinted
	PalEntry Glow = 0;            // additive, 0x00000000 = none

	// RS FORK -- DIRECT MODEL FRAME ADDRESSING.
	//
	// A HUD model's frame normally arrives through the SPRITE: psp->Frame is
	// a sprite letter index, MODELDEF's FrameIndex maps that letter to a model
	// frame, and FindModelFrame looks the pair up. That channel is one
	// character wide -- MAX_SPRITE_FRAMES is 29, inherited from Doom's
	// 8-character lump names where Boom pushed the frame char as far as ']'.
	//
	// Model meshes have no such limit. Our weapon models run to 75 frames, so
	// most of every reload and fire animation was simply unaddressable: there
	// was no letter left to name it with. Raising MAX_SPRITE_FRAMES does not
	// help -- three more ASCII characters and then lowercase, which lump names
	// case-fold away.
	//
	// So skip the encoding. When ModelFrame >= 0 the HUD model path uses it
	// verbatim instead of the sprite-derived frame, and 75 costs no more than
	// 5. FindModelFrame must still resolve (scale, offsets, skins and flags
	// all come from the FSpriteModelFrame it returns) -- only the frame NUMBER
	// is replaced.
	//
	// ModelFrameNext + ModelFrameLerp drive interpolation explicitly. Stock
	// tweening is derived from state tics and only happens across a state
	// transition, which is useless when one of OUR animations is being played
	// across THEIR state timings: it would snap between poses. Set the lerp
	// and the renderer blends ModelFrame -> ModelFrameNext by it, ignoring
	// gl_interpolate_model_frames and MDL_NOINTERPOLATION.
	//
	// IN-CLASS INITIALISERS FOR THE SAME REASON AS Tint/Glow ABOVE: the
	// private savegame constructor runs no constructor body, and a garbage
	// ModelFrame indexes a model's frame array with a random int.
	//
	// -1 on all three = inactive, stock behaviour.
	int   ModelFrame     = -1;   // model frame to show, bypassing the sprite
	int   ModelFrameNext = -1;   // frame to blend toward
	float ModelFrameLerp = -1.f; // 0..1 blend factor; <0 = use stock timing

private:
	DPSprite () {}

	void Serialize(FSerializer &arc);

public:	// must be public to be able to generate the field export tables. Grrr...
	TObjPtr<AActor*> Caller;
	TObjPtr<DPSprite*> Next;
	player_t *Owner;
	FState *State;
	int Sprite;
	int Frame;
	int ID;
	bool processPending; // true: waiting for periodic processing on this tick

	friend class player_t;
	friend void CopyPlayer(player_t *dst, player_t *src, const char *name);
};

void P_NewPspriteTick();
void P_CalcSwing (player_t *player);
void P_SetPsprite(player_t *player, PSPLayers id, FState *state, bool pending = false, AActor *newcaller = nullptr);
void P_BringUpWeapon (player_t *player);
void P_FireWeapon (player_t *player);
void P_BobWeapon(player_t* player);
void P_BobWeapon3D(player_t* player);
DAngle P_BulletSlope (AActor *mo, FTranslatedLineTarget *pLineTarget = NULL, int aimflags = 0);
AActor *P_AimTarget(AActor *mo);

void DoReadyWeaponToBob(AActor *self, int hand = 0);
void DoReadyWeaponToFire(AActor *self, bool primary = true, bool secondary = true, int hand = 0);
void DoReadyWeaponToSwitch(AActor *self, bool switchable = true, int hand = 0);

void A_ReFire(AActor *self, FState *state = NULL);

extern EPSPBobType BobType;
extern FPlayerBob PlayerBob[MAXPLAYERS];

#endif	// __P_PSPR_H__
