//===========================================================================
//
// [BB] wgType 13 -- a direct transcription of GITD's kill badge.
//
// This is not "inspired by" it and not a reconstruction of how it looked. The
// maths below is the original's, line for line, out of
// GlowInTheDark.pk3:shaders/glsl/main.fp lines 831-873. Earlier attempts
// approximated the look by composing a plate quad with one quad per digit,
// which could never be right: the original draws EVERYTHING IN ONE PASS, and
// that is what lets it punch its digits to black out of its own plate.
//
// WHAT CHANGED AND WHY IT IS ONLY THIS. The original works in world space,
// measuring from a glow spot's centre:
//
//     nBox = length(vec2(abs(nAX) / nhW, abs(nAY) / (nProg * nhH)))
//
// Here the same fragment is already in the quad's own space, -1..1 on both
// axes, so abs(nAX)/nhW IS abs(p.x) and abs(nAY)/nhH IS abs(p.y). The aspect
// the original baked into its facing vector is the quad's width over its
// height instead. Every constant below is untouched.
//
// THE PLATE OPENS VERTICALLY. Dividing the Y term by progress makes a thin
// horizontal slit at low progress that widens into the full lozenge -- and it
// is a LOZENGE, not a circle: GITD sizes it halfW = halfH * (0.60 + digits *
// 0.42), so four digits is 2.28:1. A circular one is a caller passing square
// dimensions, not this shader.
//
// DIGITS ARRIVE AT 55%. The plate opens empty and the number appears once it
// is more than half open. That gate is the original's and it is the reveal.
//
// SEVEN SEGMENTS, HARD EDGED. step(), not smoothstep -- the original has no
// antialiasing on its bars and softening them makes it look like a font
// instead of a readout. The bitmasks 63, 6, 91, 79, 102, 109, 125, 7, 127,
// 111 are the classic 7-segment table, verbatim.
//
// DIGITS ONLY. Sixteen segments and letters live in func_segment.fp; this one
// is the original, and the original could not draw a letter.
//
//===========================================================================

vec4 ProcessTexel()
{
	// Quad space, y up -- the original's nAX/nhW and nAY/nhH.
	vec2 p = vec2(vTexCoord.s * 2.0 - 1.0, 1.0 - vTexCoord.t * 2.0);

	// uAddColor carries what the original packed into its glow spot:
	// .r is progress, .gba are the number as 24 bits.
	float nProg = max(uAddColor.r, 0.05);
	float nnum = floor(uAddColor.g * 255.0 + 0.5) * 65536.0
	           + floor(uAddColor.b * 255.0 + 0.5) * 256.0
	           + floor(uAddColor.a * 255.0 + 0.5);

	float nBox = length(vec2(abs(p.x), abs(p.y) / nProg));

	float nborder = smoothstep(0.80, 0.93, nBox) * (1.0 - smoothstep(0.99, 1.12, nBox));
	float nfill   = (1.0 - smoothstep(0.88, 1.00, nBox));

	vec3 col = uObjectColor.rgb * (nfill * 0.55 + nborder * 0.6);

	if (nProg > 0.55)
	{
		float nlen = (nnum < 10.0) ? 1.0 : (nnum < 100.0) ? 2.0 : (nnum < 1000.0) ? 3.0 : (nnum < 10000.0) ? 4.0 : 5.0;
		float nx = p.x / 0.82;
		float ny = p.y / 0.60;
		if (abs(nx) < 1.0 && abs(ny) < 1.0)
		{
			float u = (nx * 0.5 + 0.5) * nlen;
			float di = clamp(floor(u), 0.0, nlen - 1.0);
			float dx = (u - di) * 2.0 - 1.0;
			float dy = ny;
			float dv = mod(floor(nnum / pow(10.0, nlen - 1.0 - di)), 10.0);
			float m;
			if (dv < 0.5) m = 63.0; else if (dv < 1.5) m = 6.0; else if (dv < 2.5) m = 91.0;
			else if (dv < 3.5) m = 79.0; else if (dv < 4.5) m = 102.0; else if (dv < 5.5) m = 109.0;
			else if (dv < 6.5) m = 125.0; else if (dv < 7.5) m = 7.0; else if (dv < 8.5) m = 127.0;
			else m = 111.0;

			float th = 0.17, sl = 0.55;
			float lit = 0.0;
			lit = max(lit, mod(floor(m / 1.0),  2.0) * step(abs(dy - 0.72), th) * step(abs(dx), sl));
			lit = max(lit, mod(floor(m / 8.0),  2.0) * step(abs(dy + 0.72), th) * step(abs(dx), sl));
			lit = max(lit, mod(floor(m / 64.0), 2.0) * step(abs(dy), th) * step(abs(dx), sl));
			lit = max(lit, mod(floor(m / 32.0), 2.0) * step(abs(dx + 0.52), th) * step(abs(dy - 0.36), 0.36 + th));
			lit = max(lit, mod(floor(m / 2.0),  2.0) * step(abs(dx - 0.52), th) * step(abs(dy - 0.36), 0.36 + th));
			lit = max(lit, mod(floor(m / 16.0), 2.0) * step(abs(dx + 0.52), th) * step(abs(dy + 0.36), 0.36 + th));
			lit = max(lit, mod(floor(m / 4.0),  2.0) * step(abs(dx - 0.52), th) * step(abs(dy + 0.36), 0.36 + th));

			// Punched to black out of the plate. One pass is what makes this
			// possible and is the reason the composed version could not do it.
			col = mix(col, vec3(0.0), lit * nfill);
		}
	}

	// Alpha follows the plate, so the badge is a shape rather than a square.
	float a = clamp(nfill * 0.55 + nborder * 0.6, 0.0, 1.0);
	return vec4(col, a * uObjectColor.a);
}
