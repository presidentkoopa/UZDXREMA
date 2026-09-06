# Visual Catalog, plain language

Reference doc. No engine jargon, just what each knob does. One topic per section, presets at the bottom of each once we're ready.

---

## Glow

### The four lanes

A wall has two lanes. A sector (room) adds two more.

| Lane | Grows from | Set with |
|---|---|---|
| WallBottom | the floor line, going up | `Sector.SetGlowColor` etc. (pos = Sector.floor) |
| WallTop | the ceiling line, going down | `Sector.SetGlowColor` etc. (pos = Sector.ceiling) |
| Floor | the floor, spreading across it | `Sector.SetFlatGlowColor` (pos = Sector.floor) |
| Ceiling | the ceiling, spreading across it | `Sector.SetFlatGlowColor` (pos = Sector.ceiling) |

Each lane is independent. You can set all four differently, or only touch one.

### What you control, per lane

- **Color** — the color right at the source (the floor/ceiling line for walls, the flat itself for floor/ceiling).
- **FarColor** — optional. The color it fades *toward* at the edge of its reach. If you don't set it, the glow just fades to nothing instead (transparent), like it always used to.
- **Height** — how far the glow reaches before it's done. This is coverage, not brightness.
- **Falloff** — the shape of the fade between Color and FarColor/nothing. Four options:
  - **Linear** — fades evenly the whole way.
  - **Squared** — stays close to full strength near the source, then drops off sharply near the edge. Reads as a "tight" glow.
  - **Sqrt (root)** — drops off fast right near the source, then lingers faint for the rest of the distance. Reads as a "soft/spread" glow.
  - **Exponential** — drops off even faster right at the source than Sqrt does, then trails off in a long faint tail. Reads as a hot core with a wisp of glow well past it.
- **Intensity** — how strong/bright the color reads. Separate from Height — Height is *how far*, Intensity is *how much*. Turning Intensity up doesn't make it reach further, it makes it punch harder within the reach it already has.

### The Color → FarColor ramp

Color sits at the source. FarColor is what it's blended into by the time you reach Height's distance. Everything between is a smooth mix from one to the other.

Example: WallBottom, Color = Green, FarColor = Orange. Right at the floor line it's green. Climbing up the wall toward Height's limit, it gradually shifts to orange.

### What a whole wall can look like

Top to bottom, if WallTop and WallBottom don't reach far enough to overlap:

1. Ceiling line: WallTop's **Color**
2. fading down to WallTop's **FarColor** (at WallTop's Height)
3. ...plain wall, no glow...
4. WallBottom's **FarColor** (at WallBottom's Height)
5. fading down to WallBottom's **Color** at the floor line

Four distinct colors possible on one wall.

### What happens when lanes overlap

They don't blend into each other or crossfade. **They add.**

Under the hood, Color/FarColor are just RGB numbers (Green might be `(0, 200, 0)`, Orange `(255, 140, 0)`). Where two lanes cover the same pixel, the engine adds each channel — R+R, G+G, B+B — and clamps the result so it can't go above max brightness.

So overlapping Green and Orange doesn't make a smooth in-between color, it makes a brighter combined color (leans yellow-ish here), and if both lanes are strong/intense it can wash out toward white in the overlap zone.

This means: if you want a clean two-tone wall, keep WallTop's and WallBottom's Height short enough that they don't reach each other. If you *want* a hot blown-out band in the middle, overlap them on purpose.

### Quick mental model

- Color/FarColor = what colors, and the ramp between them.
- Height = how far each color-ramp reaches.
- Falloff = the shape of the fade (tight vs soft).
- Intensity = how strong it reads, independent of reach.
- Overlaps between lanes = plain addition, not blending.

### Options menu — CONFIRMED shape

```
Glow Lanes (root)
├─ Glow: On / Off          master switch, top of the page
├─ Blended Seams: On / Off  junction color trick (see below); only acts where both sides of a corner are on
├─ Transition speed        slider, seconds each color breathes before shifting to the next (shared by all lanes)
├─ Presets                 6 named looks + No Preset; picking one sets all four lanes at once
│   ├─ Freeze               stops the breathing exactly where it is, so you can catch a color you like
│   └─ Keep this            bakes the current (possibly frozen) color into Customize, switches to No Preset
├─ Randomize
│   ├─ Reroll              button, rolls fresh colors into all four lanes now
│   ├─ Style               Any / Warm / Cool / Matching (Matching = one color, varied per lane)
│   └─ (Lock toggle lives on each Customize page, per lane)
├─ Wave                    NOT tied to presets -- its own toggle, always layered on top
│   ├─ Wave: On / Off
│   ├─ Wavelength
│   ├─ Speed
│   └─ Shape               Outward from a point / Sideways (either way) / In a bubble / Up and down
└─ Customize               submenu, the deep-dive
    ├─ Wall Bottom         Color (R/G/B sliders), Use Far Color, Far Color (R/G/B), Coverage, Falloff, Intensity, Lock
    ├─ Wall Top            (same + Lock)
    ├─ Floor               (same + Lock)
    └─ Ceiling             (same + Lock)
```

Fun/fast stuff (on-off, presets, randomize) up front; sliders buried one level down in Customize so they don't get in the way. Scope for v1: sets glow for the whole map at once, not per-room — per-room targeting is a later step.

Falloff shows plain words in the menu, not engine names: Even / Tight / Soft / Hot core (= Linear / Squared / Sqrt / Exponential).

**Color picking is three plain sliders** -- Customize's Color and Far Color are each Red/Green/Blue, 0-255, same shape as the original `Environmental_Lighting03_GlowInTheDark` mod this project remakes. Went through two wrong turns first: the engine's real `ColorPicker` widget (menu worked, glow never showed in-game, twice) and a 32-entry named palette (worked structurally but wasn't the interaction the user actually wanted). Landed on plain R/G/B sliders after reading the OG mod's own menudef directly. Presets and Randomize/Chaos aren't affected by any of this -- they build colors directly in code and never touch a cvar for it.

### Wave

A separate system layered on top of whatever the lanes are already showing -- ripples each lane's edge instead of leaving it a dead straight line across the whole room. Deliberately trimmed from the engine's full ten-knob version down to four: On/Off, Wavelength, Speed, Shape. Sharpness, Origin, the brightness/color wobble modes, and per-lane phase offset are all hidden behind fixed sane defaults -- only the "reach" depth channel is used, since that's the one that actually moves the edge rather than just pulsing brightness.

### Color sets — each lane cycles, doesn't sit still

A lane isn't locked to one fixed Color+FarColor forever. Each lane holds its own small playlist — 4 pairs — and slowly fades from one to the next, on and on ("breathing"). This isn't an engine feature — the engine's Color/FarColor knobs only ever hold one value each. The mod owns the list of 4 and feeds the real knob a smooth blend between whichever two it's currently between.

**Presets only own color** (the cycling sets + fade speed). Coverage, Falloff, and Intensity are always the player's own Customize settings, regardless of which preset is active — picking a preset never touches them. Presets are the "mood," Customize sliders are the "shape"; the two never overwrite each other.

All four lanes share **one speed dial** — the whole room breathes together, not four separate rhythms. That dial is a single player-set slider (Transition speed, on the root page) shared by every preset, rather than each preset carrying its own speed.

Within one instant, the four lanes don't all show the identical color either — each lane is offset to a different point in the same shared palette (staggered by one step: wall bottom / wall top / floor / ceiling), so you see several related colors from the palette at once, all rotating together. That's what makes Neon Chaos vs. Neon Unison an actually visible difference: Chaos gives each lane its own independent random color with no relationship to the others, Unison stays on one shared, harmonious palette.

### Presets — CONFIRMED list

| Preset | What it is |
|---|---|
| **No Preset** | Restores the player's own Customize values. Not a look — an "off," same idea as DarkDoomZ's own "Off = your own colours, your own dials." |
| **Red Alarm** | Red, orange, blood orange, yellow. Breathing through them. |
| **Cold War** | Same idea, blues and violets. |
| **Neon Unison** | Colors that actually go together — hot pink, electric blue, violet, teal. All cousins of each other. |
| **Neon Chaos** | Colors that don't have to get along — clashing is the point. Doesn't use a fixed palette: every fade, each lane independently rolls a fresh random color instead of picking from a shared list, so "chaos" means genuinely uncoordinated, not just four colors picked to clash. |
| **Toxic** | Sickly greens and yellow-green. Poison/radiation feel. |
| **Deep Sea** | Teals and dark blues with a hint of glowing green. Moody, underwater. |

Goal across all of them: a lot of variety preset-to-preset, but not overwhelming within one preset (except Neon Chaos, where unpredictable IS the feature).

---

## Beams

A beam is a straight line of light in 3D space — a start point and an end point, glowing along its whole length. It floats independent of geometry (unlike glow, which is glued to a wall/floor/ceiling), and it lights nearby surfaces the way a real light source would.

### Per-beam controls

- **Start / End** — the two points it stretches between.
- **Thick** — width of the bright core.
- **Soft** — width of the hazy halo around the core.
- **Color**
- **Intensity** — brightness.

### Shared "look" controls

These apply to *every* beam in the level at once, not per-beam: how much beams glow in open air, scrolling energy along their length, taper, and an impact flare.

### Used by

- **RS_Lance** — its attack. A "cook" beam plus a 3-beam core/fill setup.
- **RS_Hands grab indicator** — 2 beams, one per hand, drawing the grab-target line.

### Presets

*(empty for now — fill in once we start building them)*
