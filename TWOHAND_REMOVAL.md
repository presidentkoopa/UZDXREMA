# Two-handing: what is stock, and what is mine and prepped for removal

Written 2026-09-05. The question this answers: two-handing now works because a
retired ENGINE feature was switched back on. Everything built on top of it
during this session is separable, and this is the list.

**DONE 2026-09-05.** Everything under REMOVE below has been removed and the
tree builds clean. The two KEEP sections are what remains. Kept for the record
of what the feature is made of and what was taken off it.

---

## KEEP. This is the feature, and it predates this session.

Turning any of this off turns two-handing off.

Provenance, checked against history rather than assumed:
`weaponStabilised` and its aiming block are genuinely upstream (`c575510`, the
QuestZDoom merge). The original test beside it was a hardcoded `distance <
0.50f`. `vr_stabilize_distance_inches` and `StabilizeReach` came later, in
`efac7bcc` -- the same commit that RETIRED that test. So the reach controls
were built to be exactly this feature's distance knob and then never had a
proximity test to drive, which is why the slider sat in the menu doing
nothing. Wiring the restore to them is putting them back on the job they were
added for, not a repurposing.

| Where | What |
|---|---|
| `wadsrc/static/menudef.txt:2061-2062` | The menu: `Two Handed Weapons` on/off, and the `Stabilize Distance (in)` slider under it |
| `hw_vrmodes.cpp:961` | `vr_stabilize_distance_inches` (8.0), the global reach |
| `actor.h` `StabilizeReach` + `wadsrc/static/zscript/vr_stabilizesync.zs` | per-weapon reach in inches; 0 = use the global, negative = this weapon never braces |
| `actor.h` `TwoHandedHold`, `actor.zs` | the published "both hands are on it" flag weapons read to tighten spread |
| `vk_openxrdevice.cpp` `weaponStabilised` block | the original behaviour: the weapon aims along the line between the hands |

## KEEP, but it is mine. Small, and it is why the restore is safe.

The proximity test was retired for real reasons. These two are the reasons
being handled rather than reinstated. Removing them brings back the bugs that
got the feature retired in the first place.

| Where | What | If removed |
|---|---|---|
| `vk_openxrdevice.cpp` `offBusy` test in the proximity block | suppresses the brace when the off hand's claim is a round, shell, magazine, slide, pouch, holster or its own grip | reloading braces constantly again |
| `vk_openxrdevice.cpp` `vr_two_handed_swap_inches` (7.0) | inside that distance a bare off-grip is a hand-to-hand weapon transfer, not a brace | passing a weapon between hands braces instead |

---

## REMOVE. All of it is mine, all of it is from this session.

Its single purpose is moving the off HAND onto the gun. Stock two-handing never
did that -- it aimed the weapon and left the hands on the controllers. If that
is the behaviour you want, none of this needs to exist.

### 1. The anchor itself -- the published point where a bracing hand belongs

- `src/playsim/actor.h` -- `TwoHandAnchorPos`, `TwoHandAnchorValid` (2 fields + comment)
- `src/playsim/d_player.h` -- `vr_twohand_offset[3]`, `vr_twohand_offset_set`
- `src/playsim/vr_armik.h` -- 3 declarations
- `src/playsim/vr_armik.cpp` -- `VR_ControllerAxesGL`, `VR_TwoHandAnchorGL`,
  `VR_TwoHandOffsetFromWorld`, `VR_UpdateTwoHandAnchor`, the
  `vr_twohand_along/up/side` cvars and their comment block
- `src/playsim/p_user.cpp` -- the one `VR_UpdateTwoHandAnchor(player)` call in `P_PlayerThink`
- `src/scripting/vmthunks_actors.cpp` -- `SetTwoHandAnchorOffset`,
  `SetTwoHandAnchorWorld`, 2 `DEFINE_FIELD`s
- `wadsrc/static/zscript/actors/actor.zs` -- the matching natives and fields

### 2. Drawing a world model at the anchor

- `src/r_data/models.h` -- `MDL_TWOHANDANCHORED` (bit 22)
- `src/r_data/models.cpp` -- the `twohandanchored` MODELDEF keyword, and the
  block in the follow-hand path that replaces the translation

Already dead in practice: world models are out.

### 3. Drawing a HUD layer at the anchor

- `src/playsim/p_pspr.h` -- `bool TwoHandAnchored`
- `src/playsim/p_pspr.cpp` -- its `DEFINE_FIELD`
- `wadsrc/static/zscript/actors/player/player.zs` -- the native field
- `src/r_data/models.cpp` -- the block in the HUD path that replaces the translation

This is the only half of 2/3 that is live, since the hands are psprite.

### 4. The IK body's own pin

- `src/playsim/vr_armik.cpp` -- `vr_ik_twohand` and the block that pins the off
  hand's IK target to the anchor

Dead if the avatar is dropped.

### 5. Mod side

- `RS_VRIK/MENUDEF.txt` -- the two-handing rows I added under Arms and wrists
- `RS_VR_Unified/MODELDEF.txt` -- `TwoHandAnchored` on `RS_HandWorldOff`
- `RS_VR_Unified/zscript/hands/rs_hands.zs` -- `psp.TwoHandAnchored = (layer == LAYER_OFF)`
- `RS_VR_Unified/zscript/hands/rs_stabilize.zs` -- `ArchetypeAnchor`, the
  `SetTwoHandAnchorOffset` push, the `SetTwoHandAnchorWorld` calls in
  `Latch`/`Release`, and the `anchorDist0` release test (that last one must
  revert to the oval-score test it replaced, or the brace never releases on
  distance)

### 6. Diagnostics

- `[TWOHAND]` lines in `vk_openxrdevice.cpp`, `vr_armik.cpp` and `models.cpp` (x2)
- `[RSSTAB] anchor live` and `bracing hand sits` in `rs_stabilize.zs`

---

## Order to remove in

5, then 3, then 2, then 4, then 1, then 6. Callers before the thing they call,
so the tree builds at every step. Section 1 last, because everything else
references it.

Commits to revert if a clean back-out is preferred to hand removal, newest
first: `af88cbe` (Unified), `854208e` (Unified), part of `f879ff6` (Unified --
its detection half is NOT mine and must be kept), `b87620a`, `c21ee97`,
`67056d5`, `b50f383`, and the two-handing hunk of `1943927`. `0961472` and
`a1e1857` must NOT be reverted wholesale: they also carry the crouch fix, the
head pivot, `RenderAttachParent` and the swap guard.
