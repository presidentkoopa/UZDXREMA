#pragma once

#include "r_utility.h"
#include "matrix.h"

class DFrameBuffer;
class FCanvasTexture;
class FCanvas;
class FGameTexture;

enum
{
	VR_MONO = 0,
	VR_GREENMAGENTA = 1,
	VR_REDCYAN = 2,
	VR_SIDEBYSIDEFULL = 3,
	VR_SIDEBYSIDESQUISHED = 4,
	VR_LEFTEYEVIEW = 5,
	VR_RIGHTEYEVIEW = 6,
	VR_QUADSTEREO = 7,
	VR_SIDEBYSIDELETTERBOX = 8,
	VR_AMBERBLUE = 9,
	VR_OPENVR = 10,
	VR_TOPBOTTOM = 11,
	VR_ROWINTERLEAVED = 12,
	VR_COLUMNINTERLEAVED = 13,
	VR_CHECKERINTERLEAVED = 14,
	VR_OPENXR_MOBILE = 15,

	VR_MAINHAND = 0,
	VR_OFFHAND = 1
};

struct HWDrawInfo;

struct VRHudSurface
{
	// Shared texture-backed HUD surface for VR, and later canvas/model texture reuse.
	VRHudSurface();
	~VRHudSurface();

	void Clear();
	void EnsureSize(int width, int height);
	void BeginUpdate();
	void EndUpdate();
	void MarkDirty();
	bool IsValid() const { return Texture != nullptr; }
	bool HasGameTexture() const { return GameTexture != nullptr; }
	int GetWidth() const { return Texture != nullptr ? Texture->GetWidth() : 0; }
	int GetHeight() const { return Texture != nullptr ? Texture->GetHeight() : 0; }
	FCanvasTexture* GetTexture() const { return Texture; }
	FGameTexture* GetGameTexture() const { return GameTexture; }
	FCanvas* GetCanvas() const { return Canvas; }

private:
	FCanvasTexture* Texture = nullptr;
	FGameTexture* GameTexture = nullptr;
	FCanvas* Canvas = nullptr;
};

struct VREyeInfo
{
	float mShiftFactor;
	float mScaleFactor;

	VREyeInfo() {}
	VREyeInfo(float shiftFactor, float scaleFactor);
	virtual ~VREyeInfo() {}

	virtual VSMatrix GetProjection(float fov, float aspectRatio, float fovRatio, bool iso_ortho) const;
	virtual DVector3 GetViewShift(FRenderViewpoint& vp) const;
	virtual void SetUp() const { m_isActive = true; }
	virtual void TearDown() const { m_isActive = false; }
	virtual void AdjustHud() const {}
	virtual void AdjustBlend(HWDrawInfo* di) const {}
	bool isActive() const { return m_isActive; }

private:
	mutable bool m_isActive;
	float getShift() const;

};

struct VRMode
{
	int mEyeCount;
	float mHorizontalViewportScale;
	float mVerticalViewportScale;
	float mWeaponProjectionScale;
	VREyeInfo* mEyes[2];

	VRMode(int eyeCount, float horizontalViewportScale, 
		float verticalViewportScalem, float weaponProjectionScale, VREyeInfo eyes[2]);
	virtual ~VRMode() {}

	static const VRMode *GetVRMode(bool toscreen = true);
	static const VRMode *GetVRModeCached(bool toscreen = true);
	virtual void AdjustViewport(DFrameBuffer *fb) const;
	VSMatrix GetHUDSpriteProjection() const;

	/* hooks for setup and cleanup operations for each stereo mode */
	virtual void SetUp() const;
	virtual void TearDown() const {};

	virtual bool IsMono() const { return mEyeCount == 1; }
	virtual bool IsVR() const { return false; }
	virtual void AdjustPlayerSprites(FRenderState &state, int hand = 0) const {};
	virtual void UnAdjustPlayerSprites(FRenderState &state) const {};
	virtual void AdjustCrossHair() const {}
	virtual void UnAdjustCrossHair() const {}

	virtual void SetupOverlay() {}
	virtual void UpdateOverlaySettings() const {}
	virtual void DrawControllerModels(HWDrawInfo* di, FRenderState& state) const {}
	virtual void DrawMountedHud(HWDrawInfo* di, FRenderState& state) const {}
	
	virtual void Present() const;
	virtual void PollXREvents() const {}
	virtual bool BeginXRFrame() const { return true; }
	virtual bool AcquireXRSwapchain() const { return true; }
	virtual bool SubmitFrame() const { return true; }

	virtual bool GetHandTransform(int hand, VSMatrix* out) const { return false; }
	virtual bool GetWeaponTransform(VSMatrix* out, int hand = 0) const;
	virtual bool RenderPlayerSpritesInScene() const;
	virtual bool GetTeleportLocation(DVector3 &out) const { return false; }
	virtual bool IsInitialized() const { return true; }
	virtual void Vibrate(float duration, int channel, float intensity) const { }
};

void VR_HapticEvent(const char* event, int position, int intensity, float angle, float yHeight );
void QzDoom_GetScreenRes(uint32_t *width, uint32_t *height);

extern bool weaponStabilised;

VRHudSurface& GetVRHudSurface();
void VR_EnsureHudSurface(int width, int height);
bool VR_ShouldDrawMountedHud();
bool VR_GetMountedHudTransform(VSMatrix& out);
