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
