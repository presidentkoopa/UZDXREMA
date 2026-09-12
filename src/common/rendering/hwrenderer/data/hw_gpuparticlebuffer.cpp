/*
** hw_gpuparticlebuffer.cpp
**
** [GPUPARTICLES] The GPU half of the stateless particle ring. See the header.
**
**---------------------------------------------------------------------------
**
** Copyright 2026 UZDXREMA
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

#include <cstring>
#include <vector>
#include "hw_gpuparticlebuffer.h"
#include "shaderuniforms.h"
#include "v_video.h"
#include "hw_cvars.h"
#include "printf.h"
#include "i_time.h"

GpuParticleBuffer::GpuParticleBuffer(unsigned ringSize) : mRingSize(ringSize != 0 ? ringSize : (unsigned)GpuParticleRingCapacity())
{
	const size_t recordBytes = (size_t)mRingSize * RECORD_BYTES;

	// The ring. Storage buffer, persistently mapped, set 1 binding 5 on Vulkan
	// (vk_descriptorset.cpp). Same creation path as the bone buffer.
	mBuffer = screen->CreateDataBuffer(GPUPARTICLE_BINDINGPOINT, true, false);
	mBuffer->SetData(recordBytes, nullptr, BufferUsageType::Persistent);

	// A persistent allocation is not guaranteed to be zeroed, and a garbage
	// record with a positive life would draw. The first sync of a level only
	// uploads what was written, so the GPU copy has to start empty.
	mBuffer->Map();
	if (mBuffer->Memory() != nullptr)
		memset(mBuffer->Memory(), 0, recordBytes);
	mBuffer->Unmap();

	// The static quad buffer: six vertices per ring slot. Each carries the
	// slot index split into two 16-bit halves plus the corner number, in the
	// EXISTING integer attribute at location 8 (VATTR_BONESELECTOR,
	// VFmt_UShort4_UInt) -- no new attribute enum, no new vertex format, so
	// none of the three backends' positional format tables change.
	struct QuadVertex
	{
		uint16_t indexLow;
		uint16_t indexHigh;
		uint16_t corner;
		uint16_t pad;
	};

	const size_t vertexCount = (size_t)mRingSize * VERTICES_PER_RECORD;
	std::vector<QuadVertex> verts(vertexCount);
	for (unsigned i = 0; i < mRingSize; i++)
	{
		for (unsigned c = 0; c < VERTICES_PER_RECORD; c++)
		{
			QuadVertex &v = verts[(size_t)i * VERTICES_PER_RECORD + c];
			v.indexLow = (uint16_t)(i & 0xffff);
			v.indexHigh = (uint16_t)((i >> 16) & 0xffff);
			v.corner = (uint16_t)c;
			v.pad = 0;
		}
	}

	mQuads = screen->CreateVertexBuffer();
	static const FVertexBufferAttribute format[] = {
		{ 0, VATTR_BONESELECTOR, VFmt_UShort4_UInt, 0 },
	};
	mQuads->SetFormat(1, 1, sizeof(QuadVertex), format);
	mQuads->SetData(vertexCount * sizeof(QuadVertex), verts.data(), BufferUsageType::Static);

	Printf("GpuParticles: buffer created -- ring %u records x %u B = %llu bytes, quads %llu vertices x %u B = %llu bytes\n",
		mRingSize, RECORD_BYTES, (unsigned long long)recordBytes,
		(unsigned long long)vertexCount, (unsigned)sizeof(QuadVertex),
		(unsigned long long)(vertexCount * sizeof(QuadVertex)));
}

GpuParticleBuffer::~GpuParticleBuffer()
{
	delete mQuads;
	delete mBuffer;
}

void GpuParticleBuffer::Upload(const void *records, unsigned first, unsigned count)
{
	if (count == 0) return;
	memcpy((uint8_t *)mBuffer->Memory() + (size_t)first * RECORD_BYTES,
		(const uint8_t *)records + (size_t)first * RECORD_BYTES,
		(size_t)count * RECORD_BYTES);
	mUploadedSinceReport += count;
}

void GpuParticleBuffer::Sync(const void *records, unsigned recordCount, uint64_t serial, uint64_t written)
{
	// A level that never spawned a particle has no CPU ring. Its draw is
	// skipped (Written == 0), so whatever the GPU still holds is never read.
	if (mBuffer == nullptr || records == nullptr || recordCount == 0) return;
	if (serial == mSyncedSerial && written == mSyncedWritten) return;

	unsigned size = recordCount;
	if (size != mRingSize)
	{
		// Both sides size from GpuParticleRingCapacity(), which latches once
		// per process, so this should be unreachable. Clamp rather than write
		// past the end, and say so once.
		if (!mWarnedSizeMismatch)
		{
			Printf(TEXTCOLOR_ORANGE "GpuParticles: CPU ring %u != GPU ring %u, clamping\n", recordCount, mRingSize);
			mWarnedSizeMismatch = true;
		}
		if (size > mRingSize) size = mRingSize;
	}

	mBuffer->Map();
	if (mBuffer->Memory() == nullptr)
	{
		mBuffer->Unmap();
		return;
	}

	const bool full = serial != mSyncedSerial || written < mSyncedWritten || written - mSyncedWritten >= size;
	if (full)
	{
		Upload(records, 0, size);
		mFullSinceReport++;
	}
	else
	{
		const unsigned from = (unsigned)(mSyncedWritten % size);
		const unsigned to = (unsigned)(written % size);
		if (from < to)
		{
			Upload(records, from, to - from);
		}
		else
		{
			Upload(records, from, size - from);
			Upload(records, 0, to);
		}
		mSpanSinceReport++;
	}
	mBuffer->Unmap();

	// One line per level, the first time that level's particles reach the GPU.
	if (written > 0 && serial != mLoggedSerial)
	{
		mLoggedSerial = serial;
		Printf("GpuParticles: level ring (serial %llu) reached the GPU -- %llu records written, %s upload, shader %s\n",
			(unsigned long long)serial, (unsigned long long)written, full ? "full" : "span",
			ShaderFailed ? "FAILED" : (ShaderReady ? "ready" : "not ready"));
	}

	mSyncedSerial = serial;
	mSyncedWritten = written;
}

void GpuParticleBuffer::DebugReport(uint64_t written)
{
	if (!r_gpuparticles_debug) return;

	const uint64_t now = I_msTime();
	if (mLastReportMs != 0 && now - mLastReportMs < 2000) return;

	const uint64_t spawned = written >= mWrittenAtReport ? written - mWrittenAtReport : written;
	Printf("GpuParticles [debug]: written %llu (+%llu), uploaded %llu records (%u full, %u span), %u draws, shader %s, r_gpuparticles %d\n",
		(unsigned long long)written, (unsigned long long)spawned,
		(unsigned long long)mUploadedSinceReport, mFullSinceReport, mSpanSinceReport, mDrawsSinceReport,
		ShaderFailed ? "FAILED" : (ShaderReady ? "ready" : "not ready"), (int)*r_gpuparticles);

	mLastReportMs = now;
	mWrittenAtReport = written;
	mUploadedSinceReport = 0;
	mFullSinceReport = 0;
	mSpanSinceReport = 0;
	mDrawsSinceReport = 0;
}
