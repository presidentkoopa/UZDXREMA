/*
** hw_cvars.cpp
**
** most of the hardware renderer's CVARs.
**
**---------------------------------------------------------------------------
**
** Copyright 2005-2020 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Code written prior to 2026 is also licensed under:
**
** SPDX-License-Identifier: BSD-3-Clause
**
**---------------------------------------------------------------------------
**
*/

#include "c_cvars.h"
#include "c_dispatch.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "menu.h"
#include "printf.h"
#include "version.h"
#include <algorithm>

CUSTOM_CVAR(Int, gl_fogmode, 2, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (self > 2) self = 2;
	if (self < 0) self = 0;
}

// Optional family toggle for Selaco-style background texture/material streaming.
CVAR(Bool, gl_texture_thread, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, gl_texture_thread_models, true, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
CVAR(Bool, gl_texture_thread_upload, true, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)

// Optional tuning for the texture-thread family. The master toggle above still gates the whole feature set.
CUSTOM_CVAR(Int, gl_texture_thread_workers, 2, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
{
	if (self < 1) self = 1;
	else if (self > 8) self = 8;
}

CUSTOM_CVAR(Int, vk_max_transfer_threads, 2, CVAR_GLOBALCONFIG | CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	if (self < 0) self = 0;
	else if (self > 8) self = 8;

	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

CUSTOM_CVAR(Int, gl_background_flush_count, 100, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
{
	if (self < 1) self = 1;
	else if (self > 1000) self = 1000;
}


// OpenGL stuff moved here
// GL related CVARs
CVAR(Bool, gl_portals, true, 0)
CVAR(Bool, gl_mirrors, true, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
CVAR(Bool, gl_mirror_player, true, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
CVAR(Bool,gl_mirror_envmap, true, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
CVAR(Bool, gl_seamless, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)

// Upstream renamed r_mirror_recursions -> r_portal_recursions (and swrenderer's
// r_portal.cpp now only EXTERN_CVARs it, so this is the single definition).
// The fork's tuned default of 2 (instead of 4) and CVAR_GLOBALCONFIG are kept.
CUSTOM_CVAR(Int, r_portal_recursions, 2, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
{
	if (self > 16) self = 16;
	if (self < 0) self = 0;
}

bool gl_plane_reflection_i;	// This is needed in a header that cannot include the CVAR stuff...
CUSTOM_CVAR(Bool, gl_plane_reflection, false, CVAR_GLOBALCONFIG|CVAR_ARCHIVE)
{
	gl_plane_reflection_i = self;
}

constexpr float GAMMA_DEFAULT = 2.2;
constexpr float GAMMA_HIGH = 3.0;
constexpr float GAMMA_LOW = 0.1;

constexpr float GAMMA_LOW_FIX = (GAMMA_LOW-GAMMA_DEFAULT) / (GAMMA_HIGH-GAMMA_DEFAULT);

CUSTOM_CVARD(Float, vid_gamma, GAMMA_DEFAULT, 0, "(internal) target output gamma")
{
	if (self < GAMMA_LOW) self = GAMMA_LOW;
}

CUSTOM_CVARD(Float, vid_fixgamma, 0.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts gamma component of gamma ramp")
{
	if (self < GAMMA_LOW_FIX) self = GAMMA_LOW_FIX;
	else vid_gamma = self*(GAMMA_HIGH-GAMMA_DEFAULT) + GAMMA_DEFAULT;
}

CUSTOM_CVARD(Float, vid_contrast, 1.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts contrast component of gamma ramp")
{
	if (self < 0) self = 0;
	else if (self > 5) self = 5;
}

// Fork: vid_brightness is deleted upstream but is still consumed by the VR present
// path (vk_postprocess.cpp PresentUniforms::Brightness) and the DSPLYMNU_BRIGHTNESS
// slider. Keep it.
CUSTOM_CVARD(Float, vid_brightness, 0.05f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts brightness component of gamma ramp")
{
	if (self < -2) self = -2;
	else if (self > 2) self = 2;
}

CUSTOM_CVARD(Float, vid_saturation, 1.2f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts saturation component of gamma ramp")
{
	if (self < -3) self = -3;
	else if (self > 3) self = 3;
}

#ifndef BW_GAP
#define BW_GAP 0.2
#endif

CVAR(Float, vid_i_blackpoint, 1.f, CVAR_VIRTUAL | CVAR_NOINITCALL | CVAR_SYSTEM_ONLY);
CVAR(Float, vid_i_whitepoint, 1.f, CVAR_VIRTUAL | CVAR_NOINITCALL | CVAR_SYSTEM_ONLY);

CUSTOM_CVARD(Float, vid_blackpoint, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts what the engine outputs as black")
{
	if (self < 0) self = 0;
	if (self > 1) self = 1;

	float value = self*self;
	float bound = 1 - BW_GAP;
	float buffer = vid_i_whitepoint - BW_GAP;

	vid_i_blackpoint = min(min(buffer, value), bound);
}

CUSTOM_CVARD(Float, vid_whitepoint, 0.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "adjusts what the engine outputs as white")
{
	if (self < -1) self = -1;
	if (self > 2) self = 2;

	float value = self + 1;
	float bound = 0 + BW_GAP;
	float buffer = vid_i_blackpoint + BW_GAP;
	value = (value*value*value+1)/2;

	vid_i_whitepoint = max(max(buffer, value), bound);
}

#undef BW_GAP

CVAR(Int, gl_satformula, 2, CVAR_ARCHIVE|CVAR_GLOBALCONFIG);

//==========================================================================
//
// Texture CVARs
//
//==========================================================================
CUSTOM_CVARD(Float, gl_texture_filter_anisotropic, 4.f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL, "changes the OpenGL texture anisotropy setting")
{
	if (screen != nullptr)
	{
		screen->SetTextureFilterMode();
	}
}

CUSTOM_CVARD(Int, gl_texture_filter, 0, CVAR_ARCHIVE|CVAR_GLOBALCONFIG|CVAR_NOINITCALL, "changes the texture filtering settings")
{
	if (self < 0 || self > 6) self=6;
	if (screen != nullptr)
	{
		screen->SetTextureFilterMode();
	}
}

CVAR(Bool, gl_precache, true, CVAR_ARCHIVE)


CUSTOM_CVAR(Int, gl_shadowmap_filter, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self > 8) self = 1;
}

CVAR(Bool, gl_global_fade, false, CVAR_ARCHIVE)

CUSTOM_CVAR(Float, gl_global_fade_density, 0.001f, CVAR_ARCHIVE)
{
	if (self < 0.0001f) self = 0.0001f;
	if (self > 0.005f) self = 0.005f;
}
CUSTOM_CVAR(Float, gl_global_fade_gradient, 1.5f, CVAR_ARCHIVE)
{
	if (self < 0.1f) self = 0.1f;
	if (self > 2.f) self = 2.f;
}
CVAR(Color, gl_global_fade_color, 0x3f3f3f, CVAR_ARCHIVE)
CVAR(Bool, gl_global_fade_debug, false, 0)

CUSTOM_CVAR (Int, gl_storage_buffer_type, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("You must restart " GAMENAME " for this change to take effect.\n");
}

CVARD(Bool, gl_no_persistent_buffer, false, 0, "Disable persistent buffer storage support")
CVARD(Bool, gl_no_clip_planes, false, 0, "Disable clip planes support")
CVARD(Bool, gl_no_ssbo, false, 0, "Disable SSBO support")
CVARD(Bool, vr_scene_multithread, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG, "Allow VR BSP scene-build jobs on a worker thread")

CVAR(Bool, gl_strict_gldefs_errors, false, CVAR_GLOBALCONFIG | CVAR_ARCHIVE)
