//===========================================================================
//
// [BB] SDF text.
//
// The texture is not a picture of a letter. Each texel holds how far it is
// from the letter's EDGE -- 0.5 is exactly on it, above is inside, below is
// outside -- so this reconstructs the shape rather than resampling an image
// of it, and stays sharp at any magnification.
//
// WHY fwidth AND NOT A SCALE UNIFORM. fwidth(d) is how much the field changes
// between this pixel and the one next to it, which IS the magnification,
// measured rather than passed. Antialiasing one pixel wide either side of the
// edge therefore stays exactly one pixel wide whether the card is across the
// room or filling the screen. A uniform would have to be recomputed per draw
// and would still be wrong on a quad seen at an angle, where the scale is not
// constant across the surface.
//
//===========================================================================

vec4 ProcessTexel()
{
	float d = texture(tex, vTexCoord.st).r;

	// Signed, still in the field's own 0..1 units. Converting to pixels needs
	// the atlas spread, which is not worth a uniform: everything below is a
	// ratio and cancels it out.
	float sd = d - 0.5;

	// max() because fwidth is zero on a perfectly flat run of texels, and a
	// zero-width smoothstep is a divide by zero -- one stray NaN in the alpha
	// takes the whole glyph with it.
	float w = max(fwidth(d), 0.0001);
	float core = smoothstep(-w, w, sd);

	// The halo is read straight out of the field. Neon is brightness falling
	// off with distance from the edge, and distance from the edge is the only
	// thing this texture stores, so there is nothing to blur and no second
	// pass to run.
	//
	// uAddColor carries it: .r is reach as a fraction of the atlas spread,
	// .g is strength. The billboard draw path leaves this uniform at zero and
	// reads it nowhere else, so it is free carriage rather than a hijack.
	//
	// Reach is halved because sd only spans -0.5..0.5 -- a full 1.0 means the
	// whole spread, which is as far as the field can answer for. Past that
	// there is nothing left to read and the falloff would stop dead in a
	// square at the cell boundary.
	float reach = uAddColor.r * 0.5;
	float strength = uAddColor.g;

	float halo = 0.0;
	if (reach > 0.0 && strength > 0.0)
	{
		float h = clamp(1.0 + sd / reach, 0.0, 1.0);

		// NOT SQUARED. It was, and squaring is what made the halo invisible:
		// at half the reach it is already down to a quarter brightness, so the
		// glow only ever occupied the innermost sliver of the field and
		// widening the field did nothing anyone could see.
		//
		// A tight bright core plus a wide soft bleed instead -- which is what
		// neon actually looks like, and what the extra spread was bought for.
		// The cubic term keeps the core hot; the linear term is the part you
		// can see from across a room.
		halo = (h * 0.55 + h * h * h * 0.45) * strength;
	}

	float a = max(core, halo);

	// [BB] THE CORE GOES WHITE, the halo keeps the colour. uAddColor.b carries
	// how far, 0 being the flat single-colour glyph this always drew.
	//
	// Neon is not one colour throughout. The tube's centre is brighter than
	// anything can resolve as a hue and reads white; the colour is the bleed
	// around it. GITD's kill badge is built on exactly that split -- an
	// over-bright white filament plus a saturated halo -- and a glyph without
	// it reads as painted on a surface rather than as a light on its own.
	//
	// Applied against `core` and not against alpha, so it whitens only the
	// solid interior and leaves the falloff coloured all the way out.
	vec3 rgb = mix(uObjectColor.rgb, vec3(1.0), clamp(uAddColor.b * core, 0.0, 1.0));

	return vec4(rgb, a * uObjectColor.a);
}
