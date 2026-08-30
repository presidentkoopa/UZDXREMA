//===========================================================================
//
// [BB] SDF hexagon -- a tessellating cell, solved per pixel.
//
// WHY A HEXAGON NEEDS ITS OWN PAYLOAD RATHER THAN A TEXTURE.
//
// A rounded rect can be sampled and nobody notices, because the shapes in a
// ring never touch: every plate has daylight around it and a soft edge reads
// as a soft edge. A honeycomb is the opposite case by definition -- the cells
// SHARE EDGES, and a shared edge is the one place a sampled shape cannot hide.
// Two neighbours whose borders are each half a pixel soft do not meet, they
// overlap into a seam, and a comb of forty of them reads as a mesh of grey
// lines rather than as tiles. The field is what makes an edge exact.
//
// It also has to survive scale. A comb that opens -- cells sliding apart to
// let new ones in between -- changes every cell's size on screen mid-motion,
// and a sprite blurs on exactly the frames the eye is following. fwidth
// solves the edge at whatever size the pixel ended up being, so a cell is as
// crisp travelling as it is parked.
//
// Everything else here is deliberately identical to func_sdfpanel: the same
// halo, the same gradient rule, the same border-as-second-field, the same
// void mode, the same view-tied gloss. A caller that knows the panel knows
// this, and the two sit side by side in a layout without looking like they
// came from different mods.
//
// POINTY-TOP, and that is a real choice rather than a default. Pointy-top
// hexes tile in columns that read as rows to the eye, which is what makes a
// slot's weapons laid in adjacent cells look like a run rather than a
// staircase. Flat-top is the better shape for a single badge; this one exists
// to be one of many.
//
//===========================================================================

vec4 ProcessTexel()
{
	// Quad space, -1..1 on both axes, y up.
	vec2 p = vec2(vTexCoord.s * 2.0 - 1.0, 1.0 - vTexCoord.t * 2.0);

	// Same packing as every other field payload: red is the halo reach as a
	// fraction of the spread, green its strength.
	float reach    = uAddColor.r * 0.5;
	float strength = uAddColor.g;

	// Two nibbles in alpha, exactly as the SDF panel reads them: high is the
	// corner rounding, low is the border width, each 0..15 across the
	// half-extent. Kept identical so the same ZScript shape argument means the
	// same thing whichever of the two a caller picks.
	float packed = floor(uAddColor.a * 255.0 + 0.5);
	float radius = floor(packed / 16.0) / 15.0;
	float border = mod(packed, 16.0) / 15.0;

	// A hexagon has far less room to round than a rectangle before it stops
	// being a hexagon -- its corners are 120 degrees, not 90, so the same
	// nominal radius eats much more of the edge. Hence 0.3 here against the
	// panel's 0.6.
	radius = clamp(radius, 0.0, 1.0) * 0.3;
	border = clamp(border, 0.0, 1.0) * 0.35;

	// THE HEXAGON FIELD.
	//
	// Three mirrors -- across x, and across the two 60-degree diagonals --
	// fold the plane into a single 30-degree wedge, after which the shape is
	// one straight edge and the distance is a segment test. k is
	// (-cos30, sin30, tan30).
	//
	// Inset by the rounding first and added back after, which is the standard
	// way to round any field: shrink the shape, then grow the surface.
	const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);

	// POINTY-TOP, AND THE SWAP IS WHAT MAKES IT ONE.
	//
	// The field below is the standard hexagon distance test, and as written it
	// puts its flat edges at the TOP and BOTTOM with vertices out to the left
	// and right -- a flat-top hex. The header above claimed pointy-top, the
	// caller's lattice was built for pointy-top, and the shape drawn was
	// rotated thirty degrees away from both. Cells then meet vertex-to-flat
	// instead of flat-to-flat, which is not a tessellation at all: it is a
	// scatter of overlapping hexagons, and it looked like one.
	//
	// Swapping the axes rotates the shape by ninety degrees, which for a
	// six-fold shape is the same as thirty. One line, and it is the difference
	// between a comb and a mess.
	vec2 ph = p.yx;

	// Fit to the quad. The apothem (centre to flat) is r; the circumradius
	// (centre to vertex) is 2r/sqrt(3), which is 1.155r -- so r must stay under
	// 0.866 or the points push past the quad edge and clip square.
	float r = 0.866 - radius;

	vec2 q = abs(ph);
	q -= 2.0 * min(dot(k.xy, q), 0.0) * k.xy;
	q -= vec2(clamp(q.x, -k.z * r, k.z * r), r);

	// NEGATED to positive-inside, the convention every field payload here
	// shares so the halo and border code below is the same code in all of
	// them.
	float sd = -(length(q) * sign(q.y) - radius);

	// One pixel's worth of field. This is the whole reason the payload exists:
	// the edge is exactly as soft as it needs to be at the size it actually
	// landed on screen, so it never aliases and never blurs.
	float w = max(fwidth(sd), 0.0001);
	float core = smoothstep(-w, w, sd);

	// GRADIENT, on the same switch as the panel: uObjectColor2's ALPHA says
	// whether a second colour was ever set. Zero means it was not, and
	// blending toward transparent black would quietly darken every cell that
	// never asked for a gradient.
	vec3 tint = uObjectColor.rgb;
	if (uObjectColor2.a > 0.0)
	{
		float g = clamp(0.5 - p.y * 0.5, 0.0, 1.0);
		tint = mix(uObjectColor.rgb, uObjectColor2.rgb, g);
	}

	// The border is the field again, inset -- a subtract and a smoothstep,
	// against a second quad's extra draw call, its own size to keep in step,
	// and its z-fighting.
	//
	// It matters more here than on a panel: in a tessellation the border IS
	// the grid. Turn it off and a comb of one colour becomes a single blob
	// with no cell boundaries at all.
	float edge = 0.0;
	if (border > 0.0)
	{
		edge = core - smoothstep(-w, w, sd - border);
		edge = clamp(edge, 0.0, 1.0);
	}

	float halo = 0.0;
	if (reach > 0.0 && strength > 0.0)
	{
		float h = clamp(1.0 + sd / reach, 0.0, 1.0);
		halo = (h * 0.55 + h * h * h * 0.45) * strength;
	}

	// VOID MODE -- the cell as a hole rather than a tile. Same flag and same
	// reading as the panel and the seam.
	if (uAddColor.b > 0.5)
	{
		float rim = 1.0 - smoothstep(0.0, 0.10, sd);
		vec3 rgb = tint * rim;
		float a = max(core * 0.85, halo);
		return vec4(rgb, a * uObjectColor.a);
	}

	// The border rides brighter than the fill rather than carrying its own
	// hue, so one colour in gives two tones out and a caller setting a border
	// does not also have to choose a second colour that works with the first.
	vec3 rgb = mix(tint, tint * 1.9 + vec3(0.06), edge);

	// GLOSS, tied to the viewer rather than to a clock. A highlight that sits
	// still is a painted stripe; one that slides as you move is a surface. The
	// view vector is real -- pixelpos is world space and uCameraPos is where
	// you are -- so nothing has to be fed in by the caller.
	//
	// On a comb this does something it cannot do on a lone plate: neighbouring
	// cells catch the band at slightly different moments, so the sheen travels
	// ACROSS the tessellation as you turn. That is the effect that makes a
	// honeycomb read as one curved surface rather than as separate tiles.
	if (strength > 0.0)
	{
		vec3 viewDir = normalize(uCameraPos.xyz - pixelpos.xyz);

		vec3 dpx = dFdx(pixelpos.xyz);
		vec3 dpy = dFdy(pixelpos.xyz);
		vec3 nrm = normalize(cross(dpx, dpy));

		float facing = clamp(dot(nrm, viewDir), -1.0, 1.0);
		float grazing = 1.0 - abs(facing);

		float slide = dot(viewDir, normalize(dpx + vec3(1e-6))) * 1.6
		            + timer * 0.15;

		// Crosses the cell corner to corner rather than running parallel to a
		// flat, which on a hexagon would sit exactly along an edge and read as
		// a second border.
		float band = (p.x * 0.7 + p.y * 0.7) - slide;
		float gloss = exp(-band * band * 5.0);

		gloss *= core * (0.25 + 0.75 * grazing);

		rgb += vec3(gloss * strength * 0.85);
	}

	// EMISSIVE PUSH so a lit cell crosses the bloom threshold and throws light
	// into the room, rather than merely being a paler hexagon. Rides the same
	// strength the halo uses, so one number drives the whole reaction.
	rgb *= 1.0 + strength * 0.9;

	float a = max(core, halo);
	return vec4(rgb, a * uObjectColor.a);
}
