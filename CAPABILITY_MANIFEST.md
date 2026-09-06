# What UZDXREMA adds to UZDoom 5.0.0

UZDXREMA (shipped as **DoomXR**) is a virtual-reality fork of UZDoom. Upstream UZDoom
contains no VR code at all -- its stereo modes are flat-screen tricks, its `VRMode` is a
struct of eye-shift factors, and it has never had a headset, a tracked hand or a
controller pose. This file is the difference.

Baseline: **UZDoom 5.0.0** (tag `5.0.0`). Generated from `git diff 5.0.0..HEAD`.

| | |
|---|---|
| capabilities catalogued | 185 |
| files diverged from upstream | 457 |
| lines added | 91,124 |
| files that exist only here | 120 |

**Tags.** `NOT IN UZD` = upstream has nothing resembling it. `REPLACES` = the fork tore
out an upstream system. `EXTENDS` = an upstream system was given new capability.
Lifecycle is `active` (on by default or in normal use), `dormant` (built and reachable,
behind a default-off switch), `in flight` (uncommitted right now) or `legacy`
(superseded or not compiled for the current target, carried as history).

Because this engine is played in a headset with no keyboard, every entry records how it
is reached. An entry reachable only from the console is effectively unreachable in play.

---

## 01. OpenXR runtime and stereo pipeline

> Upstream UZDoom 5.0.0 has no VR device code at all — its `VRMode` is a plain value struct that only picks anaglyph/side-by-side eye offsets for a flat screen. UZDXREMA replaces that with a real head-mounted-display runtime: a full OpenXR 1.0 session on Vulkan (`VKOpenXRDeviceMode`, ~6200 lines, the largest fork file), a dynamically loaded `openxr_loader`, an OpenXR-directed Vulkan instance/device bootstrap, an array swapchain with an optional Vulkan multiview path, per-eye asymmetric-frustum projections taken from the runtime's own FOV, and a composition-layer stack (projection layer plus quad layers for menus, a backdrop and a menu pointer beam) submitted every frame through `xrWaitFrame`/`xrBeginFrame`/`xrEndFrame`. To make that possible the whole `VRMode`/`VREyeInfo` interface was turned into a polymorphic device abstraction with an XR frame lifecycle that the Vulkan renderer drives....

### Native OpenXR session on Vulkan (VKOpenXRDeviceMode)
`[NOT IN UZD]` `[active]`

A complete OpenXR 1.0 device mode: creates the XrInstance, gets the HMD system, binds the game's existing Vulkan instance/device/queue via XR_KHR_vulkan_enable(2), creates a reference space, builds an action set with hand poses, buttons, sticks and haptics, enumerates the stereo view configuration, and runs the xrWaitFrame/xrBeginFrame/xrLocateViews/xrEndFrame loop every frame. It handles session-state events (READY -> xrBeginSession, STOPPING ->...

**Why.** Upstream UZDoom has no HMD runtime whatsoever - its stereo modes are screen tricks. Without this there is no headset, no tracking, and no compositor submission; every other VR feature in the fork hangs off this class.

**Reached by.** Menu (indirect, and hidden in the configuration that actually needs it): Options > Display Options > OpenGL Renderer > "VR/3D mode" > "3D mode" = OpenXR. Chain: menudef.txt:403 Submenu "VideoOptions" -> :1375 Submenu OpenGLOptions -> :2487 Submenu VR3DMenu -> :2544 `Option "$GLMNU_3DMODE", vr_mode, VRMode`, with 15 = "$OPTVAL_OPENXR" in the VRMode...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### VRMode turned into a polymorphic XR device interface
`[REPLACES]` `[active]`

Rewrites `VRMode` and `VREyeInfo` from plain data structs into virtual base classes and adds an XR frame lifecycle to the interface: SetUp/TearDown, PollXREvents, BeginXRFrame, AcquireXRSwapchain, SubmitFrame, FinalizeEyeImage, RenderDesktopMirror, GetRecommendedRenderSize, ShouldUseMultiviewThisFrame, GetHandTransform, Vibrate, GetTeleportLocation and per-eye SetUp/AdjustHud/AdjustBlend/AdjustViewpointUniforms. The Vulkan renderer calls these hooks by...

**Why.** Stock UZDoom's VRMode holds `VREyeInfo mEyes[2]` by value with no virtuals, so a device can neither override projection per eye nor get called at frame boundaries. A real runtime needs to be told when a frame starts, when each eye image is finished, and when to submit - none of which upstream has a place for.

**Reached by.** Internal C++ interface with no direct user surface — the hooks are dispatched every frame from VulkanRenderDevice::BeginFrame (src/common/rendering/vulkan/system/vk_renderdevice.cpp:1070, calling SetUp/BeginXRFrame/ShouldUseRecommendedRenderSizeThisFrame/GetRecommendedRenderSize/ShouldUseMultiviewThisFrame) and ::Update (line 382, calling per-eye...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/common/rendering/vulkan/system/vk_renderdevice.cpp`, `src/common/rendering/vulkan/textures/vk_framebuffer.cpp`

### OpenXR-directed Vulkan instance and device bootstrap
`[NOT IN UZD]` `[active]`

Before any Vulkan object exists, a throwaway XrInstance is created to ask the runtime three things: which Vulkan instance extensions it requires, which device extensions it requires, and which physical device the headset is actually attached to. The Win32 Vulkan video backend feeds the instance extensions and the runtime's min/max supported Vulkan API version into VulkanInstanceBuilder; VulkanRenderDevice feeds the device extensions and pins the physical...

**Why.** OpenXR must share the application's Vulkan device, and on a multi-GPU machine the compositor will reject a session created on the wrong adapter. Upstream picks a GPU with no knowledge that a headset exists, so without this the session creation fails outright or the game renders on the iGPU while the headset sits on the discrete card.

**Reached by.** No user surface of its own. It runs automatically during video startup only when both conditions hold: the Vulkan backend is in use (VR_OPENXR_MOBILE is Vulkan-only; hw_vrmodes.cpp:1220 falls back on GL) and vr_mode == 15. vr_mode is CVAR_ARCHIVE|CVAR_GLOBALCONFIG with default 0, set from the menu at Options -> VR/3D ("VR3DMenu",...

**Files.** `src/common/rendering/stereo3d/openxr/oxr_loader.cpp`, `src/common/rendering/stereo3d/openxr/oxr_loader.h`, `src/common/platform/win32/win32vulkanvideo.h`, `src/common/rendering/vulkan/system/vk_renderdevice.cpp`

### Dynamically loaded OpenXR loader with graceful degradation
`[NOT IN UZD]` `[active]`

Every OpenXR entry point is resolved at runtime from openxr_loader.dll (tried first next to the executable via $PROGDIR, then on the system path) through an FModule with required (TReqProc) and optional (TOptProc) proc tables generated from a single X-macro list. If the DLL is absent, if the runtime advertises neither XR_KHR_vulkan_enable nor XR_KHR_vulkan_enable2, or if initialisation fails at any step, the fork logs the reason and silently resets...

**Why.** The binary has to start on a machine with no headset and no OpenXR runtime installed - a hard link against the loader would make that a launch failure. The per-run fallback matters specifically because vr_mode is an archived cvar: without it, one failed session would leave the player stuck in a mode that cannot start.

**Reached by.** Automatic, but only once OpenXR mode is selected. The entire probe is gated on vr_mode == VR_OPENXR_MOBILE (15); vr_mode defaults to 0, so on a stock config the DLL is never searched and nothing is logged. vr_mode is set from the menu: Options > OpenGL/Display Options > "$GLPREFMNU_VRMODE" (OptionMenu "VR3DMenu", menudef.txt:2541) > "3D mode" (OptionValue...

**Files.** `src/common/rendering/stereo3d/openxr/oxr_loader.cpp`, `src/common/rendering/stereo3d/openxr/oxr_procs.h`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/CMakeLists.txt`

### Per-eye asymmetric-frustum projection and runtime-sized render targets
`[NOT IN UZD]` `[active]`

Each eye's projection matrix is built directly from the XrFovf the runtime reports for that view (four independent tangent half-angles, off-centre), not from a symmetric fov/aspect pair, and is rebuilt every frame in VKOpenXRDeviceEyePose::SetUp from the freshly located views. GetRenderFov reports the true per-eye FOV back to the culling code, GetViewShift applies the IPD separation plus the HMD-vs-player height difference, and GetRecommendedRenderSize...

**Why.** HMD lenses have asymmetric, per-eye frustums; feeding a symmetric matrix produces the wrong shape at the edges and a visible mismatch between the eyes. And rendering at desktop resolution then stretching to the headset wastes pixels in the centre and loses them at the periphery, which is exactly where a VR compositor resamples.

**Reached by.** Automatic every frame for OpenXR gameplay eye rendering; not gated by any cvar. Tuning sliders exist but BOTH are hidden unless the `developer` cvar is >= 1: Options > Developer Only ("OpenXR Render Scale" vr_openxr_render_scale 0.25-4.0 default 1.0, and "OpenXR FOV Adjust" vr_openxr_fov_adjust_deg -30..30 default 0.0; menudef.txt:2130/2133, whole submenu...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/vulkan/system/vk_renderdevice.cpp`, `wadsrc/static/menudef.txt`

### Array swapchain and Vulkan multiview stereo path
`[NOT IN UZD]` `[dormant]`

The XR colour swapchain is created as a texture array with one layer per view (arraySize = xrViewCount) and a per-layer image view for each slice, so both eyes live in one image the compositor samples by array index. On top of that, InitializeMultiview probes the physical device for VK_KHR_multiview (or core Vulkan 1.1), the multiview feature bit and maxMultiviewViewCount, and publishes a layer count and a view mask. When enabled, the renderer allocates...

**Why.** Rendering both eyes as two full sequential passes doubles the CPU-side draw submission for a scene that is 95% identical. Multiview lets one draw call write both array layers, and the array swapchain lets both eyes be submitted as one image - the standard way modern VR runtimes want to be fed, which stock UZDoom has no concept of.

**Reached by.** Menu toggle "VR Multiview" (`vr_openxr_multiview`, default false) appears in THREE menus: `OptionMenu "vkoptions"` (Vulkan options, menudef.txt:2659), `OptionMenu QuickMenu` (menudef.txt:4406), and `OptionMenu VRPerfTweakMenu` ("Performance Tweak", menudef.txt:4502). The two companions, "Desktop Mirror Reuse Multiview Frames"...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/vulkan/system/vk_renderdevice.cpp`, `src/common/rendering/vulkan/renderer/vk_postprocess.cpp`, `src/common/rendering/hwrenderer/data/hw_viewpointbuffer.cpp`, `wadsrc/static/menudef.txt`

### Composition-layer submission (projection + quad overlays)
`[NOT IN UZD]` `[active]`

Each frame ends with up to four composition layers submitted in one xrEndFrame: the stereo projection layer always first, then optionally a backdrop quad, the virtual-screen quad carrying menus/console/intermission, and a menu-pointer beam quad. The eye images are blitted from per-eye present textures into the array swapchain layers with explicit image barriers, an XR fence wait, and every failure path (acquire failure, 20 ms wait timeout, missing...

**Why.** Menus and the console cannot be drawn into a head-locked 3D world without making the player sick, and drawing them as world geometry means they inherit the scene's resolution and depth. Handing them to the compositor as their own quad layer keeps them sharp and stable. Keeping the projection layer alive alongside them - documented in the...

**Reached by.** Automatic once the engine is running in OpenXR mode; no cvar gates the submission itself. Precondition: vr_mode = 15 (OpenXR, Vulkan backend), set from the stereo-3D-mode option in the display menu ("$GLMNU_3DMODE", menudef.txt:2544). vr_mode's own default is 0 (mono), so nothing here runs until the user has picked OpenXR once — it is then archived to the...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### Head pose, prediction and yaw ownership
`[NOT IN UZD]` `[active]`

Views are located at xrFrameState.predictedDisplayTime, the two eye poses are averaged into a head position, and the orientation is taken from a quaternion-averaged centred view. The HMD's yaw delta between frames is fed into G_AddViewAngle so the headset - not the mouse - owns the player's facing, while pitch and roll go straight into the render viewpoint. Roomscale translation is rotated into world space and applied as positional movement (and forwarded...

**Why.** A headset's orientation has to become the game's view angle without fighting the engine's own angle handling, and roomscale steps have to move the player pawn rather than just the camera. The stage/local fallback matters because not every runtime or guardian setup exposes a floor-relative space, and getting it wrong puts the player's...

**Reached by.** Automatic — no gate of its own; runs whenever the OpenXR device mode is the active vr_mode (Options > 3D mode). Calibration is menu-reachable: vr_vunits_per_meter and vr_height_adjust are sliders in the VR preferences menu (wadsrc/static/menudef.txt:2047-2048), vr_ipd in the 3D-mode menu (:2548). The vr_posestats diagnostic is CONSOLE-ONLY — it appears...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`

### Interaction-profile binding table for five controller families
`[NOT IN UZD]` `[active]`

Suggested bindings are built and submitted for five OpenXR interaction profiles - KHR simple, HTC Vive, Oculus Touch, Valve Index and WMR - each with its own path set (trigger click vs value, squeeze click vs value, trackpad vs thumbstick). Hands are placed from /input/aim/pose; a parallel grip-pose action and action space are created purely so the aim-vs-grip offset can be measured. Capacitive touch (thumbrest, thumbstick, X/Y/A/B and trigger /touch...

**Why.** A fork played on real hardware has to work across whatever controller the player owns, and OpenXR's binding model is all-or-nothing per profile - one wrong path silently kills every input on that controller. Capacitive touch reports where a finger is resting rather than what it pressed, which is what hand poses need.

**Reached by.** Automatic at OpenXR session creation; no cvar, menu or bind gates it. Consequence stands: thumb/trigger capacitive-touch data reads false on every controller except Oculus/Meta Touch, since those paths are suggested to that profile only.

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h`

### Split headset/desktop present with XR-only colour bias
`[EXTENDS]` `[active]`

Each eye is finalised twice when needed: once into a present texture with an OpenXR-specific gamma/contrast/brightness/saturation bias applied on top of the normal vid_* grading, and once (optionally) into a separate unbiased mirror texture for the desktop window. RenderDesktopMirror then blits side-by-side or a single chosen eye into the swapchain image, with the virtual-screen and backdrop quads composited over it so the desktop viewer sees the menus...

**Why.** The headset compositor's own transfer function makes the submitted image read brighter and flatter than the same pixels on a monitor, so one grading cannot serve both. Keeping a separate unbiased path means correcting the headset does not wreck the desktop mirror or a screenshot.

**Reached by.** Menu: Options -> VR Preferences (VROptionsMenu, menudef.txt:2026). Under a "Display Adjust" header shown only ifOption(OpenXR): four sliders - vr_openxr_present_gamma_bias, _contrast_bias, _brightness_bias, _saturation_bias (menu range -2..2, but code clamps gamma/contrast to 0.25..4 and saturation to 0..4, so the lower half of those sliders is dead...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/vulkan/renderer/vk_postprocess.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `wadsrc/static/menudef.txt`

### XR pipeline instrumentation and runtime capability report
`[EXTENDS]` `[active]`

A VRBenchmarkInfo struct reports the live XR state - multiview supported/enabled/active, view count and view mask, recommended vs actual present extents, scene sample count, render scale, requested vs runtime refresh rate, sync mode, mirror-texture strategy - and is stamped into the header of every benchmark run. Alongside it, dedicated cycle counters time the VR-specific stages (eye composite, finalise eye, final present, submit copy, submit wait, render...

**Why.** VR frame budget is a hard 1/refresh-rate deadline and the expensive stages are the ones upstream never had - per-eye composition, the blit into the swapchain and the fence wait before submission. Without naming and timing those separately there is no way to tell a slow scene from a slow submit, and a benchmark taken at 72 Hz on one...

**Reached by.** Benchmark: the `bench` console command (stock UZDoom) writes benchmarks.txt; the fork's contribution is the XR/VR content of its header and timing lines. Without a keyboard it is reached in two menu steps: set Options > Miscellaneous options > "Developer mode" (cvar `developer`, default 0) to 1 or higher, which unmasks a "Run benchmark" row in Options >...

**Files.** `src/common/rendering/hwrenderer/data/hw_clock.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `wadsrc/static/menudef.txt`

### OpenGL OpenXR device (carried, not built on desktop)
`[NOT IN UZD]` `[legacy]`

A complete second OpenXR device mode written against the OpenGL renderer - eye poses, HUD projection, hand transforms, teleport and haptics - inherited from the QuestZDoom lineage. It is referenced by no CMakeLists in the tree, so it is never compiled into the desktop binary; only its header is included by hw_vrmodes.cpp, and VR_OPENXR_MOBILE explicitly falls back to mono on any non-Vulkan backend. None of the fork's later interaction work (grip contexts,...

**Why.** It records the pre-Vulkan approach and is still wired into the Android makefile, but on desktop the only reachable OpenXR path is the Vulkan one. Worth knowing so nobody reads it as live code or expects an OpenGL+OpenXR configuration to work.

**Reached by.** Completely unreachable: the file is in no CMakeLists, so none of its code is in the binary. vr_mode is menu-exposed (Options -> 3D mode, menudef.txt:2544) and value 15 (VR_OPENXR_MOBILE) can be selected without a keyboard, but on the GL/GLES backends it resolves to mono (hw_vrmodes.cpp:1219-1231); on Vulkan it reaches the separate vk_openxrdevice, never...

**Files.** `src/gl/stereo3d/gl_openxrdevice.cpp`, `src/gl/stereo3d/gl_openxrdevice.h`, `src/CMakeLists.txt`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

## 02. OpenVR and legacy VR paths

> UZDoom 5.0.0 ships no VR code at all — its `VRMode` is a plain struct of eye-shift factors, and its VR-mode enum stops at 14 (checkerboard). UZDXREMA adds a complete SteamVR/OpenVR head-mounted device mode as `vr_mode 10` on the OpenGL backend: HMD and per-eye pose, dual-eye compositor submission, motion-controller tracking and haptics, a SteamVR-overlay "virtual screen" for menus and cutscenes with a laser pointer that drives the real GUI mouse, room-scale locomotion, teleport and natural crouch. It sits alongside — not underneath — the newer OpenXR/Vulkan device (`vr_mode 15`), sharing a backend-neutral state bus (`src/QzDoom/VrCommon.h` + `qzdoom_common.cpp`), the same `vr_*` cvars and the same menus. In practice OpenXR has overtaken it: every interaction feature added since (grip arbitration, capacitive touch, two-handed hold, per-hand reach tuning) is written only by...

### OpenVR (SteamVR) stereo device mode, vr_mode 10
`[NOT IN UZD]` `[legacy]`

Adds a full SteamVR head-mounted device mode to the OpenGL renderer. It initialises the OpenVR runtime, pulls per-eye projection and eye-to-head transforms from the SDK, blits each eye into its own LDR framebuffer and submits both to the SteamVR compositor every frame, while mirroring to the desktop window in side-by-side, left-eye or right-eye form. The OpenVR API is loaded dynamically through the engine's FModule, so a build with no openvr_api.dll...

**Why.** UZDoom 5.0.0's stereo support is limited to screen tricks (anaglyph, side-by-side, interlaced) driven by a fixed IPD shift. There is no runtime, no head pose, no compositor submission and no per-eye projection. Everything a headset needs had to be added.

**Reached by.** Options > Display Options > "OpenGL Renderer" ($DSPLYMNU_GLOPT -> OptionMenu "OpenGLOptions", menudef.txt:403, 1375, 2484) > "$GLPREFMNU_VRMODE" -> VR3DMenu (menudef.txt:2487) > 3D mode (cvar vr_mode, OptionValue VRMode at menudef.txt:2395) = "OpenVR PC" (value 10; label string OPTVAL_OPENVR, language.0:257). vr_mode is CUSTOM_CVAR(Int, vr_mode, 0,...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/rendering/gl/stereo3d/gl_openvr.h`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/openvr_include.h`, `src/CMakeLists.txt`

### OpenGL-first backend default, chosen for the OpenVR path
`[REPLACES]` `[active]`

Changes the renderer the engine picks when nothing is configured. Upstream defaults `vid_preferbackend` to BACKEND_DEFAULT (Vulkan where the build supports it); the fork hard-codes BACKEND_OPENGL, with an in-code comment naming gl_openvr.cpp as the reason. It also restores the `V_GetBackend()` accessor UZDoom 5.0.0 deleted, as a clamping wrapper the VR code routes every backend query through, and raises the default window to 1400x1400 with a new archived...

**Why.** The OpenVR device mode is written against GLRenderer and the GL framebuffer manager, so it only exists on the OpenGL backend. OpenXR's device is Vulkan-only and falls back to mono on GL. Whichever backend the engine starts on decides which of the two VR paths is even reachable, so the default is the single most load-bearing fact about...

**Reached by.** cvar vid_preferbackend via Options > Video Mode > "Preferred rendering backend" (menudef.txt:1554, OptionMenu VideoModeMenu, reached from OptionsMenu line 404 "$OPTMNU_VIDEO") - NOT Display Options. Also settable in the desktop startup launcher (i_interface.cpp DefaultBackend). vid_refreshrate via Options > Display Options > "Refresh Rate"...

**Files.** `src/common/rendering/v_video.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### SteamVR-overlay virtual screen (cinema mode)
`[NOT IN UZD]` `[legacy]`

When the game is not rendering a live 3D scene — menus, console, title screen, intermissions, cutscenes, or cinema mode toggled on during play — the frame is redirected onto a SteamVR compositor overlay quad floating in the room, with a solid colour backdrop submitted to the compositor behind it. Five placement modes are offered: stationary (anchored where you were looking), stationary-with-lazy-follow (re-targets only after 15 degrees of head yaw, then...

**Why.** A flat 2D menu drawn into a stereo eye buffer is unreadable and nauseating in a headset. The overlay gives the UI a fixed, comfortable place in the room, and lets the player watch a cutscene on a virtual screen rather than being dragged around by the game's camera.

**Reached by.** Two gates, both menu-reachable (no keyboard needed): (1) Select the backend that runs this code: Options > Display Options > "$GLMNU_3DMODE" (vr_mode, menudef.txt:2544, OptionValue VRMode at :2395) set to "OpenVR PC" = 10 (VR_OPENVR, hw_vrmodes.h:51). vr_mode default is 0 (mono), so the OpenVR path is opt-in. Requires the OpenGL backend; on Vulkan, vr_mode...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/QzDoom/qzdoom_common.cpp`, `wadsrc/static/menudef.txt`

### Controller laser pointer and virtual mouse over the overlay
`[NOT IN UZD]` `[legacy]`

While a menu or the console is open, casts a ray from the right controller, intersects it with the overlay quad's plane, converts the hit to screen pixels and posts real EV_GUI_MouseMove / LButtonDown / LButtonUp / WheelUp / WheelDown events, so every existing menu widget — including sliders you drag — works from a controller. Two extra SteamVR overlays draw the beam (a stretched 1x1 white texture tinted by the user's colour) and a ring-plus-dot cursor...

**Why.** The engine's menus assume a mouse. In a headset there is no mouse and no keyboard, so without this the only way through the UI is D-pad-style stick nudges, which cannot operate sliders or colour pickers.

**Reached by.** Not reachable at all on a default build. src/rendering/gl/stereo3d/gl_openvr.cpp is wrapped entirely in `#ifdef USE_OPENVR` (line 30, with a stub at 294), and USE_OPENVR is only defined when the CMake option ENABLE_OPENVR is turned on — src/CMakeLists.txt:393 `option( ENABLE_OPENVR "Enable OpenVR virtual reality mode" NO )`. On top of that, the OpenVR...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/common/menu/menu.cpp`, `wadsrc/static/menudef.txt`

### SteamVR display refresh-rate control
`[NOT IN UZD]` `[legacy]`

Maps the engine's vid_refreshrate cvar onto SteamVR's own preferred-refresh-rate setting. On mode construction and whenever the menu value changes, it writes steamvr/preferredRefreshRate through IVRSettings, then reads the HMD's actual Prop_DisplayFrequency_Float back to confirm, logging the request and the observed rate when developer mode is on. It caches the last request so it does not re-poke SteamVR every frame, and logs once (not repeatedly) when...

**Why.** Headsets run at 72/80/90/120 Hz and the choice materially affects comfort and GPU load, but SteamVR owns the setting — there is no engine-side display mode to change. Upstream's vid_refreshrate does not exist at all; the fork adds it and needs a path from the menu to the runtime.

**Reached by.** Two menu locations, both fork-added, both usable without a keyboard: Options > Quick Menu > Refresh Rate (the first item there; menudef.txt:380 -> OptionMenu QuickMenu at 4348, entry at 4353) and Options > Display Options > Refresh Rate (menudef.txt:1389, in OptionMenu "VideoOptions"). Not a slider: a fixed option list Auto/60/72/80/90/120 (OptionValue...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/common/rendering/v_video.cpp`

### OpenVR controllers as an engine joystick device (INPUT_OpenVR)
`[NOT IN UZD]` `[legacy]`

Registers a synthetic joystick device named "OpenVR" in the engine's JoyDevices table, exposing eight named axes — off-hand and dominant-hand trackpad and thumbstick, horizontal and vertical — whose display names follow the current handedness setting ("Right Joystick Horizontal" becomes "Left …" when left-handed). Each axis carries its own deadzone (0.25 default), scale, digital threshold and response curve, saved and reloaded through the normal joystick...

**Why.** VR thumbsticks have to be visible to the engine's binding and sensitivity machinery, but they cannot be driven by the tic-rate joystick path — head-rate turning needs render-frame sampling. This gives them a home in the input system without handing gameplay motion to it.

**Reached by.** Options > Gamepad/Joystick Options (menudef.txt:390 `Submenu "$OPTMNU_JOYSTICK", "JoystickOptions"`, menu defined at menudef.txt:1263), where the device appears as a row named "OpenVR" that opens JoystickConfigMenu (joystickmenu.cpp:269 builds one row per device from GetName()). NOT Options > Customize Controls. Per-axis deadzone and scale are editable...

**Files.** `src/win32/i_openVR.cpp`, `src/common/platform/win32/i_input.h`, `src/rendering/gl/stereo3d/gl_openvr.cpp`

### Two selectable controller schemes with grip-as-shift and handedness
`[NOT IN UZD]` `[legacy]`

Offers two whole input models, switchable from the menu. "GZDoom VR" mode turns every physical button and pad/stick direction into an ordinary engine key (KEY_PAD_A, KEY_PAD_LTRIGGER, KEY_JOYAXIS1PLUS and so on) so the whole controller is rebindable in Customize Controls, with a separate mapping while a menu is open (GK_LEFT/RIGHT/UP/DOWN/RETURN/BACK). "QuestZDoom" mode instead ships the fixed QuestZDoom layout in which holding the dominant grip acts as a...

**Why.** Motion controllers have far fewer buttons than a Doom keyboard. The grip-as-shift bank roughly doubles the reachable actions, and the two modes exist because rebindability and the familiar QuestZDoom layout pull in opposite directions.

**Reached by.** Options > VR Options (VROptionsMenu, menudef.txt:381). Control Mode = vr_joy_mode (menudef.txt:2065, shown only under ifOption(OpenVR, OpenXR)): 0 = GZDoom VR, 1 = QuestZDoom, DEFAULT 1, so QuestZDoom's grip-shift layout is what ships. Control Scheme = vr_control_scheme (menudef.txt:2067; OptionValue ControlScheme at 1631): 0 right-handed (default), 10...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`, `wadsrc/static/menudef.txt`, `wadsrc/static/engine/vr/commonbinds.txt`

### Room-scale gameplay wiring: hand poses into the playsim, natural crouch, teleport, stabilised two-handed aim
`[NOT IN UZD]` `[legacy]`

Turns tracked poses into things the game simulation can act on. Each frame it writes the dominant hand's world position and angles into the player actor's AttackPos/AttackAngle/AttackPitch/AttackRoll (plus MainHandRoll, kept where the playsim will not zero it) and the off hand's into OffhandPos/OffhandAngle/OffhandPitch/OffhandRoll, so shots originate from the controller rather than the eye. Physically ducking drives crouch directly by setting...

**Why.** Nothing in Doom's playsim knows about hands or a play space — attacks come from the player's eye and crouching is a button. These fields and conversions are what let a mod aim a weapon by pointing, duck under a fireball by actually ducking, or step around a corner.

**Reached by.** Compiled out of a default build: src/CMakeLists.txt:393 is `option( ENABLE_OPENVR "Enable OpenVR virtual reality mode" NO )`, and `case VR_OPENVR` (hw_vrmodes.cpp:1209) sits inside `#ifdef USE_OPENVR`. With OpenVR enabled at configure time it needs the OpenGL backend (OpenVR = mode 10 is GL-only; OpenXR = 15 is Vulkan-only) and vr_mode set to 10 via Options...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/QzDoom/VrCommon.h`

### Backend-neutral VR state bus with multiplayer-safe locomotion queues
`[NOT IN UZD]` `[active]`

A small shared module both device backends write into and the rest of the engine reads from: HMD position and orientation, per-hand offsets and angles, player yaw, snap turn, teleport arm/trigger flags and cinema-mode state, plus the screen-layer decision (VR_UseScreenLayer / VR_UseCinematicScreenLayer) that tells the renderer whether the frame goes to the world or the virtual screen. It also carries the fork's multiplayer contract: in a net game,...

**Why.** Two device backends, a dozen playsim and renderer files, and a netplay determinism requirement all need the same head and hand state. Without one home for it, each backend grows its own copy and the two drift — which the shared turn helper's comment says outright was the previous situation.

**Reached by.** Internal C++ API; no user surface of its own, but its behaviour is tuned from menus (Options -> VR Preferences: "$VRPREFMNU_USE_TELEPORT" -> vr_teleport, "$VRPREFMNU_CROUCH_USING_BUTTON" -> vr_crouch_use_button, "$VRPREFMNU_SNAP_TURN_ANGLE" -> vr_snapTurn slider at menudef.txt:2080/2082/2088; cinema-screen options at menudef.txt:1696-1701 ->...

**Files.** `src/QzDoom/VrCommon.h`, `src/QzDoom/qzdoom_common.cpp`, `src/g_game.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### SteamVR render-model controller meshes as engine models
`[NOT IN UZD]` `[dormant]`

Wraps SteamVR's IVRRenderModels in an FModel subclass and an FTexture subclass, so the physical controller mesh and diffuse texture the runtime ships for whatever hardware is connected are loaded asynchronously across frames, converted into an engine model vertex/index buffer and cached per render-model name. Each detected controller is associated with its loaded mesh as soon as loading completes.

**Why.** Showing the player's actual controller model — rather than a generic stand-in — is the normal way a VR title renders empty hands, and only the runtime knows what the connected hardware looks like.

**Reached by.** No user surface, and doubly gated before it even runs. The whole file is inside `#ifdef USE_OPENVR`, which comes from `option( ENABLE_OPENVR ... NO )` at CMakeLists.txt:393 — default off, though the owner's configured build has it on (build-dxr/CMakeCache.txt:416). Beyond that, the loading code only executes inside the OpenVR device-scan loop, which...

**Files.** `src/rendering/gl/stereo3d/gl_openvr.cpp`

### LSMatrix44 / LSVec3 pose-matrix helper
`[NOT IN UZD]` `[active]`

A small VSMatrix subclass that constructs directly from an OpenVR HmdMatrix34_t, adds row-style m[i][j] indexing, matrix-times-vector, transpose and a translation-stripping helper. Used to build the OpenVR-to-Doom axis permutation (including the mirror inversion and the 1990s pixel-aspect scale), to derive the stereo eye offset and the HMD height correction, and to read hand world positions back out for the playsim. Its OpenVR-specific constructor is...

**Why.** Converting between a tracking runtime's right-handed metre-scale poses and Doom's mirrored, pixel-stretched, unit-scaled world takes more matrix manipulation than VSMatrix offers, and both device backends need the same operations.

**Reached by.** Internal C++ header; no user surface, no cvar, no menu entry. Included by three compiled translation units — gl_openvr.cpp (whole body inside #ifdef USE_OPENVR), hw_vrmodes.cpp, and vk_openxrdevice.cpp — plus src/gl/stereo3d/gl_openxrdevice.cpp, which includes it but is listed in no CMakeLists and is not built. Reached indirectly whenever VR is on: on the...

**Files.** `src/rendering/gl/stereo3d/LSMatrix.h`, `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`

### QZDoomVR.bat launcher preset
`[NOT IN UZD]` `[legacy]`

A drag-and-drop Windows batch file that launches the engine straight into OpenVR mode with a working preset: OpenGL backend, vr_mode 10, vsync off, 1400x1400 default resolution, joystick input on and mouse input off, HUD pitch-locked, automap drawn on the HUD surface, and a raised dynamic-light cap.

**Why.** Before the VR options were reachable from in-headset menus, there was no way to get from a fresh install into VR without typing console commands — which is exactly what a headset user cannot do.

**Reached by.** Nominally: double-click the .bat, or drop a pk3/PWAD onto it (it does `cd /d "%~dp0"` then passes `%*` through, so drag-and-drop works as designed). In practice it is dead: it launches `qzdoom.exe`, and the fork's own build script configures `-DZDOOM_EXE_NAME:STRING=doomxr` and checks for `doomxr.exe`, so that binary is never produced. Nothing in any...

**Files.** `src/win32/QZDoomVR.bat`, `auto-setup-windows-vr.cmd`

## 03. VR input, bindings, and text entry

> Upstream UZDoom 5.0.0 assumes a keyboard, a mouse and at most an Xbox pad. UZDXREMA replaces that whole front end for a player wearing a headset with two 6DoF controllers and no keyboard within reach. OpenXR actions are turned into synthetic key codes so the ordinary bind system still works, a grip-held shift layer doubles the button roster, and a VR-only family of default bind sets (engine/vr/commonbinds.txt plus defbind0-3) is selected instead of upstream's. Everything that upstream reaches by typing — chat, console commands, cheats, which mods to launch with — gets a controller-driven menu, and the menu system itself gains a laser pointer, controller bind capture and stick scrolling. Locomotion adds snap/smooth turn on one slider, physical crouch, walk/run scaling and teleport, plus a ZScript API so a mod's in-world menu can claim the sticks and the haptics.

### VR-only default bind sets, with mod DEFBINDS off by default
`[REPLACES]` `[active]`

When VRMode::IsVR() is true, C_SetDefaultKeys loads engine/vr/commonbinds.txt instead of upstream's engine/commonbinds.txt, and cl_defaultconfiguration selects one of seven default bind sets: 0-3 are the fork's VR layouts (engine/vr/defbind0-3.txt = "QZD Pre 1.3.0", "QZD Dual Wield" which is the new default, "QZD Classic", "QZD Koopa"), 4-6 are upstream's defbinds/origbinds/leftbinds. Per-IWAD overrides live in...

**Why.** A VR layout is hand-tuned around ten physical controls, not a hundred keys, so upstream's three keyboard-shaped presets are useless and a mod's DEFBINDS silently overwriting a working grip-modifier layout is unrecoverable from inside a headset with no console. defbind2.txt is deliberately a header comment only, so that layout slot exists...

**Reached by.** Options > Customize Controls: "Layouts" (cl_defaultconfiguration) then "Reset to defaults" (resetb2defaults) or "Update missing" (binddefaults) to apply; "Enable binds from IWADs and Mods" (cl_custombinds) and, indented under it, "Allow to override user bindings" (cl_custombinds_override) at the foot of the same page (menudef.txt:592-597). Fully...

**Files.** `src/common/console/c_bind.cpp`, `wadsrc/static/engine/vr/commonbinds.txt`, `wadsrc/static/engine/vr/defbind3.txt`, `wadsrc/static/engine/vr/defbind1.txt`, `wadsrc/static/filter/game-hexen/engine/defbinds.txt`, `wadsrc/static/menudef.txt`

### Controller buttons as bindable keys, with a grip-held shift layer
`[NOT IN UZD]` `[active]`

The OpenXR backend reads its own action set (select, grip, thumbclick, menu, A/B/X/Y, thumbstick, trackpad, thumb/trigger touch) and posts each transition as a synthetic key event, so every VR control is an ordinary bindable key. While vr_secondary_button_mappings is on, holding the dominant grip remaps each button and stick direction to a second key: main-hand trigger rtrigger -> ltrigger, face A/B pad_a/pad_b -> lthumb/backspace, thumb-click enter ->...

**Why.** Two controllers carry about ten controls; Doom's action list is far longer. A modifier layer roughly doubles the roster without asking the player to memorise chords the engine does not know about, and expressing it as synthetic keys means Customize Controls, KEYCONF, and every existing mod bind keep working unchanged. defbind3.txt is...

**Reached by.** VR Options > VR Controls: "Secondary Button Mappings" (vr_secondary_button_mappings, default TRUE) and "Switch Thumbsticks" (vr_switch_sticks, default false) — both menu rows are gated on vr_joy_mode being nonzero (wadsrc/static/menudef.txt:2065-2075); "Control Mode" (vr_joy_mode, default 1, its CUSTOM_CVAR body prints that a restart is required); "Control...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `wadsrc/static/engine/vr/defbind3.txt`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/d_main.cpp`, `wadsrc/static/menudef.txt`

### Snap turn and smooth turn on one slider
`[NOT IN UZD]` `[active]`

vr_snapTurn (0-90 degrees) is a single control with two regimes. Above 10 it is latched snap turn: the turn stick past 0.60 fires one discrete step and re-arms only below 0.40, so a held stick turns once. At 10 or below it becomes analog smooth turn — VR_ApplyAnalogSmoothTurn ramps toward a 210 deg/s cap through a smoothstep ease with a 0.10 deadzone, and the same slider value sets the responsiveness via VR_GetAnalogTurnResponseScale (0 gives the...

**Why.** Snap turn is the standard comfort option and smooth turn is what experienced players want; putting both on one number means there is one thing to find in a headset menu rather than a mode switch plus a magnitude plus a rate. Resetting the latches rather than merely skipping them stops a stick held through a wheel selection from firing a...

**Reached by.** Menu-reachable, no console needed: Options > VR Options (Submenu "$VRPREFMNU_TITLE" -> VROptionsMenu, wadsrc/static/menudef.txt:381 / :2026) > "Locomotion" section > Slider "Snap-turn Angle" bound to vr_snapTurn, range 0.0-90.0 step 1.0 (menudef.txt:2088), with the brown StaticText hint "Set Snap-Turn <= 10 for Smooth Turn" directly under it...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `wadsrc/static/menudef.txt`, `src/rendering/gl/stereo3d/gl_openvr.cpp`

### Room-scale locomotion: stick walking, teleport, physical crouch, speed scaling
`[NOT IN UZD]` `[active]`

In VR the keyboard movement paths in G_BuildTiccmd are disabled outright and movement comes from VR_GetMove instead, run through a squared response curve with a 0.15 deadzone. vr_move_use_offhand steers by the off-hand controller rather than the head. vr_teleport turns a forward stick push into a teleport step. vr_move_speed rescales gameinfo.normforwardmove/normsidemove directly (an absolute walk speed), or, at 0, hands over to vr_walk_multiplier;...

**Why.** Stick locomotion, teleport and physical crouch are the standard VR comfort axes, and none of them exist in a fork-free engine that only knows about keyboard movement tiers. Rescaling the movement tables rather than multiplying the ticcmd keeps run/walk, turbo and mod speed effects consistent.

**Reached by.** Options > VR Options ("$VRPREFMNU_TITLE", menudef.txt:381 -> VROptionsMenu at 2026). "Locomotion" is a gold StaticText section header inside that menu (menudef.txt:2078), not a submenu. Items: "Off-hand Move Direction" (vr_move_use_offhand - present but inert, see correction), "Use Teleport" (vr_teleport), "Slidey movement (Momentum)" (vr_momentum), "Crouch...

**Files.** `src/g_game.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `wadsrc/static/menudef.txt`

### ZScript can claim the sticks, read them raw, and buzz the controllers
`[NOT IN UZD]` `[active]`

Four new FLevelLocals natives. level.SuppressVRInput(bool) stops the VR input path from turning or walking the player while a mod's in-world selector is open, and level.IsVRInputSuppressed() reads it back. level.GetRawStickMove() returns the locomotion stick's real (forward, side) deflection, captured before the suppression check, so a mod can suppress movement and still drive a menu with the same thumb. level.VRHaptic(hand, intensity, durationMs) fires a...

**Why.** Snap turn and stick movement are decided deep in the OpenXR path, long before any script sees a button, so a mod whose in-world menu is driven by the thumbstick was spinning and walking the player while they chose. Suppression alone also zeroed cmd.sidemove/forwardmove, which was the only channel script had for reading stick deflection...

**Reached by.** ZScript API only: level.SuppressVRInput(bool), level.IsVRInputSuppressed(), level.GetRawStickMove(), level.VRHaptic(hand, intensity, durationMs) — declared in wadsrc/static/zscript/doombase.zs (lines 901, 954, 911, 933). There is NO menu entry, cvar, or bind for any of the four; a mod must call them. The one gate on the haptic call is the cvar...

**Files.** `src/scripting/vmthunks.cpp`, `wadsrc/static/zscript/doombase.zs`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/g_game.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`

### Two-handed hold published to weapons
`[NOT IN UZD]` `[dormant]`

When the off hand's grip subject is the weapon's support point, grip or forend (published by script through GripClaimOff), the engine sets AActor.TwoHandedHold and raises WF_TWOHANDSTABILIZED on the player's WeaponState, and weapons read it to tighten their spread. While stabilized, the off-hand weapon's own button checks are skipped in P_CheckWeaponButtons and in the ZScript weapon/player state handlers, so the supporting hand cannot fire a second gun....

**Why.** The old behaviour snapped the gun the moment two controllers came within a fixed distance of each other, whether or not the off hand had anything to do with the weapon — and reloading is exactly the gesture that brings the hands together. Publishing a real, script-asserted grab means being two-handed makes you shoot straighter instead of...

**Reached by.** Nothing needs to be reached for the flag itself: AActor.TwoHandedHold is published every controller frame by the Vulkan OpenXR path and is readable from ZScript as `native readonly bool TwoHandedHold` (wadsrc/static/zscript/actors/actor.zs:508). No engine code consumes it, so it does nothing until a mod reads it. Everything else the entry describes —...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/playsim/p_pspr.cpp`, `src/playsim/actor.h`, `src/playsim/d_player.h`, `wadsrc/static/zscript/actors/inventory/weapons.zs`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### Per-weapon stabilize reach (plumbed, currently inert)
`[NOT IN UZD]` `[dormant]`

Weapon gains a StabilizeDistance property in inches, and a dedicated event handler (VRStabilizeSyncHandler, deliberately not a Weapon.Tick hook, so a weapon that overrides Tick without calling Super cannot silently break it) copies the ready weapon's value into the native AActor.StabilizeReach every tic. The OpenXR backend reads StabilizeReach, falls back to vr_stabilize_distance_inches when it is 0, and converts to metres. As of the current tree that...

**Why.** Kept as live plumbing because the shape is right — per-weapon reach authored on the Weapon class, bridged to a plain native double the input backend can read — and only the consumer was retired. Worth flagging that the menu still shows a "Stabilize Distance (in)" slider and vr_stabilize_requires_grab is still an archived cvar, neither of...

**Reached by.** ZScript only, in practice. Weapon property `StabilizeDistance` (declared wadsrc/static/zscript/actors/inventory/weapons.zs:61, property at :116) and native `AActor.StabilizeReach` (actor.zs:429, DEFINE_FIELD at vmthunks_actors.cpp:2247). The sync handler is auto-registered for every game via wadsrc/static/mapinfo/common.txt:8 (`AddEventHandlers =...

**Files.** `wadsrc/static/zscript/vr_stabilizesync.zs`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/playsim/actor.h`, `wadsrc/static/zscript/actors/inventory/weapons.zs`, `src/scripting/vmthunks_actors.cpp`

### Typing with no keyboard: chat and console character grids
`[NOT IN UZD]` `[active]`

Three separate fork-only entry points onto a 13x5 character grid driven by the D-pad/stick and trigger. messagemode/messagemode2 no longer open upstream's in-viewport chat prompt; CT_OpenTextEntryMenu resets button states, hides the console and opens ChatTextEnterMenu, which submits through native SubmitChatMessage/CancelChatMessage and knows whether it is team chat. Dropping the console opens ConsoleTextEnterMenu automatically and, in single-player,...

**Why.** The player is in a headset with no keyboard. Upstream's chat prompt and console both require typed input, which makes multiplayer chat and every console-only setting unreachable. Upstream does ship a character grid (TextEnterMenu) but only as a text-field editor; none of these three entry points exist there.

**Reached by.** Chat (active by default): any key bound to messagemode / messagemode2 — Options > Customize Controls > Multiplayer, rows "$CNTRLMNU_SAY" / "$CNTRLMNU_TEAMSAY" (menudef.txt:786-787). In single-player messagemode takes the team branch, so the prompt reads "TEAM SAY:". Console (active by default, no cvar): any key bound to toggleconsole — Options > Customize...

**Files.** `wadsrc/static/zscript/engine/ui/menu/chattextentermenu.zs`, `wadsrc/static/zscript/engine/ui/menu/consoletextentermenu.zs`, `src/ct_chat.cpp`, `src/d_main.cpp`, `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs`, `src/common/scripting/interface/vmnatives.cpp`

### Controller-driven cheat, spawner and level-select menu
`[NOT IN UZD]` `[active]`

A 1255-line ZScript menu (CheatMenu plus fifteen per-gametype subclasses) that runs cheats as menu rows instead of typed console commands: god mode, noclip, notarget, freeze, give all, kill monsters, resurrect, nextmap, developer levels, and per-IWAD item/weapon spawner, monster spawner and level-select pages for Doom, Chex, Heretic, Hexen and Strife. Opened and closed by CCMD togglecheatmenu, which is a bindable action, and the cutscene and intermission...

**Why.** Every cheat in Doom is a typed string, and there is no keyboard. Making them menu rows is the only way to reach them in a headset; letting the bind through cutscenes and intermissions matters because those are exactly the screens where you notice you need nextmap.

**Reached by.** Bind a key/controller button to "togglecheatmenu" — it appears as `WTF?` at the very bottom of Customize Controls > Other (menudef.txt `OptionMenu "OtherControlsMenu"`, line 1084). There is NO default binding, so the user must assign one once; after that it is fully reachable in VR without a keyboard. The same bind also works during cutscenes (screenjob.cpp...

**Files.** `wadsrc/static/zscript/engine/ui/menu/cheatmenu.zs`, `src/g_game.cpp`, `wadsrc/static/menudef.txt`, `src/common/cutscenes/screenjob.cpp`, `src/intermission/intermission.cpp`, `src/common/scripting/interface/vmnatives.cpp`

### Menus driven by a motion controller: laser pointer, bind capture, stick scrolling
`[EXTENDS]` `[active]`

The right controller's pose is ray-cast against the virtual screen quad and the hit point is posted as synthetic GUI mouse move/button events, with a world-space beam quad drawn from the hand to the hit; menu_allow_mouse_override lets those through even when m_use_mouse is off. vr_mouse_in_menu makes the pointer always live, otherwise grip is a hold-to-point modifier (and then grip does not also emit right-click). Trigger-as-key is suppressed while the...

**Why.** Without this, a headset user cannot point at a menu at all, and — worse — could not bind a controller button to anything, because upstream's responder discards joystick keys in exactly the state where the binding UI is waiting for one. That made the entire Customize Controls page unusable from inside VR.

**Reached by.** Options > VR Options ($VRPREFMNU_TITLE, menudef.txt:381 -> :2026), "VR Controls" section: "Virtual Mouse" (vr_mouse_in_menu, default FALSE) and "Mouse Pointer" colour picker (vr_menu_pointer_color, default 0xffffff). With vr_mouse_in_menu off — the default — the pointer is live only while the right grip is held; turning it on makes it always live and...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/menu/menu.cpp`, `src/common/menu/menu.h`, `wadsrc/static/zscript/engine/ui/menu/menu.zs`, `src/common/menu/joystickmenu.cpp`, `src/rendering/gl/stereo3d/gl_openvr.cpp`

### Double bindings exposed as a complete second control table
`[EXTENDS]` `[dormant]`

Upstream ships the DoubleBindings machinery (a second binding fired when a key is pressed twice within cl_doubleclickthreshold) but has zero menu rows for it — its own three-argument OptionMenuItemDoubleControl is never instantiated. The fork replaces that item with a two-argument form that binds straight into DoubleBindings, and MENUDEF now carries a full parallel set of Action / Weapons / Inventory / Other / Popups / Multiplayer double-binding pages,...

**Why.** Ten physical controls is not enough for Doom's action list even with a grip shift layer. Double-tap is a third layer that needs no extra hardware, and it was already implemented in the engine — it just had no way for a headset user to see or edit it.

**Reached by.** Options > Customize Controls > "Show double bindings" (menu_showdoublebindings, a YesNo row at the very top of the page). It defaults to OFF, so the double-binding pages are hidden on a fresh launch; flipping it runs a CUSTOM_CVAR callback that tears down and rebuilds the menus and drops the player back on Customize Controls, now showing the six double...

**Files.** `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs`, `wadsrc/static/menudef.txt`, `src/menu/doommenu.cpp`, `src/common/console/c_bind.cpp`, `wadsrc/static/engine/vr/defbind3.txt`

### Command-line launch profiles picked from a menu
`[NOT IN UZD]` `[active]`

ProfileManager scans the search paths and $PROGDIR/profiles/ for files named commandline_<name>.txt, reads an optional leading `#TITLE` line for the display label, and builds a sorted list. Setting the cmdlineprofile cvar strips the launch-affecting arguments the profile is going to replace (-iwad, -file, -deh, -bex, -playdemo, -skill, -savedir, -xlat, -oldsprites, -deathmatch, -altdeath) and requests a restart. doommenu.cpp populates...

**Why.** Choosing which IWAD and which mods to load normally means editing a shortcut or typing a command line — impossible mid-session in a headset. This turns the whole launch configuration into a menu pick plus a restart, which is the only keyboard-free way to change what is loaded.

**Reached by.** Fully menu-reachable in VR, no console needed. Options menu, near the bottom (E:\UZDXREMA\wadsrc\static\menudef.txt:412), row "Active Profile" — a stock LabeledSubmenu bound to the cmdlineprofile cvar, opening CommandLineProfileMenu (menudef.txt:366). The value shown on that row is the raw profile stem from the cvar (or localized "$notset" when empty), NOT...

**Files.** `src/menu/profiledef.cpp`, `src/menu/profiledef.h`, `src/menu/doommenu.cpp`, `wadsrc/static/menudef.txt`

### A claimed wrist-mount pose takes that hand's buttons (in-flight)
`[NOT IN UZD]` `[in flight]`

While AActor.HardpointClaimMain/Off is raised on a hand, that hand's trigger and face pad stop emitting their normal keys and are published to script instead as a bitfield in the new AActor.HardpointButtons (bit 0 grip, 1 pad, 2 trigger; main in the low nibble, off in the high one). Grip is deliberately not suppressed — an earlier revision did suppress it and that silently killed the store/draw bind whenever a hand came near a mount, so nothing could ever...

**Why.** A pose that binds actions to buttons has to take those buttons away from what they normally do — pull the trigger to fire a wrist mount and you must not also fire the gun in that hand. The engine is the only layer that can suppress the emission; what a mount does is script's business. The alternative, inventing six spare key codes for...

**Reached by.** ZScript API only, and mod-side on both ends. Script must itself raise `AActor.HardpointClaimMain/Off` on the console player's pawn (nothing in the engine or in wadsrc ever writes them — the only C++ uses are reads in vk_openxrdevice.cpp), and script then reads `AActor.HardpointButtons` instead of cmd.buttons. No cvar, no menu entry, no bind: the suppression...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/playsim/actor.h`, `wadsrc/static/zscript/actors/actor.zs`, `src/scripting/vmthunks_actors.cpp`

## 04. Engine-native VR weapon and item wheel

> UZDXREMA carries a complete radial selection UI written in C++ inside the renderer, in a file that does not exist upstream at all (src/common/rendering/hwrenderer/data/hw_vrwheel.cpp, 2411 lines, added wholesale relative to 5.0.0). It draws real world-space geometry — discs, textured quads, optionally full 3D weapon models, and text built glyph-by-glyph from a game font — anchored near a motion controller, and lets the player pick a weapon or inventory item by pointing, reaching, aiming or thumbstick. There is one independent wheel per hand, so the hand not holding a ring keeps its trigger, its reload and its laser. The engine deliberately refuses to decide policy it does not own: it announces open and close as netevents rather than freezing time itself, it asks ZScript what to write on the info panel, and it steps aside entirely if a loaded mod declares wr_suppress_native_wheel.

### Dual-hand radial wheel drawn as world geometry
`[NOT IN UZD]` `[active]`

Two independent wheels, indexed by VR_MAINHAND / VR_OFFHAND, can be open at the same time; each is worked by the hand that opened it, and asking for a second wheel on a hand that already holds one replaces it (and announces the replaced one as closed). The ring is drawn as real world geometry inside the main-view render pass — translucent discs and textured quads pushed straight into screen->mVertexData with depth test off — not as a screen-space HUD...

**Why.** A VR player picking a weapon needs a target the hand can physically reach and the eyes can leave. A screen-space overlay has no depth to reach into, and one shared wheel would take both hands out of the fight. Upstream UZDoom ships no VR code, so no layer of it has anything like this.

**Reached by.** Three bindable +/- commands registered in src/g_game.cpp:760-765: +vrweaponwheel, +vroffhandweaponwheel, +vrinvwheel. They are bindable from menus, so no keyboard is required — but they ship UNBOUND by default (no defbinds entry exists anywhere in wadsrc/), so the player must assign controller buttons before the wheel can be opened at all. Bind rows appear...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `src/common/rendering/hwrenderer/data/hw_vrwheel.h`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/g_game.cpp`, `wadsrc/static/menudef.txt`

### Mod override hook: wr_suppress_native_wheel
`[NOT IN UZD]` `[active]`

Before it does anything else, OpenWheel looks up a cvar named wr_suppress_native_wheel by name with FindCVar and returns immediately if it reads true. The engine READS a cvar the mod declares rather than declaring one for the mod to write, and the lookup happens per button press rather than being cached. An absent cvar means enabled, so a session with no such mod behaves exactly as before.

**Why.** The owner maintains a separate ZScript weapon wheel; two wheels on the same bind is worse than either alone. The read-don't-write direction is forced by the VM: CVar.SetInt/SetBool refuse any cvar lacking CVAR_MOD when called outside menu code, so a mod writing an engine-owned cvar from its own tick aborts the VM. An engine read of a mod...

**Reached by.** ZScript/mod-side only. The engine declares nothing: no CVAR() for this name anywhere in the tree, no CVARINFO entry, no menudef.txt line. It is reached solely by a loaded pk3 declaring its own cvar named wr_suppress_native_wheel (e.g. `server bool wr_suppress_native_wheel` in the mod's CVARINFO) and setting it true; the engine only ever reads it. Whether...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `ENGINE_DELTA.md`

### Four selection modes, pointer-ray by default
`[NOT IN UZD]` `[active]`

vr_wheel_selection_type picks how the highlight is driven: 0 touch (reach the hand into the icon's sphere), 1 aim (how far the hand pose has turned from where it was when the wheel opened), 2 thumbstick (absolute stick direction, with its own deadzone; releasing to centre keeps the last pick so letting go of stick then bind still commits), 3 pointer (a ray from the hand struck against the wheel's own plane, then binned by angle). Pointer is the default....

**Why.** Touch was the original mode and is the weakest: it asks the hand to arrive at a roughly 4cm target it cannot feel, it is measured against OpenXR's aim pose rather than the grip pose so the tested point sits past the knuckles, and it breaks outright once the ring can move, because a reach is positional and the icons are what moved. Aim is...

**Reached by.** Options > Mod Options > VR Weapon Wheel (menudef `ModOptionsMenu` -> Submenu "VRWeaponWheelOptions"), under the "Wheel Selection" heading: "Selection mode" (Touch / Aim / Thumbstick / Point at it), "Aim select angle" (vr_wheel_select_angle, 10-90), "Aim select deadzone" (vr_wheel_deadzone, 0.1-5.0), "Show where you are pointing" (vr_wheel_pointer_dot, on by...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `wadsrc/static/menudef.txt`

### Wrist leash for the wheel anchor
`[NOT IN UZD]` `[dormant]`

vr_wheel_leash above 0 makes the parked ring hold perfectly still until the wrist pulls past its edge, then drags it along, keeping the hand exactly at leash distance. The leash length is computed from the layout — outer ring radius plus half a backdrop plus the touch slack — and then scaled by the cvar, so 1.0 means exactly far enough to reach every icon without the ring moving at all, and it survives changes to wheel radius and icon scale. The drag runs...

**Why.** A ring that follows on a spring or at a speed limit cannot hold a selection: standing still to aim is precisely the condition under which it catches up, so the hover you were about to commit slides out from under your hand. A dead region means no motion at all inside it, so hover is as stable as a parked wheel while the rule stays...

**Reached by.** Options > Mod Options > "Weapon/Item Wheel Options" (OptionMenu VRWeaponWheelOptions, reached from ModOptionsMenu) > "Wheel follows wrist" slider; cvar vr_wheel_leash, range 0.0-3.0 in 0.1 steps, default 0 = off. Fully menu-reachable in VR, no console needed; two brown help lines sit under the slider and the cvar is in the menu's reset-to-defaults command.

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `wadsrc/static/menudef.txt`

### Wheel open/close announced as netevents instead of the engine touching time
`[NOT IN UZD]` `[active]`

Opening or closing a wheel sends a network event through primaryLevel->localEventManager->SendNetworkEvent, named by vr_wheel_event_open / vr_wheel_event_close (defaults vrwheel_open / vrwheel_close). Arguments are arg1 wheel type (1 main weapon, 2 offhand weapon, 3 inventory), arg2 anchor hand (0 main, 1 off), arg3 how many wheels are open after this change — so a listener acts when arg3 becomes 1 on open and reaches 0 on close, and two rings behave like...

**Why.** "What should time do while I pick a weapon" is a gameplay question, and a mod that already owns time — a bullet-time meter, a freeze mod, or nothing at all — would have to fight the engine for control if the engine answered it. Netevents arrive in ZScript's NetworkProcess, which is where such mods already listen. NOTE: the menu still...

**Reached by.** ZScript API. The open/close events fire out of the box (vr_wheel_event_open/close default to the non-empty names vrwheel_open / vrwheel_close), so a mod only has to match the name in a StaticEventHandler's NetworkProcess and read arg1 (1 main weapon / 2 offhand weapon / 3 inventory), arg2 (0 main hand / 1 off hand, VR_MAINHAND=0 / VR_OFFHAND=1 in...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `wadsrc/static/menudef.txt`, `wadsrc/static/language.0`

### Per-hand input suppression while a wheel is held
`[EXTENDS]` `[active]`

VRWheel_ShouldSuppressHandInput(hand) reports whether that specific hand is the one holding a ring, and callers use it per hand rather than globally. In G_BuildTiccmd the main hand loses BT_ATTACK, BT_ALTATTACK, BT_USE, BT_RELOAD and BT_MAINHANDDROPMAG, the off hand loses BT_OFFHANDDROPMAG, BT_OFFHANDATTACK and BT_OFFHANDALTATTACK — each only when that hand is busy. The laser sight for a hand is hidden by the same test (gated by vr_laser_hide_on_wheel),...

**Why.** The entire point of a wheel per hand is that the other hand keeps playing. Suppressing globally means opening the main-hand ring disarms the off hand as well. The stick case is separate again: the same thumbstick that is picking an icon would otherwise walk and snap-turn the player mid-choice, which is the most disorienting thing a VR...

**Reached by.** Automatic and unconditional for the per-hand button stripping, the hitscan-tracer gate and the CCMD early-outs — no cvar, no menu item, it runs whenever a wheel is open. The two cosmetic gates default ON: vr_laser_hide_on_wheel (default true) at Options > Mod Options > VR Laser Sight, and vr_wheel_hide_hand_weapon (default true) at Options > Mod Options >...

**Files.** `src/g_game.cpp`, `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`

### Entry eligibility filtering, including holster and per-hand weapon identity
`[NOT IN UZD]` `[active]`

Weapon entries are walked slot by slot from player->weapons and each candidate is filtered: powered-up sister weapons out; anything whose ZScript bHolsterHidden is set out; ammo checks mirroring the game's own usable-weapon rules (AmmoUse1, PRIMARY_USES_BOTH, MinSelAmmo1/2, AMMO_OPTIONAL); RestrictedToPlayerClass / ForbiddenToPlayerClass honoured against the live player class when vr_wheel_hide_other_class_weapons is on; and CanHandTakeWeapon drops any...

**Why.** A wheel that lists a dead entry highlights it, plays the confirm sound, and does nothing. The holster filter is load-bearing: this path calls PlayerPawn.MoveWeaponToHand directly rather than going through CheckAmmo, so without it the wheel could pull a holstered weapon straight into a hand and leave it permanently stuck, since...

**Reached by.** Automatic — the filter runs unconditionally every time a wheel is built, with no cvar to disable the filtering itself. The wheel it feeds is opened by bindable actions (`+vrweaponwheel`, `+vroffhandweaponwheel`, `+vrinvwheel`), bound from Options > Controls (menudef.txt lines 821-822, 894, 1950-1952) — no console command required, so it is fully reachable...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `src/gamedata/a_weapons.h`

### Tick-synchronised commit through a DEM_ZSC_CMD net command
`[EXTENDS]` `[active]`

Releasing the bind commits the hover. For a weapon wheel the selection is not applied inline: it is queued as DEM_ZSC_CMD with the command string vr_moveweaphand and a five-byte payload (int32 InventoryID, int8 target hand), which d_net.cpp intercepts by name, re-finds the item by InventoryID down the player's inventory chain, and calls PlayerPawn.MoveWeaponToHand from inside the tick. Inventory wheels instead set InvSel, zero inventorytics and set...

**Why.** The commit runs from a bind release, off the game tick, and MoveWeaponToHand mutates both weapon-hand slots. Applied outside P_Ticker, the psprite tick can observe the slots half-updated and destroy both layers, because TickPSprites deletes any psprite whose Caller does not match its slot — that is weapons flashing and vanishing, and...

**Reached by.** Automatic on bind release — the wheel closes from -vrweaponwheel / -vroffhandweaponwheel / -vrinvwheel (registered in src/g_game.cpp:760-765) and CloseWheel commits before resetting. All three are bindable from the Controls menu (wadsrc/static/menudef.txt lines 821-822, 894, 1950-1952), so it is fully reachable in VR without a keyboard. No cvar gates the...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `src/d_net.cpp`, `wadsrc/static/zscript/actors/player/player.zs`

### Info panel with a ZScript text hook and world-space font rendering
`[EXTENDS]` `[active]`

A panel describes the entry currently under the hand. Its content comes from a new PlayerPawn virtual, String GetVRWheelInfo(Inventory item, int hand), which returns newline-separated lines — the first drawn larger as a heading — and returns "" by default, which the engine reads as "you decide" and falls back to its own thin readout (tag, ammo counts, x-amount for stacks, or "NOT CARRIED"). Rendering the text needed a whole world-space text path: one...

**Why.** The interesting facts about a weapon are not the engine's to know. A mod that rolls each weapon its own damage, rarity, condition and upgrades holds all of that in script; an engine-side readout could only ever show class defaults, identical for six copies of a gun that are deliberately not identical. The hand argument lets a mod that...

**Reached by.** Content: ZScript API only — override PlayerPawn.GetVRWheelInfo(Inventory item, int hand), returning newline-separated lines; "" is the default and yields the engine fallback. Display: Options > Mod Options > "Weapon/Item Wheel Options" > "Weapon info panel" (Off / Centre of wheel / Beside wheel), with "Info panel text size" slider (0.2-4.0) and "Info panel...

**Files.** `wadsrc/static/zscript/actors/player/player.zs`, `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `wadsrc/static/menudef.txt`

### Automatic two-ring split for crowded wheels
`[NOT IN UZD]` `[active]`

Above 15 entries the layout splits into two concentric rings, inner and outer, with the outer offset by half a slice so icons interleave rather than stacking radially. Outer radius is solved twice — once to get a provisional icon size, once with that size folded back in — so the two rings sit at a spacing that accounts for both their backdrops. Icon size itself shrinks toward the slot chord length as a ring gets crowded, keeping the default feel for small...

**Why.** A single ring of thirty weapons gives each icon an arc too narrow to hit and too small to read. Two rings halve the count per ring without doubling the radius the hand has to reach across.

**Reached by.** Accurate. Options > Mod Options > VR Weapon Wheel > "Auto Split List" (vr_wheel_auto_split, CVAR_ARCHIVE, default true), alongside the "Wheel Radius" (vr_wheel_radius) and "Wheel Icon Scale" (vr_wheel_icon_scale) sliders in the same menu that the layout derives from. Fully menu-reachable in VR; no console command required.

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `wadsrc/static/menudef.txt`

### 3D model icons in place of sprite icons
`[NOT IN UZD]` `[dormant]`

With vr_wheel_icon_load_model on, each entry probes its Ready state for the first usable non-empty frame, looks up a MODELDEF frame for it, and if the player actually owns the item renders the real 3D model in the icon slot through FHWModelRenderer and RenderFrameModels, at ticFrac 0 (posed, not interpolated). The model is yawed to face the wheel plane with per-entry offsets, honours MDL_IGNORETRANSLATION, and detects mirroring from the sign of the...

**Why.** Doom weapon pickup sprites are small, side-on and often unrecognisable at ring scale; in a fork where weapons are already 3D models in the hand, showing the same model on the wheel makes the ring read at a glance. Owned-only because a model frame for something the player has never picked up has no live actor to render from.

**Reached by.** Options > Mod Options > "Weapon/Item Wheel Options" > "Load Model For Icons" (OptionValue WheelIconModelOption: Off / When Available; cvar vr_wheel_icon_load_model, default false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG). Four sliders below it are gated on that cvar and only appear once it is on: "Model Icon Scale Adjust" (vr_wheel_icon_model_scale, 0.1-10.0),...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`, `wadsrc/static/menudef.txt`

### Desktop (no-headset) wheel affordance
`[NOT IN UZD]` `[dormant]`

vr_wheel_desktop lets the wheel run with no headset present. VRWheel_Available(vrmode) replaces every direct IsVR() gate, so the two can never drift apart. It substitutes the two things the wheel cannot otherwise get: the anchor hand pose becomes a synthesised point in front of the camera (offset to the correct side), and the thumbstick becomes how far the view has turned since the wheel opened, normalised against vr_wheel_desktop_range degrees, with yaw...

**Why.** The binds were hard-gated to IsVR() in four places, so on a flat screen they ran a function that returned on its first line and the buttons did nothing — the wheel could not be exercised or tuned without a headset on. The synthesised hand pose is needed because the non-VR branch of GetHandPose reads AttackPos, which holds the actor's...

**Reached by.** Console cvar only. `vr_wheel_desktop` (CVAR_ARCHIVE|CVAR_GLOBALCONFIG, default false) and `vr_wheel_desktop_range` (default 22.0) have no menu row anywhere in wadsrc/ and no bind. Since the owner plays in VR with no keyboard, this is effectively unreachable for him in normal play — which is consistent with its purpose: it exists for the flat-screen testing...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrwheel.cpp`

## 05. Weapon and HUD rendering in stereo

> In stock UZDoom the player's weapon is a 2D overlay: it is drawn after the world, with the depth buffer cleared and an orthographic "HUD sprite" projection. That is invisible-at-infinity in a headset and punches through walls, so this fork moves the entire psprite list into the 3D scene pass, positions it from the controller transform instead of the camera, and rebuilds the 2D HUD as an offscreen canvas pasted onto a world-space quad worn on a hand. Around that sit the supporting pieces: per-hand routing of psprite layers, per-layer tint/glow/hide controls that reach a 3D weapon model, VR billboard modes for weapons that are still sprites, stereo-consistent sprite billboarding driven off a new center-eye viewpoint, and world-geometry laser sights and tracers drawn in the translucent pass. The 18 uncommitted files touch this area only in `hw_drawinfo.cpp::StartScene` (surface-stamp...

### Player sprites drawn inside the 3D scene, per eye
`[REPLACES]` `[active]`

In any stereo mode the weapon/hand psprite list is drawn during the world pass — after RenderScene, before RenderTranslucent, on the eye's real scene projection and against the real depth buffer. Upstream's two flat-overlay call sites (EndDrawScene's depth-clear HUD-model block and DrawEndScene2D's sprite block) are both gated off in that case, so nothing is drawn twice.

**Why.** Upstream draws the viewmodel with GetHUDSpriteProjection() (an ortho matrix) after a state.Clear(CT_Depth). That gives the gun zero stereo disparity — it fuses at infinity in both eyes — and makes it immune to occlusion, so it hangs through walls. A VR weapon has to be an object sitting at a real distance in front of the eyes.

**Reached by.** Automatic and unconditional whenever a live stereo device is running: VKOpenXRDeviceMode (vk_openxrdevice.h:187) and GL OpenVR (gl_openvr.h:124) each override RenderPlayerSpritesInScene() to return true. A third override exists at src/gl/stereo3d/gl_openxrdevice.h:77 but that directory is not referenced by src/CMakeLists.txt, so it is dead code and not a...

**Files.** `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.h`, `src/rendering/gl/stereo3d/gl_openvr.h`

### 3D weapon models and flat overlay layers drawn together
`[REPLACES]` `[active]`

PreparePlayerSprites now runs the 3D and 2D collection passes unconditionally instead of choosing one, and DrawPlayerSprites in VR ignores its hudModelStep argument and draws every collected HUDSprite in a single call. A model weapon therefore keeps its muzzle flash, laser overlay and script HUD layers. The cvar r_hudflatoverlay is the escape valve: below 1.0 an overlay layer whose owning weapon resolves to a model is dimmed to that alpha, and at 0.0...

**Why.** Upstream treats model and sprite as an either/or per player (IsHUDModelForPlayerAvailable). In a fork where essentially every weapon is a mesh, that branch silently deletes every 2D psprite layer the mod owns — flashes, hand layers, overlays — and the loss is a `continue` inside a render loop that no script can reach. r_hudflatoverlay...

**Reached by.** Two halves with different reachability. The dual pass is unconditional engine behaviour with no user surface and no gate. The overlay control is the cvar r_hudflatoverlay (CVAR_ARCHIVE, no CVAR_GLOBALCONFIG, defined at src/rendering/r_utility.cpp:119, default 1.0 = stock, whole block inert at that default). It has no menudef entry anywhere in wadsrc/, so...

**Files.** `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/rendering/r_utility.cpp`

### Per-hand psprite routing and per-hand suppression
`[NOT IN UZD]` `[active]`

Each HUDSprite is assigned to a hand by psprite LAYER ID rather than by weapon class: any layer at or above PSP_OFFHANDWEAPON (1000000) is the offhand, with PSP_FLASH (1000, shared by both hands) resolved by caller pointer identity. That hand then selects which controller transform AdjustPlayerSprites applies, and gates two suppressions — the weapon wheel hides only the hand holding it, and weaponStabilised (a two-handed grip) drops the offhand's layers...

**Why.** The previous test, WeaponSpriteMatches, compares GetClass(), so two pistols could not be told apart: one hand's suppression killed both hands' sprites, and the mainhand sprite was handed the offhand's transform and drawn where the player could not see it. The flash kept working because it is a different layer, which made a vanished...

**Reached by.** Internal to the renderer; no cvar switches the hand routing itself on or off. Mods reach the split from ZScript with the global enum EPSPLayers (wadsrc/static/zscript/constants.zs:819-828) — the bare constant PSP_OFFHANDWEAPON = 1000000, not a PSprite.PSPLayers scope. The two suppressions it feeds are each menu-reachable in VR: Options > VR Options > Weapon...

**Files.** `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/playsim/p_pspr.h`, `src/r_data/models.cpp`, `src/common/rendering/hwrenderer/data/hw_vrwheel.h`

### Controller-anchored viewmodel transform
`[NOT IN UZD]` `[active]`

The viewmodel is positioned from the tracked controller, not the camera. For a flat psprite quad, AdjustPlayerSprites loads the hand's transform into the model matrix, scales the screen-space quad down into world units and re-centres it on the hand. For a 3D weapon model, RenderHUDModel starts from the view-to-world matrix and then overwrites it with GetWeaponTransform() for that hand, applying a 0.01 model-unit conversion; if no controller transform is...

**Why.** A weapon glued to the head does not work when the head and the hands move independently — you cannot aim it, reach with it, or bring it to your eye. Everything else in this subsystem (laser origin, bone anchoring, two-handed grip) is downstream of the gun being at the hand.

**Reached by.** No on/off toggle of its own — the override is unconditional whenever the active VR mode is an OpenXR or OpenVR mode (stock's plain VRMode struct has no AdjustPlayerSprites/GetWeaponTransform, so in mono nothing happens). Prerequisite: vr_mode must select OpenXR — Options > Display Options > VR3DMenu, the "3D mode" option (vr_mode, CVAR_ARCHIVE, default 0 =...

**Files.** `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/r_data/models.cpp`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `wadsrc/static/menudef.txt`

### Two scale conventions: 34 units/m world, 173.44 HUD
`[NOT IN UZD]` `[active]`

The engine runs two different unit conventions for held geometry. The world path scales by vr_vunits_per_meter (default 34.0). The HUD/psprite path arrives through a fixed chain instead — RenderHUDModel's 0.01 model-unit conversion for models, and AdjustPlayerSprites' 0.00125 (Vulkan OpenXR) or 0.000625 (GL OpenXR) times the weapon-scale cvars for the flat quad — which the in-tree notes record as working out to a HUD viewmodel scale of 173.44.

**Why.** This is the documented convention because a model authored for one path is wildly wrong on the other, and the failure is silent (a gun the size of a room, or invisible). The 0.01 in particular is a units conversion, not a placement term, and is easily lost — anchored models were once drawn at 100x because a loadMatrix discarded it, which...

**Reached by.** The convention itself is not a setting — it is what MODELDEF authors must match — but both ends have menu sliders. World path: Options > VR Options ("World scale", `vr_vunits_per_meter`, 1..60, wadsrc/static/menudef.txt:2047; CVAR default 34.0f at src/common/rendering/hwrenderer/data/hw_vrmodes.cpp:635). HUD path: the 0.01 model-unit conversion in...

**Files.** `VR_INTERACTION_PLAN.md`, `src/r_data/models.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### Mounted / portable HUD on a world-space quad
`[NOT IN UZD]` `[active]`

The whole 2D layer — status bar, alt HUD, automap, and every mod RenderOverlay handler — is redirected into an offscreen canvas (VRHudSurface) at a virtual screen size, then composited in the world pass as a textured quad carried on a controller transform. DrawHudQuad handles the compose (premultiplied-alpha blending for the translucent canvas, depth test on, depth write optional); DrawVRHudBorder draws an optional four-sided frame around the mounted...

**Why.** A screen-locked HUD in a headset is unreadable and nauseating. Painting it onto a panel you can raise and glance at makes it a physical object, and doing it by re-targeting the existing 2D drawer means every mod's overlay comes along without any of them knowing.

**Reached by.** Options > VR Options > "VR HUD/Automap Options" (menudef.txt OptionMenu VRHUDOptions, reached from OptionsMenu > VROptionsMenu). Fully menu-reachable, no console needed. HUD half: vr_hud_mount (On/Off, default true), vr_hud_mount_pos (Main Hand / Off Hand, default 1 = off hand), vr_hud_mount_scale, _yoffset (labelled "distance"), _xoffset, _zoffset, _pitch,...

**Files.** `src/d_main.cpp`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/common/rendering/hwrenderer/hw_draw2d.cpp`, `wadsrc/static/menudef.txt`

### Mounted-HUD paths gated on an actually-running stereo mode
`[EXTENDS]` `[active]`

The mounted-HUD and mounted-automap conditions in d_main.cpp now require vrmode->IsVR() as well as the vr_hud_mount / vr_automap_mount cvars, and the same guard was applied to the chat/message drawer. Without it, on a flat session those archived cvars default true, drawMountedHud came out true, drawFaceHud came out false, and the entire 2D layer was skipped during play — vanilla status bar and every mod overlay at once, because...

**Why.** The HUD was still being rendered, into a surface only the stereo devices composite, so on desktop it went to a texture nothing displays. It reappeared whenever a menu or console opened, which reads like a mod bug rather than an engine one. This is the correctness pair to the previous feature: the offscreen HUD surface only exists when...

**Reached by.** No user surface and no cvar of its own — the guard is unconditional code that runs every frame. It removes the need for a flat/desktop player to know vr_hud_mount exists; in an actual VR session (vr_mode non-zero) stereoActive is true and behaviour is identical to before, so for a VR-only player this changes nothing. The cvars it defers to, vr_hud_mount /...

**Files.** `src/d_main.cpp`, `HUD_STEREO_GATING.md`

### VR billboard modes for weapons that are still sprites
`[NOT IN UZD]` `[dormant]`

When a psprite has no model and a stereo mode is running, the flat weapon sprite is additionally built as world-facing geometry: two crossed quads (default), a back plane only, the crossed pair without the back plane, or twelve parallel slices swept across the sprite's width to give it thickness. Sprite UVs come from GetSpritePositioning and honour PSPF_FLIP and sprite mirroring; the extruded depth is capped at three times the sprite height.

**Why.** A weapon that has no mesh is otherwise a paper cutout floating at the hand, which is the single most obvious tell that the gun is not a real object. Crossing or slicing it gives it volume from any angle at essentially no authoring cost.

**Reached by.** Options > VR Options > VR Weapon Options, "Sprite Options" section: the "Sprite 3D" option (r_PlayerSprites3DMode; 0 Crossed, 1 Back Only = default, 2 Item Only, 3 Fat Item) and the "Fat Item Width" slider (gl_fatItemWidth, default 0.5, range 0.1-1.0) which applies to mode 3. Both are menu-reachable with no keyboard; menudef.txt:1791-1792, OptionValue...

**Files.** `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/rendering/r_utility.cpp`, `wadsrc/static/menudef.txt`

### Per-layer tint, glow, hide and skin-alpha control on the held weapon
`[NOT IN UZD]` `[active]`

Four independent controls, all read in the psprite draw path and all reaching a 3D weapon model, not just a sprite. psp.Tint multiplies the model's own skin (0xffffffff = off); psp.Glow is an additive term summed on top of the sector's own AdditiveColors and set before RenderHUDModel so the model inherits it; psp.NoDraw hides one layer in both prepare passes while the weapon keeps its states, damage, ammo and slot; and MODELDEF's IgnoreSkinAlpha drops...

**Why.** Stock's only colour input for a viewmodel is the player's fillcolor, gated behind STYLEF_ColorIsFixed — a style that flattens the model to a silhouette, so the only way to get colour was to throw the texture away. And nothing could hide a layer: psp.alpha is discarded by GetRenderStyle unless PSPF_ALPHA/PSPF_FORCEALPHA is set, and the...

**Reached by.** ZScript-only, no cvar and no menu entry — which is correct for VR (the mod drives it, not the player). Fields are native on the PSprite class in wadsrc/static/zscript/actors/player/player.zs (class opens at 3075): `native bool NoDraw` at 3105, `native Color Tint` / `native Color Glow` at 3190-3191, registered by DEFINE_FIELD(DPSprite, ...) at...

**Files.** `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/playsim/p_pspr.h`, `wadsrc/static/zscript/actors/player/player.zs`, `src/r_data/models.cpp`, `src/common/models/model.h`

### HUD bone anchoring, with the resolved bone published back to script
`[NOT IN UZD]` `[dormant]`

A psprite layer can be drawn at a named bone of another layer's model: AnchorLayer names the target layer, AnchorBone the bone, and AnchorOfs/AnchorAngles place it in that bone's own frame. The renderer writes the result back onto that psprite — AnchorBonePos (offset in the model's axes), AnchorBoneWorld (in the same frame as AttackPos/OffhandPos), AnchorBoneAngles (yaw/pitch/roll), and AnchorBoneLive. PreparePlayerSprites3D opens each frame with...

**Why.** psprite layers are otherwise fully independent — nothing can follow anything — so every "put this exactly here" problem (a hand on a grip, a magazine entering its well) was hand-tuned offsets per weapon, re-tuned whenever either mesh moved. Publishing the bone as a world position and orientation means a grab test is a plain distance...

**Reached by.** ZScript API only — no cvar, no menu entry, no bind. A mod sets PSprite.AnchorLayer (target layer id, must be LOWER than the anchored layer's id since psprites draw in id order) and PSprite.AnchorBone, optionally PSprite.AnchorOfs / PSprite.AnchorAngles (yaw, pitch, roll) to seat it on the bone, then reads back the readonly fields AnchorBonePos,...

**Files.** `src/playsim/p_pspr.h`, `wadsrc/static/zscript/actors/player/player.zs`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/r_data/models.cpp`

### Sprite billboarding and culling decided once for both eyes
`[EXTENDS]` `[active]`

A new FRenderViewpoint field, CenterEyePos, holds the camera position without the per-eye view shift. Every sprite decision that must not differ between eyes now reads it instead of Viewpoint.Pos: the camera-facing billboard basis in CalculateVertices, the rotation-frame angle and RF2_ANGLEDROLL backface test, the depth sort key, the RF_MAYBEINVISIBLE and too-close-to-camera rejections, the player-missile first-frame clip, and the voxel distance cull.

**Why.** With a per-eye position, each eye picks its own answer. A sprite one frame either side of a rotation-frame boundary shows a different frame to each eye; a billboard builds a slightly different quad per eye; a near-clip test can accept in one eye and reject in the other, so the object flickers in one eye only. All of those break stereo...

**Reached by.** Internal, no user surface. CenterEyePos itself has no cvar, menu entry, or ZScript exposure — it is set unconditionally in R_SetupFrame and read unconditionally by the sprite path, so it needs no reaching. (One of the seven substitutions, the billboard basis in CalculateVertices, only has an effect while gl_billboard_faces_camera is on — that cvar defaults...

**Files.** `src/rendering/hwrenderer/scene/hw_sprites.cpp`, `src/rendering/r_utility.h`, `src/rendering/r_utility.cpp`, `src/rendering/hwrenderer/scene/hw_portal.cpp`

### Laser sights and hitscan tracers as world geometry
`[NOT IN UZD]` `[dormant]`

Both are drawn in the world pass, after RenderTranslucent, as real tube geometry rather than as HUD elements. The laser traces from the controller transform (falling back to the actor's attack pose), builds a 16-segment tube in three passes — a straight bright core plus two tapered additive halos with distance-based falloff — and caps it with a camera-facing dot and concentric glow discs. Resting on a live shootable actor drives a "lock": the dot tightens...

**Why.** In VR, iron sights on a low-resolution sprite gun are not usable, so the laser is the aiming instrument rather than a decoration — which means it must be world geometry that occludes correctly and lands on the surface you are actually pointing at, and must originate from the controller rather than the head. The core-plus-halo...

**Reached by.** Options > Mod Options > "Laser Sight Options" (OptionMenu VRLaserSightOptions, menudef.txt:1823, linked at 4425) and "Hitscan Tracer Options" (VRHitscanTracerOptions, menudef.txt:2005, linked at 4426); the Mod Options submenu itself is on the top-level Options menu at menudef.txt:382. Both are fully menu-reachable in VR with no keyboard. Roughly forty...

**Files.** `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/playsim/p_hitscantracer.h`, `wadsrc/static/menudef.txt`

### Weapon-model dynamic lighting split from world sprites
`[EXTENDS]` `[dormant]`

The dynamic-light gate on the 3D psprite path was moved from gl_light_sprites to its own cvar, gl_light_weapons, keeping upstream's RF2_NODYNAMICLIGHTING actor guard alongside it. When enabled, the held model gets its own uploaded light list and additionally samples the nearest LightProbe at the player's position for an ambient tint.

**Why.** Lighting the held weapon per-pixel from the dynamic light list is one of the more expensive things a VR frame does, and it is the one the player is closest to and can most afford to lose. Tying it to gl_light_sprites forced the choice for the whole world at once.

**Reached by.** Two menu paths, both keyboard-free: Options > Display Options > Dynamic Lights (VideoOptions line 1387 -> GLLightOptions, toggle at menudef.txt:2464) and Options > VR Performance Tweaks (OptionsMenu line 383 -> VRPerfTweakMenu, toggle at menudef.txt:4558). Both are YesNo options on gl_light_weapons, default false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG. It also...

**Files.** `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `src/rendering/hwrenderer/hw_dynlightdata.cpp`, `wadsrc/static/menudef.txt`

## 06. VR physics layer and hitscan tracing

> Two fork-only playsim modules that UZDoom 5.0.0 has no counterpart for. `p_physics.cpp` is a complete ~2,800-line rigid-body simulator that runs OUTSIDE Doom's 35 Hz playsim tick — stepped once per `D_DoomLoop` iteration, which in VR free-runs at headset rate — so an object held in a tracked hand is positioned at 90 Hz instead of lagging the hand by up to 28 ms. It simulates in metres, collides against Doom's own sector planes and linedefs, and exposes a 13-call ZScript API for grabbing, two-handed holds and throwing. **It is dormant: `vr_physics` defaults to `false` and `P_PhysicsFrame()` early-returns and clears all bodies, so nothing runs until it is switched on from the menu.** The commit that restored it says plainly that it is no longer the direction, is kept intact and switchable rather than gutted, and can never be netplay-safe as written. `p_hitscantracer.cpp` is unrelated and...

### Frame-rate physics driver outside the playsim tick
`[NOT IN UZD]` `[dormant]`

`P_PhysicsFrame()` is called unconditionally from `D_DoomLoop`, between `TryRunTics()` and `D_Display()`, so it runs once per rendered frame rather than once per game tic — including on the many frames where `TryRunTics` advances zero tics. A fixed-timestep accumulator drains `dt` in `1.0/vr_physics_hz` increments (default 90, clamped 30-240), capped at `vr_physics_maxsteps` (default 4) catch-up steps per frame with the remainder discarded, so a throw...

**Why.** Doom's playsim is 35 Hz. An object held in a tracked hand and updated at 35 Hz lags by up to 28 ms, which reads as underwater and is the single thing that decides whether VR object handling feels real. `AActor::Tick` is not a usable per-frame hook here because in VR `TryRunTics` deliberately skips its wait (the HMD compositor owns...

**Reached by.** Master switch: cvar `vr_physics`, CVAR_ARCHIVE|CVAR_GLOBALCONFIG, **default false** (src/playsim/p_physics.cpp:102). It IS exposed in the menu as `Option "VR object physics", vr_physics, OnOff` at the top of OptionMenu "VRPhysicsMenu" (wadsrc/static/menudef.txt:2573), so the owner can turn it on in-headset. That menu is reachable two ways, both committed in...

**Files.** `src/playsim/p_physics.h`, `src/playsim/p_physics.cpp`, `src/d_main.cpp`, `src/maploader/maploader.cpp`, `src/p_setup.cpp`, `wadsrc/static/menudef.txt`

### Metric rigid-body solver over Doom's own sector planes and linedefs
`[NOT IN UZD]` `[dormant]`

`StepBody` integrates each free body in METRES on the playsim's axes (Z up) and converts only at the actor boundary via `MapToM`/`MToMap` against `vr_vunits_per_meter`. Contacts come from two live sources: every hull vertex tested against the body's own sector's `floorplane.ZatPoint`/`ceilingplane.ZatPoint` (so slopes and lifts work with no extra code), and against that sector's linedefs — one-sided or `ML_BLOCKING` lines block outright, and an ordinary...

**Why.** A VR fork needs objects that fall, land, tumble and stay where they land in real units, and there is no such thing in Doom — Doom moves actors as a cylinder with a step height and no orientation. Colliding against linedefs rather than `DoomLevelMesh` is called out explicitly: the render mesh encodes what is DRAWN, so it omits two-sided...

**Reached by.** Gated by `vr_physics` (CVAR_ARCHIVE, default **false**). `P_PhysicsFrame()` is called unconditionally each frame from d_main, but returns immediately when the cvar is off and CLEARS every registered body, so switching it off mid-session destroys the simulation rather than freezing it. Bodies themselves are created from ZScript (there is no map/actor flag...

**Files.** `src/playsim/p_physics.cpp`, `wadsrc/static/menudef.txt`

### Sleep decided by displacement, not velocity
`[NOT IN UZD]` `[dormant]`

A body is put to sleep only when it has actually MOVED less than 4 mm and rotated less than ~3 degrees from a latched reference pose for 0.3 s, while a `supportTimer` latched to a 0.2 s grace window says it is resting on something. Contact with another body counts as support exactly as a floor does. A sleeping body is skipped by the integrator entirely and is woken only by a genuine approach faster than 0.15 m/s, so two objects merely leaning on each...

**Why.** A body resting on a floor under a discrete solver is never at zero velocity: gravity adds g/rate every step (a measured, rock-steady 0.109 m/s at 90 Hz) and the contact only cancels it on the steps where the body has sunk far enough to overlap again. The file records that two earlier velocity-threshold attempts failed for exactly this...

**Reached by.** No direct user control over the sleep logic itself — it is internal to the solver and has no cvar of its own. It only runs at all when the whole physics module is enabled: Options > Physics Options (or VR Options > "Object physics") > "VR object physics" (cvar `vr_physics`, CVAR_ARCHIVE, default false). The four sleep gates are printed by the...

**Files.** `src/playsim/p_physics.cpp`

### PHYSDEF: convex-hull compound collision shapes shipped with the model
`[NOT IN UZD]` `[dormant]`

A `PHYSDEF` lump, found across every loaded pk3 and parsed once into a class-name-keyed library, gives an actor class a SET of convex hulls instead of a box. Each hull stores both a vertex list (`V x y z`, metres) and an outward face-plane list (`P nx ny nz d`) because the solver asks two different questions of a shape. Several hulls make a CONCAVE shape — the pair solver tests a vertex against every hull of the destination separately, so a point in the...

**Why.** A magwell is a cavity and no convex shape has a hole in it. Geometry-accurate magazine insertion — the whole point of physical reloading in VR — is impossible against a single box, and the box-versus-box version is just two volumes overlapping. Storing both representations rather than deriving one from the other is a stated per-step cost...

**Reached by.** Two things must both be true. (1) A pk3 must ship a PHYSDEF lump keyed on the actor class name — no cvar, no menu, no ZScript switch for the lump itself; it is parsed lazily on the first ApplyPhysDefShape call, which is reached only from the AActor.PhysicsEnable ZScript native (src/playsim/p_physics.cpp:2287). Results print to the log ("[PHYS] PHYSDEF: %d...

**Files.** `src/playsim/p_physics.cpp`

### Kinematic hand and held-weapon bodies driven from the controller pose
`[NOT IN UZD]` `[dormant]`

Two infinite-mass bodies track the player's controllers every frame from `AttackPos`/`OffhandPos` and the raw pose angles `AttackAngle`/`AttackPitch`/`MainHandRoll` (and the offhand trio), sized by `vr_physics_handsize`. Each hand carries a derived linear AND angular velocity computed from how far it actually moved since the last frame, so a fast swing swats an object hard and wakes a sleeping one, rather than teleporting through it while reporting zero...

**Why.** A hand that can be blocked by a wall either stops tracking the real hand or fights it, so it is deliberately non-colliding against the world while still shoving loose objects. Orientation is built from the pose angles and never decomposed from the VR hand transform matrix, because that matrix applies a mirror and a non-uniform...

**Reached by.** Menu only, no console needed. Master switch: Options -> "Physics Options" (promoted to the top of OptionsMenu, menudef.txt:377; also reachable from the VR/3D-mode menu at :2553) -> VRPhysicsMenu -> Option "VR object physics" (`vr_physics`), which is `CVAR(Bool, vr_physics, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)` — DEFAULT OFF, so nothing in this subsystem...

**Files.** `src/playsim/p_physics.cpp`, `wadsrc/static/menudef.txt`

### Engine-rate grabbing and two-handed support holds
`[NOT IN UZD]` `[dormant]`

`PhysicsGrab(hand)` captures the object's pose RELATIVE to the hand at the moment of the grab and maintains it, so you hold a thing however you happened to take hold of it rather than snapping it to a canonical grip. Carrying happens inside `UpdateHands` at physics rate, not from script. `PhysicsSetSupport(hand, worldPoint)` puts a second hand on a held object: the grab point is captured once in the body's own frame and the object is then rotated by a...

**Why.** A held object driven from ZScript is positioned at 35 Hz and is overwritten before it is ever drawn, which is exactly the failure this module exists to remove. Capturing the support point ONCE is load-bearing: a point re-measured every step never disagrees with where the hand is, and the whole leverage effect comes from that disagreement...

**Reached by.** ZScript only for the behaviour itself: PhysicsGrab(int hand), PhysicsSetSupport(int hand, double wx, double wy, double wz) (negative hand clears the support), PhysicsGetSupport(), PhysicsRelease(), PhysicsIsHeld(), PhysicsDistanceTo(x,y,z) — declared wadsrc/static/zscript/actors/actor.zs:1074-1084, bound in src/playsim/p_physics.cpp:2583-2771. hand: 0 =...

**Files.** `src/playsim/p_physics.cpp`, `wadsrc/static/zscript/actors/actor.zs`

### Peak-velocity throw release window
`[NOT IN UZD]` `[dormant]`

Each hand keeps a 16-sample rolling history of its linear and angular velocity — about 180 ms at 90 Hz. On release the peak is chosen by scanning that window for the fastest speed of the OBJECT (`s.vel + Cross(s.angVel * vr_physics_throwspin, r)`), not of the hand centre, and is applied scaled by `vr_physics_throwscale` (default 0.45). It is used only if it beats what the hand is doing right now AND clears `vr_physics_throwmin` (default 0.9 m/s), so...

**Why.** A person releases at the END of a throwing motion, by which point the arm is already braking hard — sampling at the release instant captures the slowdown and the object dribbles out and drops instead of arcing. But the naive peak-of-180ms rule is wrong for putting something down: any tracking jitter in the window becomes the release...

**Reached by.** ZScript native `PhysicsRelease()`, declared at wadsrc/static/zscript/actors/actor.zs:1080 and bound in p_physics.cpp:2710; no in-tree ZScript caller, so a mod must call it. Inert until the master switch is on: `Option "VR object physics", vr_physics, OnOff` (menudef.txt:2573) — with it off, P_PhysicsFrame early-returns and clears all bodies, so...

**Files.** `src/playsim/p_physics.cpp`, `wadsrc/static/menudef.txt`

### Speed-scaled impact sound and kinematic-pair contact haptics
`[NOT IN UZD]` `[dormant]`

A body landing plays a per-actor sound whose volume scales with the hardest approach speed among that step's contacts (full volume at roughly a 1.5 m drop), gated by a per-body minimum speed and rate-limited by an 80 ms cooldown so a settling box does not machine-gun. Separately, when two bodies that are BOTH kinematic and belong to different hands touch — your gun against your other palm, or two held weapons — the solver fires a controller haptic pulse...

**Why.** A magazine that lands silently gives you no idea where it went, and the whole point of it being a real object is that you can find it again. The haptic covers the one case where a push is physically impossible: neither side of a hand-versus-hand contact has finite mass because both positions come from tracking, so a buzz is the only...

**Reached by.** Impact sound: ZScript `native void PhysicsSetImpactSound(sound snd, double minSpeed = 0.6)` — declared at wadsrc/static/zscript/actors/actor.zs:1059, bound at src/playsim/p_physics.cpp:2407. A non-positive minSpeed silently falls back to 0.6 m/s. Contact haptics: fully automatic, no cvar of its own. Both are gated by `vr_physics`...

**Files.** `src/playsim/p_physics.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`

### ZScript physics API and the PHYSICSBODY actor flag
`[NOT IN UZD]` `[dormant]`

Thirteen natives on Actor let a mod turn an actor into a rigid body and drive it: `PhysicsEnable(massKg, halfX, halfY, halfZ, comX, comY, comZ)` / `PhysicsDisable` / `PhysicsAddImpulse` / `PhysicsAddSpin` / `PhysicsSetImpactSound` / `PhysicsSetHeld` / `PhysicsSetTransform` / `PhysicsGrab` / `PhysicsSetSupport` / `PhysicsGetSupport` / `PhysicsRelease` / `PhysicsIsHeld` / `PhysicsIsAsleep` / `PhysicsDistanceTo`. Mass is in kilograms and extents in metres —...

**Why.** The renderer lerps actors between `Prev` and `Pos()` by ticFrac with `Prev` reset each tic; a transform written at 90 Hz against a stale tic-boundary `Prev` rubber-bands, worst on whatever is held in front of your face — hence the forced no-interpolate. Real units are exposed rather than hidden because the solver runs in them and burying...

**Reached by.** Two halves, and they are reached differently. (1) The RUNTIME is menu-gated, not ZScript-gated: `CVAR(Bool, vr_physics, false, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)` at src/playsim/p_physics.cpp:102 defaults OFF, and `P_PhysicsFrame()` early-returns and clears all bodies while it is off. It is toggled from a headset at Options > Display Options > OpenGL Renderer...

**Files.** `src/playsim/p_physics.cpp`, `wadsrc/static/zscript/actors/actor.zs`, `src/scripting/thingdef_data.cpp`, `src/playsim/actor.h`, `src/playsim/p_physics.h`

### Physics diagnostics designed for a machine with no keyboard
`[NOT IN UZD]` `[dormant]`

`vr_physics_debug` prints a once-per-second summary to the log — frames/s, steps/s, dropped steps, min/max dt, tics elapsed, gamestate, paused/menu/VR/backend flags, and live-versus-asleep body counts with hands and weapons excluded so the interesting number can read zero. When anything is awake it also identifies the single QUIETEST body and prints which of the four sleep gates is blocking it, each labelled ok or BLOCKS. `vr_physics_trace` (0-20 Hz) adds...

**Why.** There is no console in a headset and no keyboard in play, so a log line is the only window into a body's behaviour. "Nothing ever sleeps" was not actionable on its own — it had already had two different causes in this file — so each gate names itself. Both are off by default because the report line every second in every session forever...

**Reached by.** Menu: OpenGL Options > "$GLPREFMNU_VRMODE" (stereo 3D / VR mode) > "Object physics" (OptionMenu "VRPhysicsMenu", Title "Object physics"), under a "Diagnostics -- written to the log file." heading: Option "Log summary" (`vr_physics_debug`, default false) and Slider "Trace (per sec)" (`vr_physics_trace`, default 0, clamped 0-20 by its CUSTOM_CVAR body). BOTH...

**Files.** `src/playsim/p_physics.cpp`, `wadsrc/static/menudef.txt`

### Hitscan tracers and deterministic ricochets
`[NOT IN UZD]` `[dormant]`

Every hitscan and railgun shot queues a travelling tracer streak: a start point, a unit direction, a distance and a lifetime derived from `vr_hitscan_tracer_speed`. For the local player in VR the origin is taken from the tracked CONTROLLER transform rather than the attack position, so the streak leaves the actual muzzle in the room instead of the player's eye. A start offset (`vr_hitscan_tracer_offset`, default 8 map units, per-weapon overridable) keeps...

**Why.** Doom hitscans are instantaneous and invisible — the only feedback is a puff at the far end. In VR, where you are aiming a physical object down a physical line, a visible round leaving the muzzle is what connects the gun in your hand to the hole in the wall. The hash-derived ricochet exists so the effect costs nothing in playsim...

**Reached by.** Menu only, fully reachable without a keyboard: Options > Mod Options (OptionMenu ModOptionsMenu, wadsrc/static/menudef.txt:4426) > the tracer submenu (OptionMenu VRHitscanTracerOptions, menudef.txt:2004). Gating cvars, all CVAR_ARCHIVE|CVAR_GLOBALCONFIG and declared in src/common/rendering/hwrenderer/data/hw_vrmodes.cpp:919-937: `vr_hitscan_tracer`...

**Files.** `src/playsim/p_hitscantracer.cpp`, `src/playsim/p_hitscantracer.h`, `src/events.cpp`, `src/playsim/p_map.cpp`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `wadsrc/static/zscript/actors/inventory/weapons.zs`

### TSQueue.h: a thread-safe queue and background-worker template
`[NOT IN UZD]` `[dormant]`

A fork-only header adding three generic primitives: `TSQueue<T>` (a mutex-guarded TArray with `queue`/`dequeue`/`clear`/`deleteSearch`/`dequeueSearch`/`size`), `ResourceLoader2<In,Out>` (a background std::thread that drains a primary and an optional secondary input queue through a virtual `loadResource`, pushes results to an output queue, sleeps 3 ms when idle, and self-reports total/avg/min/max load time), and a fixed-size `RingBuffer<T,N>`. In this tree...

**Why.** Included here because the brief asks about the physics layer's threading design, and the honest answer is that there is none: `P_PhysicsFrame` is entirely single-threaded on the main loop thread, and `p_physics.cpp` neither includes this header nor spawns a thread. The queue machinery is a separate fork addition serving asynchronous...

**Reached by.** Mostly dormant behind a menu toggle, not internal-only. The ResourceLoader2/TSQueue background loaders are created only inside `if (gl_texture_thread)` at vk_renderdevice.cpp:246, and that cvar defaults to FALSE (hw_cvars.cpp:40). It is exposed as a menu option, so it is VR-reachable: menudef.txt:4409 (QuickMenu) and menudef.txt:4505 (VRPerfTweakMenu),...

**Files.** `src/common/utility/TSQueue.h`, `src/common/rendering/vulkan/system/vk_renderdevice.h`, `src/common/rendering/vulkan/system/vk_renderdevice.cpp`, `src/playsim/p_pspr.h`

## 07. Models, IQM, voxels, and bone control

> UZDoom 5.0.0 draws a model whose frame is chosen by a sprite letter, whose transform is committed by the playsim tic, and whose bones nothing outside its own actor can see. That is workable for a monster in a room and useless for a gun in a hand: a VR fork needs to name a mesh frame directly (the sprite table caps at 29), to read the transform from a controller at draw rate rather than from a 35Hz tic, to publish where a weapon's bones actually are so a hand can be put on them, and to let one actor's model be pitched and rolled without editing a MODELDEF shared with every other copy. UZDXREMA adds all of that, plus a per-actor voxel override that turns a held sprite into a solid object, a set of natives that let script measure and place a model instead of guessing, and a Blender-to-IQM pipeline that verifies its output against this loader's actual limits. The working tree's model-side...

### Direct model-frame addressing (psprites and world actors)
`[EXTENDS]` `[active]`

Three fields — ModelFrame, ModelFrameNext, ModelFrameLerp — on both DPSprite and AActor replace the frame NUMBER the renderer would have resolved through the sprite table, leaving scale, offsets, skins and flags to still come from the normal FSpriteModelFrame lookup. ModelFrameLerp in 0..1 is taken verbatim as the interpolation factor, blending bone matrices between the two frames, and is applied after the gl_interpolate_model_frames / MDL_NOINTERPOLATION...

**Why.** A sprite letter index caps at MAX_SPRITE_FRAMES (29). The fork's rigged hand and weapon meshes run past 70 frames — the hand poses sit at 0-10 and 1289-1297 — so most of every reload, fire and grip pose had no letter that could name it. Posing a hand from controller input is picking a frame per tic, not playing an animation, so driving...

**Reached by.** ZScript API only, and writable (plain DEFINE_FIELD, not read-only): DPSprite.ModelFrame / ModelFrameNext / ModelFrameLerp, registered at src/playsim/p_pspr.cpp:182-184; Actor.ModelFrame / ModelFrameNext / ModelFrameLerp, registered at src/scripting/vmthunks_actors.cpp:2235-2237. ModelFrame < 0 restores stock sprite-table frame resolution; ModelFrameLerp < 0...

**Files.** `src/playsim/p_pspr.h`, `src/playsim/actor.h`, `src/r_data/models.cpp`, `src/r_data/models.h`, `wadsrc/static/zscript/actors/player/player.zs`, `wadsrc/static/zscript/actors/actor.zs`

### Native state -> model frame remap
`[NOT IN UZD]` `[active]`

DActorModelData carries a TMap keyed by FState pointer whose value packs (frame << 32 | next). ZScript registers rows once at bind time with RegisterModelStateFrame; from then on CalcModelOverrides resolves the frame from whichever state the psprite is currently in, and CalcModelFrame derives intra-state progress from the state's own tic countdown plus the render fraction, so the animation runs at display rate with no per-tick script....

**Why.** The model-swap program puts the fork's meshes on other mods' weapons, whose states and timings the fork does not own. Driving that from a ZScript tick means the mesh animation is quantised to 35Hz and re-derived every tic; making the weapon's own current state the clock removes the script from the loop entirely.

**Reached by.** ZScript API only, plus one console-only diagnostic. Actor.RegisterModelStateFrame(State st, int frameNum, int frameNext) -> bool (returns false while modelData is null, i.e. A_ChangeModel must have bound a model first, and also on null state or negative frame numbers) and Actor.ClearModelStateFrames(); enumeration helpers are the clearscope statics...

**Files.** `src/playsim/actor.h`, `src/playsim/p_actionfunctions.cpp`, `src/r_data/models.cpp`, `wadsrc/static/zscript/actors/actor.zs`

### HUD bone anchoring
`[NOT IN UZD]` `[active]`

A psprite layer can name another layer and a bone on it (AnchorLayer / AnchorBone); the renderer then discards that layer's controller placement and draws it from the target bone's transform, with the layer's own MODELDEF offsets, rotations and scale still applying on top relative to the bone. The target's basis is orthonormalised so the anchored model keeps its own size and mirroring instead of inheriting the target rig's, and the HUD unit conversion is...

**Why.** psprite layers are otherwise completely independent — nothing can follow anything — so every 'put this exactly there' problem (a hand on a grip, a magazine at the magwell, a shell at the loading port) was a hand-tuned per-weapon offset that broke whenever either model was rescaled. Publishing the resolved bone position is what turns a...

**Reached by.** ZScript API on DPSprite only — no cvar, no menu entry, no bind, and nothing in wadsrc/ uses it, so it stays inert until a mod sets the fields. Inputs: AnchorLayer (target psprite id), AnchorBone (bone name on that layer's model), AnchorOfs and AnchorAngles (a seat offset/rotation in the bone's frame, summed into the same MODELDEF translate/rotate calls...

**Files.** `src/r_data/models.cpp`, `src/playsim/p_pspr.h`, `src/playsim/p_pspr.cpp`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`, `wadsrc/static/zscript/actors/player/player.zs`

### World model rides a controller (FollowMainHand / FollowOffHand)
`[NOT IN UZD]` `[active]`

Two MODELDEF flags make a world actor's model take its base transform from VRMode::GetWeaponTransform for the named hand, read fresh at draw time, instead of from the actor's tic-committed position and Angles. It is the same function and the same matrix the HUD psprite path has always used, taken whole with no Euler round trip; the actor's own Angles are zeroed so the controller alone supplies orientation, and the model's MODELDEF scale, offsets and angle...

**Why.** Nothing in a world model's transform path ever referenced a controller — ObjectToWorldMatrix contains no reference to AttackPos, weaponangles or GetWeaponTransform — so moving a gun off a psprite and into the world did not track a hand badly, it had never been wired to one. The tic path is also 35Hz and stops entirely while a menu is...

**Reached by.** MODELDEF keywords FollowMainHand, FollowOffHand and NoAutoReverse inside a Model block (parsed at src/r_data/models.cpp:2147, :2151, :2404). No cvar, menu entry, bind or console command is involved, so it is fully reachable in a keyboard-less VR session. Secondary path: the bits are also settable at runtime from ZScript via...

**Files.** `src/r_data/models.h`, `src/r_data/models.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.h`

### Per-actor forced model angles (ForceModelAngles)
`[NOT IN UZD]` `[active]`

A boolean on AActor that ORs MDL_USEACTORPITCH and MDL_USEACTORROLL into a local copy of the model flags at draw time, so this one actor's pitch and roll are honoured without touching the shared FSpriteModelFrame. Nothing leaks to anything else drawn from the same MODELDEF definition, and it is serialized with the actor.

**Why.** Pitch and roll are opt-in per MODELDEF while yaw is not, so an actor that BORROWS another class's model via A_ChangeModel — a holstered weapon is exactly that, a prop wearing the real weapon's model — could be turned in yaw and had its pitch and roll silently discarded. Setting the flags on the borrowed definition is not an option: it is...

**Reached by.** ZScript field only: Actor.ForceModelAngles (native bool, play-scope read/write via DEFINE_FIELD, serialized in AActor::Serialize). No cvar, no menu entry, no bind, no console command — a mod sets it, the player never touches it, so there is nothing unreachable for a VR player here.

**Files.** `src/playsim/actor.h`, `src/r_data/models.cpp`, `src/scripting/vmthunks_actors.cpp`, `src/playsim/p_mobj.cpp`, `wadsrc/static/zscript/actors/actor.zs`

### Per-actor voxel override, held-voxel orientation, and voxel distance culling
`[EXTENDS]` `[active]`

VoxelOverride on an actor draws it as its voxel regardless of r_drawvoxels and in preference to any model — two deliberate inversions of the normal rule, since the case it exists for is a pack loaded with the cvar off and an object that carries a MODELDEF. The voxel lookup was lifted out of FindModelFrameRaw into FindVoxelFrame so the override can reach it without the cvar gate. A held voxel also gets pitch and roll forced on (no voxel pack in the wild...

**Why.** Voxel selection is keyed on a sprite frame and gated by one global cvar, so there was no way to ask for a voxel on a single object. A billboard cannot be turned over in your hand; a voxel can — so a grabbed item wants to be a voxel for exactly as long as it is held and a sprite again the moment it is dropped. The distance limit exists...

**Reached by.** ZScript field `Actor.VoxelOverride` (native bool, wadsrc/static/zscript/actors/actor.zs:377, bound at src/scripting/vmthunks_actors.cpp:2280, serialized in src/playsim/p_mobj.cpp:418) — set on grab, clear on release; writable from play scope, no menu or bind. Menu: Options -> Mod Options -> Voxel Options (OptionMenu "VRVoxelOptions", reached from...

**Files.** `src/r_data/models.cpp`, `src/playsim/actor.h`, `src/rendering/hwrenderer/scene/hw_sprites.cpp`, `src/rendering/r_utility.cpp`, `wadsrc/static/menudef.txt`, `src/common/models/model.h`

### Mod-owned model placement (PlacementCVars and the hand-frame offsets)
`[NOT IN UZD]` `[active]`

A MODELDEF block can name a cvar prefix with PlacementCVars; the renderer then reads <prefix>_ofs_x/_y/_z, _yaw/_pitch/_roll, _scale and _scale_x/_y/_z live every frame and sums them into the SAME translate, rotate and scale calls the MODELDEF values use — on both the HUD and the world path, kept in step. Three further keywords, HandAngleOffset / HandPitchOffset / HandRollOffset, are applied after the model has been oriented and are summed with the live...

**Why.** Summing inside the existing matrix calls is what makes a slider value and a MODELDEF value genuinely interchangeable: rotations do not commute, so a value dialled in live can only be baked into the keyword verbatim if it was added at the same point in the chain. The hand triple is separate from angleoffset/pitchoffset/rolloffset...

**Reached by.** MODELDEF keywords, all opt-in per model definition: PlacementCVars <prefix> (src/r_data/models.cpp:2103), HandAngleOffset / HandPitchOffset / HandRollOffset (:2126-2139), UseHandOffsets (:2395, sets MDL_USEHANDOFFSETS), IgnoreSkinAlpha (:2399, sets MDL_IGNORESKINALPHA). The <prefix>_ofs_x/_y/_z, _yaw/_pitch/_roll, _scale, _scale_x/_y/_z cvars are the mod's...

**Files.** `src/common/models/model.h`, `src/r_data/models.cpp`, `src/rendering/hwrenderer/scene/hw_weapon.cpp`

### Model measurement and placement introspection from ZScript
`[NOT IN UZD]` `[active]`

A family of natives that let script ask the engine what a model actually is and where it actually is. Level.GetModelOrientationHint returns whether a MODELDEF Scale mirrors the model plus its baked angle/pitch/roll offsets; GetModelOffsetHint and GetModelWorldOffset return its baked position offset, the latter by building the SAME VSMatrix with the SAME rotate calls RenderModel makes rather than reconstructing the basis in script; GetModelBoundsHint...

**Why.** Script had no way to see any of this, and every consumer was guessing. Mirroring is a per-model authoring choice with no correlation to which hand a weapon is held in, so one script-side 'flip 180' guess could only ever be right for part of the arsenal. Sizing every holstered weapon with one flat multiplier makes a BFG and a pistol the...

**Reached by.** ZScript API only, and no cvar or menu gates any of it: Level.GetModelOrientationHint / GetModelOffsetHint / GetModelWorldOffset / GetModelBoundsHint / GetActorModelClass (doombase.zs 1014-1071), and Actor.ModelPointToWorld / Actor.FindBoneIndex (actor.zs 1840, 1848). Actor.hasmodel is exported as a native readonly field (actor.zs:218, DEFINE_FIELD at...

**Files.** `src/scripting/vmthunks.cpp`, `src/r_data/models.cpp`, `src/playsim/p_actionfunctions.cpp`, `src/common/models/model.h`, `wadsrc/static/zscript/doombase.zs`, `wadsrc/static/zscript/actors/actor.zs`

### IQM bone evaluation corrections
`[EXTENDS]` `[active]`

In both branches of IQMModel::CalculateBonesOnlyOffsets the per-bone TRS is now seeded from Joints[i] (the bind pose) instead of being default-constructed to identity before being multiplied by inversebaseframe[i]. RenderModelFrame also falls back to FModel::GetBasePose() when no bone data resolved, so a MDL_MODELSAREATTACHMENTS model that is not decoupled still gets a bone set uploaded instead of rendering unskinned.

**Why.** With an identity seed every bone came out displaced by the inverse of its own bind pose, so the moment anything asked for a bone the whole model was transformed by that inverse — and how badly depended entirely on what the bind pose happened to be, which is why it presented as a different bug on every model. A rig whose root carries a...

**Reached by.** Internal, no user surface. Both changes are unconditional code paths with no cvar, menu entry, bind, or ZScript API. The bind-pose seeding applies to every IQM model whose bones are evaluated through CalculateBonesOnlyOffsets; the GetBasePose fallback fires only when the render reaches the MDL_MODELSAREATTACHMENTS-or-decoupled branch with no bone data...

**Files.** `src/common/models/models_iqm.cpp`, `src/r_data/models.cpp`

### Off-thread model geometry loading
`[EXTENDS]` `[dormant]`

FModel gains a LoadState (NONE / LOADING / READY), a GetLumpNum accessor, and a LoadGeometry(FileSys::FileData*) overload implemented by the IQM, MD2/DMD and MD3 loaders so geometry can be decoded from a buffer a worker thread already read. VulkanRenderDevice::BackgroundLoadModel queues a model on first sight, the worker decodes it, and the main thread finishes the upload. The synchronous LoadGeometry() paths gained early-outs so a model that already has...

**Why.** Model decode used to happen on the main thread at the moment a model was first drawn, which in a headset is a visible hitch — and a voxel or decoration pack means hundreds of those. This is the model-side half of the fork's background asset streaming; the actor's lastModelSprite/lastModelFrame fields on AActor exist so a repeat draw of...

**Reached by.** Two cvars, both must be on, and it is Vulkan-only. Master gate: gl_texture_thread (src/common/rendering/hwrenderer/data/hw_cvars.cpp:40, CVAR(Bool, gl_texture_thread, false, GLOBALCONFIG|ARCHIVE)) — default OFF. Menu: Options > VR Options, "$VRPREFMNU_TEXTURE_THREAD" (wadsrc/static/menudef.txt:4409 and :4505), but that line sits inside an ifOption(OpenXR)...

**Files.** `src/common/models/model.h`, `src/common/models/model.cpp`, `src/common/rendering/vulkan/system/vk_renderdevice.cpp`, `src/common/models/models_iqm.cpp`, `src/common/models/models_md3.cpp`, `src/rendering/hwrenderer/scene/hw_sprites.cpp`

### fbx2iqm: a deterministic FBX->IQM build and verification pipeline
`[NOT IN UZD]` `[active]`

A single Python script with two modes. BUILD runs inside Blender from a JSON config: it never calls transform_apply, instead left-multiplying every participating object by one common matrix so mesh and armature stay in the same frame; it picks the scale so the armature's world scale lands on exactly 1.0 (jointData quantises joint scale to 1/65536, and 0.01 writes as 0.0099945); it forces a depsgraph update and re-reads matrix_world after every parent or...

**Why.** Every rule in it traces to a failure this engine produces silently. The verifier enforces this loader's own limits by citation: version must be exactly 2; num_text 0 makes the model never load; only POSITION float3, TEXCOORD float2, NORMAL float3, BLENDINDEXES ubyte4/int4 and BLENDWEIGHTS ubyte4/float4 avoid I_FatalError; a joint whose...

**Reached by.** Command line only, outside the game — an offline authoring tool, so the VR/no-keyboard constraint does not apply. BUILD (needs Blender): blender -b --factory-startup --python tools/fbx2iqm/fbx2iqm.py -- --config <cfg.json>; the script dies with that exact invocation printed if bpy is unavailable. VERIFY (stdlib only, no Blender): python...

**Files.** `tools/fbx2iqm/fbx2iqm.py`, `tools/fbx2iqm/beretta_m9.json`, `tools/fbx2iqm/hand_left.json`, `tools/fbx2iqm/hand_right.json`

### Model diagnostics
`[NOT IN UZD]` `[dormant]`

Four throttled traces on the model path. vr_validate reports, once per model, a bone-weighted mesh with zero frames (which collapses and reads as missing geometry) and a skin carrying an alpha channel without IgnoreSkinAlpha set (which alpha-tests most of the model away and reads as half transparent). vr_spatialreport prints each drawn psprite layer's final position, scale, frame and whether it is anchored — per layer, not one shared timer, because a...

**Why.** Every one of these presents as something misleading in a headset, where there is no way to inspect state: a pose that never applied looks exactly like a pose that was never set, and 'tiny and far away' looks identical to 'correctly sized but mispositioned' while being an entirely different bug. In the working tree all three of...

**Reached by.** Console-only, with no menu entry for any of them — verified absent from wadsrc/static/menudef.txt (only the unrelated vr_voxel_rollaxis has a slider there). In a headset with no keyboard these are effectively unreachable at runtime; the practical route is to flip a default in source, or read the log after the fact. Committed HEAD defaults: vr_pose_debug,...

**Files.** `src/r_data/models.cpp`, `src/playsim/p_actionfunctions.cpp`

## 08. Custom shaders and the visual layer

> Almost all of this fork's rendering work lives in one file: `wadsrc/static/shaders/glsl/main.fp` grew from ~800 lines to 3543, and everything added is a term evaluated per fragment in world space rather than an object spawned in the map. That single choice is what the whole layer is built on — a glow, a laser, a fog bank or a burn mark asks "how far is this pixel from a plane / a segment / an axis / a point", so it wraps across floor, wall and ceiling as one continuous thing with no geometry, no actors and no decals. Seven fork-only billboard payload shaders do the same trick in quad space with signed distance fields, and three postprocess passes (a marched flashlight cone, a reworked bloom, a floor heatmap) sit on top. Almost none of it has an engine menu: these are ZScript APIs on `LevelLocals` and `Sector`, and the mods that consume them supply their own VR-reachable menus.

### Four-lane surface glow
`[EXTENDS]` `[active]`

Every wall carries a glow growing up from its floor line and down from its ceiling line, and every floor and ceiling glows inward from its own linedef edges — a surface stock GZDoom's glow never touches. Each of the four lanes has a near colour, an optional far colour it ramps toward across its reach, one of four falloff curves (linear, squared, sqrt, exponential) and an intensity multiplier that is independent of reach. A single side can override its...

**Why.** The fork exists for dark-room lighting mods played in VR. A sector's colormap is one flat value wall to wall, so a large room reads as uniformly dim; a per-pixel ramp with a far colour is what lets a wall and the floor it meets arrive at their shared corner as one continuous gradient instead of two flat colours meeting at a hard edge.

**Reached by.** ZScript API only; no cvar, no engine menu, no console command (so nothing here is keyboard-gated for VR — but the engine ships no UI, the consuming mod must supply one). Sector: SetGlowColor / SetGlowColorFar / SetGlowHeight / SetGlowFalloff / SetGlowIntensity for the two wall lanes, and SetFlatGlowColor / SetFlatGlowColorFar / SetFlatGlowHeight /...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/mapdata.zs`, `src/gamedata/r_defs.h`, `src/rendering/hwrenderer/scene/hw_flats.cpp`, `src/common/rendering/hwrenderer/data/hw_renderstate.h`

### Glow wave and texture inside the glow
`[NOT IN UZD]` `[dormant]`

Two modulators layered on the glow lanes. The wave gives a lane the axis it never had — a signed sine, measured from its own origin using the same five distance shapes the sweep uses, driving the lane's REACH (so the lit edge itself rises and falls), its brightness, and where the near/far colour boundary sits, with a per-room seed so rooms do not undulate as one organism. The texture set works inside the lit area instead: 3D value noise, a flow band...

**Why.** A glow varies going up a wall and is one flat value going along it, so a wall faded beautifully top to bottom and had a dead straight edge from one end of the room to the other. Once reach is turned up far enough to saturate the surface the wave has nothing left to move, which is why the second set works on substance rather than shape.

**Reached by.** ZScript API only — there is no cvar, no menudef entry, and no in-tree caller, so nothing is player-reachable in VR until a mod drives it. Level.SetGlowWave / SetGlowWaveDepth(reach, bright, colour, detune, seed) / SetGlowWavePhase / ClearGlowWave and Level.SetGlowTexture / SetGlowFlow / SetGlowCells / SetGlowReact are clearscope, so a menu could drive them...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/doombase.zs`, `src/g_levellocals.h`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`

### Per-fragment darkness
`[NOT IN UZD]` `[dormant]`

Four darkening curves (subtract, compress, cap brightest, deepen shadows) applied to the fragment's own light rather than to a sector's colormap byte, plus two terms a sector could never express: darkness deepening with distance from the eye, and darkness pooling below a height reference. It runs after the lighting equation but before every emissive term, and is skipped for 2D draws and for fullbright sprites.

**Why.** A darkness mod that scales each sector's colour makes a large room uniformly dim and then has to multiply light back UP to reveal anything — two features undoing each other. Placing the curve before the glow, beams, sweep and stamps is what lets emissive light survive a room crushed to black, which is the entire design of the lighting...

**Reached by.** ZScript API only — no cvar, no menu entry, no bind. Level.SetDarkness(mode, adjust, minLight, preGain, postGain), Level.SetDarknessSpace(distDepth, distRange, heightDepth, heightRef, heightRange), Level.ClearDarkness (declared native clearscope in wadsrc/static/zscript/doombase.zs:1154-1156, thunks at src/scripting/vmthunks.cpp:3892/3922/5118). Mode 0 is...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/doombase.zs`, `src/common/rendering/hwrenderer/data/hw_renderstate.h`, `src/rendering/hwrenderer/scene/hw_sprites.cpp`

### Real beams (lasers as line segments)
`[NOT IN UZD]` `[active]`

Up to 128 beams, each a start and end point with a hot core width and a soft halo width. Two passes: one lights nearby SURFACES by their distance from the segment, the other solves the closest approach between the view ray and the segment so the beam is visible hanging in open air — depth-correct for free, because the solve returns a distance along the ray to compare against the fragment. Adds taper toward the muzzle, energy scrolling along the length, an...

**Why.** A Doom laser is a sprite (lights nothing) or a chain of puffs (stitches, gaps at range, an actor per segment). A segment distance field is continuous at any length, wraps floor/wall/ceiling unbroken, lights what it passes because it is near it, and writes past white so it feeds bloom without a light, a sprite or a quad.

**Reached by.** ZScript API only — no cvar, menu entry or bind turns it on. Level.SetBeamCount(count, glow, fogScatter), Level.SetBeam(index, start, end, thick, soft, col, intensity), Level.SetBeamLook(airGlow, scrollSpeed, scrollDepth, taper, flare), Level.ClearBeams — all native clearscope on FLevelLocals, declared at wadsrc/static/zscript/doombase.zs:1360-1365, thunked...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/doombase.zs`, `src/g_levellocals.h`, `src/p_tick.cpp`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`

### Sweep bands, lattice fill and the air lattice
`[NOT IN UZD]` `[dormant]`

Up to eight travelling bands of light, each with its own origin and one of five distance shapes (cylinder/ring, plane along X, plane along Y, sphere/shell, rising sheet), and one of four draw modes: ADD emits light, LIFT multiplies what is there (a reveal that works in a darkened room), CRUSH is travelling darkness, RECOLOUR retints the glow lanes as it passes. Inside a band a procedural lattice can be drawn — grid, dots, solid slab or room-measured...

**Why.** A wall of light crossing a corridor cannot be geometry: it has to wrap every surface it touches and stay continuous. Drawing the lattice as a repeating function of the ray/plane crossing rather than as beams means four bars by four and four hundred by four hundred cost the same, where the beam system caps at a fence.

**Reached by.** ZScript API only — no cvar, no menu entry, no bind, so it is unreachable in a bare game and only exists when a mod drives it each tic. Gating is two-level: the renderer uploads nothing unless Level.SweepMode > 0 AND Level.SweepCount > 0 (hw_drawinfo.cpp:1479), so SetSweepOrigin(mode>0, origin, count) must be called at least once even if every band later...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/doombase.zs`, `src/g_levellocals.h`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`

### Fog slab, tornado, tendrils and reactive mist
`[NOT IN UZD]` `[dormant]`

A horizontal layer of participating medium with a real world-space top AND bottom, solved analytically (no raymarch) from how much of the eye-to-fragment ray lay inside it. It can follow the floor or ceiling plane per fragment so it walks a staircase, swell on two interfering sine waves, repeat vertically as a rolling stack, carry a second colour across its own thickness, thin in a stretched wake behind the player, vary by drifting noise, pile against a...

**Why.** Sector fog is a distance tint on surfaces: it has no ceiling, no thickness you can stand in, and no way to be brighter where a light passes through it. Everything here is what "knee-deep mist" actually requires, and it is closed form rather than marched so it costs a handful of ALU instead of a step count.

**Reached by.** ZScript only — no cvar, no menu entry, no bind anywhere in the engine. All methods live on LevelLocals (doombase.zs:1162-1190, 1312-1315), so callers write Level.SetFogSlab / SetFogBottom / SetFogSurface / SetFogFollow / SetFogGradient / SetFogNoise / SetFogPickup / SetFogWakeMotion / SetFogBow / SetFogTendrils / ClearFogSlab, plus SetTornado /...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/doombase.zs`, `src/g_levellocals.h`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`

### Volumetric flashlight cone
`[NOT IN UZD]` `[dormant]`

A postprocess pass that lights the AIR rather than a surface: an analytic ray/cone intersection bounds a jittered march through the cone, with radial and axial falloff, world-space two-octave dust that stays put as you sweep the torch, and a fade for the degenerate case where the cone is aligned with the view and would otherwise be a bloom-fed halo over the crosshair. Works in view space, so each eye and each portal resolves its own matrix and stereo...

**Why.** Everything else in the engine lights surfaces, so a torch gives you a bright disc where it lands and nothing in between. There is no geometry to hang a cone on, which is why this has to be a postprocess pass.

**Reached by.** ZScript API only: Level.SetVolumetricBeam(pos, dir, col, inner, outer, length, density, falloff, dust, dustScale, dustDrift) and Level.ClearVolumetricBeam(), declared at wadsrc/static/zscript/doombase.zs:1106-1107. A mod must republish the beam each tic; nothing in wadsrc calls either function. Quality and the axis fade are tuning-only cvars, not gates:...

**Files.** `wadsrc/static/shaders/pp/volumetricbeam.fp`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h`, `src/scripting/vmthunks.cpp`

### Bloom rework
`[EXTENDS]` `[active]`

Bloom is on by default here, and the extract pass gained an adjustable threshold (was hardcoded at 1.0), a soft knee that rolls the transition in with a quadratic instead of snapping, and brightness judged by the strongest channel so a saturated colour blooms on its own terms. The combine pass gained a colour tint and radial chromatic fringing (applied only on the final combine, never on the shared downscale steps), and the blur gained an anamorphic mode...

**Why.** Every emissive system in this fork — glow lanes, beams, SDF payloads with their emissive push past 1.0 — depends on bloom to read as light rather than as paint, so shipping bloom off ships the headline feature broken. A hard threshold makes pulsing glow and sweeping beams pop in and out of blooming constantly, which is the first thing...

**Reached by.** Options > Display > Post-processing (OptionMenu "PostProcessMenu", wadsrc/static/menudef.txt:2631) exposes only the on/off toggle gl_bloom. Every dial is CVAR_ARCHIVE with no menu entry and no bind: gl_bloom_threshold, gl_bloom_knee, gl_bloom_anamorphic, gl_bloom_anamorphic_ratio, gl_bloom_tint_r/g/b, gl_bloom_chromatic. In a headset with no keyboard they...

**Files.** `wadsrc/static/shaders/pp/bloomextract.fp`, `wadsrc/static/shaders/pp/bloomcombine.fp`, `src/common/rendering/hwrenderer/postprocessing/hw_postprocess_cvars.cpp`, `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp`, `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.h`

### Surface stamps
`[NOT IN UZD]` `[in flight]`

Sixteen slots of emissive shape painted onto whatever real surface is at a world point, blooming out of an impact and dying on an engine-aged clock. Thirteen shapes (pool, oriented bar, jagged gouge, expanding ring, flipping hex field, rotating hex and box rings, spiral, five-lobed star, twelve-spoke sunburst, checker grid, photo-negative flash, oriented box) plus an optional second surface-texture layer of six (breathe, sweeping band, rotating checker,...

**Why.** The system it replaces measured horizontal distance only, so a ring on a wall flattened into a vertical stripe and needed a whole separate hand-written wall pattern set. It also made every publisher re-push its stamp and compute its own animation phase every tic, so a script that missed a tic dropped its effect mid-bloom.

**Reached by.** ZScript only, plus a console command. The API is on Level in wadsrc/static/zscript/doombase.zs:1357-1358 — `native clearscope void SpawnSurfaceStamp(int shape, Vector3 pos, double radius, color col, int life, Vector3 axis, int tex = 0, double texStr = 0.0)` and `native clearscope void ClearSurfaceStamps()` — thunked at src/scripting/vmthunks.cpp:4994-5025....

**Files.** `wadsrc/static/shaders/glsl/func_surfacestamps.fp`, `src/g_levellocals.h`, `src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h`, `src/p_tick.cpp`, `src/playsim/p_user.cpp`, `src/scripting/vmthunks.cpp`

### Signed-distance billboard payload shaders
`[NOT IN UZD]` `[active]`

Seven fork-only material shaders that draw a billboard's face as a distance field solved per pixel instead of a sampled texture: SDF text from an offline atlas, a sixteen-segment display built from capsule distances with LED and LCD polarities, a glowing seam that opens, a transcribed kill badge that punches its digits out of its own plate in one pass, an SDF rounded panel, a pointy-top tessellating hexagon, and an N-pointed star that can be filled or...

**Why.** A sampled plate blurs when a VR player walks up to it and structurally cannot glow, because a halo has to read the field OUTSIDE the shape and a texture has no field. Hexagons make that unavoidable: neighbouring cells share edges, and two half-pixel-soft edges do not meet, they seam.

**Reached by.** ZScript only, by picking a payload on the billboard API: LevelLocals.BB_TEXT / BB_SEGMENT / BB_SEGLCD / BB_SEAM / BB_WG13 / BB_SDFPANEL / BB_SDFHEX / BB_SDFSTAR passed to AddBillboard / AddBillboardPersistent / AttachBillboard, shaped by SetBillboardGlow, SetBillboardGradient, SetBillboardProgress, SetBillboardFont and the BBFL_VOID flag (all declared in...

**Files.** `wadsrc/static/shaders/glsl/func_sdfpanel.fp`, `wadsrc/static/shaders/glsl/func_sdfhex.fp`, `wadsrc/static/shaders/glsl/func_sdfstar.fp`, `wadsrc/static/shaders/glsl/func_segment.fp`, `wadsrc/static/shaders/glsl/func_sdftext.fp`, `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp`

### Shapes and standing shapes
`[NOT IN UZD]` `[dormant]`

128 slots of signed-distance shape (circle, ring, box, box outline, cross, hexagon, triangle) drawn either as a decal in the plane of whatever surface the fragment belongs to, or — orientation 3 — freestanding in open air on its own yaw/pitch/roll plane, found by intersecting the view ray with that plane. Slots carry an in-plane rotation, a widening seam that subtracts a slab and reveals a second colour masked by the original shape, radial and grid repeat...

**Why.** Marks that a mod wants to burn onto a floor or hang in a doorway need to conform to slopes and stairs, and to compose (min is union, max is intersection, abs is an outline) in ways a mesh cannot. Repeat modes exist so a ring of eight and a field of eight hundred cost one distance test.

**Reached by.** ZScript only, on Level: AddShape / MoveShape / SetShapeMotion / SetShapeRepeat / SetShapeOrient / LinkShape / RemoveShape / ClearShapes / SetShapeLook (declared wadsrc/static/zscript/doombase.zs:1222-1274, bound in src/scripting/vmthunks.cpp ~4340-4650). There is NO cvar and NO menudef entry anywhere for shapes, so nothing is reachable by the owner in VR...

**Files.** `wadsrc/static/shaders/glsl/main.fp`, `wadsrc/static/zscript/doombase.zs`, `src/g_levellocals.h`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`

### Kill heatmap and selective desaturation (both flagged for removal)
`[NOT IN UZD]` `[dormant]`

Two scene-wide tint systems. The heatmap accumulates a 256x256 grid over the map's bounding box, stores the height that deposited each cell's heat, and a postprocess pass reconstructs world position from depth to paint it on the floor (rejecting sky and anything too far above the recorded height). Selective desaturation weights the drain by each colour's own HSV saturation with an optional dominant-channel hue gate, so a monochrome world keeps blood and...

**Why.** Recorded here for completeness because both are fork-only and both are still in the tree. THE OWNER HAS ASKED FOR BOTH TO BE REMOVED — the heatmap as "implemented incorrectly", selective desaturation as broken, and the request predates this catalogue by at least three sessions. Do not present either as an available capability or build on...

**Reached by.** ZScript API only — no cvar, no menu entry, no console command for either. Heatmap: Level.SetHeatmap(scale, low, high, ceiling, decay, tolerance), Level.HeatmapAdd(x,y,z,radius,amount), Level.HeatmapClear(), Level.HeatmapAt(x,y) (declared wadsrc/static/zscript/doombase.zs:1306-1309). Desaturation: Level.SetDesatKeep(threshold, soft, hue) and...

**Files.** `wadsrc/static/shaders/pp/heatmap.fp`, `wadsrc/static/shaders/glsl/main.fp`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/g_levellocals.h`, `src/common/rendering/hwrenderer/postprocessing/hw_postprocess.cpp`

## 09. SDF font and text rendering

> UZDoom draws text from bitmap fonts, which is correct for a HUD (a glyph is always the same size on screen) and wrong for VR, where a label is a real object in the world that the player can walk up to and read at a foot away. UZDXREMA adds a complete signed-distance-field text stack that stock UZDoom has nothing resembling: an offline PowerShell atlas generator that turns a TTF outline into a distance-field PNG plus a plain-text metrics lump, an engine-side font loader and per-game shuffled font roster, a shared multi-line layout definition used identically by the renderer and by script-facing measuring natives, and a material shader that reconstructs the glyph edge per pixel and reads its neon halo straight out of the same field with no blur pass. Everything is reached through the BB_TEXT billboard payload and its LevelLocals natives; the only cvar is bb_sdffont, which has no menu...

### Distance-field text on world billboards (BB_TEXT)
`[NOT IN UZD]` `[active]`

The BB_TEXT billboard payload draws its string out of a signed-distance atlas instead of the engine's bitmap font. Each glyph is still an ordinary textured quad, but the texture holds distance-from-edge rather than ink, so a label stays sharp at any magnification and at any viewing angle. Arbitrary strings, no length or alphabet limit.

**Why.** A billboard in VR is a physical object the player walks toward. A bitmap glyph magnifies its hard ink/no-ink boundary along with itself and goes blocky up close -- readable across the room, mush at arm's length, which is exactly the range VR text is read at.

**Reached by.** ZScript only, no menu path and no bind. Payload constant LevelLocals.BB_TEXT (= 6) passed to Level.AddBillboard / AddBillboardPersistent / AttachBillboard with the trailing `text` argument (declared wadsrc/static/zscript/doombase.zs:767-771, thunked src/scripting/vmthunks.cpp:2963-3049). Live restring via Level.SetBillboardText(id, text)...

**Files.** `src/rendering/hwrenderer/scene/hw_sprites.cpp`, `src/rendering/hwrenderer/scene/hw_sdffont.h`, `src/g_levellocals.h`, `wadsrc/static/zscript/doombase.zs`

### SDF text shader with measured antialiasing and field-derived neon halo
`[NOT IN UZD]` `[active]`

func_sdftext.fp reconstructs the glyph edge from the sampled field, antialiases it using fwidth(d) rather than a scale uniform, and derives a glow halo directly from the same distance value. Registered as its own material shader slot (SHADER_SDFText) against material_nolight, so world text is emissive rather than lit by the room. Glow reach and strength ride in uAddColor.r/.g, which the billboard path packs and no other consumer of that uniform reads.

**Why.** fwidth measures the actual magnification per pixel, so the antialiased band stays exactly one pixel wide whether the card is across the room or filling the headset -- and stays correct across a quad seen at a steep angle, where a per-draw uniform would be wrong over most of the surface. The halo needs no blur pass or second texture...

**Reached by.** No user action needed: automatic for BB_TEXT whenever a field atlas resolves, and the fork ships the default one (cvar bb_sdffont = "sdfmono", with wadsrc/static/graphics/sdfmono.png and wadsrc/static/sdffonts/sdfmono.txt both fork-added), so the field path is the default and the bitmap glyph path is only a missing-atlas fallback. BB_TEXT billboards...

**Files.** `wadsrc/static/shaders/glsl/func_sdftext.fp`, `src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp`, `src/common/textures/textures.h`, `src/rendering/hwrenderer/scene/hw_sprites.cpp`

### Offline TTF-to-atlas generator (tools/sdffont/mksdf.ps1)
`[NOT IN UZD]` `[active]`

A PowerShell script with an embedded C# core that takes any TTF, walks each glyph's vector outline via GraphicsPath, rasterises it at 8x supersample, runs an 8SSEDT two-pass distance transform, downsamples to the cell, and writes two files: an ordinary PNG atlas and a plain-text metrics file. Parameters are TTF path, cell size, spread and supersample factor. The shipped face was generated at cell 96 / spread 16 (em box 64), giving a 1536x576 atlas of...

**Why.** Nothing is generated at runtime and no font file ships, so the engine loads the atlas like any other texture. Generating at load would mean a distance transform per glyph at every startup, a custom FImageSource to get the result into a texture, and quality capped by whatever bitmap font happened to be loaded. Working from the outline...

**Reached by.** Offline developer tool, run by hand from a PowerShell prompt outside the game: tools/sdffont/mksdf.ps1 -Ttf <font.ttf> -OutDir <dir> [-Cell 96 -Spread 16 -Super 8]. No in-game surface at all (no cvar, menu or bind gates the generator itself), so the VR/no-keyboard constraint does not apply to it. One manual step the entry omits: the script always writes...

**Files.** `tools/sdffont/mksdf.ps1`, `wadsrc/static/sdffonts/sdfmono.txt`, `wadsrc/static/graphics/sdfmono.png`

### The field encoding contract, and the sign traps in it
`[NOT IN UZD]` `[active]`

One convention, honoured by generator, shader and preview tool alike: single channel, inside positive, 0.5 encodes exactly on the edge, and the byte maps a signed distance of +/- spread onto 0..255. The generator carries three explicit guards for it -- an INF sentinel small enough that INF*INF fits in an int, atlas pixels pre-filled to opaque black ("far outside") rather than left at zero alpha, and a no-ink path that emits a metrics row without ever...

**Why.** Every one of those guards fixes a silent failure that produces a plausible-looking but wrong atlas. INF at 1<<28 squares to 2^56, which wraps to exactly 0, so no comparison ever wins and the whole atlas comes out flat mid-grey. A glyph with no ink (space, or any codepoint the font omits) has no edge, so the transform returns 0 everywhere...

**Reached by.** Internal, no user surface. The encoding convention is shared between E:\UZDXREMA\tools\sdffont\mksdf.ps1 (writes it), E:\UZDXREMA\wadsrc\static\shaders\glsl\func_sdftext.fp (decodes it at draw time) and E:\UZDXREMA\tools\sdffont\sdfpreview.ps1 (decodes it offline for the quality gate). The one adjacent knob is CVAR(String, bb_sdffont, "sdfmono",...

**Files.** `tools/sdffont/mksdf.ps1`, `wadsrc/static/shaders/glsl/func_sdftext.fp`, `src/rendering/hwrenderer/scene/hw_sdffont.h`

### Atlas quality gate (tools/sdffont/sdfpreview.ps1)
`[NOT IN UZD]` `[legacy]`

Reconstructs glyphs from a generated field the way the shader will -- bilinear sample, signed distance, smoothstep edge, optional glow falloff -- and renders a comparison sheet: a string at card size, the same string with glow, and a 300px side-by-side of a bitmap glyph upscaled against the same glyph drawn from the field.

**Why.** It answers "is the field actually better than the bitmap at the size a player reaches by walking up to a card" before any C++ is written, and it is where the glow radius was measured before the shader existed. It also means a bad atlas can be caught outside the engine, which matters here because launching the game is not a cheap...

**Reached by.** Developer PowerShell script, not wired into CMake and with no engine, cvar, menu or ZScript surface: powershell -File tools\sdffont\sdfpreview.ps1 -Dir <mksdf output dir> -Ttf <font.ttf> -Out <preview.png>. All three arguments must be passed — every default is stale (-Dir and -Out point at a scratchpad path from a prior session, 76030631-3473-..., and -Ttf...

**Files.** `tools/sdffont/sdfpreview.ps1`

### FSDFFont: atlas loader, metrics parser and failure-caching cache
`[NOT IN UZD]` `[active]`

Loads a font by base name: "<name>" as a texture and "sdffonts/<name>.txt" as its metrics. Parses cell / spread / em / atlas / per-glyph rows into a 256-entry table, converts cell rectangles to UVs, and caches the result by lowercased name. Failures are cached as null rather than dropped. The metrics file's own `atlas W H` line overrides the texture's reported dimensions when computing UVs.

**Why.** Trusting the texture's size would slide every glyph if the image were padded to a power of two on upload. Caching failures matters because otherwise a mod naming a font it does not ship makes the engine hunt for two missing lumps on every glyph of every frame -- a stutter nobody would connect to a typo. Metrics are kept in atlas pixels...

**Reached by.** Primary surface is the ZScript API on Level, declared in wadsrc/static/zscript/doombase.zs:801-810 and thunked in src/scripting/vmthunks.cpp:3125-3175: SetBillboardFont(id, slot), RollBillboardFonts(), BillboardFontCount(), BillboardFontName(slot). A billboard names a SLOT, not a font; FSDFFontRoster scans every "sdffonts/*.txt" in the load order, keeps the...

**Files.** `src/rendering/hwrenderer/scene/hw_sdffont.cpp`, `src/rendering/hwrenderer/scene/hw_sdffont.h`, `wadsrc/static/sdffonts/sdfmono.txt`

### FSDFFontRoster: many typefaces at once, shuffled per game, addressed by slot
`[NOT IN UZD]` `[active]`

Scans the whole load order for "sdffonts/*.txt", keeps entries that resolve to a real atlas and parse, excludes the default face, Fisher-Yates shuffles the rest, and hands them out by slot. Slot 0 is always bb_sdffont; slots 1..N index the rolled roster. A billboard names a slot; anything unresolvable falls back to slot 0.

**Why.** The face used to be a single global cvar, so every BB_TEXT in the world drew from one atlas -- fine for a debug caption, hopeless for a card that wants a heavy display face on the name and a clean one on the numbers. Rolling the order means a run does not look like the last one, which also means no call site may assume a slot's identity:...

**Reached by.** ZScript only, on LevelLocals (wadsrc/static/zscript/doombase.zs:801-810): SetBillboardFont(id, slot), RollBillboardFonts(), BillboardFontCount(), BillboardFontName(slot). No menu entry, no bind, no console command — a mod must drive it. The one console-only knob is CVAR String bb_sdffont (src/rendering/hwrenderer/scene/hw_sprites.cpp:103, default "sdfmono",...

**Files.** `src/rendering/hwrenderer/scene/hw_sdffont.cpp`, `src/rendering/hwrenderer/scene/hw_sdffont.h`, `src/scripting/vmthunks.cpp`, `wadsrc/static/zscript/doombase.zs`, `src/g_levellocals.h`

### One multi-line layout definition shared by the renderer and by script
`[NOT IN UZD]` `[active]`

SDFMeasureText and SDFMeasureLine, plus the SDFTEXT_LINE_PITCH constant (1.30 em), live in the header rather than in either .cpp. A string carrying '\n' draws as stacked lines, each centred on its own width; '\r' is skipped so CRLF and LF measure identically. Empty/absent text reports zero lines.

**Why.** The arithmetic is an agreement between two translation units -- hw_sprites.cpp draws the block and vmthunks.cpp measures it for script. If they disagreed, a panel would be sized by one set of rules and filled by another, and the only symptom would be text sitting slightly wrong inside its own box, which nobody finds by reading either...

**Reached by.** ZScript only, no menu or bind (correct for an author-facing API): Level.MeasureBillboardText(string text, double height, int fontSlot = 0) returns the widest line's width in map units, and Level.MeasureBillboardTextBlock(string text, double height, int fontSlot = 0) returns (widest line width, total block height) as a Vector2, both in map units for the...

**Files.** `src/rendering/hwrenderer/scene/hw_sdffont.h`, `src/scripting/vmthunks.cpp`, `wadsrc/static/zscript/doombase.zs`

### Em-box fitting and per-glyph quad trimming
`[NOT IN UZD]` `[active]`

EmitBillboardSDFText scales text to the em box (cell minus two spreads) rather than to the cell, while still drawing the full cell so the halo keeps its margin. Each glyph's quad and UVs are clipped horizontally to the glyph's advance plus one spread each side, and the pen is offset back by the spread margin so letters do not drift right.

**Why.** Cells are square but an advance is narrower than a cell (32 against 64 on the shipped font), so a full-cell quad overhangs its neighbour by half a cell of empty field -- and empty field still carries halo, and two overlapping halos add, which was brightening the gaps between letters. Fitting the cell instead of the em box spent a quarter...

**Reached by.** Internal, no user surface: the em-box fit and per-glyph trim run unconditionally inside EmitBillboardSDFText whenever a billboard draws BB_TEXT through an SDF slot. The only related knob is the archived string cvar bb_sdffont (default "sdfmono"), which picks the face for slot 0 and has no menudef entry, so it is console-only and effectively unreachable in...

**Files.** `src/rendering/hwrenderer/scene/hw_sprites.cpp`

### Bitmap-font fallback with one-shot draw-path diagnostics
`[EXTENDS]` `[active]`

If no SDF font resolves, BB_TEXT falls through to EmitBillboardGlyphs and the engine's SmallFont, so text still appears -- just soft. Three one-shot log lines separate the failure modes: the loader reports glyph count / cell / spread on a successful load, the emitter reports the first SDF draw with quad count, missing glyphs, line count, scale and first UV, and the fallback reports in yellow that drawing is going through bitmap glyphs and names the cvar...

**Why.** A mod is allowed not to ship a font, so a missing atlas must degrade rather than be fatal. But the fallback is deliberately invisible in the frame, so without logging the only symptom is "the letters look a bit off". The split matters because a good load only proves the file was read -- it says nothing about whether the draw path ever...

**Reached by.** Automatic, but only once a BB_TEXT billboard is actually drawn — nothing in the base game spawns one, so it requires a ZScript caller or the debug console commands bb_text / bb_spawn (src/p_tick.cpp:214,263), which are unreachable in VR. Output goes to the console/logfile; no interaction needed to read it. The governing cvar bb_sdffont (default "sdfmono",...

**Files.** `src/rendering/hwrenderer/scene/hw_sprites.cpp`, `src/rendering/hwrenderer/scene/hw_sdffont.cpp`

### Font cache and roster invalidation on texture reload
`[NOT IN UZD]` `[active]`

d_main.cpp calls FSDFFont::FlushAll() and FSDFFontRoster::Invalidate() immediately after TexMan.Init(), dropping every cached font and arming a fresh roster scan and shuffle.

**Why.** The font cache holds raw FGameTexture* obtained from the texture manager, so a TexMan re-init leaves every cached pointer dangling; and after a wad-set change the roster still names faces from the previous set while missing any the new set ships. FlushAll had zero callers in the entire tree before this, so the cache had never been...

**Reached by.** Internal only, no user-facing control. Runs unconditionally in D_DoomMainSetup immediately after TexMan.Init() (src/d_main.cpp:4163-4174), which covers first startup and every engine restart / wad-set change. Nothing to reach from a menu, bind, or console; no cvar gates it.

**Files.** `src/d_main.cpp`, `src/rendering/hwrenderer/scene/hw_sdffont.cpp`, `src/rendering/hwrenderer/scene/hw_sdffont.h`

## 10. ZScript and scripting API surface

> Upstream UZDoom exposes no VR concepts to ZScript at all — there is no hand, no headset, no controller, no grip. This fork adds a large scripting surface built around that gap: per-actor VR pose and grip state on Actor, a script-claims/engine-arbitrates protocol for what each hand's grip means, a rigid-body physics API for grabbable and throwable objects, model/bone introspection that can turn a mesh marker into a world position, off-hand weapon plumbing throughout the weapon system, and a set of level-side services (in-world billboard panels with ray/touch queries, runtime field reflection across mods, JSON profile persistence). Almost all of it is ZScript-only — reachable from mod code, not from a menu or a bind, which matters because the engine is played with no keyboard.

### VR hand and headset pose on Actor
`[NOT IN UZD]` `[active]`

Adds native fields to every Actor carrying the live VR rig: main-hand aim origin/angles (AttackPos, AttackAngle, AttackPitch, AttackRoll), the true wrist roll the netcode discards (MainHandRoll), the same four for the off hand (OffhandPos, OffhandAngle, OffhandPitch, OffhandRoll), the real headset transform in map units (HmdPos, HmdYaw, HmdPitch, HmdRoll), and the controller-driven component of yaw split out from physical head yaw (VRTurnYaw)....

**Why.** A body-relative VR mod — holsters, hardpoints, wrist mounts, hand menus — has to reason about where the player's head and hands physically are, and the playsim only ever exposed an aim ray. The turn-yaw split exists because a 45-degree snap turn rotates the whole virtual body while a neck deadzone should only apply to real head rotation;...

**Reached by.** ZScript API only — plain native field reads on any Actor, in practice the player pawn (e.g. players[consoleplayer].mo.HmdPos, .mo.MainHandRoll). No cvar, menu entry, or bind exposes them; they are for mod code. Note the HMD group is populated only under the Vulkan OpenXR backend in single-player gameplay mode — under the OpenVR/SteamVR path...

**Files.** `wadsrc/static/zscript/actors/actor.zs`, `src/playsim/actor.h`, `src/scripting/vmthunks_actors.cpp`, `src/playsim/p_user.cpp`

### Script-writable attack origin and direction
`[EXTENDS]` `[in flight]`

Demotes AttackPos, AttackPitch, AttackAngle, OffhandPos, OffhandPitch and OffhandAngle from readonly to writable in ZScript. The VR backend still rewrites them from the controller every frame, so a script write survives exactly one tic — enough for a single shot to be fired from somewhere other than the hand.

**Why.** A wrist-mounted or shoulder-mounted weapon fires from the mount along the arm, not from wherever the palm happens to point, and those six fields are the attack origin and direction. While they were readonly a hardpoint weapon could only spray in whatever direction the hand faced.

**Reached by.** ZScript API only — plain assignment to Actor.AttackPos/AttackPitch/AttackAngle/OffhandPos/OffhandPitch/OffhandAngle from a mod. No cvar, menu entry or bind is involved. Uncommitted in the working tree (git status shows M on wadsrc/static/zscript/actors/actor.zs; HEAD still declares them readonly). A write only affects gameplay while...

**Files.** `wadsrc/static/zscript/actors/actor.zs`

### Grip arbitration protocol (script claims, engine decides)
`[NOT IN UZD]` `[active]`

A two-sided contract on Actor. Script writes what it wants a hand's grip spent on — HolsterClaimMain/Off, HardpointClaimMain/Off, GrabClaimMain/Off, GripClaimMain/Off — and the engine writes back what actually won: GripContextMain/Off (EGripContext), GripSubjectMain/Off (EGripSubject), GripHeldMain/Off for the raw squeeze, TwoHandedHold, and FingerTouchMain/Off for capacitive thumb/index contact. HardpointButtons publishes a claimed hand's...

**Why.** One VR grip button has to mean holstering, stabilizing, the secondary-button shift layer, grabbing an object, or plain grip — and only script can tell which, because reach volumes and grabbability tables live in mods. Earlier there was a single shared claim boolean and two mods writing it every tic silently erased each other, so a hand...

**Reached by.** ZScript API only, VR/OpenXR sessions only — no cvar, menu entry or bind. Script writes the claim fields on AActor every frame (HolsterClaimMain/Off, HardpointClaimMain/Off, GrabClaimMain/Off as bools; GripClaimMain/Off as an EGripSubject int) and reads back the engine-owned readonly results (GripContextMain/Off, GripSubjectMain/Off, GripHeldMain/Off,...

**Files.** `wadsrc/static/zscript/actors/actor.zs`, `wadsrc/static/zscript/constants.zs`, `src/playsim/actor.h`, `src/scripting/vmthunks_actors.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`

### Script control of VR input, laser sight and haptics
`[NOT IN UZD]` `[active]`

Level-side calls that let a mod take temporary ownership of the VR I/O path: SuppressVRInput(bool) / IsVRInputSuppressed() claim the thumbsticks; GetRawStickMove() returns the raw (forward, side) deflection even while suppressed; ForceVRLaser(bool on, int hand) overrides the laser-sight cvars per hand without writing them; SetVRLaserRange(double) shortens the beam to a distance only script knows; VRHaptic(int hand, double intensity, double durationMs)...

**Why.** Snap turn and stick locomotion are decided deep in the VR input path before any script sees a button, so an in-world menu driven by the thumbstick used to spin and walk the player while they were choosing. The laser controls exist because the engine trace cannot see script-drawn billboards, so a pointer aimed at a panel passed straight...

**Reached by.** ZScript API only. The six level natives (level.SuppressVRInput / IsVRInputSuppressed / GetRawStickMove / ForceVRLaser / SetVRLaserRange / VRHaptic) are ungated and work the moment a mod calls them; VRHaptic additionally needs vr_enable_haptics, which defaults true but has NO menu entry (console/ini only) and a VR mode active. The Actor read-back fields...

**Files.** `wadsrc/static/zscript/doombase.zs`, `src/scripting/vmthunks.cpp`, `wadsrc/static/zscript/actors/actor.zs`

### Rigid-body physics API on Actor
`[NOT IN UZD]` `[dormant]`

Fourteen natives that hand an actor to a real rigid-body solver, which then owns its position, orientation and velocity outright (MF9_PHYSICSBODY; Doom movement is skipped for it). PhysicsEnable takes mass in kilograms, box half-extents in metres and a centre-of-mass offset; PhysicsAddImpulse/PhysicsAddSpin move it; PhysicsGrab(hand)/PhysicsSetSupport/PhysicsRelease carry it at physics rate rather than the 35Hz tic rate; PhysicsDistanceTo measures to the...

**Why.** Picking up, turning over and throwing objects is the basic verb of VR, and Doom's movement model has no concept of any of it. Carrying at physics rate rather than tic rate is what makes a throw work — a held body has been inheriting the hand's motion all along, so releasing it needs no impulse at all.

**Reached by.** ZScript API only for the fourteen Actor natives, but the whole solver is gated behind the cvar `vr_physics`, default **false**. That cvar has a menu home and is reachable without a keyboard: Options -> "Physics Options" (promoted to the top level of OptionsMenu at wadsrc/static/menudef.txt:377, also reachable via VR -> 3D mode -> "Object physics" at :2553)...

**Files.** `wadsrc/static/zscript/actors/actor.zs`, `src/playsim/p_physics.cpp`, `src/playsim/p_physics.h`, `src/playsim/actor.h`

### Per-actor render overrides
`[EXTENDS]` `[active]`

Four new per-actor drawing controls. VoxelOverride draws the actor as its voxel regardless of r_drawvoxels and in preference to any model. ForceModelAngles makes the actor's own pitch and roll apply whatever its MODELDEF says. ModelFrame / ModelFrameNext / ModelFrameLerp address a mesh frame directly, bypassing the sprite-letter table, and blend bone matrices between two frames. The readonly hasmodel flag reports whether a class appears in MODELDEF at all.

**Why.** Model presentation was per-MODELDEF, which is the wrong granularity when one actor borrows another class's model — a holstered weapon wearing the real gun's model cannot have MDL_USEACTORPITCH set on the shared definition without changing how the gun is drawn in your hand. Direct frame addressing exists because the sprite-letter table...

**Reached by.** ZScript API only — plain field assignment on any Actor (VoxelOverride, ForceModelAngles, ModelFrame, ModelFrameNext, ModelFrameLerp are writable; hasmodel is readonly and must be read off GetDefaultByType, not a live actor). No cvar, menu entry or bind touches any of them. The only console cvars in the area are diagnostics, not gates: vr_voxel_debug...

**Files.** `wadsrc/static/zscript/actors/actor.zs`, `src/playsim/actor.h`, `src/scripting/vmthunks_actors.cpp`, `src/r_data/models.cpp`

### PSprite layer control and bone anchoring
`[EXTENDS]` `[active]`

New fields on PSprite. NoDraw hides a layer without disturbing the weapon behind it. Tint and Glow colour the held 3D model per layer, so main hand and off hand tint independently. ModelFrame/ModelFrameNext/ModelFrameLerp address mesh frames directly. AnchorLayer plus AnchorBone draw one layer at a bone of another layer's model, with AnchorOfs/AnchorAngles positioning it in the bone's own frame, and the renderer publishes the resolved result back as...

**Why.** A hand holding a weapon is two psprite layers that must stay welded together at a specific point on the mesh, and there was no way to express that. The readback fields exist because script cannot derive them: Quat exposes no vector-rotate, and rebuilding the weapon's basis by hand is the trap the comment says AnchorBoneWorld was written...

**Reached by.** ZScript API only — native fields on the PSprite returned by player.GetPSprite(...) / player.FindPSprite(...). No cvar, no menu entry, no bind. Unconditional in the renderer: any layer that sets AnchorLayer + AnchorBone gets the bone transform, and any layer that sets NoDraw/Tint/Glow/ModelFrame is honoured immediately. Nothing here is reachable or...

**Files.** `wadsrc/static/zscript/actors/player/player.zs`, `src/playsim/p_pspr.h`, `src/scripting/vmthunks_actors.cpp`

### Model, bone and state-frame introspection
`[EXTENDS]` `[active]`

Actor.ModelPointToWorld(mx,my,mz) runs the renderer's own object-to-world matrix and returns position plus forward and up axes, so a model-space bone becomes a world position. Actor.FindBoneIndex(Name) answers whether a mesh carries a bone at all (-1 if not). Level-side, GetModelOrientationHint / GetModelOffsetHint / GetModelWorldOffset / GetModelBoundsHint report a MODELDEF's baked mirroring, angle offsets, position offset and physical size, and...

**Why.** TransformByNamedBone stops in model space and never sees the object-to-world matrix, so script could ask where MARKER_grip was and get an answer with no relation to the room — which is why seating a world model previously came down to a human finding an offset on a slider. Every other bone entry point fails silently on a missing name, so...

**Reached by.** ZScript API only. No cvar, menu entry or bind anywhere in the group — Actor.ModelPointToWorld / Actor.FindBoneIndex / Actor.RegisterModelStateFrame / Actor.ClearModelStateFrames (instance methods, wadsrc/static/zscript/actors/actor.zs:1570,1571,1840,1848), Actor.CountStateLabels / Actor.GetStateLabelAt (clearscope static, actor.zs:1579-1580), and...

**Files.** `src/r_data/models.cpp`, `src/playsim/p_actionfunctions.cpp`, `src/scripting/vmthunks.cpp`, `wadsrc/static/zscript/actors/actor.zs`, `wadsrc/static/zscript/doombase.zs`

### Off-hand weapon scripting surface
`[EXTENDS]` `[active]`

A second weapon hand threaded through the whole weapon system. PlayerInfo gains OffhandWeapon, ohattackdown, PremorphWeaponOffhand, PlayInVR, keepmomentum and CheckWeaponButtons(int hand). A PSP_OFFHANDWEAPON psprite layer and CHAN_OFFWEAPON sound channel are added, along with BT_OFFHANDATTACK/BT_OFFHANDALTATTACK/BT_OFFHANDDROPMAG/BT_MAINHANDDROPMAG buttons and a parallel WF_OFFHAND* weapon-state bank. PlayerPawn methods take a hand argument (FireWeapon,...

**Why.** Two motion controllers means two independent armed hands, and stock GZDoom has exactly one ReadyWeapon everywhere. bKeepWhenEmpty exists because stock CheckAmmo hands the weapon back the instant ammo hits zero, which makes manual VR reloading impossible — ejecting a magazine empties the gun and the gun then leaves your hand before you...

**Reached by.** ZScript API for the fields, flagdefs, properties and virtuals — but the player-facing half is fully reachable without a keyboard. `+oh_attack`, `+oh_altatk`, `+oh_dropmag` and `+mh_dropmag` are rebindable from the Controls menu (E:\UZDXREMA\wadsrc\static\menudef.txt lines 609-612, 676-677, 719-720) and ship with VR default binds in...

**Files.** `wadsrc/static/zscript/actors/player/player.zs`, `wadsrc/static/zscript/actors/inventory/weapons.zs`, `wadsrc/static/zscript/constants.zs`, `wadsrc/static/zscript/actors/inventory/stateprovider.zs`, `src/playsim/p_pspr.h`

### Two-hand stabilize reach as a per-weapon value
`[NOT IN UZD]` `[legacy]`

Weapon.StabilizeDistance (inches, a property) declares how close the hands must be for that weapon to read as two-handed; 0 falls back to the vr_stabilize_distance_inches cvar and a negative value disables stabilize for that weapon. An in-tree EventHandler, VRStabilizeSyncHandler, copies it into the native Actor.StabilizeReach field every tic, and the VR input backend reads that in place of a fixed constant. The arbitrated result surfaces as...

**Why.** The stabilize check runs in the VR input backend, which has no native Weapon class to read a ZScript-only property from — Weapon has no C++ backing beyond plain Actor. Making it a global cvar instead is wrong because a pistol and a rifle want different reaches and a melee weapon wants none. The sync lives in its own handler rather than...

**Reached by.** ZScript API only, and currently inert: Weapon.StabilizeDistance can be set in a Default block and VRStabilizeSyncHandler still copies it into Actor.StabilizeReach every tic, but nothing consumes the value. The fallback cvar vr_stabilize_distance_inches (default 8.0) is menu-reachable as a slider "Stabilize Distance (in)" under VR Options -> VR Controls...

**Files.** `wadsrc/static/zscript/vr_stabilizesync.zs`, `wadsrc/static/zscript/actors/inventory/weapons.zs`, `wadsrc/static/zscript/actors/actor.zs`, `src/playsim/actor.h`

### Runtime field reflection across mods
`[NOT IN UZD]` `[active]`

Ten level-side calls that read another mod's data by field NAME rather than through a typed reference: HasField, GetFieldInt, GetFieldIntArray, GetFieldBool, GetFieldFloat, GetFieldString, GetFieldName, GetFieldObject, plus FieldCount and FieldAt for enumeration (which report a type string of "int", "double", "string", "name", "object" or "other"). Private, meta and static fields are refused; read-only ones are allowed because nothing here writes. Every...

**Why.** A typed reference needs its class at compile time, so an informational consumer — a weapon-select panel showing tier, rarity or affixes — had to hard-depend on every mod it wanted to describe. Service (service.zs) only covers mods that cooperate; this covers everything already released, which never will. There is deliberately no...

**Reached by.** ZScript API only. The ten reflection calls are methods on LevelLocals (wadsrc/static/zscript/doombase.zs:976-1003), so scripts call them as level.HasField(obj, "fieldname"), level.GetFieldInt(obj, "field", out v), level.FieldCount(obj), level.FieldAt(obj, i, out name, out type), etc. A_SetUserVarName(name varname, name value) is an actor action function...

**Files.** `src/scripting/vmthunks.cpp`, `wadsrc/static/zscript/doombase.zs`, `src/playsim/p_actionfunctions.cpp`

### In-world billboard panels with pointer and touch queries
`[NOT IN UZD]` `[active]`

A native world-space quad system a mod builds UI out of: AddBillboard / AddBillboardPersistent / AttachBillboard place a quad with an explicit yaw, tilt and facing mode and one of fourteen payloads (panel, texture, digits, glyph, ring, bar, text, 16-segment LED and LCD, seam, and SDF panel/hexagon/star). Setters cover text, glow, gradient, progress, font slot, roll, resize, alpha and movement; groups apply one eased transform over a whole composed panel....

**Why.** VR UI has to exist in the room, and a mod building it out of actors gets no hit-testing, no crisp text at arbitrary size, and no way to animate a forty-quad panel without eighty setter calls a tic that step at 35Hz while the renderer does not. SweepBillboard exists because a deliberate jab crosses a thin panel between two tics without...

**Reached by.** ZScript API only, on LevelLocals — level.AddBillboardPersistent(...), level.AimBillboard(...), etc. No menu entry and no bind. Two console-only archived cvars exist as perf limiters (rs_bb_maxpanels, rs_bb_cullradius, both defaulting to 0 = unlimited), declared in src/rendering/hwrenderer/scene/hw_drawinfo.cpp and absent from menudef.txt, so they are...

**Files.** `wadsrc/static/zscript/doombase.zs`, `src/scripting/vmthunks.cpp`, `src/g_levellocals.h`, `wadsrc/static/zscript/engine/base.zs`, `src/scripting/vmthunks_actors.cpp`

### Named JSON profile persistence
`[NOT IN UZD]` `[active]`

Five level-side calls that let a mod save and reload a flat key/double document by name: JSONProfileBegin(), JSONProfileSetDouble(key, value), JSONProfileSave(name), then JSONProfileLoad(name) and JSONProfileGetDouble(key, default). Files land in an rs_profiles subfolder beside doomxr.ini and are read and written through the engine's own rapidjson. Exactly one profile is in progress at a time; names are restricted to [A-Za-z0-9_-], 1-64 characters, and...

**Why.** A VR mod's calibration — holster positions, hardpoint offsets, per-player reach — is a set of numbers a player tunes once and expects back, and ZScript had no file I/O. The alternative was dozens of cvars or a hand-rolled ZScript text scanner; going through the engine's JSON library means a hand-edited file gets real parsing.

**Reached by.** ZScript API only — the five natives are members of `struct LevelLocals native`, so play-side script calls them as level.JSONProfileBegin() / level.JSONProfileSetDouble(key, val) / level.JSONProfileSave("name") and level.JSONProfileLoad("name") / level.JSONProfileGetDouble(key, default). No cvar, no menu entry, no CCMD, no bind — a mod must call them; the...

**Files.** `wadsrc/static/zscript/doombase.zs`, `src/scripting/vmthunks.cpp`

## 11. Menus, options, and profiles

> In UZDXREMA the menu is the only interface. The player is in a headset with two motion controllers and no keyboard, so anything that upstream UZDoom exposes as a console command has to be given a menu row, a slider or a key-grid here or it is unreachable. The fork therefore adds roughly 1,800 lines to menudef.txt (about 25 new pages), a fork-only 1,255-line cheat/spawn/level-select menu, two character-grid text-entry overlays that stand in for a keyboard, a command-line profile picker that restarts the engine into a different mod set, in-engine multiplayer host/join pages, and several MENUDEF language extensions (runtime VR-mode and developer-level conditionals, auto-scrolling pages, per-item colour) that the new pages need. It also loosens two engine rules the menu system enforced: a menu may now opt out of pausing the world, and a menu's Ticker now counts as menu code for cvar writes.

### VR preferences tree (VROptionsMenu and its four sub-pages)
`[NOT IN UZD]` `[active]`

Adds a top-level "VR" page plus Cinema Mode, HUD/Automap, and Weapon sub-pages. Between them they expose world scale (vr_vunits_per_meter), standing-height offset, control scheme and handedness, snap-turn angle, off-hand-relative locomotion, teleport, momentum, walk/run multipliers, two-handed weapon hold, stabilise distance, pickup/quake/external haptic levels, OpenXR present gamma/contrast/brightness/saturation bias, desktop mirror view mode, and the...

**Why.** Every one of these is a comfort or fit value that has to be dialled in while wearing the headset. Upstream UZDoom has no VR code at all, so none of these settings or their pages exist there, and a console-only knob is unusable to a player holding two controllers.

**Reached by.** Options -> "VR Options" (menudef.txt:381, unconditional). Three sub-pages, labelled on screen as "Virtual Screen Options" (menudef.txt:1691, only present when vr_mode is OpenXR/OpenVR at startup), "VR HUD/Automap Options" (:1712) and "VR Weapon Options" (:1769). Also on the simplified page, OptionsMenuSimple (menudef.zsimple:10). All menu-reachable with a...

**Files.** `wadsrc/static/menudef.txt`, `wadsrc/static/menudef.zsimple`, `wadsrc/static/language.0`

### Mod Options tree: laser sight, hitscan tracers, weapon wheel, voxels
`[EXTENDS]` `[active]`

A second top-level page fronting four fork rendering/interaction subsystems. Laser Sight has ~45 rows: beam alpha/width/glow/emissive/taper/fade, three beam-length modes, source XYZ offset, aim-lock tightening and rate, four colour pickers, a per-weapon-slot colour mode with its own ten-picker sub-page, a hue-cycling layer, and a headshot-reaction colour with pulse. Hitscan Tracers control colour, opacity, length, width, speed, muzzle offset and ricochet...

**Why.** These are the fork's own gameplay-visible systems; without menu rows they would be console cvars, which a headset player cannot type. The voxel page in particular converts a console-only upstream cvar into a reachable setting and adds the distance limit that makes a whole-world voxel pack affordable.

**Reached by.** Options -> Mod Options -> {Laser Sight Options, Bullet Tracer Options, Weapon/Item Wheel Options, Voxel Options}. Reachable from BOTH options menus: wadsrc/static/menudef.txt:382 (full "OptionsMenu") and wadsrc/static/menudef.zsimple:11 (the simple menu used when m_simpleoptions is on; it defaults false). The four sub-pages are OptionMenu...

**Files.** `wadsrc/static/menudef.txt`

### Three-tier performance pages (Quick, VR Performance Tweak, Developer Options)
`[NOT IN UZD]` `[active]`

The same performance knobs are offered at three depths. QuickMenu is a short guided page with a one-line explanation under each row (refresh rate, dynamic lights, skydome, sprite shadows, storage-buffer type, light mode, texture filter, FPS counter, jump, custom postprocess, plus BSP/scene multithreading and OpenXR multiview when those runtimes are active). VRPerfTweakMenu is the full page: multithreading, multiview, texture streaming, anisotropy,...

**Why.** Performance tuning in VR is not optional -- a dropped frame is felt, not seen -- and the person tuning it cannot open a console. Splitting the same knobs into a guided page, a full page, and a developer-gated page means an ordinary player is not shown vk_max_transfer_threads while a developer still reaches it without typing.

**Reached by.** All three are menu-reachable, no console needed. Options -> "Quick Menu" (QuickMenu, menudef.txt:380 -> :4348) and Options -> "Performance Tweak" (VRPerfTweakMenu, :383 -> :4487) are unconditional; both also appear in the simple-options variant wadsrc/static/menudef.zsimple (lines 9 and 12). The third tier, titled "Developer Only" (DEVOPTMNU_TITLE in...

**Files.** `wadsrc/static/menudef.txt`

### Object physics options page
`[NOT IN UZD]` `[active]`

A page for the fork's rigid-body layer, promoted to sit directly on the Options menu rather than under VR. Exposes the master toggle plus restitution, friction, linear/angular damping, contact spin damping, gravity, solver rate, throw strength and throw threshold, hand collider on/off and size, the held-weapon placeholder box dimensions and offsets, wrist-spin influence on throws, and two diagnostic outputs that write to the log rather than the screen.

**Why.** These are feel values that only mean anything while you are throwing something in the headset, so the page is deliberately one level from the root -- the in-file comment says burying them under VR meant taking the headset off to find them. The diagnostics write to the log because reading console output in a headset is impractical.

**Reached by.** Options -> "Physics Options" (the first row of OptionsMenu, placed above the Title directive), which opens the page whose own title is "Object physics". A second entry point exists: VR options -> 3D mode (OptionMenu "VR3DMenu") ends with Submenu "Object physics", "VRPhysicsMenu". Pure menu access, no console needed, so it is fully usable in the headset. The...

**Files.** `wadsrc/static/menudef.txt`, `src/playsim/p_physics.cpp`

### Cheat, spawner and level-select menu
`[NOT IN UZD]` `[active]`

A fork-only ZScript menu class, opened by its own toggle command, that replaces typed cheats. The root page offers god mode, noclip, notarget, monster-fear, freeze, give-all, kill-monsters, resurrect, next map, and developer-level toggles; from there, per-IWAD sub-pages give a named item/weapon spawner, a named monster spawner, and a full level select for Doom, Chex, Heretic, Hexen and Strife. Entries that need cheats prefix their command with `sv_cheats...

**Why.** Every cheat in Doom is a typed string. With no keyboard the entire category was unreachable, which also made the engine hard to test -- you could not get to map 23 or spawn a Cyberdemon to look at a model. The per-IWAD split exists because the summon class names differ per game.

**Reached by.** The `togglecheatmenu` console command, which is bindable — but it ships with NO default bind (nothing in wadsrc/static binds it). To reach it in VR the owner must first assign a key or controller button to the "WTF?" row at the bottom of Options -> Customize Controls -> Other (menudef.txt:1084, in OptionMenu "OtherControlsMenu", reached from the...

**Files.** `wadsrc/static/zscript/engine/ui/menu/cheatmenu.zs`, `src/g_game.cpp`, `src/common/scripting/interface/vmnatives.cpp`, `wadsrc/static/menudef.txt`, `wadsrc/static/zscript.txt`

### Character-grid text entry for console and chat
`[NOT IN UZD]` `[active]`

Two fork-only menu classes that put a 13x5 on-screen character grid (A-Z, digits, punctuation, space, backspace) in front of the console and the chat line. Opening the console with no menu up automatically pushes ConsoleTextEnterMenu, which stays open while the console is visible so multiple commands can be entered in a row, and auto-pauses a single-player game while it is up. Chat and team-chat open ChatTextEnterMenu instead; closing it cancels the...

**Why.** Without a keyboard, the console and the chat line are both dead. The grid makes both usable with a thumbstick or a pointed controller. The console variant deliberately stays open between commands because re-opening the grid for every command is unbearable at grid-typing speed.

**Reached by.** Console overlay: pushed automatically whenever the console is toggled on with no menu up, so it is reached by whatever key/button is bound to "toggleconsole" — bound at Options -> Customize Controls -> "Console" (menudef.txt:1060). Default bind is the keyboard grave key (commonbinds.txt:3), so in VR it must be rebound to a controller button before it is...

**Files.** `wadsrc/static/zscript/engine/ui/menu/consoletextentermenu.zs`, `wadsrc/static/zscript/engine/ui/menu/chattextentermenu.zs`, `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs`, `src/d_main.cpp`, `src/ct_chat.cpp`, `src/common/scripting/interface/vmnatives.cpp`

### Command-line profile system
`[NOT IN UZD]` `[active]`

Scans the program directory, a `profiles/` subdirectory, and every path in the config's FileSearch.Directories for files named `commandline_<name>.txt`. Each is a saved command line; if its first line begins with `#TITLE` the rest of that line becomes the display name, otherwise the filename is used. The collected list is turned into menu rows at startup and shown under a LabeledSubmenu that displays the currently active profile. Choosing one writes the...

**Why.** A headset player launches the engine from inside the headset and never sees a shell. Switching between mod sets -- a weapon pack today, a total conversion tomorrow -- would otherwise mean editing a launcher on the PC and putting the headset back on. This makes a mod set a menu row.

**Reached by.** Options menu -> "Active Profile" (menudef.txt:412, a stock LabeledSubmenu item whose inline value is the cmdlineprofile cvar's raw string -- the short profile name, or "Not set" when empty). Opening it shows one row per discovered profile, built at startup by InitCommandLineProfileMenu() in src/menu/doommenu.cpp:1405-1422; each row runs `cmdlineprofile...

**Files.** `src/menu/profiledef.cpp`, `src/menu/profiledef.h`, `src/menu/doommenu.cpp`, `src/d_main.cpp`, `src/m_misc.cpp`, `wadsrc/static/menudef.txt`

### In-engine multiplayer host and join menus
`[NOT IN UZD]` `[active]`

A Multiplayer page with Host and Join sub-pages. The host page's option lists are built at runtime in C++ rather than written in MENUDEF: player count is generated from 2 up to MAXPLAYERS, the skill list is read from AllSkills (skipping NoMenu entries and using each skill's MenuName), and the map list is built from wadlevelinfos, de-duplicated, validated with P_CheckMapData and labelled with each map's looked-up level name. Game mode...

**Why.** Upstream reaches multiplayer through -host and -join on the command line. The same no-shell problem applies, and the map and skill lists have to be generated because they depend on whatever wads the player actually loaded. The player-count ceiling is derived from MAXPLAYERS specifically because upstream 5.0.0 removed the old...

**Reached by.** Main menu -> "Multiplayer" (fork-added TextItem in MainMenu and MainMenuTextOnly, menudef.txt:77/100/132), and Options -> Multiplayer (menudef.txt:393), and the simplified options page (menudef.zsimple:20) -> Host Game / Join Game. No gating cvar; the lists are built at startup from menudef.cpp:1837. mp_launch_host / mp_launch_join / mp_reset_defaults are...

**Files.** `src/menu/doommenu.cpp`, `src/common/engine/multiplayerlaunch.cpp`, `src/common/engine/multiplayerlaunch.h`, `wadsrc/static/menudef.txt`, `wadsrc/static/menudef.zsimple`

### MENUDEF language extensions
`[EXTENDS]` `[active]`

Six additions to the MENUDEF parser and item set. (1) ifOption gains runtime predicates evaluated against live cvars rather than compile flags: OpenXR, OpenVR, NonVR (all reading vr_mode), Developer1 and Developer2 (reading `developer`), plus compile-time VulkanRender. (2) A new `ifnotoption` block inverts any of them. (3) `AutoScroll [speed]` makes a page scroll itself continuously, sub-pixel-smooth, looping at the end and cancelling on any wheel or...

**Why.** A single menudef has to serve two VR runtimes with different capabilities and a flat-screen build, and has to hide developer-only rows without a rebuild -- upstream's ifOption could only answer compile-time questions. AutoScroll exists because an instructions or credits page in a headset cannot be scrolled comfortably by hand. The added...

**Reached by.** MENUDEF authoring only; the parser/item extensions themselves are ungated and always compiled in. Three MENUDEF content blocks sit behind runtime gates, all menu-reachable without a keyboard: Options -> Miscellaneous -> "Show experimental options" (menu_showexperimental, default false, CVAR_ARCHIVE, persists); Controls -> "Show double bindings"...

**Files.** `src/common/menu/menudef.cpp`, `src/common/menu/menu.cpp`, `src/common/menu/menu.h`, `wadsrc/static/zscript/engine/ui/menu/optionmenu.zs`, `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs`, `wadsrc/static/menudef.txt`

### Menus that let the world keep running (DontPause)
`[EXTENDS]` `[active]`

A per-menu `DontPause` flag, readable from ZScript, that keeps the playsim ticking while that menu is on top. P_CheckTickerPaused consults it as a third case alongside the existing pause conditions, and only the menu currently on top decides -- a non-pausing page opened from a pausing one still runs. Separately, a menu's Ticker now runs inside the InMenu guard, so a page can write non-mod cvars from its tick instead of only from a keypress.

**Why.** A page whose whole purpose is to adjust what you are looking at -- lighting, glow, exposure, colour -- promises a live preview it cannot deliver while the playsim is stopped: nothing re-evaluates, and anything driven from a tic is frozen. Making it opt-in and per-menu keeps the real reason to pause intact for the save menu, where the...

**Reached by.** ZScript API only. A Menu subclass sets the native field `DontPause = true` (declared wadsrc/static/zscript/engine/ui/menu/menu.zs:180, exported writable via DEFINE_FIELD(DMenu, DontPause) at src/common/menu/menu.cpp:1165). It is NOT a MENUDEF keyword — menudef.cpp has no parser entry for it — so a page declared purely in MENUDEF cannot opt in; it must be a...

**Files.** `src/common/menu/menu.h`, `src/common/menu/menu.cpp`, `src/p_tick.cpp`, `wadsrc/static/zscript/engine/ui/menu/menu.zs`

### Controller-first menu navigation and double-tap binding pages
`[EXTENDS]` `[active]`

Several changes that make the menus usable from a motion controller. Right-stick axes 3 and 4 now navigate the menu alongside the left stick, and the vertical axis polarity was corrected. In key-capture mode, controller buttons are routed straight to the menu's OnInputEvent so a Pad_* button can actually be recorded as a binding, and are swallowed so the same press cannot leak into gameplay. The joystick list is refreshed and the Options page's Joystick...

**Why.** Every button on a VR controller is scarce, so double-tap bindings roughly double what six buttons can reach -- and the binding UI has to be able to capture a controller button in the first place, which it could not while the menu was translating those presses into navigation. Devices come and go between sessions in VR, so a stale...

**Reached by.** Controller navigation, the key-capture routing, the joystick-row insert/remove, and the VR laser-as-menu-mouse all run unconditionally with no toggle to find. vr_menu_pointer (Bool, default true, vk_openxrdevice.cpp:163) has NO menu row anywhere - it is console-only, but since it defaults on nothing needs reaching. Its colour is Options -> VR Options ->...

**Files.** `src/common/menu/menu.cpp`, `src/common/menu/joystickmenu.cpp`, `src/menu/doommenu.cpp`, `wadsrc/static/menudef.txt`, `wadsrc/static/zscript/engine/ui/menu/optionmenuitems.zs`, `wadsrc/static/zscript/engine/ui/menu/menu.zs`

### In-flight: voxel roll-axis cvar rename
`[EXTENDS]` `[in flight]`

The Voxel Options page's held-voxel roll-axis slider and its defaults command are being renamed from vr_voxel_bodyyaw to vr_voxel_rollaxis. This is the only uncommitted change in the menu subsystem; the glow-lanes and surface-stamp work in the same working tree has not yet produced any menu rows.

**Why.** The old name described where the value was read rather than what it does, which is exactly the naming failure the engine's own conventions warn against -- a field named for its caller does not get reused.

**Reached by.** Options -> Mod Options -> Voxel Options -> "Held voxel roll axis" slider (range -1 to 270, step 90). Fully menu-reachable, no console needed; the same page's "Reset to defaults" row runs the resetcvar command internally. Menu chain confirmed at wadsrc/static/menudef.txt lines 382 (OptionsMenu), 4420 (ModOptionsMenu), 4452 (VRVoxelOptions), 4476 (the slider).

**Files.** `wadsrc/static/menudef.txt`

## 12. Android/Quest heritage and the SDL-free backend

> UZDXREMA descends from QuestZDoom, which ran GZDoom on a Quest headset as a native Android app with no SDL and no window system — the OpenXR runtime owned the screen, and Java/JNI owned input, paths and process lifetime. Two whole layers of that survive in the tree: a complete SDL-free posix platform backend (`src/posix/nosdl/`) and an ndk-build makefile set (`mobile/`), plus a scattering of `__MOBILE__` / `__ANDROID__` conditionals through the GL loader, GL backend, audio and filesystem code. None of it is compiled by the desktop CMake build; `mobile/` is not named anywhere in any CMakeLists.txt and `src/posix/nosdl/` is referenced only by `mobile/Android_src.mk`. What *is* live on PC is the seam the Quest port left behind: the QuestZDoom C API (`src/QzDoom/qzdoom_common.cpp`) is still the fork's internal VR state bus, `VR_OPENXR_MOBILE` is still the name of the desktop OpenXR mode,...

### SDL-free posix platform backend (nosdl)
`[NOT IN UZD]` `[legacy]`

A complete parallel implementation of GZDoom's posix platform layer with every SDL call commented out in place: video (`NoSDLGLVideo`/`SystemGLFrameBuffer`), input, joystick, GUI cursor, TTY startup screen and crash catcher. The framebuffer never swaps (`SwapBuffers()` body is the comment "No swapping required" — the XR runtime presents), always reports fullscreen, forces `vid_preferbackend = 0` (OpenGL), and gets its resolution by calling out to...

**Why.** On a standalone headset there is no window, no desktop mouse, no clipboard and no buffer swap the engine controls. Stock GZDoom's posix backend is written against SDL2 for all of those, so the Quest port needed a backend that satisfies the same link-time contract while doing nothing.

**Reached by.** Internal, no user surface, and dead in every build this tree can produce. src/CMakeLists.txt (lines 526-534) compiles common/platform/posix/sdl/* for the posix desktop build and never names src/posix/nosdl; the only reference in the whole tree is mobile/Android_src.mk (PLAT_NOSDL_SOURCES, lines 115-123, consumed at line 558). That Android path is itself...

**Files.** `src/posix/nosdl/glvideo.cpp`, `src/posix/nosdl/i_system.cpp`, `src/posix/nosdl/st_start.cpp`, `src/posix/nosdl/i_input.cpp`, `src/posix/nosdl/i_joystick.cpp`, `src/posix/nosdl/hardware.cpp`

### Android NDK build plumbing
`[NOT IN UZD]` `[legacy]`

An ndk-build makefile set that builds the engine as a `qzdoom` shared library plus static lzma/bzip2/ZWidget libs. It defines `-D__MOBILE__ -DUSE_OPENXR -DNO_SWRENDERER -DNO_GTK -DNO_SSE`, selects the nosdl backend rather than the SDL one, and links against `openal openxr_loader zmusic`. It is also what keeps `src/gl/stereo3d/gl_openxrdevice.cpp` alive — that file is in no CMakeLists.txt and is compiled only here.

**Why.** The Quest build is a native Android APK, which ndk-build/Application.mk drives rather than CMake. It has to enumerate every source file by hand because it cannot run the code generators the CMake build depends on.

**Reached by.** Internal build plumbing only — no cvar, menu path, bind, or ZScript API. Nothing in the running engine touches it. It is dead against 5.0.0 and would not build as written: five sources it lists no longer exist in src/ (common/thirdparty/richpresence.cpp:157, common/thirdparty/sfmt/SFMT.cpp:359, common/widgets/widgetresourcedata.cpp:366,...

**Files.** `mobile/Android_src.mk`, `mobile/Android.mk`, `mobile/Android_zwidget.mk`, `mobile/Android_zlib.mk`, `mobile/Android_bzip2.mk`, `mobile/Android_lzma.mk`

### Checked-in stand-ins for CMake-generated build artifacts
`[NOT IN UZD]` `[legacy]`

`mobile/src/extrafiles/` is placed on the Android include path and supplies pre-generated copies of files the CMake build produces at configure time: `sc_man_scanner.h` (re2c output for the ZScript lexer), `gitinfo.h` (empty `GIT_DESCRIPTION`/`GIT_HASH`/`GIT_TIME`), plus dtoa's `arith.h`/`gd_qnan.h`. An ETC1 texture codec (`etc1.cpp`) also sits there, referenced by nothing.

**Why.** ndk-build cannot run re2c, lemon or the CMake git-info step, so the Quest build shipped snapshots of their output instead.

**Reached by.** Internal, no user surface — no cvar, menu entry, bind, or ZScript API. Reachable only by the ndk-build Android toolchain: mobile/Android_src.mk:99 is the sole reference to the directory in the entire tree, adding it to LOCAL_C_INCLUDES. It is on no CMake include path, so the desktop build never sees it. Committed, not in-flight.

**Files.** `mobile/src/extrafiles/sc_man_scanner.h`, `mobile/src/extrafiles/gitinfo.h`, `mobile/src/extrafiles/etc1.cpp`, `src/common/scripting/frontend/zcc_parser.cpp`

### Android filesystem layout and relocated base pk3
`[REPLACES]` `[dormant]`

On Android every engine path is rooted at the process working directory rather than a home directory: `M_GetAppDataPath`/`M_GetConfigPath` return `./config/`, plus `./cache/`, `./saves/`, `./screenshots/`. `version.h` moves the base pk3 to `res/doomxr.pk3` under `__ANDROID__` (`doomxr.pk3` everywhere else), and `_BaseFileSearch` gains a `./res/<file>` probe under `__MOBILE__` so IWADs and pk3s bundled as APK assets are found.

**Why.** An Android app's writable storage is its own private directory and its read-only data is an assets tree; the unix `~/.config/gzdoom` convention does not exist there.

**Reached by.** Internal, no user surface — no cvar, menu entry, bind, or ZScript API. `mobile/src/i_specialpaths_android.cpp` is pulled in only by `ANDROID_SRC_FILES` in `mobile/Android_src.mk`, and that makefile is the sole definer of `__MOBILE__` (`LOCAL_CFLAGS := -D__MOBILE__ ...`, line 9), so both the `version.h` `__ANDROID__` branch and the `findfile.cpp`...

**Files.** `mobile/src/i_specialpaths_android.cpp`, `src/version.h`, `src/common/utility/findfile.cpp`

### GLES entry-point loader wired into the desktop GL loader
`[EXTENDS]` `[legacy]`

`gl_load.c` gains a mobile branch of `IntGetProcAddress` that `dlopen`s one of three GL providers selected by an int `glesLoad`: `libjwzgles_shared.so` (1, with a `jwzgles_` symbol prefix), `libGL4ES.so` (2, calling its `initialize_gl4es`), or `libGLESv3.so` (3, the default). Any entry point that fails to resolve is pointed at a stub that logs "CAUGHT BAD" instead of returning null, so a missing function is a no-op rather than a crash. `gl_load.h` then...

**Why.** GZDoom's generated GL loader assumes desktop GL and an SDL or WGL/GLX proc-address source. On Quest there is neither, and desktop GL entry points such as `glClearDepth`/`glDepthRange` (double-precision) simply do not exist in GLES.

**Reached by.** Not reachable in this tree. Every part is inside `#ifdef __MOBILE__`, and `__MOBILE__` is defined only by `mobile/Android_src.mk` (an ndk-build fragment: `LOCAL_CFLAGS := -D__MOBILE__ ...`), which CMake never consumes; `src/CMakeLists.txt` lists only `common/platform/posix/sdl/*` and never mentions `src/posix/nosdl`, so the desktop/Windows build compiles...

**Files.** `src/common/rendering/gl_load/gl_load.c`, `src/common/rendering/gl_load/gl_load.h`, `src/posix/nosdl/glvideo.cpp`

### GLES dialect and capability conditioning through the GL backend
`[EXTENDS]` `[dormant]`

A set of `__MOBILE__` branches that retarget the desktop OpenGL backend at GLES 3.1: both shader paths emit `#version 310 es` instead of a computed `#version`, `gl_shaderprogram.cpp` injects a `precision highp` preamble for int/float/sampler2D/sampler2DArray/samplerCube/sampler2DMS, and `gl_system.h` defines `ES_VERSION_STR "#version 310 es"`. `gl_interface.cpp` pins `gl_version = 3.31` and pre-sets `RFL_NO_CLIP_PLANES | RFL_INVALIDATE_BUFFER |...

**Why.** GLES is not a subset of desktop GL at the source level — a different `#version` string, mandatory precision qualifiers, no `glMapBuffer`, no depth clamp, no line smoothing, and a driver whose extension string cannot be trusted to describe what actually works. Feature detection has to be overridden rather than queried.

**Reached by.** Two different surfaces, and the entry conflates them. (1) The GLES retargeting itself: no user surface at all, and stronger than "dormant by default" — `__MOBILE__` is defined in exactly one place in the tree, `mobile/Android_src.mk:9` (an Android NDK makefile), and never by CMake, so on the desktop build the owner actually uses every one of these branches...

**Files.** `src/common/rendering/gl/gl_shader.cpp`, `src/common/rendering/gl/gl_shaderprogram.cpp`, `src/common/rendering/gl_load/gl_interface.cpp`, `src/common/rendering/gl/gl_hwtexture.cpp`, `src/common/rendering/gl/gl_framebuffer.cpp`, `src/common/rendering/gl_load/gl_system.h`

### GL capability kill-switch cvars
`[NOT IN UZD]` `[dormant]`

Three fork-added cvars are applied immediately after extension detection, clearing capability flags the driver claimed to have: `gl_no_ssbo` clears `RFL_SHADER_STORAGE_BUFFER`, `gl_no_persistent_buffer` clears `RFL_BUFFER_STORAGE`, `gl_no_clip_planes` sets `RFL_NO_CLIP_PLANES`. They sit in the same detection block as, and generalise, the `__MOBILE__` hard-coded flag forcing above.

**Why.** The GLES lineage established that a driver's advertised feature set cannot be taken at face value and has to be forced down. On desktop the same need appears with flaky vendor drivers, but it has to be switchable at runtime rather than at compile time, so it became cvars instead of `#ifdef`s.

**Reached by.** All three default to false and do nothing until set. They are read once per renderer init, in gl_LoadExtensions(), called from OpenGLFrameBuffer::InitializeState() (src/common/rendering/gl/gl_framebuffer.cpp:123) — so a change only bites after the GL backend re-initialises, and menudef.txt has no vid_restart entry. All three are declared with flag 0 (no...

**Files.** `src/common/rendering/gl_load/gl_interface.cpp`, `src/common/rendering/hwrenderer/data/hw_cvars.cpp`, `src/common/rendering/hwrenderer/data/hw_cvars.h`, `wadsrc/static/menudef.txt`

### QuestZDoom C API kept as the fork's VR state bus
`[NOT IN UZD]` `[active]`

`src/QzDoom/qzdoom_common.cpp` is the surviving QuestZDoom interface, and it is compiled into the desktop binary. It owns the Quest-era file-scope globals the whole VR playsim reads — `weaponangles`, `weaponoffset`, `offhandangles`, `offhandoffset`, `hmdorientation`, `hmdPosition`, `worldPosition`, `positionDeltaThisFrame`, `snapTurn`, `cinemamode`, `doomYaw` — and the functions that write and read them (`VR_SetHMDOrientation`, `VR_SetHMDPosition`,...

**Why.** On Quest this was a genuine C boundary between the Java/JNI/OpenXR host and the engine. Rather than untangle it for PC, the fork kept the boundary as an internal data bus and stubbed the host side, so the VR playsim code that reads these globals did not have to change.

**Reached by.** Internal C API only — no cvar, menu path, or bind. `src/QzDoom/qzdoom_common.cpp` is listed unconditionally in the main source list at `src/CMakeLists.txt:1038` (with `QzDoom/*.h` at :729), both fork-added lines. The bus is genuinely live in the desktop build: `VR_SetHMDOrientation`/`VR_SetHMDPosition` are written by the OpenXR device...

**Files.** `src/QzDoom/qzdoom_common.cpp`, `src/QzDoom/VrCommon.h`, `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/CMakeLists.txt`

### Process-level hard restart, heritage-branched
`[EXTENDS]` `[active]`

`QzDoom_Restart()` returns immediately under `__ANDROID__` (the Java launcher relaunched the activity) and, under `_WIN32`, re-executes the module image with `ShellExecuteW(nullptr, L"open", path, GetCommandLineW(), ...)`. The main loop branches on the same heritage: `#if defined(USE_OPENXR) && defined(__ANDROID__)` does `InitShutdown(); QzDoom_Restart();`, while desktop falls through to the normal in-process `D_Cleanup()` / `gamestate = GS_STARTUP`...

**Why.** Restarting a VR session cleanly is harder than restarting a flatscreen one — an XR session, its swapchains and its runtime connection do not always survive an in-process teardown. Android had no choice but to relaunch the process; the fork gave desktop the same escape hatch for the cases where in-process cleanup is not enough.

**Reached by.** Menu, and fully usable without a keyboard: Options -> Multiplayer -> Host Game (or Join Game) -> "Enter", which are SafeCommand items in wadsrc/static/menudef.txt (HostMultiplayerMenu line 2218, JoinMultiplayerMenu line 2231) firing the MENUDEF-only CCMDs mp_launch_host / mp_launch_join (src/menu/doommenu.cpp:994, :1001). Those call...

**Files.** `src/QzDoom/qzdoom_common.cpp`, `src/d_main.cpp`

### VR_OPENXR_MOBILE is the desktop OpenXR mode
`[NOT IN UZD]` `[active]`

The stereo-mode enum value 15 still carries its Quest-era name, `VR_OPENXR_MOBILE`, but it is the fork's live desktop OpenXR mode. It is implemented on the Vulkan backend only and falls back to mono on OpenGL and GLES. Roughly a dozen call sites across the video, menu, resolution and render-scale code branch on it, and both the Windows and posix/SDL video backends check it to decide whether to build an XR-compatible Vulkan instance.

**Why.** Pure heritage: the fork went from OpenXR-on-Quest to OpenXR-on-PC without renaming the enumerator. Worth knowing because the name reads as a mobile-only path and is not one.

**Reached by.** Menu, no console needed. Options -> Display Options -> "$DSPLYMNU_GLOPT" (OpenGL/Vulkan Options, menudef.txt:1375) -> "$GLPREFMNU_VRMODE" (Stereo 3D VR submenu, menudef.txt:2487) -> "$GLMNU_3DMODE" -> the entry whose text is "OpenXR Mobile" ($OPTVAL_OPENXR, value 15). Backed by cvar vr_mode (CVAR_GLOBALCONFIG|CVAR_ARCHIVE, default 0 = mono), so it is a...

**Files.** `src/common/rendering/hwrenderer/data/hw_vrmodes.h`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `wadsrc/static/menudef.txt`, `src/common/platform/posix/sdl/sdlglvideo.cpp`, `src/common/platform/win32/win32vulkanvideo.h`

### Orphaned Android hooks inside the stock SDL backend
`[EXTENDS]` `[legacy]`

The upstream SDL posix backend carries a second, separate set of Android conditionals: `main` is renamed `main_android` under `__ANDROID__`, followed by a 500ms `usleep` and explicit `exit(0)` after `SDL_Quit()`; fatal errors are mirrored to logcat and to `LogWritter_Write()`; the startup screen redefines `fprintf` to a `my_fprintf` that forwards to `addTextConsoleBox()`; joystick shutdown is skipped entirely with the comment "Causes crash"; and relative...

**Why.** An earlier Android port ran on SDL before the nosdl backend existed, and these hooks were how it got an entry point, a crash log and an on-screen console on a device with no terminal.

**Reached by.** Internal, no user surface, and dead in every configuration in this tree. There is no cvar, menu entry, or bind — the only gate is compile-time. On the owner's Windows/MSVC build (build-dxr) these five files are not compiled at all: src/CMakeLists.txt:553 puts PLAT_SDL_SOURCES into OTHER_SYSTEM_SOURCES under `if( WIN32 )`, and :1427 marks that whole list...

**Files.** `src/common/platform/posix/sdl/i_main.cpp`, `src/common/platform/posix/sdl/i_system.cpp`, `src/common/platform/posix/sdl/st_start.cpp`, `src/common/platform/posix/sdl/i_joystick.cpp`, `src/common/platform/posix/sdl/i_input.cpp`

### Mobile mono downmix in the OpenAL renderer
`[EXTENDS]` `[dormant]`

Under `__MOBILE__`, `OpenALSoundRenderer::LoadSound` downmixes every multi-channel sample to mono before upload. The fork's 5.0.0-merge work fixed a real defect in it: the AL format and sample size were still being computed from the layout the decoder reported, not the mono layout the buffer actually ends up with, so the format description and the data disagreed. The fix recomputes both with `ChannelConfig_Mono` when monoize is active, and moves the...

**Why.** The original comment records the reason for the downmix bluntly — 3D sounds were far too loud on the headset without it — and admits it makes every sound mono. It is a heritage workaround that was never properly fixed, but it is still being maintained rather than left to rot.

**Reached by.** No user surface at all. The whole downmix path is inside `#ifdef __MOBILE__`, and `__MOBILE__` is never defined by any tracked build file in this repo (`git grep "define __MOBILE__"` is empty; the only other consumers are three render-side `#ifdef`s in gl_hwtexture.h, gl_load.h and gl_system.h). There is no Android toolchain in-tree, so this compiles out of...

**Files.** `src/common/audio/sound/oalsound.cpp`

## 13. Multiplayer, build system, branding and tooling

> This is the part of UZDXREMA that lets a headset-only player start, join and talk in a netgame without ever touching a keyboard, plus everything that makes the tree build as, and call itself, "DoomXR" instead of "UZDoom". Stock UZDoom 5.0.0 assumes a desktop: its netgame is command-line only (`-host` / `-join`), its lobby is a desktop ZWidget window, and its chat prompt reads raw keystrokes. The fork adds a menu front end that writes a transient command line and relaunches the process, an in-headset lobby that renders through the stereo device, world-space name/health tags over other players, and on-screen character grids for chat, console and cheats. Around that sit the rename (executable, pk3, ini, save/cache paths, resources), a fork-owned string table that coexists with upstream's generated one, and the CMake/vcpkg wiring plus a one-shot script that actually produces a VR-capable...

### Menu-driven multiplayer host/join with self-relaunch
`[NOT IN UZD]` `[active]`

Adds Host Game and Join Game menus that collect player count, skill, map, game mode, net mode and a server address into archived cvars, then translate them into a real command line. `M_BeginPendingMultiplayerLaunch()` validates the request, writes `progdir/commandline_mp.txt` (`doomxr -host 4 -netmode 0 -skill 3 +map MAP01`), sets `wantToRestart`, and marks a pending hard restart. `D_DoomMain` returns `GAMEEXIT_HARD_RESTART` (1338) instead of doing the...

**Why.** A VR player has no console and no command line. Upstream 5.0.0 exposes netgame setup only through `-host`/`-join` args, so without this a headset user simply cannot start or join a multiplayer game. Relaunching rather than restarting in place is required because the network stack is brought up during boot argument parsing, not at runtime.

**Reached by.** Fully menu-reachable, no console needed. Main menu -> "Multiplayer" (a fork-added TextItem present in the Doom/Strife/Chex, Heretic/Hexen and MainMenuTextOnly main menus), or Options -> Multiplayer. That MultiplayerMenu holds three rows: Player Setup (NewPlayerMenu), Host Game (HostMultiplayerMenu), Join Game (JoinMultiplayerMenu). Host rows are plain...

**Files.** `src/common/engine/multiplayerlaunch.cpp`, `src/menu/doommenu.cpp`, `wadsrc/static/menudef.txt`, `src/d_main.cpp`, `src/common/engine/multiplayerlaunch.h`, `src/QzDoom/qzdoom_common.cpp`

### In-headset net-wait lobby with cancel-to-single-player
`[REPLACES]` `[active]`

Mirrors the whole lobby state (role, phase, status message, found/total player counts, source context) into a session object in i_net.cpp, then picks a backend: the desktop `NetStartWindow` widget, or a VR shell that draws the same information through the active stereo device. `VR_NetWaitLoop()` runs its own pump - `I_GetEvent`, `D_ProcessEvents`, `screen->BeginFrame()/Update()`, callback every 500 ms - and `VR_RenderNetWaitShellContents()` composes a...

**Why.** The upstream lobby is a desktop window the player cannot see or dismiss while wearing a headset, and a failed or abandoned connect attempt aborts the whole boot. Both make VR multiplayer unusable: you would be stuck staring at a frozen scene with a dialog you cannot reach, and giving up would close the game.

**Reached by.** Two layers, both keyboard-free. (1) Getting into a lobby at all: the fork adds an in-headset Options > Multiplayer > Host/Join menu (`HostMultiplayerMenu` / `JoinMultiplayerMenu` in wadsrc/static/menudef.txt, fork-only - absent from 5.0.0), whose SafeCommand entries fire the fork CCMDs `mp_launch_host` / `mp_launch_join` (src/menu/doommenu.cpp:994-1002)....

**Files.** `src/common/engine/i_net.cpp`, `src/common/engine/i_net.h`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `src/common/rendering/vulkan/stereo3d/vk_openxrdevice.cpp`, `src/rendering/gl/stereo3d/gl_openvr.cpp`, `src/common/rendering/gl/gl_postprocess.cpp`

### World-space name and health tags over other players
`[NOT IN UZD]` `[active]`

A built-in EventHandler spawns one `VisualThinker` per remote player carrying a 1024x256 canvas texture, drawn each time the name, health or visibility state changes: the player name in SmallFont at 8x scale, and a framed health bar (dark red track, green fill). The tag sits at the player's head (actor height minus floorclip, plus crouch compensation) and switches presentation by mode - in VR it uses a separate texture set, non-uniform scale (0.34 x...

**Why.** Doom has no player identification in world space; upstream shows nothing over other players. In VR there is no HUD scoreboard you can comfortably read and no mouse cursor to hover, so who a distant figure is - and whether they are hurt - has to be legible in the world itself, and it has to stay readable when the viewer tilts their head.

**Reached by.** Options -> Player (the "NewPlayerMenu" player-setup menu, menudef.txt:493-494) as "$PLYRMNU_MULTIPLAYER_NAMETAG" / "$PLYRMNU_MULTIPLAYER_HEALTHBAR", both using the OptionValue "OffAimAlways" (0 Off / 1 On Aim / 2 Always). Backed by `CVAR (Int, cl_otherplayernames, 2, CVAR_ARCHIVE)` and `CVAR (Int, cl_otherplayerhealth, 2, CVAR_ARCHIVE)` in...

**Files.** `wadsrc/static/zscript/otherplayertags.zs`, `wadsrc/static/animdefs.txt`, `src/d_netinfo.cpp`, `wadsrc/static/mapinfo/common.txt`, `src/scripting/vmthunks.cpp`, `wadsrc/static/menudef.txt`

### Keyboardless text entry for chat, console and cheats
`[REPLACES]` `[active]`

Replaces the raw in-viewport chat prompt with a modal ZScript menu holding a 13x5 character grid (A-Z, digits, punctuation, space, backspace) plus a live preview of the typed string. `messagemode` / `messagemode2` no longer set `chatmodeon`; they call `CT_OpenTextEntryMenu(teamChat)`, which resets button states, hides the console, clears the pending message and opens `ChatTextEnterMenu`. Submitting calls back into `CT_SubmitTextEntryMenuMessage()` which...

**Why.** There is no keyboard in play. Without an on-screen grid a VR player cannot say anything in a netgame, cannot type a console command, and cannot use a cheat - all three are keystroke-only in upstream.

**Reached by.** Chat: bind "Say" (messagemode) / "Team say" (messagemode2) in Options -> Controls (menudef "ChatControlsMenu", lines 786-787; the same two also appear in "ChatDoubleControlsMenu" as DoubleControl rows, 804-805). All bindable, no console needed. Console grid: no bind at all - System_ConsoleToggled (src/d_main.cpp:3679) opens ConsoleTextEnterMenu...

**Files.** `wadsrc/static/zscript/engine/ui/menu/chattextentermenu.zs`, `src/ct_chat.cpp`, `wadsrc/static/zscript/engine/ui/menu/consoletextentermenu.zs`, `wadsrc/static/zscript/engine/ui/menu/cheatmenu.zs`, `src/common/scripting/interface/vmnatives.cpp`, `src/d_main.cpp`

### DoomXR identity across executable, data and OS integration
`[REPLACES]` `[active]`

Renames the engine end to end. version.h sets `GAMENAME "DoomXR"`, `WGAMENAME`, `GAMENAMELOWERCASE "doomxr"`, `APPID "org.zdoom.DoomXR"` and `BASEWAD "doomxr.pk3"` (with an `__ANDROID__` branch pointing at `res/doomxr.pk3`). CMake follows: `project(DoomXR)`, `ZDOOM_EXE_NAME "doomxr"`, and `add_pk3(doomxr.pk3 ...)` with every dependent target renamed to `doomxr_pk3`. The Windows resource script reports DoomXR as FileDescription / InternalName / ProductName...

**Why.** The fork is a separate product with its own data pk3, its own config, and its own save/cache locations - sharing `uzdoom.ini` and `uzdoom.pk3` with a stock UZDoom install on the same machine would let two very different engines fight over the same settings. Keeping GAMESIG means saves are not gratuitously invalidated by the rename alone.

**Reached by.** Internal, compiled in unconditionally — no cvar, no menu, nothing to toggle. Visible to the owner as: the doomxr.exe / doomxr.pk3 filenames, doomxr.ini (and doomxr_portable.ini) and the per-user config/save/cache dirs, and the Windows window caption "DoomXR <version>" (src/common/platform/win32/i_main.cpp:325, i_mainwindow.cpp:185) plus the "DoomXR Very...

**Files.** `src/version.h`, `CMakeLists.txt`, `wadsrc/CMakeLists.txt`, `src/win32/zdoom.rc`, `src/posix/freedesktop/org.zdoom.UZDoom.desktop`, `src/common/platform/win32/i_specialpaths.cpp`

### Fork string table in language.0 / language.1
`[EXTENDS]` `[active]`

Carries roughly 525 fork-only string keys - every VR, laser, tracer, weapon-wheel, portable-HUD, performance, developer, experimental, multiplayer and Info-and-Credits menu label - in two CSV-format lumps that sit beside upstream's build-generated `static/language.csv`. Because the filesystem strips extensions, `language.0` and `language.1` both load as lumps named LANGUAGE and are picked up by `stringtable.cpp`'s scan, so fork and upstream strings...

**Why.** Upstream 5.0.0 generates `static/language.csv` at build time from `libraries/Translation/*.po` and then deletes it from the source tree in a POST_BUILD step. Any fork string put in that file would be destroyed on every build, and the fork's keys do not exist in the .po database - so they need a filename the generator never touches but...

**Reached by.** Internal, no user control. These are the `$OPTMNU_*`, `$VRPREFMNU_*`, `$VRLASERMNU_*`, `$VRHUDMNU_*`, `$VRWHEELMNU_*`, `$DEVOPTMNU_*`, `$EXPMNU_*`, `$INFOCREDMNU_*` etc. keys that `wadsrc/static/menudef.txt` dereferences. There is no cvar, bind or menu that touches them; they are reached only as the rendered text of every fork menu. Spot-checked live:...

**Files.** `wadsrc/static/language.0`, `wadsrc/static/language.1`, `wadsrc/CMakeLists.txt`, `src/common/engine/stringtable.cpp`

### DoomXR boot logo, menu mark and Info & Credits screens
`[REPLACES]` `[active]`

Replaces the engine's BOOTLOGO graphic (read by `startscreen_generic.cpp`) with the DoomXR mark - a marine wearing a VR headset - and adds a `DOOMXR` patch (the same marine plus "Doom XR Edition" wordmark) used as the header on three new menus: InfoAndCreditsMenu, DoomXRInstructions (an auto-scrolling how-to-play page of 20 localised lines) and DoomXRCredits (an auto-scrolling lineage list naming the QuestZDoom/DXR chain). The `branding/` directory...

**Why.** A fork that renames its binary but boots into somebody else's logo is confusing, and the credits chain (DrBeef/QuestZDoom -> Emawind -> Ermac -> here) is worth showing in-headset where there is no README to read. The instructions page exists because VR controls are not discoverable from a keybind list.

**Reached by.** Boot logo appears on the startup screen automatically (stock BOOTLOGO lookup, fork-replaced art). Menus: Options -> Info & Credits -> Instructions / Credits. Single entry point at menudef.txt:379, the second item of OptionsMenu; menu-only, no cvar and no console command, fully reachable in a headset.

**Files.** `wadsrc/static/graphics/DOOMXR.png`, `wadsrc/static/graphics/bootlogo.png`, `wadsrc/static/menudef.txt`, `branding/BRANDING-LICENSE.md`, `branding/banner.png`, `src/common/startscreen/startscreen_generic.cpp`

### OpenVR / OpenXR CMake wiring and vcpkg manifest control
`[NOT IN UZD]` `[active]`

Adds two build options that upstream does not have. `ENABLE_OPENVR` defines USE_OPENVR, pulls in `win32/i_openVR.cpp` and `rendering/gl/stereo3d/gl_openvr.cpp`, locates Valve's SDK by its `headers/openvr.h` layout, and offers three linkage modes (`DYN_OPENVR` dynamic load, `STATIC_OPENVR` compiling the SDK's own sources into the exe, or plain shared linking). `ENABLE_OPENXR` defines USE_OPENXR and adds `common/platform/win32/i_openXR.cpp`,...

**Why.** Upstream UZDoom has no VR code and therefore no way to find, link or ship an OpenXR or OpenVR runtime. Without this wiring none of the fork's stereo devices compile at all. The pre-project vcpkg ordering matters because getting it wrong silently produces a build with the wrong triplet and no manifest features.

**Reached by.** Configure-time only — nothing user-facing, so the no-keyboard/VR constraint does not apply. In practice it is reached by running E:\UZDXREMA\auto-setup-windows-vr.cmd (fork-only, tracked, repo root), which clones vcpkg/zmusic/openvr into build-dxr\ and configures with -DENABLE_OPENXR=ON -DOPENXR_DIR=<vcpkg installed> -DENABLE_OPENVR=ON -DOPENVR_SDK_PATH...

**Files.** `src/CMakeLists.txt`, `CMakeLists.txt`, `vcpkg.json`, `.github/workflows/continuous_integration.yml`

### auto-setup-windows-vr.cmd: one-command VR build
`[NOT IN UZD]` `[active]`

A self-contained Windows build script. It locates Visual Studio via vswhere, adds the CMake that VS ships (which is normally not on PATH), creates a dedicated `build-dxr` folder so it never fights an existing `build` directory over a locked DLL, and clones what the repo does not carry: microsoft/vcpkg, zdoom/zmusic and ValveSoftware/openvr. It installs openxr-loader and openal-soft in vcpkg *classic* mode - required, because running inside the repo makes...

**Why.** Getting a VR-capable build of this fork right requires roughly a dozen interacting decisions (SDK layouts, vcpkg mode, triplet, exe name cache type, DLL staging), each of which fails in a non-obvious way. Encoding them once, with the reasoning inline, is the difference between a reproducible build and a rediscovery every time.

**Reached by.** Run auto-setup-windows-vr.cmd — it derives its own repo root from %~dp0, so it works from any working directory or a double-click, not only from the repo root. Desktop/build-time operation at a real shell; the VR no-keyboard constraint does not apply. Output: build-dxr\RelWithDebInfo\doomxr.exe, with an IWAD dropped in that folder to play.

**Files.** `auto-setup-windows-vr.cmd`

### Command-line launch profiles chosen from a menu
`[NOT IN UZD]` `[active]`

Scans the program directory, `$PROGDIR/profiles/` and every `FileSearch.Directories` path in the config for files named `commandline_<name>.txt`. If such a file opens with `#TITLE <text>` the rest of that line becomes the display name, otherwise the filename stem is used; the list is sorted case-insensitively with a "No profile" entry pinned first. Picking one sets the archived `cmdlineprofile` cvar, which strips the previous profile's arguments (`-iwad`,...

**Why.** The no-autoload rule means every mod set has to be named on the command line, and a VR player has no command line. Profiles turn "which stack of pk3s am I launching with" into a menu pick plus a restart, and are also what `commandline_mp.txt` in the multiplayer launch path is modelled on.

**Reached by.** Accurate and fully usable in VR. `wadsrc/static/menudef.txt:412` — `LabeledSubmenu "$OPTMNU_ACTIVE_PROFILE", "cmdlineprofile", "CommandLineProfileMenu"` sits directly in the top-level `OptionsMenu`, showing the current cvar value; `CommandLineProfileMenu` (menudef.txt:366) is an otherwise empty protected menu whose rows are appended at menu-build time by...

**Files.** `src/menu/profiledef.cpp`, `src/menu/profiledef.h`, `src/d_main.cpp`, `src/menu/doommenu.cpp`, `src/m_misc.cpp`, `wadsrc/static/menudef.txt`

### Offline asset pipelines: fbx2iqm and the SDF font generator
`[NOT IN UZD]` `[active]`

`tools/fbx2iqm/fbx2iqm.py` is a deterministic FBX-to-IQM pipeline with two modes: BUILD, driven headless through Blender against a JSON config, and VERIFY, which re-reads the written .iqm's raw bytes using only the standard library. It never calls transform_apply, instead left-multiplying every participating object by one common matrix so mesh and armature stay in the same frame; it picks the scale so the armature's world scale lands on exactly 1.0...

**Why.** VR puts the player's face centimetres from hand models and readable text, so both pipelines exist to remove a class of silent failure: an IQM that exports 'FINISHED' with zero joints or a 100x scale error, and font atlases that look fine at HUD size and fall apart when you walk up to a card. Both tools verify from written bytes rather...

**Reached by.** Command line only, run by hand, and correct as written: `blender.exe -b --factory-startup --python tools/fbx2iqm/fbx2iqm.py -- --config <cfg.json>` for BUILD, `python tools/fbx2iqm/fbx2iqm.py --verify <file.iqm> [--config <cfg.json>]` for VERIFY (argparse declares exactly `--config` and `--verify`; the `bpy` import is inside a try/except, so VERIFY needs no...

**Files.** `tools/fbx2iqm/fbx2iqm.py`, `tools/fbx2iqm/hand_left.json`, `tools/fbx2iqm/beretta_m9.json`, `tools/sdffont/mksdf.ps1`, `tools/sdffont/sdfpreview.ps1`

### Dormant build paths and orphaned data carried in the tree
`[NOT IN UZD]` `[legacy]`

Several fork-only paths exist but are not currently live. (a) `mobile/` holds an ndk-build tree for the Quest standalone target - `LOCAL_MODULE := qzdoom`, `-DUSE_OPENXR -DNO_SWRENDERER`, the `src/posix/nosdl/` headless platform layer, `mobile/src/i_specialpaths_android.cpp` - but Android_src.mk lists QzDoom sources (TBXR_Common.cpp, QzDoom_OpenXR.cpp, OpenXrInput.cpp, VrInputCommon.cpp, VrInputDefault.cpp, mathlib.c, matrixlib.c, argtable3.c) that are...

**Why.** Worth naming so a future session does not mistake any of it for live infrastructure - particularly the Android tree (it looks complete but cannot build) and the two xg files (they look like a music customisation but the engine never reads them). The CI note is the opposite case: a real gap that is deliberately recorded rather than...

**Reached by.** Internal, no user surface at all — no cvar, menu path, or bind exists for any of it, so it is unreachable to the VR player by construction rather than merely console-gated. mobile/ is driven by ndk-build and is not referenced by any CMakeLists or CI workflow; the AppImage recipe by appimage-builder (and CI now uses build_core.yml's linuxdeploy path...

**Files.** `mobile/Android_src.mk`, `src/utility/data/xg.h`, `src/utility/data/xg.wopn`, `tools/AppImageBuilder.yml`, `.github/workflows/continuous_integration.yml`, `src/win32/QZDoomVR.bat`

## 14. Found by the completeness pass

> A final sweep over every changed area the thirteen subsystem passes did not claim -- the renderer BSP walk, the dynamic-light budget, the forked ZVulkan, audio, console and the retuned upstream defaults.

### Fake-flat resolution hoisted out of the render worker
`[REPLACES]` `[active]`

Upstream's single render worker resolved each wall job's front/back sector itself by calling hw_FakeFlat inside the worker thread. The fork deletes that block and computes front/back on the main thread in AddLine, passing both sectors (plus an is-culled bit) into the job. DoSubsector is also split, with everything after the visibility tests moved into a new ProcessVisibleSubsector.

**Why.** hw_FakeFlat mutates shared sector state, so only one worker could ever safely run it. Moving it to the producer side makes a wall job self-contained data, which is the precondition for handing walls to N workers. This is the change behind hw_bsp.cpp's 234 deleted lines.

**Reached by.** internal, no user surface

**Files.** `src/rendering/hwrenderer/scene/hw_bsp.cpp`, `src/rendering/hwrenderer/scene/hw_drawinfo.h`

### Multi-worker wall batching with a per-worker mesh merge
`[NOT IN UZD]` `[dormant]`

Adds a second job queue that hands walls to up to eight worker threads in batches, each worker building into its own HWMeshHelper (list / translucent / portals / missing-texture arrays). After the BSP walk the main thread merges every worker's mesh in order via PutWall/PutPortal/AddUpper-LowerMissingTexture. Segs that need main-thread handling (visual portals, Line_Horizon, sky and portal sectors) are detected up front and routed to the original...

**Why.** One worker cannot keep up with building two eyes of geometry at headset frame rates. The main-thread routing test exists because portal and sky state is not safe to touch from several threads at once.

**Reached by.** Options > Developer Options: "BSP worker threads" slider (gl_bsp_worker_threads, default 1 = off), "Wall batch size" (gl_bsp_wall_batch_size), "Sky sectors on main thread" (gl_bsp_worker_sky_mainthread); also on the VR Performance Tweak page

**Files.** `src/rendering/hwrenderer/scene/hw_bsp.cpp`, `src/rendering/hwrenderer/scene/hw_walldispatcher.h`, `wadsrc/static/menudef.txt`

### Line and decoration distance culling in the BSP walk
`[NOT IN UZD]` `[active]`

Two new culls run during the BSP descent. IsDistanceCulled drops segs whose both vertices are past gl_line_distance_cull (default 4000 units) onto a cheap culled wall path that still adds a clip range. ShouldCullDecorRenderThing skips things entirely past gl_sprite_decor_distance_cull, but only for actors that are not the player, not MF_SPECIAL/SHOOTABLE/MISSILE and not MF3_ISMONSTER.

**Why.** A VR frame is built twice, so distant geometry costs double. The decor cull is written around a gameplay-relevance test specifically so a decoration pack can be culled hard without ever making a monster or a pickup invisible.

**Reached by.** Options > OpenGL Preferences > Advanced: "Decorative sprite cull distance" and "Line cull distance" sliders; a "disablerendercull" command row and a reset row sit beside them

**Files.** `src/rendering/hwrenderer/scene/hw_bsp.cpp`, `src/console/c_cmds.cpp`, `wadsrc/static/menudef.txt`

### Dynamic-light budget: candidate scoring, distance culling and per-eye caps
`[EXTENDS]` `[active]`

Rewrites how lights reach walls, flats and render-hack planes. Each surface gathers at most gl_light_*_candidate_budget lights into a score-ordered array (score = distance/radius, best kept), then uploads at most gl_light_*_max_lights of them, counting against per-eye totals (lightsFlatPerEye / lightsWallPerEye) that reset once per eye. Lights past gl_light_distance_cull are rejected outright, and gl_light_max_intensity and gl_light_range_limit clamp what...

**Why.** Stock GZDoom uploads every light touching a surface, which in a heavily lit map is unbounded work done once per eye. The budget makes the cost per frame a number the player can set rather than a property of the map, and the per-eye counters are what stop eye two inheriting eye one's spend.

**Reached by.** Options > VR Performance Tweak: light distance cull, max intensity, max collected subsectors, wall/flat max lights, wall/flat candidate budget, range limit sliders; the four caches are cvars defaulting off (gl_light_distance_cull_cache, gl_light_spot_cache, gl_light_pos_relative_cache, gl_light_model_dedupe_cache)

**Files.** `src/playsim/a_dynlight.cpp`, `src/common/rendering/hwrenderer/data/hw_dynlightdata.h`, `src/rendering/hwrenderer/scene/hw_flats.cpp`, `src/rendering/hwrenderer/scene/hw_renderhacks.cpp`, `src/rendering/hwrenderer/scene/hw_spritelight.cpp`, `wadsrc/static/menudef.txt`

### Per-eye geometry budgets for flat vertices and portals
`[NOT IN UZD]` `[dormant]`

Two hard ceilings on scene complexity, counted per eye and reset at the start of each eye's draw: gl_max_vertices stops adding flat geometry once the eye's flat vertex count would exceed it, and gl_max_portals stops starting new portals once the eye has drawn that many.

**Why.** A single pathological room (a mirror hall, a huge open sector) can blow a frame budget that the rest of the map fits inside. These give a fixed worst case per eye rather than per scene.

**Reached by.** Options > OpenGL Preferences > Advanced: "Max rendered vertices" and "Max rendered portals" sliders; both default to unlimited

**Files.** `src/rendering/hwrenderer/scene/hw_flats.cpp`, `src/rendering/hwrenderer/scene/hw_portal.cpp`, `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/rendering/hwrenderer/hw_entrypoint.cpp`

### Background texture and model streaming threads
`[NOT IN UZD]` `[dormant]`

Adds a worker pool that decodes textures and reads model lumps off the render thread, uploading through dedicated Vulkan transfer queues where the device offers them (with an explicit queue-family ownership release when the upload family differs from the graphics family), and falling back to load-only workers otherwise. IHardwareTexture gains a NONE/CACHING/LOADING/READY state so the draw path can ask whether a texture has actually arrived; precaching...

**Why.** A texture decoded on the render thread is a dropped frame, and a dropped frame in a headset is felt rather than seen. This moves the cost off the critical path.

**Reached by.** Options > VR Performance Tweak / Developer Options: gl_texture_thread master toggle (default off) plus gl_texture_thread_models, gl_texture_thread_upload, gl_texture_thread_workers, vk_max_transfer_threads, gl_background_flush_count

**Files.** `src/common/rendering/vulkan/system/vk_renderdevice.cpp`, `src/common/rendering/hwrenderer/data/hw_cvars.cpp`, `src/common/textures/hw_ihwtexture.h`, `src/rendering/hwrenderer/hw_precache.cpp`, `src/rendering/hwrenderer/scene/hw_decal.cpp`

### Forked ZVulkan: upload queue slots, preferred physical device, multiview render passes
`[EXTENDS]` `[active]`

The bundled ZVulkan library is itself modified. VulkanDevice gains a vector of VulkanUploadSlot (queue, family, index, whether the family supports graphics) requested by count at creation; VulkanDeviceBuilder gains PreferredPhysicalDevice so the device OpenXR names can be selected rather than guessed; RenderPassBuilder gains Multiview(viewMask, correlationMask); and a VK_DEVICE_FLAG_FORCE_EXCLUSIVE_PRESENT device flag is added.

**Why.** The engine-side streaming pool and the multiview stereo path both need capabilities the stock library does not expose. Rather than working around them in engine code, the fork changed the library it vendors.

**Reached by.** internal, no user surface

**Files.** `libraries/ZVulkan/include/zvulkan/vulkandevice.h`, `libraries/ZVulkan/include/zvulkan/vulkanbuilders.h`, `libraries/ZVulkan/src/vulkandevice.cpp`, `libraries/ZVulkan/src/vulkanbuilders.cpp`

### Global depth fade
`[NOT IN UZD]` `[dormant]`

A distance haze applied in the fragment shader independent of any map's own fog: an on/off toggle, a density, a gradient exponent and a colour, uploaded as uGlobalFade* stream data by both the GL and Vulkan render states. A debug mode visualises the fade term, and five CCMDs nudge density and gradient live.

**Why.** Gives a per-player atmospheric depth cue that does not require editing any map's fog, and doubles as a way to hide the far plane cheaply when culling distances are pulled in.

**Reached by.** Options > OpenGL Preferences: "Global fade" toggle, density and gradient sliders, and a colour picker; default off

**Files.** `src/common/rendering/hwrenderer/data/hw_cvars.cpp`, `src/common/rendering/hwrenderer/data/hw_renderstate.h`, `src/common/rendering/gl/gl_renderstate.cpp`, `wadsrc/static/menudef.txt`

### Sector glow gains falloff curves, a far colour, and glow on the flat's own face
`[EXTENDS]` `[active]`

sector_t::splane grows a glow data model well past upstream's colour+height pair: GlowFalloff (linear / quadratic / sqrt / exponential), GlowIntensity, and GlowColorFar so a glow fades toward a second colour instead of staying one flat tint. It also gains an entirely separate FlatGlow family (colour, height, falloff, intensity, far colour) that makes the floor or ceiling glow inward from its own linedef edges, which upstream's wall-only glow never...

**Why.** The catalogued shader lanes needed somewhere to read from. Upstream glow only ever lights the wall above or below a plane; a room whose floor itself glows had no representation at all.

**Reached by.** ZScript / GLDEFS-side data; the shader lanes that consume it were catalogued separately

**Files.** `src/gamedata/r_defs.h`, `src/common/rendering/gl/gl_renderstate.cpp`, `src/common/rendering/hwrenderer/data/hw_renderstate.h`

### Sky drawn at infinity in stereo, and an optional flat sky cap
`[EXTENDS]` `[active]`

Two changes to the sky. RemoveMultiviewPositionParallax copies the left eye's view translation and camera position into the right eye's multiview viewpoint, so skyboxes and sky portals carry no interocular offset. Separately, gl_skydome off replaces the whole sky dome with the texture's cap colour as a scene clear colour, and skips the dome draw entirely.

**Why.** A skybox given per-eye parallax stereoscopically resolves as a small room a metre in front of the player instead of as a horizon. The flat cap is the cheap fallback for a scene that cannot afford the dome.

**Reached by.** Automatic in stereo for the parallax removal; Options > OpenGL Preferences: "Render sky dome" (gl_skydome, default on)

**Files.** `src/rendering/hwrenderer/scene/hw_drawinfo.cpp`, `src/rendering/hwrenderer/scene/hw_portal.cpp`, `src/rendering/hwrenderer/scene/hw_skyportal.cpp`, `src/rendering/hwrenderer/scene/hw_sky.cpp`

### Full-screen 2D pass for screen blends
`[EXTENDS]` `[active]`

The 2D drawer tags each command with mOutside2D and tracks whether any inside/outside commands exist, and Draw2D takes an outside2D pass argument. DoDim tags its quad outside2D whenever a VR mode is active, so damage flashes, pickup flashes, nightvision and every other screen dim cover the whole eye rather than the centred HUD rectangle.

**Why.** In VR the 2D layer is a small quad in the middle of the view. A full-screen tint drawn into it reads as a coloured card floating in front of the player instead of as the world going red.

**Reached by.** internal, no user surface

**Files.** `src/common/2d/v_2ddrawer.h`, `src/common/2d/v_2ddrawer.cpp`, `src/common/2d/v_draw.cpp`, `src/common/2d/v_draw.h`, `src/common/rendering/hwrenderer/hw_draw2d.cpp`

### Translucent canvas textures and a declared canvas pool
`[EXTENDS]` `[active]`

FCanvasTexture gains bTranslucentCanvas; when set, the renderer binds the canvas with TM_NORMAL instead of TM_OPAQUE so alpha survives onto whatever surface the canvas is applied to. FCanvas gets an OnDestroy that unhooks itself from its texture and AllCanvases. Sixteen canvas textures are pre-declared in animdefs (OPLTAG00-07 at 1024x256 and OPLVR00-07), so a world-space HUD or a floating player tag has a texture to draw into without any mod declaring...

**Why.** The mounted HUD, world-space player tags and any panel drawn on world geometry all need a canvas that is not forced opaque. Upstream assumes canvas textures are camera views, which always are.

**Reached by.** internal; the OPLTAG/OPLVR names are reachable as ordinary textures from ZScript

**Files.** `src/common/textures/textures.h`, `src/common/rendering/gl/gl_renderstate.cpp`, `src/common/2d/v_2ddrawer.cpp`, `wadsrc/static/animdefs.txt`

### Named haptic-effect intensity table
`[NOT IN UZD]` `[active]`

C_GetExternalHapticLevelValue(name) builds the cvar name "ext_haptic_level_<name>", looks it up, and multiplies it by ext_haptic_level_global_intensity, returning just the global level if no per-effect cvar exists. Call sites across the playsim pass an effect name: pickup_weapon, pickup, damage_projectile, poison, fire_weapon, doorclose, rumble, healstation, heartbeat.

**Why.** Haptics tuned as one global strength are always wrong for something. Naming each effect lets a player turn down door rumble without also losing weapon recoil, and the string-keyed lookup means adding an effect needs no engine plumbing beyond one CVAR line.

**Reached by.** Options > VR Preferences > Haptics: "External haptics intensity" slider (ext_haptic_level_global_intensity); the per-effect ext_haptic_level_* cvars are archived config values

**Files.** `src/common/console/c_cvars.cpp`, `src/common/console/c_cvars.h`, `src/playsim/p_actionfunctions.cpp`, `src/playsim/p_interaction.cpp`, `src/gamedata/a_weapons.cpp`, `wadsrc/static/menudef.txt`

### Earthquakes as controller rumble, camera shake off by default
`[REPLACES]` `[active]`

r_quakeintensity now defaults to 0, so an earthquake no longer moves the viewpoint at all. In its place, the per-axis quake intensity is converted into left and right controller vibration each frame (10ms pulses, scaled by vr_quake_haptic_level) plus rumble_front / rumble_back external haptic events, and Joy_Rumble("world/quake") on a gamepad. Item pickup gets the same treatment: a 50ms blip on both controllers scaled by vr_pickup_haptic_level, fired from...

**Why.** Shaking a headset-mounted camera is the single most reliable way to make a player sick. The information the shake carried is moved to a channel that cannot cause motion sickness.

**Reached by.** Options > VR Preferences > Haptics: "Quake haptic level" and "Item pickup haptic level" sliders; r_quakeintensity remains a cvar for anyone who wants the shake back

**Files.** `src/rendering/r_utility.cpp`, `src/rendering/2d/v_blend.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `wadsrc/static/menudef.txt`

### Sound that survives losing desktop focus, plus an off-hand sound channel
`[EXTENDS]` `[active]`

S_SetSoundPaused gains a VR gate: when a VR mode is running, losing the state flag no longer sets pauseext, so audio keeps playing even though i_pauseinbackground is on. A CHAN_OFFWEAPON channel (5) is added so the off-hand weapon's sounds do not cut the main hand's. snd_efx is flipped to default off.

**Why.** The headset is the display, but the desktop window is what Windows considers focused. Without this, putting the headset on and clicking anything else silences the game. EFX is turned off because its reverb cost is not affordable alongside a doubled render.

**Reached by.** Automatic when a VR mode is active; snd_efx remains in the sound options

**Files.** `src/common/audio/sound/s_sound.cpp`, `src/common/audio/sound/s_soundinternal.h`, `src/common/audio/sound/oalsound.cpp`

### A console usable without a keyboard, and quiet enough to leave open
`[EXTENDS]` `[active]`

C_ScrollConsole(amount) scrolls the console buffer by line, driven from g_game.cpp so a controller stick can page through it. C_HandleKey intercepts any key bound to "toggleconsole" so a controller button opens and closes it (and returns to the main menu from the full console). A new sysCallbacks.ConsoleToggled fires on both open and close. Unknown commands, cvar toggle messages and toggle results are demoted from Printf to DPrintf, and the console is...

**Why.** There is no keyboard and no scroll wheel. Reading the log is how the owner debugs, so it has to be reachable and legible from a controller; the noise demotions stop stock chatter from filling the buffer he is trying to read.

**Reached by.** A controller button bound to toggleconsole; stick scrolling handled in G_Responder; the character-input grid (catalogued separately) types into it

**Files.** `src/common/console/c_console.cpp`, `src/common/console/c_console.h`, `src/common/console/c_cvars.cpp`, `src/common/console/c_dispatch.cpp`, `src/g_game.cpp`, `src/d_main.cpp`

### Per-hand weapon selection command vocabulary
`[EXTENDS]` `[active]`

The weapon commands all take an optional hand argument: "slot <n> [hand]", "weapnext [hand]", "weapprev [hand]", "weapdrop [hand]", calling PlayerPawn.PickWeapon/PickNextWeapon/PickPrevWeapon with the hand and reading the corresponding ReadyWeapon or OffhandWeapon for the name tag and switch sound. A "switchhand <hand>" command hands the ready weapon to the other hand, routed through a DEM_ZSC_CMD net command so the swap completes inside a game tick...

**Why.** Two hands each hold a weapon, so every selection verb needs to say which hand it means. switchhand goes through the net command because doing it from the console handler nulled both weapon slots at an arbitrary point in the tick and the psprite tick then destroyed both layers.

**Reached by.** Bindable console commands, reached from the VR default bind sets and the wheel; no typing required

**Files.** `src/g_game.cpp`

### Purist mode and vanilla melee attack
`[NOT IN UZD]` `[active]`

Two toggles that turn fork behaviour back off. puristmode clears AActor::OverrideAttackPosDir every tic, so attacks originate from the eye along the view direction like flat Doom instead of from the controller. vanilla_melee_attack restores the auto-turn-to-target and auto-forward-lunge that stock melee weapons perform, which the fork otherwise suppresses whenever the player is in VR; it is honoured in single player only.

**Why.** Not everything survives being aimed with a hand. Some mods and some players want the original autoaim and the original melee lunge, and the fork chose to keep that reachable rather than assume.

**Reached by.** Options > Experimental menu: "Purist mode" and "Vanilla melee attack" toggles, each with explanatory text rows

**Files.** `src/g_game.cpp`, `src/playsim/p_user.cpp`, `src/common/rendering/hwrenderer/data/hw_vrmodes.cpp`, `wadsrc/static/zscript/actors/inventory/stateprovider.zs`, `wadsrc/static/menudef.txt`

### Canonical aim owner arbitration
`[NOT IN UZD]` `[active]`

Each tic, G_BuildTiccmd resolves which hand owns the playsim's canonical aim: the main hand wins if its attack/altattack/reload/dropmag buttons are down or its weapon is mid-sequence; otherwise the off hand wins if its buttons are down or, having previously held it, it is still mid-sequence. The resolved owner is a file-scope state that persists across tics.

**Why.** The playsim has exactly one player angle and pitch, but two hands are aiming. Something has to decide whose aim the engine's traces and autoaim follow when both hands are armed, and it has to be sticky enough that a firing sequence is not stolen mid-animation.

**Reached by.** internal, no user surface

**Files.** `src/g_game.cpp`, `src/playsim/p_map.cpp`

### ZScript frontend: version mismatch is a warning, and TextureID.GetIndex()
`[EXTENDS]` `[active]`

A mod declaring a ZScript version newer than the engine's no longer aborts with a fatal script error; it prints a red warning and parses on. Separately, the code generator recognises GetIndex() on a TextureID and compiles it to the identity on the already-int-typed self, so a texture handle can be passed to anything taking a plain int.

**Why.** The fork's version tracks upstream loosely, and refusing to load a mod written for a slightly newer ZScript would strand the mod family this engine exists for. GetIndex() exists because the billboard texture payload carries a TextureID inside an int field.

**Reached by.** ZScript source; automatic on load

**Files.** `src/common/scripting/frontend/zcc_parser.cpp`, `src/common/scripting/backend/codegen.cpp`

### Menus that write cvars from their own tick and rebuild themselves at runtime
`[EXTENDS]` `[active]`

DMenu::CallTicker now runs with InMenu incremented, so a page can write non-mod cvars from its Ticker and not only from an input event. M_RequestMenuRebuild() marks the whole menu tree for teardown and rebuild at the next M_StartControlPanel, and the developer cvar's callback calls it. The joystick options row is inserted into or deleted from the options menu according to whether any controller is actually present.

**Why.** A live-preview settings page that could only apply half its settings on a keypress was arriving in two halves at two different moments; the Ticker gate had no principle behind it. The rebuild and the joystick row exist because in a headset you cannot restart the game to see a menu change.

**Reached by.** Automatic; the developer cvar triggers the rebuild, controller presence drives the joystick row

**Files.** `src/common/menu/menu.cpp`, `src/common/menu/joystickmenu.cpp`, `src/common/console/c_console.cpp`

### Retuned upstream defaults, with a config migration that enforces them
`[EXTENDS]` `[active]`

A broad set of stock cvar defaults is changed for a headset rather than a monitor: snd_efx off, gl_plane_reflection off, r_portal_recursions 2, gl_texture_filter_anisotropic 4, gl_texture_filter 0, gl_precache on, r_mipmap off, r_actorspriteshadow off, r_quakeintensity 0, vid_contrast 1.1, vid_saturation 1.2, vid_defwidth/height 1400, vid_refreshrate 72, swrenderer's r_sprite_distance_cull 2000 and r_line_distance_cull 4000, and vid_preferbackend...

**Why.** Almost none of GZDoom's defaults were chosen for a stereo scene rendered twice at 72Hz. Migrating them on upgrade matters because an archived config from an older build would otherwise silently keep the old, unaffordable values.

**Reached by.** Defaults; each remains an ordinary cvar with its usual menu row

**Files.** `src/common/rendering/hwrenderer/data/hw_cvars.cpp`, `src/common/rendering/v_video.cpp`, `src/gameconfigfile.cpp`, `src/rendering/r_utility.cpp`, `src/rendering/swrenderer/scene/r_opaque_pass.cpp`

### Render-stage stat pages and a benchmark header
`[EXTENDS]` `[active]`

hw_clock grows about thirty new timers and counters and three stat pages. rendertimes prints a VR summary line (Scene / Post / Finalize / Submit / Composite / SyncWait) plus a per-stage VR breakdown down to subsector cull, line clip, line decide, thing and flat build, and reports wall-worker elapsed, CPU-sum and computed parallelism, batch and item counts. lightstats reports dynamic-light link/relink/unlink traffic, distance-cull counts for walls, flats...

**Why.** Every knob above needs a number attached to it, and the owner cannot read a profiler while wearing a headset. The header exists so a benchmark log identifies which build and which settings produced it without anyone having to remember.

**Reached by.** stat rendertimes / stat lightstats / stat bufferstats; the benchmark header goes to the log file, which is read afterwards

**Files.** `src/common/rendering/hwrenderer/data/hw_clock.cpp`, `src/common/rendering/hwrenderer/data/hw_clock.h`

### Billboards persist through save and load
`[EXTENDS]` `[active]`

FBillboard and FBillboardGroup get FSerializer overloads and are written into the level snapshot alongside NextBillboardID and NextBillboardGroupID. Every field travels, including roll, the SDF font slot, the payload data word, attachedTo (through the object table, so a dead owner loads as null and is reaped normally) and spawntic, so a transient billboard resumes its remaining lifetime rather than restarting it. MINSAVEVER is bumped to 4558 accordingly....

**Why.** Billboards are level state, not thinkers, so nothing would have saved them. A world panel that vanished on load would make the whole in-world UI unusable across a save.

**Reached by.** Automatic on save and load

**Files.** `src/p_saveg.cpp`, `src/g_levellocals.h`, `src/version.h`

### Normal flipping for HUD models
`[EXTENDS]` `[active]`

A TEXF_FlipNormal texture-mode flag is added and set for the duration of HUD model drawing; main.fp negates the surface normal (bumped or interpolated) when it is present.

**Why.** The HUD model is drawn in a frame whose handedness is reversed relative to the world, so its normals point the wrong way and every lit surface on the held weapon shades inside-out. One flag flips them back without touching the world path.

**Reached by.** internal, no user surface

**Files.** `src/common/textures/textures.h`, `src/rendering/hwrenderer/hw_models.cpp`, `wadsrc/static/shaders/glsl/main.fp`

### Doom's original random table restored
`[NOT IN UZD]` `[dormant]`

rndtable[256], prndindex, P_Random() and M_ClearRandom() are added back to m_random. A COMPATF2_OLD_RANDOM_GENERATOR bit and a compat_oldrandom flag cvar are defined, and the bit is included in the Doom (strict) and Boom compatibility presets. In the current tree the two P_Random() call sites are bot decision-making (b_func.cpp) and a commented-out pain-chance block; no live gameplay path is gated on the compat flag yet.

**Why.** The compat presets promise vanilla behaviour, and vanilla behaviour includes vanilla's deterministic RNG sequence. The table and the flag are in place; the gameplay wiring is not finished.

**Reached by.** compat_oldrandom cvar, set by the Doom (strict) / Boom compatibility presets

**Files.** `src/common/engine/m_random.cpp`, `src/common/engine/m_random.h`, `src/doomdef.h`, `src/d_main.cpp`

### SIGIL recognised as an IWAD
`[EXTENDS]` `[active]`

wadsrc_extra's iwadinfo gains a "Sigil of Baphomet" entry (sigil_v1_21.wad), requiring The Ultimate DOOM, autoloading sigil_shreds.wad, using the ultdoom mapinfo and the Shorttex compatibility profile, and placed in the IWAD selection order right after The Ultimate DOOM.

**Why.** SIGIL ships as a standalone IWAD-shaped file that stock UZDoom does not list, so it would not appear in the picker the fork's launch profiles select from.

**Reached by.** The IWAD selection box, and the command-line launch profile menu

**Files.** `wadsrc_extra/static/iwadinfo.txt`

---

Each capability was catalogued from source and then independently re-verified against the
tree, which corrected its lifecycle state and its reachability. 158 subsystem entries plus
27 from a completeness pass. Counts are from merge commit `8161a0e80a`.
