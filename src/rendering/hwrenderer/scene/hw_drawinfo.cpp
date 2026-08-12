// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2018 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_drawinfo.cpp
** Basic scene draw info management class
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
	}
	else
	{
		VPUniforms.mProjectionMatrix.loadIdentity();
		VPUniforms.mViewMatrix.loadIdentity();
		VPUniforms.mNormalViewMatrix.loadIdentity();
		ProjectionMatrix2.loadIdentity();
		VPUniforms.mViewHeight = viewheight;
		if (lightmode == ELightMode::Build)
		{
			VPUniforms.mGlobVis = 1 / 64.f;
			VPUniforms.mPalLightLevels = 32 | (static_cast<int>(gl_fogmode) << 8) | ((int)lightmode << 16);
		}
		else
		{
			VPUniforms.mGlobVis = (float)R_GetGlobVis(r_viewwindow, r_visibility) / 32.f;
			VPUniforms.mPalLightLevels = static_cast<int>(gl_bandedswlight) | (static_cast<int>(gl_fogmode) << 8) | ((int)lightmode << 16);
		}
		VPUniforms.mClipLine.X = -10000000.0f;
		VPUniforms.mShadowmapFilter = gl_shadowmap_filter;
		VPUniforms.mLightBlendMode = (level.info ? (int)level.info->lightblendmode : 0);
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
			for (int i = 0; i < nb; i++)
			{
				VPUniforms.mBeamA[i] = {
					(float)Level->BeamStart[i].X, (float)Level->BeamStart[i].Z,
					(float)Level->BeamStart[i].Y, (float)Level->BeamThick[i] };
				VPUniforms.mBeamB[i] = {
					(float)Level->BeamEnd[i].X, (float)Level->BeamEnd[i].Z,
					(float)Level->BeamEnd[i].Y, (float)Level->BeamSoft[i] };
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
			VPUniforms.mTornado = { (float)Level->TornadoPos.X,
				(float)Level->TornadoPos.Y, (float)Level->TornadoBase,
				(float)Level->TornadoTop };
			VPUniforms.mTornado2 = { (float)Level->TornadoRadBase,
				(float)Level->TornadoRadTop, (float)Level->TornadoDensity,
				(float)Level->TornadoSwirl };
			VPUniforms.mTornado3 = { (float)Level->TornadoSpin,
				(float)Level->TornadoTwist, (float)Level->TornadoLean,
				(float)Level->TornadoLeanPeriod };
			VPUniforms.mFogSurf = {
				(float)Level->FogSurfAmp, (float)Level->FogSurfLen,
				(float)Level->FogSurfSpeed, (float)Level->FogSurfCross };

			// The torch cone in WORLD space, so the mist can be lit by it.
			// The volumetric beam pass gets its own copy in VIEW space and
			// cannot share -- see hw_viewpointuniforms.h.
			if (Level->VolBeamActive)
			{
				VPUniforms.mFogBeamPos = {
					(float)Level->VolBeamPos.X, (float)Level->VolBeamPos.Z,
					(float)Level->VolBeamPos.Y, (float)Level->VolBeamLength };
				VPUniforms.mFogBeamDir = {
					(float)Level->VolBeamDir.X, (float)Level->VolBeamDir.Z,
					(float)Level->VolBeamDir.Y,
					(float)cos(Level->VolBeamInner * M_PI / 180.0) };
				VPUniforms.mFogBeamCol = {
					Level->VolBeamColor.r / 255.f, Level->VolBeamColor.g / 255.f,
					Level->VolBeamColor.b / 255.f,
					(float)cos(Level->VolBeamOuter * M_PI / 180.0) };
			}
			else
			{
				VPUniforms.mFogBeamPos = { 0.f, 0.f, 0.f, 0.f };
			}
		}
		else
		{
			VPUniforms.mFogSlab = { 0.f, 0.f, 24.f, 0.f };
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
	if (Viewpoint.IsAllowedOoB() || Viewpoint.IsOrtho())
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
	if (Viewpoint.IsAllowedOoB())
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
			cm = CM_FIRSTSPECIALCOLORMAP + REALINVERSECOLORMAP;
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
		else if (cplayer->fixedlightlevel != -1)
		{
			auto torchtype = PClass::FindActor(NAME_PowerTorch);
			auto litetype = PClass::FindActor(NAME_PowerLightAmp);
			for (AActor *in = cplayer->mo->Inventory; in; in = in->Inventory)
			{
				// Need special handling for light amplifiers 
				if (in->IsKindOf(torchtype))
				{
					FullbrightFlags = Fullbright;
					if (gl_enhanced_nv_stealth > 1) FullbrightFlags |= StealthVision;
				}
				else if (in->IsKindOf(litetype))
				{
					FullbrightFlags = Fullbright;
					if (gl_enhanced_nightvision) FullbrightFlags |= Nightvision;
					if (gl_enhanced_nv_stealth > 0) FullbrightFlags |= StealthVision;
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
	if (Viewpoint.IsAllowedOoB())
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
	if (Level == nullptr || !Level->VolBeamActive)
	{
		hw_postprocess.volbeam.ClearBeam();
		return;
	}

	auto worldToView = [this](const DVector3 &w, bool isDirection) -> FVector3
	{
		float pt[4] = { (float)w.X, (float)w.Z, (float)w.Y, isDirection ? 0.0f : 1.0f };
		float out[4];
		VPUniforms.mViewMatrix.multMatrixPoint(pt, out);
		return FVector3(out[0], out[1], out[2]);
	};

	VolumetricBeamUniforms u = {};
	u.BeamPos = worldToView(Level->VolBeamPos, false);

	FVector3 dir = worldToView(Level->VolBeamDir, true);
	float dl = dir.Length();
	u.BeamDir = (dl > 0.0001f) ? dir / dl : FVector3(0, 0, -1);

	u.BeamColor = FVector3(Level->VolBeamColor.r / 255.f,
		Level->VolBeamColor.g / 255.f,
		Level->VolBeamColor.b / 255.f);

	// Half-angles arrive in degrees; the shader compares cosines, so convert
	// once here rather than per pixel.
	u.CosInner = (float)cos(Level->VolBeamInner * M_PI / 180.0);
	u.CosOuter = (float)cos(Level->VolBeamOuter * M_PI / 180.0);
	u.BeamLength = (float)Level->VolBeamLength;
	u.Density = (float)Level->VolBeamDensity;
	u.Falloff = (float)Level->VolBeamFalloff;

	// Rebuilding the pixel ray in the shader needs the view frustum's shape,
	// which is exactly what the projection matrix's first two diagonals hold.
	const float *proj = VPUniforms.mProjectionMatrix.get();
	float px = (proj[0] != 0.0f) ? 1.0f / proj[0] : 1.0f;
	float py = (proj[5] != 0.0f) ? 1.0f / proj[5] : 1.0f;
	u.TanHalfFov = FVector2(px, py);

	u.StepCount = clamp((int)vol_beam_quality, 8, 64);

	// Dust is sampled in world space, so the shader needs a way back out of
	// view space. Without this the motes would ride along with the camera.
	u.DustAmount = (float)Level->VolBeamDust;
	u.DustScale = (float)Level->VolBeamDustScale;
	u.DustDrift = (float)Level->VolBeamDustDrift;
	u.DustTime = (float)(screen->FrameTime * 0.001);

	VSMatrix inv;
	if (!VPUniforms.mViewMatrix.inverseMatrix(inv)) inv.loadIdentity();
	memcpy(u.ViewToWorld, inv.get(), sizeof(float) * 16);

	hw_postprocess.volbeam.SetBeam(u);
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
	if (Viewpoint.IsAllowedOoB()) // No need for vertical clipper if viewpoint not allowed out of bounds
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
		if (Viewpoint.IsOrtho())
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
		if (r_dithertransparency && vp.IsAllowedOoB())
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
	if (Viewpoint.IsAllowedOoB() || Viewpoint.IsOrtho())
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

