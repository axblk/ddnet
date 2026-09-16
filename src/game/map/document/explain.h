#ifndef GAME_MAP_DOCUMENT_EXPLAIN_H
#define GAME_MAP_DOCUMENT_EXPLAIN_H

#include "layer.h"

namespace map_document
{
	/**
	 * What a tile of a physics layer does, in a sentence.
	 *
	 * The sentences are the ones the editor in the client shows
	 * (`CExplanations`), which is the point: somebody who learned what a tile
	 * does in one editor should not be told something else in the other. They
	 * moved out of `game/editor/` for that reason and nothing else - they were
	 * always only a table about `game/mapitems.h`.
	 *
	 * A plain tile layer has nothing to explain. Its tiles are a picture, and
	 * what a picture means is the mapper's business.
	 *
	 * @param Kind Which physics layer the tile stands in.
	 * @param Index The tile, 0 to 255.
	 *
	 * @return The sentence, or `nullptr` where there is nothing to say.
	 */
	const char *ExplainTile(ETileLayerKind Kind, int Index);

	/**
	 * The number that says what a tile *is*, whichever kind of layer it
	 * stands in.
	 *
	 * The three kinds that draw their own tiles keep it where it is drawn
	 * from; a physics layer draws nothing, so its plane of tiles is air and
	 * the meaning sits beside it. Which of the two is the file format's
	 * business, and everything that wants to know what a tile is would
	 * otherwise have to know it.
	 *
	 * @param Layer The layer to read.
	 * @param x Where, in tiles.
	 * @param y Where, in tiles.
	 *
	 * @return The number, or -1 for a place outside the layer.
	 */
	int TileMeaning(const CTileLayer &Layer, int x, int y);
} // namespace map_document

#endif
