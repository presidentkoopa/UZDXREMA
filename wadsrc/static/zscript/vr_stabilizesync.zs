// Copies each player's ready weapon's StabilizeDistance into the native-read
// AActor.StabilizeReach field every tic.
//
// StabilizeDistance is authored on the Weapon class (per-weapon, in inches),
// but the native two-hand stabilize check runs in the VR input backend, which
// has no native Weapon class to read a ZScript-only field from -- Weapon has
// no native C++ backing beyond plain Actor. So this bridges it: read once
// here, from wherever ZScript already can (Actor.ReadyWeapon), write into a
// plain native double the engine already reads in place of a fixed constant.
//
// A dedicated handler rather than hooking Weapon.Tick(): a custom weapon that
// overrides Tick() without calling Super.Tick() would silently stop syncing,
// and nothing about that failure would be visible. This runs regardless of
// what any individual weapon class does.
class VRStabilizeSyncHandler : EventHandler
{
	override void WorldTick()
	{
		for (int i = 0; i < MAXPLAYERS; ++i)
		{
			if (!playeringame[i] || players[i].mo == null)
				continue;

			let pawn = players[i].mo;
			let weap = players[i].ReadyWeapon;
			pawn.StabilizeReach = (weap != null) ? weap.StabilizeDistance : 0.0;
		}
	}
}
