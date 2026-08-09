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
	// pass to run. Squared so it hugs the letterform instead of hazing.
	//
	// The 5.0 keeps the falloff inside the generator's spread. Push it lower
	// and the halo runs off the end of the field and clips to a hard square
	// at the cell boundary -- confirmed in tools/sdffont/sdfpreview.ps1 before
	// this shader existed.
	float halo = clamp(1.0 + sd * 5.0, 0.0, 1.0);
	halo *= halo;

	float a = max(core, halo * 0.55);
	return vec4(uObjectColor.rgb, a * uObjectColor.a);
}
