# Edge Glow — design of record

*Agreed 2026-08-06. The engine builds capabilities. GITD (ZScript) controls them.
Every axis below is independent and separately exposed; the engine hardcodes no
policy and picks no defaults that a mod can't override.*

---

## What it is

**Reverse ambient occlusion.** AO darkens where surfaces meet. This brightens the
same places. Every seam in a room gets an outline — the corner where wall meets
floor, where wall meets ceiling, the lip of a step, the edge of a platform.

Today the engine only ever lands glow on **walls**: the floor is a source, the
wall is the screen, and the floor itself never changes. This adds the other half
— the floor and ceiling glowing inward from their own edges.

---

## The one new primitive

Everything rests on a single new number:

> **For each point on a floor or ceiling: how far is it from the nearest boundary?**

For a wall this is trivial (it's height above the floor plane), which is why wall
glow already works. For a flat there's no equivalent and the engine doesn't
compute one.

Worked out **once at map load**, stored on the flat's vertices, interpolated
across the surface. Per-frame cost is then identical to what wall glow already
does — one multiply and an add.

### Two distances, not one

A boundary means two different things and the owner wants both:

| | what counts | look |
|---|---|---|
| **Visible walls only** | seams with an actual wall you can see | smoother, follows architecture |
| **Every sector boundary** | including invisible splits where a mapper divided a room for lighting or height | grid-like, traces map structure |

Doom rooms are chopped into invisible pieces; one open hall may really be five
sectors with nothing between them. Counting those produces glowing lines across
open floor. That is a **wanted** look, not a bug — it's where the grid comes from.

**Both distances are stored per vertex.** This must be decided up front: they are
baked at load, and the second cannot be added later without recomputing the map.
Two floats per flat vertex.

---

## What the engine provides

Capabilities only. No policy.

1. **Edge distance on flats**, both variants, per vertex.
2. **Edge glow as paint** — additive, same path as existing wall glow. It does not
   illuminate anything; it changes the surface it is on. Free.
3. **Two colour sources, selectable:**
   - *shared seam* — one colour for the corner, bleeding onto both surfaces, so a
     green wall throws green onto the floor beside it
   - *independent* — wall and flat each carry their own colour and they mix at the
     corner
4. **Independent reach per surface.** Walls and flats are not the same size — 64
   up a wall is most of it, 64 across a floor is a trim line. Separate numbers.
5. **Reading GITD's texture glow lists at runtime, toggleable.** This is the
   `Glow { Walls { } }` block finally being honoured, behind a switch.
6. **Every parameter reachable from ZScript**, live, without a map restart.

### Out of scope, deliberately

- **Dynamic lights.** A glowing seam isn't one light, it's a light every few units
  along it. One room becomes twenty, and VR pays twice. Reserved for things that
  move and matter — muzzle flashes, projectiles, elites.
- **Lightmaps.** The engine has them (the fragment shader samples a lightmap and
  adds it), and baking popular wads is a possible later upgrade. It **layers on
  top** of edge glow rather than replacing it — spill works in every wad including
  ones that don't exist yet; a baked wad just looks better. Nothing here needs
  rewriting to add it.

---

## What GITD controls

The mod owns all of it: which surfaces, which colours, how far, how bright, and
whether any of it is on. Menu knobs:

- colour — and which colour source mode
- reach / coverage, separately for walls and flats
- intensity
- falloff **distance** and falloff **shape** (a linear ramp reads as a ramp; a
  curve reads as light)
- per-surface switches — walls, floors, ceilings independently
- edge definition — visible walls vs every seam
- a **brightness cap**: glow is additive and clamps at white, so two sources
  meeting blow out to a flat patch
- pulse / breathe rate, if it moves at all
- the texture-list toggle

---

## File ownership — who edits what

Verified filenames, 2026-08-06. Three lanes, strict ownership so they cannot
clobber each other.

### Lane 1 — edge glow on flats

| file | why |
|---|---|
| `src/rendering/hwrenderer/scene/hw_flats.cpp` | flat rendering |
| `src/rendering/hwrenderer/hw_vertexbuilder.cpp` / `.h` | where flat vertices are generated — the edge distances get computed and attached here |
| `src/common/rendering/hwrenderer/data/flatvertices.cpp` / `.h` | flat vertex buffer + format |
| `src/common/rendering/hwrenderer/data/shaderuniforms.h` | vertex attribute declaration |

### Lane 2 — wall texture glow

| file | why |
|---|---|
| `src/r_data/gldefs.cpp` | the `Glow { Walls { } }` parse branch, :1113 / :1130-1139 |
| `src/rendering/hwrenderer/scene/hw_walls.cpp` | wall rendering, glow setup at :206-211 |
| `src/common/textures/gametexture.h` | the per-texture flag and strength field |

### Lane 3 — ZScript control surface

| file | why |
|---|---|
| `src/scripting/vmthunks.cpp` | the thunks, `SetGlowColor` pair at :1106-1118 |
| `wadsrc/static/zscript/mapdata.zs` | native declarations, glow block at :541-544 |
| `src/gamedata/r_defs.h` | `SetGlowColor` :978 / `SetGlowColorAuto` :988 |
| `src/playsim/p_sectors.cpp` | `GetWallGlow` / glow resolution, ~:1196-1290 |

### SHARED — lanes 1 and 2 both need these. Integrator hand-merges.

- `src/common/rendering/hwrenderer/data/hw_renderstate.h`
- `wadsrc/static/shaders/glsl/main.fp` **and** `shaders_gles/glsl/main.fp`
- `wadsrc/static/shaders/glsl/main.vp` **and** `shaders_gles/glsl/main.vp`
- `src/common/rendering/gl/gl_shader.{h,cpp}`, `gl_renderstate.cpp`
- `src/common/rendering/gles/gles_shader.{h,cpp}`, `gles_renderstate.cpp`
- `src/common/rendering/vulkan/shaders/vk_shader.cpp`

Both lanes add a uniform and a shader term, so overlap here is unavoidable.
**Keep changes in these files minimal, additive, and in one contiguous block**
so they can be merged by hand without a diff fight. Do not reformat or
restructure anything around your addition.

### Ownership is about collisions, not permission

The lists above exist so three lanes don't clobber each other. They are **not** a
prohibition on the work.

- **Reading any file is always fine.** Ownership restricts edits only.
- If a lane genuinely needs to edit a file another lane owns, **say so and route
  it through the integrator.** Do not edit it silently, and do not contort the
  design to avoid it — a worse implementation that respects an arbitrary
  boundary is the wrong trade.
- Files not listed anywhere are unowned. Touch them, and mention it in the report
  so the integrator knows. `hw_drawstructs.h` (where `HWFlat` and `HWWall` are
  declared), `hw_bsp.cpp` and `hw_drawlistadd.cpp` fall in this category and are
  very likely needed.
- Lane 3 additionally owns `wadsrc/static/zscript/engine/base.zs` if it exposes
  anything on `TexMan` or `TextureID` — those are declared there, not in
  `mapdata.zs`.

---

## Notes for whoever builds it

- Two shader trees must stay in sync: `wadsrc/static/shaders/glsl/` and
  `wadsrc/static/shaders_gles/glsl/`. Both have `main.vp` and `main.fp`.
- Existing glow for reference: `main.vp:73-79` computes `glowdist` from the two
  sector planes; `main.fp:735-742` applies it additively with linear falloff and
  clamps with `min(color, 1.0)`.
- `HWWall::RenderWall` sets it up at `hw_walls.cpp:206-211`.
- Render state is shared in `src/common/rendering/hwrenderer/data/hw_renderstate.h`
  with three backends under `src/common/rendering/{gl,gles,vulkan}`.
- **VR doubles every per-fragment cost.** `hw_walls.cpp` carries
  `wallVerticesPerEye` and `lightsWallPerEye`. Budget accordingly.
