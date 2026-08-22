/*
** p_physics.cpp
** Rigid-body physics for VR object handling -- frame driver.
**
**---------------------------------------------------------------------------
** RS FORK -- see p_physics.h for why this does not live in the playsim tick.
**
** SLICE 0: the driver and its diagnostics only. No bodies, no collision, no
** solver. The single question this answers is whether the step fires when it
** should and stops when it should -- in a level, in a menu, paused, mid-wipe,
** across a save, across a level change. Everything above this depends on that
** being true, and every way of getting it wrong looks like a physics bug
** rather than a timing bug.
*/

#include "p_physics.h"

#include "c_cvars.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "gamestate.h"
#include "hw_vrmodes.h"
#include "i_interface.h"
#include "i_time.h"
#include "menustate.h"
#include "printf.h"
#include "v_video.h"

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// Simulation rate. Deliberately independent of both the 35Hz playsim and the
// headset's refresh: a fixed step is what makes behaviour reproducible frame to
// frame, and 90Hz is fine enough that a thrown object does not tunnel at the
// speeds a human arm produces.
CUSTOM_CVAR(Int, vr_physics_hz, 90, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 30) self = 30;
	else if (self > 240) self = 240;
}

// Ceiling on how many steps one frame may run. A frame that arrives after a
// long stall must NOT try to catch up in full -- doing so costs more time than
// it recovers and spirals. Anything past this is dropped, and the drop is
// counted so it shows up in the log rather than as mysterious slow motion.
CUSTOM_CVAR(Int, vr_physics_maxsteps, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	else if (self > 16) self = 16;
}

// One line a second to the log. On by default and read from the log file --
// there is no console in a headset, so a `stat` overlay would be unreachable.
CVAR(Bool, vr_physics_debug, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

namespace
{

// Our OWN clock, deliberately not GetDeltaTime().
//
// GetDeltaTime is set at the TOP of D_Display, before its early returns, and
// overwrites its previous timestamp unconditionally -- so every frame that
// bails silently discards its elapsed time instead of accumulating it. Worse,
// ClearPrevTime() runs after every screen wipe, and GetDeltaTime() reads 0.0
// until the next frame. A physics accumulator fed from it loses wall time and
// never notices.
uint64_t g_lastTimeNs = 0;

double   g_accumulator = 0.0;   // seconds owed to the simulation
bool     g_running     = false; // was the sim live last frame

// Diagnostics, reset each reporting second.
uint64_t g_reportStartNs = 0;
int      g_frames        = 0;
int      g_steps         = 0;
int      g_dropped       = 0;
int      g_ticsAtReport  = 0;
double   g_dtMin         = 1e9;
double   g_dtMax         = 0.0;

// Last reported situation, so a transition prints immediately rather than
// waiting out the rest of the second it happened in.
bool     g_lastRunning   = false;
int      g_lastGamestate = -999;

const char *GamestateName(int gs)
{
	switch (gs)
	{
	case GS_LEVEL:       return "LEVEL";
	case GS_INTERMISSION:return "INTERMISSION";
	case GS_FINALE:      return "FINALE";
	case GS_DEMOSCREEN:  return "DEMOSCREEN";
	case GS_FULLCONSOLE: return "FULLCONSOLE";
	case GS_HIDECONSOLE: return "HIDECONSOLE";
	case GS_STARTUP:     return "STARTUP";
	case GS_TITLELEVEL:  return "TITLELEVEL";
	case GS_INTRO:       return "INTRO";
	case GS_CUTSCENE:    return "CUTSCENE";
	default:             return "?";
	}
}

// Whether the simulation should be advancing at all right now.
//
// Note this is NOT the same question as "was this function called". The frame
// hook fires every loop iteration unconditionally -- including on the title
// screen, during a wipe, and while the menu is up -- and that is deliberate,
// because a hook that stops being called is indistinguishable from a hook that
// was never wired up. What changes is whether time is consumed.
bool ShouldStep()
{
	if (gamestate != GS_LEVEL) return false;
	if (primaryLevel == nullptr) return false;
	if (paused) return false;
	if (pauseext) return false;

	// Menu and console are a freeze, not a slow-down. D_Display keeps running
	// while the playsim is frozen, so without this a held magazine would drift
	// out of a hand that is still being tracked.
	if (menuactive != MENU_Off) return false;

	return true;
}

void ReportLine(const char *why, double dt)
{
	if (!vr_physics_debug) return;

	const uint64_t nowNs = I_nsTime();
	const double elapsed = (nowNs - g_reportStartNs) / 1000000000.0;
	const double fps     = (elapsed > 0.0) ? g_frames / elapsed : 0.0;
	const double sps     = (elapsed > 0.0) ? g_steps  / elapsed : 0.0;

	auto vrmode = VRMode::GetVRMode();
	const bool isVR = (vrmode != nullptr) && vrmode->IsVR();

	Printf("[PHYS] %-9s frames/s=%6.1f steps/s=%6.1f dropped=%d  dt=%.4f (min %.4f max %.4f)  "
		"tics=%d  %s paused=%d menu=%d  vr=%d backend=%d  level=%s\n",
		why,
		fps, sps, g_dropped,
		dt, (g_dtMin > 1e8 ? 0.0 : g_dtMin), g_dtMax,
		gametic - g_ticsAtReport,
		GamestateName(gamestate),
		paused ? 1 : 0,
		menuactive != MENU_Off ? 1 : 0,
		isVR ? 1 : 0,
		*vid_preferbackend,
		primaryLevel ? "yes" : "no");

	g_reportStartNs = nowNs;
	g_frames        = 0;
	g_steps         = 0;
	g_dropped       = 0;
	g_ticsAtReport  = gametic;
	g_dtMin         = 1e9;
	g_dtMax         = 0.0;
}

} // namespace

// ---------------------------------------------------------------------------

void P_PhysicsFrame()
{
	const uint64_t nowNs = I_nsTime();

	// First call, or the clock was reset under us.
	if (g_lastTimeNs == 0 || nowNs < g_lastTimeNs)
	{
		g_lastTimeNs    = nowNs;
		g_reportStartNs = nowNs;
		g_ticsAtReport  = gametic;
		return;
	}

	double dt = (nowNs - g_lastTimeNs) / 1000000000.0;
	g_lastTimeNs = nowNs;

	// A stall -- loading, a wipe, alt-tabbed, a breakpoint -- is time the
	// simulation must NOT try to relive. Clamped rather than accumulated.
	const double kMaxFrame = 0.25;
	if (dt > kMaxFrame) dt = kMaxFrame;
	if (dt < 0.0) dt = 0.0;

	g_frames++;
	if (dt < g_dtMin) g_dtMin = dt;
	if (dt > g_dtMax) g_dtMax = dt;

	const bool run = ShouldStep();

	if (!run)
	{
		// Frozen. Drop whatever time was owed rather than banking it: coming
		// back from a menu must not fire a burst of catch-up steps, which is
		// exactly when a held object would be flung across the room.
		g_accumulator = 0.0;
	}
	else
	{
		if (!g_running)
		{
			// Resuming. Same reasoning -- start clean.
			g_accumulator = 0.0;
		}

		const double step = 1.0 / (double)*vr_physics_hz;
		g_accumulator += dt;

		int steps = 0;
		const int maxSteps = *vr_physics_maxsteps;
		while (g_accumulator >= step && steps < maxSteps)
		{
			// SLICE 0: nothing to advance yet. The step exists so its rate can
			// be measured before anything depends on it.
			g_accumulator -= step;
			steps++;
			g_steps++;
		}

		if (g_accumulator >= step)
		{
			// Over budget -- discard the remainder rather than spiral.
			g_dropped++;
			g_accumulator = 0.0;
		}
	}

	g_running = run;

	// Report on a state change immediately, otherwise once a second.
	const bool changed = (run != g_lastRunning) || ((int)gamestate != g_lastGamestate);
	if (changed)
	{
		ReportLine(run ? "START" : "STOP", dt);
		g_lastRunning   = run;
		g_lastGamestate = (int)gamestate;
	}
	else if (nowNs - g_reportStartNs >= 1000000000ull)
	{
		ReportLine(run ? "run" : "idle", dt);
	}
}

void P_PhysicsLevelStart()
{
	g_accumulator = 0.0;
	g_running     = false;
	if (vr_physics_debug) Printf("[PHYS] level start -- physics world would be built here\n");
}

void P_PhysicsLevelEnd()
{
	g_accumulator = 0.0;
	g_running     = false;
	if (vr_physics_debug) Printf("[PHYS] level end -- physics world would be torn down here\n");
}
