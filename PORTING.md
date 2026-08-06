# PORTING.md — taking this fork's engine work to another GZDoom/QZDoom tree

This document exists so that somebody who has never seen this repository can
lift these features into their own fork without reverse-engineering a diff.
It is organised **per feature**, not per commit: the commits are interleaved
across four parallel work lanes and their boundaries are meaningless to you.

**Read the honesty section of each feature before you budget time for it.**
Some of this is finished and default-on. Some of it compiles, links, boots and
has never been looked at on a screen. Both are marked. Nothing here is
oversold; where the tree and the previous notes disagree, this file follows the
tree and says so.

* **Base:** `emawind84/gzdoom`, branch `questzdoom` (remote `origin`).
* **Our work:** `git log origin/questzdoom..questzdoom` — 27 commits,
  43 files, +2348 / −117.
* **Verification level:** the five features compile, link and boot. That is the
  entirety of the automated evidence, and per-feature visual confirmation is
  stated individually and is mostly absent.
  **The repairs in [§11](#11-defects-found-and-repaired) are newer than that
  and have NOT been compiled** — they were made by inspection, with builds
  off-limits. Every file table below describes the tree *including* those
  repairs, so a reader gets the current state; the untested ones are marked
  where they land.

---

## Table of contents

1. [Apply order](#1-apply-order)
2. [Three global invariants you cannot violate](#2-three-global-invariants-you-cannot-violate)
3. [Feature A — language CSV boot crash](#3-feature-a--language-csv-boot-crash)
4. [Feature B — wall texture glow](#4-feature-b--wall-texture-glow)
5. [Feature C — flat edge glow](#5-feature-c--flat-edge-glow)
6. [Feature D — ZScript glow API](#6-feature-d--zscript-glow-api)
7. [Feature E — billboards / in-world panels](#7-feature-e--billboards--in-world-panels)
8. [Cvar reference](#8-cvar-reference)
9. [Savegame compatibility](#9-savegame-compatibility)
10. [Conflict risk per file](#10-conflict-risk-per-file)
11. [Defects found and repaired](#11-defects-found-and-repaired)
12. [Appendix — deprecated actor flags (not a code change)](#12-appendix--deprecated-actor-flags-not-a-code-change)

---

## 1. Apply order

The five features are mostly independent, but there are three hard ordering
constraints and one soft one. Violate the hard ones and you get a build that
does not compile or, worse, one that compiles and produces silently wrong
uniform values.

### Hard constraint 1 — Feature A first, always

Feature A is a two-character data fix for a **hard boot failure**. If your base
predates it, your binary dies in `W_Init` before it reaches anything else, and
you will spend a day building against a dead executable convincing yourself
your own work is broken. It has no dependencies. Apply it before you look at
anything else.

### Hard constraint 2 — `StreamData` is claimed by B and C together

Features B and C both extend `struct StreamData`
(`src/common/rendering/hwrenderer/data/hw_renderstate.h:191`) and both were
independently written against the same spare `padding4` slot. The merge that
reconciles them puts B's member *in* the old `padding4` position and grows the
struct by three more `vec4` for C.

**If you take only one of B or C, you must still lay the members out in the
final order** if you ever intend to take the other. Adding them in a different
order later means editing two files in lockstep across an existing shader
cache. See [§2.1](#21-streamdata-member-order).

Order: **B before C** is what this tree did. The reverse works too, provided
the final member order is the one in §2.1.

### Hard constraint 3 — Feature E's three parts have a forced order

Feature E was split across three lanes and **the middle one does not compile
standalone by design** — its VM thunks reference `FBillboard` and
`FLevelLocals::Billboards`.

```
E-core  (storage, lifetime, savegame)  ->  E-script (natives, GetIndex)  ->  E-draw (renderer)
32a57563b5, 3486921d03                     cf28ed83dc, 916febbb9c            5739e27d8f, 24a8008c5d, c91a015fd8, …
```

* **E-core** — `g_levellocals.h`, `g_level.cpp`, `p_saveg.cpp`, `p_tick.cpp`,
  `p_setup.cpp`. Compiles and runs alone; billboards exist, tick, save and
  load, and draw nothing.
* **E-script** — `namedef.h`, `codegen.cpp`, `vmthunks.cpp`, `doombase.zs`,
  `base.zs`. **Requires E-core.**
* **E-draw** — `hw_drawstructs.h`, `hw_sprites.cpp`, `hw_bsp.cpp`,
  `hw_drawinfo.{h,cpp}`, `hw_shaderpatcher.cpp`. **Requires E-core** (it reads
  `FBillboard` and `FLevelLocals::Billboards`). Independent of E-script.

### Soft constraint — D after C

Feature D rewrites `sector_t::GetWallGlow` into a shared helper
`ResolvePlaneGlow`. Feature C's `SetupFlatGlow` calls `GetWallGlow` and
**depends on a guarantee that only D's rewrite provides**
([§2.3](#23-the-flat-glow-cross-file-invariant)). C's `colormode 1/2` will read
uninitialised stack without D. C's default `colormode 0` does not touch that
path, so C alone is not *broken* without D — but do not ship C with
`gl_flatglow_colormode` reachable unless D is in.

### Everything else

A, B, C, D, E are otherwise independent and can be cherry-picked in isolation.

---

## 2. Three global invariants you cannot violate

These span features. They are the parts that produce **silent** wrongness —
no compile error, no crash, no log line — and they are where this project has
lost the most time.

### 2.1 `StreamData` member order

`StreamData` (`hw_renderstate.h:191-254`) is `memcpy`'d **raw** into a Vulkan
uniform buffer:

```
src/common/rendering/vulkan/renderer/vk_streambuffer.cpp:45
  memcpy(ptr + mStreamDataOffset + sizeof(StreamData) * mDataIndex, &data, sizeof(StreamData));
```

The GLSL side declares the same struct by hand in
`src/common/rendering/vulkan/shaders/vk_shader.cpp:206-237`. **There is no
reflection, no offset check and no assert.** If the C++ struct and the GLSL
struct disagree by one member, every value from the divergence point onward
shifts and the shader reads a neighbour's data. No error. No crash. Just wrong
colours, and only under Vulkan.

The final tail order, which **both** files must carry identically:

```c
	int padding1;
	int padding2;
	int padding3;

	FVector4 uWallGlowColor;   // was 'padding4'  — Feature B
	FVector4 uFlatGlowColor;   // grows the struct — Feature C
	FVector4 uFlatGlowParms;   //                    Feature C
	FVector4 uFlatGlowShape;   //                    Feature C
```

Both files carry a `MERGE NOTE` comment saying exactly this. Keep it.

**GL and GLES are not affected by the order** — they set each uniform
individually by name (`muWallGlowColor.Set(&mStreamData.uWallGlowColor.X)`),
so a mismatch there is a link-time missing-uniform, which is loud. Vulkan is
the silent one.

Side effect worth knowing: `MAX_STREAM_DATA` is
`65536 / sizeof(StreamData)` (`vk_shader.h:27`). Growing the struct by 48 bytes
shrinks the number of draws that fit in one 64 KB uniform block by roughly
10 %. It is self-adjusting and needs no action, but it is a small throughput
cost on Vulkan.

### 2.2 There are two shader trees

```
wadsrc/static/shaders/glsl/        <- desktop GL + Vulkan
wadsrc/static/shaders_gles/glsl/   <- GLES / Android
```

**They are not generated from each other.** Editing one and not the other
produces a change that works on your desktop and is missing on Android, or the
reverse — and neither build warns.

They are not copies, either. The GLES tree is GLSL ES 1.00-shaped:
`varying`/`attribute` instead of `in`/`out`, no `layout(location=)`, and it
uses **compile-time `#define` permutations** (`DEF_USE_FLAT_GLOW`,
`DEF_USE_GLOW_TOP_COLOR`, …) where the desktop tree uses a runtime `if` on a
uniform. So the same logical change is written twice, differently:

| desktop | GLES |
|---|---|
| `layout(location = 9) in vec2 aEdgeDist;` (main.vp) | `attribute vec2 aEdgeDist;` inside `#if (DEF_USE_FLAT_GLOW)` |
| attribute location comes from the `layout` qualifier | `glBindAttribLocation(hShader, VATTR_EDGEDIST, "aEdgeDist")` in `gles_shader.cpp:543` |
| `if (uFlatGlowColor.a > 0.0 && …)` at runtime | `#if (DEF_USE_FLAT_GLOW)` block, plus a new permutation bit |

A new GLES permutation also needs a **unique tag bit** in
`FShaderManager::GetTag`-style packing — Feature C took bit 25
(`gles_shader.h:433`). If you have your own permutations, pick a free bit.

The same rule applies to `defaultshaders[]`
(`hw_shaderpatcher.cpp:270-300`): every `.fp` row listed there must exist in
**both** trees. The GLES backend substitutes the path prefix and compiles the
whole table at boot, so a missing GLES twin is a fatal `I_Error` on Android
even when the desktop build is perfect. There is a comment at the end of that
table saying so; it was left behind by the payload-shader backout
([§7.6](#76-only-bb_texture-renders)).

### 2.3 The flat-glow cross-file invariant

This one is a landmine and it is documented at the call site. Quoting
`src/rendering/hwrenderer/scene/hw_flats.cpp:121-127` verbatim:

```c
		// CROSS-FILE INVARIANT: top/bottom are deliberately left uninitialised. This is
		// only safe because ResolvePlaneGlow (p_sectors.cpp) writes glowdata[3] = 0 as its
		// FIRST statement, before any early return, so the seam[3] guard below is always
		// reading a written value. If an early return is ever added above that line, this
		// reads uninitialised stack and produces an intermittent, hardware-dependent wrong
		// colour. CheckSpriteGlow depends on the same guarantee.
		float top[4], bottom[4];
```

The other half, at `src/playsim/p_sectors.cpp:1206-1210`:

```c
 static bool ResolvePlaneGlow(sector_t *sec, int pos, float *glowdata)
 {
	 glowdata[3] = 0;                       // <- line 1208. FIRST statement. Load-bearing.
	 auto c = sec->planes[pos].GlowColor;
	 if (c == ~0u) return false;            // authored as "no glow"
```

`ResolvePlaneGlow` has three exits that leave `glowdata[0..2]` untouched. The
`glowdata[3] = 0` write on line 1208 is what makes the caller's `seam[3] > 0`
guard safe. **If you port `SetupFlatGlow`, you must port this guarantee with
it** — either keep the write first, or initialise `top`/`bottom` at the call
site. `sector_t::CheckSpriteGlow` (`p_sectors.cpp:1241`) relies on the same
thing.

### 2.4 (Bonus, and the expensive one) the UV convention in `hw_sprites.cpp`

Not a global invariant so much as **the defect this project has lost the most
time to**, and you will hit it the moment you draw anything textured through
`HWSprite`.

`HWSprite::CreateVertices` binds `ul` to vertices 0 and 2, and `ur` to
vertices 1 and 3. For a camera-facing quad, vertices 0/2 sit at `(x1,y1)` —
and `(x1,y1)` is **screen right**, because screen-right in world XY for a
camera looking along `ViewVector` is `(+ViewVector.Y, -ViewVector.X)`, which is
exactly how `x1`/`y1` are built. **So `HWSprite`'s corner names do not describe
its geometry**, and an unmirrored texture needs the *swapped* assignment.

Two conventions live in the file:

| location | form | correct? |
|---|---|---|
| `hw_sprites.cpp:1071-1072` — `HWSprite::Process`, unmirrored actor sprites | `ul = GetSpriteUR(); ur = GetSpriteUL();` — **swapped** | yes |
| `hw_sprites.cpp:1811-1814` — `HWSprite::ProcessBillboard` | **swapped** | yes |
| `hw_sprites.cpp:1897-1900` — `HWSprite::AdjustVisualThinker` | **swapped** | yes |
| `hw_sprites.cpp:1589-1590` — `HWSprite::ProcessParticle` | `ul = vt = 0; ur = vb = 1;` — **unswapped**, with `z1` *below* `z2` | **no** |

`ProcessParticle` draws its content rotated 180°. It has always been wrong and
nobody noticed, **because particles are round and a 180°-rotated circle is the
same circle.** It is four lines, it looks canonical, and it is the thing people
copy.

> **The rule:** match the convention of whoever built the corners you are
> drawing into. `HWSprite` names its corners dishonestly, so a real texture
> must be swapped. A path that builds and names its own corners honestly does
> not — `hw_decal.cpp:339,342` is the precedent, where `dv[UL]` genuinely is
> the left corner and takes the left `u`. `ProcessParticle` is the
> counter-example that proves the rule: it is unswapped while using
> `HWSprite`'s corners.

**Do not compensate downstream.** That is how the bug survived its first
round-trip: an earlier implementation left the quad mirrored and cancelled the
flip inside each payload shader, which fixed the shader-drawn payloads and left
the plain textured one — the only payload that actually carries text —
mirrored. Fix it at the corner assignment; a future payload shader must take
`vTexCoord` as it arrives and must not flip `u`.

The long-form version of this argument lives in the source at
`hw_sprites.cpp:1755-1800` and is worth reading before you touch any of it.

---

## 3. Feature A — language CSV boot crash

### What it does

`ParseLanguageCSV` indexes a fixed number of columns per row and two rows in
the upstream language data are one column short. The engine reads past the end
of the row and dies during `W_Init`, before the console exists. Appending one
comma to each row fixes it.

### Files touched

| file | change |
|---|---|
| `wadsrc/static/language.0` | `JOYMNU_INVERTDIGITALAXISBUTTONS` — one trailing comma appended, 30 → 31 cells |
| `wadsrc/static/language.csv` | `CMPTMNU_OLDRANDOM` — one trailing comma appended, 29 → 30 cells |

Commit `3469846916`. Two characters.

### Why this and not a bounds check

**A bounds check was written and then reverted on the owner's decision.** The
reasoning: the data is what is malformed, the parser's assumption is the
documented contract, and a silent bounds check would let a third short row ship
undetected. If your fork prefers a defensive parser, that is a legitimate
different call — but then make it *loud*, not silent.

### Order and dependencies

None. Apply first. See [§1](#1-apply-order).

### Cvars / savegame

None.

### Fork-specific vs upstream-safe

**Entirely upstream-safe** and almost certainly wanted upstream. Any fork on a
comparable base has the same two rows and the same crash. This is the single
highest-value two characters in this document.

### Condition

**Proven.** The engine boots. That is a complete test for this particular
change.

---

## 4. Feature B — wall texture glow

### What it does

GLDEFS has always parsed a `Glow { Walls { } }` block and **no renderer ever
consumed it** — the block was accepted and silently discarded. Now a wall drawn
with a listed texture adds a self-illumination term, so a lava wall or a fire
texture lights itself instead of relying on a sector's floor/ceiling glow
reaching it.

### Files touched

| file | lines | what changed |
|---|---|---|
| `src/common/textures/gametexture.h` | `:66`, `:118-121`, `:266-273` | New flag `GTexf_WallGlowing = 8192`; new field `uint16_t WallGlowStrength = 100`; accessors `isWallGlowing()`, `GetWallGlowStrength()`, `SetWallGlowing(int)` |
| `src/r_data/gldefs.cpp` | `:1130-1155` (`GLDefsParser::ParseGlow`, `WALLS` branch) | New `intensity <percent>` keyword; calls `tex->SetWallGlowing(strength)` alongside the existing `SetAutoGlowing()` |
| `src/common/rendering/hwrenderer/data/hw_renderstate.h` | `:242` member; `:359` reset; `:518-528` setters | `FVector4 uWallGlowColor` in the old `padding4` slot; `SetWallGlow(r,g,b,strength)` / `ClearWallGlow()` |
| `src/common/rendering/vulkan/shaders/vk_shader.cpp` | `:231`, `:336` | GLSL mirror of the member + `#define uWallGlowColor` |
| `src/common/rendering/gl/gl_shader.{h,cpp}` | `h:257`; `cpp:267-268`, `:621` | `FUniform4f muWallGlowColor`, uniform declaration, `Init` |
| `src/common/rendering/gl/gl_renderstate.cpp` | `:147` | `muWallGlowColor.Set(...)` in `ApplyShader` |
| `src/common/rendering/gles/gles_shader.{h,cpp}` | `h:345`; `cpp:309-310`, `:611` | same for GLES |
| `src/common/rendering/gles/gles_renderstate.cpp` | `:258` | same for GLES |
| `src/rendering/hwrenderer/scene/hw_walls.cpp` | `:49-65` cvars; `:230-252` in `RenderTexturedWall`; `:369` | The whole consumer |
| `wadsrc/static/shaders/glsl/main.fp` | `:800-808` in `getLightColor` | `color.rgb += desaturate(vec4(uWallGlowColor.rgb * uWallGlowColor.a, 1.0)).rgb;` |
| `wadsrc/static/shaders_gles/glsl/main.fp` | `:477-486` | same, GLES copy |

Commit `4faca0f9f3`.

### Why it is built this way

* **A separate flag and a separate strength field, not a reuse of
  `GlowColor`/`GlowHeight`.** Doom's `FIRE*` textures appear in both a `Flats`
  and a `Walls` glow block. Reusing the existing glow properties for walls
  would corrupt the flat glow of every texture used as both. `WallGlowStrength`
  is deliberately parallel and only ever read by the wall renderer.
* **Additive, not a replacement.** It sits beside `uGlowTop/BottomColor`
  (which are the sector's floor/ceiling glow landing *on* a wall) rather than
  replacing them. The two are unrelated effects that happen to share a word.
* **Outside the `SHADER_LITE` guard in `main.fp`, deliberately.** Unlike the
  sector glow it needs no interpolated `glowdist`, so it costs a branch on a
  uniform and nothing else. On GLES it is likewise *not* behind a `DEF_`
  permutation — one more shader variant would cost more than the branch.
* **`intensity` is sticky, not per-name.** In GLDEFS, `intensity <percent>`
  applies to every texture named *after* it until the next `intensity`. Names
  given before any `intensity` default to **100**. This matters
  (see below).

### Tint, and the rough edge — read this before you enable it

The glow originally shipped **hardcoded white**, using only the strength. That
is now selectable, defaulting to the texture's own colour
(`hw_walls.cpp:230-252`):

```c
	if (gl_texture_wallglow_tint)
	{
		float c[3];
		texture->GetGlowColor(c);
		state.SetWallGlow(c[0], c[1], c[2], strength);
	}
	else state.SetWallGlow(1.f, 1.f, 1.f, strength);
```

`FGameTexture::GetGlowColor` averages the texture's pixels and **caches the
result into `GlowColor`**, so this costs one decode per texture on first use
and nothing afterwards. The GLDEFS `WALLS` branch already calls
`SetAutoGlowing()`, so `GlowColor` is 0 going in and the average really is
computed.

**Three consequences a porter needs to know**, all verified in this tree:

1. **A texture with an *authored* glow colour** — one given an explicit colour
   by `Glow { Texture <name>, <color> }` — has a non-zero `GlowColor` already,
   so the tint uses the **authored** colour rather than an average. That is
   almost certainly what anyone would want, but it is not what "averaged
   colour" implies.
2. **`GetGlowColor` clears `GTexf_Glowing` when the average comes out black**,
   and it does **not** clear `GTexf_WallGlowing`. So a pure-black texture named
   under `Walls` keeps entering the branch forever and pays
   `SetWallGlow(0,0,0,strength)` + `ClearWallGlow` on every draw. Visually a
   no-op; the state churn is permanent, not a one-time cost.
3. **This moved a cross-path mutation into the draw phase.** Before, only the
   *flat* renderer ever called `GetGlowColor`, so only it could trigger that
   flag clear. Now the wall renderer can get there first — on a map where a
   black texture is listed under both `Flats` and `Walls` but is only ever seen
   as a wall, `GTexf_Glowing` is now cleared at wall-draw time where previously
   it might never have been. The end state is identical (a black glow is no
   glow either way), which is why it was left alone, but it is a genuine
   cross-path mutation from a render function and worth remembering if flat
   glow ever misbehaves near a wall-glow texture.

Also expect **a one-frame hitch the first time a glowing wall texture comes
into view**, because the BGRA decode now happens inside `RenderTexturedWall`.
The flat path has always had the same characteristic. Both call sites are
single-threaded draw-list execution, so there is no race.

**The brightness rough edge is separate and is not fixed in the engine.** The
shader adds `tint * strength` to the lit colour; at `strength = 100 %` and
`gl_texture_wallglow_intensity = 1.0` that is a full `1.0` per channel, i.e.
fullbright. Since bare texture names with no `intensity` keyword default to
100, a GLDEFS file written before this feature existed will render every listed
wall near-fullbright. That is a **one-line mod-side GLDEFS edit** (add
`intensity 40` before the bulk of the names, leave the genuinely incandescent
ones near 100), not an engine change. Taking the texture's colour instead of
white does soften it considerably on its own — a dark texture averages dark —
but it does not replace the GLDEFS fix.

### Order and dependencies

Needs [§2.1](#21-streamdata-member-order) honoured if you also take Feature C.
Otherwise standalone.

### Cvars

| cvar | type | default | notes |
|---|---|---|---|
| `gl_texture_wallglow` | bool | **true** | Master switch. **Default ON** — this feature changes the look of the game the moment it is applied. |
| `gl_texture_wallglow_intensity` | float | 1.0 | `CUSTOM_CVAR`, clamped to `[0, 4]`. Master scale over the per-texture GLDEFS `intensity`. |
| `gl_texture_wallglow_tint` | bool | true | `false` = flat white, what this originally shipped with. `true` = the texture's own colour. A cvar rather than a fixed choice because it restyles every wall in a GLDEFS `Walls` block at once. |

All three `CVAR_ARCHIVE | CVAR_GLOBALCONFIG`. No savegame keys.

### Fork-specific vs upstream-safe

* **Upstream-safe / low conflict:** `gametexture.h` (pure additions in an enum
  and a class tail), the shader `main.fp` insertions, the uniform plumbing in
  the four backend files (all appended next to existing `muGlow*` lines).
* **Will conflict:** `hw_renderstate.h`'s `StreamData` tail — anyone else who
  claimed `padding4` collides head-on. `gldefs.cpp`'s `WALLS` branch is small
  but is inside upstream code.
* **Behavioural conflict:** `hw_walls.cpp:222-231` sits inside
  `RenderTexturedWall`, a function forks routinely modify.

### Condition

**Works, default on, never inspected on screen.** Compiles and boots on all
three backends. **Not wired into the level-mesh / lightmap path** — wall glow
does not route through the baked-lighting path, so lightmaps do not see it.

---

## 5. Feature C — flat edge glow

### What it does

Floors and ceilings glow inward from their edges — the other half of the seam
that wall glow already draws. The distance from every point of a flat to its
nearest sector boundary is computed once at map load and baked into the vertex,
so per frame the cost is one multiply and an add.

It is **paint, not light**: it is added to the surface it sits on and
illuminates nothing.

### Files touched

| file | lines | what changed |
|---|---|---|
| `src/common/rendering/hwrenderer/data/buffers.h` | `:30` | New vertex attribute `VATTR_EDGEDIST` (index 9, before `VATTR_MAX`) |
| `src/common/rendering/hwrenderer/data/flatvertices.h` | `:16`, `:24-25`, `:35`, `:48`, `:51-55`, `:57-64`, `:92` | `#define FLATVERTEX_NO_EDGE 65536.0f`; `FFlatVertex` gains `float edgedist, edgedistall`; both `Set()` overloads **and `SetVertex()`** initialise them to the sentinel; new `SetEdgeDist(visible, all)`; `FFlatVertexBuffer::mHighWater` |
| `src/common/rendering/hwrenderer/data/flatvertices.cpp` | `:36`, `:99`, `:102`, `:161-164`, `:169-187` | New format entry (`VFmt_Float2` at `offsetof(FFlatVertex, edgedist)`); `SetFormat(1, 3, …)` → `SetFormat(1, 4, …)`; watermark update in `AllocVertices`; `CCMD(flatvertexpeak)`; `#include "c_dispatch.h"` |
| `src/rendering/hwrenderer/hw_vertexbuilder.h` | `:30-32`, `:49-56` | `VertexContainer::positions` parallel array; `AddInteriorVertex(const DVector2&)` |
| `src/rendering/hwrenderer/hw_vertexbuilder.cpp` | `:74-99` fan rewrite; `:213-311` the whole baking pass; `:334-350` `SetFlatVertex` retyped; `:353-455` `CreateIndexedSectorVerticesLM`; `:457-485` `CreateIndexedSectorVertices` | The bulk of the feature |
| `src/common/rendering/hwrenderer/data/hw_renderstate.h` | `:77-90` `EFlatGlowShape`; `:246-253` three members; `:265` `mFlatGlowEnabled` bit; `:371-374` reset; `:541-570` setters | State plumbing |
| `src/common/rendering/vulkan/shaders/vk_shader.cpp` | `:232-234`, `:349-351` | GLSL mirrors + `#define`s |
| `gl_shader.{h,cpp}`, `gl_renderstate.cpp` | `h:261-263,287`; `cpp:274-277,624-626`; `renderstate:167-174` | Three uniforms + `currentflatglowstate` gate |
| `gles_shader.{h,cpp}`, `gles_renderstate.cpp` | `h:270,342-344,371,433`; `cpp:316-319,543,614-616,719`; `renderstate:199,290-297` | Same, plus `DEF_USE_FLAT_GLOW` permutation, `aEdgeDist` attribute binding, tag bit 25 |
| `src/rendering/hwrenderer/scene/hw_flats.cpp` | `:56-102` cvars; `:104-152` `SetupFlatGlow`; `:441-444`, `:497` in `DrawFlat` | The consumer |
| `wadsrc/static/shaders/glsl/main.vp` | `:9`, `:18`, `:84-89` | `aEdgeDist` in, `vEdgeDist` out, `vEdgeDist = mix(aEdgeDist.x, aEdgeDist.y, uFlatGlowParms.y)` |
| `wadsrc/static/shaders/glsl/main.fp` | `:11`; `:710-752` `flatGlowFalloff`; `:788-797` in `getLightColor` | Seven falloff shapes + the additive term |
| `wadsrc/static/shaders_gles/glsl/main.{vp,fp}` | mirrored, inside `#if (DEF_USE_FLAT_GLOW)` | GLES twins |

Commits `e6cc648978` (core), `b7e2748854` (falloff shapes), `d08105b5d6`
(the invariant comment).

### Why it is built this way

**Two distances per vertex, not one.** `edgedist` counts only boundaries that
*show a wall* at this plane; `edgedistall` counts every sector boundary,
including the invisible splits mappers use to carve one room into several
sectors. The first follows architecture and looks smooth; the second traces
lines across open floor, which is where a deliberate "grid" look comes from.
Both are baked because **the second cannot be added later without rebuilding
the map geometry**, and the vertex shader picks between them per frame with a
`mix()` on `uFlatGlowParms.y` — so a mod can cross-fade between the two looks
live.

**The fan anchor moved to an interior point** (`hw_vertexbuilder.cpp:74-99`,
and the matching index generation at `:437-450`). Every vertex of a convex
subsector lies *on* one of that subsector's own edges, so a fan anchored on a
vertex carries no interior sample for the distance to interpolate towards — a
whole room comes out at distance 0 and glows flat. The synthetic centre vertex
supplies that sample.

> **Cost, unconditional:** every subsector now carries **one extra vertex and
> two extra triangles**, whether or not glow is enabled. This is the one part
> of Feature C you pay for with `gl_flatglow 0`.

**Boundaries are the sector's own linedefs, plus one ring.**
`BuildSectorBoundaries` (`:253-278`) takes every linedef of the sector for the
"all boundaries" answer — a flat only exists inside its own sector, so anything
further out is on the far side of one of them. For the "visible wall" answer it
also pulls in the linedefs of neighbours reached across an *invisible* split,
because a wall a few units past a split belongs to the neighbour and its glow
has no reason to stop dead at the split. There is a `> 4096` safety valve for
maps that split one room into hundreds of pieces.

**Heights are read at map load** (`LineShowsWall`, `:224-234`). A lift or door
that moves later does **not** move the seam that was baked from it. This is a
known, accepted limitation, not an oversight.

**Seven falloff shapes, not one ramp.** A floor is far wider than a wall is
tall, so the linear ramp that suits a wall lands its terminator right out in
the open where the eye finds the crease. Every shape in `flatGlowFalloff` is
worth exactly 1 at the edge and exactly 0 at the reach, so none of them can
leave a hard line at the far end. The branch is on a uniform, so every fragment
in a draw takes the same one.

**`FLATVERTEX_NO_EDGE = 65536.0f`, not 0 and not infinity.** Walls, sprites,
models and 2D all share this vertex buffer. Their `edgedist` must start large
enough that no `reach` setting can ever light them up, and finite enough to
interpolate without producing NaN.

**Render hack planes get no glow** (`hw_flats.cpp:441-443`). `SSRF_PLANEHACK`
and `SSRF_FLOODHACK` build throwaway vertices with no baked distance, so they
are explicitly cleared rather than left to read the sentinel.

### The invariant

See [§2.3](#23-the-flat-glow-cross-file-invariant). Non-negotiable.

### Memory cost — the honest numbers

`FFlatVertex` grew **32 → 40 bytes** (+8, +25 %). `FFlatVertexBuffer::BUFFER_SIZE`
is a fixed **2,000,000-vertex preallocation** (`flatvertices.h:93`) — that
number is upstream's, not ours.

| | per buffer | buffers | total | **delta from this change** |
|---|---|---|---|---|
| desktop (`HW_MAX_PIPELINE_BUFFERS = 2`, `buffers.h:13`) | 64 → **80 MB** | 2 | 128 → **160 MB** | **+32 MB** |
| Android / GLES (`HW_MAX_PIPELINE_BUFFERS = 4`, `buffers.h:9`; `gles_framebuffer.cpp:124` defaults to 4) | 64 → **80 MB** | 4 | 256 → **320 MB** | **+64 MB** |

So: the frequently quoted **~320 MB is the *total* on GLES**, of which this
change is responsible for 64 MB. Both numbers matter and neither is small.

**Mitigations, in order of sense:**

1. **Halve `BUFFER_SIZE` to 1,000,000.** Saves 40 MB per buffer — **160 MB on
   GLES, 80 MB on desktop** — and costs **no precision at all**. It is a pure
   headroom reduction.
   **Measure first.** `BUFFER_SIZE` is unchanged in this tree precisely because
   nobody had the number. There is now a way to get it: `FFlatVertexBuffer`
   tracks a high-water mark in `AllocVertices` (`flatvertices.cpp:161-164`) and

   ```
   flatvertexpeak
   ```

   prints the peak, the cap, the percentage, bytes per vertex and MB per
   pipeline buffer. Run the heaviest map you have, then decide. The watermark
   is a deliberately unsynchronised read-compare-write — it is a diagnostic,
   not a counter the renderer acts on, and it is not worth a lock on the
   allocation path.
2. Pack the two distances into 12+12 bits. Saves ~32 MB on GLES, costs
   precision, and is strictly worse than (1).

### Order and dependencies

* Needs [§2.1](#21-streamdata-member-order).
* **Should follow Feature D** if you enable `gl_flatglow_colormode > 0`.
* `VATTR_EDGEDIST` must be index **9** to match
  `layout(location = 9) in vec2 aEdgeDist` in the desktop `main.vp`. If your
  fork has already added a vertex attribute, renumber **both** together.
* GLES needs the tag bit (`gles_shader.h:433`, bit 25) to be free in your tree.

### Cvars — sixteen of them, all archived, master switch OFF

| cvar | type | default |
|---|---|---|
| `gl_flatglow` | bool | **false** ← nothing is visible until this is on |
| `gl_flatglow_floor` | bool | true |
| `gl_flatglow_ceiling` | bool | true |
| `gl_flatglow_reach` | float | 32.0 (map units) |
| `gl_flatglow_intensity` | float | 1.0 |
| `gl_flatglow_shape` | int | 0 (`EFlatGlowShape`, 0–6) |
| `gl_flatglow_sharpness` | float | 1.0 |
| `gl_flatglow_inset` | float | 16.0 |
| `gl_flatglow_bandwidth` | float | 8.0 |
| `gl_flatglow_spacing` | float | 64.0 |
| `gl_flatglow_edges` | float | 0.0 (0 = visible walls only, 1 = every boundary, in between cross-fades) |
| `gl_flatglow_color` | color | `0xffffff` |
| `gl_flatglow_colormode` | int | 0 |
| `gl_flatglow_cap` | float | 1.0 |
| `gl_flatglow_pulse` | float | 0.0 (Hz) |
| `gl_flatglow_pulse_depth` | float | 0.0 |

`colormode 0` (default) takes the flat global colour and **never touches sector
glow**. `colormode 1` resolves through `sector_t::GetWallGlow` and falls back
to the cvar; `colormode 2` resolves through `GetWallGlow` and draws nothing
where there is no seam colour. So a nukage floor's edge glow comes out green
under `1`/`2`.

`gl_flatglow_reach` is **deliberately not shared with the wall glow's reach**:
64 up a wall is most of its height, 64 across a floor is a trim line.

No savegame keys.

### Fork-specific vs upstream-safe

* **Will conflict, significantly:** `hw_vertexbuilder.cpp`. The fan rewrite
  changes the vertex count, the index count and the triangulation of every
  subsector. Any fork that has touched flat vertex generation — lightmaps,
  level mesh, sector rendering — collides here. This is the hardest file in the
  document to port.
* **Will conflict:** `hw_renderstate.h` `StreamData` tail; `flatvertices.h`
  (`FFlatVertex` layout is a hot struct forks like to touch);
  `hw_flats.cpp::DrawFlat`.
* **Upstream-safe / low conflict:** `buffers.h` (one enum entry),
  `flatvertices.cpp` (two lines), the four backend uniform files, the shader
  insertions.
* **GLES-only risk:** the permutation tag bit and the `glBindAttribLocation`
  call. If your fork has its own GLES permutations, this needs manual
  renumbering.

### Condition

**Compiles, links, boots. Never inspected on screen, on any backend.**
Default-off, so a player sees nothing until they turn it on — which is both
the safest default and the reason nobody has noticed if it is wrong.

---

## 6. Feature D — ZScript glow API

### What it does

Adds a notion of **glow authority**: a colour written as an explicit choice
outranks the plane texture's own GLDEFS glow, while a colour written as a
*fallback* loses to it. That lets a mod paint a glow colour across every plane
in a map without erasing the colours GLDEFS supplies — nukage stays green, and
the paint lands only where nothing else had an opinion.

Also exposes the texture's own averaged colour to ZScript.

### Files touched

| file | lines | what changed |
|---|---|---|
| `src/gamedata/r_defs.h` | `:489-497` | New plane flag `PLANEF_GLOWAUTO = 512` |
| | `:976-999` | `SetGlowColor` now clears the flag; new `SetGlowColorAuto(pos, color)` sets it; new `IsGlowAuthored(pos) const` |
| `src/playsim/p_sectors.cpp` | `:1190-1233` | New `static bool ResolvePlaneGlow(sector_t*, int pos, float *glowdata)` — the single resolution point |
| | `:1235-1260` | `CheckSpriteGlow` reduced to a call into it |
| | `:1262-1275` | `GetWallGlow` reduced to two calls into it |
| `src/scripting/vmthunks.cpp` | `:105-133` | `TexMan.GetAverageColor` native |
| | `:1110-1116` | **Bug fix:** `GetGlowColor`'s direct native retyped `double` → `int` |
| | `:1153-1180` | `Sector.SetGlowColorAuto`, `Sector.IsGlowAuthored` |
| | `:1183-1218` | `Sector.GetTextureGlow` (multi-return: `color`, `double`) |
| `wadsrc/static/zscript/mapdata.zs` | `:544-555` | Three new `Sector` declarations |
| `wadsrc/static/zscript/engine/base.zs` | `:325-331` | `TexMan.GetAverageColor(TextureID, int normalize = 153)` |

Commits `40b82a8145`, `15ddfdec5a`.

### Why it is built this way

**The flag polarity is inverted on purpose.** `PLANEF_GLOWAUTO` marks the
*weaker* case. Everything that wrote `GlowColor` before this flag existed — the
UDMF loader, ACS `SetSectorGlow`, `Sector_SetGlow`, ZScript `SetGlowColor` —
keeps its authority **without having to be found and updated**. Only the new
`SetGlowColorAuto` opts into being outranked. If the flag had marked the strong
case, every existing writer would need a code change and any one you missed
would silently lose.

**`ResolvePlaneGlow` is the single resolution point.** No consumer outside
`p_sectors.cpp` reads `planes[].GlowColor` directly, so an auto-painted colour
cannot leak past the precedence rules into a renderer. Precedence, strongest
first:

1. an authored colour (map / ACS / `Sector_SetGlow` / `SetGlowColor`) — and
   `~0u` is authored too, meaning *"no glow at all"*;
2. the plane texture's own GLDEFS `Glow { Flats { } }` colour;
3. a colour from `SetGlowColorAuto`, which only lands where the texture had
   nothing to say.

**It fixed a live bug on the way.** The hand-written `GetWallGlow` assigned to
a single `ret` twice, so an authored *floor* colour with zero reach threw away
a *ceiling* glow that had already been found. The replacement resolves both
planes and combines with `|`, not `||`, with a comment saying why
(`p_sectors.cpp:1267-1274`). **Take this even if you take nothing else in D.**

**`GetGlowColor`'s return type was genuinely wrong.** The direct native
returned `double` while the ZScript declaration says `color`. The JIT builds
the direct-call signature from the *ZScript* declaration, so the caller read an
integer return register the callee never wrote. This is the same class of bug
that [§7.8](#78-aimbillboard-has-never-been-run) warns about, and it is here
because **this engine already shipped it once**.

**`GetTextureGlow` asks `isGlowing()` a third time** (`vmthunks.cpp:1195`).
`FGameTexture::GetGlowColor` switches glowing back *off* if the averaged colour
comes out black (`gametexture.cpp:269`), so without the recheck you would hand
back a black "glow".

**`GetAverageColor` is deliberately uncached.** It decodes the texture, so it
is a load-time call, not a per-tic one. A cache here would have to decide when
to invalidate; the caller already knows how many distinct textures it cares
about. `normalize` is `averageColor`'s `maxout`: `0` gives the plain average, a
positive value scales the brightest channel up to it so a muddy average still
reads as a hue. **153 is what the engine's own glow path uses**
(`gametexture.cpp:266`), which is why it is the default.

### Order and dependencies

* Standalone. Nothing else depends on it **except** Feature C's
  `colormode 1/2` path, which needs D's `glowdata[3] = 0` guarantee
  ([§2.3](#23-the-flat-glow-cross-file-invariant)).
* `vmthunks.cpp` gains `#include "bitmap.h"` — `textures.h` only
  forward-declares `FBitmap`.

### Cvars / savegame

**None.** `PLANEF_GLOWAUTO` lives in `plane.Flags`, which is **not**
serialized as new state — it is part of the existing plane flags word.

### Fork-specific vs upstream-safe

* **Will conflict:** `p_sectors.cpp` `GetWallGlow`/`CheckSpriteGlow` are
  rewritten wholesale, not patched. If your fork touched either, expect to
  merge by hand.
* **Low conflict:** `r_defs.h` (one enum value + two methods appended),
  `vmthunks.cpp` (appended blocks), both `.zs` files (appended declarations).
* **Bit 512 in the plane flags** must be free in your tree.

### Condition

**Compiles and boots. The API has not been exercised from a mod.** The
`GetGlowColor` return-type fix is verified by inspection against
`jit_call.cpp`, not by running it.

---

## 7. Feature E — billboards / in-world panels

### 7.1 What it does

A billboard is a world-anchored, camera-facing quad drawn as **real
depth-tested geometry** through the same translucent draw lists as sprites — not
a particle, not a HUD element. Mods create them from ZScript, move them, attach
them to actors, and ray-test them for clicks; they survive savegames and expire
on their own.

Ported from `E:\DXR2` @ `bb6988908f` by three lanes.

### 7.2 Files touched

**E-core** (commits `32a57563b5`, `3486921d03`)

| file | lines | what changed |
|---|---|---|
| `src/g_levellocals.h` | `:109-136` | `enum EBillboardPayload` (BB_PANEL/TEXTURE/DIGITS/GLYPH/RING/BAR = 0..5); `enum EBillboardFlags` (PERSISTENT 1, ATTACHED 2, NODEPTHTEST 4, **VIEWRELATIVEZ 8**) |
| | `:146-203` | `struct FBillboard` |
| | `:826-853` | `FLevelLocals::Billboards`, `NextBillboardID`, eleven method declarations |
| `src/g_level.cpp` | `:36` | `#include <algorithm>` for the over-cap sort |
| | `:2299-2305` | `FLevelLocals::Mark` — `GC::Mark(b.attachedTo)` per billboard |
| | `:2332-2333` | `rs_bb_cullradius`, `rs_bb_maxpanels` cvars |
| | `:2335-2476` | `FindBillboardByID`, `BillboardViewZ`, `RS_InitBillboardZ`, `AddBillboard`, `AddBillboardPersistent`, `UpdateBillboard`, `MoveBillboard`, `RemoveBillboard`, `AttachBillboard` — **the single implementation.** The VM thunks call straight into these; see [§11.1](#111-the-zscript-natives-were-a-second-implementation-fixed) |
| | `:2478-2521` | `TickBillboards` |
| | `:2524-2556` | `GatherVisibleBillboards` |
| | `:2569-2625` | `AimBillboard` |
| `src/p_saveg.cpp` | `:916-945` | `FSerializer &Serialize(FSerializer&, const char*, FBillboard&, FBillboard*)` |
| | `:1029-1041` | `arc("billboards", Billboards)("nextbillboardid", NextBillboardID)` |
| `src/p_tick.cpp` | `:166-172` | `Level->TickBillboards()` in `P_Ticker` |
| `src/p_setup.cpp` | `:337-346` | `Billboards.Clear()` in `ClearLevelData` |

**E-script** (commits `cf28ed83dc`, `916febbb9c`)

| file | lines | what changed |
|---|---|---|
| `src/common/engine/namedef.h` | `:205` | `xx(GetIndex)` |
| `src/common/scripting/backend/codegen.cpp` | `:8905` guard, `:8938-8945` case | `TextureID.GetIndex()` as a compiler intrinsic — `x = Self;` with `Self` already retyped to `TypeSInt32`, i.e. **no operation at all** |
| `src/scripting/vmthunks.cpp` | `:1222-1241` header comment | The signature-discipline rules — read them |
| | `:1243-1390` | Seven file-static functions, each a **one-line delegation** to the matching `FLevelLocals` method, + seven `DEFINE_ACTION_FUNCTION_NATIVE`. The statics exist only to unpack the VM's flattened argument layout — **they hold no logic** |
| `wadsrc/static/zscript/doombase.zs` | `:402-444` | `EBillboardPayload`, `EBillboardFlags` (incl. `BB_VIEWRELATIVEZ = 8`), `EBillboardPalette` |
| | `:599-642` | Seven `LevelLocals` natives |
| `wadsrc/static/zscript/engine/base.zs` | `:345` | `native int GetIndex();` on `TextureID` |

**E-draw** (commits `5739e27d8f`, `9e81279e0d`, `1862053bab`, `e5a443c50b`,
`24a8008c5d`, and the backout `c91a015fd8`)

| file | lines | what changed |
|---|---|---|
| `src/rendering/hwrenderer/scene/hw_drawstructs.h` | `:372-378` | `HWSprite::isBillboard`, `HWSprite::bbData` |
| | `:417-420` | `ProcessBillboard` declaration |
| `src/rendering/hwrenderer/scene/hw_sprites.cpp` | `:202-215` | `DrawSprite` — the billboard uniform-routing branch |
| | `:1296-1297`, `:1478-1479` | `isBillboard = false` resets in `Process` and `ProcessParticle` |
| | `:1583-1590` | The `ProcessParticle` UV warning comment |
| | `:1650-1861` | `HWSprite::ProcessBillboard` |
| `src/rendering/hwrenderer/scene/hw_bsp.cpp` | `:722-783` | `HWDrawInfo::DispatchBillboards` |
| `src/rendering/hwrenderer/scene/hw_drawinfo.{h,cpp}` | `h:209-212`; `cpp:494-497` | Declaration + the call site inside `CreateScene` |
| `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp` | `:293-300` | Comment only — the shader table rows were removed by the backout |

### 7.3 `FBillboard` — the primitive

```c
	int      id;            // handle; 0 = unassigned (pure transient)
	DVector3 pos;           // world position — the CENTRE of the quad
	double   size;          // FULL extent, edge to edge, NOT a half-extent
	int      payload;       // EBillboardPayload
	int      data;          // payload-specific packed int
	PalEntry color;         // modulates every payload
	int      flags;         // EBillboardFlags
	double   lifetime;      // SECONDS; <= 0 = permanent
	int      spawntic;      // maptime at creation
	int      wipeType;      // INERT — nothing sets or reads it
	double   wipeProgress;  // INERT — nothing sets or reads it
	double   viewZOffset;   // BBF_VIEWRELATIVEZ anchor
	TObjPtr<AActor*> attachedTo = MakeObjPtr<AActor*>(nullptr);
	DVector3 attachOffset;
```

**Conventions that must not drift:**

* **`size` is the FULL extent.** The quad spans `pos ± size*0.5`; `size = 88`
  is a card 88 units tall, not 176. This was the one thing nobody could
  determine from the DXR2 source, so it was checked against **both** consumers
  rather than assumed: the renderer builds `half = size * 0.5; z1 = z + half;
  z2 = z - half` (`hw_sprites.cpp:1836,1852-1853`) and the ray test accepts a
  hit only where `|lu|, |lv| <= size * 0.5` (`vmthunks.cpp:1455-1459`). **Change
  one, change both, or clicking a row lands on the wrong row.**
* **`pos` is the CENTRE**, not the bottom edge.
* **The `attachedTo` initialiser is load-bearing.** `TObjPtr` declares a bare
  union and no default constructor (`common/objects/dobjgc.h`). Without
  `MakeObjPtr<AActor*>(nullptr)`, every transient and persistent billboard —
  neither of which ever assigns `attachedTo` — carries a garbage pointer into
  `FLevelLocals::Mark()` **and into the savegame**.
* **The ZScript enums in `doombase.zs` and the C++ `EBillboardPayload` /
  `EBillboardFlags` are a matched pair that nothing cross-checks.** Renumber
  either and every mod call site silently changes meaning.

### 7.4 Lifetime model

Billboards are **set-and-forget**, unlike effects that are cleared and
re-published by their owner every tic. `TickBillboards` (`g_level.cpp:2478`) is
maintenance, **not a clear**:

* attached → follow `mo->Pos() + attachOffset`; if `attachedTo.Get()` is null
  (actor destroyed and swept), **drop the billboard**. It never revives the
  actor — `TObjPtr` does not keep it alive.
* attached billboards **never** self-expire by lifetime.
* `BBF_VIEWRELATIVEZ` → `pos.Z = viewz + viewZOffset`, re-anchored each tic.
* otherwise, non-persistent with `lifetime > 0` → delete once
  `(maptime - spawntic) / TICRATE >= lifetime`.

It runs **once per game tic, not per render frame**, so a high VR frame rate
cannot make panels strobe.

`ClearLevelData` drops them (`p_setup.cpp:345`) because `FLevelLocals` is
reused across levels — otherwise panels, handles and `attachedTo` pointers into
a destroyed level leak into the next map. **`NextBillboardID` is deliberately
NOT reset**, so a handle held across a level change can never accidentally
match a fresh panel; a stale handle stays inert, which `RemoveBillboard` and
`UpdateBillboard` already tolerate.

### 7.5 Culling and dispatch

Billboards are **not attached to subsectors and are not in the BSP**, so the
tree walk cannot find them. `DispatchBillboards` (`hw_bsp.cpp:738`) runs once
for the whole scene from `HWDrawInfo::CreateScene` (`hw_drawinfo.cpp:497`).

> **It must stay inside the `screen->mVertexData` Map/Unmap window** —
> `HWSprite::CreateVertices` allocates from it. The call site sits immediately
> before `mVertexData->Unmap()`.

`GatherVisibleBillboards` does a squared-distance reject against
`rs_bb_cullradius`, then applies `rs_bb_maxpanels` **to the survivors, keeping
the nearest** — so an over-budget scene drops the furthest panels rather than
whichever happened to be created last. It only pays for the sort when over
budget. Both limits are applied **at gather time, never at insert time**, so
changing either at runtime brings panels straight back with no respawn.

**Storage is uncapped by design.** A dropped-weapon world is expected to hold a
large live set. Do not add a ceiling to `Billboards`.

Two things are gathered against `Viewpoint.CenterEyePos`, not
`Viewpoint.Pos`: the cull, and the sort depth. In VR
(`hw_entrypoint.cpp` walks the scene **once per eye**) a per-eye position would
let a panel fall inside the radius for one eye and outside it for the other —
it would appear in half the headset. The centre is eye-independent, so both
eyes agree.

For an **attached** panel the dispatcher overrides X/Y with
`mo->InterpolatedPosition(Viewpoint.TicFrac)`, because `TickBillboards` leaves
it quantised to the 35 Hz tic and that reads as judder on a panel held near the
face. Z is left exactly as `TickBillboards` set it whenever
`BBF_VIEWRELATIVEZ` owns it.

### 7.6 ONLY `BB_TEXTURE` RENDERS

**Verified against the tree, not taken on trust.**

Commit `5739e27d8f` ported five SDF payload shaders — `gitd_bb_panel.fp`,
`gitd_bb_digits.fp`, `gitd_bb_glyph.fp`, `gitd_bb_ring.fp`, `gitd_bb_bar.fp`,
in **both** shader trees, ten files. Commit `c91a015fd8` **backed all ten out**,
along with five `defaultshaders[]` rows, five `SHADER_` enum entries in
`src/common/textures/textures.h`, and the payload switch in `ProcessBillboard`.

Confirmed: no `gitd_bb_*` file exists in either
`wadsrc/static/shaders/glsl` or `wadsrc/static/shaders_gles/glsl`.
`src/common/textures/textures.h` is **not in the fork diff at all** — the
addition and the removal cancelled.

The reason for the backout was that the ported shaders carried visual decisions
nobody in this tree agreed to (a hardcoded eight-colour palette, a neon
core/halo look, ten fixed glyph shapes, a pile of tuned constants). They are
being rebuilt from scratch with no predecessor to copy.

**So `BB_PANEL`, `BB_DIGITS`, `BB_GLYPH`, `BB_RING` and `BB_BAR` are declared
in both enums, accepted by the API, and draw nothing.** They warn once each on
the console (`hw_sprites.cpp:1715-1723`) rather than failing silently.

**The whole hook for whoever writes the real shaders** is documented in place
at `hw_sprites.cpp:1698-1724`: give the payload an `OverrideShader` instead of
returning, bind a real material (every backend wants one even if the shader
never samples it), and put `bb->data` on `uAddColor` via `bbData`.

> **The asymmetry that costs an afternoon if missed:** a payload drawing
> through the **default** shader must **not** put anything on `uAddColor`,
> because `getTexel` does `texel.rgb += uAddColor.rgb` (`main.fp`) and would
> add the packed payload integer to the image as a tint. Only a shader that
> never calls `getTexel` can borrow that uniform. That is exactly why
> `ProcessBillboard` sets `bbData = 0` for `BB_TEXTURE` (`hw_sprites.cpp:1727`,
> commit `1862053bab`).

### 7.7 `FBillboard` HAS NO ORIENTATION

**Verified against the struct.** `FBillboard` is `id`, `pos`, `size`,
`payload`, `data`, `color`, `flags`, `lifetime`, `spawntic`, `wipeType`,
`wipeProgress`, `viewZOffset`, `attachedTo`, `attachOffset`.

**There is no yaw, no pitch, no roll, and no parent or hinge concept.**
`ProcessBillboard` sets `Angles = DRotator()` (`hw_sprites.cpp:1744`) and
derives the quad's basis purely from `Viewpoint.ViewVector`
(`hw_sprites.cpp:1843-1844`, corners at `:1847-1853`). Every billboard is an
independent world-space quad that faces the camera.

**Consequence:** a hinged multi-panel assembly (a triptych, a folding menu)
**cannot be expressed natively.** Doing it in the engine means adding per-panel
orientation to `FBillboard` and a relative-transform concept to the API — a
real feature, not a tweak. Until then the only way to build one is in ZScript
on `RF_FLATSPRITE` actors.

### 7.8 `AimBillboard` has never been run

**Verified: no call site exists in `src`, `wadsrc`, or anywhere else in the
tree.** It has been statically checked against `jit_call.cpp` and nothing more.

The declaration is a multi-return:

```
native int, Vector2 AimBillboard(Vector3 start, Vector3 dir);   // doombase.zs:642
```

and the direct native is

```c
static int AimBillboard(FLevelLocals *self, double sx, double sy, double sz,
                        double dx, double dy, double dz, DVector2 *outUV);   // vmthunks.cpp:1426
```

**Why a trailing `DVector2*` and not an out-param:** `jit_call.cpp` keeps
return 0 in the real return slot when it is `REGT_INT`, then passes every later
return as a pointer argument **appended after the declared params**
(`jit_call.cpp:477-519`), and reads a `Vector2` back as two adjacent doubles
(`REGT_FLOAT|REGT_MULTIREG2`, `jit_call.cpp:562`). `DVector2` is exactly that
pair. This is the JIT's convention, not a style choice.

> **SIGNATURE DISCIPLINE — this applies to every billboard native, not just
> this one.** Argument **count** and **return type** are **not cross-checked**
> between the ZScript declaration and the direct native in a release build.
> Only argument *types* are, by `DirectNativeDesc` at C++ compile time. A count
> or return mismatch **does not error — it returns garbage**, and **only under
> the JIT**: the interpreter runs the `DEFINE_ACTION_FUNCTION` body instead, so
> it looks correct.
>
> The mapping the `doombase.zs` declarations rely on:
> `Vector3` → three doubles · `Vector2` (returned) → trailing `DVector2*` ·
> `color` → `int` · `Actor` → `AActor*`. **Defaulted ZScript params (`flags`,
> `lifetime`) are materialised by the compiler at the call site, so they still
> count as real arguments.**

**THE TEST, and do it before you trust any uv:**

```
vm_jit 0
```
…run the call, note the answer. Then:
```
vm_jit 1
```
…run it again. **Different answers = signature mismatch**, not a logic bug.

This engine has already shipped exactly this bug once —
`Sector.GetGlowColor`, native `double` vs ZScript `color`, fixed in this same
body of work ([§6](#6-feature-d--zscript-glow-api)).

### 7.9 Other unproven areas, flagged by the authors themselves

* **Savegame round-trip**, including `TObjPtr<AActor*>` inside `FBillboard`.
  The design is sound on paper — `attachedTo` rides the object table
  (`ReadObjects` runs earlier), a pointer whose actor did not survive loads as
  null, and the next tic's `TickBillboards` drops it exactly as it would have
  live — but **it has not been loaded back.**
* **Hub transitions** and the **attached-actor-dies** path.
* **`BBF_VIEWRELATIVEZ` reads `consoleplayer`'s `viewz` inside the playsim
  tick**, and that value is serialized. Believed self-correcting; **not
  proven**, and on a non-primary level it reads the primary level's player.
  Wants a netgame/hub decision. (It was also flatly non-functional from ZScript
  until [§11.2](#112-bbf_viewrelativez-was-non-functional-from-zscript-fixed);
  it now at least runs, which means it can finally be tested.)
* **`wipeType` / `wipeProgress` are serialized and completely inert.** Nothing
  sets or reads them. Parity ballast from DXR2; drop them if you are porting
  clean, or keep them if you want savegame parity with this tree.
* **`BB_TEXTURE` is not reachable end-to-end.** Its `data` is a texture index
  and `TextureID.GetIndex()` now exists, but **nothing has ever exercised the
  pair.**

### 7.10 Order and dependencies

E-core → E-script, E-core → E-draw. See [§1](#1-apply-order). E-script and
E-draw are independent of each other.

E-draw additionally needs `HWSprite::CreateVertices` to bind `ul` to vertices
0/2 the way this tree's does — see [§2.4](#24-bonus-and-the-expensive-one-the-uv-convention-in-hw_spritescpp).

### 7.11 Cvars

| cvar | type | default | notes |
|---|---|---|---|
| `rs_bb_cullradius` | float | 1024.0 | Map units. 0 = unlimited. |
| `rs_bb_maxpanels` | int | 0 | 0 = unlimited. Applied to radius survivors, nearest kept. |

Both `CVAR_ARCHIVE | CVAR_GLOBALCONFIG`. Both applied at gather time only.

### 7.12 Savegame keys

See [§9](#9-savegame-compatibility).

### 7.13 Fork-specific vs upstream-safe

* **Almost entirely additive**, which is unusual for a feature this size.
  `g_levellocals.h`, `g_level.cpp`, `p_saveg.cpp`, `vmthunks.cpp`,
  `doombase.zs` are appended blocks.
* **Will conflict:** `hw_sprites.cpp` — `DrawSprite` gains an `else if` branch
  in a hot function, and `Process`/`ProcessParticle` each gain two lines. Forks
  touch this file constantly.
* **Small but inside upstream code:** `p_tick.cpp::P_Ticker`,
  `p_setup.cpp::ClearLevelData`, `hw_drawinfo.cpp::CreateScene`,
  `codegen.cpp::FxMemberFunctionCall::Resolve`, `namedef.h`.
* **`p_saveg.cpp`'s `Serialize(FBillboard&)` overload** must be visible to the
  `TArray<FBillboard>` serializer — keep it in the same translation unit, above
  `FLevelLocals::Serialize`.

### 7.14 Condition — summary

| part | condition |
|---|---|
| storage, lifetime, tick, GC marking | compiles, boots, **not observed** |
| savegame serialization | compiles, **never round-tripped** |
| seven natives | compile, **never called from a mod** |
| `TextureID.GetIndex()` intrinsic | compiles, **never called** |
| radial cull + cap | compiles, **never observed** |
| draw path, `BB_TEXTURE` | compiles, **never seen on screen** |
| draw path, five other payloads | **removed. Draw nothing. Warn once.** |
| orientation / hinging | **does not exist** |
| `BBF_VIEWRELATIVEZ` | reachable and named as of [§11.2](#112-bbf_viewrelativez-was-non-functional-from-zscript-fixed)/[§11.3](#113-bbf_viewrelativez-had-no-name-in-zscript-fixed); **still never run** |
| `AimBillboard` | **never run.** Run the `vm_jit` test first. |

**None of the repairs in [§11](#11-defects-found-and-repaired) have been
compiled.** They were made without a build, by inspection only, because the
owner was at the machine. Treat the whole of Feature E as unproven code that
has now had its known logic defects removed — not as code that has been
exercised.

---

## 8. Cvar reference

Every new cvar in the fork. All are `CVAR_ARCHIVE`; the wall-glow and
billboard ones are also `CVAR_GLOBALCONFIG`.

| cvar | type | default | feature | on by default? |
|---|---|---|---|---|
| `gl_texture_wallglow` | bool | `true` | B | **yes — changes the look immediately** |
| `gl_texture_wallglow_intensity` | float | `1.0` | B | (clamped 0–4) |
| `gl_texture_wallglow_tint` | bool | `true` | B | `false` restores the original flat-white glow |
| `gl_flatglow` | bool | `false` | C | no |
| `gl_flatglow_floor` | bool | `true` | C | (gated by master) |
| `gl_flatglow_ceiling` | bool | `true` | C | (gated by master) |
| `gl_flatglow_reach` | float | `32.0` | C | |
| `gl_flatglow_intensity` | float | `1.0` | C | |
| `gl_flatglow_shape` | int | `0` | C | |
| `gl_flatglow_sharpness` | float | `1.0` | C | |
| `gl_flatglow_inset` | float | `16.0` | C | |
| `gl_flatglow_bandwidth` | float | `8.0` | C | |
| `gl_flatglow_spacing` | float | `64.0` | C | |
| `gl_flatglow_edges` | float | `0.0` | C | |
| `gl_flatglow_color` | color | `0xffffff` | C | |
| `gl_flatglow_colormode` | int | `0` | C | |
| `gl_flatglow_cap` | float | `1.0` | C | |
| `gl_flatglow_pulse` | float | `0.0` | C | |
| `gl_flatglow_pulse_depth` | float | `0.0` | C | |
| `rs_bb_cullradius` | float | `1024.0` | E | |
| `rs_bb_maxpanels` | int | `0` | E | (0 = unlimited) |

Feature D adds no cvars.

---

## 9. Savegame compatibility

Feature E adds **two keys** to `FLevelLocals::Serialize`
(`p_saveg.cpp:1029-1041`):

| key | type |
|---|---|
| `billboards` | `TArray<FBillboard>` |
| `nextbillboardid` | `int` |

`FBillboard` itself serializes fourteen sub-keys: `id`, `pos`, `size`,
`payload`, `data`, `color`, `flags`, `lifetime`, `spawntic`, `wipetype`,
`wipeprogress`, `viewzoffset`, `attachedto`, `attachoffset`.

**Which direction breaks:**

* **Old save → new engine: FINE.** `FSerializer` reading is key-based; a
  missing `billboards` key leaves `Billboards` empty and `NextBillboardID` at
  its initialiser of 1. Nothing else changes.
* **New save → old engine: the keys are ignored.** An engine without
  `FBillboard` simply does not look for them. Any billboards in the save are
  silently lost. That is a *data* loss, not a crash — but if your fork has a
  savegame version gate, **bump it**, because a user who loads a panel-bearing
  save in an older build and re-saves will have destroyed the panels without a
  warning.

**All billboards travel, not just persistent and attached ones.** `maptime` is
saved just above, so a transient's `spawntic` stays meaningful and it resumes
its remaining lifetime after the load.

**A view-relative panel's saved `pos.Z` is not authoritative and does not need
to be** — `TickBillboards` re-anchors it to the *loading* player's own eye on
the first tic after the load. That is the entire point of the design. (It is
also, as [§11.2](#112-bbf_viewrelativez-was-non-functional-from-zscript-fixed)
explains, unreachable from ZScript until that repair.)

Features A, B, C and D add **no savegame keys** and are savegame-neutral in
both directions. `PLANEF_GLOWAUTO` rides the existing plane flags word.

---

## 10. Conflict risk per file

For someone merging into a fork that already diverges from `emawind84/gzdoom`.

| risk | files | why |
|---|---|---|
| **high** | `src/rendering/hwrenderer/hw_vertexbuilder.cpp` | Flat vertex count, index count and triangulation all change. Collides with lightmaps, level mesh, any sector-rendering work. |
| **high** | `src/playsim/p_sectors.cpp` | `GetWallGlow` / `CheckSpriteGlow` rewritten wholesale. |
| **high** | `src/rendering/hwrenderer/scene/hw_sprites.cpp` | Hot file, three separate insertion points, plus a 200-line new function. |
| **high** | `src/common/rendering/hwrenderer/data/hw_renderstate.h` | `StreamData` tail — the `padding4` slot is the obvious place for *anybody's* new uniform. |
| **medium** | `src/common/rendering/vulkan/shaders/vk_shader.cpp` | Must mirror the above exactly. |
| **medium** | `src/common/rendering/hwrenderer/data/flatvertices.h` | `FFlatVertex` layout. |
| **medium** | `src/rendering/hwrenderer/scene/hw_walls.cpp`, `hw_flats.cpp` | Insertions inside `RenderTexturedWall` / `DrawFlat`. |
| **medium** | `wadsrc/static/shaders*/glsl/main.{fp,vp}` (four files) | Two trees, must stay in sync. |
| **medium** | `src/common/rendering/gles/gles_shader.{h,cpp}` | Permutation tag bit 25 and the `aEdgeDist` attribute binding must not collide with your own. |
| **low** | `gl_shader.{h,cpp}`, `gles_shader` uniform lines, `gl_renderstate.cpp`, `gles_renderstate.cpp` | Appended beside existing `muGlow*` lines. |
| **low** | `g_levellocals.h`, `g_level.cpp`, `p_saveg.cpp`, `vmthunks.cpp`, `doombase.zs`, `base.zs`, `mapdata.zs`, `gametexture.h`, `r_defs.h`, `buffers.h`, `namedef.h`, `hw_drawstructs.h`, `hw_vertexbuilder.h` | Appended blocks / single enum entries. |
| **low** | `p_tick.cpp`, `p_setup.cpp`, `hw_drawinfo.{h,cpp}`, `hw_bsp.cpp`, `codegen.cpp`, `flatvertices.cpp`, `gldefs.cpp`, `hw_shaderpatcher.cpp` | One-to-ten-line insertions. |
| **none** | `wadsrc/static/language.0`, `language.csv` | Two characters. |

---

## 11. Defects found and repaired

These were found by reading the tree against the previous notes, and then
fixed. **Every repair in this section was made by inspection only — nothing
here has been compiled**, because the owner was at the machine and a build was
off-limits. Each entry says what was wrong, what the fix is, and whether a
porter has to carry it.

### 11.1 The ZScript natives were a second implementation (fixed)

`src/g_level.cpp` defined `FLevelLocals::AddBillboard`,
`AddBillboardPersistent`, `UpdateBillboard`, `MoveBillboard`,
`RemoveBillboard`, `AttachBillboard`, `AimBillboard` and `FindBillboardByID`.
Its own header comment claimed *"The script thunks are thin wrappers over
these."*

**They were not.** `src/scripting/vmthunks.cpp` held a complete, independent,
file-static reimplementation of all eight, and a tree-wide search found **no
caller** for any of the `FLevelLocals::` versions except `TickBillboards`,
`GatherVisibleBillboards` and `BillboardViewZ`. So ~150 lines of `g_level.cpp`
were dead, and `AimBillboard`'s ray-vs-panel maths existed twice, in two files,
with nothing keeping them in step.

**Fix:** the seven statics in `vmthunks.cpp:1243-1390` are now genuine one-line
delegations. `FindBillboardByID` there is deleted. The `FLevelLocals` methods
are the single implementation; the statics exist only to unpack the VM's
flattened argument layout.

> **If you port this, do not "simplify" the statics away.** Their parameter
> lists are the JIT's ABI, not style — see
> [§7.8](#78-aimbillboard-has-never-been-run). Change bodies, never
> signatures.

Fixing this fixed 11.2 as a side effect, and surfaced 11.7.

### 11.2 `BBF_VIEWRELATIVEZ` was non-functional from ZScript (fixed)

A direct consequence of 11.1. `RS_InitBillboardZ` (`g_level.cpp:2363`) is what
stashes the caller's Z into `viewZOffset` when `BBF_VIEWRELATIVEZ` is set. It
was called only from the three dead `FLevelLocals` creation paths, and from
nowhere in `vmthunks.cpp`.

So on the only reachable path `viewZOffset` stayed at `0.0`, and a mod passing
flag bit 8 got a panel pinned to **exactly** eye height with the Z it passed
silently discarded on the first tic by `TickBillboards`'
`bb.pos.Z = viewz + bb.viewZOffset`. `MoveBillboard` had the same gap.

**Fix:** none needed beyond 11.1 — routing the thunks through the `FLevelLocals`
methods puts `RS_InitBillboardZ` and `MoveBillboard`'s re-anchor back on the
live path. This is the main reason 11.1 was worth doing.

### 11.3 `BBF_VIEWRELATIVEZ` had no name in ZScript (fixed)

`EBillboardFlags` in `doombase.zs` stopped at `BB_NODEPTHTEST = 4`, even though
the C++ enum (`g_levellocals.h:138-144`) has a fourth member and
`FBillboard::viewZOffset`'s comment describes it as the feature's whole point.
A mod would have had to pass a bare `8`.

**Fix:** `BB_VIEWRELATIVEZ = 8` added (`doombase.zs:431`).

> **11.2 and 11.3 must land together.** 11.3 alone would ship a nameable flag
> that actively destroys the caller's Z: the bit reaches `FBillboard::flags`
> through the old thunk, `viewZOffset` stays 0, and the panel snaps to eye
> height. The flag only becomes useful once creation routes through
> `RS_InitBillboardZ`.

### 11.4 `AimBillboard` returned `0` when it hit a transient (fixed)

Transients are created with `id` left at its initialiser of `0`, documented as
*"unassigned"*. The loop set `bestId = bb.id` without checking, so a hit on a
transient returned `0` — neither the documented no-hit value (`-1`) nor a
usable handle, since `FindBillboardByID` and `RemoveBillboard` both early-out
on `id <= 0`.

**Fix:** the loop guard is now
`if (bb.size <= 0.0 || bb.id <= 0) continue;` (`g_level.cpp:2593`). The call
exists so a mod can resolve a hit to a row and fire a netevent; a result the
caller cannot act on is worse than no result.

> **Deliberate asymmetry a porter must know about:** `GatherVisibleBillboards`
> (`g_level.cpp:2534`) still only checks `size <= 0.0`, so **transients are
> still drawn — they are just no longer aimable.** The visible consequence is
> that a click passes through a decorative panel that looks solid. That is the
> intent (a transient has no handle, so it should not steal a click from a
> clickable panel behind it), but it is a real behavioural split between the
> draw path and the aim path and it is not self-evident from either side.

### 11.5 A commented-out debug dump would have null-dereffed (fixed)

`hw_vertexbuilder.cpp` held a commented-out block iterating `vert.vertices` and
calling `v.vertex->fX()`. `AddInteriorVertex` pushes
`FQualifiedVertex{ nullptr, -1 }`, so the synthetic centre vertices have a null
`vertex` and anyone uncommenting the block to debug flat geometry would have
crashed instantly and blamed the wrong thing.

**Fix:** it now indexes the parallel `vert.positions` array
(`hw_vertexbuilder.cpp:556-560`), which exists precisely because interior
vertices have no `vertex_t`. Still commented out; correct if re-enabled.

### 11.6 Wall glow ignored the GLDEFS colour (fixed, behind a cvar)

The tint was hardcoded white and only the strength was read, so
`Glow { Walls { LAVA1 } }` glowed white.

**Fix:** `gl_texture_wallglow_tint` (default `true`) switches between the
texture's own averaged colour and the original flat white. See
[§4](#4-feature-b--wall-texture-glow) for the three side effects of calling
`GetGlowColor` from the wall path — they are not obvious and one of them moves
a cross-path state mutation into the draw phase.

### 11.7 `AimBillboard` left `uv` uninitialised on a degenerate ray (fixed)

**Found while making the 11.1 repair, and it would have been *introduced* by a
naive delegation.**

`FLevelLocals::AimBillboard` returned `-1` on a zero-length direction vector
**without writing `*outUV`**. The old `vmthunks.cpp` copy wrote `(0,0)` there,
so the behaviour was masked. But the `DEFINE_ACTION_FUNCTION_NATIVE` body
declares

```c
	DVector2 uv;
	int hit = AimBillboard(self, sx, sy, sz, dx, dy, dz, &uv);
	if (numret > 1) ret[1].SetVector2(uv);
```

and `TVector2`'s default constructor is `= default` over two bare `vec_t`
members (`vectors.h:79-83`), i.e. **uninitialised**. So delegating without
fixing the root would have handed ZScript a garbage uv on any degenerate aim
vector — a stale VM register under the JIT.

**Fix:** every exit from `FLevelLocals::AimBillboard` now writes `*outUV`
(`g_level.cpp:2573-2580`). **Carry this if you port `AimBillboard` at all**;
it is invisible until someone aims with a zero vector, which a script can
easily do.

### 11.8 `FFlatVertex::SetVertex` left the new fields uninitialised (fixed)

Both `Set()` overloads initialise `edgedist`/`edgedistall` to
`FLATVERTEX_NO_EDGE`; `SetVertex` did not. **Nothing calls `SetVertex` today**
— verified tree-wide — so nothing was broken. The risk was forward-looking: the
next person to reach for it to fill a quad would get uninitialised edge
distances straight out of mapped GPU memory, showing up as random glow on a
random surface.

**Fix:** the sentinel is set there too (`flatvertices.h:57-64`). `SetTexCoord`
is left alone; it only touches `u`/`v` and is not a vertex-initialising call.

### 11.9 `BUFFER_SIZE` could not be sized without a number (instrumented)

Not a defect — a missing measurement, and the thing blocking a free 160 MB.
See [§5](#5-feature-c--flat-edge-glow). `BUFFER_SIZE` is **unchanged**;
`flatvertexpeak` now prints what a map actually reaches.

### 11.10 Still open

* **The C++ and ZScript billboard enums remain a matched pair that nothing
  cross-checks.** 11.3 closed today's gap; it did not add a mechanism. Renumber
  either side and every mod call site silently changes meaning.
* **Nothing in this section has been compiled**, and none of Feature E has been
  run. The `vm_jit 0` / `vm_jit 1` test in
  [§7.8](#78-aimbillboard-has-never-been-run) is still the first thing to do.
* The five payload shaders, `FBillboard`'s missing orientation, and the
  lightmap path not seeing wall glow are all **features to write**, not defects
  to repair.

### 11.11 Corrections to `ENGINE_WORK.md`

`ENGINE_WORK.md` opens by telling you to doubt it. Doing so found the
following:

* **§3's claim that `MISSILEMORE` / `MISSILEEVENMORE` / `SHORTMISSILERANGE`
  "cannot be fixed" is wrong.** All three have real property bindings. See
  [§12](#12-appendix--deprecated-actor-flags-not-a-code-change). The paragraph
  also contradicts itself, listing `+SHORTMISSILERANGE → MaxTargetRange 896` as
  a rename one sentence before saying it cannot be fixed.
* **§4's `hw_sprites.cpp` line numbers are stale.** The swapped UV form is at
  `:1811-1814`, not `:1658-1659`; the unswapped `ProcessParticle` form is at
  `:1589-1590`, not `:1559-1560`. The *claim* is correct; the anchors are not.
* **§4's "the script thunks are thin wrappers" reading is wrong** — see
  [§11.1](#111-the-zscript-natives-were-a-second-implementation-fixed).
* **§4's treatment of `BBF_VIEWRELATIVEZ` as merely "believed self-correcting,
  not proven" understates it** — see
  [§11.2](#112-bbf_viewrelativez-was-non-functional-from-zscript-fixed).
* **§2's memory figures conflate total with delta.** ~320 MB is the *total*
  flat-vertex-buffer footprint on a 4-buffer GLES config; this change is
  responsible for 64 MB of it. Desktop is 2 buffers, so 160 MB total / +32 MB
  delta. Halving `BUFFER_SIZE` saves 160 MB on GLES **and 80 MB on desktop**.
* Everything else in `ENGINE_WORK.md` that this document checked — the
  `c91a015fd8` backout, `FBillboard` having no orientation, `AimBillboard`
  never having been run, the `p_sectors.cpp:1208` invariant, the
  `hw_drawinfo.cpp:497` dispatch site, the `codegen.cpp` `:8905`/`:8938`
  anchors, the two-comma language fix, the cvar defaults — **checked out
  exactly.**

---

## 12. Appendix — deprecated actor flags (not a code change)

This is **not part of the fork's diff.** It is included because it is the
single most common source of console-warning noise on a mod running on this
base, and because the previous notes give incorrect advice about it.

**Deprecated flags in this engine are RENAMES, not removals.** They still work;
they just warn. Source of truth: `src/scripting/thingdef_properties.cpp`
(`HandleDeprecatedFlags`, `:276+`, and `CheckDeprecatedFlags`, `:390+`).

| deprecated | replace with | verified at |
|---|---|---|
| `+DONTHURTSPECIES` | `+DONTHARMCLASS` | `thingdef_data.cpp:442` maps it to `MF4_DONTHARMCLASS`; the flag is defined at `:223` |
| `+LOWGRAVITY` | `Gravity 0.125` | sets `actor->Gravity = 1./8` (`thingdef_properties.cpp:288`); `DEFINE_PROPERTY(gravity, F, Actor)` at `:1009` |
| `+SHORTMISSILERANGE` | `MaxTargetRange 896` | sets `actor->maxtargetrange = 896.` (`:290`); `property MaxTargetRange: MaxTargetRange;` at `actor.zs:344`, already used by `archvile.zs:18` |
| `+MISSILEMORE` | `MissileChanceMult 0.5` | `:332-343` |
| `+MISSILEEVENMORE` | `MissileChanceMult 0.125` | `:345+` |
| both together | `MissileChanceMult 0.0625` | the two cases compose to `0.0625` |

**`MISSILEMORE` and `MISSILEEVENMORE` *are* fixable**, contrary to the previous
notes. `MissileChanceMult` is a real bindable property —
`wadsrc/static/zscript/actors/actor.zs:352` declares
`property MissileChanceMult: MissileChanceMult;`, `:419` uses it in `Actor`'s
own defaults, and **the engine's own deprecation message says so**:

```
src/scripting/thingdef_data.cpp:930
    field->DeprecationMessage = "Use missilechancemult property instead";
```

The exact multiplier semantics, from `HandleDeprecatedFlags`:

* `+MISSILEMORE` alone → `missilechancemult = 0.5`
* `+MISSILEEVENMORE` alone → `missilechancemult = 0.125`
* both → `missilechancemult = 0.0625`

There is no engine change required for any of this, and nothing here needs
porting. It is a mod-side find-and-replace.

---

## Closing note

The four features that touch the renderer all **compiled, linked and booted on
GL, GLES and Vulkan** as of the state described in
[§1](#1-apply-order)–[§10](#10-conflict-risk-per-file). That is the entirety of
the automated evidence, and this project's own history records repeated cases
of "compiles and boots" meaning "consistent with itself" rather than "correct".

**The ten repairs in [§11](#11-defects-found-and-repaired) came afterwards and
have not been through a compiler**, let alone a game. They remove known logic
defects; they do not add evidence. If you are picking this up, the first
build after applying it is the first build anyone has done of this exact tree.

If you are integrating this, the order that will waste the least of your time
is: **Feature A immediately** (it is a boot fix and it is free), then
**Feature D** (small, self-contained, and it carries a real bug fix in
`GetWallGlow`), then **B**, then **C**, then **E** — and for E, run the
`vm_jit 0` / `vm_jit 1` test on `AimBillboard` before you build anything on
top of it.
