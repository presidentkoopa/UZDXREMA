// [GITD-BB] RING payload for in-world billboards: a progress annulus. Quad-
// space SDF -- nothing is sampled from the bound texture. The packed payload
// int arrives on uAddColor and the billboard's tint on uObjectColor (see
// HWSprite::DrawSprite).
//
// data low byte = progress 0-255, style bits above.
//
// Keep every helper GitdBB-prefixed so this file cannot collide with names
// main.fp may define. This file must compile UNCHANGED on both backends: the
// shaders_gles/glsl copy is byte-identical and both are compiled at boot.

int GitdBBData()
{
	ivec4 q = ivec4(uAddColor * 255.0 + vec4(0.5));
	return (q.a << 24) | (q.r << 16) | (q.g << 8) | q.b;
}

// Quad space: +x = screen right, +y = up, edges at +-1.
// NO u flip here -- see the note in gitd_bb_panel.fp and the UV / ORIENTATION
// comment in HWSprite::ProcessBillboard. The ring is radially symmetric, so a
// flip would be invisible HERE and would silently mirror the payloads that
// are not symmetric. Keep all five payloads on the one convention.
vec2 GitdBBQuad()
{
	return vec2((vTexCoord.s - 0.5) * 2.0, (0.5 - vTexCoord.t) * 2.0);
}

vec4 ProcessTexel()
{
	int packv = GitdBBData();
	float prog = float(packv & 255) / 255.0;
	vec2 g = GitdBBQuad();

	// house neon response around the r=0.78 annulus
	float dr   = abs(length(g) - 0.78);
	float core = clamp(1.0 - dr / 0.10, 0.0, 1.0);
	float halo = clamp(1.0 - dr / 0.34, 0.0, 1.0);
	float ring = core * core + halo * halo * 0.25;

	// angular position 0..1, 0 at 12 o'clock, increasing clockwise
	float t = atan(g.x, g.y) / 6.2831853;
	if (t < 0.0) t += 1.0;

	// feathered leading edge so the head doesn't shimmer as it advances
	float lit = prog >= 1.0 ? 1.0 : 1.0 - smoothstep(prog - 0.012, prog + 0.012, t);

	float a = ring * mix(0.15, 1.0, lit);
	return vec4(uObjectColor.rgb, clamp(a, 0.0, 1.0));
}
