#ifndef GAME_MAP_DOCUMENT_TILES_H
#define GAME_MAP_DOCUMENT_TILES_H

#include <game/map/document/edit.h>
#include <game/map/document/layer.h>

#include <cstddef>
#include <string>
#include <vector>

struct _json_value;
typedef struct _json_value json_value;

namespace map_document
{
	class CDocument;

	/**
	 * A rectangle of tiles, in tiles, on one layer.
	 */
	class CTileRect
	{
	public:
		int m_X = 0;
		int m_Y = 0;
		int m_Width = 0;
		int m_Height = 0;

		bool Empty() const { return m_Width <= 0 || m_Height <= 0; }
		int Right() const { return m_X + m_Width; }
		int Bottom() const { return m_Y + m_Height; }
	};

	/** The rectangle cut down to what lies on the layer. */
	CTileRect ClipTileRect(const CTileLayer &Layer, const CTileRect &Rect);

	/**
	 * How a rectangle of tiles is written as text.
	 *
	 * Every tile is one token: the number that says what it is, and after a
	 * slash whatever else the tile carries - the flags of a drawn tile, the
	 * number of a tele, the number, delay and flags of a switch, the force,
	 * top speed and angle of a speedup. Trailing zeroes are left off, so a
	 * plain wall is `1` and air is `0`.
	 *
	 * - `rle`: one line per row, a token per run of equal tiles with `xN`
	 *   after it for a run longer than one. The cheapest by far for a model
	 *   to read, and the one to write with.
	 * - `rows`: one line per row, one token per tile.
	 * - `sparse`: `x,y:token` for every tile that is not air, with the
	 *   coordinates counted from the corner of the rectangle.
	 * - `glyph`: one character per tile, so that the shape of a physics layer
	 *   can be seen; the legend comes with it. Only for reading - it says
	 *   what a tile is and not what number it carries.
	 */
	enum class ETileEncoding
	{
		RLE,
		ROWS,
		SPARSE,
		GLYPH,
	};

	/**
	 * Reads an encoding by name.
	 *
	 * @param pName `rle`, `rows`, `sparse` or `glyph`.
	 * @param pOut Where the encoding goes.
	 *
	 * @return Whether the name is one of them.
	 */
	bool ReadTileEncoding(const char *pName, ETileEncoding *pOut);

	/** The name of an encoding, the way `ReadTileEncoding` reads it. */
	const char *TileEncodingName(ETileEncoding Encoding);

	/**
	 * What one tile is, whatever kind of layer it stands in.
	 *
	 * Four numbers, read the way the kind of layer reads them: index and
	 * flags for a drawn tile, type and number for a tele or tune tile, type,
	 * number, delay and flags for a switch, type, force, top speed and angle
	 * for a speedup. A tile that is zero throughout is air.
	 */
	class CTileValue
	{
	public:
		int m_aFields[4] = {0, 0, 0, 0};

		bool IsAir() const { return m_aFields[0] == 0 && m_aFields[1] == 0 && m_aFields[2] == 0 && m_aFields[3] == 0; }
		/** The number that says what the tile is - see `TileMeaning`. */
		int Index() const { return m_aFields[0]; }

		bool operator==(const CTileValue &Other) const
		{
			return m_aFields[0] == Other.m_aFields[0] && m_aFields[1] == Other.m_aFields[1] &&
			       m_aFields[2] == Other.m_aFields[2] && m_aFields[3] == Other.m_aFields[3];
		}
		bool operator!=(const CTileValue &Other) const { return !(*this == Other); }
	};

	/** One tile of a layer, read the way its kind reads it. */
	CTileValue GetTileValue(const CTileLayer &Layer, int x, int y);

	/** Puts one tile onto a layer, written the way its kind writes it. */
	void SetTileValue(CTileLayer &Layer, int x, int y, const CTileValue &Value);

	/** The token for one tile - see `ETileEncoding`. */
	std::string TileToken(const CTileValue &Value);

	/**
	 * Reads one token back into a tile.
	 *
	 * @param pToken The token, without a run suffix.
	 * @param Kind What kind of layer it is for, which says what the numbers
	 * mean and how large they may be.
	 * @param pOut Where the tile goes.
	 * @param pError What was wrong with it, if anything.
	 *
	 * @return Whether it was a token.
	 */
	bool ParseTileToken(const char *pToken, ETileLayerKind Kind, CTileValue *pOut, std::string *pError);

	/**
	 * A rectangle of a layer as text.
	 *
	 * @param Layer The layer to read.
	 * @param Rect Which part of it; what lies off the layer is air.
	 * @param Encoding How to write it.
	 * @param pLegend For `glyph`, where the legend goes: one line per glyph,
	 * `c=index name`. Left alone for the other encodings.
	 *
	 * @return The text.
	 */
	std::string EncodeTiles(const CTileLayer &Layer, const CTileRect &Rect, ETileEncoding Encoding, std::string *pLegend = nullptr);

	/**
	 * Reads text back into a brush.
	 *
	 * @param pText The text, in the encoding given.
	 * @param Encoding How it was written; `glyph` cannot be read back.
	 * @param Kind What kind of layer the brush is for.
	 * @param Width How wide the brush is, or 0 to take the width from the
	 * text - the longest row, or the rightmost sparse tile.
	 * @param Height How tall, or 0 to take it from the text.
	 * @param pBrush Where the brush goes.
	 * @param pError What was wrong with the text, if anything.
	 *
	 * @return Whether the text could be read.
	 */
	bool DecodeTiles(const char *pText, ETileEncoding Encoding, ETileLayerKind Kind, int Width, int Height, CBrush *pBrush, std::string *pError);

	/**
	 * Whether a tile of that index does anything in a layer of that kind.
	 *
	 * A drawn layer takes every index, because its tiles are a picture. A
	 * physics layer takes only what the game reads there - see
	 * `DropUnusedTiles`.
	 */
	bool TileIsUsed(ETileLayerKind Kind, int Index);

	/**
	 * Puts a brush onto a layer, tile by tile.
	 *
	 * @param Layer The layer to write on.
	 * @param x Where the top left corner of the brush goes.
	 * @param y The same, downwards.
	 * @param Brush What to write; it has to be of the layer's kind.
	 * @param Overlay Whether air in the brush leaves the layer as it was,
	 * rather than clearing it.
	 *
	 * @return How many tiles were written, air not counted when overlaying.
	 */
	int WriteTiles(CTileLayer &Layer, int x, int y, const CBrush &Brush, bool Overlay);

	/**
	 * Writes one tile over a rectangle, or over its rim.
	 *
	 * @param Layer The layer to write on.
	 * @param Rect Where; clipped to the layer.
	 * @param Value What to write.
	 * @param Border How thick a rim to write instead of the whole rectangle,
	 * or 0 for the whole of it.
	 *
	 * @return How many tiles were written.
	 */
	int FillTileRect(CTileLayer &Layer, const CTileRect &Rect, const CTileValue &Value, int Border = 0);

	/**
	 * Turns every tile of one index into another, keeping whatever else the
	 * tile carried.
	 *
	 * @param Layer The layer to change.
	 * @param Rect Where; clipped to the layer.
	 * @param From Which index to look for - what `TileMeaning` answers.
	 * @param To What it becomes.
	 *
	 * @return How many tiles were changed.
	 */
	int ReplaceTileIndex(CTileLayer &Layer, const CTileRect &Rect, int From, int To);

	/** A run of tiles of one index along a row. */
	class CTileRun
	{
	public:
		int m_X = 0;
		int m_Y = 0;
		int m_Length = 0;
		int m_Index = 0;
	};

	/**
	 * Every run of tiles of the given indices in a rectangle, in reading
	 * order.
	 *
	 * @param Layer The layer to look through.
	 * @param Rect Where; clipped to the layer.
	 * @param vIndices Which indices count.
	 * @param Limit How many runs to hand back at most.
	 * @param pTotal Where the number of runs there are goes, whether or not
	 * they all fit.
	 *
	 * @return The runs, at most `Limit` of them.
	 */
	std::vector<CTileRun> FindTileRuns(const CTileLayer &Layer, const CTileRect &Rect, const std::vector<int> &vIndices, size_t Limit, size_t *pTotal);

	/** How often one index occurs. */
	class CTileCount
	{
	public:
		int m_Index = 0;
		size_t m_Count = 0;
	};

	/** What a layer holds, counted. */
	class CTileStats
	{
	public:
		/** How many tiles are not air. */
		size_t m_Tiles = 0;
		/** The smallest rectangle around everything that is not air. */
		CTileRect m_Bounds;
		/** Every index that occurs, lowest first, air left out. */
		std::vector<CTileCount> m_vCounts;
	};

	/**
	 * Counts what a rectangle of a layer holds.
	 *
	 * @param Layer The layer to count.
	 * @param Rect Where; clipped to the layer.
	 *
	 * @return The counts.
	 */
	CTileStats CountTiles(const CTileLayer &Layer, const CTileRect &Rect);

	/** How many blocks of the layer's stores two versions do not share. */
	size_t ChangedChunks(const CTileLayer &Before, const CTileLayer &After);

	/**
	 * The `tiles.*` commands of `Apply`, which live here rather than in
	 * `command.cpp` because they are about the text a rectangle of tiles is
	 * turned into, and that is this file's business.
	 *
	 * | `op` | what it needs |
	 * |---|---|
	 * | `tiles.read` | `group`, `layer`, `x`, `y`, `w`, `h`; `encoding` (`rle`), `large` for more than 128 by 128 |
	 * | `tiles.write` | `group`, `layer`, `x`, `y`, `tiles`; `encoding`, `w`, `h`, `mode` (`replace` or `overlay`), `allowUnused` |
	 * | `tiles.fill` | `group`, `layer`, `x`, `y`, `w`, `h`, `index`; `flags`, `number`, `delay`, `force`, `maxSpeed`, `angle`, `border`, `allowUnused` |
	 * | `tiles.replace` | `from`, `to`; `group` and `layer` or `kind` for every layer of that kind; `x`, `y`, `w`, `h` |
	 * | `tiles.find` | `indices` or `index`; `group` and `layer` or `kind`; `x`, `y`, `w`, `h`, `limit` |
	 * | `tiles.stats` | `group` and `layer`, or nothing for every tile layer; `x`, `y`, `w`, `h` |
	 *
	 * @param Document The document to read or change.
	 * @param pCommand The command, parsed.
	 * @param pOp Its `op`, which starts with `tiles.`.
	 * @param pMerge Its `merge`, or null.
	 *
	 * @return What `Apply` returns.
	 */
	std::string ApplyTilesCommand(CDocument &Document, const json_value *pCommand, const char *pOp, const char *pMerge);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_TILES_H
