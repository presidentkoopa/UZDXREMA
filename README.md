![Doom XR Edition](https://github.com/iAmErmac/DoomXR/blob/doomxr/branding/banner.png)

# UZDXREMA — a rendering fork of DoomXR

A fork of [DoomXR](https://github.com/iAmErmac/DoomXR) that adds a **lighting
and rendering feature set** on top of it. Everything DoomXR does — VR, dual
tracked hands, OpenVR input, the mod compatibility below — is unchanged and
inherited. What is new here is what the renderer can draw.

Branch: `doomxr`.

The full engineering write-up, with file references and the reasoning behind
each decision, is in **[`FORK_CHANGES.md`](FORK_CHANGES.md)**. This page is the
summary.

---

## Why this fork exists

It was built to support a lighting mod whose entire premise is darkness, and
that premise kept running into the same wall: **the sector is the smallest
thing stock Doom lighting can talk about.**

A sector has one light level and one colour. So a room is uniformly dim, a
glow is a flat wash on a surface, an "expanding ring of light" lights whole
rooms in sequence, and fog fills a space floor to ceiling with no shape at all.
Every effect ends up chunky at exactly the scale you most want it smooth.

Almost everything below is the same move applied to a different problem:
**stop placing objects to represent an effect, and give the fragment shader the
maths instead.** A sweep band asks each pixel its distance from a point. A beam
asks its distance from a segment. Height fog asks how much of the view ray lay
below a ceiling. Darkness asks each pixel about its own light rather than its
room's.

The result is a set of features that are cheap — they ride a shader that was
already running, costing arithmetic rather than draw calls — and that behave
correctly at any scale, because nothing is being approximated by geometry.

The features are general. None of them know anything about the mod they were
built for.

---

## What this fork adds

| | |
| --- | --- |
| **Billboards** | Oriented world quads with hit testing — an in-world UI primitive. Real depth-tested geometry, occluded by walls, pointable and touchable. Eleven payload types including panels, digits, segment displays and a glowing seam that opens. |
| **Surface glow** | Floors and ceilings glow on their **own face**, not just spilling onto a nearby wall. Wall glow gains falloff curves and intensity. Each glow carries **two colours** and ramps between them, so a corner can be a continuous gradient instead of a hard edge. |
| **Sweep** | World-space bands of light that wrap floor, wall and ceiling as one unbroken line. Eight at once, five shapes, each with its own origin, speed and colour, and four things a band can do to what it crosses: add, lift, crush, re-colour. |
| **Sweep band fill** | A band can carry a **lattice**, a field of dots or a solid slab instead of a wash — a laser grid standing across a corridor. |
| **Glow wave** | A glow's **edge** varies along a surface, not just up it. Peaks and valleys, with a phase per surface so one wave climbs a room. |
| **Per-fragment darkness** | The darkness curve leaves the sector. Same four curves, asked per pixel — which makes **distance falloff** and **height pooling** possible, neither of which a per-sector multiplier can express. |
| **Fog slab** | Fog with a **top**. A horizontal layer of mist you stand in and look down at, solved analytically rather than raymarched. Lit by the flashlight, tinted by what is behind it, and disturbed by a wake that follows you. |
| **Beams** | Real segment lasers. Continuous at any length, visible hanging in the air, correctly occluded, lighting the surfaces they pass — and **no dynamic lights, sprites or quads** anywhere in it. |
| **Volumetric beam** | A raymarched light cone with world-space dust, for the flashlight. |
| **Bloom** | Threshold, soft knee, anamorphic streak, tint, chromatic fringing. Upstream's threshold was hardcoded at 1.0, meaning only already-blown-out pixels could bloom. |
| **Non-pausing menus** | A settings page can let the world run behind it, so a lighting page can actually preview what it is adjusting. |
| **VR weapon wheel** | A wheel per hand, worked by the hand it belongs to. |
| **HUD stereo gating** | **Bug fix** — flat desktop sessions were losing the entire 2D layer: status bar, view border, and every mod's `RenderOverlay` at once. |

### Two rules the additions follow

**Frame-global values go in the viewpoint buffer, never `StreamData`.**
`StreamData` is the per-draw block, and its size divides a fixed 64KB buffer
into `MAX_STREAM_DATA` draws. Growing it costs draw batching in every frame of
the game, forever. Several features here would have been easier with a
`vec4[8]` of per-band data and deliberately are not.

**Spare bits get reused before anything grows.** The sweep's draw mode lives in
a component that began as a bare on/off flag hardcoded to `1.0`; the band fill
mode now shares it. `uFlatGlowIsCeiling` was a padding int. `MAX_STREAM_DATA`
has not moved.

---

## Compatibility

**Content built for this fork will not run on stock GZDoom.** The additions are
engine features, not ZScript libraries — a mod calling `Level.SetBeam` or
`Sector.SetFlatGlowColor` fails to compile elsewhere.

The reverse is fine: anything that ran on DoomXR runs here.

**GLES does not implement** surface glow, glow waves, per-fragment darkness,
the fog slab, band fill or beams. The Vulkan and OpenGL paths both do.

---

## Everything below is inherited from DoomXR

DoomXR is a VR port based on QuestZDoom and UZDoom. Built and tested on HP
Reverb and Oculus Quest via Virtual Desktop; other setups should work as long
as they are OpenVR-compatible.

It exposes OpenVR controller input for definition — you will need to define the
controls. Both hands are tracked for weapons.

### Controller Info
#### Index Controllers
To get the most out of your Index Controllers, choose the Community Binding
"Index Controller Bindings" by gameflorist in SteamVR. It makes the maximum
buttons available for binding in DoomXR.

### Mods

Optional mods tested with DoomXR:

- https://github.com/hh79/gz3doom/files/4378108/HDVRweapons.zip HD weapon pack made by ajantaju
- https://github.com/ajantaju/br_vr weapon pack made for Brutal Doom
- https://github.com/dxt121730/BD64Weapons weapon pack for Brutal Doom 64 by dxt121730
- https://github.com/mmaulwurff/laser-sight/releases laser sight mod, you need this for aiming
- https://github.com/iAmErmac/Virtual-Tactical-Vest adds a virtual vest with weapon slots by iAmErmac
- https://www.moddb.com/downloads/doom-neural-upscale-2x texture upscale mod
- https://forum.zdoom.org/download/file.php?id=30459&sid=df63736751c12c3ebb76230d1dc86543 blood color fixer mod

---

## Building

`auto-setup-windows-vr.cmd` locates Visual Studio's bundled CMake via
`vswhere` — CMake is generally not on PATH. Build output lands in
`build-dxr/RelWithDebInfo/`.

For the general UZDoom build process, see UZDoom's
[wiki](https://github.com/UZDoom/UZDoom/wiki/Compilation):
[Linux](https://github.com/UZDoom/UZDoom/wiki/Compilation#linux) ·
[MacOS](https://github.com/UZDoom/UZDoom/wiki/Compilation#macos) ·
[Windows](https://github.com/UZDoom/UZDoom/wiki/Compilation#windows)

---

Copyright (c) 1998-2025 ZDoom + GZDoom + UZDoom teams, and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses.

### Source code licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html

## Resources
- https://zdoom.org/ - Home Page
- https://forum.zdoom.org/ - Forum
- https://zdoom.org/wiki/ - Wiki
- https://dsc.gg/zdoom - Discord Server

## Credits

This fork stands on a long chain, and none of the work below is mine:

- [ZDoom + GZDoom + UZDoom teams](https://zdoom.org/) — the engine this is based upon
- [Emile Belanger](http://www.beloko.com/) — the developer behind the android port
- [Team Beef](https://github.com/Team-Beef-Studios) — the VR port for the Oculus Quest device
- [Emanuele Disco](https://github.com/emawind84) — QuestZDoom PCVR port, and the QoL, performance, dual-wielding and integrated mod loader work on his QuestZDoom fork
- [iAmErmac](https://github.com/iAmErmac/DoomXR) — DoomXR, which this forks directly

Special thanks to Coraline of the EDGE team for allowing use of her
[README.md](https://github.com/3dfxdev/EDGE/blob/master/README.md) as a
template for the original.
