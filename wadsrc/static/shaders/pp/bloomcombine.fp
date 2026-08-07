
layout(location=0) in vec2 TexCoord;
layout(location=0) out vec4 FragColor;

layout(binding=0) uniform sampler2D Bloom;

// [BB] Tint and chromatic fringing.
//
// Fringing is offset RADIALLY, outward from screen centre, because that is
// what a real lens does -- colour separation grows toward the edges and is
// nil dead centre. Offsetting uniformly would just look like a broken image.
//
// This shader also runs as the downscale step between bloom levels, where the
// caller passes a neutral tint and no fringing -- otherwise both would be
// applied once per level and compound.
void main()
{
	vec3 b;
	if (Chromatic > 0.0001)
	{
		vec2 dir = TexCoord - vec2(0.5);
		b.r = texture(Bloom, TexCoord + dir * Chromatic).r;
		b.g = texture(Bloom, TexCoord).g;
		b.b = texture(Bloom, TexCoord - dir * Chromatic).b;
	}
	else
	{
		b = texture(Bloom, TexCoord).rgb;
	}

	FragColor = vec4(b * Tint, 0.0);
}
