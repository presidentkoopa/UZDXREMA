/*
** model_md3.h
**
**
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
#include "model.h"

#define MD3_MAGIC			0x33504449

class FMD3Model : public FModel
{
	struct MD3Tag
	{
		// Currently I have no use for this
	};

	struct MD3TexCoord
	{
		float s,t;
	};

	struct MD3Vertex
	{
		float x,y,z;
		float nx,ny,nz;
	};

	struct MD3Triangle
	{
		int VertIndex[3];
	};

	struct MD3Surface
	{
		unsigned numVertices;
		unsigned numTriangles;
		unsigned numSkins;

		TArray<FTextureID> Skins;
		TArray<MD3Triangle> Tris;
		TArray<MD3TexCoord> Texcoords;
		TArray<MD3Vertex> Vertices;

		unsigned int vindex = UINT_MAX;	// contains numframes arrays of vertices
		unsigned int iindex = UINT_MAX;

		void UnloadGeometry()
		{
			Tris.Reset();
			Vertices.Reset();
			Texcoords.Reset();
		}
	};

	struct MD3Frame
	{
		// The bounding box information is of no use in the Doom engine
		// That will still be done with the actor's size information.
		char Name[16];
		float origin[3];
	};

	int numTags;

	TArray<MD3Frame> Frames;
	TArray<MD3Surface> Surfaces;

	// Largest |X|/|Y|/|Z| across every surface's vertices, every frame,
	// computed once in LoadGeometry() while MD3Surface::Vertices still
	// exists. BuildVertexBuffer() calls surf->UnloadGeometry() right after
	// uploading to the GPU, which Resets those TArrays for good -- by the
	// time a script-side query could ask for the model's size, the raw
	// vertex data is long gone. Caching three floats here (same idea as
	// MD3Frame::origin already caching a per-frame summary instead of
	// keeping the frame's full geometry around) survives that free.
	float cachedMaxAbsX = 0.f, cachedMaxAbsY = 0.f, cachedMaxAbsZ = 0.f;
	bool hasCachedExtent = false;

public:
	FMD3Model() = default;

	virtual bool Load(const char * fn, int lumpnum, const char * buffer, int length) override;
	virtual int FindFrame(const char* name, bool nodefault) override;
	virtual void RenderFrame(FModelRenderer *renderer, FGameTexture * skin, int frame, int frame2, double inter, FTranslationID translation, const FTextureID* surfaceskinids, int boneStartPosition) override;
	void LoadGeometry();
	void LoadGeometry(FileSys::FileData* lumpData) override;
	void BuildVertexBuffer(FModelRenderer *renderer);
	virtual void AddSkins(uint8_t *hitlist, const FTextureID* surfaceskinids) override;
	bool GetLocalExtent(float* outMaxAbsX, float* outMaxAbsY, float* outMaxAbsZ) override;
};
