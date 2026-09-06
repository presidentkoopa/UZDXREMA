# HUD stereo gating

Fixed 2026-08-08 in `src/d_main.cpp`.

**Symptom:** on a flat (non-VR) desktop session, the entire 2D layer
disappears about a second after a level starts — the status bar, the view
border, and every mod `RenderOverlay` handler, all at once. It is present on
the title screen and reappears the moment a menu or the console opens.

**Cause:** `vr_hud_mount` and `vr_automap_mount` default **true** and are
`CVAR_ARCHIVE | CVAR_GLOBALCONFIG` (`hw_vrmodes.cpp:758`), so they are true on
a fresh install and stay true. The mounted-HUD conditions did not check
whether a stereo mode was actually **running**:

```cpp
// before
const bool drawMountedHud = !menuactive && ConsoleState == c_up
                            && !automapactive && (portableHud || vr_hud_mount);
const bool drawFaceHud    = (!drawMountedHud && !drawMountedMap) || drawFaceMap;
```

With `vr_mode 0`, `drawMountedHud` came out **true**, so `drawFaceHud` came
out **false**, and the whole block below it was skipped.

The HUD was still being rendered — `DrawHudToSurface()` drew it into the
offscreen VR surface — but that surface is only composited by the two stereo
devices (`vk_openxrdevice.cpp:1498/4406`, `gl_openvr.cpp:1508/1521`). With no
stereo mode running it went to a texture nothing ever displays.

**Why it took everything at once, not just the status bar.**
`DBaseStatusBar::DrawTopStuff` dispatches `RenderOverlay`
(`shared_sbar.cpp:1240`), and that call sits inside the same gate. So one
`false` removed the vanilla bar *and* every mod overlay simultaneously, which
made it read as a mod bug rather than an engine one.

**Why it looked like a delay.** The level-start screen wipe presents the
pre-wipe frame — captured while the menu was still up, HUD included — melting
into the new one. When the wipe ends and live frames resume with
`menuactive == false`, the HUD goes. That reads as "it vanished after about a
second".

**The tell.** It came back whenever a menu or the console opened, because
`menuactive` / `ConsoleState` flip the same expression. Nothing else produces
that.

## The fix

Gate the mounted paths on an active stereo mode. `vrmode->IsVR()` is the check
this block **already** used for its own debug border ~45 lines below, so the
mounted paths now agree with it instead of contradicting it.

```cpp
// after
const bool stereoActive   = vrmode->IsVR();
const bool drawMountedHud = stereoActive && !menuactive && ConsoleState == c_up
                            && !automapactive && (portableHud || vr_hud_mount);
const bool drawMountedMap = stereoActive && !menuactive && ConsoleState == c_up
                            && automapactive && (portableHud || vr_automap_mount);
```

A second site had the identical omission — the chat/message drawer at
`d_main.cpp:1482`, which skipped `CT_Drawer()` during normal flat play for the
same reason. Same guard applied.

## Behaviour

| Mode | Before | After |
| --- | --- | --- |
| VR (`vr_mode` non-zero) | mounted HUD on the VR surface | unchanged |
| Flat (`vr_mode 0`) | no HUD at all during play | HUD draws normally |

No cvar defaults were changed, and nothing needs setting per-user. A player
running flat no longer has to know `vr_hud_mount` exists.

## Still worth a look

`DrawHudToSurface()` (`d_main.cpp:1000`) still runs in flat mode and draws
into a surface nothing composites — correctness is fine now, but it is wasted
work every frame. Left alone deliberately: it is a performance question, not a
bug, and it touches the portable-HUD cooldown logic.
