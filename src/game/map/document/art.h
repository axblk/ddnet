#ifndef GAME_MAP_DOCUMENT_ART_H
#define GAME_MAP_DOCUMENT_ART_H

#include "document.h"
#include "structure.h"

#include <cstddef>
#include <cstdint>

namespace map_document
{
	/**
	 * A picture, turned into map.
	 *
	 * Two ways of doing it, and which one is wanted depends on what the
	 * picture is. A picture becomes *tiles* by being made its own tileset:
	 * every colour in it gets a tile of that colour, and the layer is those
	 * tiles. It becomes *quads* by being one quad per pixel. The first keeps
	 * the map's grid and costs a picture; the second costs nothing but quads,
	 * of which there can be a great many.
	 *
	 * Both of them read pixels rather than a file. A browser decodes a PNG and
	 * the map already keeps RGBA, so a decoder here would be a second one.
	 */

	/** How many colours one palette picture holds, one of them being none. */
	constexpr int ART_PALETTE_SIZE = 256;

	/**
	 * Makes a group of tile layers that draw the picture.
	 *
	 * Each layer is drawn with a palette of its own: a 16-by-16 sheet whose
	 * tiles are the flat colours of the picture, so tile 7 of the sheet is
	 * the seventh colour. Tile 0 is nothing, which is what a pixel that is
	 * not opaque becomes. A picture of more than 255 colours needs more than
	 * one sheet, and gets a layer for each - together they are the picture,
	 * one layer over the next.
	 *
	 * @param Doc The document being changed.
	 * @param pName What to call the group, the layers and the palettes.
	 * @param Width How wide the picture is, in pixels.
	 * @param Height How tall.
	 * @param pPixels RGBA, `Width * Height * 4` bytes.
	 *
	 * @return Which group of the map it became.
	 */
	size_t AddTileArt(CDocument &Doc, const char *pName, int Width, int Height, const uint8_t *pPixels);

	/** How many colours a picture holds, so a page can warn before it asks. */
	size_t CountArtColors(int Width, int Height, const uint8_t *pPixels);

	/** What to make of the picture when every pixel becomes a quad. */
	class CQuadArtOptions
	{
	public:
		/** How many pixels of the picture one quad stands for; 1 is all of them. */
		int m_PixelStep = 1;
		/** How wide a quad is on the map, in world units. */
		int m_QuadSize = 64;
		/** Whether every quad turns about the same place rather than its own. */
		bool m_Centralize = false;
		/**
		 * Whether a run of pixels of one colour becomes one quad rather than
		 * one each. A picture with flat areas becomes very much smaller; a
		 * photograph does not become smaller at all.
		 */
		bool m_Merge = true;
	};

	/**
	 * Makes a group with one quad layer that draws the picture in quads.
	 *
	 * A pixel that is not opaque becomes nothing. The group clips to what was
	 * drawn, so a picture put on a map does not spread over the rest of it.
	 *
	 * @param Doc The document being changed.
	 * @param pName What to call the group and the layer.
	 * @param Width How wide the picture is, in pixels.
	 * @param Height How tall.
	 * @param pPixels RGBA, `Width * Height * 4` bytes.
	 * @param Options What to make of it.
	 *
	 * @return Which group of the map it became.
	 */
	size_t AddQuadArt(CDocument &Doc, const char *pName, int Width, int Height, const uint8_t *pPixels,
		const CQuadArtOptions &Options);

	/**
	 * Where the letters and the digits sit on a font tileset.
	 *
	 * A font tileset is a tileset like any other; what makes it a font is that
	 * `A` is at 1 and `1` is at 54, which is a convention of the tilesets
	 * people draw rather than anything the file format knows. The editor in
	 * the client types by the same two numbers.
	 */
	constexpr int FONT_LETTER_TILE = 1;
	constexpr int FONT_DIGIT_TILE = 54;

	/**
	 * Writes text into a tile layer as the tiles of a font tileset.
	 *
	 * Letters and digits become tiles, a space becomes nothing, and a newline
	 * goes down a row and back to the column it started in - so a block of
	 * text stays a block. A line that reaches the right-hand edge wraps the
	 * same way. Anything else is passed over: a font tileset has 26 letters
	 * and ten digits and nothing else, and refusing a comma would be refusing
	 * the sentence it stands in.
	 *
	 * The editor in the client does this a keystroke at a time in a mode of
	 * its own. A page has text fields, so here it is a text and one history
	 * entry - which is also the only version that can be undone in one go.
	 *
	 * @param Doc The document being changed.
	 * @param Layer Which layer, which has to hold tiles.
	 * @param x Where to start, in tiles.
	 * @param y Where to start.
	 * @param pText The text.
	 *
	 * @return How many tiles it wrote.
	 */
	int TypeText(CDocument &Doc, const CLayerAddress &Layer, int x, int y, const char *pText);
} // namespace map_document

#endif
