// Draws a rigged hand model at each VR controller.
//
// WHY PSPRITES AND NOT WORLD ACTORS
//
// The hand models ride psprite layers, the same path the weapon viewmodel uses,
// because that path is handed the controller transform inside RenderHUDModel at
// render rate. A world actor would only be repositioned once per tic and would
// visibly trail the controller, which is the one thing a VR hand cannot do.
//
// The cost of that choice: AnimateBones is never called on this path. It has a
// single call site engine-wide, on the world-actor path, gated on
// MF9_DECOUPLEDANIMATIONS (see hw_sprites.cpp). So per-render-frame finger
// posing is unavailable here, and any finger work has to go through the ordinary
// bone setters at tic rate. That is enough for now: OpenXR trigger and squeeze
// are bound as boolean actions in this fork, not floats, so there is no analog
// finger curl to smooth in the first place.
//
// HOW EACH HAND REACHES THE RIGHT CONTROLLER
//
// RenderHUDModel picks the hand from the psprite, preferring the caller and
// falling back to the layer ID. Layers at or above PSprite.OFFHANDWEAPON are the
// off hand. That fallback exists for exactly this case: before it, hand
// selection was pointer equality against player.OffhandWeapon, so any psprite
// whose caller was not literally a weapon silently drew on the main hand.
//
// Do NOT try to reach the off hand by assigning a hand actor into
// player.OffhandWeapon. GetWeaponTransform reads IntVar(NAME_WeaponFlags) off
// whatever is in that slot, and on a non-Weapon that is a fatal I_Error.

class RS_VRHand : Inventory
{
	Default
	{
		Inventory.MaxAmount 1;
		Inventory.InterHubAmount 1;
		+INVENTORY.UNDROPPABLE
		+INVENTORY.UNTOSSABLE
		+INVENTORY.QUIET
		// Model lookup goes through BaseSpriteModelFrames rather than the state's
		// sprite and frame, so the placeholder state below never needs a real
		// sprite and the mesh is addressed directly.
		+DECOUPLEDANIMATIONS
	}
	States
	{
	Spawn:
		TNT1 A -1;
		Stop;
	}
}

class RS_VRHandLeft  : RS_VRHand {}
class RS_VRHandRight : RS_VRHand {}

// Registered through GameInfo.AddEventHandlers in mapinfo/common.txt, the same
// way VRStabilizeSyncHandler is.
class RS_VRHandHandler : EventHandler
{
	// Main hand sits below PSprite.OFFHANDWEAPON, off hand at or above it, which
	// is what puts each on the correct controller. Both are far from the layer
	// numbers mods typically use (PSprite.WEAPON is 1, FLASH is 1000).
	const LAYER_MAIN = 900000;
	const LAYER_OFF  = 1900000;

	override void WorldTick()
	{
		// Deliberately local-player only. vr_hands and vr_hand_scale are client
		// CVARs, and reading a client CVAR to drive playsim state for every
		// player would not be deterministic across a network game. Hands are a
		// singleplayer feature for now; making other players' hands visible is a
		// separate job that needs the pose carried in the usercmd, since the
		// netcode currently sends main-hand aim angles only -- no hand position,
		// no roll.
		if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS)
			return;
		if (!playeringame[consoleplayer])
			return;

		let player = players[consoleplayer];
		let pawn = player.mo;
		if (pawn == null)
			return;

		if (!vr_hands)
		{
			ClearHand(player, LAYER_MAIN);
			ClearHand(player, LAYER_OFF);
			return;
		}

		// Which mesh belongs on which layer depends on handedness, not on the
		// layer's name. GetWeaponTransform resolves the main hand to physical
		// controller 1 (right) under a right-handed scheme and to controller 0
		// (left) under a left-handed one, so a fixed mapping would put the wrong
		// mesh on each hand for left-handed players. Values 10 and above are the
		// left-handed schemes.
		bool rightHanded = vr_control_scheme < 10;
		Name mainCls = rightHanded ? "RS_VRHandRight" : "RS_VRHandLeft";
		Name offCls  = rightHanded ? "RS_VRHandLeft"  : "RS_VRHandRight";

		UpdateHand(player, pawn, LAYER_MAIN, mainCls);
		UpdateHand(player, pawn, LAYER_OFF,  offCls);
	}

	// Rebuilt every tic on purpose. A_ClearOverlays exempts only STRIFEHANDS,
	// WEAPON, OFFHANDWEAPON and FLASH, so a mod clearing overlays would drop
	// these layers; re-asserting them each tic means they simply come back
	// instead of vanishing for the rest of the level.
	private void UpdateHand(PlayerInfo player, PlayerPawn pawn, int layer, Name cls)
	{
		let hand = pawn.FindInventory(cls);
		if (hand == null)
		{
			pawn.GiveInventory(cls, 1);
			hand = pawn.FindInventory(cls);
			if (hand == null)
				return;
		}

		let psp = player.FindPSprite(layer);
		if (psp == null || psp.Caller != hand)
		{
			// FindState, not ResolveState. ResolveState is declared `action`, so
			// it carries the implicit (self, stateowner, callingstate) context a
			// state function is called with. Reached from an event handler there
			// is no such context, and the native aborts. FindState is clearscope
			// and is the correct way to look a state up from outside a state.
			State st = hand.FindState("Spawn");
			if (st == null)
				return;

			player.SetPsprite(layer, st, false, hand);
			psp = player.FindPSprite(layer);
			if (psp == null)
				return;
		}

		// The fork's psprite scale channel reaches the model path, so this is the
		// live tuning knob and no re-export is needed to resize the hands. X is
		// applied uniformly on the model side; Y is set to match so nothing reads
		// as a non-uniform scale.
		double s = vr_hand_scale;
		if (s <= 0.0)
			s = 1.0;
		psp.scale = (s, s);
	}

	private void ClearHand(PlayerInfo player, int layer)
	{
		if (player.FindPSprite(layer) != null)
			player.SetPsprite(layer, null);
	}
}
