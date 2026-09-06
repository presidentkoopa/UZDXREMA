
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D DepthTexture;

// TWO SINGLE-CHANNEL TEXTURES RATHER THAN ONE WITH TWO CHANNELS, because the
// CPU side already holds two plain float arrays and R32f takes them verbatim.
// Packing intensity and height into Rg16f would mean writing half-floats by
// hand for a saving of one sampler, and hand-rolled float16 is exactly the
// sort of code that is wrong in a way nobody sees for a month.
layout(binding=1) uniform sampler2D HeatTexture;
layout(binding=2) uniform sampler2D HeatHeightTexture;

// ============================================================================
// [BB] THE HEATMAP -- where the fighting happened, painted on the floor.
//
// A postprocess pass rather than a term in the scene shader, and that is a
// deliberate trade. A scene-shader sampler would let this tint the LIGHT and
// be occluded correctly by translucent geometry, at the price of four
// coordinated edits in the Vulkan backend -- a GLSL binding, a descriptor set
// layout, a descriptor pool size, and a per-frame descriptor write -- where
// missing the pool size fails silently in review. A postprocess pass touches
// no backend file at all and is backend-agnostic by construction.
//
// What it costs: this composites after fog and tonemapping, so the heat is
// painted over the frame instead of tinting the light in it, and the pass
// cannot see surface normals unless SSAO happens to be on. For a mark on the
// ground read at world XZ, neither matters.
//
// RECONSTRUCTING WHERE A PIXEL IS. The depth buffer gives distance along the
// ray, the projection gives the ray, and ViewToWorld gives the rest -- exactly
// the route the volumetric beam already takes. The one trap is that the depth
// SAMPLE is a nonlinear 0..1 value and not a distance; treating it as one is
// what made the beam pass draw nothing at all for its entire existence.
// ============================================================================

void main()
{
	if (HeatScale <= 0.0) { FragColor = vec4(0.0); return; }

	float rawDepth = texture(DepthTexture, TexCoord).x;

	// Sky and void sit at the far plane and have no floor to mark. Rejecting
	// them explicitly rather than letting the reconstruction run is not just an
	// optimisation: an unreachable world position still lands SOMEWHERE on the
	// heatmap grid, and the sky would come back stained.
	if (rawDepth >= 0.999999) { FragColor = vec4(0.0); return; }

	float linearZ = 1.0 / (clamp(rawDepth, 0.0, 1.0) * LinearizeDepthA + LinearizeDepthB);

	vec2 ndc = TexCoord * 2.0 - 1.0;
	vec3 rayDir = vec3(ndc * TanHalfFov, -1.0);

	// rayDir.z is -1 here, so scaling by linearZ puts the point at the right
	// distance ALONG THE VIEW AXIS -- which is what the depth buffer measures.
	// Normalising first and multiplying would place it at that distance along
	// the ray instead, which is too far everywhere except the screen centre.
	vec3 viewPos = rayDir * linearZ;
	vec3 worldPos = (ViewToWorld * vec4(viewPos, 1.0)).xyz;

	// The grid covers the map's bounding box. Outside it there is nothing.
	vec2 uv = (worldPos.xz - HeatOrigin) * HeatInvSize;
	if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
	{
		FragColor = vec4(0.0);
		return;
	}

	// Bilinear, so the grid does not read as tiles.
	float heat = texture(HeatTexture, uv).r;
	if (heat <= 0.0) { FragColor = vec4(0.0); return; }
	float heatY = texture(HeatHeightTexture, uv).r;

	// THE GRID IS FLAT AND THE WORLD IS NOT. Without this a kill on a balcony
	// would also mark the ground beneath it, and every wall standing over a
	// hot cell would be painted up its whole height -- which is the thing that
	// would instantly give away that this is a screen effect.
	float dz = abs(worldPos.y - heatY);
	float onFloor = 1.0 - smoothstep(HeatTolerance * 0.5, HeatTolerance, dz);
	if (onFloor <= 0.0) { FragColor = vec4(0.0); return; }

	// Intensity to colour. sqrt rather than linear, because a heatmap is read
	// by comparing regions and a linear ramp spends most of its range on the
	// difference between "nothing happened" and "one thing happened".
	float t = clamp(sqrt(heat / HeatCeiling), 0.0, 1.0);
	vec3 col = mix(HeatColorLow, HeatColorHigh, t);

	// Faded by the same curve, so a cell with one death is dim as well as
	// cold. Colour alone would make a single kill as loud as a massacre.
	FragColor = vec4(col * t * onFloor * HeatScale, 1.0);
}
