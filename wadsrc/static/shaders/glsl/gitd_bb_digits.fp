// [GITD-BB] DIGITS payload for in-world billboards: a seven-segment numeric
// readout. Quad-space SDF -- nothing is sampled from the bound texture. The
// packed payload int arrives on uAddColor and the billboard's tint on
// uObjectColor (see HWSprite::DrawSprite).
//
// data = value (bits 0-16) | palette (bits 17+).
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
// comment in HWSprite::ProcessBillboard. This payload lays digits out most-
// significant-first toward +x, so a flipped u reverses the number.
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

// segment bits: 1=A top, 2=B top-right, 4=C bottom-right, 8=D bottom,
// 16=E bottom-left, 32=F top-left, 64=G middle. An if-chain rather than an
// int[10] constructor on purpose: array constructors are not reliably
// available on the GLES backend, and this file has to compile there as-is.
int GitdBBSegMask(int d)
{
	if (d == 1) return 0x06;
	if (d == 2) return 0x5B;
	if (d == 3) return 0x4F;
	if (d == 4) return 0x66;
	if (d == 5) return 0x6D;
	if (d == 6) return 0x7D;
	if (d == 7) return 0x07;
	if (d == 8) return 0x7F;
	if (d == 9) return 0x6F;
	return 0x3F; // 0
}

float GitdBBDigit(int d, vec2 g)
{
	int mask = GitdBBSegMask(d);
	float best = 1e9;
	if ((mask & 1)  != 0) best = min(best, GitdBBSeg(g, vec2(-0.55, 1.0),  vec2(0.55, 1.0)));
	if ((mask & 2)  != 0) best = min(best, GitdBBSeg(g, vec2(0.55, 1.0),   vec2(0.55, 0.0)));
	if ((mask & 4)  != 0) best = min(best, GitdBBSeg(g, vec2(0.55, 0.0),   vec2(0.55, -1.0)));
	if ((mask & 8)  != 0) best = min(best, GitdBBSeg(g, vec2(-0.55, -1.0), vec2(0.55, -1.0)));
	if ((mask & 16) != 0) best = min(best, GitdBBSeg(g, vec2(-0.55, 0.0),  vec2(-0.55, -1.0)));
	if ((mask & 32) != 0) best = min(best, GitdBBSeg(g, vec2(-0.55, 1.0),  vec2(-0.55, 0.0)));
	if ((mask & 64) != 0) best = min(best, GitdBBSeg(g, vec2(-0.55, 0.0),  vec2(0.55, 0.0)));
	float core = clamp(1.0 - best / 0.14, 0.0, 1.0);
	float halo = clamp(1.0 - best / 0.55, 0.0, 1.0);
	return core * core + halo * halo * 0.25;
}

vec4 ProcessTexel()
{
	int packv = GitdBBData();
	int num   = packv & 0x1FFFF;
	int cidx  = (packv >> 17) & 255;
	vec2 q = GitdBBQuad();

	// only as many cells as the number needs; most significant digit on the
	// screen-left end, so the readout reads left-to-right like a number.
	int n = num >= 10000 ? 5 : num >= 1000 ? 4 : num >= 100 ? 3 : num >= 10 ? 2 : 1;
	float cellW = 2.0 / float(n);
	float a = 0.0;
	for (int i = 0; i < 5; i++)
	{
		if (i >= n) break;
		float cx = -1.0 + (float(i) + 0.5) * cellW;
		vec2 g = vec2((q.x - cx) / (cellW * 0.34), q.y / 0.60);
		if (abs(g.x) > 1.5 || abs(g.y) > 1.4) continue;
		int p = 1;
		for (int k = 0; k < n - 1 - i; k++) p *= 10;
		a = max(a, GitdBBDigit((num / p) % 10, g));
	}

	vec3 col = GitdBBPal(cidx) * uObjectColor.rgb;
	return vec4(col, clamp(a, 0.0, 1.0));
}
