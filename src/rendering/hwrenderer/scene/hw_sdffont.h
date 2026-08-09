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
// HUD, where the glyph is always the same size on screen, and wrong for a
// billboard, which is a real object in the world that the player can walk
// toward. BB_TEXT was that -- readable across the room, mush up close.
//
// A distance field stores something else. Each texel holds HOW FAR IT IS
// FROM THE EDGE of the letter, positive inside and negative outside, and the
// hardware's ordinary bilinear filtering interpolates that smoothly. The
// shader then asks "is my distance above zero" and gets a clean answer at any
// magnification, because it is reconstructing the edge rather than resampling
// a picture of one.
//
// It also hands the glow over for free, which is the actual reason this is
// here rather than any general wish for tidier text. Neon is a bright core
// with light falling off past the edge -- which is a function of distance
// from the edge, which is exactly and only what this texture stores. No blur
// pass, no second texture.
//
// WHAT THIS PARTICULAR IMPLEMENTATION CANNOT DO
//
// It builds the field from the engine's existing bitmap fonts, because that
// needs no new asset and works with every font already loaded. That caps it
// twice, and both are worth knowing before anyone is disappointed:
//
//   * SHARP CORNERS ROUND OFF. A single-channel distance field cannot
//     represent two edges meeting at a point -- the one number per texel has
//     to compromise between them. Fixing that is MSDF, which spends three
//     channels so corners survive, and which needs a vector source to be
//     worth doing.
//   * NO DETAIL IS INVENTED. A glyph that is eight pixels tall in the lump
//     has eight pixels of information. This makes it smooth; it cannot make
//     it fine.
//
// So the win here is smooth-at-any-size and correct glow. Crisp corners wait
// for an offline generator fed by a real outline font. When that lands it
// fills the same FSDFGlyph table and nothing above this file changes.
//
//==========================================================================

#include "tarray.h"

class FFont;
class FGameTexture;

// [BB] Where one glyph lives in the atlas, and how to place it.
//
// Metrics are in SOURCE FONT PIXELS, not atlas pixels, deliberately. The
// atlas rasterises everything to a common size so one font's atlas is not
// twenty times another's, and if these were atlas pixels every caller would
// have to know that scale factor to lay out a line. In source pixels a
// caller can measure a string against FFont::GetHeight() the way it always
// has and the atlas stays an implementation detail.
struct FSDFGlyph
{
	float u0 = 0.f, v0 = 0.f;	// atlas UV, top-left
	float u1 = 0.f, v1 = 0.f;	// atlas UV, bottom-right
	float width = 0.f;			// quad extent, source pixels, INCLUDING the
	float height = 0.f;			// spread margin -- the field runs past the ink
	float offsetX = 0.f;		// pen to quad's left edge, source pixels
	float offsetY = 0.f;		// glyph top to quad's top edge, source pixels
	float advance = 0.f;		// how far the pen moves, source pixels
	bool  present = false;		// false = this codepoint is not in the font
};

//==========================================================================
//
// [BB] FSDFFont -- one atlas per source font, built once, cached forever.
//
// Building costs a distance transform per glyph, which is why this is not
// done per draw or per string. It is done the first time a font is asked for
// and then never again for the life of the process.
//
//==========================================================================

class FSDFFont
{
public:
	// Build-or-fetch. Returns null only if the font has no usable glyphs at
	// all, which a caller should treat as "fall back to the bitmap path"
	// rather than as an error -- some fonts really are empty.
	static FSDFFont *For(FFont *src);

	// Drop every cached atlas. For a texture-precache flush; not needed in
	// normal play.
	static void FlushAll();

	// Null when the codepoint is absent from the source font.
	const FSDFGlyph *Glyph(int code) const;

	FGameTexture *Atlas() const { return mAtlas; }

	// Source font's line height, in the same source pixels the metrics use.
	float Height() const { return mHeight; }

	// How far past the ink the field still carries usable distance, in
	// source pixels. The shader needs this to turn a texel's 0..1 value back
	// into a real distance, and a glow cannot be asked to reach further than
	// this without running off the end of the field and flattening.
	float Spread() const { return mSpread; }

private:
	FFont *mSource = nullptr;
	FGameTexture *mAtlas = nullptr;
	TArray<FSDFGlyph> mGlyphs;	// indexed by code - mFirst
	int mFirst = 0, mLast = 0;
	float mHeight = 0.f;
	float mSpread = 0.f;

	bool Build(FFont *src);
};
