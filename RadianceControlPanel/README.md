# Radiance Control Panel

**The control centre for the engine fork's features.** Nearly all of them concern
light and visuals, so they belong behind one panel rather than scattered across
a menu each. Glow is the first tenant, not the whole purpose — expect this to
accumulate every fork-side visual system over time.

Longer term this folds into **Radiance Silvergun Options**, either integrated or
merged outright. There will be a comprehensive menu pass; until then, sections
here should be added in the shape they'd want to survive that pass.

Because the fork adds engine capability that stock GZDoom lacks, this is also
where the fork's ZScript-facing surface gets exercised first — if a new engine
ability has no control here, it effectively doesn't exist to the player.

---

## Glow

Rebuilt in ZScript from **GlowInTheDark 1.1** (2021), which drove sector glow
from an ACS script. That script counted `0..99999` and fed those numbers to
`SetSectorGlow`, whose first argument is a sector **tag**, not an index — so it
only ever reached rooms a mapper happened to tag. In most maps, almost none of
them. ZScript walks `Level.Sectors` directly, so this reaches every room.

Also fixed on the way across: the original declared a `height` cvar, never wired
it, and passed a hardcoded `64`. Reach is a real control now. Changes apply live
instead of needing a map restart.

## In-world panels

The second tenant. Camera-facing cards drawn in the world as real depth-tested
geometry — the first use is the stat card beside a dropped weapon.

Two controls, both reading cvars the **engine** declares rather than this mod's
`CVARINFO`, because the cull that honours them runs engine-side per frame:

| cvar | control |
|---|---|
| `rs_bb_maxpanels` | how many cards may exist at once, `0` = unlimited |
| `rs_bb_cullradius` | how close you must be for a card to be built, `0` = no limit |

Neither caps storage. The engine's panel list is deliberately uncapped and both
knobs are applied when the frame gathers what to draw, so moving either takes
effect immediately with no respawn — and raising the distance costs nothing
until you are actually close enough to read a card.

That is also the point of the two-stage split: past the cull radius a drop still
emits its glow and its small light, and only the *card* is skipped. Presence and
panel are separate things, so there is no distance fade and no panel is held for
a drop whose card isn't being drawn.

**No off switch yet.** `0` means *unlimited* on both cvars, so neither can turn
cards off; that needs a master switch engine-side.

## What's here

| | |
|---|---|
| `zscript/gitd.zs` | the sweep, the authority rules, live re-apply |
| `CVARINFO` | settings |
| `MENUDEF` | Options → Radiance Control Panel, and the per-tenant sections under it |
| `MAPINFO` | handler registration — nothing runs without it |
| `GLDEFS` | the texture glow lists, `Flats` and `Walls` |
| `EDGE_GLOW_SPEC.md` | design of record for the engine-side work |

## The division

**The engine provides capability. This mod provides policy.** Which sectors, what
colour, how far, what outranks what — all decided here. The engine only supplies
abilities a mod cannot implement for itself.

Things ZScript genuinely cannot do, and which therefore live in the engine:

- read a texture's pixels, so it can never derive "what colour is this nukage"
- make a texture glow, or render a glow from a wall's own texture
- know how far a point on a floor is from the nearest wall

## Glow authority

Writing a colour onto a plane used to be one operation, and it outranked that
plane's texture. So a blanket sweep erased every colour `Glow { Flats { } }`
supplies — which is why nukage is green — and the only defence was guessing
which planes to skip.

The engine now separates the two cases and this mod chooses between them:

| call | meaning |
|---|---|
| `SetGlowColorAuto` | paint as a **fallback** — the flat's own glow wins |
| `SetGlowColor` | paint as a **choice** — we win |
| `IsGlowAuthored` | did this plane's colour come from a choice? |
| `GetTextureGlow` | what colour and reach does this flat bring itself? |

Resolution order, strongest first: an authored colour, then the plane texture's
own glow, then an auto-written colour, then nothing.

## Requires

A GZDoom build carrying those natives. Stock GZDoom does not have them, and a
call to a missing native is a hard script error that stops the whole mod
loading — not just the feature. Engine and mod ship as a pair.
