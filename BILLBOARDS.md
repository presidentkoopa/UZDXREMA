# BILLBOARDS — the goal, and why it doesn't work yet

**GOAL: player-facing interactable billboards.** A card floating in the world
that turns to face you, that you can point at and click, where the click resolves
to a specific row.

**Status: none of that works.** Nothing has ever been seen on screen. Read this
before you start, because three separate things are broken and they need
different fixes.

---

## 1. What exists

`FBillboard` (`src/g_levellocals.h`) — a world-space card. Stored in
`FLevelLocals::Billboards`, ticked from `P_Ticker`, serialized with the level,
dispatched from `HWDrawInfo::CreateScene` → `DispatchBillboards`
(`hw_bsp.cpp`) → `HWSprite::ProcessBillboard` (`hw_sprites.cpp`).

Seven ZScript natives on `LevelLocals`: `AddBillboard`,
`AddBillboardPersistent`, `UpdateBillboard`, `MoveBillboard`,
`RemoveBillboard`, `AttachBillboard`, `AimBillboard`. Plus
`TextureID.GetIndex()` as a compiler intrinsic.

Three kinds, differing only in lifetime: **transient** (self-expires, no
handle), **persistent** (lives until removed, returns a handle), **attached**
(follows an actor, dies with it).

---

## 2. THE THREE BLOCKERS

### A. Only one payload of six draws anything

`BB_TEXTURE` (=1) renders. `BB_PANEL`, `BB_DIGITS`, `BB_GLYPH`, `BB_RING`,
`BB_BAR` are declared, accepted by the API, and **draw nothing** — five payload
shaders were ported then backed out in `c91a015fd8`.

`BB_PANEL` has since been rebuilt (`0b07b37102`, `bb_panel.fp` in both shader
trees). **It has never been seen** — nothing in the mod draws a `BB_PANEL`, and
Vulkan compiles material shaders lazily, so the GLSL itself is unproven.

> **`FBillboard::payload` defaults to 0 = `BB_PANEL`.** A caller that never sets
> payload gets the one that didn't render. Watch for that.

### B. Interaction is structurally impossible today

`AimBillboard` **has never been executed** and cannot serve the mod's panels.
Three independent reasons, all found in practice by the panel lane:

1. **It iterates `FLevelLocals::Billboards`.** The mod's panels are
   `RF_FLATSPRITE` actors, which never register there. It returns `-1` forever
   regardless of aim — the candidate set is simply empty.
2. **It treats `size` as a single SQUARE half-extent** — the bounds test is
   `|lu|, |lv| <= size*0.5` on both axes. Real panels are rectangular (40×80).
   Even a registered billboard would be tested against the wrong shape.
3. **It derives the normal per call as "facing the ray origin."** Correct for a
   camera-facing quad, wrong for fixed hinged wings that are deliberately not
   camera-facing.

The panel lane wrote its own ray/plane intersection instead. **That was the
right call, not a shortcut.**

**So "interactable" needs a decision first:** either register the mod's panels in
`Billboards` and give `FBillboard` a rectangular extent and an orientation, or
accept that aiming happens mod-side and the engine primitive is display-only.

### C. `FBillboard` has no orientation at all

No yaw, no pitch, no roll, no parent. Every card is an independent world quad
that faces the camera. **Panels cannot hinge.** A triptych or folding menu
cannot be built from these natively.

---

## 3. Why nothing appears — the state of that hunt

Four lanes traced the whole path. The chain is wired: the store fills, the tick
runs, the cull passes with an 86x margin, the dispatcher is reached, the quad is
44×44 and cannot collapse, the texture round trip is an identity and cannot lose
the value.

**The load-bearing finding:** a canvas texture is forced OPAQUE on every backend
(`gl_renderstate.cpp`, `vk_renderstate.cpp`, `gles_renderstate.cpp`). An
unpainted canvas renders as an **opaque black rectangle**, never as nothing. The
owner sees nothing at all — **so the quad is not reaching the draw call.**

That points at the two early returns in `ProcessBillboard`: wrong payload, or
the texture failing to resolve. Both are diagnosed by `bb_list`.

**One exception that would break that conclusion:** a NaN in the position. It
flows into `gl_Position` and the GPU silently discards the primitive — genuinely
nothing, on every backend. `bb_list` prints the position; check it for NaN.

### `bb_list` — the diagnostic

Console command. Prints every live billboard: id, payload, size, flags,
position, distance, VISIBLE/CULLED, and resolves `BB_TEXTURE`'s data back to a
real texture name.

* **"0 billboard(s) live"** → nothing called the API; failure is script-side.
* **`CULLED`** → exists, `rs_bb_cullradius` is hiding it.
* **`data=N does NOT resolve`** → the texture round trip broke.
* **`VISIBLE` and nothing on screen** → renderer.

> **KNOWN BUG:** it measures distance from the player's **feet**
> (`p->mo->Pos()`), while the renderer culls from `Viewpoint.CenterEyePos` — the
> **eye**. ~41 units, more in VR. Only misleads near the cull boundary. Fix it.

---

## 4. Live wiring — do not break this

`E:\RS_Main\zscript\systems\player\RS_Screens.zs` calls, live:

```
mCardBB = level.AttachBillboard(mShowcase, (0, 0, 56),
    44, BB_TEXTURE, tex.GetIndex(), Color(255, 255, 255, 255));
```

Trigger: `netevent rs-showcase` (**hyphen, not underscore**) — a toggle, needs a
weapon in hand, silently does nothing if neither offhand nor ready weapon
resolves.

**Removing these natives stops the whole mod loading.** A ZScript call to a
missing native is a hard error, not a warning.

Two things about that call site worth knowing: the stand's own sprite is
`TNT1 A -1` — **invisible by design**; what you see is a separate display actor.
And the card sits ~96 units up at 96 out, roughly **30° above the eyeline**,
with its top half past the vertical half-FOV.

---

## 5. Defects found and not yet fixed

* **Canvas cards render upside down.** Hardware canvases are Y-flipped and the
  engine compensates only for walls and flats (`gametexture.cpp`
  `FTexCoordInfo::GetFromTexture`). The sprite path has no equivalent. On a card
  carrying text, this matters.
* **`BBF_NODEPTHTEST` is read by nothing.** Declared, documented as "draws
  through world geometry", never implemented. Set it and you still get depth
  testing, silently.
* **ZScript enum collision.** C++ uses `BBF_` for flags to keep them apart from
  `BB_` payloads. The ZScript copy uses `BB_` for both — so
  **every flag value is also a valid payload number.** Pass `BB_PERSISTENT`
  where a payload goes and it compiles clean and draws a texture.
* **`AimBillboard` returns `0`, not `-1`, on a transient hit** — transients have
  no handle. (A guard was added; verify it survived.)
* **Dispatched once per portal recursion**, not once per scene. Mirrors and
  skyboxes re-gather and re-submit every panel.
* **`modelframeflags` is never initialised** by `ProcessBillboard`. Safe today
  only because `modelframe` is null and every read is guarded. One edit from
  undefined behaviour.

---

## 6. If you are building "player-facing interactable"

Suggested order — do not skip step 1:

1. **Make one card visibly render.** `netevent rs-showcase`, then `bb_list`.
   Until something has been seen, everything else is building on sand. Nobody
   has done this.
2. **Fix the canvas Y-flip** or the text is upside down.
3. **Decide where aiming lives** — engine or mod. If engine: `FBillboard` needs
   a rectangular extent and the mod's panels need to register in `Billboards`.
4. **Orientation** only if hinging is actually wanted.

Do not build or launch the game unless asked. The owner is at that machine.
