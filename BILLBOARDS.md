# Billboards

A billboard is a quad placed in the world with its own orientation, drawn as
real depth-tested geometry. It is occluded by walls, sorted against sprites,
and can be shot at, touched, or pointed at.

It exists so an in-world interface can be built natively: hundreds of panels
without an actor apiece, and a pointer that can ask what it is aimed at.

Nothing here is a HUD overlay. A billboard is in the world even when it is
welded to your view.

## The shape of one

| Field | Meaning |
| --- | --- |
| `pos` | world position — or an offset from the viewer, if view-locked |
| `width`, `height` | **full** extent in map units, not half |
| `yaw` | which way the face points |
| `tilt` | 0 is vertical; positive leans the top toward the viewer |
| `facing` | how yaw is decided — see below |
| `payload` | what it draws |
| `data` | payload-specific packed int |
| `color` | tint |
| `alpha` | 0 invisible, 1 opaque |
| `flags` | see below |
| `lifetime` | seconds; ignored when persistent or attached |

Extent is **full, not half**. A billboard 48 wide measures 48 edge to edge.
This is written down because the previous engine's single `size` was ambiguous,
and for a hinge it is the difference between wings that meet a centre panel
exactly and wings that overlap it or leave a seam.

## Facing is a mode, not the definition

```
0  BBF_FIXED       use my own yaw and tilt verbatim
1  BBF_CAMERAYAW   turn to the viewer, keep my tilt
2  BBF_CAMERA      turn to the viewer including tilt
```

A quad that *always* turns to the camera cannot be hinged to another at a fixed
angle: once both turn independently the angle between them stops meaning
anything, and a hinged assembly collapses into parallel planes. So a triptych
is one `CAMERAYAW` root with `FIXED` children hinged off it.

**Hinge solving stays in script.** The engine consumes the yaw and tilt it is
handed and never computes a hinge itself.

## Flags

```
1   persistent    lives until removed; lifetime ignored
2   attached      rides an actor, dies with it
4   no depth      draws over world geometry instead of being occluded
8   view-locked   pos becomes an offset from the viewer: X ahead, Y right, Z up
16  follow angle  attached only: yaw is measured from the actor's facing
```

`4` is what a HUD-locked panel needs — welded to the view, it would otherwise
be sliced in half whenever you back into a wall.

`8` has to be native. Script runs at tic rate and the view does not, so a
head-locked panel repositioned from script lags and snaps against head
movement. Resolved in the renderer instead, it stays welded.

`16` is the difference between an object that *has* a front and one that merely
travels with an actor. Four billboards at yaw 0/90/180/270 on one actor make a
box; without this flag the box shears into a plane as the actor turns. A floor
marker under a monster wants the flag off.

## Payloads

```
0  BB_PANEL     rounded-rect backing
1  BB_TEXTURE   any TextureID on the quad; data = TextureID.GetIndex()
2  BB_DIGITS    SDF digits
3  BB_GLYPH     SDF glyph
4  BB_RING      progress ring
5  BB_BAR       progress bar
```

**Only `BB_TEXTURE` draws.** The rest are enumerated but wait on their shaders.
A billboard with an undrawn payload is silently skipped.

The texture route is not a stopgap: point `data` at a canvas texture and paint
it from ZScript, and the billboard is whatever you painted.

## Script API

```
void AddBillboard          (Vector3 pos, double w, double h, double yaw, double tilt,
                            int facing, int payload, int data, color col,
                            int flags = 0, double lifetime = 0)
int  AddBillboardPersistent (... same ...)            returns a handle
int  AttachBillboard        (Actor mo, Vector3 offset, double w, double h,
                            double yaw, double tilt, int facing, int payload,
                            int data, color col, int flags = 0)   returns a handle

void MoveBillboard      (int id, Vector3 pos)
void OrientBillboard    (int id, double yaw, double tilt, int facing)
void ResizeBillboard    (int id, double w, double h)
void UpdateBillboard    (int id, int data, color col)
void SetBillboardAlpha  (int id, double alpha)
void RemoveBillboard    (int id)

int, Vector2         AimBillboard   (Vector3 start, Vector3 dir, double maxDist = 0)
int, Vector2, double TouchBillboard (Vector3 point, double maxRange = 0)
```

`AddBillboard` issues no handle. It is the cheapest form — use it for anything
you will never address again.

## Pointing at one

Both queries answer the same question: **which billboard, and where on its
face**, as a 0..1 UV — the same UV the shader sees. Both return 0 on a miss,
and neither can hit a transient, since a transient has no handle to report.

```
int hit; Vector2 uv;
[hit, uv] = level.AimBillboard(eyePos, aimDir);

int t; Vector2 tuv; double dist;
[t, tuv, dist] = level.TouchBillboard(handPos, 8);
```

Because a trigger, a tracked fingertip and the Use key all resolve to the same
answer, a panel handles a press without knowing which one it came from. Use is
the fallback when there is no tracking, and it costs nothing extra.

`TouchBillboard` also returns distance to the surface, which is what makes
touch feel like touch: drive a hover highlight as the hand approaches, fire the
press on contact. Two things worth handling in script — a hover band, or
buttons light up whenever a hand drifts near; and debouncing, or a hand resting
on a panel fires every tic.

## Lifetime

Three kinds, checked once per game tic in `FLevelLocals::TickBillboards`:

- **transient** — expires after `lifetime` seconds
- **persistent** — lives until `RemoveBillboard`
- **attached** — follows an actor, dies with it, ignores lifetime

Attachment holds a `TObjPtr`, which does not dangle across a GC sweep and does
not itself keep the actor alive. "Dies with its actor" therefore means exactly
that, and never the reverse: once the pointer resolves null the billboard is
dropped, and the actor is never revived to keep it company.

Attached billboards read their actor's **interpolated** position when drawn, so
they track at render rate rather than stepping at 35Hz.

## Save and load

All billboards travel, transients included. `maptime` is already saved, so a
transient's `spawntic` stays meaningful and it resumes its remaining lifetime
rather than restarting it. `attachedTo` rides the object table, so a pointer
whose actor did not survive loads as null and the next tic drops that billboard
exactly as it would have live.

`drawPos` is deliberately not saved — the first frame after a load rewrites it.

## Cvars

| Cvar | Default | What |
| --- | --- | --- |
| `bb_scale` | 1.0 | scales every billboard as drawn |
| `bb_tiltbias` | 0 | degrees added to every tilt |
| `bb_flipu` | false | flips which way a face runs |
| `rs_bb_maxpanels` | 0 | most drawn at once; 0 unlimited |
| `rs_bb_cullradius` | 0 | distance limit; 0 unlimited |

All are live — next frame, no reload.

Size and tilt are sliders because neither has a correct value: comfortable card
size and the tilt that makes a panel look upright depend on the person and the
headset.

`bb_flipu` is different in kind. It is not a preference but the settlement of a
handedness question, and the bug it fixes stays invisible until something with
text on it renders backwards. This project has been caught by exactly that
before. The basis vectors follow the convention the panel layer documents:

```
face  F = ( cos y,  sin y, 0)
right R = ( sin y, -cos y, 0)
up    U tilts toward -F, so positive tilt leans the top toward the viewer
```

The renderer, the aim ray and the touch test all use those same vectors. They
have to agree, or the pointer lands somewhere other than where the panel is.

Under a budget the nearest billboards win — the far ones are unreadable anyway,
so they are both the cheapest to drop and the least missed. View-locked
billboards are never culled: welded to the eye, their distance is meaningless,
and losing one to a budget would read as the interface vanishing.

## Where it lives

| File | What |
| --- | --- |
| `src/g_levellocals.h` | `FBillboard`, the enums, storage on the level |
| `src/p_tick.cpp` | `TickBillboards` — attachment, expiry |
| `src/scripting/vmthunks.cpp` | every native, including aim and touch |
| `wadsrc/static/zscript/doombase.zs` | the ZScript declarations |
| `src/rendering/hwrenderer/scene/hw_drawinfo.cpp` | `DispatchBillboards`, culling |
| `src/rendering/hwrenderer/scene/hw_sprites.cpp` | `ProcessBillboard`, the quad |
| `src/p_saveg.cpp` | serialization |

## Not done

**SDF payloads.** Five of the six payloads need shaders. The longer aim is
building whole objects out of SDFs, so this is not merely cosmetic.

**Collision.** Billboards are render-only. You fall through one. Doom's
collision model has no place for an arbitrary oriented quad, so this is a real
feature rather than a flag. Until then: 3D floors for anything walkable, or an
invisible solid actor with a billboard attached for looks — bearing in mind
actor collision is a cylinder, so a thin plank gets a chunky invisible box.
