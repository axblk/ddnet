/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_GLYPH_RASTERIZER_H
#define ENGINE_CLIENT_GLYPH_RASTERIZER_H

#include <base/vmath.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

/**
 * One face of a font file, as the rasterizer that loaded it knows it. The
 * rasterizer owns it.
 */
class CFontFace
{
public:
	virtual ~CFontFace() = default;

	virtual const char *FamilyName() const = 0;
	virtual const char *StyleName() const = 0;

	/**
	 * @param Character The Unicode code point.
	 *
	 * @return The face's glyph for the character, `0` if it has none.
	 */
	virtual unsigned GlyphIndex(int Character) const = 0;
};

/**
 * Where the pixels of a glyph lie relative to the pen, in whole pixels.
 */
class CGlyphMetrics
{
public:
	/**
	 * The size of the glyph's bitmap, `0` for a glyph without ink.
	 */
	int m_Width = 0;
	int m_Height = 0;
	/**
	 * How far right of the pen the bitmap's left edge is.
	 */
	int m_OffsetX = 0;
	/**
	 * How far above the baseline the bitmap's bottom edge is, negative for
	 * glyphs that reach below it.
	 */
	int m_OffsetY = 0;
	/**
	 * How far the pen moves on.
	 */
	int m_AdvanceX = 0;
};

/**
 * A measured glyph whose pixels are wanted.
 */
class CGlyphRaster
{
public:
	const CFontFace *m_pFace;
	unsigned m_GlyphIndex;
	int m_Character;
	int m_FontSize;
	CGlyphMetrics m_Metrics;
	/**
	 * Where the coverage goes: one byte per pixel, `m_Metrics.m_Width` of
	 * them per row, `m_Metrics.m_Height` rows.
	 */
	uint8_t *m_pPixels;
};

/**
 * What turns the characters of font files into pixels for the text renderer:
 * FreeType, or the browser's own fonts (`WEB_GLYPHS`). The text renderer lays
 * out the glyphs and keeps them in its atlas, and asks this for the faces a
 * font file has, what a face covers, the metrics of a glyph and its pixels.
 */
class IGlyphRasterizer
{
public:
	virtual ~IGlyphRasterizer() = default;

	/**
	 * Logs what draws the glyphs, once at startup.
	 */
	virtual void LogVersion() const = 0;

	/**
	 * Loads the faces of a font file, which the rasterizer keeps from then on.
	 *
	 * @param vData The bytes of the file, which the faces may point into.
	 * @param pPath The path of the file, for messages.
	 * @param vpFaces The loaded faces are appended to this.
	 *
	 * @return `true` if at least one face was loaded.
	 */
	virtual bool LoadFaces(std::vector<uint8_t> &&vData, const char *pPath, std::vector<CFontFace *> &vpFaces) = 0;

	/**
	 * A face that stands for the fonts of the system, which covers every
	 * character. The text renderer then leaves out the fallback and language
	 * variant font files, whose characters the system's fonts draw.
	 *
	 * @return The face, `nullptr` if characters come from font files only.
	 */
	virtual const CFontFace *SystemFace() const = 0;

	/**
	 * Measures a glyph. Its pixels come with `Rasterize`.
	 *
	 * @param pFace The face.
	 * @param GlyphIndex The glyph of the face, what `CFontFace::GlyphIndex` said for the character.
	 * @param Character The character, for messages and for rasterizers that draw characters.
	 * @param FontSize The font size in pixels.
	 * @param Metrics Filled with the glyph's metrics.
	 *
	 * @return `true` on success.
	 */
	virtual bool Measure(const CFontFace *pFace, unsigned GlyphIndex, int Character, int FontSize, CGlyphMetrics &Metrics) = 0;

	/**
	 * Whether glyphs are best rasterized many at once. The text renderer then
	 * collects the glyphs it measures and hands them over together, before it
	 * next draws text. Otherwise each glyph is rasterized right after it was
	 * measured.
	 */
	virtual bool RasterizesInBatches() const = 0;

	/**
	 * Draws the coverage of measured glyphs.
	 *
	 * @param vGlyphs The glyphs, with the metrics `Measure` gave them. Their pixels start out zero.
	 *
	 * @return `true` if every glyph was drawn.
	 */
	virtual bool Rasterize(std::span<const CGlyphRaster> vGlyphs) = 0;

	/**
	 * @param pFace The face of both characters.
	 * @param FontSize The font size in pixels.
	 * @param Left The character on the left.
	 * @param Right The character on the right.
	 *
	 * @return How much closer the right character moves to the left one, in pixels.
	 */
	virtual vec2 Kerning(const CFontFace *pFace, int FontSize, int Left, int Right) = 0;

	/**
	 * The ink of a character without drawing it, for text that is drawn into
	 * textures.
	 *
	 * @param pFace The face.
	 * @param Character The character.
	 * @param FontWidth The width of the font in pixels, `0` for as wide as it is high.
	 * @param FontHeight The font size in pixels.
	 * @param Width Set to the width of the ink.
	 * @param BearingX Set to how far right of the pen the ink starts.
	 *
	 * @return `true` on success.
	 */
	virtual bool InkBox(const CFontFace *pFace, int Character, int FontWidth, int FontHeight, int &Width, int &BearingX) = 0;
};

/**
 * @return The rasterizer the build was made with.
 */
std::unique_ptr<IGlyphRasterizer> CreateGlyphRasterizer();

#endif
