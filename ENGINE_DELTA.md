# UZDXREMA — Engine Delta

**From the UZDoom 5.0 merge to now.** A code-sourced account of every change in this window,
followed by an assessment of what it takes to reach the interaction targets.

---

## 1. Scope and method

| | |
|---|---|
| Baseline | `1d2572bdcc` — *Merge UZDoom 5.0.0-rc.2 into UZDXREMA*, 2026-08-16 |
| Head | `main` @ `026d2a8a80`, plus one uncommitted working-tree change |
| Delta | **56 commits**, **80 files**, **+8,478 / −29,099** |

```
git diff 1d2572bdcc main --stat
```

The deletion count is dominated by three generated parser artifacts —
`zcc-parse.out` (−21,870), `zcc-parse.c` (−6,652), `zcc-parse.h` (−161) — plus two language
CSVs. Real content change is roughly 74 files.

### Lineage, and who wrote what

Ermac's and Emawind's DXR was brought up to UZDoom 5.0; that merge is this document's
baseline. **The VR device and controller layer — OpenXR session, OpenVR fallback, input
actions, controller poses, haptics — is their work and predates this window.** The same
feature set exists in both DXR and stock UZDoom 5.0. Where this document covers those files
(§7) it covers *only what changed inside this window*, and says plainly whether that change is
new capability or reconciliation against 5.0's refactor.

Everything else here is first-party work done in this window.

### Evidence rules

Every technical claim cites `file:line` and was read out of source.

**No `.md` file in the repository was used as a source.** `FORK_CHANGES.md`, `CHANGES.md`,
`BILLBOARDS.md`, `REFLECTION_SPEC.md`, `HUD_STEREO_GATING.md` and `README.md` are prior-session
prose; they appear below only as *artifacts that changed size*, never as evidence.

**Code comments are not evidence either.** Where a comment is quoted it is labelled as an
author's claim. Where a comment's claim was checkable, the code was checked and the code is
what is reported.

Every section was written by one pass and then adversarially fact-checked by a second pass that
re-opened each cited line. Corrections from that pass are applied. Nothing here required
building or launching the engine.


## Contents

1. [Scope and method](#1-scope-and-method)
2. [Rigid-body physics — the native VR interaction layer](#2-rigid-body-physics-the-native-vr-interaction-layer)
3. [Models, bones and IQM rendering](#3-models-bones-and-iqm-rendering)
4. [Weapon and psprite rendering, hands](#4-weapon-and-psprite-rendering-hands)
5. [Shaders and postprocess](#5-shaders-and-postprocess)
6. [Script API surface](#6-script-api-surface)
7. [VR device layer — this window’s changes only](#7-vr-device-layer-this-windows-changes-only)
8. [Input, menus and build](#8-input-menus-and-build)
9. [Gap inventory](#9-gap-inventory)
10. [Capability assessment](#10-capability-assessment)
11. [Path forward](#11-path-forward--zdoom-vr-with-high-fidelity-guns)
12. [Current working state](#12-current-working-state)
13. [Appendix — all 80 changed files](#13-appendix--all-80-changed-files)

---

## 2. Rigid-body physics — the native VR interaction layer

### Provenance

`src/playsim/p_physics.cpp` and `src/playsim/p_physics.h` do not exist at the baseline. `git show 1d2572bdcc:src/playsim/p_physics.cpp` returns nothing, and `git log --follow` on the file shows no history before the window's first commit. This is a wholly new module, not an extension of pre-existing code — 2,555 lines of `.cpp` and 61 lines of `.h`, all of it added between `1d2572bdcc` and `main`.

Ten commits build it up, all authored 2026-08-22 04:00 → 2026-08-23 11:51 (`-0700`), i.e. entirely within the last ~32 hours relative to the stated current date:

| commit | when | what it added |
|---|---|---|
| `2838cf0e16` | 08-22 04:00 | the D_DoomLoop hook, clock/accumulator |
| `25c0d15ac9` | 08-22 04:57 | box bodies, gravity, floor/ceiling/wall contact, hold/throw plumbing |
| `d24cafc3dc` | 08-22 05:39 | body-vs-body (`SolvePair`), displacement-based sleep |
| `08df006c31` | 08-22 05:54 | hand solidity, `comOffset` |
| `d670190b0f` | 08-23 05:55 | `PhysicsGrab` / the hand-carry loop |
| `1fc8715fa2` | 08-23 06:03 | hand-history ring buffer, peak-of-swing throw |
| `fca0c0d16f` | 08-23 09:18 | `UpdateWeapons` (held-weapon solidity) |
| `782abd2ffa` | 08-23 09:23 | `PhysHull` — box generalised to a convex-hull set |
| `69a6e440ce` | 08-23 09:51 | `LoadPhysDefs` / `ApplyPhysDefShape` (PHYSDEF lump) |
| `fe787e9b8e` | 08-23 11:51 | hand yaw switched from `Angles.Yaw` to `AttackAngle` |

`git status --porcelain` at review time shows **no uncommitted changes** to either `p_physics.cpp` or `p_physics.h` — both are fully committed as of `fe787e9b8e`. The only modified file in the working tree is `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, which is outside this subsystem.

**Attribution boundary.** The module consumes per-hand pose fields — `AttackPos`, `AttackAngle`, `AttackPitch`, `MainHandRoll`, `OffhandPos`, `OffhandAngle`, `OffhandPitch`, `OffhandRoll` — that already existed on `AActor` at baseline (`git show 1d2572bdcc:src/playsim/actor.h:1731-1761`). The plumbing that fills those fields from the OpenXR/OpenVR device layer predates this window; what's new here is only the *consumption* of them by a new solver.

### The call site: `d_main.cpp`

`P_PhysicsFrame()` is called from exactly one place in the entire tree (confirmed by a repo-wide grep for the four public entry points — each has exactly one caller, matching the four files that declare them). It sits inside `D_DoomLoop`, between `TryRunTics()` and `I_StartTic()`/`D_Display()`:

```cpp
// src/d_main.cpp:1879-1898
TryRunTics (); // will run at least one tic

// RS FORK -- VR object physics, stepped at frame rate.
//
// Here rather than in P_Ticker because the playsim runs at 35Hz and
// a held object updated at 35Hz lags the hand by up to 28ms. In VR
// TryRunTics deliberately does not wait (the compositor owns
// pacing), so this loop free-runs at headset rate and frequently
// runs zero tics -- which is exactly the rate a held object needs.
//
// Here rather than in a render backend because vid_preferbackend
// defaults to OpenGL and falls back to it silently on Vulkan
// failure, and because screen wipes -- every level transition --
// never reach screen->BeginFrame() at all.
P_PhysicsFrame();

// Update display, next frame, with current state.
I_StartTic ();
D_ProcessEvents();
D_Display ();
```

The same rationale is duplicated verbatim in `p_physics.h:39-48`. Two design facts follow directly from this placement, independent of the comment's claims: (1) `P_PhysicsFrame` runs once per iteration of the outer render loop, not once per game tic, so at a 90 Hz headset it runs far more often than the 35 Hz playsim ticks; (2) it runs even on frames where `TryRunTics` advanced zero tics, which `p_physics.h:18-21` states is common in VR and is why `AActor::Tick` cannot be relied on as a per-frame hook for this module.

Two more unrelated changes ride in the same `d_main.cpp` diff (`Args->AppendRawArg` replacing `AppendArg` for command-line-file tokens, `oh_reload`/`mh_reload` renamed to `oh_dropmag`/`mh_dropmag`, an unconditional `doomxr-log.txt` log-file open, and a GC::Root shutdown diagnostic) — none of these touch the physics module and are out of scope for this section.

### The clock

```cpp
// src/playsim/p_physics.cpp:1935-1956
void P_PhysicsFrame()
{
	const uint64_t nowNs = I_nsTime();

	if (g_lastTimeNs == 0 || nowNs < g_lastTimeNs)
	{
		g_lastTimeNs = nowNs;
		g_reportStartNs = nowNs;
		g_ticsAtReport = gametic;
		return;
	}

	double dt = (nowNs - g_lastTimeNs) / 1000000000.0;
	g_lastTimeNs = nowNs;

	const double kMaxFrame = 0.25;
	if (dt > kMaxFrame) dt = kMaxFrame;
	if (dt < 0.0) dt = 0.0;
```

The very first call after process start (or after any backward jump in `I_nsTime()`) does nothing but resync the clock and returns — no stepping, no `UpdateHands`. Every call after that computes a real `dt` from a monotonic nanosecond clock, clamped to 250ms (`kMaxFrame`) so a stall (disk I/O, a debugger break, a level-load hitch) cannot inject one enormous accumulator jump.

`vr_physics_hz` (`p_physics.cpp:77-81`, `CUSTOM_CVAR(Int, ..., 90, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)`, clamped to [30,240]) sets the fixed step size `step = 1.0 / vr_physics_hz`. `dt` accumulates into `g_accumulator`, and a `while (g_accumulator >= step && steps < maxSteps)` loop (`p_physics.cpp:1979-2028`) drains it in fixed increments, where `vr_physics_maxsteps` (`p_physics.cpp:83-87`, clamped [1,16], default 4) bounds how many fixed steps one frame may run — the catch-up cap after a hitch. If the accumulator still holds a full step after `maxSteps` steps have run, the remainder is discarded outright rather than carried into the next frame, and a counter is incremented:

```cpp
// src/playsim/p_physics.cpp:2030-2034
if (g_accumulator >= step)
{
	g_dropped++;
	g_accumulator = 0.0;
}
```

`ShouldStep()` (`p_physics.cpp:745-752`) gates whether any of this runs at all: `gamestate == GS_LEVEL`, a non-null `primaryLevel`, not `paused`/`pauseext`, and `menuactive == MENU_Off`. When it's false, `g_accumulator` is zeroed outright (`p_physics.cpp:1962`) and neither `UpdateHands` nor `UpdateWeapons` run — hand/weapon/held-object bodies simply stop being refreshed while paused or in a menu. On the transition back to `true`, `if (!g_running) g_accumulator = 0.0;` (`p_physics.cpp:1966`) discards whatever had built up so resuming doesn't fire a burst of catch-up steps.

### `PhysBody` — every field

`p_physics.cpp:517-643`. All positional/kinematic quantities are in **metres**, converted at the actor boundary via `MapToM`/`MToMap` (`p_physics.cpp:719-725`):

```cpp
inline float MapPerMetre() { float s = (float)*vr_vunits_per_meter; return (s > 0.01f) ? s : 34.f; }
inline float MapToM(double mapUnits) { return (float)(mapUnits / MapPerMetre()); }
inline double MToMap(float metres)   { return (double)metres * MapPerMetre(); }
```

`vr_vunits_per_meter` is a pre-existing CVar (`hw_vrmodes.cpp:635`, default `34.0f`) — the `34.f` fallback here is not an arbitrary guess, it mirrors that CVar's own default for the case where it reads as ≤0.01.

| field | type | meaning |
|---|---|---|
| `owner` | `AActor*` | null for hands (bodies with no actor) |
| `handIndex` | `int` | −1, or 0=main/1=off for a hand body |
| `weaponHand` | `int` | −1, or which hand's weapon-collider this is (distinct from `handIndex`) |
| `pos` | `FVector3` | metres, centre of mass, playsim axes (Z up) |
| `vel` | `FVector3` | m/s |
| `rot` | `Quat` | orientation |
| `angVel` | `FVector3` | rad/s |
| `half` | `FVector3` | box half-extents, metres — the fallback shape |
| `hulls` | `TArray<PhysHull>` | the real collision shape; ≥1 entry always |
| `boundRadius` | `float` | furthest vertex from body origin, over all hulls — broadphase |
| `thinHalf` | `float` | narrowest half-thickness over all hulls — CCD substep sizing |
| `comOffset` | `FVector3` | centre-of-mass offset from the actor's origin, body space, metres |
| `invMass` | `float` | 1/kg |
| `invInertia` | `FVector3` | body-space diagonal inverse inertia tensor |
| `impactSound` | `FSoundID` | landing sound, `NO_SOUND` if silent |
| `impactMinSpeed` | `float` | m/s floor below which contacts are silent |
| `impactCooldown` | `float` | seconds, rate-limits repeated impact sounds/haptics |
| `kinematic` | `bool` | true = solver does not integrate it; pose driven externally |
| `heldByHand` | `int` | −1, or which hand currently holds it |
| `grabPosOffset` | `FVector3` | captured at grab time, hand-local space |
| `grabRotOffset` | `Quat` | captured at grab time, hand-local space |
| `sleepTimer` | `float` | seconds spent under both drift thresholds |
| `traceTimer` | `float` | debug-log rate limiter |
| `supportTimer` | `float` | seconds still counted as "resting" since the last real contact |
| `velEMA` / `angEMA` | `FVector3` | exponential moving average of vel/angVel — diagnostic only |
| `sleepRefPos` / `sleepRefRot` | `FVector3`/`Quat` | pose last time the body was judged possibly-settling |
| `asleep` | `bool` | integration skipped entirely when true |
| `restReported` | `bool` | one-shot latch for the `[PHYS] rest` log line |

`ShapeFinish()` (`p_physics.cpp:551-576`) is the method that keeps `boundRadius`/`thinHalf` in sync after `hulls` is touched: if `hulls` is empty it manufactures a single box hull from `half` via `HullMakeBox` (so an unshaped body behaves exactly as a plain box), otherwise it walks every hull's vertices to find the furthest point (accounting for hulls offset from the body origin, which a compound shape needs) and takes the minimum `thinHalf` across hulls.

### Shapes: `PhysHull`, PHYSDEF

`PhysHull` (`p_physics.cpp:298-316`) stores **both** a vertex list and a face-plane list for the same convex piece, in body space relative to the centre of mass — vertices answer "where does this touch the world" (tested against floor/ceiling/wall planes and against another hull's planes), planes answer "is this point inside me" (`HullDeepest`, `p_physics.cpp:377-398`, returns the shallowest-penetration face or `false` if the point is outside every face). `radius` is furthest-vertex distance (broadphase); `thinHalf` is the smallest face offset, i.e. half the narrowest face-to-face thickness — CCD substep sizing.

`HullMakeBox` (`p_physics.cpp:321-344`) builds 8 corners and 6 axis-aligned planes from a half-extent vector — every body that never received real geometry is exactly this. `HullFinish` (`p_physics.cpp:348-367`) recomputes `radius`/`thinHalf` after real geometry (PHYSDEF or otherwise) has been written into `verts`/`planes` directly.

**PHYSDEF format** (`p_physics.cpp:400-508`, `LoadPhysDefs`), one `PHYSDEF` lump (or several, concatenated across pk3s via `fileSystem.FindLump` iteration), parsed once and cached in `TMap<FName, PhysShapeDef> g_shapeLib` keyed on actor class name:

```
Body <ClassName>
{
    Hull
    {
        V  x y z        // a vertex, body space, metres, relative to the actor's origin
        P  nx ny nz d    // a face plane, outward normal, inside when n·p <= d
    }
    Hull { ... }         // more than one hull = a concave shape (a magwell cavity)
}
```

A hull parsed with zero verts or zero planes is dropped with a warning (`p_physics.cpp:485-489`, `Printf("...dropped\n")`) rather than shipped silently broken; a body with at least one valid hull is inserted into `g_shapeLib` and counted (`p_physics.cpp:498-502`). `LoadPhysDefs` is lazily triggered on first use, not at startup (`p_physics.cpp:654`, `if (!g_shapesLoaded) LoadPhysDefs();` inside `ApplyPhysDefShape`).

`ApplyPhysDefShape` (`p_physics.cpp:652-677`) copies the matched `PhysShapeDef`'s hulls into the body and re-origins every vertex and every plane's offset by `-comOffset` — because PHYSDEF vertices are authored relative to the actor's origin (natural for an export tool) but the solver works in centre-of-mass space. The plane-offset correction (`pl.W -= (pl.X*comOffset.X + ...)`, `p_physics.cpp:668-673`) is explicitly called out in comment as the step that's easy to get wrong ("Forgetting this leaves the faces where the actor's origin was while the vertices move").

**No PHYSDEF lump content ships anywhere in this repository's diff.** The loader and the `Body`/`Hull` grammar are complete and load from any pk3 in the search path, but this window adds zero `.physdef`-bearing files, so within this tree alone `ApplyPhysDefShape` always returns `false` (`g_shapeLib` stays empty) and every `PhysicsEnable`'d body falls back to the box. The comment at `p_physics.cpp:419-421` claims the hull data is "generated from the mesh by a tool, never hand-authored" — that is a comment-claim about an external tool not present in this diff, not something verified here.

### Integration: `StepBody`

`p_physics.cpp:806-1268`. Early-outs, in order: no owner (hand bodies never reach here directly — see below), `asleep`, then **`if (b.kinematic) return;` at `p_physics.cpp:814`, before any contact collection, before any world-geometry query of any kind.** This single line is the structural reason kinematic bodies (hands, weapons, currently-held objects — every one of them constructed with `kinematic = true`) never touch the floor/ceiling/wall collision code at all; see the Hands section below for the full chain of evidence.

For a genuinely free body: gravity (`vr_physics_gravity`, default 9.81, `p_physics.cpp:821`) and linear/angular damping (`vr_physics_lineardamp`/`vr_physics_angulardamp`, `p_physics.cpp:826-829`) are applied, then position is Euler-integrated and orientation by the standard `q += 0.5·ω·q·dt` quaternion update (`p_physics.cpp:833-844`).

**Contact collection**, all in one pass, stored in fixed local arrays capped at `kMaxContacts = 96` (`p_physics.cpp:858-860`) — every hull vertex of the body is transformed to world space once (`hullWorld`, `p_physics.cpp:869-876`, a function-local `static TArray` reused every call to avoid a per-frame heap allocation) and reused for both the floor/ceiling pass and the wall pass:

- **Floor/ceiling** (`p_physics.cpp:878-915`): each world-space hull vertex is tested against `sec->floorplane.ZatPoint`/`sec->ceilingplane.ZatPoint` for the body's **own sector**, evaluated live — so slopes and moving floors (lifts) work with no special-case code, per the surrounding comment.
- **Walls** (`p_physics.cpp:917-1001`): tested against `sec->Lines`, i.e. the linedefs of the body's own sector — explicitly **not** `DoomLevelMesh` (the render geometry), because, per the comment at `p_physics.cpp:919-925`, the render mesh omits two-sided lines and any line with no texture, which would let objects fall through untextured steps. A line counts as fully blocking if one-sided or `ML_BLOCKING` (`p_physics.cpp:955-956`); otherwise it's solid only outside the vertical gap between the higher of the two floors and the lower of the two ceilings (`p_physics.cpp:972-986`) — the step/window distinction. The reach for candidate lines is `boundRadius + 2.0` map units around the body's centre (`p_physics.cpp:931`).

  **"Own sector only" is a stated, verified limitation.** The code's own comment (a comment-claim, quoted verbatim): *"Only the body's own sector's lines are considered. A box a few centimetres across cannot reach past them, and it avoids a blockmap query per body per step."* (`p_physics.cpp:927-929`). There is no blockmap or neighbour-sector query anywhere in this file — a body whose bounding radius exceeds the distance to a line in an *adjacent* sector will not see it.

  **A second, independently-verified staleness limitation, not stated in any comment:** the `sector_t *sec = a->Sector` read at the top of `StepBody` (`p_physics.cpp:816`) is re-read fresh on every call, but the value it reads is only ever updated by `WriteBack`'s call to `a->LinkToWorld(&ctx)` (`p_physics.cpp:1837-1840`), which resolves the sector via `Level->PointInSector(Pos())` when none is passed explicitly (`src/playsim/p_maputl.cpp:424-436`, `Sector = sector;` at line 436). `WriteBack` runs **once per `P_PhysicsFrame` call, after the entire fixed-step `while` loop has finished** (`p_physics.cpp:2036-2040`), not after each step. So every `StepBody` call within one frame — across up to `vr_physics_maxsteps` fixed steps, each itself possibly split into up to 16 CCD substeps — tests against the *same* `Sector` snapshot, taken at the end of the *previous* frame. A body that physically crosses a sector boundary partway through a multi-step frame keeps colliding against the old sector's floor/ceiling/lines until the next frame's `WriteBack` catches up. In the common case (one fixed step per frame, which holds whenever the headset frame rate is at or above `vr_physics_hz`) this self-corrects every ~11ms at the default 90Hz and is unlikely to be visible; it becomes a real window during a frame-time stall that forces multiple catch-up steps.

**Impact sound** (`p_physics.cpp:1005-1029`): driven by the hardest `-initialVn` among this step's contacts, gated by `impactMinSpeed`, rate-limited by `impactCooldown = 0.08f`.

**Solve** (`p_physics.cpp:1031-1136`): sequential impulses, `kSolverIterations = 8` (`p_physics.cpp:184`), Baumgarte stabilisation (`kBaumgarte = 0.15`, `p_physics.cpp:199`) biasing the velocity target rather than teleporting position, restitution (`vr_physics_restitution`, default 0.32) applied only above `kRestitutionThreshold = 0.5` m/s (`p_physics.cpp:204`) so a resting body's micro-contacts don't re-inject energy, normal impulse accumulated and clamped non-negative across iterations (`p_physics.cpp:1073-1076`), then a second loop applying Coulomb friction (`vr_physics_friction`, default 0.7) using the normal impulse accumulated so far. A separate **torsional-friction approximation** (`p_physics.cpp:1109-1135`) adds extra angular damping (`vr_physics_contactspindamp`, default 4.0) while in contact, faded out with speed so a thrown object's tumble on impact isn't killed — explicitly called an approximation in comment, standing in for a real contact-patch manifold.

**Sleep** (`p_physics.cpp:1138-1234`) is decided by **displacement, not velocity** — the file's own comment states this is the third attempt and that the first two (velocity-threshold based) failed because a resting body under constant gravity oscillates at a fixed non-zero instantaneous speed (`g/rate` = 0.109 m/s at 90Hz) that no threshold separates from real drift; that account is a comment-claim I have not independently re-derived, but the implemented mechanism itself is directly verifiable: `supportTimer` is latched to `kSupportGrace = 0.2s` on any contact and decays otherwise (`p_physics.cpp:1151-1156`); `velEMA`/`angEMA` are exponential moving averages of the *vectors* (not magnitudes) at `k=0.12` (`p_physics.cpp:1171-1175`) but are diagnostic-only and do not gate sleep; the actual gate is `driftLin = |pos - sleepRefPos|` and a quaternion-dot-derived `driftAng`, both required under `kSleepDrift = 0.004m` / `kSleepDriftAngle = 0.05rad` for `kSleepTime = 0.3s` while `supportTimer > 0` (`p_physics.cpp:1195-1226`). On first settling, `[PHYS] rest ...` is logged once (`restReported` latch, `p_physics.cpp:1216-1224`).

**CCD substepping** (`p_physics.cpp:1985-2012`, inside `P_PhysicsFrame`'s step loop, not inside `StepBody` itself): per body, per fixed step, `sub = 1 + travel/(0.4·thinHalf)` clamped to [1,16], where `travel = |vel|·step`; `StepBody` is then called `sub` times with `subDt = step/sub`. This uses `thinHalf` (narrowest hull dimension), not `boundRadius`, specifically so a thin object (a cartridge, a slide) gets finer substeps than a compact object of the same bounding size — stated directly in comment and consistent with the code.

### Body-vs-body: `SolvePair`

`p_physics.cpp:1293-1524`. Gate: `if ((A.handIndex >= 0 || B.handIndex >= 0) && !*vr_physics_hands) return;` (`p_physics.cpp:1314`) — note this checks `handIndex` specifically, **not** `weaponHand`; a held weapon body (`weaponHand ≥ 0`, `handIndex == -1`) is never gated by `vr_physics_hands`, so turning "hands solid" off does not stop a held weapon from pushing/contacting other bodies.

Broadphase is a single bounding-sphere test, `distSq > (rA+rB)²` (`p_physics.cpp:1318-1322`), using `boundRadius` computed from real hull geometry.

Narrow phase is **corner-in-hull, both directions** (`p_physics.cpp:1341-1382`): for every vertex of every hull of body A, test it against every hull of body B via `HullDeepest` (taking the shallowest penetration across B's hulls), and the same with A/B swapped. This is explicitly **not** a full separating-axis test — the comment at `p_physics.cpp:1273-1282` states it misses pure edge-edge contact (two boxes crossed like an X) and calls this an accepted, deliberate simplification for the magazine/casing/hand/pistol object set. Testing every hull of the destination (not just the nearest) is what makes a compound shape's concavity (a magwell as a gap between convex slabs) actually work, per `p_physics.cpp:1334-1340`.

Contact with another body counts as support for sleep exactly like a floor contact does (`p_physics.cpp:1391-1392`), and a real approach (`-initialVn > kWakeOnImpact = 0.15` m/s, `p_physics.cpp:214`) wakes a sleeping body on either side. A **kinematic-vs-kinematic haptic** fires when two hand/weapon-tagged bodies (`handIndex` or `weaponHand`) belonging to *different* hands touch hard enough (`p_physics.cpp:1417-1441`, `VR_ScriptHaptic`, amplitude `hardest/3` clamped [0.2,1.0]) — this is explicitly the substitute for a push the solver cannot honestly apply (neither side has finite mass). The solver loop itself is the same sequential-impulse/Baumgarte/friction structure as `StepBody`'s, splitting the impulse by each side's `invMass`/`invInertia` and gating every `ApplyImpulse` call individually with `if (!X.kinematic)` (`p_physics.cpp:1486-1487`, `1515-1516`) — so two kinematic bodies always resolve to a no-op push (both guards fail) while still generating the contact used for the haptic. A `TODO` at `p_physics.cpp:1520-1523` states body-vs-body contacts are currently silent (impact sound is wired only to world-geometry contacts in `StepBody`).

### Constraint solver: none

A case-insensitive search of `p_physics.cpp` and `p_physics.h` for `constraint|joint|hinge|prismatic|ragdoll|motor` returns **zero matches** in either file. The only mechanisms present are: (1) non-penetration + friction contact resolution between a free body and world sector planes/linedefs, (2) the same between pairs of bodies, and (3) the hand-carry pose copy described below, which is a direct transform assignment, not a constraint in any solved sense. There is no hinge, no prismatic slider, no motor, and no ragdoll/articulated-body support anywhere in this module.

### Hands: `UpdateHands`

`p_physics.cpp:1544-1720`. Called once per `P_PhysicsFrame`, before the fixed-step loop, with the **frame's** `dt` (not the fixed `step`) (`p_physics.cpp:1971`).

**Existence and kinematic status, verified exactly:**

```cpp
// src/playsim/p_physics.cpp:1573-1583
if (b == nullptr)
{
	PhysBody nb;
	nb.handIndex = hand;
	nb.owner = nullptr;
	nb.kinematic = true;
	nb.invMass = 0.f;                       // immovable
	nb.invInertia = FVector3(0, 0, 0);
	g_bodies.Push(nb);
	b = &g_bodies[g_bodies.Size() - 1];
}
```

Both `kinematic = true` and `invMass = 0.f`/`invInertia = 0` are set — confirmed, matching the task's ask exactly. `UpdateWeapons` constructs weapon bodies identically (`p_physics.cpp:1786-1788`).

**Shape, verified exactly** — a box, sized from a CVar, never from any hand mesh or bone:

```cpp
// src/playsim/p_physics.cpp:1585-1588
const float s = (float)*vr_physics_handsize;
b->half = FVector3(0.045f * s, 0.030f * s, 0.090f * s);
b->hulls.Clear();
b->ShapeFinish();
```

Base half-extents (9cm × 6cm × 18cm full box), scaled uniformly by `vr_physics_handsize` (clamped [0.25,4.0], default 1.0). `hulls.Clear()` before `ShapeFinish()` guarantees the box path in `ShapeFinish` (`hulls.Size()==0` → `HullMakeBox`) runs every single call — a hand can never accidentally retain PHYSDEF geometry from a previous state.

**Pose write, verified exactly — direct assignment from tracked pose fields, with no geometry test anywhere in the path:**

```cpp
// src/playsim/p_physics.cpp:1590-1608 (pose sample)
const DVector3 p = (hand == 0) ? pawn->AttackPos : pawn->OffhandPos;
const FVector3 newPos(MapToM(p.X), MapToM(p.Y), MapToM(p.Z));
const double yaw   = (hand == 0) ? pawn->AttackAngle.Degrees()  : pawn->OffhandAngle.Degrees();
const double pitch = (hand == 0) ? pawn->AttackPitch.Degrees()  : pawn->OffhandPitch.Degrees();
const double roll  = (hand == 0) ? pawn->MainHandRoll.Degrees() : pawn->OffhandRoll.Degrees();
const Quat newRot = Quat::FromEulerDeg(yaw, pitch, roll);
...
// src/playsim/p_physics.cpp:1668-1674 (the write)
g_handPrevPos[hand] = newPos;
g_handPrevRot[hand] = newRot;
g_handHavePrev[hand] = true;

b->pos = newPos;
b->rot = newRot;
b->asleep = false;
```

`vel`/`angVel` in between are derived purely from the finite difference against last frame's pose (`p_physics.cpp:1623-1657`) — again no world query. **I searched the entire file for `Trace`, `Sweep`, `LineTrace`, `P_CheckPosition`, `P_TryMove`, and `Blockmap`/`blockmap`; the only hits are the unrelated debug-log feature `vr_physics_trace`/`traceTimer` and the comment at `p_physics.cpp:928` explaining why the wall pass avoids a blockmap query.** There is no raycast, sweep, or position-validity check of any kind on a hand's pose, anywhere in this file.

This is also structurally guaranteed rather than merely "not called here": `StepBody` (`p_physics.cpp:806-1268`) is the **only** function in the file that queries `sec->floorplane`/`sec->ceilingplane`/`sec->Lines`, and its very first substantive line is `if (b.kinematic) return;` (`p_physics.cpp:814`), which fires for every hand body before that code is ever reached. **Confirmed unambiguously: no geometry sweep touches the hand pose, by direct assignment and by the structural fact that the only geometry-testing function in the module refuses to run on any kinematic body.**

**The carry loop — held bodies, in the same function, immediately after the hand loop** (`p_physics.cpp:1677-1719`):

```cpp
// src/playsim/p_physics.cpp:1696-1715
// Rebuild the pose it had relative to the hand when it was grabbed.
Quat q;
q.w = hand->rot.w*h.grabRotOffset.w - ...
... // quaternion multiply, hand->rot * h.grabRotOffset
h.rot = q;
h.pos = hand->pos + hand->rot.Rotate(h.grabPosOffset);

h.vel = hand->vel + Cross(hand->angVel, h.pos - hand->pos);
h.angVel = hand->angVel;

h.asleep = false;
h.sleepTimer = 0.f;
```

**This overwrites `h.pos`/`h.rot` unconditionally — no penetration test, no collision query, no clamp against `A.kinematic` or anything else — for every body with `heldByHand ≥ 0`, every physics frame.** Since a held body is also `kinematic = true` (set in `PhysicsGrab`, `p_physics.cpp:2380`), `StepBody` never runs for it either. The consequence, confirmed by these two facts together: **a held object can be moved through world geometry (a wall, the floor) by the player's own hand motion, with zero push-back or resistance from the solver** — exactly the same as a hand or a held weapon, and unlike a free body of the same shape, which does get the full floor/ceiling/wall treatment once released. The object still participates in body-vs-body contact while held (`SolvePair` runs against it, since that loop iterates all pairs regardless of `kinematic`) — so a held magazine can push a free magazine sitting on the floor, and the haptic-on-touch path is deliberately scoped to hand/weapon pairs only, not to grabbed cargo (comment at `p_physics.cpp:1413-1416`) — but it cannot itself be blocked by *world* geometry while held.

`Cross(angVel, r)` for release velocity is applied here continuously (`h.vel = hand->vel + Cross(hand->angVel, h.pos - hand->pos)`, `p_physics.cpp:1714`) rather than as a one-shot calculation at release time — by the time `PhysicsRelease` runs, `b->vel` already reflects the hand's linear-plus-rotational motion. The same `Cross` form reappears in the release peak-search (`s.vel + Cross(s.angVel * spinF, r)`, `p_physics.cpp:2450`), scaled by `vr_physics_throwspin` (clamped [0,2], default 0.5) and evaluated per historical sample, not just at the current instant.

If the hand a body was held by disappears from under it (pawn destroyed, level change), the carry loop drops it rather than leaving it kinematic and frozen: `h.heldByHand = -1; h.kinematic = false;` (`p_physics.cpp:1691-1693`).

### Weapons: `UpdateWeapons`

`p_physics.cpp:1751-1819`. Detection, verified exactly:

```cpp
// src/playsim/p_physics.cpp:1743-1749
bool IsPhysicalWeapon(AActor *weap)
{
	if (weap == nullptr) return false;
	FString name = weap->GetClass()->TypeName.GetChars();
	name.ToLower();
	return name.IndexOf("t77") >= 0;
}
```

**Case-insensitive substring match on the class TypeName for `"t77"`, exactly as suspected** — this is a hardcoded single-class special-case: whatever weapon family is currently physical in this build is entirely defined by whether its class name contains those three characters, cross-pk3, with no compile-time reference and no data-driven list.

Collider, verified exactly — one box, dimensions from CVars, position offset from CVars, rebuilt only when the CVar-derived half-extents actually change (`if (wb->hulls.Size() != 1 || wb->half != wantHalf)`, `p_physics.cpp:1800-1805`) so a live menu slider doesn't discard real geometry 90 times a second:

```cpp
// src/playsim/p_physics.cpp:1793-1809
const FVector3 wantHalf(
	*vr_physics_weaponlen * 0.5f,
	*vr_physics_weaponwidth * 0.5f,
	*vr_physics_weaponheight * 0.5f);
...
const FVector3 localOfs(*vr_physics_weapon_ofs_fwd, 0.f, *vr_physics_weapon_ofs_up);
wb->rot = handBody->rot;
wb->pos = handBody->pos + handBody->rot.Rotate(localOfs);
```

Not mesh-derived — confirmed by absence of any model/bone reference in this function, and the code's own comment calls it "PLACEHOLDER SHAPE... sized and positioned from the vr_physics_weapon* cvars, not from the mesh" (`p_physics.cpp:1737-1740`, comment-claim, but the code itself independently corroborates it). Defaults: length 0.11m, width 0.02m, height 0.07m, offset (fwd 0.08m, up 0.02m) from the hand's tracked grip point.

`UpdateWeapons` requires a corresponding hand body to already exist (`if (handBody == nullptr) continue;`, `p_physics.cpp:1779`); its inline comment reads `// hands disabled -- nothing to attach to`, but the actual gate on hand-body *existence* is `wantHands = (pawn != nullptr)` in `UpdateHands` (`p_physics.cpp:1555`), not the `vr_physics_hands` CVar (which, per the `SolvePair` section above, only gates pushing/haptics, not existence) — the comment's wording is imprecise relative to what the code actually tests.

### Grab / release (native API side)

`PhysicsGrab` (`p_physics.cpp:2333-2392`) captures the object's pose *relative to the hand* at grab time — `grabPosOffset = h->rot.Inverse().Rotate(b->pos - h->pos)` (`p_physics.cpp:2368`) and the equivalent relative quaternion for `grabRotOffset` (`p_physics.cpp:2370-2377`) — so a grabbed object keeps whatever pose it was actually holding, rather than snapping to a canonical grip. Every failure path (`FindBody` returns null, no hand body for the requested index, already held by a different hand) is logged with `Printf` rather than silently returning (`p_physics.cpp:2345-2365`) — explicitly called out in comment as a fix for grabs that used to fail invisibly.

`PhysicsRelease` (`p_physics.cpp:2397-2479`) does not compute a fresh release velocity from the current instant; `b->vel`/`b->angVel` already reflect the hand's motion from the continuous carry-loop inheritance. Instead it scans the last `kHandHistory = 16` samples (~180ms at 90Hz, `p_physics.cpp:699`) of hand linear+angular velocity, computes `v = s.vel + Cross(s.angVel * vr_physics_throwspin, r)` for each (where `r` is the object's offset from the hand at release time), and takes the **peak-speed sample**, only overriding the current velocity if that peak genuinely exceeds it (`p_physics.cpp:2443-2461`) — so setting an object down gently is unaffected, but a hard throw uses the fastest moment of the swing rather than the (already-decelerating) instant of release. Both the resulting speed and spin are hard-capped afterward — `14 m/s` (`p_physics.cpp:2466`) and `25 rad/s` (`p_physics.cpp:2476`) — specifically to absorb single-frame tracking spikes.

### `WriteBack`

`p_physics.cpp:1827-1857`. The only place a physics body's transform reaches the actor. No-ops immediately for owner-less bodies (hands, weapons) via `if (a == nullptr) return;` (`p_physics.cpp:1830`). For an owned body: converts centre-of-mass-space `pos` back to the actor's origin (`pos - rot.Rotate(comOffset)`), unlinks/relinks the actor (`UnlinkFromWorld` → `SetXYZ` → `CheckPortalTransition(false)` → `LinkToWorld`, `p_physics.cpp:1836-1840` — this is the call that also updates `AActor::Sector`, see the staleness point above), converts the quaternion back to yaw/pitch/roll degrees for `Angles`, writes `Vel` as an **output only** (`DVector3(...)/TICRATE`, `p_physics.cpp:1850`, explicit in comment that nothing reads it back as input), and sets `RF_DONTINTERPOLATE` on every call (`p_physics.cpp:1856`) so the renderer's tic-based Prev/Pos lerp never fights a transform written at frame rate.

`WriteBack` is invoked for every body in `g_bodies` once per `P_PhysicsFrame` call, but **only if at least one fixed step actually ran that frame** (`if (steps > 0) { ... }`, `p_physics.cpp:2036-2040`) — on a frame where the accumulator hadn't yet reached a full step, no actor is relinked at all that frame.

### Lifetime: registry and GC visibility

`TArray<PhysBody> g_bodies;` (`p_physics.cpp:679`) is a single anonymous-namespace, process-lifetime array. `PhysBody::owner` is a raw `AActor*`; there is no `GC::` call, no `DObject` cast, and no mark/root registration anywhere in `p_physics.cpp` (confirmed by grep — zero matches for `GC::|DObject|IsValid\(\)|::Mark`). `p_physics.h:57-58` states in comment that this is why the removal call is mandatory: *"the GC cannot see a raw AActor* held in a physics registry."*

The single defensive mechanism is `P_PhysicsRemoveBody` (`p_physics.cpp:2084-2097`, a plain linear scan-and-delete by pointer equality), called from exactly one site outside the module: `AActor::OnDestroy` (`src/playsim/p_mobj.cpp:5992`), **unconditionally**, not gated on `MF9_PHYSICSBODY`:

```cpp
// src/playsim/p_mobj.cpp:5983-5992
void AActor::OnDestroy ()
{
	// RS FORK -- drop any rigid body before the actor goes.
	//
	// The physics registry holds a raw AActor*, which the collector cannot see;
	// this is the point where the actor is still valid and is definitely going
	// away, so it is the only correct place to sever that link. Unconditional
	// rather than gated on the flag, because the flag can be cleared at runtime
	// and a body must never outlive its actor.
	P_PhysicsRemoveBody(this);
```

This is the sole lifetime safety net; `FindBody` (`p_physics.cpp:754-759`) does a bare pointer-equality scan with no secondary validity check, so correctness depends entirely on `OnDestroy` always running before an `AActor` is actually freed — consistent with this engine's normal `DObject` destruction contract, but there is no independent check within the physics module itself.

`P_PhysicsLevelStart`/`P_PhysicsLevelEnd` (`p_physics.cpp:2058-2082`) both unconditionally `g_bodies.Clear()` and reset the hand-history state — called from `maploader.cpp:3257` (after `PO_Init()`, `Level->aabbTree`/`levelMesh` construction — i.e. once static level geometry, slopes, 3D floors and polyobjects are final, per the comment at `maploader.cpp:3251-3253`) and `p_setup.cpp:411` (inside `FLevelLocals::ClearLevelData`, after thinkers are torn down and before `aabbTree`/`levelMesh` are deleted, per the comment at `p_setup.cpp:407-409`).

### Broadphase complexity

The body-vs-body pass is unconditional all-pairs, every fixed step, no spatial structure of any kind:

```cpp
// src/playsim/p_physics.cpp:2021-2023
for (unsigned i = 0; i + 1 < g_bodies.Size(); i++)
	for (unsigned j = i + 1; j < g_bodies.Size(); j++)
		SolvePair(g_bodies[i], g_bodies[j], (float)step);
```

For `M` bodies (2 hands + up to 2 weapons + one per `PhysicsEnable`'d actor) this is `M(M-1)/2` calls to `SolvePair` **per fixed step**, up to `vr_physics_maxsteps` times per frame. `SolvePair` itself early-outs on a bounding-sphere test (`p_physics.cpp:1318-1322`) before any real narrow-phase work, so the *cost per pair* that fails the broadphase is small — but the pair *enumeration* itself is `O(M²)` with no grid/blockmap/BVH culling it down, so it scales quadratically in body count regardless of how spread out the bodies are. Free-body world collision similarly has no spatial acceleration beyond the "own sector" restriction: per body per step it's `O(hull-vertex-count × 1)` for floor/ceiling plus `O(sector-line-count × hull-vertex-count)` for walls (`p_physics.cpp:933-999`, nested loop over `sec->Lines` × `hullWorld`), bounded only by whatever the current sector's own linedef count happens to be.

### Debug CVars and telemetry

All 18 CVars in the module are `CVAR_ARCHIVE | CVAR_GLOBALCONFIG` (persist globally, not per-save):

| CVar | type/default | clamp |
|---|---|---|
| `vr_physics_hz` | Int 90 | [30,240] |
| `vr_physics_maxsteps` | Int 4 | [1,16] |
| `vr_physics_debug` | Bool true | — |
| `vr_physics_gravity` | Float 9.81 | — |
| `vr_physics_restitution` | Float 0.32 | — |
| `vr_physics_friction` | Float 0.7 | — |
| `vr_physics_lineardamp` | Float 0.25 | — |
| `vr_physics_angulardamp` | Float 0.6 | — |
| `vr_physics_contactspindamp` | Float 4.0 | — |
| `vr_physics_hands` | Bool true | — |
| `vr_physics_throwspin` | Float 0.5 | [0,2] |
| `vr_physics_handsize` | Float 1.0 | [0.25,4.0] |
| `vr_physics_weaponlen` | Float 0.11 | [0.02,0.6] |
| `vr_physics_weaponwidth` | Float 0.02 | [0.005,0.2] |
| `vr_physics_weaponheight` | Float 0.07 | [0.02,0.4] |
| `vr_physics_weapon_ofs_fwd` | Float 0.08 | [−0.3,0.3] |
| `vr_physics_weapon_ofs_up` | Float 0.02 | [−0.2,0.2] |
| `vr_physics_trace` | Int 0 | [0,20] |

(`p_physics.cpp:77-177`, exhaustive — confirmed by grepping every `CVAR(`/`CUSTOM_CVAR(` line in the file.)

`ReportLine` (`p_physics.cpp:1859-1927`), gated on `vr_physics_debug` (default **on**), fires on every `run`/`paused` or gamestate transition and otherwise once per second while running (`nowNs - g_reportStartNs >= 1e9ns`, `p_physics.cpp:2052`). It logs frames/s, steps/s, dropped-step count, min/max `dt`, tics elapsed, gamestate name, paused/menu/VR/backend flags, and live vs. asleep body counts (hands and weapons excluded from the count so it can actually read zero, `p_physics.cpp:1871-1873`,1877). When any body is awake it separately identifies the single *quietest* one and prints exactly which of the four sleep gates (drift-under-threshold, support-timer-active, sleep-timer-matured, kinematic) is blocking it (`p_physics.cpp:1888-1911`) — comment states this replaced an earlier "nothing ever sleeps" bug that had two independent prior causes, hence naming each gate individually now. `vr_physics_trace` (default 0 = off) drives a separate per-body `[PHYSTRACE]` line up to 20×/s (`p_physics.cpp:1241-1267`) logging position, height above floor, velocity, spin, and the same pose as both quaternion-derived Euler angles and raw spin — explicitly to let a stuck-vs-rotating discrepancy be attributed to either the solver or the quaternion-to-Doom-angles conversion.

### Actor-side integration

**`actor.h:442`**: `MF9_PHYSICSBODY = 0x00000200` in `ActorFlag9`, the sole new flag added by this window in this file. It is set only in `PhysicsEnable` (`p_physics.cpp:2164`), cleared only in `PhysicsDisable` (`p_physics.cpp:2195`), and tested only in `AActor::Tick` (`p_mobj.cpp:4607`) — a repo-wide grep confirms exactly these three sites and no others (no script-side flag exposure, no other native reader).

`actor.h`'s diff also adds `GripClaimMain/Off`, `GripSubjectMain/Off`, `TwoHandedHold`, and `FingerTouchMain/Off` (`actor.h:1798-1828`) — these belong to the hand/grip-arbitration system, not the rigid-body solver, and are out of scope for this section beyond noting they exist in the same diff.

**`p_mobj.cpp:4607-4628`** (`AActor::Tick`): a physics body takes a dedicated branch ahead of the pre-existing `MF5_NOINTERACTION` branch:

```cpp
if (flags9 & MF9_PHYSICSBODY)
{
	// ... (comment enumerates what's skipped: scroller/carry accumulation,
	// steep-slope push, P_XYMovement/P_TryMove/friction/sliding/bouncing,
	// P_ZMovement/gravity/floor-ceiling clamp/floatbob, P_CheckOnmobj, Crash())
	flags8 &= ~MF8_INSCROLLSEC;
}
else if (flags5 & MF5_NOINTERACTION)
{ ... }
```

Independently verified against the branch body itself (not just the comment): the physics-body branch contains exactly one statement — clearing `MF8_INSCROLLSEC` — and, unlike both the `MF5_NOINTERACTION` branch and the default `else` branch, it does **not** `return`; it falls through past the whole `if`/`else if`/`else` chain to whatever runs after it (state/animation advance, per the comment at `p_mobj.cpp:4624-4626`). It also deliberately does not set `MF_NOBLOCKMAP` (unlike the `MF5_NOINTERACTION` branch), so a physics body stays present in the blockmap and reachable by hitscans and other actors' collision checks — stated in comment and consistent with the branch containing no code that would remove it.

**`p_mobj.cpp:5992`** (`AActor::OnDestroy`): unconditional `P_PhysicsRemoveBody(this)` — covered in Lifetime above.

**`p_setup.cpp:411`** / **`maploader.cpp:3257`**: `P_PhysicsLevelEnd`/`P_PhysicsLevelStart` call sites — covered in Lifetime above.

### ZScript API surface

12 native functions on `AActor`, all `DEFINE_ACTION_FUNCTION_NATIVE` (`p_physics.cpp:2177-2555`): `PhysicsEnable`, `PhysicsDisable`, `PhysicsAddImpulse`, `PhysicsAddSpin`, `PhysicsSetImpactSound`, `PhysicsSetHeld`, `PhysicsSetTransform`, `PhysicsGrab`, `PhysicsRelease`, `PhysicsIsHeld`, `PhysicsDistanceTo`, `PhysicsIsAsleep`. All 12 are declared `native` in `wadsrc/static/zscript/actors/actor.zs:991-1020` (outside the assigned path set, checked only to confirm the API is actually wired to script, not dead code) — matching signatures confirmed for all 12. `PhysicsEnable` derives inertia from a **solid-box formula using the caller-supplied half-extents** (`p_physics.cpp:2154-2162`, `ix = m(h²+d²)/12` etc.) **regardless of whether `ApplyPhysDefShape` subsequently replaced the collision shape with real hull geometry** — mass distribution is never reconciled with hull shape.

**No caller of any of these 12 functions exists anywhere in this repository's own `wadsrc`** (grepped `wadsrc/` for all three most central names — only the declarations themselves match). The API is fully implemented and exposed but unexercised end-to-end within this tree; the mod script that would call `PhysicsGrab`/`PhysicsEnable` etc. is expected to live outside this repository.

### Summary of the "critical, already-suspected" finding

Stated plainly, with the complete evidence chain: **a hand's pose (`UpdateHands`) and a held or attached body's pose (the carry loop in `UpdateHands`, and `UpdateWeapons`) are written by direct assignment from tracked/derived values, with zero world-geometry query anywhere in that path.** This holds by direct inspection of every write site (`p_physics.cpp:1672-1673`, `1708-1709`, `1808-1809`) and is structurally guaranteed by `StepBody`'s unconditional `if (b.kinematic) return;` (`p_physics.cpp:814`), since hands, weapons, and held bodies are always constructed or set `kinematic = true`. Free bodies get full floor/ceiling/wall collision; hands, held weapons, and held objects get none — they can be moved through solid geometry by the player's real-world hand motion with no resistance, though held/weapon bodies still register contact against *other* physics bodies via `SolvePair`.


---

## 3. Models, bones and IQM rendering

### Window totals

`git diff 1d2572bdcc main --numstat` over this subsystem's files:

| File | + | - |
|---|---|---|
| src/r_data/models.cpp | 757 | 27 |
| src/r_data/models.h | 14 | 0 |
| src/common/models/model.h | 44 | 0 |
| src/common/models/model_md2.h | 9 | 0 |
| src/common/models/model_md3.h | 12 | 0 |
| src/common/models/model_obj.h | 1 | 0 |
| src/common/models/models_iqm.cpp | 29 | 0 |
| src/common/models/models_md2.cpp | 43 | 0 |
| src/common/models/models_md3.cpp | 28 | 0 |
| src/common/models/models_obj.cpp | 28 | 0 |
| **Total** | **965** | **27** |

`src/r_data/models.cpp` is confirmed at exactly +757/-27, matching the figure given for this window. `model_iqm.h` and `bonecomponents.h` (the joint-query virtuals and `BoneInfo`/`BoneOverride`) are untouched in this window (zero-line diff) — every bone-access virtual this section relies on (`FindJoint`, `GetJointPosition`, `NumJoints`, `GetJointRotation`, `GetJointParent`, `GetJointName`, `GetJointChildren`, `GetRootJoints`, `GetJointBaseTRS`, `GetJointPose`, `GetBasePose`) predates this window and is IQM-only: MD2/MD3/OBJ never override any of them (verified by a repository-wide grep for each name). A skeleton is therefore definitionally an IQM-only concept in this engine. What this window changed is (a) whether the one function that turns those bones into render matrices without an active animation was computing them correctly, and (b) what a script or downstream system can now do with the result.

### 1. The IQM bind-pose fix

`IQMModel::CalculateBonesOnlyOffsets` (src/common/models/models_iqm.cpp:882) is the "no animation, no explicit frame" path — the fallback a decoupled model without an active `SetAnimation` and without a pinned static frame lands on (see the `ProcessModelFrame` branch table in §3), and also the base-pose contributor for `MDL_MODELSAREATTACHMENTS` models. It has two near-identical branches: one that also fills a caller-supplied `BoneInfo*` (models_iqm.cpp:896-961) and one that only fills the internal `boneData` array (models_iqm.cpp:962-1005).

Before this window's fix, the per-bone loop in both branches read only:
```
TRS bone;
```
`TRS`'s members default-construct to an identity transform (src/common/utility/TRS.h:31-33: `translation = (0,0,0)`, `rotation = FQuaternion::Identity()`, `scaling = (1,1,1)`), and nothing repopulated `bone` from the joint before it was fed into `(*in)[i].Modify(bone, time)` and then into a matrix (`m.translate(...); m.multQuaternion(...); m.scale(...)`) combined with `inversebaseframe[i]`. So `bone` was always identity, regardless of what the rig actually specified for that joint — every bone's resulting matrix was `inversebaseframe[i]` applied to identity rather than to the joint's own bind TRS, i.e. every bone came out transformed by the *inverse of its own bind pose*. Because `inversebaseframe` is accumulated per joint down the parent chain from each joint's individual bind TRS (models_iqm.cpp:216-224), the visible symptom was rig-dependent: a root joint with a 90-degree bind rotation renders the mesh sideways, one with a small bind scale renders it inflated by the inverse of that scale, and so on.

The fix (models_iqm.cpp:927-928 and :975-976) adds one line to each branch:
```
TRS bone;
bone = Joints[i];
```
`Joints[i]` is an `IQMJoint` (src/common/models/model_iqm.h:84-95), carrying the joint's actual bind-pose `Translate`/`Quaternion`/`Scale`. `TRS` has no converting constructor, only a templated `operator=` (TRS.h:40-47) that reads exactly those three member names off any right-hand type — so `TRS bone = Joints[i];` (copy-initialization) does not compile, which is why the fix is a declaration followed by a separate assignment rather than a one-line initializer. This is independently verifiable directly from TRS.h, not just from the fix's own comment. With the change, `bone` starts from the joint's real bind pose; `Modify()` perturbs that instead of identity; and the result — combined with `baseframe[parent]`/`inversebaseframe[i]` exactly as the animated path (`CalculateBonesIQMSpecialized`, models_iqm.cpp:718-860) already does when it seeds `bone` from real per-frame animation data — lands the bone where the rig actually put it.

Two other `TRS bone;` declarations exist nearby and were **not** touched: models_iqm.cpp:606, inside the free function `InterpolateBone`, which immediately overwrites every field of `bone` from its `from`/`to` arguments before returning it (so identity-construction there is harmless); and models_iqm.cpp:784, inside `CalculateBonesIQMSpecialized`, which is entirely outside this window's diff and out of this subsystem's scope to re-litigate. The fix is scoped exactly to the two sites that had the defect.

### 2. HUD bone anchoring: a new runtime bone-query surface

This window adds a complete mechanism — none of it existed at 1d2572bdcc — letting one HUD psprite layer draw itself at a named bone of another layer's model, and letting ZScript read that bone's resolved position/orientation back every frame. It lives entirely in models.cpp/.h; the request/response fields it reads and writes (`DPSprite::AnchorLayer/AnchorBone/AnchorOfs/AnchorAngles` and `AnchorBonePos/AnchorBoneWorld/AnchorBoneAngles/AnchorBoneLive`, declared in src/playsim/p_pspr.h, outside this subsystem) are populated exclusively from here.

**Storage and lifetime** (models.cpp:65-101): `g_hudAnchors` is a `TMap<uint64_t, HudAnchorEntry>` keyed by `HudAnchorKey(layer, bone)` = `uint32_t(layer) << 32 | uint32_t(bone.GetIndex())`, where `HudAnchorEntry{ VSMatrix mat; DVector3 offset; uint64_t frame; }`. `HudAnchor_BeginFrame()` (models.cpp:79-93; called once per HUD draw pass from src/rendering/hwrenderer/scene/hw_weapon.cpp:2371, outside this subsystem) increments a global `g_hudAnchorFrame` counter and clears every psprite's `AnchorBoneLive` flag; `HudAnchor_Get`/`HudAnchor_GetOffset` (models.cpp:95-117) reject any entry whose stamped `frame` does not match the current one. A bone that was not actually drawn this frame therefore reads as "no anchor," never as a stale position — a target that stops rendering cannot leave a grab point floating at its last known place.

**Publication** (`HudAnchor_Store`, models.cpp:122-255; called from `RenderModelFrame` at models.cpp:1496 for every rendered HUD model layer that has a `psp` and resolved `boneData`, deliberately outside the `MDL_MODELSAREATTACHMENTS`/decoupled gate at models.cpp:1467-1486, so an ordinary weapon publishes too): for each other psprite on the same player whose `AnchorLayer` equals the layer currently rendering, it resolves `j = mdl->FindJoint(q->AnchorBone)` and, if found, builds the entry from `bones[j]` (a skinning matrix — bind-space to posed-space). The load-bearing step, called out three times in the code's own comments: a skinning matrix's translation column is *not* the bone's position — at rest every skinning matrix is identity, so reading the column directly gives the same point (the model origin) for every bone. The correction is to apply the matrix to the joint's real bind position instead: `mdl->GetJointPosition(j)` (the pre-existing, parent-accumulated absolute bind position, model_iqm.h:224-227) through `boneMat.multMatrixPoint`. This is applied three times over for three different consumers of the same number:
- `q->AnchorBonePos` (models.cpp:172) — object-space, in the anchored-*to* model's own axes, scaled by the source transform's own basis-column length (models.cpp:166-168).
- `q->AnchorBoneWorld` (models.cpp:184) — the same point through the full object-to-world matrix, with the engine's Y-up-matrix/Z-up-playsim axis swap (`DVector3(world[0], world[2], world[1])`).
- `e.mat`'s own translation column (models.cpp:207-214) — the *stored* matrix `HudAnchor_Get` hands to the renderer for the anchored layer's actual draw transform gets the identical `fixed[12..14] = world[0..2]` patch. Without this, the two script-facing numbers above would be correct while the model itself still rendered at the wrong point — a fix to what script reads is not automatically a fix to what gets drawn.

Rotation is decomposed separately from `e.mat`'s (post-fix) basis columns into `AnchorBoneAngles` (yaw/pitch/roll, degrees) via `atan2`/`asin` (models.cpp:226-251); rotation needed no bind-pose correction, since orientation does not depend on which point along the bone is sampled.

**Consumption** (`RenderHUDModel`, models.cpp:736-797): if the rendering layer has `AnchorLayer >= 0` and `AnchorBone != NAME_None` and `HudAnchor_Get` finds a live entry, everything computed up to that point (controller pose, weapon bob/aim math) is discarded. The anchor matrix is orthonormalized (each basis column rescaled to unit length, models.cpp:759-769) to strip whatever scale the *target* rig carried — otherwise an item anchored to a hand joint inherits the hand rig's own scale (the VR hand rig's root joint carries 0.01, so an anchored item would render 100x too small) — then loaded wholesale via `loadMatrix`. Because `loadMatrix` replaces the matrix outright, the anchoring model's own MODELDEF scale and the HUD-model unit-conversion factor (the pre-existing `0.01f` HUD-units scale, now captured into `hudUnitScale` at models.cpp:632-638 specifically so it survives this replacement) are explicitly reapplied afterward (models.cpp:787, 793). From there the model's own offsets/rotations/scale (MODELDEF, mod-owned placement CVars §4, hand sliders §5) and a per-anchor "seat" offset/rotation (`psp->AnchorOfs`/`AnchorAngles`, summed into the same translate/rotate calls as everything else at models.cpp:817-819 and :861-865, not a separate step, because rotations do not commute) still apply — one code path serves anchored and unanchored layers alike. When anchored, the entire "position relative to the player" block — global weapon offset, bob, aim rotation, viewmodel axis-fix — is skipped outright (models.cpp:830-848), because the bone matrix already encodes the target's fully resolved placement.

This is the concrete "query a bone transform at runtime" hook this subsystem now exposes to a physics/interaction system: ZScript writes a request pair (`AnchorLayer`, `AnchorBone`, optionally `AnchorOfs`/`AnchorAngles`) onto a psprite, and every frame the target layer actually draws, this pipeline resolves and writes back `AnchorBonePos`/`AnchorBoneWorld`/`AnchorBoneAngles`/`AnchorBoneLive`, and separately drives the anchored layer's own render transform. It is HUD/psprite-only: `RenderFrameModels`'s other call site, for world actors (`RenderModel`, models.cpp:364), passes no `psp`, so `HudAnchor_Store` never runs there. World actors are not left with zero bone query, though — a separate, pre-existing mechanism (`DActorModelData::modelBoneInfo`, src/playsim/actor.h:750, populated via `ProcessModelFrame(..., &modelData->modelBoneInfo[i])` at src/playsim/p_mobj.cpp:4358, both outside this subsystem) already threads a live `BoneInfo*` through for actors. The two rendering entry points are structurally distinct call chains (`RenderModel`/`RenderFrameModels(...)` vs. `RenderHUDModel`/`RenderFrameModels(...,psp)`), and only the actor-side one had a live per-frame bone-output channel before this window — `RenderModelFrame`'s own `ProcessModelFrame` call always passes `nullptr` for `out` (models.cpp:1465), so `BoneInfo` population never happens on the HUD path. The new anchoring system is the HUD-side equivalent that did not exist before this window.

### 3. Decoupled models can now be pinned to a static authored frame

`ModelDrawInfo::modelframe_explicit` (src/r_data/models.h:143, default false) is new. It is reset false at the top of `CalcModelOverrides` (models.cpp:1168) and set true at the two places that deliberately choose a frame rather than let it fall out of the sprite table — both pre-existing mechanisms, unmodified here except for stamping this new flag: direct `psp->ModelFrame` addressing (models.cpp:1289-1293) and the native state-remap table lookup (models.cpp:1321-1332, keyed by the psprite's current `FState*`).

`ProcessModelFrame` (models.cpp:1340) previously distinguished only "an animation is playing" (`frameinfo.decoupled_frame.frame1 >= 0`) from "nothing is playing" (fall through to `CalculateBonesOnlyOffsets`, §1's rest/bind pose) on the decoupled path — `drawinfo.modelframe` was consulted only on the non-decoupled branch. A `+DECOUPLEDANIMATIONS` model, in other words, could not be pinned to one authored frame; it could only play a clip or sit at rest. This window inserts a third branch (models.cpp:1383-1418, gated on `drawinfo.modelframe_explicit`) between the two: when a frame was chosen deliberately and no animation is overriding it, it calls the same `animation->CalculateBones(...)` construction the non-decoupled branch uses (models.cpp:1404-1417 mirrors :1431-1444), with `nullptr` for `from` and `-1.0f` for the outer interpolation parameter — a single static frame, not a blend. This is the mechanism a per-tic, controller-driven hand pose needs: the pose is one of several baked frames of a clip, chosen per tic from input, with no clip actually playing — running `SetAnimation` at zero framerate to hold a frame would fight the animation clock instead of naming a frame directly. Bone overrides (`modelData->modelBoneOverrides[i]`) still compose on top in this branch exactly as in the other two.

A `vr_pose_debug` CVAR (default true, not archived — models.cpp:261) traces this pipeline end to end: `CalcModelOverrides` logs `psp.ModelFrame -> drawinfo.modelframe/explicit` (models.cpp:1296-1311, deduplicated per-hand via `psp->GetID() >= PSP_OFFHANDWEAPON`), and `ProcessModelFrame` logs which of the three decoupled branches fired, by name (models.cpp:1352-1366: `"ANIM (SetAnimation wins)"` / `"STATIC (our pose)"` / `"REST (pose discarded)"`), each printing only on change.

### 4. Mod-owned placement CVars: HUD models and world models

`FSpriteModelFrame::placementCVars` (model.h:86, an `FName` prefix, parsed from a new `PlacementCVars "<prefix>"` MODELDEF keyword at models.cpp:1763-1770) names a CVAR prefix. Six suffixed CVARs (`<prefix>_ofs_x/_ofs_y/_ofs_z`, `<prefix>_yaw/_pitch/_roll`) plus an optional `<prefix>_scale` are resolved *by name*, every frame, via the engine's generic `FindCVar` — not bound or cached at MODELDEF-parse time, because MODELDEF is guaranteed to parse before a mod's own CVARINFO has necessarily run (stated at models.cpp:695-698 and :530-532, and consistent with the deliberate per-frame lookup pattern in the code). A missing or unresolvable CVAR reads as zero for offsets/rotations and 1.0 for scale (explicitly guarded, models.cpp:717-724 / :551-559), so an absent CVAR degrades to the model's own MODELDEF values rather than collapsing it to a point. Routing through CVARINFO-declared CVARs rather than new engine CVARs lets a mod add a tunable, live-adjustable weapon with a MENUDEF slider without touching the engine, and a value found on a slider can be written back into MODELDEF verbatim afterward (`ofs_x`/`yaw` etc. sum into the identical `xoffset`/`angleoffset` contributions, same order and sign).

This mechanism was added to the HUD path first (`RenderHUDModel`, models.cpp:699-726, applied into the offset translate at :817-819 and the rotate at :863-865, and the scale multiply at :908). Confirmed by isolating the window's HEAD commit's own diff, it did **not** originally apply to world-space actors: `ObjectToWorldMatrix(FLevelLocals*, ...)` (models.cpp:428) — the transform `RenderModel` (models.cpp:348) uses for any actor with a model attached, including a physically simulated/dropped weapon placed as a real `AActor` in the map rather than drawn as a HUD viewmodel — had no equivalent block. The window's most recent commit touching this file (`026d2a8a80`, currently `HEAD`) closes that gap: it duplicates the identical CVAR-resolution block into `ObjectToWorldMatrix` (models.cpp:533-560) and folds `wPlaceOfs`/`wPlaceRot`/`wPlaceScale` into the existing scale/translate/rotate calls at the same two steps the HUD path uses (models.cpp:563 scale, :566-568 translate, :571-573 rotate) — same six CVARs, same suffixes, same order and sign, so a slider tuned on a world-placed prop means the same thing as the identical slider on a HUD-drawn one. Both paths are live in `main` as of this window.

### 5. Hand-frame orientation offsets: a second, non-commuting rotation triple

`FSpriteModelFrame` gains `handangleoffset`/`handpitchoffset`/`handrolloffset` (model.h:106, all defaulting to 0, parsed from new `HandAngleOffset`/`HandPitchOffset`/`HandRollOffset` MODELDEF keywords at models.cpp:1786-1800). These are applied in `RenderHUDModel` as a *separate*, *later* set of three `.rotate()` calls (models.cpp:901-903) than `angleoffset`/`pitchoffset`/`rolloffset` (models.cpp:863-865), summed respectively with the live `vr_hand_yaw/_pitch/_roll` / `vr_offhand_yaw/_pitch/_roll` CVARs (gated on the pre-existing `MDL_USEHANDOFFSETS` flag and hand side, models.cpp:799-807, 851-853). This is a verifiable consequence of the code's structure, not merely an assertion: intrinsic (object-space) rotations applied via sequential `VSMatrix::rotate` calls do not commute, so a second rotation triple applied *after* the model's own baked orientation is mathematically distinct from folding the same three values into the first triple. The code's stated rationale for the separation (rather than summing hand-slider values directly into `angleoffset`/`pitchoffset`/`rolloffset`) is that a model carrying a baked 90-degree `PitchOffset` — said to be the case for the VR hand rig, though that MODELDEF value itself lives outside this repository and was not independently checked here — puts that rotation's axis on top of one of the other two axes once the 90-degree turn is applied, so two nominally independent sliders end up driving one remaining axis (gimbal lock) instead of two orthogonal ones. Applying the hand triple in the frame the model's own baked orientation leaves behind restores three orthogonal axes, while the value stays bakeable: a number found on a slider can be written into the matching MODELDEF keyword and mean exactly what it did live, because it sums into the identical position in the identical call sequence either way.

### 6. Model bounds hint: `GetLocalExtent` (MD2, MD3, OBJ — not IQM)

`FModel::GetLocalExtent(float*, float*, float*)` (model.h:196, new virtual, default returns `false`) reports the largest `|X|`/`|Y|`/`|Z|` across a model's raw local-space vertices, tracked independently per axis (not necessarily the same vertex — a conservative bounding proxy, not a tight AABB), unscaled by MODELDEF `Scale`. It is consumed by a single native, `GetModelBoundsHint` (src/scripting/vmthunks.cpp:6326-6349, outside this subsystem), which multiplies the three axes by the resolved frame's own `xscale`/`yscale`/`zscale` and combines them into a world-space bounding radius at actor scale (1,1) — intended, per that native's own comment, to let a holster-style system solve a per-weapon fit scale instead of applying one flat multiplier regardless of a model's real size.

Three of the four formats gained an override this window:
- **FDMDModel/FMD2Model** (MD2/DMD): `cachedMaxAbsX/Y/Z` + `hasCachedExtent` (model_md2.h:118-119), computed inside the per-vertex loop of **two separate** `LoadGeometry` overrides — `FDMDModel::LoadGeometry(FileSys::FileData*)` (models_md2.cpp:178, cache write completes at :221) and `FMD2Model::LoadGeometry(FileSys::FileData*)` (models_md2.cpp:539, cache write completes at :586) — because `FMD2Model : public FDMDModel` (model_md2.h:150) overrides `LoadGeometry` itself rather than calling the parent's, so an actual `.md2` file needs its own copy of the fill loop or never gets one. `GetLocalExtent` (models_md2.cpp:286-292) is defined once on the base `FDMDModel` and inherited by `FMD2Model`. The cache exists because `framevtx`, the temporary per-frame vertex buffer this loop reads, is deleted by `UnloadGeometry()` once `BuildVertexBuffer` has uploaded it to the GPU.
- **FMD3Model** (MD3): identical pattern — `cachedMaxAbsX/Y/Z`/`hasCachedExtent` (model_md3.h:89-90) filled inside `LoadGeometry`'s per-surface, per-vertex loop (models_md3.cpp:196, cache write completes at :251), because `BuildVertexBuffer` calls each surface's own `UnloadGeometry()` right after GPU upload, resetting `MD3Surface::Vertices` for good. `GetLocalExtent` at models_md3.cpp:262-268.
- **FOBJModel** (OBJ): no cache needed. `verts` (`TArray<FVector3>`, model_obj.h:72) is OBJ's persistent parsed vertex list, never freed the way MD2's `framevtx` or MD3's per-surface `Vertices` are, so `GetLocalExtent` (models_obj.cpp:697-711) computes the max directly from `verts` on demand, on every call. It is unaffected by `RealignVector`'s Z-negation (used elsewhere for `BuildVertexBuffer`/`RenderFrame`) because negating one axis cannot change an absolute-value magnitude.
- **IQMModel**: **no override was added.** It falls through to `FModel`'s default (`return false`), so `GetModelBoundsHint` returns `found=0` for every IQM model. Since IQM is also the only format in this engine with any joint/bone-query implementation at all (§0), this means every model capable of carrying a skeleton — the exact class of model this window's other work (the bind-pose fix, HUD bone anchoring) is about — cannot report a size hint through this native today. See gaps.

Every `Load`/`FindFrame`/`RenderFrame`/`AddSkins` entry point in all four format loaders is otherwise untouched this window (confirmed by `--numstat`: model_iqm.h and every changed loader file shows 0 deletions, i.e. purely additive).

### 7. New `MDL_` flags and MODELDEF keywords

`src/r_data/models.h:59-61` appends three flags after the pre-existing `MDL_FIXROTATING` (`1<<15`), purely additive (no renumbering):

| Flag | Bit | MODELDEF keyword | Effect |
|---|---|---|---|
| `MDL_NOAUTOREVERSE` | `1<<16` | `NoAutoReverse` (models.cpp:2055-2058) | Passed as `!(smf_flags & MDL_NOAUTOREVERSE)` into `vrmode->GetWeaponTransform`'s `allowAutoReverse` parameter (models.cpp:633; `GetWeaponTransform` is declared in src/common/rendering/hwrenderer/data/hw_vrmodes.h:196, outside this subsystem) — suppresses the non-dominant-hand mirror for a model that already ships explicit left/right mesh variants. |
| `MDL_USEHANDOFFSETS` | `1<<17` | `UseHandOffsets` (models.cpp:2047-2050) | Gates whether the live `vr_hand_*`/`vr_offhand_*` CVARs (fixed engine CVARs, distinct from the mod-declared `placementCVars` mechanism of §4) are summed into a HUD model's offset/rotation (models.cpp:799-807, 851-853). |
| `MDL_IGNORESKINALPHA` | `1<<18` | `IgnoreSkinAlpha` (models.cpp:2051-2054) | Read via `FSpriteModelFrame::ignoresSkinAlpha()` (model.h:113, flags-only, no per-actor override) from src/rendering/hwrenderer/scene/hw_weapon.cpp:197 (outside this subsystem) to zero the alpha-test threshold, so a skin whose alpha channel packs PBR data (roughness/gloss) rather than opacity is not alpha-tested away. |

Also new: `PlacementCVars <name>` (models.cpp:1763-1770, string argument, §4), `HandAngleOffset`/`HandPitchOffset`/`HandRollOffset` (models.cpp:1786-1800, float arguments, §5).

### 8. Draw-time diagnostics

Three CVars, all `CVAR(Bool, ..., true, 0)` — default on, flag `0` meaning not `CVAR_ARCHIVE` (models.cpp:261, 270-271):
- `vr_pose_debug` — traced in §3.
- `vr_validate` — gates `ValidateHudModel` (models.cpp:283-318, called once per draw from `RenderHUDModel` at :926), which checks, once per `(smf, check-slot)` via `ValidateOnce` (models.cpp:275-281): a skinned model (`NumJoints() > 0`) with zero frames (`NumFrames() == 0`), which collapses to a shapeless mess reading as missing geometry/textures rather than an animation problem; and a skin texture with an alpha channel where `IgnoreSkinAlpha` was not set, which reads as a half-transparent model when the alpha is actually packed data.
- `vr_spatialreport` — additionally gated on a non-null `psp` (models.cpp:928, HUD-only), prints each psprite layer's resolved world position and a uniform-scale estimate (basis-column length) at most once per second *per layer* via `TMap<int, uint64_t> lastReportByLayer` keyed on `psp->GetID()` (models.cpp:940-946) — replacing what the in-code comment describes as an earlier single shared timer that could structurally only ever report the layer drawn first each second (the weapon), leaving other layers (hands) silent in a way that read as "not being drawn" rather than "never got the slot."

Both `ValidateHudModel` and the `vr_spatialreport` block are called exclusively from `RenderHUDModel`; neither runs on the world-actor (`RenderModel`) path.

### Summary: what changed per format loader

| Format | Files (+/-) | Changed |
|---|---|---|
| IQM | models_iqm.cpp (+29/0) | Bind-pose seeding fix in `CalculateBonesOnlyOffsets`, both branches (§1). No new virtual overrides; still the only format implementing any `GetJoint*`/`FindJoint`/`NumJoints`/`GetBasePose` virtual (all pre-existing). No `GetLocalExtent` override added. |
| MD2/DMD (`FDMDModel`/`FMD2Model`) | model_md2.h (+9/0), models_md2.cpp (+43/0) | New cached-extent fields + `GetLocalExtent` override, filled from two separate `LoadGeometry` overrides (§6). No change to loading/rendering logic itself. |
| MD3 (`FMD3Model`) | model_md3.h (+12/0), models_md3.cpp (+28/0) | Same cached-extent pattern, single `LoadGeometry` (§6). No change to loading/rendering logic itself. |
| OBJ (`FOBJModel`) | model_obj.h (+1/0), models_obj.cpp (+28/0) | `GetLocalExtent` computed on demand from the persistent `verts` array; no new fields, no `LoadGeometry` change (§6). |

### Gaps

Headline: IQM — the only format capable of carrying a skeleton, and the format the rest of this window's bone work targets — never got a `GetLocalExtent` override, so the one native that exists to answer "how big is this model" (`GetModelBoundsHint`) cannot answer it for a single rigged weapon or hand model today. The remaining items are smaller structural/scaling notes surfaced while reading the new code, not defects that currently misbehave.


---

## 4. Weapon and psprite rendering, hands

Scope check first, because the assigned file list is misleading for one entry: `hw_drawinfo.cpp`'s diff in this window (`+110/-4`, `git diff 1d2572bdcc main -- src/rendering/hwrenderer/scene/hw_drawinfo.cpp`) touches none of `HWDrawInfo::DrawPSprite`, `DrawPlayerSprites`, or either `PreparePlayerSprites*` function. Every hunk is inside `HWDrawInfo::StartScene`, and every hunk is either the FogDisturb/"Standing Shapes"/"sweep room" shader-uniform system or a one-line high-water-mark fix. It is covered below for completeness and correctness, but it is not part of the weapon/psprite/hand subsystem this window. The real work for this subsystem is concentrated in `p_pspr.h` (entirely new content), `hw_weapon.cpp`, `p_pspr.cpp`, and `weapons.zs`.

| File | +/- (window) | File size (HEAD) | Touched |
|---|---|---|---|
| `src/playsim/p_pspr.h` | +87/-0 | 399 lines | ~22% (one contiguous block) |
| `src/rendering/hwrenderer/scene/hw_weapon.cpp` | +123/-15 | 2581 lines | ~5% |
| `src/playsim/p_pspr.cpp` | +38/-4 | 1701 lines | ~2.5% |
| `wadsrc/static/zscript/actors/inventory/weapons.zs` | +36/-3 | 1334 lines | ~3% |
| `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` | +110/-4 | 2088 lines | ~5.5% (unrelated subsystem, see above) |

Bottom line up front, expanded on under "Critical question" below: this window gives `DPSprite` a bone-anchoring data contract and fixes a real per-hand identity bug in the psprite draw path. It does **not** connect weapon/hand rendering to the physics module (`p_physics.cpp`). The anchor data and a parallel per-hand button rework are scaffolding for a bridge — the field comments describe the bridge explicitly — but nothing in this repository, in this window or before it, performs the read on the physics or script side that would complete it.

### `p_pspr.h`: nine new `DPSprite` fields (100% of the diff, in full)

The entire diff is one block inserted at `src/playsim/p_pspr.h:274-359`, immediately before the private savegame constructor (`p_pspr.h:362`, `DPSprite () {}`). That placement matters mechanically, not just stylistically: every new field is given an in-class initializer rather than being set in the public constructor body, because `DPSprite` has a second, private, body-less constructor used by savegame deserialization (confirmed at `p_pspr.h:361-362`), and a member with no in-class default is left as uninitialized memory on load. This is the same pattern already established for the pre-existing (pre-window) `Tint`/`Glow`/`ModelFrame` fields a few dozen lines above (`p_pspr.h:226-233`, `246-249`), whose comment records a concrete historical bug from getting this wrong (an old save resuming with garbage `Tint`, "multipl[ying] the weapon by black"). The new fields follow the established, previously-learned convention correctly: `NoDraw = false`, `AnchorLayer = -1`, `AnchorBone = NAME_None`, `AnchorOfs/AnchorAngles = {0,0,0}`, `AnchorBonePos/AnchorBoneWorld/AnchorBoneAngles = {0,0,0}`, `AnchorBoneLive = false` (`p_pspr.h:288,305-306,322-323,334-335,346,359`).

The nine fields split into three functional groups:

- **`bool NoDraw`** (`p_pspr.h:288`) — hides one psprite layer in both draw passes while leaving the owning weapon actor fully alive (state, ammo, slot untouched). The header comment states the reason it can't be done from script with the existing `alpha` field: `DPSprite::GetRenderStyle` discards `psp->alpha` unless the layer carries `PSPF_ALPHA`/`PSPF_FORCEALPHA`, and the draw/no-draw branch is a `continue` inside a render loop script has no hook into.

- **Anchor input, four fields, set by whatever wants the layer positioned relative to another layer's bone**: `int AnchorLayer = -1` and `FName AnchorBone = NAME_None` (`p_pspr.h:305-306`) name the target layer id and bone; `DVector3 AnchorOfs`/`AnchorAngles` (`p_pspr.h:322-323`) are an additional offset/rotation applied in the bone's own frame. The comment is explicit that these are summed into the *same* MODELDEF offset/rotation calls a placement-CVAR-driven model already goes through, not applied as a second transform afterward, "because rotations do not commute." The ordering constraint stated in the comment — "The anchored layer must have a HIGHER id than its target" — is independently verifiable, not just an author's claim: `DPSprite`'s constructor inserts into `Owner->psprites` as a linked list sorted by ascending `ID` (`p_pspr.cpp:253-262`, `while (next != nullptr && next->ID < ID)`), and both psprite draw passes walk that list in order, so a lower-id layer is always drawn, and its bones captured, before a higher-id layer that anchors to it.

- **Anchor output, four fields, written by the renderer, read-only from script's perspective**: `DVector3 AnchorBonePos` + `bool AnchorBoneLive` (`p_pspr.h:334-335`) — the resolved bone position as an offset from the weapon's own origin, in the model's own axes, map units; `DVector3 AnchorBoneWorld` (`p_pspr.h:346`) — the same point re-expressed as a world position, deliberately in the same frame as `AttackPos`/`OffhandPos` so that, per the comment, `(psp.AnchorBoneWorld - player.mo.OffhandPos).Length()` is a valid grab-distance test with no basis reconstruction needed; `DVector3 AnchorBoneAngles` (`p_pspr.h:359`) — the same bone's orientation as yaw/pitch/roll, because ZScript's `Quat` type "exposes no vector-rotate" to derive it from `AnchorBoneWorld` alone. All four are populated every frame by `HudAnchor_Store` in `src/r_data/models.cpp:122-253` — outside this assignment's file set, so not audited here for correctness, but directly readable, and it does exactly what the header promises: it writes straight onto `q->AnchorBonePos`/`AnchorBoneWorld`/`AnchorBoneAngles`/`AnchorBoneLive` on the requesting psprite (`models.cpp:170-172, 180-181, 242-247`) rather than making script read a shared table, which the field comment says would otherwise be "a cross-thread read of a TMap while the renderer is writing it."

All nine fields are exposed to script via `DEFINE_FIELD(DPSprite, ...)` in `p_pspr.cpp:163-181`, and mirrored as `native` declarations of the same names/types in `wadsrc/static/zscript/actors/player/player.zs:3105-3164` (outside this assignment's file set; cited only to confirm the C++/script contract is complete on both ends). That player.zs block also contains the intended grab-test as a comment, not as live code:
```
if (psp.AnchorBoneLive &&
    (psp.AnchorBoneWorld - player.mo.OffhandPos).Length() < grabRadius)
```
(`player.zs:3151-3152`). See "Critical question" below for what that means.

**Serialization gap.** `DPSprite::Serialize` (`p_pspr.cpp:1490-1518`) adds exactly one new key: `("nodraw", NoDraw)` at `p_pspr.cpp:1514`. `AnchorLayer`, `AnchorBone`, `AnchorOfs`, and `AnchorAngles` — the four *input* fields, the ones a script would set once to configure an anchor — are absent from the archive chain entirely. (The four output fields are correctly left unserialized; they're per-frame renderer state and re-derive themselves.) The practical effect: any anchor configuration a mod sets on a psprite is lost on save/load, unconditionally, because the field's in-class default (`-1`/`NAME_None`/zero) is exactly what `FSerializer` leaves in place for a key it never finds. This is not the old Tint/Glow "uninitialized memory" hazard — the defaults are safe — it is simply that the configuration never round-trips through a save file at all.

### `hw_weapon.cpp`: the two passes learn to skip and to reattribute

Three independent changes, all inside `HWDrawInfo`'s player-sprite draw path:

**1. `NoDraw` enforcement**, at the two points `p_pspr.h`'s field comment promises: `PreparePlayerSprites2D` (`hw_weapon.cpp:2259`, `if (psp->NoDraw) continue;`) and `PreparePlayerSprites3D` (`hw_weapon.cpp:2408`, same check). Both are the only two sites in the repository that read `NoDraw` (confirmed by a full-repo grep) — the field does exactly what its two call sites do and nothing else.

**2. Flat-overlay dimming**, `PreparePlayerSprites2D`, `hw_weapon.cpp:2264-2312`. VR runs two prepare passes over the same psprite list: `PreparePlayerSprites3D` keeps layers that resolve to a HUD model, `PreparePlayerSprites2D` keeps layers that don't. A muzzle-flash layer has no model of its own, so when its owning weapon *is* drawn as a model, the 3D pass draws the gun and the 2D pass still draws the flash — a flat billboard in front of a 3D mesh. The fix, gated behind `EXTERN_CVAR(Float, r_hudflatoverlay)` (declared `hw_weapon.cpp:124`; actually defined `src/rendering/r_utility.cpp:105`, `CVAR(Float, r_hudflatoverlay, 1.0f, CVAR_ARCHIVE)`), walks the owner's other psprite layers (`hw_weapon.cpp:2290-2306`) looking for a sibling `PSP_WEAPON`/`PSP_OFFHANDWEAPON` layer with the same `Caller` that resolves a model frame via `FindModelFrame`; if found, the overlay is either skipped outright (`r_hudflatoverlay <= 0`, `hw_weapon.cpp:2307`) or its alpha is scaled down (`hw_weapon.cpp:2308`, applied at `hw_weapon.cpp:2322` — after `GetWeaponRenderStyle` establishes the layer's own alpha, so it composes rather than overrides). Default `1.0f` means stock behavior is unchanged until a mod or user opts in. The guard `psp->GetID() != PSP_WEAPON && psp->GetID() != PSP_OFFHANDWEAPON` (`hw_weapon.cpp:2288`) is verified, not just claimed by the comment: the main/offhand weapon layers themselves are structurally excluded from this dimming, only auxiliary layers (flash, etc.) can be affected.

**3. Per-hand identity, the substantial fix.** `DrawPlayerSprites` (`hw_weapon.cpp:1483-1524`) previously decided which VR hand a psprite belonged to by calling `WeaponSpriteMatches(player->ReadyWeapon or OffhandWeapon, caller)`, which — unchanged in this window at `hw_weapon.cpp:1559-1586` — still falls back to `equippedWeapon->GetClass() == spriteCaller->GetClass()` (`hw_weapon.cpp:1566`). Class-equality cannot distinguish "the pistol in the left hand" from "the pistol in the right hand" when both hands hold the same weapon class. The rewrite (`hw_weapon.cpp:1500-1520`) stops asking `WeaponSpriteMatches` this question at all for hand attribution and instead reads the psprite's own layer id off `hudsprite.weapon` (a `DPSprite*` despite the field's name — confirmed in `HUDSprite`'s declaration, `src/rendering/hwrenderer/scene/hw_weapon.h:57-72`): `GetID() >= PSP_OFFHANDWEAPON` (`1000000`, `p_pspr.h:50`) means offhand, otherwise mainhand, with one exception — `PSP_FLASH` (`1000`) is a shared id used by both hands, so for that case the code instead compares `caller == hudsprite.owner->player->OffhandWeapon` by raw pointer identity (`hw_weapon.cpp:1512-1518`). The resolved `spriteHand` then drives both the wheel-suppression check (`VRWheel_ShouldSuppressWeaponHand`, `hw_weapon.cpp:1522`) and which controller transform `vrmode->AdjustPlayerSprites` applies (`hw_weapon.cpp:1526`). The companion change in `WeaponSpriteMatches` itself (`hw_weapon.cpp:1567-1586`) wraps the `SisterWeapon` pointer-var lookup in an `IsKindOf(NAME_Weapon)` guard (the `sisterOf` lambda, `hw_weapon.cpp:1576-1579`), because — per the comment, and consistent with the hand-model-as-psprite-layer design elsewhere in this window — a psprite's `Caller` is no longer guaranteed to be a `Weapon`; reading a `Weapon`-only pointer-var off an `Inventory`-caller layer is stated to be a fatal `ScriptVar` error, not a null.

**Surviving instance of the same ambiguity.** `WeaponSpriteMatches`'s `GetClass()`-equality path (`hw_weapon.cpp:1566`, unchanged) is still the sole hand-selection logic in two functions this window does *not* touch: `GetWeaponPosition2D` and `GetWeaponPosition3D` (`hw_weapon.cpp:1609`, `1642`, both `w.weapon = WeaponSpriteMatches(player->ReadyWeapon, psp->GetCaller()) ? readyWeaponPsp : offhandWeaponPsp;`). Both are called from the same prepare passes this window edited (e.g. `hw_weapon.cpp:2325`-area for the 2D pass) to source the bob offset added to a layer's screen position. With identical weapon classes in both hands, a shared-id layer (`PSP_FLASH`) belonging to one hand can still be class-matched against the other hand's `ReadyWeapon`, sourcing that layer's bob interpolation from the wrong hand's weapon. Same root cause the `DrawPlayerSprites` rewrite fixed, left open in two sibling call sites — see gap list.

### Bone-anchor resolution itself lives outside this file set

`hw_weapon.cpp`'s only participation in the anchoring mechanism beyond the `NoDraw`-style boilerplate is a single call, `HudAnchor_BeginFrame();`, at the top of `PreparePlayerSprites3D` (`hw_weapon.cpp:2371`). Everything else — the `g_hudAnchors` cache, `HudAnchor_Get`/`HudAnchor_GetOffset`/`HudAnchor_Store`, and the actual application of `psp->AnchorLayer`/`AnchorBone` to override a model's placement matrix — is implemented in `src/r_data/models.cpp` (`models.cpp:65-260`, application site `models.cpp:728-784`), which is not in this assignment's path list and is not audited here beyond confirming it exists, that it is the sole writer of the four anchor-output fields, and that `BeginFrame` is the reason a stale anchor doesn't survive into a frame where its target wasn't drawn (`models.cpp:79-93`, clears `AnchorBoneLive` on every psprite of `players[consoleplayer]` — note: local player only, consistent with this being HUD-only rendering state, not networked). Flagging its existence here only so the boundary between "this subsystem" and "the model/bone system" is precise: `hw_weapon.cpp` resets the cache and refuses to draw a suppressed layer; `models.cpp` does the actual bone math.

### `hw_weapon.cpp`: HUD model skin-alpha fix

`DrawPSprite` (`hw_weapon.cpp:178-198`) alpha-tests HUD model skins against `gl_mask_threshold`. That's correct when a skin's alpha channel means transparency and wrong when it doesn't — PBR texture sets routinely pack roughness/gloss into alpha, which is mostly dark, so most of the model failed the test and was discarded, "read[ing] as a broken mesh." The fix reads a new per-model flag, `ignoresSkinAlpha()` (`src/common/models/model.h:113`, `flags & (1 << 18)` — also new in this window, confirmed via `git log -S` on `model.h`, not pre-existing infrastructure this window merely started using) and drops the threshold to `0.f` for a flagged model (`hw_weapon.cpp:197`). Narrow, correctly scoped, unrelated to physics or hand identity.

### `p_pspr.cpp`: reload buttons rewritten to do nothing, on purpose

`ButtonChecks` (`p_pspr.cpp:103-135`) maps a `player->cmd.buttons` bit to a weapon state jump (`NAME_Reload`, etc.), consumed by `P_CheckWeaponButtons` (`p_pspr.cpp:869-897`) once per hand per tick. At baseline this table had, for each hand, both a generic `BT_RELOAD` row (both hands) and a per-hand row — `BT_MAINHANDRELOAD` for hand 0, `BT_OFFHANDRELOAD` for hand 1 — both jumping straight to `NAME_Reload`. This window removes both per-hand rows and replaces them with **commented-out** rows against differently-named, differently-purposed bits:
```
//{ 0, WRF_AllowReload,	WF_WEAPONRELOADOK,	BT_MAINHANDDROPMAG,	NAME_Reload },
```
```
//{ 1, WRF_AllowReload,	WF_OFFHANDRELOADOK,	BT_OFFHANDDROPMAG,	NAME_Reload },   // RS FORK -- see above
```
(`p_pspr.cpp:127, 134`). The comment directly above states why they must stay commented, as an author's claim about intended design (not independently reproducible without running the engine, so reported as a claim, not verified fact): `BT_MAINHANDDROPMAG`/`BT_OFFHANDDROPMAG` are meant to *release a magazine as a real, physical, catchable object* rather than trigger an instant reload state, and "routing either bit to `NAME_Reload` makes one press eject the magazine and instantly refill it, which cancels out and reads on screen as the button doing nothing at all." The generic `BT_RELOAD` rows for both hands (`p_pspr.cpp:106, 129`) are untouched — the flatscreen instant-reload key still works exactly as before.

`BT_MAINHANDDROPMAG` (`1<<20`) / `BT_OFFHANDDROPMAG` (`1<<28`) are real, live bits, not dead names: declared in `src/d_event.h:81-82`, populated from `buttonMap.ButtonDown(Button_MH_DropMag/Button_OH_DropMag)` into `cmd->buttons` in `src/g_game.cpp:1098-1099`, masked appropriately alongside the other per-hand buttons at `g_game.cpp:1130,1134`, and mirrored to ZScript as `BT_MAINHANDDROPMAG`/`BT_OFFHANDDROPMAG` constants in `wadsrc/static/zscript/constants.zs:892-893`. None of that plumbing is in this assignment's file set, but it establishes that the button press itself reaches the game layer correctly — the gap is entirely on the consuming side; see "Critical question" and the gap list.

**Also in this file, unrelated to any of the above:** the baseline commit (`1d2572bdcc`) this window branches from contained a literal duplicate local declaration in `DPSprite::SetState` — `int statelooplimit = 300000;` twice in the same scope, confirmed present at baseline via `git show 1d2572bdcc:src/playsim/p_pspr.cpp` — which is not legal to compile twice in one C++ scope. Commit `ba5a979dbb` ("Make the merged tree build", the first commit in this window per `git log 1d2572bdcc..main`) collapses it to one (`p_pspr.cpp:631`). This is a data point on the raw, not-yet-buildable state of the post-merge baseline this window started from, not a functional change.

### `weapons.zs`: two new switching flags, both fully wired within this file

`bool bHolsterHidden` (`weapons.zs:72`) and `bool bKeepWhenEmpty` (`weapons.zs:90`) are added to `Weapon`. Both are consumed inside `Weapon::CheckAmmo` in the same file: `bHolsterHidden` short-circuits the whole function to `false` unconditionally first (`weapons.zs:1084-1085`, "checked first ... so none of the ammo-optional/dehacked/EitherFire branches below get a chance to override it"), making a stowed weapon permanently ineligible as a switch target; `bKeepWhenEmpty` guards the two `autoSwitch`-triggered calls to `PlayerPawn(Owner).PickNewWeapon` (`weapons.zs:1099, 1149`) so that running out of ammo no longer yanks the weapon out of the player's hand — necessary, per the header comment, because a hand-reload flow (see `BT_*DROPMAG` above) requires the empty gun to still be the equipped weapon when a fresh magazine is seated. Neither flag has a setter anywhere else in this repository (`bHolsterHidden`/`bKeepWhenEmpty` assignment sites grepped across `wadsrc/`: none found besides the field declarations); `bHolsterHidden` has exactly one other reader, `wadsrc/static/zscript/ui/statusbar/alt_hud.zs:582` (`if (weapon.bHolsterHidden) return;`, skipping ammo display for a stowed weapon) — outside this assignment's file set but confirms the flag is live, just written by something outside this repository (an external holster mod, per the field's own stated design intent — "lives here rather than on any one mod's weapon base class so ANY VR holster/stowage system ... gets the same behaviour for free," a comment-claim about intent, not verified against any such mod since none ships here). `bHolsterHidden` was added as a plain field rather than a `flagdef`; the adjacent comment claims `+WEAPON.AMMO_OPTIONAL` and `+WEAPON.NOAUTOSWITCHTO` "both fail from a pk3 while working fine in the engine's own scripts" — reported here as a comment-claim only, about the flagdef/parser system, which is outside this assignment's scope and not independently checked.

### `hw_drawinfo.cpp`: FogDisturb, Standing Shapes, sweep room — not weapon rendering

For completeness, since the file was assigned: every hunk is inside `HWDrawInfo::StartScene`. (1) A one-line fix, `liveDisturb = i + 1;` replacing `liveDisturb++` (`hw_drawinfo.cpp:423`), changing a live-fog-disturbance counter from a count to a high-water mark — the comment explains disturbance slots are recycled out of order, so a sparse live set (e.g. only slots 0 and 5 occupied) under the old counting scheme stopped the shader loop at 2 and silently dropped slot 5; matches the pre-existing pattern the "Standing Shapes" loop already used (`live = i + 1` a few hundred lines below, at `hw_drawinfo.cpp:518`, unchanged). (2) A parent-child chaining system for "Standing Shapes" (`hw_drawinfo.cpp:484-599`): four new per-slot arrays (`resolvedPos/Yaw/Pitch/Roll`, `hw_drawinfo.cpp:491-494`) let a shape at slot `i` read an already-resolved parent transform at any slot `< i` (`ShapeParent[i]`, `hw_drawinfo.cpp:532`) and compose its own position/orientation onto it via a forward/right/up basis built from the parent's yaw/pitch/roll, feeding a new shader uniform slot `mShapeE` (pitch/roll, `hw_drawinfo.cpp:509, 601`) alongside the pre-existing `mShapeA/B/C/D`. (3) A new "sweep room" bounding box pushed to `VPUniforms.mSweepRoomMin/Max` (`hw_drawinfo.cpp:611-624`). None of this touches `DPSprite`, psprites, or weapon/hand draw paths — it belongs to a decal/prop "shapes" and volumetric-fog rendering feature entirely separate from this subsystem, and is not analyzed further here.

### Critical question: does this window connect weapon/hand rendering to the physics module?

**No. As of HEAD, weapon/hand rendering and the physics module are separate systems, with zero code coupling in either direction, and this window does not change that.** Evidence:

- No header coupling: neither `hw_weapon.cpp` nor `hw_drawinfo.cpp` `#include`s anything physics-related, and neither `p_physics.h` nor `p_physics.cpp` includes `p_pspr.h` or anything psprite-related (checked both directions directly).
- `p_physics.cpp` (2555 lines) contains zero references to `DPSprite`, `AnchorBone*`, `NoDraw`, `bHolsterHidden`, `bKeepWhenEmpty`, or `GetCaller()` — confirmed by grep across the whole file.
- The physics module's own hand representation is driven entirely by `AttackPos`/`OffhandPos` (`AActor` fields, declared `src/playsim/actor.h:1734` area) and `AttackAngle`/`OffhandAngle`/`MainHandRoll`/`OffhandRoll` — read at `src/playsim/p_physics.cpp:1590-1600` to place kinematic "hand" collision bodies (`PhysBody`, `g_bodies`) — none of which originate from a psprite or from this window's new fields.
- The new `AnchorBoneWorld`/`AnchorBonePos`/`AnchorBoneAngles`/`AnchorBoneLive` fields are, by the field comments' own stated purpose, built specifically to be diffed against `AttackPos`/`OffhandPos` for a grab test — but that comparison exists in this repository only as a comment/example in `wadsrc/static/zscript/actors/player/player.zs:3151-3152`, never as executed code. A repository-wide grep for `AnchorBoneWorld`/`AnchorBonePos`/`AnchorBoneAngles`/`AnchorLayer`/`AnchorBone` outside `p_pspr.h`/`p_pspr.cpp`/`models.cpp` turns up exactly that one file, and no assignment of `.AnchorLayer`/`.AnchorBone` anywhere in `wadsrc/` — meaning no content shipped with this repository ever activates bone anchoring at all; it is present only as dormant infrastructure for an external consumer.
- Symmetrically, the `BT_MAINHANDDROPMAG`/`BT_OFFHANDDROPMAG` buttons — the input side of what would be a physical, physics-adjacent reload — are fully wired from controller input to `cmd->buttons` (`g_game.cpp`) and to a ZScript constant (`constants.zs`), but have no reader anywhere in this repository: not in `p_pspr.cpp` (explicitly commented out), not in `p_physics.cpp`, not in any `.zs` file under `wadsrc/`.

What this window *does* do is lay groundwork clearly aimed at such a bridge — matching coordinate frames for a future grab test, a magazine-drop input path with no instant-reload side effect to get in the way, a `bKeepWhenEmpty` flag so the weapon survives long enough to be reloaded by hand — but none of it is connected end-to-end within this repository as of HEAD. Whatever performs the actual grab/reload logic, if it exists, is not part of `E:/UZDXREMA`.



---

## 5. Shaders and postprocess

Ten of the sixteen assigned files have no diff at all in `1d2572bdcc..main`: `src/common/rendering/gl/gl_shader.h`, `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp`, `hw_postprocess_cvars.cpp`, `src/common/rendering/vulkan/renderer/vk_renderpass.cpp`, `src/common/rendering/gl/gl_renderstate.cpp`, and `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp` are all byte-identical to baseline (confirmed individually, `git diff 1d2572bdcc main --stat -- <path>` empty for each). The real work is in six files, 472 insertions / 25 deletions total. None of it is a lighting-model change and none of it is VR-specific: `main.fp`'s diff contains zero occurrences of eye-index, stereo, HMD, OpenXR/OpenVR, controller, or BRDF/PBR terms (checked by pattern search over the diff text). What it is: three new pieces of this fork's bespoke decorative-VFX shader library (a freestanding oriented-plane shape primitive, a room-bounded gate + new fill mode for the laser-sweep lattice, a raised live-slot cap for fog disturbances), one hot-path optimization, and — in `present.fp`/`PresentUniforms`/`vk_postprocess.cpp` — the repair of a genuine merge-inherited defect in the color-grading pipeline that fed the VR headset's present path specifically.

### `main.fp`: standing shapes, a sweep-lattice room gate, and two perf fixes (+307/-10)

**Standing shapes.** `ShapesAt()` (wadsrc/static/shaders/glsl/main.fp:1064) decals an existing G-buffer surface — it only runs where a fragment already has a world normal, and it borrows that surface's own plane. This window adds `orient == 3` as a value of the existing `kind + 16*orientation` packing and gives it to a new sibling function instead of extending `ShapesAt()`: `ShapesAt()` now explicitly skips it (`if (orient == 3) continue;`, main.fp:1119, labeled "standing -- StandingShapesAt() owns these"), and the new `StandingShapesAt(vec3 fragPos)` (main.fp:1243-1389) owns every orient-3 slot.

Mechanism: for each live slot (`nshapes = int(uShapeParams.w)`, main.fp:1250, looping to a hardcoded `128` with an early `break` past the live count, main.fp:1259) it builds a world-space plane from a center (`uShapeA[i].xyz`), a yaw (`uShapeB[i].y`), and now a pitch/roll pulled from the new `uShapeE[i].xy` (main.fp:1298-1300), constructs a right/up basis via `cross()` against world-up with a fallback to world+X when the facing normal is within 0.999 of straight up/down (main.fp:1309-1311, avoiding the degenerate near-zero-length cross product), then solves the standard ray/plane `t = dot(N, planePoint - rayOrigin) / dot(N, rayDir)` (main.fp:1322-1324) between the eye and the fragment. A hit is accepted only if `0 < t < fragDist` (main.fp:1334) — an explicit branch, not a clamp, so that a standing shape occluded by real geometry simply doesn't draw rather than ghosting through the wall in front of it. The resulting UV feeds the identical SDF kind-switch, radial/grid repeat modes, and seam-split logic `ShapesAt()` already uses (main.fp:1345-1389) — no duplicated distance-field code, only a different plane-projection front end. It is invoked unconditionally from `main()` (main.fp:3485, `frag.rgb += StandingShapesAt(pixelpos.xyz);`), directly after the existing `if (dot(vWorldNormal...)>0.5) ShapesAt(...)` call (main.fp:3483-3484) — unlike decal shapes, a standing shape has to draw against open air/sky where there is no surface normal to gate on.

Cheap reject is done against the ray's closest approach to the anchor (main.fp:1284, `reach = size + spread + soft + uShapeParams.z + 1.0`, then a perpendicular-distance-squared test), not against `fragPos`, because for a standing shape the fragment under the cursor can be an unrelated wall far behind or to the side of the shape.

**Sweep-lattice room gate and pickets fill.** `SweepAirLattice()` (main.fp:1887) draws a "laser grid hanging in the air" version of the sweep-band system by intersecting the view ray with one of three infinite planes (`shape==2/3/5`, perpendicular to X/Z/Y respectively, main.fp:1928-1932 — unchanged this window). Because those planes have no extent, a window looking toward one showed lattice lines in a room the sweep band had never entered. This window adds a room test at the ray/plane hit point (`hit`, computed pre-existing at main.fp:1961): `uSweepRoomMax.w > 0.0` gates a signed-distance-to-AABB test (`outv = max(uSweepRoomMin.xyz - hit, hit - uSweepRoomMax.xyz)`, main.fp:1990-1991) that smoothsteps a `roomFade` to zero outside the box (main.fp:1983-1995), tested at the hit point rather than `fragPos` deliberately ("a grid seen through a doorway is outside the room even though the wall beyond it is inside one"). `Max.w == 0` (a level that never publishes a room) skips the whole test at the cost of one compare per band.

A fourth band-fill mode, `bfill == 4` ("pickets", main.fp:2013-2052), draws floor-to-ceiling bars with no vertical repeat term at all — height comes free from the room geometry. Its bar spacing is derived from the room's own extent along whichever world axis the band's `uv.x` reads (Z for `shape==2`, X for `shape==3` and `shape==5` — verified consistent with the `uv` axis selection at main.fp:2001-2003) and snapped to a whole number of bars (`n = floor(span/across + 0.5)`, main.fp:2043-2045) so a bar always terminates exactly at the wall rather than being cropped mid-bar.

**Fog-disturbance loop cap: 8 to 32, with a correctness fix riding along.** Two consumers — the flat-glow "state pulse" gather in `GlowTextureAt()` (main.fp:1503-1516) and the "ignite" light-gather in `FogSlabAt()` (main.fp:2483-2490) — had a hardcoded `for (int di = 0; di < 8; di++)` with no relation to how many disturbance slots were actually live. Both are now `for (int di = 0; di < 32; di++) { if (di >= ndist) break; ... }`, where `ndist`/`ndisturb` is `int(uFogBow.w)`. `uFogBow` is otherwise "a sweep band pushes mist ahead of it" (x strength, y width, z thin-behind ratio — hw_viewpointuniforms.h:274-276); `.w` is repurposed as the live-disturbance high-water mark, a repurposing that already existed at baseline in principle (see below) but had no shader consumer actually breaking on it before this window — the two loops previously walked all 8 (now would be all 32) slots unconditionally regardless of `.w`.

**Flat-glow line search: O(n) sqrt/divide removed to O(1).** `getLightColor()`'s floor/ceiling glow-line proximity search (main.fp:2860, code at 3027-3113) runs on every floor and ceiling fragment whenever `uFlatGlowColor.a > 0` — the fork's own comment calls it "the largest per-fragment cost this fork adds" (main.fp:3035). Previously it called `length()` (a sqrt) per linedef segment inside the loop. This window converts the whole search to squared distance (`minDistSq`, main.fp:3046, 3071) — valid because `minDist` is only ever used in `minDist < reach` and `minDist / reach`, both order-preserving under squaring — and takes a single `sqrt()` once, after the loop (main.fp:3074). A per-segment bounding-box reject was added in front of the real solve (main.fp:3061-3065): the segment's own XZ box grown by the largest reach the not-yet-evaluated glow wave could produce (`rejectR`, main.fp:3053, conservatively computed from `uGlowWaveDepth.x` since the real per-fragment `reach` depends on a wave phase evaluated later in the function). This is a pure optimization; the output is unchanged.

### `hw_viewpointuniforms.h`: three additions to the per-eye, per-frame uniform block (+53/-5)

`HWViewpointUniforms` is resolved once per eye per frame by `HWDrawInfo::StartScene()` (src/rendering/hwrenderer/scene/hw_drawinfo.cpp — not in this subsystem's assigned paths, but the only producer of this struct's contents, so cited for grounding) and uploaded as a std140 UBO consumed by both the GL and Vulkan uniform-block declarations described below.

| Field | Type | Byte delta | Populated at | Consumed at |
|---|---|---|---|---|
| `mFogDisturbA/B[32]` (was `[8]`) | `FVector4[32]` ×2 | +768 B | hw_drawinfo.cpp:395-412 | main.fp `GlowTextureAt`/`FogSlabAt` |
| `mShapeE[128]` (new) | `FVector4[128]` | +2048 B | hw_drawinfo.cpp:598 (`{pitch, roll, 0, 0}`) | main.fp `StandingShapesAt` |
| `mSweepRoomMin/Max` (new) | `FVector4` ×2 | +32 B | hw_drawinfo.cpp:615-624 | main.fp `SweepAirLattice` |

Net +2,848 bytes per `HWViewpointUniforms` instance (hw_viewpointuniforms.h:251-252, 430, 452-453). The Vulkan uniform block instances this struct as `viewpoints[2]` (one slot per eye, per vk_shader.cpp's own comment at line 371-377), so the Vulkan-path UBO grows by roughly 5.7 KB total; the GL path uploads one instance per eye-pass rather than a resident array of two, so its per-upload growth is the same +2,848 bytes, not doubled.

`mShapeE[i] = {pitch, roll, 0, 0}` is documented (hw_viewpointuniforms.h:416-429) as deliberately appended after the two `float mPadding1/mPadding2` tail fields rather than beside the related `mShapeD` array in the middle of the struct, specifically so it changes no existing field's byte offset — every field between `mShapeD` and the padding (`mShapeParams`, `mShapeUnder`, `mFogFollow`, the upstream thick-fog pair) is matched to its GLSL counterpart by offset, not by name, so an insertion anywhere but the true tail would silently desync the two. `mSweepRoomMin.w` carries a soft-edge distance and `mSweepRoomMax.w` doubles as the enable flag (0 = unbounded/no room published) — one field pulling double duty rather than a fourth boolean uniform.

**Does this relate to physics-driven object placement? No.** `mShapeA`/`mShapeB`/`mShapeE`'s position and orientation are resolved in `HWDrawInfo::StartScene()` (hw_drawinfo.cpp:496-580, read for context, not part of this subsystem's diff) by straightforward CPU-side kinematics: `yaw = ShapeAngle[i] + ShapeYawRate[i] * age` (and identically for pitch/roll — base value plus a per-slot deg/sec rate times elapsed time since spawn, hw_drawinfo.cpp:598-600), with an optional static parent link that composes a child's authored local offset/rotation onto its parent's *already-resolved* transform earlier in the same slot-ordered loop (hw_drawinfo.cpp:527-573). There is no velocity integration, no collision response, and no reference to any physics or collision system anywhere in this resolve path — it is scripted rate-based animation plus rigid Euler-angle composition (the code's own comment at hw_drawinfo.cpp:569-571 calls the pitch+roll composition case "an approximation," an authored admission of Euler-composition error, not a physics claim). This is a distinct mechanism from the native VR hand/object physics module; nothing in the shader/postprocess diff touches or depends on that module.

### Vulkan/GL shader-compilation plumbing: mirroring the new fields into two hand-written GLSL string literals

`gl_shader.cpp` and `vk_shader.cpp` each embed the `ViewpointData`/anonymous uniform block as a plain C++ raw string (`static const char *shaderBindings = R"(...)"`, vk_shader.cpp:182, concatenated verbatim at vk_shader.cpp:609,636 — no substitution of any kind). Both files' diffs are the textual mirror of the `hw_viewpointuniforms.h` change: `uFogDisturbA/B[8]` → `[32]` (gl_shader.cpp:368-369; vk_shader.cpp:263-264) and `uShapeE[128]`, `uSweepRoomMin`, `uSweepRoomMax` appended after `uThickFogMultiplier` (gl_shader.cpp:402-404; vk_shader.cpp:301-303) — same tail-append-only rule as the C++ struct, for the same offset-matching reason.

Vulkan needs one thing GL does not: three new `#define` indirection macros (vk_shader.cpp:378-380, `#define uShapeE viewpoints[HW_VIEWPOINT_INDEX].uShapeE` and equivalents for `uSweepRoomMin`/`uSweepRoomMax`). This is not new machinery — every other `ViewpointData` field already has one (e.g. the preexisting `#define uFogFollow viewpoints[HW_VIEWPOINT_INDEX].uFogFollow` immediately above, vk_shader.cpp:370) — because Vulkan's `ViewpointUBO` is declared as an indexed array (`viewpoints[2]`), not GL's bare anonymous block, so `main.fp` code referencing the bare name `uShapeE` would not resolve under the Vulkan backend without it.

`gl_shader.h` has no diff in this window — no C++-side struct or binding-index change was needed on the GL side beyond the string literal.

### `present.fp` / `PresentUniforms` / `vk_postprocess.cpp`: closing a merge-inherited struct/usage mismatch, and fixing the color grade actually reaching the headset

**The baseline (`1d2572bdcc`) does not compile as merged.** `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` is not needed to see this; it's visible from the two assigned files alone. At commit `1d2572bdcc`, `src/common/rendering/vulkan/renderer/vk_postprocess.cpp` already reads/writes `uniforms.Brightness` three times inside `DrawPresentTextureToImage()` (baseline lines 365, 378, 395), but `PresentUniforms` in `hw_postprocess.h` at that same commit has no `Brightness` member at all (verified directly: `git show 1d2572bdcc:.../hw_postprocess.h | grep Brightness` returns nothing). Upstream UZDoom 5.0 dropped the `vid_brightness` cvar and the corresponding struct field; the fork's own `DrawPresentTextureToImage` — a VR/XR-only present path, present at baseline and unchanged in scope by this window — never stopped depending on it. This window's `hw_postprocess.h` change re-adds the field (`float Brightness;`, hw_postprocess.h:975, with the accompanying `EXTERN_CVAR(Float, vid_brightness)` restored in hw_postprocess_cvars.h:80, noted there as "kept after upstream removed it") — i.e., it is the fix for a real, literal compile break at the window's own start boundary, not a stylistic add.

Because `PresentUniforms` is matched to its GLSL declaration by std140 offset with no explicit per-field offsets (hw_postprocess.h:994-1005), inserting `Brightness` between `Contrast` and `Saturation` shifted every subsequent scalar by 4 bytes and broke the 8-byte alignment `FVector2 Scale`/`Offset` need under std140. The fix adds one `float padding0` immediately before `Scale` (hw_postprocess.h:983) — verifiable by arithmetic, not just by the accompanying comment: 8 scalars (32 bytes) preceded `Scale` before this change (already 8-byte aligned), 9 scalars (36 bytes) would misalign it, 10 (40 bytes) restores alignment — and backs it with two `static_assert(offsetof(...) % 8 == 0, ...)` guards (hw_postprocess.h:1009-1014) so a future field insertion here fails to compile instead of silently corrupting the layout.

**`vk_postprocess.cpp` (+22/-3 lines):** both `PresentUniforms` local declarations move from default-init (`PresentUniforms uniforms;`) to value-init (`= {}`, vk_postprocess.cpp:299, 368), justified in-place as needed because "the whole struct is memcpy-ed into the uniform buffer" and not every member is set on every branch. `DrawPresentTexture()` (the flat/mirror-window/screenshot present, vk_postprocess.cpp:289-354) gains only the two new `Brightness` assignments (identity `0.0f` at line 304, `clamp<float>(vid_brightness, -0.8f, 0.8f)` at line 313) — its `BlackPoint`/`WhitePoint` handling already existed and is untouched. `DrawPresentTextureToImage()` (the VR/XR swapchain-image present, vk_postprocess.cpp:356-432) is where the second, independent defect lived: both of its branches already read/wrote `Brightness` at baseline (hence the compile break above) but neither branch set `BlackPoint`/`WhitePoint` at all — meaning, once the struct-mismatch were hypothetically patched any other way, this path would feed the shader's `val = val*(WhitePoint-BlackPoint)+BlackPoint` (present.fp:57) with indeterminate stack bytes, for the image actually submitted to the headset compositor, while the desktop mirror path was unaffected. This window adds explicit identity values (`BlackPoint=0`, `WhitePoint=1`, vk_postprocess.cpp:379-380) to the `!applyGamma` branch and the real cvar-driven values (`clamp<float>(vid_i_blackpoint,...)`, `clamp<float>(vid_i_whitepoint,...)`, vk_postprocess.cpp:396-397) to the `applyGamma` branch, matching what `DrawPresentTexture()` already did. The OpenXR present-bias block that follows (`vr_openxr_present_gamma_bias` etc., vk_postprocess.cpp:404-415) is untouched context — it, and the whole XR-bias apparatus, predates this window.

**`present.fp`'s `ApplyGamma()` (+15/-2):** `Contrast` and (now) `Brightness` are moved to run first, on the gamma-encoded value straight after the HDR clamp (present.fp:44-45), ahead of upstream's `pow(c.rgb, 2.2)` re-linearize step (present.fp:51) — where the diff's own comment states this fork has always applied them, versus running post-linearize (pivoting contrast around 0.5 in *linear* light, "roughly 73% perceptual brightness once re-encoded"). Since `Contrast`/`Brightness` can now push a channel negative before the re-linearize `pow()` (undefined for a negative base in GLSL), that `pow()` call gained a `max(c.rgb, vec3(0.0))` guard (present.fp:51) — the pre-existing final `pow(max(val,0.0), InvGamma)` already had an equivalent guard, so this closes the one remaining unguarded `pow()` in the function. `Saturation` and the `BlackPoint`/`WhitePoint` remap remain where they were, after the re-linearize step (present.fp:53-58) — i.e. the pipeline now deliberately mixes color spaces across grade stages (contrast/brightness pivoted in gamma-encoded space, saturation and black/white-point pivoted in linear space), a design choice the diff's comment frames as restoring historical parity, not colorimetric purity.

### `volumetricbeam.fp`: dust-noise contrast before the per-step average (+18)

The flashlight-beam volumetric march (main march loop at volumetricbeam.fp:143-197, unchanged this window) accumulates a per-step `contrib` and divides by `steps` to normalize density against quality settings. Dust motes are sampled as raw value noise (`dustNoise()`, volumetricbeam.fp:47-50) and multiplied into `contrib` per step; averaging that raw noise over ~24 steps converges toward its mean, which the diff's comment states measured out to "a flat ~15% dimming... with almost no spatial structure" — correctly-computed motes averaged into invisibility. The fix inserts `d = smoothstep(0.22, 0.78, d);` immediately after the noise sample and before it's mixed into `contrib` (volumetricbeam.fp:206), pushing the field toward its extremes so the post-divide average still carries visible contrast between a thick patch and a thin one. `DustAmount` (pre-existing) still controls how strongly the (now contrast-boosted) field darkens the beam via `mix(1.0, d, clamp(DustAmount,0,1))` (volumetricbeam.fp:210) — a separate axis from the new curve, so there is no cvar/uniform path to retune the `0.22`/`0.78` shape itself without a shader edit.

### `hw_clock.cpp`: benchmark-header adaptation to the 5.0 version API (+7/-2, glue not capability)

`AppendBenchmarkHeader()`'s `"Git describe: %s\n"` line called `GetGitDescription()` (removed by UZDoom 5.0's revision-reporting rework — confirmed absent from `src/version.cpp`/`version.h`, which instead export `GetGitTag()`, `GetGitDistance()`, `GetGitTime()`, all present at src/version.cpp:65,76,87). This window's one-line change (hw_clock.cpp:288, `"Git describe: %s (+%d) @ %s\n", GetGitTag(), GetGitDistance(), GetGitTime()`) is a mechanical adaptation to that renamed API so the benchmark log still records a build fingerprint — not new functionality.

### Cross-cutting: five files, one number

Raising a live-slot cap in this shader library is not a single-source-of-truth edit. To take `MAX_FOG_DISTURB` from 8 to 32 this window, five files needed the same literal changed by hand in lockstep: `src/g_levellocals.h:1562` (the named C++ constant, sizing the CPU-side arrays), `hw_viewpointuniforms.h:251-252` (the C++ struct array sizes), `gl_shader.cpp:368-369` and `vk_shader.cpp:263-264` (the two independent raw-string GLSL mirrors), and `main.fp:1512,2487` (the two consumer loop bounds). None of these are linked by a shared constant reachable from GLSL — `shaderBindings` is a bare string literal with no substitution (vk_shader.cpp:182) — so nothing but the two new `static_assert`s (which check std140 *alignment*, not array-length agreement) would catch a future mismatch at compile time. The `Brightness`/`PresentUniforms` defect documented above is a live demonstration that this class of mismatch (a member/array referenced on one side of the C++/GLSL boundary but not declared to match on the other) is not hypothetical for this codebase — it shipped in the window's own starting commit.

The `128`-slot shape cap (`MAX_SHAPES`, src/g_levellocals.h:1316, unchanged this window) and the sweep system's `8`-band cap (main.fp:1900, `for (int sb = 0; sb < 8; sb++)`, unchanged this window) are the same kind of fixed, hand-duplicated compile-time ceiling; the `128` literal recurs independently in `main.fp` at lines 1086 (`ShapesAt`), 1259 (`StandingShapesAt`), 1671 and 1757 (`BeamLightAt`/`BeamAirGlow` — a separate, coincidentally-same-sized beam-count cap, not the shape cap).


---

## 6. Script API surface

### Scope and method

Nine assigned files; `src/common/scripting/backend/codegen.cpp` has zero diff in this window (`git diff 1d2572bdcc main -- src/common/scripting/backend/codegen.cpp` produces no output — confirmed empty, not merely unexamined). `src/common/scripting/frontend/zcc_parser.cpp` and `src/common/scripting/interface/vmnatives.cpp` are touched but carry no new script capability (see §7). Everything below is cited to current-HEAD file:line and cross-checked against the actual C++ binding, not inferred from doc comments; every stale or unverifiable comment found in the process is called out explicitly rather than repeated as fact.

The new capability clusters into four groups: a rigid-body physics API on `Actor` (12 natives + 1 flag), VR hand/grip state fields on `Actor` (7 fields), reflection/utility/shape natives on `LevelLocals` (6 natives), and a bone-anchoring API on `PSprite` (9 fields). One enum is new (`EGripSubject`), two button constants are renamed, and two files carry non-functional build/dedup fixes.

### 1. Rigid-body physics: `Physics*` natives and `bPhysicsBody` on `Actor`

Declared `wadsrc/static/zscript/actors/actor.zs:969-1020`, inside a block headed "RS FORK -- rigid-body physics. See src/playsim/p_physics.h." (actor.zs:972). All twelve are bound via `DEFINE_ACTION_FUNCTION_NATIVE(AActor, ...)` in **`src/playsim/p_physics.cpp`** (lines 2177-2551) — that file is outside this subsystem's assigned paths (owned by the physics-module review), so it is cited here only to verify the script-facing contract, not analyzed in depth.

Mechanism, verified against `p_physics.cpp` and `p_mobj.cpp`: `PhysicsEnable` (p_physics.cpp:2177) sets `AActor::flags9 |= MF9_PHYSICSBODY` (p_physics.cpp:2164) and pushes a body onto a global `g_bodies` array; `PhysicsDisable` (p_physics.cpp:2198) reverses both. `P_MobjThinker` gates on that bit: `if (flags9 & MF9_PHYSICSBODY)` (`src/playsim/p_mobj.cpp:4607`) skips scroller/carry accumulation, `P_XYMovement`, `P_ZMovement`, `P_CheckOnmobj` and `Crash()` entirely for the actor — confirming the actor.zs claim ("Doom's movement is skipped entirely for it") against the actual gate, not just the comment. Mass is kilograms, collision half-extents and centre-of-mass offset are metres, in the model's own local axes (actor.zs:975-984).

`PhysicsGrab(int hand)` / `PhysicsRelease()` (actor.zs:1015-1016; p_physics.cpp:2386, 2481) are the preferred hold path over `PhysicsSetHeld`/`PhysicsSetTransform`: `PhysicsGrab` records `grabPosOffset`/`grabRotOffset` in the grabbing hand's local frame (p_physics.cpp:2367-2376) and sets `kinematic = true`, so the engine — not script — carries the body every physics step at the pose it had at grab time; `PhysicsRelease`'s own comment ("velocity is already correct... a throw needs no separate calculation") is corroborated by the grab path never touching the body's velocity, only its kinematic-follow offsets. `hand` is a plain 0=main/1=off index, consistent with every other hand-pair field in this diff (`GripClaimMain/Off`, `HolsterClaimMain/Off`). `PhysicsIsAsleep`, `PhysicsIsHeld` and `PhysicsDistanceTo` are declared `clearscope ... const` (actor.zs:1002, 1017, 1020) — callable from render-thread/UI script scope, unlike the nine mutating calls, which are not `clearscope` and are therefore restricted to normal playsim scope.

`+PHYSICSBODY` is registered as `DEFINE_FLAG(MF9, PHYSICSBODY, AActor, flags9)` at `src/scripting/thingdef_data.cpp:344`. The `DEFINE_FLAG` macro (thingdef_data.cpp:60) grants `VARF_Native` only, with none of `DEFINE_PROTECTED_FLAG`'s (thingdef_data.cpp:61) `VARF_ReadOnly|VARF_InternalAccess` — so, like every other plain actor flag, `bPhysicsBody` is fully script-readable **and script-writable** as a boolean pseudo-field, the same exposure mechanism as the sibling flags immediately above it in the same table (`ISPUFF`, `FORCESECTORDAMAGE`, `NOAUTOOFFSKULLFLY`, thingdef_data.cpp:339-343). See Gap 5.

| Native | Signature | Semantics | Readonly |
|---|---|---|---|
| PhysicsEnable | `native void PhysicsEnable(double massKg, double halfX, double halfY, double halfZ, double comX=0, double comY=0, double comZ=0);` | Registers the actor with the solver; sets `MF9_PHYSICSBODY`, handing movement to the physics frame | — |
| PhysicsDisable | `native void PhysicsDisable();` | Unregisters and clears `MF9_PHYSICSBODY`, returning the actor to normal Doom movement | — |
| PhysicsAddImpulse | `native void PhysicsAddImpulse(double x, double y, double z);` | kg·m/s impulse at the centre of mass | — |
| PhysicsAddSpin | `native void PhysicsAddSpin(double x, double y, double z);` | Adds angular velocity, radians/sec | — |
| PhysicsSetImpactSound | `native void PhysicsSetImpactSound(sound snd, double minSpeed=0.6);` | Impact sound plus the minimum impact speed (m/s) that triggers it | — |
| PhysicsIsAsleep | `native clearscope bool PhysicsIsAsleep() const;` | True once the solver has parked the body and stopped simulating it | query |
| PhysicsSetHeld | `native void PhysicsSetHeld(bool held);` | Freezes/unfreezes solver motion for script-driven placement (legacy path; prefer PhysicsGrab) | — |
| PhysicsSetTransform | `native void PhysicsSetTransform(double x, double y, double z, double yaw, double pitch, double roll);` | Places a held body; position in map units, angles in degrees | — |
| PhysicsGrab | `native void PhysicsGrab(int hand);` | Kinematically attaches the body to hand 0 (main) or 1 (off) at physics-frame rate, keeping the grabbed pose | — |
| PhysicsRelease | `native void PhysicsRelease();` | Detaches from the grabbing hand; no impulse needed, velocity was already inherited | — |
| PhysicsIsHeld | `native clearscope bool PhysicsIsHeld() const;` | True while attached via PhysicsGrab | query |
| PhysicsDistanceTo | `native clearscope double PhysicsDistanceTo(double x, double y, double z) const;` | Metres from a map-space point to the body's collision **shape**, not its origin | query |
| `bPhysicsBody` (MF9_PHYSICSBODY) | actor flag | Set by PhysicsEnable / cleared by PhysicsDisable; gates `P_MobjThinker`'s native-movement skip (p_mobj.cpp:4607) | **N** (plain DEFINE_FLAG) |

### 2. VR hand/grip state fields on `Actor`

Declared actor.zs:412-449, bound via `DEFINE_FIELD(AActor, ...)` at `src/scripting/vmthunks_actors.cpp:2247-2255`, backed by new members of `src/playsim/actor.h:1792-1827` (outside assigned scope, cited for the storage/writer trail only). All four scalar fields (`GripSubjectMain/Off`, `FingerTouchMain/Off`) plus `TwoHandedHold` are written every VR tic by `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp:3589-3605` — that file predates this window (device layer, per this fork's attribution), but the specific write statements to these specific fields are necessarily new, since the fields themselves did not exist before this window. `TwoHandedHold` is derived there as `offSubject == GRIPSUBJ_Support || GRIPSUBJ_Forend || GRIPSUBJ_Foregrip` (vk_openxrdevice.cpp:3602-3604) — a hand actually on the other hand's weapon grip/forend/foregrip, not merely close to it.

`GripClaimMain/Off` are the only writable pair: script asserts what a hand has closed on (an `EGripSubject` value, §6), the engine arbitrates into `GripSubjectMain/Off`, overriding to the holster subject while `HolsterClaimMain/Off` (pre-existing) is active. The actor.zs comment (429-434) documents a multi-writer convention — "SET your subject while you hold it, and CLEAR it only when the current value is one of yours" — with no engine-enforced single ownership; this is a deliberate, documented design choice, not a gap.

| Field | Type | Semantics | Readonly |
|---|---|---|---|
| FingerTouchMain / Off | `native readonly int` | Capacitive contact bitmask, main/off hand: bit 0 = thumb resting, bit 1 = index resting on trigger (contact, not press) | Y |
| GripClaimMain / Off | `native int` | Script-asserted `EGripSubject` the hand has closed on; multiple mods may write, by convention only (see above) | N |
| GripSubjectMain / Off | `native readonly int` | Engine-arbitrated result: the claim, unless the hand is inside a holster | Y |
| TwoHandedHold | `native readonly bool` | True while the off hand is genuinely on the main weapon's grip/forend/foregrip; weapons read it to tighten spread, it does not move the weapon | Y |

### 3. `A_SetUserVarName`

`wadsrc/static/zscript/actors/actor.zs:1579`; native body `src/playsim/p_actionfunctions.cpp:3119-3134`. Same shape and same helper (`GetVar`, p_actionfunctions.cpp:3061-3070) as the pre-existing, deprecated `A_SetUserVar`/`A_SetUserVarFloat` (p_actionfunctions.cpp:3072-3096) — deliberately **not** marked deprecated, because it solves a problem casting cannot: writing a `Name`-typed field on a class defined in a pk3 compiled after the caller's own, where a direct `SomeClass(actor)` cast cannot resolve. It writes `value.GetIndex()` (the `FName`'s raw table index) through the field's existing `PType::SetValue(uint8_t*, int)` path — exactly the mechanism `A_SetUserVar` already uses for plain ints, since `PName` is a `PInt` subtype (per the comment at p_actionfunctions.cpp:3108-3111, and confirmed structurally: no separate storage path is added).

| Native | Signature | Semantics | Readonly |
|---|---|---|---|
| A_SetUserVarName | `native void A_SetUserVarName(name varname, name value);` | Writes an FName's raw index into a Name-typed user field located by name; lets two independently-loaded pk3s exchange a class/actor name by string with no compile-time reference | — |

### 4. `LevelLocals` additions

All six declared in `wadsrc/static/zscript/doombase.zs`, bound in `src/scripting/vmthunks.cpp`. Backing storage for the shape/room natives is new fields on `FLevelLocals` in `src/g_levellocals.h`.

**`GetRawStickMove()`** (doombase.zs:897; vmthunks.cpp:5439-5449) reads `extern float g_wheelStickForward, g_wheelStickSide` (vmthunks.cpp:5417), filled every VR tic at the same `VR_GetMove()` call site `g_game.cpp` already uses for the locomotion stick, **before** the existing `SuppressVRInput` zeroes `cmd.sidemove/forwardmove` — so a mod that calls `SuppressVRInput(true)` (to stop the stick walking the player) can still read raw deflection for something like a stick-select wheel, which the pre-existing single channel made impossible.

**`GetFieldIntArray(Object o, string field, int index, out int value)`** (doombase.zs:969; vmthunks.cpp:5798-5827) is the array-element counterpart to the pre-existing `GetFieldInt` (vmthunks.cpp:5762-5781, unchanged this window): `GetFieldInt` type-checks against `TypeSInt32/TypeUInt32` and correctly refuses a `PArray`-typed field outright. The new native resolves the field with the same `WR_ResolveField`, confirms `f->Type->isArray()` (vmthunks.cpp:6233 area) and casts to `PArray`, bounds-checks `index` against the array's **own** `ElementCount` (not caller-trusted), and checks `ElementType` is `TypeSInt32`/`TypeUInt32` before reading — false on any failure, never a reinterpreted read.

**`GetModelBoundsHint(class<Actor> cls, int sprite, int frame)` → `bool, double`** (doombase.zs:1047; vmthunks.cpp:6326-6360) resolves `(cls, sprite, frame)` through the same `FindModelFrame` path as `GetModelOrientationHint`/`GetModelWorldOffset`, calls `FModel::GetLocalExtent` on the resolved frame's first model, and scales the per-axis extent by that frame's own MODELDEF `xscale/yscale/zscale` before combining into one world-space bounding-sphere radius at actor Scale (1,1) — `sqrt(sx²+sy²+sz²)`, a conservative (never-undersized) proxy, not a tight fit. Built so a holster can solve `scale = targetRadius / measuredRadius` per weapon instead of one flat multiplier for every model (vmthunks.cpp:6259-6262). See Gap 6 for its actual format coverage.

**`SetShapeOrient` / `LinkShape` / `SetSweepRoom`** all extend the existing decal-shape system (`AddShape`/`MoveShape`/`SetShapeMotion`/`SetShapeRepeat`, unchanged signatures this window). `AddShape`'s `orient` parameter (doombase.zs:1198, unchanged signature) gains a fourth legal value: the native-side clamp changed from `clamp(orient, 0, 2)` to `clamp(orient, 0, 3)` (`vmthunks.cpp:4401`). Orient 3 ("standing") is a different primitive from 0-2: those are decals that only draw where existing surface geometry passes through them, while a standing shape defines its own vertical plane, is visible in open air, and is depth-tested against the eye's own view ray like a beam (doombase.zs:1183-1188, comment claim; consistent with the code adding pitch/roll/rate fields specifically gated to "standing" shapes). `ShapeAngle`'s meaning changes conditionally: in-plane rotation for orient 0-2, direction the plane faces (rotation around world-up) for orient 3 (g_levellocals.h:1320-1321).

- `SetShapeOrient(int slot, double pitch, double roll, double yawRate, double pitchRate, double rollRate)` (doombase.zs:1213; vmthunks.cpp:4514-4530) sets pitch/roll on top of `AddShape`'s base yaw, plus deg/sec rates for all three axes, resolved once per frame natively as base + rate×age (the same pattern `ShapeGrow`/`ShapeSeamRate` already use) — no per-tic script polling needed.
- `LinkShape(int slot, int parentSlot, double lx, double ly, double lz, double lyaw, double lpitch, double lroll)` (doombase.zs:1230; vmthunks.cpp:4544-4564) composes one shape's world transform with a parent's: local offset resolves along the *parent's own resolved basis* (its facing/right/up), local yaw/pitch/roll **Euler-add** to the parent's resolved orientation — exact for a pure-yaw chain, an approximation once pitch and roll combine at the same joint (this is stated as a known, deliberate approximation, not silently hidden — g_levellocals.h:1359-1365). `parentSlot=-1` clears the link. See Gap 1 for the ordering/cycle contract this native does **not** enforce.
- `SetSweepRoom(double minx, double miny, double minz, double maxx, double maxy, double maxz, double soft)` (doombase.zs:1249; vmthunks.cpp:4614-4630) bounds the sweep beam's air lattice — previously an literally-infinite plane with no room concept at all — to a caller-published box with a `soft`-unit fade; `soft<=0` (the default, and every map that never calls this) leaves it unbounded. Deliberately script-side per the author's stated rationale: "which sectors are one room" has no single engine-derivable answer.

| Native | Signature | Semantics | Readonly |
|---|---|---|---|
| GetRawStickMove | `native Vector2 GetRawStickMove();` | Raw (forward, side) locomotion-stick deflection, unaffected by SuppressVRInput | — |
| GetFieldIntArray | `native bool GetFieldIntArray(Object o, string field, int index, out int value);` | Reflects one element of a named int/uint array field, bounds-checked against the field's own declared size | — |
| GetModelBoundsHint | `native bool, double GetModelBoundsHint(class<Actor> cls, int sprite, int frame);` | Conservative world-space bounding-sphere radius for a resolved model frame at actor Scale (1,1); `found=false` if unresolved or format has no `GetLocalExtent` | — |
| SetShapeOrient | `native void SetShapeOrient(int slot, double pitch, double roll, double yawRate, double pitchRate, double rollRate);` | Pitch/roll + spin rates for an orient-3 ("standing") shape | — |
| LinkShape | `native void LinkShape(int slot, int parentSlot, double lx, double ly, double lz, double lyaw, double lpitch, double lroll);` | Parents a shape's transform to another shape's, composed along the parent's resolved basis | — |
| SetSweepRoom | `native void SetSweepRoom(double minx, double miny, double minz, double maxx, double maxy, double maxz, double soft);` | Bounding box + fade distance for the sweep beam's air lattice; `soft<=0` = unbounded (default) | — |

### 5. `PSprite` bone-anchor API

`wadsrc/static/zscript/actors/player/player.zs:3105-3164`, bound via `DEFINE_FIELD(DPSprite, ...)` at `src/playsim/p_pspr.cpp:161-181`. Five fields are write-side requests, four are renderer-published results. Resolution happens in `src/r_data/models.cpp` (outside assigned scope): a psprite layer sets `AnchorLayer`/`AnchorBone` to name another layer's bone; `models.cpp:128` matches `q->AnchorLayer == psp->GetID()` against every drawn layer's published bones for the frame and only then sets `AnchorBoneLive = true` (models.cpp:173) — an out-of-order request (anchoring to a numerically-higher, not-yet-drawn layer id, which player.zs:3115-3117 states is required) fails **safe**: `AnchorBoneLive` simply stays false (its default, models.cpp:91), it does not crash or read stale data.

| Field | Type | Semantics | Readonly |
|---|---|---|---|
| NoDraw | `native bool` | Suppresses drawing of this layer only; the weapon behind it keeps its states/damage/slot | N |
| AnchorLayer | `native int` | Psprite-layer id to follow; must be numerically **lower** than this layer's own id (draw order = id order); unenforced natively | N |
| AnchorBone | `native Name` | Bone name on the anchor layer's model | N |
| AnchorOfs | `native Vector3` | Offset from the anchor bone, in the bone's own local frame | N |
| AnchorAngles | `native Vector3` | (yaw, pitch, roll) orientation offset, in the bone's frame | N |
| AnchorBonePos | `native readonly Vector3` | Renderer-resolved bone offset from the weapon's own origin, model-local axes, map units; zero if not drawn this frame | Y |
| AnchorBoneLive | `native readonly bool` | True once the renderer resolved this layer's anchor this frame | Y |
| AnchorBoneWorld | `native readonly Vector3` | Same bone as a world position, directly comparable to `AttackPos`/`OffhandPos` | Y |
| AnchorBoneAngles | `native readonly Vector3` | Same bone's (yaw, pitch, roll) in degrees, playsim convention | Y |

### 6. New and renamed enums/constants

`EGripSubject` (`wadsrc/static/zscript/constants.zs:1629-1650`) is wholly new: `GRIPSUBJ_None=0` through eleven pose-shaped values (`Round, Shell, Inserting, Magazine, Grip, Forend, Foregrip, Slide, Support, Holster, Pouch`) to `GRIPSUBJ_MAX`. Pose-shaped by design, per the comment (constants.zs:1622-1624): two guns held the same way claim the same subject, independent of weapon identity. Named to mirror a same-named enum in `vk_openxrdevice.h` (device layer, out of assigned scope) — `GRIPSUBJ_Holster` is engine-written only, from `HolsterClaimMain/Off`, never from script's `GripClaimMain/Off` (constants.zs:1642-1646).

`BT_MAINHANDRELOAD` (bit 20) and `BT_OFFHANDRELOAD` (bit 28) are **renamed** to `BT_MAINHANDDROPMAG`/`BT_OFFHANDDROPMAG` (constants.zs:892-893) — same bit values, name-only change, breaking for any external script still referencing the old identifiers (no deprecated alias retained). The rename is corroborated outside this subsystem's scope by `src/d_event.h:81-82` (the canonical C++ mirror) and `src/g_game.cpp:1098-1099`, which use the new names consistently — confirming this is a completed rename, not a half-applied one.

| Symbol | Value | Note |
|---|---|---|
| EGripSubject | enum, 0..12 | New; see above |
| BT_MAINHANDDROPMAG | `1<<20` | Renamed from BT_MAINHANDRELOAD (breaking) |
| BT_OFFHANDDROPMAG | `1<<28` | Renamed from BT_OFFHANDRELOAD (breaking) |

### 7. Compiler/VM plumbing touched — no new script capability

`src/common/scripting/frontend/zcc_parser.cpp:103-114` gained a **comment only** (10 insertions, 0 deletions in this file). It documents that this fork used to check in a snapshot of the lemon-generated `zcc-parse.c/.h` under `src/scripting/zscript/`, which sat earlier on the MSVC include path than the real build-time-generated pair and silently shadowed it; after the 5.0.0-rc.2 merge added the `<=>` (`ZCC_LTEQGT`, referenced live at zcc_parser.cpp:157) and `NOROLLBACK` (`ZCC_NOROLLBACK`, zcc_parser.cpp:212) grammar terminals **upstream**, the stale snapshot lacked them and broke resolution. Per the comment the snapshot has been deleted. This is a build-correctness fix for grammar capability 5.0 already shipped, not new script-facing syntax added by this fork.

`src/common/scripting/interface/vmnatives.cpp:1339` — one line removed: a duplicate `DEFINE_GLOBAL(DoubleBindings)` (it was registered twice; `AutomapBindings` sat between the two copies). Dedup only, no capability change.

`src/common/scripting/backend/codegen.cpp` — zero diff in this window (confirmed by an empty `git diff` for the path).

### 8. Non-API behavior changes worth flagging

Not new native surface, but changed semantics of existing script methods that anything reading the hand/weapon-slot state should know about:

`PlayerPawn.BringUpWeapon()` (player.zs:1958, called from ~7 sites) now **writes** `weapon.bOffhandWeapon` (and its `SisterWeapon`'s copy) to match the slot it is placing the weapon into, and evicts the weapon from whichever opposite slot/psprite still held it (player.zs:2031-2062). The accompanying comment identifies the underlying invariant violation directly by name: the engine has two independent ways to ask "which hand is this weapon in" — `bOffhandWeapon` (read at ~41 call sites including `stateprovider.zs`'s attack routing) and slot membership (`player.ReadyWeapon`/`OffhandWeapon`) — and this fork added a second writer of the flag, `MoveWeaponToHand` (reachable from the VR wheel on the render thread, outside `P_Ticker`), so the two could disagree; when they did, `TickPSprites` destroyed both weapon layers (neither psprite's `Caller` matched its slot) while the attack traced from the wrong controller. This fix makes `BringUpWeapon` self-healing for the invariant rather than only reading it.

`PlayerPawn.SwitchWeaponHand(int hand=0)` (player.zs:2547) gained a null guard (`nextweap != null`, previously an unguarded `nextweap.bTwoHanded` read on a possibly-null pointer, player.zs:2564-2566) and now assigns `nextweap.bOffhandWeapon = (hand != 1)` before reassigning it (player.zs:2571-2572) — `BringUpWeapon` picks the psprite slot from `bOffhandWeapon`, and nothing previously set it on `nextweap`, so it landed wherever its stale flag pointed (often the hand just vacated), overwriting the weapon just moved. The comment records this as a live-observed bug (`ready=Pistol(off) off=VR_OffhandFist(main)`) and notes the upstream project (QuestZDoom) carries the identical latent bug, unfixed there.

### Gaps

---

## 7. VR device layer — this window’s changes only

Scope: `vk_openxrdevice.{cpp,h}`, `i_openVR.cpp`, `i_openXR.cpp`, `i_main.cpp`, `hw_vrmodes.{cpp,h}`, `hw_vrwheel.{cpp,h}`, `gl_openvr.cpp`, `gl_openxrdevice.cpp`, `vk_renderdevice.cpp`. All diffs are `1d2572bdcc..main` unless marked "uncommitted."

**Attribution.** The device/controller layer itself — OpenXR instance/session bootstrap, action-set creation, controller pose tracking, the OpenVR fallback, haptic submission mechanics — is Ermac/Emawind's pre-existing work, present in kind in both the pre-merge DXR branch and stock UZDoom 5.0.0-rc.2. None of the 12 files below built that layer. What this window did to them splits three ways: (1) mechanical reconciliation against UZDoom 5.0's refactored `IJoystickConfig`/backend-select interfaces, (2) first-party additions and fixes layered on the existing per-frame OpenXR polling loop, and (3) one file (`i_main.cpp`) whose only change has nothing to do with VR.

### Verdict table

| File | +/- | Verdict |
|---|---|---|
| `src/win32/i_openVR.cpp` | +168/-29 | Reconciliation, 100% |
| `src/common/platform/win32/i_openXR.cpp` | +126/-32 | Reconciliation, 100% |
| `src/rendering/gl/stereo3d/gl_openvr.cpp` | +2/-0 | Reconciliation, 100% |
| `src/common/rendering/vulkan/system/vk_renderdevice.cpp` | +1/-0 | Reconciliation, 100% |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` | +99/-7 | Mixed: 1 reconciliation hunk, rest first-party |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.h` | +1/-1 | First-party (signature extension) |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h` | +70/-0 | First-party (new enums/fields) |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp` | +206/-4 | First-party |
| `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp` | +25/-12 committed, +24/-0 uncommitted | First-party |
| `src/common/rendering/hwrenderer/data/hw_vrwheel.h` | 0/0 | Untouched |
| `src/gl/stereo3d/gl_openxrdevice.cpp` | 0/0 | Untouched; not compiled on desktop (see below) |
| `src/common/platform/win32/i_main.cpp` | +68/-0 | First-party, unrelated to VR |

### Reconciliation glue

**`i_openVR.cpp` / `i_openXR.cpp` — `IJoystickConfig` interface migration.** Both implement `IJoystickConfig` (`src/common/engine/m_joy.h:68`), which stock UZDoom 5.0 restructured before this window opened — `m_joy.h` itself has zero diff in `1d2572bdcc..main`. The per-device axis-to-game-function API (`EJoyAxis`, `GetAxisMap`/`SetAxisMap`/`IsAxisMapDefault`) is gone from the interface, replaced by digital-threshold/response-curve/haptics-capability methods (`GetAxisDigitalThreshold`, `SetAxisResponseCurve`, `HasHaptics`, …) and a bindings system addressed by axis codes (`NUM_AXIS_CODES`, `src/common/console/keydef.h:210`). Every other joystick backend in the tree (`i_dijoy.cpp`, `i_xinput.cpp`, `i_rawps2.cpp`, the SDL/Cocoa backends) already speaks this shape; these two didn't, because DXR's OpenVR/OpenXR device classes predate the 5.0 refactor. The diff rewrites both classes' `AxisInfo` struct and every accessor to the new virtual set: dead-zone default now compares against a stored `DefaultDeadZone` instead of a constant, digital thresholds default to `JOYTHRESH_STICK_X/Y` (matching what `i_dijoy` gives a physical stick), response curves default to `JOYCURVE_DEFAULT`, haptics is stubbed to `false`/`JOYHAPSTRENGTH_DEFAULT` (`i_openXR.cpp:105`: "mirror the DirectInput backend and report none"). `AddAxes()`'s parameter changes from `float[NUM_JOYAXIS]` to `float[NUM_AXIS_CODES]` in both files to match `IJoystickConfig::AddAxes`; the body of both was, and remains, a no-op (`i_openVR.cpp:169`, `i_openXR.cpp:257`) — VR locomotion was never routed through the generic axis-code path and still isn't.

Both files carry an added "fork note" recording the axis-to-function table (`OFF_HAND_PAD_X -> Side`, `ON_HAND_PAD_X -> Yaw`, etc.) that `GetAxisMap`/`SetAxisMap` used to expose, renamed to a private `VRAxisFunction`/`AxisFunctions[]` table now that it's off the public interface. The default mapping survives functionally — `GetYaw()`/`GetPitch()`/`GetDirectionalMove()` (`i_openVR.cpp:177-233`) still read `AxisFunctions[]` directly once per render frame, independent of `IJoystickConfig` — but the *user-facing remap* (reassigning which physical axis drives which VR function from Customize Controls) has no replacement, since nothing emits default axis-code bindings for either device. Both leave an explicit TODO (`i_openVR.cpp:82`, `i_openXR.cpp:78`).

**`gl_openvr.cpp`** — one line, `#include <cinttypes>`, with a comment that `PRIu64` used to arrive transitively via `m_random.h -> sfmt/SFMTObj.h -> SFMT.h -> <inttypes.h>` before UZDoom 5.0 deleted the SFMT RNG. Verified: `PRIu64` is genuinely used four times in the file (lines 2359, 2361, 3741-3742) and no `SFMT*` file exists anywhere in the tree. Pure missing-header fix, zero behavior change.

**`vk_renderdevice.cpp`** — one line, `#include "hw_vrmodes.h"`. The file calls `VRMode::GetVRModeCached(true)` at eight sites (389, 524, 556, 1026, 1072, 1132, 1237) and checks `vr_mode == VR_OPENXR_MOBILE` at line 229, all pre-existing from before `1d2572bdcc`. The 5.0 merge's header reshuffle dropped whatever transitive include used to supply the declaration; this restores it. Zero behavior change.

**`hw_vrmodes.cpp` — one reconciliation hunk.** `V_GetBackend()` has no prototype in `v_video.h` (confirmed by grep; it's defined at `v_video.cpp:134`, itself carrying a `[UZDXREMA] Upstream 5.0.0 dropped V_GetBackend()...` note), so `hw_vrmodes.cpp:76` adds a local forward declaration to call it — small merge scar tissue, not new capability. The two call sites (`GetOpenXRNetWaitMode()` at `:309`, `VRMode::GetVRMode()` at `:1223`) were also reworded from the magic number `V_GetBackend() != 1`/`== 1` to `!= BACKEND_VULKAN`/`== BACKEND_VULKAN`. Not applied tree-wide: `v_video.cpp:425` still compares against the bare literal `1`, so this was scoped to the file, not a project-wide pass.

### First-party: capacitive touch (genuine new device capability)

`vk_openxrdevice.cpp` adds two OpenXR boolean actions, `xrThumbTouchAction`/`xrTriggerTouchAction` (`vk_openxrdevice.h:263-264`), created alongside the existing button actions (`vk_openxrdevice.cpp:2046-2047`) and polled every frame in `UpdateControllerState()` (`:3191-3195`) into a two-bit-per-hand `xrFingerTouch[2]` array (`FINGERTOUCH_THUMB`/`FINGERTOUCH_INDEX`, `vk_openxrdevice.h:137-139`), published as `AActor::FingerTouchMain/Off` (`src/playsim/actor.h:1826-1827`; write site `vk_openxrdevice.cpp:3591-3592`). This is a genuinely new hardware read — capacitive skin contact, distinct from a button *press* — absent before this window.

Binding is scoped to exactly one interaction profile. Ten `XrPath`s are resolved (thumbrest, thumbstick, X/Y/A/B, both triggers' `/touch` sub-paths, `:2121-2130`) and bound only into `touchBindings`, suggested solely for the Oculus Touch profile (`:2219`). The simple, Vive, Index and WMR binding vectors never receive these two actions (verified: the action names appear nowhere else in the binding-construction code). The comment at `:2121` explains why — `xrSuggestInteractionProfileBindings` rejects an entire profile's bindings if one path is undefined for it — but the consequence is `xrFingerTouch` stays permanently `0` on every controller except Oculus/Meta Touch (`GetActionBoolean()`, `:1176`, safely returns `false` for an inactive action rather than erroring), including Valve Index, whose fuller per-finger sensing the code's own comment names as a future target (`vk_openxrdevice.h:135-136`: "more sensors (Index reads all four fingers) can be added later").

### First-party: grip-context / grip-subject arbitration rework

The largest behavioral change in the window, entirely inside `UpdateControllerState()`'s per-hand grip loop (`vk_openxrdevice.cpp:3427-3580`); it never touches the OpenXR API surface. It is the boundary where raw controller state becomes actor-level interaction state — the semantics of what a `GRIPSUBJ_*` value *means*, and who sets `AActor::GripClaimMain/Off`, live in ZScript and `src/playsim/actor.h:1808-1811` (outside this section's files); this file only arbitrates a script claim against what it can infer, then publishes the result.

Two enums are new (`vk_openxrdevice.h:83`, `:103-128`): `GRIPCTX_Object`, appended to the existing `EGripContext` (values are already published to ZScript, so it's appended rather than inserted — "priority lives in the branch order... not here"); and a wholly new `EGripSubject` (`None/Round/Shell/Inserting/Magazine/Grip/Forend/Foregrip/Slide/Support/Holster/Pouch`) describing *what* a hand is closed on, separate from `EGripContext`'s *what the grip currently means*. Per-hand resolution (`:3427-3465`):

- `claimed = GripClaimMain/Off` — whatever script last claimed.
- `supporting = claimed ∈ {Forend, Foregrip}` — still counts as holding the weapon.
- `objectHere = claimed > 0 && !supporting` — closed on something else (magazine, shell, slide…); this now outranks `GRIPCTX_Stabilize` in the priority chain (`:3508-3536`: not-held → Holster → tap/combo Modifier (main hand only) → `GRIPCTX_Object` → `GRIPCTX_Stabilize` (off hand only) → `GRIPCTX_Plain`). The inline comment at `:3396-3399` ("Priority: Holster > Stabilize > Modifier > Plain") predates `GRIPCTX_Object` and no longer states the actual order.
- `GripSubjectMain/Off` resolves to the claim if any, else `GRIPSUBJ_Holster` if `HolsterClaimMain/Off`, else `GRIPSUBJ_Support` if the off hand is gripped while the main hand holds a weapon and nothing was claimed (`:3574-3580`) — the comment at `:3560-3572` notes this branch used to be dead code, derived from `ctx == GRIPCTX_Stabilize`, which by construction only fires once Forend/Foregrip is already claimed, so the "bare two-handed pistol grip, nothing claimed" case could never resolve before.

The old proximity heuristic is retired, not removed: `stabilizeGeometryOk` is hardcoded `false` (`:3424`), and both `stabilizeRangeMeters` — computed every frame from `AActor::StabilizeReach`/`vr_stabilize_distance_inches` (`:3405-3407`) — and `vr_stabilize_requires_grab` are explicitly discarded with `(void)` (`:3425-3426`). Two-handedness now publishes as `AActor::TwoHandedHold` (`actor.h:1822`; write site `:3601-3605`), true only when the off-hand subject is `Support`, `Forend`, or `Foregrip`; `weaponStabilised` (the flag that repositions the weapon model) is `twoHanded && vr_two_handed_weapons` (`:3609`), and `vr_two_handed_weapons`'s own default flips `true` → `false` in this same window (`hw_vrmodes.cpp:955`; comment: the old behavior fired on every reload because reloading brings the hands close together).

### First-party: haptics diagnostics

`vr_haptic_debug` (`vk_openxrdevice.cpp:104`, default on, no `CVAR_ARCHIVE`) gates `Printf` tracing at three points: why `ProcessHaptics()` bailed (`:5747-5761`, one of four reasons — haptics disabled, no session, action never created, session not running), leading-edge confirmation a pulse was accepted by the runtime (`:5803-5810`), and every call to `Vibrate()` (`:5854-5859`). A companion CCMD, `vr_haptictest` (`hw_vrmodes.cpp:1706`), fires a 300ms/0.8-amplitude pulse on both hands directly through `VRMode::Vibrate()`, bypassing gameplay triggers, to separate "pipeline broken" from "nothing is calling it." Separately, `ProcessHaptics()` is now also called from the `menuMode` early-return branch of `UpdateControllerState()` (`:4076`); previously only the sibling `menuModeChanged` early return and the function's normal fall-through called it, so a pulse already in flight while a menu stayed open (as opposed to the frame it opened on) was never advanced or stopped.

### First-party: hand-model live placement and weapon-transform opt-out

Twelve new cvars — `vr_hand_ofs_{x,y,z}`, `vr_hand_{yaw,pitch,roll}` and the mirrored `vr_offhand_*` set (`hw_vrmodes.cpp:993-1004`) — apply a runtime offset/rotation on top of a hand model's own MODELDEF `Offset`, gated per-model by a `usehandoffsets` MODELDEF keyword (comment at `:981`; the MODELDEF parser side is outside this section). Stated rationale: MODELDEF is parsed once at load, so finding placement values by eye in a headset otherwise costs a repack-and-restart per trial. The off-hand needs its own six values because both hands share one mesh mirrored by a negative X scale.

`VRMode::GetWeaponTransform` gained a third parameter, `allowAutoReverse` (default `true`, `hw_vrmodes.h:196`, impl `hw_vrmodes.cpp:1483`). The existing dominant-hand mirror is normally suppressed only via `+Weapon.NoAutoReverse`, unreachable by a non-`Weapon` model-drawing caller; passing `false` lets such a caller opt out directly. Confirmed live: `src/r_data/models.cpp:633` calls it with `!(smf_flags & MDL_NOAUTOREVERSE)`; every other call site in the tree (`hw_vrwheel.cpp:298`, `p_hitscantracer.cpp:135`, `p_user.cpp:108/118`, `hw_weapon.cpp:670`, both GL/VK device files) uses the default.

Also in `hw_vrmodes.cpp`: `vr_openxr_present_gamma_bias`'s default changes `1.95f` → `0.886f` (`:660`), with a comment that `present.fp` (outside this section, unverified from these files) now linearizes with `pow(c, 2.2)` before the color ops under 5.0, so the old constant landed at an effective exponent of `~4.29` instead of `1.95` — roughly five times too dark in the midtones. `0.886 ≈ 1.95 / 2.2`, restoring the fork's prior look under the new pipeline. The CVAR-default change and the arithmetic are directly observable in the diff; the shader-behavior claim itself is the author's explanation.

### First-party: `hw_vrwheel.cpp`

Two committed changes, one uncommitted:

1. `IsWheelWeaponUsable()` (`:756`) now rejects any weapon with `bHolsterHidden` set (`:778-781`) — the wheel calls `MoveWeaponToHand` directly rather than going through the `CheckAmmo`-based filtering `weapnext`/`weapprev`/slot-select already use, so without this a holstered weapon selected from the wheel would land in-hand and stay stuck, since nothing outside the holster mod's own draw path clears that flag.
2. `AnnounceWheelClosed()` (`:1144`) no longer branches on `multiplayer` to choose between calling `MoveWeaponToHand()` directly (singleplayer) or queuing `DEM_ZSC_CMD "vr_moveweaphand"` (multiplayer, `:1263-1277`) — it now always queues. Rationale in the added comment: the wheel's close handler runs off the game tick, and `MoveWeaponToHand` mutates weapon-hand slots that `TickPSprites` reads; applied outside `P_Ticker`, `TickPSprites` can observe the slots half-updated and delete both psprite layers (it destroys any psprite whose `Caller` doesn't match its slot). Queuing through `DEM_ZSC_CMD`, consumed inside the tick, avoids the half-applied state. The comment cites `g_game.cpp`'s `switchhand` CCMD as precedent; confirmed present at `g_game.cpp:599-626`.
3. **Uncommitted** (`git diff HEAD`, not part of any commit yet): `OpenWheel()` (`:1154`) now looks up a cvar named `wr_suppress_native_wheel` via `FindCVar(...)` (`:1171-1178`) before opening the native wheel, and returns immediately if it exists and is true. The cvar is not declared anywhere in this engine tree — it is meant to be defined by a loaded mod's own CVARINFO. The comment states the direction is load-bearing, not stylistic: `CVar.SetInt`/`SetBool` from ZScript throws for any cvar lacking `CVAR_MOD` when called outside menu code — verified at `src/common/scripting/interface/vmnatives.cpp:942-949` (`ThrowAbortException(X_OTHER, "Attempt to change CVAR ... outside of menu code")`) — so an engine-owned cvar a mod could write to isn't viable; a mod-owned cvar the engine only reads has no such restriction. Absent (no such mod loaded) means enabled, so a plain session is unaffected. Looked up per-call rather than cached, since `OpenWheel` runs on a button press and a pk3 (and its cvar) can load after this translation unit's statics are initialized.

### Untouched

- **`hw_vrwheel.h`** — zero diff.
- **`gl/stereo3d/gl_openxrdevice.cpp`** (854 lines, `GLOpenXRDeviceMode`; copyright header dates to 2016-2020, predating even the Ermac/Emawind DXR lineage) — zero diff, and none of the grip-context/grip-subject/finger-touch/haptic-debug symbols described above appear anywhere in it. Cross-checked against `src/CMakeLists.txt`: `ENABLE_OPENXR` unconditionally adds `common/platform/win32/i_openXR.cpp` and `oxr_loader.cpp`, and adds `vk_openxrdevice.cpp` only `if (HAVE_VULKAN)` (`CMakeLists.txt:492-502`); `gl/stereo3d/gl_openxrdevice.cpp` is referenced by no `CMakeLists.txt` in the tree. It is not part of the compiled desktop binary under any configuration — only its header is pulled in (`hw_vrmodes.cpp:23`, the class it declares is not otherwise named in that file) — and it remains wired into the Android build only (`mobile/Android_src.mk`). OpenXR-on-GL is source-dead on Windows/desktop; the only reachable OpenXR device mode is `VKOpenXRDeviceMode`.

### Not VR: `i_main.cpp`

The file's only change this window (`wWinMain`, `:602-676`) is unrelated to VR devices: a `_DEBUG`+MSVC-only CRT report hook (`UZDXREMA_AssertBacktrace`, `:623`, installed via `_CrtSetReportHookW2` at `:674`) that captures a symbolized stack trace through `dbghelp.h` (`CaptureStackBackTrace`/`SymFromAddr`/`SymGetLineFromAddr64`) and appends it to a log file on any `_CRT_ASSERT` — including STL's `_STL_VERIFY` bounds checks, which report through the same path — then suppresses the assert dialog (`*returnValue = 0; return TRUE;`). The comment marks it explicitly temporary: "UZDXREMA TEMPORARY DIAGNOSTIC — remove once the Alt+F4 vector-subscript crash is found." The log path is a hardcoded absolute path, `E:\\UZDXREMA\\assert_trace.log` (`:614`); `CreateFileA` failure is silently swallowed, so on any checkout not at that exact path the hook installs but writes nothing, with no diagnostic of its own. No file in this section references VR from `i_main.cpp` — grep confirms zero `openxr`/`openvr`/`VRMode` hits in the file, changed or unchanged; its presence in this section's path list is incidental to it being the Win32 entry point, not device-layer work.



---

## 8. Input, menus and build

Scope: `wadsrc/static/menudef.txt`, `src/g_game.cpp`, `src/d_event.h`, `src/d_buttons.h`, `src/menu/profiledef.cpp`, `src/menu/doommenu.cpp`, `src/common/menu/menu.{cpp,h}`, `src/common/menu/resolutionmenu.cpp`, `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs`, `wadsrc/CMakeLists.txt`, `src/CMakeLists.txt`, `vcpkg.json`, `src/d_netinfo.cpp`, `src/common/engine/multiplayerlaunch.cpp`, `wadsrc/static/language.{0,1,csv}`, `wadsrc_extra/static/language.csv`, `wadsrc/static/zscript/ui/statusbar/alt_hud.zs`, `src/rendering/r_utility.cpp`, `src/scripting/zscript/zcc-parse.{c,h,out}`, `wadsrc/static/zscript/engine/base.zs`, `src/common/scripting/interface/vmnatives.cpp`, plus the four document-artifact `.md` files. `src/common/menu/menu.cpp` and `menu.h` carry **zero** diff in this window (confirmed with `git diff 1d2572bdcc main -- src/common/menu/menu.cpp src/common/menu/menu.h`, empty output; file mtimes predate the window).

This subsystem's work in the window splits cleanly into two kinds: (A) actual new/changed behavior for the fork's per-hand VR control scheme and the new VR object-physics settings panel, and (B) merge-reconciliation glue — fixes for call sites, symbols and duplicate declarations that upstream UZDoom 5.0.0-rc.2's refactor broke or collided with when the two trees were merged at `1d2572bdcc`. Every "glue" item below was independently confirmed by checking the baseline merge commit itself, not just trusted from a comment.

### A1. Per-hand reload rebound to drop-mag

`d_buttons.h:39,55` renames `Button_OH_Reload`/`Button_MH_Reload` to `Button_OH_DropMag`/`Button_MH_DropMag`. `d_event.h:81-82` renames the corresponding ticcmd bits `BT_OFFHANDRELOAD`/`BT_MAINHANDRELOAD` to `BT_OFFHANDDROPMAG` (`1<<28`) / `BT_MAINHANDDROPMAG` (`1<<20`) — same bit positions, name only. The added comment at `d_event.h:62-65` explains bit 20 was reclaimed from the dead `BT_SHOWSCORES = 0` (line 66); independently verified `BT_SHOWSCORES` is still OR'd into `cmd->buttons` at `g_game.cpp:1120` but being literal `0` this is a no-op, so the bit was genuinely free (this reuse predates the window — both sides of the `d_event.h` diff already show `= 0`; only the justifying comment and the two live bit names changed here).

`g_game.cpp` follows through: the ticcmd builder ORs the renamed bits at `g_game.cpp:1098-1099`, and both the empty-hand ready-check (`g_game.cpp:246-249`, `mainInputActive`/`offhandInputActive`) and the wheel-input suppression mask (`g_game.cpp:1130,1134`) were updated to test the new bit names — mechanical rename, no logic change.

`menudef.txt` moves the concept, not just the label. In `ActionControlsMenu`'s primary control list, the single generic `Control "$CNTRLMNU_RELOAD", "+reload"` was replaced by two per-hand entries, `Control "$CNTRLMNU_MAINHANDDROPMAG", "+mh_dropmag"` and `"$CNTRLMNU_OFFHANDDROPMAG", "+oh_dropmag"` (`menudef.txt:607-608`). Conversely, in that same menu's "Advanced Reloading" section, the old per-hand `Control "$CNTRLMNU_MAINHANDRELOAD"/"$CNTRLMNU_OFFHANDRELOAD"` pair was replaced by a single generic `Control "$CNTRLMNU_RELOAD", "+reload"` (`menudef.txt:649`). The two concepts effectively swapped shelf position: per-hand physical drop-mag is now the primary/default bind, and a single generic scripted reload became the secondary "advanced" option. `ActionDoubleControlsMenu`'s advanced-reloading double-tap pair was renamed in place, `mh_reload/oh_reload` → `mh_dropmag/oh_dropmag` (`menudef.txt:715-716`), without gaining an equivalent generic-reload double-tap counterpart.

Localization: `wadsrc/static/language.0:23-26` (current) carries the renamed `CNTRLMNU_MAINHANDDROPMAG`/`CNTRLMNU_OFFHANDDROPMAG` rows — English column and Identifier column updated, but every other language column on those two rows is untouched, still reading as a "reload" translation (e.g. German col: "Nachladen mit der Haupthand" = "reload with the main hand"). See gap below. Two now-orphaned decorative separator rows (`-,,DoomXR Options,,,...` and a bare `-,,,,,...`) were also dropped from `language.0` in the same pass, net `+2/-16` lines.

### A2. Raw locomotion stick exposed to script, independent of suppression

`g_game.cpp:559-560` adds two non-static file-scope globals, `float g_wheelStickForward`, `g_wheelStickSide`, deliberately not `static` so another translation unit can `extern` them. `G_BuildTiccmd` captures the VR locomotion stick's raw deflection into them every VR tic at `g_game.cpp:1297-1298`, positioned (per the added comment at 1291-1296) *before* the existing suppression logic that zeroes `forward`/`side` while a wheel/menu wants exclusive use of the stick. The one active consumer, confirmed by grep, is `src/scripting/vmthunks.cpp:5433-5448`: `extern float g_wheelStickForward, g_wheelStickSide;` backing a new native `FLevelLocals.GetRawStickMove()` (vmthunks.cpp is outside this subsystem's assigned paths, cited only to confirm the global isn't dead). Net effect: a mod (the weapon wheel) can read which way the stick is pushed for in-world menu selection even while `VR_IsScriptInputSuppressed()`/`VRWheel_ShouldSuppressHandInput()` (both defined in `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp` and `hw_vrmodes.cpp` — rendering subsystem, out of scope here) are blocking that same stick from walking the player.

`CCMD(switchhand)` (`g_game.cpp:591-628`) was rewritten to always queue the hand swap through the network command path — `Net_WriteInt8(DEM_ZSC_CMD)` / `Net_WriteString("vr_switchhand")` (`g_game.cpp:625-626`) — instead of branching on `multiplayer` and calling `SwitchWeaponHand` synchronously via `VMCall` in the singleplayer case. `DEM_ZSC_CMD "vr_switchhand"` is consumed by `src/d_net.cpp` and dispatched to `SwitchWeaponHand` in `wadsrc/static/zscript/actors/player/player.zs` (both out of scope, existence confirmed by grep only). The added comment attributes the old singleplayer bug to a race between the CCMD handler (run outside the game tick) and `TickPSprites` clearing a hand's psprite layer mid-swap — that specific mechanism claim is the author's, not independently traced here (`p_pspr.cpp` is out of scope), but the code change itself — collapsing two branches into one unconditional queued path — is verified directly.

### A3. New VR object-physics settings panel (`VRPhysicsMenu`)

`menudef.txt:2548` adds `Submenu "Object physics", "VRPhysicsMenu"` as the last entry of the existing `VR3DMenu` block. The new `OptionMenu "VRPhysicsMenu" protected` block spans `menudef.txt:2560-2606`, exposing 17 CVars, all defined in `src/playsim/p_physics.cpp` (out of scope; existence spot-checked only, e.g. `p_physics.cpp:77,89,102,117,173`) which this window's `src/CMakeLists.txt:952` newly adds to `PCH_SOURCES` — the menu and the CMake change are two sides of the same new file:

| Control | CVar | Range | Step | Decimals |
|---|---|---|---|---|
| Bounciness | `vr_physics_restitution` | 0.0–0.8 | 0.05 | 2 |
| Grip on surfaces | `vr_physics_friction` | 0.0–1.5 | 0.05 | 2 |
| Slowing (moving) | `vr_physics_lineardamp` | 0.0–2.0 | 0.05 | 2 |
| Slowing (spin) | `vr_physics_angulardamp` | 0.0–3.0 | 0.05 | 2 |
| Twist resistance | `vr_physics_contactspindamp` | 0.0–15.0 | 0.5 | 1 |
| Gravity (m/s²) | `vr_physics_gravity` | 0.0–25.0 | 0.5 | 1 |
| Simulation rate | `vr_physics_hz` | 30–240 | 10 | 0 |
| Hands are solid | `vr_physics_hands` | OnOff | — | — |
| Hand size | `vr_physics_handsize` | 0.25–4.0 | 0.05 | 2 |
| Weapon length/width/height | `vr_physics_weaponlen/width/height` | 0.02–0.6 / 0.005–0.2 / 0.02–0.4 | 0.01/0.005/0.01 | 2/3/2 |
| Weapon fwd/up offset | `vr_physics_weapon_ofs_fwd/up` | ±0.3 / ±0.2 | 0.01 | 2 |
| Wrist spin on throw | `vr_physics_throwspin` | 0.0–2.0 | 0.05 | 2 |
| Log summary | `vr_physics_debug` | OnOff | — | — |
| Trace (per sec) | `vr_physics_trace` | 0–20 | 1 | 0 |

The panel's own copy (`menudef.txt:2585-2586`) states plainly: *"A held T77 is now a solid placeholder box — real per-part shapes come with the real model."* This is shipped UI text, not a code comment. Cross-checked (lightly, outside this subsystem's scope) against `p_physics.cpp:1794-1795`, where `vr_physics_weaponlen/width` still size a literal box for the held item, and `p_physics.cpp:2135`, where a comment states a `PHYSDEF` lump keyed on actor class name replaces the box for world objects — confirming the held-weapon case is still hardcoded to one box shape, not a stale claim. See gap list.

`SafeCommand "$OPTMNU_DEFAULTS", "resetcvar ..."` at `menudef.txt:2603` lists 14 of the panel's 17 CVars; `vr_physics_hands`, `vr_physics_handsize`, and `vr_physics_debug` are absent from the reset list (verified by direct comparison of the two name sets). See gap list.

### B1. Merge-reconciliation: threading a new `cr` parameter through shared ZScript menu-item classes

Upstream 5.0.0-rc.2 added a "graycheck" trio (`CVar graycheck, int graycheckVal, name graycheckMode`) to `OptionMenuItemSubmenu.Init` and `OptionMenuItemStaticText.Init`; the fork already had its own extra parameter in those same slots (a per-item font-color override, `cr`). The merge produced signatures where the fork's `cr` and upstream's graycheck trio needed reconciling in one parameter list. Verified current signatures:

- `OptionMenuItemSubmenu.Init(String label, Name command, int param=0, bool centered=false, int cr=-1, CVar graycheck=null, int graycheckVal=0, name graycheckMode='Hide')` — `optionmenuitems.zs:156-165`, with `int cr=-1` explicitly placed ahead of the graycheck trio (comment at 151-155 states this ordering is load-bearing: it must stay in lockstep with `CreateOptionMenuItemSubmenu` in `src/common/menu/menu.cpp:1286` / declared `menu.h:384` — both files unchanged in this window, i.e. this native-side signature is inherited from the 5.0 merge, not new).
- `OptionMenuItemStaticText.Init(String label, int cr=-1, bool center=true, CVar graycheck=null, ...)` — `optionmenuitems.zs:780` area, same pattern.
- Subclasses `OptionMenuItemLabeledSubmenu.Init` (`optionmenuitems.zs:255-260`) and `OptionMenuItemCommand.Init` (`optionmenuitems.zs:292-297`) were updated to pass `-1` explicitly in the new `cr` slot when forwarding to `Super.init`, so their own call sites keep the same外部-visible arity.

`menudef.txt` call sites were updated to match the inserted slot: `Submenu "$DSPLYMNU_IMAGEADJUST", ImageAdjustMenu, 0, false, -1, vid_shadersupport` (`menudef.txt:1376`, was `..., 0, false, vid_shadersupport`), same pattern at `menudef.txt:1380,2482,2483`; and `StaticText " ", -1, true, vid_shadersupport` (`menudef.txt:2507`, was `..., -1, vid_shadersupport` — here the newly-inserted parameter is `center`, so `true` had to be added to keep `vid_shadersupport` landing in the graycheck slot rather than the center slot). This is purely a positional-argument fix forced by the merged signature; the one place it also enables new cosmetic behavior is `menudef.txt:403`, `Submenu "$OPTMNU_EXPERIMENTAL", "ExperimentalMenu", 0, 0, Gold` — using the now-correctly-threaded `cr` slot to color that row gold.

### B2. Merge-reconciliation: duplicate class/global declarations left by the merge

Two independent duplicate-declaration bugs, both confirmed by diffing the raw merge commit `1d2572bdcc` against itself (not inferred from comments):

- **`OptionMenuItemDoubleControl` defined twice.** At baseline, `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs` contained two classes both named `OptionMenuItemDoubleControl`: upstream's 3-argument form at line 733 (`Init(String label, Name command, Name doublecommand)`, with its own `MenuEvent` override binding a separate double-tap command into `DoubleBindings`), and the fork's pre-existing 2-argument form at line 779 (`Init(String label, Name command)`, forwarding to `OptionMenuItemControlBase.Init` with the ambient `DoubleBindings`). This is a duplicate class definition, verified directly against the baseline commit. The fix deletes upstream's 3-argument version, keeping only the fork's 2-argument one (now at `optionmenuitems.zs:753-759`). The added comment (`optionmenuitems.zs:737-741`) states upstream's own MENUDEF has zero call sites for the 3-arg form while the fork's `menudef.txt` has 151 two-argument `DoubleControl` entries — independently confirmed: `grep -c '^\s*DoubleControl\s' wadsrc/static/menudef.txt` returns exactly `151`.
- **`DoubleBindings` global declared twice.** `wadsrc/static/zscript/engine/base.zs` at baseline listed `native @KeyBindings DoubleBindings;` twice in the native-globals struct (one adjacent to `Bindings`/`AutomapBindings`, a second duplicate immediately after). Fixed by deleting the duplicate line; current struct reads `Bindings` / `DoubleBindings` / `AutomapBindings` once each (`base.zs:221-223`). The matching C++ registration table in `src/common/scripting/interface/vmnatives.cpp` had the identical duplicate — `DEFINE_GLOBAL(DoubleBindings)` twice — fixed the same way (`vmnatives.cpp:1337-1338` now list it once).

### B3. Merge-reconciliation: symbols upstream 5.0 removed or relocated

Three independent instances of the same pattern — pre-merge fork code referencing a symbol that upstream 5.0.0-rc.2's refactor deleted or moved, breaking compilation of the raw merge:

- **`MAXNETNODES` (host player-count ceiling).** Independently confirmed by `git grep MAXNETNODES 1d2572bdcc` across the *entire* baseline tree: the symbol is referenced at `doommenu.cpp` (old lines 222, 230) and `multiplayerlaunch.cpp` (old line 165) but defined **nowhere** in that same commit — the raw merge does not compile. Fixed in both files identically: a local `static constexpr int MP_MAX_HOST_PLAYERS = (int)MAXPLAYERS;` (`doommenu.cpp:224`, `multiplayerlaunch.cpp:21`), replacing the loop bound and range check (`doommenu.cpp:230,238`; `multiplayerlaunch.cpp:172`). `MAXPLAYERS` is `64` (`src/common/engine/i_net.h:33`, unchanged this window), and `HostGame()` in `i_net.cpp:1429-1430` (unchanged, out of scope) fatals past that count — so the menu's host-count option list now matches what the engine actually accepts. Net side effect: the host-count UI ceiling rose from the old (per the added comment, unverifiable since the symbol no longer exists to inspect) 8-node cap to 64.
- **`FArg`-typed command-line argument removal (`FArgs::RemoveArgs`).** `src/menu/profiledef.cpp`'s `cmdlineprofile` CVAR callback called `Args->RemoveArgs("-iwad")` etc. with raw string literals at baseline. Current `m_argv.h:119` declares `void RemoveArgs(const FArg check);` — `FArg` has no implicit single-string constructor (`m_argv.h:35-42` takes name/section/summary/usage/details), so the baseline calls do not compile against the merged header. `m_argv.h` itself is unchanged in this window, confirming the `FArg` documentation system is inherited from the 5.0 merge. Fixed by declaring `EXTERN_FARG(iwad)` etc. for the 11 arguments (`profiledef.cpp:15-25`, expanding via the pre-existing `EXTERN_FARG` macro at `m_argv.h:56-57` to `extern FArg FArg_iwad`) and calling `Args->RemoveArgs(FArg_iwad)` etc. (`profiledef.cpp:29-39`).
- **`V_GetBackend()` prototype.** `src/common/menu/resolutionmenu.cpp:45-51` adds a local forward declaration `int V_GetBackend();` with a comment stating upstream 5.0.0 dropped this prototype from `v_video.h` while the fork's definition (needed for VR-path range-clamping) still lives in `v_video.cpp` — spot-checked and confirmed: `v_video.h` declares the `BACKEND_OPENGL/BACKEND_VULKAN/BACKEND_OPENGLES` enum (`v_video.h:70-72`) but not the function; `v_video.cpp:134` defines it with a matching "Upstream 5.0.0 dropped V_GetBackend()" comment (both out of scope, cited only for corroboration). The one call site was also changed from a magic number to the named constant: `V_GetBackend() == BACKEND_VULKAN` (`resolutionmenu.cpp:83`), was `== 1`.
- **`doomcom_t` removal.** `src/d_netinfo.cpp:71-83`, `D_AssignDefaultMultiplayerName()`'s single-player test `doomcom.numnodes <= 1` (baseline) was rebuilt as `(MaxClients <= 1 && !netgame)` (`d_netinfo.cpp:83`), since upstream deleted the global `doomcom_t` block. `doomcom` no longer appears anywhere in `src/` outside comments describing its removal (grep-confirmed). The added comment walks through why both `MaxClients` and `netgame` are needed (a guest client's `Net_SetupUserInfo()` call in `i_net.cpp` runs before `MaxClients` is assigned, so `netgame` covers that window) — that sequencing detail is the author's claim, not independently re-traced through `i_net.cpp` here since it's out of this subsystem's assigned paths.

### Build: `vcpkg.json`, `CMakeLists.txt`, localization pipeline

`vcpkg.json` is a wholly new file in this window's diff (`git diff` shows it as `new file mode`), but it is **not new content**: `git show <pre-merge-DXR-parent>:vcpkg.json | diff - <(git show main:vcpkg.json)` produces zero output — it is byte-identical to the version carried by the DXR-branch parent of the `1d2572bdcc` merge, which the 5.0.0-rc.2 parent lacked. The merge commit dropped the file entirely; this window restores it verbatim. It declares vcpkg dependencies `bzip2` (static, all platforms), `libvpx` and `gtk3`/`glib` (non-Windows static), and two opt-in features `vcpkg-libvpx`/`vcpkg-openal-soft`. The consuming logic was never missing — `src/CMakeLists.txt:114-115` (`if("vcpkg-openal-soft" IN_LIST VCPKG_MANIFEST_FEATURES)`) and `:1619` (`x_vcpkg_install_local_dependencies`) are both unchanged this window — so the raw merge had build logic with no manifest to feed it; restoring the file is what fixes that, not new dependency work.

`src/CMakeLists.txt`'s only other change is adding `playsim/p_physics.cpp` to `PCH_SOURCES` at line 952 — registers the new VR-physics translation unit for the precompiled header, corroborating that `p_physics.cpp` is genuinely new in this window (backs §A3 above).

`wadsrc/CMakeLists.txt:3-15` re-enables `generate_language_files(doomxr_pk3 static/language.csv ENGINE)`, previously commented out at baseline with an explanation that it would overwrite/delete the fork's hand-maintained string keys. The fix that makes re-enabling safe: the ~525 fork-only keys were relocated out of `static/language.csv` (which the generator now owns — populated from `libraries/Translation/*.po` at build time, then POST_BUILD-deleted from the source tree) into two new never-generated files, `static/language.0` and `static/language.1`, which load as ordinary `LANGUAGE` lumps regardless. Directly verified: the 5 lines removed from `wadsrc/static/language.csv`'s tail (`DSPLYMNU_ROCKETEXPSTYLE`, `OPTVAL_ROCKETEXP_STYLE_TRANS/ADD`, `DSPLYMNU_POWERUPFADE`, `SNDMNU_FOOTSTEPVOLUME`) are exactly the 6 lines (header + 5 keys) of the new `wadsrc/static/language.1` — a real content migration, not just a doc claim. `wadsrc_extra/static/language.csv` (a separate, smaller dialogue-string CSV) was also deleted outright (`-48/+0`); `wadsrc_extra/CMakeLists.txt:3` (out of scope, unchanged this window) already calls `generate_language_files(game_support_pk3 static/language.csv GAMES)` at baseline, so that file's generator was never the thing that was broken — its tracked CSV copy was simply cleaned out of git alongside the other one, consistent with the same "generated, don't track it" pass. Both `static/language.csv` and `static/language.0` are already covered by the pre-existing `.gitignore:12-13` glob `wadsrc*/**/language.csv` / `wadsrc*/**/language.0` — they stayed tracked only because they were committed before that pattern applied; the `.csv` deletions untrack them for real, `language.0` is edited in place (still tracked) since it is not itself generated.

### `zcc-parse.c/.h/.out`: confirmed dead-file removal, not a functional change

The three files deleted under `src/scripting/zscript/` (`zcc-parse.c` −6,652, `zcc-parse.h` −161, `zcc-parse.out` −21,870) are pre-generated LALR parser tables/output from `lemon` running against `src/common/scripting/frontend/zcc-parse.lemon`. Checked against every alternative the task asked about:

- **Not a gitignore-driven untrack.** `.gitignore` has no entry for any `zcc-parse*` path (grep-confirmed).
- **Not the build's actual generated-file location.** `src/CMakeLists.txt:624-626` (unchanged this window) generates `zcc-parse.c`/`.h` via `add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/zcc-parse.c ${CMAKE_CURRENT_BINARY_DIR}/zcc-parse.h COMMAND lemon ...)` — output goes to the **build directory**, not `src/scripting/zscript/`. Confirmed by finding live build artifacts at `build-dxr/src/zcc-parse.{c,h,out}` in the working tree, a completely different path from the deleted, tracked source-tree copies.
- **Provenance of the deleted copies:** `git log --diff-filter=A -- src/scripting/zscript/zcc-parse.c` shows it was first added in commit `56effd76b0` ("Compiles on Android") — ancient history predating this window and the DXR-to-5.0 merge lineage entirely, almost certainly a pre-generated copy committed for an Android NDK build that couldn't run `lemon` at build time.
- Deleted in this window's very first commit, `ba5a979dbb` ("Make the merged tree build") — consistent with a build-fix pass, not later feature work.

Conclusion: these were stale, unreferenced, hand-committed parser output sitting at a dead path nothing in the current build reads from or writes to. Their removal is dead-weight cleanup with zero effect on the actual (lemon-into-build-dir) generation flow.

### Minor in-scope items

- `wadsrc/static/zscript/ui/statusbar/alt_hud.zs:578-582`: the alt-HUD's weapon-switch-strip builder now skips any weapon with `bHolsterHidden` set (flag defined in `wadsrc/static/zscript/actors/inventory/weapons.zs`, out of scope, existence confirmed) — a VR-holstered weapon no longer shows up in that HUD strip, mirroring an existing guard in `CheckAmmo`'s switch logic per the added comment.
- `src/rendering/r_utility.cpp:105`: adds `CVAR(Float, r_hudflatoverlay, 1.0f, CVAR_ARCHIVE)`. Declaration only; the CVar is consumed in `src/rendering/hwrenderer/scene/hw_weapon.cpp:2288-2308` (out of scope, rendering subsystem) to fade flat 2D psprite overlays on a weapon that's being drawn as a 3D model — confirmed live, not dead, by grep.
- `menudef.txt`: a blank `StaticText " "` separator was inserted between the `VRHUDOptions` and `VRWeaponOptions` blocks (around line 1763) — cosmetic spacing, no functional effect.

### Document artifacts and localization files (breadth only)

Per the evidence rules, the four `.md` files are prior-session AI-authored prose, not consulted as sources — line-count deltas only:

| File | Δ lines | Note |
|---|---|---|
| `CHANGES.md` | +971 / −0 (new file) | Document artifact; content not used as a source. |
| `FORK_CHANGES.md` | +1007 / −8 | Document artifact; content not used as a source. |
| `README.md` | +106 / −9 | Document artifact; content not used as a source. |
| `BILLBOARDS.md` | +20 / −7 | Document artifact; content not used as a source. |

Localization files (mechanism already covered in the Build section above; sizes only):

| File | Δ lines |
|---|---|
| `wadsrc/static/language.csv` | −46 (file deleted) |
| `wadsrc_extra/static/language.csv` | −48 (file deleted) |
| `wadsrc/static/language.0` | +2 / −16 |
| `wadsrc/static/language.1` | +6 (new file) |

### Attribution note

None of this subsystem's assigned files touch the VR device/controller layer (OpenXR/OpenVR session, input actions, controller poses, haptics) — that layer's files are outside this assignment's path list entirely. `g_game.cpp`'s changes operate one level up, in ticcmd construction, and are this-window work (new bits, new globals, rewritten CCMD) built on top of the pre-existing, unmodified-this-window device layer.


---

## 9. Gap inventory

Every placeholder, stub, hardcode, single-case special-case and scaling limit found across
the seven sections. **44 items: 2 blocker, 9 major, 33 minor.** These are the raw material
of the path forward.

| Sev | Finding | Location |
|---|---|---|
| **BLOCKER** | **Bone-anchor world position has no physics- or script-side consumer**<br>AnchorBonePos/AnchorBoneWorld/AnchorBoneAngles/AnchorBoneLive are populated every frame by src/r_data/models.cpp:170-247 specifically so a grab test can compare AnchorBoneWorld against AttackPos/OffhandPos (the coordinate frame p_physics.cpp:1590 already uses for hand placement) with no basis reconstruction. That comparison exists in this repository only as a comment/example in wadsrc/static/zscript/actors/player/player.zs:3151-3152 -- never as executed code -- and p_physics.cpp has zero references to DPSprite or any Anchor* field. No file under wadsrc/ ever sets .AnchorLayer/.AnchorBone on a psprite either, so the whole mechanism is currently inert in this repository: infrastructure for a bridge to the physics module that nothing here uses. | `src/playsim/p_pspr.h:334-359` |
| **BLOCKER** | **Magazine-drop buttons plumbed end-to-end, consumed nowhere in this repo**<br>BT_MAINHANDDROPMAG/BT_OFFHANDDROPMAG are defined (src/d_event.h:81-82), populated into cmd->buttons from controller input (src/g_game.cpp:1098-1099), and exposed to ZScript (wadsrc/static/zscript/constants.zs:892-893), but the only two ButtonChecks rows that would have acted on them are left commented out (p_pspr.cpp:127,134), and no other C++ or ZScript file in the repository reads either bit. Pressing the physical-reload button currently has zero effect anywhere in this repository; the feature bKeepWhenEmpty (weapons.zs:90) exists to support has no trigger. | `src/playsim/p_pspr.cpp:111-134` |
| major | **Anchor input fields silently reset on every save/load**<br>DPSprite::Serialize writes "nodraw" but omits AnchorLayer, AnchorBone, AnchorOfs, and AnchorAngles (p_pspr.h:305-306,322-323). Any mod-configured anchor is silently discarded on save/load: FSerializer leaves an absent key at the field's in-class default (-1 / NAME_None / zero), so a reloaded save always shows an anchored layer falling back to unanchored placement, with no error or log line. | `src/playsim/p_pspr.cpp:1490-1518` |
| major | **GetModelBoundsHint returns found=0 for IQM, voxel (KVX) and UE1 models**<br>GetLocalExtent is overridden only by FOBJModel (src/common/models/models_obj.cpp:697), FMD3Model (src/common/models/models_md3.cpp:262) and FDMDModel/MD2 (src/common/models/models_md2.cpp:286); model_iqm.h, model_kvx.h and model_ue1.h have no override and fall back to the base class default returning false (src/common/models/model.h:196). GetModelBoundsHint was added specifically so a holster could solve per-weapon scale instead of using one flat multiplier (vmthunks.cpp:6259-6262); for any weapon modeled in an unsupported format that use case still falls back to found=0. Separately, the function's own doc comment (vmthunks.cpp:6323-6325) claims found=0 is expected 'for every format except FOBJModel' -- true when this native was added (commit fa4425ce2a) but made stale by a later same-window commit, b2075995a4 ('Add GetLocalExtent support for MD2/MD3, not just OBJ'), which added the MD2/MD3 overrides without updating this comment. Which format this fork's actual weapon models use is undetermined from this repo (content assets live outside it per repo layout). | `src/scripting/vmthunks.cpp:6342` |
| major | **Held VR weapon collision is a hardcoded box for one weapon, not model-derived**<br>Shipped UI copy states the held weapon ("a held T77") uses a placeholder box sized by vr_physics_weaponlen/width/height/offset CVars; PHYSDEF-based per-actor shape substitution (p_physics.cpp:2135, out of scope) exists for world objects but is not wired for the item gripped in a VR hand. Blocks realistic per-weapon hand-collision beyond the one hardcoded silhouette. | `wadsrc/static/menudef.txt:2585-2592` |
| major | **Held/attached bodies bypass world-geometry collision entirely**<br>The carry loop in UpdateHands overwrites a held body's pos/rot unconditionally every physics frame (h.pos = hand->pos + hand->rot.Rotate(h.grabPosOffset), no penetration or collision test), and the body is kinematic while held so StepBody's `if (b.kinematic) return;` (line 814) skips it entirely. UpdateWeapons writes a held weapon's pose the same way (line 1808-1809). A held object or weapon can be moved through walls/floor by the player's real hand motion with zero pushback; only body-vs-body contact (SolvePair) still applies to it. | `src/playsim/p_physics.cpp:1696-1719` |
| major | **IQMModel never overrides GetLocalExtent**<br>FModel::GetLocalExtent defaults to false (model.h:196). FDMDModel (models_md2.cpp:286-292), FMD3Model (models_md3.cpp:262-268) and FOBJModel (models_obj.cpp:697-711) all gained an override this window; IQMModel did not. IQM is also the only format that overrides any joint/bone virtual (FindJoint, GetJointPosition, NumJoints, GetBasePose, etc. -- confirmed by grep across model_md2.h/model_md3.h/model_obj.h/models_md2.cpp/models_md3.cpp/models_obj.cpp, all zero matches), so it is definitionally the only format that can carry a skeleton in this engine. GetModelBoundsHint (src/scripting/vmthunks.cpp:6326-6349, outside this subsystem) calls GetLocalExtent to compute a per-weapon world-space bounding radius for fit-scale calculations and returns found=0 whenever the override is missing (vmthunks.cpp:6342) -- so every rigged model returns found=0 through this native today, i.e. exactly the models a holster/physics system most needs a real size for. | `src/common/models/model_iqm.h:128-243` |
| major | **No constraint/joint/motor solver exists**<br>A case-insensitive search of the whole file (and p_physics.h) for constraint\|joint\|hinge\|prismatic\|ragdoll\|motor returns zero matches. Only free 6-DOF contact bodies and pairwise contact resolution exist (StepBody, SolvePair). Any future articulated mechanism -- a hinged bolt/slide, a ragdoll, a magazine floorplate that swings on an axis -- has no solver primitive to build on; it would need to be added from scratch. | `src/playsim/p_physics.cpp` |
| major | **Shader/C++ uniform array bounds have no single source of truth**<br>Array-length constants for the shape/fog-disturb/beam/sweep-band systems are hand-duplicated as raw literals across at least five files (src/g_levellocals.h, hw_viewpointuniforms.h, gl_shader.cpp, vk_shader.cpp, main.fp) with no compiler-enforced link between them -- shaderBindings in vk_shader.cpp/gl_shader.cpp is a plain R"(...)" string concatenated verbatim (vk_shader.cpp:609,636), so nothing substitutes a shared constant into the GLSL text. This is not theoretical: at the window's own start commit (1d2572bdcc), vk_postprocess.cpp referenced PresentUniforms::Brightness, a member absent from the struct in hw_postprocess.h at that same commit -- the literal baseline tree does not compile. This window's hw_postprocess.h change (adding the field, a padding float, and two static_asserts) fixes that specific instance, but the static_asserts only check std140 alignment, not that an array length in the C++ struct agrees with either GLSL string literal, so the same class of silent (or, per this precedent, sometimes not-so-silent) desync remains possible for any future field/array change. | `src/common/rendering/vulkan/shaders/vk_shader.cpp:182` |
| major | **Weapon and hand colliders are single CVAR-sized boxes, not mesh-derived**<br>UpdateHands builds the hand shape from vr_physics_handsize alone (0.045/0.030/0.090m base box); UpdateWeapons builds the weapon shape from vr_physics_weaponlen/width/height and offsets from vr_physics_weapon_ofs_fwd/up, with no reference to any model or bone. The code's own comment (1737-1740) calls this a placeholder pending per-bone weapon colliders (grip/slide/magwell) built from the mesh's bind pose -- that later work does not exist in this window's diff. | `src/playsim/p_physics.cpp:1585-1588,1793-1809` |
| major | **bPhysicsBody is script-writable independent of the physics solver's own body registry**<br>PHYSICSBODY is registered with plain DEFINE_FLAG (VARF_Native only, no VARF_ReadOnly/InternalAccess), so any script can set or clear actor.bPhysicsBody directly, bypassing PhysicsEnable/PhysicsDisable (src/playsim/p_physics.cpp:2164,2195) which are the only code paths that also register/unregister the body in the solver's own g_bodies list. Setting the flag directly makes P_MobjThinker (src/playsim/p_mobj.cpp:4607) skip normal movement for an actor the solver never allocated a body for (it stops moving, permanently, with no simulation driving it); clearing it directly leaves an orphaned body in g_bodies still being simulated for an actor that has resumed normal Doom movement. No native-side guard prevents either. | `src/scripting/thingdef_data.cpp:344` |
| minor | **A_SetUserVarName/GetVar does not check the target field is actually Name-typed**<br>GetVar() only requires the resolved field be non-native/non-private/non-static and isScalar(); it does not check PType is specifically PName. A_SetUserVarName will silently write an FName's raw table index as a plain int into any scalar user field (e.g. a user_int), reinterpreting bytes with no error. This mirrors A_SetUserVar's own pre-existing lack of type-checking for its int/float siblings (p_actionfunctions.cpp:3072-3096), so the new function inherits an old gap rather than introducing a novel one. | `src/playsim/p_actionfunctions.cpp:3061` |
| minor | **BT_MAINHANDRELOAD/BT_OFFHANDRELOAD renamed with no compatibility alias**<br>BT_MAINHANDRELOAD -> BT_MAINHANDDROPMAG and BT_OFFHANDRELOAD -> BT_OFFHANDDROPMAG keep the same bit values but the old identifiers are gone outright (no deprecated const alias). Any external script (this fork's own content pk3s or third-party ones) still referencing the old names fails to compile with no migration path other than a text search-and-replace. | `wadsrc/static/zscript/constants.zs:892` |
| minor | **Bind-pose fix duplicated across two branches with no shared helper**<br>CalculateBonesOnlyOffsets's out-populating branch (905-960) and boneData-only branch (966-1004) build the per-bone matrix through an identical sequence (seed TRS from Joints[i], Modify() override, translate/multQuaternion/scale, combine with baseframe/inversebaseframe) with no shared subroutine. The bind-pose fix (927-928, 975-976) had to be hand-applied to both, as the in-code comment at 968-971 acknowledges. Nothing enforces the two stay in sync; a future change to one silently misses the other unless a maintainer remembers to mirror it by hand. | `src/common/models/models_iqm.cpp:896-1005` |
| minor | **Capacitive touch wired for Oculus Touch profile only**<br>xrThumbTouchAction/xrTriggerTouchAction bindings are added only to touchBindings, suggested solely for the Oculus Touch interaction profile (:2219). The Simple, Vive, Valve Index, and WMR binding vectors never receive them, so AActor::FingerTouchMain/Off is permanently 0 on those controllers regardless of hardware capability — including Valve Index, which the code's own comment (vk_openxrdevice.h:135-136) names as a future target for exactly this reason. | `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp:2209` |
| minor | **Decorative-shape systems are fixed-capacity, not dynamically sized**<br>MAX_SHAPES=128 (unchanged this window) and MAX_FOG_DISTURB=32 (raised from 8 this window, src/g_levellocals.h:1562) and the sweep-lattice's 8-band cap (wadsrc/static/shaders/glsl/main.fp:1900, unchanged) are compile-time array sizes baked into the std140 uniform block. A level or mod that needs more concurrent standing/decal shapes, fog disturbances, or laser-sweep bands than the current cap requires an engine rebuild touching the multi-file set described above, not a content-side or CVAR-level change. | `src/g_levellocals.h:1316` |
| minor | **Diagnostic CVars default on and are not archived**<br>vr_pose_debug, vr_validate and vr_spatialreport are all declared CVAR(Bool, ..., true, 0); flag 0 means not CVAR_ARCHIVE, so turning one off does not persist across a restart -- it reverts to default-on next launch. Output volume is bounded (ValidateOnce fires each check at most once per (smf,slot), models.cpp:275-281; the spatial report is throttled to 1/sec per psprite layer, models.cpp:940-946), so this is not unbounded spam, but there is no way to make 'off' stick short of editing the CVAR default in source. | `src/r_data/models.cpp:261,270-271` |
| minor | **FingerTouch bit constants have no ZScript symbolic names**<br>FingerTouchMain/Off is documented only in a comment ('bit 0 = thumb..., bit 1 = index...'); the actual FINGERTOUCH_THUMB/FINGERTOUCH_INDEX values exist only in src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h:138-139 (C++-only). A script author must hardcode 1/2 instead of a named constant -- inconsistent with the fresh EGripSubject enum added in the same window for the adjacent GripClaim/Subject pair. | `wadsrc/static/zscript/actors/actor.zs:417` |
| minor | **Free-body world collision is limited to the body's own sector, with no blockmap or neighbour-sector query**<br>Floor/ceiling comes from sec->floorplane/ceilingplane and walls from sec->Lines, where sec = a->Sector for the body's own sector only. The code's own comment justifies this for "a box a few centimetres across"; there is no blockmap query or adjacent-sector search, so a larger or fast object whose bounding radius exceeds the distance to a line in an adjacent sector will not detect it. | `src/playsim/p_physics.cpp:927-999` |
| minor | **GL-backend OpenXR device mode is source-dead on desktop and has fully diverged**<br>gl_openxrdevice.cpp is not referenced by any CMakeLists.txt in the tree (confirmed: src/CMakeLists.txt:492-502 adds i_openXR.cpp/oxr_loader.cpp unconditionally under ENABLE_OPENXR and vk_openxrdevice.cpp only if HAVE_VULKAN; gl_openxrdevice.cpp appears in neither list, nor anywhere else in CMakeLists.txt). It compiles only for the Android build (mobile/Android_src.mk). It received none of this window's grip-context/grip-subject/finger-touch/haptic-debug work -- none of those symbols exist in the file -- so if the GL+OpenXR path is ever revived on a non-Vulkan desktop target, all of that interaction-model work would need porting from scratch. | `src/gl/stereo3d/gl_openxrdevice.cpp` |
| minor | **GripContext still has no ZScript enum, now visibly inconsistent with GripSubject**<br>GripContextMain/Off (pre-existing, unchanged this window) is documented only via a comment pointing at a C++-only EGripContext in vk_openxrdevice.h; no enum was ever added to constants.zs for it. The new EGripSubject enum sitting immediately below it in the same file makes the omission conspicuous to anyone reading the two field pairs together. | `wadsrc/static/zscript/actors/actor.zs:419` |
| minor | **HudAnchor_Store rescans every psprite layer per rendered layer**<br>For every psprite layer whose model resolves bone data this frame, HudAnchor_Store walks the full linked list of the player's psprites (psp->Owner->psprites) looking for layers anchored to it -- O(layers^2) per frame across the whole psprite chain, versus an O(1) lookup a direct anchor-request index would need. Harmless at today's handful of HUD layers (weapon, flash, two hands); would need revisiting if a rig grows many independently anchored attachment points. | `src/r_data/models.cpp:126-128` |
| minor | **Inertia tensor is always a solid-box approximation, never reconciled with PHYSDEF hull geometry**<br>PhysicsEnable computes invInertia from the script-supplied hx/hy/hz half-extents using a solid-box formula (ix = m(h*h+d*d)/12 etc.), unconditionally -- even when ApplyPhysDefShape (called just before, line 2143) has replaced b.hulls with real convex-hull geometry from a PHYSDEF lump. An irregularly-shaped hull's actual mass distribution is never used. | `src/playsim/p_physics.cpp:2142-2162` |
| minor | **LinkShape has no parent-ordering or cycle validation**<br>LinkShape bounds-checks parentSlot to [-1, MAX_SHAPES) but never verifies parentSlot < slot (the documented caller contract, g_levellocals.h:1352-1356) nor detects a cycle. A caller violating either gets an undefined per-frame resolution order with no error, warning, or fallback -- silent wrong output, not a crash. | `src/scripting/vmthunks.cpp:4548` |
| minor | **MAX_FOG_DISTURB is a hardcoded ring-buffer capacity with silent oldest-slot eviction**<br>Raised from 8 to 32 this window (4x), deliberately not matched to MAX_SHAPES/MAX_BEAMS' 128 per the author's own stated per-fragment-cost reasoning. The underlying reuse policy (a wrapping FogDisturbNext index, unchanged this window) is not touched by this diff, but a 33rd simultaneous FogDisturb call still silently overwrites the oldest active disturbance with no script-visible signal that an overflow occurred. | `src/g_levellocals.h:1562` |
| minor | **New VRPhysicsMenu panel has no localization keys**<br>Every other OptionMenu in this file routes labels through $KEY indirection into language.0/.1/.csv. The VRPhysicsMenu block's Title and all ~31 Slider/Option/StaticText labels are raw English string literals; only 2 of 51 lines in the block contain a $, and both belong to a shared pre-existing key or the next menu block. Cannot be localized without editing menudef.txt directly. | `wadsrc/static/menudef.txt:2560-2606` |
| minor | **No PHYSDEF lump data ships in this repository**<br>LoadPhysDefs/ApplyPhysDefShape are fully implemented and load any Body/Hull-format PHYSDEF lump found via fileSystem.FindLump, but this window's diff adds zero such lump content anywhere in this tree. Within this repository alone, ApplyPhysDefShape always returns false and every PhysicsEnable'd body falls back to a box. | `src/playsim/p_physics.cpp:400-508` |
| minor | **PhysBody registry has no GC visibility; lifetime rests on one call site**<br>g_bodies holds raw AActor* pointers with no GC::Mark/root registration anywhere in the file (confirmed by grep). The only safety net is the unconditional P_PhysicsRemoveBody(this) in AActor::OnDestroy (src/playsim/p_mobj.cpp:5992). FindBody does a bare pointer-equality scan with no secondary validity check, so correctness depends entirely on OnDestroy always running before an actor is freed by any code path in the wider engine. | `src/playsim/p_physics.cpp:679,754-759,2084-2097` |
| minor | **Physical-weapon detection is a hardcoded single-family substring match**<br>IsPhysicalWeapon lowercases the weapon class's TypeName and checks IndexOf("t77") >= 0. Only one weapon family can ever be physical; adding a second physical weapon type requires either naming it with "t77" in it or editing this function -- there is no data-driven list, flag, or per-class opt-in. | `src/playsim/p_physics.cpp:1743-1749` |
| minor | **Physics native API has no caller anywhere in this repository**<br>All 12 DEFINE_ACTION_FUNCTION_NATIVE entry points (PhysicsEnable, PhysicsGrab, PhysicsRelease, etc.) are declared native in wadsrc/static/zscript/actors/actor.zs:991-1020 but have zero callers anywhere in this repository's own wadsrc. The feature is unreachable end-to-end from this tree alone; it depends entirely on mod script living outside the engine repository. | `src/playsim/p_physics.cpp:2177-2555` |
| minor | **Pose-debug tracer hardcodes exactly two hands**<br>vr_pose_debug's trace in CalcModelOverrides dedups against exactly two function-local statics (lastMain/lastOff, models.cpp:1300) split on psp->GetID() >= PSP_OFFHANDWEAPON. Diagnostic-only -- does not affect what gets rendered -- but a third simultaneously-posed decoupled psprite layer would alias one of the two slots and could suppress a trace line that should have printed. | `src/r_data/models.cpp:1299-1302` |
| minor | **Proximity-stabilize config surface is fully dead code**<br>stabilizeGeometryOk is hardcoded `const bool ... = false`, and both stabilizeRangeMeters (derived every frame from AActor::StabilizeReach / vr_stabilize_distance_inches, computed at :3405-3407) and the CVAR vr_stabilize_requires_grab (hw_vrmodes.cpp:970, CVAR_ARCHIVE, no menu reference anywhere in the tree) are explicitly cast to (void) rather than deleted. Three configuration surfaces — one global cvar, one per-weapon ZScript property, one grab-required toggle — are computed and then discarded every frame; none currently affect behavior. | `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp:3424` |
| minor | **Renamed drop-mag control strings keep stale reload-wording translations**<br>CNTRLMNU_MAINHANDDROPMAG/OFFHANDDROPMAG rows have updated English and Identifier columns but every other language column still contains the pre-rename reload translation (e.g. German "Nachladen mit der Haupthand"). Non-English players see a mistranslated label for a control that no longer reloads anything. | `wadsrc/static/language.0:23-26` |
| minor | **Same-class-both-hands ambiguity survives in the bob-position lookup**<br>DrawPlayerSprites (hw_weapon.cpp:1500-1520) stopped using WeaponSpriteMatches' GetClass()-equality fallback (hw_weapon.cpp:1566) to attribute a psprite to a hand, specifically because it cannot tell two same-class weapons apart. GetWeaponPosition2D/3D (hw_weapon.cpp:1609,1642), called from the same prepare passes to source bob-offset interpolation, still use exactly that WeaponSpriteMatches call and were not touched by this window. A shared-id layer (e.g. PSP_FLASH) can still have its bob sourced from the wrong hand's weapon when both hands hold the identical weapon class. | `src/rendering/hwrenderer/scene/hw_weapon.cpp:1609` |
| minor | **Temporary crash-diagnostic hook left in i_main.cpp with a hardcoded absolute path**<br>UZDXREMA_AssertBacktrace (installed at :674) is explicitly commented as temporary scaffolding for chasing one specific bug ("remove once the Alt+F4 vector-subscript crash is found") and is unrelated to VR. It writes to a hardcoded absolute path, E:\\UZDXREMA\\assert_trace.log; CreateFileA failure is silently swallowed, so on any checkout not at that exact path the hook installs but produces no diagnostic and gives no indication it failed. | `src/common/platform/win32/i_main.cpp:614` |
| minor | **Upstream's richer double-tap-rebind menu item was deleted rather than adopted**<br>Baseline merge had two colliding OptionMenuItemDoubleControl class definitions (upstream's 3-arg primary+double-tap-command form vs. the fork's 2-arg form). Fix kept only the fork's 2-arg form; adopting upstream's richer per-row double-tap-to-different-command capability would require adding a second command argument to all 151 DoubleControl entries in menudef.txt (count independently confirmed). | `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs:737-759` |
| minor | **VR axis-to-function remap has no replacement post-5.0**<br>UZDoom 5.0 deleted IJoystickConfig::GetAxisMap/SetAxisMap/IsAxisMapDefault in favor of a bindings system keyed by axis codes (NUM_AXIS_CODES), and neither i_openVR.cpp nor i_openXR.cpp (TODO at :78) has been re-wired to emit default axis-code bindings for their VR axes. Both files leave an explicit TODO acknowledging this. Default locomotion still works because GetYaw/GetPitch/GetDirectionalMove read a private AxisFunctions[] table directly once per render frame, bypassing IJoystickConfig entirely -- so this is a loss of the in-menu remap feature only, not broken movement. | `src/win32/i_openVR.cpp:82` |
| minor | **VRPhysicsMenu Reset-to-defaults omits 3 of its 17 CVars**<br>The panel's SafeCommand resetcvar argument list names 14 CVars; vr_physics_hands, vr_physics_handsize, and vr_physics_debug are not included, so Reset to defaults silently leaves hand-solidity, hand size, and debug logging untouched. | `wadsrc/static/menudef.txt:2603` |
| minor | **Volumetric-beam dust contrast curve is a hardcoded, non-tunable pair of constants**<br>The new dust-contrast fix, d = smoothstep(0.22, 0.78, d), has no cvar or uniform backing either literal. DustAmount (pre-existing) only controls how strongly the resulting field darkens the beam (mix(1.0, d, DustAmount)), a different axis from the curve's shape -- retuning how aggressively the noise is pushed toward its extremes requires a shader source edit, not a menu slider. | `wadsrc/static/shaders/pp/volumetricbeam.fp:206` |
| minor | **World and pair contact arrays silently truncate when full**<br>Contact collection is capped at kMaxContacts=96 (StepBody) and kMaxPair=48 (SolvePair); every collection loop is guarded by `&& numContacts < kMaxContacts` / `&& n < kMaxPair` with no Printf or counter when the cap is hit, unlike the PHYSDEF hull-drop path which does warn. A very high-vertex compound hull touching many walls or another complex hull simultaneously would lose contacts with no diagnostic. | `src/playsim/p_physics.cpp:858-860,1327-1328` |
| minor | **a->Sector used for collision is stale across multi-step frames**<br>StepBody reads a->Sector fresh every call (line 816), but a->Sector is only updated by WriteBack's call to LinkToWorld (line 1837-1840; see src/playsim/p_maputl.cpp:424-436 for where Sector is actually assigned), which runs once per P_PhysicsFrame call after the entire fixed-step while-loop finishes (line 2036-2040). When vr_physics_maxsteps causes more than one fixed step (or many CCD substeps) to run in one frame, every one of those steps collides against the sector snapshot from the end of the previous frame, not the sector the body may have crossed into mid-frame. | `src/playsim/p_physics.cpp:816,2036-2040,1837-1840` |
| minor | **placementCVars resolved by name every drawn frame, uncached**<br>Both the HUD path (models.cpp:702-726) and the world-model path (models.cpp:536-560) run 7 FString::Format + FindCVar-by-name lookups (ofs_x/y/z, yaw/pitch/roll, scale) per drawn model per frame whenever placementCVars != NAME_None, justified in-code by MODELDEF parsing before CVARINFO is guaranteed loaded (models.cpp:695-698, 530-532). The lookup only needs to fail open once at startup; nothing here caches a successful resolution, so cost scales linearly with the number of simultaneously drawn placementCVars-bearing models (HUD and world) every single frame. | `src/r_data/models.cpp:702-726` |
| minor | **resolutionmenu.cpp forward-declares V_GetBackend() locally instead of via a shared header**<br>Upstream 5.0.0 dropped V_GetBackend()'s prototype from v_video.h; the fork's definition still lives in v_video.cpp with no header declaring it, so this translation unit (and any future one that needs it) must repeat its own local forward declaration until the prototype is restored to a shared header. | `src/common/menu/resolutionmenu.cpp:45-51` |
| minor | **vr_physics_hands does not gate held-weapon contact, only hand-body contact**<br>SolvePair's gate `if ((A.handIndex >= 0 \|\| B.handIndex >= 0) && !*vr_physics_hands) return;` checks handIndex only, not weaponHand. Disabling "hands solid" leaves a held weapon body still pushing and contacting other physics bodies -- the CVar's scope is narrower than its own description ("Whether your hands are solid to physics objects", line 116) suggests. | `src/playsim/p_physics.cpp:1314` |

---

## 10. Capability assessment

Measured against two target tiers. Each gap is labelled **structurally blocked** (the
architecture prevents it), **merely unbuilt** (nothing prevents it, it just does not exist),
or **already there**.

### Rung 0 — non-penetrating hands and held objects (the Alyx/Pavlov/Boneworks shared floor)

**Verdict, stated plainly: this engine does not have Rung 0 today, for either hands or held objects, and the gap is the same one structural cause in both cases — a hardcoded `kinematic` flag that routes both around the collision code that already exists and already works for free bodies.** Every claim below was re-verified directly against `src/playsim/p_physics.cpp` at HEAD, not taken from the prior audit as given.

#### 1. Is the hand pose ever tested against level geometry? No — verified two independent ways.

`UpdateHands` (`p_physics.cpp:1544`) writes a hand's `PhysBody` pose by direct assignment and nothing else:

```cpp
// p_physics.cpp:1590-1591
const DVector3 p = (hand == 0) ? pawn->AttackPos : pawn->OffhandPos;
const FVector3 newPos(MapToM(p.X), MapToM(p.Y), MapToM(p.Z));
...
// p_physics.cpp:1672-1673
b->pos = newPos;
b->rot = newRot;
```

No `Trace`/`Sweep`/`P_CheckPosition`/blockmap call appears anywhere between reading `AttackPos`/`OffhandPos` and writing `b->pos`/`b->rot` — confirmed by reading the full function body top to bottom (`p_physics.cpp:1544-1675`).

This is also structurally guaranteed, not merely "not called here": `StepBody` (`p_physics.cpp:806`) is the *only* function in the file that reads `sec->floorplane`/`sec->ceilingplane`/`sec->Lines`, and its second line is:

```cpp
// p_physics.cpp:812-814
AActor *a = b.owner;
if (a == nullptr) return;
if (b.asleep) return;
if (b.kinematic) return;
```

Hand bodies are constructed with `nb.kinematic = true;` unconditionally (`p_physics.cpp:1580`) and this is never changed for a hand anywhere in the file (confirmed by grep — `kinematic` is written for hand bodies exactly once, at construction). So the one function capable of testing a body against the world never runs for a hand, by construction — not by omission at the call site, but by an early-return that fires before any geometry query happens.

#### 2. What shape is the hand collider?

A box, sized entirely from a cvar, rebuilt from scratch every call:

```cpp
// p_physics.cpp:1585-1588
const float s = (float)*vr_physics_handsize;
b->half = FVector3(0.045f * s, 0.030f * s, 0.090f * s);   // 9x6x18cm at s=1
b->hulls.Clear();
b->ShapeFinish();
```

`hulls.Clear()` before `ShapeFinish()` forces `ShapeFinish`'s box-fallback path (`hulls.Size()==0` → `HullMakeBox`, `p_physics.cpp:551-576`) every single call, so a hand can never pick up real geometry. Confirmed the PHYSDEF path cannot reach a hand at all: `ApplyPhysDefShape` has exactly one call site in the whole file (`p_physics.cpp:2143`), inside `PhysicsEnable`, which only ever runs for script-registered actor bodies (`self->GetClass()->TypeName`). Hand bodies have `owner = nullptr` and are never constructed via `PhysicsEnable` — there is no code path, PHYSDEF-populated or otherwise, by which a hand could ever be anything but this box.

#### 3. Where is the rendered hand pose sourced from — could it already be divorced from the physics pose?

This is the finding that changes the shape of the work. **The rendered hand and the physics hand are not two ends of one pipe today — they are two independent live reads of the identical raw controller transform**, `VRMode::GetWeaponTransform()`, called from three separate places every frame:

1. `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp:4127-4151` (device-poll time) — writes `player->mo->AttackPos`/`OffhandPos` from `GetWeaponTransform(&mat, VR_MAINHAND/VR_OFFHAND)`. This is what `p_physics.cpp`'s `UpdateHands` reads.
2. `VRMode::SetUp()`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp:1420-1449` (render-scene-setup time, later in the same loop iteration, non-multiplayer only) — calls `GetWeaponTransform` *again* and overwrites the same `AttackPos`/`OffhandPos` fields a second time with a fresh read.
3. `ObjectToWorldMatrix`, `src/r_data/models.cpp:633` — the function that actually builds the matrix the hand/weapon model is drawn with — calls `vrmode->GetWeaponTransform(&objectToWorldMatrix, hand, ...)` directly. **It never reads `AttackPos`/`OffhandPos` at all.**

So today: physics reads a copy of the tracked pose written once per loop iteration; rendering re-samples the tracked pose independently at draw time. Nothing currently reads from the physics module in either `models.cpp` or `hw_weapon.cpp` — confirmed by grep for `p_physics|PhysBody|g_bodies` across `models.cpp` and `hw_weapon.cpp`, zero hits — and nothing in `p_physics.cpp` references `DPSprite`/rendering, also zero hits. The two "hands" in this engine are presentation-layer coincidences of both reading the same OpenXR number, not a simulated body driving a visual.

**Could they be divorced? Yes, trivially, in the sense that nothing currently forces them to agree** — but productively divorcing them (so the rendered hand visibly stops while the real controller keeps moving, which is the actual Rung-0 behavior) requires building a new data path in the *opposite* direction from anything that exists now: physics-frame output feeding a render-time draw transform. The engine does have a working precedent for a render layer's draw transform being overridden by something other than a live `GetWeaponTransform()` call — the HUD bone-anchoring system added this window (`models.cpp:736-797`, `RenderHUDModel`'s `loadMatrix` substitution for an anchored psprite layer) proves the render pipeline can already accept an externally-resolved transform for a specific layer instead of re-deriving it from the controller pose. That precedent is architectural evidence this is buildable, not blocked — but it is a different data flow than anchoring (which is render-resolved-bone → script, not physics-frame → render), so it is new plumbing, not a reuse of the anchor system itself.

#### 4. What world-collision machinery already exists, and does it already work for free bodies?

**Yes, confirmed directly** — verified by re-reading `StepBody`'s floor/ceiling block (`p_physics.cpp:878-915`) and wall block (`p_physics.cpp:917-1001`) end to end. The mechanism is real and already handles arbitrary convex-hull shape, not just a box:

- Every hull vertex of the body is transformed to world space once (`hullWorld`, `p_physics.cpp:869-876`) and reused for both passes.
- **Floor/ceiling**: each world vertex is tested against `sec->floorplane.ZatPoint`/`sec->ceilingplane.ZatPoint` for the body's own, live-evaluated sector — slopes and moving floors (lifts) work with no special-case code, because the plane is queried fresh every call rather than cached.
- **Walls**: tested against `sec->Lines` (the linedefs of the body's own sector), explicitly *not* the render mesh, specifically because the render mesh omits two-sided lines and untextured lines (comment at `p_physics.cpp:919-925`, corroborated by the code doing a from-scratch linedef walk rather than touching `DoomLevelMesh` anywhere). A line is fully blocking if one-sided or `ML_BLOCKING`; otherwise solid only outside the vertical gap between the higher floor and lower ceiling of its two sides (`p_physics.cpp:972-986`) — the step/window distinction is already implemented and correct.

This is a pure function of `(pos, rot, hulls, sector)` — nothing about it depends on the body being non-kinematic except the single early-return at `p_physics.cpp:814`. **That gate is the entire distance between "a hand" and this exact, already-working collision code.** Building hand collision is fundamentally a reuse job: either factor the contact-gathering block into a callable helper invoked from `UpdateHands`, or construct the hand's candidate pose, run it through a variant of this same test, and resolve/clamp before the final `b->pos = newPos` assignment — not new collision math.

#### 5. What does GZDoom's own collision offer, and what is NOT represented?

- **Used and working**: `sec->floorplane`/`sec->ceilingplane` (live per-point evaluation — slopes, lifts), `sec->Lines` (linedef-based wall solidity with correct one-sided/`ML_BLOCKING`/step-window handling).
- **Deliberately not used, by explicit design choice, not an engine limit**: the blockmap. The comment at `p_physics.cpp:927-929` states the restriction directly: only the body's own sector's lines are considered, justified for "a box a few centimetres across." Verified: there is no blockmap call anywhere in the file. A body whose bounding radius exceeds the distance to a line in an *adjacent* sector — or a hand swung fast enough to cross a sector boundary mid-frame — will not see that geometry. This is an accepted simplification for small, slow objects; a fast-swung hand or fist is exactly the case that stresses it.
- **Not represented at all**: 3D floors. Grepped the whole file for `3dfloor|F3DFloor|Portal` — the only hit is `a->CheckPortalTransition(false)` inside `WriteBack` (`p_physics.cpp:1839`), which exists solely to keep `AActor::Sector` correct after a body crosses a line portal; it has nothing to do with contact detection. The floor/ceiling test reads only `sec->floorplane`/`sec->ceilingplane` — a sector's *own* planes — never `sec->e->XFloor.ffloors`. A body (and, once built, a hand or held object) passes straight through a 3D-floor's walking surface — a platform, a bridge, a stack of ledges — with zero contact. Portals get the same treatment: the actual per-step contact-gathering has no portal awareness at all; only the post-solve relink does.

#### 6. Held objects: do they stop against walls, or does the carry loop bypass collision unconditionally?

**Definitive answer, read directly: the carry loop bypasses collision unconditionally, identically to hands, for the identical structural reason.**

```cpp
// p_physics.cpp:1696-1715 (UpdateHands' carry loop, immediately after the hand-pose loop)
h.rot = q;                                                         // built from grabRotOffset, no test
h.pos = hand->pos + hand->rot.Rotate(h.grabPosOffset);             // direct overwrite, no test
h.vel = hand->vel + Cross(hand->angVel, h.pos - hand->pos);
h.angVel = hand->angVel;
h.asleep = false;
h.sleepTimer = 0.f;
```

No call to any contact-gathering or resolve function appears in this loop — verified by reading it in full. And this is guaranteed structurally, not incidentally: `PhysicsGrab` (`p_physics.cpp:2333-2384`) sets `b->kinematic = true;` at grab time (`p_physics.cpp:2379`, confirmed directly), so even if something in the carry loop *did* call `StepBody`, the `if (b.kinematic) return;` gate at line 814 would still refuse to run the floor/ceiling/wall test on it. A held object is moved through a wall or the floor by the player's own hand motion with zero resistance — exactly like a hand, and for exactly the same one-line reason. (Body-vs-body contact via `SolvePair` still runs against a held object each step, so it can push a *free* body sitting nearby, but `ApplyImpulse` is gated `if (!X.kinematic)` on both sides of that solve — `p_physics.cpp:1486-1487, 1515-1516` — so it can never itself be moved or stopped by that contact.)

#### What building this actually takes — mechanism-first

**(A) Hand stops at geometry — MERELY UNBUILT, not structurally blocked.** The floor/ceiling/wall test in `StepBody` is already exactly the machinery a hand needs and already handles arbitrary hull shape, slopes, moving floors, and the step/window distinction correctly. The work is: (1) factor `StepBody`'s contact-gathering (`p_physics.cpp:869-1001`) into a form callable with a candidate `(pos, rot, hulls, sector)` outside the free-body integration path; (2) add a resolve step for a *kinematic mover* — not `StepBody`'s velocity-space sequential-impulse solve, which assumes a dynamic body, but a positional correction (push the candidate position out along each penetrating contact's normal, iterated a few times, or a swept move-and-stop) applied in `UpdateHands` before the final `b->pos = newPos`; (3) keep the raw tracked pose available separately from whatever the hand `PhysBody` now resolves to, because `AttackPos`/`OffhandPos` are read elsewhere for attack-ray origin and hitscan (`MapWeaponDir`, `hw_vrmodes.cpp:1357-1387`) and must not themselves be clamped, or every hitscan would be pulled to the clamped hand position too; (4) reuse the existing `thinHalf`-based CCD substep pattern (already used for free bodies, `p_physics.cpp:1985-2012`) so a fast hand swing doesn't tunnel through a thin wall in one step. Self-contained to `p_physics.cpp`/`p_physics.h`; no other file needs to change for the physics side alone.

**(B) Making the rendered hand actually stop on screen — MERELY UNBUILT, but new plumbing in a direction nothing currently flows.** Physics-frame output has no path back into a render draw transform today (confirmed zero coupling either direction). This needs: a new output (an `AActor`-level field, mirroring `AttackPos`'s shape, or a small physics-module getter) written once per `P_PhysicsFrame` call with the resolved/clamped hand pose from (A); and a branch in `models.cpp`'s `ObjectToWorldMatrix`/`RenderHUDModel` (and possibly the `controllerTransform` read at `hw_weapon.cpp:670`) that uses it instead of a fresh `GetWeaponTransform()` call, for the hand/weapon HUD layers specifically. `P_PhysicsFrame` runs before `D_Display()` in the same `D_DoomLoop` iteration (`d_main.cpp:1879-1898`), so there is no frame-ordering problem — the clamped pose from this iteration's physics step is available in time for this iteration's render. Because `GetWeaponTransform` already returns false outside VR (the existing fallback in `MapWeaponDir`, `hw_vrmodes.cpp:1370-1372`, is the proof), gating the new branch the same way keeps this entirely inside the VR hand-model path and away from flatscreen rendering — but it does touch shared functions (`ObjectToWorldMatrix` serves every actor with a model, not just hands), so the new branch must be scoped tightly (hand-body-only, VR-only) to avoid any flatscreen regression risk.

**(C) Held objects — MERELY UNBUILT, and there is a cheaper path than duplicating (A)'s work.** Because a spring/PD-followed body is *not* kinematic, it goes through `StepBody`'s normal integration and gets the full floor/ceiling/wall test for free — zero new geometry code required. The change is: give a held object a new grab mode where `kinematic` stays `false`, and instead of the carry loop's direct `h.pos = ...` overwrite, a corrective force/torque toward the hand-relative target pose (computed exactly as today, `hand->pos + hand->rot.Rotate(grabPosOffset)`) is applied each step via the module's existing `ApplyImpulse`/`invInertia` machinery. This is the module's first constraint-like primitive — the audit's "no constraint solver" gap (zero matches for `constraint|joint|hinge|prismatic|ragdoll|motor`) is exactly what this fills in miniature. Two real costs, not just code: the throw-velocity logic (`PhysicsRelease`'s peak-of-swing search over `kHandHistory`, `p_physics.cpp:2443-2461`) currently assumes the object's velocity has been the hand's velocity continuously — a spring-followed body has its own genuine simulated velocity instead, which may need re-deriving or may turn out to be strictly better, but the existing tuning is not preserved automatically; and spring stiffness needs enough gain to feel "held," not "elastic," which is a feel-tuning pass, not just a correctness one.

**(D) Hardening the shared contact test — worth doing alongside (A)/(C), not a blocker for either.** The own-sector-only linedef search and the total absence of 3D-floor (`F3DFloor`/`XFloor`) testing are real, verified gaps in the machinery both (A) and (C) would reuse. Neither is required for a first working version at ordinary single-sector scale, but a hand or a thrown object interacting with a 3D-floor platform, or a fast swing across a sector boundary, will silently pass through today and would continue to after (A)/(C) ship unless this is also done.



### Rung 1 assessment — firearm-part fidelity and multi-body articulation (Pavlov)

#### Does any constraint/joint solver exist? No — verified directly, not inferred.

`grep -ni "constraint|joint|hinge|prismatic|ragdoll|motor" src/playsim/p_physics.cpp src/playsim/p_physics.h` returns **zero matches**. A second targeted search for `spring|slider|track|axis constraint|DOF` returns one hit, and it is a menudef UI comment ("these are live menu sliders", `p_physics.cpp:1797`) about CVAR sliders in the options panel — unrelated to physics DOF. There is no joint type, no articulation, no motor, no track, anywhere in the module. What exists (confirmed by re-reading the code, not the audit's summary of it) is exactly two mechanisms:

1. **Free-body contact resolution** (`StepBody`, `p_physics.cpp:806-1268`) — sequential-impulse, Baumgarte-stabilised (`kBaumgarte=0.15`, `p_physics.cpp:199`), 8 iterations (`kSolverIterations=8`, `:184`), against floor/ceiling planes and sector linedefs.
2. **Pairwise body-vs-body contact resolution** (`SolvePair`, `p_physics.cpp:1293-1524`) — the identical sequential-impulse structure, corner-in-hull narrow phase, invMass-weighted impulse split.

Both are *contact* solvers: they push bodies apart along a normal when they overlap and stop pushing when they don't. Neither has any concept of "these two bodies must stay a fixed distance apart" or "this body may only rotate about this one axis" — a constraint that holds whether or not the bodies are touching. **This is the correct target for the "structurally blocked" label**, but the blockage is architectural absence, not a design decision actively preventing constraints — nothing in `StepBody`/`SolvePair`'s contact-only design would need to be *torn out* to add a constraint pass; it would be a new, parallel mechanism slotted into the same step loop. So: **structurally blocked in the sense that the primitive does not exist and nothing partial exists to extend — but not blocked by any invariant that would need breaking.**

The two-handed grip today (`vk_openxrdevice.cpp:3596-3626`) is exactly the "stabilise bonus" the assignment predicted, verified by reading the code, not assuming: `TwoHandedHold` is a published boolean read by weapon script to tighten spread (comment at `:3596-3599`, corroborated by the fact that this variable is never read anywhere in `p_physics.cpp` — grep confirms zero references). `weaponStabilised` (`:3609`) only recomputes `weaponangles[0]/[1]`, a render-time aim-direction override for the *rendered* weapon orientation — it is computed from `atan2`/`atanf` on the raw hand-position delta (`:3613-3620`), not from any impulse, mass, or constraint solve, and it never touches `g_bodies`. The off hand cannot push, twist, or be resisted by the weapon body through this path; it can only bias which way the barrel visually points.

#### What the existing PHYSDEF/PhysHull system already gives toward Pavlov-tier parts

More than the contact-only framing suggests. Re-read directly, not just cited from the earlier audit:

- **`PhysHull` is already a compound-convex representation with concavity support** (`p_physics.cpp:298-316`): a `PhysBody` holds a `TArray<PhysHull>`, and the file's own header comment states the motivating case explicitly — *"A MAGWELL IS A CAVITY... A grip built from four convex slabs does [have a hole], and a magazine can be pushed up into the gap between them and be stopped by the walls if it is crooked."* (`p_physics.cpp:282-289`). This is real, working geometry, not aspirational: `HullDeepest` (`:377-398`) is a genuine point-vs-half-space depth test per hull, and `SolvePair`'s narrow phase (`:1341-1382`) tests every vertex of body A against *every* hull of body B (and vice versa) specifically so a compound shape's gap behaves as a gap. **The geometric substrate for "magazine physically fits into magwell" already exists and already works for the contact side of that problem** (a magazine bumping the magwell walls). What's missing is not the shape system — it's a constraint that can *snap and hold* the magazine once it's aligned; today, contact alone can stop a misaligned magazine but cannot pull an aligned one home and lock it there.
- **PHYSDEF is a real, working per-actor-class shape loader** (`LoadPhysDefs`/`ApplyPhysDefShape`, `p_physics.cpp:400-508`, `652-677`), keyed by class name, hulls authored relative to the actor origin and auto-corrected into centre-of-mass space. Confirmed still true at HEAD: this repository ships **zero** `.physdef` lump content (`g_shapeLib` is always populated empty within this tree), so every weapon and hand collider in practice is the CVAR-sized box. The loader itself needs no new engine work to accept real per-part geometry — only content, plus (see below) attach-point metadata the current grammar doesn't carry.
- **The IQM bone-query API is fully separate from PHYSDEF and not wired to it.** `GetJointPosition`/`GetJointRotation`/`FindJoint`/`GetJointBaseTRS` (`model_iqm.h:188-234`, all pre-existing, unmodified by physics) can answer "where is the grip bone in bind pose" for any IQM-rigged weapon, and the new HUD bone-anchoring system (`models.cpp:122-255`) proves this data is reachable at runtime with correct axis handling. But there is no code path from a model's bone hierarchy into `PhysHull` generation or into `ApplyPhysDefShape` — PHYSDEF vertices are authored (by an external tool, per the file's own comment, `:419-421`) as flat coordinate lists, not derived from bones. **A grip-point / slide-rail / magwell-alignment axis for a constraint would have to be new authored data (new PHYSDEF fields, or a new lump) — the bone API is a read-only query surface for rendering, not currently a source of physics-frame data.**

#### The constraint solver: what type, and where it slots in

Pavlov-tier articulation needs at minimum three constraint families, none of which reduce to the existing contact solver:

1. **Prismatic (1-DOF translation along an axis, with limits)** — the charging handle, slide, and bolt. Each needs: an axis in the parent weapon-body's local frame, a min/max travel distance, and (optionally) a small return-spring bias and/or a detent (locked-back state at max travel, requiring a released-latch input to move again).
2. **Hinge/revolute (1-DOF rotation about an axis, with limits)** — a selector switch, a floorplate, a folding stock. Same shape as the prismatic constraint with rotation substituted for translation.
3. **Point-to-point / distance (0- to 3-DOF positional pin, optionally with a breaking threshold)** — two-handed grip (off-hand support point pinned near the forend, with enough compliance to let the hand slide along the forend axis rather than being rigidly locked, which is really a prismatic-along-forend constraint with a perpendicular point-pin), and the magazine snap-into-magwell (a point constraint that only activates once alignment + depth thresholds are met, i.e. a "soft" constraint gated by a geometric precondition rather than always active like the others).

None of these are motorised in the classic sense (no PID/servo needed) — a spring-return default and a limit clamp cover charging handle/slide/selector; the two-handed grip needs compliance (a stiff-but-not-infinite point constraint, same numerical family as the existing Baumgarte-biased contact, just applied whether or not the two bodies currently overlap) rather than a rigid lock, since a real off-hand doesn't lock a weapon in a vice.

**Where it slots into `P_PhysicsFrame`'s step loop** (`p_physics.cpp:1979-2028`, structure verified above): constraints are a **third pass**, added after the existing per-body `StepBody` loop and interleaved with — not replacing — the existing `SolvePair` body-vs-body pass, within the same fixed-step `while` loop:

```
while (accumulator >= step) {
    for each body: integrate + CCD-substep StepBody(...)   // existing, unchanged
    for each constraint: SolveConstraint(...)               // NEW — same iteration count/warm-start pattern
    for each body pair: SolvePair(...)                       // existing, unchanged
    accumulator -= step
}
```

Constraint resolution wants to run in the **same sequential-impulse iteration structure** `StepBody`'s solve block already uses (`kSolverIterations=8` iterations, `p_physics.cpp:1044`) — a joint constraint contributes a target relative-velocity-along-the-constraint-axis of zero (or a limit-clamped nonzero, for a slider not yet at its stop), computed and applied exactly like a contact's normal impulse, just without the `if (c.penetration > slop)`-gated Baumgarte bias being conditional on overlap — a joint's positional-error bias is unconditional, since a joint is always "in contact" by definition. This is architecturally the same math already in the file (`EffectiveMass`, `ApplyImpulse`, accumulated-and-clamped impulse across iterations, `:1031-1136`) generalized from "3 possible contact normals per vertex" to "1-2 constrained axes per joint," so the amount of genuinely new numerical code is smaller than a solver built from scratch — but there is currently no `Constraint` struct, no registry parallel to `g_bodies`, and no call site, so this is new infrastructure, not a parameter change.

**Interaction with sleep**: today, `supportTimer`/`sleepTimer` are per-body and driven by *this body's own* displacement drift plus *this body's own* contact latch (`p_physics.cpp:1151-1226`). A constrained sub-part (a slide sitting in its rearmost detent, held there by a limit, not by contact) needs the same displacement-drift sleep gate to apply — which it will, for free, once the constraint's positional correction stops changing the body's pose — but a **linked pair of bodies should share a wake state**: waking the weapon body (grabbing it) must wake every part constrained to it, or a script `PhysicsAddImpulse` on the frame could apply to a slide the solver still considers asleep and skip integrating the correction until the next tick. This is a small but real addition to `SolvePair`'s existing kinematic-wake logic (`kWakeOnImpact=0.15`, `p_physics.cpp:1403`, currently pair-local) — a constraint edge needs the same "wake the other side" propagation contacts already do.

#### `IsPhysicalWeapon`'s "t77" substring: what should replace it

Confirmed exactly as flagged: `p_physics.cpp:1743-1749`, `name.IndexOf("t77") >= 0` on the lowercased class `TypeName`, the sole gate for whether `UpdateWeapons` builds a weapon collider at all. The general mechanism this needs is the same pattern the module already uses for enabling a rigid body on any other actor — **a script-set flag, not a name pattern**:

- `PhysicsEnable`/`bPhysicsBody` (`MF9_PHYSICSBODY`) already exists as the actor-level "this thing has a physics body" contract (`src/scripting/thingdef_data.cpp:344`, gates `AActor::Tick`'s native-movement skip at `p_mobj.cpp:4607`). `UpdateWeapons` should key off an equivalent per-weapon marker — either reusing `bPhysicsBody` directly (a `Weapon` calls `PhysicsEnable` in its own `BeginPlay`/`Spawn`, the same way any other physical prop would) and having `UpdateWeapons` iterate `g_bodies` for entries whose `owner` is the currently-readied/offhand weapon and already has a body registered, or a narrower `bPhysicalWeapon`/equivalent flag if weapon-hand attachment needs to stay logically distinct from generic rigid-body registration (e.g. because the weapon-hand body is `owner == nullptr` today, unlike a PhysicsEnable'd prop's body — see below).
- One real wrinkle: today's weapon-hand `PhysBody` is deliberately owner-less (`nb.owner = nullptr`, `p_physics.cpp:1786`), sized and positioned purely from hand pose + CVar offsets, decoupled from any actual `AActor`. Moving to "any weapon can be physical" means this body needs to be tied to the *actual equipped weapon actor* (to pull its PHYSDEF shape, its mass, its part hierarchy) — which changes `UpdateWeapons` from "does the class name match" to "does `pl->ReadyWeapon` have a body already registered via `PhysicsEnable`, and if so, drive its kinematic pose from the hand instead of building a parallel anonymous body." This is a bigger structural change than swapping one string check for one flag check — it's collapsing two parallel representations (the CVar-box weapon-hand body, and a hypothetical PhysicsEnable'd weapon-actor body) into one, and the "just replace the string match" framing understates that.

#### Sizing

- **Constraint solver core** (types, registry, prepare/warm-start/solve, integration into the step loop): this is the load-bearing item everything else depends on. Genuinely new numerical code, but reuses the existing sequential-impulse/Baumgarte machinery's shape — realistically a few hundred to ~1000 lines given the file's existing density per feature (compare: body-vs-body contact resolution, a comparably-scoped feature, is `SolvePair` at ~230 lines). Depends on nothing else in this list; everything else depends on it.
- **Per-part PHYSDEF authoring + attach-point metadata**: the loader/grammar exists; what's missing is (a) new PHYSDEF keywords or a sibling lump for naming attach points/axes on a hull (today's grammar is `Body`/`Hull`/`V`/`P` only — no named points, no axis declarations), and (b) actual per-weapon content, which is external-tool/mesh work outside this repository's own scope per the model-conversion rules already on file. The engine-side grammar extension is small; the content-authoring is unbounded and not this codebase's problem to size.
- **Two-handed grip as a constraint**: small once the solver core exists — it's one constraint instantiated/torn down alongside the existing `GripSubjectOff == Support/Forend/Foregrip` transitions already computed in `vk_openxrdevice.cpp`.
- **Slide/charging-handle prismatic + positional chamber state**: the largest single item after the solver core itself, because "positional, not boolean" chamber/feed state is a design problem (what does a half-cycled slide *mean* for firing logic, extraction, double-feed) as much as a physics problem, and touches `weapons.zs`'s state machine, not just `p_physics.cpp`.
- **Magazine snap constraint**: medium — the alignment *test* is nearly free (reuse `HullDeepest`/corner-in-hull, already proven for the magwell case), the new part is the constraint that activates conditionally and the UX of "how crooked is still insertable."



### Rung 2 assessment — Boneworks-class (force authority, simulated player body)

#### The contradiction, confirmed directly from code

Boneworks-class hands require finite mass: the world resists them and they resist back, symmetrically. The current hand model is the opposite of that by construction, at three independent points that all have to agree for a hand to move at all:

1. **Construction, `UpdateHands`** (`src/playsim/p_physics.cpp:1573-1583`): every hand body is built with `nb.kinematic = true; nb.invMass = 0.f; nb.invInertia = FVector3(0,0,0);` — invMass 0 is the standard rigid-body encoding of infinite mass. No force applied to it can ever change its velocity, by definition of the impulse math (`ApplyImpulse`, `p_physics.cpp:800-803`: `b.vel += impulse * b.invMass` — multiplying by zero).
2. **Integration, `StepBody`** (`p_physics.cpp:814`): `if (b.kinematic) return;` — the very first substantive line. A kinematic body never reaches gravity, damping, integration, or contact collection. It is not merely heavy; it is not simulated at all.
3. **Contact response, `SolvePair`** (`p_physics.cpp:1486-1487, 1515-1516`): `if (!A.kinematic) ApplyImpulse(...)` guards every impulse application on both sides of every pair. Two kinematic bodies touching (hand vs. hand, hand vs. held weapon) resolve to a geometric contact used only to fire a haptic (`p_physics.cpp:1417-1441`) — zero momentum changes hands on either side.

Pose itself is a direct assignment from tracked/derived values with no world query anywhere in the path (`p_physics.cpp:1590-1608, 1668-1674`) — confirmed by grep: no `Trace`/`Sweep`/`P_CheckPosition`/`Blockmap` call exists anywhere in the file outside an unrelated debug feature. So today: the rendered hand can be stopped visually at a wall by other means (not audited here), but the **physics** hand is not stopped by anything — it teleports through geometry every frame at exactly the controller's raw position, and nothing it touches can push it off that line. This matches Alyx/Pavlov's kinematic-hand floor exactly (see Tier 1 table), not Boneworks'.

#### What "force authority" requires, mechanism by mechanism

**1. The hand becomes a PD/spring-damper-driven dynamic body, not a pose copy.** `kinematic` flips to `false`, `invMass`/`invInertia` get real values (a hand mass on the order of 0.3-0.6 kg is the usual VR-hand convention), and instead of `UpdateHands` writing `b->pos = newPos; b->rot = newRot;` directly (`p_physics.cpp:1672-1673`), it computes a spring force/torque toward the tracked pose (`F = k·(target - pos) - c·vel`, and the quaternion equivalent for torque) and applies it as an impulse each fixed step, then lets `StepBody`'s existing gravity/damping/integration/contact pipeline run normally — for the first time, ever, on a hand. This reuses `StepBody`, `SolvePair`, sleep, and CCD substepping (`p_physics.cpp:1985-2012`) completely unmodified; none of that machinery cares whether a body is a hand or a prop once it's no longer kinematic.

**2. Mass ratio and stability at the current step rate.** The solver is a low-iteration sequential-impulse scheme: `kSolverIterations = 8` (`p_physics.cpp:184`), `kBaumgarte = 0.15` (`p_physics.cpp:199`), stepped at `vr_physics_hz` (default 90, clamp [30,240], `p_physics.cpp:77-81`) as one flat fixed-step loop over a single `TArray<PhysBody> g_bodies` (`p_physics.cpp:679`) shared by hands, weapons, and every physics prop with no per-class rate separation. Two problems compound:

   - *Explicit spring stiffness has a hard ceiling proportional to `1/dt²`.* At 90 Hz, `dt ≈ 11.1 ms`. A hand needs to feel essentially rigid — near-zero perceptible lag or give against the controller target, because your real hand is still moving and any daylight between it and the rendered hand reads immediately as "floaty." That demands a PD gain far above what semi-implicit Euler integration can absorb stably at 11 ms steps; push the gain past the stability ceiling and the hand oscillates or explodes on the very first frame it contacts something stiff (a wall). This is not a tuning problem solvable by picking a slightly-better constant — it is the standard explicit-integration stiffness/step-size tradeoff, and `vr_physics_hz`'s own clamp tops out at 240 Hz (`p_physics.cpp:81`), which is still well short of the 500 Hz-1 kHz dedicated sub-loops that force-feedback-hand implementations typically decouple for exactly this reason. Nothing in this architecture decouples a hand-rate sub-loop from the world-contact rate today — `g_bodies` is one array, stepped once, at one shared rate.
   - *8 iterations is thin for the mass ratios this would introduce.* Today the "infinite mass vs. anything" case is dodged entirely — the kinematic guard means a hand never actually participates in impulse resolution, so the low iteration count has never been tested against it. Once the hand is a real (if light) dynamic body under a stiff spring, pushing it against an immovable wall is a large-effective-stiffness-vs-low-iteration-count convergence problem: sequential impulse solvers converge slowly for large mass/stiffness disparities, and 8 iterations at 90 Hz will visibly under-resolve a hand jammed into a corner — it will sink in slightly, or judder, unless the spring gain is capped below what "feels rigid" would want.
   - *Honesty check against the reference itself:* Boneworks' own hands are visibly soft against stiff geometry — the well-known hand-clipping/wobble against walls in that game is the observable symptom of this exact tradeoff, not a bug the reference solved and this engine hasn't. Force-driven hands trade Alyx/Pavlov's perfect positional fidelity for physical presence, and some give against stiff obstacles is inherent to the approach, not an engineering gap to close.

**3. Grabbing is no longer a rigid pose copy — two lag stages appear.** Today, `PhysicsGrab` captures a hand-local offset once (`p_physics.cpp:2367-2377`) and the carry loop re-applies it as a direct, unconditional pos/rot overwrite every frame (`p_physics.cpp:1696-1715`) — the held object tracks the *raw controller pose* perfectly, because the hand itself currently *is* the raw controller pose. Once the hand becomes a spring-driven approximation of the controller target, that assumption breaks in a way that has to be designed, not just ported: does the held object (a) rigidly follow the hand's *actual, potentially-lagging, potentially-penetrating* simulated pose (one lag stage: controller→hand), or (b) get its own second spring relative to the hand (two lag stages, softer but with graceful give under load, and now needs its own breakaway threshold so a magazine caught on a doorframe doesn't drag your hand back with it)? Neither exists in this codebase today; both are new state and new logic. Either way, `PhysicsGrab`/`PhysicsRelease`/the carry loop's unconditional-overwrite design (`p_physics.cpp:1696-1719`) is fundamentally incompatible with a non-infinitely-stiff hand and has to be rewritten, along with adding a force-limited breakaway that does not exist in any form today (a grab currently never auto-releases; it holds until `PhysicsRelease` is explicitly called, no matter how much force resists it).

#### The player-body wall: `P_PlayerThink` / `P_XYMovement` / `P_ZMovement`

`src/playsim/p_user.cpp`, `p_map.cpp`, and (outside the one 33-line `MF9_PHYSICSBODY` insertion already covered in the physics-module review) `p_mobj.cpp` all carry **zero diff** in this entire window (`git diff 1d2572bdcc main --stat` confirms `p_user.cpp`/`p_map.cpp` untouched). Player movement is exactly stock Doom architecture, never forked for VR at the movement-authority level:

- `P_XYMovement(AActor *mo, DVector2 scroll)` (`p_mobj.cpp:2435`) is a per-actor XY momentum-and-friction integrator (`STOPSPEED`/`CARRYSTOPSPEED` constants, `p_mobj.cpp:2430-2431`) called once per 35 Hz tic from `AActor::Tick`. Its **only** external-force-like input is the `scroll` parameter — and that is a **position delta**, not a force: it's how roomscale head movement nudges the player capsule (the function is explicitly marked non-static "since the VR device layer... calls this directly for roomscale locomotion," `p_mobj.cpp:2434`). This is real, pre-existing evidence the mover *can* accept an externally-supplied displacement — but it is XY-only, once per tic, and a teleport-style delta, not a continuous force integrated against mass.
- `P_ZMovement` (`p_mobj.cpp:2969`) owns gravity and floor/ceiling clamping for a single cylinder — one z-range, one floor contact, one ceiling contact. There is no slot in this model for a second contact point (a hand gripping a ledge above your head while your feet are still on the floor) or for torque (being shoved sideways while off-balance).
- Velocity itself has exactly one writer: the ticcmd (`player->cmd.forwardmove/sidemove`), converted to momentum once per 35 Hz tic inside `P_PlayerThink` (`p_user.cpp:1717`) and consumed by `P_XYMovement`. There is no additive external-force accumulator field on `AActor` that this code drains — and there cannot be one read continuously at physics rate without hitting the exact 35 Hz-lag problem the engine's own physics module was built specifically to avoid for held objects: the `D_DoomLoop` comment justifying `P_PhysicsFrame`'s placement outside `TryRunTics` states plainly that a held object updated at 35 Hz "lags the hand by up to 28ms" (`d_main.cpp:1879-1898`, `p_physics.h:39-48`). A shove or a climbing grip driving the player only once per tic would reintroduce that same lag for your entire body, not just a held prop — a much worse regression, since a laggy weapon is tolerable and a laggy sense of your own body's momentum is nauseating in VR.
- The player pawn is never registered with the physics module — `PhysicsEnable` has zero callers anywhere in this repository (confirmed in the Script API surface review), so there is no `g_bodies` entry for the player and no `WriteBack` path writing its transform. `WriteBack` (`p_physics.cpp:1827-1857`) is the only mechanism that lets a physics-simulated pose reach an `AActor` off-tic, and it explicitly no-ops for owner-less bodies and is never invoked for the player.

#### Structurally blocked vs. merely unbuilt — explicit classification

**Already there** (reusable as-is): the sequential-impulse contact/friction/restitution solver (`StepBody`, `SolvePair`), CCD substepping keyed off `thinHalf`, sleep/wake, haptics-on-contact, and — narrowly — the one position-delta locomotion channel (`P_XYMovement`'s `scroll` parameter) proving the mover has at least one precedent for accepting externally-driven displacement.

**Merely unbuilt** — nothing in the architecture prevents these, they simply don't exist yet:
- Finite-mass, PD-driven hands (item below). Reuses `StepBody`/`SolvePair` unmodified; the change is confined to how a hand body is constructed and how its target pose is applied.
- Grab-as-spring with breakaway. Reuses `ApplyImpulse`/`EffectiveMass`; no new solver primitive, just a new per-frame force term and a threshold check.
- A constraint/joint solver. Large, foundational, and currently **totally absent** (zero matches for `constraint|joint|hinge|prismatic|ragdoll|motor` across the whole module) — but the data model (`g_bodies` array, pairwise `SolvePair`) does not architecturally exclude adding a joint list and a joint-solve pass. This is a "doesn't exist yet and is a lot of work" gap, not a "the architecture fights you" gap.

**Structurally blocked** — the current architecture actively prevents these, not merely omits them:
- **Responsive (near-90Hz) force coupling from hand contact into player movement.** `P_XYMovement`/`P_ZMovement` have exactly one velocity authority (ticcmd, once per 35 Hz tic) and no force-accumulator concept at all; adding one that's drained once a tic reproduces the identical 28 ms-lag problem the engine's own physics module exists to solve for held props, now applied to the player's entire sense of momentum. Avoiding that lag requires pulling player-position authority out of the tic loop the way `WriteBack` does for `PhysBody`-owned actors — but the player's collision authority (`P_TryMove`, blockmap-based) is itself tic-bound, and reconciling an off-tic position write against tic-bound blockmap collision is unbuilt in either direction. This is the same class of problem `p_physics.cpp:927-929`'s own "own sector only" comment admits is a simplification for small props — scaled up to the player's entire collision-sensitive movement stack.
- **A simulated, articulated player body for climbing/bracing/ragdoll.** `P_ZMovement`'s single-cylinder, single-floor-contact, single-ceiling-contact model has no slot for a second contact point, sustained off-axis grip force, or torque. Building one is not an extension of the existing mover, it is a second player-movement architecture running in parallel — one that all of Doom's sector-special/lift/door/crusher/portal/monster-interaction code implicitly assumes talks to the classic single-cylinder mover. `p_user.cpp` has zero VR-only movement branch today (confirmed zero diff this window) — flatscreen and VR share the exact same movement code path, so any change deep enough to let hand-forces move the player body risks flatscreen regressions directly, not incidentally.

#### Cost and what breaks

- Finite-mass hands + spring grab: self-contained inside `p_physics.cpp`. No player-mover touch, no flatscreen risk, no new files. This is the only piece of Rung-2 machinery that doesn't put non-VR play at risk.
- A constraint/joint solver: large, foundational, self-contained inside the physics module (`p_physics.cpp`/`.h`) — no flatscreen risk by itself, but it is a multi-week undertaking in scope (warm-starting, joint-vs-contact solve ordering, per-joint break thresholds) comparable to bootstrapping a second physics engine subsystem from nothing.
- Player force coupling and articulated player body: **directly touches `p_user.cpp`/`p_mobj.cpp`/`p_map.cpp`**, the exact files that have been untouched (and therefore unbroken) through this entire window. This is where flatscreen risk becomes real and immediate — the player movement code that a flatscreen mod exercises is the same code a VR force-driven body would have to reroute through or replace.

#### Recommendation

Full Boneworks-class — force-authority hands **and** a simulated player body that those forces can act on — is not the right next target for this codebase, and going for it now is the trap the framing asks about directly. It requires, in order: a constraint/joint solver that does not exist at all today (large, foundational, self-contained but expensive); a wholly new force-coupling path into a player mover (`p_user.cpp`/`p_mobj.cpp`) that has been left completely untouched through 56 commits of otherwise aggressive rework, meaning this is truly unstarted territory, not a gap in existing VR-specific code; and a multi-segment articulated player representation the single-cylinder `P_ZMovement` model has no slot for — which is a second player-movement architecture, not an addition to the first, sitting directly in the path of every flatscreen mod this build still runs.

**Finite-mass, PD-driven hands with a spring-and-breakaway grab — with the player body left exactly as it is today — is the correct next step**, and it is a real, meaningfully higher rung than Alyx/Pavlov's fully-kinematic model: the hand itself gets genuine mass-driven resistance (a heavy prop drags on it, a wall stops it with actual give rather than a render-only clamp elsewhere), while `P_XYMovement`/`P_ZMovement` and the entire flatscreen movement stack stay untouched and unendangered. It reuses the existing contact/friction/sleep/CCD solver wholesale, needs no new primitive, and is bounded, estimable work (see items). It should also be sequenced before the constraint solver, not after: the same missing joint primitive that blocks climbing/bracing also blocks the Tier 1 Pavlov-axis firearm fidelity work (a magazine sliding into a magwell, a charging handle on a constrained track) — so if a constraint solver gets built at all, its business case should be firearm-part fidelity, which this codebase's own shipped UI copy already flags as a placeholder (`vr_physics_weaponlen/width/height` sizing "a solid placeholder box," `wadsrc/static/menudef.txt:2585-2592`), not player climbing. Player-body force coupling and full ragdoll/climb parity should be treated as out of scope until and unless the user is willing to accept real flatscreen-movement risk in exchange — which, given the stated priority of preserving flatscreen play, is a bar this doesn't clear today.



### Tier 2 — Environmental Combat Reactivity (F.E.A.R. / Trepang2 / Selaco)

#### What Selaco actually proves is reachable here

Selaco ships as content (ZScript + assets) on stock GZDoom, not as an engine fork with bespoke renderer capability. Every primitive it needs is already present in this tree, predates this window entirely, and carries zero diff in `1d2572bdcc..main` (confirmed directly, not inferred):

- **Decals** — `src/playsim/a_decals.cpp` (`DImpactDecal`, `SpawnDecal`, ACS `ACSF_SpawnDecal`). Already there.
- **Particles** — `src/playsim/p_effect.cpp/.h`. Already there.
- **Dynamic lights** — `src/playsim/a_dynlight.cpp/.h`, `src/rendering/hwrenderer/hw_dynlightdata.cpp`. Already there.
- **Destructible geometry** (sector/linedef health groups, texture-swap and event callbacks on damage-threshold crossing) — `src/playsim/p_destructible.cpp`, copyright-attributed to ZZYZX 2018. Already there, and — verified by grep across this repo — exercised by **zero shipped content** here.
- **Terrain-keyed floor/splash sound** — `src/gamedata/p_terrain.{cpp,h}`, `FTerrainTypeArray` keyed by `FTextureID`. Already there, but its only two call sites both query a **sector's flat texture** (`p_sectors.cpp:970`, `p_3dmidtex.cpp:303`) — never a wall.

None of this is this window's work; it's the shared floor this fork inherited. This window's own two Tier-2-relevant additions — the rigid-body physics module and the shader/postprocess VFX layer — are separate concerns layered on top of that same baseline, not replacements for it.

#### Debris: real physics today, a confirmed scaling cliff at density

`PhysicsEnable`/`PhysicsAddImpulse`/`PhysicsGrab` give a shell casing or blast chunk real gravity, friction, restitution, and sleep **today** (`src/playsim/p_physics.cpp:2177` etc.) — already there as engine capability, merely unbuilt as content (zero callers anywhere in this repo's `wadsrc`, matching the physics-module audit's own finding for the API generally).

Fidelity ceiling on that debris, all merely-unbuilt refinements rather than architecture walls: box collider unless a PHYSDEF hull is authored (none ship here); own-sector-only wall collision (`p_physics.cpp:927-929`), so a casing rolling through a doorway can miss the next sector's walls until `WriteBack` catches `AActor::Sector` up at end-of-frame; and fixed contact-array caps (`kMaxContacts=96`, `kMaxPair=48`, `p_physics.cpp:858-860,1327-1328`) that silently drop excess contacts in a dense pile with no warning, unlike the PHYSDEF hull-drop path which does warn.

**New finding, verified directly against the source rather than assumed from the audit: sleep never reduces pairwise cost.** `StepBody`'s own early-out — `if (b.asleep) return;` (`p_physics.cpp:810`) — skips a sleeping body's single-body integration and world-collision query. But the `O(M²)` pair loop that calls `SolvePair` (`p_physics.cpp:2021-2023`) has **no** sleep check at all — it is unconditional. Inside `SolvePair` itself, `.asleep` is read in exactly two places (`p_physics.cpp:1405-1406`), both only to *wake* a body on a hard-enough impact, never to skip resolution. So a resting pile of spent casings — the literal steady state after a firefight — pays full bounding-sphere broadphase cost, and for any pair packed within bounding-sphere reach of each other, full corner-in-hull narrowphase cost (`p_physics.cpp:1341-1382`), every fixed step, indefinitely. Sleep in this module makes a body's own motion cheap; it never makes a body cheap to be *near*.

Scale arithmetic, grounded in the audited mechanism (2 hands + up to 2 weapons + one `PhysBody` per `PhysicsEnable`'d actor, all-pairs, zero spatial structure):

| Concurrent bodies | Pairs/fixed-step | Pairs/sec @ 90Hz steady-state |
|---|---|---|
| 34 (2 hands, 2 weapons, 30 debris) | 561 | ~50k |
| 100 | 4,950 | ~445k |
| 300 (a level's worth, never despawned) | 44,850 | ~4M |

Thirty-odd bodies — one Trepang2-style room fight — is free. Hundreds of bodies — a level that accumulates casings and chunks across a sustained campaign with nothing despawning them — is where `O(M²)` enumeration, not the physics math itself, becomes the avoidable dominant cost, on a hot path that already shares the VR headset's 11ms/90Hz budget with everything else in `D_DoomLoop` (`d_main.cpp:1879-1898`). No despawn/pooling policy exists anywhere in the module; nothing bounds body count except a mod remembering to `Destroy()` its own debris.

The fix is a spatial broadphase (a uniform grid is the natural fit — every body already carries `pos`/`boundRadius` in metres, so bucketing needs no new per-body state). This is contained: it touches the wall-search loop (`p_physics.cpp:927-999`) and the pair-enumeration loop (`p_physics.cpp:2021-2023`) without touching `StepBody`'s integration, the hand/weapon/carry code, or the ZScript API. **Merely unbuilt**, medium size — but it's real engine surgery on a module that is 10 commits and ~32 hours old, with zero automated tests and zero in-repo callers to validate a refactor against. That's precisely the situation that calls for a real debris stress-test scene before sizing a grid cell, not a guess.

**Kickable debris specifically** (walk through a pile of casings and scatter them) is a Tier 1/Tier 2 crossover worth naming plainly: `SolvePair` only resolves `PhysBody`-vs-`PhysBody` contact, and the only `PhysBody` owners are hands, weapons, held objects, and `PhysicsEnable`'d actors — there is no `PhysBody` for the player's own torso or feet, because there is no player-body simulation at all (Tier 1's core finding). Debris can be swatted by a hand today; it cannot be kicked by walking into it, because the physics module that gives debris real momentum response never sees the player's body as a body. Adding one would reuse `UpdateHands`' existing kinematic-box construction pattern — merely unbuilt, but dependent on a locomotion-tracking decision this window never made.

#### Surface-type awareness: a generic data structure, a missing wire

`FTerrainTypeArray` (`src/gamedata/p_terrain.h:35-68`) is keyed by `FTextureID` — nothing about its data structure is floor-specific. But both of its call sites in the tree query a **sector's** flat texture (`p_sectors.cpp:970`, `p_3dmidtex.cpp:303`); a repo-wide check for any wall-texture-keyed lookup (`GetWallTerrain`, `WallTerrain`, `TerrainTypes[...]` against a `side_t` texture) returns nothing. Decal selection at a hitscan impact (`a_decals.cpp`) is driven entirely by the *weapon's* own DECAL name — never by what was actually hit. A round striking concrete, drywall, or a metal locker plays the identical decal, sound, and (if wired to the physics module) would spawn identical debris.

**This is merely unbuilt, not structurally blocked.** The lookup table is already texture-ID-generic; the TERRAIN lump format and DECORATE/ZScript binding convention already exist. Nothing prevents reading `TerrainTypes[]` for a wall hit at the same point `P_LineAttack`/`P_SpawnPuff` already resolves the impact, and threading that result into decal/sound/puff (and, downstream, debris-archetype) selection. What's missing is a small engine hook (a few dozen lines, mirroring the existing floor-splash call pattern) plus the actual content: TERRAIN/DECALDEF entries per material × per weapon caliber — real per-asset work whose volume, not difficulty, is the driver.

#### Reactive lighting and smoke: one narrow primitive, no general system

- **Shot-out lights**: pure content on an already-there baseline (`a_dynlight.cpp/.h`, zero diff this window) — a light-fixture actor with a damage-triggered state that kills or flickers its `DynamicLight`. Zero engine gap.
- **Degrading cover**: the already-there `p_destructible.cpp` health-group system is the exact mechanism, present and functional by inspection, unchanged this window, and — like PHYSDEF for the physics module — exercised by zero shipped content here. Same pattern repeats: the engine hook exists (years before this window, going by its copyright header), the content authoring against it does not.
- **Smoke lit by muzzle flash**: narrower than it sounds. `volumetricbeam.fp` (covered in the Shaders section) ray-marches density for exactly **one hardcoded source** — the flashlight — reading its own CVar-driven uniforms, not a light list. The `FogSlabAt`/`GlowTextureAt`/fog-disturbance system in `main.fp` is a screen-space glow/decal trick over up to 32 disturbance slots, not participating media any light can illuminate. Nothing in this tree lets an arbitrary transient light — a muzzle flash lasting one or two frames — illuminate a smoke volume the way Selaco's do. Merely unbuilt: extending the beam-march to accept a small dynamic light list instead of one hardcoded source is a real shader rewrite (per-step light accumulation against N sources inside an already-expensive per-fragment loop — a scoped-down forward-plus step), large, and squarely rendering-pipeline work independent of anything this window built.

#### AI: least engine-blocked, largest pure-authoring lift

Doom's monster AI (`A_Chase`/`A_Look` state-machine plus line-of-sight target acquisition, pre-existing and untouched by this window) is per-actor and stateless about neighbors — no squad concept, no cover graph, no blackboard, no goal-scored planning. F.E.A.R.-style squad behavior needs, at minimum: a cover-node graph with cached line-of-sight-to-threat scoring (Doom has sector/blockmap geometry and a raycast `P_CheckSight`, but no navmesh and no cover-node concept); a shared blackboard for squadmates to broadcast spotted/suppressed/flanking state (nothing like this exists, though the ZScript `EventHandler`/global-state machinery could carry it); and a planner selecting actions from scored world state rather than fixed transitions (also absent).

None of this is structurally blocked by this fork's rendering or physics work — ZScript fully owns `AActor::Tick`, `P_CheckSight` already exists, and a cover graph, blackboard, and planner are all buildable as ordinary ZScript/thinker systems with zero engine C++ changes. But it is the largest lift in this entire assessment: cover-node graphs are per-map content, a working squad planner is a multi-month AI-systems design effort independent of engine, and — unlike debris or decals — Selaco is weaker evidence this is "just content," because good squad AI is a hard design problem on any engine. Realistic framing: already there at the level of "the engine doesn't stop you," essentially unbuilt at the level of "a working system exists," and overwhelmingly design/authoring work rather than engine programming.

#### Bottom line

Selaco's floor is entirely already there in this engine, predates this window, and is completely unexercised by any shipped content in this repository — that gap is authorship, not capability. This window's own contribution to Tier 2, the rigid-body physics module, gives real debris physics that works today for anyone who calls it, but has a genuine, now-confirmed scaling cliff for a sustained, never-cleaned debris field — sleep cheapens a body's own motion but never its pairwise cost — and nothing owns a despawn budget. Wall-surface material awareness, muzzle-flash-lit smoke, and destructible cover are all merely-unbuilt wiring or content on top of primitives that already exist, sized small, large, and large respectively. Squad AI is the outlier: the least architecturally constrained of everything assessed here, and by far the largest actual undertaking.



---

### Cross-cutting bottlenecks and architectural risk

#### 1. Physics lives entirely outside the deterministic playsim tick — this is the substrate risk everything else sits on

`P_PhysicsFrame()` is called unconditionally from `D_DoomLoop` (`src/d_main.cpp:1897`), gated only by `ShouldStep()`'s check of `gamestate`/`paused`/`pauseext`/`menuactive` (`p_physics.cpp:745-752`). A grep of the whole file for `netgame|multiplayer|demoplayback|demorecording` returns zero hits — there is no gate of any kind distinguishing single-player, netgame, or demo record/playback. Three concrete consequences follow, not hypothetically but by direct code inspection:

- **Save/load silently freezes physics actors.** `P_PhysicsLevelStart`/`P_PhysicsLevelEnd` unconditionally `g_bodies.Clear()` (`p_physics.cpp:2058-2082`) with no scan that re-registers actors still carrying `MF9_PHYSICSBODY`. `PhysicsEnable` has zero callers anywhere in this repository (confirmed by grep against `wadsrc/`), so nothing re-populates `g_bodies` for a persisted actor after a save/load or hub transition. Meanwhile `P_MobjThinker`'s `MF9_PHYSICSBODY` branch (`p_mobj.cpp:4607-4626`) permanently skips `P_XYMovement`/`P_ZMovement`/gravity for any actor with the flag set, flag or no body. The result: any actor a mod flags physical, then the player saves and reloads mid-session, becomes a permanently inert prop — no gravity, no collision, no movement — with no error, no log line, and no native the script side can call to detect the orphaned state (`PhysicsIsHeld`/`PhysicsIsAsleep` just return their default-false values for an unregistered body rather than failing). This is not an edge case for Tier 1 or Tier 2 — both explicitly want physics objects (dropped weapon parts, kicked debris) to persist through ordinary play, and ordinary play includes saving mid-firefight.
- **Demo playback cannot reproduce a physics actor's trajectory.** `WriteBack` (`p_physics.cpp:1827-1857`) is the only place a physics body's transform reaches the actor, and it runs once per `P_PhysicsFrame` call, itself paced by `I_nsTime()` wall-clock delta, not by tic count. Demo record/playback assumes `P_Ticker`'s tic-driven simulation reproduces identical positions from identical ticcmds; a physics-flagged actor's position instead depends on the real frame timing of whichever session is running, which record and playback will not match.
- **Netgame has no representation for remote hands and no synchronization for shared physics actors.** `UpdateHands` and `UpdateWeapons` both open with `player_t *pl = &players[consoleplayer];` (`p_physics.cpp:1546-1547`, `1753-1754`) — hand and weapon `PhysBody`s exist only for the local viewing client. A remote player's hands and held weapon are invisible to this solver on every other client; there is no code path that builds a `PhysBody` for `players[i]`, `i != consoleplayer`. For an actor-owned body that *is* a shared game object (a thrown magazine, a dropped weapon), `P_PhysicsFrame` runs independently on every connected client, driven by that client's own clock and its own `vr_physics_hz`/`vr_physics_maxsteps` (both plain `CVAR_ARCHIVE|CVAR_GLOBALCONFIG` — user-settable per client, not server-forced, `p_physics.cpp:77-87`). Nothing in the file exchanges position, velocity, or sleep state between clients. Two clients' views of the same physics-driven actor will diverge from frame one, with no resync mechanism, and the physics system's writes to that actor's transform have no defined relationship to whatever the ordinary netcode actor-sync path is doing to the same actor.

**Classification: merely unbuilt, not structurally blocked.** Nothing in the solver's math requires frame-rate stepping to be undoable — the save/load gap specifically just needs `P_PhysicsLevelStart` (or an equivalent load hook) to re-scan actors for `bPhysicsBody` and re-derive a body from stored parameters (which themselves currently aren't stored on the actor either — `PhysicsEnable`'s mass/half-extent/COM arguments are consumed once and discarded, `p_physics.cpp:2142-2176` — so this item also needs the actor to remember its own enable parameters). This is the item that must be fixed first, ahead of both tiers, because every additional physics-enabled object either tier adds (Tier 1's held/dropped parts, Tier 2's persistent debris) is one more object that silently breaks the moment a save happens.

#### 2. O(M²) broadphase with no sleep pruning, directly opposed to Tier 2's debris ask

The body-vs-body pass is unconditional all-pairs every fixed step (`p_physics.cpp:2021-2023`), and `SolvePair` only *wakes* a sleeping body on hard contact (`p_physics.cpp:1405-1406`) — the enclosing double loop never skips a pair where both sides are already asleep. `g_bodies` has no cap, no pool, and no eviction beyond `PhysicsDisable`/`OnDestroy`, so a Selaco/Trepang2-style debris field (shell casings that never despawn, per Tier 2's own reference set) only grows.

Concrete numbers: at M=200 resting bodies (2 hands + 2 weapons + ~196 shells/parts, a realistic outcome of one sustained firefight with no cleanup), the pair loop runs 200×199/2 = 19,900 bounding-sphere tests **every fixed step**, forever, even though every one of those 200 bodies is asleep and motionless. At the default `vr_physics_hz`=90 that's ~1.8M broadphase tests/sec of pure bookkeeping for a pile nobody is touching; at the CVar's clamped ceiling of 240Hz it's ~4.8M/sec. This cost is paid at *frame* rate, not the 35Hz the rest of the engine's per-tic systems use, so identical body counts cost roughly 2.6–6.9x more absolute CPU/sec here than an equivalent tic-rate system. (Exact affordability depends on hardware and per-test cost, which I have not benchmarked — the point is the growth curve is quadratic and unthrottled by activity, not the specific millisecond figure.)

This compounds with a second, separate finding: `AActor::OnDestroy` calls `P_PhysicsRemoveBody(this)` **unconditionally for every actor destroyed in the game**, physics-enabled or not (`p_mobj.cpp:5992`, confirmed unconditional both in the audit and by direct re-read) — a linear scan over the current `g_bodies` (`p_physics.cpp:2084-2097`). With 200 resting shells in `g_bodies`, every ordinary hitscan puff, gib, or corpse destroyed anywhere else in the level now pays an extra ~200-entry scan it would not have paid without the debris field. The more successful Tier 2's "persistent kickable debris" is, the slower unrelated, unconnected actor churn becomes engine-wide — a coupling that will not show up in testing with a handful of physics objects and will only appear once a real firefight's worth of debris has accumulated.

**Classification: merely unbuilt.** A spatial grid or per-sector body list (serving both body-body and the existing "own sector" world-collision query) plus an asleep-pair skip removes both costs and fits cleanly on the existing per-pair contact code; it does not require reworking the contact solver itself.

#### 3. No constraint/joint representation — the largest missing primitive for Tier 1

A case-insensitive search of `p_physics.cpp`/`p_physics.h` for `constraint|joint|hinge|prismatic|ragdoll|motor` returns zero matches (independently reconfirmed, matching the audit). The only mechanisms present are free-body contact resolution against world geometry, free-body contact resolution against other bodies, and the hand-carry pose copy (a direct transform assignment, not a solved constraint). There is also no contact caching or warm-starting of any kind — every step's contacts are recomputed from scratch (no per-contact ID field exists on `PhysBody` or in the pair-contact struct) — which is adequate for the current single-contact-per-frame use case but will show as instability under any real constraint or resting-stack load.

This is the ceiling for Tier 1's Pavlov-axis fidelity: a charging handle pulled along a constrained track, a magazine retained in a magwell rather than teleported, a hinged floorplate, two-hand grip that actually resists twist — every one of these needs a joint primitive this module does not have. It is also the prerequisite for any future ragdoll or ledge-grab work implied by Boneworks-tier body simulation.

**Classification: merely unbuilt, not structurally blocked.** The existing sequential-impulse/Baumgarte contact solver is the same family of architecture joint solvers are conventionally layered onto (this is the standard Box2D/Bullet pattern); nothing here forecloses it. It is, however, large: a real implementation needs constraint types (hinge, prismatic, distance, point), a constraint graph or per-body constraint list, warm-starting for stability, and integration ordering against the existing contact pass — plausibly comparable in scope to the ~1,400 lines the existing contact solver (`StepBody`+`SolvePair`) already represents.

#### 4. Raw `AActor*` registry, no GC visibility — correct today, fragile by construction

`g_bodies` holds raw `AActor*` with zero `GC::Mark`/root registration anywhere in the file (reconfirmed by grep). The sole safety net is the single, unconditional call from `AActor::OnDestroy` (`p_mobj.cpp:5992`), and `FindBody` does bare pointer-equality with no secondary validity check (`p_physics.cpp:754-759`). This is currently sound because it rests on one disciplined, unconditional call site matching the engine's normal `DObject` destruction contract — but it means the invariant "no `PhysBody` ever outlives its actor" has exactly one thing holding it up, forever, with no independent check inside the physics module itself. Any future code path that reclaims `AActor` memory without routing through `OnDestroy` (a pooling/recycling scheme, a reordering bug in the destroy sequence) turns `g_bodies` into a dangling-pointer array with nothing to catch it. Rank this below items 1–3: it is not misbehaving today, and fixing it (a validity token, or migrating the registry to weak references) is worth doing but is not urgent relative to the items that are actively wrong.

#### 5. Weapon-family detection is a single hardcoded substring

`IsPhysicalWeapon` (`p_physics.cpp:1743-1749`) lowercases the class `TypeName` and checks `IndexOf("t77") >= 0` — the only gate deciding which weapon, of any that might exist in a loaded mod, ever gets a weapon-shaped collider at all. Every other weapon participates in physics only via the generic hand-box collider. This single-cases exactly the kind of special-case the task asks to flag: it is small to fix mechanically (a flag or class-list lookup replaces the substring test) but it currently caps Tier 1's per-weapon fidelity work at one hardcoded gun, and extending physical-weapon support to anything else must go through this function first.

#### 6. Maintaining three VR backends is a liability for interaction work specifically — the roadmap should commit to one

Beyond what the raw audit already established (GL+OpenXR is Android-only dead code on desktop; OpenVR's axis remap has no post-5.0 replacement), direct verification here shows something sharper: `GripClaimMain/Off`, `GripSubjectMain/Off`, `FingerTouchMain/Off`, and `TwoHandedHold` — the entire new grip-arbitration and capacitive-touch data model this window built — are written **exclusively** by `vk_openxrdevice.cpp`. A repo-wide grep for those four symbol names outside `hw_vrmodes.cpp` (field pass-through only), `actor.h` (declarations), and `vmthunks_actors.cpp` (script bindings) finds nothing in `i_openVR.cpp` or `gl_openvr.cpp` — the OpenVR/SteamVR backend never touches any of them. Combined with the physics module's own d_main.cpp comment noting `vid_preferbackend` defaults to OpenGL and silently falls back to it on Vulkan init failure, this means a session that lands on GL+OpenVR (a real, reachable configuration, not a hypothetical) runs with **zero** grip-context arbitration, zero capacitive touch, and zero two-handed detection — every one of those fields silently reads its zero/default value forever, with no error and no log line distinguishing "not supported here" from "nothing is gripping anything."

Given the user's explicit context (no distribution constraints, no compatibility requirement, engine changes unrestricted), the correct move is not to port this work to the other backends — it is to stop pretending they're equivalent. Fail loud (a startup check that refuses or clearly warns when the resolved backend isn't Vulkan+OpenXR) rather than letting a misconfigured or fallback session ship a silently degraded interaction model. This is a policy/guard change, not new engineering, and it removes a maintenance trap for all future interaction work rather than adding one.

#### Ranking — what endangers the goal, and what must be fixed first

1. **Save/load and cross-session persistence of physics bodies (§1)** — must be fixed first, regardless of which tier is prioritized. It is the only item on this list that actively corrupts existing behavior (silent, permanent freezing of a persisted physics actor) rather than merely capping how far a feature can go. Every physics object either tier adds is one more object that hits this the moment a player saves.
2. **Broadphase scaling and the OnDestroy tax (§2)** — ranked second because it is the item most directly opposed to Tier 2's specific ask (debris that persists and accumulates), and it degrades silently: nothing will look wrong in early testing with a handful of objects, and the cost only becomes visible once a level has accumulated the debris count Tier 2 wants by design.
3. **Constraint/joint absence (§3)** — ranked third: it caps Tier 1's ceiling but does not corrupt anything that already works, so meaningful free-body Tier-1-lite interaction (grab/throw/collide) can ship before this exists. It is, however, the largest single build item on this list.
4. **VR backend commitment (§6)** — ranked fourth: doesn't block either tier's development on a correctly-configured dev machine, but left unaddressed it is a standing silent-degradation trap and a source of wasted future porting effort if anyone assumes backend parity that doesn't exist.
5. **GC/raw-pointer registry fragility (§4)** — ranked lowest: correct today, worth hardening, not urgent.
6. **Weapon substring gate (§5)** — small and mechanical; bundle into whatever work first extends physical-weapon support past the current single hardcoded family, not urgent standalone.



## 11. Path forward — ZDoom VR with high-fidelity guns

Goal, as stated: **ZDoom VR featuring high-fidelity guns and gunplay, built in this engine.**
Cost is not a constraint. Nothing below is scoped down for budget; where something is deferred
it is deferred because building it earlier would waste the work, not because it is expensive.

This sequence was produced by synthesis across five assessments and then adversarially
critiqued against source. **The critique found five real defects in the first draft** — they are
corrected here and flagged inline, because two of them would have cost days.

### Decision points, and the answers

| | Decision | Answer |
|---|---|---|
| **DP1** | Alyx-rigid hands, or Boneworks-lite force-driven hands? | **Force-driven.** Self-contained in `p_physics.cpp`. Not full Boneworks — see below. |
| **DP2** | Build a constraint solver at all? | **Yes.** It gates every weapon-fidelity feature. Scope it to point/distance first and prove it on two-handed grip. |
| **DP3** | Support all three VR backends? | **No — commit to Vulkan + OpenXR and fail loud.** `GripClaim*`, `GripSubject*`, `FingerTouch*` and `TwoHandedHold` are written *exclusively* by `vk_openxrdevice.cpp`. A session resolving to GL+OpenVR gets zero grip arbitration, silently. |

### Phase 0 — prerequisites the first draft missed

**These are not optional and nothing in Phase B or C works without them.**

**P0.1 — Gate hand/weapon bodies on `IsVR()`.** *small*
There is no VR check anywhere in `P_PhysicsFrame`, `UpdateHands`, `UpdateWeapons` or
`IsPhysicalWeapon`. `ShouldStep()` ([p_physics.cpp:745](src/playsim/p_physics.cpp:745)) tests
gamestate/pause/menu only, and `UpdateHands` runs whenever `pawn != nullptr`
([:1555](src/playsim/p_physics.cpp:1555)) using `AttackPos`, which has a defined flatscreen
fallback (`hw_vrmodes.cpp:1458`). **An invisible kinematic hand collider already rides the
flatscreen player's muzzle height in every mod today.** It is harmless only because nothing
calls `PhysicsEnable` yet. The moment wall-clamping and debris land, a flatscreen player gets a
phantom mass-bearing collider on their crosshair. You play non-VR mods on this build — fix this
first.

**P0.2 — Give hand and weapon bodies a sector source.** *medium–large*
`StepBody` opens with `AActor *a = b.owner; if (a == nullptr) return;`
([:808](src/playsim/p_physics.cpp:808)) — *before* the `kinematic` check at
[:814](src/playsim/p_physics.cpp:814) — then immediately does `sector_t *sec = a->Sector;`.
Hands ([:1577](src/playsim/p_physics.cpp:1577)) and weapon bodies
([:1785](src/playsim/p_physics.cpp:1785)) are both created `owner = nullptr` by design.

**Consequence: flipping `kinematic = false` on a hand does nothing at all.** Execution returns
at line 809 and never reaches gravity, contacts or CCD. Every "integrate the hand through
`StepBody`'s existing pipeline" plan is false until this is fixed — either synthesize an owner,
or restructure the sector lookup into a point-in-sector query on `b.pos`. This is the single
highest-leverage unblocking item in the entire list.

### Phase A — the shared floor: hands and held objects that stop

**A1 — Commit to Vulkan + OpenXR, fail loud otherwise.** *small*, deps: none
Hook at `VRMode::GetVRMode()` (`hw_vrmodes.cpp:1135`), **not** `i_main.cpp` — that file's whole
diff this window is an unrelated crash-debug shim.

**A2 — Hand-vs-geometry clamp, with velocity derived from the clamped delta.** *medium*, deps: P0.2
The world-contact machinery at [p_physics.cpp:806-1001](src/playsim/p_physics.cpp:806) already
handles arbitrary hulls, slopes and moving floors. It never runs for a hand only because of the
ownerless/kinematic gates.

> **Corrected from the first draft.** It is not enough to clamp before the `b->pos = newPos`
> write at [:1672](src/playsim/p_physics.cpp:1672). `b->vel` is differenced from the *unclamped*
> tracked pose at [:1623-1652](src/playsim/p_physics.cpp:1623) — **earlier in the same
> function**. Clamp only at the write and the hand visibly stops at the wall while still
> reporting full tracking speed into `SolvePair` ([:1372](src/playsim/p_physics.cpp:1372)),
> so anything it touches gets shoved as if the wall weren't there. Clamp first, then derive
> velocity from the clamped delta.

Keep `AttackPos`/`OffhandPos` themselves unclamped — they still feed hitscan.

**A3 — Draw the hand from the resolved pose.** *large*, deps: A2
This is what makes A2 visible. Today the render path never reads physics output in either
direction. The integration point is `RenderHUDModel` ([models.cpp:584](src/r_data/models.cpp:584)),
which is already psprite-scoped — **not** the generic `ObjectToWorldMatrix`, so the blast radius
is far smaller than first assessed.

> **Two cautions.** The hand-body comment at [:1539](src/playsim/p_physics.cpp:1539) argues
> hands deliberately *don't* collide because a blocked hand "would either stop tracking your
> real hand or fight it." That's an author's claim, not a measured result — but it names a real
> comfort risk, and the answer is the standard one: a soft offset with a ghost hand at the
> tracked pose, not a rigid clamp. Second: the HUD bone-anchoring code cited as precedent for
> substituting a transform is the same mechanism recorded as broken after five prior fix
> attempts. Treat it as a warning, not a proof of feasibility.

**A4 — Held objects on a spring, replacing the rigid pin.** *medium*, **not parallel with A2**
> **Corrected.** The rigid pin is *not* the `kinematic` flag. It is the carry loop at
> [p_physics.cpp:1677-1719](src/playsim/p_physics.cpp:1677), whose only guard is
> `if (h.heldByHand < 0) continue;` — it never checks `kinematic` at all and overwrites
> `h.pos`/`h.rot` unconditionally. Flipping the flag alone leaves the object exactly as pinned.
> A2 and A4 rewrite the same ~150-line region of `UpdateHands`; they need one coordinated
> design pass, not two independent tickets.

Also re-validate the peak-of-swing throw search at [:2443](src/playsim/p_physics.cpp:2443),
which assumes held velocity is a direct copy of the hand's. A spring-followed body has genuine
independent velocity.

**A5 — Save/load persistence and registry hygiene.** *medium*
`P_PhysicsLevelStart/End` clear `g_bodies` with no rescan on load, and `PhysicsEnable`'s
parameters aren't stored to rebuild from. Latent today because `PhysicsEnable` has **zero
callers**; it bites the moment debris content exists.

> **Drop one item from this bundle.** Gating `AActor::OnDestroy`'s registry scan on the actor's
> own flag is *dangerous*: `bPhysicsBody` is independently script-settable
> (`thingdef_data.cpp:344`), so a script clearing the flag without calling `PhysicsDisable`
> leaves a stale entry the gated scan would miss — the exact dangling-pointer case the
> unconditional scan at `p_mobj.cpp:5992` exists to prevent.

**A6 — Broadphase.** *medium*
`SolvePair`'s pair loop is unconditional all-pairs every fixed step; `asleep` is read only to
*wake* a body, never to skip one. ~200 resting bodies is ~1.8M tests/sec at 90 Hz for a pile
nobody is touching. Uniform grid + sleep-aware culling + surfacing the currently-silent
`kMaxContacts=96` / `kMaxPair=48` overflows.

### Phase B — force authority in the hand

**B1 — Finite-mass PD-driven hands.** *large*, deps: **P0.2**, A2, A3
Real `invMass`/`invInertia` (~0.3–0.6 kg), spring/torque toward the tracked pose. Accept some
give against stiff geometry: explicit integration at 90 Hz with 8 solver iterations
(`kSolverIterations=8`) has a hard stiffness ceiling. Boneworks has the same symptom; it is
inherent, not a bug to close.

**B2 — Grab as a breakaway spring.** *medium*, deps: B1
Force-limited release threshold. Nothing like it exists today — a grab never auto-releases.

### Phase C — weapon-part fidelity, the actual guns

**C1 — Kill the `"t77"` substring match.** *small*, deps: none — **do this early, it is trivial**
[`IsPhysicalWeapon`](src/playsim/p_physics.cpp:1743) is `IndexOf("t77")` on the lowercased class
name, and it is the sole gate on any weapon getting a weapon-shaped collider. Meanwhile
`ApplyPhysDefShape` — a working per-class hull loader — has exactly **one** caller,
`PhysicsEnable` ([:2185](src/playsim/p_physics.cpp:2185)), which the held weapon never reaches.
So a dropped magazine collides as its real mesh and the gun in your hand collides as a cvar box.
Replace the name test with PHYSDEF-presence, and route the weapon body through the same loader.

**C2 — Per-part weapon PHYSDEF content, plus attach points.** *large*, deps: C1
`PhysHull` is **already** a compound-convex representation that supports concavity — a magwell
built from four convex slabs, with `SolvePair` testing every vertex of A against every hull of B
so a gap behaves as a gap. That substrate works today. What is missing is content: this repo
ships **zero** PHYSDEF lumps, which is why every collider is a box. The grammar (`Body`/`Hull`/
`V`/`P`) needs named attach points and axes added for constraints to bind to.

**C3 — Constraint solver core, point/distance first.** *very large*, deps: none structurally
Zero joint code exists. New `PhysJoint` pass slotted between `StepBody` and `SolvePair`, reusing
the existing sequential-impulse/Baumgarte math generalized to constrained axes. Include wake
propagation across joints — today waking is pair-local.
*(For budgeting: the existing contact solver is ~850–900 lines, not the ~1,400 first claimed.)*

**C4 — Two-handed grip as a real constraint.** *medium*, deps: C3, **P0.2**
Today `TwoHandedHold` is cosmetic: it is never read anywhere in `p_physics.cpp`, and
`weaponStabilised` only recomputes render-time aim angles from an `atan2` on hand positions. The
off hand cannot push, twist, or be resisted. **This is the first visible payoff of the solver.**

**C5 — Magazine insertion: alignment plus snap volume.** *large*, deps: C2 + C3
The alignment test reuses `HullDeepest` almost free. The new part is a soft point-constraint
that engages once alignment and depth preconditions are met. Contact alone can already stop a
crooked magazine; it cannot pull an aligned one home and hold it.

**C6 — Prismatic slider: charging handle, slide, bolt.** *large*, deps: C2 + C3 — travel, limits, detents.
**C7 — Hinge: selector, floorplate, folding stock.** *medium*, deps: C3.
**C8 — Separated parts inherit linear + angular velocity.** *small*, deps: C3 — reuses the existing `Cross(angVel, r)` throw pattern.
**C9 — Positional chamber/feed state.** *large*, deps: C5 + C6 — **sequence last.** It is a `weapons.zs` state-machine problem as much as a physics one; building it before the part feel is proven means building it twice.

### Phase D — gunplay reacting to the room

**D1** shell casings and debris on the existing `Physics*` API (complete, zero callers today) —
deps A5, A6. **D2** player-foot body so you kick casings. **D3** wall-surface material lookup:
`FTerrainTypeArray` is already texture-ID-generic but both call sites only ever query a sector's
flat, never a wall — a small hook at the hitscan impact point gives concrete-vs-metal response
immediately. **D4/D5** per-material and destructible-cover content; `p_destructible.cpp`'s
health-group system has been present since 2018 and is exercised by zero content. **D6** 3D
floors and adjacent-sector lines in the world test — today it searches the body's own sector
only, which is fine for a magazine and wrong for a rifle. **D7** warm-started contact IDs so
debris piles read as settled.

### Supplemental — off the critical path, multiplies the result

- **Muzzle-flash-lit smoke** — generalize `volumetricbeam.fp`'s single hardcoded source to a
  small dynamic light list. Independent of everything above.
- **Squad AI** — needs zero engine C++; `AActor::Tick` and `P_CheckSight` already expose enough.
  It is the largest pure-authoring lift here and the wrong thing to start early.

### Do not do this

- **Full Boneworks player body** (force-coupled locomotion, climbing, ragdoll). Not a cost
  objection — a sequencing one. It needs a force accumulator drained into `P_XYMovement`/
  `P_ZMovement`, files with *zero* diff this window, plus a multi-segment player model the
  single-cylinder mover has no slot for. It depends on the constraint solver *and* virgin
  player-mover work, and `p_user.cpp` has no VR-only branch — it is the one place flatscreen
  regression becomes direct. Revisit after Phase C lands.
- **Porting grip/touch/two-handed to OpenVR/GL.** Fail loud instead.
- **Four constraint types before one is proven in a headset.** Ship C4, then extend.
- **Squad AI while hands still pass through walls.**

---

## 12. Current working state

Three files are modified in the working tree and **not** part of the committed
`1d2572bdcc..main` scope the rest of this document covers. Two of them post-date the audit.
They are recorded here because uncommitted work is invisible to a commit-range diff and would
otherwise leave a hole in the record.

### `src/scripting/vmthunks.cpp` + `wadsrc/static/zscript/doombase.zs`

One new native on `FLevelLocals`, plus its ZScript declaration:

```
native class<Actor> GetActorModelClass(Actor act);
```

It answers *which class's MODELDEF a live actor instance actually resolves against right now*,
as distinct from what its own type is:

```cpp
if (act->modelData != nullptr && act->modelData->modelDef != nullptr)
    return act->modelData->modelDef;
return act->GetClass();
```

**Verified:** this mirrors `FindModelFrame(AActor*)`'s own fallback exactly — the ternary at
[models.cpp:2163](src/r_data/models.cpp:2163) is
`(thing->modelData && thing->modelData->modelDef) ? thing->modelData->modelDef : thing->GetClass()`.
Read-only; it changes nothing about how anything renders.

**Why it exists.** `A_ChangeModel` sets `modelData->modelDef` on the *instance* it is called on.
A mod that model-swaps that way — ModelSwapper is the motivating case, pointing a flat-sprite
weapon's psprite at a donor class's model — never registers a MODELDEF entry under the weapon's
own class name. So any caller doing a class-name-keyed lookup finds nothing for exactly that
weapon, because the model only ever lived on the instance. Every `GetModel*Hint` native above it
in the same file has that blind spot; this one does not.

Relevant to §9: it is the same class of problem as the `GetLocalExtent` gap — a model-identity
question that class-keyed lookup cannot answer.

### `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`

Predates this session and belongs to a different lane sharing this engine tree. `OpenWheel`
gains an early-out reading a **mod-owned** cvar:

```cpp
if (FBaseCVar *suppress = FindCVar("wr_suppress_native_wheel", nullptr))
{
    UCVarValue v = suppress->GetGenericRep(CVAR_Bool);
    if (v.Bool) return;
}
```

The engine *reads* a cvar the mod declares, rather than declaring one for the mod to write.
Absent means enabled, so a session with no such mod loaded behaves exactly as before. Looked up
per call rather than cached, since a pk3 can create the cvar long after this translation unit's
statics initialise.

---

## 13. Appendix — all 80 changed files

| File | Lines |
|---|---|
| `BILLBOARDS.md` | +20 / −7 |
| `CHANGES.md` | +971 / −0 |
| `FORK_CHANGES.md` | +1007 / −8 |
| `README.md` | +106 / −9 |
| `src/CMakeLists.txt` | +1 / −0 |
| `src/common/engine/multiplayerlaunch.cpp` | +8 / −1 |
| `src/common/menu/resolutionmenu.cpp` | +10 / −1 |
| `src/common/models/model.h` | +44 / −0 |
| `src/common/models/model_md2.h` | +9 / −0 |
| `src/common/models/model_md3.h` | +12 / −0 |
| `src/common/models/model_obj.h` | +1 / −0 |
| `src/common/models/models_iqm.cpp` | +29 / −0 |
| `src/common/models/models_md2.cpp` | +43 / −0 |
| `src/common/models/models_md3.cpp` | +28 / −0 |
| `src/common/models/models_obj.cpp` | +28 / −0 |
| `src/common/platform/win32/i_main.cpp` | +68 / −0 |
| `src/common/platform/win32/i_openXR.cpp` | +126 / −32 |
| `src/common/rendering/gl/gl_shader.cpp` | +12 / −2 |
| `src/common/rendering/hwrenderer/data/hw_clock.cpp` | +6 / −1 |
| `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` | +53 / −5 |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` | +99 / −7 |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.h` | +1 / −1 |
| `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp` | +25 / −12 |
| `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h` | +20 / −0 |
| `src/common/rendering/hwrenderer/postprocessing/hw_postprocess_cvars.h` | +1 / −0 |
| `src/common/rendering/vulkan/renderer/vk_postprocess.cpp` | +20 / −2 |
| `src/common/rendering/vulkan/shaders/vk_shader.cpp` | +19 / −2 |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp` | +206 / −4 |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h` | +70 / −0 |
| `src/common/rendering/vulkan/system/vk_renderdevice.cpp` | +1 / −0 |
| `src/common/scripting/frontend/zcc_parser.cpp` | +10 / −0 |
| `src/common/scripting/interface/vmnatives.cpp` | +0 / −1 |
| `src/d_buttons.h` | +2 / −2 |
| `src/d_event.h` | +11 / −3 |
| `src/d_main.cpp` | +99 / −4 |
| `src/d_netinfo.cpp` | +13 / −1 |
| `src/g_game.cpp` | +45 / −21 |
| `src/g_levellocals.h` | +63 / −4 |
| `src/maploader/maploader.cpp` | +6 / −0 |
| `src/menu/doommenu.cpp` | +10 / −2 |
| `src/menu/profiledef.cpp` | +25 / −11 |
| `src/p_setup.cpp` | +7 / −0 |
| `src/playsim/actor.h` | +32 / −0 |
| `src/playsim/p_actionfunctions.cpp` | +30 / −0 |
| `src/playsim/p_mobj.cpp` | +33 / −1 |
| `src/playsim/p_physics.cpp` | +2555 / −0 |
| `src/playsim/p_physics.h` | +61 / −0 |
| `src/playsim/p_pspr.cpp` | +38 / −4 |
| `src/playsim/p_pspr.h` | +87 / −0 |
| `src/r_data/models.cpp` | +757 / −27 |
| `src/r_data/models.h` | +14 / −0 |
| `src/rendering/gl/stereo3d/gl_openvr.cpp` | +2 / −0 |
| `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` | +110 / −4 |
| `src/rendering/hwrenderer/scene/hw_weapon.cpp` | +123 / −15 |
| `src/rendering/r_utility.cpp` | +4 / −0 |
| `src/scripting/thingdef_data.cpp` | +1 / −0 |
| `src/scripting/vmthunks.cpp` | +260 / −5 |
| `src/scripting/vmthunks_actors.cpp` | +7 / −0 |
| `src/scripting/zscript/zcc-parse.c` | +0 / −6652 |
| `src/scripting/zscript/zcc-parse.h` | +0 / −161 |
| `src/scripting/zscript/zcc-parse.out` | +0 / −21870 |
| `src/win32/i_openVR.cpp` | +168 / −29 |
| `vcpkg.json` | +47 / −0 |
| `wadsrc/CMakeLists.txt` | +13 / −7 |
| `wadsrc/static/language.0` | +2 / −16 |
| `wadsrc/static/language.1` | +6 / −0 |
| `wadsrc/static/language.csv` | +0 / −46 |
| `wadsrc/static/menudef.txt` | +71 / −11 |
| `wadsrc/static/shaders/glsl/main.fp` | +307 / −10 |
| `wadsrc/static/shaders/pp/present.fp` | +16 / −3 |
| `wadsrc/static/shaders/pp/volumetricbeam.fp` | +18 / −0 |
| `wadsrc/static/zscript/actors/actor.zs` | +91 / −0 |
| `wadsrc/static/zscript/actors/inventory/weapons.zs` | +36 / −3 |
| `wadsrc/static/zscript/actors/player/player.zs` | +119 / −1 |
| `wadsrc/static/zscript/constants.zs` | +37 / −3 |
| `wadsrc/static/zscript/doombase.zs` | +81 / −1 |
| `wadsrc/static/zscript/engine/base.zs` | +0 / −1 |
| `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs` | +12 / −38 |
| `wadsrc/static/zscript/ui/statusbar/alt_hud.zs` | +5 / −0 |
| `wadsrc_extra/static/language.csv` | +0 / −48 |
