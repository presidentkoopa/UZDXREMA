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
#include "vm.h"
#include "playsim/p_pspr.h"
#include <QzDoom/VrCommon.h>
#include "hw_vrmodes.h"
#include "r_data/models.h"
#include "rendering/hwrenderer/scene/hw_drawinfo.h"
#include "rendering/hwrenderer/hw_models.h"
#include "r_data/sprites.h"
#include "r_utility.h"

EXTERN_CVAR(Int, vr_control_scheme)
EXTERN_CVAR(Float, i_timescale)
EXTERN_CVAR(Float, gl_mask_sprite_threshold)

CVAR(Bool, vr_wheel_weapon_all, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_switch_hands, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_hide_hand_weapon, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_sound, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_icon_load_model, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_wheel_auto_split, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_bg_color, (int)MAKEARGB(128, 63, 63, 63), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_select_color, (int)MAKEARGB(160, 255, 208, 0), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_wheel_icon_disable_color, (int)MAKEARGB(160, 96, 16, 16), CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_distance, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
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
CVAR(Float, vr_wheel_icon_scale, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_scale, 0.8f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_yaw, -135.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_xoffset, -40.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_icon_model_zoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_wheel_select_angle, 30.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_wheel_selection_type, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

void RenderFrameModels(FModelRenderer* renderer, FLevelLocals* Level, const FSpriteModelFrame* smf, const FState* curState, const int curTics, FTranslationID translation, AActor* actor);

namespace
{
	enum class EVRWheelType
	{
		None,
		MainWeapon,
		OffhandWeapon,
		Inventory
	};

	struct VRWheelEntry
	{
		AActor* Item = nullptr;
		FGameTexture* Icon = nullptr;
		FSpriteModelFrame* ModelFrame = nullptr;
		FState* ModelState = nullptr;
		bool Selectable = false;
		bool Owned = false;
	};

	struct VRWheelState
	{
		EVRWheelType Type = EVRWheelType::None;
		int AnchorHand = VR_MAINHAND;
		AActor* Owner = nullptr;
		FLevelLocals* Level = nullptr;
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

	struct VRWheelRingLayout
	{
		int StartIndex = 0;
		int Count = 0;
		float Radius = 0.0f;
		float IconSize = 0.0f;
		double AngleOffset = 0.0;
	};

	struct VRWheelLayoutInfo
	{
		int RingCount = 0;
		VRWheelRingLayout Rings[2];
	};

	VRWheelState GVRWheel;

	static void UpdateHover(player_t* player);
	static void ReleaseWheelTimeControl();
	static void MoveWeaponToHand(player_t* player, AActor* weapon, bool targetOffhand)
	{
		if (player == nullptr || player->mo == nullptr || weapon == nullptr)
		{
			return;
		}

		IFVIRTUALPTRNAME(player->mo, NAME_PlayerPawn, MoveWeaponToHand)
		{
			VMValue param[] = { player->mo, weapon, targetOffhand ? 1 : 0 };
			VMCall(func, param, 3, nullptr, 0);
		}
	}

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

		if (hand != VR_MAINHAND && hand != VR_OFFHAND)
		{
			hand = VR_MAINHAND;
		}

		const bool rightHanded = vr_control_scheme < 10;
		const int hapticChannel = rightHanded
			? (hand == VR_MAINHAND ? 1 : 0)
			: hand;

		vrmode->Vibrate(35.0f, hapticChannel, intensity);
	}

	static int GetPreferredAnchorHand(EVRWheelType type)
	{
		if (type == EVRWheelType::MainWeapon)
		{
			return VR_MAINHAND;
		}
		if (type == EVRWheelType::OffhandWeapon)
		{
			return VR_OFFHAND;
		}
		return vr_wheel_switch_hands ? VR_MAINHAND : VR_OFFHAND;
	}

	static bool IsWeaponWheelType(EVRWheelType type)
	{
		return type == EVRWheelType::MainWeapon || type == EVRWheelType::OffhandWeapon;
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

	static bool UseCinemaWheelOverride()
	{
		return VR_UseCinematicScreenLayer();
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

	static bool GetTouchPoint(player_t* player, DVector3& out)
	{
		DVector3 unusedDir;
		return GetHandPose(player, GVRWheel.AnchorHand, out, unusedDir);
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

	static FState* FindFirstUsableStateFrame(FState* state)
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
			return state;
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

	static FSpriteModelFrame* ResolveWheelModel(AActor* item, bool owned, FState*& outState)
	{
		outState = nullptr;
		if (!vr_wheel_icon_load_model || item == nullptr)
		{
			return nullptr;
		}

		// For wheel models icons, probe the first usable non-empty frame from Ready state
		outState = FindFirstUsableStateFrame(item->FindState(NAME_Ready));
		if (outState == nullptr)
		{
			return nullptr;
		}

		auto modelFrame = FindModelFrame(item, outState->sprite, outState->GetFrame(), false);
		if (modelFrame != nullptr && owned && item->Level != nullptr)
		{
			return modelFrame;
		}

		outState = nullptr;
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
		FState* modelState = nullptr;
		auto modelFrame = ResolveWheelModel(item, owned, modelState);
		if (item == nullptr || (icon == nullptr && modelFrame == nullptr))
		{
			return;
		}

		VRWheelEntry entry;
		entry.Item = item;
		entry.Icon = icon;
		entry.ModelFrame = modelFrame;
		entry.ModelState = modelState;
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
		if (IsWeaponWheelType(GVRWheel.Type))
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

	static bool IsWheelOwnerValid(player_t* player)
	{
		if (GVRWheel.Type == EVRWheelType::None)
		{
			return true;
		}

		if (player == nullptr || player->mo == nullptr)
		{
			return false;
		}

		return GVRWheel.Owner == player->mo && GVRWheel.Level == player->mo->Level;
	}

	static void InvalidateWheelIfOwnerChanged(player_t* player)
	{
		if (IsWheelOwnerValid(player))
		{
			return;
		}

		ReleaseWheelTimeControl();
		ResetWheel();
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
		auto player = &players[consoleplayer];
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
		GVRWheel.Owner = player->mo;
		GVRWheel.Level = player->mo->Level;
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

		if (IsWeaponWheelType(GVRWheel.Type))
		{
			auto weapon = player->mo->FindInventory(entry.Item->GetClass());
			if (weapon != nullptr)
			{
				const bool targetOffhand = GVRWheel.Type == EVRWheelType::OffhandWeapon;
				MoveWeaponToHand(player, weapon, targetOffhand);
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
		state.ResetColor();
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.SetDynLight(0, 0, 0);
		state.SetNoSoftLightLevel();
		state.SetLightParms(1.f, 0.f);
		state.EnableFog(false);
		state.SetFog(0, 0);
		state.ResetFadeColor();
		state.EnableTextureMatrix(false);
		state.mModelMatrix.loadIdentity();
		state.EnableModelMatrix(false);
		state.SetVertexBuffer(screen->mVertexData);
		state.EnableBrightmap(false);
		state.EnableDepthTest(false);
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
		state.ResetColor();
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		state.SetDynLight(0, 0, 0);
		state.SetNoSoftLightLevel();
		state.SetLightParms(1.f, 0.f);
		state.EnableFog(false);
		state.SetFog(0, 0);
		state.ResetFadeColor();
		state.EnableTextureMatrix(false);
		state.mModelMatrix.loadIdentity();
		state.EnableModelMatrix(false);
		state.SetVertexBuffer(screen->mVertexData);
		state.EnableBrightmap(false);
		state.EnableDepthTest(false);
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

	static void DrawWheelModel(HWDrawInfo* di, FRenderState& state, const VRWheelEntry& entry, const DVector3& center, const DVector3& wheelForward, float iconSize)
	{
		if (di == nullptr || entry.Item == nullptr || entry.ModelFrame == nullptr || entry.ModelState == nullptr || entry.Item->Level == nullptr)
		{
			return;
		}

		const unsigned int smfFlags = entry.ModelFrame->getFlags(entry.Item->modelData);
		FTranslationID translation = NO_TRANSLATION;
		if (!(smfFlags & MDL_IGNORETRANSLATION))
		{
			translation = entry.Item->Translation;
		}

		const float wheelModelScale = max(0.01f, iconSize * 0.025f * max(0.01f, (float)vr_wheel_icon_model_scale));
		const float scaleFactorX = -entry.Item->Scale.X * entry.ModelFrame->xscale * wheelModelScale;
		const float scaleFactorY = entry.Item->Scale.X * entry.ModelFrame->yscale * wheelModelScale;
		const float scaleFactorZ = entry.Item->Scale.Y * entry.ModelFrame->zscale * wheelModelScale;
		const float yaw = (float)wheelForward.Angle().Degrees();

		VSMatrix objectToWorldMatrix;
		objectToWorldMatrix.loadIdentity();
		objectToWorldMatrix.translate((FLOATTYPE)center.X, (FLOATTYPE)center.Z, (FLOATTYPE)center.Y);
		objectToWorldMatrix.rotate(-(yaw - (float)vr_wheel_icon_model_yaw), 0, 1, 0);
		objectToWorldMatrix.scale(scaleFactorX, scaleFactorZ, scaleFactorY);
		objectToWorldMatrix.translate(
			vr_wheel_icon_model_xoffset + (entry.ModelFrame->xoffset / entry.ModelFrame->xscale),
			entry.ModelFrame->zoffset / entry.ModelFrame->zscale,
			vr_wheel_icon_model_zoffset + (entry.ModelFrame->yoffset / entry.ModelFrame->yscale));
		objectToWorldMatrix.rotate(-entry.ModelFrame->angleoffset, 0, 1, 0);
		objectToWorldMatrix.rotate(entry.ModelFrame->pitchoffset, 0, 0, 1);
		objectToWorldMatrix.rotate(-entry.ModelFrame->rolloffset, 1, 0, 0);

		FHWModelRenderer renderer(di, state, -1);
		const bool mirrored = (scaleFactorX * scaleFactorY * scaleFactorZ) < 0.0f;
		state.SetRenderStyle(STYLE_Normal);
		state.EnableTexture(true);
		state.SetTextureMode(TM_NORMAL);
		state.AlphaFunc(Alpha_GEqual, 0.5f);
		state.SetColorAlpha(0xffffff, 1.0f, 0);
		state.SetNoSoftLightLevel();
		state.SetLightParms(1.f, 0.f);
		state.EnableFog(false);
		state.SetFog(0, 0);
		state.SetDynLight(0, 0, 0);
		state.ResetFadeColor();
		state.EnableTextureMatrix(false);
		state.EnableDepthTest(true);
		state.SetDepthMask(true);
		state.EnableBrightmap(true);
		state.SetCulling(Cull_None);
		state.ClearDepthBias();
		state.ResetColor();
		state.SetObjectColor(0xffffffff);
		state.SetAddColor(0);
		renderer.BeginDrawModel(DefaultRenderStyle(), (int)smfFlags, objectToWorldMatrix, mirrored);
		RenderFrameModels(&renderer, entry.Item->Level, entry.ModelFrame, entry.ModelState, 0, translation, entry.Item);
		renderer.EndDrawModel(DefaultRenderStyle(), (int)smfFlags);
		state.SetVertexBuffer(screen->mVertexData);
	}

	static float GetWheelIconSizeForCount(int count, float ringRadius)
	{
		const float baseSize = clamp<float>(2.0f * max(0.1f, (float)vr_wheel_icon_scale), 0.5f, 4.0f);
		if (count <= 1)
		{
			return baseSize;
		}

		// Keep the default feel for small wheels, then shrink toward the slot chord
		// length once the wheel becomes crowded.
		const double slotChord = 2.0 * double(ringRadius) * sin(M_PI / double(count));
		const float maxBackdropSize = max(0.4f, float(slotChord * 0.82));
		const float autoSize = clamp<float>(maxBackdropSize / 1.45f, 0.25f, baseSize);
		return autoSize;
	}

	static VRWheelLayoutInfo BuildWheelLayoutInfo(int count)
	{
		VRWheelLayoutInfo layout = {};
		if (count <= 0)
		{
			return layout;
		}

		const float innerRadius = max(5.0f, (float)vr_wheel_radius);
		if (!vr_wheel_auto_split || count <= 15)
		{
			layout.RingCount = 1;
			layout.Rings[0].StartIndex = 0;
			layout.Rings[0].Count = count;
			layout.Rings[0].Radius = innerRadius;
			layout.Rings[0].IconSize = GetWheelIconSizeForCount(count, innerRadius);
			layout.Rings[0].AngleOffset = 0.0;
			return layout;
		}

		const int innerCount = (count + 1) / 2;
		const int outerCount = count - innerCount;
		layout.RingCount = outerCount > 0 ? 2 : 1;
		layout.Rings[0].StartIndex = 0;
		layout.Rings[0].Count = innerCount;
		layout.Rings[0].Radius = innerRadius;
		layout.Rings[0].IconSize = GetWheelIconSizeForCount(innerCount, innerRadius);
		layout.Rings[0].AngleOffset = 0.0;

		if (outerCount > 0)
		{
			const float innerBackdrop = layout.Rings[0].IconSize * 1.45f;
			float outerRadius = innerRadius + innerBackdrop + 2.0f;
			float outerIconSize = GetWheelIconSizeForCount(outerCount, outerRadius);
			const float outerBackdrop = outerIconSize * 1.45f;
			outerRadius = innerRadius + (innerBackdrop * 0.65f) + (outerBackdrop * 0.65f) + 1.25f;
			outerIconSize = GetWheelIconSizeForCount(outerCount, outerRadius);

			layout.Rings[1].StartIndex = innerCount;
			layout.Rings[1].Count = outerCount;
			layout.Rings[1].Radius = outerRadius;
			layout.Rings[1].IconSize = outerIconSize;
			layout.Rings[1].AngleOffset = outerCount > 0 ? (M_PI / double(outerCount)) : 0.0;
		}

		return layout;
	}

	static const VRWheelRingLayout* FindRingForEntry(const VRWheelLayoutInfo& layout, int index, int& localIndex)
	{
		for (int ring = 0; ring < layout.RingCount; ++ring)
		{
			const auto& ringLayout = layout.Rings[ring];
			if (index >= ringLayout.StartIndex && index < ringLayout.StartIndex + ringLayout.Count)
			{
				localIndex = index - ringLayout.StartIndex;
				return &ringLayout;
			}
		}
		localIndex = -1;
		return nullptr;
	}

	static double GetWheelEntryAngle(const VRWheelRingLayout& ring, int localIndex)
	{
		const double slice = (2.0 * M_PI) / double(max(1, ring.Count));
		return (M_PI * 0.5) - (slice * localIndex) + ring.AngleOffset;
	}

	static double DotProduct(const DVector3& a, const DVector3& b)
	{
		return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
	}

	static int GetAimRingIndex(player_t* player, const VRWheelLayoutInfo& layout, const DVector3& center, const DVector3& wheelRight, const DVector3& wheelUp)
	{
		if (layout.RingCount <= 1)
		{
			return 0;
		}

		DVector3 touchPoint;
		if (!GetTouchPoint(player, touchPoint))
		{
			return 0;
		}

		const DVector3 delta = touchPoint - center;
		const double planeX = DotProduct(delta, wheelRight);
		const double planeY = DotProduct(delta, wheelUp);
		const double radialDistance = sqrt((planeX * planeX) + (planeY * planeY));
		const double switchRadius = layout.Rings[0].Radius * 0.58;
		return radialDistance <= switchRadius ? 0 : 1;
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

		if (!UseCinemaWheelOverride() && vr_wheel_selection_type == 0)
		{
			DVector3 center;
			DVector3 wheelRight;
			DVector3 wheelUp;
			DVector3 wheelForward;
			if (!GetWheelLayout(center, wheelRight, wheelUp, wheelForward))
			{
				return;
			}

			DVector3 touchPoint;
			if (!GetTouchPoint(player, touchPoint))
			{
				return;
			}

			const int count = GVRWheel.Entries.Size();
			const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(count);

			int hoveredIndex = -1;
			double hoveredDistanceSq = DBL_MAX;
			for (int i = 0; i < count; ++i)
			{
				int localIndex = -1;
				const auto* ring = FindRingForEntry(layout, i, localIndex);
				if (ring == nullptr)
				{
					continue;
				}

				const float backdropSize = ring->IconSize * 1.45f;
				const float iconRadius = backdropSize * 0.50f;
				const float touchRadius = backdropSize * 0.25f;
				const float selectDistance = iconRadius + touchRadius;
				const double angle = GetWheelEntryAngle(*ring, localIndex);
				const DVector3 iconCenter = center
					+ wheelRight * (cos(angle) * ring->Radius)
					+ wheelUp * (sin(angle) * ring->Radius);
				const double distanceSq = (touchPoint - iconCenter).LengthSquared();
				if (distanceSq <= double(selectDistance * selectDistance) && distanceSq < hoveredDistanceSq)
				{
					hoveredIndex = i;
					hoveredDistanceSq = distanceSq;
				}
			}

			if (hoveredIndex >= 0)
			{
				GVRWheel.HoveredIndex = hoveredIndex;
				GVRWheel.HoverValid = GVRWheel.Entries[hoveredIndex].Selectable;
			}
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

		const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(GVRWheel.Entries.Size());
		DVector3 center;
		DVector3 wheelRight;
		DVector3 wheelUp;
		DVector3 wheelForward;
		const bool hasWheelLayout = GetWheelLayout(center, wheelRight, wheelUp, wheelForward);
		double angle = atan2(y, x) - M_PI * 0.5;
		if (angle < 0.0) angle += 2.0 * M_PI;
		const int ringIndex = hasWheelLayout ? GetAimRingIndex(player, layout, center, wheelRight, wheelUp) : (layout.RingCount > 1 && len >= 0.75 ? 1 : 0);
		const VRWheelRingLayout& ring = layout.Rings[ringIndex];
		const double ringAngle = angle - ring.AngleOffset;
		const double normalizedAngle = ringAngle < 0.0 ? ringAngle + 2.0 * M_PI : ringAngle;
		const double slice = (2.0 * M_PI) / double(max(1, ring.Count));
		const int localHover = int(normalizedAngle / slice) % max(1, ring.Count);
		const int hover = ring.StartIndex + localHover;
		if (hover >= 0 && hover < GVRWheel.Entries.Size())
		{
			GVRWheel.HoveredIndex = hover;
			GVRWheel.HoverValid = GVRWheel.Entries[hover].Selectable;
		}
	}

}

void VRWheel_OpenWeapon()
{
	OpenWheel(EVRWheelType::MainWeapon);
}

void VRWheel_CloseWeapon()
{
	CloseWheel(EVRWheelType::MainWeapon);
}

void VRWheel_OpenOffhandWeapon()
{
	OpenWheel(EVRWheelType::OffhandWeapon);
}

void VRWheel_CloseOffhandWeapon()
{
	CloseWheel(EVRWheelType::OffhandWeapon);
}

void VRWheel_OpenInventory()
{
	OpenWheel(EVRWheelType::Inventory);
}

void VRWheel_CloseInventory()
{
	CloseWheel(EVRWheelType::Inventory);
}

void VRWheel_Reset()
{
	ReleaseWheelTimeControl();
	ResetWheel();
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

	auto player = &players[consoleplayer];
	if (player == nullptr || player->mo == nullptr || player->health <= 0)
	{
		return;
	}

	InvalidateWheelIfOwnerChanged(player);

	if (menuactive != MENU_Off || ConsoleState != c_up || (VR_UseScreenLayer() && !UseCinemaWheelOverride()))
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
	const VRWheelLayoutInfo layout = BuildWheelLayoutInfo(count);
	const float maxIconSize = layout.RingCount > 1
		? max(layout.Rings[0].IconSize, layout.Rings[1].IconSize)
		: layout.Rings[0].IconSize;
	const float touchIndicatorRadius = (maxIconSize * 1.45f) * 0.25f;
	const float centerIndicatorRadius = maxIconSize * 0.38f;
	const float outerIndicatorRadius = centerIndicatorRadius * 1.85f;
	const float outerIndicatorInnerRadius = centerIndicatorRadius * 1.20f;
	const PalEntry bgColor = PalEntry(MAKEARGB(128,
		RPART(vr_wheel_icon_bg_color),
		GPART(vr_wheel_icon_bg_color),
		BPART(vr_wheel_icon_bg_color)));
	const PalEntry selectedBgColor = PalEntry(MAKEARGB(160,
		RPART(vr_wheel_icon_select_color),
		GPART(vr_wheel_icon_select_color),
		BPART(vr_wheel_icon_select_color)));
	const PalEntry disabledBgColor = PalEntry(MAKEARGB(160,
		RPART(vr_wheel_icon_disable_color),
		GPART(vr_wheel_icon_disable_color),
		BPART(vr_wheel_icon_disable_color)));

	if (!UseCinemaWheelOverride() && vr_wheel_selection_type == 0)
	{
		DVector3 touchPoint;
		if (GetTouchPoint(player, touchPoint))
		{
			DrawWorldDisc(di, state, touchPoint, wheelRight, wheelUp, touchIndicatorRadius, selectedBgColor);
		}
	}
	else if (layout.RingCount > 1)
	{
		const int ringIndex = GetAimRingIndex(player, layout, center, wheelRight, wheelUp);
		const PalEntry innerColor = ringIndex == 0 ? selectedBgColor : bgColor;
		const PalEntry outerColor = ringIndex == 1 ? selectedBgColor : bgColor;
		DrawWorldDisc(di, state, center, wheelRight, wheelUp, outerIndicatorRadius, outerColor);
		DrawWorldDisc(di, state, center, wheelRight, wheelUp, outerIndicatorInnerRadius, bgColor);
		DrawWorldDisc(di, state, center, wheelRight, wheelUp, centerIndicatorRadius, innerColor);
	}

	for (int i = 0; i < count; ++i)
	{
		const auto& entry = GVRWheel.Entries[i];
		int localIndex = -1;
		const auto* ring = FindRingForEntry(layout, i, localIndex);
		if (ring == nullptr)
		{
			continue;
		}

		const float iconSize = ring->IconSize;
		const float backdropSize = iconSize * 1.45f;
		const double angle = GetWheelEntryAngle(*ring, localIndex);
		const DVector3 iconCenter = center
			+ wheelRight * (cos(angle) * ring->Radius)
			+ wheelUp * (sin(angle) * ring->Radius);

		PalEntry iconColor = entry.Selectable ? PalEntry(235, 255, 255, 255) : PalEntry(115, 180, 180, 180);
		if (i == GVRWheel.HoveredIndex)
		{
			iconColor = PalEntry(255, 255, 255, 255);
		}

		float iconWidth = iconSize;
		float iconHeight = iconSize;
		GetIconQuadSize(entry.Icon, iconSize, iconWidth, iconHeight);

		const PalEntry backdropColor = i == GVRWheel.HoveredIndex
			? selectedBgColor
			: (entry.Selectable ? bgColor : disabledBgColor);
		DrawWorldDisc(di, state, iconCenter, wheelRight, wheelUp, backdropSize * 0.50f, backdropColor);
		if (entry.ModelFrame != nullptr)
		{
			DrawWheelModel(di, state, entry, iconCenter, wheelForward, iconSize);
		}
		else
		{
			DrawWorldQuad(di, state, iconCenter, wheelRight, wheelUp, iconWidth, iconHeight, entry.Icon, iconColor, true, true);
		}
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
