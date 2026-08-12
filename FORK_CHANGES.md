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

Eight beams, in the viewpoint block: enough for a weapon beam plus a tripwire
grid, and the per-fragment cost is eight cheap segment tests.

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

## 15. Direct model frame addressing on psprites

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

## Building

`auto-setup-windows-vr.cmd` locates Visual Studio's bundled CMake via
`vswhere` — CMake is generally not on PATH. Build output lands in
`build-dxr/RelWithDebInfo/`.
