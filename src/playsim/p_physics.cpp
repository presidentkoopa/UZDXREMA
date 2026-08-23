/*
** p_physics.cpp
** Rigid-body physics for VR object handling.
**
**---------------------------------------------------------------------------
** RS FORK -- see p_physics.h for why this does not live in the playsim tick.
**
** SLICE 1: one box body per actor, gravity, and contact against the sector's
** own floor and ceiling planes. No walls, no hands, no grabbing, no library.
**
** The point of doing it in this order is that the questions which actually
** killed the previous seven attempts at physical reloading are all answered
** here, with nothing else in the way to blame:
**
**   - unit conversion between metres and map units
**   - whether an orientation survives the round trip out to the actor and back
**   - render interpolation fighting a transform written between tics
**   - keeping an actor correctly linked into the world from the render frame
**   - actor lifetime, since a body outliving its actor is a crash
**
** If a magazine tumbles and comes to rest convincingly on a floor, all five are
** correct and everything after this is additive.
**
** UNITS. The simulation runs in METRES on the playsim's own axes (Z up), and
** converts only at the actor boundary. Physics constants -- gravity, sleep
** thresholds, penetration slop -- are meaningless at any other scale: expressed
** in map units they are all wrong by the ~34x conversion factor, in the
** direction where a body never sleeps and jitters on the floor forever.
*/

#include "p_physics.h"

#include "actor.h"
#include "actorinlines.h"
#include "c_cvars.h"
#include "doomdata.h"
#include "doomstat.h"
#include "g_levellocals.h"
#include "gamestate.h"
#include "hw_vrmodes.h"
#include "i_interface.h"
#include "i_time.h"
#include "menustate.h"
#include "printf.h"
#include "r_defs.h"
#include "s_doomsound.h"
#include "s_soundinternal.h"
#include "v_video.h"
#include "vm.h"

#include <float.h>
#include <math.h>

EXTERN_CVAR(Float, vr_vunits_per_meter)

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------

// Simulation rate.
//
// Deliberately NOT the headset's refresh rate, and that is the entire reason it
// is a fixed step: 72, 80, 90 and 120 are all real headset rates, and if the
// simulation ran at whichever one the player happened to have, the same throw
// would behave differently on different hardware -- and differently again every
// time the framerate dipped. A fixed rate fed by an accumulator makes it
// reproducible regardless of what the display is doing.
//
// 90 is chosen as a middle value: comfortably above the 72 floor so the
// simulation is never coarser than the slowest headset, and cheap enough at
// this object count that raising it buys nothing measurable. It is NOT what
// decides whether a hard throw passes through a wall -- fast bodies are
// substepped automatically, see the frame loop.
CUSTOM_CVAR(Int, vr_physics_hz, 90, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 30) self = 30;
	else if (self > 240) self = 240;
}

CUSTOM_CVAR(Int, vr_physics_maxsteps, 4, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 1) self = 1;
	else if (self > 16) self = 16;
}

CVAR(Bool, vr_physics_debug, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Real gravity. Doom's own gravity is a different number in different units and
// is deliberately not reused: this simulation is metric, and a magazine should
// fall the way a magazine falls.
CVAR(Float, vr_physics_gravity, 9.81f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// How bouncy and how grippy a landing is. A magazine on concrete barely bounces
// and does not slide far.
// 0.15 was tuned for a magazine settling onto a floor, where a real one barely
// bounces at all. It is too dead for a hard throw into a wall, which should
// visibly kick back and turn over. Raised, with the low-speed cutoff below
// still ensuring a resting object does not buzz.
CVAR(Float, vr_physics_restitution, 0.32f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_physics_friction, 0.7f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Energy bleed. Real objects lose speed to air and to contacts that are never
// quite perfectly modelled; a little damping is what lets a body actually reach
// the sleep thresholds instead of shivering just above them forever.
CVAR(Float, vr_physics_lineardamp, 0.25f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CVAR(Float, vr_physics_angulardamp, 0.6f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Resistance to twisting on the spot while touching a surface -- see the
// torsional friction note in the solver. Only applies during contact, so a
// thrown object still tumbles freely through the air.
CVAR(Float, vr_physics_contactspindamp, 4.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// Whether your hands are solid to physics objects.
CVAR(Bool, vr_physics_hands, true, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)

// How much a rotating wrist contributes to the direction of a throw.
//
// 1.0 is physically honest: an object held away from the hand's centre really
// does get flung along a tangent when the wrist turns. But hand tracking
// reports angular velocity noisily, and a throw naturally involves a wrist
// flick, so at full strength the sideways term can visibly steer a throw away
// from where it was aimed. Lower values keep throws going where they are
// pointed at some cost in realism.
CUSTOM_CVAR(Float, vr_physics_throwspin, 0.5f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.f) self = 0.f;
	else if (self > 2.f) self = 2.f;
}

// How big the invisible collision shape on each hand is, as a multiplier. The
// base is roughly a real palm; larger makes objects easier to bat around and
// harder to reach past.
CUSTOM_CVAR(Float, vr_physics_handsize, 1.0f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0.25f) self = 0.25f;
	else if (self > 4.0f) self = 4.0f;
}

// PLACEHOLDER weapon shape, one box for the whole gun.
//
// Genuinely temporary: real weapons will get a collision primitive PER BONE
// (grip, slide, magwell) built from the mesh's own bind pose, which is what
// eventually turns the magwell into an actual hole a magazine enters. Until
// that model work lands, this single box is what makes a held weapon solid at
// all -- clank two pistols together, feel one against your other palm, bump a
// magazine off it -- while the T77 mesh stands in for whichever real model ends
// up in the grip.
//
// Local +X is forward (out the muzzle), +Y is width, +Z is height -- the same
// axis convention the rest of this engine builds a facing vector from out of
// yaw/pitch (fwd = cos(pitch)*cos(yaw), cos(pitch)*sin(yaw), -sin(pitch)).
CUSTOM_CVAR(Float, vr_physics_weaponlen, 0.11f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{ if (self < 0.02f) self = 0.02f; else if (self > 0.6f) self = 0.6f; }
CUSTOM_CVAR(Float, vr_physics_weaponwidth, 0.02f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{ if (self < 0.005f) self = 0.005f; else if (self > 0.2f) self = 0.2f; }
CUSTOM_CVAR(Float, vr_physics_weaponheight, 0.07f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{ if (self < 0.02f) self = 0.02f; else if (self > 0.4f) self = 0.4f; }

// Where the box sits relative to the tracked grip point -- forward and up from
// it, since the grip is normally near the back-bottom of a handgun. Guessed,
// not measured; tune in the menu, or replace outright once real per-bone
// shapes exist.
CUSTOM_CVAR(Float, vr_physics_weapon_ofs_fwd, 0.08f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{ if (self < -0.3f) self = -0.3f; else if (self > 0.3f) self = 0.3f; }
CUSTOM_CVAR(Float, vr_physics_weapon_ofs_up, 0.02f, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{ if (self < -0.2f) self = -0.2f; else if (self > 0.2f) self = 0.2f; }

// Per-body trace to the log while awake, N times a second. 0 = off. This is the
// only way to see what a body is doing without a console in the headset.
CUSTOM_CVAR(Int, vr_physics_trace, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < 0) self = 0;
	else if (self > 20) self = 20;
}

// ---------------------------------------------------------------------------

namespace
{

constexpr int   kSolverIterations = 8;
// How far a body may drift and still count as settled. A resting box bobs by
// roughly a millimetre as gravity and the contact take turns; anything actually
// creeping covers far more than this in the sleep window.
constexpr float kSleepDrift      = 0.004f;  // metres
constexpr float kSleepDriftAngle = 0.05f;   // radians, ~3 degrees

// Diagnostic thresholds only -- see the sleep block; velocity does not decide.
constexpr float kSleepLinear      = 0.05f;   // m/s
constexpr float kSleepAngular     = 0.4f;    // rad/s
constexpr float kSleepTime        = 0.3f;    // seconds below both thresholds
constexpr float kPenetrationSlop  = 0.0015f; // m -- allowed overlap before pushing out

// Fraction of the penetration error corrected per step, as a velocity bias.
// Low on purpose: high values overshoot and make a resting body hop.
constexpr float kBaumgarte = 0.15f;

// Below this approach speed a contact is treated as inelastic. A resting box
// generates a contact every single step; letting each one bounce, however
// slightly, is what makes it buzz and never settle.
constexpr float kRestitutionThreshold = 0.5f;   // m/s

// How long a body keeps counting as supported after its last contact. Must be
// comfortably longer than the gap between contacts on a resting body, which the
// penetration bias creates by pushing it slightly clear.
constexpr float kSupportGrace = 0.2f;   // seconds

// How hard a sleeping body must be struck before it wakes. Above zero on
// purpose: two objects merely leaning on each other would otherwise keep each
// other awake indefinitely.
constexpr float kWakeOnImpact = 0.15f;  // m/s

// --- a minimal quaternion -------------------------------------------------
//
// Deliberately local rather than TQuaternion: this is four floats and three
// operations, and owning them removes any question about which convention the
// engine's type uses. The engine type is still used at the boundary.
struct Quat
{
	float x, y, z, w;

	static Quat Identity() { return { 0.f, 0.f, 0.f, 1.f }; }

	void Normalize()
	{
		float n = sqrtf(x*x + y*y + z*z + w*w);
		if (n > 1e-8f) { float i = 1.f / n; x *= i; y *= i; z *= i; w *= i; }
		else { *this = Identity(); }
	}

	// Rotate a vector by this quaternion.
	FVector3 Rotate(const FVector3 &v) const
	{
		// v + 2w(q x v) + 2(q x (q x v))
		FVector3 q(x, y, z);
		FVector3 t = FVector3(
			q.Y * v.Z - q.Z * v.Y,
			q.Z * v.X - q.X * v.Z,
			q.X * v.Y - q.Y * v.X) * 2.f;
		return v + t * w + FVector3(
			q.Y * t.Z - q.Z * t.Y,
			q.Z * t.X - q.X * t.Z,
			q.X * t.Y - q.Y * t.X);
	}

	Quat Inverse() const { return { -x, -y, -z, w }; }

	static Quat FromEulerDeg(double yawDeg, double pitchDeg, double rollDeg)
	{
		const double d2r = M_PI / 180.0;
		double cy = cos(yawDeg * d2r * 0.5),   sy = sin(yawDeg * d2r * 0.5);
		double cp = cos(pitchDeg * d2r * 0.5), sp = sin(pitchDeg * d2r * 0.5);
		double cr = cos(rollDeg * d2r * 0.5),  sr = sin(rollDeg * d2r * 0.5);
		Quat q;
		q.w = (float)(cr * cp * cy + sr * sp * sy);
		q.x = (float)(sr * cp * cy - cr * sp * sy);
		q.y = (float)(cr * sp * cy + sr * cp * sy);
		q.z = (float)(cr * cp * sy - sr * sp * cy);
		q.Normalize();
		return q;
	}

	void ToEulerDeg(double &yawDeg, double &pitchDeg, double &rollDeg) const
	{
		const double r2d = 180.0 / M_PI;
		double sinp = 2.0 * ((double)w * y - (double)z * x);
		if (sinp > 1.0) sinp = 1.0; else if (sinp < -1.0) sinp = -1.0;
		pitchDeg = asin(sinp) * r2d;
		yawDeg   = atan2(2.0 * ((double)w * z + (double)x * y),
		                 1.0 - 2.0 * ((double)y * y + (double)z * z)) * r2d;
		rollDeg  = atan2(2.0 * ((double)w * x + (double)y * z),
		                 1.0 - 2.0 * ((double)x * x + (double)y * y)) * r2d;
	}
};

// --- shape ----------------------------------------------------------------
//
// THE SHAPE IS THE MODEL. A body is a set of CONVEX HULLS, not a box.
//
// One hull is enough for a magazine, a casing or a slide -- those really are
// convex. The set exists for the things that are not, and specifically for the
// one that matters most here: a MAGWELL IS A CAVITY, and no convex shape has a
// hole in it. A grip built from four convex slabs does, and a magazine can be
// pushed up into the gap between them and be stopped by the walls if it is
// crooked. That is the whole difference between geometry-accurate insertion
// and two boxes overlapping.
//
// Both representations the solver needs are stored, because it asks two
// different questions and each wants a different one:
//   verts  -- "where does this shape touch the world", asked of the floor,
//             the ceiling and every wall plane. Vertex against plane.
//   planes -- "is that point inside me, and how deep", asked by the pair
//             solver. Point against half-spaces.
// Deriving either from the other every step would cost more than storing both.
struct PhysHull
{
	// Body space, metres, relative to the CENTRE OF MASS -- the same space the
	// solver integrates in, so no per-step conversion is needed.
	TArray<FVector3> verts;

	// Outward face planes. xyz is a unit normal, w is the offset, so a point p
	// is inside this face when (n | p) - w <= 0.
	TArray<FVector4> planes;

	// Distance from the body origin to the furthest vertex. Broadphase only.
	float radius = 0.f;

	// Half the smallest face-to-face thickness. This is what decides how far a
	// body may move in one substep before it can tunnel: a thin object needs
	// finer steps than a fat one of the same overall size, and using the
	// bounding radius instead would let a casing pass clean through a floor.
	float thinHalf = 0.f;
};

// A box, as a hull. Every body starts as one of these, so a body that has never
// been given real geometry behaves exactly as it did before hulls existed --
// this generalisation cannot regress anything on its own.
inline void HullMakeBox(PhysHull &h, const FVector3 &half)
{
	h.verts.Clear();
	h.planes.Clear();
	for (int c = 0; c < 8; c++)
	{
		h.verts.Push(FVector3(
			(c & 1) ? half.X : -half.X,
			(c & 2) ? half.Y : -half.Y,
			(c & 4) ? half.Z : -half.Z));
	}
	h.planes.Push(FVector4( 1,  0,  0, half.X));
	h.planes.Push(FVector4(-1,  0,  0, half.X));
	h.planes.Push(FVector4( 0,  1,  0, half.Y));
	h.planes.Push(FVector4( 0, -1,  0, half.Y));
	h.planes.Push(FVector4( 0,  0,  1, half.Z));
	h.planes.Push(FVector4( 0,  0, -1, half.Z));

	h.radius = half.Length();
	float t = half.X;
	if (half.Y < t) t = half.Y;
	if (half.Z < t) t = half.Z;
	h.thinHalf = t;
}

// Recompute the cached scalars after verts/planes have been filled in from
// real geometry.
inline void HullFinish(PhysHull &h)
{
	h.radius = 0.f;
	for (unsigned i = 0; i < h.verts.Size(); i++)
	{
		const float r = h.verts[i].Length();
		if (r > h.radius) h.radius = r;
	}

	// Smallest distance from the hull's own origin to any face. For a convex
	// hull containing its origin that is exactly the half-thickness across the
	// narrowest direction.
	h.thinHalf = h.radius;
	for (unsigned i = 0; i < h.planes.Size(); i++)
	{
		const float d = h.planes[i].W;
		if (d > 0.f && d < h.thinHalf) h.thinHalf = d;
	}
	if (h.thinHalf < 1e-4f) h.thinHalf = 1e-4f;
}

// The face of this hull that a local-space point is deepest inside, or false if
// the point is outside it.
//
// The exact generalisation of the box version this replaced: a box asked which
// of six axis-aligned slabs the point was least far inside, and the answer was
// the contact normal. A hull asks the same question of N arbitrary half-spaces.
// Smallest penetration is the cheapest way out, and that is the direction the
// contact should push.
inline bool HullDeepest(const FVector3 &local, const PhysHull &h,
                        FVector3 &axisOut, float &depthOut)
{
	if (h.planes.Size() == 0) return false;

	float best = FLT_MAX;
	int   bestIdx = -1;

	for (unsigned i = 0; i < h.planes.Size(); i++)
	{
		const FVector4 &p = h.planes[i];
		const float dist = (local.X * p.X + local.Y * p.Y + local.Z * p.Z) - p.W;
		if (dist > 0.f) return false;          // outside this face, so outside
		const float depth = -dist;
		if (depth < best) { best = depth; bestIdx = (int)i; }
	}

	const FVector4 &p = h.planes[bestIdx];
	axisOut = FVector3(p.X, p.Y, p.Z);
	depthOut = best;
	return true;
}

// --- a body ---------------------------------------------------------------

struct PhysBody
{
	// Null for the hands, which are bodies without actors: nothing in the
	// playsim owns them, they are never drawn by this code, and they exist only
	// to shove other bodies around.
	AActor  *owner = nullptr;

	// -1 for an ordinary body; 0 = main hand, 1 = off hand.
	int      handIndex = -1;

	// -1 for anything else; 0 = attached to the main hand, 1 = the off hand.
	// Separate from handIndex: this tags the WEAPON riding a hand, not the hand
	// itself.
	int      weaponHand = -1;

	FVector3 pos    = FVector3(0, 0, 0);   // metres, playsim axes, Z up
	FVector3 vel    = FVector3(0, 0, 0);   // m/s
	Quat     rot    = Quat::Identity();
	FVector3 angVel = FVector3(0, 0, 0);   // rad/s

	FVector3 half   = FVector3(0.05f, 0.05f, 0.05f);  // box half-extents, metres

	// THE ACTUAL COLLISION SHAPE. One entry is a convex body; several make a
	// concave one, which is what a magwell needs. Always at least one -- a body
	// given only half-extents gets a single box hull, so this is a
	// generalisation of the old behaviour rather than a replacement for it.
	TArray<PhysHull> hulls;

	// Cached over every hull: furthest vertex from the body origin (broadphase)
	// and the narrowest half-thickness in the set (substep sizing).
	float boundRadius = 0.f;
	float thinHalf    = 0.f;

	// Rebuild the cached scalars. Call after touching hulls.
	void ShapeFinish()
	{
		if (hulls.Size() == 0)
		{
			hulls.Reserve(1);
			HullMakeBox(hulls[0], half);
		}
		boundRadius = 0.f;
		thinHalf = FLT_MAX;
		for (unsigned i = 0; i < hulls.Size(); i++)
		{
			// A hull sitting away from the body origin reaches further than its
			// own radius -- the offset has to be counted, or the broadphase
			// misses contacts on a compound shape.
			float far_ = 0.f;
			for (unsigned v = 0; v < hulls[i].verts.Size(); v++)
			{
				const float r = hulls[i].verts[v].Length();
				if (r > far_) far_ = r;
			}
			if (far_ > boundRadius) boundRadius = far_;
			if (hulls[i].thinHalf < thinHalf) thinHalf = hulls[i].thinHalf;
		}
		if (boundRadius < 1e-4f) boundRadius = 1e-4f;
		if (thinHalf > boundRadius || thinHalf <= 0.f) thinHalf = boundRadius;
	}

	// Where the centre of mass sits relative to the ACTOR's origin, in body
	// space, in metres.
	//
	// Almost never zero for a real model. A magazine exported with its origin at
	// the base has its mass centred 5cm above that, and a box centred on the
	// origin instead would cover only the bottom half of it -- which is exactly
	// what a hand passing through the top half of a magazine looks like.
	//
	// The solver works entirely in centre-of-mass space, because that is the
	// only point a rigid body actually rotates about. This offset exists solely
	// to convert back and forth at the actor boundary.
	FVector3 comOffset = FVector3(0, 0, 0);
	float    invMass = 1.f;
	FVector3 invInertia = FVector3(1, 1, 1);          // body-space diagonal

	// What it sounds like when it hits something, and how hard it has to hit
	// before it is worth hearing. Sound is not decoration here: a magazine that
	// lands silently gives you no idea where it went, and the whole point of it
	// being a real object is that you can find it again.
	FSoundID impactSound = NO_SOUND;
	float    impactMinSpeed = 0.6f;   // m/s
	float    impactCooldown = 0.f;    // stops a bouncing box machine-gunning

	// Held: the solver does not integrate this body, something outside drives
	// its transform instead. It keeps its shape and mass, so releasing it is
	// just a matter of handing it a velocity and letting go.
	bool kinematic = false;

	// Which hand is holding it, and where it sits in that hand -- the offset
	// captured at the moment it was grabbed, so an object keeps the pose it had
	// rather than snapping to the hand's own.
	//
	// Driven from the ENGINE at physics rate rather than from script at tic
	// rate. That is not a detail: the playsim runs at 35Hz, so a held object
	// positioned from script updates a third as often as everything around it,
	// and lags the hand by up to 28ms. Latency on the object in front of your
	// face is the one thing this whole design exists to get right.
	int   heldByHand = -1;
	FVector3 grabPosOffset = FVector3(0, 0, 0);
	Quat     grabRotOffset = Quat::Identity();

	float sleepTimer = 0.f;
	float traceTimer = 0.f;

	// How long this body still counts as "resting on something", even on steps
	// where it happens to register no contact. See the sleep block: without
	// this, a body can never fall asleep at all.
	float supportTimer = 0.f;

	// Smoothed velocity. Diagnostic only -- it is NOT what decides sleep; see
	// the sleep block for why velocity is the wrong question entirely.
	FVector3 velEMA = FVector3(0, 0, 0);
	FVector3 angEMA = FVector3(0, 0, 0);

	// Where this body was when it last looked like it might be settling. Sleep
	// is decided by how far it has actually MOVED from here.
	FVector3 sleepRefPos = FVector3(0, 0, 0);
	Quat     sleepRefRot = Quat::Identity();


	bool  asleep = false;

	// Logged once when it first comes to rest, so "did it settle and where"
	// is answerable from the log without asking anyone to watch it.
	bool  restReported = false;
};

TArray<PhysBody> g_bodies;

// Previous hand poses, for deriving how fast they are moving. Kept outside the
// bodies because the bodies are created and destroyed as the hands come and go.
FVector3 g_handPrevPos[2];
Quat     g_handPrevRot[2];
bool     g_handHavePrev[2] = { false, false };

// A short history of how each hand has been moving.
//
// A throw does NOT leave at the speed the hand had when the button came up. A
// human releases at the END of the motion, by which point the arm is already
// decelerating -- so sampling at the instant of release captures the slowdown
// rather than the throw, and the object dribbles out and drops instead of
// arcing. Keeping a fraction of a second and taking the FASTEST moment in it is
// what every VR game that throws convincingly does.
//
// 16 samples at 90Hz is about 180ms, which comfortably spans the accelerating
// part of a throw without reaching back into whatever the hand was doing before
// it started.
constexpr int kHandHistory = 16;
struct HandSample { FVector3 vel, angVel; };
HandSample g_handHist[2][kHandHistory];
int  g_handHistPos[2] = { 0, 0 };
bool g_handHistFilled[2] = { false, false };

// --- clock / accumulator (slice 0) ---------------------------------------

uint64_t g_lastTimeNs = 0;
double   g_accumulator = 0.0;
bool     g_running = false;

uint64_t g_reportStartNs = 0;
int      g_frames = 0, g_steps = 0, g_dropped = 0, g_ticsAtReport = 0;
double   g_dtMin = 1e9, g_dtMax = 0.0;
bool     g_lastRunning = false;
int      g_lastGamestate = -999;

// --- unit conversion ------------------------------------------------------

inline float MapPerMetre()
{
	float s = (float)*vr_vunits_per_meter;
	return (s > 0.01f) ? s : 34.f;
}
inline float MapToM(double mapUnits) { return (float)(mapUnits / MapPerMetre()); }
inline double MToMap(float metres)   { return (double)metres * MapPerMetre(); }

const char *GamestateName(int gs)
{
	switch (gs)
	{
	case GS_LEVEL:        return "LEVEL";
	case GS_INTERMISSION: return "INTERMISSION";
	case GS_FINALE:       return "FINALE";
	case GS_DEMOSCREEN:   return "DEMOSCREEN";
	case GS_FULLCONSOLE:  return "FULLCONSOLE";
	case GS_HIDECONSOLE:  return "HIDECONSOLE";
	case GS_STARTUP:      return "STARTUP";
	case GS_TITLELEVEL:   return "TITLELEVEL";
	case GS_INTRO:        return "INTRO";
	case GS_CUTSCENE:     return "CUTSCENE";
	default:              return "?";
	}
}

bool ShouldStep()
{
	if (gamestate != GS_LEVEL) return false;
	if (primaryLevel == nullptr) return false;
	if (paused || pauseext) return false;
	if (menuactive != MENU_Off) return false;
	return true;
}

PhysBody *FindBody(AActor *a)
{
	for (unsigned i = 0; i < g_bodies.Size(); i++)
		if (g_bodies[i].owner == a) return &g_bodies[i];
	return nullptr;
}

// Apply the inverse world-space inertia tensor to a vector.
// The tensor is diagonal in BODY space, so rotate in, scale, rotate back.
FVector3 ApplyInvInertia(const PhysBody &b, const FVector3 &v)
{
	FVector3 local = b.rot.Inverse().Rotate(v);
	local.X *= b.invInertia.X;
	local.Y *= b.invInertia.Y;
	local.Z *= b.invInertia.Z;
	return b.rot.Rotate(local);
}

// A contact point, collected ONCE per step and then solved iteratively.
//
// Collecting up front matters. The first version recomputed corner positions
// inside the solver loop while also teleporting the body out of the floor on
// every iteration, so each corner was resolved against a surface that had
// already moved for the previous corner. Uneven penetration across the eight
// corners then turned that vertical teleport into a lateral push, which is
// exactly the "lands, wobbles, and wanders off slowly" behaviour it produced.
struct Contact
{
	FVector3 point;          // world, metres
	FVector3 normal;
	float    penetration;
	float    initialVn;      // approach speed when first detected -- restitution uses this
	float    normalImpulse;  // accumulated over iterations
};

FVector3 Cross(const FVector3 &a, const FVector3 &b)
{
	return FVector3(a.Y*b.Z - a.Z*b.Y, a.Z*b.X - a.X*b.Z, a.X*b.Y - a.Y*b.X);
}

float EffectiveMass(const PhysBody &b, const FVector3 &r, const FVector3 &dir)
{
	FVector3 t = ApplyInvInertia(b, Cross(r, dir));
	return b.invMass + (dir | Cross(t, r));
}

void ApplyImpulse(PhysBody &b, const FVector3 &r, const FVector3 &impulse)
{
	b.vel += impulse * b.invMass;
	b.angVel += ApplyInvInertia(b, Cross(r, impulse));
}

void StepBody(PhysBody &b, float dt)
{
	AActor *a = b.owner;
	if (a == nullptr) return;
	if (b.asleep) return;

	// Held. Its transform comes from whatever is holding it; the solver leaves
	// it alone until it is let go.
	if (b.kinematic) return;

	sector_t *sec = a->Sector;
	if (sec == nullptr) return;

	// --- integrate --------------------------------------------------------

	b.vel.Z -= *vr_physics_gravity * dt;

	// Damping. Real objects bleed energy to air and to imperfect contacts, and
	// without a little of it a rigid body solver will happily keep a box
	// shivering forever below the sleep threshold.
	const float linDamp = 1.f - *vr_physics_lineardamp * dt;
	const float angDamp = 1.f - *vr_physics_angulardamp * dt;
	b.vel *= (linDamp > 0.f) ? linDamp : 0.f;
	b.angVel *= (angDamp > 0.f) ? angDamp : 0.f;

	b.pos += b.vel * dt;

	// Integrate orientation: q += 0.5 * omega * q * dt
	{
		Quat w{ b.angVel.X, b.angVel.Y, b.angVel.Z, 0.f };
		Quat &q = b.rot;
		Quat d;
		d.w = 0.5f * (w.w*q.w - w.x*q.x - w.y*q.y - w.z*q.z);
		d.x = 0.5f * (w.w*q.x + w.x*q.w + w.y*q.z - w.z*q.y);
		d.y = 0.5f * (w.w*q.y - w.x*q.z + w.y*q.w + w.z*q.x);
		d.z = 0.5f * (w.w*q.z + w.x*q.y - w.y*q.x + w.z*q.w);
		q.x += d.x * dt; q.y += d.y * dt; q.z += d.z * dt; q.w += d.w * dt;
		q.Normalize();
	}

	// --- collect contacts -------------------------------------------------
	//
	// Floor and ceiling come from the sector's own planes, evaluated LIVE at
	// the body's position rather than from a baked mesh. Two consequences, both
	// wanted: slopes work with no extra code, and a body resting on a lift
	// rides it -- when the plane rises, the next step simply finds the floor
	// higher.
	// Raised from 32 with hulls. A box could only ever present eight points to
	// the world; a real magazine hull presents a few dozen, and a body landing
	// flat on its side can legitimately have a dozen of them touching at once.
	// Running out mid-face makes an object rock on the corners it happened to
	// report first.
	Contact contacts[96];
	int numContacts = 0;
	const int kMaxContacts = 96;

	// Every hull vertex, in world space. Computed once because the wall pass
	// below tests the same points against the linedef planes.
	//
	// THIS IS WHERE THE SHAPE STOPS BEING A BOX. The eight corners this
	// replaced were the whole of the old collision geometry -- floors, walls
	// and everything else saw a rectangular block no matter what the model
	// looked like. Now the points offered to the world are the model's own.
	static TArray<FVector3> hullWorld;
	hullWorld.Clear();
	for (unsigned hi = 0; hi < b.hulls.Size(); hi++)
	{
		const PhysHull &h = b.hulls[hi];
		for (unsigned vi = 0; vi < h.verts.Size(); vi++)
			hullWorld.Push(b.pos + b.rot.Rotate(h.verts[vi]));
	}

	for (unsigned ci = 0; ci < hullWorld.Size() && numContacts < kMaxContacts; ci++)
	{
		const FVector3 world = hullWorld[ci];

		const double mapX = MToMap(world.X);
		const double mapY = MToMap(world.Y);

		const float floorZ = MapToM(sec->floorplane.ZatPoint(mapX, mapY));
		const float ceilZ  = MapToM(sec->ceilingplane.ZatPoint(mapX, mapY));

		FVector3 n(0, 0, 0);
		float pen = 0.f;

		if (world.Z < floorZ)
		{
			const DVector3 &nn = sec->floorplane.Normal();
			n = FVector3((float)nn.X, (float)nn.Y, (float)nn.Z);
			if (n.Length() < 1e-6f) n = FVector3(0, 0, 1);
			n.MakeUnit();
			pen = floorZ - world.Z;
		}
		else if (world.Z > ceilZ)
		{
			n = FVector3(0, 0, -1);
			pen = world.Z - ceilZ;
		}
		else continue;

		FVector3 r = world - b.pos;
		FVector3 vRel = b.vel + Cross(b.angVel, r);

		Contact &c = contacts[numContacts++];
		c.point = world;
		c.normal = n;
		c.penetration = pen;
		c.initialVn = vRel | n;
		c.normalImpulse = 0.f;
	}

	// --- walls ------------------------------------------------------------
	//
	// From the LINEDEFS, not from a rendering mesh. That is a deliberate choice
	// and it matters: DoomLevelMesh encodes what is DRAWN, so its wall quads are
	// gated on a texture existing and two-sided lines contribute nothing to it
	// at all. Collide against that and objects fall through untextured steps and
	// straight past every ML_BLOCKING railing in the game. The linedef IS Doom's
	// definition of solid, so it is the honest source -- and it moves with
	// polyobjects and needs no rebuild when a door opens.
	//
	// Only the body's own sector's lines are considered. A box a few centimetres
	// across cannot reach past them, and it avoids a blockmap query per body per
	// step.
	{
		const double reachMap = MToMap(b.boundRadius) + 2.0;

		for (unsigned li = 0; li < sec->Lines.Size() && numContacts < kMaxContacts; li++)
		{
			line_t *ld = sec->Lines[li];
			if (ld == nullptr) continue;

			const DVector2 v1 = ld->v1->fPos();
			const DVector2 d  = ld->Delta();
			const double lenSq = d.LengthSquared();
			if (lenSq < 1e-9) continue;

			// 2D outward normal of the line.
			DVector2 n2(d.Y, -d.X);
			n2 /= sqrt(lenSq);

			// Which side of the line the body's centre is on decides which way
			// it gets pushed. A two-sided blocking line is solid from both
			// sides, so the side is read per body rather than baked in.
			const DVector2 cMap(MToMap(b.pos.X), MToMap(b.pos.Y));
			const double dc = (cMap - v1) | n2;
			if (fabs(dc) > reachMap) continue;
			const double side = (dc >= 0) ? 1.0 : -1.0;

			const bool oneSided = (ld->backsector == nullptr);
			const bool blockAll = oneSided || (ld->flags & ML_BLOCKING);

			for (unsigned ci = 0; ci < hullWorld.Size() && numContacts < kMaxContacts; ci++)
			{
				const FVector3 &w = hullWorld[ci];
				const DVector2 pMap(MToMap(w.X), MToMap(w.Y));

				// Past the wall plane, on the body's own side?
				const double dCorner = (pMap - v1) | n2;
				const double depthMap = -(dCorner * side);
				if (depthMap <= 0.0) continue;

				// Within the segment, not off its end.
				const double t = ((pMap - v1) | d) / lenSq;
				if (t < 0.0 || t > 1.0) continue;

				// How high the line is solid. A two-sided line that is not
				// explicitly blocking is still solid below the higher floor and
				// above the lower ceiling -- that is what makes a step a step
				// and a window a window.
				if (!blockAll)
				{
					const double zc = MToMap(w.Z);
					const double fA = ld->frontsector->floorplane.ZatPoint(pMap);
					const double fB = ld->backsector->floorplane.ZatPoint(pMap);
					const double cA = ld->frontsector->ceilingplane.ZatPoint(pMap);
					const double cB = ld->backsector->ceilingplane.ZatPoint(pMap);
					const double solidBelow = (fA > fB) ? fA : fB;
					const double solidAbove = (cA < cB) ? cA : cB;
					if (zc >= solidBelow && zc <= solidAbove) continue;   // clear gap
				}

				FVector3 n((float)(n2.X * side), (float)(n2.Y * side), 0.f);

				FVector3 r = w - b.pos;
				FVector3 vRel = b.vel + Cross(b.angVel, r);

				Contact &c = contacts[numContacts++];
				c.point = w;
				c.normal = n;
				c.penetration = MapToM(depthMap);
				c.initialVn = vRel | n;
				c.normalImpulse = 0.f;
			}
		}
	}

	const bool anyContact = (numContacts > 0);

	// --- impact sound -----------------------------------------------------
	//
	// Driven by the hardest approach speed among this step's contacts, so a
	// glancing scrape is quiet and a solid drop is not. Rate-limited, because a
	// box settling generates contacts every step and would otherwise rattle
	// like a machine gun.
	if (b.impactCooldown > 0.f) b.impactCooldown -= dt;

	if (anyContact && b.impactSound != NO_SOUND && b.impactCooldown <= 0.f)
	{
		float hardest = 0.f;
		for (int i = 0; i < numContacts; i++)
			if (-contacts[i].initialVn > hardest) hardest = -contacts[i].initialVn;

		if (hardest >= b.impactMinSpeed)
		{
			// Full volume at roughly a metre-and-a-half drop.
			float vol = hardest / 5.f;
			if (vol > 1.f) vol = 1.f;
			if (vol < 0.15f) vol = 0.15f;

			S_Sound(a, CHAN_BODY, CHANF_OVERLAP, b.impactSound, vol, ATTN_NORM);
			b.impactCooldown = 0.08f;
		}
	}

	// --- solve ------------------------------------------------------------
	//
	// Sequential impulses with Baumgarte stabilisation. The penetration is
	// corrected by biasing the VELOCITY target rather than by moving the body,
	// which is what keeps a resting box still: a positional teleport adds
	// energy the solver never accounted for, and that energy has to go
	// somewhere.
	if (numContacts > 0)
	{
		const float restitution = *vr_physics_restitution;
		const float friction    = *vr_physics_friction;
		const float invDt       = 1.f / dt;

		for (int iter = 0; iter < kSolverIterations; iter++)
		{
			for (int i = 0; i < numContacts; i++)
			{
				Contact &c = contacts[i];
				FVector3 r = c.point - b.pos;
				FVector3 vRel = b.vel + Cross(b.angVel, r);
				float vn = vRel | c.normal;

				// Push out of penetration gently, over several steps.
				float bias = 0.f;
				if (c.penetration > kPenetrationSlop)
					bias = kBaumgarte * invDt * (c.penetration - kPenetrationSlop);

				// Restitution only for a genuine impact. Below the threshold it
				// is dropped entirely -- otherwise every micro-bounce of a
				// resting box re-injects energy and it buzzes forever.
				float target = bias;
				if (c.initialVn < -kRestitutionThreshold)
					target += -restitution * c.initialVn;

				float denom = EffectiveMass(b, r, c.normal);
				if (denom < 1e-8f) continue;

				float lambda = (-vn + target) / denom;

				// Accumulate and clamp so the TOTAL impulse stays pushing, not
				// pulling. Clamping each increment instead would let the solver
				// yank the body down onto the surface.
				float old = c.normalImpulse;
				c.normalImpulse = old + lambda;
				if (c.normalImpulse < 0.f) c.normalImpulse = 0.f;
				lambda = c.normalImpulse - old;

				if (lambda != 0.f)
					ApplyImpulse(b, r, c.normal * lambda);
			}

			// Friction, using the normal impulse accumulated SO FAR. Using only
			// the current iteration's increment makes friction vanish as the
			// solve converges, which is the other half of the slow-drift bug.
			for (int i = 0; i < numContacts; i++)
			{
				Contact &c = contacts[i];
				if (c.normalImpulse <= 0.f) continue;

				FVector3 r = c.point - b.pos;
				FVector3 vRel = b.vel + Cross(b.angVel, r);
				FVector3 vt = vRel - c.normal * (vRel | c.normal);
				float vtLen = vt.Length();
				if (vtLen < 1e-6f) continue;

				FVector3 t = vt / vtLen;
				float denomT = EffectiveMass(b, r, t);
				if (denomT < 1e-8f) continue;

				float jt = -(vRel | t) / denomT;
				const float maxF = friction * c.normalImpulse;
				if (jt >  maxF) jt =  maxF;
				if (jt < -maxF) jt = -maxF;

				ApplyImpulse(b, r, t * jt);
			}
		}

		// Torsional friction, approximated.
		//
		// Point friction at the corners resists sliding well but resists SPIN
		// about the contact normal poorly -- the classic symptom is an object
		// that lands, settles flat, and then keeps slowly turning on the spot
		// like it is on ice. A real contact is a patch, not a point, and the
		// patch resists twist.
		//
		// Modelled as extra angular damping applied only WHILE TOUCHING, so
		// airborne tumbling is untouched. An approximation, deliberately: the
		// honest version needs a contact manifold with a real patch radius,
		// which is a lot of machinery to stop a magazine pirouetting.
		//
		// FADED OUT WITH SPEED, and this part is not optional. A twisting patch
		// of contact is what a RESTING object has. A magazine hurled into a
		// wall is not resting on it -- it touches for an instant and leaves --
		// and killing its spin there would stop it tumbling at the exact moment
		// it should be tumbling hardest, which reads as the throw going dead on
		// impact. So this only reaches full strength once the body has settled.
		{
			float speed = b.vel.Length();
			float scale = 1.f - speed / 1.5f;
			if (scale < 0.f) scale = 0.f;

			const float d = 1.f - *vr_physics_contactspindamp * scale * dt;
			b.angVel *= (d > 0.f) ? d : 0.f;
		}
	}

	// --- sleep ------------------------------------------------------------
	//
	// Support is LATCHED for a short window rather than tested per step, and
	// that detail is the whole thing working.
	//
	// A body at rest does not report a contact every step: the penetration
	// bias pushes it a fraction of a millimetre clear, the next step finds no
	// overlap, and the step after that gravity puts it back. Requiring a
	// contact on the very step the sleep timer matures meant the timer was
	// reset to zero every few steps and could never reach its threshold. The
	// visible result was that NOTHING ever settled -- measured at 45 bodies
	// dropped and 45 still awake -- which reads as endless low-energy wobbling
	// and slow drift rather than as the sleep bug it actually is.
	if (anyContact) b.supportTimer = kSupportGrace;
	else if (b.supportTimer > 0.f)
	{
		b.supportTimer -= dt;
		if (b.supportTimer < 0.f) b.supportTimer = 0.f;
	}

	// Smoothed VELOCITY VECTORS, not smoothed speeds, and that distinction is
	// the whole fix.
	//
	// A body at rest is not still. Gravity adds ~11cm/s downward every step and
	// the contact cancels it a moment later, so its instantaneous speed
	// oscillates between roughly +3 and -8 cm/s forever and never drops below
	// any threshold worth having. Averaging the speed does not help -- the
	// average of an oscillation's MAGNITUDE is not small.
	//
	// Averaging the vector does: equal and opposite contributions cancel, so a
	// body bouncing about zero averages to nearly zero, while a body genuinely
	// creeping at 8cm/s averages to 8cm/s. That tells "resting" and "drifting"
	// apart, which is exactly the question being asked.
	{
		const float k = 0.12f;
		b.velEMA = b.velEMA * (1.f - k) + b.vel * k;
		b.angEMA = b.angEMA * (1.f - k) + b.angVel * k;
	}

	// SLEEP IS DECIDED BY DISPLACEMENT, NOT BY VELOCITY.
	//
	// This is the third attempt at it and the first correct one. The two before
	// tested velocity, and velocity cannot answer the question: a body at rest
	// on a floor is never at zero velocity. Gravity adds g/rate every step --
	// 0.109 m/s at 90Hz -- and the contact only cancels it on the steps where
	// the body has actually sunk far enough to overlap the surface again. In
	// between it is genuinely falling. Measured on a magazine sitting
	// motionless: a rock-steady 0.1090 m/s forever, which is exactly 9.81/90.
	//
	// Smoothing does not rescue it either. The signal is a sawtooth that never
	// approaches zero, so no threshold separates "resting" from "creeping"
	// without also being larger than a real slow drift.
	//
	// Displacement asks the question directly: has this thing actually gone
	// anywhere. A resting body bobs by about a millimetre and stays put; a
	// creeping one does not. That holds at any simulation rate, under any
	// gravity, which velocity thresholds never did.
	if (!b.asleep)
	{
		const float driftLin = (b.pos - b.sleepRefPos).Length();

		// Angle between two orientations, via the quaternion dot product.
		float qd = b.rot.x * b.sleepRefRot.x + b.rot.y * b.sleepRefRot.y
		         + b.rot.z * b.sleepRefRot.z + b.rot.w * b.sleepRefRot.w;
		if (qd < 0.f) qd = -qd;
		if (qd > 1.f) qd = 1.f;
		const float driftAng = 2.f * acosf(qd);

		const bool slow = (driftLin < kSleepDrift) && (driftAng < kSleepDriftAngle);
		if (slow && b.supportTimer > 0.f)
		{
			b.sleepTimer += dt;
			if (b.sleepTimer >= kSleepTime)
			{
				b.asleep = true;
				b.vel = FVector3(0, 0, 0);
				b.angVel = FVector3(0, 0, 0);

				if (vr_physics_debug && !b.restReported)
				{
					b.restReported = true;
					const float floorZ = MapToM(sec->floorplane.ZatPoint(MToMap(b.pos.X), MToMap(b.pos.Y)));
					Printf("[PHYS] rest %s  z=%.3fm floor=%.3fm above=%.4fm  (map z=%.1f floor=%.1f)\n",
						a->GetClass()->TypeName.GetChars(),
						b.pos.Z, floorZ, b.pos.Z - floorZ,
						MToMap(b.pos.Z), MToMap(floorZ));
				}
			}
		}
		else
		{
			// It moved. Start measuring again from where it is now.
			b.sleepTimer = 0.f;
			b.sleepRefPos = b.pos;
			b.sleepRefRot = b.rot;
		}
	}

	// --- trace ------------------------------------------------------------
	//
	// The only window into a body's behaviour, since there is no console in a
	// headset. Off by default; set vr_physics_trace to a few Hz from the menu
	// when something needs watching.
	if (*vr_physics_trace > 0 && !b.asleep)
	{
		b.traceTimer += dt;
		const float period = 1.f / (float)*vr_physics_trace;
		if (b.traceTimer >= period)
		{
			b.traceTimer = 0.f;
			const float floorZ = MapToM(sec->floorplane.ZatPoint(MToMap(b.pos.X), MToMap(b.pos.Y)));

			// Euler is logged alongside the angular velocity ON PURPOSE, so a
			// visible spin can be attributed. If spin is ~0 while yaw/pitch/roll
			// keep changing, the body is NOT rotating and the quaternion-to-
			// Doom-angles conversion is the culprit. If spin is non-zero, the
			// rotation is real and belongs to the solver.
			double yaw, pitch, roll;
			b.rot.ToEulerDeg(yaw, pitch, roll);

			Printf("[PHYSTRACE] %s pos=(%.3f %.3f %.3f) above=%.4f vel=(%.3f %.3f %.3f)|%.3f "
				"spin=(%.2f %.2f %.2f)|%.2f ypr=(%.1f %.1f %.1f) contacts=%d sleepT=%.2f\n",
				a->GetClass()->TypeName.GetChars(),
				b.pos.X, b.pos.Y, b.pos.Z, b.pos.Z - floorZ,
				b.vel.X, b.vel.Y, b.vel.Z, b.vel.Length(),
				b.angVel.X, b.angVel.Y, b.angVel.Z, b.angVel.Length(),
				yaw, pitch, roll,
				numContacts, b.sleepTimer);
		}
	}
}

// ---------------------------------------------------------------------------
// Body against body
//
// Corner-in-box, both ways round. For every corner of A that lies inside B, the
// contact normal is B's face that it is nearest to escaping through, and the
// penetration is that distance.
//
// This misses pure edge-against-edge contact, where neither box has a corner
// inside the other -- two boxes crossed like an X. That is deliberate: catching
// it needs a full separating-axis test with face clipping, several hundred more
// lines, and for the objects here (magazines, rounds, hands, a pistol) the
// corner case dominates overwhelmingly. If two objects ever visibly pass
// through each other at a crossed angle, this is why, and that is the fix.
// ---------------------------------------------------------------------------

// The box version of the above lived here. It is gone rather than kept beside
// HullDeepest: a box is now expressed AS a hull (HullMakeBox), so keeping a
// second code path for the same question would be two things to keep in step
// for no gain.

// Solve one pair. Impulses are shared according to inverse mass, so a kinematic
// body (invMass 0) pushes without being pushed -- which is exactly what a hand
// and a held weapon need.
void SolvePair(PhysBody &A, PhysBody &B, float dt)
{
	// NOT skipped just because both sides are kinematic.
	//
	// Two externally-tracked bodies -- a hand and the OTHER hand, a weapon and
	// a hand, two weapons -- cannot physically push each other: neither has a
	// finite mass for an impulse to move, since both positions come from
	// outside the simulation. That is real and stays true below, where every
	// impulse application is individually guarded by `if (!X.kinematic)`, so
	// two kinematic bodies always resolve to a no-op there via the existing
	// `denom < 1e-8f` check.
	//
	// But returning early HERE, before contacts are even gathered, would also
	// throw away the one thing that pair CAN honestly report: that contact
	// happened at all. That is what "feel the gun against your palm" needs --
	// not a push, since your real hand cannot be shoved by software, but a
	// detected touch to hang a haptic pulse and a sound off. Skipping here
	// would have made that permanently impossible instead of merely unbuilt.

	// Hands only PUSH things when they are set solid. They still exist and can
	// still hold things when they are not.
	if ((A.handIndex >= 0 || B.handIndex >= 0) && !*vr_physics_hands) return;

	// Broadphase: bounding spheres, now sized from the real geometry rather
	// than from a box nobody's model actually was.
	const float rA = A.boundRadius;
	const float rB = B.boundRadius;
	FVector3 delta = B.pos - A.pos;
	const float distSq = delta.LengthSquared();
	if (distSq > (rA + rB) * (rA + rB)) return;

	struct PairContact { FVector3 point, normal; float penetration, initialVn, impulse; };
	// Raised with hulls, same reason as the world contact array: two real
	// shapes meeting face-on present many more points than eight corners could.
	PairContact pc[48];
	const int kMaxPair = 48;
	int n = 0;

	// Vertices of one inside the other, both directions. The normal always
	// points from A toward B, so the sign handling below stays uniform.
	//
	// TESTING EVERY HULL OF THE DESTINATION, not just one, is what makes a
	// cavity work. A magwell is several convex slabs with a gap between them:
	// a magazine vertex in the gap is inside NONE of them and is free to move,
	// and the same vertex a few millimetres off-axis is inside a wall slab and
	// gets pushed back. That behaviour is not written anywhere -- it falls out
	// of asking each convex piece separately, which is exactly why the shape
	// has to be a set and not a single volume.
	for (int pass = 0; pass < 2 && n < kMaxPair; pass++)
	{
		PhysBody &src = pass ? B : A;
		PhysBody &dst = pass ? A : B;

		for (unsigned sh = 0; sh < src.hulls.Size() && n < kMaxPair; sh++)
		for (unsigned vi = 0; vi < src.hulls[sh].verts.Size() && n < kMaxPair; vi++)
		{
			FVector3 world = src.pos + src.rot.Rotate(src.hulls[sh].verts[vi]);

			FVector3 local = dst.rot.Inverse().Rotate(world - dst.pos);

			// Shallowest penetration across the destination's hulls. A point
			// inside two overlapping slabs should be pushed out the near way,
			// not the far one.
			FVector3 axis(0, 0, 0); float depth = 0.f;
			bool hit = false;
			for (unsigned dh = 0; dh < dst.hulls.Size(); dh++)
			{
				FVector3 a2; float d2;
				if (!HullDeepest(local, dst.hulls[dh], a2, d2)) continue;
				if (!hit || d2 < depth) { axis = a2; depth = d2; hit = true; }
			}
			if (!hit) continue;

			FVector3 nWorld = dst.rot.Rotate(axis);
			// Normal must point from A to B.
			if (pass == 0) nWorld = -nWorld;

			FVector3 rA2 = world - A.pos;
			FVector3 rB2 = world - B.pos;
			FVector3 vA = A.vel + Cross(A.angVel, rA2);
			FVector3 vB = B.vel + Cross(B.angVel, rB2);

			PairContact &c = pc[n++];
			c.point = world;
			c.normal = nWorld;
			c.penetration = depth;
			c.initialVn = (vB - vA) | nWorld;
			c.impulse = 0.f;
		}
	}

	if (n == 0) return;

	// Resting on ANOTHER BODY counts as being supported, exactly as resting on
	// the floor does. Without this, a magazine that comes to rest on top of
	// another one can never sleep: it registers no world contact at all, so its
	// sleep timer would be reset every single step by something that is, from
	// its point of view, holding it up perfectly well.
	A.supportTimer = kSupportGrace;
	B.supportTimer = kSupportGrace;

	// Wake a sleeper that has just been hit. Without this a settled magazine
	// would sit there while a thrown one passed straight through it -- the
	// sleeping body is skipped by the integrator, so nothing would ever move it
	// again no matter how hard it was struck.
	//
	// Only a real approach wakes it. Merely resting against something must not,
	// or two touching objects keep each other awake forever.
	for (int i = 0; i < n; i++)
	{
		if (-pc[i].initialVn <= kWakeOnImpact) continue;

		if (A.asleep) { A.asleep = false; A.sleepTimer = 0.f; A.restReported = false; }
		if (B.asleep) { B.asleep = false; B.sleepTimer = 0.f; B.restReported = false; }
		break;
	}

	// A CONTACT HAPTIC, for the one case a push is physically impossible: your
	// own gun touching your own other hand, or two held weapons meeting.
	// Neither side can be shoved -- both positions come from tracking, not
	// from the solver -- so a buzz is the only honest way to make that contact
	// felt at all. Deliberately scoped to hand/weapon pairs only, not to
	// grabbed objects riding a hand: that is a related but separate question
	// for once basic weapon solidity has been felt out.
	{
		const int handA = (A.handIndex >= 0) ? A.handIndex : A.weaponHand;
		const int handB = (B.handIndex >= 0) ? B.handIndex : B.weaponHand;

		if (A.kinematic && B.kinematic && handA >= 0 && handB >= 0 && handA != handB)
		{
			float hardest = 0.f;
			for (int i = 0; i < n; i++)
				if (-pc[i].initialVn > hardest) hardest = -pc[i].initialVn;

			const float kHapticMinSpeed = 0.15f;   // m/s -- matches the wake threshold
			if (hardest >= kHapticMinSpeed && A.impactCooldown <= 0.f && B.impactCooldown <= 0.f)
			{
				float amp = hardest / 3.f;
				if (amp > 1.f) amp = 1.f;
				if (amp < 0.2f) amp = 0.2f;

				VR_ScriptHaptic(handA, amp, 40.0);
				VR_ScriptHaptic(handB, amp, 40.0);

				A.impactCooldown = 0.1f;
				B.impactCooldown = 0.1f;
			}
		}
	}

	const float restitution = *vr_physics_restitution;
	const float friction    = *vr_physics_friction;
	const float invDt       = 1.f / dt;

	for (int iter = 0; iter < kSolverIterations; iter++)
	{
		for (int i = 0; i < n; i++)
		{
			PairContact &c = pc[i];
			FVector3 rA2 = c.point - A.pos;
			FVector3 rB2 = c.point - B.pos;

			FVector3 vA = A.vel + Cross(A.angVel, rA2);
			FVector3 vB = B.vel + Cross(B.angVel, rB2);
			float vn = (vB - vA) | c.normal;

			// POSITIVE. The normal points from A toward B, so separating means
			// driving the relative velocity along it upwards -- B away from A.
			// This was negative, which drove the correction the wrong way: the
			// overlap was never resolved, and the two bodies slid past each
			// other under the velocity term alone instead of coming to rest in
			// contact. The visible symptom was objects refusing to stack while
			// still clearly avoiding each other.
			float bias = 0.f;
			if (c.penetration > kPenetrationSlop)
				bias = kBaumgarte * invDt * (c.penetration - kPenetrationSlop);

			float target = bias;
			if (c.initialVn < -kRestitutionThreshold)
				target += -restitution * c.initialVn;

			float denom = EffectiveMass(A, rA2, c.normal) + EffectiveMass(B, rB2, c.normal);
			if (denom < 1e-8f) continue;

			float lambda = (target - vn) / denom;

			float old = c.impulse;
			c.impulse = old + lambda;
			if (c.impulse < 0.f) c.impulse = 0.f;
			lambda = c.impulse - old;
			if (lambda == 0.f) continue;

			FVector3 imp = c.normal * lambda;
			if (!A.kinematic) ApplyImpulse(A, rA2, -imp);
			if (!B.kinematic) ApplyImpulse(B, rB2,  imp);
		}

		for (int i = 0; i < n; i++)
		{
			PairContact &c = pc[i];
			if (c.impulse <= 0.f) continue;

			FVector3 rA2 = c.point - A.pos;
			FVector3 rB2 = c.point - B.pos;
			FVector3 vA = A.vel + Cross(A.angVel, rA2);
			FVector3 vB = B.vel + Cross(B.angVel, rB2);
			FVector3 vRel = vB - vA;

			FVector3 vt = vRel - c.normal * (vRel | c.normal);
			float vtLen = vt.Length();
			if (vtLen < 1e-6f) continue;

			FVector3 t = vt / vtLen;
			float denomT = EffectiveMass(A, rA2, t) + EffectiveMass(B, rB2, t);
			if (denomT < 1e-8f) continue;

			float jt = -(vRel | t) / denomT;
			const float maxF = friction * c.impulse;
			if (jt >  maxF) jt =  maxF;
			if (jt < -maxF) jt = -maxF;

			FVector3 fimp = t * jt;
			if (!A.kinematic) ApplyImpulse(A, rA2, -fimp);
			if (!B.kinematic) ApplyImpulse(B, rB2,  fimp);
		}
	}

	// TODO: objects hitting EACH OTHER are still silent -- impact sound is
	// currently driven only by contacts against world geometry. Two magazines
	// clacking together should be audible, and will matter more once a
	// magazine meets a gun.
}

// ---------------------------------------------------------------------------
// The hands
//
// Two kinematic bodies driven from the controllers. Infinite mass, so they push
// everything and nothing pushes back -- which is what a hand should do, and is
// the same arrangement a held weapon will use.
//
// Orientation comes from the pose angles the device layer publishes, NOT from
// the hand transform matrix. That matrix applies a mirror and a non-uniform
// pixelstretch scale BEFORE its rotations, so its basis is sheared whenever the
// wrist is tilted and its determinant is negative for the off hand. No rotation
// can be recovered from it honestly.
//
// They collide only with other bodies, never with level geometry: a hand that
// could be blocked by a wall would either stop tracking your real hand or fight
// it, and both are worse than letting it pass through.
// ---------------------------------------------------------------------------

void UpdateHands(float dt)
{
	if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS) return;
	player_t *pl = &players[consoleplayer];
	AActor *pawn = pl->mo;

	// The hand bodies exist whenever there is a player, REGARDLESS of whether
	// hands are set solid. Solidity is about whether they shove objects around;
	// grabbing needs a hand to attach things to either way, and tying both to
	// one switch meant turning off "hands are solid" silently stopped you
	// picking anything up. Two unrelated behaviours, one switch, no clue why.
	const bool wantHands = (pawn != nullptr);

	for (int hand = 0; hand < 2; hand++)
	{
		PhysBody *b = nullptr;
		for (unsigned i = 0; i < g_bodies.Size(); i++)
			if (g_bodies[i].handIndex == hand) { b = &g_bodies[i]; break; }

		if (!wantHands)
		{
			if (b != nullptr)
			{
				for (unsigned i = 0; i < g_bodies.Size(); i++)
					if (g_bodies[i].handIndex == hand) { g_bodies.Delete(i); break; }
			}
			continue;
		}

		if (b == nullptr)
		{
			PhysBody nb;
			nb.handIndex = hand;
			nb.owner = nullptr;
			nb.kinematic = true;
			nb.invMass = 0.f;                       // immovable
			nb.invInertia = FVector3(0, 0, 0);
			g_bodies.Push(nb);
			b = &g_bodies[g_bodies.Size() - 1];
		}

		const float s = (float)*vr_physics_handsize;
		b->half = FVector3(0.045f * s, 0.030f * s, 0.090f * s);
		b->hulls.Clear();
		b->ShapeFinish();

		const DVector3 p = (hand == 0) ? pawn->AttackPos : pawn->OffhandPos;
		const FVector3 newPos(MapToM(p.X), MapToM(p.Y), MapToM(p.Z));

		const double yaw   = (hand == 0) ? pawn->Angles.Yaw.Degrees()   : pawn->OffhandAngle.Degrees();
		const double pitch = (hand == 0) ? pawn->AttackPitch.Degrees()  : pawn->OffhandPitch.Degrees();
		const double roll  = (hand == 0) ? pawn->MainHandRoll.Degrees() : pawn->OffhandRoll.Degrees();
		const Quat newRot = Quat::FromEulerDeg(yaw, pitch, roll);

		// A HAND MUST CARRY ITS VELOCITY, not just its position.
		//
		// Setting only the position gives a body that teleports each frame while
		// reporting that it is standing perfectly still. Everything downstream
		// then behaves accordingly: the contact solver sees no approach speed
		// and produces only a feeble push from overlap, and a SLEEPING object is
		// never woken at all, because waking requires a real impact. The visible
		// result is a hand that passes through a magazine on the floor as though
		// neither existed.
		//
		// Derived from how far it actually moved since the last frame, which is
		// also exactly what makes a swat carry the force of a fast swing rather
		// than a slow reach.
		if (g_handHavePrev[hand] && dt > 1e-6f)
		{
			b->vel = (newPos - g_handPrevPos[hand]) / dt;

			// Angular velocity from the rotation delta: axis and angle of the
			// turn that gets from last frame's orientation to this one.
			Quat prev = g_handPrevRot[hand];
			Quat inv = prev.Inverse();
			Quat d;
			d.w = newRot.w*inv.w - newRot.x*inv.x - newRot.y*inv.y - newRot.z*inv.z;
			d.x = newRot.w*inv.x + newRot.x*inv.w + newRot.y*inv.z - newRot.z*inv.y;
			d.y = newRot.w*inv.y - newRot.x*inv.z + newRot.y*inv.w + newRot.z*inv.x;
			d.z = newRot.w*inv.z + newRot.x*inv.y - newRot.y*inv.x + newRot.z*inv.w;
			d.Normalize();

			float w = d.w;
			if (w < -1.f) w = -1.f; else if (w > 1.f) w = 1.f;
			float angle = 2.f * acosf(fabsf(w));
			FVector3 axis(d.x, d.y, d.z);
			const float axisLen = axis.Length();
			if (axisLen > 1e-6f && angle > 1e-6f)
			{
				if (w < 0.f) angle = -angle;
				b->angVel = (axis / axisLen) * (angle / dt);
			}
			else
			{
				b->angVel = FVector3(0, 0, 0);
			}
		}
		else
		{
			b->vel = FVector3(0, 0, 0);
			b->angVel = FVector3(0, 0, 0);
		}

		// Remember this frame's motion for the release window.
		{
			int &wp = g_handHistPos[hand];
			g_handHist[hand][wp].vel = b->vel;
			g_handHist[hand][wp].angVel = b->angVel;
			wp = (wp + 1) % kHandHistory;
			if (wp == 0) g_handHistFilled[hand] = true;
		}

		g_handPrevPos[hand] = newPos;
		g_handPrevRot[hand] = newRot;
		g_handHavePrev[hand] = true;

		b->pos = newPos;
		b->rot = newRot;
		b->asleep = false;
	}

	// Carry whatever the hands are holding, at physics rate.
	for (unsigned i = 0; i < g_bodies.Size(); i++)
	{
		PhysBody &h = g_bodies[i];
		if (h.heldByHand < 0) continue;

		const PhysBody *hand = nullptr;
		for (unsigned k = 0; k < g_bodies.Size(); k++)
			if (g_bodies[k].handIndex == h.heldByHand) { hand = &g_bodies[k]; break; }

		if (hand == nullptr)
		{
			// The hand went away underneath it -- drop it rather than leave it
			// frozen in mid-air forever.
			h.heldByHand = -1;
			h.kinematic = false;
			continue;
		}

		// Rebuild the pose it had relative to the hand when it was grabbed.
		Quat q;
		q.w = hand->rot.w*h.grabRotOffset.w - hand->rot.x*h.grabRotOffset.x
		    - hand->rot.y*h.grabRotOffset.y - hand->rot.z*h.grabRotOffset.z;
		q.x = hand->rot.w*h.grabRotOffset.x + hand->rot.x*h.grabRotOffset.w
		    + hand->rot.y*h.grabRotOffset.z - hand->rot.z*h.grabRotOffset.y;
		q.y = hand->rot.w*h.grabRotOffset.y - hand->rot.x*h.grabRotOffset.z
		    + hand->rot.y*h.grabRotOffset.w + hand->rot.z*h.grabRotOffset.x;
		q.z = hand->rot.w*h.grabRotOffset.z + hand->rot.x*h.grabRotOffset.y
		    - hand->rot.y*h.grabRotOffset.x + hand->rot.z*h.grabRotOffset.w;
		q.Normalize();

		h.rot = q;
		h.pos = hand->pos + hand->rot.Rotate(h.grabPosOffset);

		// Inherit the hand's motion continuously, so letting go needs no
		// separate "how fast was I moving" calculation -- the object already
		// knows. Angular too, or a spun object would stop dead on release.
		h.vel = hand->vel + Cross(hand->angVel, h.pos - hand->pos);
		h.angVel = hand->angVel;

		h.asleep = false;
		h.sleepTimer = 0.f;
	}
}

// ---------------------------------------------------------------------------
// The weapon in each hand -- a solid object, nothing more.
//
// Slice 4. One box per hand that currently holds a T77, rigidly attached to
// that hand's own body exactly the way a held object rides it, kinematic for
// the same reason hands are: the gun follows the controller exactly and is
// never blocked by level geometry, but it pushes everything that is not
// static, and (see SolvePair) hitting your other hand or another weapon with
// it is felt as a haptic pulse even though nothing can physically shove a
// tracked hand.
//
// Detected by class name substring, matched cross-pk3 the same way
// rs_hands.zs matches weapons without a compile-time reference to any of
// them -- this file cannot name RS_T77 and does not need to.
//
// PLACEHOLDER SHAPE. One box for the whole gun, sized and positioned from the
// vr_physics_weapon* cvars, not from the mesh. Real per-bone weapon colliders
// (grip, slide, magwell) are later work; this exists so "is a held gun solid"
// can be felt and judged before that model work is done.
// ---------------------------------------------------------------------------

bool IsPhysicalWeapon(AActor *weap)
{
	if (weap == nullptr) return false;
	FString name = weap->GetClass()->TypeName.GetChars();
	name.ToLower();
	return name.IndexOf("t77") >= 0;
}

void UpdateWeapons()
{
	if (consoleplayer < 0 || consoleplayer >= MAXPLAYERS) return;
	player_t *pl = &players[consoleplayer];
	if (pl->mo == nullptr) return;

	for (int hand = 0; hand < 2; hand++)
	{
		AActor *weap = (hand == 0) ? pl->ReadyWeapon : pl->OffhandWeapon;
		const bool want = IsPhysicalWeapon(weap);

		PhysBody *wb = nullptr;
		for (unsigned i = 0; i < g_bodies.Size(); i++)
			if (g_bodies[i].weaponHand == hand) { wb = &g_bodies[i]; break; }

		if (!want)
		{
			if (wb != nullptr)
			{
				for (unsigned i = 0; i < g_bodies.Size(); i++)
					if (g_bodies[i].weaponHand == hand) { g_bodies.Delete(i); break; }
			}
			continue;
		}

		const PhysBody *handBody = nullptr;
		for (unsigned i = 0; i < g_bodies.Size(); i++)
			if (g_bodies[i].handIndex == hand) { handBody = &g_bodies[i]; break; }
		if (handBody == nullptr) continue;   // hands disabled -- nothing to attach to

		if (wb == nullptr)
		{
			PhysBody nb;
			nb.weaponHand = hand;
			nb.owner = nullptr;
			nb.kinematic = true;
			nb.invMass = 0.f;
			nb.invInertia = FVector3(0, 0, 0);
			g_bodies.Push(nb);
			wb = &g_bodies[g_bodies.Size() - 1];
		}

		const FVector3 wantHalf(
			*vr_physics_weaponlen * 0.5f,
			*vr_physics_weaponwidth * 0.5f,
			*vr_physics_weaponheight * 0.5f);
		// Rebuilt only when it actually changes -- these are live menu sliders,
		// so a blind rebuild every frame would throw away real hull geometry
		// the moment a weapon supplied any, and would do it 90 times a second.
		if (wb->hulls.Size() != 1 || wb->half != wantHalf)
		{
			wb->half = wantHalf;
			wb->hulls.Clear();
			wb->ShapeFinish();
		}

		const FVector3 localOfs(*vr_physics_weapon_ofs_fwd, 0.f, *vr_physics_weapon_ofs_up);
		wb->rot = handBody->rot;
		wb->pos = handBody->pos + handBody->rot.Rotate(localOfs);

		// Inherits the hand's motion the same way a grabbed object does --
		// needed for the impact-speed haptic above, and for pushing a
		// magazine at something close to the speed the gun actually swung at.
		wb->vel = handBody->vel + Cross(handBody->angVel, wb->pos - handBody->pos);
		wb->angVel = handBody->angVel;

		wb->asleep = false;
	}
}

// Push the solver's transform back onto the actor.
//
// Done here rather than in AActor::Tick because in VR the playsim frequently
// runs ZERO tics between rendered frames -- Tick is not a per-frame hook, and a
// body whose actor is never relinked renders lit by the wrong sector, or is
// culled and vanishes.
void WriteBack(PhysBody &b)
{
	AActor *a = b.owner;
	if (a == nullptr) return;

	// Back from centre-of-mass space to where the actor's origin belongs.
	const FVector3 originPos = b.pos - b.rot.Rotate(b.comOffset);
	const DVector3 newPos(MToMap(originPos.X), MToMap(originPos.Y), MToMap(originPos.Z));

	FLinkContext ctx;
	a->UnlinkFromWorld(&ctx);
	a->SetXYZ(newPos);
	a->CheckPortalTransition(false);
	a->LinkToWorld(&ctx);

	double yaw, pitch, roll;
	b.rot.ToEulerDeg(yaw, pitch, roll);
	a->Angles.Yaw   = DAngle::fromDeg(yaw);
	a->Angles.Pitch = DAngle::fromDeg(pitch);
	a->Angles.Roll  = DAngle::fromDeg(roll);

	// Doom's own velocity is kept roughly in step so anything that reads it
	// sees something sane, but it is an output here, never an input.
	a->Vel = DVector3(MToMap(b.vel.X), MToMap(b.vel.Y), MToMap(b.vel.Z)) / TICRATE;

	// The renderer lerps between Prev and Pos() by ticFrac, and Prev is only
	// reset on a tic. A transform written at frame rate against a stale
	// tic-boundary Prev rubber-bands -- worst on whatever is held in front of
	// your face. Physics bodies opt out of interpolation entirely.
	a->renderflags |= RF_DONTINTERPOLATE;
}

void ReportLine(const char *why, double dt)
{
	if (!vr_physics_debug) return;

	const uint64_t nowNs = I_nsTime();
	const double elapsed = (nowNs - g_reportStartNs) / 1000000000.0;
	const double fps = (elapsed > 0.0) ? g_frames / elapsed : 0.0;
	const double sps = (elapsed > 0.0) ? g_steps / elapsed : 0.0;

	auto vrmode = VRMode::GetVRMode();
	const bool isVR = (vrmode != nullptr) && vrmode->IsVR();

	// Hands are excluded from both counts: they are always present and always
	// "awake" by construction, and including them would mean the interesting
	// number never reads zero.
	int awake = 0, real = 0;
	for (unsigned i = 0; i < g_bodies.Size(); i++)
	{
		if (g_bodies[i].handIndex >= 0 || g_bodies[i].weaponHand >= 0) continue;
		real++;
		if (!g_bodies[i].asleep) awake++;
	}

	// WHY the quietest body is not asleep.
	//
	// "Nothing ever sleeps" is not actionable on its own -- it has already had
	// two different causes in this file, and guessing at a third would be worse
	// than useless. Each of the four conditions is printed separately so the
	// failing one names itself.
	if (awake > 0)
	{
		const PhysBody *quietest = nullptr;
		float best = 1e9f;
		for (unsigned i = 0; i < g_bodies.Size(); i++)
		{
			const PhysBody &b = g_bodies[i];
			if (b.asleep || b.handIndex >= 0 || b.weaponHand >= 0) continue;
			const float m = b.velEMA.Length();
			if (m < best) { best = m; quietest = &b; }
		}

		if (quietest != nullptr)
		{
			const float drift = (quietest->pos - quietest->sleepRefPos).Length();
			Printf("[PHYS] quietest: drift=%.5f (need <%.4f)%s  support=%.2f%s  "
				"sleepT=%.2f/%.2f  held=%d  rawvel=%.3f rawspin=%.3f\n",
				drift, kSleepDrift, (drift < kSleepDrift) ? " ok" : " BLOCKS",
				quietest->supportTimer, (quietest->supportTimer > 0.f) ? " ok" : " BLOCKS",
				quietest->sleepTimer, kSleepTime,
				quietest->kinematic ? 1 : 0,
				quietest->vel.Length(), quietest->angVel.Length());
		}
	}

	Printf("[PHYS] %-9s frames/s=%6.1f steps/s=%6.1f dropped=%d  dt=%.4f (min %.4f max %.4f)  "
		"tics=%d  %s paused=%d menu=%d  vr=%d backend=%d  bodies=%u awake=%d\n",
		why, fps, sps, g_dropped,
		dt, (g_dtMin > 1e8 ? 0.0 : g_dtMin), g_dtMax,
		gametic - g_ticsAtReport,
		GamestateName(gamestate),
		paused ? 1 : 0, menuactive != MENU_Off ? 1 : 0,
		isVR ? 1 : 0, *vid_preferbackend,
		real, awake);

	g_reportStartNs = nowNs;
	g_frames = g_steps = g_dropped = 0;
	g_ticsAtReport = gametic;
	g_dtMin = 1e9; g_dtMax = 0.0;
}

} // namespace

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void P_PhysicsFrame()
{
	const uint64_t nowNs = I_nsTime();

	if (g_lastTimeNs == 0 || nowNs < g_lastTimeNs)
	{
		g_lastTimeNs = nowNs;
		g_reportStartNs = nowNs;
		g_ticsAtReport = gametic;
		return;
	}

	double dt = (nowNs - g_lastTimeNs) / 1000000000.0;
	g_lastTimeNs = nowNs;

	const double kMaxFrame = 0.25;
	if (dt > kMaxFrame) dt = kMaxFrame;
	if (dt < 0.0) dt = 0.0;

	g_frames++;
	if (dt < g_dtMin) g_dtMin = dt;
	if (dt > g_dtMax) g_dtMax = dt;

	const bool run = ShouldStep();

	if (!run)
	{
		g_accumulator = 0.0;
	}
	else
	{
		if (!g_running) g_accumulator = 0.0;

		// Hands first: they are the one thing whose position comes from outside
		// the simulation, and everything else this step reacts to where they
		// are now rather than where they were last frame.
		UpdateHands((float)dt);
		UpdateWeapons();

		const double step = 1.0 / (double)*vr_physics_hz;
		g_accumulator += dt;

		int steps = 0;
		const int maxSteps = *vr_physics_maxsteps;
		while (g_accumulator >= step && steps < maxSteps)
		{
			for (unsigned i = 0; i < g_bodies.Size(); i++)
			{
				PhysBody &b = g_bodies[i];

				// SUBSTEP ANYTHING MOVING FAST. Collision here is discrete: a
				// contact exists only if the body OVERLAPS a surface on the
				// step that gets tested. A magazine thrown at 12 m/s travels
				// 14cm in one 90Hz step -- further than the magazine is long --
				// so it can be in front of a wall on one step and behind it on
				// the next, having overlapped it on neither. It passes straight
				// through, which is exactly what was seen.
				//
				// Splitting the step so the body never moves more than a
				// fraction of its own smallest dimension is the cheap fix and
				// costs nothing at rest, where sub == 1.
				// The narrowest half-thickness in the hull set. A long
				// thin object -- a cartridge, a slide -- can tunnel through a
				// floor at a speed a compact one of the same bounding radius
				// never would, so the bound has to come from the thin
				// direction, not the overall size.
				float smallest = b.thinHalf;

				const float travel = b.vel.Length() * (float)step;
				int sub = 1;
				if (smallest > 1e-5f)
					sub = 1 + (int)(travel / (0.4f * smallest));
				if (sub < 1) sub = 1;
				else if (sub > 16) sub = 16;

				const float subDt = (float)step / (float)sub;
				for (int s = 0; s < sub; s++)
					StepBody(b, subDt);
			}

			// Bodies against each other, after all of them have moved.
			//
			// Separate from the per-body pass because a pair cannot be resolved
			// from one side: both bodies' inverse masses decide how the impulse
			// is shared, and doing it inside each body's own step would apply it
			// twice and roughly double the bounce.
			for (unsigned i = 0; i + 1 < g_bodies.Size(); i++)
				for (unsigned j = i + 1; j < g_bodies.Size(); j++)
					SolvePair(g_bodies[i], g_bodies[j], (float)step);

			g_accumulator -= step;
			steps++;
			g_steps++;
		}

		if (g_accumulator >= step)
		{
			g_dropped++;
			g_accumulator = 0.0;
		}

		if (steps > 0)
		{
			for (unsigned i = 0; i < g_bodies.Size(); i++)
				WriteBack(g_bodies[i]);
		}
	}

	g_running = run;

	const bool changed = (run != g_lastRunning) || ((int)gamestate != g_lastGamestate);
	if (changed)
	{
		ReportLine(run ? "START" : "STOP", dt);
		g_lastRunning = run;
		g_lastGamestate = (int)gamestate;
	}
	else if (nowNs - g_reportStartNs >= 1000000000ull)
	{
		ReportLine(run ? "run" : "idle", dt);
	}
}

void P_PhysicsLevelStart()
{
	g_bodies.Clear();
	g_accumulator = 0.0;
	g_running = false;

	// Forget where the hands were. A level change teleports the player, and a
	// stale previous pose would derive one enormous frame of hand velocity and
	// fire everything nearby across the map.
	g_handHavePrev[0] = g_handHavePrev[1] = false;
	if (vr_physics_debug) Printf("[PHYS] level start -- bodies cleared\n");
}

void P_PhysicsLevelEnd()
{
	g_bodies.Clear();
	g_accumulator = 0.0;
	g_running = false;

	// Forget where the hands were. A level change teleports the player, and a
	// stale previous pose would derive one enormous frame of hand velocity and
	// fire everything nearby across the map.
	g_handHavePrev[0] = g_handHavePrev[1] = false;
	if (vr_physics_debug) Printf("[PHYS] level end -- bodies cleared\n");
}

void P_PhysicsRemoveBody(AActor *a)
{
	// The hands are ownerless; a null actor must not match them.
	if (a == nullptr) return;

	for (unsigned i = 0; i < g_bodies.Size(); i++)
	{
		if (g_bodies[i].owner == a)
		{
			g_bodies.Delete(i);
			return;
		}
	}
}

// ---------------------------------------------------------------------------
// ZScript API
// ---------------------------------------------------------------------------

// Turn an actor into a physics body. Mass in kilograms, half-extents in METRES
// -- real units, because that is what the solver runs in and hiding the
// conversion here would just move the confusion somewhere less visible.
//
// Explicit rather than derived from Radius/Height: those are a Doom collision
// cylinder in map units, which is neither the shape nor the scale wanted.
static void PhysicsEnable(AActor *self, double massKg, double hx, double hy, double hz,
	double comX, double comY, double comZ)
{
	if (self == nullptr) return;

	P_PhysicsRemoveBody(self);

	PhysBody b;
	b.owner = self;
	b.comOffset = FVector3((float)comX, (float)comY, (float)comZ);
	b.rot = Quat::FromEulerDeg(self->Angles.Yaw.Degrees(), self->Angles.Pitch.Degrees(), self->Angles.Roll.Degrees());

	// The solver's position is the CENTRE OF MASS, not the actor's origin.
	const FVector3 originPos(MapToM(self->X()), MapToM(self->Y()), MapToM(self->Z()));
	b.pos = originPos + b.rot.Rotate(b.comOffset);

	b.vel = FVector3(MapToM(self->Vel.X * TICRATE), MapToM(self->Vel.Y * TICRATE), MapToM(self->Vel.Z * TICRATE));
	b.angVel = FVector3(0, 0, 0);

	if (hx < 0.002) hx = 0.002;
	if (hy < 0.002) hy = 0.002;
	if (hz < 0.002) hz = 0.002;
	b.half = FVector3((float)hx, (float)hy, (float)hz);
	// No real geometry supplied, so the shape is the box -- one hull, and the
	// solver treats it exactly as it treated a box before hulls existed.
	b.hulls.Clear();
	b.ShapeFinish();

	if (massKg < 0.001) massKg = 0.001;
	b.invMass = (float)(1.0 / massKg);

	// Solid box inertia, then inverted. Diagonal in body space.
	const double w = 2 * hx, h = 2 * hy, d = 2 * hz;
	const double ix = massKg * (h*h + d*d) / 12.0;
	const double iy = massKg * (w*w + d*d) / 12.0;
	const double iz = massKg * (w*w + h*h) / 12.0;
	b.invInertia = FVector3(
		(float)(ix > 1e-9 ? 1.0 / ix : 0.0),
		(float)(iy > 1e-9 ? 1.0 / iy : 0.0),
		(float)(iz > 1e-9 ? 1.0 / iz : 0.0));

	self->flags9 |= MF9_PHYSICSBODY;
	self->renderflags |= RF_DONTINTERPOLATE;

	g_bodies.Push(b);

	if (vr_physics_debug)
	{
		Printf("[PHYS] enable %s mass=%.3fkg half=(%.3f %.3f %.3f)m at map(%.1f %.1f %.1f)\n",
			self->GetClass()->TypeName.GetChars(), massKg, hx, hy, hz,
			self->X(), self->Y(), self->Z());
	}
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsEnable, PhysicsEnable)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(massKg);
	PARAM_FLOAT(hx);
	PARAM_FLOAT(hy);
	PARAM_FLOAT(hz);
	PARAM_FLOAT(comX);
	PARAM_FLOAT(comY);
	PARAM_FLOAT(comZ);
	PhysicsEnable(self, massKg, hx, hy, hz, comX, comY, comZ);
	return 0;
}

static void PhysicsDisable(AActor *self)
{
	if (self == nullptr) return;
	P_PhysicsRemoveBody(self);
	self->flags9 &= ~MF9_PHYSICSBODY;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsDisable, PhysicsDisable)
{
	PARAM_SELF_PROLOGUE(AActor);
	PhysicsDisable(self);
	return 0;
}

// Impulse in kg*m/s, applied at the centre of mass. This is how a throw gets
// its speed and how a drop inherits the weapon's motion.
static void PhysicsAddImpulse(AActor *self, double x, double y, double z)
{
	if (self == nullptr) return;
	PhysBody *b = FindBody(self);
	if (b == nullptr) return;
	b->vel += FVector3((float)x, (float)y, (float)z) * b->invMass;
	b->asleep = false;
	b->sleepTimer = 0.f;
	b->restReported = false;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsAddImpulse, PhysicsAddImpulse)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	PhysicsAddImpulse(self, x, y, z);
	return 0;
}

// Spin, in radians per second. A magazine dropped from a moving hand tumbles;
// one placed gently does not.
static void PhysicsAddSpin(AActor *self, double x, double y, double z)
{
	if (self == nullptr) return;
	PhysBody *b = FindBody(self);
	if (b == nullptr) return;
	b->angVel += FVector3((float)x, (float)y, (float)z);
	b->asleep = false;
	b->sleepTimer = 0.f;
	b->restReported = false;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsAddSpin, PhysicsAddSpin)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(x);
	PARAM_FLOAT(y);
	PARAM_FLOAT(z);
	PhysicsAddSpin(self, x, y, z);
	return 0;
}

// What this object sounds like when it hits something. minSpeed is in metres
// per second -- below it, contacts are silent, which is what stops a settling
// object from chattering.
static void PhysicsSetImpactSound(AActor *self, int soundid, double minSpeed)
{
	if (self == nullptr) return;
	PhysBody *b = FindBody(self);
	if (b == nullptr) return;
	b->impactSound = FSoundID::fromInt(soundid);
	b->impactMinSpeed = (minSpeed > 0.0) ? (float)minSpeed : 0.6f;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsSetImpactSound, PhysicsSetImpactSound)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_SOUND(snd);
	PARAM_FLOAT(minSpeed);
	PhysicsSetImpactSound(self, snd.index(), minSpeed);
	return 0;
}

// Hold this body, or let it go.
//
// While held the solver does not touch it -- position it with
// PhysicsSetTransform every tic. Letting go leaves whatever velocity it was
// last given, so a throw is: hold, move, release with an impulse.
static void PhysicsSetHeld(AActor *self, int held)
{
	if (self == nullptr) return;
	PhysBody *b = FindBody(self);
	if (b == nullptr) return;

	b->kinematic = (held != 0);
	if (b->kinematic)
	{
		b->vel = FVector3(0, 0, 0);
		b->angVel = FVector3(0, 0, 0);
	}
	b->asleep = false;
	b->sleepTimer = 0.f;
	b->supportTimer = 0.f;
	b->restReported = false;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsSetHeld, PhysicsSetHeld)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_BOOL(held);
	PhysicsSetHeld(self, held ? 1 : 0);
	return 0;
}

// Place a held body. Position in MAP units (so it can be fed straight from a
// controller position), angles in degrees.
static void PhysicsSetTransform(AActor *self, double x, double y, double z,
	double yaw, double pitch, double roll)
{
	if (self == nullptr) return;
	PhysBody *b = FindBody(self);
	if (b == nullptr) return;

	b->rot = Quat::FromEulerDeg(yaw, pitch, roll);
	b->pos = FVector3(MapToM(x), MapToM(y), MapToM(z)) + b->rot.Rotate(b->comOffset);
	b->asleep = false;
	b->sleepTimer = 0.f;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsSetTransform, PhysicsSetTransform)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	PARAM_FLOAT(yaw); PARAM_FLOAT(pitch); PARAM_FLOAT(roll);
	PhysicsSetTransform(self, x, y, z, yaw, pitch, roll);
	return 0;
}

// Take hold of this body with a hand (0 = main, 1 = off).
//
// The pose it currently has RELATIVE TO THE HAND is captured and maintained, so
// picking something up does not snap it into a canonical grip -- you hold it
// however you happened to grab it, which is what makes reaching for a specific
// end of a thing meaningful.
static void PhysicsGrab(AActor *self, int hand)
{
	if (self == nullptr) return;
	if (hand < 0 || hand > 1) return;

	PhysBody *b = FindBody(self);
	if (b == nullptr) return;

	const PhysBody *h = nullptr;
	for (unsigned i = 0; i < g_bodies.Size(); i++)
		if (g_bodies[i].handIndex == hand) { h = &g_bodies[i]; break; }
	if (h == nullptr) return;      // hands disabled

	// Offsets in the hand's frame.
	b->grabPosOffset = h->rot.Inverse().Rotate(b->pos - h->pos);

	Quat inv = h->rot.Inverse();
	Quat rel;
	rel.w = inv.w*b->rot.w - inv.x*b->rot.x - inv.y*b->rot.y - inv.z*b->rot.z;
	rel.x = inv.w*b->rot.x + inv.x*b->rot.w + inv.y*b->rot.z - inv.z*b->rot.y;
	rel.y = inv.w*b->rot.y - inv.x*b->rot.z + inv.y*b->rot.w + inv.z*b->rot.x;
	rel.z = inv.w*b->rot.z + inv.x*b->rot.y - inv.y*b->rot.x + inv.z*b->rot.w;
	rel.Normalize();
	b->grabRotOffset = rel;

	b->heldByHand = hand;
	b->kinematic = true;
	b->asleep = false;
	b->sleepTimer = 0.f;
	b->restReported = false;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsGrab, PhysicsGrab)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_INT(hand);
	PhysicsGrab(self, hand);
	return 0;
}

// Let go. Velocity is already correct -- a held body has been inheriting the
// hand's motion every step -- so a throw needs no separate calculation and
// cannot disagree with what the hand was actually doing.
static void PhysicsRelease(AActor *self)
{
	if (self == nullptr) return;
	PhysBody *b = FindBody(self);
	if (b == nullptr) return;

	const int hand = b->heldByHand;

	b->heldByHand = -1;
	b->kinematic = false;
	b->asleep = false;
	b->sleepTimer = 0.f;
	b->sleepRefPos = b->pos;
	b->sleepRefRot = b->rot;

	// THE THROW USES THE FASTEST RECENT MOMENT, NOT THIS ONE.
	//
	// A person lets go at the end of a throwing motion, by which point the arm
	// is already decelerating -- often sharply. Taking the velocity at the
	// instant the button is released therefore captures the slowdown rather
	// than the throw, and the object leaves far slower than it felt, dropping
	// almost straight down instead of arcing. It reads as gravity grabbing it
	// the moment you let go.
	//
	// Scanning back over the last ~180ms and taking the peak recovers the
	// throw the hand actually made.
	if (hand >= 0 && hand <= 1)
	{
		const int have = g_handHistFilled[hand] ? kHandHistory : g_handHistPos[hand];

		// Where the object sits relative to the hand -- a spinning wrist throws
		// it along a tangent from here.
		const PhysBody *h = nullptr;
		for (unsigned i = 0; i < g_bodies.Size(); i++)
			if (g_bodies[i].handIndex == hand) { h = &g_bodies[i]; break; }
		const FVector3 r = (h != nullptr) ? (b->pos - h->pos) : FVector3(0, 0, 0);
		const float spinF = *vr_physics_throwspin;

		// Peak measured on the OBJECT's speed, not the hand centre's.
		//
		// These differ whenever the wrist is turning, which during a throw is
		// always. Picking the fastest moment of the HAND and then adding
		// whatever rotation happened to be present at that instant chooses a
		// moment on one basis and a direction on another -- so a throw could
		// leave in a direction that was never the fastest thing the object did,
		// and it read as throws not going where they were aimed.
		const HandSample *best = nullptr;
		float bestSpeed = -1.f;
		FVector3 bestVel(0, 0, 0);

		for (int i = 0; i < have; i++)
		{
			const HandSample &s = g_handHist[hand][i];
			const FVector3 v = s.vel + Cross(s.angVel * spinF, r);
			const float sp = v.Length();
			if (sp > bestSpeed) { bestSpeed = sp; best = &s; bestVel = v; }
		}

		// Only if it genuinely beats what the hand is doing now -- setting
		// something down gently must stay gentle.
		if (best != nullptr && bestSpeed > b->vel.Length())
		{
			b->vel = bestVel;
			b->angVel = best->angVel;
		}
	}

	// A tracking spike can report an absurd single-frame lunge; without a cap
	// the object simply leaves the map.
	const float cap = 14.f;   // m/s, well past a human throw
	const float s = b->vel.Length();
	if (s > cap) b->vel *= cap / s;

	// The same for SPIN, which is worse because it is derived from a single
	// frame's rotation: a quick flick of the wrist -- or one jittery tracking
	// sample -- produced a measured 77 rad/s, twelve revolutions a second, and
	// the object left the hand spinning like a drill bit. A hard throw really
	// does impart a fast tumble, so this is set well above anything a wrist can
	// actually do rather than tuned for looks.
	const float spinCap = 25.f;   // rad/s, ~4 turns a second
	const float w = b->angVel.Length();
	if (w > spinCap) b->angVel *= spinCap / w;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsRelease, PhysicsRelease)
{
	PARAM_SELF_PROLOGUE(AActor);
	PhysicsRelease(self);
	return 0;
}

static int PhysicsIsHeld(AActor *self)
{
	PhysBody *b = FindBody(self);
	return (b != nullptr && b->heldByHand >= 0) ? 1 : 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsIsHeld, PhysicsIsHeld)
{
	PARAM_SELF_PROLOGUE(AActor);
	ACTION_RETURN_BOOL(PhysicsIsHeld(self) != 0);
}

// How far this body's SURFACE is from a point, in METRES -- not the distance
// to the actor's origin. Reaching for a long object should succeed when you
// touch its end, not only when you touch its middle.
//
// Measured against the real hulls now. For a convex hull the distance to a
// point outside it is the largest of its signed face distances, which is exact
// for a point facing a face and a slight underestimate out past an edge or a
// corner. That error is well under a millimetre at these sizes and it errs
// toward being generous about a grab, which is the right way to be wrong when
// a hand is reaching for something.
static double PhysicsDistanceTo(AActor *self, double x, double y, double z)
{
	PhysBody *b = FindBody(self);
	if (b == nullptr) return 1e9;

	const FVector3 p(MapToM(x), MapToM(y), MapToM(z));
	FVector3 local = b->rot.Inverse().Rotate(p - b->pos);

	// Nearest over the set: a compound shape is as close as its closest piece.
	double best = 1e9;
	for (unsigned hi = 0; hi < b->hulls.Size(); hi++)
	{
		const PhysHull &h = b->hulls[hi];
		if (h.planes.Size() == 0) continue;

		float outside = -FLT_MAX;
		for (unsigned i = 0; i < h.planes.Size(); i++)
		{
			const FVector4 &pl = h.planes[i];
			const float d = (local.X * pl.X + local.Y * pl.Y + local.Z * pl.Z) - pl.W;
			if (d > outside) outside = d;
		}
		const double d = (outside < 0.f) ? 0.0 : (double)outside;   // inside = touching
		if (d < best) best = d;
	}
	return (best > 1e8) ? 1e9 : best;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsDistanceTo, PhysicsDistanceTo)
{
	PARAM_SELF_PROLOGUE(AActor);
	PARAM_FLOAT(x); PARAM_FLOAT(y); PARAM_FLOAT(z);
	ACTION_RETURN_FLOAT(PhysicsDistanceTo(self, x, y, z));
}

static int PhysicsIsAsleep(AActor *self)
{
	PhysBody *b = FindBody(self);
	return (b != nullptr && b->asleep) ? 1 : 0;
}

DEFINE_ACTION_FUNCTION_NATIVE(AActor, PhysicsIsAsleep, PhysicsIsAsleep)
{
	PARAM_SELF_PROLOGUE(AActor);
	ACTION_RETURN_BOOL(PhysicsIsAsleep(self) != 0);
}
