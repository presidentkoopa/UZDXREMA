//===========================================================================
//
// [BB] Seam -- a glowing slit that opens.
//
// GITD's wgType 1 was an oriented box whose width scaled with progress:
//
//     float wgBox = max(wgAX / wgHHalf, wgAY / (wgProg * wgWHalf));
//     wgAdd = wgCol * smoothstep(1.00, 0.94, wgBox);
//
// A rectangle that widens, and nothing more. This is that with a soft edge, a
// hot line down the middle so it reads as an OPENING rather than a plain
// panel, and the same halo the other payloads use.
//
// THE OPENING IS NOT IN HERE. GITD baked progress into the shader because its
// decals had no other way to animate. A billboard does: ResizeBillboard drives
// the width from script, which means the caller owns the easing curve, can
// pause it, reverse it, or hold it open -- none of which a progress term
// hardcoded here would allow.
//
// FLAT OR VERTICAL IS FREE. This draws in the quad's own space and knows
// nothing about which way the quad faces, so tilt 90 lays it on the floor and
// tilt 0 stands it up as a door. GITD's could only ever be flat, because it
// was painted into a floor rather than being a thing in the world.
//
//===========================================================================

vec4 ProcessTexel()
{
	vec2 p = vec2(vTexCoord.s * 2.0 - 1.0, 1.0 - vTexCoord.t * 2.0);

	float reach    = uAddColor.r * 0.5;
	float strength = uAddColor.g;

	// Rounded box, signed, positive inside -- same convention as the other two
	// payloads so the halo below is the same code.
	vec2 ext = vec2(0.93, 0.88);
	vec2 q = abs(p) - ext;
	float sd = -(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0));

	float w = max(fwidth(sd), 0.0001);
	float core = smoothstep(-w, w, sd);

	// The hot line along the long axis. Without it a seam is just a lit
	// rectangle; with it the eye reads a split that something could come
	// through, which is the whole point of the effect.
	float slit = 1.0 - smoothstep(0.0, 0.42, abs(p.y));
	slit *= slit;

	float halo = 0.0;
	if (reach > 0.0 && strength > 0.0)
	{
		float h = clamp(1.0 + sd / reach, 0.0, 1.0);
		halo = (h * 0.55 + h * h * h * 0.45) * strength;
	}

	// VOID MODE -- the seam is a hole rather than a panel.
	//
	// A lit slab is the wrong read for something that is supposed to open and
	// let a monster out: the monster ends up standing in FRONT of a light
	// instead of coming from anywhere. So the interior goes dark and only the
	// rim burns, which is what the edge of a hole looks like.
	//
	// The rim is measured from the same distance field, so it tracks the shape
	// automatically as the caller widens the quad -- there is nothing to keep
	// in step by hand.
	if (uAddColor.b > 0.5)
	{
		float rim = 1.0 - smoothstep(0.0, 0.13, sd);	// bright only near the edge
		float inner = smoothstep(0.0, 0.20, sd);		// 1 well inside the hole

		// Interior is drawn as near-black rather than as nothing, so the hole
		// occludes the floor texture instead of letting it show through.
		vec3 rgb = uObjectColor.rgb * rim;
		float a = max(max(core * mix(1.0, 0.82, inner), halo), core);
		return vec4(rgb, a * uObjectColor.a);
	}

	float a = max(core * (0.35 + 0.65 * slit), halo);
	return vec4(uObjectColor.rgb, a * uObjectColor.a);
}
