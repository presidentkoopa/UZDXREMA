//==========================================================================
//
// [BB] SDF font loading. See hw_sdffont.h for what a distance field is and
// why the atlas is built offline instead of here.
//
// This file does nothing clever on purpose. The generator
// (tools/sdffont/mksdf.ps1) already did the work; all that is left is to find
// two lumps, read a handful of numbers out of one of them, and turn cell
// rectangles into UVs. If something looks expensive in here it is a bug.
//
//==========================================================================

#include "hw_sdffont.h"

#include "filesystem.h"
#include "texturemanager.h"
#include "printf.h"

//==========================================================================
//
// The cache. Keyed by the name the caller asked for, and it stores FAILURES
// as null rather than dropping them -- otherwise a mod that names a font it
// does not ship makes the engine hunt for two missing lumps on every single
// glyph of every frame, which is a stutter nobody would connect to a typo.
//
//==========================================================================

static TMap<FString, FSDFFont *> sSDFFonts;

FSDFFont *FSDFFont::Get(const char *name)
{
	if (name == nullptr || *name == 0) return nullptr;

	FString key = name;
	key.ToLower();

	if (FSDFFont **found = sSDFFonts.CheckKey(key))
		return *found;

	FSDFFont *font = new FSDFFont;
	if (!font->Load(name))
	{
		delete font;
		font = nullptr;
	}
	sSDFFonts.Insert(key, font);
	return font;
}

void FSDFFont::FlushAll()
{
	TMap<FString, FSDFFont *>::Iterator it(sSDFFonts);
	TMap<FString, FSDFFont *>::Pair *pair;
	while (it.NextPair(pair))
	{
		if (pair->Value != nullptr) delete pair->Value;
	}
	sSDFFonts.Clear();
}

const FSDFGlyph *FSDFFont::Glyph(int code) const
{
	const int i = code - mFirst;
	if (i < 0 || (unsigned)i >= mGlyphs.Size()) return nullptr;
	const FSDFGlyph &g = mGlyphs[i];
	return g.present ? &g : nullptr;
}

//==========================================================================
//
// Load. "<name>" is a texture; "sdffonts/<name>.txt" is its metrics.
//
// Returns false rather than complaining loudly for a font that simply is not
// there -- callers fall back to the bitmap glyph path, and a mod is allowed
// not to ship one. A metrics file that exists but does not parse IS worth a
// line in the log, because that is a broken asset rather than an absent one.
//
//==========================================================================

bool FSDFFont::Load(const char *name)
{
	mName = name;

	FTextureID id = TexMan.CheckForTexture(name, ETextureType::Any);
	if (!id.isValid())
	{
		Printf(TEXTCOLOR_YELLOW "SDF font '%s': no such texture; falling back to bitmap glyphs\n", name);
		return false;
	}
	mAtlas = TexMan.GetGameTexture(id, true);
	if (mAtlas == nullptr) return false;

	FString lumpName;
	lumpName.Format("sdffonts/%s.txt", name);
	const int lump = fileSystem.CheckNumForFullName(lumpName.GetChars(), true);
	if (lump < 0)
	{
		Printf(TEXTCOLOR_YELLOW "SDF font '%s': texture found but '%s' is missing; "
			"falling back to bitmap glyphs\n", name, lumpName.GetChars());
		return false;
	}

	auto fileData = fileSystem.ReadFile(lump);
	const size_t len = fileData.size();
	if (len == 0) return false;

	// Own a NUL-terminated copy: the parse below walks it destructively and
	// the filesystem's buffer is not ours to chew on.
	TArray<char> text((unsigned)len + 1, true);
	memcpy(text.Data(), fileData.data(), len);
	text[(unsigned)len] = 0;

	// The generator writes 32..126 today, but the table is sized to the whole
	// byte range so a future atlas can add glyphs without touching this.
	mFirst = 0;
	mLast = 255;
	mGlyphs.Clear();
	mGlyphs.Resize(256);
	for (auto &g : mGlyphs) g = FSDFGlyph();

	float atlasW = (float)mAtlas->GetTexelWidth();
	float atlasH = (float)mAtlas->GetTexelHeight();
	int glyphCount = 0;

	// Walked by hand rather than with strtok: the reentrant spelling differs
	// between MSVC and everyone else, and this needs no state to avoid the
	// question entirely.
	char *cursor = text.Data();
	while (*cursor != 0)
	{
		char *line = cursor;
		while (*cursor != 0 && *cursor != '\n' && *cursor != '\r') cursor++;
		if (*cursor != 0) { *cursor = 0; cursor++; }
		while (*cursor == '\n' || *cursor == '\r') cursor++;

		while (*line == ' ' || *line == '\t') line++;
		if (*line == 0 || *line == '#') continue;

		if (!strncmp(line, "cell ", 5))
		{
			mCell = (float)atof(line + 5);
		}
		else if (!strncmp(line, "spread ", 7))
		{
			mSpread = (float)atof(line + 7);
		}
		else if (!strncmp(line, "atlas ", 6))
		{
			// Trust the generator's dimensions over the texture's. A texture
			// can be padded to a power of two on upload, and UVs computed
			// against the padded size would slide every glyph.
			float w = 0.f, h = 0.f;
			if (sscanf(line + 6, "%f %f", &w, &h) == 2 && w > 0.f && h > 0.f)
			{
				atlasW = w;
				atlasH = h;
			}
		}
		else if (!strncmp(line, "g ", 2))
		{
			int code = 0;
			float gx = 0.f, gy = 0.f, gw = 0.f, gh = 0.f, adv = 0.f;
			if (sscanf(line + 2, "%d %f %f %f %f %f", &code, &gx, &gy, &gw, &gh, &adv) != 6)
				continue;
			if (code < mFirst || code > mLast || gw <= 0.f || gh <= 0.f)
				continue;

			FSDFGlyph &g = mGlyphs[code - mFirst];
			g.u0 = gx / atlasW;
			g.v0 = gy / atlasH;
			g.u1 = (gx + gw) / atlasW;
			g.v1 = (gy + gh) / atlasH;
			g.advance = adv;
			g.present = true;
			glyphCount++;
		}
	}

	if (glyphCount == 0 || mCell <= 0.f)
	{
		Printf(TEXTCOLOR_RED "SDF font '%s': metrics lump found but unusable "
			"(%d glyphs, cell %g)\n", name, glyphCount, mCell);
		return false;
	}

	// A spread of zero would make the shader's halo term meaningless and is
	// almost certainly a generator that was interrupted. The field still
	// draws, so this is a warning rather than a rejection.
	if (mSpread <= 0.f)
	{
		Printf(TEXTCOLOR_YELLOW "SDF font '%s': no spread in metrics; glow will be flat\n", name);
	}

	// Say so, once. The fallback to bitmap glyphs is deliberately invisible in
	// the frame -- text still appears, just soft -- so without a line here the
	// only symptom of a missing atlas is "the letters look a bit off", which is
	// not something anyone diagnoses quickly. Caching means this prints once.
	Printf("SDF font '%s': %d glyphs, cell %g, spread %g\n",
		name, glyphCount, mCell, mSpread);
	return true;
}
