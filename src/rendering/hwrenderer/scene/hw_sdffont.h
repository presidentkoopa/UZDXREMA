#pragma once

//==========================================================================
//
// [BB] SDF fonts -- text that stays sharp when you walk up to it.
//
// WHAT A DISTANCE FIELD IS, AND WHY THIS EXISTS
//
// An ordinary font glyph is a picture: each texel says "ink" or "no ink".
// Magnify it and the hard edge between those two answers magnifies with it,
// so a letter drawn small and viewed close goes blocky. That is fine for a
// HUD, where a glyph is always the same size on screen, and wrong for a
// billboard, which is a real object the player can walk toward. BB_TEXT was
// exactly that -- readable across the room, mush up close.
//
// A distance field stores something else. Each texel holds HOW FAR IT IS FROM
// THE EDGE of the letter, positive inside and negative outside, and ordinary
// bilinear filtering interpolates that smoothly. The shader asks "is my
// distance above zero" and gets a clean answer at any magnification, because
// it is reconstructing the edge rather than resampling a picture of one.
//
// It also hands over the glow, which is the real reason this is here. Neon is
// a bright core with light falling off past the edge -- a function of distance
// from the edge, which is exactly and only what this texture stores. No blur
// pass, no second texture, no post-process.
//
// THE ATLAS IS BUILT OFFLINE. tools/sdffont/mksdf.ps1 takes a TTF, walks each
// glyph's OUTLINE, and writes two lumps: an ordinary PNG and a plain-text
// metrics file. The engine loads the PNG like any other texture and parses the
// metrics here. Nothing is generated at runtime and no font file ships.
//
// That split is load-bearing rather than tidy-minded. Generating at runtime
// would mean a distance transform per glyph at every startup, a custom
// FImageSource to get the result into a texture, and a field whose quality is
// capped by whatever bitmap font happened to be loaded. Offline, the source is
// a vector outline supersampled 8x, so the only limit is what the generator
// was told to do.
//
// WHAT IT STILL CANNOT DO
//
// Corners round off, because a single channel per texel cannot describe two
// edges meeting at a point -- the one number has to compromise between them.
// Measured at spread 8 in a 64px cell on a pixel font this is not visible, so
// MSDF (three channels, corners survive) is budgeted and NOT built. If a
// future font shows it, MSDF fills this same table and nothing above this file
// changes.
//
// GLOW RADIUS IS BOUNDED BY THE SPREAD. Past it there is no field left to
// read, and the glow clips to a hard square at the cell boundary. Anything
// wanting a wider glow needs an atlas regenerated with a wider spread, which
// costs glyph area inside the same cell.
//
//==========================================================================

#include "tarray.h"
#include "zstring.h"

class FGameTexture;

// [BB] One glyph's cell in the atlas, plus what a layout needs to place it.
//
// Metrics are in ATLAS PIXELS -- the same units the generator wrote -- and a
// caller scales them by whatever height it actually wants. Storing them
// pre-normalised would bake in one text size and make every other one a
// division.
struct FSDFGlyph
{
	float u0 = 0.f, v0 = 0.f;	// atlas UV, top-left of the cell
	float u1 = 0.f, v1 = 0.f;	// atlas UV, bottom-right of the cell
	float advance = 0.f;		// pen movement, atlas pixels
	bool  present = false;		// false = codepoint absent from this atlas
};

//==========================================================================
//
// [BB] FSDFFont -- one atlas plus its metrics, loaded once and cached.
//
//==========================================================================

class FSDFFont
{
public:
	// Load-or-fetch by base name. Looks for "<name>.png" and "<name>.txt".
	// Returns null if either lump is missing or the metrics do not parse,
	// which callers should treat as "fall back to the bitmap glyph path"
	// rather than as fatal -- a mod is allowed to not ship one.
	static FSDFFont *Get(const char *name);

	static void FlushAll();

	// Null when the codepoint is not in the atlas.
	const FSDFGlyph *Glyph(int code) const;

	FGameTexture *Atlas() const { return mAtlas; }

	// Cell size the atlas was generated at, in atlas pixels. Layout divides
	// by this to turn a requested text height into a scale factor.
	float Cell() const { return mCell; }

	// How far past the ink the field still carries usable distance, in atlas
	// pixels. The shader needs it to turn a texel's 0..1 back into a real
	// distance, and it is the hard ceiling on glow radius.
	float Spread() const { return mSpread; }

private:
	FString mName;
	FGameTexture *mAtlas = nullptr;
	TArray<FSDFGlyph> mGlyphs;	// indexed by code - mFirst
	int mFirst = 0, mLast = 0;
	float mCell = 0.f;
	float mSpread = 0.f;

	bool Load(const char *name);
};

//==========================================================================
//
// [BB] THE FONT ROSTER -- MORE THAN ONE TYPEFACE ON SCREEN AT ONCE.
//
// Until 2026-08-09 the face was `bb_sdffont`, a CVAR, and therefore GLOBAL:
// every BB_TEXT in the world drew from the same atlas. That is fine for a
// debug caption and hopeless for a card, where the whole craft of the thing
// is a heavy display face on the name and a clean one on the numbers.
//
// So a billboard now names a SLOT and the slot names a font.
//
//   slot 0        always `bb_sdffont`. Deterministic, always present, and
//                 what every existing call site already gets by leaving the
//                 field at its default -- so nothing that worked before
//                 changes appearance.
//   slot 1..N     the rolled roster, shuffled at game load from every font
//                 actually shipped.
//
// The roster is rolled rather than fixed at the owner's direction: the mod
// ships 42 arcade faces and a run should not look like the last one. Rolling
// means NO CALL SITE MAY ASSUME A SLOT'S IDENTITY -- slot 3 is a different
// typeface every game, which is the point. Ask for a slot because you want
// "the display face", not because you want a particular font.
//
// Anything the roster cannot resolve falls back to slot 0 rather than to
// nothing. A missing font must degrade to the wrong typeface, never to
// invisible text -- an empty card gives the player no way to tell a content
// bug from a missing lump.
//
//==========================================================================

class FSDFFontRoster
{
public:
	// Scan, shuffle, and fill. Safe to call repeatedly -- each call is a
	// fresh roll, which is how a new game gets a new look.
	static void Roll();

	// The font in this slot, or the slot-0 font when the roster is empty,
	// the index is out of range, or that entry failed to load. Never null
	// unless slot 0 itself is unavailable, in which case the caller is
	// already on the bitmap fallback path.
	static FSDFFont *Slot(int n);

	// How many rolled entries exist, NOT counting slot 0. Script uses this
	// to know the legal range; 0 means "only the default font is present".
	static int Count();

	// Family name in this slot, for diagnostics. Never null.
	static const char *SlotName(int n);
};

//==========================================================================
//
// [BB] MULTI-LINE LAYOUT -- ONE DEFINITION, SHARED BY THE RENDERER AND THE
// SCRIPT-SIDE MEASURE.
//
// BB_TEXT was single-line until 2026-08-09. One pen, walking left to right,
// and '\n' resolved to no glyph at all -- so it was silently skipped while
// the pen carried on, and two "lines" ran together on one row. That made a
// stat table cost one billboard per row when the whole point of a two-column
// table is that it should be two draws: one string of labels, one of values.
//
// This lives in the header, and not in either .cpp, because the layout is an
// AGREEMENT BETWEEN TWO TRANSLATION UNITS -- hw_sprites.cpp draws the block
// and vmthunks.cpp measures it for script. If those two ever disagreed, a
// panel would be sized from one set of rules and filled using another, and
// the only symptom would be text sitting slightly wrong inside its own box.
// That is the kind of defect nobody finds by reading either file alone, so
// there is deliberately only one copy of the arithmetic.
//
//==========================================================================

// Baseline-to-baseline spacing, in em boxes. 1.0 would set the lines exactly
// touching; the surplus is leading, and it also gives the halo somewhere to
// live so two stacked lines do not add their glow into each other -- the same
// overlap problem the per-glyph quad trimming exists to solve horizontally.
inline constexpr double SDFTEXT_LINE_PITCH = 1.30;

struct FSDFTextMetrics
{
	double widest = 0.0;	// widest single line, in raw advance units
	int    lines  = 0;		// 0 only for empty/absent text, otherwise >= 1
	double blockEm = 0.0;	// total block height, in em boxes
};

// Walks the string once and reports what the block needs. '\r' is skipped so
// a CRLF string measures the same as an LF one -- these come from script and
// from text files, and a stray carriage return rendering as a missing glyph
// would be a maddening thing to chase.
inline FSDFTextMetrics SDFMeasureText(const FSDFFont *font, const char *text)
{
	FSDFTextMetrics m;
	if (font == nullptr || text == nullptr || *text == 0) return m;

	double cur = 0.0;
	m.lines = 1;
	for (const uint8_t *c = (const uint8_t *)text; *c != 0; ++c)
	{
		if (*c == '\n')
		{
			if (cur > m.widest) m.widest = cur;
			cur = 0.0;
			m.lines++;
			continue;
		}
		if (*c == '\r') continue;
		if (const FSDFGlyph *g = font->Glyph((int)*c)) cur += g->advance;
	}
	if (cur > m.widest) m.widest = cur;

	m.blockEm = 1.0 + (double)(m.lines - 1) * SDFTEXT_LINE_PITCH;
	return m;
}

// Width of the one line starting at `text` and ending at '\n' or NUL, in raw
// advance units. The emitter needs this per line so each line is centred on
// its OWN width -- otherwise a short line in a wide block sits left-ragged
// against a centred one, which reads as a layout bug rather than a choice.
inline double SDFMeasureLine(const FSDFFont *font, const uint8_t *text)
{
	if (font == nullptr || text == nullptr) return 0.0;
	double total = 0.0;
	for (const uint8_t *c = text; *c != 0 && *c != '\n'; ++c)
	{
		if (*c == '\r') continue;
		if (const FSDFGlyph *g = font->Glyph((int)*c)) total += g->advance;
	}
	return total;
}
