//===========================================================================
//
// [BB] SDF panel -- BB_PANEL's job, solved per pixel instead of sampled.
//
// BB_PANEL samples a small rounded-rect texture. That works, it is cheap, and
// it has two limits that only show up once a panel is doing real work:
//
//   * It blurs. The plate is a fixed-resolution image stretched to whatever
//     size the caller asked for, so a card held close in VR shows the
//     interpolation.
//   * It cannot glow. SetBillboardGlow reads the distance field OUTSIDE the
//     shape to place the halo, and a sampled texture has no field to read --
//     which is why the glow gate is `payload >= BB_TEXT` and why a label can
//     have a halo while the plate under it cannot.
//
// This is the same rectangle as a field. Crisp at any size, haloed by the same
// four lines every other field payload uses, and the border is a second
// distance test rather than a second quad.
//
// NOT a replacement for BB_PANEL. Sampling one small texture is cheaper than
// solving two distance fields, and a ring of forty background plates that
// nobody looks closely at should stay sampled. Both exist; the caller picks.
//
//===========================================================================

vec4 ProcessTexel()
{
	// Quad space, -1..1 on both axes, y up.
	vec2 p = vec2(vTexCoord.s * 2.0 - 1.0, 1.0 - vTexCoord.t * 2.0);

	// Same packing as the other field payloads: red is the halo reach as a
	// fraction of the spread, green its strength.
	float reach    = uAddColor.r * 0.5;
	float strength = uAddColor.g;

	// THE SHAPE NUMBERS RIDE IN uAddColor's ALPHA, two nibbles.
	//
	// High nibble is the corner radius, low nibble the border width, each
	// 0..15 across the half-extent. Sixteen steps is coarse and it is enough:
	// these are a corner and a hairline, not a measurement.
	//
	// Alpha because it is the only channel left and it is genuinely free --
	// rgb already carry halo reach, halo strength and the void flag, and
	// nothing downstream reads uAddColor.a. uSpecularMaterial was the obvious
	// alternative and is not usable: it is filled from the TEXTURE's glossiness
	// and specular level, and only on the GL backend.
	float packed = floor(uAddColor.a * 255.0 + 0.5);
	float radius = floor(packed / 16.0) / 15.0;
	float border = mod(packed, 16.0) / 15.0;

	// A radius of 1 would round the rectangle into a lozenge and swallow the
	// whole plate, so the useful range stops short of the half-extent.
	radius = clamp(radius, 0.0, 1.0) * 0.6;
	border = clamp(border, 0.0, 1.0) * 0.35;

	// Rounded box, signed, POSITIVE INSIDE -- the same convention as the seam
	// and segment payloads so the halo below is identical code in all three.
	vec2 ext = vec2(1.0, 1.0) - vec2(radius);
	vec2 q = abs(p) - ext;
	float sd = -(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius);

	// One pixel's worth of the field, so the edge is exactly as soft as it
	// needs to be at whatever size this ended up on screen. This is the whole
	// trick -- fwidth is why it never aliases and never blurs.
	float w = max(fwidth(sd), 0.0001);
	float core = smoothstep(-w, w, sd);

	// GRADIENT. uObjectColor2's ALPHA is the switch, not its colour: zero
	// means the caller never set one, and treating that as "blend toward
	// transparent black" would quietly darken every panel that never asked.
	// Runs top to bottom, which is the axis a lit surface reads along.
	vec3 tint = uObjectColor.rgb;
	if (uObjectColor2.a > 0.0)
	{
		float g = clamp(0.5 - p.y * 0.5, 0.0, 1.0);
		tint = mix(uObjectColor.rgb, uObjectColor2.rgb, g);
	}

	// The border is the field again, inset. A second distance test costs a
	// subtract and a smoothstep; a second quad would cost another draw, would
	// have to be kept in step with the plate's size by the caller, and would
	// z-fight it.
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

	// VOID MODE -- the panel is a hole rather than a plate.
	//
	// Interior goes dark and only the rim burns, which is what the edge of an
	// opening looks like. Same flag and same reading as the seam payload, so a
	// caller that knows one knows the other.
	if (uAddColor.b > 0.5)
	{
		float rim = 1.0 - smoothstep(0.0, 0.10, sd);
		vec3 rgb = tint * rim;
		float a = max(core * 0.85, halo);
		return vec4(rgb, a * uObjectColor.a);
	}

	// The border rides BRIGHTER than the fill rather than being its own
	// colour. One colour in, two tones out -- a caller setting a border does
	// not also have to pick a second hue that works with the first.
	vec3 rgb = mix(tint, tint * 1.9 + vec3(0.06), edge);

	// GLOSS -- a highlight that slides across the plate as you move.
	//
	// This is what stops a card reading as a coloured rectangle and starts it
	// reading as an object with a SURFACE. The trick is that it must be tied to
	// the viewer: a highlight that sits still is just a painted stripe, and the
	// eye knows the difference immediately.
	//
	// pixelpos is world space and uCameraPos is where you are, so their
	// difference is a real view vector -- no approximation and nothing for the
	// caller to feed in. Projecting it onto the quad's own axes gives a band
	// position that slides as you turn your head or walk past, exactly as a
	// specular streak on a real panel would.
	//
	// timer adds a slow drift on top, so a card is never completely dead even
	// when you are standing still. Small on purpose: the view term should
	// dominate or it stops reading as reflection and starts reading as an
	// animation.
	if (strength > 0.0)
	{
		vec3 viewDir = normalize(uCameraPos.xyz - pixelpos.xyz);

		// Two axes of the quad's own basis, recovered from screen-space
		// derivatives of world position -- no extra varying, no vertex change.
		vec3 dpx = dFdx(pixelpos.xyz);
		vec3 dpy = dFdy(pixelpos.xyz);
		vec3 nrm = normalize(cross(dpx, dpy));

		// How square-on you are to the plate. Glancing angles get the streak;
		// dead-on gets almost none, which is the correct behaviour for a
		// glossy surface and the reason it feels like a material.
		float facing = clamp(dot(nrm, viewDir), -1.0, 1.0);
		float grazing = 1.0 - abs(facing);

		// Band position: the view vector projected onto the plate, plus drift.
		float slide = dot(viewDir, normalize(dpx + vec3(1e-6))) * 1.6
		            + timer * 0.15;

		// A diagonal band, so it crosses the card rather than running parallel
		// to an edge and reading as a border.
		float band = (p.x * 0.7 + p.y * 0.7) - slide;
		float gloss = exp(-band * band * 5.0);

		// Only inside the shape, and never on the halo -- a glow with a
		// highlight in it looks like two effects fighting.
		gloss *= core * (0.25 + 0.75 * grazing);

		rgb += vec3(gloss * strength * 0.85);
	}

	// EMISSIVE PUSH, so the card reaches the bloom threshold.
	//
	// Bloom keys off brightness, and a plate sitting politely under 1.0 never
	// crosses it however saturated its colour is. Scaling past 1 on the hovered
	// card is what makes it throw a streak across the room rather than merely
	// being a lighter rectangle -- and because it rides on the same strength the
	// halo uses, one slider drives the whole effect.
	rgb *= 1.0 + strength * 0.9;

	float a = max(core, halo);
	return vec4(rgb, a * uObjectColor.a);
}
