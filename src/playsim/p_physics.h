/*
** p_physics.h
** Rigid-body physics for VR object handling.
**
**---------------------------------------------------------------------------
** RS FORK -- see FORK_CHANGES.md.
**
** Doom's playsim ticks at 35Hz. A held object updated at 35Hz lags the hand by
** up to 28ms, which reads as underwater and is the single thing that decides
** whether VR object handling feels real. So this does NOT live in the playsim
** tick: it is stepped once per iteration of D_DoomLoop, which in VR free-runs
** at headset rate (TryRunTics deliberately disables its wait in VR, and
** frequently runs zero tics).
**
** Consequences of that, spelled out because each one has a wrong-looking
** symptom:
**
**  - AActor::Tick is NOT a reliable per-frame hook here. With zero tics run,
**    it may not fire for many consecutive rendered frames. Anything a physics
**    body needs every frame -- world linking, sector refresh -- this module
**    must do itself.
**
**  - Physics bodies must carry RF_DONTINTERPOLATE. The renderer draws actors
**    lerped between Prev and Pos() by ticFrac, with Prev reset each tic; a
**    position written at 90Hz against a stale tic-boundary Prev rubber-bands,
**    worst on the object held in front of your face.
**
** The simulation runs in METRES, isotropic, and converts at the boundary.
** Map space is not a metric space in this engine: the VR hand transform
** applies a mirror and a non-uniform pixelstretch scale BEFORE its rotations,
** so its basis is sheared whenever the wrist is tilted. Orientation is
** therefore built from the raw OpenXR pose angles, never decomposed from that
** matrix.
*/

#ifndef __P_PHYSICS_H__
#define __P_PHYSICS_H__

// Stepped once per D_DoomLoop iteration, from d_main.cpp, between TryRunTics()
// and D_Display().
//
// Deliberately NOT hooked into a render backend. vid_preferbackend defaults to
// BACKEND_OPENGL (v_video.cpp), and Vulkan init failure silently falls back to
// GL, so a Vulkan-only hook would never run on a default config and would
// vanish after a driver update. screen->BeginFrame() is wrong for the same
// family of reasons: the start screen calls it before any level exists,
// D_Display early-returns before reaching it, and screen wipes -- every level
// transition -- bypass it entirely.
void P_PhysicsFrame();

// Called when a level's geometry is final, and when it is torn down.
void P_PhysicsLevelStart();
void P_PhysicsLevelEnd();

// Drop an actor's body. MUST be called from AActor::OnDestroy: a body whose
// actor has been freed is a dangling pointer, and the GC cannot see a raw
// AActor* held in a physics registry.
class AActor;
void P_PhysicsRemoveBody(AActor *a);

#endif // __P_PHYSICS_H__
