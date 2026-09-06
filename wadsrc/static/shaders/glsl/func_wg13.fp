//===========================================================================
//
// [BB] wgType 13 -- GITD's number panel. The ORIGINAL.
//
// This replaces an earlier 7-segment version that stood in for it. That one
// was a readout: hard-edged bars, step() not smoothstep, digits punched black
// out of the plate. This is the other thing entirely -- a neon tube. Glass
// backplate, glyphs from a distance-field atlas, a white-hot filament with the
// colour bleeding out around it, and the whole sign buzzing.
//
// Transcribed from GITD's main.fp lines 1247-1298 plus the four helpers it
// leans on (hash, value noise, neon flicker, warm-up, vibrance). The arithmetic
// is the original's; what changed is listed at the bottom and is only what had
// to.
//
// WHY IT LOOKS LIKE NEON AND A COLOURED NUMBER DOES NOT
//
// Two things, and both are easy to leave out.
//
// THE CORE IS WHITE, NOT THE COLOUR. A real tube's filament is brighter than
// an eye or a sensor can resolve as a hue, so it reads white; the colour is
// the bleed around it. vec3(2.6) is that filament -- over ONE deliberately,
// because the point is a value the display cannot represent, which is what
// bloom then picks up. A glyph drawn in its own colour throughout reads as
// paint on a card no matter how bright it is.
//
// IT IS ADDED, NOT BLENDED OVER. The original was a glow spot: its result went
// into `color.rgb +=` inside the lighting function. Light added to the scene.
// Blended at 30% alpha, that same halo is 30% glyph and 70% dark floor, which
// is grey. That is the difference between a lamp and a sticker, and it is
// decided by the blend mode, not in here -- see BB_WG13 in hw_sprites.cpp,
// which draws this additive for exactly this reason.
//
// ONE QUAD FOR THE WHOLE NUMBER. The digits are not separate draws. This works
// out which digit belongs at which x from the packed value and samples the
// atlas for it, so a five-figure number costs exactly what a one-figure number
// costs, and everything shares one plate.
//
// WHAT CHANGED FROM THE ORIGINAL
//
// THE ATLAS GRID. GITD's neonfont.png packed EIGHT fonts into one sheet -- 128
// x 78 cells, each font a 16 x 6 block, and the wall-pattern lane picked which
// block. sdfmono.png is one font on the same 16 x 6 cell layout, so the block
// offset drops out and the divisor becomes 16 x 6 instead of 128 x 78. Glyph
// indexing is otherwise identical: ASCII minus 32, column is index mod 16, row
// is index over 16.
//
// THE INPUTS. The original read a glow spot's mask lanes. This is a billboard,
// so the number and progress arrive in uAddColor (packed in hw_sprites.cpp) and
// the position is the quad's own UV rather than a distance from a spot centre.
// Every constant below is untouched by that.
//
// NO EARLY RETURN. The original could `return vec4(...)` straight out of
// getLightColor and skip fog, fade and everything after it. A material shader
// has no such exit -- ProcessTexel returns a texel and the pipeline continues.
// Billboards are already fullbright and darkness-exempt, so the practical gap
// is only fog, and a number panel is not usually far enough away to fog.
//
//===========================================================================

// cheap hash + 1D value noise for organic flicker.
float wg13_hash(float n) { return fract(sin(n) * 43758.5453123); }

float wg13_vnoise(float x)
{
	float i = floor(x), f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(wg13_hash(i), wg13_hash(i + 1.0), f);
}

// Master neon flicker multiplier (~0.55 .. 1.15), de-correlated per panel by
// seed so two signs in one room do not buzz in step.
float wg13_flicker(float t, float seed)
{
	float ph = seed * 6.2831853;
	float buzz = 0.5 * sin(t * 458.0 + ph) + 0.5 * sin(t * 572.0 + ph * 1.7);
	buzz = 1.0 + 0.045 * buzz;
	float jit = 1.0 - 0.06 * wg13_vnoise(t * 7.0 + seed * 13.0);
	float breathe = 0.96 + 0.06 * sin(t * 0.9 + ph);
	float drv = wg13_vnoise(t * 0.55 + seed * 31.0);
	float gate = 1.0 - smoothstep(0.0, 0.14, drv);
	float stut = step(0.5, fract(t * 17.0 + seed * 5.0));
	float drop = mix(1.0, mix(0.35, 0.85, stut), gate);
	return clamp(buzz * jit * breathe * drop, 0.55, 1.15);
}

// Spawn warm-up: while brightness ramps 0->1 the tube over-brightens and
// shivers, the way one does when it strikes.
float wg13_warmup(float pb, float t, float seed)
{
	float warm = 1.0 - smoothstep(0.0, 1.0, pb);
	float surge = 1.0 + 0.55 * warm;
	float shiver = 1.0 + 0.20 * warm * sin(t * 95.0 + seed * 9.0);
	return surge * shiver;
}

// Additive-safe vibrance: expand channels away from luma, no white blowout.
vec3 wg13_vibrance(vec3 c, float amt)
{
	float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
	return clamp(mix(vec3(l), c, 1.0 + amt), 0.0, 2.0);
}

vec4 ProcessTexel()
{
	// The number and the brightness, unpacked from where hw_sprites.cpp put
	// them. Red is the low byte, alpha the middle, blue the high -- 24 bits,
	// so up to 16.7 million. Green is progress.
	float pnum = float(int(uAddColor.r * 255.0 + 0.5)
	                 | (int(uAddColor.a * 255.0 + 0.5) << 8)
	                 | (int(uAddColor.b * 255.0 + 0.5) << 16));
	float pbright = clamp(uAddColor.g, 0.0, 1.0);

	vec3 wgCol = uObjectColor.rgb;

	// The quad's own space, -1..1 on both axes. s is flipped so the digits read
	// forward to the viewer rather than mirrored.
	float nx = -(vTexCoord.s * 2.0 - 1.0);
	float ny = vTexCoord.t * 2.0 - 1.0;
	float pny = -ny;                                   // +y = up for the digit maths

	// HOW MANY DIGITS. The original's ladder stopped at five, because its
	// caller clamped the value to 99999 before packing it. The lanes here carry
	// 24 bits -- 16.7 million, eight digits -- so stopping at five would have
	// drawn the wrong digits entirely for anything larger: place-value
	// extraction against too small a length shifts every column.
	//
	// Costs nothing to go the whole way. It is still ONE quad however many
	// digits there are; the number of slots the width is divided into is the
	// only thing that changes.
	float pnlen = (pnum < 10.0)       ? 1.0
	            : (pnum < 100.0)      ? 2.0
	            : (pnum < 1000.0)     ? 3.0
	            : (pnum < 10000.0)    ? 4.0
	            : (pnum < 100000.0)   ? 5.0
	            : (pnum < 1000000.0)  ? 6.0
	            : (pnum < 10000000.0) ? 7.0 : 8.0;

	// Per-panel flicker seed, de-correlated by colour and by value, so two
	// badges showing different numbers do not buzz together.
	float pseed = fract(dot(wgCol, vec3(0.37, 0.71, 0.19)) + pnum * 0.013);
	float pflick = wg13_flicker(timer, pseed) * wg13_warmup(pbright, timer, pseed);

	// ---- the glass plate -------------------------------------------------
	vec2 fp = vec2(nx, ny);
	float rr = length(max(abs(fp) - vec2(0.80, 0.66), 0.0));
	float plate = 1.0 - smoothstep(0.0, 0.34, rr);
	float vign = mix(0.20, 1.0, plate);
	float rim = smoothstep(0.10, 0.0, rr);

	vec3 glassBase = wgCol * 0.10 * vign;
	vec3 rimRGB = (wgCol + vec3(0.20)) * rim * 0.35;
	vec3 outRGB = (glassBase + rimRGB) * pbright * pflick;
	float outA = max(plate * 0.55, rim) * pbright;

	// ---- the digits ------------------------------------------------------
	if (abs(nx) < 0.92 && abs(pny) < 0.78)
	{
		// Which digit slot this pixel falls in, and where inside it.
		float u = (nx / 0.92 * 0.5 + 0.5) * pnlen;
		float di = clamp(floor(u), 0.0, pnlen - 1.0);
		float dx = (u - di) * 2.0 - 1.0;
		float dy = pny / 0.78;

		// The digit itself, dug out of the packed number by place value.
		float dv = mod(floor(pnum / pow(10.0, pnlen - 1.0 - di)), 10.0);

		// Atlas cell. ASCII minus 32, on a 16 x 6 grid.
		float gidx = (48.0 + dv) - 32.0;
		float ccol = mod(gidx, 16.0);
		float crow = floor(gidx / 16.0);
		vec2 lUV = clamp(vec2(dx, -dy) * 0.62 + 0.5, 0.02, 0.98);
		vec2 aUV = (vec2(ccol, crow) + lUV) / vec2(16.0, 6.0);

		float sdf = texture(tex, aUV).r;               // 0.5 = the glyph edge

		const float SAT = 0.55;                        // halo vibrance
		float dd = 0.5 - sdf;                          // >0 outside, <0 inside
		float core = smoothstep(0.03, -0.10, dd);      // the white filament
		float halo = exp(-max(dd, 0.0) * 11.0);        // colour bleeding outward
		halo *= halo;

		vec3 hue = wg13_vibrance(wgCol, SAT);

		// Over ONE on purpose. See the note at the top: a filament is a value
		// the display cannot show, and that is what makes it read as light.
		vec3 coreColor = (vec3(2.6) * core + hue * (halo * 2.2 + core * 0.6))
		               * pbright * pflick;

		// The plate picks up the glyph's spill as well, which is what stops a
		// number looking pasted onto its own backing.
		outRGB += hue * (halo * 0.9 * vign) * pbright * pflick;

		if (halo > 0.0035 || core > 0.001)
		{
			outRGB = coreColor;
			outA = 1.0;
		}
	}

	return vec4(outRGB, outA * uObjectColor.a);
}
