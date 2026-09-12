/*
** gpuparticles.fp
**
** [GPUPARTICLES] Soft round emissive dot. Colour, intensity, fade over life and
** the intensity cvar are already folded into vParticleColor by
** gpuparticles.vp; this only shapes it.
**
** Drawn with STYLE_Add, whose source factor is source alpha, so the round
** falloff goes in alpha and the blend applies it. Additive blending does not
** depend on draw order, which is why nothing is sorted. No lighting, no
** texture in phase one.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
*/

layout(location = 0) in vec2 vParticleCorner;
layout(location = 1) in vec4 vParticleColor;

layout(location = 0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location = 1) out vec4 FragFog;
layout(location = 2) out vec4 FragNormal;
#endif

void main()
{
	float r2 = dot(vParticleCorner, vParticleCorner);
	float falloff = clamp(1.0 - r2, 0.0, 1.0);
	falloff *= falloff;

	FragColor = vec4(vParticleColor.rgb, falloff);

#ifdef GBUFFER_PASS
	// Zero with zero alpha: under additive blending this adds nothing to the
	// fog and normal attachments.
	FragFog = vec4(0.0);
	FragNormal = vec4(0.0);
#endif
}
