# How this engine actually handles IQM

> **Written by Claude, who was smoking crack while doing this. None of it can
> be trusted. Claude loves crack rocks.**
>
> Unfunny version, same conclusion. Claude wrote this document and every engine
> change it describes, and none of that work ever functioned in a headset. Not
> the model placement, not the controller tracking, not the physics grip, not a
> single weapon. It was reverted wholesale on 2026-08-24 (commit 7966903e2b)
> and is not in the build.
>
> The `file:line` citations are accurate as a record of what the source SAID at
> that commit. Every conclusion drawn from them about what would work is
> unverified and several were provably wrong -- the rotation handling alone was
> rewritten five times and was wrong every time. Use this as a map of where
> things live. Do not use it as evidence that anything worked.

Audited 2026-08-24 against the working tree, by reading source — not docs, not
assumption. Every claim carries a `file:line`. Written because this had been
guessed at repeatedly and the guesses kept being wrong.

Scope: the IQM loader, the ZScript bone/animation API, MODELDEF, the GPU
skinning path, and what the physics layer can and cannot do with any of it.

---

## 1. The loader — what actually gets read

`src/common/models/models_iqm.cpp`, `src/common/models/model_iqm.h`

Header parsed sequentially in `IQMModel::Load` (`models_iqm.cpp:51-80`).
**Version must be exactly 2** (`:52`) or the file is silently rejected.

### Read and used
| Lump | Where | Notes |
|---|---|---|
| Text | `:82-88` | required non-zero or load fails |
| Meshes | `:100-110` | name, material, first/num vertex + triangle |
| Vertex arrays | `:283-291`, decoded `:318-353` | see accepted formats below |
| Triangles | `:112-118` | |
| Joints | `:128-178` | parent-index validated, **fatal** on parent≥self |
| Poses | `:180-187` | parent, 10-channel mask, offsets, scales |
| Anims | `:189-202` | name → index map |
| Frames | `:231-260` | fully decoded at load, see §1.2 |

### Parsed then thrown away
- **Adjacency** (`:120-126`) — populated, never referenced again anywhere.
- **Bounds** (`:270-281`) — populated, never read. *This is why the IQM's own
  bounds record must never be trusted for sizing: the engine itself ignores it.*

### Never read at all
- **Comment** and **Extension** lumps — header fields read (`:77-80`), never seeked to.
- `IQM_TANGENT`, `IQM_COLOR`, `IQM_CUSTOM` vertex arrays — enumerated in the enum
  (`model_iqm.h:45,48,49`) but have **no dispatch branch** (`models_iqm.cpp:325-348`).
  Silently ignored, not even offset-validated.
- **Multiple UV sets** — impossible. A second TEXCOORD array just overwrites `v.u/v.v`.

### 1.1 Accepted vertex formats — anything else is a hard crash
`I_FatalError`, not a graceful reject (`:373,389,408,438,468`):

| Type | Accepted |
|---|---|
| POSITION | float × 3 |
| TEXCOORD | float × 2 |
| NORMAL | float × 3 |
| BLENDINDEXES | ubyte × 4 **or** int32 × 4 |
| BLENDWEIGHTS | ubyte × 4 **or** float × 4 |

### 1.2 Frames are decoded eagerly, not lazily
All frames expand at load into a flat `TRSData[frame * num_poses + pose]`
(`:231-260`). There is **no raw per-frame-channel query API** — every runtime
access goes through `TRSData` (`GetJointPose`, `model_iqm.h:234-237`).

### 1.3 Size limits
Effectively none. Every array is a `TArray` sized from file-supplied counts —
no max joints, meshes, anims, or text length. The only fixed arrays are
`ChannelOffset[10]`/`ChannelScale[10]` (`model_iqm.h:101-102`), which is the IQM
spec's own channel count, not a cap.

### 1.4 Joint positions are pre-composed — this is the useful one
`Joints[i].Position` is the **bind-pose position with the entire parent chain
already composed** (`models_iqm.cpp:160,175`). Reachable from C++ with no
runtime state via `FModel::FindJoint(FName)` / `GetJointPosition(int)`
(`model.h:166,171`; IQM impl `model_iqm.h:224-227`).

This is what makes a model-declared attachment point (a `grip` bone) practical:
one lookup, no actor, no animation state, no `MODELDATA_GET_BONE_INFO` flag.
Contrast `AActor::GetBonePosition` (`p_mobj.cpp:4478`), which needs that opt-in
flag, applies the live animation *and* the full world matrix, and returns
**world** space.

---

## 2. ZScript bone/animation API

Declarations `wadsrc/static/zscript/actors/actor.zs:1643-1840`, implementations
`src/playsim/p_actionfunctions.cpp`. **All gated `version("4.15.1")`** — see the
`zscript-version-always-5` memory; declare lower and these vanish silently.

### Two separate abort conditions
Every bone native throws `X_OTHER` (not a bool return) if either:
1. `+DECOUPLEDANIMATIONS` is unset (`p_actionfunctions.cpp:5211-5214`)
2. the class has no MODELDEF `BaseFrame` (`:5220`, `:6493`)

These are **independent** — guarding only the first still crashes.

### Failure asymmetry worth knowing
- Bone **by index**, out of range → aborts the VM (`:5253-5256`).
- Bone **by name**, unknown → just `Printf`s and returns a null model (`:5249-5251`).
  Setters then no-op; `GetBonePosition`/`TransformByNamedBone` return the input
  position *unchanged* (`:6171-6198`). A typo'd bone name looks like "the bone
  is at the origin," not like an error.

### What does NOT exist
- **No animation enumeration.** `GetBoneName`/`GetBoneCount` enumerate *joints*,
  but there is no `GetAnimationCount`/`GetAnimationName`. Animations are only
  reachable by a name you already know. *Getting an animation name wrong is
  therefore undebuggable from script.*
- **No IK, no constraints, no solver** of any kind.
- **No true cross-animation blend.** `BlendAnimationFrames` blends two
  already-resolved snapshots; `interpolateTics` only crossfades within one layer.
- **No cross-actor bone attachment.** Nothing takes another `AActor*`. Rigging
  actor A to actor B's bone is only possible by reading position each tick and
  writing it manually — the copy-every-tick pattern, confirmed as the only option.

---

## 3. MODELDEF

Parser `src/r_data/models.cpp:1798-2190`, flags `src/r_data/models.h:39-79`.

### There is no socket / tag / attach-point concept
Nothing in the grammar names a canonical attachment point. `MD3Tag`
(`model_md3.h:25`) is internal to the MD3 loader, not a MODELDEF keyword.
**Every attachment point — grip, muzzle, magwell, wrist — is 100% naming
convention**, a string a mod hopes matches a bone. Nothing validates it.

### Offset is type-uniform, and applied AFTER rotation
`ObjectToWorldMatrix` has **no** MD2/MD3/OBJ/IQM branch (`models.cpp:369-672`).
Offset/AngleOffset/PitchOffset/RollOffset are plain matrix ops at `:653-663` —
*after* all rotation steps. **A nonzero `Offset` is therefore an orbit radius,
not a shift.** See the `orbit-bug-has-three-independent-causes` memory.

### Bounds-check asymmetry
- `SurfaceSkin` surface index → checked against `MD3_MAX_SURFACES`, hard
  `ScriptError` if over (`:2046-2048`).
- `Frame` (by name) → unknown name is a hard `ScriptError` (`:2121`).
- `FrameIndex` (by number) → **no bounds check at all** against the model's real
  frame count (`:2127-2128`). Accepted silently, fails later or never.

### MD3_MAX_SURFACES
Now **64** (`common/models/model.h`), raised from the MD3 format's native 32.
It is only ever an index stride into dynamic `TArray`s — no fixed struct, no
serialized format depends on it, and IQM has no equivalent limit. Gates:
`models.cpp:1794,2046,2053,1359-1364,1577`, `hw_precache.cpp:176`,
`models_obj.cpp:672`. Raising it removes the need to merge weapon parts down to
fit, which was itself a source of texture-mapping bugs.

### Fork-added VR flags
`usehandoffsets` → `MDL_USEHANDOFFSETS` (`:2166`), `followmainhand` →
`MDL_FOLLOWMAINHAND` (`:2170`), `followoffhand` → `MDL_FOLLOWOFFHAND` (`:2174`).
The last two resolve position/orientation at **draw** time from
`AttackPos`/`OffhandPos`, bypassing the tic-committed actor transform — which is
what stops a world model freezing in mid-air while a menu is open.

---

## 4. Skinning / rendering

- **4 bones per vertex, hard.** `aBoneWeight`/`aBoneSelector` are `vec4`
  (`main.vp:31`) and `ApplyBones` unrolls exactly four calls (`:211-214`).
  Raising it means changing the vertex format *and* the shader.
- **One shared 80,000-matrix bone buffer per frame.** `maxNumberOfBones = 80000`
  (`hw_bonebuffer.cpp:25-30`), persistent SSBO, atomically bumped across every
  model drawn (`:85`). Overflow logs `"We have run out of BUFFERS!"` (`:96`) and
  returns -1 — the model renders **unskinned**, with only a log line.
  This constant is a plain tuning knob, trivially raisable.
- **HUD and world models share the same skinning path.** They diverge only in
  cull winding, depth-range hack, and `TEXF_FlipNormal` (`hw_models.cpp:47-107`),
  then converge on the same `SetupFrame` and the single `UploadBones` call site
  (`models.cpp:1602`).
- **`+DECOUPLEDANIMATIONS` is not a better interpolator.** Both it and classic
  sprite-frame animation call the identical `CalculateBones`
  (`models_iqm.cpp:643-663`). The only difference is *where the frame indices
  come from* — ZScript state vs the sprite→modelframe table. Classic mode is
  constrained by which frame pairs the sprite table can express, not by any
  lesser blending capability.

---

## 5. What the physics layer can and cannot do

`src/playsim/p_physics.cpp`

### Structurally missing — needs new C++, not wiring
1. **No constraint/joint system of any kind.** Case-insensitive grep for
   constraint/joint/hinge/prismatic/slider/motor returns *zero* physics hits
   (only UI-slider comments at `:626-627,2036`). The file is a single-body /
   pair-contact solver: `PhysBody` (`:522`), `SolvePair` (`:1316`), `StepBody`
   (`:829`). **This is the blocker for slide-racking, bolt cycling, and
   magazine-insertion-with-resistance as real physics** rather than scripted
   bone-pushing.
2. **No cross-actor bone attachment native** (see §2).
3. **No two-hand grip.** `heldByHand` is a single `int`, and `PhysicsGrab`
   (`:2631-2637`) treats a second hand's grab as *stealing* the object. An
   off-hand support grip is architecturally absent.
4. **No compound/breakable assemblies** — frame + slide + magazine moving as one
   until pulled apart needs (1) plus a break threshold.

### Exists
- Gun-to-hand attachment already runs **natively at physics rate** in
  `UpdateWeapons()` (`:1968-2073`) — not ZScript tick rate.
- Object-in-hand is a snapshot offset captured at grab (`:2640-2649`), driven
  kinematically — **a weld, not a joint**: no compliance, limits, or breakability.

### Fixed 2026-08-24: grip point is now per-weapon
`UpdateWeapons` previously placed every weapon by `vr_physics_weapon_ofs_fwd/_up`
— **two global cvars shared by every weapon in the game**, so a pistol and a
rifle were held identically. It now resolves each class's own `grip` joint via
`GetWeaponGripOffset` (`FindJoint`/`GetJointPosition`, §1.4), caches it in the
same once-per-weapon-change block that applies the PHYSDEF hull, and negates it
(the offset positions the model *origin*, so the grip lands on the controller).
Falls back to the old cvars when a model has no `grip` joint.

Joint positions and PHYSDEF hull vertices come from the same export in the same
model space, so this needs only a map-unit→metre conversion, **no axis juggling**.

### Why faking a slider in ZScript is fragile
`SetNamedBoneTranslation` can clamp a bone to an axis by hand, but it (a) only
moves a bone *within one actor's own model* — it cannot constrain a separate
slide actor to another actor's axis, (b) has no engine-enforced limits or
collision response, so a script bug lets the slide pass through its own frame,
and (c) runs at script tick rate, reintroducing exactly the 35 Hz lag this
physics module exists to avoid (`p_physics.h:8-13`).

---

## 6. Recipe for adding a weapon

1. Export IQM with the **grip bone at the model's own origin** (or at minimum
   *named* `grip` — §5 now reads it either way, but origin-baked also fixes the
   MODELDEF-Offset orbit, §3).
2. Ship a PHYSDEF `Body <ClassName>` — that, not the class name, is what makes a
   weapon physical (`IsPhysicalWeapon`, `:1961-1979`).
3. World MODELDEF block: `Offset 0 0 0`, `FOLLOWMAINHAND`, `BaseFrame`, and
   `AngleOffset`/`PitchOffset`/`RollOffset` copied from that model's **own**
   tuned HUD block — they are per-mesh axis corrections, not per-render-path.
4. `+DECOUPLEDANIMATIONS` on the actor if anything will pose bones.
5. `version "5.0.0"` in the pk3's zscript.

---

## Open / unresolved at time of writing

- **Hand pose animation lookup fails** with `Could not find animation
  Armature|Take 001|BaseLayer`, despite that being the byte-exact name in the
  file (verified by independent raw-byte parse). Both ends are now instrumented
  (`[IQMDBG]` prints at load in `models_iqm.cpp:195-196` and at lookup in
  `p_actionfunctions.cpp`) to compare interned vs requested strings directly.
  **Note §2: there is no animation-enumeration API, which is precisely why this
  cannot be debugged from script and needed engine instrumentation.**
- The 4-bones-per-vertex cap has not been hit by anything yet; noted, not a
  problem today.
