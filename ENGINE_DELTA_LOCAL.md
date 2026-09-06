# ENGINE_DELTA_LOCAL.md

**What got added to my project.** One honest answer, measured rather than
estimated, on 2026-09-06.

The trunk is now the owner's pristine `UZDXR 5.0.0-dxr` source. Everything the
parallel lanes built since sits on `work` as a single commit. `git diff
main..work` is the whole answer, and this file explains it.

```
main   68ec2561  UZDXR 5.0.0-dxr, pristine      (sole root commit, 10,429 files)
work   07b540ba  Local work as of 2026-09-06    (107 files changed)

legacy-main / tag pre-reseat-2026-09-06  ->  8a1839ae, the pre-reseat lineage,
                                             with its 29,449-commit upstream
                                             history and the `upstream` remote
                                             still reachable. Nothing was
                                             destroyed to make this.
```

## Both branches build

| branch | result | exe |
|---|---|---|
| `main` (pristine) | **built clean**, exit 0 | 18,152,448 bytes, archived as `_backups/doomxr_pristine_5.0.0-dxr.exe` |
| `work` (local) | **built clean**, exit 0 | 18,184,192 bytes, the same size as the pre-reseat exe |

The baseline is sound, so the classification below rests on something real.
`_backups/doomxr_pre-reseat_2026-09-06.exe` is the exe as it stood before any of
this, kept for comparison.

## The delta at a glance

107 files: **5 added, 41 deleted, 61 modified**, +5,848 / -41,488 lines.

(`git diff main..work --stat` now reports 108, because this report is itself
committed on `work`. Every count below is of the 107 that are engine changes.)

The plan predicted 65. The extra is documentation: 39 of the 41 deletions are
`.md` and licence files, not code. Restricted to code, the plan's estimate was
close, 52 src files predicted against 55 actual.

| area | files | what lives there |
|---|---|---|
| `src/common/rendering/**` | 20 | OpenXR device, VR modes and cvars, render state, shader plumbing, weapon wheel |
| `src/rendering/hwrenderer/scene/**` | 7 | flats, walls, sprites, decals, sky, weapon, draw info |
| `src/playsim/**` | 12 | VR arm IK (new), actor/player fields, per-part model frames, physics (removed) |
| infra | 10 | `CMakeLists.txt`, `d_main`, `d_net*`, `events.*`, `g_levellocals.h`, `maploader`, `p_setup`, `QzDoom` |
| `wadsrc/static/shaders/**` | 6 | air stamps, sprite outlines, neon panel, sweeps, SDF text, `main.fp` |
| `wadsrc/static/**` other | 5 | `menudef.txt`, `language.0`, `actor.zs`, `player.zs`, `doombase.zs` |
| models | 4 | IQM 16-bit joints, model pivot, VR body placement |
| `src/scripting/**` | 3 | the natives exposed to ZScript |
| root docs | 14 | 13 removed, 1 added |
| upstream `docs/` + `unused/docs/` | 26 | GZDoom's own docs and third-party licences, all removed |

The 130 files that differed only by line endings are **gone**. The trunk's
`.gitattributes` (`* text=auto eol=lf`, which pristine already shipped)
normalizes on import, so that noise cannot come back.

---

# Findings that need your decision

Six things. Two are clean bills of health on questions the plan raised, three
want a decision from you, and one is a confirmed dead knob.

### F1 - `docs/` and the licence files were deleted by a commit that says nothing about it - **question**

Commit `fdf85d5a` (2026-09-04) is titled *"Close the flat-glow leak everywhere,
and actually send the weapon's room glow"*. Its message is eleven paragraphs
about render state. It is also the commit that deleted **39 documentation
files**: 27 lines of code changed, 38,163 lines deleted. The deletion is not
mentioned anywhere in the message.

This is the exact failure mode this exercise exists to find, a change nobody
described and nobody noticed.

**Nothing is lost.** All 13 fork documents are sitting in
`E:\DOOMWork\Engine docs\`, byte-identical to pristine. I compared every one.
So for those it is a **relocation**, and arguably a deliberate one: that folder
also holds `BUILD NOTES 5.0.0-dxr.md`, `MODELS.md` and `MODEL_INVENTORY.md`,
which pristine never had.

**What is actually gone** is upstream's own `docs/` tree, 24 files including
`docs/licenses/gpl.txt`, `lgpl.txt`, `MIT.txt`, `Doom-Source-License.txt` and
the rest of the third-party licence set, plus `unused/docs/`. These are not in
`Engine docs` and are not anywhere else on the drive. They are still in
`legacy-main`, `upstream/trunk` and pristine, so restoring them is one command.

> GZDoom is GPL and those licence files are what document the third-party code
> in `libraries/`. You have said this fork is never distributed, so nothing is
> breached by their absence, but they cost nothing to keep and they were removed
> by accident. **Recommendation: restore `docs/` and `unused/docs/` on `work`.**

### F2 - `vr_ik_shoulder_width` is a dead knob - **suspect**

```
src/common/rendering/hwrenderer/data/hw_vrmodes.cpp:679
CVAR(Float, vr_ik_shoulder_width, 7.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
                                        // half body width, collar offset from head
```

Declared, archived to the ini, given a descriptive comment, and **read by
nothing**. Not by `vr_armik.cpp`, not by any menu, not by any mod. It is the
only one of the **59 cvars this branch adds** with no consumer at all; the other
58 are wired. It is console-only, so it never appeared in a menu doing nothing,
which is why it went unnoticed.

(For scale, the branch also *removes* 27 cvars: the 4 headshot ones from **F4**
and 23 `vr_physics_*` ones from **F3**. 960 cvars in pristine, 992 in `work`.)

Either the collar offset was meant to be configurable and the wiring never
landed, or the cvar outlived its use. Your call which.

### F3 - the `p_physics` removal was deliberate and documented - **keep**

The plan asked whether deleting `src/playsim/p_physics.cpp/.h` (2,845 lines) was
intended. It was, explicitly, and the history says so:

| commit | date | what |
|---|---|---|
| `2838cf0e`..`fe787e9b` | Aug 2026 | the module built over ~11 commits |
| `7966903e` | 2026-08-24 | reverted wholesale |
| `0589089a` | 2026-08-25 | restored **dormant behind `vr_physics`, default OFF**, "banking the module rather than leaving 2800 lines loose", with a recorded reason it could never be netplay-safe as written |
| `9a9d8b8a` | 2026-09-02 | **removed outright**, "Nothing scripted or linked to it, and it moved shared actors from local controller tracking at render rate with no netgame check" |

The removal is complete and coherent: the module, its three hooks (`d_main`,
`maploader`, `p_setup`), the `MF9_PHYSICSBODY` flag, all **14 `Physics*` ZScript
natives** and the whole Physics Options menu went together. Nothing dangles.
`VR_INTERACTION_PLAN.md` records the change of direction.

**No action needed.** Flagged only because the plan asked.

### F4 - the headshot laser reaction was removed cleanly - **keep**

The `VRLASERMNU_HEADSHOT_*` menu block, its 5 language strings, its 4 cvars, the
draw-time reaction in `hw_weapon.cpp`, and both `LaserHeadshotLinedUp*` actor
fields all went together. I checked for orphans and found none.

One cosmetic leftover: `src/rendering/hwrenderer/scene/hw_weapon.cpp:612` still
carries a comment referring to `vr_laser_headshot_color`, a cvar that no longer
exists. One line, harmless, worth deleting when you are next in the file.

### F5 - `language.0` is gitignored, and was hiding a real change - **fixed**

`wadsrc/static/language.0` matches `.gitignore:13`. Upstream force-adds it; a
plain `git add -A` does not. It was carrying an **unreviewed content change**:
local work removed the five `VRLASERMNU_HEADSHOT_*` strings and added
`VRWEAPONMNU_INSTANT_HANDSWITCH`. That change was invisible to any normal diff.

Two more files were in the same position: `mobile/src/extrafiles/gitinfo.h` and
`libraries/Translation/engine/macros.csv`, both identical across trees.

**All three are now force-added to both `main` and `work`**, so they show up in
future diffs. This is why the trunk has 10,429 files rather than 10,426.

### F6 - nine natives and fields have no caller anywhere - **suspect / question**

Of the **20 natives and 15 fields** this branch adds, most are genuinely in use.
These are not:

| symbol | subsystem |
|---|---|
| `ModelFramePart`, `ModelFrameNextPart`, `ModelFrameLerpPart`, `ModelPartHidden` | per-part MD3 frame addressing (`p_pspr.h`) |
| `SetModelBonePose`, `SetModelUseProceduralPose`, `SetArmIKEnabled` | procedural bone pose and arm IK gate |
| `SetRenderAttachment` | render attachment (the `AtWorld` variant *is* used) |
| `SetVRBodyGripAxis` | VR body rig description |

Searched: every mod in `E:\DOOMWork` (loose files), the shelved `_old/` copies,
and the `RS_VRIK` backup archive.

These are **not necessarily wrong**. The per-part frame set in particular reads
as deliberate capability-building. Its comment block explains at length that it
exists so split-mesh weapons need not hand-author the cross product of every
part's position, and the mod that would use it does not exist yet. Building a
capability ahead of its caller is legitimate; it is only a problem if it was
believed to be *running*.

For contrast, these new symbols do have real callers, so they are **keep**:
`SetDarknessActors` (RS_Darkness), `SetFogZones` (RS_Fog), `FollowBodyMode` and
`FollowBodyOfs` (RS_VR_Unified), the eight `Outline*` fields (RS_Sweeps),
`SetBillboardCore` (`_old/RS_KillCounter`), `SetBillboardUV`
(`_old/RS_DeathFX`), and the eleven `SetVRBody*` / `RenderAttachParent` symbols
(RS_VRIK, which is deleted from disk and lives only in
`_backups/RS_VRIK_round3.zip`; **restore that mod or the VR body has no
driver**).

---

# The delta, cluster by cluster

Classification: **keep** = verified wired and coherent. **question** = wants a
decision from you. **suspect** = present with no evidence it has ever run.

## `src/common/rendering/**` - 20 files - mostly **keep**

| file(s) | what the change does | |
|---|---|---|
| `vulkan/stereo3d/vk_openxrdevice.cpp/.h` | **Restores two-handing by proximity**, feeding the same `TwoHandedHold` the mod's support-oval claim feeds. Deliberately wired to the controls that already existed, `vr_two_handed_weapons` and the Stabilize Distance slider, which had outlived the logic they were built for and sat in the menu doing nothing. Adds an `offBusy` test so reloading does not brace, and a swap distance so passing a weapon between hands does not brace. Also fixes `vr_haptic_debug`, which was `true` and flagless so no ini could hold it down. | keep |
| `hwrenderer/data/hw_vrmodes.cpp/.h` | 59 new cvars, almost all the VR body and arm-IK set, plus `vr_handswitch_instant` and `vr_two_handed_swap_inches`. Removes the 4 headshot cvars and the 23 `vr_physics_*` ones. Note `vr_stabilize_distance_inches` and `vr_two_handed_weapons` are **not** new: they are pristine, and are the controls the restore in the row above was wired back onto. | keep, one exception: **F2** |
| `hwrenderer/data/hw_renderstate.h` | Three new per-draw lanes: sprite outlines (`uOutline*`), fog density scale (`uFogDensityScale`), and `uDarknessExempt` promoted from bool to float so actors can take a *partial* darkening. All default to inert, so every existing draw is untouched. | keep |
| `hwrenderer/postprocessing/hw_postprocess.cpp/.h` | Volumetric beam pass made per-slot and additive, so four beams composite for free with no shader change. | keep |
| `gl/gl_shader.cpp/.h`, `vulkan/shaders/vk_shader.cpp`, `hwrenderer/data/hw_shaderpatcher.cpp`, `vulkan/renderer/vk_renderpass.cpp` | Register and prepend the two new shader functions so `main.fp` can call them with no forward declaration. | keep |
| `gl/gl_buffers.cpp`, `gles/gles_buffers.cpp`, `hwrenderer/data/buffers.h`, `hwrenderer/data/hw_modelvertexbuffer.cpp`, `i_modelvertexbuffer.h` | `boneselector` widened to 16-bit so rigs over 256 joints work (the Slayer has 924). | keep |
| `gl/gl_renderstate.cpp`, `hwrenderer/data/hw_viewpointuniforms.h`, `hwrenderer/data/hw_vrwheel.cpp` | Plumbing for the above; the wheel passes `exactInstance` to `MoveWeaponToHand`. | keep |

## `src/rendering/hwrenderer/scene/**` - 7 files - **keep**, and largely bug fixes

This cluster is one coherent piece of work: **flat glow was leaking onto
everything**. `main.fp` applies flat glow to any surface without asking whether
that surface is a flat, so every path that neither set nor cleared it inherited
the last flat drawn.

| file | what the change does | |
|---|---|---|
| `hw_flats.cpp` | Adds `FlatGlowAtPoint`, `SplitRoomGlow` and `GlowWaveAtPoint`, the flat-glow arithmetic evaluated on the **CPU at one world point**, so something drawn in view space (the weapon, the VR hands) can be lit by the room it stands in. Written as a general capability: takes a sector and a point, nothing else. | keep |
| `hw_weapon.cpp` | Applies that room glow to HUD models, and fixes a **dead-code bug**: `SetAddColor` was called *before* the glow was folded in, so the fold wrote to a variable nothing sent. Gives psprites the same darkness exemption world sprites get, so VR hands do not shade differently depending on which mode they are in. Removes the headshot reaction (**F4**). | keep |
| `hw_sprites.cpp` | Clears inherited flat glow; drives the per-actor neon outline. | keep |
| `hw_walls.cpp` | Adds `FogScaleForSector` (a sky ceiling means outdoors, so no new mapping work and no new flag). Clears flat glow *before* the draw as well as after, fixing walls that lit and unlit as the player moved. | keep |
| `hw_decal.cpp`, `hw_skyportal.cpp` | Same exemptions for decals and sky, so a blood splat is not lit and fogged differently from the wall under it. | keep |
| `hw_drawinfo.cpp` | Uploads per-slot beam uniforms. Also fixes a **crash**: `PointInSubsector` returns `&subsectors[0]` for a null node, so drawing the error screen after `I_Error` freed the map was a null read. The engine died with an access violation instead of showing the error that caused it. | keep |

## `src/playsim/**` - 12 files - **keep**, with **F6** noted

| file(s) | what the change does | |
|---|---|---|
| `vr_armik.cpp/.h` **(new, ~2,000 lines)** | Native VR arm IK and body avatar, ported from the old DXR fork. Solves the arm chain at **render** rate from the same controller pose the weapon is drawn with. Driven by RS_VRIK. | keep, but see the RS_VRIK warning in **F6** |
| `d_player.h` | The VR body state block: rig role table, hand contact, foregrip, crouch drop, hidden bones. Explicitly transient, not serialized, not networked, local player only. | keep |
| `actor.h` | `proceduralPose` and `useProceduralPose`; `FollowBodyMode` and `FollowBodyOfs` (worn-on-body props placed at draw rate, per-actor rather than a MODELDEF flag because a dozen props share one class and each sits somewhere different); the 8 sprite-outline fields. Removes `LaserHeadshotLinedUp*`. (`StabilizeReach` and `TwoHandedHold` are pristine, not additions.) | keep |
| `p_pspr.h/.cpp` | Per-part model frame addressing, the MD3 answer to a skeleton. | **suspect** (**F6**), no caller |
| `p_user.cpp` | Calls the IK, then settles decoupled body facing. | keep |
| `p_map.cpp` | Extends the script collision veto to **non-solid but shootable** actors, which previously stopped a missile with no ZScript veto available at all. | keep |
| `p_actionfunctions.cpp` | Exports `P_EnsureActorModelData` so there is exactly one allocator. | keep |
| `p_mobj.cpp` | Small VR body plumbing. | keep |
| `p_physics.cpp/.h` **(deleted)** | See **F3**. | keep |

## infra - 10 files - **keep**

| file | what the change does | |
|---|---|---|
| `QzDoom/qzdoom_common.cpp` | **`QzDoom_Vibrate` had an empty body.** Twenty-odd call sites across `p_map`, `p_mobj`, `p_interaction`, `a_weapons`, `sbar_mugshot` and `t_func` each computed a haptic intensity and handed it to nothing. This is why controllers never buzzed for anything the game itself did, and why the whole `ext_haptic_level_*` menu had no effect. Now translated to `VRMode::Vibrate`. A real fix for a real dead feature. | keep |
| `g_levellocals.h` | Volumetric beams **1 to 4 slots** (a singleton meant every caller was the same caller, so opening the weapon wheel put your torch out). Billboard UV sub-rect and `coreWhite`. Slot 0 is what a caller that never heard of slots gets. | keep |
| `events.cpp/.h` | Damage source and inflictor were set only on the line and sector paths, so a handler reading them for **thing** damage read uninitialised stack. Now initialised and filled. | keep |
| `d_netinfo.cpp` | Names the cvars that actually put traffic on the wire, so a net buffer overrun says who filled it. | keep |
| `d_net.cpp`, `d_main.cpp`, `maploader.cpp`, `p_setup.cpp`, `CMakeLists.txt` | `exactInstance` argument; physics hook removals; `vr_armik.cpp` added to the build. | keep |

## `wadsrc/static/shaders/**` - 6 files - **keep**

| file | what the change does | |
|---|---|---|
| `func_airstamps.fp` **(new)** | wgTypes 14 to 20, the air-hanging stamp family. Only shape 13 had been ported; these seven are the impact ring, disc flash, smoke puff, casing and shard that were missing from every impact. | keep |
| `func_spriteoutline.fp` **(new)** | An actor traced in neon by its own sprite. Transcribed from the previous fork's `monster_neon.fp`, which was a material shader bound by sprite name in gldefs, so it knew nothing about modded monsters and was all-or-nothing for the scene. Same arithmetic, moved into the standard sprite path and driven **per actor**. | keep |
| `func_wg13.fp` | Replaces a stand-in 7-segment readout with the original neon number panel. | keep |
| `main.fp` | Signed sweep shapes 6 to 9, so a band can cross a whole level instead of splitting into two fronts. | keep |
| `func_surfacestamps.fp` | Colour gradient and fade evaluated in-shader from the stamp's own life, replacing per-tic re-pushing by the caller. | keep |
| `func_sdftext.fp` | Glyph core pushed toward white, leaving colour to the halo, the difference between a coloured letter and a neon tube. | keep |

## `wadsrc/static/**` other - 5 files - **keep**

| file | what the change does | |
|---|---|---|
| `menudef.txt` | Adds Instant Hand Switch. Removes the Physics Options menu (**F3**) and the headshot block (**F4**). | keep |
| `language.0` | See **F5**. | keep |
| `actor.zs`, `player.zs`, `doombase.zs` | Declares the 20 new natives and 15 new fields; drops the `Physics*` family and `LaserHeadshotLinedUp*`. `player.zs:2665` is where `vr_handswitch_instant` is consumed. | keep, except the nine in **F6** |

## models - 4 files - **keep**

| file | what the change does | |
|---|---|---|
| `r_data/models.cpp` | VR body placement: autofit, neck-to-HMD (the seated case, sliding the body down rather than scaling it so the player's reach is not shortened), head-pivot correction, crouch measurement. | keep |
| `common/models/model.h` | **`pivotx/y/z`**, the point a model turns about, subtracted *before* rotation. `Offset` is applied after, so it cannot express this: a mesh not authored on its turning point orbits instead of spinning. Wanted independently by a grenade, a hand model and anything on a holster bracket, so it is a MODELDEF field rather than a fix inside one caller. | keep |
| `common/models/models_iqm.cpp`, `model_iqm.h` | 16-bit joint indices for rigs over 256 joints. | keep |

## `src/scripting/**` - 3 files - **keep**

`vmthunks_actors.cpp` and `vmthunks.cpp` expose the new fields and natives.
Worth knowing: `SetVolumetricBeam` is registered with **`NATIVE0`, not
`NATIVE`**. The plain macro also registers a JIT direct-call pointer, and asmjit
caps that at 16 arguments. Adding `slot` took it to 17, at which point the JIT
died while compiling at load: no window, no dialog, nothing in any log. That
comment is worth leaving exactly where it is.

`thingdef_data.cpp` drops `MF9_PHYSICSBODY`.

---

# What "suspect" can and cannot mean here

The plan asked for files marked **suspect**, "present but never observed
running". I cannot observe anything running; I do not launch the game. So
everything above is classified on **wiring evidence**, which is checkable:

- a cvar with no reader is dead, mechanically true, and that is **F2**;
- a native with no caller in any mod on disk is unreached, and that is **F6**,
  though the caller may simply not be written yet, which is not the same as
  broken;
- a removal is clean if no orphan references survive it, checked for **F3** and
  **F4**, and both are clean.

What this cannot tell you is whether wired code *does the right thing when it
runs*. The two-hand stabilize whose test volume was 2mm wide would have passed
every check in this document. Only the headset settles that class of question.

The honest summary: of 107 changed files, **one** contains something provably
dead (**F2**), **nine symbols** are unreached (**F6**), **one** deletion needs
your decision (**F1**), and everything else is wired, coherent, and builds.

---

*Written 2026-09-06 alongside the reseat that produced these two branches.
Method: `git diff main..work`, plus set-difference audits of every cvar, native
and `DEFINE_FIELD` on both sides, cross-referenced against every mod in
`E:\DOOMWork`, the shelved `_old/` copies and the `_backups` archives.*
