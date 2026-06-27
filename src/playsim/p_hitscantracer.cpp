#include "p_hitscantracer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "actor.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "gamedata/a_weapons.h"
#include "playsim/p_local.h"

EXTERN_CVAR(Int, vr_hitscan_tracer)
EXTERN_CVAR(Bool, vr_hitscan_ricochet)
EXTERN_CVAR(Float, vr_hitscan_ricochet_chance)
EXTERN_CVAR(Float, vr_hitscan_tracer_offset)
EXTERN_CVAR(Float, vr_hitscan_tracer_speed)

static std::vector<FHitscanTracer> gHitscanTracers;

static uint32_t HitscanTracerHash(uint64_t v)
{
	v ^= v >> 33;
	v *= 0xff51afd7ed558ccdULL;
	v ^= v >> 33;
	v *= 0xc4ceb9fe1a85ec53ULL;
	v ^= v >> 33;
	return (uint32_t)v;
}

static double HashToUnit(uint32_t value)
{
	return (double)value / 4294967295.0;
}

static uint64_t MakeRicochetSeed(const DVector3& attackPos, const DVector3& damagePos, int flags)
{
	uint64_t seed = 0;
	seed ^= (uint64_t)(int64_t)std::llround(attackPos.X * 16.0) * 0x9e3779b185ebca87ULL;
	seed ^= (uint64_t)(int64_t)std::llround(attackPos.Y * 16.0) * 0xc2b2ae3d27d4eb4fULL;
	seed ^= (uint64_t)(int64_t)std::llround(attackPos.Z * 16.0) * 0x165667b19e3779f9ULL;
	seed ^= (uint64_t)(int64_t)std::llround(damagePos.X * 16.0) * 0x85ebca77c2b2ae63ULL;
	seed ^= (uint64_t)(int64_t)std::llround(damagePos.Y * 16.0) * 0x27d4eb2f165667c5ULL;
	seed ^= (uint64_t)(int64_t)std::llround(damagePos.Z * 16.0) * 0x94d049bb133111ebULL;
	seed ^= (uint64_t)flags * 0x632be59bd9b4e019ULL;
	return seed;
}

static bool ShouldSpawnRicochet(const DVector3& attackPos, const DVector3& damagePos, int flags)
{
	if (!vr_hitscan_ricochet || vr_hitscan_ricochet_chance <= 0.0f)
	{
		return false;
	}

	const uint64_t seed = MakeRicochetSeed(attackPos, damagePos, flags);
	const double roll = HashToUnit(HitscanTracerHash(seed));
	return roll * 100.0 <= (double)vr_hitscan_ricochet_chance;
}

static DVector3 MakeRicochetDirection(const DVector3& direction, const DVector3& attackPos, const DVector3& damagePos, int flags)
{
	DVector3 basis = -direction;
	DVector3 right, up;
	basis.GetRightUp(right, up);
	if (right.LengthSquared() < 1e-8)
	{
		right = DVector3(0.0, 1.0, 0.0);
	}
	if (up.LengthSquared() < 1e-8)
	{
		up = DVector3(0.0, 0.0, 1.0);
	}
	right.MakeUnit();
	up.MakeUnit();

	const uint64_t seed = MakeRicochetSeed(attackPos, damagePos, flags);

	const double rx = HashToUnit(HitscanTracerHash(seed ^ 0xA2E3D7F9u)) * 2.0 - 1.0;
	const double ry = HashToUnit(HitscanTracerHash(seed ^ 0x6C8E9CF3u)) * 2.0 - 1.0;
	DVector3 ricochetDir = basis + right * (rx * 0.75) + up * (ry * 0.35);
	if (ricochetDir.LengthSquared() < 1e-8)
	{
		return basis;
	}
	ricochetDir.MakeUnit();
	return ricochetDir;
}

void P_ClearHitscanTracers()
{
	gHitscanTracers.clear();
}

std::vector<FHitscanTracer>& P_GetHitscanTracers()
{
	return gHitscanTracers;
}

static AActor* GetFiringWeapon(AActor* actor, int flags)
{
	if (actor == nullptr)
	{
		return nullptr;
	}

	if (actor->IsKindOf(NAME_Weapon))
	{
		return actor;
	}

	if (actor->player == nullptr)
	{
		return nullptr;
	}

	return (flags & LAF_ISOFFHAND) ? actor->player->OffhandWeapon : actor->player->ReadyWeapon;
}

void P_QueueHitscanTracer(AActor* actor, const DVector3& attackPos, const DVector3& damagePos, int flags)
{
	if (actor == nullptr || actor->Level == nullptr)
	{
		return;
	}

	DVector3 tracerVector = damagePos - attackPos;
	const double totalDistance = tracerVector.Length();
	if (totalDistance <= 0.01)
	{
		return;
	}

	tracerVector.MakeUnit();

	AActor* weapon = GetFiringWeapon(actor, flags);
	const bool forceEnabled = weapon != nullptr && (weapon->IntVar(NAME_WeaponFlags) & WIF_HASHITSCANTRACER);
	if (!forceEnabled)
	{
		if (vr_hitscan_tracer == 0)
		{
			return;
		}

		const bool playerWeapon = actor->player != nullptr && weapon != nullptr;
		if (vr_hitscan_tracer == 1 && !playerWeapon)
		{
			return;
		}
	}

	double startOffset = std::max(0.0, (double)vr_hitscan_tracer_offset);
	if (weapon != nullptr)
	{
		if (double* weaponOffset = (double*)weapon->ScriptVar(NAME_HitscanTracerOffset, nullptr); weaponOffset != nullptr && *weaponOffset >= 0.0)
		{
			startOffset = *weaponOffset;
		}
	}

	startOffset = std::min(startOffset, totalDistance);
	const double remainingDistance = totalDistance - startOffset;
	if (remainingDistance <= 0.01)
	{
		return;
	}

	const double tracerSpeed = std::max(1.0, (double)vr_hitscan_tracer_speed * 100.0 / (double)TICRATE);
	FHitscanTracer tracer;
	tracer.Start = attackPos + tracerVector * startOffset;
	tracer.Direction = tracerVector;
	tracer.Distance = remainingDistance;
	tracer.SpawnTime = actor->Level->maptime;
	tracer.Lifetime = remainingDistance / tracerSpeed;
	tracer.SpeedScale = 1.0;
	tracer.Weapon = weapon;
	gHitscanTracers.push_back(tracer);
}

void P_QueueHitscanRicochet(AActor* actor, const DVector3& attackPos, const DVector3& impactPos, int flags)
{
	if (actor == nullptr || actor->Level == nullptr)
	{
		return;
	}

	DVector3 shotVector = impactPos - attackPos;
	const double totalDistance = shotVector.Length();
	if (totalDistance <= 0.01)
	{
		return;
	}

	shotVector.MakeUnit();

	AActor* weapon = GetFiringWeapon(actor, flags);
	const bool forceEnabled = weapon != nullptr && (weapon->IntVar(NAME_WeaponFlags) & WIF_HASHITSCANTRACER);
	if (!forceEnabled)
	{
		if (vr_hitscan_tracer == 0)
		{
			return;
		}

		const bool playerWeapon = actor->player != nullptr && weapon != nullptr;
		if (vr_hitscan_tracer == 1 && !playerWeapon)
		{
			return;
		}
	}

	if (!ShouldSpawnRicochet(attackPos, impactPos, flags))
	{
		return;
	}

	const uint64_t seed = MakeRicochetSeed(attackPos, impactPos, flags);
	const double ricochetScale = 2.0 + HashToUnit(HitscanTracerHash(seed ^ 0x7F4A7C15u));
	const double ricochetDistance = std::max(2.0, (totalDistance / 9.0) * ricochetScale);
	const double tracerSpeed = std::max(1.0, (double)vr_hitscan_tracer_speed * 100.0 / (double)TICRATE);
	const double ricochetSpeed = tracerSpeed * 0.25;

	FHitscanTracer ricochet;
	ricochet.Start = impactPos;
	ricochet.Direction = MakeRicochetDirection(shotVector, attackPos, impactPos, flags);
	ricochet.Distance = ricochetDistance;
	ricochet.SpawnTime = actor->Level->maptime;
	ricochet.Lifetime = ricochetDistance / ricochetSpeed;
	ricochet.SpeedScale = 0.25;
	ricochet.bRicochet = true;
	ricochet.Weapon = weapon;
	gHitscanTracers.push_back(ricochet);
}
