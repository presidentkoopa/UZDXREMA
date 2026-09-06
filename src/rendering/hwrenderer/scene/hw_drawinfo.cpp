/*
** hw_drawinfo.cpp
**
** Basic scene draw info management class
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2018 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <algorithm>
#include "a_sharedglobal.h"
#include "r_utility.h"
#include "r_sky.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "hw_fakeflat.h"
#include "hw_portal.h"
#include "hw_renderstate.h"
#include "hwrenderer/postprocessing/hw_postprocess.h"
#include "hw_drawinfo.h"
#include "po_man.h"
#include "models.h"
#include "hw_clock.h"
#include "hw_cvars.h"
#include "hw_viewpointbuffer.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_bonebuffer.h"
#include "hw_vrmodes.h"
#include "hw_vrwheel.h"
#include "hw_clipper.h"
#include "v_draw.h"
#include "a_corona.h"
#include "texturemanager.h"
#include "actorinlines.h"
#include "g_levellocals.h"

void DrawLaserSightWorld(FRenderState& state);
void DrawHitscanTracers(FRenderState& state);

EXTERN_CVAR(Float, r_visibility)
EXTERN_CVAR(Int, gl_max_portals);
CVAR(Bool, gl_bandedswlight, false, CVAR_ARCHIVE)
CVAR(Bool, gl_sort_textures, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, gl_no_skyclear, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_enhanced_nv_stealth, 3, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_texture, true, 0)
CVAR(Float, gl_mask_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_mask_sprite_threshold, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, gl_coronas, true, CVAR_ARCHIVE);

// [BB] How many samples the volumetric beam takes along each pixel's ray.
// The single knob that trades beam quality for framerate: fewer steps means
// coarser haze, not a dimmer beam, because the march normalises by count.
CVAR(Int, vol_beam_quality, 24, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// [BB] How much the volumetric cone fades as your VIEW lines up with it.
//
// A cone seen end-on is a disc, and on a flat screen the default mount points
// exactly where you look -- so without this the beam is a permanent soft halo
// over the middle of the frame, fed straight into bloom, carrying no
// information at all because it marks the place you are already looking.
//
// Not a rule, because in VR a tracked hand pointed forward is a real thing
// somebody might want to see. 0 restores the unfaded behaviour.
CVAR(Float, vol_beam_axisfade, 0.85f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

// [BB] Smooth segment beams between tics instead of stepping at 35Hz.
//
// Beams are written from script, so they only change 35 times a second while
// the view redraws 90-120. Off, a beam is exactly where script last put it and
// visibly stutters against the world; on, it is drawn where it was passing
// through at this instant. An escape hatch, not a taste setting -- leave it on
// unless a beam is doing something strange, in which case turning it off says
// whether the interpolation or the script is at fault.
CVAR(Bool, r_beam_interpolate, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);

sector_t * hw_FakeFlat(sector_t * sec, sector_t * dest, area_t in_area, bool back);

std::pair<PalEntry, PalEntry>& R_GetSkyCapColor(FGameTexture* tex);

extern int portalsPerEye;

//==========================================================================
//
//
//
//==========================================================================

class FDrawInfoList
{
public:
	TDeletingArray<HWDrawInfo *> mList;

	HWDrawInfo * GetNew();
	void Release(HWDrawInfo *);
};


FDrawInfoList di_list;

//==========================================================================
//
// Try to reuse the lists as often as possible as they contain resources that
// are expensive to create and delete.
//
// Note: If multithreading gets used, this class needs synchronization.
//
//==========================================================================

HWDrawInfo *FDrawInfoList::GetNew()
{
	if (mList.Size() > 0)
	{
		HWDrawInfo *di;
		mList.Pop(di);
		return di;
	}
	return new HWDrawInfo();
}

void FDrawInfoList::Release(HWDrawInfo * di)
{
	di->ClearBuffers();
	di->Level = nullptr;
	mList.Push(di);
}

//==========================================================================
//
// Sets up a new drawinfo struct
//
//==========================================================================

HWDrawInfo *HWDrawInfo::StartDrawInfo(FLevelLocals *lev, HWDrawInfo *parent, FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	HWDrawInfo *di = di_list.GetNew();
	di->Level = lev;
	di->StartScene(parentvp, uniforms);
	return di;
}


//==========================================================================
//
//
//
//==========================================================================

static Clipper staticClipper;		// Since all scenes are processed sequentially we only need one clipper.
static Clipper staticVClipper;		// Another clipper to clip vertically (used if (VPSF_ALLOWOUTOFBOUNDS & camera->viewpos->Flags)).
static Clipper staticRClipper;		// Another clipper for radar (doesn't actually clip. Changes SSECMF_DRAWN setting).
static HWDrawInfo * gl_drawinfo;	// This is a linked list of all active DrawInfos and needed to free the memory arena after the last one goes out of scope.

void HWDrawInfo::StartScene(FRenderViewpoint &parentvp, HWViewpointUniforms *uniforms)
{
	staticClipper.Clear();
	staticVClipper.Clear();
	staticRClipper.Clear();
	mClipper = &staticClipper;
	vClipper = &staticVClipper;
	rClipper = &staticRClipper;
	rClipper->amRadar = true;

	Viewpoint = parentvp;
	auto vrmode = VRMode::GetVRModeCached(true);
	IsVRScene = vrmode != nullptr && vrmode->IsVR();
	if (Level != nullptr)
		lightmode = getRealLightmode(Level, true);
	if (uniforms)
	{
		VPUniforms = *uniforms;
		// The clip planes will never be inherited from the parent drawinfo.
		VPUniforms.mClipLine.X = -1000001.f;
		VPUniforms.mClipHeight = 0;
		VPUniforms.mClipHeightDirection = 0.f;
	}
	else
	{
		VPUniforms.mProjectionMatrix.loadIdentity();
		VPUniforms.mViewMatrix.loadIdentity();
		VPUniforms.mNormalViewMatrix.loadIdentity();
		ProjectionMatrix2.loadIdentity();
		VPUniforms.mViewHeight = viewheight;
		int fogmode = Viewpoint.bDoOrtho && (lightmode == ELightMode::ZDoomSoftware) ? 2 : gl_fogmode; // Force radial if Ortho and ZDoomSoftware
		if (lightmode == ELightMode::Build)
		{
			VPUniforms.mGlobVis = 1 / 64.f;
			VPUniforms.mPalLightLevels = 32 | (static_cast<int>(fogmode) << 8) | ((int)lightmode << 16);
		}
		else
		{
			VPUniforms.mGlobVis = (float)R_GetGlobVis(r_viewwindow, r_visibility) / 32.f;
			VPUniforms.mPalLightLevels = static_cast<int>(gl_bandedswlight) | (static_cast<int>(fogmode) << 8) | ((int)lightmode << 16);
		}
		VPUniforms.mClipLine.X = -10000000.0f;
		VPUniforms.mShadowmapFilter = gl_shadowmap_filter;
		VPUniforms.mLightBlendMode = (level.info ? (int)level.info->lightblendmode : 0);
		VPUniforms.mThickFogDistance = Level->thickfogdistance;
		VPUniforms.mThickFogMultiplier = Level->thickfogmultiplier;
	}

	// [BB] Glow wave, scene-global. Copied here rather than per draw because
	// every draw in the frame reads the same wave -- only the phase differs,
	// and that is per CHANNEL, not per surface.
	//
	// Doom's Z is the shader's Y, the same swizzle the sweep origin uses just
	// below in RenderScene. Getting it wrong is not a crash, it is a ring
	// wave that expands through the floor instead of across it.
	if (Level != nullptr)
	{
		VPUniforms.mGlowWave = {
			(float)Level->GlowWaveLength, (float)Level->GlowWaveSpeed,
			(float)Level->GlowWaveSharp,  (float)Level->GlowWaveShape };
		VPUniforms.mGlowWaveDepth = {
			(float)Level->GlowWaveReach,  (float)Level->GlowWaveBright,
			(float)Level->GlowWaveColour, (float)Level->GlowWaveDetune };
		VPUniforms.mGlowWavePhase = {
			(float)Level->GlowWavePhase[0], (float)Level->GlowWavePhase[1],
			(float)Level->GlowWavePhase[2], (float)Level->GlowWavePhase[3] };
		VPUniforms.mGlowWaveOrigin = {
			(float)Level->GlowWaveOrigin.X, (float)Level->GlowWaveOrigin.Z,
			(float)Level->GlowWaveOrigin.Y, (float)Level->GlowWaveSeed };

		// [BB] Beams. Swizzled like everything else here: Doom's Z is the
		// shader's Y. A beam laid along a corridor with the axes crossed
		// becomes a beam standing in a wall, which is a memorable bug.
		{
			int nb = clamp(Level->BeamCount, 0, FLevelLocals::MAX_BEAMS);

			// Script sets beams at 35Hz; this loop runs every frame. Without the
			// lerp a beam holds still for a whole tic and then jumps, which at
			// 90-120Hz reads as a beam that stutters against smoothly moving
			// geometry. cl_capfps and r_NoInterpolate already pin TicFrac to 1.0
			// upstream (hw_entrypoint.cpp), so those cases collapse to the old
			// behaviour on their own and need no guard here.
			const double ticFrac = r_beam_interpolate ? Viewpoint.TicFrac : 1.0;

			for (int i = 0; i < nb; i++)
			{
				// INTERPOLATE ONLY A BEAM THAT WAS ALREADY LIT AND STILL IS.
				// Callers park a released slot at (0,0,0) rather than leaving
				// stale endpoints behind, so both transitions have to snap: on
				// the tic a beam lights up prev is the map origin and lerping
				// would drag it across the level, and on the tic it goes out the
				// same thing happens in reverse. Between those it is the same
				// beam moving, which is exactly what wants smoothing.
				const bool lerpable =
					i < Level->PrevBeamCount &&
					Level->PrevBeamIntensity[i] > 0.0 &&
					Level->BeamIntensity[i] > 0.0;
				const double f = lerpable ? ticFrac : 1.0;

				const DVector3 a = Level->PrevBeamStart[i] +
					(Level->BeamStart[i] - Level->PrevBeamStart[i]) * f;
				const DVector3 b = Level->PrevBeamEnd[i] +
					(Level->BeamEnd[i] - Level->PrevBeamEnd[i]) * f;

				VPUniforms.mBeamA[i] = {
					(float)a.X, (float)a.Z,
					(float)a.Y, (float)Level->BeamThick[i] };
				VPUniforms.mBeamB[i] = {
					(float)b.X, (float)b.Z,
					(float)b.Y, (float)Level->BeamSoft[i] };
				// Colour is NOT interpolated on purpose. A band change is a
				// deliberate step -- see the tier bands in RS_Lance -- and
				// crossfading it would turn a power-up into a smear.
				VPUniforms.mBeamCol[i] = {
					Level->BeamColor[i].r / 255.f, Level->BeamColor[i].g / 255.f,
					Level->BeamColor[i].b / 255.f, (float)Level->BeamIntensity[i] };
			}
			VPUniforms.mBeamParams = { (float)nb, (float)Level->BeamGlow,
				(float)Level->BeamFogScatter, (float)Level->BeamAirGlow };
			VPUniforms.mBeamFX = { (float)Level->BeamScrollSpeed,
				(float)Level->BeamScrollDepth, (float)Level->BeamTaper,
				(float)Level->BeamFlare };
		}

		// [STAMP] Surface stamps. Progress carries the tic fraction so a stamp
		// blooms smoothly instead of stepping 35 times a second -- the same
		// reason the beams above interpolate. Nothing else here needs
		// smoothing: position, colour and shape do not move once published.
		{
			// Viewpoint.TicFrac directly: the beams' own ticFrac above is
			// scoped to their block, and a stamp has no reason to follow
			// r_beam_interpolate.
			const double stampFrac = Viewpoint.TicFrac;
			int ns = 0;
			for (int st = 0; st < FLevelLocals::MAX_SURFACE_STAMPS; st++)
			{
				if (Level->StampLife[st] <= 0 || Level->StampRadius[st] <= 0.0)
					continue;

				const double prog = clamp((Level->StampAge[st] + stampFrac)
					/ (double)Level->StampLife[st], 0.0, 1.0);
				const DVector3 &p = Level->StampPos[st];
				const DVector3 &a = Level->StampAxis[st];
				const PalEntry  c = Level->StampColor[st];

				// Game (x, y, z) -> shader (x, z, y): y is up in shader space.
				VPUniforms.mStampPos[ns] = {
					(float)p.X, (float)p.Z, (float)p.Y,
					(float)Level->StampRadius[st] };
				VPUniforms.mStampCol[ns] = {
					c.r / 255.f, c.g / 255.f, c.b / 255.f, (float)prog };
				VPUniforms.mStampArg[ns] = {
					(float)Level->StampShape[st],
					(float)a.X, (float)a.Z, (float)a.Y };
				VPUniforms.mStampMod[ns] = {
					(float)Level->StampTex[st],
					(float)Level->StampTexStrength[st], 0.f, 0.f };
				const PalEntry c2 = Level->StampColor2[st];
				VPUniforms.mStampCol2[ns] = {
					c2.r / 255.f, c2.g / 255.f, c2.b / 255.f,
					(float)Level->StampFadeAt[st] };
				ns++;
			}
			VPUniforms.mStampParams = { (float)ns, 0.f, 0.f, 0.f };
		}

		// [BB] Sweep fill -- the pattern inside a band. Frame-global style;
		// only the mode is per band, packed into the draw mode.
		VPUniforms.mSweepFill = {
			(float)Level->SweepFillSpacingU, (float)Level->SweepFillSpacingV,
			(float)Level->SweepFillWidth,    (float)Level->SweepFillSoft };
		VPUniforms.mSweepFill2 = {
			(float)Level->SweepFillRotate,   (float)Level->SweepFillDrift,
			(float)Level->SweepFillMajor,    (float)Level->SweepFillJitter };
		VPUniforms.mSweepFill3 = {
			(float)Level->SweepFillGrad,     (float)Level->SweepFillGradAxis,
			(float)Level->SweepFillFlicker,  (float)Level->SweepFillMajorBoost };
		VPUniforms.mSweepFillCol = {
			Level->SweepFillColor.r / 255.f, Level->SweepFillColor.g / 255.f,
			Level->SweepFillColor.b / 255.f, (float)Level->SweepFillGap };
		VPUniforms.mSweepAir = { (float)Level->SweepFillAir, 0.f, 0.f, 0.f };

		// [BB] Darkness. Frame-global for the same reason: the curve and its
		// gains are the same everywhere, and only the FRAGMENT it is asked
		// about differs.
		VPUniforms.mDarkness = {
			(float)Level->DarkMode,     (float)Level->DarkAdjust,
			(float)Level->DarkMinLight, (float)Level->DarkPreGain };
		VPUniforms.mDarkness2 = {
			(float)Level->DarkPostGain, (float)Level->DarkDistDepth,
			(float)Level->DarkDistRange, 0.0f };
		VPUniforms.mDarkness3 = {
			(float)Level->DarkHeightDepth, (float)Level->DarkHeightRef,
			(float)Level->DarkHeightRange, 0.0f };

		// [BB] Fog slab. Doom's Z is the shader's Y, the same swizzle the
		// sweep origin and the wave origin use -- get it wrong and the mist
		// hangs against a wall instead of lying on the floor.
		//
		// ONLY THE SLAB IS BEHIND THIS GATE. Everything else made of mist --
		// the tornado, the tendrils, the disturbances, the torch that lights
		// them -- is pushed below it, unconditionally.
		//
		// They used to be inside, and that quietly cancelled the work that
		// made them independent in the first place: the shader was perfectly
		// willing to draw a funnel in clear air, and the uniform carrying its
		// density was never written unless floor fog happened to be on. A
		// feature can be switched off by code that does not mention it.
		if (Level->FogSlabActive && Level->FogSlabDensity > 0.0)
		{
			VPUniforms.mFogSlab = {
				(float)Level->FogSlabTop, (float)Level->FogSlabDensity,
				(float)Level->FogSlabSoft, (float)Level->FogSlabScatter };
			VPUniforms.mFogSlabColor = {
				Level->FogSlabColor.r / 255.f, Level->FogSlabColor.g / 255.f,
				Level->FogSlabColor.b / 255.f, (float)Level->FogSlabWakeStrength };
			VPUniforms.mFogSlabWake = {
				(float)Level->FogSlabWakePos.X, (float)Level->FogSlabWakePos.Z,
				(float)Level->FogSlabWakePos.Y, (float)Level->FogSlabWakeRadius };
			VPUniforms.mFogSlabExtra = {
				(float)Level->FogSlabWakeStrength, (float)Level->FogSlabPickup,
				0.0f, 0.0f };
			VPUniforms.mFogSlab2 = { (float)Level->FogSlabBottom,
				(float)Level->FogSlabPeriod, (float)Level->FogSlabRoll, 0.f };
			VPUniforms.mFogSurf = {
				(float)Level->FogSurfAmp, (float)Level->FogSurfLen,
				(float)Level->FogSurfSpeed, (float)Level->FogSurfCross };
		}
		else
		{
			VPUniforms.mFogSlab = { 0.f, 0.f, 24.f, 0.f };
			VPUniforms.mFogSurf = { 0.f, 256.f, 1.f, 0.f };
		}

		VPUniforms.mTornado = { (float)Level->TornadoPos.X,
			(float)Level->TornadoPos.Y, (float)Level->TornadoBase,
			(float)Level->TornadoTop };
		VPUniforms.mTornado2 = { (float)Level->TornadoRadBase,
			(float)Level->TornadoRadTop, (float)Level->TornadoDensity,
			(float)Level->TornadoSwirl };
		VPUniforms.mTornado3 = { (float)Level->TornadoSpin,
			(float)Level->TornadoTwist, (float)Level->TornadoLean,
			(float)Level->TornadoLeanPeriod };
		VPUniforms.mTornadoCol = {
			Level->TornadoColor.r / 255.f, Level->TornadoColor.g / 255.f,
			Level->TornadoColor.b / 255.f, (float)Level->TornadoScatter };

		// [BB] Disturbances. Age is resolved HERE rather than in script, so a
		// ripple expands at render rate instead of in 35Hz steps -- a ring
		// crawling outward one tic at a time is a visible staircase.
		// Counted so the shader can know, from ONE compare, whether any
		// disturbance is live. Ignite adds light rather than mist and has to
		// work in a room with the fog switched off -- without this the whole
		// function early-outs before the loop that would draw it.
		int liveDisturb = 0;
		{
			// [BB] TicFrac, which this had always claimed to do and did not.
			//
			// The comment above has said "resolved HERE rather than in script,
			// so a ring expands at render rate" since this was written, but the
			// clock was plain maptime -- so a disturbance did crawl outward one
			// tic at a time, and a small fast ripple showed exactly the
			// staircase the comment warns about. Same treatment the beam block
			// above already gets from Viewpoint.TicFrac.
			double now = (Level->maptime + Viewpoint.TicFrac) / (double)TICRATE;

			for (int i = 0; i < FLevelLocals::MAX_FOG_DISTURB; i++)
			{
				double life = Level->FogDisturbLife[i];
				double age = now - Level->FogDisturbBirth[i];
				if (life <= 0.0 || age < 0.0 || age > life)
				{
					VPUniforms.mFogDisturbA[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mFogDisturbB[i] = { 0.f, 0.f, 0.f, 0.f };
					continue;
				}
				// Strength decays over the slot's life, so nothing has to be
				// freed on a schedule: an expired slot is one whose strength
				// has already reached zero. Squared, because a linear fade on
				// an expanding ring reads as a hard stop at the end.
				float fade = (float)(1.0 - age / life);
				VPUniforms.mFogDisturbA[i] = {
					(float)Level->FogDisturbPos[i].X,
					(float)Level->FogDisturbPos[i].Z,
					(float)Level->FogDisturbPos[i].Y,
					(float)Level->FogDisturbRadius[i] };
				VPUniforms.mFogDisturbB[i] = { (float)age,
					(float)Level->FogDisturbStrength[i] * fade * fade,
					(float)Level->FogDisturbSpeed[i],
					(float)Level->FogDisturbMode[i] };

				// HIGH-WATER MARK, NOT A COUNT, and the distinction is load
				// bearing now that the shader breaks on this value. Slots are
				// recycled out of order -- FogDisturb() takes the first free
				// or the oldest -- so a live set can be sparse. With slots 0
				// and 5 live, a count of 2 would stop the shader loop at 2 and
				// slot 5 would silently stop being drawn.
				//
				// Same shape as the shape loop's own `live = i + 1` a few
				// hundred lines below, for the same reason.
				liveDisturb = i + 1;
			}
		}

		VPUniforms.mFogNoise = { (float)Level->FogNoiseScale,
			(float)Level->FogNoiseDepth, (float)Level->FogNoiseDrift.X,
			(float)Level->FogNoiseDrift.Y };

		VPUniforms.mFogTendril = { (float)Level->FogTendrilSpacing,
			(float)Level->FogTendrilRadius, (float)Level->FogTendrilHeight,
			(float)Level->FogTendrilDensity };
		VPUniforms.mFogTendril2 = { (float)Level->FogTendrilRise,
			(float)Level->FogTendrilSpread, (float)Level->FogTendrilLean,
			(float)Level->FogTendrilTaper };

		VPUniforms.mFogWake2 = { (float)Level->FogWakeVel.X,
			(float)Level->FogWakeVel.Y, (float)Level->FogWakeStretch, 0.f };

		VPUniforms.mFogBow = { (float)Level->FogBowStrength,
			(float)Level->FogBowWidth, (float)Level->FogBowThin,
			(float)liveDisturb };

		// [BB] What each fog edge follows, and the floor and ceiling AT THE EYE.
		//
		// The eye pair is resolved here rather than in the shader because the
		// plane uniforms describe the FRAGMENT's sector. Using those for the
		// eye end of the ray would raise the fog around your head the moment
		// you looked at a wall on the floor above.
		{
			double eyeFloor = 0.0, eyeCeil = 0.0;
			// [BB] Only ask for a sector when there is geometry to find one in.
			//
			// PointInSubsector answers a null gamenode with &subsectors[0], and
			// PointInSector reads ->sector straight off that. On a level whose
			// map has been freed the array is empty, so that is a read through a
			// null data pointer at offset 4 rather than a missing fog value.
			//
			// This path is reached exactly when something has ALREADY gone
			// wrong: I_Error frees the map and then the error screen still has
			// to draw, which comes back through here via AdjustBlend with no
			// map loaded. Without this the engine dies with an access violation
			// instead of showing the error that actually caused the problem.
			auto vsec = Level->subsectors.Size() > 0
				? Level->PointInSector(Viewpoint.Pos)
				: nullptr;
			if (vsec)
			{
				eyeFloor = vsec->floorplane.ZatPoint(Viewpoint.Pos);
				eyeCeil = vsec->ceilingplane.ZatPoint(Viewpoint.Pos);
			}
			VPUniforms.mFogFollow = { (float)Level->FogFollowTop,
				(float)Level->FogFollowBottom, (float)eyeFloor, (float)eyeCeil };
		}

		// [BB] Shapes. Size, growth and the seam all resolve HERE rather than
		// in script, so a mark that opens does it at render rate instead of in
		// 35Hz steps -- a seam crawling apart one tic at a time is a visible
		// staircase, and it is the one part of the effect anyone looks at.
		//
		// [BB] TicFrac added. As with the disturbances above, this claimed
		// render rate and was reading plain maptime, so growth and the seam
		// stepped at 35Hz regardless.
		{
			double now = (Level->maptime + Viewpoint.TicFrac) / (double)TICRATE;

			// THE HIGH-WATER MARK, and it is why a 128-slot array is
			// affordable. The shader loops to this rather than to the cap, so
			// the array's size costs nothing until it is actually used.
			//
			// Recomputed here rather than tracked on add and remove: a slot
			// can also fall vacant by simply ageing out, which no caller
			// observes, and a counter that only some of the ways of becoming
			// empty know about is a counter that drifts.
			int live = 0;

			// [BB] Resolved once per slot, IN SLOT ORDER, so a linked child
			// can read its parent's ALREADY-RESOLVED world transform within
			// this same forward pass -- see the contract on ShapeParent in
			// g_levellocals.h (parent index must be smaller than the
			// child's, there is no cycle check and no topological sort).
			// Zero-initialized so a parent reference to a dead or
			// not-yet-valid slot reads a defined zero rather than garbage.
			DVector3 resolvedPos[FLevelLocals::MAX_SHAPES] = {};
			double resolvedYaw[FLevelLocals::MAX_SHAPES] = {};
			double resolvedPitch[FLevelLocals::MAX_SHAPES] = {};
			double resolvedRoll[FLevelLocals::MAX_SHAPES] = {};

			for (int i = 0; i < FLevelLocals::MAX_SHAPES; i++)
			{
				double base = Level->ShapeSize[i];
				double life = Level->ShapeLife[i];
				double age = now - Level->ShapeBirth[i];

				if (base <= 0.0 || Level->ShapeKind[i] <= 0 ||
					(life > 0.0 && (age < 0.0 || age > life)))
				{
					VPUniforms.mShapeA[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeB[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeCol[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeD[i] = { 0.f, 0.f, 0.f, 0.f };
					VPUniforms.mShapeE[i] = { 0.f, 0.f, 0.f, 0.f };
					continue;
				}

				live = i + 1;

				// [BB] YAW/PITCH/ROLL, resolved from base + rate * age --
				// the identical shape grow/seamRate already use, just three
				// of them. Orient 0-2 (decals) never set a rate, so this is
				// a no-op arithmetic pass for every shape that isn't
				// standing -- yaw comes out exactly as authored.
				double yaw = Level->ShapeAngle[i] + Level->ShapeYawRate[i] * age;
				double pitch = Level->ShapePitch[i] + Level->ShapePitchRate[i] * age;
				double roll = Level->ShapeRoll[i] + Level->ShapeRollRate[i] * age;
				DVector3 pos = Level->ShapePos[i];

				// [BB] LINKING. A valid parent (an earlier, already-resolved
				// slot) replaces this shape's own authored position and
				// orientation with one composed onto the parent's.
				// Anything else -- no parent, or a parent index that is not
				// actually earlier -- leaves pos/yaw/pitch/roll exactly as
				// authored, which is also correct behaviour for an
				// unparented shape.
				int parent = Level->ShapeParent[i];
				if (parent >= 0 && parent < i)
				{
					double pyaw = resolvedYaw[parent];
					double ppitch = resolvedPitch[parent];
					double proll = resolvedRoll[parent];

					double pyawR = pyaw * M_PI / 180.0;
					double ppitchR = ppitch * M_PI / 180.0;
					double prollR = proll * M_PI / 180.0;

					// The parent's own facing/right/up, built the identical
					// way StandingShapesAt() builds it in main.fp -- Doom
					// space (Z up) here instead of shader space (Y up).
					// Guarded for the parent facing straight up or down,
					// where "right" would otherwise divide by a
					// near-zero-length cross product: fall back to world
					// +X as the reference instead of world-up.
					DVector3 fwd(cos(ppitchR) * cos(pyawR),
						cos(ppitchR) * sin(pyawR), sin(ppitchR));
					DVector3 worldUp(0.0, 0.0, 1.0);
					DVector3 right0 = (fabs(fwd.Z) > 0.999)
						? DVector3(1.0, 0.0, 0.0) ^ fwd
						: worldUp ^ fwd;
					if (right0.Length() > 0.0001) right0 = right0.Unit();
					DVector3 up0 = fwd ^ right0;

					double cr = cos(prollR), sr = sin(prollR);
					DVector3 right = right0 * cr + up0 * sr;
					DVector3 up = up0 * cr - right0 * sr;

					DVector3 local = Level->ShapeLocalPos[i];
					pos = resolvedPos[parent]
						+ fwd * local.X + right * local.Y + up * local.Z;

					// Euler addition onto the parent's resolved orientation
					// -- see the long comment on ShapeParent for why this
					// is exact for a pure-yaw chain and an approximation
					// once pitch and roll combine at the same joint.
					yaw = pyaw + Level->ShapeLocalYaw[i];
					pitch = ppitch + Level->ShapeLocalPitch[i];
					roll = proll + Level->ShapeLocalRoll[i];
				}

				resolvedPos[i] = pos;
				resolvedYaw[i] = yaw;
				resolvedPitch[i] = pitch;
				resolvedRoll[i] = roll;

				float fade = (life > 0.0) ? (float)(1.0 - age / life) : 1.0f;
				float size = (float)(base + Level->ShapeGrow[i] * age);
				float seam = (float)clamp(Level->ShapeSeam[i]
					+ Level->ShapeSeamRate[i] * age, 0.0, 1.0);

				VPUniforms.mShapeA[i] = { (float)pos.X, (float)pos.Z, (float)pos.Y,
					size };
				VPUniforms.mShapeB[i] = {
					(float)(Level->ShapeKind[i] + 16 * Level->ShapeOrient[i]),
					(float)yaw, (float)Level->ShapeThick[i],
					seam };
				VPUniforms.mShapeCol[i] = {
					Level->ShapeColor[i].r / 255.f, Level->ShapeColor[i].g / 255.f,
					Level->ShapeColor[i].b / 255.f,
					(float)Level->ShapeIntensity[i] * fade };
				VPUniforms.mShapeD[i] = { (float)Level->ShapeRepeat[i],
					(float)Level->ShapeRepCount[i], (float)Level->ShapeRepSpace[i],
					(float)Level->ShapeRepSpin[i] };
				// [BB] Resolved pitch/roll for StandingShapesAt() in main.fp.
				// z/w spare.
				VPUniforms.mShapeE[i] = { (float)pitch, (float)roll, 0.f, 0.f };
			}

			VPUniforms.mShapeParams = { (float)Level->ShapeSoft,
				(float)Level->ShapeHeightFade, (float)Level->ShapeReach,
				(float)live };
		}
		VPUniforms.mShapeUnder = { Level->ShapeUnder.r / 255.f,
			Level->ShapeUnder.g / 255.f, Level->ShapeUnder.b / 255.f, 0.f };

		// [BB] The sweep's room box, in SHADER space -- Y and Z swapped, the
		// same reordering every world position in this block gets. Doing it
		// here rather than in the shader keeps the swap in one place instead
		// of at every read.
		//
		// The soft distance rides on Min.w and the bound flag on Max.w, so a
		// level that never publishes a room leaves both zero and the shader's
		// test costs one compare.
		VPUniforms.mSweepRoomMin = { (float)Level->SweepRoomMin.X,
			(float)Level->SweepRoomMin.Z, (float)Level->SweepRoomMin.Y,
			(float)Level->SweepRoomSoft };
		VPUniforms.mSweepRoomMax = { (float)Level->SweepRoomMax.X,
			(float)Level->SweepRoomMax.Z, (float)Level->SweepRoomMax.Y,
			Level->SweepRoomSoft > 0 ? 1.f : 0.f };

		// w was spare and is now the global drain. See FLevelLocals::DesatGlobal.
		VPUniforms.mDesatKeep = { (float)Level->DesatKeep,
			(float)Level->DesatKeepSoft, (float)Level->DesatKeepHue,
			(float)Level->DesatGlobal };
		VPUniforms.mGlowTex = { (float)Level->GlowTexNoise,
			(float)Level->GlowTexScale, (float)Level->GlowTexDrift,
			(float)Level->GlowTexContrast };
		VPUniforms.mGlowTex2 = { (float)Level->GlowFlow,
			(float)Level->GlowFlowSpacing, (float)Level->GlowFlowSpeed,
			(float)Level->GlowFlowSharp };
		VPUniforms.mGlowTex3 = { (float)Level->GlowCell,
			(float)Level->GlowCellScale, (float)Level->GlowCellSpeed,
			(float)Level->GlowCellWidth };
		VPUniforms.mGlowTex4 = { (float)Level->GlowReact,
			(float)Level->GlowPulse, (float)Level->GlowPulseLevel,
			(float)Level->GlowPulseRate };

		VPUniforms.mFogColor2 = { Level->FogColor2.r / 255.f,
			Level->FogColor2.g / 255.f, Level->FogColor2.b / 255.f,
			(float)Level->FogColor2Mix };

		// The torch cone in WORLD space, so mist can be lit by it. The
		// volumetric beam pass gets its own copy in VIEW space and cannot
		// share -- see hw_viewpointuniforms.h. Outside the slab gate too:
		// a tornado standing in clear air is exactly the case that needs it.
		// The fog carries ONE torch cone -- there is a single set of mFogBeam
		// uniforms -- so with several beams live it takes the lowest slot
		// rather than whichever was written most recently. Deterministic, and
		// slot 0 is the one a flashlight would naturally hold.
		const int fogBeam = Level->FirstVolBeam();
		if (fogBeam >= 0)
		{
			VPUniforms.mFogBeamPos = {
				(float)Level->VolBeamPos[fogBeam].X, (float)Level->VolBeamPos[fogBeam].Z,
				(float)Level->VolBeamPos[fogBeam].Y, (float)Level->VolBeamLength[fogBeam] };
			VPUniforms.mFogBeamDir = {
				(float)Level->VolBeamDir[fogBeam].X, (float)Level->VolBeamDir[fogBeam].Z,
				(float)Level->VolBeamDir[fogBeam].Y,
				(float)cos(Level->VolBeamInner[fogBeam] * M_PI / 180.0) };
			VPUniforms.mFogBeamCol = {
				Level->VolBeamColor[fogBeam].r / 255.f, Level->VolBeamColor[fogBeam].g / 255.f,
				Level->VolBeamColor[fogBeam].b / 255.f,
				(float)cos(Level->VolBeamOuter[fogBeam] * M_PI / 180.0) };
		}
		else
		{
			VPUniforms.mFogBeamPos = { 0.f, 0.f, 0.f, 0.f };
		}
	}
	mClipper->SetViewpoint(Viewpoint);
	vClipper->SetViewpoint(Viewpoint);
	rClipper->SetViewpoint(Viewpoint);

	ClearBuffers();

	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	hudsprites.Clear();
//	Coronas.Clear();
	vpIndex = 0;
	HasMultiviewViewpoints = false;
	HasMultiviewProjectionMatrix2 = false;

	// Fullbright information needs to be propagated from the main view.
	if (outer != nullptr) FullbrightFlags = outer->FullbrightFlags;
	else FullbrightFlags = 0;

	outer = gl_drawinfo;
	gl_drawinfo = this;

}

//==========================================================================
//
//
//
//==========================================================================

HWDrawInfo *HWDrawInfo::EndDrawInfo()
{
	assert(this == gl_drawinfo);
	for (int i = 0; i < GLDL_TYPES; i++) drawlists[i].Reset();
	gl_drawinfo = outer;
	di_list.Release(this);
	if (gl_drawinfo == nullptr)
		ResetRenderDataAllocator();
	return gl_drawinfo;
}


//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::ClearBuffers()
{
	otherFloorPlanes.Clear();
	otherCeilingPlanes.Clear();
	floodFloorSegs.Clear();
	floodCeilingSegs.Clear();

	// clear all the lists that might not have been cleared already
	MissingUpperTextures.Clear();
	MissingLowerTextures.Clear();
	MissingUpperSegs.Clear();
	MissingLowerSegs.Clear();
	SubsectorHacks.Clear();
	//CeilingStacks.Clear();
	//FloorStacks.Clear();
	HandledSubsectors.Clear();
	spriteindex = 0;

	if (Level)
	{
		CurrentMapSections.Resize(Level->NumMapSections);
		CurrentMapSections.Zero();

		section_renderflags.Resize(Level->sections.allSections.Size());
		ss_renderflags.Resize(Level->subsectors.Size());
		no_renderflags.Resize(Level->subsectors.Size());

		memset(&section_renderflags[0], 0, Level->sections.allSections.Size() * sizeof(section_renderflags[0]));
		memset(&ss_renderflags[0], 0, Level->subsectors.Size() * sizeof(ss_renderflags[0]));
		memset(&no_renderflags[0], 0, Level->nodes.Size() * sizeof(no_renderflags[0]));
	}

	Decals[0].Clear();
	Decals[1].Clear();

	mClipPortal = nullptr;
	mCurrentPortal = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::UpdateCurrentMapSection()
{
	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.bDoOob || Viewpoint.bDoOrtho)
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
}


//-----------------------------------------------------------------------------
//
// Sets the area the camera is in
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetViewArea()
{
	auto &vp = Viewpoint;
	// The render_sector is better suited to represent the current position in GL
	vp.sector = Level->PointInRenderSubsector(vp.Pos)->render_sector;
	if (Viewpoint.bDoOob)
		vp.sector = Level->PointInRenderSubsector(vp.camera->Pos())->render_sector;

	// Get the heightsec state from the render sector, not the current one!
	if (vp.sector->GetHeightSec())
	{
		in_area = vp.Pos.Z <= vp.sector->heightsec->floorplane.ZatPoint(vp.Pos) ? area_below :
			(vp.Pos.Z > vp.sector->heightsec->ceilingplane.ZatPoint(vp.Pos) &&
				!(vp.sector->heightsec->MoreFlags&SECMF_FAKEFLOORONLY)) ? area_above : area_normal;
	}
	else
	{
		in_area = Level->HasHeightSecs ? area_default : area_normal;	// depends on exposed lower sectors, if map contains heightsecs.
	}
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

int HWDrawInfo::SetFullbrightFlags(player_t *player)
{
	FullbrightFlags = 0;

	// check for special colormaps
	player_t * cplayer = player? player->camera->player : nullptr;
	if (cplayer)
	{
		int cm = CM_DEFAULT;
		if (cplayer->extralight == INT_MIN)
		{
			cm = static_cast<int>(CM_FIRSTSPECIALCOLORMAP) + static_cast<int>(REALINVERSECOLORMAP);
			Viewpoint.extralight = 0;
			FullbrightFlags = Fullbright;
			// This does never set stealth vision.
		}
		else if (cplayer->fixedcolormap != NOFIXEDCOLORMAP)
		{
			cm = CM_FIRSTSPECIALCOLORMAP + cplayer->fixedcolormap;
			FullbrightFlags = Fullbright;
			if (gl_enhanced_nv_stealth > 2) FullbrightFlags |= StealthVision;
		}
		else if (cplayer->fixedlightlevel != -1 || cplayer->bForceFullbright)
		{
			EFullbrightMode fbmode = cplayer->GetFullbrightMode();
			if (fbmode != FBMODE_NONE)
			{
				FullbrightFlags = Fullbright;
				if (fbmode == FBMODE_TORCH)
				{
					FullbrightFlags |= StealthVision * (gl_enhanced_nv_stealth > 1);
				}
				else
				{
					FullbrightFlags |= Nightvision * (fbmode == FBMODE_NIGHTVISION);
					FullbrightFlags |= StealthVision * (gl_enhanced_nv_stealth > 0);
				}
			}
		}
		return cm;
	}
	else
	{
		return CM_DEFAULT;
	}
}

//-----------------------------------------------------------------------------
//
// R_FrustumAngle
//
//-----------------------------------------------------------------------------

angle_t OoBFrustumAngle(FRenderViewpoint* Viewpoint)
{
	// If pitch is larger than this you can look all around at an FOV of 90 degrees
	if (fabs(Viewpoint->HWAngles.Pitch.Degrees()) > 89.0)  return 0xffffffff;
	int aspMult = AspectMultiplier(r_viewwindow.WidescreenRatio); // 48 == square window
	double absPitch = fabs(Viewpoint->HWAngles.Pitch.Degrees());
	 // Smaller aspect ratios still clip too much. Need a better solution
	if (aspMult > 36 && absPitch > 30.0)  return 0xffffffff;
	else if (aspMult > 40 && absPitch > 25.0)  return 0xffffffff;
	else if (aspMult > 45 && absPitch > 20.0)  return 0xffffffff;
	else if (aspMult > 47 && absPitch > 10.0) return 0xffffffff;

	double xratio = r_viewwindow.FocalTangent / Viewpoint->PitchCos;
	double floatangle = 0.05 + atan ( xratio ) * 48.0 / aspMult; // this is radians
	angle_t a1 = DAngle::fromRad(floatangle).BAMs();

	if (a1 >= ANGLE_90) return 0xffffffff;
	return a1;
}

angle_t HWDrawInfo::FrustumAngle()
{
	if (Viewpoint.bDoOob)
	{
		return OoBFrustumAngle(&Viewpoint);
	}
	else
	{
		float tilt = fabs(Viewpoint.HWAngles.Pitch.Degrees());

		// If the pitch is larger than this you can look all around at a FOV of 90°
		if (tilt > 46.0f) return 0xffffffff;

		// ok, this is a gross hack that barely works...
		// but at least it doesn't overestimate too much...
		double floatangle = 2.0 + (45.0 + ((tilt / 1.9)))*Viewpoint.GetFieldOfView().Degrees() * 48.0 / AspectMultiplier(r_viewwindow.WidescreenRatio) / 90.0;
		angle_t a1 = DAngle::fromDeg(floatangle).BAMs();
		if (a1 >= ANGLE_180) return 0xffffffff;
		return a1;
	}
}

//-----------------------------------------------------------------------------
//
// Setup the modelview matrix
//
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
//
// [BB] Resolve the volumetric beam from world space into view space.
//
// Done per scene draw rather than once per frame, and that is the point: each
// eye and each portal view builds its own view matrix, so resolving here
// means stereo and portals are correct without the shader or the script
// knowing either exists.
//
// Vertex positions in this engine are (mapX, height, mapY) -- GL convention,
// Y up -- so the world point is swizzled before the matrix is applied.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetupVolumetricBeam()
{
	if (Level == nullptr)
	{
		hw_postprocess.volbeam.ClearBeams();
		return;
	}

	hw_postprocess.volbeam.ClearBeams();

	auto worldToView = [this](const DVector3 &w, bool isDirection) -> FVector3
	{
		float pt[4] = { (float)w.X, (float)w.Z, (float)w.Y, isDirection ? 0.0f : 1.0f };
		float out[4];
		VPUniforms.mViewMatrix.multMatrixPoint(pt, out);
		return FVector3(out[0], out[1], out[2]);
	};

	// Every live slot gets its own uniform set. The pass draws them one after
	// another and is ADDITIVE, so they composite correctly with no blending
	// work and no shader change -- each pass contributes only its own light.
	for (int bi = 0; bi < FLevelLocals::MAX_VOL_BEAMS; bi++)
	{
	if (!Level->VolBeamActive[bi]) continue;

	VolumetricBeamUniforms u = {};
	u.BeamPos = worldToView(Level->VolBeamPos[bi], false);

	FVector3 dir = worldToView(Level->VolBeamDir[bi], true);
	float dl = dir.Length();
	u.BeamDir = (dl > 0.0001f) ? dir / dl : FVector3(0, 0, -1);

	u.BeamColor = FVector3(Level->VolBeamColor[bi].r / 255.f,
		Level->VolBeamColor[bi].g / 255.f,
		Level->VolBeamColor[bi].b / 255.f);

	// Half-angles arrive in degrees; the shader compares cosines, so convert
	// once here rather than per pixel.
	u.CosInner = (float)cos(Level->VolBeamInner[bi] * M_PI / 180.0);
	u.CosOuter = (float)cos(Level->VolBeamOuter[bi] * M_PI / 180.0);
	u.BeamLength = (float)Level->VolBeamLength[bi];
	u.Density = (float)Level->VolBeamDensity[bi];
	u.Falloff = (float)Level->VolBeamFalloff[bi];

	// Rebuilding the pixel ray in the shader needs the view frustum's shape,
	// which is exactly what the projection matrix's first two diagonals hold.
	const float *proj = VPUniforms.mProjectionMatrix.get();
	float px = (proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f;
	float py = (proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f;
	u.TanHalfFov = FVector2(px, py);

	u.StepCount = clamp((int)vol_beam_quality, 8, 64);

	// Dust is sampled in world space, so the shader needs a way back out of
	// view space. Without this the motes would ride along with the camera.
	u.DustAmount = (float)Level->VolBeamDust[bi];
	u.DustScale = (float)Level->VolBeamDustScale[bi];
	u.DustDrift = (float)Level->VolBeamDustDrift[bi];
	u.DustTime = (float)(screen->FrameTime * 0.001);
	u.AxisFade = (float)clamp<double>(vol_beam_axisfade, 0.0, 1.0);

	// The two constants that turn a raw depth sample back into a view-space
	// distance, identical to the pair PPAmbientOcclusion feeds lineardepth.fp.
	// The pass was comparing the raw 0..1 sample against a march distance in
	// map units, so anything in front of the camera pinned the march to under
	// one unit and the beam integrated across nothing at all.
	u.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	u.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);

	VSMatrix inv;
	if (!VPUniforms.mViewMatrix.inverseMatrix(inv)) inv.loadIdentity();
	memcpy(u.ViewToWorld, inv.get(), sizeof(float) * 16);

	hw_postprocess.volbeam.AddBeam(u);
	}
}

//-----------------------------------------------------------------------------
//
// [BB] The heatmap: where the fighting happened, painted on the floor.
//
// The grid is stamped on the CPU when something dies -- see HeatmapAdd in
// vmthunks.cpp -- and this hands it to the postprocess pass, re-uploading only
// when it has actually changed. Deaths are rare, so almost every frame this is
// four uniform writes and nothing else.
//
// A postprocess pass rather than a term in the scene shader, deliberately. The
// scene-shader route would let the heat tint the LIGHT rather than paint over
// the frame, and would be occluded correctly by translucent geometry, but it
// costs four coordinated edits inside the Vulkan backend -- a GLSL binding, a
// descriptor set layout, a descriptor POOL SIZE, and a per-frame descriptor
// write -- and missing any one of them fails either silently or on every draw.
// This route touches no backend file at all.
//
//-----------------------------------------------------------------------------

void HWDrawInfo::SetupHeatmap()
{
	if (Level == nullptr || Level->HeatScale <= 0.0 || Level->HeatIntensity.Size() == 0)
	{
		hw_postprocess.heatmap.ClearHeat();
		return;
	}

	const int R = FLevelLocals::HEAT_RES;

	// DECAY, applied here rather than on a timer, because this is the one
	// place that already knows a frame has passed and already has to re-upload
	// when the values move. A separate decay tick would dirty the grid every
	// frame forever even with nothing happening.
	if (Level->HeatDecay > 0.0)
	{
		float drop = float(Level->HeatDecay * screen->FrameTime * 0.001);
		if (drop > 0.0f)
		{
			bool moved = false;
			for (unsigned i = 0; i < Level->HeatIntensity.Size(); i++)
			{
				float v = Level->HeatIntensity[i];
				if (v <= 0.0f) continue;
				Level->HeatIntensity[i] = std::max(v - drop, 0.0f);
				moved = true;
			}
			if (moved) Level->HeatDirty = true;
		}
	}

	if (Level->HeatDirty || !hw_postprocess.heatmap.HasGrid())
	{
		// Copied rather than referenced. PPTexture keeps its data alive through
		// a shared_ptr and the backend uploads from it at an unspecified later
		// point, so handing it a pointer into a TArray the playsim is still
		// writing to would be a race the moment two monsters died in one frame.
		std::shared_ptr<void> idata(new float[R * R], [](void *p) { delete[](float*)p; });
		std::shared_ptr<void> hdata(new float[R * R], [](void *p) { delete[](float*)p; });
		memcpy(idata.get(), &Level->HeatIntensity[0], R * R * sizeof(float));
		memcpy(hdata.get(), &Level->HeatHeight[0], R * R * sizeof(float));

		hw_postprocess.heatmap.SetGrid(R, idata, hdata);
		Level->HeatDirty = false;
	}

	HeatmapUniforms u = {};
	u.HeatScale = (float)Level->HeatScale;
	u.HeatCeiling = (float)std::max(Level->HeatCeiling, 0.01);
	u.HeatTolerance = (float)std::max(Level->HeatTolerance, 1.0);
	u.HeatColorLow = FVector3(Level->HeatColorLow.r / 255.f,
		Level->HeatColorLow.g / 255.f, Level->HeatColorLow.b / 255.f);
	u.HeatColorHigh = FVector3(Level->HeatColorHigh.r / 255.f,
		Level->HeatColorHigh.g / 255.f, Level->HeatColorHigh.b / 255.f);

	// The grid covers the map's own bounding box, from the blockmap, which is
	// the one structure that already knows it.
	double mw = std::max((double)(Level->blockmap.bmapwidth * FBlockmap::MAPBLOCKUNITS), 1.0);
	double mh = std::max((double)(Level->blockmap.bmapheight * FBlockmap::MAPBLOCKUNITS), 1.0);
	u.HeatOrigin = FVector2((float)Level->blockmap.bmaporgx, (float)Level->blockmap.bmaporgy);
	u.HeatInvSize = FVector2((float)(1.0 / mw), (float)(1.0 / mh));

	// Rebuilding the pixel ray needs the frustum's shape, which is the
	// projection matrix's first two diagonals -- same as the beam pass.
	const float *proj = VPUniforms.mProjectionMatrix.get();
	u.TanHalfFov = FVector2(
		(proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f,
		(proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f);

	u.LinearizeDepthA = 1.0f / screen->GetZFar() - 1.0f / screen->GetZNear();
	u.LinearizeDepthB = max(1.0f / screen->GetZNear(), 1.e-8f);

	VSMatrix inv;
	if (!VPUniforms.mViewMatrix.inverseMatrix(inv)) inv.loadIdentity();
	memcpy(u.ViewToWorld, inv.get(), sizeof(float) * 16);

	hw_postprocess.heatmap.SetHeat(u);
}

void HWDrawInfo::SetViewMatrix(const FRotator &angles, float vx, float vy, float vz, bool mirror, bool planemirror)
{
	float mult = mirror ? -1.f : 1.f;
	float planemult = planemirror ? -Level->info->pixelstretch : Level->info->pixelstretch;

	VPUniforms.mViewMatrix.loadIdentity();
	VPUniforms.mViewMatrix.rotate(angles.Roll.Degrees(), 0.0f, 0.0f, 1.0f);
	VPUniforms.mViewMatrix.rotate(angles.Pitch.Degrees(), 1.0f, 0.0f, 0.0f);
	VPUniforms.mViewMatrix.rotate(angles.Yaw.Degrees(), 0.0f, mult, 0.0f);
	VPUniforms.mViewMatrix.translate(vx * mult, -vz * planemult, -vy);
	VPUniforms.mViewMatrix.scale(-mult, planemult, 1);
}


//-----------------------------------------------------------------------------
//
// SetupView
// Setup the view rotation matrix for the given viewpoint
//
//-----------------------------------------------------------------------------
void HWDrawInfo::SetupView(FRenderState &state, float vx, float vy, float vz, bool mirror, bool planemirror, bool upload)
{
	auto &vp = Viewpoint;
	vp.SetViewAngle(r_viewwindow);
	HWViewpointUniforms previousLeft = VPUniforms;
	const HWViewpointUniforms previousRight = MultiviewVPUniforms[1];
	SetViewMatrix(vp.HWAngles, vx, vy, vz, mirror, planemirror);
	SetCameraPos({ vx, vy, vz });
	VPUniforms.CalcDependencies();
	if (HasMultiviewViewpoints)
	{
		HWViewpointUniforms nextRight = VPUniforms;
		nextRight.mProjectionMatrix = previousRight.mProjectionMatrix;

		VSMatrix inverseLeft;
		if (previousLeft.mViewMatrix.inverseMatrix(inverseLeft))
		{
			VSMatrix viewDelta = inverseLeft;
			viewDelta.multMatrix(VPUniforms.mViewMatrix);
			nextRight.mViewMatrix = previousRight.mViewMatrix;
			nextRight.mViewMatrix.multMatrix(viewDelta);
		}
		else
		{
			nextRight.mViewMatrix = previousRight.mViewMatrix;
		}

		const FVector4 cameraDelta = VPUniforms.mCameraPos - previousLeft.mCameraPos;
		nextRight.mCameraPos = previousRight.mCameraPos + cameraDelta;
		nextRight.CalcDependencies();

		MultiviewVPUniforms[0] = VPUniforms;
		MultiviewVPUniforms[1] = nextRight;
	}
	if (upload)
		ApplyViewpoint(state);
}

void HWDrawInfo::ApplyViewpoint(FRenderState &state)
{
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0] = VPUniforms;
		MultiviewVPUniforms[0].CalcDependencies();
		MultiviewVPUniforms[1].CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoints(state, MultiviewVPUniforms, 2);
	}
	else
	{
		VPUniforms.CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
	}
}

void HWDrawInfo::ApplyMultiviewViewpoints(FRenderState &state, const HWViewpointUniforms *viewpoints, int count)
{
	if (viewpoints == nullptr || count <= 0)
		return;

	VPUniforms = viewpoints[0];
	HasMultiviewViewpoints = count >= 2;
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0] = viewpoints[0];
		MultiviewVPUniforms[1] = viewpoints[1];
		MultiviewVPUniforms[0].CalcDependencies();
		MultiviewVPUniforms[1].CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoints(state, MultiviewVPUniforms, 2);
	}
	else
	{
		VPUniforms.CalcDependencies();
		vpIndex = screen->mViewpoints->SetViewpoint(state, &VPUniforms);
	}
}

void HWDrawInfo::RemoveMultiviewPositionParallax()
{
	if (!HasMultiviewViewpoints)
		return;

	FLOATTYPE leftView[16];
	FLOATTYPE rightView[16];
	MultiviewVPUniforms[0].mViewMatrix.copy(leftView);
	MultiviewVPUniforms[1].mViewMatrix.copy(rightView);

	rightView[12] = leftView[12];
	rightView[13] = leftView[13];
	rightView[14] = leftView[14];
	MultiviewVPUniforms[1].mViewMatrix.loadMatrix(rightView);
	MultiviewVPUniforms[1].mCameraPos = MultiviewVPUniforms[0].mCameraPos;
	MultiviewVPUniforms[1].CalcDependencies();
	VPUniforms = MultiviewVPUniforms[0];
}

void HWDrawInfo::TranslateViewpointMatrices(double x, double y, double z)
{
	VPUniforms.mViewMatrix.translate(x, y, z);
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0].mViewMatrix.translate(x, y, z);
		MultiviewVPUniforms[1].mViewMatrix.translate(x, y, z);
		VPUniforms = MultiviewVPUniforms[0];
	}
}

void HWDrawInfo::InheritMultiviewState(const HWDrawInfo& other)
{
	HasMultiviewViewpoints = other.HasMultiviewViewpoints;
	if (HasMultiviewViewpoints)
	{
		MultiviewVPUniforms[0] = other.MultiviewVPUniforms[0];
		MultiviewVPUniforms[1] = other.MultiviewVPUniforms[1];
		VPUniforms = MultiviewVPUniforms[0];
	}

	HasMultiviewProjectionMatrix2 = other.HasMultiviewProjectionMatrix2;
	if (HasMultiviewProjectionMatrix2)
	{
		MultiviewProjectionMatrix2[0] = other.MultiviewProjectionMatrix2[0];
		MultiviewProjectionMatrix2[1] = other.MultiviewProjectionMatrix2[1];
	}
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWPortal * HWDrawInfo::FindPortal(const void * src)
{
	int i = Portals.Size() - 1;

	while (i >= 0 && Portals[i] && Portals[i]->GetSource() != src) i--;
	return i >= 0 ? Portals[i] : nullptr;
}

//-----------------------------------------------------------------------------
//
//
//
//-----------------------------------------------------------------------------

HWDecal *HWDrawInfo::AddDecal(bool onmirror)
{
	auto decal = (HWDecal*)RenderDataAllocator.Alloc(sizeof(HWDecal));
	Decals[onmirror ? 1 : 0].Push(decal);
	return decal;
}

//-----------------------------------------------------------------------------
//
// CreateScene
//
// creates the draw lists for the current scene
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//
// [BB] DispatchBillboards
//
// Every live billboard becomes a quad in the draw lists. Attached ones read
// their actor's INTERPOLATED position here rather than the ticked one, so
// they track smoothly at render framerate instead of stepping at 35Hz.
//
//-----------------------------------------------------------------------------

CVAR(Int, rs_bb_maxpanels, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)		// 0 = unlimited
CVAR(Float, rs_bb_cullradius, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)	// 0 = no distance limit

void HWDrawInfo::DispatchBillboards()
{
	if (!Level || Level->Billboards.Size() == 0) return;

	const auto &vp = Viewpoint;

	// Under a budget, the nearest billboards win. Distance is measured
	// squared and only compared, never rooted -- and the far ones are the
	// ones nobody can read anyway, so dropping those first is both the
	// cheapest and the least noticeable thing to do.
	const double cullR = rs_bb_cullradius;
	const double cullR2 = cullR * cullR;
	const int budget = rs_bb_maxpanels;

	double keepDist2 = FLT_MAX;
	if (budget > 0 && (int)Level->Billboards.Size() > budget)
	{
		TArray<double> dists;
		dists.Reserve(Level->Billboards.Size());
		unsigned n = 0;
		for (auto &bb : Level->Billboards)
		{
			DVector3 probe = (bb.flags & BBFL_VIEWLOCKED) ? vp.Pos : bb.pos;
			dists[n++] = (probe - vp.Pos).LengthSquared();
		}
		TArray<double> sorted = dists;
		std::sort(sorted.begin(), sorted.end());
		keepDist2 = sorted[budget - 1];
	}

	for (auto &bb : Level->Billboards)
	{
		// [BB] The group transform, resolved HERE so it moves at frame rate.
		//
		// It scales the member's LOCAL offset -- before the view-lock or the
		// attachment is resolved -- because the group origin is expressed in
		// that same local space. Scaling the world position instead would
		// drag a head-locked panel toward the map origin as it shrank.
		//
		// Attached billboards carry their offset in attachOffset rather than
		// pos, so that is what gets scaled for them.
		double gscale = 1.0;
		DVector3 gorigin(0, 0, 0);
		DVector3 lpos = bb.pos;
		DVector3 lattach = bb.attachOffset;
		if (bb.group)
		{
			gscale = Level->BillboardGroupScale(bb.group, vp.TicFrac, &gorigin);
			if (gscale <= 0.0) continue;		// fully collapsed: nothing to submit
			lpos    = gorigin + (lpos - gorigin) * gscale;
			lattach = gorigin + (lattach - gorigin) * gscale;
		}

		DVector3 bpos = lpos;

		if (bb.flags & BBFL_VIEWLOCKED)
		{
			// pos is an offset from the viewer, not a world point: X ahead,
			// Y to the right, Z up. Resolved here rather than in script
			// because script runs at tic rate and the view does not -- a
			// head-locked panel repositioned at 35Hz lags and snaps against
			// head movement, which is exactly the thing that makes people
			// ill. Doing it against the render viewpoint keeps it welded.
			double yawRad = vp.Angles.Yaw.Radians();
			double cy = cos(yawRad), sy = sin(yawRad);
			bpos = vp.Pos
				+ DVector3(cy, sy, 0.0) * lpos.X		// ahead
				+ DVector3(-sy, cy, 0.0) * lpos.Y		// right
				+ DVector3(0.0, 0.0, 1.0) * lpos.Z;		// up
		}
		else if ((bb.flags & BBFL_ATTACHED) && bb.attachedTo != nullptr)
		{
			bpos = bb.attachedTo->InterpolatedPosition(Viewpoint.TicFrac) + lattach;
		}

		// Remember where it landed so the aim and touch queries test against
		// what was actually drawn.
		bb.drawPos = bpos;

		// View-locked panels are never culled: they are welded to the eye, so
		// distance to them is meaningless and losing one to a budget would
		// read as the UI vanishing.
		if (!(bb.flags & BBFL_VIEWLOCKED))
		{
			double d2 = (bpos - vp.Pos).LengthSquared();
			if (cullR2 > 0.0 && d2 > cullR2) continue;
			if (d2 > keepDist2) continue;
		}

		auto sector = Level->PointInSector(bpos.XY());
		if (!sector) continue;

		HWSprite sprite;
		sprite.ProcessBillboard(this, &bb, bpos, sector, gscale);
	}
}

void HWDrawInfo::CreateScene(bool drawpsprites)
{
	const auto &vp = Viewpoint;
	angle_t a1 = FrustumAngle(); // horizontally clip the back of the viewport
	mClipper->SafeAddClipRangeRealAngles(vp.Angles.Yaw.BAMs() + a1, vp.Angles.Yaw.BAMs() - a1);
	Viewpoint.FrustAngle = a1;
	if (Viewpoint.bDoOob) // No need for vertical clipper if viewpoint not allowed out of bounds
	{
		double a2 = 20.0 + 0.5*Viewpoint.GetFieldOfView().Degrees(); // FrustumPitch for vertical clipping
		if (a2 > 179.0) a2 = 179.0;
		double pitchmult = !!(portalState.PlaneMirrorFlag & 1) ? -1.0 : 1.0;
		vClipper->SafeAddClipRangeDegPitches(pitchmult * vp.HWAngles.Pitch.Degrees() - a2, pitchmult * vp.HWAngles.Pitch.Degrees() + a2); // clip the suplex range
		Viewpoint.PitchSin *= pitchmult;
	}

	// reset the portal manager
	portalState.StartFrame();

	if (IsVRScene) VRSceneBuild.Clock();
	ProcessAll.Clock();

	// clip the scene and fill the drawlists
	screen->mVertexData->Map();
	screen->mLights->Map();

	RenderBSP(Level->HeadNode(), drawpsprites);

	// [BB] billboards join the scene here -- after the BSP walk has filled
	// the draw lists, before the vertex buffer unmaps below.
	DispatchBillboards();

	// And now the crappy hacks that have to be done to avoid rendering anomalies.
	// These cannot be multithreaded when the time comes because all these depend
	// on the global 'validcount' variable.

	if (IsVRScene) VRScenePostBSP.Clock();
	HandleMissingTextures(in_area);	// Missing upper/lower textures
	HandleHackedSubsectors();	// open sector hacks for deep water
	PrepareUnhandledMissingTextures();
	DispatchRenderHacks();
	if (IsVRScene) VRScenePostBSP.Unclock();
	screen->mLights->Unmap();
	screen->mVertexData->Unmap();

	ProcessAll.Unclock();
	if (IsVRScene) VRSceneBuild.Unclock();

}

//-----------------------------------------------------------------------------
//
// RenderScene
//
// Draws the current draw lists for the non GLSL renderer
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderScene(FRenderState &state)
{
	const auto &vp = Viewpoint;
	if (IsVRScene) VRSceneDraw.Clock();
	RenderAll.Clock();

	state.SetDepthMask(true);

	// [BB] A DEFINED FOG SCALE FOR EVERYTHING, not just walls, flats and
	// sprites.
	//
	// Only those three set it, and FRenderState::Reset runs once at startup --
	// so anything drawn outside them inherited whatever the previous draw left,
	// including across frames. The sky is the worst case: it is drawn BEFORE
	// this, so it took the last draw of the previous frame. In VR each eye is
	// its own pass, so the two eyes disagreed about the sky -- binocular
	// rivalry on the largest surface in view.
	//
	// Reset here so the default is 1, and the sky portal sets the outdoor
	// value for itself, a sky being the outdoor case by definition.
	state.SetFogDensityScale(1.0f);

	// [BB] And flat glow, for the same reason. It is written by the flat path
	// and main.fp applies it to ANY surface, so every path that neither sets
	// nor clears it inherits the last flat drawn -- which is what made walls
	// light and unlight as the viewpoint moved. The wall, sprite and weapon
	// paths decide for themselves now; this covers everything else, including
	// whatever is drawn before the first flat of the frame.
	state.ClearFlatGlow();

	// [BB] Sweep: set once for the whole scene rather than per draw. It is a
	// world-space band, not a property of any sector or surface, so every
	// draw that follows inherits it and the band stays continuous across
	// floor, wall and ceiling without any of them coordinating.
	if (Level != nullptr && Level->SweepMode > 0 && Level->SweepCount > 0)
	{
		int n = min(Level->SweepCount, FLevelLocals::MAX_SWEEP_BANDS);
		state.SetSweepOrigin(Level->SweepMode,
			(float)Level->SweepOrigin.X, (float)Level->SweepOrigin.Z, (float)Level->SweepOrigin.Y, n,
			(float)Level->SweepTrail);
		for (int i = 0; i < n; i++)
		{
			state.SetSweepBand(i,
				(float)Level->SweepRadius[i], (float)Level->SweepThickness[i], (float)Level->SweepSoftness[i],
				Level->SweepColor[i].r / 255.f, Level->SweepColor[i].g / 255.f, Level->SweepColor[i].b / 255.f,
				(float)Level->SweepIntensity[i]);

			// Same swizzle as the shared origin above: Doom's Z is the
			// shader's Y. A band left at mode 0 falls back to the shared
			// origin, which SetSweepOrigin already seeded into all eight.
			// A FILL WITH NO DRAW OVERRIDE STILL HAS TO BE WRITTEN.
			// SetSweepBand seeds mode 1 into this component, so leaving
			// the call out when SweepBandDraw is 0 would silently drop the
			// fill for every band that never overrode its draw mode --
			// which is most of them.
			if (Level->SweepBandDraw[i] > 0 || Level->SweepBandFill[i] > 0)
			{
				int dm = Level->SweepBandDraw[i] > 0 ? Level->SweepBandDraw[i] : 1;
				state.SetSweepBandDraw(i, dm, Level->SweepBandFill[i]);
			}

			if (Level->SweepBandMode[i] > 0)
			{
				state.SetSweepBandOrigin(i,
					(float)Level->SweepBandOrigin[i].X,
					(float)Level->SweepBandOrigin[i].Z,
					(float)Level->SweepBandOrigin[i].Y,
					Level->SweepBandMode[i]);
			}
		}
	}
	else
	{
		state.ClearSweep();
	}

	SetupVolumetricBeam();
	SetupHeatmap();

	state.EnableFog(true);
	state.SetRenderStyle(STYLE_Source);

	if (gl_sort_textures)
	{
		drawlists[GLDL_PLAINWALLS].SortWalls();
		drawlists[GLDL_PLAINFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLS].SortWalls();
		drawlists[GLDL_MASKEDFLATS].SortFlats();
		drawlists[GLDL_MASKEDWALLSOFS].SortWalls();
	}

	// Part 1: solid geometry. This is set up so that there are no transparent parts
	state.SetDepthFunc(DF_Less);
	state.AlphaFunc(Alpha_GEqual, 0.f);
	state.ClearDepthBias();

	state.EnableTexture(gl_texture);
	state.EnableBrightmap(true);
	drawlists[GLDL_PLAINWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_PLAINFLATS].DrawFlats(this, state, false);


	// Part 2: masked geometry. This is set up so that only pixels with alpha>gl_mask_threshold will show
	state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
	drawlists[GLDL_MASKEDWALLS].DrawWalls(this, state, false);
	drawlists[GLDL_MASKEDFLATS].DrawFlats(this, state, false);

	// Part 3: masked geometry with polygon offset. This list is empty most of the time so only waste time on it when in use.
	if (drawlists[GLDL_MASKEDWALLSOFS].Size() > 0)
	{
		state.SetDepthBias(-1, -128);
		drawlists[GLDL_MASKEDWALLSOFS].DrawWalls(this, state, false);
		state.ClearDepthBias();
	}

	drawlists[GLDL_MODELS].Draw(this, state, false);

	state.SetRenderStyle(STYLE_Translucent);

	// Part 4: Draw decals (not a real pass)
	state.SetDepthFunc(DF_LEqual);
	DrawDecals(state, Decals[0]);

	RenderAll.Unclock();
	if (IsVRScene) VRSceneDraw.Unclock();
}

//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderTranslucent(FRenderState &state)
{
	if (IsVRScene) VRSceneDraw.Clock();
	RenderAll.Clock();

	// final pass: translucent stuff
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetRenderStyle(STYLE_Translucent);

	state.EnableBrightmap(true);
	drawlists[GLDL_TRANSLUCENTBORDER].Draw(this, state, true);
	state.SetDepthMask(false);

	drawlists[GLDL_TRANSLUCENT].DrawSorted(this, state);
	state.EnableBrightmap(false);


	state.AlphaFunc(Alpha_GEqual, 0.5f);
	state.SetDepthMask(true);

	RenderAll.Unclock();
	if (IsVRScene) VRSceneDraw.Unclock();
}


//-----------------------------------------------------------------------------
//
// RenderTranslucent
//
//-----------------------------------------------------------------------------

void HWDrawInfo::RenderPortal(HWPortal *p, FRenderState &state, bool usestencil)
{
	if (gl_max_portals > -1 && portalsPerEye >= gl_max_portals) return;
	auto gp = static_cast<HWPortal *>(p);
	gp->SetupStencil(this, state, usestencil);
	auto new_di = StartDrawInfo(this->Level, this, Viewpoint, &VPUniforms);
	new_di->InheritMultiviewState(*this);
	new_di->ProjectionMatrix2 = ProjectionMatrix2;
	new_di->mCurrentPortal = gp;
	state.SetLightIndex(-1);
	gp->DrawContents(new_di, state);
	new_di->EndDrawInfo();
	state.SetVertexBuffer(screen->mVertexData);
	screen->mViewpoints->Bind(state, vpIndex);
	gp->RemoveStencil(this, state, usestencil);

}

void HWDrawInfo::DrawCorona(FRenderState& state, ACorona* corona, double dist)
{
#if 0
	spriteframe_t* sprframe = &SpriteFrames[sprites[corona->sprite].spriteframes + (size_t)corona->SpawnState->GetFrame()];
	FTextureID patch = sprframe->Texture[0];
	if (!patch.isValid()) return;
	auto tex = TexMan.GetGameTexture(patch, false);
	if (!tex || !tex->isValid()) return;

	// Project the corona sprite center
	FVector4 worldPos((float)corona->X(), (float)corona->Z(), (float)corona->Y(), 1.0f);
	FVector4 viewPos, clipPos;
	VPUniforms.mViewMatrix.multMatrixPoint(&worldPos[0], &viewPos[0]);
	VPUniforms.mProjectionMatrix.multMatrixPoint(&viewPos[0], &clipPos[0]);
	if (clipPos.W < -1.0f) return; // clip z nearest
	float halfViewportWidth = screen->GetWidth() * 0.5f;
	float halfViewportHeight = screen->GetHeight() * 0.5f;
	float invW = 1.0f / clipPos.W;
	float screenX = halfViewportWidth + clipPos.X * invW * halfViewportWidth;
	float screenY = halfViewportHeight - clipPos.Y * invW * halfViewportHeight;

	float alpha = corona->CoronaFade * float(corona->Alpha);

	// distance-based fade - looks better IMO
	float distNearFadeStart = float(corona->RenderRadius()) * 0.1f;
	float distFarFadeStart = float(corona->RenderRadius()) * 0.5f;
	float distFade = 1.0f;

	if (float(dist) < distNearFadeStart)
		distFade -= abs(((float(dist) - distNearFadeStart) / distNearFadeStart));
	else if (float(dist) >= distFarFadeStart)
		distFade -= (float(dist) - distFarFadeStart) / distFarFadeStart;

	alpha *= distFade;

	state.SetColorAlpha(0xffffff, alpha, 0);
	if (isSoftwareLighting()) state.SetSoftLightLevel(255);
	else state.SetNoSoftLightLevel();

	state.SetLightIndex(-1);
	state.SetRenderStyle(corona->RenderStyle);
	state.SetTextureMode(corona->RenderStyle);

	state.SetMaterial(tex, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, 0, 0);

	float scale = screen->GetHeight() / 1000.0f;
	float tileWidth = corona->Scale.X * tex->GetDisplayWidth() * scale;
	float tileHeight = corona->Scale.Y * tex->GetDisplayHeight() * scale;
	float x0 = screenX - tileWidth, y0 = screenY - tileHeight;
	float x1 = screenX + tileWidth, y1 = screenY + tileHeight;

	float u0 = 0.0f, v0 = 0.0f;
	float u1 = 1.0f, v1 = 1.0f;

	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	unsigned int vertexindex = vert.second;

	vp[0].Set(x0, y0, 1.0f, u0, v0);
	vp[1].Set(x1, y0, 1.0f, u1, v0);
	vp[2].Set(x0, y1, 1.0f, u0, v1);
	vp[3].Set(x1, y1, 1.0f, u1, v1);

	state.Draw(DT_TriangleStrip, vertexindex, 4);
#endif
}

//==========================================================================
//
// TraceCallbackForDitherTransparency
// Toggles dither flag on anything that occludes the actor's
// position from viewpoint.
//
//==========================================================================

static ETraceStatus TraceCallbackForDitherTransparency(FTraceResults& res, void* userdata)
{
	BitArray* CurMapSections = (BitArray*)userdata;
	double bf, bc;

	switch(res.HitType)
	{
	case TRACE_HitWall:
		{
			sector_t* linesec = res.Line->sidedef[res.Side]->sector;
			if (linesec->subsectorcount > 0 && (*CurMapSections)[linesec->subsectors[0]->mapsection])
			{
				bf = res.Line->sidedef[res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY());
				bc = res.Line->sidedef[res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY());
				if (res.Line->sidedef[!res.Side])
				{
					// Two sided line! So let's find out if mid, top, or bottom texture needs dithered transparency
					bf = max(bf, res.Line->sidedef[!res.Side]->sector->floorplane.ZatPoint(res.HitPos.XY()));
					bc = min(bc, res.Line->sidedef[!res.Side]->sector->ceilingplane.ZatPoint(res.HitPos.XY()));
					if (res.HitPos.Z <= bf) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_BOTTOM;
					else if (res.HitPos.Z < bc) res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					else res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_TOP;

					res.Line->sidedef[res.Side]->dithertranscount = max<int>(1, res.Line->sidedef[!res.Side]->sector->e->XFloor.ffloors.Size());
				}
				else if ((res.HitPos.Z <= bc) && (res.HitPos.Z >= bf))
				{
					res.Line->sidedef[res.Side]->Flags |= WALLF_DITHERTRANS_MID;
					res.Line->sidedef[res.Side]->dithertranscount = 1;
				}
			}
		}
		break;
	case TRACE_HitFloor:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->floorplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->floorplane.ZatPoint(res.HitPos))
			{
				res.Sector->floorplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->top.plane->ZatPoint(res.HitPos))
						{
							rover->top.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitCeiling:
		if (res.Sector->subsectorcount > 0 && (*CurMapSections)[res.Sector->subsectors[0]->mapsection] && res.HitVector.dot(res.Sector->ceilingplane.Normal()) < 0.0)
		{
			if (res.HitPos.Z == res.Sector->ceilingplane.ZatPoint(res.HitPos))
			{
				res.Sector->ceilingplane.dithertransflag = true;
			}
			else if (res.Sector->e->XFloor.ffloors.Size()) // Maybe it was 3D floors
			{
				F3DFloor *rover;
				int kk;
				for (kk = 0; kk < (int)res.Sector->e->XFloor.ffloors.Size(); kk++)
				{
					rover = res.Sector->e->XFloor.ffloors[kk];
					if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
					{
						if (res.HitPos.Z == rover->bottom.plane->ZatPoint(res.HitPos))
						{
							rover->bottom.plane->dithertransflag = true;
							break; // Out of for loop
						}
					}
				}
			}
		}
		break;
	case TRACE_HitActor:
	default:
		break;
	}

	return TRACE_ContinueOutOfBounds;
}


void HWDrawInfo::SetDitherTransFlags(AActor* actor)
{
	// This should really be moved to a shader and have the GPU do some shape-tracing.
	if (actor && actor->Sector)
	{
		FTraceResults results;
		double horix = Viewpoint.Sin * actor->radius;
		double horiy = Viewpoint.Cos * actor->radius;
		DVector3 actorpos = actor->Pos();
		DVector3 vvec = actorpos - Viewpoint.Pos;
		if (Viewpoint.bDoOrtho)
		{
			vvec = 5.0 * Viewpoint.camera->ViewPos->Offset.Length() * Viewpoint.ViewVector3D; // Should be 4.0? (since zNear is behind screen by 3*dist in VREyeInfo::GetProjection())
		}
		double distance = vvec.Length() - actor->radius;
		DVector3 campos = actorpos - vvec;
		sector_t* startsec;

		vvec = vvec.Unit();
		campos.X -= horix; campos.Y += horiy; campos.Z += actor->Height * 0.25;
		for (int iter = 0; iter < 3; iter++)
		{
			startsec = Level->PointInRenderSubsector(campos)->sector;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z += actor->Height * 0.5;
			Trace(campos, startsec, vvec, distance,
				  0, 0, actor, results, TRACE_PortalRestrict, TraceCallbackForDitherTransparency, &CurrentMapSections);
			campos.Z -= actor->Height * 0.5;
			campos.X += horix; campos.Y -= horiy;
		}

		// Tracers don't work on 3D floors when you are starting in the same sector (standing under them, for example)
		if (actor->Sector->e->XFloor.ffloors.Size()) // 3D floor
		{
			F3DFloor *rover;
			for (int kk = 0; kk < (int)actor->Sector->e->XFloor.ffloors.Size(); kk++)
			{
				rover = actor->Sector->e->XFloor.ffloors[kk];
				rover->top.plane->dithertransflag = true;
				rover->bottom.plane->dithertransflag = true;
			}
		}
	}
}

static ETraceStatus CheckForViewpointActor(FTraceResults& res, void* userdata)
{
	FRenderViewpoint* data = (FRenderViewpoint*)userdata;
	if (res.HitType == TRACE_HitActor && res.Actor && res.Actor == data->ViewActor)
	{
		return TRACE_Skip;
	}

	return TRACE_Stop;
}


void HWDrawInfo::DrawCoronas(FRenderState& state)
{
	state.EnableDepthTest(false);
	state.SetDepthMask(false);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = VRMode::GetVRModeCached(true)->GetHUDSpriteProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);

	float timeElapsed = (screen->FrameTime - LastFrameTime) / 1000.0f;
	LastFrameTime = screen->FrameTime;

#if 0
	for (ACorona* corona : Coronas)
	{
		auto cPos = corona->Vec3Offset(0., 0., corona->Height * 0.5);
		DVector3 direction = Viewpoint.Pos - cPos;
		double dist = direction.Length();

		// skip coronas that are too far
		if (dist > corona->RenderRadius())
			continue;

		static const float fadeSpeed = 9.0f;

		direction.MakeUnit();
		FTraceResults results;
		if (!Trace(cPos, corona->Sector, direction, dist, MF_SOLID, ML_BLOCKEVERYTHING, corona, results, 0, CheckForViewpointActor, &Viewpoint))
		{
			corona->CoronaFade = std::min(corona->CoronaFade + timeElapsed * fadeSpeed, 1.0f);
		}
		else
		{
			corona->CoronaFade = std::max(corona->CoronaFade - timeElapsed * fadeSpeed, 0.0f);
		}

		if (corona->CoronaFade > 0.0f)
			DrawCorona(state, corona, dist);
	}
#endif

	state.SetTextureMode(TM_NORMAL);
	screen->mViewpoints->Bind(state, vpIndex);
	state.EnableDepthTest(true);
	state.SetDepthMask(true);
}


//-----------------------------------------------------------------------------
//
// Draws player sprites and color blend
//
//-----------------------------------------------------------------------------


void HWDrawInfo::EndDrawScene(sector_t * viewsector, FRenderState &state)
{
	HWSkyInfo skyinfo;
	skyinfo.init(this, viewsector, sector_t::ceiling, viewsector->skytransfer, viewsector->Colormap.FadeColor);
	if (skyinfo.texture[0])
	{
		auto& col = R_GetSkyCapColor(skyinfo.texture[0]);
		state.SetSceneColor(col.first);
	}
	state.InitSceneClearColor();

	state.EnableFog(false);

	/*if (gl_coronas && Coronas.Size() > 0)
	{
		DrawCoronas(state);
	}*/

	// [VR] In stereo modes the weapon/HUD model is drawn inside the scene per eye,
	// never as a flat post-scene overlay, so this whole block is gated off.
	auto vrmode = VRMode::GetVRModeCached(true);
	if (!vrmode->RenderPlayerSpritesInScene())
	{
		// [BB] HUD models need to be rendered here. 
		const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
		if (renderHUDModel)
		{
			// [BB] The HUD model should be drawn over everything else already drawn.
			state.Clear(CT_Depth);
			screen->mBones->Map();
			DrawPlayerSprites(true, state);
			screen->mBones->Unmap();
		}
	}

	state.EnableStencil(false);
	state.SetViewport(screen->mScreenViewport.left, screen->mScreenViewport.top, screen->mScreenViewport.width, screen->mScreenViewport.height);

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

void HWDrawInfo::DrawEndScene2D(sector_t * viewsector, FRenderState &state)
{
	const bool renderHUDModel = IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player);
	auto vrmode = VRMode::GetVRModeCached(true);

	HWViewpointUniforms vp = VPUniforms;
	vp.mViewMatrix.loadIdentity();
	vp.mProjectionMatrix = vrmode->GetHUDProjection();
	screen->mViewpoints->SetViewpoint(state, &vp);
	state.EnableDepthTest(false);
	state.EnableMultisampling(false);

	if (!vrmode->RenderPlayerSpritesInScene())
	{
		// [BB] Only draw the sprites if we didn't render a HUD model before.
		if ( renderHUDModel == false )
		{
			DrawPlayerSprites(false, state);
		}
	}

	state.SetNoSoftLightLevel();

	// Restore standard rendering state
	state.SetRenderStyle(STYLE_Translucent);
	state.ResetColor();
	state.EnableTexture(true);
	state.SetScissor(0, 0, -1, -1);
}

//-----------------------------------------------------------------------------
//
// sets 3D viewport and initial state
//
//-----------------------------------------------------------------------------

void HWDrawInfo::Set3DViewport(FRenderState &state)
{
	// Always clear all buffers with scissor test disabled.
	// This is faster on newer hardware because it allows the GPU to skip
	// reading from slower memory where the full buffers are stored.
	state.SetScissor(0, 0, -1, -1);
	state.Clear(CT_Color | CT_Depth | CT_Stencil);

	const auto &bounds = screen->mSceneViewport;
	state.SetViewport(bounds.left, bounds.top, bounds.width, bounds.height);
	state.SetScissor(bounds.left, bounds.top, bounds.width, bounds.height);
	state.EnableMultisampling(true);
	state.EnableDepthTest(true);
	state.EnableStencil(true);
	state.SetStencil(0, SOP_Keep, SF_AllOn);
}

//-----------------------------------------------------------------------------
//
// gl_drawscene - this function renders the scene from the current
// viewpoint, including mirrors and skyboxes and other portals
// It is assumed that the HWPortal::EndFrame returns with the
// stencil, z-buffer and the projection matrix intact!
//
//-----------------------------------------------------------------------------

void HWDrawInfo::DrawScene(int drawmode)
{
	static int recursion = 0;
	static int ssao_portals_available = 0;
	auto& vp = Viewpoint;

	bool applySSAO = false;
	if (drawmode == DM_MAINVIEW)
	{
		ssao_portals_available = gl_ssao_portals;
		applySSAO = true;
		if (r_dithertransparency && vp.bDoOob)
		{
			vp.camera->tracer ? SetDitherTransFlags(vp.camera->tracer) : SetDitherTransFlags(players[consoleplayer].mo);
		}
	}
	else if (drawmode == DM_OFFSCREEN)
	{
		ssao_portals_available = 0;
	}
	else if (drawmode == DM_PORTAL && ssao_portals_available > 0)
	{
		applySSAO = (mCurrentPortal->AllowSSAO() || Level->flags3&LEVEL3_SKYBOXAO);
		ssao_portals_available--;
	}

	if (vp.camera != nullptr)
	{
		ActorRenderFlags savedflags = vp.camera->renderflags;
		CreateScene(drawmode == DM_MAINVIEW);
		vp.camera->renderflags = savedflags;
	}
	else
	{
		CreateScene(false);
	}
	auto& RenderState = *screen->RenderState();

	RenderState.SetDepthMask(true);
	if (!gl_no_skyclear) portalState.RenderFirstSkyPortal(recursion, this, RenderState);

	RenderScene(RenderState);

	auto vrmode = VRMode::GetVRModeCached(true);
	if (drawmode == DM_MAINVIEW && vrmode->RenderPlayerSpritesInScene())
	{
		DrawPlayerSprites(IsHUDModelForPlayerAvailable(players[consoleplayer].camera->player), RenderState);
	}

	if (applySSAO && RenderState.GetPassType() == GBUFFER_PASS)
	{
		screen->AmbientOccludeScene(VPUniforms.mProjectionMatrix.get()[5]);
		screen->mViewpoints->Bind(RenderState, vpIndex);
	}

	// Handle all portals after rendering the opaque objects but before
	// doing all translucent stuff
	recursion++;
	portalState.EndFrame(this, RenderState);
	recursion--;
	RenderTranslucent(RenderState);
	if (drawmode == DM_MAINVIEW)
	{
		if (vrmode->RenderPlayerSpritesInScene())
		{
			vrmode->DrawMountedHud(this, RenderState);
		}
		DrawHitscanTracers(RenderState);
		DrawLaserSightWorld(RenderState);
		VRWheel_Draw(this, RenderState);
	}
}


//-----------------------------------------------------------------------------
//
// R_RenderView - renders one view - either the screen or a camera texture
//
//-----------------------------------------------------------------------------

void HWDrawInfo::ProcessScene(bool toscreen)
{
	portalState.BeginScene();

	int mapsection = Level->PointInRenderSubsector(Viewpoint.Pos)->mapsection;
	if (Viewpoint.bDoOob || Viewpoint.bDoOrtho)
		mapsection = Level->PointInRenderSubsector(Viewpoint.OffPos)->mapsection;
	CurrentMapSections.Set(mapsection);
	screen->mBones->Map();
	DrawScene(toscreen ? DM_MAINVIEW : DM_OFFSCREEN);
	screen->mBones->Unmap();
}

//==========================================================================
//
//
//
//==========================================================================

void HWDrawInfo::AddSubsectorToPortal(FSectorPortalGroup *ptg, subsector_t *sub)
{
	auto portal = FindPortal(ptg);
	if (!portal)
	{
		portal = new HWSectorStackPortal(&portalState, ptg);
		Portals.Push(portal);
	}
	auto ptl = static_cast<HWSectorStackPortal*>(portal);
	ptl->AddSubsector(sub);
}
