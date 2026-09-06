// func_spriteoutline.fp -- an actor traced in neon by its own sprite.
//
// WHERE THIS CAME FROM.
//
// This is monster_neon.fp from the previous fork, transcribed. That one was a
// MATERIAL shader: a separate .fp bound by sprite name in gldefs, fifteen
// HardwareShader lines covering the fifteen stock Doom monster sprites, driven
// by one global blackout cvar. It worked, and it had two hard limits -- it knew
// nothing about any monster a mod added, and it was all-or-nothing for the
// whole scene.
//
// Both limits came from WHERE it lived, not from what it did. So the edge
// detect below is the same arithmetic, constant for constant, moved into the
// standard sprite path and driven per actor. Every actor is covered, including
// one a mod added this morning; nothing is named anywhere; and a single corpse
// can light up on its own.
//
// WHY THE OUTLINE IS THE SPRITE.
//
// A silhouette assembled out of quads laid on the floor has to guess the body
// size and has to cope with the ground under it changing height. This has
// neither problem, because there is no second piece of geometry: the outline is
// computed inside the sprite the actor is already drawing. It is exactly the
// right size by construction, it turns when the actor turns, and stairs are not
// a concept it can be wrong about.
//
// COST.
//
// Nine texture samples per fragment, on sprites that opted in. uOutlineParms.w
// is the mode and it is zero for everything else in the level, so the ordinary
// case is one float compare and an early return.

#define OUTLINE_OFF       0
#define OUTLINE_EDGE      1   // keep the body, add a glowing edge
#define OUTLINE_WIRE      2   // erase the body, edge only -- the old blackout look
#define OUTLINE_GHOST     3   // body flattened to the tint, edge bright over it

// This lump is prepended AHEAD of main.fp so main.fp can call it without a
// forward declaration, which means it may only use what the backend preamble
// already declared -- the samplers, timer, the StreamData block. vTexCoord is
// declared inside main.fp itself, so the coordinate comes in as a parameter
// rather than being read from the varying directly.
//
// The emissive half of the effect is handed over in this global. It is set
// during material setup, at the top of main(), and spent much further down in
// getLightColor() alongside the surface stamps -- two different functions, and
// no way to pass a value between them but this.
vec3 gOutlineEmissive = vec3(0.0);

// Returns edge strength in .x and the pulsed colour in .yzw.
vec4 SpriteOutlineEdge(vec2 uv)
{
	vec2 size = vec2(textureSize(tex, 0));

	// uOutlineParms.x is thickness, in texels. It scales the sample offset,
	// which is the only thing that sets how wide the traced line comes out.
	vec2 off = uOutlineParms.x / max(size, vec2(1.0));

	// Sobel over two channels at once: alpha finds the silhouette against the
	// empty part of the sprite cell, luminance finds the detail inside it --
	// the arm against the chest, the horns against the head. Alpha alone traces
	// a blank shape; luminance alone misses the outer edge on a bright
	// background. Taking both is what makes it read as a drawing of the thing
	// rather than a shadow of it.
	float a[9];
	float l[9];

	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			vec4 col = texture(tex, uv + off * vec2(i, j));
			int idx = (j + 1) * 3 + (i + 1);
			a[idx] = col.a;
			l[idx] = dot(col.rgb, vec3(0.299, 0.587, 0.114));
		}
	}

	float sXA = a[0] + 2.0*a[3] + a[6] - a[2] - 2.0*a[5] - a[8];
	float sYA = a[0] + 2.0*a[1] + a[2] - a[6] - 2.0*a[7] - a[8];
	float edgeA = sqrt(sXA*sXA + sYA*sYA);

	float sXL = l[0] + 2.0*l[3] + l[6] - l[2] - 2.0*l[5] - l[8];
	float sYL = l[0] + 2.0*l[1] + l[2] - l[6] - 2.0*l[7] - l[8];
	float edgeL = sqrt(sXL*sXL + sYL*sYL);

	// Luminance at 0.8 so a busy texture does not out-shout the silhouette.
	float edge = max(edgeA, edgeL * 0.8);

	// uOutlineParms.y is the threshold: how much contrast counts as an edge.
	// The 0.4 ramp above it is what stops the line being one hard pixel.
	edge = smoothstep(uOutlineParms.y, uOutlineParms.y + 0.4, edge);

	// Two colours crossfading. A single colour reads as paint; two reading into
	// each other read as something powered. uOutlineColorB.a is the speed, and
	// it is a speed rather than a period because zero then means "hold on A",
	// which is the setting a mod wants when it is driving the colour itself.
	float pulse = sin(timer * uOutlineColorB.a) * 0.5 + 0.5;
	vec3 hue = mix(uOutlineColorA.rgb, uOutlineColorB.rgb, pulse);

	return vec4(edge, hue);
}

// Called from main() the moment the material is set up, which is BEFORE the
// alpha test -- that ordering is the whole reason the wire mode can erase a
// body at all.
//
// `base` is the sprite as the material produced it, and this may rewrite it:
// the wire and ghost modes are about what happens to the BODY. The glowing part
// goes into gOutlineEmissive and is added after the lighting equation and after
// DarknessAt, alongside the surface stamps and the beam light, which is what
// lets a traced corpse stay visible in a room turned black.
void ApplySpriteOutline(inout vec4 base, vec2 uv)
{
	int mode = int(uOutlineParms.w);
	if (mode == OUTLINE_OFF) return;

	vec4 e = SpriteOutlineEdge(uv);
	float edge = e.x;
	vec3 hue = e.yzw;

	// uOutlineParms.z is the glow. The exponential is the halo: it reaches out
	// from a strong edge into the fragments either side of it, so the line has
	// something around it instead of just stopping.
	float glow = exp(-2.0 * (1.0 - edge)) * uOutlineParms.z;

	// uOutlineColorA.a is the master strength, and 0 is a legitimate value a
	// fade passes through on its way out.
	float amt = uOutlineColorA.a;

	if (mode == OUTLINE_WIRE)
	{
		// The body goes. What is left is the edge, and the sprite is a wire
		// figure of itself. The alpha test is dropped to zero on the C++ side
		// for outlined sprites (see HWSprite::DrawSprite), because the default
		// 0.5 would otherwise cut the soft half of every line off.
		base.rgb = hue * (edge * 2.0 + glow);
		base.a = edge * amt;
	}
	else if (mode == OUTLINE_GHOST)
	{
		// The body stays, but flat -- all of the picture inside the silhouette
		// is thrown away and replaced by the tint. Dark tint and it is a hole
		// in the light; bright tint and it is a ghost. Same code, and the two
		// read completely differently in a dark room.
		base.rgb = mix(base.rgb, hue, amt);
	}

	// Every mode gets the emissive line. In EDGE mode this is the whole effect:
	// the sprite is drawn exactly as it always was and the outline is laid over
	// it, so a live monster can be traced without being replaced.
	gOutlineEmissive += hue * (edge * 2.0 + glow) * amt;
}
