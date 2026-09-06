//
// [STAMP] Surface stamps -- emissive shapes painted onto whatever real surface
// happens to be at a world point.
//
// A stamp is an event, not an object: something was hit at a place, and for a
// short while a shape blooms outward from it. It is not geometry, not a decal
// texture and not a light. It is a term added to the surface's own colour while
// that surface is being shaded, which is what makes it conform for free -- a
// ring crossing a staircase climbs the steps, a ring on a slope follows the
// slope, and nothing ever z-fights because there is nothing to fight with.
//
// THE SHAPES ARE TRANSCRIBED, NOT REDESIGNED
//
// Every shape below is the original's arithmetic, constant for constant. Where
// the original added un-tinted white on top of the tint -- the +0.15 on the
// ring family, the +0.18 on the spiral, the core term on the star, the white
// flash as each hex tile turns over -- that white is kept, because it is what
// makes a stamp read as LIGHT rather than as coloured paint. In a room darkened
// to black these are among the only lit things on screen, and a shape carrying
// only its tint reads flat there. So a shape returns a vec3, never a scalar to
// be multiplied by the tint afterwards; the achromatic part has to survive.
//
// ONE DELIBERATE DIFFERENCE: MEASURED THROUGH SPACE, NOT ACROSS THE GROUND
//
// The original measured horizontal distance only -- the impact's height was
// discarded. On a floor that is fine. On a wall it is fatal: every pixel up the
// wall sits at the same horizontal distance from the hit, so a ring flattens
// into a vertical stripe that is identical floor to ceiling. That is why the
// original needed a separate hand-written set of wall-only patterns.
//
// Here the distance is the full 3D distance, so a stamp is a BUBBLE expanding
// from where the shot landed. It cuts the floor and cuts the wall at the same
// instant, so a shot into a corner reads as one event: the two arcs meet at the
// seam and travel on together. Stairs and slopes come out right for free.
//
// The cost, stated rather than hidden: a stamp no longer looks the same on a
// surface it is far from. Shoot a wall at chest height and the ring reaching
// the floor is smaller than the original would have drawn, because the bubble
// has to travel down before it travels out. That is the correct reading of a
// light source at chest height, but it is a change to how floors look and not
// only to how walls do.
//
// SURFACE-LOCAL FRAME
//
// Distance alone carries the radial shapes. Anything that spins or tiles also
// needs to know which way is "round" and which way is "across", and that answer
// differs per surface. StampFrame builds a 2D basis from the surface normal, so
// an angle or a grid is well defined on a floor, a wall, a ceiling or a slope
// without any of them being special-cased. The original got this for free by
// only ever working in the floor plane, which is the same reason it could not
// leave it.
//
// Honest limit: the basis turns with the surface, so where two surfaces meet at
// an angle it changes. Radial shapes cross that seam invisibly. Shapes with a
// spin or a tiling continue across it but kink, because which way is round
// genuinely does change when the surface does. There is no per-pixel fix short
// of true geodesic distance, which is not something a fragment shader can be
// asked for.
//
// UNIFORM CONTRACT (declared in the shader preamble)
//
//   uSurfaceStampParams  x      how many slots are live; 0 means do nothing
//                        yzw    spare
//   uSurfaceStampPos[i]  xyz    impact point in shader world space (y is up)
//                        w      radius, how far the bubble reaches, map units
//   uSurfaceStampCol[i]  rgb    colour 0..1 -- not packed, and no alpha flag
//                        w      progress 0..1, how far through its life it is
//   uSurfaceStampArg[i]  x      which shape (STAMP_* below)
//                        yzw    axis in WORLD space, for the oriented shapes;
//                               zero for the shapes that have no direction
//   uSurfaceStampMod[i]  x      which surface texture (STAMPTEX_* below), the
//                               second layer; 0 for none
//                        y      that layer's strength, 0..1
//                        zw     spare
//   uSurfaceStampCol2[i] rgb    the colour it grades TOWARD across its life;
//                               equal to the first colour means no gradient
//                        w      fade start, 0..1 of life; 1 = never fade
//
// The gradient and the fade are here rather than driven per tic by the caller
// because a stamp is spawned once and then owned by the engine. The original
// implementation recomputed its colour every single tic and re-pushed the whole
// effect, which is also why it had to ring-buffer its own slots. Handing the
// shader both endpoints and letting it read its own progress gets the same
// picture with one call and no bookkeeping.
//
// The axis is world space and not surface space on purpose. A caller knows the
// direction a shot travelled, or the way a blade swept; it cannot know the
// surface frame, which does not exist until a pixel is being shaded and differs
// per surface. So the caller hands over the direction it actually has and the
// shader projects it -- which is also what makes one gouge run across a floor
// and continue up the wall at the matching angle instead of at an unrelated
// one. StampAxis does the projection.
//
// Colour gets three real lanes rather than being folded into one float with
// arithmetic. The original packed R*65536+G*256+B because it had run out of
// room, which is also why a colour built without alpha silently disabled the
// whole effect with no error anywhere. Nothing here is packed, and nothing here
// is a flag in disguise.
//
// TWO LAYERS, NOT ONE
//
// A stamp draws a SHAPE -- a burst expanding from the impact -- and optionally a
// TEXTURE underneath it, which is not a burst at all but a surface treatment
// keyed to the surface's own up-direction: a band sweeping, sparks falling, a
// slow shimmer. The original ran both at once and that was the point of it. One
// shot put a ring across the ground and sent trails down the wall beside it.
//
// The original could only manage that by giving each surface KIND a different
// one -- bursts on floors, textures on walls -- because its bursts could not
// climb. Here either layer draws on any surface, so the split is the caller's to
// make and not the engine's to assume. Ask for a texture and no shape, a shape
// and no texture, or both, and they land wherever the stamp reaches.
//
// Progress is aged by the engine, not by the caller. A mod spawns a stamp once
// and forgets it: it does not re-push per tic and does not compute its own
// animation phase.
//

// ---------------------------------------------------------------- shape ids
//
// Renumbered. Nothing in this engine consumed the old ordering, and that
// ordering was an artifact of a descending if-chain rather than a decision.
// The names are the shapes, not their old indices.
//
#define STAMP_POOL      0    // plain radial falloff, static
#define STAMP_BAR       1    // oriented bar, opens along its axis
#define STAMP_GOUGE     2    // jagged directional tear
#define STAMP_RING      3    // single expanding ring
#define STAMP_HEXFIELD  4    // hex tiles flipping as the front passes over them
#define STAMP_HEXRING   5    // ring burst, hexagonal, rotating
#define STAMP_SPIRAL    6    // arms winding out of the impact
#define STAMP_BOXRING   7    // ring burst, square, rotating
#define STAMP_STAR      8    // five-lobed burst
#define STAMP_SUNBURST  9    // twelve radial spokes
#define STAMP_GRID     10    // square cells lighting as the front passes
#define STAMP_INVERT   11    // photo-negative flash rather than added light
#define STAMP_BOX      12    // oriented box, border and fill

// How many stamps the uniform block carries. FOUR PLACES have to agree on this
// number, exactly as MAX_BEAMS' 128 does: here, HWViewpointUniforms::mStamp*,
// and the viewpoint block declared in gl_shader.cpp and vk_shader.cpp. A
// uniform block is matched by offset, so a mismatch is silent corruption rather
// than an error.
#define MAX_SURFACE_STAMPS 16

// ------------------------------------------------------------- texture ids
//
// The second layer. Numbered from 1 so 0 means "no texture", and a stamp that
// never sets one is unaffected.
//
#define STAMPTEX_NONE     0
#define STAMPTEX_BREATHE  1    // soft pool, slow pulse and shimmer
#define STAMPTEX_BAND     2    // a band sweeping along the surface up-axis
#define STAMPTEX_CHECKER  3    // cells, each rotating on its own phase
#define STAMPTEX_TRAIL    4    // streaks running one way, red bleed
#define STAMPTEX_TRAILUP  5    // streaks running the other way, orange
#define STAMPTEX_BARS     6    // concentric bars, each decaying on its own beat

// ------------------------------------------------------------------ helpers

// A 2D basis lying in the surface, for the shapes that need an angle or a grid.
// Seeding off the dominant normal component keeps the cross products away from
// degenerate near-parallel cases. Which direction ends up as "across" is
// arbitrary but consistent for a given surface, which is all a tiling or a spin
// needs. On a floor this reduces to the world XZ plane the original worked in.
void StampFrame(vec3 n, out vec3 tx, out vec3 ty)
{
	vec3 seed = (abs(n.y) < 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	tx = normalize(cross(seed, n));
	ty = cross(n, tx);
}

// Project a world-space direction into the surface frame, normalised.
//
// Falls back to the frame's own first axis when the direction has no meaningful
// component in the surface -- which is the ordinary case of shooting straight
// into a wall, where the shot direction IS the surface normal and its shadow on
// the surface is a point. An oriented shape still has to point somewhere, and
// any consistent in-surface direction reads better than a jittering one.
vec2 StampAxis(vec3 worldAxis, vec3 tx, vec3 ty)
{
	vec2 a = vec2(dot(worldAxis, tx), dot(worldAxis, ty));
	float l = length(a);
	return (l > 0.001) ? (a / l) : vec2(1.0, 0.0);
}

// One stable pseudo-random value per impact POINT, for the shapes that want
// noise that differs between hits but never shimmers within one.
//
// Hashes all three components. The original seeded from the impact's world x
// and y, which meant two hits at the same spot on the floor plan but different
// heights -- a wall shot and the floor below it -- drew the identical jagged
// edge. Cheap to include the third axis and it removes that.
float StampSeed(vec3 worldPos)
{
	return fract(sin(dot(worldPos, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

// The original's 2x2 ordered dither, kept. Large soft falloffs on a dark wall
// band badly without it, and these are meant to be seen in the dark.
vec3 StampDither()
{
	return vec3((mod(gl_FragCoord.x, 2.0) * 0.5
	           + mod(gl_FragCoord.y, 2.0) * 0.25 - 0.375) * (1.7 / 255.0));
}

// ------------------------------------------------------------------- shapes
//
// Each returns the colour to ADD, tint and achromatic part together.
//
//   col   the stamp's tint
//   dist  distance from the impact, map units
//   rad   the stamp's radius, map units
//   t     dist / rad, 0..1
//   p     progress through its life, 0..1
//   uv    position in the surface's own 2D frame, map units
//   nrm   uv / rad -- the original's `rel`, normalised by radius
//   axis  the stamp's direction in the surface frame, for the oriented shapes
//

// Plain radial pool. Static -- no animation at all, unlike the wall equivalent.
vec3 StampPool(vec3 col, float t)
{
	return col * (1.0 - t);
}

// Oriented box whose width opens with progress.
vec3 StampBar(vec3 col, vec2 uv, vec2 axis, float rad, float p)
{
	float ax = abs(dot(uv, axis));
	float ay = abs(dot(uv, vec2(-axis.y, axis.x)));
	float hHalf = rad * 0.62;
	float wHalf = rad * 0.30;
	float prog  = max(p, 0.05);
	float box   = max(ax / hHalf, ay / (prog * wHalf));
	return col * smoothstep(1.00, 0.94, box);
}

// Jagged directional tear, with the original's red bleed along its edges.
//
// `org` is the stamp's own world position -- x and z, the two the original
// used. It seeds all four of the noise lanes below, and it has to be the RAW
// coordinate rather than a hash of it: the original multiplies it by 0.01 in
// one lane and by 2.3 in another, and those two numbers only mean anything
// against a world coordinate. Hashing it first and inventing new multipliers
// gives a jagged tear, but not THIS jagged tear.
vec3 StampGouge(vec3 col, vec2 uv, vec2 axis, vec2 org, float rad, float p)
{
	float along = dot(uv, axis);
	float perp  = dot(uv, vec2(-axis.y, axis.x));
	float halfLen = max(p, 0.001) * rad;
	float onB  = step(abs(along), halfLen);
	float anB  = clamp(abs(along) / halfLen, 0.0, 1.0);
	float taper = 1.0 - anB * anB;

	float sdB = along * 0.045 + org.x * 0.01;
	float siB = floor(sdB), sfB = fract(sdB);
	float h0 = fract(sin(siB * 12.9898) * 43758.5453);
	float h1 = fract(sin((siB + 1.0) * 12.9898) * 43758.5453);
	float wob = mix(h0, h1, sfB * sfB * (3.0 - 2.0 * sfB)) - 0.5;
	float jag = fract(sin(along * 0.9 + org.y) * 43758.5453) - 0.5;

	float wHalf  = rad * 0.06 * taper + 0.001;
	float centre = wob * wHalf * 1.6;
	float wj = max(wHalf * (0.7 + 0.55 * jag), 0.001);
	float pj = abs(perp - centre);

	float body = (1.0 - smoothstep(wj * 0.45, wj, pj)) * onB;
	float core = (1.0 - smoothstep(0.0, wj * 0.4, pj)) * onB;
	float scratch = 0.55 + 0.45 * smoothstep(0.1, 0.4,
		fract(sin(floor(along * 0.3) * 7.31 + org.x) * 43758.5453));
	float halo = (1.0 - smoothstep(wj, wj * 2.8, pj)) * onB;
	halo *= (0.35 + 0.65 * fract(sin(along * 1.27 + org.y * 2.3) * 43758.5453));
	float bleed = max(halo - body, 0.0);

	return (col * body + vec3(core * 0.7)) * scratch
	     + vec3(0.5, 0.02, 0.015) * (bleed * 0.85);
}

// Single expanding ring. Constant thickness, no life fade -- as written.
vec3 StampRing(vec3 col, float dist, float rad, float p)
{
	float ringR = p * rad;
	float thick = rad * 0.10;
	return col * (1.0 - smoothstep(0.0, thick, abs(dist - ringR)));
}

// Hex tiles turning over as the front passes. The white flash at each tile's
// halfway point is achromatic on purpose -- it is the tile catching the light,
// not the tile being tinted brighter.
vec3 StampHexField(vec3 col, vec2 uv, float rad, float p)
{
	float cellS = rad * 0.16;
	vec2 hp  = uv / cellS;
	vec2 hgs = vec2(1.0, 1.7320508);
	vec4 hC  = floor(vec4(hp, hp - vec2(0.5, 1.0)) / hgs.xyxy) + 0.5;
	vec4 hh  = vec4(hp - hC.xy * hgs, hp - (hC.zw + vec2(0.5)) * hgs);
	bool firstH = dot(hh.xy, hh.xy) < dot(hh.zw, hh.zw);
	vec2 lp  = firstH ? hh.xy : hh.zw;
	vec2 cid = firstH ? hC.xy : hC.zw + vec2(0.5);

	float cellDist = length(cid * hgs * cellS);
	float wave = p * rad * 1.25;
	float fp = (wave - cellDist) / (rad * 0.22);
	if (fp <= 0.0) return vec3(0.0);

	float flip   = clamp(fp, 0.0, 1.0);
	float squash = max(0.12, abs(cos(flip * 3.14159265)));
	vec2  sp2    = vec2(lp.x, lp.y / squash);
	float hd     = max(dot(abs(sp2), vec2(0.8660254, 0.5)), abs(sp2).x);

	float fill   = 1.0 - smoothstep(0.34, 0.46, hd);
	float edge   = smoothstep(0.30, 0.46, hd) * (1.0 - smoothstep(0.46, 0.54, hd));
	float flashH = 1.0 - abs(flip - 0.5) * 2.0;
	float aH     = min(1.0, fp * 0.9);

	return (col * (fill * 0.35) + (col * 0.6 + vec3(flashH * 0.8)) * edge) * aH;
}

// Rotating hexagonal ring burst.
vec3 StampHexRing(vec3 col, vec2 nrm, float p)
{
	float ca = cos(p * 1.5), sa = sin(p * 1.5);
	vec2 rel = mat2(ca, -sa, sa, ca) * nrm;
	float hd = max(dot(abs(rel), vec2(0.8660254, 0.5)), abs(rel).x);
	float front = p * 1.15;
	if (hd >= front) return vec3(0.0);

	float rings = abs(fract(hd * 7.0 - p * 9.0) - 0.5) * 2.0;
	float mask  = smoothstep(0.78, 1.0, rings);
	float fade  = 1.0 - smoothstep(front - 0.12, front, hd);
	float a     = (p < 0.8) ? 1.0 : (1.0 - (p - 0.8) / 0.2);
	return (col + vec3(0.15)) * (mask * fade * a);
}

// Arms winding out of the impact.
vec3 StampSpiral(vec3 col, vec2 nrm, float p)
{
	float rr = length(nrm);
	float front = p * 1.1;
	if (rr >= front) return vec3(0.0);

	float th = atan(nrm.y, nrm.x) / 6.2831853;
	float spiral = fract(th * 2.0 + rr * 4.0 - p * 3.0);
	float arm = smoothstep(0.14, 0.0, min(spiral, 1.0 - spiral));
	float fadeS = 1.0 - smoothstep(front - 0.12, front, rr);
	float aS = (p < 0.8) ? 1.0 : (1.0 - (p - 0.8) / 0.2);
	return (col + vec3(0.18)) * (arm * fadeS * aS);
}

// Rotating square ring burst.
vec3 StampBoxRing(vec3 col, vec2 nrm, float p)
{
	float ca = cos(p * 0.8), sa = sin(p * 0.8);
	vec2 rel = mat2(ca, -sa, sa, ca) * nrm;
	float sd = max(abs(rel.x), abs(rel.y));
	float front = p * 1.15;
	if (sd >= front) return vec3(0.0);

	float rings = abs(fract(sd * 7.0 - p * 9.0) - 0.5) * 2.0;
	float rm    = smoothstep(0.78, 1.0, rings);
	float fade  = 1.0 - smoothstep(front - 0.12, front, sd);
	float a     = (p < 0.8) ? 1.0 : (1.0 - (p - 0.8) / 0.2);
	return (col + vec3(0.15)) * (rm * fade * a);
}

// Five-lobed burst with a hot centre.
vec3 StampStar(vec3 col, vec2 nrm, float p)
{
	float r   = length(nrm) / max(p * 1.1, 0.05);
	float ang = atan(nrm.y, nrm.x) + p * 0.8;
	float sr  = 0.55 + 0.45 * cos(ang * 5.0);
	float a   = (p < 0.85) ? 1.0 : (1.0 - (p - 0.85) / 0.15);
	if (r >= sr) return vec3(0.0);

	float fill = 1.0 - smoothstep(sr - 0.12, sr, r);
	float core = 1.0 - smoothstep(0.0, sr * 0.5, r);
	return (col * fill + vec3(core * 0.4)) * a;
}

// Twelve rotating radial spokes.
vec3 StampSunburst(vec3 col, vec2 nrm, float p)
{
	float r = length(nrm);
	float front = p * 1.1;
	if (r >= front || r <= 0.02) return vec3(0.0);

	float ang = atan(nrm.y, nrm.x) + p * 1.2;
	float spk = abs(fract(ang / 6.2831853 * 12.0) - 0.5) * 2.0;
	float sm   = smoothstep(0.6, 0.95, spk);
	float fade = (1.0 - smoothstep(front - 0.1, front, r)) * (1.0 - r * 0.3);
	float a    = (p < 0.8) ? 1.0 : (1.0 - (p - 0.8) / 0.2);
	return (col + vec3(0.15)) * (sm * fade * a);
}

// Square cells lighting in a checker as the front passes.
vec3 StampGrid(vec3 col, vec2 uv, float rad, float p)
{
	float cellS = rad * 0.18;
	vec2 g  = uv / cellS;
	vec2 gc = floor(g);
	vec2 gf = fract(g) - 0.5;
	float cellDist = length(gc * cellS);
	float wave = p * rad * 1.3;
	float fp = (wave - cellDist) / (rad * 0.25);
	if (fp <= 0.0) return vec3(0.0);

	float checker = mod(gc.x + gc.y, 2.0);
	float cell = 1.0 - smoothstep(0.35, 0.48, max(abs(gf.x), abs(gf.y)));
	float fl   = max(0.0, 1.0 - abs(clamp(fp, 0.0, 1.0) - 0.5) * 2.0);
	float a    = min(1.0, fp * 0.9);
	return (col * (cell * (0.25 + 0.5 * checker)) + vec3(fl * 0.5) * cell) * a;
}

// Oriented box, border and fill.
vec3 StampBox(vec3 col, vec2 uv, vec2 axis, float rad, float p)
{
	float asp = max(length(axis), 0.001);
	vec2  dir = axis / asp;
	float hH = rad / sqrt(1.0 + asp * asp);
	float hW = asp * hH;
	float ax = abs(dot(uv, dir));
	float ay = abs(dot(uv, vec2(-dir.y, dir.x)));
	float prog = max(p, 0.05);
	float box  = max(ax / hW, ay / (prog * hH));
	float fillv  = smoothstep(1.00, 0.90, box);
	float border = smoothstep(0.80, 0.94, box) * (1.0 - smoothstep(0.97, 1.06, box));
	return col * (fillv * 0.45) + (col + vec3(0.35)) * border;
}

// ----------------------------------------------------------------- textures
//
// The surface layer. Transcribed like the shapes, with one substitution: where
// the original read the pixel's absolute world height it reads height ABOVE THE
// IMPACT in the surface frame (v), and where it read a diagonal horizontal
// coordinate it reads the frame across-axis (u).
//
// On a wall the frame up-axis IS vertical, so these are the original exactly.
// On a floor, where the original had no answer at all, the frame supplies a
// sensible one instead of a constant: a band sweeps across the ground rather
// than lighting the whole of it at once.
//
//   col  tint      u  across the surface, from the impact
//   t    dist/rad  v  up the surface, from the impact
//   p    progress  seed  one stable value per impact point

vec3 StampTexBreathe(vec3 col, float t, float v, float seed)
{
	float c = 1.0 - t;
	float core = c;
	float halo = sqrt(c) * 0.55;
	float breathe = 0.84 + 0.16 * sin(timer * 1.1 + seed * 8.2);
	float shimmer = 0.92 + 0.08 * sin(v * 0.05 + timer * 0.7);
	return col * ((core * 0.65 + halo) * breathe * shimmer);
}

vec3 StampTexBand(vec3 col, float t, float v, float p)
{
	float c2 = (1.0 - t) * (1.0 - t);
	float band = abs(fract(v * 0.035 - p * 1.6) - 0.5) * 2.0;
	float sl = smoothstep(0.55, 0.95, band);
	return (col + vec3(0.08)) * (c2 * (0.25 + 0.75 * sl));
}

vec3 StampTexChecker(vec3 col, float t, float u, float v)
{
	vec2 gp = vec2(u, v) * 0.04;
	vec2 cid = floor(gp);
	vec2 cell = fract(gp) - 0.5;
	float phase = fract(sin(dot(cid, vec2(12.9898, 78.233))) * 43758.5453);
	float ang = timer * 1.6 + phase * 6.2831;
	float ca = cos(ang), sa = sin(ang);
	vec2 rc = mat2(ca, -sa, sa, ca) * cell;
	float sq = max(abs(rc.x), abs(rc.y));
	float block = 1.0 - smoothstep(0.24, 0.34, sq);
	float edge = smoothstep(0.20, 0.30, sq) * (1.0 - smoothstep(0.34, 0.42, sq));
	return col * ((1.0 - t) * (block * 0.5 + edge));
}

vec3 StampTexTrail(vec3 col, float t, float u, float v, float p, float seed)
{
	float c = 1.0 - t;
	float sd = fract(sin(floor(u * 0.12) * 12.9898 + seed * 27.1) * 43758.5453);
	float dr = fract(v * 0.02 + sd * 2.3 + p * 0.9);
	float trail = smoothstep(0.0, 0.45, dr) * (1.0 - smoothstep(0.45, 1.0, dr));
	return col * (c * trail * 0.9) + vec3(0.5, 0.02, 0.015) * (c * trail * 0.5);
}

vec3 StampTexTrailUp(vec3 col, float t, float u, float v, float p, float seed)
{
	float c = 1.0 - t;
	float sd = fract(sin(floor(u * 0.12) * 12.9898 + seed * 27.1) * 43758.5453);
	float rs = fract(v * 0.02 - sd * 2.3 - p * 0.9);
	float trail = smoothstep(0.0, 0.45, rs) * (1.0 - smoothstep(0.45, 1.0, rs));
	return (col + vec3(0.25, 0.10, 0.0)) * (c * trail * 0.95);
}

vec3 StampTexBars(vec3 col, float t, float seed)
{
	float barf = t * 7.0;
	float bar = floor(barf);
	float sd = fract(sin(bar * 7.31 + seed * 13.9) * 43758.5453);
	float t1 = fract(timer * 0.9 + sd);
	float env = exp(-t1 * 3.2);
	float pulse = 0.32 + 0.68 * env;
	float band = 1.0 - smoothstep(0.36, 0.50, abs(fract(barf) - 0.5));
	return col * ((1.0 - t) * pulse * (0.3 + 0.7 * band));
}

vec3 StampTexture(int tex, vec3 col, float t, float u, float v, float p, float seed)
{
	if (tex == STAMPTEX_BREATHE) return StampTexBreathe(col, t, v, seed);
	if (tex == STAMPTEX_BAND)    return StampTexBand(col, t, v, p);
	if (tex == STAMPTEX_CHECKER) return StampTexChecker(col, t, u, v);
	if (tex == STAMPTEX_TRAIL)   return StampTexTrail(col, t, u, v, p, seed);
	if (tex == STAMPTEX_TRAILUP) return StampTexTrailUp(col, t, u, v, p, seed);
	if (tex == STAMPTEX_BARS)    return StampTexBars(col, t, seed);
	return vec3(0.0);
}

// -------------------------------------------------------------- accumulation
//
// STAMP_INVERT is not an addition -- it replaces the surface colour with its
// own negative -- so the accumulator takes `color` by reference rather than
// returning a sum the caller adds. That one shape is why this is not a pure
// function, exactly as in the original.
//
void ApplySurfaceStamps(inout vec3 color, out vec3 addOut, vec3 worldPos, vec3 worldNormal)
{
	addOut = vec3(0.0);
	int count = int(uSurfaceStampParams.x + 0.5);
	if (count <= 0) return;

	vec3 tx, ty;
	StampFrame(normalize(worldNormal), tx, ty);

	vec3 add = vec3(0.0);
	bool hit = false;

	for (int i = 0; i < MAX_SURFACE_STAMPS; i++)
	{
		if (i >= count) break;

		vec4 sp = uSurfaceStampPos[i];
		float rad = sp.w;
		if (rad <= 0.0) continue;

		// The bubble. This one line is the whole cornering behaviour: 3D
		// distance, where the original used horizontal distance only.
		vec3  rel  = worldPos - sp.xyz;
		float dist = length(rel);
		if (dist >= rad) continue;

		vec4 sc = uSurfaceStampCol[i];
		vec4 sa = uSurfaceStampArg[i];
		vec4 s2 = uSurfaceStampCol2[i];

		float t   = dist / rad;
		float p   = clamp(sc.w, 0.0, 1.0);

		// Grade toward the second colour across the stamp's life, then fade out
		// over whatever tail is left after fadeStart. Both are inert at their
		// defaults -- an equal second colour and a fade start of 1 leave a stamp
		// exactly as it was before either existed.
		vec3 col = mix(sc.rgb, s2.rgb, p);
		float fadeAt = clamp(s2.w, 0.0, 1.0);
		if (p > fadeAt && fadeAt < 1.0)
			col *= 1.0 - (p - fadeAt) / max(1.0 - fadeAt, 0.0001);
		vec2  uv  = vec2(dot(rel, tx), dot(rel, ty));
		vec2  nrm = uv / rad;
		vec4 sm = uSurfaceStampMod[i];

		vec2  axis  = StampAxis(sa.yzw, tx, ty);
		float seed  = StampSeed(sp.xyz);
		int   shape = int(sa.x + 0.5);

		float u = dot(rel, tx);
		float v = dot(rel, ty);

		hit = true;

		// Second layer first, so the burst reads as sitting on top of it.
		int tex = int(sm.x + 0.5);
		if (tex != STAMPTEX_NONE)
			add += StampTexture(tex, col, t, u, v, p, seed) * clamp(sm.y, 0.0, 1.0);

		if (shape == STAMP_INVERT)
		{
			// Photo-negative flash. Writes the surface directly; everything
			// else only ever adds to it.
			float strg = p;
			vec3  inv  = clamp(vec3(1.0) - color, 0.0, 1.0);
			float core = 1.0 - smoothstep(rad * 0.60, rad * 0.90, dist);
			color = mix(color, inv, core * strg);
			float rim = 1.0 - smoothstep(0.0, rad * 0.08, abs(dist - rad * 0.84));
			color += inv * (rim * strg * 0.7);
			continue;
		}

		if      (shape == STAMP_BAR)      add += StampBar(col, uv, axis, rad, p);
		else if (shape == STAMP_GOUGE)    add += StampGouge(col, uv, axis, vec2(sp.x, sp.z), rad, p);
		else if (shape == STAMP_RING)     add += StampRing(col, dist, rad, p);
		else if (shape == STAMP_HEXFIELD) add += StampHexField(col, uv, rad, p);
		else if (shape == STAMP_HEXRING)  add += StampHexRing(col, nrm, p);
		else if (shape == STAMP_SPIRAL)   add += StampSpiral(col, nrm, p);
		else if (shape == STAMP_BOXRING)  add += StampBoxRing(col, nrm, p);
		else if (shape == STAMP_STAR)     add += StampStar(col, nrm, p);
		else if (shape == STAMP_SUNBURST) add += StampSunburst(col, nrm, p);
		else if (shape == STAMP_GRID)     add += StampGrid(col, uv, rad, p);
		else if (shape == STAMP_BOX)      add += StampBox(col, uv, axis, rad, p);
		else                              add += StampPool(col, t);
	}

	if (hit) add += StampDither();

	// HANDED BACK, NOT ADDED. The original ended on
	//     color.rgb += desaturate(vec4(wgAdd, 1.0)).rgb;
	// so a stamp in a sector the mapper drained of colour drained with it.
	// That desaturate had been dropped, which left stamps fully saturated in
	// rooms where nothing else is -- the effect visibly ignoring the map.
	//
	// It cannot be called from in here: this lump is prepended AHEAD of
	// main.fp, so main.fp's desaturate() does not exist yet. So the light goes
	// back to the caller and main.fp applies the real function to it, one line
	// after the call. INVERT is the exception and still writes `color`
	// directly, because it replaces the surface rather than lighting it.
	addOut = add;
}
