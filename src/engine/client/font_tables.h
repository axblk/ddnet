/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_FONT_TABLES_H
#define ENGINE_CLIENT_FONT_TABLES_H

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * The names and the character map of a TrueType or OpenType font file, read
 * the way FreeType reads them, for a rasterizer that has the glyphs drawn by
 * somebody who does not say which characters a font has.
 */
class CFontTables
{
	const uint8_t *m_pData = nullptr;
	size_t m_Size = 0;
	// The Unicode subtable of the character map FreeType chooses
	size_t m_CmapOffset = 0;
	int m_CmapFormat = -1;
	unsigned m_NumGlyphs = 0;
	std::string m_FamilyName;
	std::string m_StyleName;

	bool ReadNames(size_t Offset, size_t Length);
	bool ReadCmap(size_t Offset, size_t Length);

public:
	/**
	 * @return Whether the bytes are a font collection, which holds several
	 * fonts and is not read here.
	 */
	static bool IsCollection(const uint8_t *pData, size_t Size);

	/**
	 * Reads a font file that is not a collection.
	 *
	 * @param pData The bytes of the file, which must outlive this.
	 * @param Size The number of bytes.
	 *
	 * @return `true` if the file has names and a Unicode character map.
	 */
	bool Read(const uint8_t *pData, size_t Size);

	const char *FamilyName() const { return m_FamilyName.c_str(); }
	const char *StyleName() const { return m_StyleName.c_str(); }

	/**
	 * @param Character The Unicode code point.
	 *
	 * @return The glyph of the character, `0` if the font has none.
	 */
	unsigned GlyphIndex(int Character) const;
};

#endif
