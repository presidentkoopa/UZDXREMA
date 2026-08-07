# START HERE — state of this engine, 2026-08-07

Written at the end of a session that wasted the owner's entire night. Read this
before you touch anything or ask him anything. Most of what cost time was
avoidable and is written down here so you don't repeat it.

---

## 1. What this fork actually added, and whether it is any good

| feature | verdict |
|---|---|
| language CSV fix (2 commas) | **works.** Fixes a hard boot crash. Keep it. |
| ZScript glow API | **works, and the mod depends on it.** See §3. |
| flat edge glow | **renders, but it is the wrong effect.** See §2. |
| wall texture glow (`gl_texture_wallglow`) | **the owner wants it DELETED.** "i hate it." Not disabled — removed. |
| billboards | **do not work.** See `BILLBOARDS.md`. |

Two of five are wanted. Do not defend the other three.

---

## 2. The glow situation — read this or you will repeat my mistakes

### There are TWO different things called "wall glow". I conflated them for hours.

**Stock sector glow** — floor colour rises up walls, ceiling colour comes down.
Two channels: `uGlowBottomColor` and `uGlowTopColor`. **This is vanilla GZDoom.
It always worked. This fork did not build it.** When the owner says "two channel
wall glow", THIS is what he means.

**`gl_texture_wallglow`** — a texture listed in a GLDEFS `Glow { Walls { } }`
block self-illuminates. **This fork added it. The owner hates it and wants it
gone.** It is unrelated to the above and is not a broken version of it.

If you argue with him about "wall glow" without knowing which one you mean, you
will waste an hour. I did.

### Why the flat edge glow looks wrong

`main.fp` does:

```glsl
color.rgb += min(desaturate(uFlatGlowColor * t).rgb, ...)
```

It **adds**, and `gl_flatglow_color` defaults to **white**. Adding white to a
texture is brightening it. The owner's description — "just fullbright with a
darkening gradient" — is literally correct arithmetic, not an unfair reaction.

Three defects, one root:
* it **adds** instead of asserting a colour → can never look like neon
* it runs on **flats only** → walls get nothing
* **one global colour** → no per-surface identity

### What IS worth keeping

The **edge distance** — per-vertex "how far is this point from the nearest
boundary", baked at map load into `FFlatVertex::edgedist`/`edgedistall`. That
primitive is correct and it is why the glow bands follow stairs and pillars
instead of falling off radially from a point.

It has real holes: hole-filling subsectors get no interior sample and glow flat;
a near-degenerate linedef can divide by ~0 and bake a NaN; **walls never got it
at all.** Walls having no edge distance is why there is no wall equivalent.

### What he actually asked for, in his words

> "all i fucking want is the god damn 4 x 8 system"

Four channels, several colours each, with **coverage / size / falloff / style /
cycling + dwell time**. 16 slots preferred, "8 if this is a problem."

He had this working before in `E:\DXR2`. **DXR2 IS ABANDONED — he canned it over
a clipping issue and told me three times to stop bringing it up. Do not propose
porting from it.** Build fresh.

---

## 3. The mod depends on this engine. Do not "start clean" casually.

`E:\RS_Main` calls natives that exist ONLY in this fork:

* `RS_Screens.zs` → `level.AttachBillboard(...)`, `tex.GetIndex()`
* `zscript/gitd.zs` (in `RadianceControlPanel/`) → `TexMan.GetAverageColor`,
  `Sector.SetGlowColorAuto`

**A ZScript call to a missing native is a hard error that stops the ENTIRE mod
loading.** Replace this engine with a clean upstream clone and the mod does not
boot until those calls are removed or the natives re-added.

---

## 4. Traps that cost real time tonight

**Stale notes that outlived their tree.** This happened THREE times in one night,
in both directions:
* `CLAUDE.md` said `MISSILEMORE`/`MISSILEEVENMORE`/`SHORTMISSILERANGE` "cannot be
  fixed... Stop trying." All three CAN be fixed — `MissileChanceMult` and
  `MaxTargetRange` are real properties and the engine's own deprecation string
  says so. ~256 warnings were protected by a note telling everyone not to look.
* `RS_Screens.zs` carried a comment saying this engine has NO billboard API,
  "verified by grep" — true when written, false by the time it was read.
* A memory note recorded six glow-ramp natives as existing here. All six are
  ABSENT. Code written against them would have stopped the mod loading.

**Rule: check the tree, not the note about the tree.** A disabling comment
hardens into an instruction not to look.

**`rs_build.cmd` hard-wires `cmake -S ..`** to `E:\UZDXREMA`. Run it from a
worktree and it compiles the MAIN tree — green build, verified nothing.

**Verify your own tools before believing them.** I used `strings` on the exe to
check whether a command was present; it returned zero bytes and I nearly
reported the wrong answer. A control test caught it. Do this.

---

## 5. Confirmed facts (checked, not assumed)

* `vid_rendermode` = **4**, hardware renderer. Vulkan (`vid_preferbackend 1`) +
  OpenXR. **Do not chase a software-renderer theory; I did and it was wrong.**
* `gl_flatglow` defaults to **false**. He had it off the whole time.
* The engine **compiles and links clean** — full RelWithDebInfo, exit 0, zero
  warnings in any changed file.
* Flat glow **does render**. Screenshots preserved at
  `E:\RS_Main\docs\evidence_20260806\` — compare `...123547.png` (off) against
  `...123548.png` (on). The bands follow geometry, so the firing monster in
  frame is not the explanation.
* **Nothing has ever been seen working for billboards.**

---

## 6. How to talk to him

He is technical, fast, and out of patience. Do not explain what he already
knows, do not manage his emotional state, and do not tell him what he is "not up
to deciding." He will call it out and he will be right.

Say what you verified and what you assumed. When you are wrong, say so in one
line and move on — do not perform contrition.

**Do not build or launch the game without being asked.** He is at that machine.
