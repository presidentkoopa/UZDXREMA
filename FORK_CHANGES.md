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

Six payloads: `BB_PANEL`, `BB_TEXTURE`, `BB_DIGITS`, `BB_GLYPH`, `BB_RING`,
`BB_BAR`. All six draw. Also: view-locking (resolved at *render* rate, not tic
rate, which is what makes a head-locked panel not swim), per-billboard alpha,
`BBFL_NODEPTH`, `BBFL_FOLLOWANGLE`, save/load, budget and distance culling.

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

---

## Building

`auto-setup-windows-vr.cmd` locates Visual Studio's bundled CMake via
`vswhere` — CMake is generally not on PATH. Build output lands in
`build-dxr/RelWithDebInfo/`.
