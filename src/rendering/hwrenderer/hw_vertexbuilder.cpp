// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2015-2018 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//



#include "g_levellocals.h"
#include "hw_vertexbuilder.h"
#include "flatvertices.h"
#include "earcut.hpp"
#include "v_video.h"

//=============================================================================
//
// Creates vertex meshes for sector planes
//
//=============================================================================

//=============================================================================
//
//
//
//=============================================================================

static void CreateVerticesForSubsector(subsector_t *sub, VertexContainer &gen, int qualifier)
{
	if (sub->numlines < 3) return;
	
	uint32_t startindex = gen.indices.Size();
	
	if ((sub->flags & SSECF_HOLE) && sub->numlines > 3)
	{
		// Hole filling "subsectors" are not necessarily convex so they require real triangulation.
		// These things are extremely rare so performance is secondary here.
		
		using Point = std::pair<double, double>;
		std::vector<std::vector<Point>> polygon;
		std::vector<Point> *curPoly;

		polygon.resize(1);
		curPoly = &polygon.back();
		curPoly->resize(sub->numlines);

		for (unsigned i = 0; i < sub->numlines; i++)
		{
			(*curPoly)[i] = { sub->firstline[i].v1->fX(), sub->firstline[i].v1->fY() };
		}
		auto indices = mapbox::earcut(polygon);
		for (auto vti : indices)
		{
			gen.AddIndexForVertex(sub->firstline[vti].v1, qualifier);
		}
	}
	else
	{
		// Fan from an interior point rather than from the first vertex.
		// Every vertex of a convex subsector lies on one of that subsector's own edges, so
		// a fan anchored on one of them carries no interior sample for the edge distance to
		// interpolate towards - a whole room would come out at distance 0 and glow flat.
		// The centre point supplies that sample. It costs two extra triangles per subsector.
		DVector2 center(0, 0);
		for (unsigned i = 0; i < sub->numlines; i++)
		{
			center += DVector2(sub->firstline[i].v1->fX(), sub->firstline[i].v1->fY());
		}
		center /= double(sub->numlines);
		int centerndx = gen.AddInteriorVertex(center);

		int firstndx = gen.AddVertex(sub->firstline[0].v1, qualifier);
		int previndx = firstndx;
		for (unsigned int k = 1; k < sub->numlines; k++)
		{
			auto ndx = gen.AddVertex(sub->firstline[k].v1, qualifier);
			gen.AddIndex(centerndx);
			gen.AddIndex(previndx);
			gen.AddIndex(ndx);
			previndx = ndx;
		}
		gen.AddIndex(centerndx);
		gen.AddIndex(previndx);
		gen.AddIndex(firstndx);
	}
}

//=============================================================================
//
//
//
//=============================================================================

static void TriangulateSection(FSection &sect, VertexContainer &gen, int qualifier)
{
	if (sect.segments.Size() < 3) return;
	
	// todo
}

//=============================================================================
//
//
//
//=============================================================================


static void CreateVerticesForSection(FSection &section, VertexContainer &gen, bool useSubsectors)
{
	section.vertexindex = gen.indices.Size();

	if (useSubsectors)
	{
		for (auto sub : section.subsectors)
		{
			CreateVerticesForSubsector(sub, gen, -1);
		}
	}
	else
	{
		TriangulateSection(section, gen, -1);
	}
	section.vertexcount = gen.indices.Size() - section.vertexindex;
}

//==========================================================================
//
// Creates the vertices for one plane in one subsector
//
//==========================================================================

static void CreateVerticesForSector(sector_t *sec, VertexContainer &gen)
{
	auto sections = sec->Level->sections.SectionsForSector(sec);
	for (auto &section :sections)
	{
		CreateVerticesForSection( section, gen, true);
	}
}


TArray<VertexContainer> BuildVertices(TArray<sector_t> &sectors)
{
	TArray<VertexContainer> verticesPerSector(sectors.Size(), true);
	for (unsigned i=0; i< sectors.Size(); i++)
	{
		CreateVerticesForSector(&sectors[i], verticesPerSector[i]);
	}
	return verticesPerSector;
}

//==========================================================================
//
// Creates the vertices for one plane in one subsector
//
//==========================================================================

//==========================================================================
//
// Find a 3D floor
//
//==========================================================================

static F3DFloor *Find3DFloor(sector_t* target, sector_t* model, int &ffloorIndex)
{
	for (unsigned i = 0; i < target->e->XFloor.ffloors.Size(); i++)
	{
		F3DFloor* ffloor = target->e->XFloor.ffloors[i];
		if (ffloor->model == model && !(ffloor->flags & FF_THISINSIDE))
		{
			ffloorIndex = i;
			return ffloor;
		}
	}
	ffloorIndex = -1;
	return NULL;
}

//==========================================================================
//
// Edge distance baking.
//
// The one primitive edge glow on flats rests on: for each point of a floor or
// ceiling, how far is it to the nearest boundary? A flat's boundaries are the
// linedefs of its own sector, so the candidate set is small and this is just a
// minimum over a handful of segments, done once here at map load.
//
// Two answers are stored per vertex, because both looks are wanted and the
// second one cannot be added later without rebuilding the map:
//   x - only boundaries that show a wall at this plane. Smoother, follows
//       architecture.
//   y - every sector boundary, including the invisible splits mappers use to
//       carve one room into several sectors. Those trace lines across open
//       floor, which is where the grid look comes from.
//
//==========================================================================

struct FEdgeBoundary
{
	DVector2 v1, v2;
	double minx, miny, maxx, maxy;
	bool visible;
};

// A one sided line always shows a wall. A two sided one only does where the two planes sit
// at different heights - that is what makes a step, a lip or a doorway something you can
// see. Heights are read as the map loads, so a lift or a door that moves later does not
// move the seam that was baked from it.
static bool LineShowsWall(line_t* ln, int plane)
{
	sector_t* front = ln->frontsector;
	sector_t* back = ln->backsector;
	if (front == nullptr || back == nullptr) return true;

	const secplane_t& fp = front->GetSecPlane(plane);
	const secplane_t& bp = back->GetSecPlane(plane);
	return fabs(fp.ZatPoint(ln->v1->fX(), ln->v1->fY()) - bp.ZatPoint(ln->v1->fX(), ln->v1->fY())) > EQUAL_EPSILON ||
		fabs(fp.ZatPoint(ln->v2->fX(), ln->v2->fY()) - bp.ZatPoint(ln->v2->fX(), ln->v2->fY())) > EQUAL_EPSILON;
}

static void AddBoundary(line_t* ln, int plane, TArray<FEdgeBoundary>& out, bool keepinvisible)
{
	FEdgeBoundary bd;
	bd.v1 = DVector2(ln->v1->fX(), ln->v1->fY());
	bd.v2 = DVector2(ln->v2->fX(), ln->v2->fY());
	if (bd.v1 == bd.v2) return;	// degenerate linedef, it bounds nothing

	bd.visible = LineShowsWall(ln, plane);
	if (!bd.visible && !keepinvisible) return;

	bd.minx = min(bd.v1.X, bd.v2.X);
	bd.maxx = max(bd.v1.X, bd.v2.X);
	bd.miny = min(bd.v1.Y, bd.v2.Y);
	bd.maxy = max(bd.v1.Y, bd.v2.Y);
	out.Push(bd);
}

static void BuildSectorBoundaries(sector_t* sec, int plane, TArray<FEdgeBoundary>& out)
{
	// A flat only exists inside its own sector, so its own linedefs are the whole answer for
	// "every sector boundary" - anything further out is on the far side of one of them.
	out.Clear();
	for (auto ln : sec->Lines) AddBoundary(ln, plane, out, true);

	// "Boundaries with a visible wall" needs one more step. A wall a few units past an
	// invisible split belongs to the neighbouring sector, not to this one, and the glow it
	// throws has no reason to stop dead at the split. Pull in one ring of those. Walls
	// reached across a visible boundary are left alone - that boundary already stops it.
	TArray<sector_t*> seen;
	for (auto ln : sec->Lines)
	{
		if (ln->frontsector == nullptr || ln->backsector == nullptr) continue;
		sector_t* other = ln->frontsector == sec ? ln->backsector : ln->frontsector;
		if (other == nullptr || other == sec) continue;
		if (LineShowsWall(ln, plane)) continue;
		if (seen.Find(other) < seen.Size()) continue;
		seen.Push(other);

		// Safety valve for maps that split one room into hundreds of pieces.
		if (out.Size() > 4096) break;
		for (auto ln2 : other->Lines) AddBoundary(ln2, plane, out, false);
	}
}

static double SquaredDistanceToSegment(const DVector2& p, const FEdgeBoundary& bd)
{
	double dx = bd.v2.X - bd.v1.X;
	double dy = bd.v2.Y - bd.v1.Y;
	double len2 = dx * dx + dy * dy;
	double t = ((p.X - bd.v1.X) * dx + (p.Y - bd.v1.Y) * dy) / len2;
	t = clamp(t, 0.0, 1.0);
	double ox = bd.v1.X + t * dx - p.X;
	double oy = bd.v1.Y + t * dy - p.Y;
	return ox * ox + oy * oy;
}

// Cheap reject: no point on the segment can be nearer than its bounding box is.
static double SquaredDistanceToBox(const DVector2& p, const FEdgeBoundary& bd)
{
	double dx = max(max(bd.minx - p.X, p.X - bd.maxx), 0.0);
	double dy = max(max(bd.miny - p.Y, p.Y - bd.maxy), 0.0);
	return dx * dx + dy * dy;
}

static void ComputeEdgeDistances(const TArray<FEdgeBoundary>& bounds, const TArray<DVector2>& points, TArray<FVector2>& out)
{
	out.Resize(points.Size());
	const double nolimit = double(FLATVERTEX_NO_EDGE) * double(FLATVERTEX_NO_EDGE);

	for (unsigned i = 0; i < points.Size(); i++)
	{
		double bestvisible = nolimit;
		double bestall = nolimit;
		for (auto& bd : bounds)
		{
			// bestall is never larger than bestvisible, so this rejects for both.
			if (SquaredDistanceToBox(points[i], bd) >= bestvisible) continue;
			double d = SquaredDistanceToSegment(points[i], bd);
			if (d < bestall) bestall = d;
			if (bd.visible && d < bestvisible) bestvisible = d;
		}
		out[i] = FVector2((float)sqrt(bestvisible), (float)sqrt(bestall));
	}
}

//==========================================================================
//
// Initialize a single vertex
//
//==========================================================================

static void SetFlatVertex(FFlatVertex& ffv, const DVector2& pos, const secplane_t& plane)
{
	ffv.x = (float)pos.X;
	ffv.y = (float)pos.Y;
	ffv.z = (float)plane.ZatPoint(pos.X, pos.Y);
	ffv.u = (float)pos.X / 64.f;
	ffv.v = -(float)pos.Y / 64.f;
	ffv.lindex = -1.0f;
	ffv.edgedist = ffv.edgedistall = FLATVERTEX_NO_EDGE;
}

static void SetFlatVertex(FFlatVertex& ffv, const DVector2& pos, const secplane_t& plane, float llu, float llv, int llindex)
{
	SetFlatVertex(ffv, pos, plane);
	ffv.lu = llu;
	ffv.lv = llv;
	ffv.lindex = (float)llindex;
}

//==========================================================================
//
// Creates the vertices for one plane in one subsector w/lightmap support.
// Sectors with lightmaps cannot share subsector vertices.
//
//==========================================================================

static int CreateIndexedSectorVerticesLM(FFlatVertexBuffer* fvb, sector_t* sec, const secplane_t& plane, int floor, int h, int lightmapIndex)
{
	int i, pos;
	float diff;

	auto& ibo_data = fvb->ibo_data;

	int rt = ibo_data.Size();
	if (sec->transdoor && floor) diff = -1.f;
	else diff = 0.f;

	// Allocate space. Every subsector gets one extra vertex in the middle so the edge
	// distance has an interior sample to interpolate towards, and is fanned from it.
	unsigned totalverts = 0, totaltris = 0;
	for (i = 0; i < sec->subsectorcount; i++)
	{
		unsigned n = sec->subsectors[i]->numlines;
		totalverts += n + 1;
		if (n >= 3) totaltris += n;
	}

	auto& vbo_shadowdata = fvb->vbo_shadowdata;
	int vi = vbo_shadowdata.Reserve(totalverts);
	int idx = ibo_data.Reserve(totaltris * 3);

	// Collect the positions first so the edge distances can be worked out in one pass.
	TArray<DVector2> positions(totalverts, true);
	for (i = 0, pos = 0; i < sec->subsectorcount; i++)
	{
		subsector_t* sub = sec->subsectors[i];
		DVector2 center(0, 0);
		for (unsigned int j = 0; j < sub->numlines; j++)
		{
			positions[pos + j] = DVector2(sub->firstline[j].v1->fX(), sub->firstline[j].v1->fY());
			center += positions[pos + j];
		}
		if (sub->numlines > 0) center /= double(sub->numlines);
		positions[pos + sub->numlines] = center;
		pos += sub->numlines + 1;
	}

	TArray<FEdgeBoundary> bounds;
	TArray<FVector2> edgedist;
	BuildSectorBoundaries(sec, h, bounds);
	ComputeEdgeDistances(bounds, positions, edgedist);

	// Create the actual vertices.
	for (i = 0, pos = 0; i < sec->subsectorcount; i++)
	{
		subsector_t* sub = sec->subsectors[i];
		LightmapSurface* lightmap = &sub->lightmap[h][lightmapIndex];
		if (lightmap->Type != ST_NULL)
		{
			float* luvs = lightmap->TexCoords;
			int lindex = lightmap->LightmapNum;
			float clu = 0, clv = 0;
			for (unsigned int j = 0; j < sub->numlines; j++)
			{
				SetFlatVertex(vbo_shadowdata[vi + pos + j], positions[pos + j], plane, luvs[j * 2], luvs[j * 2 + 1], lindex);
				clu += luvs[j * 2];
				clv += luvs[j * 2 + 1];
			}
			// The lightmap mapping is affine in world space, so the centre of the polygon
			// maps to the centre of its texture coordinates.
			if (sub->numlines > 0) { clu /= sub->numlines; clv /= sub->numlines; }
			SetFlatVertex(vbo_shadowdata[vi + pos + sub->numlines], positions[pos + sub->numlines], plane, clu, clv, lindex);
		}
		else
		{
			for (unsigned int j = 0; j <= sub->numlines; j++)
			{
				SetFlatVertex(vbo_shadowdata[vi + pos + j], positions[pos + j], plane);
			}
		}
		for (unsigned int j = 0; j <= sub->numlines; j++)
		{
			vbo_shadowdata[vi + pos + j].z += diff;
			vbo_shadowdata[vi + pos + j].SetEdgeDist(edgedist[pos + j].X, edgedist[pos + j].Y);
		}
		pos += sub->numlines + 1;
	}

	// Create the indices for the subsectors
	for (i = 0, pos = 0; i < sec->subsectorcount; i++)
	{
		subsector_t* sub = sec->subsectors[i];
		int firstndx = vi + pos;
		int centerndx = firstndx + sub->numlines;
		if (sub->numlines >= 3)
		{
			for (unsigned int k = 0; k < sub->numlines; k++)
			{
				ibo_data[idx++] = centerndx;
				ibo_data[idx++] = firstndx + k;
				ibo_data[idx++] = firstndx + ((k + 1) % sub->numlines);
			}
		}
		pos += sub->numlines + 1;
	}

	sec->ibocount = ibo_data.Size() - rt;
	return rt;
}

static int CreateIndexedSectorVertices(FFlatVertexBuffer* fvb, sector_t* sec, const secplane_t& plane, int floor, VertexContainer& verts, int h, int lightmapIndex)
{
	if (sec->HasLightmaps && lightmapIndex != -1)
		return CreateIndexedSectorVerticesLM(fvb, sec, plane, floor, h, lightmapIndex);

	auto& vbo_shadowdata = fvb->vbo_shadowdata;
	unsigned vi = vbo_shadowdata.Reserve(verts.vertices.Size());
	float diff;

	TArray<FEdgeBoundary> bounds;
	TArray<FVector2> edgedist;
	BuildSectorBoundaries(sec, h, bounds);
	ComputeEdgeDistances(bounds, verts.positions, edgedist);

	// Create the actual vertices.
	if (sec->transdoor && floor) diff = -1.f;
	else diff = 0.f;
	for (unsigned i = 0; i < verts.vertices.Size(); i++)
	{
		SetFlatVertex(vbo_shadowdata[vi + i], verts.positions[i], plane);
		vbo_shadowdata[vi + i].z += diff;
		vbo_shadowdata[vi + i].SetEdgeDist(edgedist[i].X, edgedist[i].Y);
	}

	auto& ibo_data = fvb->ibo_data;
	unsigned rt = ibo_data.Reserve(verts.indices.Size());
	for (unsigned i = 0; i < verts.indices.Size(); i++)
	{
		ibo_data[rt + i] = vi + verts.indices[i];
	}
	return (int)rt;
}

//==========================================================================
//
//
//
//==========================================================================

static int CreateIndexedVertices(FFlatVertexBuffer* fvb, int h, sector_t* sec, const secplane_t& plane, int floor, VertexContainers& verts)
{
	auto& vbo_shadowdata = fvb->vbo_shadowdata;
	sec->vboindex[h] = vbo_shadowdata.Size();
	// First calculate the vertices for the sector itself
	for (int n = 0; n < screen->mPipelineNbr; n++)
		sec->vboheight[n][h] = sec->GetPlaneTexZ(h);
	sec->ibocount = verts[sec->Index()].indices.Size();
	sec->iboindex[h] = CreateIndexedSectorVertices(fvb, sec, plane, floor, verts[sec->Index()], h, 0);

	// Next are all sectors using this one as heightsec
	TArray<sector_t*>& fakes = sec->e->FakeFloor.Sectors;
	for (unsigned g = 0; g < fakes.Size(); g++)
	{
		sector_t* fsec = fakes[g];
		fsec->iboindex[2 + h] = CreateIndexedSectorVertices(fvb, fsec, plane, false, verts[fsec->Index()], h, -1);
	}

	// and finally all attached 3D floors
	TArray<sector_t*>& xf = sec->e->XFloor.attached;
	for (unsigned g = 0; g < xf.Size(); g++)
	{
		sector_t* fsec = xf[g];
		int ffloorIndex;
		F3DFloor* ffloor = Find3DFloor(fsec, sec, ffloorIndex);

		if (ffloor != NULL && ffloor->flags & FF_RENDERPLANES)
		{
			bool dotop = (ffloor->top.model == sec) && (ffloor->top.isceiling == h);
			bool dobottom = (ffloor->bottom.model == sec) && (ffloor->bottom.isceiling == h);

			if (dotop || dobottom)
			{
				auto ndx = CreateIndexedSectorVertices(fvb, fsec, plane, false, verts[fsec->Index()], h, ffloorIndex + 1);
				if (dotop) ffloor->top.vindex = ndx;
				if (dobottom) ffloor->bottom.vindex = ndx;
			}
		}
	}
	sec->vbocount[h] = vbo_shadowdata.Size() - sec->vboindex[h];
	return sec->iboindex[h];
}


//==========================================================================
//
//
//
//==========================================================================

static void CreateIndexedFlatVertices(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	auto verts = BuildVertices(sectors);

	int i = 0;
	/*
	for (auto &vert : verts)
	{
		Printf(PRINT_LOG, "Sector %d\n", i);
		Printf(PRINT_LOG, "%d vertices, %d indices\n", vert.vertices.Size(), vert.indices.Size());
		// Read positions, not vertices: the synthetic interior points have no vertex_t to deref.
		for (unsigned j = 0; j < vert.positions.Size(); j++)
		{
			Printf(PRINT_LOG, "    %d: (%2.3f, %2.3f)\n", j, vert.positions[j].X, vert.positions[j].Y);
		}
		for (unsigned i=0;i<vert.indices.Size();i+=3)
		{
			Printf(PRINT_LOG, "     %d, %d, %d\n", vert.indices[i], vert.indices[i + 1], vert.indices[i + 2]);
		}

		i++;
	}
	*/


	for (int h = sector_t::floor; h <= sector_t::ceiling; h++)
	{
		for (auto& sec : sectors)
		{
			CreateIndexedVertices(fvb, h, &sec, sec.GetSecPlane(h), h == sector_t::floor, verts);
		}
	}

	// We need to do a final check for Vavoom water and FF_FIX sectors.
	// No new vertices are needed here. The planes come from the actual sector
	for (auto& sec : sectors)
	{
		for (auto ff : sec.e->XFloor.ffloors)
		{
			if (ff->top.model == &sec)
			{
				ff->top.vindex = sec.iboindex[ff->top.isceiling];
			}
			if (ff->bottom.model == &sec)
			{
				ff->bottom.vindex = sec.iboindex[ff->top.isceiling];
			}
		}
	}
}

//==========================================================================
//
//
//
//==========================================================================

static void UpdatePlaneVertices(FFlatVertexBuffer *fvb, sector_t* sec, int plane)
{
	int startvt = sec->vboindex[plane];
	int countvt = sec->vbocount[plane];
	secplane_t& splane = sec->GetSecPlane(plane);
	FFlatVertex* vt = &fvb->vbo_shadowdata[startvt];
	FFlatVertex* mapvt = fvb->GetBuffer(startvt);
	for (int i = 0; i < countvt; i++, vt++, mapvt++)
	{
		vt->z = (float)splane.ZatPoint(vt->x, vt->y);
		if (plane == sector_t::floor && sec->transdoor) vt->z -= 1;
		mapvt->z = vt->z;
	}
	
	fvb->mVertexBuffer->Upload(startvt * sizeof(FFlatVertex), countvt * sizeof(FFlatVertex));
}

//==========================================================================
//
//
//
//==========================================================================

static void CreateVertices(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	fvb->vbo_shadowdata.Resize(FFlatVertexBuffer::NUM_RESERVED);
	CreateIndexedFlatVertices(fvb, sectors);
}

//==========================================================================
//
//
//
//==========================================================================

static void CheckPlanes(FFlatVertexBuffer* fvb, sector_t* sector)
{
	if (sector->GetPlaneTexZ(sector_t::ceiling) != sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::ceiling])
	{
		UpdatePlaneVertices(fvb, sector, sector_t::ceiling);
		sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::ceiling] = sector->GetPlaneTexZ(sector_t::ceiling);
	}
	if (sector->GetPlaneTexZ(sector_t::floor) != sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::floor])
	{
		UpdatePlaneVertices(fvb, sector, sector_t::floor);
		sector->vboheight[screen->mVertexData->GetPipelinePos()][sector_t::floor] = sector->GetPlaneTexZ(sector_t::floor);
	}
}

//==========================================================================
//
// checks the validity of all planes attached to this sector
// and updates them if possible.
//
//==========================================================================

void CheckUpdate(FFlatVertexBuffer* fvb, sector_t* sector)
{
	CheckPlanes(fvb, sector);
	sector_t* hs = sector->GetHeightSec();
	if (hs != NULL) CheckPlanes(fvb, hs);
	for (unsigned i = 0; i < sector->e->XFloor.ffloors.Size(); i++)
		CheckPlanes(fvb, sector->e->XFloor.ffloors[i]->model);
}

//==========================================================================
//
//
//
//==========================================================================

void CreateVBO(FFlatVertexBuffer* fvb, TArray<sector_t>& sectors)
{
	fvb->vbo_shadowdata.Resize(fvb->mNumReserved);
	CreateVertices(fvb, sectors);
	fvb->mCurIndex = fvb->mIndex = fvb->vbo_shadowdata.Size();
	fvb->Copy(0, fvb->mIndex);
	fvb->mIndexBuffer->SetData(fvb->ibo_data.Size() * sizeof(uint32_t), &fvb->ibo_data[0], BufferUsageType::Static);
}
