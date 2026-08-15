# DoomXR fork — engine changes

Everything this fork adds on top of upstream, in one place, so another
engine developer can see what was touched and why without reading the log.

Branch: `doomxr`. Each area below is self-contained; nothing here depends on
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
| [Everything else this fork adds](#27-everything-else-this-fork-adds) | the laser sight itself, tracers, the psprite recursion guard, and every native with no API entry | — |

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

**Ten payloads**, not the six this paragraph claimed until the fork was audited
against itself: `BB_PANEL`, `BB_TEXTURE`, `BB_DIGITS`, `BB_GLYPH`, `BB_RING`,
`BB_BAR`, then `BB_SEGMENT` (a 16-segment display drawn procedurally, no atlas),
`BB_SEGLCD` (its inverse — a lit plate with the digits punched out of it),
`BB_SEAM` (a glowing slit you open with `ResizeBillboard`; with the void flag the
opening is a *hole* with a bright rim rather than a lit slab), and `BB_WG13` (a
transcribed kill badge, plate and digits in one pass). Index 6 is unused, so the
enum runs 0–10. All ten draw. `BILLBOARDS.md:81` documents them properly.

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

## 27. Everything else this fork adds

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
