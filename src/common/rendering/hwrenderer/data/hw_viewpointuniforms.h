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

	void CalcDependencies()
	{
		mNormalViewMatrix.computeNormalMatrix(mViewMatrix);
	}
};

static_assert((sizeof(HWViewpointUniforms) % 16) == 0, "HWViewpointUniforms must remain 16-byte aligned for std140 array stride.");



