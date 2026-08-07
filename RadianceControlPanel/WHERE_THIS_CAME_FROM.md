# Radiance Control Panel — the mod half

This folder is **not part of the engine build.** CMake globs only inside the
`wadsrc*` directories and otherwise adds named subdirectories, so nothing here
is compiled or packed into `qzdoom.exe`. It is a loadable mod that lives here
so the engine and its controls are in one repository instead of three.

## What it is

The engine adds capabilities and declares their cvars in C++. It adds **no
menus at all**. This is where those cvars become something a player can reach
without opening the console:

| submenu | drives | declared in |
|---|---|---|
| Glow in the Dark | `gitd_*` (this mod's own) | `CVARINFO` here |
| Glowing Wall Textures | `gl_texture_wallglow`, `..._intensity` | engine, `hw_walls.cpp` |
| Edge Glow on Floors | the 16 `gl_flatglow_*` | engine, `hw_flats.cpp` |
| In-World Panels | `rs_bb_cullradius`, `rs_bb_maxpanels` | engine, `g_level.cpp` |

**Engine cvars are never redeclared in `CVARINFO`.** MENUDEF reaches them by
name. A mod-side mirror of a native cvar collides with it, and two cvars for
one setting is how they drift apart. `CVARINFO` carries a note at each spot so
the absence reads as a decision rather than an oversight.

## Why it is here

The same content was previously pushed to two standalone repos —
`RadianceControlPanel` (private) and `GlowInTheDark_V5` (public) — created from
one instruction given separately to two sessions. Keeping the engine, its
porting documentation and its controls in one place removes the question of
which copy is current. **If either standalone repo still exists, this folder is
the one to trust.**

## Relationship to PORTING.md

`PORTING.md` at the repo root documents every C++ change in this fork so
another GZDoom/QZDoom fork can implement it. It deliberately does **not** cover
this folder, because a porting fork will have its own menu conventions. What it
does say is the part that matters: the engine ships no menus, so anyone porting
a feature must supply their own way to reach its cvars, or it is
console-only — which for most players means it does not exist.

## Status

**Unvalidated.** MENUDEF is parsed at runtime, so the two newer submenus
(Glowing Wall Textures, Edge Glow on Floors) have never been loaded. They are
written against verified signatures — `Slider`'s sixth argument is decimal
places, and `Option` works on a float cvar because `GetSelection` compares
`GetFloat()` approximately — but reading the parser is not the same as opening
the menu. Boot it and look before trusting it.
