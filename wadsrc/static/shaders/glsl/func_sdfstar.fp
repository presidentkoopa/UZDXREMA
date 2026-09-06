//===========================================================================
//
// [BB] A FIVE-POINTED STAR. The symbol, not the astrophysics.
//
// WHAT THIS IS FOR, and it is worth being exact because the first version of
// this file got it wrong in an expensive way: a star CHART draws stars the way
// a chart draws them -- as a glyph, an outline you can put a name inside. It
// does not draw them as points of light. A lens flare with diffraction spikes
// is what a star looks like through a telescope; it is not what a star looks
// like on a map, and a map is what this is.
//
// The practical difference is that a glyph has an INSIDE. That is the whole
// reason the shape matters here: the label goes in the middle of it. A point
// of light has no middle to write in, so a chart built out of them has to hang
// its labels off to one side, and then it is a scatter plot with annotations
// rather than a chart.
//
// WHAT IT DRAWS.
//
//   a star polygon of N points, solved as a distance field
//   either FILLED, or STROKED as an outline of a given width
//   an optional faint interior wash from the gradient colour
//   a halo outside the edge on the usual reach/strength pair
//
// STROKE OR FILL IS THE HIERARCHY. A big hollow star with a name in it is a
// place you can go; a small solid one is dust. One nibble picks between them,
// which means the same payload draws the whole chart -- the named stars, their
// variants, and the hundreds of specks around them -- and they all match
// because they are literally the same shape at different sizes.
//
// The field is a fold: the plane is wrapped into one point's wedge, and what
// is left is a single line segment to measure against. Cheap, exact at any
// size, and -- the reason this is not a texture -- crisp while it is being
// animated, because fwidth solves the edge at whatever size the pixel actually
// landed on screen.
//
//===========================================================================

vec4 ProcessTexel()
{
	// Quad space, -1..1 on both axes, y up.
	vec2 p = vec2(vTexCoord.s * 2.0 - 1.0, 1.0 - vTexCoord.t * 2.0);

	// Same packing as every other payload here: red is the halo reach as a
	// fraction of the spread, green its strength.
	float reach    = clamp(uAddColor.r, 0.0, 1.0);
	float strength = uAddColor.g;

	// Two nibbles in alpha, in the same places the panel and the hex use, with
	// the meanings this shape needs:
	//
	//   high  number of points. Under 3 is not a star, so it means the 5 a
	//         caller passing nothing wants.
	//   low   stroke width, and ZERO MEANS FILLED. That is the hierarchy
	//         switch -- hollow for something you can name and choose, solid
	//         for the dust around it.
	float packed  = floor(uAddColor.a * 255.0 + 0.5);
	float pts     = floor(packed / 16.0);
	float strokeN = mod(packed, 16.0);
	if (pts < 3.0) pts = 5.0;

	// THE STAR FIELD.
	//
	// Fold the plane into one point's wedge and the shape reduces to a single
	// segment. atan(x, y) rather than atan(y, x) puts a point straight UP,
	// which is the orientation every drawn star has had since people started
	// drawing them.
	//
	// 2.6 is the waist. The parameter runs from 2 (needle spikes) to N (a
	// plain polygon); this is the value that gives the shape everyone means
	// when they say "star".
	//
	// 0.58 AND NOT 0.94, and the difference is the whole reason the first
	// version looked broken. A glow has to fall off SOMEWHERE, and the only
	// place it can is the transparent margin between the shape and the quad
	// edge -- past that the quad simply stops and the halo is cut off square,
	// so every star sat in a faint rectangle. Fitting the star to its quad is
	// exactly the wrong instinct for anything that glows: the quad is the
	// canvas, not the frame.
	//
	// The caller pays for this by sizing the billboard about 1.6x larger for
	// the same drawn star, which is cheap -- it is transparent.
	float r  = 0.58;
	float an = 3.14159265 / pts;
	float en = 3.14159265 / 2.6;
	vec2 acs = vec2(cos(an), sin(an));
	vec2 ecs = vec2(cos(en), sin(en));

	float bn = mod(atan(p.x, p.y), 2.0 * an) - an;
	vec2 q = length(p) * vec2(cos(bn), abs(sin(bn)));
	q -= r * acs;
	q += ecs * clamp(-dot(q, ecs), 0.0, r * acs.y / ecs.y);
	float sd = length(q) * sign(q.x);   // negative inside

	// One pixel's worth of field, which is the whole reason this is a payload
	// and not a sprite: the edge is exactly as soft as it needs to be at the
	// size it actually landed, so it never aliases and never blurs while the
	// chart is drawing itself.
	float w = max(fwidth(sd), 0.0001);

	// FILLED, or an outline centred ON the edge rather than inside it. Centred
	// matters: an inset stroke shrinks the shape as it thickens, so a heavy
	// main star and a light variant would not be the same size.
	float shape;
	float halfStroke = 0.0;  // NOT 'half': reserved in GLSL
	if (strokeN < 0.5)
	{
		shape = 1.0 - smoothstep(-w, w, sd);
	}
	else
	{
		halfStroke = (strokeN / 15.0) * 0.20;
		shape = 1.0 - smoothstep(-w, w, abs(sd) - halfStroke);
	}

	// THE INTERIOR WASH, on the same switch every payload here uses: it is
	// uObjectColor2's ALPHA that says whether a second colour was ever set,
	// because blending toward an unset transparent black would quietly fill
	// every star that never asked for it. Set, it puts a faint tint inside a
	// hollow star -- which is how the one you are pointing at says so without
	// changing size and sliding out from under your hand.
	float inside = 1.0 - smoothstep(-w, w, sd + halfStroke);
	vec3 tint = uObjectColor.rgb;

	// HALO, outside the edge only. A glow that also fills the middle would
	// undo the hollowness that the label needs.
	float halo = 0.0;
	if (reach > 0.0 && strength > 0.0)
	{
		// MEASURED AGAINST THE MARGIN, not against the quad. reach is a
		// fraction of the room the halo actually has -- the gap between the
		// shape and the edge -- so reach 1.0 means "use all of it" and can
		// never spill past the quad however the caller sets it. Tying it to
		// the quad instead is what let a perfectly reasonable reach clip.
		float margin = 1.0 - r;
		float outside = max(sd, 0.0);
		float h = clamp(1.0 - outside / max(margin * reach, 0.0001), 0.0, 1.0);
		halo = (h * 0.45 + h * h * h * 0.55) * strength;
	}

	// VOID MODE -- the star as an empty socket: edge only, no wash, no halo.
	// What a slot you do not own should look like on a chart, the place still
	// marked and nothing in it.
	if (uAddColor.b > 0.5)
	{
		float rim = 1.0 - smoothstep(-w, w, abs(sd) - 0.03);
		return vec4(tint * rim * 0.7, rim * 0.55 * uObjectColor.a);
	}

	vec3 rgb = tint;
	float a  = shape;

	if (uObjectColor2.a > 0.0)
	{
		// The wash sits UNDER the stroke, so the outline stays the caller's
		// colour at full strength and only the middle changes.
		float wash = inside * 0.55;
		rgb = mix(uObjectColor2.rgb, tint, shape);
		a   = max(shape, wash);
	}

	if (halo > 0.0)
	{
		rgb = mix(tint * 0.85, rgb, shape);
		a   = max(a, halo * 0.6);
	}

	// EMISSIVE PUSH past 1.0 so a lit star crosses the bloom threshold and
	// throws light into the room rather than merely being a paler outline.
	// Rides the same strength the halo uses, so one number drives the whole
	// reaction.
	rgb *= 1.0 + strength * 1.1;

	return vec4(rgb, clamp(a, 0.0, 1.0) * uObjectColor.a);
}
