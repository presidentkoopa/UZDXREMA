// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2000-2016 Christoph Oelckers
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
** hw_weapon.cpp
** Weapon sprite utilities
**
*/

#include "sbar.h"
#include "r_utility.h"
#include "v_video.h"
#include "doomstat.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "models.h"
#include "hw_weapon.h"
#include "hw_fakeflat.h"
#include "texturemanager.h"

#include "hw_models.h"
#include "hw_dynlightdata.h"
#include "hw_material.h"
#include "hw_lighting.h"
#include "hw_cvars.h"
#include "hw_vrwheel.h"
#include "hw_vrmodes.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "flatvertices.h"
#include "hw_lightbuffer.h"
#include "hw_renderstate.h"
#include "menu.h"
#include <algorithm>
#include <cmath>
#include "playsim/p_local.h"
#include "playsim/p_linetracedata.h"
#include "playsim/p_trace.h"

#include "vm.h"

EXTERN_CVAR(Float, transsouls)
EXTERN_CVAR(Int, gl_fuzztype)
EXTERN_CVAR(Bool, r_drawplayersprites)
EXTERN_CVAR(Bool, r_deathcamera)
EXTERN_CVAR(Bool, vr_laser_sight)
EXTERN_CVAR(Bool, vr_laser_show_melee)
EXTERN_CVAR(Bool, vr_laser_hide_on_wheel)
EXTERN_CVAR(Bool, vr_laser_beam)
EXTERN_CVAR(Color, vr_laser_color)
EXTERN_CVAR(Float, vr_laser_beam_alpha)
EXTERN_CVAR(Float, vr_laser_beam_width)
EXTERN_CVAR(Float, vr_laser_pointer_scale)
EXTERN_CVAR(Float, vr_laser_pointer_alpha)
EXTERN_CVAR(Int, vr_laser_pointer_glow)
EXTERN_CVAR(Float, vr_laser_pointer_glow_scale)
EXTERN_CVAR(Float, vr_laser_pointer_glow_intensity)
EXTERN_CVAR(Int, vr_laser_beam_length)
EXTERN_CVAR(Int, vr_laser_fixed_length)
EXTERN_CVAR(Float, vr_laser_source_offset_x)
EXTERN_CVAR(Float, vr_laser_source_offset_y)
EXTERN_CVAR(Float, vr_laser_source_offset_z)

extern float hmdorientation[3];
extern float weaponangles[3];
extern float offhandangles[3];
extern float doomYaw;
//To force translucency for weapon sprites, tex->GetTranslucency returns false result for 32 bit PNG
CVAR(Bool, r_transparentPlayerSprites, true, CVAR_ARCHIVE)

EXTERN_CVAR(Int, r_PlayerSprites3DMode)
EXTERN_CVAR(Float, gl_fatItemWidth)

enum PlayerSprites3DMode
{
	CROSSED,
	BACK_ONLY,
	ITEM_ONLY,
	FAT_ITEM,
};

static bool WeaponSpriteMatches(AActor* equippedWeapon, AActor* spriteCaller);


//==========================================================================
//
// R_DrawPSprite
//
//==========================================================================

void HWDrawInfo::DrawPSprite(HUDSprite *huds, FRenderState &state)
{
	if (huds->RenderStyle.BlendOp == STYLEOP_Shadow)
	{
		state.SetColor(0.2f, 0.2f, 0.2f, 0.33f, huds->cm.Desaturation);
	}
	else
	{
		SetColor(state, Level, lightmode, huds->lightlevel, 0, isFullbrightScene(), huds->cm, huds->alpha, true);
	}
	state.SetLightIndex(-1);
	state.SetRenderStyle(huds->RenderStyle);
	state.SetTextureMode(huds->RenderStyle);
	state.SetObjectColor(huds->ObjectColor);
	if (huds->owner->Sector)
	{
		state.SetAddColor(huds->owner->Sector->AdditiveColors[sector_t::sprites] | 0xff000000);
	}
	else
	{
		state.SetAddColor(0);
	}
	state.SetDynLight(huds->dynrgb[0], huds->dynrgb[1], huds->dynrgb[2]);
	state.EnableBrightmap(!(huds->RenderStyle.Flags & STYLEF_ColorIsFixed));

	if (huds->mframe)
	{
		state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);

		FHWModelRenderer renderer(this, state, huds->lightindex);
		RenderHUDModel(&renderer, huds->weapon, huds->translation, huds->rotation + FVector3(huds->mx / 4., (huds->my - WEAPONTOP) / -4., 0), huds->pivot, huds->mframe);
		state.SetVertexBuffer(screen->mVertexData);
	}
	else
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		float thresh = (huds->texture->GetTranslucency() || huds->OverrideShader != -1) && !vrmode->IsVR() ? 0.f : gl_mask_sprite_threshold;
		state.AlphaFunc(Alpha_GEqual, thresh);
		FTranslationID trans = huds->weapon->GetTranslation();
		if ((huds->weapon->Flags & PSPF_PLAYERTRANSLATED)) trans = huds->owner->Translation;
		
		if (r_PlayerSprites3DMode != ITEM_ONLY && r_PlayerSprites3DMode != FAT_ITEM)
		{
			state.SetMaterial(huds->texture, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, trans, huds->OverrideShader);
			state.Draw(DT_TriangleStrip, huds->mx, 4);
		}
		
		DPSprite* psp = huds->weapon;
		FTextureID lump;
		bool mirror;
		if (psp->GetCaller() != nullptr)
		{
			FState* spawn = psp->GetCaller()->FindState(NAME_Spawn);
			lump = sprites[spawn->sprite].GetSpriteFrame(0, 0, nullAngle, &mirror);
		}
		else lump.SetNull();

		auto gtex = TexMan.GetGameTexture(lump, false);
		FMaterial* tex = FMaterial::ValidateTexture(gtex, true, false);

		//TODO Cleanup code for rendering weapon models from sprites in VR mode
		if ((psp->GetID() == PSP_WEAPON || psp->GetID() == PSP_OFFHANDWEAPON) 
		&& vrmode->IsVR()
		&& r_PlayerSprites3DMode != BACK_ONLY
		&& psp->GetCaller() != nullptr
		&& tex != nullptr
		&& lump.isValid())
		{
			float vw = (float)viewwidth;
			float vh = (float)viewheight;

			state.AlphaFunc(Alpha_GEqual, 1);
			state.SetMaterial(gtex, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, trans, huds->OverrideShader);
			
			auto spi = gtex->GetSpritePositioning(0);

			float fU1, fV1;
			float fU2, fV2;
			float z1 = 0.0f;
			float z2 = (huds->y2 - huds->y1) * std::min(3, spi.spriteWidth / spi.spriteHeight);

			if (!(mirror) != !(psp->Flags & PSPF_FLIP))
			{
				fU2 = spi.GetSpriteUL();
				fV1 = spi.GetSpriteVT();
				fU1 = spi.GetSpriteUR();
				fV2 = spi.GetSpriteVB();
			}
			else
			{
				fU1 = spi.GetSpriteUL();
				fV1 = spi.GetSpriteVT();
				fU2 = spi.GetSpriteUR();
				fV2 = spi.GetSpriteVB();
			}

			if (r_PlayerSprites3DMode == FAT_ITEM)
			{
				float x1 = vw / 2 + (huds->x1 - vw / 2) * gl_fatItemWidth;
				float x2 = vw / 2 + (huds->x2 - vw / 2) * gl_fatItemWidth;

				float inc = (x2 - x1) / 12.0f;
				for (float x = x1; x < x2; x += inc)
				{
					screen->mVertexData->Map();
					auto vert = screen->mVertexData->AllocVertices(4);
					auto vp = vert.first;
					vp[0].Set(x, huds->y1, -z1, fU1, fV1);
					vp[1].Set(x, huds->y2, -z1, fU1, fV2);
					vp[2].Set(x, huds->y1, -z2, fU2, fV1);
					vp[3].Set(x, huds->y2, -z2, fU2, fV2);
					screen->mVertexData->Unmap();
					state.Draw(DT_TriangleStrip, vert.second, 4, x == x1);
				}
			}
			else
			{
				float sy;
				float crossAt;
				if (r_PlayerSprites3DMode == ITEM_ONLY)
				{
					crossAt = 0.0f;
					sy = 0.0f;
				}
				else
				{
					sy = huds->y2 - huds->y1;
					crossAt = sy * 0.25f;
				}

				float y1 = huds->y1 - crossAt;
				float y2 = huds->y2 - crossAt;

				screen->mVertexData->Map();
				auto vert = screen->mVertexData->AllocVertices(4);
				auto vp = vert.first;
				vp[0].Set(vw / 2 - crossAt, y1, -z1, fU1, fV1);
				vp[1].Set(vw / 2 + sy / 2, y2, -z1, fU1, fV2);
				vp[2].Set(vw / 2 - crossAt, y1, -z2, fU2, fV1);
				vp[3].Set(vw / 2 + sy / 2, y2, -z2, fU2, fV2);

				auto vert2 = screen->mVertexData->AllocVertices(4);
				auto vp2 = vert2.first;
				vp2[0].Set(vw / 2 + crossAt, y1, -z1, fU1, fV1);
				vp2[1].Set(vw / 2 - sy / 2, y2, -z1, fU1, fV2);
				vp2[2].Set(vw / 2 + crossAt, y1, -z2, fU2, fV1);
				vp2[3].Set(vw / 2 - sy / 2, y2, -z2, fU2, fV2);
				
				screen->mVertexData->Unmap();
				state.Draw(DT_TriangleStrip, vert.second, 4, true);
				state.Draw(DT_TriangleStrip, vert2.second, 4, false);
			}
		}
	}

	state.SetTextureMode(TM_NORMAL);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
}

void HWDrawInfo::DrawHudQuad(FRenderState& state, FGameTexture* texture, float width, float height, float xoffset, float yoffset, bool flipX, bool depthMask)
{
	if (texture == nullptr || width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	texture->SetTranslucent(true);

	state.SetLightIndex(-1);
	state.SetRenderStyle(STYLE_Translucent);
	state.AlphaFunc(Alpha_Greater, 0.0f);
	state.SetTextureMode(TM_NORMAL);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableBrightmap(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(depthMask);
	state.SetMaterial(texture, UF_Sprite, CTF_Expand, CLAMP_XY_NOMIP, 0, -1);

	screen->mVertexData->Map();
	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	const float halfWidth = width * 0.5f;
	const float halfHeight = height * 0.5f;
	float u0 = flipX ? 1.0f : 0.0f;
	float u1 = flipX ? 0.0f : 1.0f;
	vp[0].Set(xoffset - halfWidth, yoffset - halfHeight, 0.0f, u0, 0.0f);
	vp[1].Set(xoffset + halfWidth, yoffset - halfHeight, 0.0f, u1, 0.0f);
	vp[2].Set(xoffset - halfWidth, yoffset + halfHeight, 0.0f, u0, 1.0f);
	vp[3].Set(xoffset + halfWidth, yoffset + halfHeight, 0.0f, u1, 1.0f);
	screen->mVertexData->Unmap();

	state.Draw(DT_TriangleStrip, vert.second, 4);
}

void HWDrawInfo::DrawVRHudBorder(FRenderState& state, float width, float height, PalEntry color, float xoffset, float yoffset)
{
	if (width <= 0.0f || height <= 0.0f)
	{
		return;
	}

	state.SetLightIndex(-1);
	state.SetRenderStyle(STYLE_Source);
	state.SetTextureMode(TM_NORMAL);
	state.SetColor(color);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.EnableBrightmap(false);
	state.EnableDepthTest(false);
	state.SetDepthMask(false);
	state.EnableTexture(false);

	screen->mVertexData->Map();
	auto vert = screen->mVertexData->AllocVertices(4);
	auto vp = vert.first;
	const float halfWidth = width * 0.5f;
	const float halfHeight = height * 0.5f;
	vp[0].Set(xoffset - halfWidth, yoffset - halfHeight, 0.0f, 0.0f, 0.0f);
	vp[1].Set(xoffset + halfWidth, yoffset - halfHeight, 0.0f, 1.0f, 0.0f);
	vp[2].Set(xoffset - halfWidth, yoffset + halfHeight, 0.0f, 0.0f, 1.0f);
	vp[3].Set(xoffset + halfWidth, yoffset + halfHeight, 0.0f, 1.0f, 1.0f);
	screen->mVertexData->Unmap();

	state.Draw(DT_TriangleStrip, vert.second, 4);
	state.EnableTexture(true);
	state.SetDepthMask(true);
	state.EnableDepthTest(true);
}

struct FLaserBeamPoints
{
	DVector3 Start;
	DVector3 HitEnd;
	DVector3 BeamEnd;
};

static DVector3 GetWeaponLaserBeamOffset(AActor* weapon)
{
	if (weapon == nullptr)
	{
		return DVector3(0.0, 0.0, 0.0);
	}

	auto* offset = (DVector3*)weapon->ScriptVar(NAME_LaserBeamOffset, nullptr);
	return offset != nullptr ? *offset : DVector3(0.0, 0.0, 0.0);
}

static DVector3 LaserAngleToVector(DAngle yaw, DAngle pitch)
{
	const double pc = pitch.Cos();
	return DVector3(pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin());
}

static DVector3 GetLaserBeamControllerDirection(bool offhand)
{
	const float* controllerAngles = offhand ? offhandangles : weaponangles;
	const DAngle yaw = DAngle::fromDeg(doomYaw + controllerAngles[1] - hmdorientation[1]);
	return LaserAngleToVector(yaw, DAngle::fromDeg(controllerAngles[0]));
}

static bool GetLaserBeamEndpoints(player_t* player, AActor* weapon, bool offhand, FLaserBeamPoints& points)
{
	if (player == nullptr || player->mo == nullptr || !player->mo->OverrideAttackPosDir)
	{
		return false;
	}

	auto* mo = player->mo;
	const DVector3 direction = GetLaserBeamControllerDirection(offhand);
	const DVector3 base = offhand ? mo->OffhandPos : mo->AttackPos;
	const DVector3 weaponOffset = GetWeaponLaserBeamOffset(weapon);
	const DVector3 forward = direction;
	DVector3 side = DVector3(0.0, 0.0, 1.0) ^ forward;
	if (side.LengthSquared() < 1e-8)
	{
		side = DVector3(0.0, 1.0, 0.0);
	}
	side.MakeUnit();
	DVector3 up = forward ^ side;
	if (up.LengthSquared() < 1e-8)
	{
		up = DVector3(0.0, 0.0, 1.0);
	}
	up.MakeUnit();

	const DVector3 totalOffset = DVector3(
		(double)vr_laser_source_offset_y + weaponOffset.Y,
		(double)vr_laser_source_offset_x + weaponOffset.X,
		(double)vr_laser_source_offset_z + weaponOffset.Z);

	points.Start = base +
		forward * totalOffset.X +
		side * totalOffset.Y +
		up * totalOffset.Z;

	const double maxDistance = 8192.0;
	FTraceResults trace{};
	const bool hit = Trace(points.Start, mo->Sector, direction, maxDistance, MF_SHOOTABLE,
		ML_BLOCKEVERYTHING | ML_BLOCKHITSCAN | ML_BLOCKUSE, mo, trace, TRACE_NoSky);
	points.HitEnd = hit ? trace.HitPos : (points.Start + forward * maxDistance);
	DVector3 beamVector = points.HitEnd - points.Start;
	double beamDistance = beamVector.Length();
	double visibleDistance = beamDistance;
	switch (vr_laser_beam_length)
	{
	case 1:
		visibleDistance *= 0.5;
		break;
	case 2:
		visibleDistance = std::min((double)vr_laser_fixed_length, beamDistance);
		break;
	default:
		break;
	}

	if (beamDistance <= 0.01)
	{
		points.BeamEnd = points.Start;
		return true;
	}

	beamVector.MakeUnit();
	points.BeamEnd = points.Start + beamVector * visibleDistance;
	return true;
}

static void DrawLaserBeamGeometry(FRenderState& state, const DVector3& beamStart, const DVector3& beamEnd, const DVector3& hitEnd, bool drawBeam, bool drawPointer)
{
	if (!drawBeam && !drawPointer)
	{
		return;
	}

	DVector3 pointerCenter = hitEnd;
	if (drawPointer)
	{
		DVector3 pointerBackDir = hitEnd - beamStart;
		if (pointerBackDir.LengthSquared() > 1e-8)
		{
			pointerBackDir.MakeUnit();
			pointerCenter -= pointerBackDir * 4.0;
		}
	}

	const int beamColor = (int)vr_laser_color;
	state.EnableModelMatrix(false);
	state.SetLightIndex(-1);
	state.AlphaFunc(Alpha_Greater, 0.0f);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
	state.EnableTexture(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(false);
	const float beamAlpha = std::clamp<float>(vr_laser_beam_alpha, 0.0f, 1.0f);
	if (drawBeam && beamAlpha > 0.0f)
	{
		const bool beamOpaque = beamAlpha >= 0.999f;

		const DVector3 beamTarget = drawPointer ? pointerCenter : beamEnd;
		DVector3 beamVec = beamTarget - beamStart;
		const double beamLength = beamVec.Length();
		if (beamLength > 0.01)
		{
			beamVec.MakeUnit();

			DVector3 beamRight, beamUp;
			beamVec.GetRightUp(beamRight, beamUp);

			const float beamRadius = 0.25f * std::max(0.05f, (float)vr_laser_beam_width);
			constexpr int beamSegments = 8;
			const int vertexCount = (beamSegments + 1) * 2;

			state.SetRenderStyle(beamOpaque ? STYLE_Source : STYLE_Add);
			state.SetColor(RPART(beamColor) / 255.0f, GPART(beamColor) / 255.0f, BPART(beamColor) / 255.0f, beamAlpha);

			screen->mVertexData->Map();
			auto verts = screen->mVertexData->AllocVertices(vertexCount);
			auto vp = verts.first;
			for (int i = 0; i <= beamSegments; ++i)
			{
				const double t = (double)i / (double)beamSegments;
				const double ang = t * 6.28318530717958647692;
				const double cs = std::cos(ang);
				const double sn = std::sin(ang);
				const DVector3 ringOffset = (beamRight * cs + beamUp * sn) * beamRadius;
				const DVector3 startPos = beamStart + ringOffset;
				const DVector3 endPos = beamTarget + ringOffset;
				vp[i * 2 + 0].Set((float)startPos.X, (float)startPos.Z, (float)startPos.Y, 0.0f, 0.0f);
				vp[i * 2 + 1].Set((float)endPos.X, (float)endPos.Z, (float)endPos.Y, 0.0f, 1.0f);
			}
			screen->mVertexData->Unmap();

			state.Draw(DT_TriangleStrip, verts.second, vertexCount, true);
		}
	}

	if (drawPointer)
	{
		const DVector3 camForward = r_viewpoint.ViewVector3D;
		DVector3 camUp(0.0, 0.0, 1.0);
		DVector3 camRight = camUp ^ camForward;
		if (camRight.LengthSquared() < 1e-8)
		{
			camUp = DVector3(0.0, 1.0, 0.0);
			camRight = camUp ^ camForward;
		}
		camRight.MakeUnit();
		camUp = camForward ^ camRight;
		camUp.MakeUnit();

		const double pointerDistance = (hitEnd - r_viewpoint.Pos).Length();
		const double fovScale = std::tan(r_viewpoint.GetFieldOfView().Radians() * 0.5);
		const double pointerScale = std::max(0.25, (double)vr_laser_pointer_scale);
		const float pointerRadius = (float)std::max(0.006, pointerDistance * fovScale * 0.01 * pointerScale);
		const int pointerSegments = 16;
		const int pointerVertexCount = pointerSegments + 2;
		screen->mVertexData->Map();
		auto pointerVerts = screen->mVertexData->AllocVertices(pointerVertexCount);
		auto pv = pointerVerts.first;
		pv[0].Set((float)pointerCenter.X, (float)pointerCenter.Z, (float)pointerCenter.Y, 0.5f, 0.5f);
		for (int i = 0; i <= pointerSegments; ++i)
		{
			const double t = (double)i / (double)pointerSegments;
			const double ang = t * 6.28318530717958647692;
			const double cs = std::cos(ang);
			const double sn = std::sin(ang);
			const DVector3 ringOffset = (camRight * cs + camUp * sn) * pointerRadius;
			const DVector3 pos = pointerCenter + ringOffset;
			pv[i + 1].Set((float)pos.X, (float)pos.Z, (float)pos.Y, 0.0f, 0.0f);
		}
		screen->mVertexData->Unmap();

		const float pointerAlpha = std::clamp<float>(vr_laser_pointer_alpha, 0.0f, 1.0f);
		const bool pointerOpaque = pointerAlpha >= 0.999f;
		state.SetColor(RPART(beamColor) / 255.0f, GPART(beamColor) / 255.0f, BPART(beamColor) / 255.0f, pointerAlpha);
		state.SetRenderStyle(pointerOpaque ? STYLE_Source : STYLE_Add);
		state.Draw(DT_TriangleFan, pointerVerts.second, pointerVertexCount, true);

		if (vr_laser_pointer_glow != 0)
		{
			const float glowScale = std::max(1.1f, (float)vr_laser_pointer_glow_scale);
			const float glowIntensity = std::max(0.1f, (float)vr_laser_pointer_glow_intensity);
			const bool dynamicGlow = vr_laser_pointer_glow == 2;
			state.SetRenderStyle(STYLE_Add);
			const int glowPasses = dynamicGlow ? 4 : 3;
			for (int pass = 0; pass < glowPasses; ++pass)
			{
				const float passScale = dynamicGlow
					? glowScale * (1.8f + pass * 0.9f)
					: glowScale * (1.0f + pass * 0.5f);
				const float passAlpha = pointerAlpha * glowIntensity * (dynamicGlow
					? (0.20f / (pass + 1))
					: (pass == 0 ? 0.45f : pass == 1 ? 0.22f : 0.10f));
				screen->mVertexData->Map();
				auto glowVerts = screen->mVertexData->AllocVertices(pointerVertexCount);
				auto gv = glowVerts.first;
				gv[0].Set((float)pointerCenter.X, (float)pointerCenter.Z, (float)pointerCenter.Y, 0.5f, 0.5f);
				for (int i = 0; i <= pointerSegments; ++i)
				{
					const double t = (double)i / (double)pointerSegments;
					const double ang = t * 6.28318530717958647692;
					const double cs = std::cos(ang);
					const double sn = std::sin(ang);
					const DVector3 ringOffset = (camRight * cs + camUp * sn) * (pointerRadius * passScale);
					const DVector3 pos = pointerCenter + ringOffset;
					gv[i + 1].Set((float)pos.X, (float)pos.Z, (float)pos.Y, 0.0f, 0.0f);
				}
				screen->mVertexData->Unmap();
				state.SetColor(RPART(beamColor) / 255.0f, GPART(beamColor) / 255.0f, BPART(beamColor) / 255.0f, passAlpha);
				state.Draw(DT_TriangleFan, glowVerts.second, pointerVertexCount, true);
			}
		}
	}

	state.EnableTexture(true);
	state.SetDepthMask(true);
	state.SetRenderStyle(DefaultRenderStyle());
	state.SetTextureMode(TM_NORMAL);
	state.SetColor(1.f, 1.f, 1.f, 1.f);
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.SetDynLight(0, 0, 0);
	state.EnableBrightmap(false);
	state.EnableModelMatrix(false);
	state.ResetColor();
}

void DrawLaserSightWorld(FRenderState& state)
{
	if (!vr_laser_sight && !vr_laser_beam)
	{
		return;
	}

	if (menuactive != MENU_Off)
	{
		return;
	}

	if (vr_laser_hide_on_wheel && VRWheel_IsActive())
	{
		return;
	}

	player_t* player = &players[consoleplayer];
	if (player == nullptr || player->mo == nullptr || !player->mo->OverrideAttackPosDir)
	{
		return;
	}

	auto drawHand = [&state, player](bool offhand)
	{
		AActor* weapon = offhand ? player->OffhandWeapon : player->ReadyWeapon;
		if (weapon == nullptr)
		{
			if (!vr_laser_show_melee)
			{
				return;
			}
		}
		else if (!vr_laser_show_melee && (weapon->IntVar(NAME_WeaponFlags) & WIF_MELEEWEAPON))
		{
			return;
		}

		FLaserBeamPoints points;
		if (GetLaserBeamEndpoints(player, weapon, offhand, points))
		{
			const bool drawBeam = vr_laser_beam || (weapon != nullptr && (weapon->IntVar(NAME_WeaponFlags) & WIF_HASLASERBEAM));
			const bool drawPointer = vr_laser_sight;
			DrawLaserBeamGeometry(state, points.Start, points.BeamEnd, points.HitEnd, drawBeam, drawPointer);
		}
	};

	drawHand(false);
	drawHand(true);
}

//==========================================================================
//
// R_DrawPlayerSprites
//
//==========================================================================

void HWDrawInfo::DrawPlayerSprites(bool hudModelStep, FRenderState &state)
{
	auto vrmode = VRMode::GetVRModeCached(true);
	
	auto oldlightmode = lightmode;
	for (auto &hudsprite : hudsprites)
	{
		if (!vrmode->IsVR() && (!!hudsprite.mframe) != hudModelStep) continue;
		if (!hudsprite.mframe && isSoftwareLighting(oldlightmode)) SetFallbackLightMode();	// Software lighting cannot handle 2D content.
		if (hudsprite.weapon != nullptr && hudsprite.owner != nullptr && hudsprite.owner->player != nullptr)
		{
			AActor* caller = hudsprite.weapon->GetCaller();
			player_t* spritePlayer = hudsprite.owner->player;
			if (WeaponSpriteMatches(spritePlayer->ReadyWeapon, caller) && VRWheel_ShouldSuppressWeaponHand(VR_MAINHAND))
			{
				continue;
			}
			if (WeaponSpriteMatches(spritePlayer->OffhandWeapon, caller) && VRWheel_ShouldSuppressWeaponHand(VR_OFFHAND))
			{
				continue;
			}
		}
		if (!hudsprite.mframe)
		{
			const bool isOffhandSprite = hudsprite.weapon != nullptr &&
				hudsprite.owner != nullptr &&
				hudsprite.owner->player != nullptr &&
				WeaponSpriteMatches(hudsprite.owner->player->OffhandWeapon, hudsprite.weapon->GetCaller());
			vrmode->AdjustPlayerSprites(state, isOffhandSprite);
		}
		DrawPSprite(&hudsprite, state);
		if (!hudsprite.mframe) vrmode->UnAdjustPlayerSprites(state);
		lightmode = oldlightmode;
	}
}


//==========================================================================
//
//
//
//==========================================================================

static bool isBright(DPSprite *psp)
{
	if (psp != nullptr && psp->GetState() != nullptr)
	{
		bool disablefullbright = false;
		FTextureID lump = sprites[psp->GetSprite()].GetSpriteFrame(psp->GetFrame(), 0, nullAngle, nullptr);
		if (lump.isValid())
		{
			auto tex = TexMan.GetGameTexture(lump, true);
			if (tex) disablefullbright = tex->isFullbrightDisabled();
		}
		return psp->GetState()->GetFullbright() && !disablefullbright;
	}
	return false;
}

static bool WeaponSpriteMatches(AActor* equippedWeapon, AActor* spriteCaller)
{
	if (equippedWeapon == nullptr || spriteCaller == nullptr)
	{
		return false;
	}

	if (equippedWeapon == spriteCaller || equippedWeapon->GetClass() == spriteCaller->GetClass())
	{
		return true;
	}

	auto equippedSister = equippedWeapon->PointerVar<AActor>(NAME_SisterWeapon);
	auto callerSister = spriteCaller->PointerVar<AActor>(NAME_SisterWeapon);
	if (equippedSister == spriteCaller || callerSister == equippedWeapon)
	{
		return true;
	}

	return (equippedSister != nullptr && equippedSister->GetClass() == spriteCaller->GetClass()) ||
		(callerSister != nullptr && callerSister->GetClass() == equippedWeapon->GetClass());
}

//==========================================================================
//
// Weapon position
//
//==========================================================================

static WeaponPosition2D GetWeaponPosition2D(player_t *player, double ticFrac, DPSprite *psp)
{
	WeaponPosition2D w;
	P_BobWeapon(player, &w.bobx, &w.boby, ticFrac);

	DPSprite *readyWeaponPsp = player->FindPSprite(PSP_WEAPON);
	DPSprite *offhandWeaponPsp = player->FindPSprite(PSP_OFFHANDWEAPON);

	// Interpolate the main weapon layer once so as to be able to add it to other layers.
	w.weapon = WeaponSpriteMatches(player->ReadyWeapon, psp->GetCaller()) ? readyWeaponPsp : offhandWeaponPsp;
	if (w.weapon != nullptr)
	{
		if (w.weapon->firstTic)
		{
			w.wx = (float)w.weapon->x;
			w.wy = (float)w.weapon->y;
		}
		else
		{
			w.wx = (float)(w.weapon->oldx + (w.weapon->x - w.weapon->oldx) * ticFrac);
			w.wy = (float)(w.weapon->oldy + (w.weapon->y - w.weapon->oldy) * ticFrac);
		}
	}
	else
	{
		w.wx = 0;
		w.wy = 0;
	}
	return w;
}

static WeaponPosition3D GetWeaponPosition3D(player_t *player, double ticFrac, DPSprite *psp)
{
	WeaponPosition3D w;
	P_BobWeapon3D(player, &w.translation, &w.rotation, ticFrac);

	DPSprite *readyWeaponPsp = player->FindPSprite(PSP_WEAPON);
	DPSprite *offhandWeaponPsp = player->FindPSprite(PSP_OFFHANDWEAPON);

	// Interpolate the main weapon layer once so as to be able to add it to other layers.
	w.weapon = WeaponSpriteMatches(player->ReadyWeapon, psp->GetCaller()) ? readyWeaponPsp : offhandWeaponPsp;
	if (w.weapon != nullptr)
	{
		if (w.weapon->firstTic)
		{
			w.wx = (float)w.weapon->x;
			w.wy = (float)w.weapon->y;
		}
		else
		{
			w.wx = (float)(w.weapon->oldx + (w.weapon->x - w.weapon->oldx) * ticFrac);
			w.wy = (float)(w.weapon->oldy + (w.weapon->y - w.weapon->oldy) * ticFrac);
		}
		
		auto weaponActor = w.weapon->GetCaller();

		if (weaponActor && weaponActor->IsKindOf(NAME_Weapon))
		{
			DVector3 *dPivot = (DVector3*) weaponActor->ScriptVar(NAME_BobPivot3D, nullptr);
			w.pivot.X = (float) dPivot->X;
			w.pivot.Y = (float) dPivot->Y;
			w.pivot.Z = (float) dPivot->Z;
		}
		else
		{
			w.pivot = FVector3(0,0,0);
		}
	}
	else
	{
		w.wx = 0;
		w.wy = 0;
		w.pivot = FVector3(0,0,0);
	}
	return w;
}

//==========================================================================
//
// Bobbing
//
//==========================================================================

static FVector2 BobWeapon2D(WeaponPosition2D &weap, DPSprite *psp, double ticFrac)
{
	if (psp->firstTic)
	{ // Can't interpolate the first tic.
		psp->firstTic = false;
		psp->ResetInterpolation();
	}

	float sx = float(psp->oldx + (psp->x - psp->oldx) * ticFrac);
	float sy = float(psp->oldy + (psp->y - psp->oldy) * ticFrac);

	if (psp->Flags & PSPF_ADDBOB)
	{
		sx += (psp->Flags & PSPF_MIRROR) ? -weap.bobx : weap.bobx;
		sy += weap.boby;
	}

	if (psp->Flags & PSPF_ADDWEAPON && !(psp->GetID() == PSP_WEAPON || psp->GetID() == PSP_OFFHANDWEAPON))
	{
		sx += weap.wx;
		sy += weap.wy;
	}
	return { sx, sy };
}

static FVector2 BobWeapon3D(WeaponPosition3D &weap, DPSprite *psp, FVector3 &translation, FVector3 &rotation, FVector3 &pivot, double ticFrac)
{
	if (psp->firstTic)
	{ // Can't interpolate the first tic.
		psp->firstTic = false;
		psp->ResetInterpolation();
	}

	float sx = float(psp->oldx + (psp->x - psp->oldx) * ticFrac);
	float sy = float(psp->oldy + (psp->y - psp->oldy) * ticFrac);
	float sz = 0;

	if (psp->Flags & PSPF_ADDBOB)
	{
		if (psp->Flags & PSPF_MIRROR)
		{
			translation = FVector3(-weap.translation.X, weap.translation.Y, weap.translation.Z);
			rotation = FVector3(-weap.rotation.X, weap.rotation.Y, weap.rotation.Z);
			pivot = FVector3(-weap.pivot.X, weap.pivot.Y, weap.pivot.Z);
		}
		else
		{
			translation = weap.translation ;
			rotation = weap.rotation ;
			pivot = weap.pivot ;
		}
	}
	else
	{
		translation = rotation = pivot = FVector3(0,0,0);
	}

	if (psp->Flags & PSPF_ADDWEAPON && !(psp->GetID() == PSP_WEAPON || psp->GetID() == PSP_OFFHANDWEAPON))
	{
		sx += weap.wx;
		sy += weap.wy;
	}
	return { sx, sy };
}

//==========================================================================
//
// Lighting
//
//==========================================================================

WeaponLighting HWDrawInfo::GetWeaponLighting(sector_t *viewsector, const DVector3 &pos, int cm, area_t in_area, const DVector3 &playerpos)
{
	WeaponLighting l;

	if (cm)
	{
		l.lightlevel = 255;
		l.cm.Clear();
		l.isbelow = false;
	}
	else
	{
		auto fakesec = hw_FakeFlat(viewsector, in_area, false);

		// calculate light level for weapon sprites
		l.lightlevel = hw_ClampLight(fakesec->lightlevel);

		// calculate colormap for weapon sprites
		if (viewsector->e->XFloor.ffloors.Size() && !(Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING))
		{
			TArray<lightlist_t> & lightlist = viewsector->e->XFloor.lightlist;
			for (unsigned i = 0; i<lightlist.Size(); i++)
			{
				double lightbottom;

				if (i<lightlist.Size() - 1)
				{
					lightbottom = lightlist[i + 1].plane.ZatPoint(pos);
				}
				else
				{
					lightbottom = viewsector->floorplane.ZatPoint(pos);
				}

				if (lightbottom < pos.Z)
				{
					l.cm = lightlist[i].extra_colormap;
					l.lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
					break;
				}
			}
		}
		else
		{
			l.cm = fakesec->Colormap;
			if (Level->flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING) l.cm.ClearColor();
		}

		l.lightlevel = CalcLightLevel(lightmode, l.lightlevel, getExtraLight(), true, 0);

		if (isSoftwareLighting(lightmode) || l.lightlevel < 92)
		{
			// Korshun: the way based on max possible light level for sector like in software renderer.
			double min_L = 36.0 / 31.0 - ((l.lightlevel / 255.0) * (63.0 / 31.0)); // Lightlevel in range 0-63
			if (min_L < 0)
				min_L = 0;
			else if (min_L > 1.0)
				min_L = 1.0;

			l.lightlevel = int((1.0 - min_L) * 255);
		}
		else
		{
			l.lightlevel = (2 * l.lightlevel + 255) / 3;
		}
		l.lightlevel = viewsector->CheckSpriteGlow(l.lightlevel, playerpos);
		l.isbelow = fakesec != viewsector && in_area == area_below;
	}

	// Korshun: fullbright fog in opengl, render weapon sprites fullbright (but don't cancel out the light color!)
	if (Level->brightfog && ((Level->flags&LEVEL_HASFADETABLE) || l.cm.FadeColor != 0))
	{
		l.lightlevel = 255;
	}
	return l;
}

//==========================================================================
//
//
//
//==========================================================================

void HUDSprite::SetBright(bool isbelow)
{
	if (!isbelow)
	{
		cm.MakeWhite();
	}
	else
	{
		// under water areas keep most of their color for fullbright objects
		cm.LightColor.r = (3 * cm.LightColor.r + 0xff) / 4;
		cm.LightColor.g = (3 * cm.LightColor.g + 0xff) / 4;
		cm.LightColor.b = (3 * cm.LightColor.b + 0xff) / 4;
	}
	lightlevel = 255;
}

//==========================================================================
//
// Render Style
//
//==========================================================================

bool HUDSprite::GetWeaponRenderStyle(DPSprite *psp, AActor *playermo, sector_t *viewsector, WeaponLighting &lighting)
{
	auto rs = psp->GetRenderStyle(playermo->RenderStyle, playermo->Alpha);

	visstyle_t vis;

	vis.RenderStyle = STYLE_Count;
	vis.Alpha = rs.second;
	vis.Invert = false;
	playermo->AlterWeaponSprite(&vis);

	alpha = (psp->Flags & PSPF_FORCEALPHA) ? 0.f : vis.Alpha;

	if (vis.RenderStyle != STYLE_Count && !(psp->Flags & PSPF_FORCESTYLE))
	{
		RenderStyle = vis.RenderStyle;
	}
	else
	{
		RenderStyle = rs.first;
	}
	if (RenderStyle.BlendOp == STYLEOP_None) return false;

	if (vis.Invert)
	{
		// this only happens for Strife's inverted weapon sprite
		RenderStyle.Flags |= STYLEF_InvertSource;
	}

	// Set the render parameters

	OverrideShader = -1;
	if (RenderStyle.BlendOp == STYLEOP_Fuzz)
	{
		if (gl_fuzztype != 0)
		{
			// Todo: implement shader selection here
			RenderStyle = LegacyRenderStyles[STYLE_Translucent];
			OverrideShader = SHADER_NoTexture + gl_fuzztype;
			alpha = 0.99f;	// trans may not be 1 here
		}
		else
		{
			RenderStyle.BlendOp = STYLEOP_Shadow;
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		alpha	= transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		alpha = 1.f;
	}
	else if (alpha == 0.f)
	{
		alpha = vis.Alpha;
	}
	if (!RenderStyle.IsVisible(alpha)) return false;	// if it isn't visible skip the rest.

	PalEntry ThingColor = (playermo->RenderStyle.Flags & STYLEF_ColorIsFixed) ? playermo->fillcolor : 0xffffff;
	ThingColor.a = 255;

	const bool bright = isBright(psp);
	ObjectColor = bright ? ThingColor : ThingColor.Modulate(viewsector->SpecialColors[sector_t::sprites]);

	lightlevel = lighting.lightlevel;
	cm = lighting.cm;
	if (bright) SetBright(lighting.isbelow);

	return true;
}

//==========================================================================
//
// Coordinates
//
//==========================================================================

bool HUDSprite::GetWeaponRect(HWDrawInfo *di, DPSprite *psp, float sx, float sy, player_t *player, double ticfrac)
{
	float			tx;
	float			scale;
	float			scalex;
	float			ftextureadj;
	float			ftexturemid;

	// decide which patch to use
	bool mirror;
	FTextureID lump = sprites[psp->GetSprite()].GetSpriteFrame(psp->GetFrame(), 0, nullAngle, &mirror);
	if (!lump.isValid()) return false;

	auto tex = TexMan.GetGameTexture(lump, false);
	if (!tex || !tex->isValid()) return false;
	auto& spi = tex->GetSpritePositioning(1);

	float vw = (float)viewwidth;
	float vh = (float)viewheight;

	FloatRect r = spi.GetSpriteRect();

	// calculate edges of the shape
	scalex = psp->baseScale.X * (320.0f / (240.0f * r_viewwindow.WidescreenRatio)) * (vw / 320);

	tx = (psp->Flags & PSPF_MIRROR) ? ((160 - r.width) - (sx + r.left)) : (sx - (160 - r.left));
	x1 = tx * scalex + vw / 2;
	// [MC] Disabled these because vertices can be manipulated now.
	//if (x1 > vw)	return false; // off the right side
	x1 += viewwindowx;


	tx += r.width;
	x2 = tx * scalex + vw / 2;
	//if (x2 < 0) return false; // off the left side
	x2 += viewwindowx;

	// killough 12/98: fix psprite positioning problem
	ftextureadj = (120.0f / psp->baseScale.Y) - 100.0f; // [XA] scale relative to weapon baseline
	ftexturemid = 100.f - sy - r.top - psp->GetYAdjust(screenblocks >= 11) - ftextureadj;

	// [XA] note: Doom's native 1.2x aspect ratio was originally
	// handled here by multiplying SCREENWIDTH by 200 instead of
	// 240, but now the baseScale var defines this from now on.
	scale = psp->baseScale.Y * (SCREENHEIGHT*vw) / (SCREENWIDTH * 240.0f);
	y1 = viewwindowy + vh / 2 - (ftexturemid * scale);
	y2 = y1 + (r.height * scale) + 1;

	const bool flip = (psp->Flags & PSPF_FLIP);
	if (!(mirror) != !(flip))
	{
		u2 = spi.GetSpriteUL();
		v1 = spi.GetSpriteVT();
		u1 = spi.GetSpriteUR();
		v2 = spi.GetSpriteVB();
	}
	else
	{
		u1 = spi.GetSpriteUL();
		v1 = spi.GetSpriteVT();
		u2 = spi.GetSpriteUR();
		v2 = spi.GetSpriteVB();
	}

	// [MC] Code copied from DTA_Rotate.
	// Big thanks to IvanDobrovski who helped me modify this.

	WeaponInterp Vert;
	Vert.v[0] = FVector2(x1, y1);
	Vert.v[1] = FVector2(x1, y2);
	Vert.v[2] = FVector2(x2, y1);
	Vert.v[3] = FVector2(x2, y2);

	for (int i = 0; i < 4; i++)
	{
		const float cx = (flip) ? -psp->Coord[i].X : psp->Coord[i].X;
		Vert.v[i] += FVector2(cx * scalex, psp->Coord[i].Y * scale);
	}
	if (psp->rotation != nullAngle || !psp->scale.isZero())
	{
		// [MC] Sets up the alignment for starting the pivot at, in a corner.
		float anchorx, anchory;
		switch (psp->VAlign)
		{
			default:
			case PSPA_TOP:		anchory = 0.0;	break;
			case PSPA_CENTER:	anchory = 0.5;	break;
			case PSPA_BOTTOM:	anchory = 1.0;	break;
		}

		switch (psp->HAlign)
		{
			default:
			case PSPA_LEFT:		anchorx = 0.0;	break;
			case PSPA_CENTER:	anchorx = 0.5;	break;
			case PSPA_RIGHT:	anchorx = 1.0;	break;
		}
		// Handle PSPF_FLIP.
		if (flip) anchorx = 1.0 - anchorx;

		FAngle rot = FAngle::fromDeg(float((flip) ? -psp->rotation.Degrees() : psp->rotation.Degrees()));
		const float cosang = rot.Cos();
		const float sinang = rot.Sin();
		
		float xcenter, ycenter;
		const float width = x2 - x1;
		const float height = y2 - y1;
		const float px = float((flip) ? -psp->pivot.X : psp->pivot.X);
		const float py = float(psp->pivot.Y);

		// Set up the center and offset accordingly. PivotPercent changes it to be a range [0.0, 1.0]
		// instead of pixels and is enabled by default.
		if (psp->Flags & PSPF_PIVOTPERCENT)
		{
			xcenter = x1 + (width * anchorx + width * px);
			ycenter = y1 + (height * anchory + height * py);
		}
		else
		{
			xcenter = x1 + (width * anchorx + scalex * px);
			ycenter = y1 + (height * anchory + scale * py);
		}

		// Now adjust the position, rotation and scale of the image based on the latter two.
		for (int i = 0; i < 4; i++)
		{
			Vert.v[i] -= {xcenter, ycenter};
			const float xx = xcenter + psp->scale.X * (Vert.v[i].X * cosang + Vert.v[i].Y * sinang);
			const float yy = ycenter - psp->scale.Y * (Vert.v[i].X * sinang - Vert.v[i].Y * cosang);
			Vert.v[i] = {xx, yy};
		}
	}
	psp->Vert = Vert;

	if (psp->scale.X == 0.0 || psp->scale.Y == 0.0)
		return false;

	const bool interp = (psp->InterpolateTic || psp->Flags & PSPF_INTERPOLATE);

	for (int i = 0; i < 4; i++)
	{
		FVector2 t = Vert.v[i];
		if (interp)
			t = psp->Prev.v[i] + (psp->Vert.v[i] - psp->Prev.v[i]) * ticfrac;

		Vert.v[i] = t;
	}
	
	// [MC] If this is absolutely necessary, uncomment it. It just checks if all the vertices 
	// are all off screen either to the right or left, but is it honestly needed?
	/*
	if ((
		Vert.v[0].X > 0.0 &&
		Vert.v[1].X > 0.0 &&
		Vert.v[2].X > 0.0 &&
		Vert.v[3].X > 0.0) || (
		Vert.v[0].X < vw &&
		Vert.v[1].X < vw &&
		Vert.v[2].X < vw &&
		Vert.v[3].X < vw))
		return false;
	*/
	auto verts = screen->mVertexData->AllocVertices(4);
	mx = verts.second;

	verts.first[0].Set(Vert.v[0].X, Vert.v[0].Y, 0, u1, v1);
	verts.first[1].Set(Vert.v[1].X, Vert.v[1].Y, 0, u1, v2);
	verts.first[2].Set(Vert.v[2].X, Vert.v[2].Y, 0, u2, v1);
	verts.first[3].Set(Vert.v[3].X, Vert.v[3].Y, 0, u2, v2);

	texture = tex;
	return true;
}

//==========================================================================
//
// R_DrawPlayerSprites
//
//==========================================================================
void HWDrawInfo::PreparePlayerSprites2D(sector_t * viewsector, area_t in_area)
{
	static PClass * wpCls = PClass::FindClass("Weapon");
	static unsigned ModifyBobLayerVIndex = GetVirtualIndex(wpCls, "ModifyBobLayer");
	static VMFunction * ModifyBobLayerOrigFunc = wpCls->Virtuals.Size() > ModifyBobLayerVIndex ? wpCls->Virtuals[ModifyBobLayerVIndex] : nullptr;
	
	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;

	const auto &vp = Viewpoint;

	AActor *camera = vp.camera;

	WeaponLighting light = GetWeaponLighting(viewsector, vp.Pos, isFullbrightScene(), in_area, camera->Pos());

	// hack alert! Rather than changing everything in the underlying lighting code let's just temporarily change
	// light mode here to draw the weapon sprite.
	auto oldlightmode = lightmode;
	if (isSoftwareLighting(oldlightmode)) SetFallbackLightMode();

	for (DPSprite *psp = player->psprites; psp != nullptr && psp->GetID() < PSP_TARGETCENTER; psp = psp->GetNext())
	{
		if (weaponStabilised && psp->GetCaller() == player->OffhandWeapon)
		{
			continue;
		}
		if (!psp->GetState()) continue;
		
		FSpriteModelFrame *smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);

		// This is an 'either-or' proposition. This maybe needs some work to allow overlays with weapon models but as originally implemented this just won't work.
		if (smf) continue;

		HUDSprite hudsprite;
		hudsprite.owner = playermo;
		hudsprite.mframe = smf;
		hudsprite.weapon = psp;

		if (!hudsprite.GetWeaponRenderStyle(psp, camera, viewsector, light)) continue;

		WeaponPosition2D weap = GetWeaponPosition2D(camera->player, vp.TicFrac, psp);

		VMFunction * ModifyBobLayer = nullptr;
		DVector2 bobxy = DVector2(weap.bobx , weap.boby);

		if(weap.weapon && weap.weapon->GetCaller())
		{
			PClass * cls = weap.weapon->GetCaller()->GetClass();
			ModifyBobLayer = cls->Virtuals.Size() > ModifyBobLayerVIndex ? cls->Virtuals[ModifyBobLayerVIndex] : nullptr;

			if( ModifyBobLayer == ModifyBobLayerOrigFunc) ModifyBobLayer = nullptr;
		}

		if(ModifyBobLayer && (psp->Flags & PSPF_ADDBOB))
		{
			DVector2 out;
			VMValue param[] = { weap.weapon->GetCaller() , bobxy.X , bobxy.Y , psp->GetID() , vp.TicFrac };
			VMReturn ret(&out);

			VMCall(ModifyBobLayer, param, 5, &ret, 1);

			weap.bobx = out.X;
			weap.boby = out.Y;
		}

		FVector2 spos = BobWeapon2D(weap, psp, vp.TicFrac);

		hudsprite.dynrgb[0] = hudsprite.dynrgb[1] = hudsprite.dynrgb[2] = 0;
		hudsprite.lightindex = -1;
		// set the lighting parameters
		if (hudsprite.RenderStyle.BlendOp != STYLEOP_Shadow && Level->HasDynamicLights && !isFullbrightScene() && gl_light_sprites)
		{
			GetDynSpriteLight(playermo, nullptr, hudsprite.dynrgb);
		}

		if (!hudsprite.GetWeaponRect(this, psp, spos.X, spos.Y, player, vp.TicFrac)) continue;
		hudsprites.Push(hudsprite);
	}
	lightmode = oldlightmode;
}

void HWDrawInfo::PreparePlayerSprites3D(sector_t * viewsector, area_t in_area)
{
	static PClass * wpCls = PClass::FindClass("Weapon");
	
	static unsigned ModifyBobLayer3DVIndex = GetVirtualIndex(wpCls, "ModifyBobLayer3D");
	static unsigned ModifyBobPivotLayer3DVIndex = GetVirtualIndex(wpCls, "ModifyBobPivotLayer3D");

	static VMFunction * ModifyBobLayer3DOrigFunc = wpCls->Virtuals.Size() > ModifyBobLayer3DVIndex ? wpCls->Virtuals[ModifyBobLayer3DVIndex] : nullptr;
	static VMFunction * ModifyBobPivotLayer3DOrigFunc = wpCls->Virtuals.Size() > ModifyBobPivotLayer3DVIndex ? wpCls->Virtuals[ModifyBobPivotLayer3DVIndex] : nullptr;
	
	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;

	const auto &vp = Viewpoint;

	AActor *camera = vp.camera;

	WeaponLighting light = GetWeaponLighting(viewsector, vp.Pos, isFullbrightScene(), in_area, camera->Pos());

	// hack alert! Rather than changing everything in the underlying lighting code let's just temporarily change
	// light mode here to draw the weapon sprite.
	auto oldlightmode = lightmode;
	if (isSoftwareLighting(oldlightmode)) SetFallbackLightMode();

	for (DPSprite *psp = player->psprites; psp != nullptr && psp->GetID() < PSP_TARGETCENTER; psp = psp->GetNext())
	{
		if (weaponStabilised && psp->GetCaller() == player->OffhandWeapon)
		{
			continue;
		}
		if (!psp->GetState()) continue;
		FSpriteModelFrame *smf = FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame(), false);

		// This is an 'either-or' proposition. This maybe needs some work to allow overlays with weapon models but as originally implemented this just won't work.
		if (!smf) continue;

		HUDSprite hudsprite;
		hudsprite.owner = playermo;
		hudsprite.mframe = smf;
		hudsprite.weapon = psp;

		WeaponPosition3D weap = GetWeaponPosition3D(camera->player, vp.TicFrac, psp);

		VMFunction * ModifyBobLayer3D = nullptr;
		VMFunction * ModifyBobPivotLayer3D = nullptr;

		DVector3 translation = DVector3(weap.translation);
		DVector3 rotation = DVector3(weap.rotation);
		DVector3 pivot = DVector3(weap.pivot);

		if(weap.weapon && weap.weapon->GetCaller())
		{
			PClass * cls = weap.weapon->GetCaller()->GetClass();
			ModifyBobLayer3D = cls->Virtuals.Size() > ModifyBobLayer3DVIndex ? cls->Virtuals[ModifyBobLayer3DVIndex] : nullptr;
			ModifyBobPivotLayer3D = cls->Virtuals.Size() > ModifyBobPivotLayer3DVIndex ? cls->Virtuals[ModifyBobPivotLayer3DVIndex] : nullptr;

			if( ModifyBobLayer3D == ModifyBobLayer3DOrigFunc) ModifyBobLayer3D = nullptr;
			if( ModifyBobPivotLayer3D == ModifyBobPivotLayer3DOrigFunc) ModifyBobPivotLayer3D = nullptr;
		}

		if(ModifyBobLayer3D && (psp->Flags & PSPF_ADDBOB))
		{
			DVector3 t, r;
			
			VMReturn returns[2];

			returns[0].Vec3At(&t);
			returns[1].Vec3At(&r);

			VMValue param[] = { weap.weapon->GetCaller() , translation.X, translation.Y, translation.Z, rotation.X, rotation.Y, rotation.Z, psp->GetID() , vp.TicFrac };
			VMCall(ModifyBobLayer3D, param, 9, returns, 2);

			weap.translation = FVector3(t);
			weap.rotation = FVector3(r);
		}

		if(ModifyBobPivotLayer3D && (psp->Flags & PSPF_ADDBOB))
		{
			DVector3 p;

			VMReturn ret(&p);

			VMValue param[] = { weap.weapon->GetCaller() , pivot.X, pivot.Y, pivot.Z, psp->GetID() , vp.TicFrac };
			VMCall(ModifyBobPivotLayer3D, param, 6, &ret, 1);

			weap.pivot = FVector3(p);
		}

		if (!hudsprite.GetWeaponRenderStyle(psp, camera, viewsector, light)) continue;

		//FVector2 spos = BobWeapon3D(weap, psp, hudsprite.translation, hudsprite.rotation, hudsprite.pivot, vp.TicFrac);

		FVector2 spos = BobWeapon3D(weap, psp, hudsprite.translation, hudsprite.rotation, hudsprite.pivot, vp.TicFrac);

		hudsprite.dynrgb[0] = hudsprite.dynrgb[1] = hudsprite.dynrgb[2] = 0;
		hudsprite.lightindex = -1;
		// set the lighting parameters
		if (hudsprite.RenderStyle.BlendOp != STYLEOP_Shadow && Level->HasDynamicLights && !isFullbrightScene() && gl_light_weapons)
		{
			hw_GetDynModelLight(playermo, lightdata);
			hudsprite.lightindex = screen->mLights->UploadLights(lightdata);
			LightProbe* probe = FindLightProbe(playermo->Level, playermo->X(), playermo->Y(), playermo->Center());
			if (probe)
			{
				hudsprite.dynrgb[0] = probe->Red;
				hudsprite.dynrgb[1] = probe->Green;
				hudsprite.dynrgb[2] = probe->Blue;
			}
		}

		// [BB] In the HUD model step we just render the model and break out. 
		hudsprite.mx = spos.X;
		hudsprite.my = spos.Y;

		hudsprites.Push(hudsprite);
	}
	lightmode = oldlightmode;
}

void HWDrawInfo::PreparePlayerSprites(sector_t * viewsector, area_t in_area)
{

	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;
	
    const auto &vp = Viewpoint;

	AActor *camera = vp.camera;

	// this is the same as the software renderer
	if (!player ||
		!r_drawplayersprites ||
		!camera->player ||
		(player->cheats & CF_CHASECAM) ||
		(r_deathcamera && camera->health <= 0))
		return;

	PreparePlayerSprites3D(viewsector,in_area);
	PreparePlayerSprites2D(viewsector,in_area);

	PrepareTargeterSprites(vp.TicFrac);
}


//==========================================================================
//
// R_DrawPlayerSprites
//
//==========================================================================

void HWDrawInfo::PrepareTargeterSprites(double ticfrac)
{
	AActor * playermo = players[consoleplayer].camera;
	player_t * player = playermo->player;
	AActor *camera = Viewpoint.camera;

	// this is the same as above
	if (!player ||
		!r_drawplayersprites ||
		!camera->player ||
		(player->cheats & CF_CHASECAM) ||
		(r_deathcamera && camera->health <= 0))
		return;

	HUDSprite hudsprite;

	hudsprite.owner = playermo;
	hudsprite.mframe = nullptr;
	hudsprite.cm.Clear();
	hudsprite.lightlevel = 255;
	hudsprite.ObjectColor = 0xffffffff;
	hudsprite.alpha = 1;
	hudsprite.RenderStyle = DefaultRenderStyle();
	hudsprite.OverrideShader = -1;
	hudsprite.dynrgb[0] = hudsprite.dynrgb[1] = hudsprite.dynrgb[2] = 0;

	// The Targeter's sprites are always drawn normally.
	for (DPSprite *psp = player->FindPSprite(PSP_TARGETCENTER); psp != nullptr; psp = psp->GetNext())
	{
		if (psp->GetState() != nullptr && (psp->GetID() != PSP_TARGETCENTER || CrosshairImage == nullptr))
		{
			hudsprite.weapon = psp;
			
			if (hudsprite.GetWeaponRect(this, psp, psp->x, psp->y, player, ticfrac))
			{
				hudsprites.Push(hudsprite);
			}
		}
	}
}

