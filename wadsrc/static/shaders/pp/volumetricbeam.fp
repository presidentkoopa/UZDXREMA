
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D DepthTexture;

// ============================================================================
// [BB] Volumetric flashlight beam.
//
// Everything else in this engine lights SURFACES. This lights the air: the
// cone is drawn where nothing is, so you see the beam itself rather than only
// the disc it lands on. That is the whole point of it, and it is why this has
// to be a postprocess pass -- there is no geometry to hang it on.
//
// Working in VIEW space rather than world space is deliberate. The ray for a
// pixel is trivial there (it starts at the origin), and each eye resolves its
// own view matrix on the CPU, so stereo and portals come out right without
// this shader knowing either exists.
//
// The march is bounded by an analytic ray/cone intersection first. Without
// that, every pixel on screen would march the full ray even when the cone
// covers a tenth of the view -- the difference between a beam you can afford
// and one you cannot.
// ============================================================================

// ---------------------------------------------------------------------------
// Value noise, 3D. Cheap enough to afford once per march step.
// ---------------------------------------------------------------------------

float hash13(vec3 p)
{
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float valueNoise(vec3 p)
{
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);   // smoothstep, so cells blend instead of blocking

	return mix(
		mix(mix(hash13(i + vec3(0,0,0)), hash13(i + vec3(1,0,0)), f.x),
		    mix(hash13(i + vec3(0,1,0)), hash13(i + vec3(1,1,0)), f.x), f.y),
		mix(mix(hash13(i + vec3(0,0,1)), hash13(i + vec3(1,0,1)), f.x),
		    mix(hash13(i + vec3(0,1,1)), hash13(i + vec3(1,1,1)), f.x), f.y), f.z);
}

// Two octaves. One reads as smooth blobs; two gives the finer grain that
// makes it look like motes rather than fog.
float dustNoise(vec3 p)
{
	return valueNoise(p) * 0.65 + valueNoise(p * 2.7) * 0.35;
}

void main()
{
	// Scene depth for this pixel: how far along the ray the world is. The
	// beam must stop there, or it would shine through walls.
	float sceneDepth = texture(DepthTexture, TexCoord).x;

	// View-space ray for this pixel. Origin is the eye, at (0,0,0).
	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 rayDir = normalize(vec3(ndc * TanHalfFov, -1.0));

	// --- analytic ray/cone intersection --------------------------------
	// Bounds the march to the segment that can possibly be lit. Solves the
	// standard infinite-cone quadratic, then clamps to the cone's actual
	// length and to the scene depth.
	vec3 co = -BeamPos;                  // eye relative to the cone apex
	float cosT = CosOuter;
	float cos2 = cosT * cosT;

	float dv = dot(rayDir, BeamDir);
	float cv = dot(co, BeamDir);

	float a = dv * dv - cos2;
	float b = 2.0 * (dv * cv - dot(rayDir, co) * cos2);
	float c = cv * cv - dot(co, co) * cos2;

	float tMin = 0.0;
	float tMax = 0.0;

	if (abs(a) < 1e-6)
	{
		// Ray parallel to the cone surface: one root, or none worth having.
		if (abs(b) < 1e-6) { FragColor = vec4(0.0); return; }
		float t = -c / b;
		tMin = max(t, 0.0);
		tMax = BeamLength;
	}
	else
	{
		float disc = b * b - 4.0 * a * c;
		if (disc < 0.0) { FragColor = vec4(0.0); return; }
		float sq = sqrt(disc);
		float t0 = (-b - sq) / (2.0 * a);
		float t1 = (-b + sq) / (2.0 * a);
		tMin = min(t0, t1);
		tMax = max(t0, t1);
	}

	// The quadratic also solves the mirror cone behind the apex. Reject any
	// stretch of the segment that is on the wrong side.
	float midCheck = dot(BeamPos + rayDir * max(tMin, 0.0) * 0.0, BeamDir);

	tMin = max(tMin, 0.0);
	tMax = min(tMax, min(BeamLength, sceneDepth));
	if (tMax <= tMin) { FragColor = vec4(0.0); return; }

	// --- march ----------------------------------------------------------
	// Jittered start, so banding across the cone turns into fine noise the
	// eye reads as haze rather than as visible steps. Interleaved gradient
	// noise: one cheap expression, no texture lookup.
	float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

	int steps = StepCount;
	float dt = (tMax - tMin) / float(steps);
	float t = tMin + dt * jitter;

	float accum = 0.0;
	for (int i = 0; i < 64; i++)
	{
		if (i >= steps) break;

		vec3 p = rayDir * t;
		vec3 toP = p - BeamPos;
		float dist = length(toP);

		if (dist > 0.0001)
		{
			float ct = dot(toP / dist, BeamDir);

			// Inside the cone at all?
			if (ct > cosT)
			{
				// Radial: full brightness inside the inner angle, easing out
				// to nothing at the outer. This is the soft edge of the beam.
				float radial = smoothstep(cosT, CosInner, ct);

				// Axial: fades along the length so the beam dies out instead
				// of ending. Falloff shapes the curve -- 1 linear, higher
				// concentrates the light near the lens.
				float axial = 1.0 - clamp(dist / BeamLength, 0.0, 1.0);
				axial = pow(axial, Falloff);

				float contrib = radial * axial;

				// Dust. Sampled in WORLD space, not beam space, and that is
				// the whole trick: dust hangs in the room, it does not travel
				// with the torch. Sample it relative to the beam and the
				// motes slide along with the cone as you sweep, which reads
				// instantly as fake. World space means sweeping the beam
				// reveals different dust, the way it should.
				if (DustAmount > 0.0)
				{
					vec3 worldP = (ViewToWorld * vec4(p, 1.0)).xyz;
					worldP.y -= DustTime * DustDrift;   // slow settle
					float d = dustNoise(worldP * DustScale);
					// Never fully dark: dust thickens the beam in places, it
					// does not punch holes through it.
					contrib *= mix(1.0, d, clamp(DustAmount, 0.0, 1.0));
				}

				accum += contrib;
			}
		}

		t += dt;
	}

	// Normalise by step count so density means the same thing regardless of
	// quality setting -- turning quality down must not turn the beam down.
	accum *= Density / float(steps);
	accum *= (tMax - tMin);

	FragColor = vec4(BeamColor * accum, 1.0);
}
