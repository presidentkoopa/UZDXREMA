# UZDXREMA — a QZDoom/GZDoom fork with engine changes
--upcoming major features: gravity manipulation, portal stacking--
**This repository is a fork of [`emawind84/gzdoom`](https://github.com/emawind84/gzdoom)
(branch `questzdoom`) carrying renderer and scripting changes to the engine
itself.** It is not a mod, a texture pack, or a config. The C++ is modified.

## If you only want the engine changes — read [`PORTING.md`](PORTING.md)

**[`PORTING.md`](PORTING.md) documents every C++ change in this fork so another
GZDoom/QZDoom fork can implement them without reverse-engineering a diff.**
That is what it is for. It is written for a stranger, not for us.

It covers, per feature rather than per commit:

* **what it does**, in two sentences;
* **every file touched**, with function names and line ranges, plus a manifest
  of all 45 changed files mapped to the feature that owns each;
* **why** — especially where the obvious approach was rejected, which is the
  part no diff can tell you;
* **the order things must be applied in**, because some of it will not compile
  otherwise;
* **new cvars, new savegame keys**, and which direction they break;
* **what will conflict** with a fork that has already diverged.

The five features are: a two-character fix for a hard boot crash in upstream's
language data; wall texture glow (GLDEFS `Glow { Walls { } }` was parsed and
never consumed by any renderer); edge glow on floors and ceilings; a ZScript
glow-authority API; and in-world billboard panels.

### Read the condition notes before budgeting time

`PORTING.md` marks what is unproven, and a lot of it is. Some features are
finished and on by default. Others compile, link, and have never been looked at
on a screen. The billboard feature in particular is **partial** — only one of
its six payload types renders anything, and panels cannot be rotated. All of
that is stated where it applies rather than buried, because a porting document
that oversells what works is worse than none.

## Layout

| path | what |
|---|---|
| [`PORTING.md`](PORTING.md) | **the engine changes, for other forks** |
| [`ENGINE_WORK.md`](ENGINE_WORK.md) | earlier status notes, corrected in place; `PORTING.md` wins where they disagree |
| `src/`, `wadsrc/` | the engine, as upstream lays it out |
| `RadianceControlPanel/` | the companion mod that gives the new cvars a menu — **not built into the exe** |

The engine ships **no menus** for anything it adds. Every new cvar is
console-only until something supplies a menu, which is what
`RadianceControlPanel/` is for. If you port a feature, plan to supply your own.

## Licence

**GPL v3**, unchanged from upstream — see below. This fork adds no licence
terms and removes none. If you take code from here, the same terms apply.

---

# Upstream README (QuestZDoom / GZDoom)

*Everything below is upstream's and is preserved for attribution and licensing.*

# QuestZDoom fork of LZDoom for Oculus Quest VR port!

[![Build Status](https://github.com/emawind84/gzdoom/actions/workflows/continuous_integration.yml/badge.svg?branch=questzdoom)](https://github.com/emawind84/gzdoom/actions/workflows/continuous_integration.yml)

Built/tested on HP Reverb and Oculus Quest using Virtual Desktop but other VR setups should work as long they are compatible with OpenVR API.

This build exposes OpenVR controller input for definition (you will need to define the controls).
One hand (right by default) is tracked for the weapon. I have included two modified weapon packs authored by Fishbiter. 

## Controller Info

### Index Controllers
To get the most out of your Index Controllers, choose the Community Binding "Index Controller Bindings" by gameflorist in SteamVR. It makes the maximum buttons available for binding in GZDoom.

## Mods
There are some optional mods tested for using with this fork

https://github.com/hh79/gz3doom/files/4378108/HDVRweapons.zip HD weapon pack made by ajantaju

https://github.com/ajantaju/br_vr weapon pack made for Brutal Doom

https://github.com/dxt121730/BD64Weapons weapon pack for Brutal Doom 64 by dxt121730

https://github.com/mmaulwurff/laser-sight/releases laser sight mod, you need this for aiming

https://github.com/iAmErmac/Virtual-Tactical-Vest adds a virtual vest with weapon slots by iAmErmac

https://www.moddb.com/downloads/doom-neural-upscale-2x texture upscale mod

https://forum.zdoom.org/download/file.php?id=30459&sid=df63736751c12c3ebb76230d1dc86543 blood color fixer mod

#
Copyright (c) 1998-2023 ZDoom + GZDoom teams, and contributors

Doom Source (c) 1997 id Software, Raven Software, and contributors

Please see license files for individual contributor licenses

### Licensed under the GPL v3
##### https://www.gnu.org/licenses/quick-guide-gplv3.en.html
---



# Resources
- https://zdoom.org/ - Home Page
- https://forum.zdoom.org/ - Forum
- https://zdoom.org/wiki/ - Wiki
- https://dsc.gg/zdoom - Discord Server
- https://docs.google.com/spreadsheets/d/1pvwXEgytkor9SClCiDn4j5AH7FedyXS-ocCbsuQIXDU/edit?usp=sharing - Translation sheet (Google Docs)

Credits
-------

* [The ZDoom Teams](https://zdoom.org/index) - The team behind the engine this based upon.
* [Emile Belanger](http://www.beloko.com/) - The developer behind the android porting.
* [DrBeef & Teams](https://www.questzdoom.com) - For the awesome work behind the VR port for the Oculus Quest device
