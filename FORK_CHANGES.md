# DoomXR fork — engine changes

Everything this fork adds on top of upstream, in one place, so another
engine developer can see what was touched and why without reading the log.

Branch: `main`. Each area below is self-contained; nothing here depends on
anything else here.

| Area | What it is | Detail doc |
| --- | --- | --- |
| [Billboards](#1-billboards) | oriented world quads with hit testing — an in-world UI primitive | [`BILLBOARDS.md`](BILLBOARDS.md) |
| [Surface glow](#2-surface-glow) | floors and ceilings glow on their own face; wall glow gains shape | — |
| [Sweep](#3-sweep) | world-space bands of light that wrap every surface | — |
| [Volumetric beam](#4-volumetric-beam) | a raymarched light cone with dust in the air | — |
| [Bloom](#5-bloom) | threshold, knee, anamorphic streak, tint, chromatic fringing | — |
| [VR weapon wheel](#6-vr-weapon-wheel) | a wheel per hand, worked by the hand it belongs to | — |
| [HUD stereo gating](#7-hud-stereo-gating-bug-fix) | **bug fix** — flat sessions lost the entire HUD | [`HUD_STEREO_GATING.md`](HUD_STEREO_GATING.md) |
| [Glow wave](#8-glow-wave) | a glow's edge varies along a surface, not just up it | — |
| [Per-fragment darkness](#9-per-fragment-darkness) | the darkness curve leaves the sector | — |
| [Non-pausing menus](#10-non-pausing-menus) | a settings page can let the world run behind it | — |
| [Fog slab](#11-fog-slab) | fog with a **top** — a layer of mist you stand in | — |
| [Sweep band fill](#12-sweep-band-fill) | a band can carry a lattice, not just a wash | — |
| [Beams](#13-beams) | segment lasers lit per pixel — continuous, and they light the room | — |
| [The fog slab, shaped](#14-the-fog-slab-shaped) | a bottom edge, a vertical hold, and tornadoes | — |
| [Reactive fog](#15-reactive-fog) | one disturbance primitive; mist that banks, reacts and sprouts | — |
| [The heatmap](#16-the-heatmap) | the floor accumulates where the fighting happened | — |
| [Selective desaturation](#17-selective-desaturation) | a grey world that still has blood in it | — |
| [Two bugs worth recording](#18-two-bugs-worth-recording) | the volumetric cone, and the fourth uniform list | — |
| [Direct model frame addressing](#19-direct-model-frame-addressing-on-psprites) | a HUD model's frame stops going through the sprite, and the 29-frame ceiling goes with it | — |
| [Texture inside the glow](#20-texture-inside-the-glow) | five terms for a lane whose coverage is too high for the wave to help | — |
| [Native state remap](#21-native-state-remap) | a psprite's own state becomes the model's animation clock | — |
| [Shapes](#21b-shapes--signed-distance-fields-painted-onto-surfaces) | 128 SDF glyphs painted onto surfaces; they grow, split open, and repeat into formations | — |
| [Billboard hit tests](#22-billboard-hit-tests-two-defects-found-by-their-first-consumer) | **bug fixes** — the group transform moved the picture and not the target, and a hidden panel stayed clickable | — |
| [Psprite model scale](#23-a-psprites-scale-reaches-the-model-path) | a HUD model can finally be resized from script | — |
| [Script VR input suppression](#24-script-side-vr-input-suppression) | a mod's own in-world menu can claim the sticks, so snap turn stops firing mid-choice | — |
| [The laser as a borrowed cursor](#25-the-laser-as-a-borrowed-cursor) | a script menu can force the laser on one named hand and stop it at a billboard | — |
| [Haptics reach ZScript](#26-haptics-reach-zscript) | the controllers can finally be buzzed from script — and `VR_HapticEvent` turns out to be a stub | — |
| [Billboard roll](#27-billboard-roll) | the third angle — and it lives in the shared basis, so a rolled quad is hittable where it is drawn | — |
| [A panel as a field](#28-a-panel-as-a-field) | `BB_SDFPANEL` — the rounded rect solved per pixel, so a plate can finally glow | — |
| [Everything else this fork adds](#29-everything-else-this-fork-adds) | the laser sight itself, tracers, the psprite recursion guard, and every native with no API entry | — |
| [Field reflection](#30-field-reflection--reading-another-mods-data-without-linking-to-it) | read another mod's field by name — no shared header, no hard dependency | — |
| [`MainHandRoll`](#31-mainhandroll--the-main-hands-real-wrist-roll) | the main hand's real wrist roll, since the network-safe field is zeroed every tic | — |
| [Standing shapes](#32-standing-shapes--the-same-glyphs-freestanding-in-open-air) | the same SDF glyphs, freestanding in open air instead of painted onto a surface | — |
| [Fog disturbance capacity](#33-fog-disturbance-capacity--8-to-32-and-why-not-128) | 8 to 32 live disturbance slots, and why not 128 | — |
| [Mod-owned placement](#34-mod-owned-placement--per-weapon-offset-sliders-without-an-engine-change) | per-weapon offset (and scale) sliders a mod declares itself, no engine change | — |
| [Skin alpha that is not opacity](#35-skin-alpha-that-is-not-opacity) | a model skin whose alpha is packed data, not opacity, stops being alpha-tested away | — |
| [Per-psprite model tint](#36-per-psprite-model-tint) | `Tint`/`Glow` — a held weapon's model finally takes colour, independently per hand | — |
| [Script-suppressed psprite layers](#37-script-suppressed-psprite-layers-hiding-the-weapon-without-hiding-what-it-does) | `NoDraw` — hide a layer without touching the weapon it belongs to | — |
| [Flat overlays on a modeled weapon](#38-flat-overlays-on-a-weapon-that-is-itself-a-model) | a flash overlay dims to match the 3D gun it's stapled to, instead of floating over it | — |
| [Static poses on decoupled models](#39-static-poses-on-decoupled-models-and-the-trace-built-to-find-them-missing) | a bone-animated hand can hold one authored frame instead of only ever playing a clip | — |
| [Array-element field reflection](#40-array-element-field-reflection) | §30's reflection, extended to read one element out of another mod's array | — |
| [HUD bone anchoring](#41-hud-bone-anchoring--a-psprite-layer-drawn-at-another-layers-bone) | a psprite layer draws at a named bone of another layer's model | — |
| [Model diagnostics](#42-model-diagnostics-vr_validate--vr_spatialreport-and-a-crash-one-step-behind-them) | catch a collapsed mesh or a mispositioned model on sight — and the crash found one step behind them | — |
| [Sweep room bound](#43-sweep-room-bound--the-air-lattice-gets-a-room-instead-of-an-infinite-plane) | the lattice fades outside a script-published box, instead of standing in every room its plane reaches | — |

---

## 1. Billboards

A quad placed in the world with its own orientation, drawn as real
depth-tested geometry — occluded by walls, sorted against sprites, and able to
be pointed at or touched. Built so an in-world interface needs neither an
actor per panel nor a HUD overlay.

```
Level.AddBillboard(pos, w, h, yaw, tilt, facing, payload, data, col, flags, lifetime)
Level.AddBillboardPersistent(...) -> handle
Level.AttachBillboard(actor, ofs, ...)      // follows an actor, dies with it
Level.AimBillboard(start, dir)  -> hit id, UV
Level.TouchBillboard(point, r)  -> hit id, UV, distance
Level.SweepBillboard(from,to,r) -> hit id, UV, fraction along the segment
```

**Twelve payloads** — not the six this paragraph originally claimed, and not
the ten a later audit corrected it to, either: that audit missed that index 6
was already `BB_TEXT`, not unused, and predates §28 adding a twelfth. In enum
order (`g_levellocals.h`, `EBillboardPayload`): `BB_PANEL`, `BB_TEXTURE`,
`BB_DIGITS`, `BB_GLYPH`, `BB_RING`, `BB_BAR`, `BB_TEXT` (an arbitrary string —
reads the billboard's own text field and ignores `data`), `BB_SEGMENT` (that
same kind of string drawn as a 16-segment display, procedurally — no atlas),
`BB_SEGLCD` (its inverse — a lit plate with the digits punched out of it),
`BB_SEAM` (a glowing slit you open with `ResizeBillboard`; with the void flag
the opening is a *hole* with a bright rim rather than a lit slab), `BB_WG13`
(a transcribed kill badge, plate and digits in one pass), and `BB_SDFPANEL`
(§28 — `BB_PANEL`'s job solved as a distance field instead of a sampled
texture, which is what lets it take a glow). The enum runs 0–11 with no gaps.
All twelve draw. `BILLBOARDS.md`'s own payload table lists all twelve.

Also: view-locking (resolved at *render* rate, not tic rate, which is what makes
a head-locked panel not swim), per-billboard alpha, `BBFL_NODEPTH`,
`BBFL_FOLLOWANGLE`, save/load, budget and distance culling.

**Groups.** One transform — origin, scale, and an engine-eased animated scale —
over a whole composed panel of many quads, so a forty-quad readout grows as one
object instead of forty. `AddBillboardGroup`, `SetBillboardGroup`,
`SetBillboardGroupScale`, `AnimateBillboardGroup`, `SetBillboardGroupOrigin`,
`RemoveBillboardGroup`. §22 fixes two group bugs without this section ever having
introduced them; it does now.

**Text is signed-distance, with its own font system.** `FSDFFont` /
`FSDFFontRoster` (`hw_sdffont.h`), a glyph cache flushed on texture reload
(`d_main.cpp:3798`), and measuring natives so a caller can size a panel to its
text before drawing it: `MeasureBillboardText`, `MeasureBillboardTextBlock`,
`SetBillboardFont`, `RollBillboardFonts`, `BillboardFontCount`,
`BillboardFontName`. The roster is **reshuffled every game**, so a font slot
names a role and never a typeface.

`TextureID.GetIndex()` was exposed for this, and the payload/facing/flag
constants are named on `LevelLocals` so callers don't invent their own copies.

### Two defects fixed 2026-08-08

**Colour was discarded — every billboard rendered white.**
`ProcessBillboard` sets `ThingColor = bb->color` (`hw_sprites.cpp:2042`), but
`DrawSprite` only applied it inside `if (cursec != nullptr)` (`:215`). A
billboard is neither an actor nor a particle (`:2010-2011`), so `cursec` was
always null, the branch never ran, and the draw state kept the white reset
from `:385`. Tier colours, meters and label/value contrast were all computed
and then thrown away, with nothing logged.

Fixed with an `else if (isBillboard)` that applies `ThingColor` directly —
deliberately *without* the sector's sprite tint or additive colour, since a
billboard is a UI primitive placed in world space rather than a thing standing
in a room. The colour a caller asks for is the colour it gets, in any sector.

**Horizontal basis was inverted — every billboard texture rendered mirrored.**
`hw_sprites.cpp:2083` used `DVector3 right(sy, -cy, 0)`, which is the viewer's
**left**. The ordinary sprite path proves it: a sprite's extent runs along
`(-V.y, V.x)` (`:1285-1308`) and the *unmirrored* branch maps `v[0] → u = UR`
with `UL=0, UR=1` (`:1247-1248`, `gametexture.cpp:340`), so that basis points
left. A billboard yaws to face the eye, so `V = -F` and `(-V.y, V.x)` reduces
to the same `(sy, -cy)` — identical geometry, therefore also left. Billboards
had inherited the sprite path's **mirror** branch.

Corrected to `right(-sy, cy, 0)`. This also fixes `BB_DIGITS`, which walks its
own pen along `right` (`:1884-1897`) and was laying multi-digit numbers out
backwards — `120` read as `021`. `bb_flipu` was a workaround for the first
symptom and could never have fixed the second, because it flips U *inside*
each quad rather than changing the direction the pen travels; with the basis
corrected its default (off) is now the right value.

`AimBillboard` and `TouchBillboard` carry their own copies of the expression
(`vmthunks.cpp:3186`, `:3271`) and were corrected in the same change — all
three must always agree or the pointer lands somewhere other than where the
panel is drawn.

### Touch, made usable — 2026-08-08

Four changes, all in service of a pointer that lands where the panel draws and
a hand that can actually press one.

**One basis, not three copies.** The defect above was a single expression
written out three times and wrong in all three. Correcting three copies is
three chances to correct only two of them, and the symptom — a row clickable
half a panel away from where it looks — stays invisible for a long time. The
expression now lives once, in `BillboardBasis` (`g_levellocals.h`), and the
renderer, the aim ray, the touch test and the sweep all call it. Agreement is
structural rather than a comment asking for it.

**Queries ignored `bb_scale` and `bb_tiltbias`.** The renderer scales a
billboard's extent by `bb_scale` and adds `bb_tiltbias` to its lean; the two
queries used the raw `bb.width`/`bb.height` and the unbiased tilt. So a player
who raised `bb_scale` to make panels readable got a bigger panel with a dead
border, and any nonzero tilt bias moved the picture out from under the pointer.
Both cvars are `CVAR_ARCHIVE`, so this shipped as "the panels feel like they
ignore me sometimes". `BillboardBasis` takes both as parameters — passed, not
read, so the header stays free of cvar dependencies — and every caller passes
the live values.

**`BBFL_NOHIT` (flag 32).** Decoration: drawn like anything else, never
returned by a query. A composed panel is not one quad but forty — every glyph
of every label, a bar's track and its fill — and the queries return the
*nearest* hit, so a panel's own face is permanently masked by the text written
on it and a pointer aimed at a row comes back holding a letter. Without a way
to say "this one is not a target" the caller has no move: it cannot map a glyph
handle to a row, and it cannot ask the engine for the second-nearest.

**`SweepBillboard(from, to, radius) -> id, UV, frac`.** Segment versus
billboard: the first face the path touches, where on it, and how far along as
a 0..1 fraction.

`TouchBillboard` asks whether a hand is in a panel *right now*, and script only
gets to ask 35 times a second. A panel's touch slab is a few map units thick
and a deliberate jab moves a controller several units per tic, so the hand can
be in front of the panel on one tic and behind it on the next without ever
being inside it. The gentle touch works and the hard one does nothing — the
failure lands on the most emphatic gesture a player can make, which is the
worst possible place for it. Script cannot sample faster, and thickening the
slab to cover the fastest hand makes a panel you cannot stand near without
pressing.

Sweeping the path also yields "the hand *arrived* this tic" directly, instead
of making every caller infer an edge from two containment samples and hope it
saw both. `radius` inflates the face into a slab and pads its edges, which is
what a fingertip is.

Deliberately **not** in the engine: debounce, cooldown, hysteresis, and which
hand did it. Those are panel policy — how many presses a held hand is worth is
a design question, not a geometric one — and they belong to whoever owns the
panel. This reports geometry.

## 2. Surface glow

Upstream glow only ever reached wall edges. This adds glow on the **flat
itself** — a floor or ceiling lit on its own face.

```
Sector.SetFlatGlowColor(pos, color)       // pos = Sector.floor | Sector.ceiling
Sector.SetFlatGlowHeight(pos, height)
Sector.SetFlatGlowFalloff(pos, falloff)
Sector.SetFlatGlowIntensity(pos, intensity)
```

Wall glow gained matching `SetGlowFalloff` / intensity controls so all four
lanes (wall bottom, wall top, ceiling, floor) can be driven identically
instead of the wall pair behaving differently from the flat pair.

### Two colours per glow

A glow used to hold one colour and only dim as it faded, so a wall and the
floor it meets both arrived at their shared line at full strength in two
different colours — a hard edge nothing could soften. Each channel now takes a
second colour and ramps between them by attenuation.

```
Sector.SetGlowColorFar(pos, color)        // wall glow's far end
Sector.SetFlatGlowColorFar(pos, color)    // flat glow's far end
```

The primary colour sits **at** the plane the glow grows from; the far colour is
what it fades toward. Give both surfaces the same colour at their junction and
the corner is one continuous ramp — floor colour, blend, wall colour — instead
of a seam. Alpha 0 means unset and the glow is byte-for-byte the flat wash it
always was, the same convention `Side.SetGlowColor` uses.

Three uniforms cover four logical channels: floor and ceiling flat glow
time-share one, because a draw only ever covers one of them. They are paid for
out of slack already present in `StreamData`, so `MAX_STREAM_DATA` is unchanged
at 34 and draw batching is not affected.

**`SetFlatGlowIntensity` changed meaning.** It scaled the glow's *reach* while
the identically named wall control scaled *colour*. It scales colour now, so
all four lanes' intensity means one thing; reach belongs to height alone.
Content relying on intensity to widen a flat glow must raise its height.

GLES does not implement flat glow, wall falloff or intensity, and does not
implement this either.

## 3. Sweep

A band of light defined in **world space** that wraps floor, wall and ceiling
continuously as it passes — rather than being a per-surface effect that breaks
at every edge. Eight bands can be drawn at once.

```
Level.SetSweepOrigin(mode, origin, count)
Level.SetSweepBand(index, radius, thickness, softness, col, intensity)
Level.SetSweepTrail(length)      // signed wake; 0 = symmetric band
Level.ClearSweep()
```

Five distance modes: cylinder ring, X plane, Y plane, sphere shell, and a
**signed vertical** mode (5) so one band can rise or fall through a map.

The **wake** (`SetSweepTrail`) stretches each band backwards along its
direction of travel — one falloff widened on the trailing side rather than a
second gradient bolted on, so there is no seam at the band's core. The sign
carries direction; at zero the band is exactly the symmetric one it always
was. The uniform reuses a dead pad int in `StreamData`, so the Vulkan buffer
layout did not change.

Eight is a **GPU limit only** — `uSweepBands[8]` in a fixed 64KB uniform
block. Script-side systems (see the GITD mod's wave list) can run any number
of *logical* waves and allocate the eight drawn slots by priority; the engine
neither knows nor cares. Per-band origins were considered and rejected:
another `vec4[8]` in `StreamData` costs roughly a tenth of draw batching in
every frame, permanently, to make simultaneous multi-origin waves visible.

## 4. Volumetric beam

A raymarched cone that lights the air, not just the surface it lands on, with
world-space dust motes so the beam has texture and the motes stay put as the
viewer moves.

```
Level.SetVolumetricBeam(pos, dir, col, inner, outer, length,
                        density, falloff, dust, dustScale, dustDrift)
```

`vol_beam_quality` trades grain against framerate.

## 5. Bloom

`gl_bloom_threshold` was a hardcoded constant; it is now a control. Added
`gl_bloom_knee` (soft knee, so bloom ramps instead of switching on at a hard
cutoff), an anamorphic horizontal streak, a tint, and chromatic fringing.
`gl_bloom` now defaults **on**.

## 6. VR weapon wheel

One wheel per hand, each operated by the hand it belongs to, picked with the
thumbstick — and the stick no longer walks the player while the wheel is open.
The wheel announces its selection rather than deciding game state itself, and
can leash to the wrist so it follows naturally.

The wheel's info panel asks the mod what to show, rather than the engine
guessing:

```
virtual String PlayerPawn.GetVRWheelInfo(Inventory item, int hand)
```

Panel text obeys colour escape codes instead of printing them literally.

## 7. HUD stereo gating (bug fix)

`vr_hud_mount` defaults true and is `CVAR_GLOBALCONFIG`, and the mounted-HUD
condition never checked whether a stereo mode was actually running. On a flat
desktop session the engine took the mounted branch anyway and skipped the
entire 2D layer — status bar, view border, and every mod `RenderOverlay`
handler at once — while still rendering the HUD into an offscreen VR surface
that nothing composites.

Gated on `vrmode->IsVR()`, which the same block already used for its own debug
border. Full write-up, including why it reads as a one-second delay, in
[`HUD_STEREO_GATING.md`](HUD_STEREO_GATING.md).

## 8. Glow wave

A glow varies per pixel going **up** a wall — `glowdist` is the fragment's
distance from the plane, which is what makes coverage and falloff smooth. It
could not vary **along** the wall at all, because reach arrives as one number
for the whole surface. So a wall faded beautifully top to bottom and had a dead
straight top edge from one end of a room to the other.

Sweep recolour (mode 4) already worked around this for *colour*. Nothing
addressed *shape*. This does.

```
Level.SetGlowWave(wavelength, speed, sharpness, shape)      // wavelength 0 = off
Level.SetGlowWaveOrigin(origin)
Level.SetGlowWaveDepth(reach, bright, colour, detune, seed)
Level.SetGlowWavePhase(wallTop, wallBottom, floorPhase, ceilPhase)
Level.ClearGlowWave()
```

`GlowWaveRaw()` in `main.fp` returns a signed −1..+1 modulation for the
fragment. Three separate depths read it, and they look nothing like each other:

- **reach** — scales the glow's reach *before* the cutoff test, so the band's
  **edge** rises and falls. This is the one that cannot be produced any other
  way; multiplying the finished contribution only pulses a straight-edged band
  brighter and dimmer.
- **bright** — multiplies the contribution. Straight edge, moving light.
- **colour** — offsets the near/far mix instead, so the two-colour boundary
  slides up and down *inside* a band whose shape never changes. Requires the
  far colour from §2 to be set; on a one-colour glow it does nothing.

The far-colour ramp already rides `atten`, so the corner gradient stretches and
squashes with the edge without any extra work.

**The distance function is the sweep's.** Same five shapes, same per-band origin
vocabulary. Not tidiness: it means a wave running along a floor and a sweep band
crossing the room can be given one shape and made to *arrive together*. Two
systems that measure the world differently can never be lined up; two that share
a distance function line up by construction.

**Detune** adds a second sine at an irrational multiple of the first. One sine
is legible as machinery within about ten seconds because the eye finds the
period; two that never resolve are not. One extra `sin`.

**Per-room scatter** (`seed`) offsets phase by a hash of geometry that is
already uploaded — the glow plane's height for a wall, the first linedef
endpoint for a flat. Without it the whole map undulates as one organism, which
reads as a filter over the game rather than as lighting in it. No new data.

### Two things it needed from elsewhere

`uFlatGlowPad1` became **`uFlatGlowIsCeiling`**. Floor and ceiling time-share
`uFlatGlowColor` (§2), so the fragment shader had no way to tell them apart —
and they need different phases, or a wave cannot climb a room. It was a pad, so
`MAX_STREAM_DATA` is unchanged.

The parameters live in **`HWViewpointUniforms`, not `StreamData`**. They are
identical in every draw of a frame; `StreamData`'s size divides 64KB into
`MAX_STREAM_DATA` draws, so spending 64 bytes of it to say the same thing
thirty-four times would cost batching in every frame of the game. The viewpoint
block is written a handful of times per frame. The four `vec4` are appended
after `mLightBlendMode`/`mPadding0`; those end at 28 bytes and std140 aligns a
`vec4` to 16, so the compiler pads to 32 and the GLSL offsets match the C++
struct without the padding int being declared in either shader. **Do not insert
a scalar before them.**

`timer` already existed in `StreamData` as a live per-frame float, so no clock
uniform was added.

### A bug fixed alongside

Sweep recolour reached **two of the four glow channels**. Both wall blocks mixed
`sweepTint`; the flat-edge block never did, so a recolour band sweeping a room
changed the walls and left the floor and ceiling on the old palette — visible as
the band crossing a corner and stopping dead at it. `main.fp`, one line.

GLES implements none of this, consistent with §2.

## 9. Per-fragment darkness

A darkness mod darkens by scaling each **sector's** colour: one multiplier, one
room, wall to wall. That was correct when a sector's light level was the only
lever there was. It stopped being correct once a band of light could be measured
per pixel — and the tell was that the *reveal* worked by multiplying back up
exactly what the darkness had multiplied down. Two features undoing each other
and calling the result a lighting model.

```
Level.SetDarkness(mode, adjust, minLight, preGain, postGain)   // mode 0 = off
Level.SetDarknessSpace(distDepth, distRange, heightDepth, heightRef, heightRange)
Level.ClearDarkness()
```

`DarknessAt(lightLevel)` in `main.fp` returns the fraction of light that
survives. **The four curves are unchanged** — subtract, compress, cap brightest,
deepen shadows, with pre-gain before and min-light and post-gain after, in that
order. They are transcribed from the ZScript that had them, which took them
verbatim from DarkDoomZ. They work in Doom's 0–255 light because every constant
in them (256, 33, /8) is in those units; the conversion happens at the top
rather than rescaling the curves, so they stay readable against the original.

`adjust` is pre-multiplied by the caller (`32 ×` a 0–8 dial, in the original) so
the shader never has to know what a "preset" is.

**Where it is applied is load-bearing.** In `getLightColor`, *after* the Doom
lighting equation — so it scales the light the room actually ended up with,
including whatever the blink/flicker/strobe thinkers did this tic, which the
per-sector version had to fight for — and *before* the glow and sweep blocks,
because those are **emissive**. They are light being added, not light the room
has. Darkening them would darken the only thing left to see in a black room.

The fog path has no scalar light — the colour *is* the light — so its luminance
stands in, which keeps both paths agreeing about how dark a room is instead of
one quietly opting out.

### The two terms a sector cannot express

- **distance** — `distance(pixelpos, uCameraPos)`, deepening with range. This is
  the one that makes a dark room feel like it has depth rather than like the
  brightness slider went down, and it is flatly impossible per sector: every
  pixel in a room is the same distance as far as a sector multiplier knows.
- **height** — dark pooling below a world Z, or rising as a tide.

Three `vec4` in `HWViewpointUniforms`, appended after §8's four, same alignment
rule.

### Known gap

**Sky scaling is not implemented in the per-fragment path.** The per-sector
version scales the *adjustment* (not the result) for sectors whose floor or
ceiling is the sky flat. The fragment shader has no per-draw sky flag; adding
one means another pad plus writes in both `hw_flats.cpp` and `hw_walls.cpp`.
`uSweepPad1` is still free and is the obvious place. Until then, sky sectors are
darkened by the unscaled adjustment.

### Consumers should treat this as a mode, not a replacement

Every level of every curve was tuned against a per-sector multiply and will not
feel identical through a per-fragment one. The two must never both run — the
curve would apply twice and every value on the dial would read wrong, in a way
that is confusing precisely because both halves are behaving correctly.

Also worth knowing: **per-fragment darkness is invisible to the playsim.**
Nothing can ask "is this spot dark?" any more, because the answer is no longer
stored anywhere. Content that needs it must keep a per-sector shadow of the
term.

## 10. Non-pausing menus

A menu pauses the game in single player. That is right for almost every menu and
wrong for one whose entire purpose is to adjust what you are looking at: nothing
re-evaluates while the playsim is stopped, so a slider takes effect when you
back out rather than when you move it — and anything driven from a tic is simply
frozen while you look at it.

```
DontPause   // bool on DMenu, alongside DontDim / DontBlur
```

`P_CheckTickerPaused` already had a `MENU_OnNoPause` case; this adds a third
condition, `M_MenuPauses()`, which consults the menu currently on top. Only the
top menu decides, so a non-pausing page opened from a pausing one runs — which
is what keeps a submenu behaving like the page that opened it instead of
freezing halfway down a chain.

Opt-in per menu, and the cost is real: monsters keep moving and the player can
be hurt while the page is open. That is the right trade for a lighting page and
the wrong one for the save menu, which is why it is not a global.

## 11. Fog slab

Sector fog is a distance tint on **surfaces**: the further a wall is, the more
it blends toward the fog colour. Nothing is simulated in the air. That is why
it has no shape — no ceiling, no thickness, and no way to be brighter where a
light passes through it. You cannot be knee deep in it, because it has no
knees.

This adds a horizontal slab of participating medium with a world-space top.

```
Level.SetFogSlab(topZ, density, softness, scatter, col)   // density 0 = off
Level.SetFogWake(pos, radius, strength)
Level.SetFogPickup(amount)
Level.ClearFogSlab()
```

### Analytic, not raymarched

`FogSlabAt()` in `main.fp` solves it in closed form. For a flat-topped slab the
answer is exact without marching: find how much of the eye-to-fragment ray lay
below the ceiling, and fog by that length.

```glsl
float dEye  = smoothstep(topZ + soft, topZ - soft, uCameraPos.y);
float dFrag = smoothstep(topZ + soft, topZ - soft, fragPos.y);
float travel = distance(eye, fragPos) * 0.5 * (dEye + dFrag);
float amount = 1.0 - exp(-density * travel * 0.001);
```

No loop, no step count, no undersampling banding, and the cost is a handful of
ALU. The `smoothstep` across the soft band is not decoration — a hard cut at
the ceiling reads as a sheet of coloured glass lying across the room, and the
fade is what turns it into a *surface* you can look down at.

Averaging occupancy at the two ends is exact for a linear ramp and its error
against a true integral through the smoothstep is far below what the eye
resolves in fog.

### Three terms that make it a substance rather than a filter

**Pickup** mixes the fog toward the colour of the pixel behind it. Without it
the slab is a flat colour laid over the scene: mist standing in front of a red
glowing wall stays its own colour, which is wrong in a way that is instantly
obvious even if you cannot name it. A true scattering integral would gather
light along the ray; this gathers it from the one place it is already known —
the fragment behind the fog, which carries the wall, its glow, and any sweep
band crossing it. One `mix`, no extra sampling.

**Scatter** brightens fog inside the flashlight cone, so the torch lights the
mist it sweeps.

**Wake** thins the slab inside a radius around a point that *lags* the player.
The lag is the whole effect — the disturbance is where you were a moment ago,
so walking drags a thinned channel behind you that closes as the point catches
up. A wake pinned to the player is a hole you carry, not a trail. One point on
a spring rather than a history buffer, because a trail that settles *is* a
point that follows you slowly, and a ring buffer of positions would need a
uniform array.

### The beam had to be duplicated, and why

The volumetric beam (§4) is a **postprocess** pass working in **view** space;
its uniforms are not reachable from `main.fp`. Rather than plumb a second copy
of the beam through the postprocess chain, the three values the scatter term
needs — world position with length, world direction with `cos(inner)`, colour
with `cos(outer)` — are handed to the fragment shader in the viewpoint block as
`uFogBeamPos/Dir/Col`, filled from the same `FLevelLocals` fields the
postprocess pass reads.

### Where it is applied, and why that is the opposite of §9

The slab runs **last, over everything**, in `main()` after `ApplyFadeColor`.

That is deliberately the reverse of per-fragment darkness, which runs *before*
the glow so emissive light survives being in a dark room. Fog is not darkness —
it is a substance sitting *between* the eye and the surface, so it occludes
whatever is behind it including the glow. A glowing floor seen through
knee-deep mist should be a glow diffused by mist, not a glow with mist politely
behind it.

### Uniforms

Seven `vec4` appended to `HWViewpointUniforms` after §9's, same alignment rule
and same reasoning about `StreamData`: `uFogSlab`, `uFogSlabColor`,
`uFogSlabWake`, `uFogBeamPos`, `uFogBeamDir`, `uFogBeamCol`, `uFogSlabExtra`.

`uFogSlabExtra` carries wake strength and pickup. It has its own slot rather
than being packed into a spare component of an existing one — the first attempt
overloaded `uFogSlabColor.w`, which was already wake strength, and the two then
could not be set independently. Worth stating because the temptation to reuse a
spare `w` is exactly how that happens.

**Correction, verified against the source.** That last paragraph describes an
intention the code did not follow through on. `hw_drawinfo.cpp` still writes the
wake strength into **both** `mFogSlabColor.w` and `mFogSlabExtra.x`, and
`main.fp` reads `uFogSlabExtra` exactly once — at the pickup mix, taking `.y`.
`uFogSlabExtra.x` is uploaded every frame and never read anywhere in the shader;
the wake gate still tests `uFogSlabColor.w`. So the independence the slot was
added for was never actually taken up. Do not build anything on
`uFogSlabExtra.x`, and do not assume the wake can be silenced by zeroing it.

**Sprites were fogged against the wrong plane. Fixed.** `FogSlabAt` resolves the
slab's top through `uGlowTopPlane` / `uGlowBottomPlane` (`SetFogFollow` — listed
in §27, since no section ever gave it an API entry), and those are set per draw
by `hw_walls.cpp` and
`hw_flats.cpp` — but were never set by `hw_sprites.cpp`, which called neither
`SetGlowPlanes` nor `EnableGlow`. `FRenderState::Reset` zeroes them, and
`FogSlabAt` is invoked at `main.fp:3106`, *before* the
`dot(vWorldNormal, vWorldNormal) > 0.5` sprite guard further down. So with a
non-zero follow, an actor standing in a pit was fogged as though the mist top
were computed from whichever wall or flat happened to be drawn before it, or
from zero after a state reset — reading as over-fogged by roughly the pit's own
depth, and changing with draw order.

`HWSprite::DrawSprite` now sets the planes from the sprite's own sector before
drawing: `actor->Sector` for a thing, `particle->subsector->sector` for a
particle, nothing if it has neither. `SetGlowPlanes` is declared locally in
`hw_sprites.cpp` because it lives in `hw_walls.cpp` and was never in a header —
until now nothing outside that file wanted it.

**Planes only.** `EnableGlow` is deliberately not touched, so sprites still do
not receive sector glow: that is gated on `uGlowTopColor.a` /
`uGlowBottomColor.a` rather than on the planes, and walls set it back to false
after themselves (`hw_walls.cpp:432`). The planes are the fog's input here and
nothing else's.

Density 0 switches the whole thing off and the fragment shader returns
immediately.

GLES does not implement this, consistent with §2 and §8.

## 12. Sweep band fill

A sweep band (§3) knows two things about every pixel it covers: **how strongly**
it covers it, and **where that pixel is**. It discarded the second and blended
one flat colour weighted by the first, so a band could only ever be a wash.

The observation that makes a pattern cheap: **every shape that defines a
distance also implies two tangent coordinates**, and a pattern is a function of
those two.

| shape | distance (what makes the band) | pattern runs in |
| --- | --- | --- |
| bar E/W | `abs(x - ox)` | `z`, `y` |
| bar N/S | `abs(z - oz)` | `x`, `y` |
| rising | `y - oy` | `x`, `z` |
| ring | `length(xz - o)` | arc length, `y` |
| shell | `length(xyz - o)` | longitude, `y` |

So one function draws a lattice standing across a corridor *and* a cage on an
expanding cylinder, with no per-shape casing beyond picking the two axes.

```
Level.SetSweepFill(spacingU, spacingV, width, soft, col, gap)
Level.SetSweepFillMotion(rotate, drift, major, majorBoost, jitter, flicker, grad, gradAxis)
Level.SetSweepBandFill(index, fill)   // 0 none, 1 grid, 2 dots, 3 solid slab
```

### Two decisions worth stating

**Spacing 0 in an axis means no lines in that axis.** That single rule collapses
grid, slats and a lone tripwire into one mode with one number changed, which is
why there is no enum entry for any of them. Fewer modes, more range.

**Line width is in world units, not a fraction of the spacing.** A fractional
width makes a lattice coarsen with distance and shimmer under motion. A world
width holds its real size and antialiases against `fwidth`, so a line a hundred
units away is one clean line rather than moiré.

Arc length is used for ring and shell rather than raw angle, for the same
reason: an angular grid spreads apart as the band expands, which is a fan
rather than a cage.

### Colour: the band is the field, the fill is the lines

`gap` is how much of the **band's own colour** fills the space between lines.
0 means only the lines are lit and the room shows through between them, which
is what reads as actual lasers; 1 makes it a lit pane with structure in it —
a completely different object. Negative inverts: lit gaps, dark lines, a grid
of shadow.

### Per band without costing a uniform

Only the **mode** is per band, and it is packed into the draw mode's spare
bits: `drawmode + 16 * fill` in `uSweepBands[i].w`, decoded with a mask and a
shift.

That component has form — it began as a bare on/off flag hardcoded to `1.0`,
four bytes of nothing, and became the draw mode at no cost. Draw mode is 0–4
and always will be, since it names the four things a band can do to a pixel,
so the rest of the float was free.

The **style** — spacing, width, softness, rotation, drift, jitter, flicker,
gradient, major lines — is frame-global in `HWViewpointUniforms`. Per band
would mean another `vec4[8]` in `StreamData`, and that buffer's size divides
64KB into `MAX_STREAM_DATA` draws, so it would cost draw batching in every
frame of the game to let band 3 have a different line width from band 4. In
practice the limit barely bites: a train can still be a solid wall, then a
lattice, then travelling darkness, because that is all mode.

One trap this created and the fix for it: `SetSweepBandDraw` was only called
when a band overrode its draw mode, and `SetSweepBand` seeds mode 1 into that
component — so a band with a fill but no draw override would have had its fill
silently dropped. The call site now fires when **either** is set.

GLES does not implement this, consistent with §2, §8 and §11.

## 13. Beams

A laser in Doom is usually a sprite, or a chain of puffs spawned close enough
together to read as a line. Both show what they are: the sprite lights nothing,
and the chain stitches, gaps at long range, and costs an actor per segment.

A beam is a **segment**, and the honest way to draw one is the way a sweep band
is drawn — light every pixel by its distance from the thing. The only
difference is which distance:

```
sweep band   distance from a POINT     length(p - origin)
beam         distance from a SEGMENT   length(p - closest(a,b))
```

```
Level.SetBeam(index, start, end, thick, soft, col, intensity)
Level.SetBeamCount(count, glow, fogScatter)
Level.ClearBeams()
```

Because it is per pixel in world space, everything else follows without being
asked for. The beam is **continuous** at any length, with no repeat and no
stitching. It **wraps** floor, wall and ceiling as one unbroken object. And the
surfaces near it brighten because they *are* near it, not because something
also spawned a dynamic light to fake that.

### Two falloffs from one distance

This is what separates a beam that looks hot from a bright line: a hard narrow
**core** a couple of units across, and a wide soft **halo** around it. Either
alone reads as a drawn line or a smear. Together they read as incandescent.

The closest point is clamped to the **segment**, not the infinite line — that
clamp is what makes a beam end where it ends instead of lighting everything
along its axis to the edge of the map.

### Not a sweep band, deliberately

A band's radius is a distance that grows. A beam does not travel; it simply
is. Sharing the sweep's slots would have meant a per-band endpoint — another
`vec4[8]` in `StreamData` — for a thing that is not a band.

**128 beams**, in the viewpoint block. This said "eight" until the fork was
audited against itself; eight was the original count and `a6182489c2` raised it
so a real firefight can have bolts crossing in both directions.

Cost is per **active** beam, not per slot: both shader loops break at the live
count and each survivor gets a bounding-sphere reject before the real solve, so
an empty slot costs nothing. 3 arrays × 128 × 16B = 6KB per viewpoint, 12KB for
two eyes against a 64KB range.

The index space is **caller-managed with no allocator** — two mods writing beams
will overwrite each other silently. Agree a range.

**Interpolated between tics.** Script sets beams at 35Hz and the upload runs
every frame, so without a lerp a beam holds still for a whole tic and then jumps,
which at 90–120Hz reads as stuttering against smoothly moving geometry.
`PrevBeamStart/End/Intensity` are snapshotted each tic (`p_tick.cpp:377`) and
blended at render rate (`hw_drawinfo.cpp:245`), behind `r_beam_interpolate`. Only
a beam that was already lit **and still is** gets interpolated — callers park a
released slot at the origin, and lerping toward that would draw a beam whipping
across the map on the frame it was switched off. Beams are also cleared on map
change (`p_setup.cpp:307`) so nothing interpolates from the level you just left.

Raising the count is also what produced the misalignment recorded in §18: the
C++ array and `main.fp` were widened and the two GLSL declaration lists were
not, so for several commits every uniform after the beams read at the wrong
offset on both backends. Fixed in `f228cc23ea`. **Four lists, not three.**

### They light fog

When the fog slab (§11) is on, beams add into it, so a laser through knee-deep
mist is a visible shaft along its whole length rather than a bright line on
whatever it eventually hits. Evaluated at the fragment rather than integrated
along the ray — an approximation, but the fog amount already scales with how
much mist is in the way, so mist glows near a beam and does not far from one,
which is the entire read.

**Seen in the air.** `BeamLightAt` lights surfaces; `BeamAirGlow` draws the beam
itself. That is a segment-to-ray closest approach — "how close does my line of
sight pass to this beam" — which needs no geometry, no camera-facing quad strip,
no sorting against translucents, and does not vanish viewed end-on.

It comes out **depth-correct for free**: the closest approach has a distance
*along* the ray, so clamping that to the distance to the fragment is the depth
test. A beam behind a wall is simply not drawn — the one artefact the surface
lighting genuinely cannot avoid, solved as a side effect rather than by a
shadow pass.

It feeds **bloom** without being told to, since a core burning past white is
exactly what the bloom pass thresholds for.

**Along the beam** (`SetBeamLook`): taper, scrolling energy, and an impact
flare. All three ride the position along the segment that the closest-approach
solve already produced, so none of them costs a second pass. Scroll matters
more than it sounds — a held beam with nothing travelling along it goes static
within a second and stops reading as carrying anything.

Beams are applied **after** the darkness term, with the glow, because they are
emissive. A laser that dimmed as the room got darker would be a contradiction.

GLES does not implement this, consistent with §2, §8, §11 and §12.

---

## 14. The fog slab, shaped

Three additions to §11, all of them changes to *where the density is* rather
than new systems. That distinction is the point of this section: the fog's
colour, its softness, the scatter it takes from the torch, the glow it picks up
off the walls and the light it takes from a beam crossing it are written once
and are not duplicated by any of the below. Each of these only answers the
question `FogSlabAt` was already asking — *how much mist is between the eye and
this pixel* — from a different shape.

### A bottom edge, and what one number bought

`FogSlabBottom` (`SetFogBottom`, `uFogSlab2.x`). Without it the slab is a
half-space: everything below a height, which can only ever be mist lying on a
floor. With it the slab is a **layer**, and the same code path is now four
effects depending on where the two edges sit:

| bottom | top | what it is |
|---|---|---|
| far below the map | at the knee | floor fog, the previous behaviour exactly |
| near the ceiling | above it | **ceiling fog** |
| both mid-room | just above | a band floating at chest height |
| animated toward the other | — | **drain** and **fill** |

The default is `-32768`, which is below any map, so the second test is always 1
and the old single-edge behaviour is bit-identical. Nothing that existed before
had to be told about this.

The bottom takes the **same swell** as the top, so a ceiling layer's underside
undulates. That is the surface you actually see from below, and animating only
the top would have left it a flat plate — a bug that would have shipped
invisibly, because from above it looks right.

### Vertical hold

`FogSlabPeriod` / `FogSlabRoll` (`uFogSlab2.yz`). With a period set, the layer
**repeats up the room** — a stack of them at that spacing, all rolling together,
wrapping at the top and re-entering at the bottom. The old television fault.

It is one `mod()` away from the single-layer case, not a second system, and the
whole stack costs **exactly what one layer costs**. Twenty decks or one is the
same instruction count, for the same reason the lattice in §12 can be a screen
door: a repeating thing is arithmetic, not a loop.

Sampled separately at the eye and at the fragment, so the stack has depth
rather than being a flat repeat pasted over the view.

### The tornado

`Tornado*` (`SetTornado`, `SetTornadoMotion`, `uTornado`/`2`/`3`). Density near
a **vertical axis** instead of below a plane. That is the entire difference; it
is added into the same `amount` the slab accumulates, so it takes the torch, the
pickup, and any beam crossing it, because it *is* the same mist.

Four decisions worth stating:

**The centre is hollow.** Density peaks at the wall of the funnel and falls to
nothing in the middle (`smoothstep(0, radius*0.55, r)` against
`1 - smoothstep(radius*0.8, radius, r)`). This is not a performance concession —
it is what lets you **stand inside one** and see out through the far wall with
the room beyond it still legible. A solid cone would be an opaque box you cannot
be in.

**The radius flares on a curve** (`mix(base, top, pow(h, 0.55))`) rather than a
straight taper, because a straight taper is a cone and the pinch near the ground
is most of what makes the silhouette read.

**Swirl is what reads as rotation**, not spin. Three arms of density around the
axis, wound up over the height by `twist` and turned over time by `spin`. With
swirl at 0 the funnel is a smooth cone of haze and does not appear to rotate at
any speed, because there is nothing on it to watch go past.

**Lean scales with height**, so the column bends rather than tilting like a
rigid pole, and the foot stays where it was put while the top wanders. Two axes
at slightly different rates (`lt` and `lt*0.83`), so the top traces a wobbling
ellipse; a circle reads as a mechanism turning.

The one cost worth flagging to anyone consuming this: **it does not early out
the way a floor layer does**. A knee-high slab stops mattering the moment the
player looks up. A funnel is on screen from every angle that can see it. Density
0 is tested before any of the maths, so off is genuinely free, but on is the most
expensive thing in this shader. It is off by default and the menu says so.

### Uniforms

Appended to `HWViewpointUniforms`, all three copies (C++, `gl_shader.cpp`,
`vk_shader.cpp`) — remember these are matched **by byte offset, not by name**:

```
uFogSlab2   x bottom Z, y repeat period, z roll speed, w unused
uTornado    x world X, y world Y, z base height, w top height
uTornado2   x base radius, y top radius, z density (0 = off), w swirl depth
uTornado3   x spin, y twist, z lean, w lean period in SECONDS
```

`uTornado3.w` is a period in seconds rather than an angular rate, converted to
radians in the shader, so the slider driving it means what it says without the
person moving it needing to know 2π.

GLES does not implement this, consistent with §2, §8, §11, §12 and §13.

---

## 19. Direct model frame addressing on psprites

A HUD weapon model gets its frame through the *sprite*. `psp->Frame` is a
sprite letter index; MODELDEF's `FrameIndex` maps that letter to a model
frame; `FindModelFrame` looks the pair up and hands back an
`FSpriteModelFrame` carrying scale, offsets, skins, flags — and the frame
number.

That channel is one character wide. `MAX_SPRITE_FRAMES` is **29**, inherited
from Doom's 8-character lump names where the frame is a single character and
Boom pushed it as far as `]`. Model meshes have no such limit: the weapon
models this fork ships run to 75 frames. Everything past the 29th was
unreachable — not awkward, *unaddressable*, because there was no letter left
to name it with.

That is fine for a weapon whose own states drive its own model. It is fatal
for playing one weapon's animation on a different weapon's timing, which is
what the model-swap program does: it puts our meshes on weapons from other
mods and needs to drive Raise/Ready/Lower/Fire/AltFire/Reload frame ranges
against whatever tic counts the foreign weapon happens to use.

Raising `MAX_SPRITE_FRAMES` is not the fix. Three more ASCII characters exist
after `]` and then it is lowercase, which lump names case-fold away — call it
32 against a requirement of 75. It is also baked into
`spriteframewithrotate sprtemp[MAX_SPRITE_FRAMES]` and the lump-name scanner
in `sprites.cpp`, so it would change how every sprite in the game parses to
buy something only the model path wants.

So the encoding is skipped rather than stretched.

### Three fields on DPSprite

`p_pspr.h`, next to `Tint`/`Glow` and for the same reasons:

```
int   ModelFrame     = -1;   // model frame to show, bypassing the sprite
int   ModelFrameNext = -1;   // frame to blend toward
float ModelFrameLerp = -1.f; // 0..1 blend factor; <0 = stock timing
```

**In-class initialisers, not constructor-body ones.** Identical trap to §
Tint/Glow: `DPSprite` has a private argument-less constructor used only by
savegame deserialisation, which runs none of the public constructor's body,
and the serialiser leaves unknown fields alone when reading an older save. A
garbage `ModelFrame` indexes a model's frame array with a random int.

They live on the psprite rather than the weapon actor so the two hands animate
independently — mainhand and offhand are separate layers (`PSP_WEAPON` /
`PSP_OFFHANDWEAPON`), and `AActor::modelData` is per-actor.

Serialised in `DPSprite::Serialize`, exported via `DEFINE_FIELD`, declared
`native` in `player.zs`.

### Where the frame is replaced

`CalcModelOverrides` (`models.cpp`), after every existing branch — the
`modelFrameGenerators` path, the plain `data` path and the no-`data` path all
resolve their frame first and are then overridden together.

Only the frame *number* is replaced. Scale, offsets, angle/pitch/roll, skins
and flags still come from the `FSpriteModelFrame` the sprite lookup returned,
which is why `FindModelFrame` must still succeed upstream. This is an override
of one integer, not a bypass of the model system.

The override applies to every model index. A donor with several models is
showing frames of one animation, so they advance together; per-index
divergence would need an array and nothing needs it yet.

Out-of-range frames are **not** clamped, deliberately.
`FMD3Model::RenderFrame` rejects `(unsigned)frameno >= Frames.Size()` and
draws nothing — a visible failure. Clamping to the last frame would hide the
bug, and there are already two live cases of exactly this mistake in
hand-written MODELDEF (`RS_PS_Chainsaw` maps `SAWF C/D` to frames 6–7 of a
6-frame mesh; `RS_PS_SSG` maps `SSGA A/B` to 12–13 of a 12-frame mesh).

### Explicit interpolation

Stock `inter` is derived from state tics in `CalcModelFrame` and only tweens
across a state transition. Playing our animation across someone else's state
timings, that blend restarts on every state change and sits at zero between
them, so the model snaps from pose to pose.

When `ModelFrameLerp >= 0` it is taken verbatim. Two details make it work:

- `smfNext` is forced to `smf`. `RenderModelFrame` discards `inter` unless
  `frameinfo.smfNext` is non-null; same definition, different frame number, so
  `smf` is the correct "next" here.
- The override sits *after* the `gl_interpolate_model_frames` /
  `MDL_NOINTERPOLATION` branch, so an explicit blend is not silently dropped
  when a user turns that CVar off.

### The threading, and the landmine in it

`RenderHUDModel` had the `DPSprite` in hand and passed only `psp->Caller`, so
the psprite never reached the code that picks a frame. It is now threaded
`RenderFrameModels` → `CalcModelFrame` → `CalcModelOverrides` as a trailing
`const DPSprite* = nullptr`. World models pass the default and are unaffected.

**`hw_vrwheel.cpp` carries its own forward declaration of
`RenderFrameModels`** and does not include `models.h`. A signature change
there is a link error, not a compile error, and the two must be kept in step.
The declaration is commented to say so.

### hasmodel exported

`AActor::hasmodel` is now a `native readonly bool` in `actor.zs`. It is set on
the class defaults by the MODELDEF parser and is the same flag
`FindModelFrameRaw` gates on, so it answers "does this class have a model at
all" — which is what the swapper needs in order not to paint over a mod that
already ships 3D weapons.

Read it off `GetDefaultByType`, never off a live actor: `EnsureModelData`
sets it on the *instance* as a side effect of `A_ChangeModel`, so an instance
read reports true for anything already swapped.

---

## 15. Reactive fog

**A wake, a ripple, an ignition, fog draining from a point and a monster
shouldering mist aside are the same function** — a point, a radius, an age, a
strength and a sign. They differ only in whether the radius grows with age, and
whether the result subtracts density, adds it, or adds light.

So there is one array of eight slots (`mFogDisturbA/B`, `Level.FogDisturb`)
rather than five features, and everything reactive built on it afterwards is a
script call with no engine change at all.

| mode | shape | what it does |
|---|---|---|
| 0 `DISC` | fixed radius | thins the mist — wakes, and actor displacers |
| 1 `RIPPLE` | ring at `r = age × speed` | a wave travelling out, crest and trough |
| 2 `IGNITE` | expanding sphere | adds **light**, not density — works in clear air |
| 3 `GOUT` | expanding disc | adds mist — a vent, a burst |

The ring recycles the **oldest** slot rather than refusing a ninth. Refusing
makes the ninth gunshot of a firefight silently do nothing, which is the exact
moment the effect exists for.

Age is resolved outside script rather than being counted down by the caller.
Strength decays over the slot's life, so nothing is freed on a schedule — an
expired slot is one whose strength reached zero.

**Fixed, having been wrong since it was written.** This section and the comment
beside the code both claimed the age resolved at *render rate* so a ring would
not expand in visible 35Hz steps. It did not — `hw_drawinfo.cpp` computed
`double now = Level->maptime / (double)TICRATE;` with no `TicFrac` term, at both
the disturbance site and the shape site, while the beam block ~130 lines earlier
had taken `Viewpoint.TicFrac` explicitly all along. The staircase the paragraph
warns about was real for small fast rings, and for a shape's growth and seam.

Both sites now read `(Level->maptime + Viewpoint.TicFrac) / (double)TICRATE`.
Unlike the beam path this is not put behind `r_beam_interpolate`: that cvar is
about beam *positions* stuttering against moving geometry, which is a different
question from an age advancing smoothly, and `cl_capfps` / `r_NoInterpolate`
already pin `TicFrac` to 1.0 upstream so those cases collapse to the previous
behaviour on their own.

### Density stopped being one number

`mFogNoise`. Uniform density was the single biggest tell that this was a filter
rather than a substance: real mist banks, thick in corners and thin across the
open. One noise sample scales it, and drifting the field makes the banks travel
with nothing animated.

Sampled at the **midpoint** of the eye-to-fragment segment, not at the fragment.
The fog for a pixel is an integral along that whole line; sampling the far end
pins the banks to the walls, so you walk through one and watch it stay put.

### Tendrils, as a lattice

`mFogTendril`. Wisps rising off the surface — one per cell of a `fract()` grid
rather than one object each, so four hundred cost what one costs. It is the
tornado's own maths at small scale, and the same trick §12's lattice uses.

Each tendril's hashed offset is **bounded so it cannot leave its own cell**,
which is what allows sampling one cell instead of the nine around it: a ninefold
saving for a constraint nobody can see, since the offset still moves it far
enough off the lattice that the grid does not read as a grid.

### Two more, both nearly free

`mFogBow` — a sweep band piles mist against its leading face and scours it out
behind, from a distance function the band already computes.

`mFogWake2` — the wake stretches along the direction of travel. **Only the
trailing half.** Stretching both ends clears as much air in front of you as
behind, which is a bubble rather than a wake.

---

## 16. The heatmap

Where the fighting happened, accumulated over the whole life of a map and
painted on the floor. `Level.HeatmapAdd`, `Level.HeatmapAt`, `Level.SetHeatmap`.

**Deliberately not the disturbance array.** That is eight short-lived events in
uniforms; this is hundreds of permanent deposits that have to be *summed*, and a
sum wants a bucket rather than a list. So it is a 256² grid over the map's own
bounding box (from the blockmap), and the thousandth death costs exactly what
the first one cost. Resolution is not exposed — the deposit radius is in world
units instead, so a slider means the same thing on a cramped map as an open one.

`HeatmapAt` reads it back, which is what makes it a design tool rather than only
a picture: a spawn director can weight against ground already fought over.

### Drawn as a postprocess pass, and why

`PPHeatmap` + `shaders/pp/heatmap.fp`. **Four files, none of them backend
files.** The alternative — a sampler in the scene shader — would let the heat
tint the *light* rather than paint over the frame, and be occluded correctly by
translucent geometry. It would also need four coordinated Vulkan edits: a GLSL
binding, a descriptor set layout, a descriptor **pool size**, and a per-frame
descriptor write. Missing the pool size fails silently in review; a layout
mismatch is a validation error on every draw. `PPRenderState` already handles
descriptor allocation and image transitions and is backend-agnostic.

The pass reconstructs world position from depth and a `ViewToWorld` matrix. Two
traps, both live in this file's history:

- The depth **sample** is a nonlinear 0..1 value, not a distance. Linearised
  with the same two constants `lineardepth.fp` uses.
- The ray is **scaled, not normalised**. The depth buffer measures along the
  view axis; normalising first places every pixel too far out except at the
  exact centre of the screen.

**The grid is flat and the world is not**, so each cell stores the height of
whatever deposited most of its heat and a fragment too far from it is rejected.
Without that, a kill on a balcony marks the ground beneath it and every wall
over a hot cell is painted up its full height — which is what would give away
that this is a screen effect.

Two `R32f` textures rather than one two-channel one: the CPU side already holds
two plain float arrays and `R32f` takes them verbatim. Packing into `Rg16f`
would mean hand-rolling half-floats to save one sampler.

Sampled `Linear`, because a heatmap that reads as tiles reads as a debug
overlay. Coloured by `sqrt` of intensity, because a heatmap is read by comparing
regions and a linear ramp spends most of its range on the difference between
nothing happening and one thing happening.

The grid is **copied** before upload. `PPTexture` keeps its data through a
`shared_ptr` and uploads at an unspecified later point, so handing it a pointer
into a `TArray` the playsim is still writing would race the first time two
monsters died in one frame.

---

## 17. Selective desaturation

`mDesatKeep`, `Level.SetDesatKeep`. Desaturation was all or nothing, so a
monochrome world made blood exactly as grey as the wall it was sprayed on. The
drain is now weighted by each colour's **own** saturation.

**Nothing is tagged.** No actor, sprite or texture knows it is exempt, because
the rule is about the colour and not about the thing wearing it — which is what
makes it work on blood, gore decals, keycards and score badges without one of
them being touched.

And it reaches all of them for one structural reason: **there is exactly one
`dodesaturate()` in the shader** and every path goes through it — textures,
sprites, glow, sweep bands, brightmaps, the flat-edge glow. A rule added there
lands on every one and cannot disagree with itself. This is the payoff for a
choke point that was already there.

Saturation is chroma over brightness (HSV `S`), which is what the eye reads as
"how colourful"; a raw channel difference would keep dark saturated colours that
look black anyway. The hue gate is by **dominant channel** rather than a hue
angle — no `atan`, and it answers the only question being asked: blood is
red-dominant, nukage is green-dominant.

### The drain itself, scene-global

`Level.SetDesatGlobal(amount)`, `FLevelLocals::DesatGlobal`, carried in the
**`w` component of `mDesatKeep`** — which the upload site had been writing as a
literal `0.f`, so this costs no new uniform and no layout change.

`SetDesatKeep` decides what *survives* desaturation. This is how much
desaturation there is to survive. Until it existed, the only way to grey a map
from script was to walk every sector and rewrite its colormap byte — which is
the same per-sector mutation `SetDarkness` (§10) exists to spare a mod, and it
carries the same costs: it fights anything else that touches sector colour, a
savegame stores the *modified* values, and switching the effect off means
restoring every sector by hand rather than passing a zero.

The shader takes `max(uDesaturationFactor, uDesatKeep.w)` rather than adding or
replacing, so a sector a mapper deliberately drained harder than the global
stays drained harder, and dropping the global back to 0 restores it exactly.

Clamped in the thunk rather than the shader: a caller passing `2.0` means "as
grey as possible" and should get that, not a wrapped value.

Threshold 0 skips the whole block and the result is bit-for-bit unchanged.

---

## 18. Two bugs worth recording

Both were silent, both survived review, and both are the kind that repeat.

### The volumetric cone had never drawn a lit pixel

`volumetricbeam.fp` bounded its march with a general ray/cone intersection. That
solve is **degenerate when the apex is at the eye** — which is the normal case,
not an edge case: `AttackPos` *is* the eye position, and the head and chest
mounts are within a few units of it. Apex at origin → `co`, `b` and `c` all zero
→ discriminant zero → both roots zero → `tMax <= tMin` → return black. Every
pixel, every frame, since the pass was written.

It is also the case that needs no quadratic at all. Apex at the eye means the
ray is either inside the cone or not — one dot product — and if it is, the lit
stretch runs from the eye to whatever stops it. **The general solve was asking a
degenerate question robustly instead of asking an easy question.**

Independently, the same shader clamped its march against the raw depth sample as
though it were map units, capping `tMax` under one unit whenever anything was on
screen.

### And fixing one of them made the other visible

The pass integrates along the ray: average contribution × density × **marched
length**. The last multiply is correct — it turns a mean into an integral — but
the units were never stated. Density is a per-unit figure multiplied by a length
in map units, so a beam crossing a thousand-unit room comes out a thousand times
its dial.

Nobody saw it, because the broken depth clamp had capped that length at 1.0 and
made the scale *accidentally sane*. Repairing the depth made the length real and
the beam arrived correct in shape and three orders of magnitude too bright,
straight into an additive pass that runs **before bloom**. Now normalised per
1000 units, the same convention the fog slab uses.

**When two faults cancel, repairing the first looks exactly like causing the
second.**

Then a third, from the same family: a cone seen end-on is a **disc**. Look down
your own torch and the cross-section you look through is the whole cone, so it
fills the middle of the screen. The integral is right and the result is useless —
a wash centred on the crosshair carries no information about the beam, because
the beam is where you are already looking. On a flat screen the default mainhand
mount tracks the view, so end-on is the *only* way it is ever seen. Faded by how
well the view axis agrees with the beam axis (`vol_beam_axisfade`); in view space
the view direction is exactly `(0,0,-1)`, so the test is one component.

### A padding comment that was wrong about itself

`VolumetricBeamUniforms` ended `DustTime, padding0, mat4` — std140 aligns a
`mat4` to 16 and the C++ struct does not, so world-space dust was sampled
through a matrix built from shifted floats.

The repair added **two** pad floats where the row needed one, pushing
`ViewToWorld` to offset 100 where std140 expects 112 — *while its own comment
claimed to be fixing exactly that alignment.* It was only caught when a later
field forced a recount. The byte offsets are now written out row by row in the
header so the next person counts instead of eyeballing.

**A comment asserting an invariant is not the same as the invariant holding.**

### There are FOUR uniform lists, not three

The header said a new field in `HWViewpointUniforms` means editing three
declarations matched by byte offset. Wrong. Vulkan reaches the block through
`viewpoints[HW_VIEWPOINT_INDEX]`, so `vk_shader.cpp` also carries a `#define`
per field mapping the bare name onto that array access.

A field can be present and correctly aligned in all three declarations, pass
every check of the documented three-way agreement, and **fail to compile on
Vulkan with "undeclared identifier" while OpenGL runs perfectly** — because on
GL the block is declared plainly and no defines exist. `uTornadoCol` shipped
that way for a session; adding eight more fields turned the silent gap into a
hard startup failure.

The check that catches it is not "do the three lists match" but "is every field
in the VK struct also `#define`d", and both are one line each. They are now
written at the top of `hw_viewpointuniforms.h`.

**The general lesson, and it has now cost three separate days on this fork:**
*turning something off is a thing you do, not a thing you skip.* A push that
returns early leaves the last value live in engine state, so the feature keeps
costing its full per-fragment price while the switch naming it reads Off. Seen
as eight beams standing in an empty room, as a tornado whose density uniform was
only written when floor fog happened to be on, and as the entire render push
freezing whenever a sweep ran with underlay off.

---

## 20. Texture inside the glow

The glow wave (§8) varies a lane's **edge**. That is the right answer while the
edge is on screen and no answer at all once coverage saturates — turn reach up
far enough and the wall is a solid card of colour with a wave moving a boundary
nobody can see any more. High coverage is exactly where people end up, and there
was nothing left to reach for once they got there.

Five terms that happen **inside** the lit area instead. All five are multipliers
on the glow's finished contribution, so none of them can move a band's shape:
*the wave owns shape, these own substance.* All off at 0, and the whole function
early-outs on one compare when they are.

Applied to all four lanes, including the flat-edge glow, seeded identically so
the two wall lanes and the floor agree where they meet.

### Sampled in world space, not surface space

Every term reads `pixelpos` directly. A pattern crossing a wall/floor join
therefore carries **through** the corner instead of restarting at it, which is
what makes it read as something the room is made of rather than a decal stuck on
each face. It also costs no tangent frame — the two terms that need a direction
pick it from `vWorldNormal`, so "vertical on a wall, along X on a flat" needs one
`abs(normal.y)` test and no per-surface basis.

### The five

**Noise.** A lit wall was one flat brightness across its whole face; real glowing
material is veined and uneven. Two octaves of value noise scaling intensity, with
a contrast dial that takes it from marble to plasma, drifting slowly so it is not
a decal. Same move the fog's density field makes and for the same reason — *a
uniform value is the single biggest tell that something is a filter rather than a
substance.*

**Flow.** The wave arrives *from* an origin; this travels *along* the surface,
which is a different axis of motion entirely and reads as current through the
material rather than weather over it.

**Cells.** The sweep's lattice trick (§12) made organic. Lighting the distance to
the nearest cell **edge** rather than the cell body — the difference between the
two nearest sites, not the nearest one — gives veins instead of tiles, and each
cell pulses on its own hashed clock so the network crawls rather than blinking as
one. Free at any density, same as the laser lattice, because it is a pattern and
not a set of objects. Nine samples rather than twenty-seven: two axes are enough
for a surface, and the third is the one you cannot see.

**The disturbance array reaches the walls.** §15's eight slots already existed
and already fired on gunfire and death, and only the fog consumed them. A second
consumer costs nothing to build and makes a shot visibly cross the lit surfaces
of a room rather than only the air in it. Rings only — a disc that *dimmed* the
wall would read as damage to the light rather than a pulse through it.

**One state level.** Every glow in the level pulsing together, driven by nearby
monsters, player health, or a scripted number. This is the only one that makes a
lane carry *information* rather than only look good. The rate rises with the
level as well as the depth, because **faster is what reads as urgency — brighter
alone just reads as brighter.**

### Two decisions worth stating

The alarm level is resolved in ZScript, not here, because counting nearby
monsters is a playsim read — the same split the glow wave's origin and the
tornado's anchor already use. It is **eased** toward its target rather than set:
a count dropping from three to zero the instant the last monster dies would snap
every glow in the level in one tic, which reads as a bug. Eased, it reads as the
room settling.

`GITDHash21` is forward-declared here and defined further down beside the fog
field that first needed it. Same pattern as `SweepLineAxis` for the air lattice —
each hash stays next to the code it was written for, and GLSL gets its prototype.

### Uniforms

```
uGlowTex   x noise amount, y noise scale, z drift, w contrast
uGlowTex2  x flow amount,  y spacing,     z speed, w sharpness
uGlowTex3  x cell amount,  y cell scale,  z pulse speed, w vein width
uGlowTex4  x disturbance reach, y state pulse depth, z state level, w -
```

Appended to `HWViewpointUniforms` — **all four lists**, including the Vulkan
`#define` block that §18 exists to warn about. Both checks were run before
building and are recorded in the header.

GLES does not implement this, consistent with §2, §8, §11, §12, §13 and §14.

---

## 21. Native state remap

ModelSwapper's animation engine, moved into the engine — where it should have
lived from the start. §19 gave ZScript per-tick fields to force a model frame
onto a psprite; that made the *script* the animation clock, with everything
that entails: event ordering, tick timing, and silent failure when any link
in the script chain broke. This section makes the **psprite's own current
state** the clock, natively.

**The table.** `DActorModelData` (the per-instance data `A_ChangeModel`
creates) gains `TMap<intptr_t, int64_t> stateRemap` — `FState*` cast to
`intptr_t` mapping to two packed non-negative int32s: `(frame << 32) | next`.
Not serialized: state pointers don't survive a session, and binds re-register
on load.

**Registration** (`p_actionfunctions.cpp`, ZScript-callable on Actor):

```
native bool RegisterModelStateFrame(State st, int frameNum, int frameNext);
native void ClearModelStateFrames();
```

Registration fails until `modelData` exists — call `A_ChangeModel` first.
A bind registers a weapon's whole table once; after that no script runs in
the animation path at all.

**Consult points** (`models.cpp`, both shared by VR and flat, mainhand and
offhand):

- `CalcModelFrame`: when the psprite's current state is in the table,
  `inter` = intra-state progress — `(Tics - curTics + ticFrac) / Tics` —
  computed with the renderer's own frame fraction, so interpolation runs at
  display rate, not 35Hz. `smfNext = smf`, same trick as §19.
- `CalcModelOverrides`: table hit replaces `modelframe`/`modelframenext`.
  Placed after the §19 fields on purpose: a live table beats stale serialized
  per-tick values from older builds. Unmapped states fall through to the
  sprite-derived resolution (the pinned anchor's rest pose) — a pause, never
  garbage.

**Debugging** — `rs_remap_dump` (ccmd): one line per hand — weapon class,
table row count, and whether the state in the psprite *right now* resolves,
to which frames. When something looks wrong, this says what, in one line,
without relaunching anything.

§19 stays intact — the explicit per-tick fields still work and still win when
a script sets them and no table exists. The two compose: table for the normal
path, fields for manual overrides.

---

## 21b. Shapes — signed distance fields painted onto surfaces

**Undocumented until the fork was audited against itself.** This is the largest
system in the renderer with no section, which is worth recording as its own
finding: it has 128 slots, seven primitives, seven natives and a shader function
of its own, and nothing in this file mentioned it.

`Level.AddShape`, `SetShapeMotion`, `SetShapeRepeat`, `MoveShape`,
`RemoveShape`, `ClearShapes`, `SetShapeLook`. State at `g_levellocals.h:1186`,
thunks at `vmthunks.cpp:4329`, upload at `hw_drawinfo.cpp:480`, `ShapesAt()` in
`main.fp`.

A shape is a **flat emissive glyph projected onto whatever surface passes
through it** — disc, ring, square, square outline, cross, hexagon, triangle. It
lies *on* the floor or wall rather than hanging in the air, faded by the
fragment's height offset, and it is added after lighting, so a mark does not dim
in a dark room. Sprites are skipped deliberately: they have no world normal, and
without that exclusion a shape would smear across every monster standing in it.

Three things separate it from a decal:

**It can grow, and it can split.** `SetShapeMotion` gives a shape a growth rate
and a **seam** — the shape opens down its middle, revealing a second colour
masked by the original outline. Both resolve at render rate rather than in 35Hz
steps, for the same reason the disturbances do: a seam crawling apart one tic at
a time is a visible staircase, and it is the part anyone actually looks at.

**One slot can draw a formation.** `SetShapeRepeat` mode 1 puts N copies in a
ring that orbits and spins; mode 2 tiles an infinite drifting grid. The
coordinate is folded before the distance test, so eight hundred copies cost what
one costs — the same trick as the sweep's lattice and the fog's tendrils.

**Cost is bounded by the highest live index, not the live count.** The shader
loops to a high-water mark, so a single permanent shape parked at slot 120 costs
121 iterations per fragment for the rest of the map while 119 slots sit empty.
Keep anything with `life 0` at low indices. Related trap: the allocator recycles
the oldest *expiring* shape, so if every slot holds a permanent there is nothing
to recycle and it returns slot 0 and overwrites it rather than refusing.

---

## 22. Billboard hit tests: two defects found by their first consumer

Both found the night the billboard queries got their first caller ever — a
ZScript VR weapon wheel. Neither could have been noticed before, because
`AimBillboard`, `TouchBillboard` and `SweepBillboard` had shipped with **zero
callers in the tree**. Both are in `src/scripting/vmthunks.cpp`.

The queries and the renderer already share `BillboardBasis` for orientation and
extent — that consolidation is recorded in `BILLBOARDS.md`. These are the two
places where they still disagreed.

### The group transform moved the picture and not the target

`BillboardWorldPos` returned `bb.pos` verbatim. The renderer does not draw it
there: a grouped billboard is scaled **about its group's origin**, position as
well as extent (`hw_drawinfo.cpp`, `lpos = gorigin + (lpos - gorigin) * gscale`).
`BillboardQueryScale` already mirrored the extent half, so the quad was tested
at full-size offsets with scaled extent — displaced from the picture by
`(pos - origin) * (1 - gscale)`.

Consequence: `AnimateBillboardGroup` was unusable for anything clickable. A
panel opening with a grow animation was wrong for the whole animation, which is
exactly when the player is already reaching for it.

`BillboardWorldPos` now takes the level and applies the same transform.
Attached members scale their `attachOffset` instead, matching the renderer's
`lattach` — their `pos` is rewritten unscaled every tic by `p_tick.cpp`, so
scaling that would compound. View-locked members are deliberately excluded: the
renderer scales those in view-local space before resolving them against the
viewpoint, and the existing `drawPos` branch already carries the result.

The function returns by value now rather than a const reference, so the three
call sites hold a value.

### A group collapsed to zero was invisible and still clickable

The renderer drops a group at scale ≤ 0 before submitting it
(`hw_drawinfo.cpp`, `if (gscale <= 0.0) continue`), and `SetBillboardGroupScale(gid, 0)`
is the documented way to hide a panel. The queries had no equivalent, and
`BillboardBasis` floors scale at 0.01 rather than zero — so a hidden panel kept
a tiny hit box sitting at its full-size offset, and could win a hit against the
visible panel in front of it.

`BillboardHittable` now takes the level and rejects `bb.group` whose scale is
≤ 0, which is the same test the renderer uses to skip drawing it.

### Confirmed by the thing that found them

A probe map spawned four panels — fixed, camera-facing, half-scaled-group, and
zero-scaled-group — and printed what each query returned. Before: the
half-scaled panel missed at its drawn centre and hit at its *unscaled* position;
the zero-scaled panel answered rays while drawing nothing. After: both correct,
and the fixed and camera-facing controls unchanged.

A third defect was **predicted and did not reproduce**: an audit argued that
camera-facing billboards are tested against the wrong orientation because the
queries pass their own origin where the renderer passes the eye. The probe's
camera-facing panel passed both aim and touch. Worth knowing before anyone
"fixes" it.

---

## 23. A psprite's scale reaches the model path

`src/r_data/models.cpp`, `RenderHUDModel`.

`psp->scale` was read only by the 2D weapon-sprite path (`hw_weapon.cpp`). A mod
that shrank a psprite saw nothing happen to a weapon drawn as a **model** — and
there was no other way to resize one from script at all, since model scale comes
from `MODELDEF` and the sprite frame.

`RenderHUDModel`'s scaling step now folds it in:

```cpp
float pspScale = 1.0f;
if (!psp->scale.isZero()) pspScale = (float)psp->scale.X;
objectToWorldMatrix.scale(smf->xscale * pspScale, smf->zscale * pspScale,
                          (smf->yscale / fovscale) * pspScale);
```

`scale` defaults to `(0,0)` and the sprite path already treats zero as "unset",
so this is a free channel — every existing model is untouched unless something
opts in, and no content that does not can be affected.

X alone, applied uniformly. A model scaled unevenly on two axes shears rather
than resizes, and "half size" is one number in every caller's head.

Written for a VR weapon menu that shrinks the held weapon while it is open, so
the hand reads as a pointer rather than a gun. Any script wanting a smaller HUD
model gets it.

**Related and worth knowing:** a HUD model is looked up as
`FindModelFrame(psp->Caller, psp->GetSprite(), psp->GetFrame())`. The **Caller**
is half the key, so swapping only a psprite's sprite leaves a model-drawn weapon
showing its own model. Script must repoint `Caller` too.

---

## 24. Script-side VR input suppression

`hw_vrmodes.h/.cpp`, `vk_openxrdevice.cpp`, `g_game.cpp`, `vmthunks.cpp`,
`doombase.zs`.

Snap turn and stick movement are decided deep in the VR input path, long before
any script sees a button. The native wheel already suppresses both while it is
open — `VRWheel_ShouldSuppressStickMove`, `VRWheel_ShouldSuppressHandInput` —
but every one of those lives in C++ with no ZScript reach, and a grep of
`wadsrc/static/zscript/` for `VRWheel` returns nothing.

So a mod with its own in-world selector had no way to say *the stick is mine
right now*. Driving a menu with the thumbstick spun and walked the player while
they were choosing, which is about the most disorienting thing a VR menu can do,
and no amount of script could stop it.

```
Level.SuppressVRInput(bool)
Level.IsVRInputSuppressed()
```

One flag, checked in the same two places the native wheel is checked:

- **Turning** — `vk_openxrdevice.cpp`, the `if (gameplayMode)` block that owns
  both analogue smooth turn and the latched snap. Suppressed takes an early
  branch that also **resets the latches and the analogue rate**, so a stick held
  over during suppression does not fire the moment it lifts.
- **Movement** — `g_game.cpp:1117`, ANDed into the existing
  `VRWheel_ShouldSuppressStickMove()` test rather than replacing it, so the two
  compose and the native wheel is unaffected.

**Not a cvar, deliberately.** This is transient state, not a preference. A value
that survived a crash or got archived would leave someone unable to turn with
nothing to blame — so it is a plain global with no persistence, and the mod that
sets it is expected to clear it on close, on level end and on death.

Head-driven turning is untouched. Leaning is posture, not input, and freezing it
is its own kind of wrong — the same reasoning the stick-move comment already
records at that site.

**Only the Vulkan OpenXR path is gated.** That is what this fork runs. The GL
OpenXR and OpenVR devices have their own copies of the turn block and would each
need the same one-line guard.

---

## 25. The laser as a borrowed cursor

An in-world menu made of billboards needs a pointer, and the fork already draws
a very good one: the VR laser sight, with a beam, a dot, glow, per-hand colours
and a trace behind it. Rather than have every mod build its own, three natives
let a script borrow the real one.

```
Level.ForceVRLaser(bool on, int hand = -1)   // -1 both, 0 main, 1 off
Level.SetVRLaserRange(double range)          // map units; 0 = engine decides
```

### An override, never a cvar write

`vr_laser_sight` and friends are **archived**. A mod that switched them on for
the duration of its menu would be editing the player's saved settings to draw a
line for four seconds, and the VM refuses the write anyway — *"Attempt to change
CVAR outside of menu code"*, correctly. So the state is a separate global the
renderer consults **on top of** the cvars, touching none of them. Drop the
override and the player's own preference is exactly where they left it.

Transient, for the same reason §24 is: no persistence, no archiving. A menu that
died mid-frame must not leave a laser welded on.

### Named to one hand

`hand` is the point. The first version forced both, which put a second beam on
the hand still holding a gun — pointing at nothing, clamped to the menu's
arm's-length range. `VR_IsScriptLaserForcedFor(offhand)` is asked separately for
each hand in `hw_weapon.cpp`, so the unnamed hand keeps whatever the player's own
cvars give it, including nothing.

Naming the hand does one more thing, and it is the reason the off-hand case
worked at all. `drawHand` bails early for a hand holding **no weapon**, or a
**melee** weapon, when `vr_laser_show_melee` is off:

```cpp
AActor* weapon = offhand ? player->OffhandWeapon : player->ReadyWeapon;
if (weapon == nullptr) { if (!vr_laser_show_melee && !forcedHere) return; }
```

Both gates ask *is this hand worth drawing a laser for*, and for a gun that is a
sensible question. For a **cursor** it is the wrong question entirely — an empty
off hand is precisely the hand an off-hand menu is most likely to be worn on, and
without the `forcedHere` exemption that menu has no pointer at all. So a hand
that has been explicitly claimed by script skips both gates.

### Stopping at something the engine cannot see

The laser's trace knows level geometry and actors. A billboard is neither, so a
beam aimed at a panel passes straight through and lands on the wall behind it —
which reads as the laser ignoring the very thing it is selecting.

Teaching the trace about billboards would mean putting a UI concern inside
`P_LineTrace`. Instead the script, which has *just done the hit test anyway*,
publishes the distance and the renderer clamps to it:

```cpp
const double scriptRange = VR_IsScriptLaserForcedFor(offhand) ? VR_GetScriptLaserRange() : 0.0;
if (scriptRange > 0.0) visibleDistance = std::min(visibleDistance, scriptRange);
```

**Shortening only** — `std::min` against the world's own answer, so this can
never be used to put a laser through a wall. Scoped to the forced hand, so an
ordinary laser sight on the other hand is not cut short by a menu it has nothing
to do with. Republished every tic; a stale value cannot outlive its menu.

### Touched

- `hw_vrmodes.h/.cpp` — `VR_SetScriptLaserForced(bool, int)`,
  `VR_IsScriptLaserForced()`, `VR_IsScriptLaserForcedFor(bool)`,
  `VR_SetScriptLaserRange`/`VR_GetScriptLaserRange`
- `hw_weapon.cpp` — the `forcedHere` exemption in `drawHand`, per-hand
  `allowPointer`/`allowBeamToggle`, and the range clamp in
  `GetLaserBeamEndpoints`
- `vmthunks.cpp`, `doombase.zs` — the two natives

Adding a native to `doombase.zs` means **both** targets have to be rebuilt:
`src/zdoom.vcxproj` for the engine and `wadsrc/doomxr_pk3.vcxproj` for the
declarations. Building only the first gives a clean compile and a script error
at load.

---

## 26. Haptics reach ZScript

```
Level.VRHaptic(int hand, double intensity, double durationMs)
```

The fork already had complete per-hand OpenXR haptics: a vibration action bound
for the Touch, Index, Vive and simple interaction profiles, with left and right
output paths, an amplitude/duration pair tracked per hand, and a stop path.
`VKOpenXRDeviceMode::Vibrate(duration, channel, intensity)` drives all of it.
**Script could not reach any of it.** An in-world menu could draw itself and
could be pointed at, and it could not make your hand feel anything.

### The stub worth knowing about

There is an existing engine-wide entry point, `VR_HapticEvent`, called from a
dozen places across the playsim — `p_interaction.cpp` for fire and slime damage,
`a_weapons.cpp` on weapon pickup, `sbar_mugshot.cpp`, the door code. On this
platform its body is **empty**:

```cpp
void VR_HapticEvent(const char* event, int position, int intensity, float angle, float yHeight )
{
}
```

So none of those call sites do anything, and routing a new native through it
would have produced a function that compiled, ran, reported success and buzzed
nothing. `VRMode::Vibrate` is the live path.

### Abstract hand in, physical side out

`Vibrate`'s `channel` is a **physical** side — 0 left, 1 right. Everything
script-facing is addressed as main/off. Getting that backwards is a miserable
bug to chase because it still works, just on the other arm, so the swap is done
once inside the engine rather than in each caller:

```cpp
const bool rightHanded = vr_control_scheme < 10;
const int channel = rightHanded ? (hand == VR_MAINHAND ? 1 : 0) : hand;
```

Same swap the native wheel does in `hw_vrwheel.cpp`, now with one owner.

Intensity is clamped to 0..1 and duration to 500 ms. A script asking for a
two-second pulse at full strength is a script with a bug, and the controller has
no way to refuse it. `vr_enable_haptics` is still checked inside `Vibrate`, so a
player who has turned haptics off cannot be overridden by a mod.

### Touched

- `hw_vrmodes.h/.cpp` — `VR_ScriptHaptic(int hand, double intensity, double durationMs)`
- `vmthunks.cpp`, `doombase.zs` — the native

Both build targets, as in §25.

---

## Building

`auto-setup-windows-vr.cmd` locates Visual Studio's bundled CMake via
`vswhere` — CMake is generally not on PATH. Build output lands in
`build-dxr/RelWithDebInfo/`.

---

## 27. Billboard roll

```
Level.RollBillboard(int id, double roll)
```

Yaw and tilt **aim** a billboard; neither can turn its face. A card that tumbles
as it arrives, a dial, a readout that rotates to stay level — all of them wanted
an angle that did not exist.

### It goes in the shared basis, not the vertex builder

This is the whole point of the change, and the reason it is three lines rather
than one. `BillboardBasis` in `g_levellocals.h` is the single function that
solves orientation for the renderer **and** for `AimBillboard`, `TouchBillboard`
and `SweepBillboard`. Rolling in the vertex builder alone would have drawn a
rotated quad that was still *hit-tested unrotated* — silently un-clickable at
its visible corners, exactly the class of bug §22 records for the group
transform.

```cpp
if (bb.roll != 0.0)
{
    const double rollRad = bb.roll * DEG2RAD;
    const double cr = cos(rollRad), sr = sin(rollRad);
    const DVector3 r0 = right, u0 = up;
    right = r0 * cr + u0 * sr;
    up    = u0 * cr - r0 * sr;
}
```

A rotation about the normal leaves the normal alone, so it is untouched.

**Independent of facing.** A `BBF_CAMERA` billboard still rolls: the camera
solve decides where the face *points*, and roll decides which way is up on it.

### Its own setter

Not a fourth argument to `OrientBillboard`. That call is made every tic by
everything that orients anything, and widening it would mean editing every
existing call site to pass a value nearly all of them do not care about. Roll
also changes on a completely different schedule — a card tumbles once on arrival
and then holds at zero forever — so paying for it in the per-tic call is
backwards.

Serialised alongside yaw and tilt. A reloaded panel that lost its roll would come
back upright *and*, because roll is in the shared basis, hittable somewhere other
than where the save left it.

---

## 28. A panel as a field

```
BB_SDFPANEL = 11
```

`BB_PANEL` samples a small rounded-rect texture. That is cheap, correct, and has
two limits that only appear once a panel is doing real work: it blurs when
stretched large — which a card held close in VR always is — and **it cannot
glow.**

That second one is structural rather than an oversight. `SetBillboardGlow` places
its halo by reading the distance field *outside* the shape, and a sampled texture
has nothing out there to read. It is why the glow packing is gated on
`payload >= BB_TEXT`, and why a label could carry a halo while the plate directly
behind it could not.

This is the same rectangle solved per pixel. Crisp at any size, haloed by the
same four lines every other field payload uses, and the border is a second
distance test rather than a second quad — no extra draw, nothing to keep in step
with the plate's size, no z-fighting.

**A second payload, deliberately, not a change to `BB_PANEL`.** Sampling one
small texture is cheaper than solving two distance fields, the two look
different, and a caller should get to pick. A ring of forty background plates
nobody looks closely at should stay sampled.

### The shape numbers ride in `uAddColor`'s alpha

Corner radius in the high nibble, border width in the low one, each 0–15 across
the half-extent. Sixteen steps is coarse and it is enough: these are a corner and
a hairline, not a measurement.

Alpha because it is the only channel left — rgb already carry halo reach, halo
strength and the void flag — and because it is genuinely free; nothing downstream
reads `uAddColor.a`. **`uSpecularMaterial` was the obvious alternative and is not
usable:** it is filled from the *texture's* glossiness and specular level, and
only on the GL backend.

Numbered 11 so it lands inside the `payload >= BB_TEXT` gate and inherits the
halo packing without a special case.

`BBFL_VOID` turns it into a hole — dark interior, only the rim lit — reading
exactly as it does on `BB_SEAM`, so a caller who knows one knows the other.

### Touched

- `g_levellocals.h` — the payload, and `roll` on `FBillboard`
- `textures.h`, `hw_shaderpatcher.cpp` — `SHADER_SDFPanel`, kept in step with
  `defaultshaders[]`, which is indexed by the enum with nothing checking they
  agree
- `shaders/glsl/func_sdfpanel.fp`
- `hw_sprites.cpp` — the draw case and the alpha packing
- `p_saveg.cpp`, `doombase.zs`

---

## 29. Everything else this fork adds

Found by auditing the source against this document rather than the other way
round. Each of these was shipping and working with no entry here — which is the
finding as much as any individual item, since a change nobody wrote down is a
change the next person removes.

### The VR laser sight itself

§25 documents a script *borrowing* the laser. It never documents the laser.

`hw_weapon.cpp`, `DrawLaserSightWorld` / `GetLaserBeamEndpoints`, ~50
`vr_laser_*` cvars, `CCMD toggle_laser_sight` (`g_game.cpp:1491`).

Sixteen segments rather than eight, because eight is an octagon at the distance
a VR player holds a gun from their face. A bright **core** inside a soft
**halo**, tapered on the halo only — the core is what you aim with. Emissive
with no dynamic light at all: the scene target is half-float and the bloom
threshold is 1.0, so pushing the core past white lets the existing bloom pass
pick it up on its own. Core only; an overbright halo hazes the view instead of
looking hot.

A **target lock** built from information the trace was already discarding: the
trace runs with `MF_SHOOTABLE` and `FTraceResults` carries the actor. On
something alive the dot tightens, brightens and breathes while its glow swells
*outward* — opposite directions, which is what makes it read as pressure rather
than as a size change.

Colour resolves in four tiers, highest first: the weapon's own
`Weapon.LaserBeamColor`, the per-slot colour, then a mode ladder of one colour /
per hand / all four. `LaserBeamColor` is an **int with a -1 sentinel rather than
a Color**, because `Color` has no spare value — the property parser routes
strings through `V_GetColor`, which fills RGB and leaves alpha 0, so `PalEntry 0`
means both "unset" and "black".

### Hitscan tracers and ricochet

`hw_weapon.cpp:1179`, `Weapon.HitscanTracerOffset`, nine `vr_hitscan_*` cvars.
Visible tracer rounds for hitscan weapons with a per-weapon muzzle offset and a
ricochet chance.

### A psprite state-change re-entrancy guard

`p_pspr.cpp`, `FPSpriteDepthGuard`, depth cap 64.

`DPSprite::SetState` had only `statelooplimit` — a *local* counter that catches
an iterative state chain but is **reset by any state whose action calls
`P_SetPsprite`**. So a recursive chain ran until the stack died, with no message.
The trigger was this fork's own two-handing suppression returning early from
`A_WeaponReady`/`FireWeapon` before the ready flags were set; it reproduced every
time on VanillaVRPlus's rifle. Now it stops at 64 and prints the state's name.

### Natives with no API listing anywhere

- `Actor.CountStateLabels(cls)` / `Actor.GetStateLabelAt(cls, i)`
  (`p_actionfunctions.cpp:5656`) — walk a class's whole state-label table. The
  model-remap walker needs this to register frame binds without hardcoding label
  names.
- `Actor.GetSpriteTextureID(rotation)` — the current sprite frame as a
  `TextureID`, so a caller can ask `TexMan.GetSize()` for the **artwork** size.
  Sizing off `Height` measures the collision cylinder, which is wrong by a lot on
  tall monsters.
- `SetFogSurface`, `SetFogFollow`, `SetFogGradient`, `SetTornadoLook`,
  `SetSweepFillAir`, `SetSweepCount`, `SetSweepBandAt`, `SetSweepBandDraw` — all
  described in prose somewhere, none listed as callable API.
- `SetGlowTexture`, `SetGlowFlow`, `SetGlowCells`, `SetGlowReact`
  (`vmthunks.cpp:3996`) — §20 explains all five terms at length and never names
  the four functions that drive them.

### Console commands and budget cvars

`bb_spawn`, `bb_text`, `bb_clear` (`p_tick.cpp:130`) for poking billboards
without a mod. `rs_bb_cullradius`, `rs_bb_maxpanels`, `bb_scale`, `bb_tiltbias`,
`bb_flipu`, `bb_sdffont`.

### VR mount systems

Sixteen `vr_automap_*` cvars — a complete parallel mount for the automap
mirroring the HUD's (distance, scale, pitch/roll/yaw, offsets, border, stereo,
fixed-pitch, and a "use hud" mode). Plus `vr_overlayscreen*` and
`CCMD toggleportablehud` (`hw_vrmodes.cpp:922`), `CCMD togglecheatmenu`.

Largely inherited from DoomXR rather than authored here, but a real delta from
stock and worth knowing exists before someone rebuilds it.

### Bloom, beyond §5

§5 names threshold, knee, anamorphic, tint and chromatic. Also live:
`gl_bloom_amount`, `gl_bloom_kernel_size`, `gl_bloom_anamorphic_ratio`, and the
tint is **three cvars** (`gl_bloom_tint_r/g/b`), not one.

### Known-dead things, recorded so they are not mistaken for load-bearing

- `uFogSlabExtra.x` is written every frame with the wake strength and **read by
  nothing** — the wake gate tests `uFogSlabColor.w`. §11's rationale for the slot
  describes an intention the code never followed through on.
- `uSweepPad1` is reserved for sky scaling in the per-fragment darkness path,
  which is not implemented (§9 records the gap).

---

## 30. Field reflection — reading another mod's data without linking to it

```
Level.HasField(Object o, string field) -> bool
Level.GetFieldInt(Object o, string field, out int value) -> bool
Level.GetFieldBool(Object o, string field, out int value) -> bool
Level.GetFieldFloat(Object o, string field, out double value) -> bool
Level.GetFieldString(Object o, string field, out string value) -> bool
Level.GetFieldName(Object o, string field, out name value) -> bool
Level.GetFieldObject(Object o, string field, out Object value) -> bool
Level.FieldCount(Object o) -> int
Level.FieldAt(Object o, int index, out string fieldName, out string fieldType) -> bool
```

A typed reference needs its class at **compile** time, so a mod that wants to
*describe* another mod's weapon normally has to hard-depend on it. That is the
wrong shape for an informational consumer: a weapon-select panel wants tier,
rarity and affixes off DoomRL Arsenal, LegenDoom, Doomablo and mods not written
yet, none of which will ever publish an interface for it. `Service`
(`service.zs`) already solves the case where the other mod cooperates; nothing
already released will.

The VM already knows all of this. `PClass::Fields` lists every field of every
class in every loaded mod, and each `PField` carries its `Offset`, `Type` and
`Flags` (`src/common/objects/dobjtype.h:83`, `src/common/scripting/core/
symbols.h:78-94`). None of it reached script. This is a door onto data the VM
already maintains, not a new mechanism.

### Enumeration is the half that matters

The typed getters let a caller ask a question it already knew to ask.
`FieldCount`/`FieldAt` let it **discover** what there is to ask — the
difference between supporting a fixed list of mods and degrading usefully on
all of them, including ones released after this fork stops being maintained.
`FieldAt` reports each field's type as a plain string (`"int"`, `"bool"`,
`"double"`, `"string"`, `"name"`, `"object"`, `"other"`) so a caller can pick
the matching getter without the fork exporting the type system.

Walks the class hierarchy base-first (`WR_CollectFields`, recursive over
`PClass::ParentClass`) rather than reading `Fields` directly. `Fields` holds
only what a class *declares* — a weapon subclass that adds nothing of its own
reports zero fields there, and everything it inherited from `Weapon` and
`Actor` would be invisible, which is the entire useful content. Filtered
during collection so `FieldCount` and `FieldAt` always agree, with no holes
in `0..count-1` for a caller's loop to trip over.

### Read-only, permanently

There is deliberately no `SetField`. Writing into another mod's private state
puts the corruption and the eventual crash in two different mods with nothing
tying them together. Reading cannot corrupt anything; keep the asymmetry.

### What a caller may see

Refused: `VARF_Private` (the declaring mod said no), `VARF_Meta` and
`VARF_Static` (class data, not instance data — reading either at an instance
offset is meaningless). Allowed: `VARF_ReadOnly`, which means "script may not
*write* this," and every native here only reads.

Resolution goes through `FindSymbol(name, searchparents: true)`, so a
subclass reports fields it inherits rather than only ones it redeclares.

### Every getter is type-checked, never reinterpreted

`Offset` is a raw byte offset into the object. Reading an `int32` field
through a `double*` is not a wrong answer, it is garbage or a crash. Each
getter compares the field's `PType*` against the expected singleton
(`TypeSInt32`, `TypeFloat64`, `TypeString`, `TypeBool`, …) and returns `false`
on any mismatch. `GetFieldFloat` **widens** — it also serves `float32` and
integer fields, since every one of those survives the conversion to `double`
— but nothing narrows anywhere: a `double` read as an `int` would silently
discard, and a stat sheet quietly showing `3` for `3.7` is worse than showing
nothing.

Every getter returns `false` and leaves its out parameter untouched when it
cannot answer. `false` means "could not answer," never "the answer is zero" —
a caller has to be able to tell an absent value from one that is genuinely 0,
because a stat sheet renders those two differently.

### Bools have two storage shapes, and missing the second is the trap

`GetFieldBool` is its own getter rather than folded into `GetFieldInt`,
because `TypeBool` derives from `PInt` but is a distinct singleton, and
reaching for `GetFieldInt` on a flag has usually meant misunderstanding what
it is.

A standalone `bool` field is a whole byte (`PBool::GetValueInt` is
`*(bool *)addr`, `types.cpp:806`). A `flagdef` — every `+WEAPON.OFFHANDWEAPON`,
every actor flag — is a **bit** packed into a shared byte, read by codegen
against `1 << BitValue` (`OP_LBIT`, `codegen.cpp:7389`). `PField::BitValue` is
`-1` for the first shape and the bit index for the second. `GetFieldBool`
branches on it, so every engine flag is readable by name through the same
call that reads a plain bool field.

### Four things only running it found

Written up because none of them were visible in the diff, and each produced a
different failure shape:

- `types.h` was not included in `vmthunks.cpp` — `PType` was an incomplete
  type. 22 compile errors.
- `PARAM_PROLOGUE` where `PARAM_SELF_STRUCT_PROLOGUE` was needed. These are
  declared as methods on `LevelLocals`, not free statics, so the VM passes
  `self` as parameter zero; a bare prologue does not consume it, and every
  argument after shifts by one. Compiled clean, then read the target object
  as the level itself on every call.
- `PClass::Fields` holding only declared fields, above. `FieldCount` returned
  `0` on a stock `Pistol` until enumeration walked the hierarchy.
- Bools unreadable at all, until `GetFieldBool` existed — and the curse flags
  this was built to read (`LockedDamage`, `LockedCritChance`, …) are bools.

### Verified in-engine against a real mod (RS_Main)

Read a live `VR_Revolver`'s rolled `Tier`, `DamagePerShot`, `Capacity`,
`Accuracy`, `Velocity`, `CritChance`, `CritMult` and `ReloadSpeed`; read its
five `Locked*` curse flags and caught one genuinely set
(`LockedCritChance = true`) on a weapon rolled during the test; enumerated
712 fields on it with names and types. Nothing in the test names an `RS_Main`
type — every read is a runtime string.

### Touched

- `src/scripting/vmthunks.cpp` — `WR_ResolveField`, `WR_CollectFields`, and
  the nine `DEFINE_ACTION_FUNCTION` thunks
- `wadsrc/static/zscript/doombase.zs` — the native declarations on
  `LevelLocals`

Both build targets, as §25/§26 note: the executable *and* the pk3 target
carrying the ZScript declarations. Building only the first gives a clean
compile and a script error at load.

### What it does not solve

A value never stored cannot be read, because there is nothing to read:

```zscript
A_FireBullets(0, 0, 1, 25);     // 25 is an operand in compiled bytecode
```

There is no field holding `25`. `FState` does carry `ActionFunc`
(`src/gamedata/info.h:104`), but it is deliberately absent from the fields
exported to script (`p_states.cpp:1136-1150`), and even exported it would be
a `VMFunction*` — the call's arguments are pushed on the VM stack at
execution time and are not retained as data anywhere.

Matters less than it sounds for the case this was built for. Any mod that
*rolls* stats per instance — every loot mod, which is the whole category —
has to store them in fields to vary them at all. Weapons with hardcoded
damage are the ones whose damage never varies, and those are readable once
from `Default`, or measurable by observation.

---

## 31. `MainHandRoll` — the main hand's real wrist roll

```zscript
native readonly double AActor.MainHandRoll;   // degrees
```

The main hand's true roll, for anything drawn *on* the held weapon.

`AttackRoll` cannot carry it. The VR backends compute the real value from
`weaponangles[ROLL]` and assign it, and then `UpdateCanonicalMainHandPose`
overwrites it with zero on the very next tic
(`src/playsim/p_user.cpp:163`), before `WorldTick` runs
(`src/p_tick.cpp:403-408`). Script therefore never saw anything but 0.

That zero is correct and must stay. `AttackPitch` and `AttackAngle` survive
because the usercmd has a `weaponpitch` and a `weaponyaw` to rebuild them
from; there is no `weaponroll` (`src/d_protocol.h:64-75`), so a peer has no
way to arrive at the same number. Zeroing is what keeps it deterministic.

So this is a second field rather than a fix to the first, written by the VR
backends beside the `AttackRoll` assignment they already made:

| | |
| --- | --- |
| `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp` | shared VR path |
| `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp` | OpenXR / Vulkan |
| `src/gl/stereo3d/gl_openxrdevice.cpp` | OpenXR / GL |
| `src/rendering/gl/stereo3d/gl_openvr.cpp` | OpenVR |

Renderer-owned like the `Hmd*` block: never touched by the playsim, never
serialised, no usercmd involvement.

**Which to read.** `MainHandRoll` for presentation — a readout welded to the
gun, anything that should agree with what the player can see in their hand.
`AttackRoll` for anything that must agree across a network. `OffhandRoll`
needs no counterpart; nothing zeroes it outside `if (multiplayer)`
(`p_user.cpp:168-174`).

**Why it matters.** Without it the held model rolls with the wrist while
script is told the wrist is level, so anything script positions on the gun
stays upright as the gun turns over. That reads as a bug in the mod, and the
mod cannot fix it.

Both build targets, as §25/§26 note.

---

## 32. Standing shapes — the same glyphs, freestanding in open air

§21b describes a shape as *painted onto whatever surface passes through it*,
and main.fp's own comment was blunt about it: "Flat decals, not solids... makes
it lie ON the floor rather than hang in the air." `orient` (0 floor, 1 wall,
2 any) only filtered which already-rendered surface a shape was allowed to
paint, by its normal. It never lifted one into space.

**`orient == 3` is a shape that stands.** Anchor point plus a full
yaw/pitch/roll orientation, intersected against the view ray — not projected
onto anything. Everything below the coordinate solve is the identical
`sdBox`/`opOutline`/seam-split colour code §21b already documents, fed a
different `uv`.

**It cost no new capacity, and that was the point.** `orient` had four bits in
the existing `mShapeB.x` packing (`kind | orient << 4`) and was using three
values out of sixteen. So standing shapes reuse the same 128 slots, the same
copy loop, the same `AddShape` signature — no new `MAX_*`, no new uniform
array, and none of the four-file sync a new array demands. `ShapesAt()` skips
`orient == 3` and a new `StandingShapesAt()` claims them; neither sees the
other's rows.

**Depth is the beam trick, not a new one.** `BeamAirGlow` already resolves a
world position from the depth buffer and refuses to draw past it. A standing
shape solves where the eye→fragment ray crosses its plane and applies the same
comparison: `t <= 0.0 || t >= fragDist` and it is behind real geometry.

### The traps, all four of them real

**`angle` means two different things now.** For a decal it rotates the pattern
*in* its plane (`opRotate(uv, uShapeB[i].y)`). For a standing shape it orients
the plane *itself* around world-up. Same field, different reading, gated on
`orient` — deliberately, rather than spending a new parameter on it.

**Facing straight up or down collapses the basis.** `cross(worldUp, facing)`
degenerates to zero when the two are parallel, which a pitch of ±90° reaches
exactly. Guarded with a fallback to world +X in *both* places that build the
basis — `StandingShapesAt()` in main.fp and the parent-composition block in
`hw_drawinfo.cpp`. Two implementations of one rule is a thing to keep in step.

**Rates resolve natively, like `grow` and `seamRate` already did.**
`SetShapeOrient` takes yaw/pitch/roll rates, resolved once per frame as
`base + rate * age`. A caller that wants a panel spinning sets a rate once;
nothing polls it, and it does not step in 35Hz staircases.

**Linking is a single forward pass with a stated contract.** `LinkShape` gives
a shape a parent whose resolved transform it composes onto. The resolve loop
walks slots in order and a child reads its parent's *already-resolved* world
transform in that same pass — which works only because **a parent's index must
be lower than its child's**. There is no cycle check and no topological sort;
an out-of-order parent is ignored and the shape resolves as unparented, which
is at least a defined answer. Orientation composes by Euler addition: exact for
a pure-yaw chain, an approximation once pitch and roll combine at one joint.
Good enough to build a box out of panels, not a substitute for quaternions.

`mShapeE` carries the resolved pitch/roll and was appended at the **true tail**
of `HWViewpointUniforms`, past the existing explicit padding — never mid-struct,
per the invariant at the top of CHANGES.md: a uniform block is matched by
offset, not by name.

**And it failed on Vulkan first, exactly as invariant 2 warns.** `uShapeE` was
declared correctly in both `gl_shader.cpp` and `vk_shader.cpp`, and still threw
"undeclared identifier" on Vulkan only. Vulkan instances `ViewpointData` as
`viewpoints[2]` for stereo, so every member needs a
`#define uShapeE viewpoints[HW_VIEWPOINT_INDEX].uShapeE` alias next to its
declaration. Miss the alias and GL compiles, Vulkan does not, and nothing about
the error names the real cause. **Adding a viewpoint member is four edits, not
three.**

## 33. Fog disturbance capacity — 8 to 32, and why not 128

`MAX_BEAMS` and `MAX_SHAPES` are both 128. `MAX_FOG_DISTURB` went to **32**,
and the asymmetry is deliberate rather than an oversight.

A shape's cost per fragment is a squared-distance reject that usually misses;
a beam's is a segment-distance test. Both are cheap and both are skipped early
for most pixels. A disturbance is read by *two* loops in main.fp — the glow
feed and the fog density calculation — and those run across **every fragment
inside the fog volume**, which on a screen-filling bank of mist is most of the
screen. The per-slot multiplier is simply larger here than anywhere else the
fork raised a cap.

32 covers a busy firefight's worth of simultaneous gunfire, deaths and
explosions without making the fog pass four times heavier for slots that are
empty most of the time.

Four sync points, the same four any uniform array resize needs:
`g_levellocals.h`, `hw_viewpointuniforms.h`, both shader-side struct mirrors
(`gl_shader.cpp` and `vk_shader.cpp`), plus — easy to miss — the two
**hardcoded loop bounds** in `main.fp`, which are literals rather than a
constant and will silently keep reading only the first 8 if left behind.

### Regression fixed the same day: the shader walked every slot regardless

Raising the cap to 32 did not add an early-out. Both disturbance loops in
`main.fp` — the glow feed and the fog density calculation — walked all 32
slots on every fragment inside the fog volume and `continue`d past the empty
ones, which still costs the iteration and the uniform read. Shapes and beams
already break on their live count; fog didn't, so idle fog went from paying
for 8 wasted reads per fragment to paying for 32, on the loop that runs across
every fragment the mist touches.

Fixed by reading a live count (`uFogBow.w`, already uploaded, already used as
a boolean) and breaking at it, the same way the shape and beam loops do.

That forced a meaning change on the C++ side. `liveDisturb` was a running
total, but disturbance slots are recycled out of order — `FogDisturb()` takes
the first free slot or the oldest — so a live set can be sparse. With slots 0
and 5 live, a *count* of 2 would make the shader break before it ever reached
slot 5 and silently stop drawing it. `liveDisturb` is a **high-water mark**
now (`i + 1`), the same shape the shape loop's own bound already used.

---

## 34. Mod-owned placement — per-weapon offset sliders without an engine change

`src/common/models/model.h:78`, `src/r_data/models.cpp:365` (`RenderHUDModel`), MODELDEF
keyword `placementcvars` (`models.cpp:1238`).

The engine already had one way to nudge a HUD model live: `MDL_USEHANDOFFSETS`
pulls `vr_hand_ofs_x/y/z` and `vr_hand_yaw/pitch/roll` (and the `vr_offhand_*`
pair for the other hand) into the same translate/rotate calls that apply
`MODELDEF`'s `offset`/`angleoffset`. That works because those six CVARs are
declared by the *engine*, so every model that opts in shares the same slider.
Fine for "move the whole VR grip a bit," useless for a mod that wants one
specific weapon's model sitting wrong relative to its own sprite frame and
needs a knob to fix just that one — a hand-scanner prop riding two centimetres
low, a rifle a mod author wants tilted five degrees for its idle pose. Six new
engine CVARs and a menu entry per misbehaving weapon is not a fix, it is a
standing liability: it means every mod that ships a slightly-off model needs
an engine PR, and the engine's own option tree grows a slider for content the
engine has never seen.

`placementcvars <prefix>` in `MODELDEF` sidesteps that by naming CVARs instead
of holding values. `FSpriteModelFrame::placementCVars` (`model.h:86`) is just
an `FName` — a prefix, not a set of floats — and `RenderHUDModel` resolves it
to real numbers on every frame it draws that model:

```cpp
static const char *sufOfs[3] = { "_ofs_x", "_ofs_y", "_ofs_z" };
static const char *sufRot[3] = { "_yaw", "_pitch", "_roll" };
for (int i = 0; i < 3; ++i)
{
    nm.Format("%s%s", smf->placementCVars.GetChars(), sufOfs[i]);
    if (FBaseCVar *cv = FindCVar(nm.GetChars(), nullptr))
        placeOfs[i] = (float)cv->GetGenericRep(CVAR_Float).Float;
    nm.Format("%s%s", smf->placementCVars.GetChars(), sufRot[i]);
    if (FBaseCVar *cv = FindCVar(nm.GetChars(), nullptr))
        placeRot[i] = (float)cv->GetGenericRep(CVAR_Float).Float;
}
```

Six `FindCVar` calls by string, keyed off `<prefix>_ofs_x`, `_ofs_y`, `_ofs_z`,
`_yaw`, `_pitch`, `_roll`. Nothing here reaches into engine state — the CVARs
being looked up are declared wherever the mod that wrote the `MODELDEF` entry
put its own `CVARINFO`, with its own `MENUDEF` page to expose them as sliders.
The engine supplies the plumbing (a name, a lookup, a place to add the result)
and the mod supplies everything else: the six CVAR declarations, their
defaults, and the UI a player actually touches. Adding a tunable weapon is
therefore a content-only change — a `MODELDEF` line plus a `CVARINFO`/`MENUDEF`
pair — and never touches engine source or the engine's own options menu.

**Looked up live, not cached.** The comment at `models.cpp:367` gives the
reason directly: `MODELDEF` is parsed well before a mod's `CVARINFO` is
guaranteed to have loaded, so resolving the `FBaseCVar*` once at parse time and
caching it would frequently cache a null — permanently, since nothing revisits
that cache later. Re-resolving by name every draw call costs six hash lookups
per drawn model per frame, which the comment dismisses outright ("not worth
optimising away") because the number of on-screen HUD models with this flag
set is always small.

**Composes with the engine's own hand offsets, not instead of them.** The
resolved `placeOfs`/`placeRot` are summed into the *same* translate and rotate
calls as `MDL_USEHANDOFFSETS`'s `handOfs*`/`hand*` values and the raw
`MODELDEF` `offset`/`angleoffset`/`pitchoffset`/`rolloffset` fields
(`models.cpp:399-401`, `439-441`), not applied as extra transforms afterward.
That matters for rotation specifically: rotations don't commute, so a
mod-declared yaw applied as a fourth `rotate()` call after the engine's pitch
and roll would spin around an already-tilted axis and produce a different pose
than the same number folded into the sum before rotating. Summing first is
what makes a value that started life as a `MODELDEF` constant, a value from
the engine's hand-offset slider, and a value from a mod's own CVAR all
genuinely interchangeable — any of the three can move to either of the other
two with nothing visibly moving on screen.

**A missing or half-declared CVAR degrades silently, by design.** The MODELDEF
parser comment says it plainly: "a missing CVAR reads as zero, so a
half-finished set degrades to the MODELDEF values instead of failing"
(`models.cpp:1240-1242`). `FindCVar` returning null just leaves that slot at
its `0.0f` initializer — there's no error, no console warning, no failed load.
A mod that declares `placementcvars myweapon` but only adds `myweapon_ofs_x` to
its `CVARINFO` gets exactly one working axis and five that behave as if the
keyword were never used, rather than a startup failure over five unrelated
axes it hasn't gotten around to wiring up yet.

**Gotcha:** the prefix is resolved by string concatenation with a fixed
suffix list, so `placementcvars myweapon` requires the CVARs to be named
*exactly* `myweapon_ofs_x`, `myweapon_ofs_y`, `myweapon_ofs_z`, `myweapon_yaw`,
`myweapon_pitch`, `myweapon_roll`, `myweapon_scale` — no partial sets with
different suffixes, no case variation beyond what `FindCVar`'s own lookup
tolerates, and any typo in either the `MODELDEF` prefix or the `CVARINFO` name
fails the same silent way a genuinely absent CVAR does: that axis just sits at
its default forever, with nothing in the log to say why.

### A seventh axis: `_scale`

`placementcvars` originally resolved the six offset/rotation suffixes above.
A `<prefix>_scale` lookup was added alongside them, read the same live,
no-cache way and for the same reason (`MODELDEF` parses before a mod's
`CVARINFO` is guaranteed loaded, so caching the `FBaseCVar*` would often cache
a null). It multiplies onto `vr_weaponScale` rather than replacing it, so a
per-weapon size fix composes with the existing global slider instead of
fighting it: `objectToWorldMatrix.scale(vr_weaponScale * placeScale, ...)`.

**Absent or non-positive reads as 1, not 0.** A missing or zeroed slider must
leave the model's size alone, not collapse it to a point — the same "default
that isn't the falsy value" trap the rest of this file's CVAR-reading code
already avoids elsewhere.

---

## 35. Skin alpha that is not opacity

`src/rendering/hwrenderer/scene/hw_weapon.cpp`, `DrawPSprite`; `src/common/models/model.h`;
`src/r_data/models.cpp`, `ParseModelDefLump`; `src/r_data/models.h`.

A model-drawn HUD weapon is alpha-tested unconditionally:

```cpp
state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
```

That is correct for a skin whose alpha channel means transparency, and
catastrophic for one where it does not. Ripped or converted weapon models
routinely arrive with a PBR-style texture set, and PBR alpha channels are
conventionally used to carry roughness or a gloss mask, not opacity — a
channel that is mostly dark by design, since most of a gun's surface is not
supposed to be glossy. Alpha-tested against `gl_mask_threshold`, most of the
skin fails the test and is discarded outright. The result is a weapon that is
largely invisible with a few solid patches where the roughness data happened
to read bright, which presents as a broken mesh or a broken export and is
neither — the geometry and the real color data are both fine, only the
alpha-as-opacity assumption is wrong for this particular skin.

The fix is a per-model opt-out, not a global change — most skins DO use alpha
for real transparency (scopes, vents, grates) and alpha-testing them is
correct. A new `MODELDEF` flag marks the exception:

```
Model SomeWeapon
{
    ...
    IgnoreSkinAlpha
}
```

which sets `MDL_IGNORESKINALPHA` (`models.h`, bit `1<<18`, the flag word's
next free bit after `MDL_USEHANDOFFSETS`). `FSpriteModelFrame` exposes it as
`ignoresSkinAlpha()` (`model.h`) — read straight off the parsed `MODELDEF`
flags, with no per-actor override, since the draw path needs the answer
before it has resolved an actor's model data at all, and nothing about
whether a texture's alpha channel means opacity is a per-actor question
anyway. `DrawPSprite` reads it once per draw and drops the threshold to zero
for that model instead of the usual `gl_mask_threshold`:

```cpp
state.AlphaFunc(Alpha_GEqual, huds->mframe->ignoresSkinAlpha() ? 0.f : gl_mask_threshold);
```

Nothing is discarded, and the alpha channel is simply left unread for
transparency purposes — which is exactly correct, since for this model it was
never encoding transparency in the first place.

**The alternative was stripping the alpha channel out of every texture of
every ripped weapon by hand, forever** — a per-asset fix repeated for every
future import, instead of a one-line flag set once per model that needs it.

**Only affects the model draw path.** A weapon still drawn as a flat 2D
sprite is untouched; `gl_mask_threshold` there behaves exactly as before.

---

## 36. Per-psprite model tint

`p_pspr.h:213`, `hw_weapon.cpp:1926`, `player.zs:3127` (`native Color Tint`, `native Color Glow`).

Stock GZDoom's only colour input for a weapon was `playermo->fillcolor`, read in `HUDSprite::GetWeaponRenderStyle` and gated behind `STYLEF_ColorIsFixed` — a render-style flag whose actual job is forcing a flat stencil silhouette (the ice-death palette, `A_SetBlend`'s solid-fill mode, that family). So the only way to put a colour on a weapon was to also throw its texture away, and even that one colour was the *player's*, shared across every psprite the player owned — a mainhand pistol and an offhand shotgun could not be tinted differently, because there was exactly one `fillcolor` and no per-layer channel to read instead. For a mod that wants a rarity-coloured glow on a held weapon, or two hands carrying visibly different elemental infusions, stock had nothing to reach for short of baking separate skins per colour.

Two fields fix that, added directly to `DPSprite` rather than the weapon actor:

```cpp
PalEntry Tint = 0xffffffff;   // multiply, 0xffffffff = untinted
PalEntry Glow = 0;            // additive, 0x00000000 = none
```

`Tint` multiplies the model's own skin texture, so surface detail survives instead of being flattened to a silhouette. `Glow` adds on top, for a rim-light or emissive effect that doesn't depend on the skin's own brightness. Both live on the psprite, not the actor, specifically so mainhand and offhand tint independently — `PSP_WEAPON` and `PSP_OFFHANDWEAPON` are separate `DPSprite` instances, and `AActor::modelData` (where a per-actor colour would have had to live instead) is shared by the one actor holding both.

**In-class initialisers, not constructor-body ones, and that is load-bearing.** `DPSprite` has a second, private, argument-less constructor (`DPSprite() {}`, further down in `p_pspr.h`) used only by savegame deserialisation, and it runs none of the public constructor's body. `p_pspr.cpp`'s public constructor *also* sets `Tint = 0xffffffff; Glow = 0;` explicitly at line 215-216, which is redundant there but is the detail that would have mattered if the field defaults had been left to the constructor body alone: the private deserialisation constructor would skip it, and `PSerializer::Serialize` (`p_pspr.cpp:1475-1476`, `arc("tint", Tint)` / `arc("glow", Glow)`) leaves a field alone when the save file being read predates it. Loading a pre-2026-08-08 save would then resume every psprite with whatever bytes happened to be sitting in that memory — and garbage near zero multiplies the weapon's texture by black, i.e. every old save's weapons would render as flat silhouettes for no reason a player could explain. The in-class default is what makes an old save resume at the correct identity value instead.

**Where it's read.** `HUDSprite::GetWeaponRenderStyle` (`hw_weapon.cpp:1862`) computes `ThingColor` exactly the way stock did — `playermo->fillcolor` if `STYLEF_ColorIsFixed` is set, otherwise white — and then, new at line 1938, unconditionally folds the psprite's own tint in on top:

```cpp
ThingColor = ThingColor.Modulate(psp->Tint);
ThingColor.a = 255;
```

This runs regardless of `STYLEF_ColorIsFixed`, which is the actual fix: `Tint` no longer needs the stencil flag to have an effect, so a normally-textured weapon can be coloured without losing its texture. It also means the two inputs *compose* rather than one replacing the other — an actor genuinely rendering under `STYLEF_ColorIsFixed` (frozen, for instance) will show its `fillcolor` multiplied by the psprite's `Tint`, not one or the other. `ThingColor.a` is forced back to 255 immediately after the modulate, so a script that puts anything in `Tint`'s alpha channel has no effect — opacity is controlled elsewhere (`psp.alpha`, `A_SetTranslucent`), and `Tint` is colour-only by design.

`Glow` is simpler — `AddColor = psp->Glow;` at line 1943, unconditional, no gating at all. It is then combined, not overwritten, with the sector's own ambient additive term in `HWDrawInfo::DrawPSprite` (`hw_weapon.cpp:163-175`): the sector's `AdditiveColors[sector_t::sprites]` and the psprite's `Glow` are summed per-channel and clamped to 255, so a weapon's own glow rides on top of whatever ambient sector glow (see §2, "Two colours per glow") is already present rather than fighting it.

One more branch worth knowing: `bright = isBright(psp)` (`hw_weapon.cpp:1543`, true when the psprite's current state is fullbright and its sprite frame isn't flagged to disable that). When bright, `ObjectColor` is `ThingColor` as-is; when not, it's further modulated by `viewsector->SpecialColors[sector_t::sprites]` — the sector's own sprite tint. So `Tint` always applies, but whether the *sector's* colour also applies depends on the weapon's own fullbright state, exactly mirroring how stock sprite colouring already worked; this fork didn't change that half, only added `Tint` into the chain ahead of it.

**How it reaches the 3D model and not just the flat sprite.** `GetWeaponRenderStyle` only computes `ObjectColor`/`AddColor` on the `HUDSprite`; the values still have to reach the actual draw call. `HWDrawInfo::DrawPSprite` calls `state.SetObjectColor(huds->ObjectColor)` and `state.SetAddColor(add)` (the sector-combined value above) *before* branching on whether this psprite draws as a 2D sprite or, when `huds->mframe` is set, as a 3D model via `RenderHUDModel` (`hw_weapon.cpp:200`). Neither `hw_models.cpp` nor `modelrenderer.h` touch object or add colour anywhere in the model path — they were written assuming a caller had already set the draw state's colour, which was true for monsters and decorations but had never been wired up for the HUD weapon model specifically. Setting both before the `if (huds->mframe)` branch, rather than inside either arm of it, is what makes a tinted weapon look the same whether MODELDEF gives it a model or not, and is the entire reason this feature needed to touch `hw_weapon.cpp` at all rather than stopping at `p_pspr.h`.

**Gotcha for a future reader:** this is invisible by default, deliberately — `0xffffffff` and `0` are both true identities, so no existing mod, no existing save, and no weapon that never touches `Tint`/`Glow` changes appearance at all. If a weapon looks tinted and nothing in that weapon's own script sets `Tint`, check whether its actor also carries `STYLEF_ColorIsFixed` (ice death, `A_SetBlend` solid fill, a status-effect palette swap) — the multiply in `GetWeaponRenderStyle` stacks with `fillcolor` rather than being overridden by it, so a frozen player wielding a weapon with a custom `Tint` set will show both colours combined, which reads as a bug the first time someone hits it but is exactly what line 1938 does on purpose.

---

## 37. Script-suppressed psprite layers: hiding the weapon without hiding what it does

A VR hand-swap wants to show a rigged fist model in place of a mod's stock fist weapon, while leaving that weapon completely operational — same states, same damage, same ammo and slot, same `A_Punch` firing on the same tic. The obvious way to do that from ZScript is to zero the layer's alpha or flip its render style, and both of those turn out to be dead ends for reasons the fork had already run into twice before this feature: `DPSprite::GetRenderStyle` discards `psp->alpha` unless the layer explicitly carries `PSPF_ALPHA` or `PSPF_FORCEALPHA` (most weapons set neither), and `RenderStyle` itself is one of the few `DPSprite` fields the fork deliberately blocks from script — `player.zs:3106` still carries `//native readonly int RenderStyle; had to be blocked because the internal representation was not ok`. Even granting both of those, the actual draw/skip decision for a psprite layer is a bare `continue` inside `HWDrawInfo::PreparePlayerSprites`/`PreparePlayerSprites3D`'s render loop in `hw_weapon.cpp` — code no script participates in at all. So "hide this layer, script-triggered, without touching the actor it belongs to" had no legal path in, the same wall the flat-overlay dimming fix (see below) had already documented.

**The field.** `p_pspr.h:288` adds `bool NoDraw = false;` to `DPSprite`, in the block headed `RS FORK -- SCRIPT-SUPPRESSED LAYER` at `p_pspr.h:274`. Like `Tint`/`Glow`/`ModelFrame` before it, this is an in-class initialiser rather than one set in the public constructor's body, and for the same load-bearing reason documented alongside them: `DPSprite` has a second, private, argument-less constructor (`DPSprite () {}`, `p_pspr.h:291`) used only by savegame deserialisation, which runs none of the public constructor's body. Without the `= false` on the declaration itself, a psprite resurrected from a save predating this field would carry whatever garbage sat in that memory rather than a defined "not suppressed" state. It's exported to script as a plain field — `DEFINE_FIELD(DPSprite, NoDraw)` at `p_pspr.cpp:147` — and declared `native bool NoDraw;` at `player.zs:3105`, with its own two-line comment restating the contract: *"Hide this layer without touching the weapon behind it. The weapon keeps its states, damage and slot; only the drawing stops."* No dedicated action function wraps it — a mod just writes `psp.NoDraw = true` on whichever `PSprite` layer it wants gone.

**Checked in both passes, on purpose.** The gate is `if (psp->NoDraw) continue;`, and it appears twice: once in the 2D/flat path at `hw_weapon.cpp:2259` (commented tersely, `// RS FORK -- see the 3D pass`, since the real explanation lives at the other site), and once in the 3D model path at `hw_weapon.cpp:2403`, where the comment spells out why both copies exist: *"Checked in BOTH passes, so a suppressed weapon disappears whether it draws as a model or a sprite; hiding it in one pass only would make the result depend on whether the mod happened to ship a mesh."* That's a real failure mode the fork clearly anticipated rather than found by accident — VR runs these two loops over the same `player->psprites` list, one keeping layers whose sprite/frame resolves a `FSpriteModelFrame` and one keeping the layers that don't, and a check placed in only one of them would make suppression silently conditional on whether `FindModelFrame` happened to succeed for that particular weapon.

**This is not the flat-overlay fix, despite sitting four lines away and quoting the same wall.** `hw_weapon.cpp:2267`'s `RS FORK -- FLAT OVERLAYS ON A WEAPON THAT IS ITSELF A MODEL` block is a separate mechanism solving a separate problem: when a weapon renders as a 3D model but one of its psprite layers (a muzzle flash from `A_GunFlash`, typically) has no model of its own, that layer falls through to the flat 2D pass and draws as a billboard hanging in front of the mesh. That block's fix is driven entirely by the `r_hudflatoverlay` cvar and an ownership walk (`ownerDrawsAsModel`, `hw_weapon.cpp:2291`-2304) that asks *"does the weapon owning this layer draw as a model?"* — script never sets anything on the layer itself, and a sprite-drawn weapon's flash is untouched regardless of the cvar. It reuses the *same justification* — `psp->alpha` being discarded absent `PSPF_ALPHA`/`PSPF_FORCEALPHA`, and the decision being a `continue` no script reaches — because both features hit that wall independently, not because they're one feature. `NoDraw` is script-driven and binary, applies to any layer regardless of what it owns, and is checked before the model-vs-sprite branch even splits (`hw_weapon.cpp:2260`/2265 for the 2D pass, `2404`/2408 for the 3D pass — the `NoDraw` check sits above both). Treat them as two independent fixes that happen to share a paragraph of reasoning; this entry is about `NoDraw` only.

**Was unserialised; fixed same day.** `DPSprite::Serialize` (`p_pspr.cpp:1456`-1487) writes `tint`, `glow`, `modelframe`, `modelframenext` and `modelframelerp` through its chained `(...)` builder right next to each other — `NoDraw` was left out when it was added, so a game saved while a layer was actively suppressed reloaded with the layer visible again: the in-class initialiser correctly gives an *old* save (predating this field) a defined `NoDraw == false`, but the same default silently overwrote a *deliberately-set* `true` on any save/reload, since nothing had ever written it to the archive in the first place. As of the bug being found, no shipped script actually set `.NoDraw` yet (a repo-wide search of `wadsrc/` turned up zero assignments, only the field declaration), so this had not visibly bitten anything — caught by documentation review, not by a report. Fixed by adding `("nodraw", NoDraw)` to the chain (`p_pspr.cpp:1479`), the same one-line pattern every other RS-fork `DPSprite` field already used.

---

## 38. Flat overlays on a weapon that is itself a model

`src/rendering/hwrenderer/scene/hw_weapon.cpp:2267` (`PreparePlayerSprites`),
cvar `r_hudflatoverlay` (`r_utility.cpp:105`, default `1.0`).

VR runs two passes over the same `player->psprites` list: `PreparePlayerSprites3D`
keeps whichever layers resolve a `FindModelFrame` (drawn as a mesh), and
`PreparePlayerSprites` — the flat 2D pass — keeps the layers that don't
(`smf` null, `hw_weapon.cpp:2265`, `if (smf) continue;`). A muzzle flash from
`A_GunFlash` is its own psprite layer, owned by the same weapon, and it
almost never has a model of its own — MODELDEF entries are written for the
gun, not for every one-frame flash state it can jump to. So when the gun
itself IS a model, the 3D pass draws the mesh and the 2D pass still draws the
flash, and the flash lands as a flat billboard hanging in space in front of a
3D weapon: it never got smaller, closer, or better integrated just because
its owner did.

**Not fixable from the mod side, which is why this is a fork change and not a
ZScript one.** `DPSprite::GetRenderStyle` discards `psp->alpha` unless the
layer explicitly carries `PSPF_ALPHA` or `PSPF_FORCEALPHA` — a plain
`A_GunFlash` overlay sets neither — and the actual draw/skip decision is a
bare `continue` inside a render loop no script participates in at all. A mod
wanting to dim or hide its own flash overlay when the gun is a model has
nothing to reach for.

The fix is scoped as narrowly as the problem: *this weapon's owner is drawn
as a model*, not *this is a flash* or *hide all overlays*. Before building the
`HUDSprite`, the 2D pass walks the player's other psprite layers looking for
the one that actually owns the weapon slot (`PSP_WEAPON`/`PSP_OFFHANDWEAPON`)
with the same `Caller`, and checks whether THAT layer resolves a model:

```cpp
bool ownerDrawsAsModel = false;
for (DPSprite *own = player->psprites; own != nullptr && ...; own = own->GetNext())
{
    if (own == psp || own->Caller != psp->Caller) continue;
    if (own->GetID() != PSP_WEAPON && own->GetID() != PSP_OFFHANDWEAPON) continue;
    if (!own->GetState()) continue;
    if (FindModelFrame(own->Caller, own->GetSprite(), own->GetFrame(), false))
    {
        ownerDrawsAsModel = true;
        break;
    }
}
```

Only if that's true does `r_hudflatoverlay` do anything at all. A sprite-drawn
weapon's flash is completely untouched regardless of the cvar's value, because
the lookup above returns false for it and nothing downstream even checks the
cvar.

**Dimmed, not just hidden — and the cvar is a fader, not a switch.**
`r_hudflatoverlay` runs 0 to 1. At `<= 0` the overlay is skipped outright
(`continue`, before a `HUDSprite` is even built). Between 0 and 1 it survives
but is scaled down: `hudsprite.alpha *= flatOverlayAlpha`, applied *after*
`GetWeaponRenderStyle` runs, since that call is what establishes the layer's
base alpha in the first place — multiplying before it ran would just get
overwritten. At the `1.0` default nothing is touched either way, so stock
behaviour is exactly preserved until a mod or a player opts in by lowering the
cvar; this is off by default in the same sense most of this fork's rendering
opt-ins are.

**Distinct from `NoDraw` (script-suppressed layers, previous entry), despite
living four lines away and citing the identical wall.** Both hit the same
`psp->alpha`-is-discarded, `continue`-in-an-unreachable-loop problem
independently, but `NoDraw` is script-driven, binary, and applies to any
layer for any reason a mod chooses; this fix is cvar-driven, continuous, and
applies specifically to non-weapon-slot overlays whose owning weapon draws as
a model. The `NoDraw` check sits above this one in the same loop and is
checked first — a layer a script has explicitly hidden never reaches the
`ownerDrawsAsModel` walk at all.

**Gotcha:** the ownership walk excludes `PSP_WEAPON`/`PSP_OFFHANDWEAPON`
themselves (`psp->GetID() != PSP_WEAPON && psp->GetID() != PSP_OFFHANDWEAPON`,
`hw_weapon.cpp:2289`) — it only ever dims layers that are NOT the main weapon
slot, since the main weapon slot is what the walk is searching FOR, not a
candidate for its own dimming. A mod that adds a flash as a third, independent
`PSP_WEAPON`-slot layer rather than a genuine overlay layer would sit outside
this mechanism entirely and always draw at full opacity.

---

## 39. Static poses on decoupled models, and the trace built to find them missing

`+DECOUPLEDANIMATIONS` (`MF9_DECOUPLEDANIMATIONS`) is the flag that switches a model from legacy MD3 frame-morphing to true bone animation — `SetAnimation`, per-bone get/set, the whole `CalculateBones` machinery in `p_actionfunctions.cpp:6450` onward is gated on it, and it throws `X_OTHER` on anything that isn't decoupled. It's the mesh type this fork's hand and weapon models use. Two other sections in this file exist specifically to let ZScript pin a HUD model to one authored frame: §19 (`DPSprite::ModelFrame`/`ModelFrameNext`, models.cpp:757) and §21 (native state remap, `CalcModelOverrides`'s `stateRemap` table, models.cpp:799-820). Both write their answer into the same two fields — `drawinfo.modelframe` / `drawinfo.modelframenext` — and both set `drawinfo.modelframe_explicit = true` (`ModelDrawInfo::modelframe_explicit`, models.h:143) to say "this number was chosen on purpose, not fallen into." Both landed, both compiled, and both were **entirely inert on a decoupled model**, because nothing downstream of `CalcModelOverrides` ever consulted `modelframe` for that model type. A pose that never applies looks exactly like a pose that was never set — nothing throws, nothing logs, the hand just sits in whatever the animation system last put it in.

### Where it actually broke

`ProcessModelFrame` (models.cpp:826) is where a model's frame number turns into bone matrices. Before this fix, the `is_decoupled` branch (models.cpp:854) had exactly two outcomes: `frameinfo.decoupled_frame.frame1 >= 0` — an animation clip is actively playing via `SetAnimation`, so `CalculateBones` runs against `frameinfo.decoupled_frame` (models.cpp:856-868) — or nothing is playing, so `CalculateBonesOnlyOffsets` returns the rest pose plus whatever bone overrides are active (the old final `else`, now at models.cpp:905-913). Neither branch read `drawinfo.modelframe` at all — that field is consulted only on the *non-decoupled* branch further down (models.cpp:915-931). So `ModelFrame = 42` set by a controller-driven hand-pose script, or a `stateRemap` table hit, would resolve correctly, get written into `drawinfo.modelframe`, get flagged `modelframe_explicit = true` — and then get silently discarded by `ProcessModelFrame`, which would render either whatever animation clip happened to be active or the bind-pose rest frame instead. A decoupled model could not be pinned to a single authored frame at all; the entire apparatus §19 and §21 built for exactly this purpose was a no-op the moment a mesh switched to bones.

### The fix: a third branch, gated on `modelframe_explicit`

models.cpp:869-904 inserts a middle branch between "animation playing" and "nothing at all": `else if (drawinfo.modelframe_explicit)`. It calls the same `CalculateBones` the animation branch uses, but builds the frame argument by hand — `ModelAnimFrameInterp{ nextFrame ? frameinfo.inter : -1.0f, drawinfo.modelframe, drawinfo.modelframenext }`, `prev = nullptr`, `prevInter = -1.0f` — which is, deliberately, **the identical call the ordinary non-decoupled branch makes** a few dozen lines later (models.cpp:917-931). The in-code comment says as much: "Same construction as the non-decoupled branch below, so an explicitly addressed frame means the same thing on both paths." Bone overrides (`modelData->modelBoneOverrides`) still compose on top afterward, same as both other branches, so a posed hand remains proceduarally adjustable — finger-curl offsets etc. — after the baked pose is applied.

This is exactly what hand posing needs: the poses are baked as frames of one clip, and ZScript picks one per tic from controller input (trigger pull, grip state) — there's no clip to *play*, just one of N discrete poses to *hold*. The alternative — driving it through `SetAnimation` at zero framerate to fake stillness — means fighting the animation clock (`calcFrames`, `SetAnimationFrameRateInternal`) every tic just to make it produce a static result, instead of reusing the frame-pinning path that already existed and was already correct for MD3 models.

### Branch order is the priority order

The three cases are checked in this order: **playing animation wins over a held static pose, which wins over the rest-pose fallback.** So if a script calls `SetAnimation` on a hand actor while a static `ModelFrame` pose is also set, the animation always overrides the pose — the static branch only ever fires when `decoupled_frame.frame1 < 0`, i.e. nothing is actively animating (`curAnim.flags & MODELANIM_NONE`, or no `SetAnimation` call has ever landed). This is presumably intentional — a hand that's mid-reload-animation shouldn't be yanked into a grip pose — but it's a real trap: a stale `SetAnimation` call left active (or an `AnimInfo` that never got flagged `MODELANIM_NONE`) will silently outrank a `ModelFrame` poke, with the exact same "looks unset" failure signature this whole fix exists to cure. Worth checking first if a pose still won't stick after this section's mechanism is confirmed present.

### `vr_pose_debug`: the trace that this was built and verified with

`CVAR(Bool, vr_pose_debug, true, 0)` (models.cpp:52) — default **on**, no `CVAR_ARCHIVE`, so it never persists to config and starts live on every launch. That's deliberate for a print that only fires on state change (see below), not continuously, so leaving it on by default costs nothing in the common case but means an unrelated session chasing console spam should check this cvar first.

Two probes, one on each side of the frame-resolution boundary:

- **`[POSE/in ]`** — models.cpp:782-797, inside `CalcModelOverrides`, placed right after the §19 direct-addressing block (models.cpp:775-779) but *before* the §21 native-remap block (models.cpp:799-820) runs. It logs `psp->ModelFrame` (the raw script input) against the `out.modelframe`/`modelframenext`/`explicit` that resulted — i.e. the state after §19, before §21 has a chance to overwrite it. Each hand gets its own static `last` slot (`lastMain`/`lastOff`, keyed off `psp->GetID() >= PSP_OFFHANDWEAPON`) specifically "so the two do not mask each other's changes" — with one shared slot, an offhand update would update `last` and suppress the next genuine mainhand change from printing.
- **`[POSE/out]`** — models.cpp:838-852, inside `ProcessModelFrame`, only when `is_decoupled`. It names the exact three-way branch this section documents: `names[3] = { "ANIM (SetAnimation wins)", "STATIC (our pose)", "REST (pose discarded)" }`, selected by the same `frame1 >= 0` / `modelframe_explicit` check the real code branches on. This is the probe that actually exposed the bug — before the STATIC branch existed, this classification would only ever have had two real outcomes, and "our pose" landing in "REST (pose discarded)" is precisely what a silently-dropped `ModelFrame` looks like from here.

Both prints key on a packed integer and only print when that key changes ("in": `psp.ModelFrame*4 + explicit-bit + modelframe<<12`; "out": `branch + drawinfo.modelframe<<4`), so a held pose reports once instead of spamming every rendered frame — relevant given `CalcModelOverrides`/`ProcessModelFrame` run per hand, per frame, at VR framerates.

### Gotcha: the two probes don't describe the same instant

Because `[POSE/in ]` is placed *before* the §21 state-remap block and `[POSE/out]` is read *after* it, a weapon using a `stateRemap` table can show a `[POSE/in ]` line reporting the pre-remap frame while `[POSE/out]` (and the actual render) reflects the remap-supplied frame instead. Trust `/out` for what actually rendered; `/in` is only useful for confirming what the psprite fields themselves carried before the table had a chance to win. And if a fourth branch is ever added to the decoupled `if/else if/else` in `ProcessModelFrame`, the `names[3]` array and the `branch` ternary at models.cpp:841-842 have to grow together, or the debug line will print the wrong label for a real, correctly-rendered branch.

---

## 40. Array-element field reflection

```
Level.GetFieldIntArray(Object o, string field, int index, out int value) -> bool
```

§30's field reflection reads a *scalar* by name — `GetFieldInt` type-checks a
resolved `PField` against `TypeSInt32`/`TypeUInt32` and refuses anything
else, which is exactly correct for a plain field and exactly wrong for a
fixed array. `int user_equippedDamage[MAX_EQUIPPED_ITEMS];` is declared as
ONE field whose `PField::Type` is a `PArray`, not `MAX_EQUIPPED_ITEMS`
separate int fields — so the scalar getter's own type check, working as
designed, said no to every element of it.

Two real mods hit this wall in the same session doing the exact reading
§30 was built for: Doomablo's `int currentStats[totalStatsCount]` (its
Vitality/CritChance/CritDmg/Strength/RareFind rolls) and DECORATE-only
BorderDoom's `user_equipped{Damage,Accuracy,Firerate,...}[MAX_EQUIPPED_ITEMS]`
family, keyed by weapon slot. Both are plain, safe-to-read data with a
correct getter refusing them for a type-system reason a caller has no way
to work around from script.

`GetFieldIntArray` reuses `WR_ResolveField` unchanged (same private/meta/
static refusal, same inherited-field walk) and adds exactly what an array
needs on top: confirm `f->Type->isArray()`, `static_cast` to `PArray`, and
bounds-check the requested index against the array's OWN `ElementCount`
(`vmthunks.cpp`, next to `GetFieldInt`) — a caller cannot read past the end
of another mod's array by guessing a larger size than it actually has.
Element type is checked too (`arr->ElementType != TypeSInt32 &&
... != TypeUInt32` fails clean), so an array of structs or strings refuses
rather than reinterpreting its bytes as an int. The element address is
`f->Offset + index * arr->ElementSize` — `ElementSize` comes straight off
the `PArray` itself, not recomputed from `ElementType->Size`, so it is
correct even for element types with nonstandard padding.

**`dyn_cast` does not work here, and cost a full build cycle finding out.**
`PField`/`PSymbol` (used by `WR_ResolveField` for the field lookup itself)
belong to the scripting *symbol* hierarchy, where `dyn_cast` is a real,
working RTTI-checked cast. `PType`/`PArray` belong to a completely separate
hierarchy — the *type system* — and `dyn_cast<PArray>(f->Type)` fails to
compile outright (`C2672: no matching overloaded function`, the only
overloads found being for `DObject*`, a third, unrelated hierarchy again).
The actual idiom, confirmed against `codegen.cpp`'s own array-index handling
(`FxArrayElement`, e.g. `codegen.cpp:13269-13270`): check the predicate
`type->isArray()` first, then `static_cast<PArray*>(type)` once it's true.
Three different "is this a subclass" mechanisms across three hierarchies in
the same codebase — cast idiom is not a fact that generalises from one
`Cast<T>` site to the next one in this engine; check what the *target*
hierarchy actually uses before assuming.

**Read-only, same as §30, for the same reason.** No `SetFieldIntArray`
exists or is planned — writing into another mod's array is exactly the
"corruption and crash in different mods" problem §30's own header already
rules out for scalars, and an array write is the same problem with an added
way to get the bounds check wrong.

**Not itself an ACS reader.** This closes the specific "array of plain
ints" gap, and only that gap — it does nothing for BorderDoom's real
per-shot stats, which live behind live `CallACS` calls into compiled
bytecode with a confirmed mutating side effect (strips and regrants ammo
capacity as normal control flow — traced by hand, see the wheel-side
project memory on this), not behind a field at all. Field reflection, array
or scalar, only ever reaches data a mod already stored as an object field.

---

## 41. HUD bone anchoring — a psprite layer drawn at another layer's bone

psprite layers were completely independent — nothing could follow anything —
so every "put this exactly there" problem (a hand on a grip, a magazine
entering its well, a shell at a loading port) was solved by hand-tuning
offsets per weapon, and re-tuned whenever either model moved. Those are all
the same problem: one layer needs to sit at a measured point on another
layer's mesh, not at a guessed offset from the controller.

```zscript
native int  PSprite.AnchorLayer;   // layer id to follow, -1 = not anchored
native Name PSprite.AnchorBone;    // bone name on that layer's model
```

When a model's bones are resolved for the GPU (`RenderModelFrame`,
`models.cpp`), any bone another layer has asked for is combined with that
model's world transform and published into a small table keyed by
`(layer id, bone name)` (`HudAnchor_Store`/`HudAnchor_Get`, `models.cpp`).
The anchored layer reads that transform in `RenderHUDModel` instead of
starting from the controller — driven by requests rather than storing every
bone, since a rigged weapon has dozens and almost none are ever anchored to.

**The anchored layer must have a higher id than its target.** Psprites draw
in id order, and the target's bones aren't known until it has already been
drawn — this is a same-frame dependency with no scheduling behind it, just an
ordering constraint on the caller.

Three things had to be right for it to work at all:

**An anchored model skips the PLAYER-relative block entirely** — global
weapon offset, bob, aim rotation, the viewmodel axis fix. The bone matrix was
captured *after* its target had already been through all of that, so applying
it again would add the weapon's position and rotation a second time and throw
the attachment clear of the bone. Only the model's own `MODELDEF` offsets,
rotations and scale survive for an anchored layer; they now apply relative to
the bone instead of the controller, which is what lets `MODELDEF` still
fine-tune the fit exactly as it does for an unanchored model.

**Bones are published for every HUD model, not only decoupled ones.**
Restricting publication to `MODELSAREATTACHMENTS`/decoupled models meant a
plain (non-decoupled) weapon model silently published nothing, so anchoring
to it did nothing and looked like a script bug rather than a missing feature.

**Anchors are frame-stamped.** `HudAnchor_BeginFrame()` runs once per psprite
render pass and ticks a counter; a stored anchor is only honoured if its stamp
matches the current one. A target that stops being drawn — hidden, `NoDraw`d,
switched away from — leaves no stale entry freezing an attachment where the
weapon last was; the requesting layer simply falls back to its own placement.

---

## 42. Model diagnostics: `vr_validate` / `vr_spatialreport`, and a crash one step behind them

Two CVars, on by default, that check a HUD model the first time it is drawn
and print once — never every frame, and never a behaviour change:

```
vr_validate       // bool, default true
vr_spatialreport  // bool, default true
```

`ValidateHudModel` (`models.cpp`) catches two things that each present as an
unrelated bug and cost a full headset session to find by eye: a skinned model
(bone count > 0) with **zero animation frames** — collapses on the GPU and
reads as missing geometry or missing textures, never as a pose problem — and
a skin with a translucent alpha channel where `IgnoreSkinAlpha` (§35) is
**not** set, which reads as a half-transparent gun rather than as packed PBR
data. Each check fires at most once per `FSpriteModelFrame`/slot
(`ValidateOnce`), not once per frame.

`vr_spatialreport` prints where a layer's model actually landed — position is
the transform's translation column, scale is the length of its first column
— because "tiny and far away" and "correctly sized but mispositioned" are
indistinguishable through a headset and are entirely different bugs. The
gate is a single shared timestamp (`static uint64_t lastReport` in
`RenderHUDModel`, throttled to once per second), not one per layer, so with
several layers drawing in the same frame only one line prints per second —
whichever layer's draw happens to be current when the second ticks over —
not a report for every layer every second.

### The crash it was one step from causing

`ValidateHudModel` was originally called as
`Models[smf->modelIDs.Size() ? smf->modelIDs[0] : 0]` — the first model id in
the frame's array, or index 0 if the array is empty. A model frame can have a
non-empty `modelIDs` whose first entry is `-1` (declared, but with no model
bound), and `-1` was passed straight through as an array index: `Models[-1]`,
an out-of-bounds read on the first HUD weapon draw that hit it, on any map
that used one. Fixed by resolving the index defensively — checked against
both `0` and `Models.SSize()` — and passing `nullptr` through when it's out
of range, which `ValidateHudModel` already early-outs on.

---

## 43. Sweep room bound — the air lattice gets a room, instead of an infinite plane

The sweep's air lattice (§12) is built from a plane with no extent —
"perpendicular to X at `o.x`" exists at every Y and Z on the map. The band
had a radius; the plane defining the lattice did not. So this was never a
leak to patch: the primitive had no concept of a room at all, and any window
pointing anywhere near a lattice-filled sweep showed lasers hanging in a room
the sweep had never entered.

```
Level.SetSweepRoom(min, max, soft)
```

Publishes a box (`SweepRoomMin`/`SweepRoomMax`/`SweepRoomSoft` on
`FLevelLocals`) that the lattice fades out past. Tested at **the lattice's
own hit point**, not at the fragment behind it — the question is where the
grid is hanging in the air, not what surface happens to be drawn there, so a
grid seen through a doorway reads as outside the room even though the wall
beyond the doorway is inside one.

`soft <= 0` removes the bound entirely, which is what every level that never
calls this gets — bit-identical to the old unbounded behaviour, and why this
needed no separate enable flag.

**Why script decides the box, not the renderer.** "Which sectors are one
room" is a judgement, not a fact the engine can derive: a Doom room is almost
never one sector — steps, light panels, door tracks and alcoves are all their
own — and whether a window or a step ends a room has no single right answer.
The rule belongs in readable script where a mod author can argue with it, not
welded into the renderer.

Four sync points, the same four any `HWViewpointUniforms` addition needs
(`g_levellocals.h`, `hw_viewpointuniforms.h`, both shader-side struct
mirrors) — plus the Vulkan per-eye `#define` aliases, which is exactly where
this class of change has failed silently before (§32): declared correctly in
both `gl_shader.cpp` and `vk_shader.cpp` is not sufficient on its own if a
member is missing its `viewpoints[HW_VIEWPOINT_INDEX].` alias next to it.

## 44. Capacitive finger contact — where a finger RESTS, not what it presses

Touch controllers report skin contact on the thumbrest, thumbstick and face
buttons without a press. That distinction is the whole point: a thumb lying on
the stick and a thumb lifted clear are the same button state, and so are a
finger indexed along the frame and a finger sitting on the trigger. Buttons
cannot tell those apart, so a hand posed from buttons alone can never show the
difference.

Published to script as `FingerTouchMain` / `FingerTouchOff`, a bitfield rather
than two booleans so Index-style per-finger sensing can be added later without
changing the field:

```
FINGERTOUCH_THUMB = 1 << 0
FINGERTOUCH_INDEX = 1 << 1
```

Thumb contact is **one boolean action bound to six surfaces** — thumbrest,
thumbstick, A, B, X, Y. A boolean action is true when any bound source is, so
the runtime does the OR and "the thumb is resting on something" needs no
per-surface logic on our side.

**Suggested only for the Oculus Touch profile.** A suggested binding naming a
path a profile does not define makes `xrSuggestInteractionProfileBindings`
reject the *entire profile* rather than the one binding — which presents as the
controller losing all input, not as a binding failing. Vive, WMR and the simple
profile define none of these paths, so they are not offered them.

## 45. Haptics: serviced on the menu path, and a way to see them

`UpdateControllerState` returned early when a menu was open, before reaching the
`ProcessHaptics()` at the end of the function. A pulse in flight when a menu
opened was therefore never advanced and never stopped. The sibling early return
immediately above it already called `ProcessHaptics` for exactly this reason;
this one did not.

Also adds `vr_haptic_debug` and a `vr_haptictest` command that fires a pulse
directly, bypassing every gameplay trigger. A pipeline that drops pulses and a
game that never requests them both present as a silent controller, and nothing
in the existing logging separated the two.

Worth recording for whoever chases haptics next: `MakeOpenXRHapticDuration`
caps every pulse at 10ms and re-issues it per frame. At 90Hz the frame interval
is 11.1ms, so there is a gap in every pulse — which, combined with
`vr_pickup_haptic_level` defaulting to 0.2, can read as haptics not working at
all while every stage reports success.

## 46. The engine always writes a log

Written to the current working directory — wherever the launcher started the
game — as `doomxr-log.txt`, on every launch, with no argument required. An
explicit `-logfile` still wins; this only fills in when nothing was asked for.

Unconditional on purpose. Console scrollback dies with the process, so a hard
crash otherwise leaves nothing behind, and a log that depends on having
remembered a flag is exactly the log you do not have when you need one.

**Not routed through `execLogfile()`.** That helper prefixes `log-`, runs
`C_SanitizeFileName` — which replaces every `.` and `:` with `-` — and then
appends `.txt` regardless. So `+logfile E:/rslog.txt` never wrote to
`E:\rslog.txt`; it wrote `log-E-/rslog-txt.txt` into the working directory.
Hours were lost to logs that were being written correctly under names nobody
would look for. A predictable name needs a plain `fopen`, with a fallback
beside the exe if the launch directory is not writable.

## 47. Per-hand reload keys freed for physical reloading

`BT_MAINHANDRELOAD` and `BT_OFFHANDRELOAD` were wired into `ButtonChecks`
alongside the generic `BT_RELOAD`, so all three jumped a weapon to its `Reload`
state.

That is wrong once reloading is physical. The per-hand keys now mean *drop that
hand's magazine*, or *rack that hand's pump* — and leaving them also bound to
the Reload state meant one press both ejected the magazine and instantly
refilled it, which cancels out and looks like the button doing nothing.

The two per-hand rows are commented out. The generic `BT_RELOAD` rows for both
hands are untouched, so the plain reload key still performs the classic instant
reload for either hand, which is the intended fallback when physical reloading
is switched off.

The controls menu was reordered to match: per-hand reload sits with Attack and
Alt Attack where it is now a primary action, and the generic Reload moved down
under Advanced Reloading where it is now the legacy path.

## 48. `bKeepWhenEmpty` — an empty weapon stays in your hand

`CheckAmmo` calls `PickNewWeapon` the moment ammo reaches zero. Under manual
reloading that is fatal: ejecting a magazine empties the weapon, so the weapon
leaves your hand before a fresh magazine can go into it. Manual reloading is
impossible by construction while that happens.

A `bool bKeepWhenEmpty` on `Weapon`, checked at both `PickNewWeapon` sites.
Firing is unaffected — the return value still reports the true ammo state, so an
empty weapon still refuses to fire. It simply is not confiscated.

**A plain field, deliberately not a flagdef**, and the reason is worth writing
down because it cost most of a session:

ZScript `flagdef`s reach the flag parser through `@flagdef@`-prefixed symbols
that `CompileFlagDefs` adds to the class symbol table — and, per the comment
there, removes again once the compiler finishes. Flags defined in **C++**
`FlagLists` persist; flags defined as ZScript flagdefs do not survive to mod
compile time. The observable result is that `+WEAPON.NOAUTOAIM` works from a
pk3 while `+WEAPON.AMMO_OPTIONAL` and `+WEAPON.NOAUTOSWITCHTO` both fail with
"Unknown flag", despite all three being valid and all three working in the
engine's own scripts.

That asymmetry is a real bug and is *not* fixed here — this section only routes
around it. Anyone fixing it properly should start at
`ZCCDoomCompiler::CompileFlagDefs` in `zcc_compile_doom.cpp` and at
`FindFlag`'s `strict` handling in `thingdef_data.cpp`, where the bare-name
lookup is additionally marked `decorateOnly` whenever a prefix exists.

A field is set by plain assignment from `BeginPlay` and cannot be broken by any
of it.
