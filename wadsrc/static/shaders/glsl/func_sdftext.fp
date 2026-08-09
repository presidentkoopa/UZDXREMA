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
		halo = clamp(1.0 + sd / reach, 0.0, 1.0);
		halo *= halo;			// squared, so it hugs the letter instead of hazing
		halo *= strength;
	}

	float a = max(core, halo);
	return vec4(uObjectColor.rgb, a * uObjectColor.a);
}
