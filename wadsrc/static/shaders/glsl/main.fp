

layout(location = 0) in vec4 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 pixelpos;
layout(location = 3) in vec3 glowdist;
layout(location = 4) in vec3 gradientdist;
layout(location = 5) in vec4 vWorldNormal;
layout(location = 6) in vec4 vEyeNormal;
layout(location = 9) in vec3 vLightmap;

#ifdef NO_CLIPDISTANCE_SUPPORT
layout(location = 7) in vec4 ClipDistanceA;
layout(location = 8) in vec4 ClipDistanceB;
#endif

layout(location=0) out vec4 FragColor;
#ifdef GBUFFER_PASS
layout(location=1) out vec4 FragFog;
layout(location=2) out vec4 FragNormal;
#endif

struct Material
{
	vec4 Base;
	vec4 Bright;
	vec4 Glow;
	vec3 Normal;
	vec3 Specular;
	float Glossiness;
	float SpecularLevel;
	float Metallic;
	float Roughness;
	float AO;
};

vec4 Process(vec4 color);
vec4 ProcessTexel();
Material ProcessMaterial(); // note that this is deprecated. Use SetupMaterial!
void SetupMaterial(inout Material mat);
vec4 ProcessLight(Material mat, vec4 color);
vec3 ProcessMaterialLight(Material material, vec3 color);
vec2 GetTexCoord();

// These get Or'ed into uTextureMode because it only uses its 3 lowermost bits.
const int TEXF_Brightmap = 0x10000;
const int TEXF_Detailmap = 0x20000;
const int TEXF_Glowmap = 0x40000;
const int TEXF_ClampY = 0x80000;
const int TEXF_FlipNormal = 0x100000;

//===========================================================================
//
// RGB to HSV
//
//===========================================================================

vec3 rgb2hsv(vec3 c)
{
	vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
	vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
	vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));

	float d = q.x - min(q.w, q.y);
	float e = 1.0e-10;
	return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

//===========================================================================
//
// Color to grayscale
//
//===========================================================================

float grayscale(vec4 color)
{
	return dot(color.rgb, vec3(0.3, 0.56, 0.14));
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 dodesaturate(vec4 texel, float factor)
{
#ifdef SHADER_LITE
	return texel;
#else
	if (factor != 0.0)
	{
		float gray = grayscale(texel);
		return mix (texel, vec4(gray,gray,gray,texel.a), factor);
	}
	else
	{
		return texel;
	}
#endif
}

//===========================================================================
//
// Desaturate a color
//
//===========================================================================

vec4 desaturate(vec4 texel)
{
	return dodesaturate(texel, uDesaturationFactor);
}

//===========================================================================
//
// Texture tinting code originally from JFDuke but with a few more options
//
//===========================================================================

const int Tex_Blend_Alpha = 1;
const int Tex_Blend_Screen = 2;
const int Tex_Blend_Overlay = 3;
const int Tex_Blend_Hardlight = 4;

 vec4 ApplyTextureManipulation(vec4 texel, int blendflags)
 {
	// Step 1: desaturate according to the material's desaturation factor. 
	texel = dodesaturate(texel, uTextureModulateColor.a);

	// Step 2: Invert if requested
	if ((blendflags & 8) != 0)
	{
		texel.rgb = vec3(1.0 - texel.r, 1.0 - texel.g, 1.0 - texel.b);
	}

	// Step 3: Apply additive color
	texel.rgb += uTextureAddColor.rgb;

	// Step 4: Colorization, including gradient if set.
	texel.rgb *= uTextureModulateColor.rgb;

	// Before applying the blend the value needs to be clamped to [0..1] range.
	texel.rgb = clamp(texel.rgb, 0.0, 1.0);

	// Step 5: Apply a blend. This may just be a translucent overlay or one of the blend modes present in current Build engines.
	if ((blendflags & 7) != 0)
	{
		vec3 tcol = texel.rgb * 255.0;	// * 255.0 to make it easier to reuse the integer math.
		vec4 tint = uTextureBlendColor * 255.0;

		switch (blendflags & 7)
		{
			default:
				tcol.b = tcol.b * (1.0 - uTextureBlendColor.a) + tint.b * uTextureBlendColor.a;
				tcol.g = tcol.g * (1.0 - uTextureBlendColor.a) + tint.g * uTextureBlendColor.a;
				tcol.r = tcol.r * (1.0 - uTextureBlendColor.a) + tint.r * uTextureBlendColor.a;
				break;
			// The following 3 are taken 1:1 from the Build engine
			case Tex_Blend_Screen:
				tcol.b = 255.0 - (((255.0 - tcol.b) * (255.0 - tint.r)) / 256.0);
				tcol.g = 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 256.0);
				tcol.r = 255.0 - (((255.0 - tcol.r) * (255.0 - tint.b)) / 256.0);
				break;
			case Tex_Blend_Overlay:
				tcol.b = tcol.b < 128.0? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tcol.g < 128.0? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tcol.r < 128.0? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
			case Tex_Blend_Hardlight:
				tcol.b = tint.b < 128.0 ? (tcol.b * tint.b) / 128.0 : 255.0 - (((255.0 - tcol.b) * (255.0 - tint.b)) / 128.0);
				tcol.g = tint.g < 128.0 ? (tcol.g * tint.g) / 128.0 : 255.0 - (((255.0 - tcol.g) * (255.0 - tint.g)) / 128.0);
				tcol.r = tint.r < 128.0 ? (tcol.r * tint.r) / 128.0 : 255.0 - (((255.0 - tcol.r) * (255.0 - tint.r)) / 128.0);
				break;
		}
		texel.rgb = tcol / 255.0;
	}
	return texel;
}

//===========================================================================
//
// This function is common for all (non-special-effect) fragment shaders
//
//===========================================================================

vec4 getTexel(vec2 st)
{
	vec4 texel = texture(tex, st);

	//
	// Apply texture modes
	//
	switch (uTextureMode & 0xffff)
	{
		case 1:	// TM_STENCIL
			texel.rgb = vec3(1.0,1.0,1.0);
			break;

		case 2:	// TM_OPAQUE
			texel.a = 1.0;
			break;

		case 3:	// TM_INVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, texel.a);
			break;

		case 4:	// TM_ALPHATEXTURE
		{
			float gray = grayscale(texel);
			texel = vec4(1.0, 1.0, 1.0, gray*texel.a);
			break;
		}

		case 5:	// TM_CLAMPY
			if (st.t < 0.0 || st.t > 1.0)
			{
				texel.a = 0.0;
			}
			break;

		case 6: // TM_OPAQUEINVERSE
			texel = vec4(1.0-texel.r, 1.0-texel.b, 1.0-texel.g, 1.0);
			break;

		case 7: //TM_FOGLAYER 
			return texel;

	}
#ifndef SHADER_LITE
	if ((uTextureMode & TEXF_ClampY) != 0)
	{
		if (st.t < 0.0 || st.t > 1.0)
		{
			texel.a = 0.0;
		}
	}

	// Apply the texture modification colors.
	int blendflags = int(uTextureAddColor.a);	// this alpha is unused otherwise
	if (blendflags != 0)	
	{
		// only apply the texture manipulation if it contains something.
		texel = ApplyTextureManipulation(texel, blendflags);
	}

	// Apply the Doom64 style material colors on top of everything from the texture modification settings.
	// This may be a bit redundant in terms of features but the data comes from different sources so this is unavoidable.
	texel.rgb += uAddColor.rgb;
	if (uObjectColor2.a == 0.0) texel *= uObjectColor;
	else texel *= mix(uObjectColor, uObjectColor2, gradientdist.z);
#else
	texel *= uObjectColor;
#endif
	// Last but not least apply the desaturation from the sector's light.
	return desaturate(texel);
}

//===========================================================================
//
// Vanilla Doom wall colormap equation
//
//===========================================================================
float R_WallColormap(float lightnum, float z, vec3 normal)
{
	// R_ScaleFromGlobalAngle calculation
	float projection = 160.0; // projection depends on SCREENBLOCKS!! 160 is the fullscreen value
	vec2 line_v1 = pixelpos.xz; // in vanilla this is the first curline vertex
	vec2 line_normal = normal.xz;
	float texscale = projection * clamp(dot(normalize(uCameraPos.xz - line_v1), line_normal), 0.0, 1.0) / z;

	float lightz = clamp(16.0 * texscale, 0.0, 47.0);

	// scalelight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	return startmap - lightz * 0.5;
}

//===========================================================================
//
// Vanilla Doom plane colormap equation
//
//===========================================================================
float R_PlaneColormap(float lightnum, float z)
{
	float lightz = clamp(z / 16.0f, 0.0, 127.0);

	// zlight[lightnum][lightz] lookup
	float startmap = (15.0 - lightnum) * 4.0;
	float scale = 160.0 / (lightz + 1.0);
	return startmap - scale * 0.5;
}

//===========================================================================
//
// zdoom colormap equation
//
//===========================================================================
float R_ZDoomColormap(float light, float z)
{
	float L = light * 255.0;
	float vis = min(uGlobVis / z, 24.0 / 32.0);
	float shade = 2.0 - (L + 12.0) / 128.0;
	float lightscale = shade - vis;
	return lightscale * 31.0;
}

float R_DoomColormap(float light, float z)
{
#ifdef SHADER_LITE
	return R_ZDoomColormap(light, z);
#else
	if ((uPalLightLevels >> 16) == 16) // gl_lightmode 16
	{
		float lightnum = clamp(light * 15.0, 0.0, 15.0);

		if (dot(vWorldNormal.xyz, vWorldNormal.xyz) > 0.5)
		{
			vec3 normal = normalize(vWorldNormal.xyz);
			return mix(R_WallColormap(lightnum, z, normal), R_PlaneColormap(lightnum, z), abs(normal.y));
		}
		else // vWorldNormal is not set on sprites
		{
			return R_PlaneColormap(lightnum, z);
		}
	}
	else
	{
		return R_ZDoomColormap(light, z);
	}
#endif	
}

//===========================================================================
//
// Doom software lighting equation
//
//===========================================================================
float R_DoomLightingEquation(float light)
{
	// z is the depth in view space, positive going into the screen
	float z;
	if (((uPalLightLevels >> 8)  & 0xff) == 2)
	{
		z = distance(pixelpos.xyz, uCameraPos.xyz);
	}
	else 
	{
		z = pixelpos.w;
	}
#ifndef SHADER_LITE
	if ((uPalLightLevels >> 16) == 5) // gl_lightmode 5: Build software lighting emulation.
	{
		// This is a lot more primitive than Doom's lighting...
		float numShades = float(uPalLightLevels & 255);
		float curshade = (1.0 - light) * (numShades - 1.0);
		float visibility = max(uGlobVis * uLightFactor * z, 0.0);
		float shade = clamp((curshade + visibility), 0.0, numShades - 1.0);
		return clamp(shade * uLightDist, 0.0, 1.0);
	}
#endif
	float colormap = R_DoomColormap(light, z);

	if ((uPalLightLevels & 0xff) != 0)
		colormap = floor(colormap) + 0.5;

	// Result is the normalized colormap index (0 bright .. 1 dark)
	return clamp(colormap, 0.0, 31.0) / 32.0;
}

//===========================================================================
//
// Check if light is in shadow
//
//===========================================================================

#ifdef SUPPORTS_RAYTRACING

bool traceHit(vec3 origin, vec3 direction, float dist)
{
	rayQueryEXT rayQuery;
	rayQueryInitializeEXT(rayQuery, TopLevelAS, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.01f, direction, dist);
	while(rayQueryProceedEXT(rayQuery)) { }
	return rayQueryGetIntersectionTypeEXT(rayQuery, true) != gl_RayQueryCommittedIntersectionNoneEXT;
}

vec2 softshadow[9 * 3] = vec2[](
	vec2( 0.0, 0.0),
	vec2(-2.0,-2.0),
	vec2( 2.0, 2.0),
	vec2( 2.0,-2.0),
	vec2(-2.0, 2.0),
	vec2(-1.0,-1.0),
	vec2( 1.0, 1.0),
	vec2( 1.0,-1.0),
	vec2(-1.0, 1.0),

	vec2( 0.0, 0.0),
	vec2(-1.5,-1.5),
	vec2( 1.5, 1.5),
	vec2( 1.5,-1.5),
	vec2(-1.5, 1.5),
	vec2(-0.5,-0.5),
	vec2( 0.5, 0.5),
	vec2( 0.5,-0.5),
	vec2(-0.5, 0.5),

	vec2( 0.0, 0.0),
	vec2(-1.25,-1.75),
	vec2( 1.75, 1.25),
	vec2( 1.25,-1.75),
	vec2(-1.75, 1.75),
	vec2(-0.75,-0.25),
	vec2( 0.25, 0.75),
	vec2( 0.75,-0.25),
	vec2(-0.25, 0.75)
);

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	if (shadowIndex >= 1024.0)
		return 1.0; // Don't cast rays for this light

	vec3 origin = pixelpos.xzy;
	vec3 target = lightpos.xzy + 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	vec3 direction = normalize(target - origin);
	float dist = distance(origin, target);

	if (uShadowmapFilter <= 0)
	{
		return traceHit(origin, direction, dist) ? 0.0 : 1.0;
	}
	else
	{
		vec3 v = (abs(direction.x) > abs(direction.y)) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		vec3 xdir = normalize(cross(direction, v));
		vec3 ydir = cross(direction, xdir);

		float sum = 0.0;
		int step_count = uShadowmapFilter * 9;
		for (int i = 0; i <= step_count; i++)
		{
			vec3 pos = target + xdir * softshadow[i].x + ydir * softshadow[i].y;
			sum += traceHit(origin, normalize(pos - origin), dist) ? 0.0 : 1.0;
		}
		return sum / step_count;
	}
}

#else
#ifdef SUPPORTS_SHADOWMAPS

float shadowDirToU(vec2 dir)
{
	if (abs(dir.y) > abs(dir.x))
	{
		float x = dir.x / dir.y * 0.125;
		if (dir.y >= 0.0)
			return 0.125 + x;
		else
			return (0.50 + 0.125) + x;
	}
	else
	{
		float y = dir.y / dir.x * 0.125;
		if (dir.x >= 0.0)
			return (0.25 + 0.125) - y;
		else
			return (0.75 + 0.125) - y;
	}
}

vec2 shadowUToDir(float u)
{
	u *= 4.0;
	vec2 raydir;
	switch (int(u))
	{
	case 0: raydir = vec2(u * 2.0 - 1.0, 1.0); break;
	case 1: raydir = vec2(1.0, 1.0 - (u - 1.0) * 2.0); break;
	case 2: raydir = vec2(1.0 - (u - 2.0) * 2.0, -1.0); break;
	case 3: raydir = vec2(-1.0, (u - 3.0) * 2.0 - 1.0); break;
	}
	return raydir;
}

float sampleShadowmap(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	ivec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x) * 0.25;

	// Snap to shadow map texel grid
	if (abs(ray.z) > abs(ray.x))
	{
		ray.y = ray.y / abs(ray.z);
		ray.x = ray.x / abs(ray.z);
		ray.x = (floor((ray.x + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.z = sign(ray.z);
	}
	else
	{
		ray.y = ray.y / abs(ray.x);
		ray.z = ray.z / abs(ray.x);
		ray.z = (floor((ray.z + 1.0) * 0.5 * scale) + 0.5) / scale * 2.0 - 1.0;
		ray.x = sign(ray.x);
	}

	float t = negD / dot(vWorldNormal.xyz, ray) - bias;
	vec2 dir = ray.xz * t;

	float u = shadowDirToU(dir);
	float dist2 = dot(dir, dir);
	return step(dist2, texture(ShadowMap, vec2(u, v)).x);
}

float sampleShadowmapPCF(vec3 planePoint, float v)
{
	float bias = 1.0;
	float negD = dot(vWorldNormal.xyz, planePoint);

	vec3 ray = planePoint;

	if (abs(ray.z) > abs(ray.x))
		ray.y = ray.y / abs(ray.z);
	else
		ray.y = ray.y / abs(ray.x);

	ivec2 isize = textureSize(ShadowMap, 0);
	float scale = float(isize.x);
	float texelPos = floor(shadowDirToU(ray.xz) * scale);

	float sum = 0.0;
	float step_count = float(uShadowmapFilter);

	texelPos -= step_count + 0.5;
	for (float x = -step_count; x <= step_count; x++)
	{
		float u = fract(texelPos / scale);
		vec2 dir = shadowUToDir(u);

		ray.x = dir.x;
		ray.z = dir.y;
		float t = negD / dot(vWorldNormal.xyz, ray) - bias;
		dir = ray.xz * t;

		float dist2 = dot(dir, dir);
		sum += step(dist2, texture(ShadowMap, vec2(u, v)).x);
		texelPos++;
	}
	return sum / (float(uShadowmapFilter) * 2.0 + 1.0);
}

float shadowmapAttenuation(vec4 lightpos, float shadowIndex)
{
	if (shadowIndex >= 1024.0)
		return 1.0; // No shadowmap available for this light

	vec3 planePoint = pixelpos.xyz - lightpos.xyz;
	planePoint += 0.01; // nudge light position slightly as Doom maps tend to have their lights perfectly aligned with planes

	if (dot(planePoint.xz, planePoint.xz) < 1.0)
		return 1.0; // Light is too close

	float v = (shadowIndex + 0.5) / 1024.0;

	if (uShadowmapFilter <= 0)
	{
		return sampleShadowmap(planePoint, v);
	}
	else
	{
		return sampleShadowmapPCF(planePoint, v);
	}
}

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	float shadowIndex = abs(lightcolorA) - 1.0;
	return shadowmapAttenuation(lightpos, shadowIndex);
}

#else

float shadowAttenuation(vec4 lightpos, float lightcolorA)
{
	return 1.0;
}

#endif
#endif

float spotLightAttenuation(vec4 lightpos, vec3 spotdir, float lightCosInnerAngle, float lightCosOuterAngle)
{
	vec3 lightDirection = normalize(lightpos.xyz - pixelpos.xyz);
	float cosDir = dot(lightDirection, spotdir);
	return smoothstep(lightCosOuterAngle, lightCosInnerAngle, cosDir);
}

//===========================================================================
//
// Adjust normal vector according to the normal map
//
//===========================================================================

#if defined(NORMALMAP)
mat3 cotangent_frame(vec3 n, vec3 p, vec2 uv)
{
	// get edge vectors of the pixel triangle
	vec3 dp1 = dFdx(p);
	vec3 dp2 = dFdy(p);
	vec2 duv1 = dFdx(uv);
	vec2 duv2 = dFdy(uv);

	// solve the linear system
	vec3 dp2perp = cross(n, dp2); // cross(dp2, n);
	vec3 dp1perp = cross(dp1, n); // cross(n, dp1);
	vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;

	// construct a scale-invariant frame
	float invmax = inversesqrt(max(dot(t,t), dot(b,b)));
	return mat3(t * invmax, b * invmax, n);
}

vec3 ApplyNormalMap(vec2 texcoord)
{
	#define WITH_NORMALMAP_UNSIGNED
	#define WITH_NORMALMAP_GREEN_UP
	//#define WITH_NORMALMAP_2CHANNEL

	vec3 interpolatedNormal = normalize(vWorldNormal.xyz);

	vec3 map = texture(normaltexture, texcoord).xyz;
	#if defined(WITH_NORMALMAP_UNSIGNED)
	map = map * 255./127. - 128./127.; // Math so "odd" because 0.5 cannot be precisely described in an unsigned format
	#endif
	#if defined(WITH_NORMALMAP_2CHANNEL)
	map.z = sqrt(1 - dot(map.xy, map.xy));
	#endif
	#if defined(WITH_NORMALMAP_GREEN_UP)
	map.y = -map.y;
	#endif

	mat3 tbn = cotangent_frame(interpolatedNormal, pixelpos.xyz, vTexCoord.st);
	vec3 bumpedNormal = normalize(tbn * map);
	if ((uTextureMode & TEXF_FlipNormal) != 0)
	{
		bumpedNormal = -bumpedNormal;
	}
	return bumpedNormal;
}
#else
vec3 ApplyNormalMap(vec2 texcoord)
{
	vec3 normal = normalize(vWorldNormal.xyz);
	if ((uTextureMode & TEXF_FlipNormal) != 0)
	{
		normal = -normal;
	}
	return normal;
}
#endif

//===========================================================================
//
// Sets the common material properties.
//
//===========================================================================

void SetMaterialProps(inout Material material, vec2 texCoord)
{
#ifdef NPOT_EMULATION
	if (uNpotEmulation.y != 0.0)
	{
		float period = floor(texCoord.t / uNpotEmulation.y);
		texCoord.s += uNpotEmulation.x * floor(mod(texCoord.t, uNpotEmulation.y));
		texCoord.t = period + mod(texCoord.t, uNpotEmulation.y);
	}
#endif	
	material.Base = getTexel(texCoord.st); 
	material.Normal = ApplyNormalMap(texCoord.st);

// OpenGL doesn't care, but Vulkan pukes all over the place if these texture samplings are included in no-texture shaders, even though never called.
#ifndef NO_LAYERS
	if ((uTextureMode & TEXF_Brightmap) != 0)
		material.Bright = desaturate(texture(brighttexture, texCoord.st));

	if ((uTextureMode & TEXF_Detailmap) != 0)
	{
		vec4 Detail = texture(detailtexture, texCoord.st * uDetailParms.xy) * uDetailParms.z;
		material.Base.rgb *= Detail.rgb;
	}

	if ((uTextureMode & TEXF_Glowmap) != 0)
		material.Glow = desaturate(texture(glowtexture, texCoord.st));
#endif
}

float SweepBandAttenAt(int sb)
{
	vec4 sband = uSweepBands[sb];
	if (sband.w <= 0.0) return 0.0;

	vec4 sorg = uSweepBandOrigin[sb];
	int smode = int(sorg.w);
	if (smode <= 0) return 0.0;

	float sdist;
	if (smode == 1)      sdist = length(pixelpos.xz - sorg.xz);
	else if (smode == 2) sdist = abs(pixelpos.x - sorg.x);
	else if (smode == 3) sdist = abs(pixelpos.z - sorg.z);
	else if (smode == 5) sdist = pixelpos.y - sorg.y;
	else                 sdist = length(pixelpos.xyz - sorg.xyz);

	float ssigned = sdist - sband.x;
	float thick = max(sband.y, 0.001);
	float strail = abs(uSweepTrail);
	float sbehind = (uSweepTrail >= 0.0) ? -ssigned : ssigned;
	float swidth = (strail > thick && sbehind > 0.0) ? strail : thick;

	float b = abs(ssigned);
	if (b >= swidth) return 0.0;
	return pow(1.0 - b / swidth, max(sband.z, 0.01));
}

//===========================================================================
//
// Calculate light
//
// It is important to note that the light color is not desaturated
// due to ZDoom's implementation weirdness. Everything that's added
// on top of it, e.g. dynamic lights and glows are, though, because
// the objects emitting these lights are also.
//
// This is making this a bit more complicated than it needs to
// because we can't just desaturate the final fragment color.
//
//===========================================================================

//
// [BB] THE GLOW WAVE -- the axis a glow never had.
//
// A glow varies per pixel going UP a wall and is one flat value going ALONG
// it, because reach arrives as a single number for the whole surface. So a
// wall faded beautifully top to bottom and had a dead straight top edge from
// one end of the room to the other. Recolour bands fixed that for COLOUR.
// Nothing fixed it for SHAPE.
//
// This returns a signed -1..+1 modulation for this fragment. Feed it a
// channel's phase and the blocks below apply it to REACH -- so the edge
// itself rises and falls -- and separately to brightness and to the near/far
// colour boundary. One wave, three terms, and they look nothing like each
// other: reach moves the shape, brightness moves the light, and colour moves
// where one lane's colour becomes the other's INSIDE a band whose shape never
// changes at all.
//
// Distance is measured with the SAME five shapes as the sweep, from the
// wave's own origin, so a wave running along the floor and a band crossing
// the room can be given one shape and made to arrive together. Two systems
// that measure the world differently can never be lined up; two that share a
// distance function line up by construction.
//
//   uGlowWave        x wavelength, y speed, z sharpness, w shape
//   uGlowWaveDepth   x reach, y brightness, z colour, w detune
//   uGlowWavePhase   x wall top, y wall bottom, z floor, w ceiling
//   uGlowWaveOrigin  xyz origin, w per-room seed scatter
//
// Wavelength 0 means off, and every glow falls through to exactly the
// arithmetic it did before this existed.
//
float GlowWaveRaw(float phase, float seedOff)
{
	if (uGlowWave.x <= 0.0) return 0.0;

	int wshape = int(uGlowWave.w);
	vec3 wo = uGlowWaveOrigin.xyz;
	float d;
	if (wshape == 2)      d = abs(pixelpos.x - wo.x);
	else if (wshape == 3) d = abs(pixelpos.z - wo.z);
	else if (wshape == 4) d = length(pixelpos.xyz - wo);
	else if (wshape == 5) d = pixelpos.y - wo.y;
	else                  d = length(pixelpos.xz - wo.xz);

	float t = d / uGlowWave.x + timer * uGlowWave.y + phase + seedOff;
	float w = 0.5 + 0.5 * sin(t);

	// DETUNE. One sine is legible as machinery within about ten seconds,
	// because it repeats and the eye finds the period. A second sine at a
	// wavelength that does not divide the first never resolves, so the room
	// keeps almost-repeating and never quite does. That is the whole
	// difference between "animated" and "alive", and it costs one sin.
	if (uGlowWaveDepth.w > 0.0)
	{
		float w2 = 0.5 + 0.5 * sin(t * 0.6180339887 + 1.7);
		w = mix(w, w * w2 * 2.0, uGlowWaveDepth.w);
	}

	// Sharpness narrows the CREST and never the trough, because it is a pow
	// on a 0..1 value. At 1 it is a plain swell; high, it is a spike through
	// an otherwise flat lane -- weather against machinery.
	w = pow(clamp(w, 0.0, 1.0), max(uGlowWave.z, 0.001));

	return 2.0 * w - 1.0;
}

// PER-ROOM SCATTER. Without this the entire map undulates as one organism,
// which reads as a filter over the game rather than as lighting in it. The
// seed is taken from geometry that is already uploaded and already differs
// per sector -- the glow plane's height for a wall, the first linedef
// endpoint for a flat -- so every room gets its own moment for nothing.
float GlowWaveSeedOff(float src)
{
	if (uGlowWaveOrigin.w <= 0.0) return 0.0;
	return fract(sin(src * 12.9898) * 43758.5453) * 6.2831853 * uGlowWaveOrigin.w;
}

//
// [BB] DARKNESS, PER FRAGMENT.
//
// A darkness mod scales each SECTOR's colour: one multiplier, one room, wall
// to wall. That was correct when a sector's light level was the only lever
// there was. It stopped being correct the moment a band of light could be
// measured per pixel -- and the tell was that the reveal worked by
// multiplying back UP what the darkness had multiplied DOWN. Two features
// politely undoing each other and calling the result a lighting model.
//
// THE FOUR CURVES ARE UNCHANGED. Subtract, compress, cap brightest and deepen
// shadows, transcribed from the ZScript they came from, which took them
// verbatim from the original. Pre-gain lifts the input before the curve;
// min-light floors the result and post-gain lifts it after. Same arithmetic,
// same order, same numbers.
//
// What changes is the INPUT: the fragment's own light rather than the room's.
// That alone stops a large room being uniformly dim. Then two terms that a
// sector could never have expressed at all:
//
//   DISTANCE -- darkness deepening with range. This is the one that makes a
//   dark room feel like it has depth instead of like the brightness slider
//   went down, and it is one line here and impossible per sector.
//
//   HEIGHT -- dark pooling at floor level, or rising as a tide.
//
// Returns the fraction of light that survives, 0..1.
//
//   uDarkness   mode, adjust, min light, pre-gain
//   uDarkness2  post-gain, distance depth, distance range, -
//   uDarkness3  height depth, height reference, height range, -
//
float DarknessAt(float lightLevel)
{
	int dmode = int(uDarkness.x);
	if (dmode <= 0) return 1.0;

	// The curves work in Doom's 0-255 light, because that is what they were
	// written against and every one of their constants -- 256, 33, /8 -- is
	// in those units. Converting here rather than rescaling the curves keeps
	// them readable against the original.
	float base = lightLevel * 255.0;
	if (base <= 0.0) return 1.0;      // already black; nothing to scale

	float A = uDarkness.y;
	float L = base + uDarkness.w;     // pre-gain, before the curve

	float outL;
	if (dmode == 1)                   // subtract -- a simple fade
	{
		outL = L - A;
	}
	else if (dmode == 2)              // compress -- the one that ignores base
	{
		outL = L * (1.0 - A / 256.0);
	}
	else if (dmode == 3)              // cap brightest -- dark rooms survive
	{
		outL = min(L, 256.0 - A);
	}
	else                              // deepen shadows -- exponential gamma
	{
		if (A <= 0.0) outL = L;
		else outL = (256.0 - pow(A, A / 256.0))
		          * pow(L / 256.0, 1.0 + (A / (33.0 - (A / 8.0))));
	}

	outL = max(outL, uDarkness.z);    // min light, a floor
	outL += uDarkness2.x;             // post-gain, a lift

	float mul = clamp(outL / base, 0.0, 1.0);

	// DISTANCE. Measured from the eye to this fragment, so it deepens with
	// range rather than with which room you are standing in.
	if (uDarkness2.y > 0.0)
	{
		float d = distance(pixelpos.xyz, uCameraPos.xyz) / max(uDarkness2.z, 1.0);
		mul *= 1.0 - uDarkness2.y * clamp(d, 0.0, 1.0);
	}

	// HEIGHT. Below the reference, dark pools; above it, nothing changes. The
	// range is how far below the reference it takes to reach full depth.
	if (uDarkness3.x > 0.0)
	{
		float below = (uDarkness3.y - pixelpos.y) / max(uDarkness3.z, 1.0);
		mul *= 1.0 - uDarkness3.x * clamp(below, 0.0, 1.0);
	}

	return clamp(mul, 0.0, 1.0);
}

//
// [BB] REAL BEAMS.
//
// A laser in Doom is usually a sprite, or a chain of puffs spawned close
// enough together to read as a line. Both show what they are: the sprite
// lights nothing, and the chain stitches, gaps at long range, and costs an
// actor per segment.
//
// A beam is a SEGMENT. Light every pixel by its distance from that segment
// and you get the real thing -- the same idea as a sweep band, with the only
// difference being which distance is measured:
//
//   sweep band   distance from a POINT     length(p - origin)
//   beam         distance from a SEGMENT   length(p - closest(a,b))
//
// Because it is per pixel in world space, everything else follows without
// being asked for. The beam is continuous at any length with no repeat and no
// stitching. It wraps across floor, wall and ceiling as one unbroken object.
// And the surfaces near it brighten because they ARE near it, not because
// something also spawned a dynamic light to fake that.
//
// TWO FALLOFFS FROM ONE DISTANCE, and this is what separates a beam that
// looks hot from a bright line. A hard narrow CORE a couple of units across,
// and a wide soft HALO around it. One without the other reads as either a
// drawn line or a smear; together they read as something incandescent.
//
vec3 BeamLightAt(vec3 p)
{
	vec3 sum = vec3(0.0);
	int n = int(uBeamParams.x);
	if (n <= 0) return sum;

	for (int i = 0; i < 8; i++)
	{
		if (i >= n) break;

		vec3 a = uBeamA[i].xyz;
		vec3 b = uBeamB[i].xyz;
		vec3 ab = b - a;
		vec3 ap = p - a;

		// Closest point on the SEGMENT, not the infinite line -- the clamp is
		// what makes a beam end where it ends instead of lighting everything
		// along its axis out to the edge of the map.
		float t = clamp(dot(ap, ab) / max(dot(ab, ab), 0.0001), 0.0, 1.0);
		float d = length(ap - ab * t);

		float thick = max(uBeamA[i].w, 0.01);
		float soft  = max(uBeamB[i].w, 0.01);

		float core = 1.0 - smoothstep(thick, thick + soft, d);
		float halo = 1.0 - smoothstep(thick, thick + soft * 8.0 + 1.0, d);

		sum += uBeamCol[i].rgb * (core + halo * uBeamParams.y) * uBeamCol[i].w;
	}
	return sum;
}

//
// [BB] THE BEAM SEEN IN THE AIR -- emissive glow, no light and no geometry.
//
// BeamLightAt above lights SURFACES near a beam. That is not the same as
// seeing the beam: in a clear room it gives you a bright patch where the beam
// lands and nothing in between, which is a spotlight, not a laser.
//
// Making the beam itself visible normally means geometry -- a camera-facing
// quad strip with an additive texture -- and that brings everything a quad
// brings: a draw call, sorting against translucents, a seam where segments
// meet, and a beam that vanishes when viewed end-on.
//
// It does not need any of that. Asking "how close does my LINE OF SIGHT pass
// to this beam" is a segment-to-ray closest approach, which is a dozen lines
// of algebra with no object in it. Every fragment already knows where the eye
// is and where it is; that is enough.
//
// AND IT COMES OUT DEPTH-CORRECT FOR FREE. The closest approach has a distance
// ALONG the ray, so comparing that against the distance to the fragment says
// whether the beam passes in front of this pixel or behind it. A beam behind a
// wall is simply not drawn -- which is the one artefact the surface lighting
// genuinely cannot avoid, solved here as a side effect of the maths rather
// than by a shadow pass.
//
// It also feeds bloom without being told to. The bloom pass thresholds bright
// pixels, and a beam core writes values well past white, so the glow blooms
// exactly as an emissive thing should.
//
vec3 BeamAirGlow(vec3 fragPos)
{
	vec3 sum = vec3(0.0);
	int n = int(uBeamParams.x);
	if (n <= 0 || uBeamParams.w <= 0.0) return sum;

	vec3 eye = uCameraPos.xyz;
	vec3 toFrag = fragPos - eye;
	float fragDist = length(toFrag);
	if (fragDist < 0.001) return sum;
	vec3 dir = toFrag / fragDist;

	for (int i = 0; i < 8; i++)
	{
		if (i >= n) break;

		vec3 a = uBeamA[i].xyz;
		vec3 b = uBeamB[i].xyz;
		vec3 v = b - a;
		vec3 w = eye - a;

		float bb = dot(dir, v);
		float cc = dot(v, v);
		float dd = dot(dir, w);
		float ee = dot(v, w);
		float den = cc - bb * bb;      // dot(dir,dir) is 1

		float sc, tc;
		if (abs(den) < 0.0001)
		{
			// Looking straight down the beam. Any point does; take the near
			// end, which keeps an end-on beam a bright dot instead of the
			// division blowing up.
			sc = -dd;
			tc = 0.0;
		}
		else
		{
			sc = (bb * ee - cc * dd) / den;
			tc = (bb * -dd + ee) / den;
		}

		// Clamp to the ray in front of the eye, no further than the surface
		// this pixel is on -- that clamp IS the depth test -- and to the
		// segment, so the glow ends where the beam ends.
		sc = clamp(sc, 0.0, fragDist);
		tc = clamp(tc, 0.0, 1.0);

		float dist = length((eye + dir * sc) - (a + v * tc));

		float thick = max(uBeamA[i].w, 0.01);
		float soft  = max(uBeamB[i].w, 0.01);

		// ---- WHAT HAPPENS ALONG THE BEAM ------------------------------
		//
		// tc is where on the segment we are, 0 at the muzzle and 1 at the
		// impact. It fell out of the closest-approach solve, so everything
		// below is arithmetic on a number already in hand.
		//
		// TAPER. A perfectly parallel-sided beam reads as a drawn line. Real
		// glare is tighter at the aperture and blooms toward what it hits.
		float bw = mix(1.0 - uBeamFX.z, 1.0, tc);
		thick *= bw;
		soft  *= bw;

		float bright = 1.0;

		// SCROLL. Energy travelling muzzle-to-impact. Without it a held beam
		// is completely static and the eye stops believing it is carrying
		// anything -- this is the single thing that makes it read as a beam
		// under load rather than a stick of light.
		if (uBeamFX.y > 0.0)
		{
			float along = tc * length(v);
			float s = sin(along * 0.06 - timer * uBeamFX.x);
			bright *= 1.0 + uBeamFX.y * s;
		}

		// IMPACT FLARE. A beam that simply stops looks unfinished; the far
		// end is where the energy is actually going, so it is the brightest
		// part of the whole thing.
		if (uBeamFX.w > 0.0)
			bright += uBeamFX.w * pow(clamp(tc, 0.0, 1.0), 8.0);

		// The same two-falloff shape the surface light uses, so the beam in
		// the air and the light it casts agree about how thick it is.
		float core = 1.0 - smoothstep(thick * 0.5, thick + soft, dist);
		float halo = 1.0 - smoothstep(thick, thick + soft * 6.0 + 1.0, dist);

		sum += uBeamCol[i].rgb * (core * 1.6 + halo * uBeamParams.y)
			* uBeamCol[i].w * uBeamParams.w * bright;
	}
	return sum;
}

//
// [BB] FOG WITH A TOP.
//
// Sector fog is a distance tint on SURFACES -- the further a wall is, the more
// it blends toward the fog colour. Nothing is simulated in the air, which is
// why it has no shape: no ceiling, no thickness you can stand in, and no way
// to be brighter where a light passes through it. You cannot be knee deep in
// it, because it has no knees.
//
// This is a horizontal slab of participating medium with a world-space top,
// and it is solved ANALYTICALLY rather than raymarched. For a flat-topped slab
// the answer is closed form: find how much of the eye-to-fragment ray lay
// below the ceiling, and fog by that length. No loop, no step count, no
// undersampling artefacts, and the cost is a handful of ALU.
//
//   uFogSlab       x top Z, y density per 1000 units, z soft edge, w scatter
//   uFogSlabColor  rgb, w wake strength
//   uFogSlabWake   xyz wake point, w radius
//
// THE SOFT TOP IS NOT DECORATION. A hard cut at the ceiling reads as a plane
// of coloured glass lying across the room. Fading the density over the last
// few units turns it into a surface you can look down at, which is the whole
// effect being asked for.
//
// SCATTER uses the flashlight cone the renderer already knows about, so mist
// inside the beam brightens without a second trace. That is the difference
// between fog you are standing in and fog you are standing in WITH A TORCH.
//
// Returns rgb = the fog's own colour for this pixel, a = how much of it.
//
vec4 FogSlabAt(vec3 fragPos)
{
	if (uFogSlab.y <= 0.0) return vec4(0.0);

	vec3  eye  = uCameraPos.xyz;
	float topZ = uFogSlab.x;
	float soft = max(uFogSlab.z, 0.001);

	// Depth below the top, softened at the boundary, at each end of the ray.
	// smoothstep across the soft band is what gives the mist a surface rather
	// than an edge.
	float dEye  = smoothstep(topZ + soft, topZ - soft, eye.y);
	float dFrag = smoothstep(topZ + soft, topZ - soft, fragPos.y);

	// Average occupancy along the segment. Exact for a linear ramp, and the
	// error against a true integral through the smoothstep is far below what
	// the eye can see in fog.
	float occupancy = 0.5 * (dEye + dFrag);
	if (occupancy <= 0.0) return vec4(0.0);

	float travel = distance(eye, fragPos) * occupancy;

	// WAKE. Inside the radius the mist has been disturbed and is thinner --
	// this is the trail you kick up walking through it. One point that lags
	// behind the player, because a trail that settles IS a point that follows
	// you slowly.
	if (uFogSlabColor.w > 0.0 && uFogSlabWake.w > 0.0)
	{
		float d = distance(fragPos.xz, uFogSlabWake.xz);
		float w = 1.0 - smoothstep(0.0, uFogSlabWake.w, d);
		travel *= 1.0 - uFogSlabColor.w * w;
	}

	float amount = 1.0 - exp(-uFogSlab.y * travel * 0.001);
	amount = clamp(amount, 0.0, 1.0);

	vec3 col = uFogSlabColor.rgb;

	// SCATTER off the torch. Cheap: how far inside the beam cone this
	// fragment sits, using the cone the volumetric beam already describes.
	if (uFogSlab.w > 0.0 && uFogBeamPos.w > 0.0)
	{
		vec3 toFrag = fragPos - uFogBeamPos.xyz;
		float len = length(toFrag);
		if (len > 0.001)
		{
			float cosA = dot(toFrag / len, uFogBeamDir.xyz);
			float lit = smoothstep(uFogBeamCol.w, uFogBeamDir.w, cosA);
			lit *= 1.0 - clamp(len / uFogBeamPos.w, 0.0, 1.0);
			col += uFogBeamCol.rgb * lit * uFogSlab.w;
		}
	}

	// AND BEAMS LIGHT THE MIST THEY CROSS.
	//
	// This is the shot worth having: a laser through knee-deep fog should be
	// visible as a shaft along its whole length, not just as a bright line on
	// whatever it eventually hits. Evaluated at the fragment rather than
	// integrated along the ray, which is an approximation -- but the fog
	// amount already scales with how much mist is in the way, so the mist
	// glows near a beam and does not far from one, which is the whole read.
	if (uBeamParams.z > 0.0)
		col += BeamLightAt(fragPos) * uBeamParams.z;

	return vec4(col, amount);
}

//
// [BB] WHAT IS DRAWN INSIDE A BAND.
//
// A band knows two things about every pixel it covers: how strongly it covers
// it, and where that pixel is in the world. It used to discard the second and
// blend one flat colour weighted by the first, so a band could only ever be a
// wash.
//
// EVERY SHAPE THAT DEFINES A DISTANCE ALSO IMPLIES TWO TANGENT COORDINATES,
// and a pattern is a function of those two:
//
//   bar east/west   distance abs(x-ox)     pattern runs in (z, y)
//   bar north/south distance abs(z-oz)     pattern runs in (x, y)
//   rising          distance  y-oy         pattern runs in (x, z)
//   ring            distance length(xz-o)  pattern runs in (arc, y)
//   shell           distance length(xyz-o) pattern runs in (longitude, y)
//
// So one function draws a lattice standing in a corridor AND a cage on an
// expanding cylinder, with no per-shape casing beyond picking the two axes.
//
// SPACING 0 IN AN AXIS MEANS NO LINES IN THAT AXIS. That single rule collapses
// grid, slats and a lone tripwire into one mode with one number changed, which
// is why there is no separate enum for any of them.
//
// LINE WIDTH IS IN WORLD UNITS, not a fraction of the spacing. A fractional
// width makes the lattice coarsen with distance and shimmer under motion; a
// world width holds its real size and antialiases cleanly against fwidth.
//
//   uSweepFill     spacing U, spacing V, line width, softness
//   uSweepFill2    rotation (deg), drift, major every N, jitter
//   uSweepFill3    gradient amount, gradient axis, flicker, major boost
//   uSweepFillCol  rgb line colour, w gap fill
//
// Returns line coverage 0..1.
//
float SweepLineAxis(float coord, float spacing, float width, float soft, float t)
{
	if (spacing <= 0.0) return 0.0;

	float idx = floor(coord / spacing + 0.5);

	// JITTER -- push each line off the lattice by a stable hash of its own
	// index, so it reads as a row of emitters rather than a printed texture.
	if (uSweepFill2.w > 0.0)
	{
		float h = fract(sin(idx * 78.233) * 43758.5453);
		coord += (h - 0.5) * spacing * uSweepFill2.w;
		idx = floor(coord / spacing + 0.5);
	}

	// FLICKER -- individual lines dropping out and returning. Instantly reads
	// as failing equipment, and it is per LINE rather than per band, which is
	// what stops it looking like the whole thing is blinking.
	if (uSweepFill3.z > 0.0)
	{
		float h = fract(sin(idx * 12.9898 + floor(t * 8.0) * 3.717) * 43758.5453);
		if (h < uSweepFill3.z) return 0.0;
	}

	// MAJOR LINES -- every Nth one wider. Graph-paper structure for one mod.
	float w = width;
	if (uSweepFill2.z >= 2.0 && mod(abs(idx), uSweepFill2.z) < 0.5)
		w *= max(uSweepFill3.w, 1.0);

	// Distance to the nearest line, in world units.
	float d = abs(coord - idx * spacing);

	// Antialias against the screen-space derivative as well as the authored
	// softness, so a line a hundred units away is still one clean line rather
	// than a moire.
	float aa = max(fwidth(coord), 0.0001);
	return 1.0 - smoothstep(w, w + soft + aa, d);
}

float SweepFillAt(int fill, int shape, vec3 origin)
{
	if (fill <= 0) return 1.0;   // no fill: the band is a wash, as before

	// Pick the band's two tangent axes.
	vec2 uv;
	if (shape == 2)       uv = vec2(pixelpos.z, pixelpos.y);
	else if (shape == 3)  uv = vec2(pixelpos.x, pixelpos.y);
	else if (shape == 5)  uv = vec2(pixelpos.x, pixelpos.z);
	else
	{
		// Ring and shell: arc length around the axis, and height. Arc length
		// rather than raw angle so the spacing stays constant in world units
		// as the band expands -- an angular grid would spread apart as it
		// grew, which is not a cage, it is a fan.
		vec2 rel = pixelpos.xz - origin.xz;
		float r = max(length(rel), 0.001);
		uv = vec2(atan(rel.y, rel.x) * r, pixelpos.y);
	}

	// DRIFT -- the pattern sliding within the band as the band travels. This
	// is what makes it read as projected rather than painted onto the band.
	float t = timer;
	uv.x += t * uSweepFill2.y;

	// ROTATION in the band's own plane. Animate it and you get a lattice
	// turning inside a wall of light that is itself moving down a corridor.
	if (uSweepFill2.x != 0.0)
	{
		float a = radians(uSweepFill2.x);
		float cs = cos(a), sn = sin(a);
		uv = vec2(uv.x * cs - uv.y * sn, uv.x * sn + uv.y * cs);
	}

	float lu = SweepLineAxis(uv.x, uSweepFill.x, uSweepFill.z, uSweepFill.w, t);
	float lv = SweepLineAxis(uv.y, uSweepFill.y, uSweepFill.z, uSweepFill.w, t);
	float cov = max(lu, lv);

	// DOTS -- fill 2 keeps only where the two axes CROSS, so the lattice
	// becomes a field of points. Same maths, one operator changed.
	if (fill == 2) cov = min(lu, lv);

	// GRADIENT along one axis, so the lattice can be hot at floor level and
	// fade out overhead. Cheap, and it stops a grid looking like a decal.
	if (uSweepFill3.x > 0.0)
	{
		float g = (uSweepFill3.y > 0.5) ? uv.x : uv.y;
		g = clamp(g * 0.002 + 0.5, 0.0, 1.0);
		cov *= mix(1.0, g, uSweepFill3.x);
	}

	return clamp(cov, 0.0, 1.0);
}

vec4 getLightColor(Material material, float fogdist, float fogfactor)
{
	vec4 color = vColor;
#ifndef SHADER_LITE
	if (uLightLevel >= 0.0)
	{
		float newlightlevel = 1.0 - R_DoomLightingEquation(uLightLevel);
		color.rgb *= newlightlevel;
	}
	else if (uFogEnabled > 0)
	{
		// brightening around the player for light mode 2
		if (fogdist < uLightDist)
		{
			color.rgb *= uLightFactor - (fogdist / uLightDist) * (uLightFactor - 1.0);
		}

		//
		// apply light diminishing through fog equation
		//
		color.rgb = mix(vec3(0.0, 0.0, 0.0), color.rgb, fogfactor);
	}

	//
	// [BB] DARKNESS, AND IT GOES HERE FOR A REASON.
	//
	// AFTER the lighting equation, so it scales the light the room actually
	// ended up with -- including whatever Doom's own blinking and flickering
	// sectors did to it this tic, which is a thing the per-sector version had
	// to fight for and gets here for free.
	//
	// BEFORE the glow and the sweep, which is the important half. Those are
	// EMISSIVE: they are light this mod is adding, not light the room has.
	// Darkening them would darken the only thing left to see in a black room,
	// and the whole design is that the glow survives the dark. Everything
	// below this line is added on top of an already-darkened surface, exactly
	// as a light in a dark room is.
	//
	// uLightLevel is the sector's light, 0..1, and is what the curves want.
	// The fog path has no scalar light -- the colour IS the light there -- so
	// its luminance stands in, which keeps the two paths agreeing about how
	// dark a room is instead of one of them quietly opting out.
	if (uDarkness.x > 0.0)
	{
		float dl = (uLightLevel >= 0.0) ? uLightLevel : grayscale(vec4(color.rgb, 1.0));
		color.rgb *= DarknessAt(dl);
	}

	//
	// [BB] How strongly band sb covers THIS pixel, 0 if it does not.
	//
	// Factored out because two passes need it and they are separated by the
	// whole glow block: the recolour pass below has to run BEFORE the glow is
	// drawn (it changes what colour the glow is), and the light pass has to
	// run after (it changes what the finished pixel becomes). One copy of the
	// maths, so the two can never drift apart and disagree about where a band
	// is.
	//

	//
	// [BB] SWEEP RECOLOUR -- runs BEFORE the glow, because it changes what
	// colour the glow IS rather than adding light on top of it.
	//
	// Glow already varies per pixel VERTICALLY: glowdist is the fragment's
	// distance from the floor or ceiling plane, which is what makes coverage
	// and falloff smooth up a wall. Horizontally it could not vary at all,
	// because the colour arrives as one value for the whole surface. So a
	// wall could fade top to bottom beautifully and was a single flat colour
	// left to right.
	//
	// A band in recolour mode (4) blends the glow toward its own colour by
	// how strongly it covers this pixel. The result is a colour change that
	// SWEEPS ACROSS the wall instead of the room flipping: glow ahead of the
	// band is your palette, glow inside it is the band's, and the boundary
	// travels.
	//
	// Strongest band wins rather than accumulating -- two overlapping
	// recolours should hand over, not average into mud.
	//
	vec3 sweepTint = vec3(0.0);
	float sweepTintW = 0.0;
	for (int rb = 0; rb < 8; rb++)
	{
		if (rb >= uSweepCount) break;
		if (int(uSweepBands[rb].w) != 4) continue;
		float ra = SweepBandAttenAt(rb) * uSweepColors[rb].a;
		if (ra > sweepTintW) { sweepTintW = ra; sweepTint = uSweepColors[rb].rgb; }
	}
	sweepTintW = clamp(sweepTintW, 0.0, 1.0);

	//
	// handle glowing walls
	//
	// [BB] Each glow can carry a SECOND colour. Without one a glow holds a
	// single colour and only dims as it fades, so a wall and the floor it
	// meets arrive at their shared line as two different colours at full
	// strength -- a hard edge no amount of falloff tuning removes. With a far
	// colour the glow ramps instead: the primary colour sits AT the plane
	// (atten 1) and the far colour is where it fades to (atten 0). Hand the
	// same junction colour to both surfaces and the corner becomes one
	// continuous ramp -- floor colour, blend, wall colour -- with no flat
	// region in it. Alpha 0 on the far colour means unset, and the glow is
	// byte-for-byte the flat wash it always was.
	//
	// [BB] The wave, per channel. Reach is modulated BEFORE the cutoff test,
	// which is the whole point: multiplying the finished contribution only
	// makes a straight-edged band pulse brighter and dimmer, while moving the
	// reach moves the edge itself. The far-colour ramp rides atten, so the
	// corner gradient stretches and squashes with the edge for free.
	//
	// Seeded from the glow plane's height, which is already here, already
	// per sector, and already differs between rooms.
	float wTop = GlowWaveRaw(uGlowWavePhase.x, GlowWaveSeedOff(uGlowTopPlane.w));
	float wBot = GlowWaveRaw(uGlowWavePhase.y, GlowWaveSeedOff(uGlowBottomPlane.w));

	float topReach = uGlowTopColor.a * (1.0 + uGlowWaveDepth.x * wTop);
	if (uGlowTopColor.a > 0.0 && glowdist.x < topReach)
	{
		float topfrac = glowdist.x / topReach;
		float topatten;
		if (uGlowTopFalloff == 0)      topatten = 1.0 - topfrac;
		else if (uGlowTopFalloff == 1) topatten = 1.0 - topfrac * topfrac;
		else if (uGlowTopFalloff == 2) topatten = 1.0 - sqrt(topfrac);
		else                            topatten = exp(-topfrac * 3.0);
		vec3 gtop = uGlowTopColor.rgb;
		// Colour depth slides the near/far boundary without touching the
		// shape -- the band stands still and the colour moves through it.
		if (uGlowTopFar.a > 0.0)
			gtop = mix(uGlowTopFar.rgb, gtop, clamp(topatten + uGlowWaveDepth.z * wTop, 0.0, 1.0));
		gtop = mix(gtop, sweepTint, sweepTintW);
		color.rgb += desaturate(vec4(gtop * topatten * uGlowTopIntensity
			* (1.0 + uGlowWaveDepth.y * wTop), 1.0)).rgb;
	}
	float botReach = uGlowBottomColor.a * (1.0 + uGlowWaveDepth.x * wBot);
	if (uGlowBottomColor.a > 0.0 && glowdist.y < botReach)
	{
		float botfrac = glowdist.y / botReach;
		float botatten;
		if (uGlowBottomFalloff == 0)      botatten = 1.0 - botfrac;
		else if (uGlowBottomFalloff == 1) botatten = 1.0 - botfrac * botfrac;
		else if (uGlowBottomFalloff == 2) botatten = 1.0 - sqrt(botfrac);
		else                                botatten = exp(-botfrac * 3.0);
		vec3 gbot = uGlowBottomColor.rgb;
		if (uGlowBottomFar.a > 0.0)
			gbot = mix(uGlowBottomFar.rgb, gbot, clamp(botatten + uGlowWaveDepth.z * wBot, 0.0, 1.0));
		gbot = mix(gbot, sweepTint, sweepTintW);
		color.rgb += desaturate(vec4(gbot * botatten * uGlowBottomIntensity
			* (1.0 + uGlowWaveDepth.y * wBot), 1.0)).rgb;
	}

	//
	// [BB] flat-edge glow: floors/ceilings glow inward from their own
	// linedef edges. Unlike the wall glow above, this tints the flat's OWN
	// pixels, not a nearby wall's.
	//
	if (uFlatGlowColor.a > 0.0 && uFlatGlowLineCount > 0)
	{
		vec2 pixXZ = pixelpos.xz;
		float minDist = 999999.0;

		for (int i = 0; i < uFlatGlowLineCount; i++)
		{
			vec4 seg = uFlatGlowLines[i];
			vec2 a = seg.xy;
			vec2 b = seg.zw;
			vec2 ab = b - a;
			vec2 ap = pixXZ - a;
			float t = clamp(dot(ap, ab) / max(dot(ab, ab), 0.001), 0.0, 1.0);
			float d = length(ap - ab * t);
			minDist = min(minDist, d);
		}

		// [BB] Floor and ceiling TIME-SHARE this uniform, because a draw only
		// ever covers one of them -- so the shader cannot tell which it is
		// and has to be told. uFlatGlowIsCeiling is what used to be a pad.
		// Without it both flats would take the same phase and the wave could
		// not climb a room.
		//
		// Seeded off the first linedef endpoint, which is per sector and
		// already uploaded for the distance search above.
		float flatPhase = (uFlatGlowIsCeiling != 0) ? uGlowWavePhase.w : uGlowWavePhase.z;
		float wFlat = GlowWaveRaw(flatPhase,
			GlowWaveSeedOff(uFlatGlowLines[0].x + uFlatGlowLines[0].y));

		float reach = uFlatGlowColor.a * (1.0 + uGlowWaveDepth.x * wFlat);
		if (minDist < reach)
		{
			float frac = minDist / reach;
			float atten;
			if (uFlatGlowFalloff == 0)      atten = 1.0 - frac;
			else if (uFlatGlowFalloff == 1) atten = 1.0 - frac * frac;
			else if (uFlatGlowFalloff == 2) atten = 1.0 - sqrt(frac);
			else                             atten = exp(-frac * 3.0);

			vec3 gflat = uFlatGlowColor.rgb;
			if (uFlatGlowFar.a > 0.0)
				gflat = mix(uFlatGlowFar.rgb, gflat, clamp(atten + uGlowWaveDepth.z * wFlat, 0.0, 1.0));
			// [BB] RECOLOUR REACHED TWO CHANNELS OUT OF FOUR. Both wall glows
			// mix sweepTint above and this one never did, so a recolour band
			// sweeping a room changed the walls and left the floor and the
			// ceiling on the old palette -- visible as the band crossing a
			// corner and stopping dead at it.
			gflat = mix(gflat, sweepTint, sweepTintW);
			color.rgb += desaturate(vec4(gflat * atten
				* (1.0 + uGlowWaveDepth.y * wFlat), 1.0)).rgb;
		}
	}

	//
	// [BB] sweep: a thin band of light at a fixed distance from an origin,
	// measured in world space. Because the distance is world-space and this
	// runs on every surface, the band wraps across floor, wall and ceiling
	// by itself -- a cylinder cuts all three at the same radius, a plane
	// draws an unbroken rectangle around a corridor. Nothing here knows or
	// cares what kind of surface it is shading, which is the entire trick.
	//
	if (uSweepCount > 0)
	{
		// EVERY BAND CARRIES ITS OWN ORIGIN AND ITS OWN SHAPE.
		//
		// This used to compute the distance ONCE, outside the loop, from a
		// single shared origin -- which made a train of eight nearly free but
		// forced all eight to be concentric and the same shape. That was a
		// shortcut, not a limit: nothing about a band requires it to agree
		// with its neighbours about where the centre of the world is.
		//
		// Now the distance moves inside the loop and reads uSweepBandOrigin
		// per band -- xyz is that band's origin, w is its shape. So a ring can
		// expand from the map centre while a column climbs out of a corner and
		// a plane sweeps the long axis, all in the same frame. The cost is eight
		// distance evaluations instead of one, which is a handful of ALU in a
		// shader that is already sampling textures.
		//
		// w <= 0 means "this band is off", which is also the gate that used to
		// live on the shared origin.
		for (int sb = 0; sb < 8; sb++)
		{
			if (sb >= uSweepCount) break;
			vec4 sband = uSweepBands[sb];

			// [BB] The draw mode and the FILL mode share this component --
			// drawmode + 16 * fill. See SetSweepBandDraw for why: draw mode
			// is 0-4 and always will be, so the rest of the float was free,
			// and a vec4[8] of per-band fill in StreamData would have cost
			// draw batching in every frame of the game.
			int bandpack = int(sband.w);
			int bmode = bandpack & 15;
			int bfill = bandpack >> 4;

			if (bmode <= 0) continue;
			// Recolour bands already had their say, above the glow.
			if (bmode == 4) continue;

			float satten = SweepBandAttenAt(sb);
			if (satten <= 0.0) continue;
			vec4 scol = uSweepColors[sb];

			// WHAT IS INSIDE THE BAND.
			//
			// The band's own colour is the FIELD and the fill colour is the
			// LINES. Gap 0 means only the lines are lit and the room shows
			// through between them, which is what reads as actual lasers --
			// turn it up and it becomes a lit pane with structure in it,
			// which is a completely different object.
			//
			// A negative gap inverts: lit gaps, dark lines. A grid of shadow.
			if (bfill > 0)
			{
				float cov = SweepFillAt(bfill, int(uSweepBandOrigin[sb].w), uSweepBandOrigin[sb].xyz);

				// Fill 3 is SOLID -- the band ignores its own falloff and
				// becomes a flat slab of light with hard edges. A wall
				// rather than a glow, and the only fill that wants no lines.
				if (bfill == 3) { cov = 1.0; satten = satten > 0.0 ? 1.0 : 0.0; }

				float gap = uSweepFillCol.w;
				scol.rgb = mix(scol.rgb * max(gap, 0.0), uSweepFillCol.rgb, cov);
				if (gap < 0.0) scol.rgb = mix(uSweepFillCol.rgb, scol.rgb * (-gap), cov);
				satten *= max(cov, max(gap, 0.0));
			}

			// The wake. A band is symmetric until uSweepTrail says otherwise,
			// and then it is simply WIDER on the side it came from -- one
			// falloff stretched, not a second gradient bolted alongside, so
			// there is no seam at the core. That lives in SweepBandAttenAt
			// now, shared with the recolour pass above.
			//
			// WHAT THE BAND DOES TO THE PIXEL. sband.w was a bare on/off flag
			// hardcoded to 1.0 -- four bytes of nothing -- and is the mode
			// now, so this cost no extra uniform space. 0 still means off.
			//
			// ADD (1)    emits light additively. A glowing line, and useless
			//            as a reveal: adding a colour to a room crushed
			//            toward black gives you that colour, not the room.
			// LIFT (2)   multiplies what is already there. Darkness scales a
			//            sector's colour down, so the detail is not gone, it
			//            is small -- scaling back up restores it. Per pixel,
			//            so a room ten times the band's width reveals in a
			//            moving strip instead of switching on whole.
			// CRUSH (3)  the same operation inverted: a travelling darkness.
			// RECOLOUR (4) handled above, before the glow.
			if (bmode == 2)
			{
				color.rgb *= (1.0 + satten * scol.a);
			}
			else if (bmode == 3)
			{
				color.rgb *= max(0.0, 1.0 - satten * scol.a);
			}
			else
			{
				color.rgb += desaturate(vec4(scol.rgb * satten * scol.a, 1.0)).rgb;
			}
		}

	}

	// [BB] BEAMS. Additive, and here for the same reason the glow is here:
	// they are EMISSIVE. The darkness term ran further up, before all of
	// this, precisely so that light this mod adds survives being in a dark
	// room -- a laser that got dimmer as the room got darker would be a
	// contradiction.
	//
	// Surfaces near a beam brighten because they are near it. Nothing else
	// had to be spawned to make that happen.
	color.rgb += BeamLightAt(pixelpos.xyz);
#endif
	color = min(color, 1.0);

	// these cannot be safely applied by the legacy format where the implementation cannot guarantee that the values are set.
#if !defined LEGACY_USER_SHADER && !defined NO_LAYERS
	//
	// apply glow 
	//
	color.rgb = mix(color.rgb, material.Glow.rgb, material.Glow.a);

	//
	// apply brightmaps 
	//
	color.rgb = min(color.rgb + material.Bright.rgb, 1.0);
#endif

	//
	// apply other light manipulation by custom shaders, default is a NOP.
	//
	color = ProcessLight(material, color);
	
	//
	// apply lightmaps
	//
	if (vLightmap.z >= 0.0)
	{
		color.rgb += texture(LightMap, vLightmap).rgb;
	}

	//
	// apply dynamic lights
	//
	return vec4(ProcessMaterialLight(material, color.rgb), material.Base.a * vColor.a);
}

//===========================================================================
//
// Applies colored fog
//
//===========================================================================

vec4 applyFog(vec4 frag, float fogfactor)
{
	return vec4(mix(uFogColor.rgb, frag.rgb, fogfactor), frag.a);
}

//===========================================================================
//
// The color of the fragment if it is fully occluded by ambient lighting
//
//===========================================================================

vec3 AmbientOcclusionColor()
{
	float fogdist;
	float fogfactor;

	//
	// calculate fog factor
	//
	if (uFogEnabled == -1) 
	{
		fogdist = max(16.0, pixelpos.w);
	}
	else 
	{
		fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
	}
	fogfactor = exp2 (uFogDensity * fogdist);

	return mix(uFogColor.rgb, vec3(0.0), fogfactor);
}

vec4 ApplyFadeColor(vec4 frag)
{
	if (uGlobalFade == 1 && uFogEnabled != 0)
	{
		float fogdist;
		if (uFogEnabled == 1 || uFogEnabled == -1) 
		{
			// standard fog (1 or -1)
			fogdist = max(16.0, pixelpos.w);
		}
		else 
		{
			// radial fog (2 or -2)
			fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
		}
		float visibility = exp(-pow((fogdist * uGlobalFadeDensity), uGlobalFadeGradient));
		visibility = clamp(visibility, 0.0, 1.0);
		vec4 fogcolor = uGlobalFadeColor;
		if (uGlobalFadeMode == -1)
		{
			frag = vec4(mix(fogcolor.rgb, frag.rgb, visibility), frag.a * visibility);
		}
		else if (uGlobalFadeMode == 2)
		{
			frag = vec4(fogcolor.rgb, frag.a) * visibility;
		}
	}
	return frag;
}

//===========================================================================
//
// Main shader routine
//
//===========================================================================

void main()
{
#ifdef NO_CLIPDISTANCE_SUPPORT
	if (ClipDistanceA.x < 0.0 || ClipDistanceA.y < 0.0 || ClipDistanceA.z < 0.0 || ClipDistanceA.w < 0.0 || ClipDistanceB.x < 0.0) discard;
#endif

#ifndef LEGACY_USER_SHADER
	Material material;

	material.Base = vec4(0.0);
	material.Bright = vec4(0.0);
	material.Glow = vec4(0.0);
	material.Normal = vec3(0.0);
	material.Specular = vec3(0.0);
	material.Glossiness = 0.0;
	material.SpecularLevel = 0.0;
	material.Metallic = 0.0;
	material.Roughness = 0.0;
	material.AO = 0.0;
	SetupMaterial(material);
#else
	Material material = ProcessMaterial();
#endif
	vec4 frag = material.Base;

#ifndef NO_ALPHATEST
	if (frag.a <= uAlphaThreshold) discard;
#endif

	if (uFogEnabled != -3)	// check for special 2D 'fog' mode.
	{
		float fogdist = 0.0;
		float fogfactor = 0.0;
#ifdef SHADER_LITE
		fogdist = max(16.0, pixelpos.w);
		fogfactor = exp2 (uFogDensity * fogdist);
		frag = getLightColor(material, fogdist, fogfactor);
#else
		//
		// calculate fog factor
		//
		if (uFogEnabled != 0)
		{
			if (uFogEnabled == 1 || uFogEnabled == -1) 
			{
				fogdist = max(16.0, pixelpos.w);
			}
			else 
			{
				fogdist = max(16.0, distance(pixelpos.xyz, uCameraPos.xyz));
			}
			fogfactor = exp2 (uFogDensity * fogdist);
		}

		if ((uTextureMode & 0xffff) != 7)
		{
			frag = getLightColor(material, fogdist, fogfactor);

			//
			// colored fog
			//
			if (uFogEnabled < 0) 
			{
				frag = applyFog(frag, fogfactor);
			}
		}
		else
		{
			frag = vec4(uFogColor.rgb, (1.0 - fogfactor) * frag.a * 0.75 * vColor.a);
		}
#endif
		frag = ApplyFadeColor(frag);

		// [BB] THE FOG SLAB GOES LAST, AND OVER EVERYTHING.
		//
		// This is the opposite placement to the darkness term, which runs
		// BEFORE the glow so that emissive light survives being in a dark
		// room. Fog is not darkness -- it is a substance sitting BETWEEN the
		// eye and the surface, so it occludes whatever is behind it including
		// the glow. A glowing floor seen through knee-deep mist should be a
		// glow diffused by mist, not a glow with mist politely behind it.
		vec4 slab = FogSlabAt(pixelpos.xyz);
		if (slab.a > 0.0)
		{
			// [BB] THE MIST PICKS UP WHAT IS BEHIND IT.
			//
			// Without this the slab is a flat colour laid over the scene, and
			// it reads as a filter rather than as a substance: mist standing
			// in front of a red glowing wall stays its own colour, which is
			// wrong in a way that is instantly obvious even if you cannot
			// name it. Real mist near a coloured light IS that colour.
			//
			// A true scattering integral would gather light along the ray.
			// This gathers it from the one place it is already known -- the
			// pixel behind the fog, which is the wall, its glow, any sweep
			// band crossing it, everything. So a red wall glow bleeds into
			// the mist in front of it for one mix, no extra sampling.
			//
			// At pickup 0 the fog keeps its own colour exactly as before.
			vec3 fogCol = mix(slab.rgb, slab.rgb * (0.35 + 0.65 * frag.rgb), uFogSlabExtra.y);
			frag.rgb = mix(frag.rgb, fogCol, slab.a);
		}

		// [BB] THE BEAM ITSELF, SEEN IN THE AIR.
		//
		// Last, and after the fog on purpose. Everything before this point
		// lights SURFACES; this is the beam as an object hanging in space,
		// and it should not be dimmed by mist it is in front of.
		//
		// It is also the last thing written before the frame is handed to
		// bloom, so a core burning past white blooms the way an emissive
		// thing should -- without a light, a sprite, or a quad.
		frag.rgb += BeamAirGlow(pixelpos.xyz);
	}
	else // simple 2D (uses the fog color to add a color overlay)
	{
		if ((uTextureMode & 0xffff) == 7)
		{
			float gray = grayscale(frag);
			vec4 cm = (uObjectColor + gray * (uAddColor - uObjectColor)) * 2.0;
			frag = vec4(clamp(cm.rgb, 0.0, 1.0), frag.a);
		}
			frag = frag * ProcessLight(material, vColor);
		frag.rgb = frag.rgb + uFogColor.rgb;
	}
	
	FragColor = frag;

#ifdef DITHERTRANS
	int index = (int(pixelpos.x) % 8) * 8 + int(pixelpos.y) % 8;
	const float DITHER_THRESHOLDS[64] =
	float[64](
		1.0 / 65.0, 33.0 / 65.0, 9.0 / 65.0, 41.0 / 65.0, 3.0 / 65.0, 35.0 / 65.0, 11.0 / 65.0, 43.0 / 65.0,
		49.0 / 65.0, 17.0 / 65.0, 57.0 / 65.0, 25.0 / 65.0, 51.0 / 65.0, 19.0 / 65.0, 59.0 / 65.0, 27.0 / 65.0,
		13.0 / 65.0, 45.0 / 65.0, 5.0 / 65.0, 37.0 / 65.0, 15.0 / 65.0, 47.0 / 65.0, 7.0 / 65.0, 39.0 / 65.0,
		61.0 / 65.0, 29.0 / 65.0, 53.0 / 65.0, 21.0 / 65.0, 63.0 / 65.0, 31.0 / 65.0, 55.0 / 65.0, 23.0 / 65.0,
		4.0 / 65.0, 36.0 / 65.0, 12.0 / 65.0, 44.0 / 65.0, 2.0 / 65.0, 34.0 / 65.0, 10.0 / 65.0, 42.0 / 65.0,
		52.0 / 65.0, 20.0 / 65.0, 60.0 / 65.0, 28.0 / 65.0, 50.0 / 65.0, 18.0 / 65.0, 58.0 / 65.0, 26.0 / 65.0,
		16.0 / 65.0, 48.0 / 65.0, 8.0 / 65.0, 40.0 / 65.0, 14.0 / 65.0, 46.0 / 65.0, 6.0 / 65.0, 38.0 / 65.0,
		64.0 / 65.0, 32.0 / 65.0, 56.0 / 65.0, 24.0 / 65.0, 62.0 / 65.0, 30.0 / 65.0, 54.0 / 65.0, 22.0 /65.0
	);

	vec3 fragHSV = rgb2hsv(FragColor.rgb);
	float brightness = clamp(1.5*fragHSV.z, 0.1, 1.0);
	if (DITHER_THRESHOLDS[index] < brightness) discard;
	else FragColor *= 0.5;
#endif

#ifdef GBUFFER_PASS
	FragFog = vec4(AmbientOcclusionColor(), 1.0);
	FragNormal = vec4(vEyeNormal.xyz * 0.5 + 0.5, 1.0);
#endif
}
