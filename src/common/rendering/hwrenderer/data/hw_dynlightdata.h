// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2005-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
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

#ifndef __GLC_DYNLIGHT_H
#define __GLC_DYNLIGHT_H

#include "tarray.h"
#include "a_dynlight.h"
#include "hw_cvars.h"
#include "r_utility.h"

struct FDynLightData
{
	TArray<float> arrays[3];

	void Clear()
	{
		arrays[0].Clear();
		arrays[1].Clear();
		arrays[2].Clear();
	}

	void Combine(int *siz, int max)
	{
		siz[0] = arrays[0].Size();
		siz[1] = siz[0] + arrays[1].Size();
		siz[2] = siz[1] + arrays[2].Size();
		arrays[0].Resize(arrays[0].Size() + arrays[1].Size() + arrays[2].Size());
		memcpy(&arrays[0][siz[0]], &arrays[1][0], arrays[1].Size() * sizeof(float));
		memcpy(&arrays[0][siz[1]], &arrays[2][0], arrays[2].Size() * sizeof(float));
		siz[0]>>=2;
		siz[1]>>=2;
		siz[2]>>=2;
		if (siz[0] > max) siz[0] = max;
		if (siz[1] > max) siz[1] = max;
		if (siz[2] > max) siz[2] = max;
	}


};

extern unsigned int gl_dynlight_viewid;

inline DVector3 gl_GetLightPosRelative(FDynamicLight *light, int portalgroup)
{
	if (!gl_light_pos_relative_cache)
	{
		return light->PosRelative(portalgroup);
	}

	if (light->mPosRelativeCacheViewId == gl_dynlight_viewid && light->mPosRelativeCacheGroup == portalgroup)
	{
		return light->mPosRelativeCache;
	}

	light->mPosRelativeCache = light->PosRelative(portalgroup);
	light->mPosRelativeCacheViewId = gl_dynlight_viewid;
	light->mPosRelativeCacheGroup = portalgroup;
	return light->mPosRelativeCache;
}

inline bool gl_IsDistanceCulled(FDynamicLight *light)
{
	if (!gl_light_distance_cull_cache)
	{
		double dist3 = gl_light_distance_cull * gl_light_distance_cull;
		if (dist3 <= 0.0)
			return false;

		double dist1 = (light->Pos - r_viewpoint.Pos).LengthSquared();
		return dist1 > dist3;
	}

	if (light->mDistanceCullViewId == gl_dynlight_viewid)
		return light->mDistanceCullResult;

	double dist3 = gl_light_distance_cull * gl_light_distance_cull;
	if (dist3 <= 0.0)
	{
		light->mDistanceCullViewId = gl_dynlight_viewid;
		light->mDistanceCullResult = false;
		return false;
	}

	double dist1 = (light->Pos - r_viewpoint.Pos).LengthSquared();
	light->mDistanceCullViewId = gl_dynlight_viewid;
	light->mDistanceCullResult = dist1 > dist3;
	return light->mDistanceCullResult;
}

extern thread_local FDynLightData lightdata;


#endif
