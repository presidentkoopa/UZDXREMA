/*
** bloomextract.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2016 Magnus Norddahl
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;
layout(binding=0) uniform sampler2D SceneTexture;
layout(binding=1) uniform sampler2D ExposureTexture;

// [BB] Soft-knee threshold.
//
// The original was a hard cutoff: subtract 1.0 and clamp. A pixel just under
// the line contributed nothing and a pixel just over it contributed fully, so
// anything drifting across the threshold POPPED. With glow that pulses and
// beams that sweep, things cross that line constantly and the popping is the
// first thing you notice.
//
// The knee rolls the transition over a range below the threshold using a
// quadratic, so bloom eases in. Knee 0 restores the old hard behaviour.
void main()
{
	float exposureAdjustment = texture(ExposureTexture, vec2(0.5)).x;
	vec3 c = (texture(SceneTexture, Offset + TexCoord * Scale).rgb + vec3(0.001)) * exposureAdjustment;

	// Judge brightness by the strongest channel, so a saturated colour blooms
	// on its own terms rather than needing to be bright in all three.
	float brightness = max(c.r, max(c.g, c.b));

	float contribution;
	if (Knee > 0.0001)
	{
		float soft = brightness - Threshold + Knee;
		soft = clamp(soft, 0.0, 2.0 * Knee);
		soft = soft * soft / (4.0 * Knee);
		contribution = max(soft, brightness - Threshold) / max(brightness, 0.0001);
	}
	else
	{
		contribution = max(brightness - Threshold, 0.0) / max(brightness, 0.0001);
	}

	FragColor = vec4(c * contribution, 1.0);
}
