/*
** model.h
**
** General model handling code
**
**---------------------------------------------------------------------------
**
** Copyright 2013-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#pragma once

#include <stdint.h>
#include "textureid.h"
#include "i_modelvertexbuffer.h"
#include "matrix.h"
#include "palettecontainer.h"
#include "TRS.h"
#include "tarray.h"
#include "name.h"
#include "fs_files.h"

#include "bonecomponents.h"

class DBoneComponents;
class FModelRenderer;
class FGameTexture;
class IModelVertexBuffer;
class FModel;
class PClass;
class AActor;
struct FSpriteModelFrame;
struct FLevelLocals;

FTextureID LoadSkin(const char* path, const char* fn);
void FlushModels();


extern TDeletingArray<FModel*> Models;
extern TArray<FSpriteModelFrame> SpriteModelFrames;
extern TMap<const PClass*, FSpriteModelFrame> BaseSpriteModelFrames;

#define MD3_MAX_SURFACES	64	// [XR] was 32; the Slayer body ships 64 mesh parts and every one must be addressable by SurfaceSkin
#define MIN_MODELS	4

struct FSpriteModelFrame
{
	uint8_t modelsAmount = 0;
	TArray<int> modelIDs;
	TArray<FTextureID> skinIDs;
	TArray<FTextureID> surfaceskinIDs;
	TArray<int> modelframes;
	TArray<int> animationIDs;
	float xscale, yscale, zscale;
	// [BB] Added zoffset, rotation parameters and flags.
	// Added xoffset, yoffset
	float xoffset, yoffset, zoffset;

	// [BB] THE POINT THE MODEL TURNS ABOUT, IN ITS OWN SPACE.
	//
	// Offset above cannot express this, and the difference is not a nicety.
	// Offset is applied AFTER the rotations (ObjectToWorldMatrix runs step 4 then
	// step 5, so on the vertex it lands second), which makes it a rigid
	// displacement in the parent frame: it moves the model without moving the
	// point the model SPINS about. A mesh whose own origin is not where it ought
	// to turn from therefore ORBITS that origin instead of rotating in place, and
	// no value of Offset shrinks that orbit -- it only moves the whole circle
	// somewhere else.
	//
	// This is subtracted BEFORE the rotations instead, which is the ordinary
	// v' = R * (v - p). It is the only way to say "turn about HERE" for a mesh
	// that was not authored centred on the point it should turn from.
	//
	// Zero by default, so every existing model is untouched.
	//
	// Wanted independently by three things already, which is why it is a MODELDEF
	// field and not a fix inside one caller: a grenade whose mesh sits 5.85 units
	// off its own origin and swings rather than spins when thrown; a hand model
	// whose palm is nowhere near its origin; and anything wearing a borrowed model
	// on a holster bracket or a hardpoint mount, where the mount rotates and the
	// borrowed mesh was authored for a different anchor entirely.
	float pivotx, pivoty, pivotz;

	float xrotate, yrotate, zrotate;
	float rotationCenterX, rotationCenterY, rotationCenterZ;
	float rotationSpeed;
	float viewModelFOV;
private:
	unsigned int flags;
public:
	const void* type;	// used for hashing, must point to something usable as identifier for the model's owner.
	short sprite;
	short frame;
	int hashnext;
	float angleoffset;

	// RS FORK -- MOD-OWNED PLACEMENT.
	//
	// Names a CVAR prefix. The six placement values are then read live from
	// <prefix>_ofs_x/_ofs_y/_ofs_z and <prefix>_yaw/_pitch/_roll.
	//
	// The point is WHERE those CVARs live: in the mod's own CVARINFO, with the
	// mod's own MENUDEF page, so adding a tunable weapon needs nothing in the
	// engine and does not put a per-weapon slider in the engine's option tree.
	FName placementCVars = NAME_None;
	// added pithoffset, rolloffset.
	float pitchoffset, rolloffset; // I don't want to bother with type transformations, so I made this variables float.

	// RS FORK -- HAND-FRAME ORIENTATION, the bakeable twin of vr_hand_*.
	//
	// angleoffset/pitchoffset/rolloffset above are applied as one intrinsic
	// triple that orients the model itself. These three are applied AFTER
	// that, in the frame the oriented model leaves behind, and are summed
	// with the live vr_hand_yaw/_pitch/_roll sliders -- the same position,
	// the same order, the same sign.
	//
	// That is the whole point of them existing separately. A value dialled in
	// on a slider can be written into the matching keyword here and mean
	// EXACTLY the same thing, which is what makes a tuning pass permanent.
	// Summing the sliders into pitchoffset instead cannot do that: a model
	// carrying a 90 degree pitchoffset puts that rotation between the yaw and
	// the roll, and a 90 degree turn about Z lands the roll axis on top of the
	// yaw axis -- so both sliders drive one rotation and neither drives the
	// other. Applied here, past the baked pitch, the three stay orthogonal.
	float handangleoffset = 0.f, handpitchoffset = 0.f, handrolloffset = 0.f;
	bool isVoxel;
	unsigned int getFlags(class DActorModelData * defs) const;

	// RS FORK -- read straight off the MODELDEF flags, with no per-actor
	// override applied. The draw path needs this before it has resolved an
	// actor's model data, and no actor overrides it anyway.
	bool ignoresSkinAlpha() const { return !!(flags & (1 << 18)); }
	friend void InitModels();
	friend void ParseModelDefLump(int Lump);

	VSMatrix ObjectToWorldMatrix(AActor * actor, float x, float y, float z, double ticFrac);
	// bodyPivotZ: height above the actor's origin to turn about, in map units.
	// Zero keeps the historical behaviour of turning about the origin itself --
	// which for anything standing on a floor is the point between its feet, so a
	// held object swings through an arc instead of turning in place. Only the
	// held-voxel path passes anything else.
	// followBodyMode/followBodyOfs come from the ACTOR, not from MODELDEF, so
	// they arrive as arguments rather than as flags -- see AActor::FollowBodyMode.
	// Defaulted off: every caller written before this existed is unaffected.
	VSMatrix ObjectToWorldMatrix(FLevelLocals *Level, DVector3 translation, DRotator rotation, DVector2 scaling, unsigned int flags, double tic, float bodyPivotZ = 0.f, int followBodyMode = 0, DVector3 followBodyOfs = DVector3(0, 0, 0));
};


enum ModelRendererType
{
	GLModelRendererType,
	SWModelRendererType,
	PolyModelRendererType,
	NumModelRendererTypes
};

enum EFrameError
{
	FErr_NotFound = -1,
	FErr_Voxel = -2,
	FErr_Singleframe = -3
};

class FModel
{
public:
	enum LoadState
	{
		NONE = 0,
		LOADING = 1,
		READY = 2
	};

	FModel();
	virtual ~FModel();

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) = 0;

	virtual int FindFrame(const char * name, bool nodefault = false) = 0;

	virtual int NumJoints() { return 0; }
	virtual int FindJoint(FName name) { return -1; }

	virtual int GetJointParent(int joint) { return -1; }
	virtual FName GetJointName(int joint) { return NAME_None; }
	virtual FQuaternion GetJointRotation(int joint) { return FQuaternion(0.0f,0.0f,0.0f,1.0f); }
	virtual FVector3 GetJointPosition(int joint) { return FVector3(0.0f,0.0f,0.0f); }
	virtual TRS GetJointBaseTRS(int joint) { return {}; }
	virtual TRS GetJointPose(int joint, int frame) { return {}; }
	virtual int NumFrames() { return -1; }

	virtual void GetJointChildren(int joint, TArray<int> &out) {}

	virtual void GetRootJoints(TArray<int> &out) {}

	// [RL0] these are used for decoupled iqm animations
	virtual int FindFirstFrame(FName name) { return FErr_NotFound; }
	virtual int FindLastFrame(FName name) { return FErr_NotFound; }
	virtual double FindFramerate(FName name) { return FErr_NotFound; }

	virtual void RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition) = 0;
	virtual void BuildVertexBuffer(FModelRenderer *renderer) = 0;
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) = 0;
	virtual float getAspectFactor(float vscale) { return 1.f; }
	virtual const TArray<TRS>* AttachAnimationData() { return nullptr; };

	virtual ModelAnimFrame PrecalculateFrame(const ModelAnimFrame &from, const ModelAnimFrameInterp &to, float inter, const TArray<TRS>* animationData) { return nullptr; };

	virtual const TArray<VSMatrix>* CalculateBones(const ModelAnimFrame &from, const ModelAnimFrameInterp &to, float inter, const TArray<TRS>* animationData, TArray<BoneOverride> *in, BoneInfo *out, double time) { return nullptr; };
	virtual const TArray<VSMatrix>* CalculateBonesOnlyOffsets(TArray<BoneOverride> *in, BoneInfo *out, double time) { return nullptr; };

	virtual const TArray<VSMatrix>* GetBasePose() {return nullptr;}

	// [XR] Joint introspection under the names the DXR arm-IK was written against
	// (playsim/vr_armik.cpp): thin readers over the joint API above. GetJointCount()==0 on a
	// non-IQM model is itself the "not an IQM" signal, so callers need no RTTI/dynamic_cast.
	int  GetJointCount() { return NumJoints(); }
	int  FindJointByName(FName name) { return FindJoint(name); }
	// Case-INSENSITIVE joint-name lookup so authored names resolve regardless of the case the
	// modeler used. Base no-op -> -1 on any non-IQM model, same pattern as FindJoint above.
	virtual int FindJointByNameCI(FName name) { return -1; }
	// RAW per-joint local bind TRS exactly as read off disk -- callers compose these themselves;
	// nothing here is skinning-space (no swapYZ, no inversebaseframe).
	bool GetJointBindTRS(int jointIndex, TRS& out)
	{
		if (jointIndex < 0 || jointIndex >= NumJoints()) return false;
		out = GetJointBaseTRS(jointIndex);
		return true;
	}
	// Parent-resolved MODEL-LOCAL bind position of a joint -- the translation column of
	// baseframe[jointIndex] (VSMatrix is column-major; translation at [12/13/14]).
	bool GetJointBaseframePos(int jointIndex, FVector3& out)
	{
		const TArray<VSMatrix>* bf = GetBasePose();
		if (bf == nullptr || jointIndex < 0 || (unsigned)jointIndex >= bf->Size()) return false;
		const auto* m = (*bf)[jointIndex].get();
		out.X = (float)m[12];
		out.Y = (float)m[13];
		out.Z = (float)m[14];
		return true;
	}

	// Largest |X|/|Y|/|Z| across the model's own raw local-space vertices,
	// tracked independently per axis (not necessarily from the same vertex --
	// a conservative bounding proxy, not a tight AABB). Unscaled by the
	// MODELDEF's own Scale block; the caller (GetModelBoundsHint) applies
	// that the same way GetModelWorldOffset applies actor scale to an
	// offset -- this just answers the one thing script has no other way to
	// see: how big the raw mesh actually is. False/zero for any format that
	// does not override this; a holster falling back to a flat guess for an
	// unmeasured model is no worse off than it is today.
	virtual bool GetLocalExtent(float *outMaxAbsX, float *outMaxAbsY, float *outMaxAbsZ) { return false; }

	void SetVertexBuffer(int type, IModelVertexBuffer *buffer) { mVBuf[type] = buffer; }
	IModelVertexBuffer *GetVertexBuffer(int type) const { return mVBuf[type]; }
	void DestroyVertexBuffer();
	LoadState GetLoadState() const { return loadState; }
	void SetLoadState(LoadState state) { loadState = state; }
	virtual void LoadGeometry(FileSys::FileData* lumpData);
	int GetLumpNum() const { return mLumpNum; }

	bool hasSurfaces = false;

	FString mFileName;
	std::pair<FString, FString> mFilePath;

	FSpriteModelFrame *baseFrame;
private:
	IModelVertexBuffer *mVBuf[NumModelRendererTypes];
	LoadState loadState = NONE;
protected:
	int mLumpNum = -1;
};

int ModelFrameHash(FSpriteModelFrame* smf);
unsigned FindModel(const char* path, const char* modelfile, bool silent = false);
