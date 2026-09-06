//===========================================================================
//
// [BB] Segment display -- a font with no font.
//
// There is no atlas here and no texture lookup. A character is sixteen bars
// arranged in a fixed frame, and this draws whichever of them the caller says
// are lit. The shape comes out of arithmetic; nothing is sampled.
//
// That is the whole point of it existing next to the SDF path rather than
// being replaced by it. SDF buys arbitrary typefaces at the cost of shipping
// an atlas. This buys nothing except numbers -- and gives back the property
// that made it worth having in the first place: it is pure math, so it is
// infinitely sharp at any size, needs no asset, and cannot be got wrong by a
// bad export. A live-ticking score wants this. A monster's name wants SDF.
//
// SIXTEEN BARS, NOT SEVEN. GITD's original was seven, which cannot draw B, T
// or X -- and its B is indistinguishable from 8, so an ID like B0002 reads as
// 80002. RS_Main names every monster with those letters, so seven was never
// going to survive contact.
//
//        --a1-- --a2--
//       |\     |     /|
//       f  h   i   j  b
//       |    \ | /    |
//        -g1--   --g2-
//       |    / | \    |
//       e  k   l   m  c
//       |/     |     \|
//        --d1-- --d2--
//
// WHERE THE CHARACTER COMES FROM. uAddColor carries it: .b is the low byte of
// the lit-segment mask and .a is the high byte. The mask is computed on the
// CPU, in readable C++, precisely so this file does not need a 36-entry
// lookup chain running per pixel to answer "which bars does R use".
//
// A mask of zero means PLATE -- the bordered backing panel, drawn as its own
// quad underneath the characters. Zero is safe as a sentinel because a space
// emits no quad at all.
//
// GLOW COMES FREE. Every bar is measured as a distance, not painted as a
// shape, so this produces a real distance field the same way the atlas does.
// The halo below is the same falloff as func_sdftext.fp for exactly that
// reason -- the two payloads should not glow differently.
//
//===========================================================================

// Distance to a thick line segment -- a capsule. Every bar is one of these,
// which is why the whole glyph ends up being a distance field rather than a
// stencil.
float segDist(vec2 p, vec2 a, vec2 b, float th)
{
	vec2 pa = p - a;
	vec2 ba = b - a;
	float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
	return length(pa - ba * h) - th;
}

// Pull a bar's ends in a little so neighbouring bars read as separate strokes
// instead of fusing into a blob at the corners. Real displays have this gap.
vec2 shrinkA(vec2 a, vec2 b, float g) { return mix(a, b, g); }

vec4 ProcessTexel()
{
	// Quad UV -> centred coords, y up.
	vec2 p = vec2(vTexCoord.s * 2.0 - 1.0, 1.0 - vTexCoord.t * 2.0);

	float reach    = uAddColor.r * 0.5;
	float strength = uAddColor.g;
	int   mask     = int(uAddColor.b * 255.0 + 0.5) | (int(uAddColor.a * 255.0 + 0.5) << 8);

	// Signed distance, positive INSIDE, to match the SDF path's convention so
	// the shared halo maths below reads the same way in both files.
	float sd = -1e9;

	// 0 and 1 are both plate sentinels: 0 is the LED bed, 1 is the LCD face.
	// Neither collides with a character -- the lowest real mask is 0x000C.
	if (mask <= 1)
	{
		// PLATE -- AN ELLIPSE, not a rounded rectangle.
		//
		// GITD's plate was always an oval and the variable that says so is
		// called `nBox`, which is how it got built wrong here the first time:
		// length() of normalised coordinates is a radial metric, so
		// length(vec2(x/w, y/h)) traces an ellipse whatever the name claims.
		// The thresholds below are GITD's own, kept verbatim -- fill out to
		// 0.88 then fading to the edge, rim riding between 0.80 and 1.12.
		//
		// NOT named `half` -- that is a GLSL reserved word and naming a local
		// after it fails the compile outright.
		vec2 ext = vec2(0.95, 0.92);
		float nb = length(p / ext);

		float fill   = 1.0 - smoothstep(0.88, 1.00, nb);
		float border = smoothstep(0.80, 0.93, nb) * (1.0 - smoothstep(0.99, 1.12, nb));

		// TWO POLARITIES, and the plate has to know which one it is in.
		//
		// LED (mask 0): a faint bed. The characters ADD on top of it, so a
		// bright plate would simply swallow them -- which it did, the first
		// time this was built.
		//
		// LCD (mask 1): GITD's own values, a genuinely lit face. Here the
		// characters SUBTRACT instead, punching themselves out of it dark,
		// so the plate wants to be bright or there is nothing to punch.
		float f = (mask == 0) ? 0.13 : 0.55;
		float b = (mask == 0) ? 0.55 : 0.65;

		float a = clamp(fill * f + border * b, 0.0, 1.0);
		return vec4(uObjectColor.rgb, a * uObjectColor.a);
	}

	// The frame. x narrower than y so characters are taller than wide, which
	// is most of what makes a row of these read as a display rather than as
	// text. X is the tuning knob for letter spacing: raising it fattens the
	// glyphs and closes the gaps, lowering it does the reverse. It trades
	// against the cell width in EmitBillboardSegments, so change one knowing
	// the other exists.
	const float X = 0.66, Y = 0.86;
	vec2 TL = vec2(-X,  Y), TM = vec2(0.0,  Y), TR = vec2( X,  Y);
	vec2 ML = vec2(-X, 0.0), MM = vec2(0.0, 0.0), MR = vec2( X, 0.0);
	vec2 BL = vec2(-X, -Y), BM = vec2(0.0, -Y), BR = vec2( X, -Y);

	const float TH = 0.085;		// bar half-thickness
	const float G  = 0.10;		// end gap, as a fraction of each bar

	// min() over the lit bars: the union of a set of distance fields is the
	// nearest one, so the whole character stays a field.
	#define BAR(bit, A, B) if ((mask & (1 << bit)) != 0) sd = max(sd, -segDist(p, shrinkA(A, B, G), shrinkA(B, A, G), TH));

	BAR( 0, TL, TM)		// a1
	BAR( 1, TM, TR)		// a2
	BAR( 2, TR, MR)		// b
	BAR( 3, MR, BR)		// c
	BAR( 4, BM, BR)		// d2
	BAR( 5, BL, BM)		// d1
	BAR( 6, BL, ML)		// e
	BAR( 7, ML, TL)		// f
	BAR( 8, ML, MM)		// g1
	BAR( 9, MM, MR)		// g2
	BAR(10, TL, MM)		// h
	BAR(11, TM, MM)		// i
	BAR(12, TR, MM)		// j
	BAR(13, BL, MM)		// k
	BAR(14, BM, MM)		// l
	BAR(15, BR, MM)		// m

	#undef BAR

	if (sd < -1e8) return vec4(0.0);

	float w = max(fwidth(sd), 0.0001);
	float core = smoothstep(-w, w, sd);

	// Same curve as func_sdftext.fp, deliberately: a linear term for the bleed
	// you see across a room plus a cubic one to keep the core hot. Two payloads
	// glowing to different rules would be worse than either rule.
	float halo = 0.0;
	if (reach > 0.0 && strength > 0.0)
	{
		float h = clamp(1.0 + sd / reach, 0.0, 1.0);
		halo = (h * 0.55 + h * h * h * 0.45) * strength;
	}

	float a = max(core, halo);
	return vec4(uObjectColor.rgb, a * uObjectColor.a);
}
