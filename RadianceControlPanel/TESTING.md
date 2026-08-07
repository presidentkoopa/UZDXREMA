# Testing the glow natives

What to check on the first integrated build, and how to tell a real defect from
a false alarm. Written by the lane that built the natives; keep it with them.

Best map: **Doom 2 MAP03** — it has nukage. MAP01 has very little.

---

## 0. Check this before believing any defect

**A stale `qzdoom.pk3` looks exactly like a broken export.** Three of the four
natives are declared in `wadsrc/static/zscript/`, which is packed into the pk3 at
build time. If the exe rebuilds and the pk3 doesn't, the C++ registers fine, the
engine boots fine, and then **the mod fails to compile** with
`Unknown identifier 'SetGlowColorAuto'`.

That is a real error message from a build that is fine — the same shape as the
missing-`zmusic.dll` incident. **Confirm the pk3 timestamp before concluding
anything.**

Declaration sites, if the packed pk3 needs checking directly:
- `wadsrc/static/zscript/mapdata.zs:545-556` — the three `Sector` methods
- `wadsrc/static/zscript/engine/base.zs:325` — `TexMan.GetAverageColor`

---

## 1. Hard startup failures — loud

A script-error window naming any of the four natives. Cause is a name mismatch
between the two sides; they must match exactly:

| ZScript | C++ registration |
|---|---|
| `Sector.SetGlowColorAuto` | `DEFINE_ACTION_FUNCTION_NATIVE(_Sector, SetGlowColorAuto, SetGlowColorAuto)` |
| `Sector.IsGlowAuthored` | `(_Sector, IsGlowAuthored, IsGlowAuthored)` |
| `Sector.GetTextureGlow` | `(_Sector, GetTextureGlow, GetTextureGlow)` |
| `TexMan.GetAverageColor` | `(_TexMan, GetAverageColor, GetAverageColor)` |

**Parameter types are already ruled out.** `DirectNativeDesc`'s constructor
(`vm.h:634`) runs `ValidateRet` / `ValidateType` as template instantiation at each
DEFINE site, so an illegal type is a C++ compile error. Both files compiled clean.
Don't look there.

---

## 2. Silent wrong values — the reason this document exists

Argument **count** and **return type** are not cross-checked in a release build.
A mismatch doesn't error, it returns garbage.

This is not hypothetical. `Sector.GetGlowColor` had exactly this bug: the direct
native returned `double` while ZScript declared `color`, so the caller read an
integer register the callee never wrote. Upstream, live here, and it was
corrupting this mod's restore path.

### The diagnostic: toggle `vm_jit`

The interpreter calls the `DEFINE_ACTION_FUNCTION` body. The JIT calls the direct
native instead (`vmframe.cpp:298` gates it, `jit_call.cpp:57` prefers the direct
native, `jit_call.cpp:362` builds the signature from the **ZScript** prototype —
which is why a mismatched C++ return type goes unnoticed).

> **Correct with `vm_jit 0`, wrong with `vm_jit 1` = direct-native signature
> mismatch.** One toggle isolates the entire bug class.

---

## 3. The four checks

**(a) `SetGlowColorAuto` — the point of the whole lane**
Glow on, `gitd_respect_textures 1` → nukage stays green. Set `0` → nukage takes
the painted colour. Back to `1` → green returns.
*Fails as:* nukage grey/painted with the cvar at 1 — the auto write is claiming
authority. Look at the flag or the precedence order in `ResolvePlaneGlow`.

**(b) `GetAverageColor` — `gitd_colour_source 2`**
Every flat takes a plausible colour from its own artwork: brown floors brown,
blue-grey computer flats blue-grey.
*Fails as:* everything near-identical mid-grey → `normalize` is arriving as **0**
instead of its declared default 153, so you're getting a plain average instead of
a normalised hue. **This is the subtle one — grey but plausible reads as
working.**
*Fails as:* everything black or wildly wrong → return wiring.

**(c) `GetTextureGlow` — `gitd_unify_reach 1`, on nukage**
Nukage keeps its green but the reach follows the Glow reach slider. This is the
only path consuming **both** return values, so it's the multi-return check.
*Fails as:* reach never changes (second return always 0), or colour is nonsense
(returns swapped).

**(d) `IsGlowAuthored` — restore fidelity**
Toggle `gitd_enabled` off and on several times. The map must land in the same
state every cycle.
*Fails as:* progressive drift — authority isn't round-tripping and planes are
being restored through the wrong setter.

---

## Risk order

1. **`GetAverageColor`** — defaulted parameter, and its failure mode looks
   plausible rather than broken.
2. **`GetTextureGlow`** — multi-return, most moving parts.
3. **`IsGlowAuthored`** — `int`-for-`bool`, but an exact clone of the
   `PlaneMoving` pattern at `vmthunks.cpp:412`. Low risk.
4. **`SetGlowColorAuto`** — lowest. Line-for-line clone of the `SetGlowColor`
   pair directly above it.

## Regression check

Existing ACS `SetSectorGlow` content and UDMF `floorglowcolor` maps must behave
**exactly** as before. The flag polarity was deliberately inverted so that they
would — a regression here shows as mapper-set glow losing to a glowing texture.
