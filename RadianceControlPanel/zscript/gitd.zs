// =====================================================================
// GlowInTheDark -- ZScript rebuild of GlowInTheDark 1.1 (2021).
//
// The original drove sector glow from an ACS OPEN script that counted
// 0..99999 and fed those numbers to SetSectorGlow. That first argument
// is a sector TAG, not a sector index, so it only ever reached rooms a
// mapper happened to tag -- in most maps, almost none of them.
//
// ZScript can walk Level.Sectors by index, so this reaches every room.
// It also makes the glow height control real (the original declared a
// `height` cvar, never wired it, and passed a hardcoded 64), and it
// re-applies live instead of needing a map restart.
//
// GLOW AUTHORITY. Writing a colour onto a plane used to be one thing:
// SetGlowColor, which outranks that plane's texture. A blanket pass
// therefore erased every colour GLDEFS `Glow { Flats { } }` supplies --
// it is why nukage is green -- and the only defence was to guess which
// planes to skip. The engine now separates the two cases, and this file
// is where the choice between them is made:
//
//   SetGlowColorAuto      paint as a fallback; the flat's own glow wins
//   SetGlowColor          paint as a choice; we win
//   IsGlowAuthored        did this plane's colour come from a choice?
//   Sector.GetTextureGlow what colour and reach does this flat bring?
//   TexMan.GetAverageColor the average colour of any texture's pixels
//
// That last one closes the hole this file used to open with: a colour
// could only be invented, never taken from the artwork. The GLDEFS flat
// list is roughly four hundred names typed out by hand; the average is
// the same computation the engine runs on them, so colour source "from
// the flat itself" works on every flat in every wad, including wads
// that do not exist yet. The GLDEFS list stays -- it is hand-tuned and
// it still outranks us -- but it is no longer the only way.
//
// Which of those to use, and where, is policy, and policy lives here.
// =====================================================================

class GITD_Handler : StaticEventHandler
{
	// Originals, so switching the mod off restores what the map author
	// (or GLDEFS) had rather than leaving our paint behind. The authored
	// flag is stored too: putting a colour back through the wrong setter
	// would restore the value but not the authority.
	private Array<int> mOrigFloorColor;
	private Array<int> mOrigCeilColor;
	private Array<double> mOrigFloorHeight;
	private Array<double> mOrigCeilHeight;
	private Array<int> mOrigFloorAuthored;
	private Array<int> mOrigCeilAuthored;
	private bool mStored;
	private bool mPainted;

	// TextureID index -> average colour. GetAverageColor decodes the
	// texture, so it is a load-time cost; a map has a few dozen distinct
	// flats but thousands of planes, and a repaint happens every time a
	// slider moves. Decode each texture once and keep it.
	private Map<int, int> mFlatColor;

	// Settings for the pass in progress. Read once in Apply rather than
	// per plane -- CVar.FindCVar does a name lookup every call.
	private int mSource;
	private double mReach;
	private bool mSkipLiquid, mRespectTex, mRespectMapper, mUnifyReach;
	private Color mFloorFixed, mCeilFixed;

	// Change detection. UiTick runs while a menu is open, which
	// WorldTick does not -- so dragging a slider updates immediately.
	// UI-scoped because UiTick is the only thing that touches it, and
	// a ui function may not write play-scope state.
	private ui int mLastSig;

	// -----------------------------------------------------------------

	// clearscope: these are read from Apply (play) AND from UiTick (ui).
	// A plain static defaults to the class's scope, which is play, and
	// calling it from UiTick is a hard compile error.
	static clearscope int ReadCvar(string name, int def)
	{
		let c = CVar.FindCVar(name);
		return c ? c.GetInt() : def;
	}

	static clearscope bool ReadBool(string name, bool def)
	{
		let c = CVar.FindCVar(name);
		return c ? c.GetBool() : def;
	}

	// Everything that forces a repaint, folded into one number.
	static clearscope int Signature()
	{
		int s = ReadBool("gitd_enabled", true) ? 1 : 0;
		s = s * 3 + clamp(ReadCvar("gitd_colour_source", 0), 0, 2);
		s = s * 513 + clamp(ReadCvar("gitd_height", 64), 0, 512);
		s = s * 256 + clamp(ReadCvar("gitd_ceiling_r", 135), 0, 255);
		s = s * 256 + clamp(ReadCvar("gitd_ceiling_g", 135), 0, 255);
		s = s * 256 + clamp(ReadCvar("gitd_ceiling_b", 135), 0, 255);
		s = s * 256 + clamp(ReadCvar("gitd_floor_r", 135), 0, 255);
		s = s * 256 + clamp(ReadCvar("gitd_floor_g", 135), 0, 255);
		s = s * 256 + clamp(ReadCvar("gitd_floor_b", 135), 0, 255);
		s = s * 2 + (ReadBool("gitd_skip_liquid", true) ? 1 : 0);
		s = s * 2 + (ReadBool("gitd_respect_textures", true) ? 1 : 0);
		s = s * 2 + (ReadBool("gitd_respect_mapper", true) ? 1 : 0);
		s = s * 2 + (ReadBool("gitd_unify_reach", false) ? 1 : 0);
		return s;
	}

	// -----------------------------------------------------------------

	override void WorldLoaded(WorldEvent e)
	{
		mStored = false;
		mPainted = false;
		mFlatColor.Clear();
		StoreOriginals();
		Apply();
		// mLastSig is deliberately NOT seeded here -- it is ui-scoped and
		// this is play scope. The first UiTick seeds it and fires one
		// redundant refresh, which is harmless because Apply is idempotent.
	}

	override void UiTick()
	{
		int sig = Signature();
		if (sig != mLastSig)
		{
			mLastSig = sig;
			EventHandler.SendNetworkEvent("gitd_refresh");
		}
	}

	override void NetworkProcess(ConsoleEvent e)
	{
		if (e.Name == "gitd_refresh") Apply();
	}

	// -----------------------------------------------------------------

	private void StoreOriginals()
	{
		if (mStored) return;
		mOrigFloorColor.Clear();
		mOrigCeilColor.Clear();
		mOrigFloorHeight.Clear();
		mOrigCeilHeight.Clear();
		mOrigFloorAuthored.Clear();
		mOrigCeilAuthored.Clear();

		for (int i = 0; i < level.Sectors.Size(); i++)
		{
			let sec = level.Sectors[i];
			mOrigFloorColor.Push(int(sec.GetGlowColor(Sector.floor)));
			mOrigCeilColor.Push(int(sec.GetGlowColor(Sector.ceiling)));
			mOrigFloorHeight.Push(sec.GetGlowHeight(Sector.floor));
			mOrigCeilHeight.Push(sec.GetGlowHeight(Sector.ceiling));
			mOrigFloorAuthored.Push(sec.IsGlowAuthored(Sector.floor) ? 1 : 0);
			mOrigCeilAuthored.Push(sec.IsGlowAuthored(Sector.ceiling) ? 1 : 0);
		}
		mStored = true;
	}

	private void Restore()
	{
		if (!mStored || !mPainted) return;
		int n = min(level.Sectors.Size(), mOrigFloorColor.Size());
		for (int i = 0; i < n; i++)
		{
			let sec = level.Sectors[i];
			Paint(sec, Sector.floor, Color(mOrigFloorColor[i]),
				mOrigFloorHeight[i], mOrigFloorAuthored[i] != 0);
			Paint(sec, Sector.ceiling, Color(mOrigCeilColor[i]),
				mOrigCeilHeight[i], mOrigCeilAuthored[i] != 0);
		}
		mPainted = false;
	}

	// One place that decides which setter a write goes through, so the
	// authority rule is never accidentally different in two branches.
	private void Paint(Sector sec, int plane, Color col, double reach, bool authored)
	{
		if (authored) sec.SetGlowColor(plane, col);
		else sec.SetGlowColorAuto(plane, col);
		sec.SetGlowHeight(plane, reach);
	}

	// -----------------------------------------------------------------

	private void Apply()
	{
		StoreOriginals();

		if (!ReadBool("gitd_enabled", true))
		{
			Restore();
			return;
		}

		mReach = clamp(ReadCvar("gitd_height", 64), 0, 512);
		if (mReach <= 0)
		{
			// A reach of zero is a perfectly reasonable way to mean
			// "off". Painting a colour with no reach would light
			// nothing while still displacing the flat's own glow.
			Restore();
			return;
		}

		mSource        = clamp(ReadCvar("gitd_colour_source", 0), 0, 2);
		mSkipLiquid    = ReadBool("gitd_skip_liquid", true);
		mRespectTex    = ReadBool("gitd_respect_textures", true);
		mRespectMapper = ReadBool("gitd_respect_mapper", true);
		mUnifyReach    = ReadBool("gitd_unify_reach", false) && mRespectTex;

		mCeilFixed = Color(255,
			clamp(ReadCvar("gitd_ceiling_r", 135), 0, 255),
			clamp(ReadCvar("gitd_ceiling_g", 135), 0, 255),
			clamp(ReadCvar("gitd_ceiling_b", 135), 0, 255));
		mFloorFixed = Color(255,
			clamp(ReadCvar("gitd_floor_r", 135), 0, 255),
			clamp(ReadCvar("gitd_floor_g", 135), 0, 255),
			clamp(ReadCvar("gitd_floor_b", 135), 0, 255));

		int n = min(level.Sectors.Size(), mOrigFloorColor.Size());
		for (int i = 0; i < n; i++)
		{
			let sec = level.Sectors[i];
			PaintPlane(sec, i, Sector.floor);
			PaintPlane(sec, i, Sector.ceiling);
		}

		mPainted = true;
	}

	private void PaintPlane(Sector sec, int index, int plane)
	{
		// The map author picked this one deliberately. Leave it exactly as
		// found -- colour, reach and authority. Read from the stored
		// originals, not from the plane, because by now the plane may be
		// carrying our own paint from an earlier pass.
		if (mRespectMapper)
		{
			bool wasAuthored = (plane == Sector.floor ? mOrigFloorAuthored[index] : mOrigCeilAuthored[index]) != 0;
			int wasColor = plane == Sector.floor ? mOrigFloorColor[index] : mOrigCeilColor[index];
			if (wasAuthored && wasColor != 0)
			{
				Paint(sec, plane, Color(wasColor),
					plane == Sector.floor ? mOrigFloorHeight[index] : mOrigCeilHeight[index], true);
				return;
			}
		}

		// Liquid terrain, skipped wholesale rather than by colour source.
		// This is a broader test than "the flat glows": a liquid whose flat
		// is not in the GLDEFS list is still caught here.
		if (mSkipLiquid)
		{
			let terrain = sec.GetFloorTerrain(plane);
			if (terrain && terrain.IsLiquid) return;
		}

		// The flat's own glow, straight from GLDEFS -- colour 0 if it has none.
		Color texColor;
		double texReach;
		[texColor, texReach] = sec.GetTextureGlow(plane);

		if (mUnifyReach && int(texColor) != 0)
		{
			// Keep what the texture says the colour should be, but at the
			// reach the player asked for. A texture's glow height is shared
			// by every plane using it, so the only way to vary it per plane
			// is to take authority and restate the colour ourselves. If it
			// already reaches that far, don't: claiming authority we do not
			// need is the whole mistake this API exists to avoid.
			if (texReach != mReach) Paint(sec, plane, texColor, mReach, true);
			else Paint(sec, plane, Color(0), mReach, false);
			return;
		}

		// mRespectTex: write as a fallback, and the flat's own glow -- if it
		// has one -- simply outranks it. No guessing about which planes.
		Paint(sec, plane, ChooseColor(sec, plane), mReach, !mRespectTex);
	}

	private Color ChooseColor(Sector sec, int plane)
	{
		if (mSource == 2) return FlatColor(sec, plane);
		if (mSource == 1) return plane == Sector.floor ? mFloorFixed : mCeilFixed;
		return RollColor();
	}

	// The average of the flat's own pixels, decoded once per texture.
	// Keyed by texture rather than by sector, so a plane whose flat changes
	// mid-game picks up the new colour on the next repaint by itself.
	private Color FlatColor(Sector sec, int plane)
	{
		TextureID tex = sec.GetTexture(plane);
		int key = int(tex);
		// Color(int) is not a constructor -- Color's forms are (r,g,b) and
		// (a,r,g,b). Go through a local and let the int convert implicitly.
		if (mFlatColor.CheckKey(key))
		{
			Color cached = mFlatColor.Get(key);
			return cached;
		}

		Color c = TexMan.GetAverageColor(tex);
		mFlatColor.Insert(key, int(c));
		return c;
	}

	// The original's roll: each channel independent, floor of 25 so a
	// sector can never come out black.
	private Color RollColor()
	{
		return Color(255, random(25, 255), random(25, 255), random(25, 255));
	}
}
