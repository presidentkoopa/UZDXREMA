// [GITD-BB] PANEL payload for in-world billboards: a rounded-rect backing
// plate with an optional neon border. Quad-space SDF -- nothing is sampled
// from the bound texture. The packed payload int arrives on uAddColor and the
// billboard's tint on uObjectColor (see HWSprite::DrawSprite).
//
// data byte0 = corner radius, byte1 = border width.
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
// NO u flip here. HWSprite::ProcessBillboard sets the billboard quad's UVs in
// the sprite convention (s = 0 at screen left, t = 0 at the top), so
// vTexCoord already arrives the right way round. Flipping u here would mirror
// every SDF payload AND would still leave BB_TEXTURE mirrored -- that split
// fix is the bug that cost this project months. Read the UV / ORIENTATION
// comment in ProcessBillboard before touching this.
vec2 GitdBBQuad()
{
	return vec2((vTexCoord.s - 0.5) * 2.0, (0.5 - vTexCoord.t) * 2.0);
}

// signed distance to a rounded rectangle centred on the origin
float GitdBBRoundBox(vec2 p, vec2 halfSize, float r)
{
	vec2 q = abs(p) - halfSize + vec2(r);
	return length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
}

vec4 ProcessTexel()
{
	int packv = GitdBBData();
	float corner = float(packv & 255) / 255.0 * 0.60;
	float border = float((packv >> 8) & 255) / 255.0 * 0.16;

	float d = GitdBBRoundBox(GitdBBQuad(), vec2(0.94, 0.94), corner);

	// plate: dark version of the tint, soft-edged, readable backing
	float plate = (1.0 - smoothstep(-0.03, 0.0, d)) * 0.62;
	vec3 col = uObjectColor.rgb * 0.10;
	float a = plate;

	// neon border straddling the plate edge, full tint
	if (border > 0.0)
	{
		float edge = clamp(1.0 - abs(d) / border, 0.0, 1.0);
		float edgeA = edge * edge;
		col = mix(col, uObjectColor.rgb, edgeA);
		a = max(a, edgeA);
	}

	return vec4(col, clamp(a, 0.0, 1.0));
}
