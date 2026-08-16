# CHANGES.md — every file this fork changes, and what owns it

A map of the fork's engine work against stock UZDoom, for anyone who wants to read it,
port it, or merge upstream into it.

This file answers **where**. [`FORK_CHANGES.md`](FORK_CHANGES.md) answers **why**, in
detail, with the reasoning behind each decision.

---

## The one command

Everything below comes from a single diff, and you can regenerate it at any time:

```
git diff 5.0.0-rc.2 HEAD -- src wadsrc
```

The baseline is the upstream tag `5.0.0-rc.2`, not an ancestor commit, so that diff is
the fork's work and nothing else — no upstream churn mixed in. It stays accurate as
upstream advances: after the next merge, re-run it against the new tag.

---

## The engine is only half of it

**This repository is the engine half.** Almost every feature here is a *capability* plus
a ZScript native to reach it. The code that decides when to draw a beam, what a panel
says, or which shape goes where is **not in this repository** — it lives in the mods that
consume the engine.

So reading this repo tells you what the engine can be asked to do. It does not tell you
what any particular mod asks of it. If a feature looks like it does nothing, that is
usually correct: nothing in the engine calls it. Something external does.

The practical consequence for porting: taking a feature to another tree means taking the
C++ *and* the native declaration *and* keeping the signature stable, because content
outside this repo is compiled against it.

The bridge is 111 natives on `Level` added to
`wadsrc/static/zscript/doombase.zs`, plus a smaller set on `Actor`. That file is the
contract: if it changes shape, external content stops compiling.

```
  native Vector2 MeasureBillboardTextBlock(string text, double height, int fontSlot = 0);
  native bool FieldAt(Object o, int index, out string fieldName, out string fieldType);
  native bool GetFieldBool(Object o, string field, out int value);
  native bool GetFieldFloat(Object o, string field, out double value);
  native bool GetFieldInt(Object o, string field, out int value);
  native bool GetFieldName(Object o, string field, out name value);
  native bool GetFieldObject(Object o, string field, out Object value);
  native bool GetFieldString(Object o, string field, out string value);
  native bool HasField(Object o, string field);
  native bool IsVRInputSuppressed();
  native bool JSONProfileLoad(string name);
  native bool JSONProfileSave(string name);
  native bool, bool, double, double, double GetModelOrientationHint(class<Actor> cls, int sprite, int frame);
  native bool, double, double, double GetModelOffsetHint(class<Actor> cls, int sprite, int frame, double pixelstretch);
  ... and 97 more
```

Full list: `git diff 5.0.0-rc.2 HEAD -- wadsrc/static/zscript/doombase.zs`

---

## Scale

| | |
| --- | --- |
| Files changed | **398** |
| Lines | **+60,901 / −2,199** |
| Files the fork created outright | 78 |
| ZScript natives exposed | 111 |
| `src/` | 281 |
| ZScript | 69 |
| Shaders | 19 |

The deletion count is the interesting one. This fork is overwhelmingly **additive** — it
bolts on rather than rewriting, which is why it can be merged forward from upstream
instead of re-ported onto it.

---

## Invariants you cannot violate

Three rules hold across the whole fork. Breaking any of them fails in a way that does not
look like its cause.

**1. `StreamData` member order.** `StreamData` is the per-draw uniform block, and its size
divides a fixed 64KB buffer into `MAX_STREAM_DATA` draws. Growing it costs draw batching
in every frame of the game, forever. Several features here would have been easier with a
`vec4[8]` of per-band data and deliberately are not — frame-global values go in the
viewpoint buffer instead.

**2. There are two shader trees.** Changes under `wadsrc/static/shaders/` must be made in
both the GLSL and Vulkan-facing paths where both exist, or one backend silently renders
differently from the other.

**3. Uniform blocks are matched by offset, not by name.** `UniformBlockDecl::Create` emits
fields to GLSL in declaration order under plain `std140` with no explicit offsets, so the
C++ struct *is* the GLSL layout — and `vec2` needs an 8-byte boundary. Add a float to the
middle of `PresentUniforms` without a compensating pad and the present pass samples a
garbage UV rect and the screen goes black, with no error anywhere. There are
`static_assert`s guarding that one now, added after it happened.

---

## Feature index

Most renderer features live in **shared** files — `main.fp`, `g_levellocals.h`,
`vmthunks.cpp`, `hw_drawinfo.cpp` — so they rarely own a file outright. This index unions
primary and secondary ownership: every file a feature touches.

**Billboards** — 25 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `src/rendering/hwrenderer/scene/hw_sprites.cpp` *(1130)*
- `src/g_levellocals.h` *(845)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/rendering/hwrenderer/scene/hw_sdffont.cpp` *(325)*
- `src/rendering/hwrenderer/scene/hw_sdffont.h` *(253)*
- `src/p_tick.cpp` *(245)*
- `wadsrc/static/shaders/glsl/func_sdfpanel.fp` *(175)*
- `wadsrc/static/shaders/glsl/func_segment.fp` *(166)*
- `wadsrc/static/sdffonts/sdfmono.txt` *(101)*
- `wadsrc/static/shaders/glsl/func_wg13.fp` *(97)*
- `wadsrc/static/shaders/glsl/func_seam.fp` *(94)*
- `src/p_saveg.cpp` *(93)*
- `src/rendering/hwrenderer/scene/hw_drawstructs.h` *(72)*
- `wadsrc/static/shaders/glsl/func_sdftext.fp` *(70)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.h` *(23)*
- `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp` *(17)*
- `src/common/textures/textures.h` *(15)*
- `src/common/scripting/backend/codegen.cpp` *(12)*
- `wadsrc/static/zscript/engine/base.zs` *(6)*
- `src/namedef.h` *(6)*
- `wadsrc/static/graphics/sdfmono.png` *(0)*
- `wadsrc/static/graphics/bbwhite.png` *(0)*
- `wadsrc/static/graphics/bbring.png` *(0)*
- `wadsrc/static/graphics/bbpanel.png` *(0)*

**Billboard roll** — 2 files

- `src/rendering/hwrenderer/scene/hw_sprites.cpp` *(1130)*
- `src/g_levellocals.h` *(845)*

**Panel as a field** — 6 files

- `src/rendering/hwrenderer/scene/hw_sprites.cpp` *(1130)*
- `wadsrc/static/shaders/glsl/func_sdfpanel.fp` *(175)*
- `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp` *(17)*
- `src/common/textures/textures.h` *(15)*
- `wadsrc/static/graphics/bbwhite.png` *(0)*
- `wadsrc/static/graphics/bbpanel.png` *(0)*

**Surface glow** — 11 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_walls.cpp` *(571)*
- `src/common/rendering/hwrenderer/data/hw_renderstate.h` *(240)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*
- `src/common/rendering/gl/gl_renderstate.cpp` *(83)*
- `src/rendering/hwrenderer/scene/hw_drawstructs.h` *(72)*
- `src/gamedata/r_defs.h` *(62)*
- `wadsrc/static/zscript/mapdata.zs` *(46)*
- `src/common/rendering/gl/gl_shader.h` *(25)*
- `src/rendering/hwrenderer/scene/hw_weapon.h` *(2)*

**Texture inside the glow** — 3 files

- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*

**Glow wave** — 7 files

- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/common/rendering/hwrenderer/data/hw_renderstate.h` *(240)*
- `src/common/rendering/vulkan/shaders/vk_shader.cpp` *(230)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*

**Sweep** — 10 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/common/rendering/hwrenderer/data/hw_renderstate.h` *(240)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*
- `src/common/rendering/gl/gl_renderstate.cpp` *(83)*
- `src/common/rendering/gl/gl_shader.h` *(25)*

**Sweep band fill** — 6 files

- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/common/rendering/hwrenderer/data/hw_renderstate.h` *(240)*
- `src/common/rendering/gl/gl_renderstate.cpp` *(83)*

**Beams** — 10 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/p_tick.cpp` *(245)*
- `src/common/rendering/vulkan/shaders/vk_shader.cpp` *(230)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*
- `src/p_setup.cpp` *(30)*

**Volumetric beam** — 6 files

- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `wadsrc/static/shaders/pp/volumetricbeam.fp` *(254)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h` *(241)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp` *(117)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.h` *(23)*

**Bloom** — 6 files

- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h` *(241)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp` *(117)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess_cvars.cpp` *(53)*
- `wadsrc/static/shaders/pp/bloomextract.fp` *(32)*
- `wadsrc/static/shaders/pp/bloomcombine.fp` *(24)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess_cvars.h` *(9)*

**Fog slab** — 11 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_sprites.cpp` *(1130)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/rendering/hwrenderer/scene/hw_walls.cpp` *(571)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h` *(241)*
- `src/common/rendering/vulkan/shaders/vk_shader.cpp` *(230)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*

**Reactive fog** — 5 files

- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/rendering/hwrenderer/scene/hw_setcolor.cpp` *(4)*

**Per-fragment darkness** — 5 files

- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*

**Selective desaturation** — 4 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*

**The heatmap** — 6 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `src/g_levellocals.h` *(845)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h` *(241)*
- `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp` *(117)*
- `wadsrc/static/shaders/pp/heatmap.fp` *(92)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.h` *(23)*

**Shapes** — 11 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/shaders/glsl/main.fp` *(2522)*
- `src/rendering/hwrenderer/scene/hw_sprites.cpp` *(1130)*
- `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` *(939)*
- `src/g_levellocals.h` *(845)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` *(357)*
- `src/common/rendering/vulkan/shaders/vk_shader.cpp` *(230)*
- `src/common/rendering/gl/gl_shader.cpp` *(140)*
- `wadsrc/static/graphics/sdfmono.png` *(0)*
- `wadsrc/static/graphics/bbwhite.png` *(0)*

**Native state remap** — 4 files

- `src/playsim/p_actionfunctions.cpp` *(232)*
- `src/r_data/models.cpp` *(170)*
- `src/playsim/actor.h` *(124)*
- `src/r_data/models.h` *(10)*

**Direct model frame addressing** — 8 files

- `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp` *(2374)*
- `src/playsim/p_mobj.cpp` *(254)*
- `src/playsim/p_pspr.cpp` *(216)*
- `src/r_data/models.cpp` *(170)*
- `src/scripting/vmthunks_actors.cpp` *(145)*
- `wadsrc/static/zscript/actors/actor.zs` *(120)*
- `src/playsim/p_pspr.h` *(73)*
- `src/r_data/models.h` *(10)*

**psprite scale to model path** — 4 files

- `src/rendering/hwrenderer/scene/hw_weapon.cpp` *(1657)*
- `src/r_data/models.cpp` *(170)*
- `src/playsim/p_pspr.h` *(73)*
- `src/r_data/models.h` *(10)*

**Laser sight** — 15 files

- `wadsrc/static/menudef.txt` *(1772)*
- `src/rendering/hwrenderer/scene/hw_weapon.cpp` *(1657)*
- `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` *(1501)*
- `src/g_game.cpp` *(718)*
- `wadsrc/static/language.0` *(524)*
- `wadsrc/static/zscript/actors/inventory/weapons.zs` *(201)*
- `src/common/rendering/hwrenderer/data/hw_vrmodes.h` *(200)*
- `src/scripting/vmthunks_actors.cpp` *(145)*
- `src/playsim/actor.h` *(124)*
- `wadsrc/static/zscript/actors/actor.zs` *(120)*
- `wadsrc/static/engine/vr/defbind3.txt` *(84)*
- `src/g_statusbar/shared_sbar.cpp` *(21)*
- `src/gamedata/a_weapons.h` *(10)*
- `src/events.cpp` *(7)*
- `src/namedef.h` *(6)*

**Laser as a borrowed cursor** — 3 files

- `src/rendering/gl/stereo3d/gl_openvr.cpp` *(3793)*
- `src/rendering/hwrenderer/scene/hw_weapon.cpp` *(1657)*
- `wadsrc/static/zscript/engine/ui/menu/menu.zs` *(8)*

**Haptics in ZScript** — 4 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` *(1501)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/common/rendering/hwrenderer/data/hw_vrmodes.h` *(200)*

**Script-side VR input suppression** — 5 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` *(1501)*
- `src/g_game.cpp` *(718)*
- `wadsrc/static/zscript/doombase.zs` *(606)*
- `src/common/rendering/hwrenderer/data/hw_vrmodes.h` *(200)*

**MainHandRoll** — 3 files

- `src/scripting/vmthunks_actors.cpp` *(145)*
- `src/playsim/actor.h` *(124)*
- `wadsrc/static/zscript/actors/actor.zs` *(120)*

**Field reflection** — 2 files

- `src/scripting/vmthunks.cpp` *(3546)*
- `wadsrc/static/zscript/doombase.zs` *(606)*

**VR weapon wheel** — 6 files

- `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp` *(2374)*
- `wadsrc/static/menudef.txt` *(1772)*
- `src/g_game.cpp` *(718)*
- `wadsrc/static/language.0` *(524)*
- `src/g_level.cpp` *(34)*
- `src/common/rendering/hwrenderer/data/hw_vrwheel.h` *(24)*

**HUD stereo gating** — 4 files

- `src/d_main.cpp` *(746)*
- `src/common/rendering/gl/gl_postprocess.cpp` *(46)*
- `src/common/2d/v_2ddrawer.h` *(12)*
- `src/common/rendering/gles/gles_framebuffer.h` *(2)*

**Non-pausing menus** — 5 files

- `src/d_main.cpp` *(746)*
- `src/p_tick.cpp` *(245)*
- `src/common/menu/menu.cpp` *(117)*
- `src/common/menu/menu.h` *(29)*
- `wadsrc/static/zscript/engine/ui/menu/menu.zs` *(8)*

**Hitscan tracers and ricochet** — 2 files

- `src/playsim/p_map.cpp` *(487)*
- `src/playsim/p_hitscantracer.h` *(24)*

---

## Full manifest

All 398 files, grouped by the area that primarily owns them, largest first. The number
in brackets is total changed lines. **New** marks a file the fork created outright.

### VR core — 101 files, ~20072 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp` **New** | 5736 | New: the whole OpenXR Vulkan stereo device - session, swapchains, HMD/controller poses, layer submit, virtual screen. |
| `src/rendering/gl/stereo3d/gl_openvr.cpp` **New** | 3793 | New OpenVR stereo mode: HMD/eye poses, controller tracking, overlay screen, menu laser pointer. |
| `wadsrc/static/menudef.txt` | 1772 | Adds the whole VR options tree (VROptions/HUD/Weapon/Wheel/Laser/PerfTweak), double-bind control menus, multiplayer and credits menus. |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` | 1501 | Defines every vr_* CVAR and VRMode selection/Present/weapon transforms, plus the HUD canvas surface and net-wait shell. |
| `src/gl/stereo3d/gl_openxrdevice.cpp` **New** | 854 | GL renderer's OpenXR stereo mode: per-eye projection and view shift, HMD/hand pose, eye buffers, frame submit. |
| `src/common/rendering/vulkan/system/vk_renderdevice.cpp` | 744 | Drives the per-eye postprocess/present loop, OpenXR frame begin/acquire, multiview scene targets and recommended eye render size. |
| `src/playsim/p_map.cpp` | 487 | Routes aim, LineAttack, LineTrace and RailAttack through per-hand AttackPos/OffhandPos and their direction vectors. |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h` **New** | 333 | New header for the OpenXR Vulkan stereo mode: eye poses, grip-context enum, per-hand thumbstick read. |
| `src/QzDoom/qzdoom_common.cpp` **New** | 315 | Defines the shared VR globals (hand/HMD pose, doomYaw) plus the multiplayer teleport queue and roomscale offsets. |
| `src/common/rendering/stereo3d/openxr/oxr_loader.cpp` **New** | 285 | New dynamic openxr_loader module plus the bootstrap query for the Vulkan device and extensions OpenXR requires. |
| `src/playsim/p_hitscantracer.cpp` **New** | 259 | New hitscan tracer and ricochet queue, deterministically seeded and originated at the VR controller muzzle. |
| `src/playsim/p_mobj.cpp` | 254 | Exposes P_XYMovement for roomscale, +2 floor slack, spawns missiles from the VR hand pose, damage haptics. |
| `src/common/rendering/vulkan/textures/vk_renderbuffers.cpp` | 238 | Makes scene and pipeline targets layered arrays with per-eye views and framebuffers for the multiview path. |
| `src/playsim/p_pspr.cpp` | 216 | Adds the PSP_OFFHANDWEAPON layer, per-hand button checks, VR bob suppression and a psprite state recursion guard. |
| `wadsrc/static/zscript/actors/inventory/weapons.zs` | 201 | Adds the offhand/two-handed weapon flags and hand-routes every ready/raise/lower/fire helper to the offhand psprite. |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.h` | 200 | Turns VRMode/VREyeInfo into a virtual interface and declares the OpenXR/OpenVR hooks, HUD surface and script VR natives. |
| `src/common/rendering/vulkan/renderer/vk_postprocess.cpp` | 197 | Adds per-eye pipeline image pairs, layered multiview scene blits, and the XR present-to-image path with gamma/contrast bias. |
| `src/rendering/gl/stereo3d/gl_openvr.h` **New** | 173 | New header declaring OpenVREyePose/OpenVRMode and the OpenVR function-table forward decls. |
| `src/rendering/gl/stereo3d/LSMatrix.h` **New** | 162 | New OpenVR matrix/vector helper for turning HmdMatrix34_t poses into engine matrices. |
| `wadsrc/static/zscript/actors/attacks.zs` | 160 | Spawns items and grenades from the tracked hand's AttackPos/OffhandPos and direction instead of the player centre. |
| `src/rendering/hwrenderer/hw_entrypoint.cpp` | 154 | Rewrites the eye loop for per-eye setup, single-pass multiview scene, and a postprocess-only second eye. |
| `src/scripting/vmthunks_actors.cpp` | 145 | Routes offhand psprite sounds to CHAN_OFFWEAPON and exports the VR pose, hand and laser fields to ZScript. |
| `src/playsim/actor.h` | 124 | Adds the hand/HMD pose, grip-context and laser-trace field block on AActor, plus stateRemap on model data. |
| `src/playsim/p_user.cpp` | 114 | Computes the canonical main/offhand weapon pose each tic and carries OffhandWeapon through copy, GC and save. |
| `src/gl/stereo3d/gl_openxrdevice.h` **New** | 104 | Declares OpenXRDeviceEyePose and OpenXRDeviceMode, the GL side's VRMode/VREyeInfo implementation for OpenXR. |
| `src/QzDoom/VrCommon.h` **New** | 89 | New shared VR header: weapon/offhand pose globals, yaw-reset flags, net-safe teleport and smooth-turn helpers. |
| `src/rendering/r_utility.cpp` | 87 | Forces the full-screen scene window in stereo, keeps an unshifted CenterEyePos, adds haptic quake and roomscale view drift. |
| `src/common/menu/menudef.cpp` | 78 | Adds MENUDEF ifoption OpenXR/OpenVR/NonVR/Developer filters, ifnotoption blocks and AutoScroll. |
| `wadsrc/static/zscript/actors/strife/weapongrenade.zs` | 63 | Grenade launcher fires from the offhand and spawns/aims the grenade along the VR controller direction. |
| `src/common/rendering/stereo3d/openxr/oxr_procs.h` **New** | 62 | X-macro list of every OpenXR entry point the loader resolves: core, input, haptics, Vulkan and FB extensions. |
| `src/common/rendering/stereo3d/openxr/oxr_loader.h` **New** | 61 | New dynamic OpenXR loader header plus the Vulkan instance/device bootstrap query used before device creation. |
| `src/common/rendering/vulkan/renderer/vk_pprenderstate.cpp` | 58 | Adds DrawToImage so a postprocess pass can render into an XR swapchain image, with a per-eye layer framebuffer cache. |
| `src/common/rendering/vulkan/textures/vk_framebuffer.cpp` | 57 | Routes swapchain present through VRMode::RenderDesktopMirror when a VR mode is active, with barriers and fallback. |
| `wadsrc/static/zscript/actors/actions.zs` | 52 | A_ChangeVelocity becomes an action and gains CVF_RELATIVETOWEAPON recoil aligned to the firing hand's aim direction. |
| `src/rendering/hwrenderer/scene/hw_portal.cpp` | 48 | Swaps sky projection for both multiview eyes, strips skybox eye parallax, counts portals for the per-eye budget. |
| `src/common/rendering/vulkan/renderer/vk_renderstate.cpp` | 47 | Layered/multiview render targets with per-eye layer views, queued clear colour, translucent canvas alpha. |
| `src/common/rendering/vulkan/textures/vk_imagetransition.h` | 45 | Adds per-layer image views and layer-keyed framebuffer maps so Vulkan can render the multiview stereo target. |
| `src/common/rendering/v_video.cpp` | 45 | Defaults the backend to OpenGL for the OpenVR path, restores V_GetBackend(), adds vid_refreshrate and a 1400-square default mode. |
| `src/common/rendering/vulkan/renderer/vk_renderpass.cpp` | 42 | Adds the multiview ViewMask to render and postprocess passes, and makes a corrupt pipeline cache non-fatal. |
| `src/common/platform/posix/sdl/sdlglvideo.cpp` | 42 | Builds the Vulkan instance from OpenXR's required extensions and API-version range; adds I_FocusWindow. |
| `wadsrc/static/zscript/actors/hexen/fighteraxe.zs` | 39 | Routes every psprite state and melee trace through the offhand weapon when the axe is held off-hand. |
| `src/common/rendering/hwrenderer/data/hw_viewpointbuffer.cpp` | 35 | Adds SetViewpoints so multiview uploads one viewpoint block per eye; fixes block-align rounding. |
| `src/common/menu/resolutionmenu.cpp` | 31 | On OpenXR/Vulkan applies the chosen resolution via SetVirtualSize as internal render size instead of resizing the window. |
| `src/common/platform/win32/win32vulkanvideo.h` | 29 | Asks OpenXR for its required Vulkan instance extensions and API version range before building the instance. |
| `wadsrc/static/zscript/actors/hexen/fighterfist.zs` | 28 | Resolves the invoking hand and passes ALF_ISOFFHAND/LAF_ISOFFHAND through both punch attacks. |
| `src/playsim/d_player.h` | 26 | Adds OffhandWeapon, twelve WF_OFFHAND* flags, PlayInVR, resetDoomYaw and the premorph offhand slot. |
| `src/rendering/hwrenderer/scene/hw_drawinfo.h` | 23 | Adds the two multiview viewpoint sets and apply/inherit helpers, IsVRScene, and VR HUD quad/border draws. |
| `src/playsim/p_acs.cpp` | 22 | ACS GetWeapon takes a hand argument and CheckWeapon now matches either hand's weapon. |
| `src/common/rendering/vulkan/textures/vk_renderbuffers.h` | 22 | Threads layer counts through scene and pipeline render targets for multiview stereo; pipeline images go 2 to 4. |
| `src/common/rendering/gl/gl_framebuffer.cpp` | 21 | Adds NewRefreshRate driving VRMode::ApplyRefreshRate, switches to the cached VR mode, threads outside2D into Draw2D. |
| `src/common/rendering/vulkan/textures/vk_texture.cpp` | 20 | GetTextureView hands back the current eye's array-layer view for postprocess textures. |
| `src/common/rendering/r_videoscale.cpp` | 20 | Ignores vid_scalefactor for width and height while the OpenXR mobile VR mode is active. |
| `wadsrc/static/shaders/glsl/material_pbr.fp` | 18 | PBR dynlight loops skip back-facing lights and scale radiance by NdotL, fixing HUD weapon model lighting. |
| `wadsrc/static/shaders/pp/present.fp` | 17 | Restores the additive brightness lift and forces opaque alpha so the XR compositor cannot blend the frame. |
| `src/common/audio/sound/s_sound.cpp` | 17 | Keeps sound unpaused when the desktop window loses focus while a VR mode is active. |
| `wadsrc/static/zscript/actors/doom/weaponchaingun.zs` | 15 | Picks the invoking hand's weapon, aims with ALF_ISOFFHAND and animates the offhand psprite layer. |
| `wadsrc/static/zscript/actors/hexen/fighterquietus.zs` | 14 | Threads ALF_ISOFFHAND into the five FSwordMissile spawns when Quietus is held in the offhand. |
| `wadsrc/static/zscript/actors/player/player_morph.zs` | 13 | Carries the offhand weapon through morph via PremorphWeaponOffhand and clears both hands on unmorph. |
| `wadsrc/static/zscript/actors/doom/weaponbfg.zs` | 13 | Resolves the firing hand and passes ALF_ISOFFHAND to the BFG and plasma SpawnPlayerMissile calls. |
| `src/playsim/p_local.h` | 13 | Adds ISOFFHAND flags to aim/line/rail/trace, an explicit pitch on player missiles, and the teleport-hits-player check. |
| `src/common/rendering/gl/gl_stereo3d.cpp` | 13 | Defines the per-eye vertex/portal/light counters and switches the eye blit to the cached VR mode. |
| `wadsrc/static/zscript/actors/hexen/baseweapons.zs` | 12 | Keeps Hexen's melee auto-aim angle in a local so it no longer snaps the player's yaw in VR. |
| `wadsrc/static/zscript/actors/heretic/weaponskullrod.zs` | 12 | Skull rod resolves its invoking weapon per hand and passes ALF_ISOFFHAND into SpawnPlayerMissile aiming. |
| `wadsrc/static/zscript/actors/doom/weaponrlaunch.zs` | 12 | Resolves the invoking hand and passes ALF_ISOFFHAND to the rocket and grenade missile spawns. |
| `wadsrc/static/zscript/actors/doom/weaponpistol.zs` | 12 | Routes A_FirePistol through the offhand weapon and passes ALF_ISOFFHAND/LAF_ISOFFHAND to aim and hitscan. |
| `wadsrc/static/zscript/actors/heretic/weaponblaster.zs` | 11 | Blaster picks the invoking hand's weapon and passes offhand aim/attack flags to BulletSlope and LineAttack. |
| `src/rendering/hwrenderer/hw_models.cpp` | 11 | Clamps HUD-model depth range to the front 30% and flips normals so the held model cannot poke through walls. |
| `wadsrc/static/zscript/actors/doom/weaponchainsaw.zs` | 10 | Saw aims and puffs from the offhand when invoked there, and skips the turn-to-face while in VR. |
| `src/gamedata/a_weapons.h` | 10 | Adds WIF_OFFHANDWEAPON, NOHANDSWITCH, TWOHANDED, NO_AUTO_REVERSE, HASLASERBEAM, HASHITSCANTRACER; renumbers BFG/EXPLOSIVE. |
| `wadsrc/static/shaders/glsl/main.vp` | 9 | Copies gl_ViewIndex into hwViewIndex under SUPPORTS_MULTIVIEW; float-literal fixes for GLES. |
| `src/playsim/p_teleport.cpp` | 9 | Sets resetDoomYaw on player teleports so the HMD yaw re-centres, and defaults telezoom off. |
| `src/common/rendering/vulkan/renderer/vk_postprocess.h` | 9 | Adds the OpenXR gamma-bias flag and per-eye pipeline image pair selection to the present path. |
| `src/common/startscreen/endoom.cpp` | 8 | Redraws the ENDOOM screen every frame in VR and forces GS_MENUSCREEN so it actually composites. |
| `src/common/rendering/vulkan/renderer/vk_descriptorset.cpp` | 8 | Sizes the viewpoint UBO descriptor range to two eyes and binds the layer-aware postprocess texture view. |
| `src/rendering/r_utility.h` | 7 | Adds CenterEyePos (camera position before the eye shift) and hides FieldOfView behind Get/SetFieldOfView. |
| `wadsrc/static/zscript/actors/inventory_util.zs` | 6 | Inventory give, clear and auto-select paths account for OffhandWeapon alongside ReadyWeapon. |
| `wadsrc/static/zscript/actors/hexen/clericholy.zs` | 6 | Passes ALF_ISOFFHAND to the Wraithverge's HolyMissile when fired from the offhand. |
| `wadsrc/static/zscript/actors/hexen/clericflame.zs` | 6 | Fires CWeapFlame from the offhand weapon and passes ALF_ISOFFHAND to SpawnPlayerMissile. |
| `src/common/rendering/vulkan/renderer/vk_renderstate.h` | 6 | Adds layers/viewMask/layerIndex to SetRenderTarget so one pass can target both multiview eye layers. |
| `src/d_main.h` | 5 | Declares VR_DoomMain plus accessors for the startup language, IWAD query and support-wad settings. |
| `wadsrc/static/zscript/actors/hexen/blastradius.zs` | 4 | Lets A_Blast charge ammo from the offhand weapon instead of assuming ReadyWeapon. |
| `wadsrc/static/zscript/actors/heretic/ironlich.zs` | 4 | Whirlwind sets player.resetDoomYaw so the HMD yaw reference re-bases after it spins the player's view. |
| `src/widgets/launcherwindow.cpp` | 4 | Saves defaults when the launcher starts the game so a startup crash cannot lose the backend/VR selections. |
| `src/rendering/swrenderer/plane/r_skyplane.cpp` | 4 | Reads the viewpoint FOV through the GetFieldOfView() accessor added for per-eye VR field of view. |
| `src/playsim/p_lnspec.cpp` | 4 | ThrustThingHelper sets player keepmomentum so line-special thrust survives the VR momentum damping. |
| `src/openvr_include.h` **New** | 4 | Wraps openvr_capi.h in an openvr namespace so the C API does not collide with engine symbols. |
| `src/common/rendering/vulkan/renderer/vk_renderpass.h` | 4 | Adds Layers and ViewMask to the render-pass and postprocess pass keys so multiview passes get distinct pipelines. |
| `src/common/rendering/vulkan/renderer/vk_pprenderstate.h` | 4 | Declares DrawToImage so the present pass renders straight into the XR swapchain image on a given cmdbuffer. |
| `src/common/rendering/gl/gl_framebuffer.h` | 3 | Declares NewRefreshRate and the outside2D-taking Draw2D override used by the VR present path. |
| `wadsrc/static/zscript/scriptutil/scriptutil.zs` | 2 | Treats the offhand weapon as in-use so the drop-item helper does not discard it. |
| `wadsrc/static/zscript/actors/doom/doomweapons.zs` | 2 | Picks the invoking hand's weapon instead of ReadyWeapon before depleting ammo in the shared fire helper. |
| `src/rendering/swrenderer/viewport/r_viewport.cpp` | 2 | Follows the FRenderViewpoint change: reads the FOV through GetFieldOfView() instead of the now-private field. |
| `src/rendering/swrenderer/r_swrenderer.cpp` | 2 | Uses GetFieldOfView() when saving the camera-texture FOV, matching the per-eye FOV accessor. |
| `src/rendering/hwrenderer/scene/hw_weapon.h` | 2 | HUDSprite gains AddColor for per-psprite glow and keeps its quad screen/UV coords as members. |
| `src/rendering/hwrenderer/scene/hw_portal.h` | 2 | Adds a tempmatrix scratch member on FPortalSceneState for saving projection across sky portals. |
| `src/rendering/hwrenderer/scene/hw_lighting.cpp` | 2 | Ships gl_weaponlight defaulted to 0 instead of upstream's 8 as a VR-tuned default. |
| `src/p_conversation.cpp` | 2 | Comments out the forced player yaw at conversation start so dialogue cannot wrench the headset view. |
| `src/common/utility/matrix.h` | 2 | Makes VSMatrix::copy const so the stereo LSMatrix helper and multiview viewpoint upload can copy const matrices. |
| `src/common/audio/sound/s_soundinternal.h` | 2 | Adds CHAN_OFFWEAPON (channel 5) so offhand weapon sounds do not collide with the mainhand's. |
| `src/common/rendering/vulkan/textures/vk_texture.h` | 1 | Declares the GetTextureView(type, tex, depthOnly) overload the descriptor-set code needs for the multiview Vulkan path. |
| `src/common/rendering/hwrenderer/data/hw_viewpointbuffer.h` | 1 | Declares SetViewpoints(count) so both eyes' viewpoint uniforms upload as one block for multiview. |

### VR input — 70 files, ~4208 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/g_game.cpp` | 718 | Feeds controller stick/HMD motion and per-hand weapon yaw/pitch into usercmd, and gates keyboard turning in VR. |
| `src/win32/i_openVR.cpp` **New** | 544 | New Win32 OpenVR controller device: maps buttons and pad/stick axes to Doom keys with deadzones. |
| `wadsrc/static/zscript/actors/player/player.zs` | 524 | Threads an offhand weapon through fire/switch/drop plus VR momentum, crouch and instant 180 turn. |
| `src/common/platform/win32/i_openXR.cpp` **New** | 290 | New FOpenXRJoystick device exposing eight VR pad/stick axes with wider dead zones to the joystick config. |
| `wadsrc/static/zscript/engine/ui/menu/consoletextentermenu.zs` **New** | 288 | New console-command entry menu with a controller-driven character grid that stays open while the console is up. |
| `wadsrc/static/zscript/actors/inventory/stateprovider.zs` | 180 | Routes A_FireBullets/A_FireProjectile through the offhand weapon and the controller's AttackPos/OffhandDir. |
| `wadsrc/static/zscript/actors/actor.zs` | 120 | Declares the VR hand/HMD pose, grip-context, laser-trace and MainHandRoll natives, plus aimflags on missile spawns. |
| `src/console/c_cmds.cpp` | 113 | Adds the classic id* cheats as console commands, widens the warp cheat to 32-bit coords, adds disablerendercull. |
| `src/playsim/p_interaction.cpp` | 104 | Fires controller vibration and VR_HapticEvent on damage by damage type, and drops the offhand weapon too. |
| `wadsrc/static/zscript/actors/strife/sigil.zs` | 94 | Routes every Sigil fire/select action through the invoking hand's weapon using ALF_ISOFFHAND and PSP_OFFHANDWEAPON. |
| `wadsrc/static/zscript/actors/heretic/weaponphoenix.zs` | 89 | Resolves the invoker's hand and fires the phoenix rod from OffhandPos/OffhandDir with ALF_ISOFFHAND. |
| `wadsrc/static/engine/vr/defbind3.txt` **New** | 84 | New: President_Koopa's alternative VR controller layout - grip-modified binds, offhand fire, laser-sight toggle. |
| `wadsrc/static/zscript/actors/heretic/weaponmace.zs` | 64 | Fires the Heretic mace from the controller's AttackPos/OffhandPos and tags its aim with ALF_ISOFFHAND. |
| `src/common/scripting/interface/vmnatives.cpp` | 61 | Natives for the character-grid chat/console/cheat entry menus that let headset users type without a keyboard. |
| `src/common/console/c_console.cpp` | 57 | Adds C_ScrollConsole for gamepad console scrolling plus a bound-key toggleconsole path and a ConsoleToggled callback. |
| `src/common/menu/joystickmenu.cpp` | 55 | Inserts or removes the Joystick Options submenu item as physical controllers appear and disappear. |
| `src/common/console/c_bind.cpp` | 50 | Selects engine/vr/* commonbinds and defbind0-3 profiles in VR and gates mod DEFBINDS behind new cl_custombinds cvars. |
| `wadsrc/static/zscript/actors/heretic/weapongauntlets.zs` | 47 | Routes the gauntlets through OffhandWeapon: per-hand psprite, offhand aim/attack flags, no auto-turn in VR. |
| `wadsrc/static/engine/vr/defbind0.txt` **New** | 45 | Ships the pre-1.3.0 QuestZDoom default keybinding set shared by all games. |
| `wadsrc/static/zscript/actors/inventory/powerups.zs` | 41 | Tome of Power now swaps in the powered sister weapon for the off hand as well as the main hand. |
| `wadsrc/static/engine/vr/commonbinds.txt` **New** | 41 | Default keyboard/gamepad/automap bind set shared by every VR configuration. |
| `src/common/console/c_cvars.cpp` | 39 | Adds C_GetExternalHapticLevelValue, the ext_haptic_level_* cvar lookup scaled by the global haptic intensity. |
| `wadsrc/static/zscript/actors/hexen/clericstaff.zs` | 37 | Picks the invoking hand's weapon, passes ALF/LAF_ISOFFHAND, and skips the melee yaw snap while playing in VR. |
| `wadsrc/static/zscript/constants.zs` | 31 | Adds offhand attack/reload buttons, PSP_OFFHANDWEAPON, the WF_OFFHAND* weapon states and the *_ISOFFHAND aim/attack/trace flags. |
| `wadsrc/static/zscript/vr_stabilizesync.zs` **New** | 29 | New event handler copying the ready weapon's StabilizeDistance into AActor.StabilizeReach each tic. |
| `wadsrc/static/zscript/actors/heretic/weaponwand.zs` | 29 | Picks the invoking hand's weapon and passes ALF/LAF_ISOFFHAND into BulletSlope and LineAttack. |
| `wadsrc/static/zscript/actors/hexen/fighterhammer.zs` | 27 | Picks the offhand hammer as invoker and passes LAF_ISOFFHAND/ALF_ISOFFHAND to its melee and missile attacks. |
| `wadsrc/static/zscript/actors/strife/weaponmauler.zs` | 25 | Routes mauler fire through the invoking hand with LAF_ISOFFHAND/ALF_ISOFFHAND aim flags. |
| `wadsrc/static/zscript/actors/heretic/weaponcrossbow.zs` | 25 | Resolves the invoker's hand and tags every SpawnPlayerMissile with ALF_ISOFFHAND. |
| `wadsrc/static/zscript/actors/strife/weapondagger.zs` | 23 | Offhand-aware dagger jab with LAF_ISMELEEATTACK, and skips the doomYaw reset while playing in VR. |
| `wadsrc/static/zscript/actors/hexen/magelightning.zs` | 23 | Spawns lightning missiles with the offhand aim flag and depletes that hand's ammo; marks the weapon TWOHANDED. |
| `wadsrc/static/zscript/actors/hexen/clericmace.zs` | 17 | Selects the offhand mace as invoker and adds offhand aim/line-attack flags to both melee swings. |
| `wadsrc/static/zscript/actors/doom/weaponfist.zs` | 17 | Routes A_Punch through OffhandWeapon with offhand aim/attack flags and skips the turn-to-target in VR. |
| `src/g_statusbar/sbar_mugshot.cpp` | 17 | Fires a controller haptic blip on item pickup and counts offhand attacks toward the rampage face. |
| `src/common/platform/win32/i_input.cpp` | 17 | Adds I_AllowBackgroundGameInput so VR keeps taking input while unfocused, and starts the OpenXR input device. |
| `wadsrc/static/zscript/actors/hexen/magestaff.zs` | 16 | Threads the offhand aim flag through MStaffSpawn so Bloodscourge aims from whichever hand holds it. |
| `wadsrc/static/zscript/actors/doom/weaponssg.zs` | 15 | Fires the super shotgun from whichever hand invoked it, tagging the flash psprite and LineAttack. |
| `wadsrc/static/zscript/actors/strife/weaponassault.zs` | 13 | Routes the assault gun through OffhandWeapon with offhand aim/line-attack flags; fires the sound after the ammo check. |
| `wadsrc/static/zscript/actors/heretic/weaponstaff.zs` | 13 | Offhand-aware staff melee that skips the auto-yaw snap to target while playing in VR. |
| `wadsrc/static/zscript/actors/heretic/chicken.zs` | 13 | Uses the invoking hand's psprite layer for the beak and skips the yaw snap when playing in VR. |
| `wadsrc/static/zscript/actors/strife/weaponcrossbow.zs` | 12 | Picks the invoking hand's weapon for the flash psprite and passes its offhand aim flag. |
| `wadsrc/static/zscript/actors/player/player_inventory.zs` | 12 | Clears OffhandWeapon and picks a replacement for that hand when the held weapon is taken away. |
| `src/rendering/2d/v_blend.cpp` | 12 | Fires a short both-hands controller rumble and a 'pickup' haptic event on the item-pickup flash. |
| `src/playsim/p_effect.cpp` | 12 | Takes the rail attack sound from OffhandWeapon when RAF_ISOFFHAND, and defaults cl_rockettrails to sprite trails. |
| `wadsrc/static/zscript/actors/doom/weaponshotgun.zs` | 10 | Routes A_FireShotgun through OffhandWeapon, flashes the correct hand's psprite, and passes the offhand aim flag. |
| `wadsrc/static/engine/vr/defbind1.txt` **New** | 10 | New default bind set for the dual-wield scheme: switchhand, +oh_attack, +oh_altatk, stick weapon cycling. |
| `src/common/cutscenes/screenjob.cpp` | 10 | Honours the togglecheatmenu and menu_main bindings during cutscenes, not just console and screenshot. |
| `wadsrc/static/zscript/actors/strife/weaponmissile.zs` | 9 | Resolves the mini missile launcher's weapon from the invoking hand and spawns the missile with ALF_ISOFFHAND. |
| `wadsrc/static/zscript/actors/strife/weaponflamer.zs` | 9 | Resolves the Strife flamer to the offhand weapon and passes ALF_ISOFFHAND when spawning FlameMissile. |
| `wadsrc/static/zscript/actors/hexen/magecone.zs` | 9 | Routes the Frost cone through OffhandWeapon with the offhand aim flag; marks the weapon TWOHANDED. |
| `src/common/platform/win32/i_input.h` | 9 | Adds INPUT_OpenVR/INPUT_OpenXR joystick slots, their startup calls, and I_AllowBackgroundGameInput. |
| `wadsrc/static/filter/game-hexen/engine/defbinds.txt` | 8 | Strips Hexen's number-key artifact and showscores defaults, leaving invuseall so the VR bind sets own those keys. |
| `src/d_event.h` | 7 | Adds BT_OFFHANDATTACK/ALTATTACK/RELOAD and reuses the freed BT_SHOWSCORES bit for BT_MAINHANDRELOAD. |
| `src/common/console/c_cvars.h` | 7 | Declares C_GetExternalHapticLevelValue and friends it to FBaseCVar for ext_haptic_level_* lookup. |
| `wadsrc/static/zscript/actors/doom/weaponplasma.zs` | 6 | Resolves A_FirePlasma's weapon from the invoking hand and spawns the plasma ball with ALF_ISOFFHAND. |
| `src/gamedata/a_weapons.cpp` | 6 | Fires a 'pickup_weapon' haptic event at the configured external level when a weapon enters a slot. |
| `src/playsim/fragglescript/t_func.cpp` | 4 | Fires a VR_HapticEvent("doorclose") from FraggleScript's SF_CloseDoor. |
| `src/intermission/intermission.cpp` | 4 | Lets a key bound to togglecheatmenu through the intermission responder, and zero-inits the background FTextureID. |
| `src/d_buttons.h` | 4 | Declares the offhand attack/altattack/reload and mainhand reload buttons for dual-wield controller bindings. |
| `src/common/platform/win32/i_xinput.cpp` | 2 | Polls XInput through I_AllowBackgroundGameInput so pads keep working while the VR window is unfocused. |
| `src/common/platform/win32/i_keyboard.cpp` | 2 | Accepts key-down events while the desktop window is unfocused whenever a stereo mode is running. |
| `wadsrc/static/zscript/actors/strife/strifeplayer.zs` | 1 | Clears player.OffhandWeapon alongside ReadyWeapon when the Strife player's hands catch fire. |
| `wadsrc/static/filter/game-strife/defbind3.txt` **New** | 1 | Empty Strife slot in the numbered defbind0-3 set the fork's VR binding lumps are loaded from. |
| `wadsrc/static/filter/game-strife/defbind2.txt` **New** | 1 | Empty Strife filter override for the fork's VR default binding preset 2 (engine/vr/defbind2.txt). |
| `wadsrc/static/filter/game-strife/defbind1.txt` **New** | 1 | Placeholder Strife binding lump for the fork's cl_defaultconfiguration controller binding profiles. |
| `wadsrc/static/filter/game-strife/defbind0.txt` **New** | 1 | New comment-only Strife override so bind profile 0 supplies no game-specific defaults for that IWAD. |
| `wadsrc/static/filter/game-hexen/defbind0.txt` **New** | 1 | Reduces Hexen's game-specific default binds to a bare comment so the fork's controller bind profiles win. |
| `wadsrc/static/engine/vr/defbind2.txt` **New** | 1 | New: placeholder second VR bind set - header comment only, so the layout slot exists. |
| `wadsrc/static/engine/commonbinds.txt` | 1 | Removes the default `enter invuse` binding so Enter is free as a menu accept key. |
| `src/common/console/c_console.h` | 1 | Declares C_ScrollConsole so gamepad sticks/d-pad and the mouse wheel can scroll the console. |

### Performance — 49 files, ~3269 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/rendering/hwrenderer/scene/hw_bsp.cpp` | 970 | Adds a batched wall work queue with N worker threads/meshes plus decor-sprite distance culling. |
| `src/rendering/hwrenderer/scene/hw_walls.cpp` | 571 | Caps wall dynlights via a scored candidate budget plus distance culling, and adds a per-eye vertex cap. |
| `src/common/utility/TSQueue.h` **New** | 239 | New thread-safe queue plus ResourceLoader2 worker thread used by the Vulkan texture and model loaders. |
| `src/common/rendering/hwrenderer/data/hw_clock.cpp` | 236 | Adds VR pipeline, wall-worker and dynlight stat counters plus the VR summary in stat rendertimes. |
| `src/rendering/hwrenderer/scene/hw_flats.cpp` | 169 | Scores flat dynamic lights into a candidate budget and caps how many reach the draw per eye. |
| `src/common/rendering/vulkan/system/vk_renderdevice.h` | 111 | Declares the background texture and model loader threads and queues; also the per-eye layer index accessors. |
| `src/common/rendering/vulkan/textures/vk_hwtexture.cpp` | 105 | Adds a second mLoadedImage with background create, queue transfer and swap so uploads leave the render thread. |
| `src/common/rendering/hwrenderer/data/hw_cvars.cpp` | 92 | Adds the gl_texture_thread worker family, storage-buffer and global-fade cvars, retunes renderer defaults. |
| `src/rendering/hwrenderer/scene/hw_renderhacks.cpp` | 89 | Applies the same candidate-budget and distance-cull light limiting to render-hack (missing-texture) planes. |
| `src/common/rendering/hwrenderer/data/hw_dynlightdata.h` | 79 | Per-view caches for light PosRelative and distance culling, plus a bounded best-N candidate insert. |
| `src/rendering/hwrenderer/hw_dynlightdata.cpp` | 57 | Adds per-view caches for spotlight cosines and relative light positions behind five gl_light_*_cache cvars. |
| `src/rendering/hwrenderer/scene/hw_spritelight.cpp` | 47 | Adds distance culling, a gather-id dedupe cache and telemetry counters to the model dynlight gather. |
| `src/rendering/hwrenderer/scene/hw_decal.cpp` | 39 | Queues a decal's texture into the background cache and draws its previous patch until the real one is resident. |
| `src/rendering/hwrenderer/scene/hw_sky.cpp` | 35 | Defers sky portal info when the worker thread has no drawinfo, and adds the gl_skydome toggle. |
| `wadsrc/static/shaders/glsl/material_normal.fp` | 34 | Adds the SHADER_LITE cheap per-light path and uLightRangeLimit early-outs in the light loops. |
| `src/common/rendering/hwrenderer/data/hw_cvars.h` | 34 | Declares the light-budget, distance-cull, dynlight-cache and texture-thread cvars, plus vr_scene_multithread. |
| `src/rendering/hwrenderer/hw_precache.cpp` | 33 | Routes precaching through PrequeueMaterial and BackgroundLoadModel when the texture thread is enabled, then flushes. |
| `src/common/rendering/vulkan/system/vk_commandbuffer.cpp` | 31 | Parameterises the command-buffer manager by queue/family so the background upload thread gets its own pool. |
| `src/common/models/models_md2.cpp` | 28 | Splits LoadGeometry into a FileData overload so the background loader thread can parse DMD/MD2 geometry. |
| `src/playsim/a_dynlight.cpp` | 24 | Adds the gl_light_* budget and cull cvars with radius clamps, plus dynlight link/collect instrumentation counters. |
| `src/common/rendering/v_video.h` | 22 | Adds the background cache/model-stream virtuals to DFrameBuffer, plus NewRefreshRate, Draw2D(outside2D) and a 0.5 znear. |
| `src/playsim/a_dynlight.h` | 18 | Adds per-view distance-cull, spot-cone and pos-relative caches to FDynamicLight for the light budget. |
| `src/common/models/models_iqm.cpp` | 18 | Adds the FileData LoadGeometry overload plus an early-out when the IQM vertices are already loaded. |
| `src/rendering/hwrenderer/scene/hw_skyportal.cpp` | 17 | Adds a gl_skydome=0 flat sky-cap fast path and strips stereo parallax before drawing the dome. |
| `src/common/rendering/hwrenderer/data/hw_clock.h` | 16 | Externs for the VR pipeline, wall-worker and dynlight stat counters declared in hw_clock.cpp. |
| `src/rendering/hwrenderer/scene/hw_fakeflat.cpp` | 14 | Checks side texture validity by TextureID instead of calling TexMan from the threaded clip test. |
| `src/common/models/model.h` | 14 | Adds NONE/LOADING/READY load state and a LoadGeometry(FileData*) hook for background model loading. |
| `src/common/rendering/vulkan/system/vk_commandbuffer.h` | 13 | Takes an explicit queue/family plus uploadOnly, adds unmanaged command buffers for the texture threads and a present-acquire flag. |
| `wadsrc/static/shaders/pp/ssao.fp` | 12 | Fades ambient occlusion out with distance using the global-fade density/gradient curve instead of writing raw occlusion. |
| `src/common/textures/hw_ihwtexture.h` | 12 | Adds the NONE/CACHING/LOADING/READY hardware state the Vulkan background texture uploader tracks. |
| `src/common/models/models_md3.cpp` | 12 | Splits the MD3 parse into LoadGeometry(FileData*) and skips it when surfaces are already loaded. |
| `src/rendering/hwrenderer/scene/hw_walldispatcher.h` | 10 | Adds per-worker wall/batch/cycle counters and moves walls into the mesh lists instead of copying them. |
| `src/g_cvars.cpp` | 8 | Lowers stock defaults for VR headroom: dynamic lights off, corpse queue 10, max decals 20. |
| `src/common/rendering/vulkan/textures/vk_hwtexture.h` | 8 | Adds the background upload path: BackgroundCreateTexture, queue release/acquire, and the staged mLoadedImage swap. |
| `src/common/rendering/hwrenderer/data/hw_lightbuffer.cpp` | 8 | Makes the 80000-light buffer size the gl_max_lights cvar and logs buffer exhaustion instead of failing silently. |
| `src/common/models/model.cpp` | 6 | Resets loadState when the vertex buffer is destroyed and adds the FileData LoadGeometry base that fatals for unsupported formats. |
| `src/rendering/hwrenderer/scene/hw_clipper.h` | 5 | Adds GetRadarClipAngle, a per-vertex angle cache so the fog-of-war radar clipper stops recomputing pseudo-angles. |
| `src/common/models/model_md2.h` | 5 | Declares the LoadGeometry(FileData*) overrides and drops mLumpNum for background model streaming. |
| `src/rendering/swrenderer/scene/r_opaque_pass.cpp` | 4 | Turns the software renderer's sprite and line distance culls on by default (2000 and 4000 units). |
| `src/playsim/a_decals.cpp` | 3 | Initialises DBaseDecal::LastPatch in all three constructors so the threaded texture cache has a valid fallback. |
| `src/common/scripting/vm/vmframe.cpp` | 3 | Breaks the VM stat readout onto two lines for the stats overlay, and adds a missing va_end. |
| `src/common/rendering/hwrenderer/data/hw_bonebuffer.cpp` | 3 | Records bonebuffer_curindex for the buffer stats readout and warns when the bone buffer runs out. |
| `src/common/models/model_iqm.h` | 3 | Declares the IQM LoadGeometry(FileData*) override and removes the per-model lump number. |
| `src/rendering/swrenderer/drawers/r_draw_rgba.cpp` | 2 | Flips the r_mipmap default to false in the truecolor software drawers. |
| `src/playsim/a_sharedglobal.h` | 2 | Adds DBaseDecal::LastPatch, the fallback texture drawn while the real decal texture loads in background. |
| `src/common/rendering/hwrenderer/data/hw_shadowmap.cpp` | 2 | Drops gl_shadowmap_quality's default from 1024 to 128 for VR. |
| `src/common/rendering/hwrenderer/data/flatvertices.cpp` | 2 | Publishes the flat vertex buffer's mCurIndex into vertexbuffer_curindex for the stat readout. |
| `src/common/rendering/hwrenderer/data/buffers.h` | 2 | Comments out HW_BLOCK_SSBO so allowSSBO() is unconditionally true on every pipeline. |
| `src/common/models/model_md3.h` | 2 | Adds the FileData-taking LoadGeometry override for background model loading and drops mLumpNum now owned by FModel. |

### Android/Quest — 33 files, ~2280 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/posix/nosdl/i_system.cpp` **New** | 492 | No-SDL POSIX system layer (startup, timing, error and stub paths) used by the Android/Quest build. |
| `src/posix/nosdl/crashcatcher.c` **New** | 428 | New signal-handler crash dumper (forks a gdb/sysinfo reporter) for the no-SDL POSIX/Quest build. |
| `src/posix/nosdl/st_start.cpp` **New** | 328 | New terminal-only startup screen for the SDL-less mobile build, with Android log redirection. |
| `src/posix/nosdl/glvideo.cpp` **New** | 236 | No-SDL GL video backend for the Quest build; creates the OpenGL framebuffer with no window system. |
| `src/posix/nosdl/i_joystick.cpp` **New** | 108 | Stub joystick manager for the no-SDL Quest build; no devices, no axes, use_mouse kept for scripts. |
| `src/posix/nosdl/glvideo.h` **New** | 89 | Declares NoSDLGLVideo and NoSDLGLFB, the video/framebuffer classes for the no-SDL Android build. |
| `src/posix/nosdl/hardware.cpp` **New** | 85 | New SDL-free video/input hardware layer used by the Quest build in place of the SDL one. |
| `src/common/rendering/gl_load/gl_load.c` | 72 | Adds the __MOBILE__ proc loader that dlopens libGLESv3/GL4ES and stubs entry points it cannot resolve. |
| `src/posix/nosdl/i_input.cpp` **New** | 64 | New: empty I_GetEvent/I_StartTic/mouse-capture stubs so the headless no-SDL mobile build links. |
| `src/common/audio/sound/oalsound.cpp` | 60 | Downmixes multi-channel samples to mono on mobile and pauses/resumes the OpenSL device with app focus. |
| `src/posix/nosdl/gl_sysfb.h` **New** | 56 | New SDL-free SystemGLFrameBuffer header for the Quest/Android GL framebuffer path. |
| `src/posix/nosdl/i_gui.cpp` **New** | 43 | New stub file providing a no-op I_SetCursor so the no-SDL POSIX/Quest target links. |
| `src/common/rendering/gl/gl_renderbuffers.cpp` | 32 | GLES fallbacks: RGBA8_SNORM SSAO noise texture, glTexStorage2DMultisample, float depth-clear query. |
| `src/common/platform/posix/sdl/i_system.cpp` | 23 | Routes fatal errors and messages to the Android log and LogWritter on both of upstream's batchrun paths. |
| `src/common/platform/posix/sdl/st_start.cpp` | 22 | Redirects startup progress printf into the Android on-screen console box via addTextConsoleBox. |
| `src/common/rendering/gl_load/gl_interface.cpp` | 19 | Sets GLES 3.31 caps for mobile and honours gl_no_ssbo/gl_no_persistent_buffer/gl_no_clip_planes overrides. |
| `src/m_misc.cpp` | 17 | Adds M_GetActiveProfile(), reading cmdlineprofile from GlobalSettings so commandline.txt can be per-profile. |
| `src/common/rendering/gl/gl_shaderprogram.cpp` | 13 | Forces #version 310 es and highp precision qualifiers into the GL shader preamble under __MOBILE__. |
| `src/common/rendering/gl/gl_hwtexture.cpp` | 11 | Under __MOBILE__ forces BGRA upload format and maps the pixel-unpack buffer with glMapBufferRange plus a tracked size. |
| `src/common/rendering/gl_load/gl_load.h` | 10 | Aliases glClearDepth and glDepthRange to the GLES float entry points under __MOBILE__. |
| `src/doomtype.h` | 9 | Defines the LOGI android-log macro used by the touch-control and mobile code paths. |
| `src/common/platform/posix/sdl/i_main.cpp` | 9 | Renames main() to main_android() and forces a delayed exit after SDL_Quit on Android. |
| `src/common/utility/m_alloc.cpp` | 8 | Excludes Android from the malloc_usable_size/_msize allocation-accounting paths. |
| `src/common/utility/findfile.cpp` | 8 | Checks ./res/<file> first on __MOBILE__ builds before the normal WAD search order. |
| `src/common/console/c_dispatch.cpp` | 7 | Adds the extern "C" C_DoCommandC entry for QzDoom C code; unknown commands drop to DPrintf. |
| `src/common/platform/posix/sdl/i_input.cpp` | 6 | Re-clears the SDL relative-mouse delta after enabling relative mode on __MOBILE__, and maps SDLK_SYSREQ. |
| `src/common/utility/engineerrors.cpp` | 5 | Mirrors fatal error text to Android logcat via LOGI before the normal error path. |
| `wadsrc/static/shaders/pp/tonemap.fp` | 4 | Makes the two tonemap literals explicit floats so the stricter GLES shader compiler accepts them. |
| `src/common/rendering/gl_load/gl_system.h` | 4 | Defines ES_VERSION_STR as "#version 310 es" for the mobile GL shader path. |
| `src/common/rendering/gl/gl_hwtexture.h` | 4 | Adds a `size` member to the GL hardware texture on __MOBILE__ builds only. |
| `src/common/platform/posix/sdl/i_joystick.cpp` | 4 | Skips SDL joystick shutdown on Android because it crashed, and adds the cstdint/cstdlib includes. |
| `wadsrc/static/shaders/pp/colormap.fp` | 2 | Compares uFixedColormapRange.a against a float literal so GLSL ES accepts the pass. |
| `src/common/console/c_dispatch.h` | 2 | Declares extern "C" C_DoCommandC so the Quest-side C VR code can run console commands. |

### Engine plumbing — 30 files, ~7553 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/scripting/vmthunks.cpp` | 3546 | Adds ~120 FLevelLocals/Sector/Side natives exporting every fork renderer and VR system to ZScript. |
| `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` | 939 | Per-frame upload of glow-wave, beam, sweep-fill, darkness, fog-slab, tornado and shape uniforms, with beam tic interpolation. |
| `src/g_levellocals.h` | 845 | Declares FBillboard/FBillboardGroup/BillboardBasis plus every script-published render field: sweep, beams, shapes, fog slab, heatmap. |
| `wadsrc/static/zscript/doombase.zs` | 606 | Declares the fork's LevelLocals natives script-side: billboards, fog slab, shapes, beams, sweep, VR hooks. |
| `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h` | 357 | Grows the scene-global viewpoint UBO by ~40 vec4s and pads it to std140 array stride. |
| `src/common/rendering/hwrenderer/data/hw_renderstate.h` | 240 | Adds the sweep-band, flat-glow, glow-far and global-fade fields to StreamData with their setters and clears. |
| `src/common/rendering/vulkan/shaders/vk_shader.cpp` | 230 | Declares the fork's viewpoint uniforms and turns ViewpointUBO into a two-element array indexed by gl_ViewIndex. |
| `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs` | 176 | Adds submenu label colour, StaticPatch and a keyboardless CommandInput item; re-merges DoubleControl/StaticText signatures. |
| `src/menu/profiledef.cpp` **New** | 152 | New: scans commandline_*.txt launch profiles and the cmdlineprofile CVAR that strips argv and restarts. |
| `src/common/rendering/gl/gl_shader.cpp` | 140 | Declares the fork's whole extra ViewpointUBO vec4 set in GLSL and binds every new uniform location. |
| `src/common/rendering/gl/gl_renderstate.cpp` | 83 | Uploads the fork's new stream-data uniforms on GL: glow far/falloff, sweep bands, flat-glow lines, global fade. |
| `src/gameconfigfile.cpp` | 63 | Version-gated resets of fork-tuned cvar defaults, Android search paths, and a null-Args guard. |
| `src/common/engine/m_random.cpp` | 43 | Restores vanilla Doom's rndtable/P_Random and M_ClearRandom, reset at level start and demo playback. |
| `src/g_level.cpp` | 34 | Hooks level start/load to destroy the VR HUD surface, reset the weapon wheel and relink dynamic lights. |
| `src/menu/profiledef.h` **New** | 22 | New ProfileManager header for the commandline_*.txt launch profiles the options menu lists. |
| `src/common/utility/m_argv.cpp` | 17 | Makes FArgs::RemoveArgs strip every occurrence, so a profile switch clears all -file/-iwad instances. |
| `src/playsim/dthinker.cpp` | 14 | Routes deserialization through CallPostSerialize so a ZScript Thinker.OnLoad override runs after a savegame load. |
| `src/common/utility/configfile.h` | 11 | Adds FCmdFile, an FConfigFile subclass exposing ReadLine so startup can parse commandline.txt files. |
| `src/common/platform/win32/i_main.cpp` | 7 | Adds I_FocusWindow, raising and focusing the game window before the main loop starts. |
| `src/common/engine/m_random.h` | 6 | Re-declares the vanilla lookup-table P_Random, M_ClearRandom and prndindex so callers outside m_random.cpp can use them. |
| `src/rendering/hwrenderer/scene/hw_setcolor.cpp` | 4 | Keeps fog enabled at zero density so the global fade path still runs, and null-guards cmap. |
| `src/rendering/hwrenderer/scene/hw_clipper.cpp` | 4 | Switches two frustum FOV reads to the viewpoint's GetFieldOfView() accessor. |
| `src/common/platform/posix/cocoa/i_video.mm` | 3 | Empty macOS I_FocusWindow so the fork's new startup focus call in d_main links on Cocoa. |
| `src/doomdef.h` | 2 | Declares COMPATF2_OLD_RANDOM_GENERATOR, the vanilla RNG-table compat flag wired into compatmode 1 and 2. |
| `src/common/textures/hw_material.cpp` | 2 | Makes gl_customshader archived and user-settable again so the GL prefs menu can toggle it. |
| `src/common/rendering/vulkan/shaders/vk_shader.h` | 2 | Drops padding1 so the C++ push-constant struct matches the GLSL block, which declares only two paddings. |
| `src/common/engine/d_gui.h` | 2 | Renames the unused GK_FREE2 key slot to GK_SYSRQ so the SDL input path can report PrtSc. |
| `src/playsim/fragglescript/t_parse.cpp` | 1 | Adds the missing va_end before script_error throws, closing the varargs list. |
| `src/playsim/dthinker.h` | 1 | Declares DThinker::CallPostSerialize, the dispatcher for the fork's new ZScript Thinker.OnLoad virtual. |
| `src/common/platform/posix/cocoa/i_input.mm` | 1 | Maps macOS kVK_F13 to GK_SYSRQ in the menu keyboard event handler. |

### Billboards — 19 files, ~2696 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/rendering/hwrenderer/scene/hw_sprites.cpp` | 1130 | Adds ProcessBillboard and every BB_* payload emitter, plus billboard colour/no-depth draw state. |
| `src/rendering/hwrenderer/scene/hw_sdffont.cpp` **New** | 325 | Loads the offline SDF glyph atlas and metrics lumps for BB_TEXT, caches failures, runs the font roster. |
| `src/rendering/hwrenderer/scene/hw_sdffont.h` **New** | 253 | FSDFFontRoster glyph cache and metrics table for the offline SDF atlas that billboard text draws from. |
| `src/p_tick.cpp` | 245 | Adds TickBillboards, the bb_spawn/bb_text/bb_clear debug CCMDs, and the per-tic beam interpolation snapshot. |
| `wadsrc/static/shaders/glsl/func_segment.fp` **New** | 166 | Sixteen-segment display shader: builds each character from capsule distance fields carried in uAddColor, with glow. |
| `wadsrc/static/sdffonts/sdfmono.txt` **New** | 101 | Generated glyph metrics for the default monospace SDF atlas that BB_TEXT and BB_SEGMENT draw from. |
| `wadsrc/static/shaders/glsl/func_wg13.fp` **New** | 97 | BB_WG13 payload shader: GITD kill badge, plate and 7-segment digits punched in one pass from uAddColor. |
| `wadsrc/static/shaders/glsl/func_seam.fp` **New** | 94 | New BB_SEAM payload shader: a glowing slit with a hot centre line and a void-mode bright rim. |
| `src/p_saveg.cpp` | 93 | Serializes FBillboard/FBillboardGroup and the level's billboard arrays; also carries ohattackdown across player copies. |
| `src/rendering/hwrenderer/scene/hw_drawstructs.h` | 72 | Adds HWSprite billboard corners, glow/gradient colours, FBillboardUV, and the payload and segment emit hooks. |
| `wadsrc/static/shaders/glsl/func_sdftext.fp` **New** | 70 | Signed-distance glyph shader for billboard text: fwidth antialiasing plus a uAddColor-driven neon halo. |
| `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp` | 17 | Registers the five fork material shaders: SDF Text, Segment, Seam, WG13 and SDF Panel, all nolight. |
| `src/common/textures/textures.h` | 15 | Registers SDFText/Segment/Seam/WG13/SDFPanel shader indices, plus TEXF_FlipNormal and the translucent-canvas flag. |
| `src/common/scripting/backend/codegen.cpp` | 12 | Adds TextureID.GetIndex() so script can pass a texture as the plain int a billboard payload takes. |
| `wadsrc/static/zscript/engine/base.zs` | 6 | Exposes TextureID.GetIndex() for billboard texture payloads and adds the CHAN_OFFWEAPON sound channel. |
| `wadsrc/static/graphics/sdfmono.png` **New** | 0 | The signed-distance-field glyph atlas BB_TEXT samples; bb_sdffont defaults to this lump. |
| `wadsrc/static/graphics/bbwhite.png` **New** | 0 | New 79-byte white texture that BB_BAR/BB_SEAM/BB_WG13/SDF payloads draw their untextured quads with. |
| `wadsrc/static/graphics/bbring.png` **New** | 0 | Ring glyph texture fetched by GetBillboardShape("bbring") for the ring billboard payload. |
| `wadsrc/static/graphics/bbpanel.png` **New** | 0 | The panel shape texture GetBillboardShape("bbpanel") loads for the built-in panel billboard payload. |

### Multiplayer — 15 files, ~2019 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/common/engine/i_net.cpp` | 450 | Mirrors lobby wait state into a session the headset shell renders; adds local-IP lookup and cancel-to-singleplayer. |
| `src/menu/doommenu.cpp` | 386 | Builds the host/join menu option groups, the mp_launch_host/join CCMDs, and the local-IP line on the host menu. |
| `wadsrc/static/zscript/otherplayertags.zs` **New** | 280 | New VisualThinker painting each remote player's name and health bar on a canvas, scaled differently in VR. |
| `src/common/engine/multiplayerlaunch.cpp` **New** | 262 | New: pending host/join launch state, map validation, and the transient -host/-join argv the VR menus commit. |
| `wadsrc/static/zscript/engine/ui/menu/chattextentermenu.zs` **New** | 255 | New modal chat entry menu with a 13x5 character grid, usable with no physical keyboard. |
| `src/d_net.cpp` | 112 | Adds vr_switchhand/vr_moveweaphand net commands, 32-bit roomscale teleport, and skips input/frame-pacing blocking in VR. |
| `src/d_protocol.cpp` | 86 | Widens the usercmd packing flag word to 16 bits and packs/unpacks/skips the VR weaponpitch and weaponyaw fields. |
| `src/ct_chat.cpp` | 66 | Routes messagemode/messagemode2 into a ZScript text-entry menu instead of the raw in-viewport chat prompt. |
| `src/common/engine/i_net.h` | 50 | Declares the net-wait session enums and API that the VR lobby shell and the menus consume. |
| `src/d_netinfo.cpp` | 37 | Generates a unique 'Player#nnn' default name, adds cl_otherplayernames/health cvars, zeroes the movebob default. |
| `wadsrc/static/animdefs.txt` | 16 | Declares the eight OPLTAG and eight OPLVR canvas textures the other-player name tag renderer draws into. |
| `src/common/engine/multiplayerlaunch.h` **New** | 11 | New header declaring the pending host/join launch API that the multiplayer menu CCMDs call. |
| `src/d_protocol.h` | 6 | Adds weaponpitch/weaponyaw to usercmd with delta flags so VR hand aim survives the net. |
| `wadsrc/static/mapinfo/common.txt` | 1 | Auto-adds the OtherPlayerTagHandler and VRStabilizeSyncHandler event handlers to every game. |
| `src/menu/doommenu.h` | 1 | Declares M_BuildMultiplayerOptionGroups, the builder behind the fork's host/join menu. |

### Build — 14 files, ~9303 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/utility/data/xg.h` **New** | 5185 | Generated XG instrument bank byte array emitted by wopn2hpp.sh; vendored data, not hand-authored. |
| `src/gamedata/xlat/xlat_parser.c` **New** | 1999 | Generated LEMON parser output for the xlat grammar, checked into the tree instead of built. |
| `src/gamedata/xlat/xlat_parser.out` **New** | 1849 | Checked-in lemon parser report for the xlat grammar; generated artifact, not hand-written. |
| `src/CMakeLists.txt` | 157 | Adds ENABLE_OPENVR/ENABLE_OPENXR options, SDK discovery, the stereo3d/QzDoom/vrwheel source lists and runtime DLL install rules. |
| `src/gamedata/xlat/xlat_parser.h` **New** | 39 | Generated token defines accompanying the checked-in xlat LEMON parser. |
| `src/win32/QZDoomVR.bat` **New** | 23 | New OpenVR launcher batch file presetting vr_mode 10, HUD mount cvars and video defaults. |
| `wadsrc/CMakeLists.txt` | 22 | Renames the pk3 target to doomxr.pk3 and documents why fork strings live in language.0, not language.csv. |
| `src/common/utility/cmdlib.cpp` | 13 | Adds a MinGW branch to DoCreatePath using _splitpath/_makepath instead of the MSVC _s variants. |
| `src/common/scripting/frontend/zcc_parser.cpp` | 13 | Records removal of the checked-in zcc-parse snapshot that shadowed the generated one; version mismatch now warns, not errors. |
| `src/common/utility/configfile.cpp` | 3 | Adds cmdlib.h and engineerrors.h includes only; nothing else in the file changed. |
| `wadsrc/static/sounds/dsgrnexp.ogg` **New** | 0 | Adds the missing weapons/grenlx grenade explosion sound lump the Grenade actor's DeathSound needs. |
| `wadsrc/static/sounds/dsglaunc.ogg` **New** | 0 | New packaged sound lump backing the existing weapons/grenlf sndinfo mapping used by the Grenade actor. |
| `wadsrc/static/sounds/dsbounce.ogg` **New** | 0 | Ships the missing weapons/grbnce bounce sound lump the Skulltag-derived Grenade actor already referenced. |
| `src/utility/data/xg.wopn` **New** | 0 | Vendored WOPN instrument bank binary loaded by name from i_music.cpp; data artifact, not code. |

### Upstream merge fix — 13 files, ~386 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/zscript/actors/player/player_inventory.txt` **New** | 359 | Unreferenced pre-fork copy of player_inventory.zs left by a lineage merge; zscript.txt never includes it. |
| `src/d_iwad.cpp` | 6 | Records dropping the fork's tri-state i_loadsupportwad and reads the language via D_GetStartupLanguage(). |
| `src/rendering/hwrenderer/scene/hw_drawlistadd.cpp` | 4 | Drops the now-unused gl_seamless extern and corrects a stale FDrawInfo::AddFlat comment. |
| `src/playsim/p_trace.cpp` | 4 | Two pointer declarations restored to the fork's spacing style; no behaviour change. |
| `src/gamedata/statistics.cpp` | 3 | Adds the d_player.h include the fork's headers no longer supply, and wraps the per-level stats line onto two lines. |
| `src/common/rendering/gl/gl_samplers.cpp` | 2 | Drops the fork's redundant filter>0 anisotropy guard and comments why it was unnecessary on both branches. |
| `src/common/audio/music/i_soundfont.cpp` | 2 | Duplicate identical #define SF_LOG left by the 5.0.0 merge; harmless redefinition. |
| `src/sound/s_sound.h` | 1 | Drops the now-redundant struct FLevelLocals forward declaration. |
| `src/scripting/thingdef_data.cpp` | 1 | A single stray blank line left in InitThingdef by the 5.0.0 merge; no functional change. |
| `src/gamedata/textures/animations.cpp` | 1 | Clears mAnimationIndices in DeleteAll so a texture reload cannot keep stale animation indices. |
| `src/common/rendering/hwrenderer/data/hw_lightbuffer.h` | 1 | Adds a second, redundant <atomic> include ahead of the one upstream already has; merge leftover. |
| `src/common/engine/serializer.cpp` | 1 | A duplicated `/*` at the head of the licence comment, a harmless 5.0.0 merge artifact. |
| `src/common/audio/sound/s_environment.cpp` | 1 | Adds the doomtype.h include the file needs to compile after the 5.0.0 merge. |

### VR HUD — 12 files, ~2148 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/zscript/engine/ui/menu/cheatmenu.zs` **New** | 1255 | New controller-driven cheat/spawner/level-select menu opened by CCMD togglecheatmenu, paged per game type. |
| `src/d_main.cpp` | 746 | Draws status bar and automap into the offscreen VR HUD surface; gates the flat 2D layer on IsVR(). |
| `src/common/rendering/hwrenderer/hw_draw2d.cpp` | 50 | Splits 2D commands by mOutside2D and gives the mounted HUD/automap canvas its own viewport and projection. |
| `src/common/2d/v_2ddrawer.cpp` | 28 | Tracks whether queued 2D commands are inside or outside the 2D layer so VR can draw them in separate passes. |
| `src/g_statusbar/shared_sbar.cpp` | 21 | Skips the status bar background when drawing to the portable HUD canvas and suppresses the 2D crosshair in VR. |
| `src/common/2d/v_draw.cpp` | 18 | Adds VR_GetUIScale and marks DoDim's color quad as an outside-2D command whenever a stereo mode is running. |
| `wadsrc/static/zscript/ui/statusbar/alt_hud.zs` | 12 | Alt HUD lists ammo and highlights weapon icons for the offhand weapon as well as the ready weapon. |
| `src/common/rendering/gles/gles_framebuffer.cpp` | 6 | Threads the outside2D flag through Draw2D and uses the cached VR mode when adjusting the viewport. |
| `wadsrc/static/zscript.txt` | 5 | Registers the fork's added scripts: cheat menu, on-screen console/chat entry, player tags, VR stabilize sync. |
| `src/common/2d/v_draw.h` | 4 | Draw2D gains an outside2D flag selecting the full-screen VR 2D pass instead of the HUD quad. |
| `wadsrc/static/zscript/engine/ui/menu/loadsavemenu.zs` | 2 | Uses the full screen width for the 4:3 save box when the aspect ratio is narrower than 1.3. |
| `src/g_statusbar/sbar.h` | 1 | Declares the gPortableHudCanvasRender flag that the portable-HUD canvas pass uses to skip background refresh. |

### Branding — 12 files, ~693 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/language.0` **New** | 524 | New 30-language string table for every fork-only menu key (VRLASERMNU, VRPREFMNU, VRHUDMNU, VRWHEELMNU). |
| `wadsrc/static/zscript/engine/ui/menu/optionmenu.zs` | 106 | Adds AutoScroll with smooth sub-row offset and looping for the DoomXR instructions/credits pages. |
| `src/win32/zdoom.rc` | 26 | Renames the Win32 version resource, filenames and crash dialog caption from UZDoom to DoomXR. |
| `src/version.h` | 17 | Renames GAMENAME/BASEWAD/APPID to DoomXR and bumps MINSAVEVER to 4558. |
| `wadsrc/static/menudef.zsimple` | 8 | Replaces the simple-menu options list with the fork's VR options, perf, mod and multiplayer submenus. |
| `wadsrc/static/language.1` **New** | 6 | New language table adding rocket-explosion style, powerup fade and footstep volume menu strings. |
| `src/posix/freedesktop/org.zdoom.UZDoom.desktop` | 4 | Renames the freedesktop launcher entries from UZDoom to DoomXR. |
| `src/posix/osx/zdoom-info.plist` | 2 | macOS bundle name changed from UZDoom to DoomXR. |
| `wadsrc/static/widgets/banner.png` **New** | 0 | New fork banner artwork shipped in the pk3's widgets folder. |
| `wadsrc/static/graphics/bootlogo.png` | 0 | Replaces the stock startup logo bitmap with the fork's own DoomXR boot image. |
| `wadsrc/static/graphics/QZDOOM.png` **New** | 0 | Adds the QuestZDoom logo graphic to the base wad; nothing in the tree references it yet. |
| `wadsrc/static/graphics/DOOMXR.png` **New** | 0 | New DOOMXR logo patch drawn by the info and credits menu pages in menudef.txt. |

### Bloom — 5 files, ~235 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp` | 117 | Bloom threshold/knee, anamorphic blur, tint and fringing; also runs the new volbeam and heatmap passes before bloom. |
| `src/common/rendering/hwrenderer/postprocessing/hw_postprocess_cvars.cpp` | 53 | Turns gl_bloom on by default and adds threshold, soft knee, anamorphic ratio, tint and chromatic-fringe cvars. |
| `wadsrc/static/shaders/pp/bloomextract.fp` | 32 | Replaces the hard 1.0 cutoff with a soft-knee threshold judged on the brightest channel. |
| `wadsrc/static/shaders/pp/bloomcombine.fp` | 24 | Applies bloom tint and radial chromatic fringing in the final combine; stays neutral on downscale passes. |
| `src/common/rendering/hwrenderer/postprocessing/hw_postprocess_cvars.h` | 9 | Declares the bloom threshold/knee/anamorphic/tint/chromatic cvars, and re-declares vid_brightness that upstream removed. |

### Surface glow — 4 files, ~2655 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/shaders/glsl/main.fp` | 2522 | Hosts every fork surface term: glow lanes and recolour, wave, in-glow texture, sweep, beams, fog slab, SDF shapes. |
| `src/gamedata/r_defs.h` | 62 | Adds far colour, falloff, intensity and flat-edge glow fields to sector planes and per-side glow. |
| `wadsrc/static/zscript/mapdata.zs` | 46 | Declares the Side and Sector glow natives: far colour, falloff, intensity and flat-edge glow. |
| `src/common/rendering/gl/gl_shader.h` | 25 | Adds GL uniform handles for far glow colours, falloffs, sweep bands, flat-glow lines and global fade. |

### Non-pausing menus — 3 files, ~154 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/common/menu/menu.cpp` | 117 | Adds DMenu::DontPause plus M_MenuPauses, and wraps the menu ticker in InMenu so cvars apply live. |
| `src/common/menu/menu.h` | 29 | Adds DMenu::DontPause and M_MenuPauses, plus autoscroll fields, M_RequestMenuRebuild and the mouse-override extern. |
| `wadsrc/static/zscript/engine/ui/menu/menu.zs` | 8 | Declares the native DontPause flag and lets vr_menu_pointer drive menu mouse-move events without m_use_mouse. |

### Laser sight — 3 files, ~1670 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/rendering/hwrenderer/scene/hw_weapon.cpp` | 1657 | Adds DrawLaserSightWorld/GetLaserBeamEndpoints, ~50 vr_laser_* cvars, hitscan tracer geometry, and the world-space VR HUD quad. |
| `src/events.cpp` | 7 | Queues a hitscan tracer on each WorldHitscanFired event and clears the tracer list on level load/unload. |
| `src/namedef.h` | 6 | Adds LaserBeamOffset/LaserBeamColor/HitscanTracerOffset/SlotNumber names, plus GetIndex and CheatMenu. |

### HUD stereo gating — 3 files, ~60 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/common/rendering/gl/gl_postprocess.cpp` | 46 | Splits the GL eye loop's Draw2D into gated inside/outside passes and routes present plus net-wait shell through vrmode. |
| `src/common/2d/v_2ddrawer.h` | 12 | Tags 2D commands with mOutside2D and adds HasCommandsForPass so full-screen VR quads draw in their own pass. |
| `src/common/rendering/gles/gles_framebuffer.h` | 2 | Matches the GLES framebuffer's Draw2D override to the new outside2D pass parameter. |

### Direct model frame addressing — 3 files, ~253 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/r_data/models.cpp` | 170 | Passes the DPSprite down so ModelFrame, the remap table and psp.scale override the sprite lookup. |
| `src/playsim/p_pspr.h` | 73 | Adds ModelFrame/ModelFrameNext/ModelFrameLerp and per-psprite Tint/Glow to DPSprite, plus PSP_OFFHANDWEAPON. |
| `src/r_data/models.h` | 10 | Threads a const DPSprite* through CalcModelFrame/CalcModelOverrides so psprite frame overrides reach the model path. |

### Volumetric beam — 2 files, ~495 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/shaders/pp/volumetricbeam.fp` **New** | 254 | View-space cone raymarch with dust noise, bounded by an analytic ray/cone intersection. |
| `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h` | 241 | Adds the beam's view-space uniform block and PPVolumetricBeam, with the std140 row-by-row layout proof. |

### VR weapon wheel — 2 files, ~2398 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp` **New** | 2374 | Whole radial weapon/inventory wheel: pointer/touch/aim/stick selection, icon and model drawing, open/close netevents. |
| `src/common/rendering/hwrenderer/data/hw_vrwheel.h` **New** | 24 | New header declaring the wheel's open/close, per-hand and stick input suppression, transform and draw entry points. |

### The heatmap — 1 files, ~92 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/shaders/pp/heatmap.fp` **New** | 92 | New postprocess pass reading depth plus two R32f intensity/height textures to paint combat heat on the floor. |

### Panel as a field — 1 files, ~175 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `wadsrc/static/shaders/glsl/func_sdfpanel.fp` **New** | 175 | New SHADER_SDFPanel shader: the rounded-rect panel solved per pixel, with border and halo. |

### Native state remap — 1 files, ~232 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/playsim/p_actionfunctions.cpp` | 232 | Adds RegisterModelStateFrame, state-label enumeration and rs_remap_dump; also VR recoil/haptic cvars. |

### Hitscan tracers and ricochet — 1 files, ~24 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/playsim/p_hitscantracer.h` **New** | 24 | New FHitscanTracer record and the queue/clear/fetch API for visible tracer and ricochet rounds. |

### Beams — 1 files, ~30 lines

| File | Lines | What the fork does there |
| --- | ---: | --- |
| `src/p_setup.cpp` | 30 | Zeroes beam count and previous intensities on level teardown so a beam cannot survive a map change. |

---

## Where to read more

| Document | Covers |
| --- | --- |
| [`FORK_CHANGES.md`](FORK_CHANGES.md) | The engineering write-up. 33 sections, file references, and the reasoning behind each decision. |
| [`BILLBOARDS.md`](BILLBOARDS.md) | The billboard system and its eleven payload types, in full. |
| [`REFLECTION_SPEC.md`](REFLECTION_SPEC.md) | The field reflection API. |
| [`HUD_STEREO_GATING.md`](HUD_STEREO_GATING.md) | One bug traced end to end — a fair example of how this fork documents itself. |
| [`README.md`](README.md) | What the features are, for someone deciding whether they want them. |

---

*Generated from `git diff 5.0.0-rc.2 HEAD`. Regenerate after any upstream merge.*
