# VR Interaction — the plan, and what not to rebuild

Written 2026-08-25, at the end of a long session that ended in a pivot. This
exists so the next session starts from the conclusion instead of re-deriving it.

---

## The decision

**Interaction runs at 35 Hz, on the playsim, using Doom's own movement.**

Bone-based gunplay is abandoned. Fifteen attempts, and the last one is documented
below so it does not get attempted a sixteenth time.

What actually needed 90 Hz was exactly one thing: continuously simulating an
object while it is held in your hand. Everything else — traces, targeting,
scripted motion, pickups, throwing, melee, hit reactions — is playsim work that
Doom has done at 35 Hz since 1993.

Consequences that are *good*, not compromises:

- A thrown object is an actor with `Vel`. `P_XYMovement`/`P_ZMovement` already
  give gravity, floors, ceilings, stairs, wall sliding, bouncing and impact.
- A held object moved with `TryMove` instead of `SetOrigin` **stops at walls**.
  That was the whole goal of the abandoned "wire the solver to the renderer"
  stage, available on the standard path for free.
- At 35 Hz you *want* `SetOrigin(pos, true)` so the renderer interpolates between
  tics. The physics module had to force `RF_DONTINTERPOLATE` because it wrote
  faster than the tick; going back to 35 means leaning on interpolation instead
  of fighting it.

---

## The list, in order

### Foundations — small, everything leans on them
- **Held-state machine.** One hand, both hands, hand-to-hand pass, level change,
  second grab on an already-held thing. First, because hardpoints, gestures and
  throwing all consume it — and because `heldByHand` as a single `int` meant a
  second grab silently *stole* the object.
- **Grabbability policy, as data.** Health pack yes, +1 bonus no, barrel yes,
  live imp no. A table to tune for a year, not `if`s scattered through grab code.

### The flagship
- **Distance grabbing.** Cone cast from the hand, target scoring, lock, flick,
  ease-out arc that lifts through the middle and homes to where the hand *is* on
  arrival. Reach and spread as *separate* numbers so the drawn cone IS the tested
  cone. Real-time menu sliders.
- **Voxel on grab.** `A_ChangeModel` to a `.kvx`, per instance, so the floor copy
  stays a sprite. KVX is a model type in this engine (`model_kvx.h`), so a voxel
  is not a special case. Kills the billboard-in-your-palm problem: a sprite always
  faces you, so an item in a 3D hand never turns over.

### Cheap, high return
- **Haptics and diegetic sound** on grab, impact and release. Speed-scaled impact
  haptics existed in the physics module and are worth salvaging standalone.

### One foundation, three features
- **Swing history, peak-velocity release.** ~180 ms window; take the peak, not the
  last sample — the last sample is usually deceleration, which throws limply.
- **Throwing** → free once velocity exists.
- **Grenades** → throwing plus a fuse.
- **Melee** → the same velocity, applied on contact instead of release.

### Once things can be held
- **Persistence.** Pouch/holster contents surviving save and level change.
  `DPSprite::Serialize` already drops every anchor field silently — works all
  session, evaporates on load. Same failure shape.
- **Hardpoint refinement.** Consumes held-state and persistence.
- **Gesture support.** Consumes the velocity machinery and hardpoints.

### Parallel — blocked by nothing
- **Offline control mapper exe** for Quest.
- **Voxel culling.** A perf pass. Do it when there is enough on screen to
  measure; guessing early optimises the wrong thing.

### Gated on a decision, not on work
- **Off-hand in the usercmd.** `usercmd_t` carries `weaponpitch`/`weaponyaw`
  only — main hand, direction, no position, no off hand at all. So off-hand
  distance grabbing and off-hand gestures are single-player until the protocol
  grows. ~8 shorts; breaks demo compat with builds lacking it, which does not
  matter for a fork talking to itself.
- **Player IK body.** Much later.

---

## Netplay

The target is this fork playing against itself.

- Everything above is deterministic playsim work and is netplay-safe.
- **The physics module can never be netplay-safe as written.** `WriteBack` calls
  `a->SetXYZ()`, `LinkToWorld()` and assigns `Angles`/`Vel` from
  `P_PhysicsFrame()` at *frame rate*, outside the tick. Peers at different
  framerates take different step counts between tics and diverge immediately.
  A flag does not fix that; stepping on the tick would.
- Throwing survives 35 Hz because a throw is determined by one number — the
  velocity at release. Measure it at render rate, transmit it as an event
  payload, apply it on the tick. Sample fast, transmit slow.
- What genuinely degrades at 35 Hz: continuous *fine* manipulation. A held object
  grinding along a wall, two hands working against each other on one object.
  Throwing, catching, shoving and dropping are unaffected.

---

## State of the tree

- **`vr_physics` defaults OFF**, `vr_physics_debug` OFF. `P_PhysicsFrame()`
  early-returns and clears bodies. The module is intact behind the switch, not
  gutted. Engine timing is stock: 35 Hz playsim, headset-rate render.
- **Mod**: `E:\rs_hands`, a clone of `presidentkoopa/RS_Hands`, branch `main`.
  Packs to `build-dxr/Debug/RS_Hands.pk3`, which is what the ini autoloads.
  `E:\UZDXR_Hands` was the uncommitted working copy and is now **dead** — it
  was merged into the clone on 2026-08-25 and editing it changes nothing that
  loads. The packer skips `README.md`, `docs/`, `tools/` and `hand_frames.txt`;
  `docs/` alone is megabytes of PNG the engine would index as textures.
- **Hands**: the **psprite** hands are the ones on screen. `rs_hands=true`,
  `rs_handworld=false`. The world hands stay in the tree and stay switchable —
  they are what collision and grabbing would need if that comes back — but two
  hand systems drawing at once put one mesh on top of another at two different
  scales, which is not a comparison anyone can judge. Both were saved **true**
  in the ini while CVARINFO defaulted both to false: a saved value always wins,
  which is why changing a CVARINFO default reads as a fix that silently failed.
- **Removed**: the M9 package (pk3 preserved as `UZDXR_M9.pk3.removed`), gravity
  gloves, and the psprite hands' seat/anchor machinery — `SEAT_MATCH`/`MAIN`/
  `SUPPORT`/`RACK`, `AnchorToGrip`, all `rs_seat_*` cvars and menus.
- **Rendering/cosmetic engine work was never touched** by any of this. Different
  files entirely.

### Engine additions that are useful and staying

| addition | what it does |
|---|---|
| `ModelPointToWorld(mx,my,mz)` | model-space point → world position + forward + up, via the renderer's own object-to-world matrix. The only way to ask where a bone is *in the room*. |
| `FindBoneIndex(name)` | does this model have this bone, −1 if not. Every other bone call fails silently on a missing name. |
| per-axis placement scale `_scale_x/_y/_z` | both render paths |
| `AActor::ModelFrame/Next/Lerp` | address a model frame by number on a *world* actor. Sprite-letter lookup caps at `MAX_SPRITE_FRAMES`, so frame 1293 has no letter that can name it. |
| IQM bind-pose seeding fix | bones were displaced by the inverse of their own bind pose. **The one non-opt-in behaviour change in the tree.** |

---

## Do not rebuild

- Bone-based weapon seating, per-weapon bone tables, grip/support/rack/insert
  anchoring. Fifteen attempts. The last one got as far as deriving the seat from
  `MARKER_grip` → `HANDPALM_joint` in world space, which is correct maths and
  still not worth the cost.
- World-actor guns as physics bodies.
- Anything that reads a controller pose from inside the playsim and expects it to
  be netplay-safe.

---

## Traps that each cost hours

- **`out Vector3` crashes the ZScript JIT.** "Unknown REGT value passed to
  EmitPARAM", at class-load, taking the whole handler down. Return vectors by
  value. Hit twice in one session — the second time after writing a comment
  warning about the first.
- **CVARINFO defaults never override a value already saved in the ini.** Changing
  a default to fix something does nothing and looks like the fix silently failed.
  Use a *fresh cvar name* when a default must actually take effect.
- **`PlacementCVars <prefix>` fails silently** when the cvars do not exist. The
  renderer looks them up by name every frame and an absent cvar reads exactly like
  a zeroed one — that axis sits at its default forever with nothing in the log.
- **`FrameIndex` and `+DECOUPLEDANIMATIONS` are mutually exclusive.** A decoupled
  actor resolves its model through `BaseSpriteModelFrames`, which only `BaseFrame`
  populates. Set the flag on a `FrameIndex` model and it draws nothing, silently.
- **`TNT1` is not an empty sprite.** It is an instruction to skip the actor
  entirely, checked before any model is considered.
- **Bone-by-*name* failures only `Printf`**; bone-by-*index* aborts the VM.
- **A followed model is culled on its real world position** while being drawn at
  your hand, so it must be kept near the player or it silently vanishes.
- **Model axis convention**: model X = forward, Y = **up**, Z = sideways. A cone
  built down +Z opens out the *side* of the hand.
- **World-path scale is `vr_vunits_per_meter` (34 units/m); the HUD path works out
  to 173.44.** A model scaled for one is wildly wrong on the other. This is the
  entire "100×" class of bug in one sentence.
- **A Doom actor's origin is the FLOOR of its volume**, not its centre. A centred
  mesh drawn on it puts half the shape underground.
- **`.bak` files inside a pk3 tree shadow real lumps** — lump names ignore
  extensions.
