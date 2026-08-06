// [GITD-BB] BAR payload for in-world billboards: a horizontal progress gauge.
// Quad-space SDF -- nothing is sampled from the bound texture. The packed
// payload int arrives on uAddColor and the billboard's tint on uObjectColor
// (see HWSprite::DrawSprite).
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
// comment in HWSprite::ProcessBillboard. This payload is the one that makes a
// wrong convention obvious: the fill grows toward +x, so a flipped u sends the
// gauge backwards.
vec2 GitdBBQuad()
{
	return vec2((vTexCoord.s - 0.5) * 2.0, (0.5 - vTexCoord.t) * 2.0);
}

float GitdBBSeg(vec2 p, vec2 a, vec2 b)
{
	vec2 pa = p - a, ba = b - a;
	float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
	return length(pa - ba * h);
}

vec4 ProcessTexel()
{
	int packv = GitdBBData();
	float prog = float(packv & 255) / 255.0;
	vec2 g = GitdBBQuad();

	// hollow track outline, always visible so 0% still reads as a gauge
	float dT = GitdBBSeg(g, vec2(-0.78, 0.0), vec2(0.78, 0.0));
	float track = clamp(1.0 - abs(dT - 0.17) / 0.045, 0.0, 1.0);
	float a = track * track * 0.35;

	// neon fill capsule, screen-left end toward screen-right
	if (prog > 0.0)
	{
		float hx = -0.78 + 1.56 * prog;
		float dF = GitdBBSeg(g, vec2(-0.78, 0.0), vec2(hx, 0.0));
		float core = clamp(1.0 - dF / 0.12, 0.0, 1.0);
		float halo = clamp(1.0 - dF / 0.30, 0.0, 1.0);
		a = max(a, core * core + halo * halo * 0.25);
	}

	return vec4(uObjectColor.rgb, clamp(a, 0.0, 1.0));
}
