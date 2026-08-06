# Engine work in this fork — what was added, and what state it is really in

**Doubt this file.** It is a summary written on 2026-08-06 and it can go
stale. The source, the compiler and the running game are the authorities.
When it matters, read the code. When it is a question of scope or intent,
**ask the owner** — not this file, not a handoff, not another agent.

Base: fresh clone of `emawind84/gzdoom`, branch `questzdoom`, 2026-08-06.
Remote: `rsorigin` → `github.com/presidentkoopa/UZDXREMA` (private).
`origin` is upstream and **cannot be pushed to**.

**NOTHING BELOW HAS BEEN LOOKED AT ON SCREEN.** Everything compiles, links
and boots. That is the entirety of the evidence. This project's own rules
record repeated cases of "compiles and boots" meaning "consistent with
itself" rather than "correct", and this session produced two more.

---

## 0. The startup crash (fixed — read this first if it returns)

`ParseLanguageCSV` indexed out of bounds on two short rows. Fixed by
appending **one comma each** to `wadsrc/static/language.0` and
`language.csv` (`JOYMNU_INVERTDIGITALAXISBUTTONS` 30→31 cells,
`CMPTMNU_OLDRANDOM` 29→30). Commit `3469846916`.

**A data fix, not a code fix** — the bounds-check version was reverted on
the owner's order. Any branch cut before that commit dies at `W_Init` and
cannot boot. Three lanes spent a day building against a dead binary because
of it.

---

## 1. Wall texture glow — WORKS, DEFAULT ON

GLDEFS has always parsed a `Glow { Walls { } }` block and **no renderer ever
consumed it.** Now one does.

* `gl_texture_wallglow` (bool, default **true**)
* `gl_texture_wallglow_intensity` (float, default 1.0)

Additive on top of the existing `uGlowTop/BottomColor` path — it does not
replace sector glow, it adds beside it.

**Known rough edge:** with the owner's GLDEFS this is near-fullbright,
because his file uses bare texture names with no `intensity` keyword and
bare names default to 100. His ~190 waterfall frames probably want ~40 with
the 11 fire textures near 100. **One-line GLDEFS edit on the mod side, no
engine change.**

**Not wired:** the level-mesh / lightmap path. Wall glow does not route
through `RenderTexturedWall`, so baked lighting does not see it.

---

## 2. Flat edge glow (floors and ceilings) — DEFAULT **OFF**

Floors and ceilings glowing inward from their edges. `gl_flatglow` is
**false by default**, so a player sees nothing until they turn it on.

Cvars: `gl_flatglow`, `_floor`, `_ceiling`, `_reach` (32), `_intensity`,
`_color`, `_colormode` (0–2), `_cap`, `_edges`, `_shape` (0–6),
`_sharpness`, `_inset`, `_bandwidth`, `_spacing`, `_pulse`, `_pulse_depth`.

`colormode 0` (default) takes a flat global colour and never touches sector
glow. `colormode 1/2` resolve through `Sector::GetWallGlow`, so a nukage
floor's edge glow comes out green.

### ⚠ CROSS-FILE INVARIANT — this one is a landmine

`SetupFlatGlow` (`hw_flats.cpp`) declares `float top[4], bottom[4]`
**uninitialised**, calls `GetWallGlow`, then guards on `seam[3] > 0`.

That is safe **only** because `ResolvePlaneGlow` (`p_sectors.cpp:1208`)
writes `glowdata[3] = 0` as its **first statement, ahead of every early
return.** Add an early return above that line and you get an intermittent,
hardware-dependent wrong colour — a miserable bug to chase. Documented at
the call site in commit `d08105b5d6`. `CheckSpriteGlow` relies on the same
guarantee.

### Cost, unmitigated

`FFlatVertex` grew 32→40 bytes. `FFlatVertexBuffer::BUFFER_SIZE` is a fixed
**2,000,000-vertex preallocation** — 80 MB per pipeline buffer, ~320 MB
across four on GLES. Packing the baked distances to 12+12 bits saves ~32 MB
and costs precision; **halving BUFFER_SIZE saves ~160 MB and costs nothing.**
Nobody has measured actual peak flat-vertex use on a heavy map, which is the
number that decides it.

Also: every subsector carries two extra triangles whether glow is on or off,
and visible-wall distances are baked at map load — a lift that moves later
does not move the seam it was baked from.

---

## 3. ZScript glow API

`Sector.SetGlowColorAuto`, `Sector.IsGlowAuthored`, `Sector.GetTextureGlow`,
`TexMan.GetAverageColor`. `ResolvePlaneGlow` is the single resolution point;
no consumer outside `p_sectors.cpp` reads `planes[].GlowColor` directly, so
an auto-painted colour cannot leak past it into a renderer.

**Deprecation flags are RENAMES, not removals** (source of truth is
`src/scripting/thingdef_properties.cpp`): `+DONTHURTSPECIES` →
`+DONTHARMCLASS`, `+LOWGRAVITY` → `Gravity 0.125`, `+SHORTMISSILERANGE` →
`MaxTargetRange 896`. `MISSILEMORE` / `MISSILEEVENMORE` /
`SHORTMISSILERANGE` **cannot be fixed** — they set native fields with no
`Property` binding, so the deprecated flag is the only declarative way to
set them. ~256 permanent warnings. Stop trying.

---

## 4. Billboards / in-world panels — **PARTIAL. READ THIS BEFORE BUILDING ON IT.**

Ported from `E:\DXR2` @ `bb6988908f` by three lanes. Merged in forced order
(core → zscript → renderer) because the zscript branch does not compile
standalone by design.

### What is real

* **Storage and lifetime** — `FBillboard`, `FLevelLocals::Billboards`,
  `TickBillboards()` from `P_Ticker`, `Billboards.Clear()` in
  `ClearLevelData`, savegame serialization, GC marking of `attachedTo`.
* **Seven natives** on `LevelLocals` + `TextureID.GetIndex()` as a compiler
  intrinsic (`codegen.cpp` ~:8905 guard, ~:8938 case).
* **The draw path is wired** — `DispatchBillboards()` is called from
  `hw_drawinfo.cpp:497`. Billboards are not in the BSP so they are
  dispatched separately.
* `GatherVisibleBillboards` — radial cull, `rs_bb_cullradius` (1024) and
  `rs_bb_maxpanels` (0 = unlimited), applied at gather time so changing
  either at runtime brings panels back with no respawn.

### ❌ THREE THINGS THAT ARE NOT TRUE, however it may read elsewhere

1. **ONLY `BB_TEXTURE` RENDERS.** The rendering lane ported five payload
   shaders (`gitd_bb_panel/digits/glyph/ring/bar.fp`, both shader trees)
   and then **backed all of them out** in commit `c91a015fd8`. The `.fp`
   files are gone. `BB_PANEL`, `BB_DIGITS`, `BB_GLYPH`, `BB_RING`, `BB_BAR`
   are declared in the enums, accepted by the API, and draw nothing.

2. **BILLBOARDS CANNOT HINGE.** `FBillboard` is `id`, `pos`, `size`,
   `payload`, `data`, `color`, `flags`, `lifetime`, `attachedTo`,
   `attachOffset`. **There is no orientation — no yaw, no pitch, no roll —
   and no parent or hinge concept.** Every billboard is an independent
   world-space quad that faces the camera.
   **So the owner's triptych cannot be expressed natively.** The mod-side
   `RS_PanelAssembly` in `E:\RS_Main\zscript\systems\ui\RS_Panel.zs` — which
   hinges panels in ZScript on `RF_FLATSPRITE` — is the only thing that can
   build it. Making the native path capable means adding per-panel
   orientation to `FBillboard` and a relative-transform concept to the API.

3. **`BB_TEXTURE` IS NOT REACHABLE END-TO-END** — its `data` is a texture
   index, and `TextureID.GetIndex()` now exists, but nothing has ever
   exercised the pair.

### Conventions that must not drift

* **`size` is FULL extent, edge to edge.** The quad spans `pos ± size*0.5`;
  `size = 88` is a card 88 tall. Renderer and ray test were checked against
  each other. **Change one, change both.**
* **`pos` is the panel CENTRE.**
* **UV convention.** `hw_sprites.cpp` carries two: the swapped form at
  `:1658-1659` used by everything drawing a real texture, and the unswapped
  form at `:1559-1560` used only by `ProcessParticle`. **The unswapped one
  is wrong and invisible, because particles are round.** DXR2's own
  `ProcessBillboard` used the unswapped form — that is the mirrored-text
  bug this project has lost the most time to. The rendering lane fixed it at
  source rather than porting it (commit `5739e27d8f`).
* **ZScript's payload/flag enums in `doombase.zs` and the C++
  `EBillboardPayload` are a matched pair that nothing cross-checks.**
  Renumber either and every mod call site silently changes meaning.

### Unverified, flagged by the authors themselves

* Savegame round-trip, including `TObjPtr<AActor*>` inside `FBillboard`.
* Hub transitions; the attached-actor-dies path.
* `BBF_VIEWRELATIVEZ` reads `consoleplayer`'s `viewz` inside the playsim
  tick and that value is serialized. Believed self-correcting; **not
  proven**, and on a non-primary level it reads the primary level's player.
  Wants a netgame/hub decision.
* `wipeType` / `wipeProgress` are serialized and **completely inert** —
  nothing sets or reads them. Parity ballast from DXR2.
* **`AimBillboard`'s `[hit, uv]` multi-return has never been run.**
  Statically verified against `jit_call.cpp` only. **Argument count and
  return type are NOT cross-checked in a release build** — a mismatch
  returns garbage silently, and this engine already shipped exactly that bug
  in `Sector.GetGlowColor` (native `double` vs ZScript `color`).
  **The test: run it under `vm_jit 0`, then `vm_jit 1`.** The interpreter
  calls the `DEFINE_ACTION_FUNCTION` body; the JIT calls the direct native.
  **Different answers = signature mismatch.** Do this before trusting any uv.

---

## 5. Console spam, undiagnosed

`AL_INVALID_ENUM (0xa002)` from `oalsound.cpp:226` and `:1006`, repeating
per sound. `getALError()` returns the *last* error, so the printed line is
usually not the guilty one — the engine is asking OpenAL for an extension
the loaded DLL reports but does not implement. **Sound works.** Noise, not
breakage.

---

## 6. Where the player-facing controls belong

Every engine capability gets its switch in **Radiance Control Panel**
(`E:\GlowInTheDark`), not in a menu of its own. If it must live in the
engine, it goes in Display Options **and** is mirrored there.

**An ability with no control in that panel effectively does not exist to the
player.** Currently unexposed there: most of the 16 flat-glow cvars, and
`rs_bb_cullradius` / `rs_bb_maxpanels` (added in that repo's commit
`83729c0`, unpushed).
