/*
** hw_vrmodes.cpp
** Matrix handling for stereo 3D rendering
**
**---------------------------------------------------------------------------
** Copyright 2015 Christopher Bruns
** Copyright 2016-2021 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
/*
** hw_vrmodes.cpp
** Matrix handling for stereo 3D rendering
**
**---------------------------------------------------------------------------
** Copyright 2015 Christopher Bruns
** Copyright 2016-2021 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
**
*/

#include "vectors.h"
#include "hw_cvars.h"
#include "hw_vrmodes.h"
#include "v_video.h"
#include "version.h"
#include "i_interface.h"
#include "menu.h"
#include "gl_load/gl_system.h"

#include "gl_renderer.h"
#include "d_player.h"
#include "actorinlines.h"
#include "LSMatrix.h"
#include "gl/stereo3d/gl_openvr.h"
#include "gl/stereo3d/gl_openxrdevice.h"
#include "vulkan/stereo3d/vk_openxrdevice.h"
#include <QzDoom/VrCommon.h>

#include "textures.h"
#include "gametexture.h"
#include "common/2d/v_2ddrawer.h"
#include <algorithm>
#include <functional>
#include <thread>
#include "c_dispatch.h"
#include "c_console.h"
#include "common/scripting/jit/jit.h"

using namespace OpenGLRenderer;

extern thread_local bool isWorkerThread;

EXTERN_CVAR(Bool, vr_hud_mount);
EXTERN_CVAR(Int, vr_hud_mount_pos);
EXTERN_CVAR(Float, vr_hud_mount_scale);
EXTERN_CVAR(Float, vr_hud_mount_xoffset);
EXTERN_CVAR(Float, vr_hud_mount_yoffset);
EXTERN_CVAR(Float, vr_hud_mount_zoffset);
EXTERN_CVAR(Float, vr_hud_mount_pitch);
EXTERN_CVAR(Float, vr_hud_mount_yaw);
EXTERN_CVAR(Bool, vr_hud_mount_roll);
EXTERN_CVAR(Bool, vr_automap_mount);
EXTERN_CVAR(Int, vr_automap_mount_pos);
EXTERN_CVAR(Float, vr_automap_mount_scale);
EXTERN_CVAR(Float, vr_automap_mount_xoffset);
EXTERN_CVAR(Float, vr_automap_mount_yoffset);
EXTERN_CVAR(Float, vr_automap_mount_zoffset);
EXTERN_CVAR(Float, vr_automap_mount_pitch);
EXTERN_CVAR(Float, vr_automap_mount_yaw);
EXTERN_CVAR(Bool, vr_automap_mount_roll);
EXTERN_CVAR(Bool, portablehud);
EXTERN_CVAR(Int, vr_mode);

extern float weaponangles[3];
extern float offhandangles[3];

static int gSuppressMountedHudFrames = 0;
static uint64_t gSuppressMountedHudLastFrameTime = 0;

VRHudSurface::VRHudSurface() = default;

VRHudSurface::~VRHudSurface()
{
	Clear();
}

void VRHudSurface::Clear()
{
	if (Canvas != nullptr)
	{
		if (Texture != nullptr && Texture->Canvas == Canvas)
		{
			Texture->Canvas = nullptr;
		}
		Canvas->Tex = nullptr;
		auto idx = AllCanvases.Find(Canvas);
		if (idx != -1)
		{
			AllCanvases.Delete(idx);
		}
		Canvas = nullptr;
	}
	GameTexture = nullptr;
	Texture = nullptr;
}

bool VRHudSurface::IsCanvasLive() const
{
	if (Texture == nullptr || Canvas == nullptr)
	{
		return false;
	}
	if (Texture->Canvas != Canvas || Canvas->Tex != Texture)
	{
		return false;
	}
	return AllCanvases.Find(Canvas) != -1;
}

void VRHudSurface::EnsureSize(int width, int height)
{
	if (width <= 0 || height <= 0)
	{
		return;
	}
	if (Texture && Texture->GetWidth() == width && Texture->GetHeight() == height && IsCanvasLive())
	{
		return;
	}
	Clear();
	Texture = new FCanvasTexture(width, height);
	// Mark this canvas as translucent so the render loop clears the FBO to
	// transparent black before Draw2D, and ApplyMaterial uses TM_NORMAL
	// instead of TM_OPAQUE. This propagates automatically to all surfaces
	// that sample this texture: VR quad, world geometry, model textures.
	Texture->bTranslucentCanvas = true;
	GameTexture = MakeGameTexture(Texture, nullptr, ETextureType::Wall);
	Canvas = Create<FCanvas>();
	Texture->Canvas = Canvas;
	Canvas->Tex = Texture;
	Canvas->Drawer.SetSize(width, height);
	AllCanvases.Push(Canvas);
}

void VRHudSurface::BeginUpdate()
{
	if (Canvas != nullptr)
	{
		Canvas->Drawer.Clear();
	}
}

void VRHudSurface::EndUpdate()
{
	MarkDirty();
}

void VRHudSurface::MarkDirty()
{
	if (Texture != nullptr)
	{
		Texture->NeedUpdate();
	}
}

VRHudSurface& GetVRHudSurface()
{
	static VRHudSurface surface;
	return surface;
}

void VR_DestroyHudSurface()
{
	GetVRHudSurface().Clear();
}

void VR_EnsureHudSurface(int width, int height)
{
	GetVRHudSurface().EnsureSize(width, height);
}

void VR_SuppressMountedHudForFrames(int frames)
{
	if (frames > gSuppressMountedHudFrames)
	{
		gSuppressMountedHudFrames = frames;
	}
}

bool VR_ShouldDrawMountedHud()
{
	if (gSuppressMountedHudFrames > 0 && screen != nullptr)
	{
		if (gSuppressMountedHudLastFrameTime != screen->FrameTime)
		{
			gSuppressMountedHudLastFrameTime = screen->FrameTime;
			gSuppressMountedHudFrames--;
		}
		return false;
	}

	const bool portableHud = VR_UsePortableHud();
	if (!portableHud && !vr_hud_mount && !vr_automap_mount)
	{
		return false;
	}

	// [MR] Hide mounted HUD/Map when menu or console is active to allow facial overlay restoration
	if (menuactive || ConsoleState != c_up)
	{
		return false;
	}

	// [MR] Only draw if the respective feature is active
	if (automapactive && !portableHud && !vr_automap_mount) return false;
	if (!automapactive && !portableHud && !vr_hud_mount) return false;

	auto& surface = GetVRHudSurface();
	return surface.HasGameTexture() && surface.GetWidth() > 0 && surface.GetHeight() > 0;
}

bool VR_GetMountedHudTransform(VSMatrix& out)
{
	const bool portableHud = VR_UsePortableHud();
	if (!portableHud && !vr_hud_mount && !vr_automap_mount)
	{
		return false;
	}

	VSMatrix mountTransform;
	int mountedHand;
	if (automapactive && (portableHud || vr_automap_mount))
	{
		mountedHand = vr_automap_mount_pos == 0 ? VR_MAINHAND : VR_OFFHAND;
		if (!VRMode::GetVRModeCached(true)->GetWeaponTransform(&mountTransform, mountedHand)) return false;

		const float handSign = mountedHand == VR_MAINHAND ? -1.f : 1.f;
		mountTransform.translate(-vr_automap_mount_xoffset * handSign, -vr_automap_mount_zoffset, -vr_automap_mount_yoffset);
		mountTransform.rotate(vr_automap_mount_yaw * handSign, 0, 1, 0);
		mountTransform.rotate(-vr_automap_mount_pitch, 1, 0, 0);
		if (!vr_automap_mount_roll)
		{
			const float controllerRoll = mountedHand == VR_MAINHAND ? weaponangles[2] : offhandangles[2];
			mountTransform.rotate(-controllerRoll, 0, 0, 1);
		}
	}
	else
	{
		mountedHand = vr_hud_mount_pos == 0 ? VR_MAINHAND : VR_OFFHAND;
		if (!VRMode::GetVRModeCached(true)->GetWeaponTransform(&mountTransform, mountedHand)) return false;

		const float handSign = mountedHand == VR_MAINHAND ? -1.f : 1.f;
		mountTransform.translate(-vr_hud_mount_xoffset * handSign, -vr_hud_mount_zoffset, -vr_hud_mount_yoffset);
		mountTransform.rotate(vr_hud_mount_yaw * handSign, 0, 1, 0);
		mountTransform.rotate(-vr_hud_mount_pitch, 1, 0, 0);
		if (!vr_hud_mount_roll)
		{
			const float controllerRoll = mountedHand == VR_MAINHAND ? weaponangles[2] : offhandangles[2];
			mountTransform.rotate(-controllerRoll, 0, 0, 1);
		}
	}
	out = mountTransform;
	return true;
}

bool VR_UsePortableHud()
{
	// Portable HUD is world-space only. While virtual screen/screen-layer mode
	// is active, use the normal screen composition path and skip the portable pass.
	return portablehud && !VR_UseScreenLayer();
}

const VRMode *VRMode::GetVRModeCached(bool toscreen)
{
	if (isWorkerThread)
	{
		static VREyeInfo safeMonoEyes[2] = { VREyeInfo(0.f, 1.f), VREyeInfo(0.f, 0.f) };
		static VRMode safeMono(1, 1.f, 1.f, 1.f, safeMonoEyes);
		return &safeMono;
	}

	struct CacheEntry
	{
		bool valid = false;
		uint64_t frameTime = 0;
		int vrMode = 0;
		int backend = 0;
		bool disableTextureFilter = false;
		const VRMode* mode = nullptr;
	};

	thread_local CacheEntry cache[2];
	auto& entry = cache[toscreen ? 1 : 0];
	const uint64_t frameTime = screen != nullptr ? screen->FrameTime : 0;
	const int currentVrMode = (int)vr_mode;
	const int currentBackend = V_GetBackend();
	const bool currentDisableTextureFilter = sysCallbacks.DisableTextureFilter && sysCallbacks.DisableTextureFilter();

	if (entry.valid &&
		entry.frameTime == frameTime &&
		entry.vrMode == currentVrMode &&
		entry.backend == currentBackend &&
		entry.disableTextureFilter == currentDisableTextureFilter)
	{
		return entry.mode;
	}

	entry.valid = true;
	entry.frameTime = frameTime;
	entry.vrMode = currentVrMode;
	entry.backend = currentBackend;
	entry.disableTextureFilter = currentDisableTextureFilter;
	entry.mode = GetVRMode(toscreen);
	return entry.mode;
}
// Set up 3D-specific console variables:
CUSTOM_CVAR(Int, vr_mode, 0, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
{
	// Keep the selected VR mode stable across renderers.
	// OpenGL can use OpenVR (10), Vulkan can use OpenXR (15).
	if (self < 0)
		self = 0;
}

#define PITCH 0
#define YAW 1
#define ROLL 2

typedef float vec_t;
typedef vec_t vec3_t[3];

// switch left and right eye views
CVAR(Bool, vr_swap_eyes, false, CVAR_GLOBALCONFIG   | CVAR_ARCHIVE)
// intraocular distance in meters
CVAR(Float, vr_ipd, 0.064f, CVAR_ARCHIVE|CVAR_GLOBALCONFIG) // METERS

// distance between viewer and the display screen
CVAR(Float, vr_screendist, 0.80f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // METERS

CVAR(Int, vr_desktop_view, 2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_overlayscreen, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_overlayscreen_always, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_overlayscreen_size, 1., CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_overlayscreen_dist, 0., CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_overlayscreen_vpos, 0., CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_overlayscreen_bg, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_kill_momentum, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// default conversion between (vertical) DOOM units and meters
CVAR(Float, vr_vunits_per_meter, 34.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // METERS
CVAR(Float, vr_height_adjust, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // METERS
CVAR(Float, vr_openxr_fov_adjust_deg, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // DEGREES PER SIDE
CVAR(Float, vr_openxr_eye_shift_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_debug_projection_compare, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_openxr_debug_sizes, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_openxr_debug_present, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_openxr_debug_weapon, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Experimental: render the OpenXR scene at runtime-recommended eye size
// instead of desktop framebuffer size to reduce upscale aliasing.
CVAR(Bool, vr_openxr_force_recommended_viewport, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// OpenXR-only internal render scale relative to recommended eye dimensions.
// 1.0 means recommended size, below 1.0 trades quality for performance.
CVAR(Float, vr_openxr_render_scale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Use the full screen viewport as XR present source by default to reduce
// aspect-stretch upscaling artifacts compared to mSceneViewport.
CVAR(Bool, vr_openxr_use_screen_viewport_for_submit, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_gamma_bias, 1.30f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_contrast_bias, 1.02f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_brightness_bias, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_openxr_present_saturation_bias, 1.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Int, vr_control_scheme, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	M_ResetButtonStates();
}
CUSTOM_CVAR(Int, vr_joy_mode, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("Changing the control mode requires a restart for " GAMENAME ".\n");
}
CVAR(Bool, vr_move_use_offhand, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_teleport, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_weaponRotate, -30.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_weaponScale, 1.02f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_3dweaponOffsetX, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_3dweaponOffsetY, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_3dweaponOffsetZ, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponOffsetX, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponOffsetY, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponOffsetZ, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_2dweaponScale, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG);
CVAR(Float, vr_snapTurn, 45.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_switch_sticks, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_use_alternate_mapping, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_secondary_button_mappings, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_two_handed_weapons, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_momentum, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG) // Only used in player.zs
CVAR(Float, vr_momentum_threshold, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_crouch_use_button, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, use_action_spawn_yzoffset, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Bool, vr_enable_haptics, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_pickup_haptic_level, 0.2, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_quake_haptic_level, 0.8, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_missile_haptic_level, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//HUD control
CVAR(Float, vr_hud_scale, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_stereo, 1.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_distance, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_rotate, 10.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_fixed_pitch, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_fixed_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_mount, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_hud_mount_pos, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_xoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_yoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_zoffset, -0.20f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_scale, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_pitch, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_hud_mount_yaw, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_hud_mount_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
// Runtime override that forces the mounted HUD/automap path on without
// changing the individual menu toggles.
CVAR(Bool, portablehud, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

//AutoMap control
CVAR(Bool, vr_automap_use_hud, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_scale, 0.4f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_stereo, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_distance, 1.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_rotate, 13.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_fixed_pitch, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_fixed_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_mount, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_automap_mount_pos, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_scale, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_xoffset, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_yoffset, 0.15f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_zoffset, -0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_pitch, 45.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_automap_mount_yaw, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Bool, vr_automap_mount_roll, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, vr_automap_border, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Color, vr_automap_border_color, 0x636363, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CCMD(toggleportablehud)
{
	portablehud = !portablehud;
	Printf("portablehud %s\n", portablehud ? "enabled" : "disabled");
}

void VR_InitPortableHudBinding()
{
	// Allow the menu/keybind entry named "portablehud" to invoke the toggle
	// path instead of only querying the boolean cvar.
	C_SetAlias("portablehud", "toggleportablehud");
}


CVARD(Bool, vr_override_weap_pos, false, 0, "Only used for testing VR environment on PC");
CVARD(Bool, vr_render_weap_in_scene, false, 0, "Only used for testing VR environment on PC");

EXTERN_CVAR(Bool, puristmode);
EXTERN_CVAR(Float, turbo);

CUSTOM_CVAR(Int, vr_move_speed, 19, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	turbo->Callback();
}
CUSTOM_CVAR(Float, vr_run_multiplier, 1.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	turbo->Callback();
}
CUSTOM_CVAR(Float, vr_walk_multiplier, 1.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	turbo->Callback();
}

#define isqrt2 0.7071067812f

VRMode::VRMode(int eyeCount, float horizontalViewportScale,
	float verticalViewportScale, float weaponProjectionScale, VREyeInfo eyes[2])
{
	mEyeCount = eyeCount;
	mHorizontalViewportScale = horizontalViewportScale;
	mVerticalViewportScale = verticalViewportScale;
	mWeaponProjectionScale = weaponProjectionScale;
	mEyes[0] = &eyes[0];
	mEyes[1] = &eyes[1];

}

static float DEG2RAD(float deg)
{
	return deg * float(M_PI / 180.0);
}

static float RAD2DEG(float rad)
{
	return rad * float(180. / M_PI);
}

const VRMode *VRMode::GetVRMode(bool toscreen)
{
	static VREyeInfo vrmi_mono_eyes[2] = { VREyeInfo(0.f, 1.f), VREyeInfo(0.f, 0.f) };
	static VREyeInfo vrmi_stereo_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
	static VREyeInfo vrmi_sbsfull_eyes[2] = { VREyeInfo(-.5f, .5f), VREyeInfo(.5f, .5f) };
	static VREyeInfo vrmi_sbssquished_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
	static VREyeInfo vrmi_lefteye_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(0.f, 0.f) };
	static VREyeInfo vrmi_righteye_eyes[2] = { VREyeInfo(.5f, 1.f), VREyeInfo(0.f, 0.f) };
	static VREyeInfo vrmi_topbottom_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
	static VREyeInfo vrmi_checker_eyes[2] = { VREyeInfo(-.5f, 1.f), VREyeInfo(.5f, 1.f) };
#if 0 //def USE_OPENVR
	static s3d::OpenVREyePose vrmi_openvr_eyes[2] = { s3d::OpenVREyePose(0, -.5f, 1.f), s3d::OpenVREyePose(1, .5f, 1.f) };
#endif

	static VRMode vrmi_mono(1, 1.f, 1.f, 1.f, vrmi_mono_eyes);
	static VRMode vrmi_stereo(2, 1.f, 1.f, 1.f, vrmi_stereo_eyes);
	static VRMode vrmi_sbsfull(2, .5f, 1.f, 2.f, vrmi_sbsfull_eyes);
	static VRMode vrmi_sbssquished(2, .5f, 1.f, 1.f, vrmi_sbssquished_eyes);
	static VRMode vrmi_lefteye(1, 1.f, 1.f, 1.f, vrmi_lefteye_eyes);
	static VRMode vrmi_righteye(1, 1.f, 1.f, 1.f, vrmi_righteye_eyes);
	static VRMode vrmi_topbottom(2, 1.f, .5f, 1.f, vrmi_topbottom_eyes);
	static VRMode vrmi_checker(2, isqrt2, isqrt2, 1.f, vrmi_checker_eyes);
#if 0 //def USE_OPENVR
	static s3d::OpenVRMode vrmi_openvr(vrmi_openvr_eyes);
#endif

	int mode = !toscreen || (sysCallbacks.DisableTextureFilter && sysCallbacks.DisableTextureFilter()) ? 0 : vr_mode;

	switch (mode)
	{
	default:
	case VR_MONO:
		return &vrmi_mono;

	case VR_GREENMAGENTA:
	case VR_REDCYAN:
	case VR_QUADSTEREO:
	case VR_AMBERBLUE:
	case VR_SIDEBYSIDELETTERBOX:
		return &vrmi_stereo;

	case VR_SIDEBYSIDESQUISHED:
	case VR_COLUMNINTERLEAVED:
		return &vrmi_sbssquished;

	case VR_SIDEBYSIDEFULL:
		return &vrmi_sbsfull;

	case VR_TOPBOTTOM:
	case VR_ROWINTERLEAVED:
		return &vrmi_topbottom;

	case VR_LEFTEYEVIEW:
		return &vrmi_lefteye;

	case VR_RIGHTEYEVIEW:
		return &vrmi_righteye;

	case VR_CHECKERINTERLEAVED:
		return &vrmi_checker;
#ifdef USE_OPENVR
	case VR_OPENVR:
	{
		// When calling a function of this class, ensure that you are using a pointer or reference to the derived class
		Printf("VRMode select: choosing OpenVR mode 10.\n");
		const VRMode &vrmode = s3d::OpenVRMode::getInstance();
		return vrmode.IsInitialized() ? &vrmode : &vrmi_mono;
		//return vrmi_openvr.IsInitialized() ? &vrmi_openvr : &vrmi_mono;
	}
#endif
#ifdef USE_OPENXR
	case VR_OPENXR_MOBILE:
		if (V_GetBackend() == 1)
		{
			Printf("VRMode select: choosing Vulkan/OpenXR mode 15.\n");
			return &s3d::VKOpenXRDeviceMode::getInstance();
		}
		Printf("VRMode select: Vulkan/OpenXR requested but backend is not Vulkan.\n");
		return &vrmi_mono;
#endif
	}
}

void VRMode::AdjustViewport(DFrameBuffer *screen) const
{
	screen->mSceneViewport.height = (int)(screen->mSceneViewport.height * mVerticalViewportScale);
	screen->mSceneViewport.top = (int)(screen->mSceneViewport.top * mVerticalViewportScale);
	screen->mSceneViewport.width = (int)(screen->mSceneViewport.width * mHorizontalViewportScale);
	screen->mSceneViewport.left = (int)(screen->mSceneViewport.left * mHorizontalViewportScale);

	screen->mScreenViewport.height = (int)(screen->mScreenViewport.height * mVerticalViewportScale);
	screen->mScreenViewport.top = (int)(screen->mScreenViewport.top * mVerticalViewportScale);
	screen->mScreenViewport.width = (int)(screen->mScreenViewport.width * mHorizontalViewportScale);
	screen->mScreenViewport.left = (int)(screen->mScreenViewport.left * mHorizontalViewportScale);
}

void VRMode::Present() const {
	GLRenderer->PresentStereo();
}

VSMatrix VRMode::GetHUDSpriteProjection() const
{
	VSMatrix mat;
	int w = screen->GetWidth();
	int h = screen->GetHeight();
	float scaled_w = w / mWeaponProjectionScale;
	float left_ofs = (w - scaled_w) / 2.f;
	mat.ortho(left_ofs, left_ofs + scaled_w, (float)h, 0, -1.0f, 1.0f);
	return mat;
}

VREyeInfo::VREyeInfo(float shiftFactor, float scaleFactor)
{
	mShiftFactor = shiftFactor;
	mScaleFactor = scaleFactor;
	m_isActive = false;
}

float VREyeInfo::getShift() const
{
	auto res = mShiftFactor * vr_ipd;
	return vr_swap_eyes ? -res : res;
}

VSMatrix VREyeInfo::GetProjection(float fov, float aspectRatio, float fovRatio, bool iso_ortho) const
{
	VSMatrix result;

	if (iso_ortho) // Orthographic projection for isometric viewpoint
	{
		double zNear = -3.0/fovRatio; // screen->GetZNear();
		double zFar = screen->GetZFar();

		double fH = tan(DEG2RAD(fov) / 2) / fovRatio;
		double fW = fH * aspectRatio * mScaleFactor;
		double left = -fW;
		double right = fW;
		double bottom = -fH;
		double top = fH;

		VSMatrix fmat(1);
		fmat.ortho((float)left, (float)right, (float)bottom, (float)top, (float)zNear, (float)zFar);
		return fmat;
	}
	else if (mShiftFactor == 0)
	{
		float fovy = (float)(2 * RAD2DEG(atan(tan(DEG2RAD(fov) / 2) / fovRatio)));
		result.perspective(fovy, aspectRatio, screen->GetZNear(), screen->GetZFar());
		return result;
	}
	else
	{
		double zNear = screen->GetZNear();
		double zFar = screen->GetZFar();

		// For stereo 3D, use asymmetric frustum shift in projection matrix
		// Q: shouldn't shift vary with roll angle, at least for desktop display?
		// A: No. (lab) roll is not measured on desktop display (yet)
		double frustumShift = zNear * getShift() / vr_screendist; // meters cancel, leaving doom units
																  // double frustumShift = 0; // Turning off shift for debugging
		double fH = zNear * tan(DEG2RAD(fov) / 2) / fovRatio;
		double fW = fH * aspectRatio * mScaleFactor;
		double left = -fW - frustumShift;
		double right = fW - frustumShift;
		double bottom = -fH;
		double top = fH;

		VSMatrix fmat(1);
		fmat.frustum((float)left, (float)right, (float)bottom, (float)top, (float)zNear, (float)zFar);
		return fmat;
	}
}

VSMatrix VREyeInfo::GetHUDProjection() const
{
	VSMatrix mat;
	int w = screen->GetWidth();
	int h = screen->GetHeight();
	mat.ortho(0.f, (float)w, (float)h, 0.f, -1.0f, 1.0f);
	return mat;
}



/* virtual */
DVector3 VREyeInfo::GetViewShift(FRenderViewpoint& vp) const
{
	if (mShiftFactor == 0)
	{
		// pass-through for Mono view
		return { 0, 0, 0 };
	}
	else
	{
		float yaw = vp.HWAngles.Yaw.Degrees();
		double dx = -cos(DEG2RAD(yaw)) * vr_vunits_per_meter * getShift();
		double dy = sin(DEG2RAD(yaw)) * vr_vunits_per_meter * getShift();
		return { dx, dy, 0 };
	}
}

//Fishbiter's Function.. Thank-you!!
static DVector3 MapWeaponDir(AActor* actor, DAngle yaw, DAngle pitch, int hand = 0)
{
	LSMatrix44 mat;
	auto vrmode = VRMode::GetVRModeCached(true);
	if (!vrmode->GetWeaponTransform(&mat, hand))
	{
		double pc = pitch.Cos();
		DVector3 direction = { pc * yaw.Cos(), pc * yaw.Sin(), -pitch.Sin() };
		return direction;
	}

	yaw -= actor->Angles.Yaw;
	pitch -= actor->Angles.Pitch;

	double pc = pitch.Cos();

	LSVec3 local = { (float)(pc * yaw.Cos()), (float)(pc * yaw.Sin()), (float)(-pitch.Sin()), 0.0f };

	DVector3 dir;
	dir.X = local.x * -mat[2][0] + local.y * -mat[0][0] + local.z * -mat[1][0];
	dir.Y = local.x * -mat[2][2] + local.y * -mat[0][2] + local.z * -mat[1][2];
	dir.Z = local.x * -mat[2][1] + local.y * -mat[0][1] + local.z * -mat[1][1];
	dir.MakeUnit();

	return dir;
}

static DVector3 MapAttackDir(AActor* actor, DAngle yaw, DAngle pitch)
{
	return MapWeaponDir(actor, yaw, pitch, 0);
}

static DVector3 MapOffhandDir(AActor* actor, DAngle yaw, DAngle pitch)
{
	return MapWeaponDir(actor, yaw, pitch, 1);
}

bool VRMode::RenderPlayerSpritesInScene() const
{
	return vr_render_weap_in_scene;
}

void VRMode::SetUp() const
{
	player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;
	if (player && player->mo)
	{
		player->PlayInVR = IsVR();
		player->mo->OverrideAttackPosDir = !puristmode && (IsVR() || vr_override_weap_pos);
		player->mo->AttackDir = MapAttackDir;
		player->mo->OffhandDir = MapOffhandDir;
		double shootz = player->mo->Center() - player->mo->Floorclip + player->mo->AttackOffset();
		player->mo->AttackPos = player->mo->OffhandPos = player->mo->PosAtZ(shootz);
		player->mo->AttackAngle = player->mo->OffhandAngle = r_viewpoint.Angles.Yaw - DAngle::fromDeg(90.);
		player->mo->AttackPitch = player->mo->OffhandPitch = - r_viewpoint.Angles.Pitch;
	}
}

//---------------------------------------------------------------------------
//
// The parameter hand_weapon is 0 for mainhand and 1 for offhand
// you can use the enum VR_MAINHAND and VR_OFFHAND
//
//---------------------------------------------------------------------------
bool VRMode::GetWeaponTransform(VSMatrix* out, int hand_weapon) const
{
	player_t * player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;
	bool autoReverse = true;
	if (player)
	{
		AActor *weap = (hand_weapon == VR_OFFHAND) ? player->OffhandWeapon : player->ReadyWeapon;
		autoReverse = weap == nullptr || !(weap->IntVar(NAME_WeaponFlags) & WIF_NO_AUTO_REVERSE);
	}
	bool rightHanded = vr_control_scheme < 10;
	int hand = (hand_weapon == VR_OFFHAND) ? 1 - rightHanded : rightHanded;
	if (GetHandTransform(hand, out))
	{
		if (!hand && autoReverse)
			out->scale(-1.0f, 1.0f, 1.0f);
		return true;
	}
	return false;
}

float length(float x, float y)
{
    return sqrtf(powf(x, 2.0f) + powf(y, 2.0f));
}

#define NLF_DEADZONE 0.1
#define NLF_POWER 2.2

float nonLinearFilter(float in)
{
    float val = 0.0f;
    if (in > NLF_DEADZONE)
    {
        val = in > 1.0f ? 1.0f : in;
        val -= NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = powf(val, NLF_POWER);
    }
    else if (in < -NLF_DEADZONE)
    {
        val = in < -1.0f ? -1.0f : in;
        val += NLF_DEADZONE;
        val /= (1.0f - NLF_DEADZONE);
        val = -powf(fabsf(val), NLF_POWER);
    }

    return val;
}

bool between(float min, float val, float max)
{
    return (min < val) && (val < max);
}

// Function to normalize an angle to the [-180, 180] range
double normalizeAngle(double angle) {
	// Reduce the angle to [0, 359]
	angle = fmod(angle, 360.0);
	// Force it to be the positive remainder
	angle = fmod(angle + 360.0, 360.0);
	// Normalize to the [-180, 180] range
	if (angle > 180.0) {
		angle -= 360.0;
	}
	return angle;
}

extern float weaponoffset[3];
extern float weaponangles[3];
extern float offhandoffset[3];
extern float offhandangles[3];
extern float hmdorientation[3];
extern float hmdPosition[3];

ADD_STAT(vrstats)
{
	FString out;

	player_t* player = r_viewpoint.camera ? r_viewpoint.camera->player : nullptr;
	if (player && player->mo)
	{
		out.AppendFormat("AttackPos: X=%2.f, Y=%2.f, Z=%2.f\n"
			"AttackAngle=%2.f, AttackPitch=%2.f, AttackRoll=%2.f\n", 
			player->mo->AttackPos.X, player->mo->AttackPos.Y, player->mo->AttackPos.Z,
			player->mo->AttackAngle.Degrees(), player->mo->AttackPitch.Degrees(), player->mo->AttackRoll.Degrees());

		out.AppendFormat("OffhandPos: X=%2.f Y=%2.f Z=%2.f\n"
			"OffhandAngle=%2.f, OffhandPitch=%2.f, OffhandRoll=%2.f\n", 
			player->mo->OffhandPos.X, player->mo->OffhandPos.Y, player->mo->OffhandPos.Z,
			player->mo->OffhandAngle.Degrees(), player->mo->OffhandPitch.Degrees(), player->mo->OffhandRoll.Degrees());
	}

	out.AppendFormat("weaponangles: yaw=%2.f, pitch=%2.f, roll=%2.f\n",
		weaponangles[YAW], weaponangles[PITCH], weaponangles[ROLL]);

	out.AppendFormat("weaponoffset: x=%1.3f, y=%1.3f, z=%1.3f\n",
		weaponoffset[0], weaponoffset[1], weaponoffset[2]);
	
	out.AppendFormat("offhandangles: yaw=%2.f, pitch=%2.f, roll=%2.f\n",
		offhandangles[YAW], offhandangles[PITCH], offhandangles[ROLL]);

	out.AppendFormat("hmdorientation: yaw=%2.f, pitch:%2.f, roll:%2.f\n", 
		hmdorientation[YAW], hmdorientation[PITCH], hmdorientation[ROLL]);

	out.AppendFormat("hmdpos: x=%1.3f, y:%1.3f, z:%1.3f\n", 
		hmdPosition[0], hmdPosition[1], hmdPosition[2]);

	out.AppendFormat("gamestate:%d - menuactive:%d - paused:%d", gamestate, menuactive, paused);

	return out;
}
