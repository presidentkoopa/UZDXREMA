#pragma once

#include "matrix.h"

struct HWDrawInfo;

enum class ELightBlendMode : uint8_t
{
	CLAMP = 0,
	CLAMP_COLOR = 1,
	NOCLAMP = 2,

	DEFAULT = CLAMP,
};

struct HWViewpointUniforms
{
	VSMatrix mProjectionMatrix;
	VSMatrix mViewMatrix;
	VSMatrix mNormalViewMatrix;
	FVector4 mCameraPos;
	FVector4 mClipLine;

	float mGlobVis = 1.f;
	int mPalLightLevels = 0;
	int mViewHeight = 0;
	float mClipHeight = 0.f;
	float mClipHeightDirection = 0.f;
	int mShadowmapFilter = 1;

	int mLightBlendMode = 0;
	int mPadding0 = 0;

	// [BB] Glow wave. Scene-global -- one wave for the frame, four channels
	// reading it at their own phase -- so it lives here rather than in
	// StreamData. StreamData is the per-draw block and its size divides
	// 64KB into MAX_STREAM_DATA draws; spending 64 bytes of it on values
	// that are identical in every draw would cost batching in every frame of
	// the game forever, to say the same thing thirty-four times.
	//
	// xyzw packing, and it has to match the ViewpointData struct in
	// vk_shader.cpp and the ViewpointUBO block in gl_shader.cpp exactly:
	//
	//   mGlowWave        wavelength, speed, sharpness, shape
	//   mGlowWaveDepth   reach swing, brightness swing, colour slide, detune
	//   mGlowWavePhase   wall top, wall bottom, flat floor, flat ceiling
	//   mGlowWaveOrigin  x, y, z, per-room seed scatter
	//
	// Wavelength 0 means the whole feature is off and every glow block falls
	// through to exactly the arithmetic it did before.
	FVector4 mGlowWave = { 0.f, 0.f, 1.f, 1.f };
	FVector4 mGlowWaveDepth = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mGlowWavePhase = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mGlowWaveOrigin = { 0.f, 0.f, 0.f, 0.f };

	// [BB] Darkness as a shader term. Same packing rules as the wave above,
	// and it has to match both GLSL declarations exactly.
	//
	//   mDarkness       mode, adjust, min light, pre-gain
	//   mDarkness2      post-gain, distance depth, distance range, unused
	//   mDarkness3      height depth, height reference, height range, unused
	//
	// Mode 0 means off and every fragment keeps the light it already had.
	FVector4 mDarkness = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mDarkness2 = { 0.f, 0.f, 2048.f, 0.f };
	FVector4 mDarkness3 = { 0.f, 0.f, 256.f, 0.f };

	// [BB] Fog slab -- fog with a top. Same packing discipline as above.
	//
	//   mFogSlab       top Z, density, soft edge, scatter
	//   mFogSlabColor  r, g, b, wake strength
	//   mFogSlabWake   x, y, z, wake radius
	//
	// Density 0 means off and the fragment shader falls straight through.
	FVector4 mFogSlab = { 0.f, 0.f, 24.f, 0.f };
	FVector4 mFogSlabColor = { 1.f, 0.19f, 0.09f, 0.f };
	FVector4 mFogSlabWake = { 0.f, 0.f, 0.f, 0.f };

	// [BB] The flashlight cone, in WORLD space, so the fog slab can be lit by
	// it. The volumetric beam itself is a postprocess pass working in VIEW
	// space, and its uniforms are not reachable from main.fp -- so rather than
	// plumb a second copy of the beam through the postprocess chain, the three
	// numbers the scatter term needs are handed to the fragment shader here.
	//
	//   mFogBeamPos  xyz world position, w beam length (0 = no beam)
	//   mFogBeamDir  xyz world direction, w cos(inner angle)
	//   mFogBeamCol  rgb colour, w cos(outer angle)
	FVector4 mFogBeamPos = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mFogBeamDir = { 0.f, 0.f, 1.f, 1.f };
	FVector4 mFogBeamCol = { 1.f, 1.f, 1.f, 1.f };

	// [BB] mFogSlabExtra: x wake strength, y glow pickup, zw spare.
	//
	// POSITION IS LOAD-BEARING. This sits between mFogBeamCol and mSweepFill
	// because that is where both GLSL copies put it, and a uniform block is
	// matched by OFFSET, not by name. It was declared at the end of this
	// struct once: every field after uFogBeamCol then read the one before it,
	// so beam count came from mBeamFX, resolved as zero, and no beam ever drew
	// -- silently, with no error anywhere, because a mismatched block is still
	// a valid block.
	FVector4 mFogSlabExtra = { 0.f, 0.f, 0.f, 0.f };

	// [BB] Sweep fill -- the pattern drawn INSIDE a band.
	//
	//   mSweepFill    spacing U, spacing V, line width, line softness
	//   mSweepFill2   rotation (deg), drift, major every N, jitter
	//   mSweepFill3   gradient amount, gradient axis, flicker, major boost
	//   mSweepFillCol rgb line colour, w gap fill amount
	//
	// The band's OWN colour is the field; this colour is the lines. Gap 0
	// means only the lines are lit and the room shows between them, which is
	// what reads as lasers rather than as a lit pane.
	//
	// Shared rather than per band, deliberately -- per band would mean another
	// vec4[8] in StreamData and a permanent draw-batching cost. Only the fill
	// MODE is per band, packed into the draw mode's spare bits.
	FVector4 mSweepFill = { 64.f, 64.f, 3.f, 1.5f };
	FVector4 mSweepFill2 = { 0.f, 0.f, 0.f, 0.f };
	FVector4 mSweepFill3 = { 0.f, 0.f, 0.f, 2.f };
	FVector4 mSweepFillCol = { 1.f, 1.f, 1.f, 0.f };

	// [BB] Beams -- real ones. A segment lit per pixel by distance, so it is
	// continuous at any length, wraps every surface, and lights what it passes.
	//
	//   mBeamA[i]    xyz start, w core thickness
	//   mBeamB[i]    xyz end,   w softness (how far the halo reaches)
	//   mBeamCol[i]  rgb colour, w intensity
	//   mBeamParams  x count, y halo strength, z fog scatter, w spare
	//
	// In the viewpoint block rather than StreamData: these are scene-global,
	// and StreamData's size divides 64KB into MAX_STREAM_DATA draws.
	FVector4 mBeamA[8];
	FVector4 mBeamB[8];
	FVector4 mBeamCol[8];
	FVector4 mBeamParams = { 0.f, 0.35f, 1.f, 0.f };

	// [BB] What happens ALONG the beam, which is what stops it being a stick.
	//
	//   x  scroll speed   energy travelling from muzzle to impact
	//   y  scroll depth   how much it modulates; 0 = a smooth beam
	//   z  taper          how much thinner at the muzzle end than the impact
	//   w  impact flare   brightness boost at the far end, where it lands
	//
	// All four ride the position ALONG the segment, which the closest-approach
	// solve already produces -- so none of them costs a second pass over the
	// beam, only arithmetic on a number that was already in hand.
	FVector4 mBeamFX = { 0.f, 0.f, 0.f, 0.f };

	// [BB] The fog slab's SURFACE, animated.
	//
	//   x amplitude (world units the top rises and falls)
	//   y wavelength
	//   z speed
	//   w cross-swell ratio -- a second wave at an angle to the first, so the
	//     surface rolls rather than corrugating in one direction
	//
	// Amplitude 0 leaves the top perfectly flat, exactly as before.
	//
	// Appended LAST here and last in both GLSL copies, in one change. See
	// mFogSlabExtra above for what happens when those two facts stop being
	// true.
	FVector4 mFogSurf = { 0.f, 256.f, 1.f, 0.6f };


	void CalcDependencies()
	{
		mNormalViewMatrix.computeNormalMatrix(mViewMatrix);
	}
};

static_assert((sizeof(HWViewpointUniforms) % 16) == 0, "HWViewpointUniforms must remain 16-byte aligned for std140 array stride.");



