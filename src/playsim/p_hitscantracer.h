#pragma once

#include "vectors.h"

#include <vector>

class AActor;

struct FHitscanTracer
{
	DVector3 Start;
	DVector3 Direction;
	double Distance = 0.0;
	double SpawnTime = 0.0;
	double Lifetime = 0.0;
	double SpeedScale = 1.0;
	bool bRicochet = false;
	AActor* Weapon = nullptr;
};

void P_ClearHitscanTracers();
void P_QueueHitscanTracer(AActor* actor, const DVector3& attackPos, const DVector3& damagePos, int flags);
void P_QueueHitscanRicochet(AActor* actor, const DVector3& attackPos, const DVector3& impactPos, int flags);
std::vector<FHitscanTracer>& P_GetHitscanTracers();
