//-----------------------------------------------------------------------------
//
// Copyright 1993-1996 id Software
// Copyright 1999-2016 Randy Heit
// Copyright 2002-2016 Christoph Oelckers
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//		Ticker.
//
//-----------------------------------------------------------------------------

#include "p_local.h"
#include "p_effect.h"
#include "c_console.h"
#include "b_bot.h"
#include "doomstat.h"
#include "sbar.h"
#include "r_data/r_interpolate.h"
#include "d_player.h"
#include "r_utility.h"
#include "p_spec.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "g_game.h"
#include "i_interface.h"
#include "c_dispatch.h"
#include "texturemanager.h"
#include "menu.h"          // [BB] M_MenuPauses -- see P_CheckTickerPaused

extern gamestate_t wipegamestate;
extern uint8_t globalfreeze, globalchangefreeze;

//==========================================================================
//
// P_CheckTickerPaused
//
// Returns true if the ticker should be paused. In that case, it also
// pauses sound effects and possibly music. If the ticker should not be
// paused, then it returns false but does not unpause anything.
//
//==========================================================================

bool P_CheckTickerPaused ()
{
	// pause if in menu or console and at least one tic has been run
	// [BB] M_MenuPauses() is the third case: a menu that opted out of pausing
	// via DMenu::DontPause. Without it a lighting page can turn off the dim
	// and the blur to show you the room it is adjusting, and then show you a
	// frozen one -- no slider takes effect until you back out, because
	// nothing re-evaluates while the playsim is stopped.
	if ( !netgame
		 && gamestate != GS_TITLELEVEL
		 && ((menuactive != MENU_Off && menuactive != MENU_OnNoPause && M_MenuPauses()) ||
			 ConsoleState == c_down || ConsoleState == c_falling)
		 && !demoplayback
		 && !demorecording
		 && players[consoleplayer].viewz != NO_VALUE
		 && wipegamestate == gamestate)
	{
		// Only the current UI level's settings are relevant for sound.
		S_PauseSound (!(primaryLevel->flags2 & LEVEL2_PAUSE_MUSIC_IN_MENUS), false);
		return true;
	}
	return false;
}

//==========================================================================
//
// [BB] FLevelLocals::TickBillboards
//
// Billboards are set-and-forget, so this is maintenance rather than a
// rebuild: make attached ones follow, drop the ones whose actor has gone,
// and expire transients. Runs once per game tic, not per rendered frame,
// so lifetime means the same thing regardless of framerate.
//
// Iterates backwards because entries are deleted in place.
//
//==========================================================================

void FLevelLocals::TickBillboards()
{
	for (int bi = (int)Billboards.Size() - 1; bi >= 0; bi--)
	{
		auto &bb = Billboards[bi];

		if (bb.flags & BBFL_ATTACHED)
		{
			// Attached billboards die with their actor -- and only ever in
			// that direction. Resolving null here means the actor was
			// destroyed and swept, so the billboard goes; it never keeps
			// the actor alive to avoid the question.
			if (bb.attachedTo == nullptr)
			{
				Billboards.Delete(bi);
				continue;
			}
			bb.pos = bb.attachedTo->Pos() + bb.attachOffset;
			continue;	// attachment overrides lifetime entirely
		}

		if (!(bb.flags & BBFL_PERSISTENT) && bb.lifetime > 0.0)
		{
			double ageSec = (maptime - bb.spawntic) / (double)TICRATE;
			if (ageSec >= bb.lifetime)
			{
				Billboards.Delete(bi);
			}
		}
	}
}

//==========================================================================
//
// [BB] bb_spawn -- put a billboard in front of the player and look at it.
//
// A diagnostic, not a feature. Everything about a billboard that can only be
// judged by eye -- whether the size is sane, whether the tilt leans the way
// the documentation claims, whether the texture comes out mirrored -- needs
// one on screen to answer, and this is the shortest path to that.
//
//   bb_spawn [texture] [width] [height] [tilt] [facing]
//
// Defaults to a texture every IWAD has, so it draws something without
// needing a mod loaded.
//
//==========================================================================

//==========================================================================
//
// [BB] bb_text -- put a line of distance-field text in front of the player.
//
//   bb_text <string> [glowRadius] [glowStrength] [rrggbb]
//
// The companion to bb_spawn, and the reason it exists is that testing BB_TEXT
// otherwise means writing a mod, packing a pk3 and restarting -- which is a
// long way round to find out whether a halo is too tight.
//
// glowRadius is a fraction of the atlas spread; 1.0 is the whole field and
// the practical maximum. Colour is hex, default cyan.
//
//==========================================================================

CCMD(bb_text)
{
	if (gamestate != GS_LEVEL || players[consoleplayer].mo == nullptr)
	{
		Printf("bb_text: not in a level\n");
		return;
	}
	if (argv.argc() < 2)
	{
		Printf("bb_text <string> [glowRadius 0..1] [glowStrength 0..1] [rrggbb]\n");
		return;
	}

	AActor *pmo = players[consoleplayer].mo;
	auto Level = pmo->Level;

	const double gr = (argv.argc() > 2) ? atof(argv[2]) : 0.0;
	const double gs = (argv.argc() > 3) ? atof(argv[3]) : 0.0;
	const uint32_t rgb = (argv.argc() > 4) ? (uint32_t)strtoul(argv[4], nullptr, 16) : 0x28FFFFu;

	DAngle ang = pmo->Angles.Yaw;
	DVector3 where = pmo->Pos()
		+ DVector3(ang.Cos() * 96.0, ang.Sin() * 96.0, pmo->Height * 0.7);

	FBillboard bb;
	bb.id = Level->NextBillboardID++;
	bb.pos = where;
	bb.width = 96.0;
	bb.height = 24.0;
	// Turned to face back at the player, so it is readable where it lands
	// rather than edge-on.
	bb.yaw = (ang + DAngle::fromDeg(180)).Degrees();
	bb.tilt = 0.0;
	bb.facing = BBF_FIXED;
	bb.payload = BB_TEXT;
	bb.data = 0;
	bb.text = argv[1];
	bb.glowRadius = gr;
	bb.glowStrength = gs;
	bb.color = PalEntry(255, (uint8_t)((rgb >> 16) & 0xff), (uint8_t)((rgb >> 8) & 0xff), (uint8_t)(rgb & 0xff));
	bb.alpha = 1.0;
	bb.flags = BBFL_PERSISTENT;
	bb.spawntic = Level->maptime;
	Level->Billboards.Push(bb);

	Printf("bb_text: id %d, \"%s\", glow %g/%g (%d live) -- bb_clear removes them\n",
		bb.id, bb.text.GetChars(), gr, gs, Level->Billboards.Size());
}

CCMD(bb_spawn)
{
	if (gamestate != GS_LEVEL || players[consoleplayer].mo == nullptr)
	{
		Printf("bb_spawn: not in a level\n");
		return;
	}

	AActor *pmo = players[consoleplayer].mo;
	auto Level = pmo->Level;

	const char *texname = (argv.argc() > 1) ? argv[1] : "STARTAN2";
	double w      = (argv.argc() > 2) ? atof(argv[2]) : 64.0;
	double h      = (argv.argc() > 3) ? atof(argv[3]) : 64.0;
	double tilt   = (argv.argc() > 4) ? atof(argv[4]) : 0.0;
	int    facing = (argv.argc() > 5) ? atoi(argv[5]) : BBF_FIXED;

	FTextureID tid = TexMan.CheckForTexture(texname, ETextureType::Any);
	if (!tid.isValid())
	{
		Printf("bb_spawn: no texture '%s'\n", texname);
		return;
	}

	// 96 units ahead at eye height, and turned to face back at the player, so
	// a FIXED billboard is readable where it lands instead of edge-on.
	DAngle ang = pmo->Angles.Yaw;
	DVector3 where = pmo->Pos()
		+ DVector3(ang.Cos() * 96.0, ang.Sin() * 96.0, pmo->Height * 0.7);

	FBillboard bb;
	bb.id = Level->NextBillboardID++;
	bb.pos = where;
	bb.width = w;
	bb.height = h;
	bb.yaw = (ang + DAngle::fromDeg(180)).Degrees();
	bb.tilt = tilt;
	bb.facing = facing;
	bb.payload = BB_TEXTURE;
	bb.data = tid.GetIndex();
	bb.color = 0xFFFFFF;
	bb.alpha = 1.0;
	bb.flags = BBFL_PERSISTENT;
	bb.spawntic = Level->maptime;
	Level->Billboards.Push(bb);

	Printf("bb_spawn: id %d, '%s', %g x %g, tilt %g, facing %d (%d live)\n",
		bb.id, texname, w, h, tilt, facing, Level->Billboards.Size());
}

//==========================================================================
//
// [BB] bb_clear -- remove every billboard. The companion to bb_spawn.
//
//==========================================================================

CCMD(bb_clear)
{
	if (gamestate != GS_LEVEL || players[consoleplayer].mo == nullptr)
	{
		Printf("bb_clear: not in a level\n");
		return;
	}
	auto Level = players[consoleplayer].mo->Level;
	unsigned n = Level->Billboards.Size();
	Level->Billboards.Clear();
	Printf("bb_clear: removed %u\n", n);
}

//
// P_Ticker
//
void P_Ticker (void)
{
	int i;

	for (auto Level : AllLevels())
	{
		Level->interpolator.UpdateInterpolations();
	}
	r_NoInterpolate = true;

	if (!demoplayback)
	{
		// This is a separate slot from the wipe in D_Display(), because this
		// is delayed slightly due to latency. (Even on a singleplayer game!)
//		GSnd->SetSfxPaused(!!playerswiping, 2);
	}

	// run the tic
	if (paused || P_CheckTickerPaused())
	{
		// This must run even when the game is paused to catch changes from netevents before the frame is rendered.
		for (auto Level : AllLevels())
		{
			auto it = Level->GetThinkerIterator<AActor>();
			AActor* ac;

			while ((ac = it.Next()))
			{
				if (ac->flags8 & MF8_RECREATELIGHTS)
				{
					ac->flags8 &= ~MF8_RECREATELIGHTS;
					ac->SetDynamicLights();
				}
			}
		}
		return;
	}

	DPSprite::NewTick();

	// [RH] Frozen mode is only changed every 4 tics, to make it work with A_Tracer().
	// This may not be perfect but it is not really relevant for sublevels that tracer homing behavior is preserved.
	if ((primaryLevel->maptime & 3) == 0)
	{
		if (globalchangefreeze)
		{
			globalfreeze ^= 1;
			globalchangefreeze = 0;
			for (auto Level : AllLevels())
			{
				Level->frozenstate = (Level->frozenstate & ~2) | (2 * globalfreeze);
			}
		}
	}

	// [BC] Do a quick check to see if anyone has the freeze time power. If they do,
	// then don't resume the sound, since one of the effects of that power is to shut
	// off the music.
	for (i = 0; i < MAXPLAYERS; i++ )
	{
		if (playeringame[i] && players[i].timefreezer != 0)
			break;
	}

	if ( i == MAXPLAYERS )
		S_ResumeSound (false);

	P_ResetSightCounters (false);
	R_ClearInterpolationPath();

	// Since things will be moving, it's okay to interpolate them in the renderer.
	r_NoInterpolate = false;

	// Reset all actor interpolations on all levels before the current thinking turn so that indirect actor movement gets properly interpolated.
	for (auto Level : AllLevels())
	{
		// todo: set up a sandbox for secondary levels here.
		auto it = Level->GetThinkerIterator<AActor>();
		AActor *ac;

		while ((ac = it.Next()))
		{
			ac->ClearInterpolation();
			ac->ClearFOVInterpolation();
		}

		P_ThinkParticles(Level);	// [RH] make the particles think

		Level->TickBillboards();	// [BB] follow attachments, expire transients

		for (i = 0; i < MAXPLAYERS; i++)
			if (Level->PlayerInGame(i))
				P_PlayerThink(Level->Players[i]);

		// [ZZ] call the WorldTick hook
		Level->localEventManager->WorldTick();
		Level->Tick();			// [RH] let the level tick
		Level->Thinkers.RunThinkers(Level);

		//if added by MC: Freeze mode.
		if (!Level->isFrozen())
		{
			P_UpdateSpecials(Level);
		}

		// for par times
		Level->time++;
		Level->maptime++;
		Level->totaltime++;
	}
	if (players[consoleplayer].mo != NULL) {
		if (players[consoleplayer].mo->Vel.Length() > primaryLevel->max_velocity) { primaryLevel->max_velocity = players[consoleplayer].mo->Vel.Length(); }
		primaryLevel->avg_velocity += (players[consoleplayer].mo->Vel.Length() - primaryLevel->avg_velocity) / primaryLevel->maptime;
	}
	StatusBar->CallTick();		// Status bar should tick AFTER the thinkers to properly reflect the level's state at this time.
}
