/*
** hw_postprocess_cvars.cpp
**
** Postprocessing framework
**
**---------------------------------------------------------------------------
**
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
** Copyright 2016-2020 Magnus Norddahl
**
** SPDX-License-Identifier: Zlib
**
**---------------------------------------------------------------------------
**
*/

#include "hw_postprocess_cvars.h"
#include "v_video.h"

//==========================================================================
//
// CVARs
//
//==========================================================================
// [BB] On by default in this fork. Stock GZDoom ships it off, which is a
// reasonable default for vanilla Doom and the wrong one here -- the glow
// system relies on bloom to read as emissive rather than as paint, so
// shipping it off means shipping the headline feature broken.
CVAR(Bool, gl_bloom, true, CVAR_ARCHIVE);
CUSTOM_CVAR(Float, gl_bloom_amount, 1.4f, CVAR_ARCHIVE)
{
	if (self < 0.1f) self = 0.1f;
}

// [BB] How bright a pixel must be before it blooms. This was hardcoded at
// 1.0, which meant only pixels that had already blown past full white could
// glow -- fine for a muzzle flash, useless for anything trying to read as
// emissive at a sane brightness. Lowering it lets glow bloom without being
// driven to absurd intensities first.
CUSTOM_CVAR(Float, gl_bloom_threshold, 1.0f, CVAR_ARCHIVE)
{
	if (self < 0.05f) self = 0.05f;
	if (self > 4.0f) self = 4.0f;
}

// [BB] Soft knee. A hard threshold makes things POP in and out of blooming
// as they cross it, which is constant and obvious when glow is pulsing or a
// beam is sweeping. This rolls the transition over a range instead, so bloom
// eases in rather than snapping on. 0 is the old hard cutoff.
CUSTOM_CVAR(Float, gl_bloom_knee, 0.5f, CVAR_ARCHIVE)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 8.0f) self = 8.0f;
}

// [BB] Anamorphic: blur wider horizontally than vertically, so bright things
// streak sideways the way they do through an anamorphic lens. Free, because
// the blur already runs as separate horizontal and vertical passes -- this
// only gives them different amounts.
CVAR(Bool, gl_bloom_anamorphic, false, CVAR_ARCHIVE)
CUSTOM_CVAR(Float, gl_bloom_anamorphic_ratio, 3.0f, CVAR_ARCHIVE)
{
	if (self < 1.0f) self = 1.0f;
	if (self > 16.0f) self = 16.0f;
}

// [BB] Bloom tint and chromatic fringing. Tint colours the bloom
// independently of what produced it; fringing offsets the colour channels
// radially so bright edges break up toward the screen edges, the way light
// through glass does.
CVAR(Float, gl_bloom_tint_r, 1.0f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_tint_g, 1.0f, CVAR_ARCHIVE)
CVAR(Float, gl_bloom_tint_b, 1.0f, CVAR_ARCHIVE)
CUSTOM_CVAR(Float, gl_bloom_chromatic, 0.0f, CVAR_ARCHIVE)
{
	if (self < 0.0f) self = 0.0f;
	if (self > 0.1f) self = 0.1f;
}

CVAR(Float, gl_exposure_scale, 1.3f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_min, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_base, 0.35f, CVAR_ARCHIVE)
CVAR(Float, gl_exposure_speed, 0.05f, CVAR_ARCHIVE)

CUSTOM_CVAR(Int, gl_tonemap, 0, CVAR_ARCHIVE)
{
	if (self < 0 || self > 5)
		self = 0;
}

CVAR(Bool, gl_lens, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CVAR(Float, gl_lens_k, -0.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_kcube, 0.1f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, gl_lens_chromatic, 1.12f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

CUSTOM_CVAR(Int, gl_fxaa, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self >= IFXAAShader::Count)
	{
		self = 0;
	}
}

CUSTOM_CVAR(Int, gl_ssao, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0 || self > 3)
		self = 0;
}

CUSTOM_CVAR(Int, gl_ssao_portals, 1, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0)
		self = 0;
}

CVAR(Float, gl_ssao_strength, 0.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Int, gl_ssao_debug, 0, 0)
CVAR(Float, gl_ssao_bias, 0.2f, 0)
CVAR(Float, gl_ssao_radius, 80.0f, 0)
CUSTOM_CVAR(Float, gl_ssao_blur, 16.0f, 0)
{
	if (self < 0.1f) self = 0.1f;
}

CUSTOM_CVAR(Float, gl_ssao_exponent, 1.8f, 0)
{
	if (self < 0.1f) self = 0.1f;
}

CUSTOM_CVAR(Float, gl_paltonemap_powtable, 2.0f, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	screen->UpdatePalette();
}

CUSTOM_CVAR(Bool, gl_paltonemap_reverselookup, true, CVAR_ARCHIVE | CVAR_NOINITCALL)
{
	screen->UpdatePalette();
}
// reminder: if is negative, use the gameinfo entry
CVAR(Float, gl_menu_blur, -1.0f, CVAR_ARCHIVE)
