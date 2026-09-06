/*
** hw_flats.cpp
**
** Flat processing
**
**---------------------------------------------------------------------------
**
** Copyright 2000-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include "a_sharedglobal.h"
#include "a_dynlight.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "actorinlines.h"
#include "p_lnspec.h"
#include "matrix.h"
#include "hw_dynlightdata.h"
#include "hw_cvars.h"

struct FFlatLightCandidate
{
	FDynamicLight *Light;
	float Score;
};

static thread_local TArray<FFlatLightCandidate> flatLightCandidates;
#include "hw_clock.h"
#include "hw_lighting.h"
#include "hw_material.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_drawstructs.h"
#include "hw_renderstate.h"
#include "texturemanager.h"
#include "hw_viewpointbuffer.h"
#include "m_round.h"

CVAR(Int, gl_max_vertices, 0, CVAR_ARCHIVE)

extern int flatVerticesPerEye;
extern int lightsFlatPerEye;

#ifdef _DEBUG
CVAR(Int, gl_breaksec, -1, 0)
#endif
//==========================================================================
//
// Sets the texture matrix according to the plane's texture positioning
// information
//
//==========================================================================

bool hw_SetPlaneTextureRotation(const HWSectorPlane * secplane, FGameTexture * gltexture, VSMatrix &dest)
{
	// only manipulate the texture matrix if needed.
	if (!secplane->Offs.isZero() ||
		secplane->Scale.X != 1. || secplane->Scale.Y != 1 ||
		secplane->Angle != 0 ||
		gltexture->GetDisplayWidth() != 64 ||
		gltexture->GetDisplayHeight() != 64)
	{
		float uoffs = secplane->Offs.X / gltexture->GetDisplayWidth();
		float voffs = secplane->Offs.Y / gltexture->GetDisplayHeight();

		float xscale1 = secplane->Scale.X;
		float yscale1 = secplane->Scale.Y;
		if (gltexture->isHardwareCanvas())
		{
			yscale1 = 0 - yscale1;
		}
		float angle = -secplane->Angle;

		float xscale2 = 64.f / gltexture->GetDisplayWidth();
		float yscale2 = 64.f / gltexture->GetDisplayHeight();

		dest.loadIdentity();
		dest.scale(xscale1, yscale1, 1.0f);
		dest.translate(uoffs, voffs, 0.0f);
		dest.scale(xscale2, yscale2, 1.0f);
		dest.rotate(angle, 0.0f, 0.0f, 1.0f);
		return true;
	}
	return false;
}

void SetPlaneTextureRotation(FRenderState &state, HWSectorPlane* plane, FGameTexture* texture)
{
	if (hw_SetPlaneTextureRotation(plane, texture, state.mTextureMatrix))
	{
		state.EnableTextureMatrix(true);
	}
}



//==========================================================================
//
// special handling for skyboxes which need texture clamping.
// This will find the bounding rectangle of the sector and just
// draw one single polygon filling that rectangle with a clamped
// texture.
//
//==========================================================================

void HWFlat::CreateSkyboxVertices(FFlatVertex *vert)
{
	float minx = FLT_MAX, miny = FLT_MAX;
	float maxx = -FLT_MAX, maxy = -FLT_MAX;

	for (auto ln : sector->Lines)
	{
		float x = ln->v1->fX();
		float y = ln->v1->fY();
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
		x = ln->v2->fX();
		y = ln->v2->fY();
		if (x < minx) minx = x;
		if (y < miny) miny = y;
		if (x > maxx) maxx = x;
		if (y > maxy) maxy = y;
	}

	static float uvals[] = { 0, 0, 1, 1 };
	static float vvals[] = { 1, 0, 0, 1 };
	int rot = -RoundDown(plane.Angle / 90.f);

	vert[0].Set(minx, z, miny, uvals[rot & 3], vvals[rot & 3]);
	vert[1].Set(minx, z, maxy, uvals[(rot + 1) & 3], vvals[(rot + 1) & 3]);
	vert[2].Set(maxx, z, miny, uvals[(rot + 3) & 3], vvals[(rot + 3) & 3]);
	vert[3].Set(maxx, z, maxy, uvals[(rot + 2) & 3], vvals[(rot + 2) & 3]);
}

//==========================================================================
//
//
//
//==========================================================================

void HWFlat::SetupLights(HWDrawInfo *di, FDynLightData &lightdata, int portalgroup)
{
	Plane p;

	lightdata.Clear();
	if (renderstyle == STYLE_Add && !di->Level->lightadditivesurfaces)
	{
		dynlightindex = -1;
		return;	// no lights on additively blended surfaces.
	}

	// [UZDXREMA] Dynamic-light budget for flats. Upstream's per-section TMap of
	// FLightNode replaces the old intrusive ->lighthead chain, but the fork's
	// render limit / candidate budget still applies on top of it.
	if (section == nullptr || di->Level->lightlists.flat_dlist.SSize() <= section->Index())
	{
		dynlightindex = -1;
		return;
	}

	const int renderLimit = gl_light_flat_max_lights;
	const int candidateBudget = gl_light_flat_candidate_budget;

	auto &dlist = di->Level->lightlists.flat_dlist[section->Index()];

	if (candidateBudget > 0)
	{
		auto &candidates = flatLightCandidates;
		candidates.Clear();

		TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Iterator it(dlist);
		TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Pair *pair;
		while (it.NextPair(pair))
		{
			auto node = pair->Value.get();
			if (!node) continue;

			FDynamicLight * light = node->lightsource;

			if (!light->IsActive() || light->DontLightMap() || gl_IsDistanceCulled(light))
			{
				if (light->IsActive() && !light->DontLightMap() && gl_IsDistanceCulled(light)) dynlights_distance_culled_flats++;
				continue;
			}
			iter_dlightf++;

			// we must do the side check here because gl_GetLight needs the correct plane orientation
			// which we don't have for Legacy-style 3D-floors
			double planeh = plane.plane.ZatPoint(light->Pos);
			if ((planeh<light->Z() && ceiling) || (planeh>light->Z() && !ceiling))
			{
				continue;
			}

			p.Set(plane.plane.Normal(), plane.plane.fD());
			DVector3 posrel = gl_GetLightPosRelative(light, portalgroup);
			float radius = light->GetRadius();
			float dist = fabsf(p.DistToPoint((float)posrel.X, (float)posrel.Z, (float)posrel.Y));
			if (radius > 0.f && dist <= radius)
			{
				gl_InsertBestLightCandidate(candidates, { light, dist / radius }, candidateBudget);
			}
		}

		for (unsigned int c = 0; c < candidates.Size() && (!renderLimit || lightsFlatPerEye < renderLimit); ++c)
		{
			lightsFlatPerEye++;
			draw_dlightf += 1;
			AddLightToList(lightdata, portalgroup, candidates[c].Light, false);
		}
	}
	else
	{
		TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Iterator it(dlist);
		TMap<FDynamicLight *, std::unique_ptr<FLightNode>>::Pair *pair;
		while (it.NextPair(pair))
		{
			if (renderLimit && lightsFlatPerEye >= renderLimit)
				break;

			auto node = pair->Value.get();
			if (!node) continue;

			FDynamicLight * light = node->lightsource;

			if (!light->IsActive() || light->DontLightMap() || gl_IsDistanceCulled(light))
			{
				if (light->IsActive() && !light->DontLightMap() && gl_IsDistanceCulled(light)) dynlights_distance_culled_flats++;
				continue;
			}
			iter_dlightf++;

			// we must do the side check here because gl_GetLight needs the correct plane orientation
			// which we don't have for Legacy-style 3D-floors
			double planeh = plane.plane.ZatPoint(light->Pos);
			if ((planeh<light->Z() && ceiling) || (planeh>light->Z() && !ceiling))
			{
				continue;
			}

			p.Set(plane.plane.Normal(), plane.plane.fD());
			DVector3 posrel = gl_GetLightPosRelative(light, portalgroup);
			float radius = light->GetRadius();
			float dist = fabsf(p.DistToPoint((float)posrel.X, (float)posrel.Z, (float)posrel.Y));
			if (radius > 0.f && dist <= radius)
			{
				lightsFlatPerEye++;
				draw_dlightf += 1;
				AddLightToList(lightdata, portalgroup, light, false);
			}
		}
	}

	dynlightindex = screen->mLights->UploadLights(lightdata);
}

//==========================================================================
//
//
//
//==========================================================================

void HWFlat::DrawSubsectors(HWDrawInfo *di, FRenderState &state)
{
	auto vcount = sector->ibocount;
	if (gl_max_vertices > 0 && flatVerticesPerEye + vcount >= gl_max_vertices)
	{
		return;
	}

	if (di->Level->HasDynamicLights && screen->BuffersArePersistent() && !di->isFullbrightScene())
	{
		SetupLights(di, lightdata, sector->PortalGroup);
	}
	state.SetLightIndex(dynlightindex);

	state.DrawIndexed(DT_Triangles, iboindex + section->vertexindex, section->vertexcount);
	flatVerticesPerEye += section->vertexcount;
	flatvertices += section->vertexcount;
	flatprimitives++;
}


//==========================================================================
//
// Drawer for render hacks
//
//==========================================================================

void HWFlat::DrawOtherPlanes(HWDrawInfo *di, FRenderState &state)
{
	state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);

	// Draw the subsectors assigned to it due to missing textures
	auto pNode = (renderflags&SSRF_RENDERFLOOR) ?
		di->otherFloorPlanes.CheckKey(sector->sectornum) : di->otherCeilingPlanes.CheckKey(sector->sectornum);

	if (!pNode) return;
	auto node = *pNode;

	while (node)
	{
		state.SetLightIndex(node->lightindex);
		auto num = node->sub->numlines;
		flatVerticesPerEye += num;
		flatvertices += num;
		flatprimitives++;
		state.Draw(DT_TriangleFan,node->vertexindex, num);
		node = node->next;
	}
}

//==========================================================================
//
// Drawer for render hacks
//
//==========================================================================

void HWFlat::DrawFloodPlanes(HWDrawInfo *di, FRenderState &state)
{
	// Flood gaps with the back side's ceiling/floor texture
	// This requires a stencil because the projected plane interferes with
	// the depth buffer

	state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);

	// Draw the subsectors assigned to it due to missing textures
	auto pNode = (renderflags&SSRF_RENDERFLOOR) ?
		di->floodFloorSegs.CheckKey(sector->sectornum) : di->floodCeilingSegs.CheckKey(sector->sectornum);
	if (!pNode) return;

	auto fnode = *pNode;

	state.SetLightIndex(-1);
	while (fnode)
	{
		flatVerticesPerEye += 12;
		flatvertices += 12;
		flatprimitives += 3;

		// Push bleeding floor/ceiling textures back a little in the z-buffer
		// so they don't interfere with overlapping mid textures.
		state.SetDepthBias(1, 128);

		// Create stencil
		state.SetEffect(EFF_STENCIL);
		state.EnableTexture(false);
		state.SetStencil(0, SOP_Increment, SF_ColorMaskOff);
		state.Draw(DT_TriangleStrip, fnode->vertexindex, 4);

		// Draw projected plane into stencil
		state.EnableTexture(true);
		state.SetEffect(EFF_NONE);
		state.SetStencil(1, SOP_Keep, SF_DepthMaskOff);
		state.EnableDepthTest(false);
		state.Draw(DT_TriangleStrip, fnode->vertexindex + 4, 4);

		// clear stencil
		state.SetEffect(EFF_STENCIL);
		state.EnableTexture(false);
		state.SetStencil(1, SOP_Decrement, SF_ColorMaskOff | SF_DepthMaskOff);
		state.Draw(DT_TriangleStrip, fnode->vertexindex, 4);

		// restore old stencil op.
		state.EnableTexture(true);
		state.EnableDepthTest(true);
		state.SetEffect(EFF_NONE);
		state.SetDepthBias(0, 0);
		state.SetStencil(0, SOP_Keep, SF_AllOn);

		fnode = fnode->next;
	}

}


//==========================================================================
//
//
//
//==========================================================================
//==========================================================================
//
// [BB] THE FLAT GLOW AT ONE WORLD POINT, ON THE CPU.
//
// main.fp computes flat glow per fragment from pixelpos.xz -- the fragment's
// own world position. Anything drawn in VIEW space has no world position to
// give it: the weapon and the VR hands sit in front of the camera, not in the
// room, so they can never take the shader path however the render state is
// set. What they picked up before was the last flat's uniforms applied to
// view-space coordinates, which is why the gun glowed in some rooms and some
// facings and not others.
//
// This runs the same arithmetic once, for one point, so something drawn in
// view space can still be lit by the room it is standing in. Floor and ceiling
// are summed: a room lit from both should light what stands in it from both.
//
// NOT A WEAPON FEATURE. It takes a sector and a point and nothing else, so
// anything else drawn outside world space can ask the same question.
//
// The glow WAVE is not applied here. The shader modulates reach and brightness
// with it, so a room that breathes will breathe while this stays steady. Worth
// adding, and deliberately not guessed at.
//
//==========================================================================

// The room's PULSE, evaluated at a point. Mirrors GlowWaveRaw in main.fp --
// same distance functions, same detune, same sharpness, same per-room scatter
// off the sector's first vertex -- so a thing standing in a breathing room
// breathes WITH it rather than beside it. Returns -1..1, as the shader does.
//
// Without this the surfaces of a room pulsed and everything standing in them
// held perfectly steady, which reads as the actors not being part of the scene.
static double GlowWaveAtPoint(FLevelLocals *Level, sector_t *sector,
	const DVector3 &at, double timeSec, bool ceiling)
{
	if (Level == nullptr || Level->GlowWaveLength <= 0.0) return 0.0;

	// Shader space is (x, z, y) -- see the upload in hw_drawinfo.cpp -- so the
	// shader's xz plane is the game's xy, and its y is the game's z.
	const DVector3 &wo = Level->GlowWaveOrigin;
	double d;
	switch (Level->GlowWaveShape)
	{
	case 2:  d = fabs(at.X - wo.X); break;
	case 3:  d = fabs(at.Y - wo.Y); break;
	case 4:  d = (at - wo).Length(); break;
	case 5:  d = at.Z - wo.Z; break;
	default: d = (at.XY() - wo.XY()).Length(); break;
	}

	double seedOff = 0.0;
	if (Level->GlowWaveSeed > 0.0 && sector != nullptr && sector->Lines.Size() > 0)
	{
		const double src = sector->Lines[0]->v1->fX() + sector->Lines[0]->v1->fY();
		seedOff = (sin(src * 12.9898) * 43758.5453);
		seedOff = (seedOff - floor(seedOff)) * 6.2831853 * Level->GlowWaveSeed;
	}

	const double phase = Level->GlowWavePhase[ceiling ? 3 : 2];
	const double t = d / Level->GlowWaveLength + timeSec * Level->GlowWaveSpeed + phase + seedOff;
	double w = 0.5 + 0.5 * sin(t);

	if (Level->GlowWaveDetune > 0.0)
	{
		const double w2 = 0.5 + 0.5 * sin(t * 0.6180339887 + 1.7);
		w = w + (w * w2 * 2.0 - w) * Level->GlowWaveDetune;
	}
	w = pow(clamp(w, 0.0, 1.0), max(Level->GlowWaveSharp, 0.001));
	return 2.0 * w - 1.0;
}

//==========================================================================
//
// [BB] THE ROOM'S COLOUR ON A THING STANDING IN IT.
//
// Folding the glow in as a pure ADD was wrong, and wrong in a specific way: an
// add raises every channel, so a bright room pushed the sprite toward white and
// the gun went fullbright instead of going RED. Adding light is not how a
// coloured light looks on a surface.
//
// A coloured light does two things. It adds its own colour, and it takes away
// the channels it does not have -- a red lamp on a grey wall makes the wall red
// by suppressing green and blue, not by adding red until the wall is pink. So
// this splits the glow into a TINT and a much smaller ADD.
//
// The tint multiplies, which is what keeps the sprite's own shading and its own
// colours: a green imp under a red lamp goes dark and muddy, which is correct,
// where an add would have made it pale.
//
// The add is what stops a strongly tinted thing reading as merely dark. It is
// deliberately a third of the strength, and it carries the same hue, so it
// brightens toward the light's colour rather than toward white.
//
// Takes the raw glow, returns the multiply, and leaves the additive part in
// `addOut` as 0-255 channels ready to fold into an existing PalEntry.
//
//==========================================================================

void SplitRoomGlow(const FVector3 &glow, FVector3 &tintOut, FVector3 &addOut)
{
	tintOut = { 1.f, 1.f, 1.f };
	addOut = { 0.f, 0.f, 0.f };

	const float m = max(glow.X, max(glow.Y, glow.Z));
	if (m <= 0.f) return;

	// The chroma on its own, so the amount and the colour are separable.
	const FVector3 hue = glow / m;
	const float amt = min(m, 1.f);

	// Multiply toward the light's colour. At amt 1 the surface keeps only what
	// the light actually emits, which is what makes it read as coloured rather
	// than as brightened.
	tintOut = { 1.f - amt + amt * hue.X,
	            1.f - amt + amt * hue.Y,
	            1.f - amt + amt * hue.Z };

	// A third, and in the light's own hue.
	addOut = hue * (amt * 0.33f * 255.f);
}

FVector3 FlatGlowAtPoint(sector_t *sector, const DVector3 &at, FLevelLocals *Level, double timeSec)
{
	FVector3 out(0.f, 0.f, 0.f);
	if (sector == nullptr) return out;

	const int count = min<int>((int)sector->Lines.Size(), 64);
	if (count <= 0) return out;

	for (int pass = 0; pass < 2; pass++)
	{
		auto &sp = sector->planes[pass == 0 ? sector_t::floor : sector_t::ceiling];
		if (sp.FlatGlowColor.a == 0 || sp.FlatGlowHeight <= 0.f) continue;

		// The wave rides reach and brightness, the same two the shader gives it.
		const double wv = GlowWaveAtPoint(Level, sector, at, timeSec, pass != 0);
		const float reach = (float)(sp.FlatGlowHeight
			* (1.0 + (Level ? Level->GlowWaveReach : 0.0) * wv));
		if (reach <= 0.f) continue;

		// Squared throughout with one root at the end -- the same trick the
		// shader uses, and for the same reason.
		double bestSq = 1e30;
		for (int i = 0; i < count; i++)
		{
			auto ln = sector->Lines[i];
			const DVector2 a(ln->v1->fX(), ln->v1->fY());
			const DVector2 b(ln->v2->fX(), ln->v2->fY());
			const DVector2 ab = b - a;
			const double len2 = ab.LengthSquared();
			double t = 0.0;
			if (len2 > 0.0) t = clamp(((at.XY() - a) | ab) / len2, 0.0, 1.0);
			const DVector2 d = at.XY() - (a + ab * t);
			const double dsq = d.LengthSquared();
			if (dsq < bestSq) bestSq = dsq;
		}

		const float minDist = (float)sqrt(bestSq);
		if (minDist >= reach) continue;

		const float frac = minDist / reach;
		float atten;
		switch (sp.FlatGlowFalloff)
		{
		case 0:  atten = 1.f - frac;              break;
		case 1:  atten = 1.f - frac * frac;       break;
		case 2:  atten = 1.f - (float)sqrt(frac); break;
		default: atten = (float)exp(-frac * 3.f); break;
		}
		if (atten <= 0.f) continue;

		// HEIGHT FALLOFF. Flat glow is a 2D distance field -- distance to the
		// sector's edges and nothing else -- so without this an imp is lit as
		// brightly at the horns as at the hooves, and the room reads as evenly
		// flooded rather than as light coming off the floor.
		//
		// Fades over the same reach the glow already spreads inward by, so a
		// floor that pools light 128 units in from its edges also pools it 128
		// units up. No new knob to set, and it scales with the effect.
		//
		// SURFACES ARE UNAFFECTED. Flats are drawn by the shader, which never
		// calls this -- a floor sits at its own height and would fade by zero
		// anyway. This changes what things STANDING in the room receive.
		const double planeZ = (pass == 0)
			? sector->floorplane.ZatPoint(at.XY())
			: sector->ceilingplane.ZatPoint(at.XY());
		const double dz = fabs(at.Z - planeZ);
		if (dz >= reach) continue;
		atten *= float(1.0 - dz / reach);
		if (atten <= 0.f) continue;

		const float inten = sp.FlatGlowIntensity > 0.f ? sp.FlatGlowIntensity : 1.f;
		FVector3 col(sp.FlatGlowColor.r / 255.f * inten,
		             sp.FlatGlowColor.g / 255.f * inten,
		             sp.FlatGlowColor.b / 255.f * inten);

		// The far colour, same ramp along the reach as the shader.
		const PalEntry farCol = sp.FlatGlowColorFar;
		if (farCol.a > 0)
		{
			FVector3 fc(farCol.r / 255.f * inten, farCol.g / 255.f * inten, farCol.b / 255.f * inten);
			col = fc + (col - fc) * atten;
		}

		const float bright = (float)(1.0 + (Level ? Level->GlowWaveBright : 0.0) * wv);
		out += col * atten * max(bright, 0.f);
	}
	return out;
}

float FogScaleForSector(FLevelLocals *Level, sector_t *sec);

void HWFlat::DrawFlat(HWDrawInfo *di, FRenderState &state, bool translucent)
{
	state.SetFogDensityScale(FogScaleForSector(di->Level, sector));
#ifdef _DEBUG
	if (sector->sectornum == gl_breaksec)
	{
		int a = 0;
	}
#endif

	int rel = getExtraLight();

	// [BB] The sector's own floor and ceiling, for the fog slab.
	//
	// Flats never set these -- glow was the only reader and a flat does not
	// glow from its own edge -- so a floor fragment inherited whatever plane
	// the last WALL happened to leave behind. Harmless while nothing read it
	// on a flat; wrong the moment the fog surface started sitting a fixed
	// height above the floor, because looking down at the mist is exactly
	// where a stale plane shows.
	{
		auto tp = sector->ceilingplane;
		auto bp = sector->floorplane;
		state.SetGlowPlanes(
			FVector4(tp.Normal().X, tp.Normal().Y, tp.negiC, tp.fD()),
			FVector4(bp.Normal().X, bp.Normal().Y, bp.negiC, bp.fD()));
	}

	state.SetNormal(plane.plane.Normal().X, plane.plane.Normal().Z, plane.plane.Normal().Y);
	double zshift = (plane.plane.Normal().Z > 0.0 ? 0.01f : -0.01f); // The HWPlaneMirrorPortal::DrawPortalStencil() z-fights with flats

	SetColor(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), Colormap, alpha);
	SetFog(state, di->Level, di->lightmode, lightlevel, rel, di->isFullbrightScene(), &Colormap, false);
	state.SetObjectColor(FlatColor | 0xff000000);
	state.SetAddColor(AddColor | 0xff000000);
	state.ApplyTextureManipulation(TextureFx);
	if (plane.plane.dithertransflag) state.SetEffect(EFF_DITHERTRANS);

	// [BB] Flat-edge glow: this plane's own surface, glowing inward from its
	// linedef edges. Separate from the standard wall-bleed glow above --
	// that one never reaches the flat itself.
	{
		int planeIdx = ceiling ? sector_t::ceiling : sector_t::floor;
		auto &sp = sector->planes[planeIdx];
		if (sp.FlatGlowColor.a > 0 && sp.FlatGlowHeight > 0.f)
		{
			// Intensity scales the COLOUR, not the reach. It used to be folded
			// into reach here while the wall glow's identically named slider
			// multiplied colour, so the same control did two different jobs
			// depending on which lane it sat on. Values above 1 are legal and
			// feed bloom exactly as the wall glow's already do; reach is the
			// coverage slider's alone now.
			float inten = sp.FlatGlowIntensity > 0.f ? sp.FlatGlowIntensity : 1.f;
			float r = sp.FlatGlowColor.r / 255.f * inten;
			float g = sp.FlatGlowColor.g / 255.f * inten;
			float b = sp.FlatGlowColor.b / 255.f * inten;
			float reach = sp.FlatGlowHeight;

			// The far colour rides the same intensity, or the ramp would
			// change brightness along its length.
			PalEntry farCol = sp.FlatGlowColorFar;
			FVector4 farColor = farCol.a > 0
				? FVector4(farCol.r / 255.f * inten, farCol.g / 255.f * inten, farCol.b / 255.f * inten, 1.0f)
				: FVector4(0.f, 0.f, 0.f, 0.f);

			int count = (int)sector->Lines.Size();
			if (count > 64) count = 64;
			FVector4 lines[64];
			for (int i = 0; i < count; i++)
			{
				auto ln = sector->Lines[i];
				lines[i] = { (float)ln->v1->fX(), (float)ln->v1->fY(),
				             (float)ln->v2->fX(), (float)ln->v2->fY() };
			}
			state.SetFlatGlowParams(r, g, b, reach, farColor, sp.FlatGlowFalloff, count, lines, ceiling ? 1 : 0);
		}
		else
		{
			state.ClearFlatGlow();
		}
	}

	if (hacktype & SSRF_PLANEHACK)
	{
		DrawOtherPlanes(di, state);
	}
	else if (hacktype & SSRF_FLOODHACK)
	{
		DrawFloodPlanes(di, state);
	}
	else if (!translucent)
	{
		if (sector->special != GLSector_Skybox)
		{
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);
			SetPlaneTextureRotation(state, &plane, texture);
			DrawSubsectors(di, state);
			state.EnableTextureMatrix(false);
		}
		else if (!hacktype)
		{
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_XY, NO_TRANSLATION, -1);
			state.SetLightIndex(dynlightindex);
			state.Draw(DT_TriangleStrip,iboindex, 4);
			flatVerticesPerEye += 4;
			flatvertices += 4;
			flatprimitives++;
		}
	}
	else
	{
		state.SetRenderStyle(renderstyle);
		if (!texture || !texture->isValid())
		{
			state.AlphaFunc(Alpha_GEqual, 0.f);
			state.EnableTexture(false);
			DrawSubsectors(di, state);
			state.EnableTexture(true);
		}
		else
		{
			if (!texture->GetTranslucency()) state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
			else state.AlphaFunc(Alpha_GEqual, 0.f);
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_NONE, NO_TRANSLATION, -1);
			SetPlaneTextureRotation(state, &plane, texture);
			di->TranslateViewpointMatrices(0.0, zshift, 0.0);
			di->ApplyViewpoint(state);
			DrawSubsectors(di, state);
			di->TranslateViewpointMatrices(0.0, -zshift, 0.0);
			di->ApplyViewpoint(state);
			state.EnableTextureMatrix(false);
		}
		state.SetRenderStyle(DefaultRenderStyle());
	}
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.ApplyTextureManipulation(nullptr);
	if (plane.plane.dithertransflag) state.SetEffect(EFF_NONE);
}

//==========================================================================
//
// HWFlat::PutFlat
//
// submit to the renderer
//
//==========================================================================

inline void HWFlat::PutFlat(HWDrawInfo *di, bool fog)
{
	if (di->isFullbrightScene())
	{
		Colormap.Clear();
	}
	else if (!screen->BuffersArePersistent())
	{
		if (di->Level->HasDynamicLights && texture != nullptr && !di->isFullbrightScene() && !(hacktype & (SSRF_PLANEHACK|SSRF_FLOODHACK)) )
		{
			SetupLights(di, lightdata, sector->PortalGroup);
		}
	}
	di->AddFlat(this, fog);
}

//==========================================================================
//
// This draws one flat
// The whichplane boolean indicates if the flat is a floor(false) or a ceiling(true)
//
//==========================================================================

void HWFlat::Process(HWDrawInfo *di, sector_t * model, int whichplane, bool fog)
{
	plane.GetFromSector(model, whichplane);
	model->ceilingplane.dithertransflag = false; // Resetting this every frame
	model->floorplane.dithertransflag = false; // Resetting this every frame
	if (whichplane != int(ceiling))
	{
		// Flip the normal if the source plane has a different orientation than what we are about to render.
		plane.plane.FlipVert();
	}

	if (!fog)
	{
		texture =  TexMan.GetGameTexture(plane.texture, true);
		if (!texture || !texture->isValid()) return;
		if (texture->isFullbright())
		{
			Colormap.MakeWhite();
			lightlevel=255;
		}
	}
	else
	{
		texture = NULL;
		lightlevel = abs(lightlevel);
	}

	z = plane.plane.ZatPoint(0.f, 0.f);
	if (sector->special == GLSector_Skybox)
	{
		auto vert = screen->mVertexData->AllocVertices(4);
		CreateSkyboxVertices(vert.first);
		iboindex = vert.second;
	}

	// For hacks this won't go into a render list.
	PutFlat(di, fog);
	rendered_flats++;
}

//==========================================================================
//
// Sets 3D floor info. Common code for all 4 cases
//
//==========================================================================

void HWFlat::SetFrom3DFloor(F3DFloor *rover, bool top, bool underside)
{
	F3DFloor::planeref & plane = top? rover->top : rover->bottom;

	// FF_FOG requires an inverted logic where to get the light from
	lightlist_t *light = P_GetPlaneLight(sector, plane.plane, underside);
	lightlevel = RescaleLightLevel(*light->p_lightlevel);

	if (rover->flags & FF_FOG)
	{
		Colormap.LightColor = light->extra_colormap.FadeColor;
		FlatColor = 0xffffffff;
		AddColor = 0;
		TextureFx = nullptr;
	}
	else
	{
		CopyFrom3DLight(Colormap, light);
		FlatColor = plane.model->SpecialColors[plane.isceiling];
		AddColor = plane.model->AdditiveColors[plane.isceiling];
		TextureFx = &plane.model->planes[plane.isceiling].TextureFx;
	}


	alpha = rover->alpha/255.0f;
	renderstyle = rover->flags&FF_ADDITIVETRANS? STYLE_Add : STYLE_Translucent;
	iboindex = plane.vindex;
}

//==========================================================================
//
// Process a sector's flats for rendering
// This function is only called once per sector.
// Subsequent subsectors are just quickly added to the ss_renderflags array
//
//==========================================================================

void HWFlat::ProcessSector(HWDrawInfo *di, sector_t * frontsector, int which)
{
	lightlist_t * light;
	FSectorPortal *port;

#ifdef _DEBUG
	if (frontsector->sectornum == gl_breaksec)
	{
		int a = 0;
	}
#endif

	// Get the real sector for this one.
	sector = &di->Level->sectors[frontsector->sectornum];
	extsector_t::xfloor &x = sector->e->XFloor;
	dynlightindex = -1;
	hacktype = (which & (SSRF_PLANEHACK|SSRF_FLOODHACK));

	uint8_t sink;
	uint8_t &srf = hacktype? sink : di->section_renderflags[di->Level->sections.SectionIndex(section)];
	auto &vp = di->Viewpoint;

	//
	//
	//
	// do floors
	//
	//
	//
	if ((which & SSRF_RENDERFLOOR) && (vp.bDoOrtho ? vp.ViewVector3D.dot(frontsector->floorplane.Normal()) < 0.0 : frontsector->floorplane.ZatPoint(vp.Pos) <= vp.Pos.Z) && (!section || !(section->flags & FSection::DONTRENDERFLOOR)))
	{
		// process the original floor first.

		srf |= SSRF_RENDERFLOOR;

		lightlevel = RescaleLightLevel(frontsector->GetFloorLight());
		Colormap = frontsector->Colormap;
		FlatColor = frontsector->SpecialColors[sector_t::floor];
		AddColor = frontsector->AdditiveColors[sector_t::floor];
		TextureFx = &frontsector->planes[sector_t::floor].TextureFx;

		port = frontsector->ValidatePortal(sector_t::floor);
		if ((stack = (port != NULL)))
		{
			/* to be redone in a less invasive manner
			if (port->mType == PORTS_STACKEDSECTORTHING)
			{
				di->AddFloorStack(sector);	// stacked sector things require visplane merging.
			}
			 */
			alpha = frontsector->GetAlpha(sector_t::floor);
		}
		else
		{
			alpha = 1.0f - frontsector->GetReflect(sector_t::floor);
		}

		if (alpha != 0.f && frontsector->GetTexture(sector_t::floor) != skyflatnum)
		{
			iboindex = frontsector->iboindex[sector_t::floor];

			ceiling = false;
			renderflags = SSRF_RENDERFLOOR;

			if (x.ffloors.Size())
			{
				light = P_GetPlaneLight(sector, &frontsector->floorplane, false);
				if ((!(sector->GetFlags(sector_t::floor)&PLANEF_ABSLIGHTING) || light->lightsource == NULL)
					&& (light->p_lightlevel != &frontsector->lightlevel))
				{
					lightlevel = RescaleLightLevel(*light->p_lightlevel);
				}

				CopyFrom3DLight(Colormap, light);
			}
			renderstyle = STYLE_Translucent;
			Process(di, frontsector, sector_t::floor, false);
		}
	}

	//
	//
	//
	// do ceilings
	//
	//
	//
	if ((which & SSRF_RENDERCEILING) && (vp.bDoOrtho ? vp.ViewVector3D.dot(frontsector->ceilingplane.Normal()) < 0.0 : frontsector->ceilingplane.ZatPoint(vp.Pos) >= vp.Pos.Z) && (!section || !(section->flags & FSection::DONTRENDERCEILING)))
	{
		// process the original ceiling first.

		srf |= SSRF_RENDERCEILING;

		lightlevel = RescaleLightLevel(frontsector->GetCeilingLight());
		Colormap = frontsector->Colormap;
		FlatColor = frontsector->SpecialColors[sector_t::ceiling];
		AddColor = frontsector->AdditiveColors[sector_t::ceiling];
		TextureFx = &frontsector->planes[sector_t::ceiling].TextureFx;
		port = frontsector->ValidatePortal(sector_t::ceiling);
		if ((stack = (port != NULL)))
		{
			/* as above for floors
			if (port->mType == PORTS_STACKEDSECTORTHING)
			{
				di->AddCeilingStack(sector);
			}
			 */
			alpha = frontsector->GetAlpha(sector_t::ceiling);
		}
		else
		{
			alpha = 1.0f - frontsector->GetReflect(sector_t::ceiling);
		}

		if (alpha != 0.f && frontsector->GetTexture(sector_t::ceiling) != skyflatnum)
		{
			iboindex = frontsector->iboindex[sector_t::ceiling];
			ceiling = true;
			renderflags = SSRF_RENDERCEILING;

			if (x.ffloors.Size())
			{
				light = P_GetPlaneLight(sector, &sector->ceilingplane, true);

				if ((!(sector->GetFlags(sector_t::ceiling)&PLANEF_ABSLIGHTING))
					&& (light->p_lightlevel != &frontsector->lightlevel))
				{
					lightlevel = RescaleLightLevel(*light->p_lightlevel);
				}
				CopyFrom3DLight(Colormap, light);
			}
			renderstyle = STYLE_Translucent;
			Process(di, frontsector, sector_t::ceiling, false);
		}
	}

	//
	//
	//
	// do 3D floors
	//
	//
	//

	stack = false;
	if ((which & SSRF_RENDER3DPLANES) && x.ffloors.Size())
	{
		renderflags = SSRF_RENDER3DPLANES;
		srf |= SSRF_RENDER3DPLANES;
		// 3d-floors must not overlap!
		double lastceilingheight = sector->CenterCeiling();	// render only in the range of the
		double lastfloorheight = sector->CenterFloor();		// current sector part (if applicable)
		F3DFloor * rover;
		int k;

		// floors are ordered now top to bottom so scanning the list for the best match
		// is no longer necessary.

		ceiling = true;
		Colormap = frontsector->Colormap;
		for (k = 0; k < (int)x.ffloors.Size(); k++)
		{
			rover = x.ffloors[k];

			if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
			{
				if (rover->flags&FF_FOG && di->isFullbrightScene()) continue;
				if (!rover->top.copied && rover->flags&(FF_INVERTPLANES | FF_BOTHPLANES))
				{
					double ff_top = rover->top.plane->ZatPoint(sector->centerspot);
					if (ff_top < lastceilingheight)
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->top.plane->Normal()) > 0.0 : vp.Pos.Z <= rover->top.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, true, !!(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;
							Process(di, rover->top.model, rover->top.isceiling, !!(rover->flags&FF_FOG));
						}
						lastceilingheight = ff_top;
					}
				}
				if (!rover->bottom.copied && !(rover->flags&FF_INVERTPLANES))
				{
					double ff_bottom = rover->bottom.plane->ZatPoint(sector->centerspot);
					if (ff_bottom < lastceilingheight)
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->bottom.plane->Normal()) > 0.0 : vp.Pos.Z <= rover->bottom.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, false, !(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;
							Process(di, rover->bottom.model, rover->bottom.isceiling, !!(rover->flags&FF_FOG));
						}
						lastceilingheight = ff_bottom;
						if (rover->alpha < 255) lastceilingheight += EQUAL_EPSILON;
					}
				}
			}
		}

		ceiling = false;
		for (k = x.ffloors.Size() - 1; k >= 0; k--)
		{
			rover = x.ffloors[k];

			if ((rover->flags&(FF_EXISTS | FF_RENDERPLANES | FF_THISINSIDE)) == (FF_EXISTS | FF_RENDERPLANES))
			{
				if (rover->flags&FF_FOG && di->isFullbrightScene()) continue;
				if (!rover->bottom.copied && rover->flags&(FF_INVERTPLANES | FF_BOTHPLANES))
				{
					double ff_bottom = rover->bottom.plane->ZatPoint(sector->centerspot);
					if (ff_bottom > lastfloorheight || (rover->flags&FF_FIX))
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->bottom.plane->Normal()) > 0.0 : vp.Pos.Z >= rover->bottom.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, false, !(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;

							if (rover->flags&FF_FIX)
							{
								lightlevel = RescaleLightLevel(rover->model->lightlevel);
								Colormap = rover->GetColormap();
							}

							Process(di, rover->bottom.model, rover->bottom.isceiling, !!(rover->flags&FF_FOG));
						}
						lastfloorheight = ff_bottom;
					}
				}
				if (!rover->top.copied && !(rover->flags&FF_INVERTPLANES))
				{
					double ff_top = rover->top.plane->ZatPoint(sector->centerspot);
					if (ff_top > lastfloorheight)
					{
						if ((vp.bDoOrtho ? vp.ViewVector3D.dot(rover->top.plane->Normal()) > 0.0 : vp.Pos.Z >= rover->top.plane->ZatPoint(vp.Pos)))
						{
							SetFrom3DFloor(rover, true, !!(rover->flags&FF_FOG));
							Colormap.FadeColor = frontsector->Colormap.FadeColor;
							Process(di, rover->top.model, rover->top.isceiling, !!(rover->flags&FF_FOG));
						}
						lastfloorheight = ff_top;
						if (rover->alpha < 255) lastfloorheight -= EQUAL_EPSILON;
					}
				}
			}
		}
	}
}
