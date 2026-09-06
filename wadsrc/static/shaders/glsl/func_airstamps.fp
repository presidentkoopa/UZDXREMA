//===========================================================================
//
// [BB] AIR STAMPS -- wgTypes 14, 15, 16, 17, 18, 19 and 20. The ORIGINALS.
//
// The old wgType set was two families, not one, and only one of them ever got
// ported. Shapes 0-12 lie flat on floors and walls and are in
// func_surfacestamps.fp. Shapes 13 and up hang in the AIR at a point, facing
// the camera, and of those only 13 -- the number panel -- had been brought
// across. These seven are the rest of the ones worth having.
//
// This is what was missing from impacts. A shot produced the flat pattern on
// the surface and nothing at all in the air; the original also gave you a ring
// expanding off the wall, a white disc flash at the hit, a smoke puff, and the
// casing and shard thrown out of the gun.
//
// Transcribed from GITD main.fp lines 947-1099, constant for constant.
//
//   14  SHOCKWAVE RING    a contour marching outward, thinning as it grows
//   15  FILLED DISC FLASH a white-hot disc that snaps off
//   16  CASING            a rounded-box shell with the damage number stamped on it
//   17  SHARD             a four-armed spark with a hot centre
//   18  CORNER BRACKETS   a target reticle: four L corners and a centre pip
//   19  WAVEFORM          an oscilloscope trace over a baseline
//   20  SMOKE PUFF        a soft haze, lumped out of round by value noise
//
// 21, 22, 23 and 26 are deliberately not here.
//
// WHAT CHANGED, AND IT IS ONLY WHAT HAD TO
//
// THE EARLY RETURN. The original ran inside getLightColor and could
// return vec4(coreColor, 1.0) straight out of the lighting function, which both
// drew the shape and skipped everything after it. Where that test failed it
// instead spilled wgAdd onto the surface behind. A material shader has no such
// exit -- ProcessTexel returns a texel -- so the same either/or is written out
// longhand at the bottom of each shape: the core where the test passes, the
// halo spill where it does not. Per fragment that is the original behaviour
// exactly, because the early return meant a fragment could never take both.
//
// SMOKE HAS NO CORE. Shape 20 never had an early return in the original either
// -- it is additive haze and nothing else -- so it returns its spill and that
// is the whole shape. Written the same way here.
//
// THE ATLAS GRID, for the casing only. GITD neonfont.png packed eight fonts
// into 128 x 78 cells and the wall-pattern lane picked the block. sdfmono.png
// is one font on the same 16 x 6 cell layout, so the block offset drops out and
// the divisor becomes 16 x 6. This is the same change func_wg13.fp made, for
// the same reason. Glyph indexing is otherwise identical.
//
// THE HELPERS are copied rather than shared. A material shader is compiled as
// its own program with main.fp, so wg13_flicker does not exist here even though
// it is the same function -- exactly as wg13_ itself is a copy of the original
// radiance_ helpers. Byte identical to both.
//
// INPUTS, as packed by hw_sprites.cpp:
//
//   uObjectColor.rgb   the colour                           (old wgCol)
//   uObjectColor2.r    which shape, 14..20                  (old wgType)
//   uAddColor.g        the animation lane, 1 -> 0 over life (old wgMask.y)
//   uAddColor.r/.a/.b  a 24-bit number, low/mid/high        (old wgMask.z)
//
// Only the casing reads the number. Everything else ignores it, and it still
// feeds the flicker seed so two effects of the same colour do not buzz in step.
//
//===========================================================================

// cheap hash + 1D value noise for organic flicker.
float air_hash(float n) { return fract(sin(n) * 43758.5453123); }

float air_vnoise(float x)
{
	float i = floor(x), f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(air_hash(i), air_hash(i + 1.0), f);
}

// Master neon flicker multiplier (~0.55 .. 1.15), de-correlated per effect by
// seed so two of these in one room do not buzz together.
float air_flicker(float t, float seed)
{
	float ph = seed * 6.2831853;
	float buzz = 0.5 * sin(t * 458.0 + ph) + 0.5 * sin(t * 572.0 + ph * 1.7);
	buzz = 1.0 + 0.045 * buzz;
	float jit = 1.0 - 0.06 * air_vnoise(t * 7.0 + seed * 13.0);
	float breathe = 0.96 + 0.06 * sin(t * 0.9 + ph);
	float drv = air_vnoise(t * 0.55 + seed * 31.0);
	float gate = 1.0 - smoothstep(0.0, 0.14, drv);
	float stut = step(0.5, fract(t * 17.0 + seed * 5.0));
	float drop = mix(1.0, mix(0.35, 0.85, stut), gate);
	return clamp(buzz * jit * breathe * drop, 0.55, 1.15);
}

// Spawn warm-up: while brightness ramps 0->1 the tube over-brightens and
// shivers, the way one does when it strikes.
float air_warmup(float pb, float t, float seed)
{
	float warm = 1.0 - smoothstep(0.0, 1.0, pb);
	float surge = 1.0 + 0.55 * warm;
	float shiver = 1.0 + 0.20 * warm * sin(t * 95.0 + seed * 9.0);
	return surge * shiver;
}

// Additive-safe vibrance: expand channels away from luma, no white blowout.
vec3 air_vibrance(vec3 c, float amt)
{
	float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
	return clamp(mix(vec3(l), c, 1.0 + amt), 0.0, 2.0);
}

// Signed distance to a rounded box (half-extents he, corner radius rad). <0
// inside. The keystone for the BRACKETS (18) corner frame.
float air_box(vec2 p, vec2 he, float rad)
{
	vec2 q = abs(p) - he + vec2(rad);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rad;
}

vec4 ProcessTexel()
{
	// Which shape. Anything outside the range draws nothing.
	float wgType = floor(uObjectColor2.r * 255.0 + 0.5);
	if (wgType < 13.5 || wgType > 20.5) return vec4(0.0);

	vec3 wgCol = uObjectColor.rgb;

	// The quad's own space, -1..1 on both axes. s is flipped so a stamped
	// number reads forward to the viewer rather than mirrored, exactly as the
	// original did for the digit panel.
	float nx = -(vTexCoord.s * 2.0 - 1.0);
	float ny = vTexCoord.t * 2.0 - 1.0;

	// The number, unpacked the way func_wg13.fp unpacks it. Red is the low
	// byte, alpha the middle, blue the high.
	float pnum = float(int(uAddColor.r * 255.0 + 0.5)
	                 | (int(uAddColor.a * 255.0 + 0.5) << 8)
	                 | (int(uAddColor.b * 255.0 + 0.5) << 16));

	// The animation lane. Called pbright by the shapes that treat it as
	// brightness and panim by the ones that treat it as progress; it is one
	// number and the original used both names for it.
	float pbright = clamp(uAddColor.g, 0.0, 1.0);
	float panim = pbright;

	float pseed = fract(dot(wgCol, vec3(0.37, 0.71, 0.19)) + pnum * 0.013);
	float pflick = air_flicker(timer, pseed) * air_warmup(pbright, timer, pseed);

	vec3 wgAdd = vec3(0.0);

	// ============================================================
	//  BRASS STORM SHAPES. Each computes its own dd (~0 on the glowing
	//  contour, >0 away) then falls into the same neon core/halo the digits
	//  use. 16 = shell casing body + stamped damage number. 17 = bounce shard.
	// ============================================================
	if (wgType > 15.5 && wgType < 16.5)        // ---- CASING (16) ----
	{
		vec2  bp  = vec2(nx, ny);
		vec2  he  = vec2(0.66, 0.30);            // long x, short y: the casing lies across
		vec2  q   = abs(bp) - he + vec2(0.18);
		float body = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - 0.18;  // rounded-box SDF
		float ddBody = abs(body) - 0.05;        // glow the case-wall outline
		float mouth  = max(abs(nx - 0.50) - 0.04, abs(ny) - 0.22);          // open mouth at +x cap
		float ddMouth = abs(mouth) - 0.03;
		float dd = min(ddBody, ddMouth);

		// stamp the damage NUMBER on the body (font atlas, like the digit branch)
		float snum  = pnum;
		float snlen = (snum < 10.0) ? 1.0 : (snum < 100.0) ? 2.0 : (snum < 1000.0) ? 3.0 : 4.0;
		if (abs(nx) < 0.52 && abs(ny) < 0.20)
		{
			float u  = (nx / 0.52 * 0.5 + 0.5) * snlen;
			float di = clamp(floor(u), 0.0, snlen - 1.0);
			float dxx = (u - di) * 2.0 - 1.0;
			float dyy = ny / 0.20;
			float dv  = mod(floor(snum / pow(10.0, snlen - 1.0 - di)), 10.0);
			float gidx = (48.0 + dv) - 32.0;
			float ccol = mod(gidx, 16.0);
			float crow = floor(gidx / 16.0);
			vec2  lUV  = clamp(vec2(dxx, dyy) * 0.62 + 0.5, 0.02, 0.98);
			vec2  aUV  = (vec2(ccol, crow) + lUV) / vec2(16.0, 6.0);
			float ssdf = texture(tex, aUV).r;
			dd = min(dd, 0.5 - ssdf);
		}

		float core = smoothstep(0.03, -0.10, dd);
		float halo = exp(-max(dd, 0.0) * 11.0); halo *= halo;
		vec3  hue  = air_vibrance(wgCol, 0.55);
		vec3  coreColor = ( vec3(2.4) * core + hue * (halo * 2.2 + core * 0.6) ) * pbright * pflick;
		wgAdd += hue * (halo * 0.9) * pbright * pflick;
		if (halo > 0.0035 || core > 0.001)
			return vec4(coreColor, 1.0);
	}
	else if (wgType > 16.5 && wgType < 17.5)   // ---- SHARD (17): bounce clink ----
	{
		float bar1 = max(abs(ny) - 0.06, abs(nx) - 0.92);   // horizontal sliver
		float bar2 = max(abs(nx) - 0.06, abs(ny) - 0.92);   // vertical sliver
		float spark = min(bar1, bar2);
		float ctr   = length(vec2(nx, ny)) - 0.12;          // hot centre dot
		float dd = min(abs(spark) - 0.02, ctr);
		float core = smoothstep(0.03, -0.10, dd);
		float halo = exp(-max(dd, 0.0) * 13.0); halo *= halo;
		vec3  hue  = air_vibrance(wgCol, 0.45);
		vec3  coreColor = ( vec3(2.8) * core + hue * (halo * 2.0 + core * 0.6) ) * pbright * pflick;
		wgAdd += hue * (halo * 0.8) * pbright * pflick;
		if (halo > 0.0035 || core > 0.001)
			return vec4(coreColor, 1.0);
	}
	// ============================================================
	//  WEAPON-SIGNATURE SHAPES. Animation lane = panim. Each computes dd
	//  (~0 on the glowing contour, >0 away) and falls into the same core/halo
	//  the digits use, EXCEPT smoke (20) which is a soft additive haze with no
	//  white-hot core at all.
	//  14 = shockwave ring, 15 = filled disc flash, 20 = smoke puff.
	// ============================================================
	else if (wgType > 13.5 && wgType < 14.5)   // ---- SHOCKWAVE RING (14) ----
	{
		float r   = length(vec2(nx, ny));
		float rad = mix(0.16, 0.96, panim);          // contour marches outward
		float dd  = abs(r - rad) - mix(0.10, 0.02, panim);   // thins as it grows
		float env = 1.0 - smoothstep(0.55, 1.0, panim);      // dissipates near the end
		float core = smoothstep(0.03, -0.06, dd);
		float halo = exp(-max(dd, 0.0) * 12.0); halo *= halo;
		vec3  hue  = air_vibrance(wgCol, 0.55);
		vec3  coreColor = ( vec3(2.4) * core + hue * (halo * 2.0 + core * 0.5) )
		                  * pbright * pflick * env;
		wgAdd += hue * (halo * 0.8) * pbright * pflick * env;
		if ((halo > 0.0035 || core > 0.001) && env > 0.001)
			return vec4(coreColor, 1.0);
	}
	else if (wgType > 14.5 && wgType < 15.5)   // ---- FILLED DISC FLASH (15) ----
	{
		float r   = length(vec2(nx, ny));
		float dd  = r - 0.85;                         // inside the disc -> dd<0 -> white core
		float env = panim * panim;                   // caller fades panim toward 0 for snap-off
		float core = smoothstep(0.10, -0.30, dd);     // big soft interior
		float halo = exp(-max(dd, 0.0) * 8.0); halo *= halo;
		vec3  hue  = air_vibrance(wgCol, 0.45);
		vec3  coreColor = ( vec3(2.8) * core + hue * (halo * 1.6 + core * 0.4) )
		                  * pbright * pflick * env;
		wgAdd += hue * (halo * 0.7) * pbright * pflick * env;
		if ((halo > 0.0035 || core > 0.001) && env > 0.002)
			return vec4(coreColor, 1.0);
	}
	else if (wgType > 19.5 && wgType < 20.5)   // ---- SMOKE PUFF (20) ----
	{
		vec2  q = vec2(nx, ny);
		// two cheap 1-D value-noise lumps break the circle into smoke
		float n1 = air_vnoise(q.x * 3.1 + timer * 0.6 + pseed * 7.0);
		float n2 = air_vnoise(q.y * 2.7 - timer * 0.5 + pseed * 3.0);
		float lump = (n1 + n2) * 0.25;               // ~0..0.5
		float r    = length(q) * (1.0 + 0.35 * (lump - 0.25));
		float edge = 0.55 + 0.25 * panim;            // billows outward with t
		float dd   = r - edge;
		float cloud = 1.0 - smoothstep(-0.25, 0.20, dd);   // fuzzy fill
		vec3  hue   = wgCol;                          // already desaturated grey from ZScript
		// soft additive haze ONLY -- deliberately no white-hot early-return.
		wgAdd += hue * (cloud * 0.45 * panim) * pbright * pflick;
		// (panim here = brightness; ZScript fades it to 0 as the puff dies.)
	}
	// ============================================================
	//  NEON DISPLAY SHAPES. Both compute their own dd then fall into the same
	//  canonical neon core/halo the digits use.
	//  18 = corner BRACKETS, 19 = WAVEFORM/oscilloscope.
	//  Brightness/fade lane = pbright. pnum carries the per-shape data seed.
	// ============================================================
	else if (wgType > 17.5 && wgType < 18.5)   // ---- CORNER BRACKETS (18) ----
	{
		// Target-reticle frame: four L-shaped corner brackets. A bracket = the
		// outline band of a rounded box, KEPT only near the four corners (the
		// long mid-runs are masked out so it reads as [   ] corners, not a box).
		vec2  bp  = vec2(nx, ny);
		vec2  he  = vec2(0.78, 0.62);                 // frame half-extents
		float frame = abs(air_box(bp, he, 0.05)) - 0.035;   // hollow outline band
		// keep only the corner runs: a point is "corner" if it is near BOTH
		// the x-edge and y-edge bands (within arm of a corner along each axis).
		float arm = 0.30;
		float cornerX = he.x - abs(nx);              // distance inside from the x edge
		float cornerY = he.y - abs(ny);              // distance inside from the y edge
		float keep = max(cornerX, cornerY);          // >arm in the long mid-runs
		// mask the band OUT in the mid-runs by pushing dd positive there
		float dd = (keep < arm) ? frame : (frame + 0.40);
		// small hot tick at dead-center (aiming pip)
		float pip = length(bp) - 0.05;
		dd = min(dd, abs(pip) - 0.02);

		float core = smoothstep(0.03, -0.10, dd);
		float halo = exp(-max(dd, 0.0) * 11.0); halo *= halo;
		vec3  hue  = air_vibrance(wgCol, 0.55);
		vec3  coreColor = ( vec3(2.6) * core + hue * (halo * 2.2 + core * 0.6) ) * pbright * pflick;
		wgAdd += hue * (halo * 0.9) * pbright * pflick;
		if (halo > 0.0035 || core > 0.001)
			return vec4(coreColor, 1.0);
	}
	else if (wgType > 18.5 && wgType < 19.5)   // ---- WAVEFORM / OSCILLOSCOPE (19) ----
	{
		// A horizontal trace y = f(nx) drawn as a thin glowing tube. The wave is
		// a sum of value-noise lumps + a sine, scrolled by timer and seeded by
		// pnum (amplitude seed) so different panels read different traces.
		float amp  = 0.30 + 0.45 * fract(pnum * 0.013 + pseed);   // 0.30..0.75 trace height
		float ph   = pseed * 6.2831853;
		// composite waveform (cheap, no loops)
		float w  = sin(nx * 6.0 + timer * 3.0 + ph) * 0.55;
		w += sin(nx * 13.0 - timer * 2.0 + ph * 1.7) * 0.30;
		w += (air_vnoise(nx * 5.0 + timer * 1.5 + pseed * 9.0) - 0.5) * 0.55;
		float wy = clamp(w * amp, -0.92, 0.92);                  // target y at this x
		// distance from this pixel to the trace (vertical band -> tube)
		float dTrace = abs(ny - wy) - 0.025;
		// faint center baseline (zero line) so it reads as a scope
		float dBase  = abs(ny) - 0.006;
		float dd = min(dTrace, dBase + 0.30);     // baseline dimmer (pushed out a touch)
		dd = min(dd, dTrace);

		float core = smoothstep(0.03, -0.10, dd);
		float halo = exp(-max(dd, 0.0) * 12.0); halo *= halo;
		vec3  hue  = air_vibrance(wgCol, 0.55);
		vec3  coreColor = ( vec3(2.6) * core + hue * (halo * 2.2 + core * 0.6) ) * pbright * pflick;
		wgAdd += hue * (halo * 0.9) * pbright * pflick;
		if (halo > 0.0035 || core > 0.001)
			return vec4(coreColor, 1.0);
	}

	// The spill. In the original this went onto the surface behind the effect,
	// on exactly the fragments where the core test above failed -- the early
	// return meant no fragment ever took both. Same either/or here.
	return vec4(wgAdd, 1.0);
}
