// [GITD-BB] GLYPH payload for in-world billboards: one of a small set of
// vector marker shapes. Quad-space SDF -- nothing is sampled from the bound
// texture. The packed payload int arrives on uAddColor and the billboard's
// tint on uObjectColor (see HWSprite::DrawSprite).
//
// data low byte = glyph id, second byte = palette index.
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
// comment in HWSprite::ProcessBillboard. Several glyphs below are chiral (the
// arrow, the crown, the triple slash); a flipped u mirrors them.
vec2 GitdBBQuad()
{
	return vec2((vTexCoord.s - 0.5) * 2.0, (0.5 - vTexCoord.t) * 2.0);
}

vec3 GitdBBPal(int c)
{
	if (c == 1)  return vec3(1.0, 0.80, 0.16);   // gold
	if (c == 2)  return vec3(1.0, 0.25, 0.15);   // red
	if (c == 3)  return vec3(0.25, 1.0, 0.45);   // green
	if (c == 4)  return vec3(1.0, 1.0, 1.0);     // white
	if (c == 5)  return vec3(1.0, 0.55, 0.10);   // orange
	if (c == 6)  return vec3(0.75, 0.30, 1.0);   // purple
	if (c == 7)  return vec3(1.0, 0.35, 0.80);   // magenta
	return vec3(0.25, 0.82, 1.0);                // 0 and everything else: cyan
}

float GitdBBSeg(vec2 p, vec2 a, vec2 b)
{
	vec2 pa = p - a, ba = b - a;
	float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
	return length(pa - ba * h);
}

float GitdBBRing(vec2 g, float r, float w) { return clamp(1.0 - abs(length(g) - r) / w, 0.0, 1.0); }

float GitdBBGlyph(int id, vec2 g)
{
	float a = 0.0;
	if (id == 0)            // triple slash: three parallel diagonal bars
	{
		for (int i = -1; i <= 1; i++)
		{
			vec2 o = vec2(float(i) * 0.45, 0.0);
			float d = GitdBBSeg(g - o, vec2(-0.30, -0.75), vec2(0.30, 0.75));
			a = max(a, clamp(1.0 - d / 0.13, 0.0, 1.0));
		}
	}
	else if (id == 1)       // ring with pupil
	{
		a = max(GitdBBRing(g, 0.72, 0.14), clamp(1.0 - length(g) / 0.22, 0.0, 1.0));
	}
	else if (id == 2)       // crown: five spikes fanned upward
	{
		for (int i = 0; i < 5; i++)
		{
			float x = -0.8 + float(i) * 0.4;
			float d = GitdBBSeg(g, vec2(x, -0.6), vec2(x * 0.7, 0.55 + 0.25 * float(1 - abs(i - 2))));
			a = max(a, clamp(1.0 - d / 0.12, 0.0, 1.0));
		}
		float base = GitdBBSeg(g, vec2(-0.8, -0.6), vec2(0.8, -0.6));
		a = max(a, clamp(1.0 - base / 0.12, 0.0, 1.0));
	}
	else if (id == 3)       // crosshair: ring + four ticks
	{
		a = GitdBBRing(g, 0.62, 0.10);
		for (int i = 0; i < 4; i++)
		{
			vec2 dir2 = (i == 0) ? vec2(1.0, 0.0) : (i == 1) ? vec2(-1.0, 0.0) : (i == 2) ? vec2(0.0, 1.0) : vec2(0.0, -1.0);
			float d = GitdBBSeg(g, dir2 * 0.40, dir2 * 0.92);
			a = max(a, clamp(1.0 - d / 0.10, 0.0, 1.0));
		}
	}
	else if (id == 4)       // double circle
	{
		a = max(GitdBBRing(g, 0.85, 0.10), GitdBBRing(g, 0.50, 0.10));
	}
	else if (id == 5)       // X
	{
		float d1 = GitdBBSeg(g, vec2(-0.7, -0.7), vec2(0.7, 0.7));
		float d2 = GitdBBSeg(g, vec2(-0.7, 0.7), vec2(0.7, -0.7));
		a = max(clamp(1.0 - d1 / 0.13, 0.0, 1.0), clamp(1.0 - d2 / 0.13, 0.0, 1.0));
	}
	else if (id == 6)       // triangle
	{
		float d1 = GitdBBSeg(g, vec2(0.0, 0.85), vec2(0.75, -0.6));
		float d2 = GitdBBSeg(g, vec2(0.75, -0.6), vec2(-0.75, -0.6));
		float d3 = GitdBBSeg(g, vec2(-0.75, -0.6), vec2(0.0, 0.85));
		a = clamp(1.0 - min(d1, min(d2, d3)) / 0.12, 0.0, 1.0);
	}
	else if (id == 7)       // diamond
	{
		float d1 = GitdBBSeg(g, vec2(0.0, 0.9), vec2(0.65, 0.0));
		float d2 = GitdBBSeg(g, vec2(0.65, 0.0), vec2(0.0, -0.9));
		float d3 = GitdBBSeg(g, vec2(0.0, -0.9), vec2(-0.65, 0.0));
		float d4 = GitdBBSeg(g, vec2(-0.65, 0.0), vec2(0.0, 0.9));
		a = clamp(1.0 - min(min(d1, d2), min(d3, d4)) / 0.12, 0.0, 1.0);
	}
	else if (id == 8)       // arrow (points +Y = up in quad space)
	{
		float d1 = GitdBBSeg(g, vec2(0.0, -0.8), vec2(0.0, 0.7));
		float d2 = GitdBBSeg(g, vec2(0.0, 0.85), vec2(-0.5, 0.25));
		float d3 = GitdBBSeg(g, vec2(0.0, 0.85), vec2(0.5, 0.25));
		a = clamp(1.0 - min(d1, min(d2, d3)) / 0.12, 0.0, 1.0);
	}
	else                     // 9+: hollow square (generic marker)
	{
		float sq = max(abs(g.x), abs(g.y));
		a = clamp(1.0 - abs(sq - 0.7) / 0.12, 0.0, 1.0);
	}
	return a * a + a * 0.25;
}

vec4 ProcessTexel()
{
	int packv = GitdBBData();
	int gid = packv & 255;
	int pal = (packv >> 8) & 255;
	float a = GitdBBGlyph(gid, GitdBBQuad());
	vec3 col = GitdBBPal(pal) * uObjectColor.rgb;
	return vec4(col, clamp(a, 0.0, 1.0));
}
