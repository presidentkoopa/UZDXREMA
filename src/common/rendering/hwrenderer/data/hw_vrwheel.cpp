#include "hw_vrwheel.h"

#include <cmath>

#include "playsim/actor.h"
#include "c_console.h"
#include "c_cvars.h"
#include "d_player.h"
#include "g_game.h"
#include "gamedata/a_weapons.h"
#include "gamedata/info.h"
#include "menu.h"
#include "common/textures/gametexture.h"
#include "common/textures/textures.h"
#include "common/textures/texturemanager.h"
#include "common/statusbar/base_sbar.h"
#include "common/rendering/hwrenderer/data/flatvertices.h"
#include "common/rendering/hwrenderer/data/hw_renderstate.h"
#include "common/rendering/hwrenderer/data/hw_viewpointbuffer.h"
#include "common/utility/i_time.h"
#include "g_statusbar/sbar.h"
#include "sound/s_doomsound.h"
#include <QzDoom/VrCommon.h>
#include "hw_vrmodes.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "r_data/sprites.h"
#include "r_utility.h"

EXTERN_CVAR(Int, vr_control_scheme)
EXTERN_CVAR(Float, i_timescale)
EXTERN_CVAR(Float, gl_mask_sprite_threshold)

CVAR(Bool, vr_wheel_weapon_all, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_switch_hands, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_hide_hand_weapon, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_sound, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_bg_color, (int)MAKEARGB(128, 63, 63, 63), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_select_color, (int)MAKEARGB(160, 255, 208, 0), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_distance, 0.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Float, vr_wheel_time_slow, 0.3f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.0f)
	{
		self = 0.0f;
	}
	else if (self > 1.0f)
	{
		self = 1.0f;
	}
	else if (self > 0.0f && self < 0.1f)
	{
		self = 0.1f;
	}
}
CVAR(Float, vr_wheel_xoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_yoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_radius, 8.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_deadzone, 0.30f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_select_angle, 22.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

namespace
{
	enum class EVRWheelType
	{
		None,
		Weapon,
		Inventory
	};

	struct VRWheelEntry
	{
		AActor* Item = nullptr;
		FGameTexture* Icon = nullptr;
		bool Selectable = false;
		bool Owned = false;
	};

	struct VRWheelState
	{
		EVRWheelType Type = EVRWheelType::None;
		int AnchorHand = VR_MAINHAND;
		VSMatrix Transform;
		DVector3 HeadLocalOffset = {};
		DAngle OpenYaw = nullAngle;
		DAngle OpenPitch = nullAngle;
		int HoveredIndex = -1;
		bool HoverValid = false;
		bool TimeControlActive = false;
		bool TimeControlFrozen = false;
		double SavedTimeScale = 1.0;
		TArray<VRWheelEntry> Entries;
	};

	VRWheelState GVRWheel;

	static void UpdateHover(player_t* player);

	static DVector3 ToGamePoint(const double* xyz)
	{
		return { xyz[0], xyz[2], xyz[1] };
	}

	static DVector3 MatrixPointToGame(VSMatrix& mat, double x, double y, double z)
	{
		FLOATTYPE in[4] =
		{
			(FLOATTYPE)x,
			(FLOATTYPE)y,
			(FLOATTYPE)z,
			(FLOATTYPE)1.0
		};
		FLOATTYPE out[4] = {};
		mat.multMatrixPoint(in, out);
		const double point[3] = { (double)out[0], (double)out[1], (double)out[2] };
		return ToGamePoint(point);
	}

	static DVector3 AngleToVector(DAngle yaw, DAngle pitch)
	{
		const double pc = pitch.Cos();
		DVector3 vec = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };
		vec.MakeUnit();
		return vec;
	}

	static void PlayWheelSound(const char* sound)
	{
		if (!vr_wheel_sound || sound == nullptr || *sound == '\0')
		{
			return;
		}

		S_Sound(CHAN_VOICE, CHANF_UI, sound, snd_menuvolume, ATTN_NONE);
	}

	static void PlayWheelHaptics(const VRMode* vrmode, int hand, float intensity)
	{
		if (vrmode == nullptr)
		{
			return;
		}

		vrmode->Vibrate(35.0f, hand, intensity);
	}

	static int GetPreferredAnchorHand(EVRWheelType type)
	{
		if (type == EVRWheelType::Weapon)
		{
			return vr_wheel_switch_hands ? VR_OFFHAND : VR_MAINHAND;
		}
		return vr_wheel_switch_hands ? VR_MAINHAND : VR_OFFHAND;
	}

	static bool GetHandPose(player_t* player, int abstractHand, DVector3& pos, DVector3& dir)
	{
		if (player == nullptr || player->mo == nullptr)
		{
			return false;
		}

		if (abstractHand == VR_OFFHAND)
		{
			pos = player->mo->OffhandPos;
			dir = AngleToVector(player->mo->OffhandAngle + DAngle::fromDeg(90.0), player->mo->OffhandPitch);
		}
		else
		{
			pos = player->mo->AttackPos;
			dir = AngleToVector(player->mo->AttackAngle + DAngle::fromDeg(90.0), player->mo->AttackPitch);
		}
		return true;
	}

	static void GetHandAimAngles(player_t* player, int abstractHand, DAngle& yaw, DAngle& pitch)
	{
		if (abstractHand == VR_OFFHAND)
		{
			yaw = player->mo->OffhandAngle + DAngle::fromDeg(90.0);
			pitch = player->mo->OffhandPitch;
		}
		else
		{
			yaw = player->mo->AttackAngle + DAngle::fromDeg(90.0);
			pitch = player->mo->AttackPitch;
		}
	}

	static DVector3 GetHeadAnchorOrigin()
	{
		if (r_viewpoint.CenterEyePos.LengthSquared() > 1e-8)
		{
			return r_viewpoint.CenterEyePos;
		}
		return r_viewpoint.Pos;
	}

	static void GetHeadAnchorBasis(DVector3& forward, DVector3& right, DVector3& up)
	{
		forward = AngleToVector(r_viewpoint.Angles.Yaw, r_viewpoint.Angles.Pitch);
		if (forward.LengthSquared() <= 1e-8)
		{
			forward = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
		}
		if (forward.LengthSquared() <= 1e-8)
		{
			forward = { 1.0, 0.0, 0.0 };
		}
		forward.MakeUnit();

		const DVector3 worldUp = { 0.0, 0.0, 1.0 };
		right = worldUp ^ forward;
		if (right.LengthSquared() <= 1e-8)
		{
			right = AngleToVector(r_viewpoint.Angles.Yaw + DAngle::fromDeg(90.0), nullAngle);
		}
		if (right.LengthSquared() <= 1e-8)
		{
			right = { 0.0, 1.0, 0.0 };
		}
		right.MakeUnit();
		up = forward ^ right;
		up.MakeUnit();
	}

	static bool GetControllerAnchoredCenter(player_t* player, int abstractHand, DVector3& center)
	{
		DVector3 handPos;
		DVector3 handDir;
		if (!GetHandPose(player, abstractHand, handPos, handDir))
		{
			return false;
		}

		const double doomUnitsPerMeter = 60.0;
		const double handSign = abstractHand == VR_MAINHAND ? -1.0 : 1.0;
		const DVector3 worldUp = { 0.0, 0.0, 1.0 };
		DVector3 cameraForward = GetHeadAnchorOrigin() - handPos;
		cameraForward.Z = 0.0;
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
			cameraForward.Z = 0.0;
		}
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = { 1.0, 0.0, 0.0 };
		}
		cameraForward.MakeUnit();

		DVector3 right = worldUp ^ cameraForward;
		if (right.LengthSquared() <= 1e-8)
		{
			right = { 1.0, 0.0, 0.0 };
		}
		else
		{
			right.MakeUnit();
		}
		DVector3 up = worldUp;
		DVector3 forward;
		forward = up ^ right;
		forward.MakeUnit();

		center = handPos
			+ forward * (max(0.0f, (float)vr_wheel_distance) * doomUnitsPerMeter)
			+ right * (vr_wheel_xoffset * handSign * doomUnitsPerMeter)
			+ up * (vr_wheel_yoffset * doomUnitsPerMeter);
		return true;
	}

	static void CaptureHeadLockedAnchor(const DVector3& center)
	{
		DVector3 forward;
		DVector3 right;
		DVector3 up;
		GetHeadAnchorBasis(forward, right, up);

		const DVector3 offset = center - GetHeadAnchorOrigin();
		GVRWheel.HeadLocalOffset = {
			offset.X * right.X + offset.Y * right.Y + offset.Z * right.Z,
			offset.X * forward.X + offset.Y * forward.Y + offset.Z * forward.Z,
			offset.X * up.X + offset.Y * up.Y + offset.Z * up.Z
		};
	}

	static bool GetHeadLockedCenter(DVector3& center)
	{
		DVector3 forward;
		DVector3 right;
		DVector3 up;
		GetHeadAnchorBasis(forward, right, up);

		center = GetHeadAnchorOrigin()
			+ right * GVRWheel.HeadLocalOffset.X
			+ forward * GVRWheel.HeadLocalOffset.Y
			+ up * GVRWheel.HeadLocalOffset.Z;
		return true;
	}

	static bool GetWheelLayout(DVector3& center, DVector3& right, DVector3& up, DVector3& forward)
	{
		if (!GetHeadLockedCenter(center))
		{
			return false;
		}

		const DVector3 worldUp = { 0.0, 0.0, 1.0 };
		DVector3 cameraForward = GetHeadAnchorOrigin() - center;
		cameraForward.Z = 0.0;
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = AngleToVector(r_viewpoint.Angles.Yaw, nullAngle);
			cameraForward.Z = 0.0;
		}
		if (cameraForward.LengthSquared() <= 1e-8)
		{
			cameraForward = { 1.0, 0.0, 0.0 };
		}
		cameraForward.MakeUnit();

		right = worldUp ^ cameraForward;
		if (right.LengthSquared() <= 1e-8)
		{
			right = { 1.0, 0.0, 0.0 };
		}
		else
		{
			right.MakeUnit();
		}
		up = worldUp;
		forward = up ^ right;
		forward.MakeUnit();
		return true;
	}

	static FGameTexture* ResolveStateIcon(FState* state)
	{
		for (int steps = 0; state != nullptr && steps < 16; ++steps, state = state->GetNextState())
		{
			if (state->sprite <= 0 || state->sprite >= (int)sprites.Size())
			{
				continue;
			}
			if (memcmp(sprites[state->sprite].name, "TNT1", 4) == 0 || memcmp(sprites[state->sprite].name, "NULL", 4) == 0 || sprites[state->sprite].numframes <= state->GetFrame())
			{
				continue;
			}

			bool mirror = false;
			FTextureID texid = sprites[state->sprite].GetSpriteFrame(state->GetFrame(), 0, nullAngle, &mirror);
			if (texid.isValid())
			{
				return TexMan.GetGameTexture(texid, true);
			}
		}
		return nullptr;
	}

	static FGameTexture* ResolveWheelIcon(AActor* item)
	{
		if (item == nullptr)
		{
			return nullptr;
		}

		const FTextureID icon = item->TextureIDVar(NAME_Icon);
		if (icon.isValid())
		{
			return TexMan.GetGameTexture(icon, true);
		}

		if (!item->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			const FTextureID inventoryIcon = FSetTextureID(GetInventoryIcon(item, DI_ALTICONFIRST));
			if (inventoryIcon.isValid())
			{
				return TexMan.GetGameTexture(inventoryIcon, true);
			}
		}

		if (item->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			if (item->SpawnState != nullptr)
			{
				if (auto tex = ResolveStateIcon(item->SpawnState))
				{
					return tex;
				}
			}
			if (auto readyState = item->FindState(NAME_Ready))
			{
				return ResolveStateIcon(readyState);
			}
		}
		return nullptr;
	}

	static bool IsWheelWeaponUsable(AActor* weapon)
	{
		if (weapon == nullptr || !weapon->GetClass()->IsDescendantOf(NAME_Weapon))
		{
			return false;
		}

		// Match the game's own usable-weapon filtering as closely as we can from C++.
		if (weapon->IntVar(NAME_WeaponFlags) & WIF_POWERED_UP)
		{
			return false;
		}

		auto sisterWeapon = weapon->PointerVar<AActor>(NAME_SisterWeapon);
		if (sisterWeapon != nullptr && weapon->GetClass()->IsDescendantOf(sisterWeapon->GetClass()))
		{
			return false;
		}

		auto ammo1 = weapon->PointerVar<AActor>(NAME_Ammo1);
		auto ammo2 = weapon->PointerVar<AActor>(NAME_Ammo2);
		const int weaponFlags = weapon->IntVar(NAME_WeaponFlags);

		if (!(weaponFlags & WIF_AMMO_OPTIONAL))
		{
			if (ammo1 != nullptr)
			{
				const int use1 = weapon->IntVar(NAME_AmmoUse1);
				if (use1 > 0 && ammo1->IntVar(NAME_Amount) < use1)
				{
					return false;
				}
			}

			if ((weaponFlags & WIF_PRIMARY_USES_BOTH) && ammo2 != nullptr)
			{
				const int use2 = weapon->IntVar("AmmoUse2");
				if (use2 > 0 && ammo2->IntVar(NAME_Amount) < use2)
				{
					return false;
				}
			}
		}

		if (weapon->IntVar(NAME_MinSelAmmo1) > 0 && (ammo1 == nullptr || ammo1->IntVar(NAME_Amount) < weapon->IntVar(NAME_MinSelAmmo1)))
		{
			return false;
		}

		if (weapon->IntVar("MinSelAmmo2") > 0 && (ammo2 == nullptr || ammo2->IntVar(NAME_Amount) < weapon->IntVar("MinSelAmmo2")))
		{
			return false;
		}

		return true;
	}

	static void AddWheelEntry(TArray<VRWheelEntry>& entries, AActor* item, bool owned, bool selectable)
	{
		auto icon = ResolveWheelIcon(item);
		if (item == nullptr || icon == nullptr)
		{
			return;
		}

		VRWheelEntry entry;
		entry.Item = item;
		entry.Icon = icon;
		entry.Selectable = selectable;
		entry.Owned = owned;
		entries.Push(entry);
	}

	static void BuildWeaponEntries(player_t* player, TArray<VRWheelEntry>& out)
	{
		out.Clear();
		if (player == nullptr || player->mo == nullptr)
		{
			return;
		}

		for (int slot = 0; slot < NUM_WEAPON_SLOTS; ++slot)
		{
			for (int index = 0; index < player->weapons.SlotSize(slot); ++index)
			{
				auto weapType = player->weapons.GetWeapon(slot, index);
				if (weapType == nullptr)
				{
					continue;
				}

				auto owned = player->mo->FindInventory(weapType);
				AActor* weapon = owned != nullptr ? owned : GetDefaultByType(weapType);
				if (weapon == nullptr || !IsWheelWeaponUsable(weapon))
				{
					continue;
				}

				if (!vr_wheel_weapon_all && owned == nullptr)
				{
					continue;
				}
				AddWheelEntry(out, weapon, owned != nullptr, owned != nullptr);
			}
		}
	}

	static void BuildInventoryEntries(player_t* player, TArray<VRWheelEntry>& out)
	{
		out.Clear();
		if (player == nullptr || player->mo == nullptr)
		{
			return;
		}

		for (AActor* inv = player->mo->Inventory; inv != nullptr; inv = inv->Inventory)
		{
			if (inv->GetClass()->IsDescendantOf(NAME_Weapon) || !inv->BoolVar("bInvBar") || inv->IntVar(NAME_Amount) <= 0)
			{
				continue;
			}
			AddWheelEntry(out, inv, true, true);
		}
	}

	static void RefreshEntries(player_t* player)
	{
		if (GVRWheel.Type == EVRWheelType::Weapon)
		{
			BuildWeaponEntries(player, GVRWheel.Entries);
		}
		else if (GVRWheel.Type == EVRWheelType::Inventory)
		{
			BuildInventoryEntries(player, GVRWheel.Entries);
		}
		else
		{
			GVRWheel.Entries.Clear();
		}
	}

	static void ResetWheel()
	{
		GVRWheel = {};
	}

	static void SetGameTimeScale(double scale)
	{
		FString value;
		value.Format("%g", scale);
		cvar_set("i_timescale", value.GetChars());
	}

	static void ApplyWheelTimeControl()
	{
		if (GVRWheel.TimeControlActive)
		{
			return;
		}

		GVRWheel.SavedTimeScale = i_timescale;
		GVRWheel.TimeControlActive = true;
		GVRWheel.TimeControlFrozen = false;

		if (vr_wheel_time_slow <= 0.0f)
		{
			GVRWheel.TimeControlFrozen = true;
			I_FreezeTime(true);
			return;
		}

		SetGameTimeScale(vr_wheel_time_slow);
	}

	static void ReleaseWheelTimeControl()
	{
		if (!GVRWheel.TimeControlActive)
		{
			return;
		}

		if (GVRWheel.TimeControlFrozen)
		{
			I_FreezeTime(false);
		}

		SetGameTimeScale(GVRWheel.SavedTimeScale);
		GVRWheel.TimeControlActive = false;
		GVRWheel.TimeControlFrozen = false;
	}

	static void OpenWheel(EVRWheelType type)
	{
		auto vrmode = VRMode::GetVRModeCached(true);
		auto player = r_viewpoint.camera ? r_viewpoint.camera->player : &players[consoleplayer];
		if (vrmode == nullptr || !vrmode->IsVR() || player == nullptr || player->mo == nullptr)
		{
			return;
		}

		const int anchorHand = GetPreferredAnchorHand(type);
		DVector3 initialCenter;
		if (!GetControllerAnchoredCenter(player, anchorHand, initialCenter))
		{
			return;
		}

		TArray<VRWheelEntry> entries;
		if (type == EVRWheelType::Inventory)
		{
			BuildInventoryEntries(player, entries);
			if (entries.Size() == 0)
			{
				return;
			}
		}

		GVRWheel.Type = type;
		GVRWheel.AnchorHand = anchorHand;
		CaptureHeadLockedAnchor(initialCenter);
		GetHandAimAngles(player, anchorHand, GVRWheel.OpenYaw, GVRWheel.OpenPitch);
		GVRWheel.HoveredIndex = -1;
		GVRWheel.HoverValid = false;
		ApplyWheelTimeControl();
		if (type == EVRWheelType::Inventory)
		{
			GVRWheel.Entries = entries;
		}
		else
		{
			RefreshEntries(player);
		}

		UpdateHover(player);
		PlayWheelSound("menu/activate");
		PlayWheelHaptics(vrmode, GVRWheel.AnchorHand, 0.20f);
	}

	static void CommitWheelSelection()
	{
		auto player = &players[consoleplayer];
		if (!GVRWheel.HoverValid || GVRWheel.HoveredIndex < 0 || GVRWheel.HoveredIndex >= GVRWheel.Entries.Size())
		{
			return;
		}

		const auto& entry = GVRWheel.Entries[GVRWheel.HoveredIndex];
		if (!entry.Selectable || entry.Item == nullptr || player == nullptr || player->mo == nullptr)
		{
			return;
		}

		if (GVRWheel.Type == EVRWheelType::Weapon)
		{
			auto weapon = entry.Item->IsKindOf(NAME_Weapon) ? entry.Item : player->mo->FindInventory(entry.Item->GetClass());
			if (weapon != nullptr && weapon != player->ReadyWeapon)
			{
				player->PendingWeapon = weapon;
			}
		}
		else if (GVRWheel.Type == EVRWheelType::Inventory)
		{
			player->mo->PointerVar<AActor>(NAME_InvSel) = entry.Item;
			player->inventorytics = 0;
			SendItemUse = entry.Item;
		}
	}

	static void CloseWheel(EVRWheelType type)
	{
		if (GVRWheel.Type != type)
		{
			return;
		}
		CommitWheelSelection();
		ReleaseWheelTimeControl();
		PlayWheelSound("menu/clear");
		ResetWheel();
	}

	static void GetIconQuadSize(FGameTexture* texture, float maxSize, float& outWidth, float& outHeight)
	{
		outWidth = maxSize;
		outHeight = maxSize;
		if (texture == nullptr)
		{
			return;
		}

		const double texWidth = max<double>(1.0, texture->GetDisplayWidth());
		const double texHeight = max<double>(1.0, texture->GetDisplayHeight());
		if (texWidth >= texHeight)
		{
			outHeight = (float)(maxSize * (texHeight / texWidth));
		}
		else
		{
			outWidth = (float)(maxSize * (texWidth / texHeight));
		}
	}

	static void DrawWorldDisc(HWDrawInfo* di, FRenderState& state, const DVector3& center, const DVector3& right, const DVector3& up, float radius, PalEntry color)
	{
		if (di == nullptr || radius <= 0.0f)
		{
			return;
		}

		state.SetLightIndex(-1);
		state.SetRenderStyle(STYLE_Translucent);
		state.AlphaFunc(Alpha_Greater, 0.0f);
		state.SetTextureMode(TM_NORMAL);
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.mModelMatrix.loadIdentity();
		state.EnableModelMatrix(false);
		state.SetVertexBuffer(screen->mVertexData);
		state.EnableBrightmap(false);
		state.EnableDepthTest(true);
		state.SetDepthMask(false);
		state.EnableTexture(false);
		state.SetColor(color);

		static constexpr int Segments = 24;
		screen->mVertexData->Map();
		auto vert = screen->mVertexData->AllocVertices(Segments + 2);
		auto vp = vert.first;
		vp[0].Set((float)center.X, (float)center.Z, (float)center.Y, 0.5f, 0.5f);
		for (int i = 0; i <= Segments; ++i)
		{
			const double ang = (2.0 * M_PI * double(i)) / double(Segments);
			const DVector3 point = center + right * (cos(ang) * radius) + up * (sin(ang) * radius);
			vp[i + 1].Set((float)point.X, (float)point.Z, (float)point.Y, 0.5f, 0.5f);
		}
		screen->mVertexData->Unmap();
		state.Draw(DT_TriangleFan, vert.second, Segments + 2);
	}

	static void DrawWorldQuad(HWDrawInfo* di, FRenderState& state, const DVector3& center, const DVector3& right, const DVector3& up, float width, float height, FGameTexture* texture, PalEntry color, bool textured, bool rotate180 = false)
	{
		if (di == nullptr || width <= 0.0f || height <= 0.0f)
		{
			return;
		}

		const DVector3 halfRight = right * (width * 0.5);
		const DVector3 halfUp = up * (height * 0.5);
		const DVector3 corners[4] =
		{
			center - halfRight - halfUp,
			center + halfRight - halfUp,
			center - halfRight + halfUp,
			center + halfRight + halfUp,
		};

		state.SetLightIndex(-1);
		state.SetRenderStyle(STYLE_Translucent);
		state.AlphaFunc(Alpha_Greater, 0.0f);
		state.SetTextureMode(TM_NORMAL);
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.mModelMatrix.loadIdentity();
		state.EnableModelMatrix(false);
		state.SetVertexBuffer(screen->mVertexData);
		state.EnableBrightmap(false);
		state.EnableDepthTest(true);
		state.SetDepthMask(false);

		if (textured && texture != nullptr)
		{
			state.SetColorAlpha(0xffffff, color.a / 255.0f, 0);
			state.EnableTexture(true);
			state.SetMaterial(texture, UF_Texture, 0, CLAMP_XY_NOMIP, 0, -1);
		}
		else
		{
			state.SetColor(color);
			state.EnableTexture(false);
		}

		screen->mVertexData->Map();
		auto vert = screen->mVertexData->AllocVertices(4);
		auto vp = vert.first;
		const float u0 = rotate180 ? 1.0f : 0.0f;
		const float u1 = rotate180 ? 0.0f : 1.0f;
		const float v0 = rotate180 ? 1.0f : 0.0f;
		const float v1 = rotate180 ? 0.0f : 1.0f;
		for (int i = 0; i < 4; ++i)
		{
			const float u = (i & 1) ? u1 : u0;
			const float v = (i >= 2) ? v1 : v0;
			vp[i].Set((float)corners[i].X, (float)corners[i].Z, (float)corners[i].Y, u, v);
		}
		screen->mVertexData->Unmap();
		state.Draw(DT_TriangleStrip, vert.second, 4);
	}

	static void UpdateHover(player_t* player)
	{
		GVRWheel.HoveredIndex = -1;
		GVRWheel.HoverValid = false;
		if (player == nullptr || player->mo == nullptr || GVRWheel.Entries.Size() == 0)
		{
			return;
		}

		auto vrmode = VRMode::GetVRModeCached(true);
		if (vrmode == nullptr || !vrmode->IsVR())
		{
			return;
		}

		DAngle aimYaw;
		DAngle aimPitch;
		GetHandAimAngles(player, GVRWheel.AnchorHand, aimYaw, aimPitch);

		const double selectAngle = max(5.0f, (float)vr_wheel_select_angle);
		double x = sin((aimYaw - GVRWheel.OpenYaw).Radians()) / sin(DAngle::fromDeg(selectAngle).Radians());
		double y = (aimPitch - GVRWheel.OpenPitch).Degrees() / selectAngle;
		const double len = sqrt(x * x + y * y);
		if (len > 1.0)
		{
			x /= len;
			y /= len;
		}

		if (len < clamp<float>(vr_wheel_deadzone, 0.05f, 0.95f))
		{
			return;
		}

		const double slice = (2.0 * M_PI) / double(GVRWheel.Entries.Size());
		double angle = atan2(y, x) - M_PI * 0.5;
		if (angle < 0.0) angle += 2.0 * M_PI;
		const int hover = int(angle / slice) % GVRWheel.Entries.Size();
		if (hover >= 0 && hover < GVRWheel.Entries.Size())
		{
			GVRWheel.HoveredIndex = hover;
			GVRWheel.HoverValid = GVRWheel.Entries[hover].Selectable;
		}
	}

}

void VRWheel_OpenWeapon()
{
	OpenWheel(EVRWheelType::Weapon);
}

void VRWheel_CloseWeapon()
{
	CloseWheel(EVRWheelType::Weapon);
}

void VRWheel_OpenInventory()
{
	OpenWheel(EVRWheelType::Inventory);
}

void VRWheel_CloseInventory()
{
	CloseWheel(EVRWheelType::Inventory);
}

bool VRWheel_IsActive()
{
	return GVRWheel.Type != EVRWheelType::None;
}

bool VRWheel_ShouldSuppressGameplayInput()
{
	return VRWheel_IsActive();
}

bool VRWheel_ShouldSuppressWeaponHand(int hand)
{
	return vr_wheel_hide_hand_weapon && VRWheel_IsActive() && GVRWheel.AnchorHand == hand;
}

bool VRWheel_GetTransform(VSMatrix& out)
{
	if (!VRWheel_IsActive())
	{
		return false;
	}

	DVector3 center;
	if (!GetHeadLockedCenter(center))
	{
		return false;
	}

	out.loadIdentity();
	out.translate((FLOATTYPE)center.X, (FLOATTYPE)center.Z, (FLOATTYPE)center.Y);
	GVRWheel.Transform = out;
	return true;
}

void VRWheel_Draw(HWDrawInfo* di, FRenderState& state)
{
	if (di == nullptr)
	{
		return;
	}

	auto vrmode = VRMode::GetVRModeCached(true);
	if (vrmode == nullptr || !vrmode->IsVR())
	{
		return;
	}

	auto player = r_viewpoint.camera ? r_viewpoint.camera->player : &players[consoleplayer];
	if (player == nullptr || player->mo == nullptr || player->health <= 0)
	{
		return;
	}

	if (menuactive != MENU_Off || ConsoleState != c_up || VR_UseScreenLayer())
	{
		return;
	}

	if (!VRWheel_IsActive())
	{
		return;
	}

	RefreshEntries(player);
	UpdateHover(player);
	if (GVRWheel.Entries.Size() == 0)
	{
		return;
	}

	DVector3 center;
	DVector3 wheelRight;
	DVector3 wheelUp;
	DVector3 wheelForward;
	if (!GetWheelLayout(center, wheelRight, wheelUp, wheelForward))
	{
		return;
	}

	const int count = GVRWheel.Entries.Size();
	const float ringRadius = max(5.0f, (float)vr_wheel_radius);
	const float iconSize = clamp<float>(2.0f * max(0.1f, (float)vr_wheel_icon_scale), 0.5f, 4.0f);
	const float backdropSize = iconSize * 1.45f;
	const double slice = (2.0 * M_PI) / double(count);
	const PalEntry bgColor = PalEntry(MAKEARGB(128,
		RPART(vr_wheel_icon_bg_color),
		GPART(vr_wheel_icon_bg_color),
		BPART(vr_wheel_icon_bg_color)));
	const PalEntry selectedBgColor = PalEntry(MAKEARGB(160,
		RPART(vr_wheel_icon_select_color),
		GPART(vr_wheel_icon_select_color),
		BPART(vr_wheel_icon_select_color)));

	for (int i = 0; i < count; ++i)
	{
		const auto& entry = GVRWheel.Entries[i];
		const double angle = (M_PI * 0.5) - (slice * i);
		const DVector3 iconCenter = center
			+ wheelRight * (cos(angle) * ringRadius)
			+ wheelUp * (sin(angle) * ringRadius);

		PalEntry iconColor = entry.Selectable ? PalEntry(235, 255, 255, 255) : PalEntry(115, 180, 180, 180);
		if (i == GVRWheel.HoveredIndex)
		{
			iconColor = PalEntry(255, 255, 255, 255);
		}

		float iconWidth = iconSize;
		float iconHeight = iconSize;
		GetIconQuadSize(entry.Icon, iconSize, iconWidth, iconHeight);

		DrawWorldDisc(di, state, iconCenter, wheelRight, wheelUp, backdropSize * 0.50f, i == GVRWheel.HoveredIndex ? selectedBgColor : bgColor);
		DrawWorldQuad(di, state, iconCenter, wheelRight, wheelUp, iconWidth, iconHeight, entry.Icon, iconColor, true, true);
	}

	state.EnableTexture(true);
	state.EnableBrightmap(true);
	state.SetRenderStyle(STYLE_Translucent);
	state.SetTextureMode(TM_NORMAL);
	state.ResetColor();
	state.SetObjectColor(0xffffffff);
	state.SetAddColor(0);
	state.AlphaFunc(Alpha_GEqual, gl_mask_sprite_threshold);
	state.EnableModelMatrix(false);
	state.EnableDepthTest(true);
	state.SetDepthMask(true);
}
