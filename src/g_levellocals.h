/*
** g_levellocals.h
** The static data for a level
**
**---------------------------------------------------------------------------
** Copyright 1998-2016 Randy Heit
** Copyright 2005-2017 Christoph Oelckers
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#pragma once

#include "doomdata.h"
#include "g_level.h"
#include "r_defs.h"
#include "r_sky.h"
#include "portal.h"
#include "p_blockmap.h"
#include "p_local.h"
#include "po_man.h"
#include "p_acs.h"
#include "p_tags.h"
#include "p_spec.h"
#include "actor.h"
#include "b_bot.h"
#include "p_effect.h"
#include "d_player.h"
#include "p_destructible.h"
#include "r_data/r_sections.h"
#include "r_data/r_canvastexture.h"
#include "r_data/r_interpolate.h"
#include "doom_aabbtree.h"
#include "doom_levelmesh.h"
#include "p_visualthinker.h"

//============================================================================
//
// This is used to mark processed portals for some collection functions.
//
//============================================================================

struct FPortalBits
{
	TArray<uint32_t> data;

	void setSize(int num)
	{
		data.Resize((num + 31) / 32);
		clear();
	}

	void clear()
	{
		memset(&data[0], 0, data.Size() * sizeof(uint32_t));
	}

	void setBit(int group)
	{
		data[group >> 5] |= (1 << (group & 31));
	}

	int getBit(int group)
	{
		return data[group >> 5] & (1 << (group & 31));
	}
};

class DACSThinker;
class DFraggleThinker;
class DSpotState;
class DSeqNode;
struct FStrifeDialogueNode;
class DAutomapBase;
struct wbstartstruct_t;
class DSectorMarker;
struct FTranslator;
struct EventManager;

typedef TMap<int, int> FDialogueIDMap;				// maps dialogue IDs to dialogue array index (for ACS)
typedef TMap<FName, int> FDialogueMap;				// maps actor class names to dialogue array index
typedef TMap<int, FUDMFKeys> FUDMFKeyMap;
class DIntermissionController;

// [BB] What a billboard draws. The shader owns the shapes; what a payload
// number MEANS in a given card or readout is a mod/ZScript decision.
enum EBillboardPayload
{
	BB_PANEL   = 0,  // rounded-rect backing; data byte0 = corner radius, byte1 = border width
	BB_TEXTURE = 1,  // arbitrary TextureID on the quad; data = TextureID.GetIndex()
	BB_DIGITS  = 2,  // one integer, printed; data = the value
	BB_GLYPH   = 3,  // one character; data = its code
	BB_RING    = 4,  // progress ring; data = progress (low byte, 0-255) | style bits above
	BB_BAR     = 5,  // progress bar; data = progress (low byte, 0-255) | style bits above
	BB_TEXT    = 6,  // arbitrary string; reads FBillboard::text, ignores data
	BB_SEGMENT = 7,  // same string, drawn as a 16-segment display -- no atlas
	BB_SEGLCD  = 8,  // as BB_SEGMENT but inverted: lit plate, digits punched out
	BB_SEAM    = 9,  // a glowing slit; widen it with ResizeBillboard to open it
	// [BB] GITD's kill badge, transcribed rather than approximated. ONE quad:
	// the lozenge plate and its digits are drawn in a single pass, which is
	// what lets the digits punch to black out of the plate. data = the number.
	// Drive `progress` to open it. Digits only; letters are BB_SEGMENT.
	BB_WG13    = 10,
};

// [BB] How a billboard decides which way it points. Facing is a MODE, not
// the definition of the primitive: a quad that always turns to the camera
// cannot be hinged to another at a fixed angle, because once both turn
// independently the angle between them stops meaning anything and a hinged
// assembly collapses into parallel planes. Hinge solving stays in ZScript;
// the engine only ever consumes the yaw/tilt it is handed.
enum EBillboardFacing
{
	BBF_FIXED      = 0,  // use my own yaw/tilt verbatim
	BBF_CAMERAYAW  = 1,  // turn to the viewer, stay upright (tilt preserved)
	BBF_CAMERA     = 2,  // turn to the viewer including tilt
};

// [BB] Billboard flag bits.
enum EBillboardFlags
{
	BBFL_PERSISTENT = 1,  // lives until RemoveBillboard(); ignores lifetime
	BBFL_ATTACHED   = 2,  // follows attachedTo; dies when that actor does
	BBFL_NODEPTH    = 4,  // skip depth test; draws over world geometry
	BBFL_VIEWLOCKED = 8,  // pos is an offset from the viewer, not a world point
	BBFL_FOLLOWANGLE = 16, // attached only: yaw is relative to the actor's facing, so faces turn with it

	// [BB] Decoration. Drawn like anything else, but never returned by
	// AimBillboard/TouchBillboard/SweepBillboard.
	//
	// A composed panel is not one quad -- it is forty small ones, a bar's
	// track and fill and every glyph of every label, each a billboard with
	// its own handle. The queries return the NEAREST hit, so without this
	// flag the panel's own face is permanently masked by the text drawn on
	// it: a pointer aimed at a row comes back holding the handle of a
	// letter, and no caller can map that to a row. Flag the decoration and
	// the one quad that means something is the one that answers.
	BBFL_NOHIT      = 32,

	// [BB] BB_SEAM only: the opening is a HOLE, not a lit panel. Dark
	// interior, bright rim. Without it a seam is a glowing slab and anything
	// stepping out of it reads as standing in front of a light rather than
	// emerging from somewhere.
	BBFL_VOID       = 64,
};

// [BB] A world-anchored quad: real depth-tested geometry, not a HUD overlay
// and not a surface-shader term. Extent is per-axis (width/height) rather
// than one radius because these back rectangular panels, and orientation is
// stored in design space -- mYaw is which way the face points, mTilt is how
// far the top leans, 0 being vertical. Converting that to whatever the
// renderer wants is the draw path's job, not the caller's.
//
// Lifetime is one of three: transient (expires by lifetime), persistent
// (until removed), or attached (until its actor dies). See
// FLevelLocals::TickBillboards().
struct FBillboard
{
	int      id = 0;               // handle for Update/Move/Remove; 0 = transient, no handle issued
	DVector3 pos;                  // world position; recomputed each tic while attached
	double   width = 32.0;
	double   height = 32.0;
	double   yaw = 0.0;            // design space: which way the face points
	double   tilt = 0.0;           // design space: 0 = vertical, + leans the top toward the viewer
	int      facing = BBF_FIXED;   // EBillboardFacing
	int      payload = 0;          // EBillboardPayload
	int      data = 0;             // payload-specific packed int

	// [BB] Which typeface, for the distance-field payloads. 0 is the default
	// face and is what every existing call site gets by not setting this;
	// 1..N index the rolled roster. See FSDFFontRoster in hw_sdffont.h --
	// the roster is SHUFFLED PER GAME, so a slot names a ROLE ("the display
	// face") and is never a promise about which typeface answers to it.
	int      font = 0;

	// [BB] BB_TEXT's string, and only BB_TEXT's.
	//
	// It lives here rather than inside `data` because `data` is one int and
	// text is not a number. BB_DIGITS works by packing a value into that int,
	// and there is no equivalent trick for a string: 0-9 plus A-Z is 36
	// symbols, six bits each, and an int safely carries four characters before
	// it runs out. "B0002" is five and "CG B0001" is eight, so the packing
	// cannot represent the names this is for. Empty on every other payload,
	// which costs one empty FString per billboard and nothing else.
	FString  text;

	// [BB] Neon, for the payloads drawn from a distance field.
	//
	// glowRadius is how far past the letter's edge the halo reaches, as a
	// fraction of the atlas's spread: 1.0 uses the whole field, 0 is off.
	// It CANNOT usefully exceed 1 -- past the spread there is no field left to
	// read and the halo clips to a hard square at the glyph's cell boundary.
	// Measured before the shader existed, in tools/sdffont/sdfpreview.ps1.
	//
	// Ignored by every payload that is not distance-field text, because a
	// glow needs an edge to fall away from and a plain quad has no such thing.
	// [BB] How far through its reveal a payload is, 0..1. THE ANIMATED HALF OF
	// GITD's wgType 13 lived here and was the point of it: its plate is a thin
	// slit that opens vertically into a full ellipse, and the number only
	// appears once it is more than half open. Drawing the end state and
	// skipping the reveal throws away the effect.
	//
	// 1.0 by default, so anything that never sets it draws fully formed.
	double   progress = 1.0;

	double   glowRadius = 0.0;
	double   glowStrength = 0.0;   // 0 = off, 1 = halo as bright as the core

	PalEntry color;

	// [BB] Second colour, for payloads that draw a gradient. Alpha 0 means
	// "no gradient" and the payload uses `color` flat -- which is why this
	// defaults to 0 rather than to white: a white second colour would wash
	// every existing billboard the moment the field appeared.
	PalEntry color2 = 0;

	double   alpha = 1.0;          // 0 = invisible, 1 = opaque; the fade handle
	int      flags = 0;            // EBillboardFlags
	double   lifetime = 0.0;       // seconds; <= 0 = permanent. Moot once persistent/attached.
	int      spawntic = 0;         // level.maptime at creation, for transient expiry

	// [BB] Which group's transform this rides, 0 for none. See
	// FBillboardGroup -- the short version is that a composed panel is forty
	// quads and scaling it is one number, not eighty setter calls.
	int      group = 0;

	// A raw AActor* would dangle across a GC sweep. TObjPtr does not, and
	// does not itself keep the actor alive -- "attached billboards die with
	// their actor" means exactly that: once this resolves null the billboard
	// is dropped, never the other way round.
	TObjPtr<AActor*> attachedTo;
	DVector3 attachOffset;

	// Where this actually ended up last time it was drawn. View-locked
	// billboards have no fixed world position -- theirs is resolved per
	// frame against the interpolated viewpoint -- so aiming and touching
	// have to test against what was drawn rather than against pos, or the
	// pointer would disagree with what the player sees. Written by the
	// renderer, read by the aim/touch queries; not serialized, since the
	// first frame after a load rewrites it.
	DVector3 drawPos;
};

// [BB] A SHARED TRANSFORM FOR A COMPOSED PANEL.
//
// A panel is not a quad, it is forty of them: a shell, a face, every rule,
// every glyph of every label. Scaling that as one object means scaling each
// member's SIZE and its OFFSET FROM THE PANEL'S CENTRE together -- shrink the
// quads without shrinking the gaps and you get forty tiny elements in the
// original layout, which is not a smaller panel, it is a broken one.
//
// Script could do that: ResizeBillboard and MoveBillboard both exist. It
// would be eighty calls per step, each an O(n) scan of the billboard array,
// and -- the part that actually matters -- it would step at 35Hz, because
// that is when script runs. A UI element scaling in twelve visible jumps in
// front of someone's face is worse than not animating it at all.
//
// So the transform lives here and resolves in the renderer, at frame rate,
// from a start tic and a duration. Script says "grow from 0 to 1 over ten
// tics" ONCE and never touches it again. Same argument BBFL_VIEWLOCKED makes
// for position: anything welded to the eye that updates at tic rate reads as
// lag, and in a headset lag reads as nausea.
//
// The origin is in the MEMBERS' OWN SPACE, whatever that is for them -- an
// offset from the viewer for BBFL_VIEWLOCKED, an offset from the actor for
// BBFL_ATTACHED, a world point otherwise. A group whose members do not all
// share a space is a caller error and draws as nonsense; there is no cheap
// way to detect it and no attempt is made.
struct FBillboardGroup
{
	int      id = 0;               // handle; 0 is never issued
	DVector3 origin;               // the point members scale about, in their own space

	// The animation, as a declaration rather than a state machine. durTics 0
	// means settled and the scale is simply `to`, which is also how a plain
	// SetBillboardGroupScale is stored.
	double   from = 1.0;
	double   to = 1.0;
	int      startTic = 0;         // level.maptime when it began
	int      durTics = 0;          // 0 = settled
};

// [BB] THE ONE TRUE BILLBOARD BASIS. Renderer, aim ray, touch test and sweep
// all resolve their orientation HERE and nowhere else.
//
// This used to be three hand-copied pairs of lines, and on 2026-08-08 all
// three were wrong in the same way at once: `right` was the viewer's LEFT, so
// every billboard texture drew mirrored and BB_DIGITS laid multi-digit numbers
// out backwards (120 read as 021). Correcting three copies is three chances to
// correct only two of them, and a pointer that lands somewhere other than
// where the panel draws is invisible until someone notices a row is clickable
// half a panel away from where it looks. So there is one copy now, and
// "all three must always agree" is structural rather than a comment.
//
//   face   F = ( cos y,  sin y, 0)
//   right  R = (-sin y,  cos y, 0)   the viewer's right
//   up     U tilts toward -F, so positive tilt leans the top toward the viewer
//   normal N = R x U, which reduces to F at zero tilt
//
// `bpos` is where the billboard actually is -- drawPos for a view-locked one,
// pos otherwise. `eye` is whatever the caller looks FROM: the viewpoint in the
// renderer, the ray origin or the touching point in a query. A pointer and a
// viewpoint are not the same thing in VR, but resolving a camera-facing
// billboard against the thing doing the asking is what a player expects.
//
// tiltBias and scale are the bb_tiltbias / bb_scale cvars, passed in rather
// than read here so this header stays free of cvar dependencies. EVERY caller
// must pass the live values: they are comfort dials that change what is DRAWN,
// so a query that ignored them would put the clickable region somewhere other
// than the picture. That was a real defect -- the queries used bb.width
// unscaled while the renderer scaled it, so raising bb_scale to make a panel
// readable grew the panel and left its new edges dead.
// yawBias is the VIEW-LOCKED half of the same idea as tiltBias, and it is not
// optional for BBFL_VIEWLOCKED. That flag resolves POSITION against the
// viewpoint; without this it left ORIENTATION in world space, so a head-locked
// panel followed you around the room while permanently facing world-east. You
// could walk around your own HUD. Off-axis it foreshortened until it was a
// third of its authored width, and from behind it drew its back -- which reads
// as every glyph mirrored, and cost an afternoon chasing bb_flipu.
//
// A BIAS rather than "face the camera", deliberately. BBF_CAMERAYAW makes each
// quad yaw about its OWN position, which bows a composed panel into a cylinder
// and breaks hinged assemblies. Adding the view yaw to the STORED yaw keeps
// every element's angle RELATIVE to every other, so a flat panel stays flat, a
// hinge stays hinged, and the whole assembly turns with the head as one rigid
// object -- which is what view-locked was always supposed to mean.
inline void BillboardBasis(const FBillboard &bb, const DVector3 &bpos, const DVector3 &eye,
	double tiltBias, double scale,
	DVector3 &right, DVector3 &up, DVector3 &normal, double &halfw, double &halfh,
	double yawBias = 0.0)
{
	const double DEG2RAD = 0.01745329251994329576923690768489;
	const double RAD2DEG = 57.29577951308232087679815481410517;

	double useYaw = bb.yaw + yawBias;
	double useTilt = bb.tilt;

	// An attached billboard can hold its yaw relative to the actor it rides,
	// so a thing that turns takes its faces with it instead of sliding around
	// still pointing whichever way it was born facing.
	if ((bb.flags & BBFL_FOLLOWANGLE) && (bb.flags & BBFL_ATTACHED) && bb.attachedTo != nullptr)
	{
		useYaw += bb.attachedTo->Angles.Yaw.Degrees();
	}

	if (bb.facing == BBF_CAMERAYAW || bb.facing == BBF_CAMERA)
	{
		double dx = eye.X - bpos.X;
		double dy = eye.Y - bpos.Y;
		useYaw = atan2(dy, dx) * RAD2DEG;

		if (bb.facing == BBF_CAMERA)
		{
			double dz = eye.Z - bpos.Z;
			useTilt = atan2(dz, sqrt(dx * dx + dy * dy)) * RAD2DEG;
		}
	}

	double yawRad = useYaw * DEG2RAD;
	double tiltRad = (useTilt + tiltBias) * DEG2RAD;
	double cy = cos(yawRad), sy = sin(yawRad);
	double ct = cos(tiltRad), st = sin(tiltRad);

	right = DVector3(-sy, cy, 0.0);
	up = DVector3(-cy * st, -sy * st, ct);
	normal = right ^ up;

	double g = scale > 0.01 ? scale : 0.01;
	halfw = bb.width * 0.5 * g;
	halfh = bb.height * 0.5 * g;
}

struct FLevelLocals
{
	void *level;
	void *Level;	// bug catchers.
	FLevelLocals();
	~FLevelLocals();

	void *operator new(size_t blocksize)
	{
		// Null the allocated memory before running the constructor.
		// If we later allocate secondary levels they need to behave exactly like a global variable, i.e. start nulled.
		auto block = ::operator new(blocksize);
		memset(block, 0, blocksize);
		return block;
	}


	friend class MapLoader;

	DIntermissionController* CreateIntermission();
	void Tick();
	void Mark();
	void AddScroller(int secnum);
	void SetInterMusic(const char *nextmap);
	void SetMusicVolume(float v);
	void ClearLevelData(bool fullgc = true);
	void ClearPortals();
	bool CheckIfExitIsGood(AActor *self, level_info_t *newmap);
	void FormatMapName(FString &mapname, const char *mapnamecolor);
	void ClearAllSubsectorLinks();
	void TranslateLineDef (line_t *ld, maplinedef_t *mld, int lineindexforid = -1);
	int TranslateSectorSpecial(int special);
	bool IsTIDUsed(int tid);
	int FindUniqueTID(int start_tid, int limit);
	int GetConversation(int conv_id);
	int GetConversation(FName classname);
	void SetConversation(int convid, PClassActor *Class, int dlgindex);
	int FindNode (const FStrifeDialogueNode *node);
    int GetInfighting();
	void SetCompatLineOnSide(bool state);
	int GetCompatibility(int mask);
	int GetCompatibility2(int mask);
	void ApplyCompatibility();
	void ApplyCompatibility2();
	AActor* SelectActorFromTID(int tid, size_t index, AActor* defactor);

	void Init();

private:
	bool ShouldDoIntermission(cluster_info_t* nextcluster, cluster_info_t* thiscluster);
	line_t *FindPortalDestination(line_t *src, int tag, int matchtype = -1);
	void BuildPortalBlockmap();
	void UpdatePortal(FLinePortal *port);
	void CollectLinkedPortals();
	void CreateLinkedPortals();
	bool ChangePortalLine(line_t *line, int destid);
	void AddDisplacementForPortal(FSectorPortal *portal);
	void AddDisplacementForPortal(FLinePortal *portal);
	bool ConnectPortalGroups();

	void SerializePlayers(FSerializer &arc, bool skipload);
	void CopyPlayer(player_t *dst, player_t *src, const char *name);
	void ReadOnePlayer(FSerializer &arc, bool fromHub);
	void ReadMultiplePlayers(FSerializer &arc, int numPlayers, bool fromHub);
	void SerializeSounds(FSerializer &arc);
	void PlayerSpawnPickClass (int playernum);

public:
	void SnapshotLevel();
	void UnSnapshotLevel(bool hubLoad);

	void FinalizePortals();
	bool ChangePortal(line_t *ln, int thisid, int destid);
	unsigned GetSkyboxPortal(AActor *actor);
	unsigned GetPortal(int type, int plane, sector_t *orgsec, sector_t *destsec, const DVector2 &displacement);
	unsigned GetStackPortal(AActor *point, int plane);
	DVector2 GetPortalOffsetPosition(double x, double y, double dx, double dy);
	bool CollectConnectedGroups(int startgroup, const DVector3 &position, double upperz, double checkradius, FPortalGroupArray &out);

	void ActivateInStasisPlat(int tag);
	bool CreateCeiling(sector_t *sec, DCeiling::ECeiling type, line_t *line, int tag, double speed, double speed2, double height, int crush, int silent, int change, DCeiling::ECrushMode hexencrush);
	void ActivateInStasisCeiling(int tag);
	bool CreateFloor(sector_t *sec, DFloor::EFloor floortype, line_t *line, double speed, double height, int crush, int change, bool hexencrush, bool hereticlower);
	void DoDeferedScripts();
	void AdjustPusher(int tag, int magnitude, int angle, bool wind);
	int Massacre(bool baddies = false, FName cls = NAME_None);
	AActor *SpawnMapThing(FMapThing *mthing, int position);
	AActor *SpawnMapThing(int index, FMapThing *mt, int position);
	AActor *SpawnPlayer(FPlayerStart *mthing, int playernum, int flags = 0);
	void StartLightning();
	void ForceLightning(int mode, FSoundID tempSound = NO_SOUND);
	void ClearDynamic3DFloorData();
	void WorldDone(void);
	void AirControlChanged();
	AActor *SelectTeleDest(int tid, int tag, bool norandom, bool isPlayer);
	bool AlignFlat(int linenum, int side, int fc);
	void ReplaceTextures(const char *fromname, const char *toname, int flags);

	bool EV_Thing_Spawn(int tid, AActor *source, int type, DAngle angle, bool fog, int newtid);
	bool EV_Thing_Move(int tid, AActor *source, int mapspot, bool fog);
	bool EV_Thing_Projectile(int tid, AActor *source, int type, const char *type_name, DAngle angle,
		double speed, double vspeed, int dest, AActor *forcedest, int gravity, int newtid, bool leadTarget);
	int EV_Thing_Damage(int tid, AActor *whofor0, int amount, FName type);

	bool EV_DoPlat(int tag, line_t *line, DPlat::EPlatType type, double height, double speed, int delay, int lip, int change);
	void EV_StopPlat(int tag, bool remove);
	bool EV_DoPillar(DPillar::EPillar type, line_t *line, int tag, double speed, double height, double height2, int crush, bool hexencrush);
	bool EV_DoDoor(DDoor::EVlDoor type, line_t *line, AActor *thing, int tag, double speed, int delay, int lock, int lightTag, bool boomgen = false, int topcountdown = 0);
	bool EV_SlidingDoor(line_t *line, AActor *thing, int tag, int speed, int delay, DAnimatedDoor::EADType type);
	bool EV_DoCeiling(DCeiling::ECeiling type, line_t *line, int tag, double speed, double speed2, double height, int crush, int silent, int change, DCeiling::ECrushMode hexencrush = DCeiling::ECrushMode::crushDoom);
	bool EV_CeilingCrushStop(int tag, bool remove);
	bool EV_StopCeiling(int tag, line_t *line);
	bool EV_BuildStairs(int tag, DFloor::EStair type, line_t *line, double stairsize, double speed, int delay, int reset, int igntxt, int usespecials);
	bool EV_DoFloor(DFloor::EFloor floortype, line_t *line, int tag, double speed, double height, int crush, int change, bool hexencrush, bool hereticlower = false);
	bool EV_FloorCrushStop(int tag, line_t *line);
	bool EV_StopFloor(int tag, line_t *line);
	bool EV_DoDonut(int tag, line_t *line, double pillarspeed, double slimespeed);
	bool EV_DoElevator(line_t *line, DElevator::EElevator type, double speed, double height, int tag);
	bool EV_StartWaggle(int tag, line_t *line, int height, int speed, int offset, int timer, bool ceiling);
	bool EV_DoChange(line_t *line, EChange changetype, int tag);

	void EV_StartLightFlickering(int tag, int upper, int lower);
	void EV_StartLightStrobing(int tag, int upper, int lower, int utics, int ltics);
	void EV_StartLightStrobing(int tag, int utics, int ltics);
	void EV_TurnTagLightsOff(int tag);
	void EV_LightTurnOn(int tag, int bright);
	void EV_LightTurnOnPartway(int tag, double frac);
	void EV_LightChange(int tag, int value);
	void EV_StartLightGlowing(int tag, int upper, int lower, int tics);
	void EV_StartLightFading(int tag, int value, int tics);
	void EV_StopLightEffect(int tag);

	bool EV_Teleport(int tid, int tag, line_t *line, int side, AActor *thing, int flags);
	bool EV_SilentLineTeleport(line_t *line, int side, AActor *thing, int id, INTBOOL reverse);
	bool EV_TeleportOther(int other_tid, int dest_tid, bool fog);
	bool EV_TeleportGroup(int group_tid, AActor *victim, int source_tid, int dest_tid, bool moveSource, bool fog);
	bool EV_TeleportSector(int tag, int source_tid, int dest_tid, bool fog, int group_tid);

	void RecalculateDrawnSubsectors();
	FSerializer &SerializeSubsectors(FSerializer &arc, const char *key);
	void SpawnExtraPlayers();
	void Serialize(FSerializer &arc, bool hubload);
	DThinker *FirstThinker (int statnum);

	// g_Game
	void PlayerReborn (int player);
	bool CheckSpot (int playernum, FPlayerStart *mthing);
	void DoReborn (int playernum, bool force = false);
	void QueueBody (AActor *body);
	double PlayersRangeFromSpot (FPlayerStart *spot);
	FPlayerStart *SelectFarthestDeathmatchSpot (size_t selections);
	FPlayerStart *SelectRandomDeathmatchSpot (int playernum, unsigned int selections);
	void DeathMatchSpawnPlayer (int playernum);
	FPlayerStart *PickPlayerStart(int playernum, int flags = 0);
	bool DoCompleted(FString nextlevel, wbstartstruct_t &wminfo);
	void StartTravel();
	int FinishTravel();
	void ChangeLevel(const char *levelname, int position, int flags, int nextSkill = -1);
	const char *GetSecretExitMap();
	void ExitLevel(int position, bool keepFacing);
	void SecretExitLevel(int position);
	void DoLoadLevel(const FString &nextmapname, int position, bool autosave, bool newGame);

	void DeleteAllAttachedLights();
	void RecreateAllAttachedLights();


private:
	// Work data for CollectConnectedGroups.
	FPortalBits processMask;
	TArray<FLinePortal*> foundPortals;
	TArray<int> groupsToCheck;

public:

	FSectorTagIterator GetSectorTagIterator(int tag)
	{
		return FSectorTagIterator(tagManager, tag);
	}
	FSectorTagIterator GetSectorTagIterator(int tag, line_t *line)
	{
		return FSectorTagIterator(tagManager, tag, line);
	}
	FLineIdIterator GetLineIdIterator(int tag)
	{
		return FLineIdIterator(tagManager, tag);
	}
	template<class T> TThinkerIterator<T> GetThinkerIterator(FName subtype = NAME_None, int statnum = MAX_STATNUM+1)
	{
		if (subtype == NAME_None) return TThinkerIterator<T>(this, statnum);
		else return TThinkerIterator<T>(this, subtype, statnum);
	}
	template<class T> TThinkerIterator<T> GetThinkerIterator(FName subtype, int statnum, AActor *prev)
	{
		return TThinkerIterator<T>(this, subtype, statnum, prev);
	}
	FActorIterator GetActorIterator(int tid)
	{
		return FActorIterator(TIDHash, tid);
	}
	FActorIterator GetActorIterator(int tid, AActor *start)
	{
		return FActorIterator(TIDHash, tid, start);
	}
	NActorIterator GetActorIterator(FName type, int tid)
	{
		return NActorIterator(TIDHash, type, tid);
	}
	AActor *SingleActorFromTID(int tid, AActor *defactor)
	{
		return tid == 0 ? defactor : GetActorIterator(tid).Next();
	}

	bool SectorHasTags(sector_t *sector)
	{
		return tagManager.SectorHasTags(sector);
	}
	bool SectorHasTag(sector_t *sector, int tag)
	{
		return tagManager.SectorHasTag(sector, tag);
	}
	bool SectorHasTag(int sector, int tag)
	{
		return tagManager.SectorHasTag(sector, tag);
	}
	int GetFirstSectorTag(const sector_t *sect) const
	{
		return tagManager.GetFirstSectorTag(sect);
	}
	int GetFirstSectorTag(int i) const
	{
		return tagManager.GetFirstSectorTag(i);
	}
	int GetFirstLineId(const line_t *sect) const
	{
		return tagManager.GetFirstLineID(sect);
	}

	bool LineHasId(int line, int tag)
	{
		return tagManager.LineHasID(line, tag);
	}
	bool LineHasId(line_t *line, int tag)
	{
		return tagManager.LineHasID(line, tag);
	}

	int FindFirstSectorFromTag(int tag)
	{
		auto it = GetSectorTagIterator(tag);
		return it.Next();
	}
	
	int FindFirstLineFromID(int tag)
	{
		auto it = GetLineIdIterator(tag);
		return it.Next();
	}

	int isFrozen()
	{
		return frozenstate;
	}

private:	// The engine should never ever access subsectors of the game nodes. This is only needed for actually implementing PointInSector.
	subsector_t *PointInSubsector(double x, double y);
public:
	sector_t *PointInSectorBuggy(double x, double y);
	subsector_t *PointInRenderSubsector (fixed_t x, fixed_t y);

	sector_t *PointInSector(const DVector2 &pos)
	{
		return PointInSubsector(pos.X, pos.Y)->sector;
	}

	sector_t* PointInSector(const DVector3& pos)
	{
		return PointInSubsector(pos.X, pos.Y)->sector;
	}

	sector_t *PointInSector(double x, double y)
	{
		return PointInSubsector(x, y)->sector;
	}

	subsector_t *PointInRenderSubsector (const DVector2 &pos)
	{
		return PointInRenderSubsector(FloatToFixed(pos.X), FloatToFixed(pos.Y));
	}

	subsector_t* PointInRenderSubsector(const DVector3& pos)
	{
		return PointInRenderSubsector(FloatToFixed(pos.X), FloatToFixed(pos.Y));
	}
	
	FPolyObj *GetPolyobj (int polyNum)
	{
		auto index = Polyobjects.FindEx([=](const auto &poly) { return poly.tag == polyNum; });
		return index == Polyobjects.Size()? nullptr : &Polyobjects[index];
	}


	void ClearTIDHashes ()
	{
		memset(TIDHash, 0, sizeof(TIDHash));
	}


	bool CheckReject(sector_t *s1, sector_t *s2)
	{
		if (rejectmatrix.Size() > 0)
		{
			int pnum = int(s1->Index()) * sectors.Size() + int(s2->Index());
			return !(rejectmatrix[pnum >> 3] & (1 << (pnum & 7)));
		}
		return true;
	}

	DThinker *CreateThinker(PClass *cls, int statnum = STAT_DEFAULT)
	{
		DThinker *thinker = static_cast<DThinker*>(cls->CreateNew());
		assert(thinker->IsKindOf(RUNTIME_CLASS(DThinker)));
		thinker->ObjectFlags |= OF_JustSpawned;
		Thinkers.Link(thinker, statnum);
		thinker->Level = this;
		return thinker;
	}

	template<typename T, typename... Args>
	T* CreateThinker(Args&&... args)
	{
		auto thinker = static_cast<T*>(CreateThinker(RUNTIME_CLASS(T), T::DEFAULT_STAT));
		thinker->Construct(std::forward<Args>(args)...);
		return thinker;
	}

	void SetMusic();

	TArray<vertex_t> vertexes;
	TArray<sector_t> sectors;
	TArray<extsector_t> extsectors; // container for non-trivial sector information. sector_t must be trivially copyable for *_fakeflat to work as intended.
	TArray<line_t*> linebuffer;	// contains the line lists for the sectors.
	TArray<subsector_t*> subsectorbuffer;	// contains the subsector lists for the sectors.
	TArray<line_t> lines;
	TArray<side_t> sides;
	TArray<seg_t *> segbuffer;	// contains the seg links for the sidedefs.
	TArray<seg_t> segs;
	TArray<subsector_t> subsectors;
	TArray<node_t> nodes;
	TArray<subsector_t> gamesubsectors;
	TArray<node_t> gamenodes;
	node_t *headgamenode;
	TArray<uint8_t> rejectmatrix;
	TArray<zone_t>	Zones;
	TArray<FPolyObj> Polyobjects;

	TArray<FSectorPortal> sectorPortals;
	TArray<FLinePortal> linePortals;

	// Lightmaps
	TArray<LightmapSurface> LMSurfaces;
	TArray<float> LMTexCoords;
	int LMTextureCount = 0;
	int LMTextureSize = 0;
	TArray<uint16_t> LMTextureData;
	TArray<LightProbe> LightProbes;
	int LPMinX = 0;
	int LPMinY = 0;
	int LPWidth = 0;
	int LPHeight = 0;
	static const int LPCellSize = 32;
	TArray<LightProbeCell> LPCells;

	// Portal information.
	FDisplacementTable Displacements;
	FPortalBlockmap PortalBlockmap;
	TArray<FLinePortal*> linkedPortals;	// only the linked portals, this is used to speed up looking for them in P_CollectConnectedGroups.
	TArray<FSectorPortalGroup *> portalGroups;
	TArray<FLinePortalSpan> linePortalSpans;
	FSectionContainer sections;
	FCanvasTextureInfo canvasTextureInfo;
	EventManager *localEventManager = nullptr;
	DoomLevelAABBTree* aabbTree = nullptr;
	DoomLevelMesh* levelMesh = nullptr;

	// [ZZ] Destructible geometry information
	TMap<int, FHealthGroup> healthGroups;

	FBlockmap blockmap;
	TArray<polyblock_t *> PolyBlockMap;
	FUDMFKeyMap UDMFKeys[4];

	// These are copies of the loaded map data that get used by the savegame code to skip unaltered fields
	// Without such a mechanism the savegame format would become too slow and large because more than 80-90% are normally still unaltered.
	TArray<sector_t>	loadsectors;
	TArray<line_t>	loadlines;
	TArray<side_t>	loadsides;

	// Maintain single and multi player starting spots.
	TArray<FPlayerStart> deathmatchstarts;
	FPlayerStart		playerstarts[MAXPLAYERS];
	TArray<FPlayerStart> AllPlayerStarts;

	FBehaviorContainer Behaviors;
	AActor *TIDHash[128];

	TArray<FStrifeDialogueNode *> StrifeDialogues;
	FDialogueIDMap DialogueRoots;
	FDialogueMap ClassRoots;
	FCajunMaster BotInfo;

	int ii_compatflags = 0;
	int ii_compatflags2 = 0;
	int ib_compatflags = 0;
	int i_compatflags = 0;
	int i_compatflags2 = 0;

	DSectorMarker *SectorMarker;

	uint8_t		md5[16];			// for savegame validation. If the MD5 does not match the savegame won't be loaded.
	int			time;			// time in the hub
	int			maptime;			// time in the map
	int			totaltime;		// time in the game
	int			starttime;
	int			partime;
	int			sucktime;
	uint32_t	spawnindex;

	level_info_t *info;
	int			cluster;
	int			clusterflags;
	int			levelnum;
	int			lumpnum;
	FString		LevelName;
	FString		MapName;			// the lump name (E1M1, MAP01, etc)
	FString		NextMap;			// go here when using the regular exit
	FString		NextSecretMap;		// map to go to when used secret exit
	FString		AuthorName;
	FString		F1Pic;
	FTranslator *Translator;
	EMapType	maptype;
	FTagManager tagManager;
	FInterpolator interpolator;

	uint64_t	ShaderStartTime = 0;	// tell the shader system when we started the level (forces a timer restart)

	static const int BODYQUESIZE = 32;
	TObjPtr<AActor*> bodyque[BODYQUESIZE];
	TObjPtr<DAutomapBase*> automap = MakeObjPtr<DAutomapBase*>(nullptr);
	int bodyqueslot;
	
	// For now this merely points to the global player array, but with this in place, access to this array can be moved over to the level.
	// As things progress each level needs to be able to point to different players, even if they are just null if the second level is merely a skybox or camera target.
	// But even if it got a real player, the level will not own it - the player merely links to the level.
	// This should also be made a real object eventually.
	player_t *Players[MAXPLAYERS];
	
	// This is to allow refactoring without refactoring the data right away.
	bool PlayerInGame(int pnum)
	{
		return playeringame[pnum];
	}
	
	// This needs to be done better, but for now it should be good enough.
	bool PlayerInGame(player_t *player)
	{
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			if (player == Players[i]) return PlayerInGame(i);
		}
		return false;
	}

	int PlayerNum(player_t *player)
	{
		for (int i = 0; i < MAXPLAYERS; i++)
		{
			if (player == Players[i]) return i;
		}
		return -1;
	}
	
	bool isPrimaryLevel() const
	{
		return true;
	}
	
	// Gets the console player without having the calling code be aware of the level's state.
	player_t *GetConsolePlayer() const
	{
		return isPrimaryLevel()? Players[consoleplayer] : nullptr;
	}
	
	bool isConsolePlayer(AActor *mo) const
	{
		auto p = GetConsolePlayer();
		if (!p) return false;
		return p->mo == mo;
	}

	bool isCamera(AActor *mo) const
	{
		auto p = GetConsolePlayer();
		if (!p) return false;
		return p->camera == mo;
	}

	bool MBF21Enabled() const
	{
		// The affected features only are a problem with Doom format maps - the flag should have no effect in Hexen and UDMF format.
		return !(i_compatflags2 & COMPATF2_NOMBF21) || maptype != MAPTYPE_DOOM;
	}

	int NumMapSections;

	uint32_t		flags;
	uint32_t		flags2;
	uint32_t		flags3;

	uint32_t		fadeto;					// The color the palette fades to (usually black)
	uint32_t		outsidefog;				// The fog for sectors with sky ceilings

	uint32_t		hazardcolor;			// what color strife hazard blends the screen color as
	uint32_t		hazardflash;			// what color strife hazard flashes the screen color as

	FString		LightningSound = "world/thunder";
	FString		Music;
	int			musicorder;
	int			cdtrack;
	unsigned int cdid;
	FTextureID	skytexture1;
	FTextureID	skytexture2;

	float		skyspeed1;				// Scrolling speed of sky textures, in pixels per ms
	float		skyspeed2;

	double		sky1pos, sky2pos;
	float		hw_sky1pos, hw_sky2pos;
	bool		skystretch;
	uint32_t	globalcolormap;

	int			total_secrets;
	int			found_secrets;

	int			total_items;
	int			found_items;

	int			total_monsters;
	int			killed_monsters;

	double      max_velocity;
	double      avg_velocity;

	double		gravity;
	double		aircontrol;
	double		airfriction;
	int			airsupply;
	int			DefaultEnvironment;		// Default sound environment.

	DSeqNode *SequenceListHead;

	// [RH] particle globals
	uint32_t			OldestParticle; // [MC] Oldest particle for replacing with SPF_REPLACE
	uint32_t			ActiveParticles;
	uint32_t			InactiveParticles;
	TArray<particle_t>	Particles;
	TArray<uint16_t>	ParticlesInSubsec;
	FThinkerCollection Thinkers;

	TArray<DVector2>	Scrolls;		// NULL if no DScrollers in this level

	int8_t		WallVertLight;			// Light diffs for vert/horiz walls
	int8_t		WallHorizLight;

	bool		FromSnapshot;			// The current map was restored from a snapshot
	bool		HasHeightSecs;			// true if some Transfer_Heights effects are present in the map. If this is false, some checks in the renderer can be shortcut.
	bool		HasDynamicLights;		// Another render optimization for maps with no lights at all.
	int		frozenstate;

	double		teamdamage;

	TArray<FString> savedModelFiles;

	// former OpenGL-exclusive properties that should also be usable by the true color software renderer.
	int fogdensity;
	int outsidefogdensity;
	int skyfog;

	FName		deathsequence;
	float		pixelstretch;
	float		MusicVolume;

	// Hardware render stuff that can either be set via CVAR or MAPINFO
	bool		brightfog;
	bool		lightadditivesurfaces;
	bool		notexturefill;
	int			ImpactDecalCount;

	FDynamicLight *lights;
	DVisualThinker* VisualThinkerHead = nullptr;

	// [BB] Billboards: world-anchored oriented quads backing the in-world
	// panel system. Set-and-forget, unlike anything rebuilt per tic.
	TArray<FBillboard> Billboards;
	int NextBillboardID = 1;
	void TickBillboards();

	// [BB] Shared transforms for composed panels. See FBillboardGroup.
	TArray<FBillboardGroup> BillboardGroups;
	int NextBillboardGroupID = 1;

	FBillboardGroup *FindBillboardGroupByID(int gid)
	{
		if (gid == 0) return nullptr;
		for (auto &g : BillboardGroups) if (g.id == gid) return &g;
		return nullptr;
	}

	// [BB] The group's scale RIGHT NOW, eased, at render resolution.
	//
	// ticFrac is the renderer's fraction through the current tic, so this
	// returns a different number on every drawn frame while an animation is
	// running -- which is the whole point of the group living in the engine
	// rather than in script.
	//
	// TWO CURVES, chosen by direction, because growing and collapsing are not
	// the same gesture. Growth overshoots slightly and settles: a panel that
	// arrives at exactly its final size and stops reads as a texture being
	// swapped in, whereas a few percent past and back reads as an object
	// arriving. A collapse does the opposite -- it accelerates away, because a
	// thing leaving should not linger and should certainly not bounce.
	//
	// Returns `to` and does no work at all once the animation is spent, so a
	// settled group costs one comparison per billboard per frame.
	double BillboardGroupScale(int gid, double ticFrac, DVector3 *origin = nullptr)
	{
		const FBillboardGroup *g = FindBillboardGroupByID(gid);
		if (!g) return 1.0;
		if (origin) *origin = g->origin;

		if (g->durTics <= 0) return g->to;

		double elapsed = (double(maptime) + ticFrac) - double(g->startTic);
		double t = elapsed / double(g->durTics);
		if (t <= 0.0) return g->from;
		if (t >= 1.0) return g->to;

		double e;
		if (g->to >= g->from)
		{
			// Ease-out back. c is the classic 1.70158 taken down to about a
			// third: full strength overshoots ~10% and on a panel a foot from
			// someone's eyes that is a wobble, not a flourish.
			const double c = 0.6;
			const double u = t - 1.0;
			e = 1.0 + (c + 1.0) * u * u * u + c * u * u;
		}
		else
		{
			e = t * t;		// ease-in quad: leaves faster than it arrived
		}

		double s = g->from + (g->to - g->from) * e;
		return s > 0.0 ? s : 0.0;
	}

	// [BB] Sweep: a thin band of light at a fixed distance from an origin,
	// measured in WORLD space and tested on every surface. Because the test
	// is world-space rather than per-surface, the band wraps continuously
	// across floor, wall and ceiling on its own -- a cylinder expanding from
	// a point slices all three at the same radius, and a plane travelling
	// down a corridor draws an unbroken rectangle around it.
	//
	// This is not a sector property and deliberately not one of the four
	// lanes: it is a single world-space overlay, so it costs one set of
	// uniforms per frame rather than anything per sector.
	//
	// mode: 0 off, 1 cylinder from origin, 2 plane along X, 3 plane along Y,
	//       4 sphere from origin
	// [BB] Volumetric beam -- a cone of visible light in the air, for a
	// flashlight. Published from script each tic and consumed by the
	// renderer, which resolves it into view space per eye.
	bool VolBeamActive = false;
	DVector3 VolBeamPos;
	DVector3 VolBeamDir;
	PalEntry VolBeamColor;
	double VolBeamInner = 10.0;    // degrees, full brightness inside this
	double VolBeamOuter = 25.0;    // degrees, faded to nothing by here
	double VolBeamLength = 1024.0;
	double VolBeamDensity = 1.0;
	double VolBeamFalloff = 1.5;
	double VolBeamDust = 0.0;      // 0 = clean beam, 1 = heavily mottled
	double VolBeamDustScale = 0.04;// higher = finer motes
	double VolBeamDustDrift = 0.0; // world units per second the dust settles

	// Up to eight bands travel at once, so a train of them can chase each
	// other with their own colours and spacing.
	static const int MAX_SWEEP_BANDS = 8;
	int SweepMode = 0;
	DVector3 SweepOrigin;
	int SweepCount = 0;
	double SweepRadius[MAX_SWEEP_BANDS] = {};     // where each band sits
	double SweepThickness[MAX_SWEEP_BANDS] = {};  // band width, map units
	double SweepSoftness[MAX_SWEEP_BANDS] = {};   // 1 linear, higher = tighter core
	PalEntry SweepColor[MAX_SWEEP_BANDS] = {};
	double SweepIntensity[MAX_SWEEP_BANDS] = {};
	double SweepTrail = 0;                        // wake length, signed
	// Per band, so eight sweeps need not agree about where the centre of the
	// world is. Seeded from SweepOrigin/SweepMode; overridden per band by
	// SetSweepBandAt. Shape 0 means the band is off.
	DVector3 SweepBandOrigin[MAX_SWEEP_BANDS];
	int SweepBandMode[MAX_SWEEP_BANDS] = {};
	// What each band does to the pixels it covers: 1 add, 2 lift, 3 crush.
	int SweepBandDraw[MAX_SWEEP_BANDS] = {};

	// [BB] WHAT IS *INSIDE* A BAND.
	//
	// A band knows, for every pixel it covers, both how strongly it covers it
	// AND where that pixel is in the world. It used to throw the second away
	// and blend one flat colour weighted by the first -- so a band could only
	// ever be a wash.
	//
	// But every shape that defines a distance also implies two TANGENT
	// coordinates, and a pattern is just a function of those two. A bar
	// sweeping a corridor has (height, across); a ring has (height, arc). So
	// the same code that draws a lattice in a hallway draws a cage on an
	// expanding cylinder, with no per-shape special casing beyond picking the
	// two axes.
	//
	// PER BAND: only the fill MODE, packed into the draw mode's spare bits --
	// see SetSweepBandDraw. That is what lets a train be a solid wall, then a
	// grid, then a band of travelling darkness.
	//
	// SHARED: the style. Spacing, width, softness, rotation and the rest are
	// frame-global, because putting them per band would mean another
	// vec4[8] in StreamData -- and that buffer's size divides 64KB into
	// MAX_STREAM_DATA draws, so it would cost draw batching in every frame of
	// the game to let band 3 have a different line width from band 4.
	int      SweepBandFill[MAX_SWEEP_BANDS] = {};
	double   SweepFillSpacingU = 64;   // 0 = no lines in this axis
	double   SweepFillSpacingV = 64;
	double   SweepFillWidth = 3;       // world units, so it does not shimmer
	double   SweepFillSoft = 1.5;      // hard laser vs glowing filament
	double   SweepFillRotate = 0;      // degrees, in the band's own plane
	double   SweepFillDrift = 0;       // pattern sliding as the band travels
	double   SweepFillMajor = 0;       // every Nth line emphasised; 0 = off
	double   SweepFillMajorBoost = 2;  // how much wider a major line is
	double   SweepFillJitter = 0;      // emitters, not a texture
	double   SweepFillFlicker = 0;     // individual lines dropping out
	double   SweepFillGrad = 0;        // fade along one axis
	int      SweepFillGradAxis = 0;    // 0 = along V (height), 1 = along U
	double   SweepFillGap = 0;         // how much of the band colour fills the
	                                   // gaps. 0 = only the lines are lit and
	                                   // you see the room between them, which
	                                   // is what reads as actual lasers.
	PalEntry SweepFillColor = 0xFFFFFF;
	// How strongly the lattice is drawn IN THE AIR inside the band, rather
	// than only on the surfaces the band lands on. 0 = the old behaviour.
	double   SweepFillAir = 0;

	// [BB] REAL BEAMS.
	//
	// A laser in Doom is usually a sprite, or a chain of puffs spawned close
	// enough together to read as a line. Both are fakes and both show it: the
	// sprite does not light anything, and the chain stitches, gaps at long
	// range, and costs an actor per segment.
	//
	// A beam is a SEGMENT, and the honest way to draw one is the same way a
	// sweep band is drawn -- light every pixel by its distance from the thing.
	// The only difference is which distance:
	//
	//   sweep band   distance from a POINT     length(p - origin)
	//   beam         distance from a SEGMENT   length(p - closest(a,b))
	//
	// Everything else follows for free, because it is per pixel in world
	// space: the beam wraps across floor, wall and ceiling as one unbroken
	// object, it is continuous at any length, and the surfaces near it
	// brighten because they ARE near it rather than because something also
	// spawned a dynamic light.
	//
	// Not a sweep band, though, and deliberately so: a band's radius is a
	// distance that grows, and a beam does not travel. It simply is.
	//
	// Eight, because that is enough for a weapon beam plus a tripwire grid,
	// and the per-fragment cost is eight cheap segment tests.
// [BB] SHAPES. Sixteen, not eight -- eight was the beam budget, chosen for a
	// system where every slot costs a segment solve per fragment. A shape is a
	// couple of ALU behind an early reject, so the old cap was being copied
	// rather than reasoned about.
	static const int MAX_SHAPES = 16;
	DVector3 ShapePos[MAX_SHAPES];
	double   ShapeSize[MAX_SHAPES] = {};      // 0 = the slot is free
	int      ShapeKind[MAX_SHAPES] = {};      // 0 off, see hw_viewpointuniforms.h
	int      ShapeOrient[MAX_SHAPES] = {};    // 0 floor, 1 wall, 2 any
	double   ShapeAngle[MAX_SHAPES] = {};
	double   ShapeThick[MAX_SHAPES] = {};
	double   ShapeSeam[MAX_SHAPES] = {};      // 0 closed, 1 fully split
	PalEntry ShapeColor[MAX_SHAPES] = {};
	double   ShapeIntensity[MAX_SHAPES] = {};
	// Lifetime, resolved at render rate so a seam opens smoothly rather than
	// in 35Hz steps -- same reason the disturbances resolve their age there.
	double   ShapeBirth[MAX_SHAPES] = {};
	double   ShapeLife[MAX_SHAPES] = {};       // 0 = it never expires
	double   ShapeGrow[MAX_SHAPES] = {};       // size added per second
	double   ShapeSeamRate[MAX_SHAPES] = {};   // seam opened per second

	double   ShapeSoft = 2.0;
	double   ShapeHeightFade = 24.0;
	double   ShapeReach = 0.0;
	PalEntry ShapeUnder = 0xffff2610;

	static const int MAX_BEAMS = 8;
	int      BeamCount = 0;
	DVector3 BeamStart[MAX_BEAMS];
	DVector3 BeamEnd[MAX_BEAMS];
	double   BeamThick[MAX_BEAMS] = {};   // the hot core, world units
	double   BeamSoft[MAX_BEAMS] = {};    // how far the halo reaches past it
	PalEntry BeamColor[MAX_BEAMS] = {};
	double   BeamIntensity[MAX_BEAMS] = {};
	double   BeamGlow = 0.35;             // halo strength relative to the core
	double   BeamFogScatter = 1.0;        // how much a beam lights fog it crosses

	// The beam seen IN THE AIR rather than the light it casts on surfaces.
	// 0 means it only lights what it touches, which is a spotlight; turn it up
	// and the beam is an object hanging in space. Depth-correct for free --
	// see BeamAirGlow in main.fp.
	double   BeamAirGlow = 1.0;
	double   BeamScrollSpeed = 6.0;       // energy travelling muzzle to impact
	double   BeamScrollDepth = 0.25;      // 0 = a smooth beam
	double   BeamTaper = 0.35;            // thinner at the muzzle than the hit
	double   BeamFlare = 1.5;             // brightness boost where it lands

	// [BB] GLOW WAVE -- the missing axis.
	//
	// A glow already varies per pixel VERTICALLY: the fragment's distance
	// from the plane is what makes coverage and falloff smooth up a wall.
	// Horizontally it could not vary at all, because reach arrives as one
	// number for the whole surface -- so a wall faded beautifully top to
	// bottom and had a dead straight top edge from one end to the other.
	//
	// This is a world-space wave that modulates that reach per fragment, so
	// the edge itself rises and falls along the surface. It can drive the
	// brightness and the two-colour boundary as well, which are the same
	// wave read into different terms and look nothing like each other.
	//
	// Measured with the SAME five shapes as the sweep, from its own origin,
	// so a wave along the floor and a band crossing the room can be given
	// the same shape and made to agree. Two systems that measure the world
	// differently can never be lined up; two that share one distance
	// function line up by construction.
	//
	// Scene-global, so unlike the sweep this does not ride StreamData -- it
	// goes in the viewpoint block, which is written a handful of times a
	// frame rather than once per draw. MAX_STREAM_DATA stays at 34.
	double GlowWaveLength = 0;      // world units per cycle; 0 = off
	double GlowWaveSpeed = 0;       // radians per second
	double GlowWaveSharp = 1;       // pow() on the crest; 1 = plain sine
	int    GlowWaveShape = 1;       // same vocabulary as SweepMode
	double GlowWaveReach = 0;       // how far the edge moves, 0-1
	double GlowWaveBright = 0;      // how much the brightness swings, 0-1
	double GlowWaveColour = 0;      // how far the near/far boundary slides
	double GlowWaveDetune = 0;      // second sine, offset from the first
	double GlowWaveSeed = 0;        // per-room phase scatter, 0 = all as one
	double GlowWavePhase[4] = {};   // wall top, wall bottom, floor, ceiling
	DVector3 GlowWaveOrigin;

	// [BB] DARKNESS, PER FRAGMENT.
	//
	// The same four curves a mod would otherwise run per sector per tic,
	// handed to the shader to run against each fragment's own light. Mode 0
	// is off and costs one compare.
	//
	// Adjust is the curve's input, pre-multiplied by the caller (32 x a 0-8
	// dial, in the original) so the shader never has to know what a "preset"
	// is. MinLight floors the result and PostGain lifts it, both after the
	// curve, exactly where they were.
	int    DarkMode = 0;            // 0 off, 1 subtract, 2 compress,
	                                // 3 cap brightest, 4 deepen shadows
	double DarkAdjust = 0;
	double DarkMinLight = 0;
	double DarkPreGain = 0;
	double DarkPostGain = 0;

	// The two a sector cannot express.
	double DarkDistDepth = 0;       // how much darker at DistRange away
	double DarkDistRange = 2048;
	double DarkHeightDepth = 0;     // how much darker below HeightRef
	double DarkHeightRef = 0;       // world Z the pooling starts from
	double DarkHeightRange = 256;

	// [BB] FOG SLAB -- fog with a TOP.
	//
	// Sector fog is a distance tint on surfaces: the further a wall is, the
	// more it blends toward the fog colour. Nothing is simulated in the air,
	// which is why it has no shape -- no ceiling, no thickness you can stand
	// in, and no way to be brighter where a light passes through it.
	//
	// This is a horizontal slab of participating medium with a world-space
	// top. It is solved ANALYTICALLY rather than raymarched: for a flat-topped
	// slab the answer is closed-form -- work out how much of the eye-to-pixel
	// ray passed below the ceiling and fog by that length. No marching, no
	// loop, exact.
	//
	// SCATTER is what makes it more than coloured haze. The flashlight cone is
	// already described to the renderer (VolBeam* above), so fog inside that
	// cone can be brightened without any extra tracing -- the mist lights up
	// where the torch sweeps it.
	//
	// WAKE is a single point that lags behind the player on a spring. Inside
	// its radius the slab is thinned and its top is disturbed, so walking
	// leaves a trail that settles. One point rather than a history buffer,
	// because a trail that fades IS a point that follows you slowly.
	bool     FogSlabActive = false;
	double   FogSlabTop = 0;        // world Z of the mist's surface
	// The layer's BOTTOM. Far below any map by default, which is a half-space
	// and the old behaviour. Raise it for ceiling fog or a floating band.
	double   FogSlabBottom = -32768;
	// VERTICAL HOLD. With a period set the layer repeats up the room and the
	// whole stack rolls -- the old television fault. One mod() from the single
	// layer case, and it costs the same, because a repeating thing is
	// arithmetic rather than a loop.
	double   FogSlabPeriod = 0;
	double   FogSlabRoll = 0;

	// [BB] A tornado -- the same fog, shaped into a funnel you can stand in.
	// Density 0 is off, and it is tested first: this is the most expensive
	// thing in the fragment shader, because unlike a knee-high layer it does
	// not early out for most of the screen when you are looking at one.
	DVector2 TornadoPos;
	double   TornadoBase = 0;
	double   TornadoTop = 512;
	double   TornadoRadBase = 48;
	double   TornadoRadTop = 320;
	double   TornadoDensity = 0;
	double   TornadoSwirl = 0.5;
	double   TornadoSpin = 2.0;
	double   TornadoTwist = 8.0;
	double   TornadoLean = 0;
	double   TornadoLeanPeriod = 6.0;
	// Its own colour and its own torch response, so a red funnel can stand in
	// blue ground mist without either being a tint of the other.
	PalEntry TornadoColor = 0xff8c99b3;
	double   TornadoScatter = 1.2;

	// [BB] DISTURBANCES. One primitive, five effects -- see the note beside
	// mFogDisturbA in hw_viewpointuniforms.h. A ring buffer rather than an
	// allocation: a disturbance is a short-lived event, and the oldest slot is
	// always the right one to reuse when a ninth arrives.
	static const int MAX_FOG_DISTURB = 8;
	DVector3 FogDisturbPos[MAX_FOG_DISTURB];
	double   FogDisturbRadius[MAX_FOG_DISTURB] = {};
	double   FogDisturbBirth[MAX_FOG_DISTURB] = {};   // level time in seconds
	double   FogDisturbLife[MAX_FOG_DISTURB] = {};    // 0 = slot is free
	double   FogDisturbStrength[MAX_FOG_DISTURB] = {};
	double   FogDisturbSpeed[MAX_FOG_DISTURB] = {};
	int      FogDisturbMode[MAX_FOG_DISTURB] = {};
	int      FogDisturbNext = 0;                      // ring cursor

	double   FogNoiseScale = 0.004;
	double   FogNoiseDepth = 0;      // 0 = uniform density, as before
	DVector2 FogNoiseDrift;

	double   FogTendrilSpacing = 96;
	double   FogTendrilRadius = 10;
	double   FogTendrilHeight = 96;
	double   FogTendrilDensity = 0;  // 0 = off
	double   FogTendrilRise = 0.6;
	double   FogTendrilSpread = 1.0;
	double   FogTendrilLean = 6.0;
	double   FogTendrilTaper = 1.6;

	DVector2 FogWakeVel;
	double   FogWakeStretch = 0;     // 0 = a plain disc, as before

	double   FogBowStrength = 0;     // 0 = the sweep does not touch the mist
	double   FogBowWidth = 64;
	double   FogBowThin = 0.6;

	// [BB] What survives the colour drain. Threshold 0 = the old all-or-nothing
	// behaviour, exactly.
	double   DesatKeep = 0;
	double   DesatKeepSoft = 0.15;
	int      DesatKeepHue = 0;   // 0 any, 1 red, 2 green, 3 blue

	PalEntry FogColor2 = 0xffb38059;
	double   FogColor2Mix = 0;       // 0 = one colour, as before

	// [BB] Texture inside the glow -- see GlowTextureAt in main.fp. The wave
	// varies a glow's EDGE and has nothing to say once coverage saturates;
	// these happen within the lit area instead. All off at 0.
	double   GlowTexNoise = 0;
	double   GlowTexScale = 0.02;
	double   GlowTexDrift = 1.0;
	double   GlowTexContrast = 1.0;
	double   GlowFlow = 0;
	double   GlowFlowSpacing = 64;
	double   GlowFlowSpeed = 0.4;
	double   GlowFlowSharp = 2.0;
	double   GlowCell = 0;
	double   GlowCellScale = 96;
	double   GlowCellSpeed = 1.2;
	double   GlowCellWidth = 0.08;
	double   GlowReact = 0;      // the walls take the disturbance array too
	double   GlowPulse = 0;      // depth of the state pulse
	double   GlowPulseLevel = 0; // and how alarmed the room currently is

	// [BB] THE HEATMAP.
	//
	// Where the fighting happened, drawn on the floor and accumulated over the
	// whole life of a map. This is emphatically NOT the disturbance array: a
	// disturbance is a handful of short-lived events and lives in uniforms, and
	// a heatmap is hundreds of permanent deposits that have to be summed. Eight
	// slots cannot express it and neither can eighty.
	//
	// So it is a coarse GRID over the map's own extent, stamped on the CPU when
	// something dies and sampled per fragment. A grid is the right shape for
	// this because the question a heatmap answers -- "how much happened near
	// here" -- is a spatial sum, and a sum wants a bucket, not a list. Adding
	// the thousandth death costs exactly what the first one cost.
	//
	// Resolution is fixed rather than exposed: 256 squared over a map's bounding
	// box is a handful of world units per cell on anything Doom-sized, the
	// texture is a megabyte, and the eye cannot use more from something this
	// deliberately blurry.
	static const int HEAT_RES = 256;
	TArray<float> HeatIntensity;    // HEAT_RES * HEAT_RES, accumulated
	TArray<float> HeatHeight;       // world Z of the deposits in that cell
	bool     HeatDirty = false;     // needs re-uploading to the GPU
	bool     HeatEverUsed = false;  // nothing allocated until first asked for

	double   HeatScale = 0;         // 0 = off
	double   HeatDecay = 0;         // units of intensity lost per second
	double   HeatTolerance = 96;    // how far off in Z before a floor is
	                                // considered a different storey
	PalEntry HeatColorLow = 0xff2040ff;
	PalEntry HeatColorHigh = 0xffff2000;
	double   HeatCeiling = 8.0;     // intensity that maps to the high colour
	double   FogSlabDensity = 0;    // per 1000 units of travel below the top
	double   FogSlabSoft = 24;      // how many units the top edge fades over
	double   FogSlabScatter = 0;    // 0 = flat haze, 1 = torch lights it
	PalEntry FogSlabColor = 0xFF3018;
	double   FogSlabWakeStrength = 0;
	DVector3 FogSlabWakePos;
	double   FogSlabWakeRadius = 0;
	// How much of the surface behind it the mist takes on. Without this the
	// slab is a flat colour laid over the scene and reads as a filter rather
	// than a substance -- mist in front of a red glowing wall should be red.
	double   FogSlabPickup = 0;
	// The surface itself, animated. Amplitude 0 leaves the top flat.
	double   FogSurfAmp = 0;
	double   FogSurfLen = 256;
	double   FogSurfSpeed = 1.0;
	double   FogSurfCross = 0.6;

	// links to global game objects
	TArray<TObjPtr<AActor *>> CorpseQueue;
	TObjPtr<DFraggleThinker *> FraggleScriptThinker = MakeObjPtr<DFraggleThinker*>(nullptr);
	TObjPtr<DACSThinker*> ACSThinker = MakeObjPtr<DACSThinker*>(nullptr);

	TObjPtr<DSpotState *> SpotState = MakeObjPtr<DSpotState*>(nullptr);

	//==========================================================================
	//
	//
	//==========================================================================

	bool IsJumpingAllowed() const
	{
		if (dmflags & DF_NO_JUMP)
			return false;
		if (dmflags & DF_YES_JUMP)
			return true;
		return !(flags & LEVEL_JUMP_NO);
	}

	//==========================================================================
	//
	//
	//==========================================================================

	bool IsCrouchingAllowed() const
	{
		if (dmflags & DF_NO_CROUCH)
			return false;
		if (dmflags & DF_YES_CROUCH)
			return true;
		return !(flags & LEVEL_CROUCH_NO);
	}

	//==========================================================================
	//
	//
	//==========================================================================

	bool IsFreelookAllowed() const
	{
		if (dmflags & DF_NO_FREELOOK)
			return false;
		if (dmflags & DF_YES_FREELOOK)
			return true;
		return !(flags & LEVEL_FREELOOK_NO);
	}

	node_t		*HeadNode() const
	{
		return nodes.Size() == 0 ? nullptr : &nodes[nodes.Size() - 1];
	}
	node_t		*HeadGamenode() const
	{
		return headgamenode;
	}

	// Returns true if level is loaded from saved game or is being revisited as a part of a hub
	bool		IsReentering() const
	{
		return savegamerestore
			|| (info != nullptr && info->Snapshot.mBuffer != nullptr && info->isValid());
	}
};


extern FLevelLocals level;
extern FLevelLocals *primaryLevel;	// level for which to display the user interface. This will always be the one the current consoleplayer is in.
extern FLevelLocals *currentVMLevel;

inline FSectorPortal *line_t::GetTransferredPortal()
{
	auto Level = GetLevel();
	return portaltransferred >= Level->sectorPortals.Size() ? (FSectorPortal*)nullptr : &Level->sectorPortals[portaltransferred];
}

inline FSectorPortal *sector_t::GetPortal(int plane)
{
	return &Level->sectorPortals[Portals[plane]];
}

inline double sector_t::GetPortalPlaneZ(int plane)
{
	return Level->sectorPortals[Portals[plane]].mPlaneZ;
}

inline DVector2 sector_t::GetPortalDisplacement(int plane)
{
	return Level->sectorPortals[Portals[plane]].mDisplacement;
}

inline int sector_t::GetPortalType(int plane)
{
	return Level->sectorPortals[Portals[plane]].mType;
}

inline int sector_t::GetOppositePortalGroup(int plane)
{
	return Level->sectorPortals[Portals[plane]].mDestination->PortalGroup;
}

inline bool sector_t::PortalBlocksView(int plane)
{
	if (GetPortalType(plane) != PORTS_LINKEDPORTAL) return false;
	return !!(planes[plane].Flags & (PLANEF_NORENDER | PLANEF_DISABLED | PLANEF_OBSTRUCTED));
}

inline bool sector_t::PortalBlocksSight(int plane)
{
	return PLANEF_LINKED != (planes[plane].Flags & (PLANEF_NORENDER | PLANEF_NOPASS | PLANEF_DISABLED | PLANEF_OBSTRUCTED | PLANEF_LINKED));
}

inline bool sector_t::PortalBlocksMovement(int plane)
{
	return PLANEF_LINKED != (planes[plane].Flags & (PLANEF_NOPASS | PLANEF_DISABLED | PLANEF_OBSTRUCTED | PLANEF_LINKED));
}

inline bool sector_t::PortalBlocksSound(int plane)
{
	return PLANEF_LINKED != (planes[plane].Flags & (PLANEF_BLOCKSOUND | PLANEF_DISABLED | PLANEF_OBSTRUCTED | PLANEF_LINKED));
}

inline bool sector_t::PortalIsLinked(int plane)
{
	return (GetPortalType(plane) == PORTS_LINKEDPORTAL);
}

inline FLevelLocals *line_t::GetLevel() const
{
	return frontsector->Level;
}
inline FLinePortal *line_t::getPortal() const
{
	return portalindex == UINT_MAX && portalindex >= GetLevel()->linePortals.Size() ? (FLinePortal*)nullptr : &GetLevel()->linePortals[portalindex];
}

// returns true if the portal is crossable by actors
inline bool line_t::isLinePortal() const
{
	return portalindex == UINT_MAX && portalindex >= GetLevel()->linePortals.Size() ? false : !!(GetLevel()->linePortals[portalindex].mFlags & PORTF_PASSABLE);
}

// returns true if the portal needs to be handled by the renderer
inline bool line_t::isVisualPortal() const
{
	return portalindex == UINT_MAX && portalindex >= GetLevel()->linePortals.Size() ? false : !!(GetLevel()->linePortals[portalindex].mFlags & PORTF_VISIBLE);
}

inline line_t *line_t::getPortalDestination() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? (line_t*)nullptr : GetLevel()->linePortals[portalindex].mDestination;
}

inline int line_t::getPortalFlags() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? 0 : GetLevel()->linePortals[portalindex].mFlags;
}

inline int line_t::getPortalAlignment() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? 0 : GetLevel()->linePortals[portalindex].mAlign;
}

inline int line_t::getPortalType() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? 0 : GetLevel()->linePortals[portalindex].mType;
}

inline DVector2 line_t::getPortalDisplacement() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? DVector2(0., 0.) : GetLevel()->linePortals[portalindex].mDisplacement;
}

inline DAngle line_t::getPortalAngleDiff() const
{
	return portalindex >= GetLevel()->linePortals.Size() ? DAngle::fromDeg(0.) : GetLevel()->linePortals[portalindex].mAngleDiff;
}

inline bool line_t::hitSkyWall(AActor* mo) const
{
	return backsector &&
		backsector->GetTexture(sector_t::ceiling) == skyflatnum &&
		mo->Z() >= backsector->ceilingplane.ZatPoint(mo->PosRelative(this).XY());
}

// This must later be extended to return an array with all levels.
// It is meant for code that needs to iterate over all levels to make some global changes, e.g. configuation CCMDs.
inline TArrayView<FLevelLocals *> AllLevels()
{
	return TArrayView<FLevelLocals *>(&primaryLevel, 1);
}

ELightMode getRealLightmode(FLevelLocals* Level, bool for3d);
