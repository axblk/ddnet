#ifndef GAME_MAP_DOCUMENT_REPORT_H
#define GAME_MAP_DOCUMENT_REPORT_H

#include <cstddef>
#include <string>

namespace map_document
{
	class CDocument;
	class CMapState;

	/**
	 * What the map is made of, as JSON, for the shadow copy the interface
	 * keeps of it.
	 *
	 * The interface draws its panels out of this and nothing else: a layer
	 * tree, the properties of whatever is selected, the list of images. What
	 * is *in* a layer is not here - a tile layer says how large it is, a quad
	 * layer how many quads it has, and the tiles and the quads themselves are
	 * asked for one at a time or drawn by the renderer. A map with four
	 * million tiles would otherwise be a JSON text of forty megabytes after
	 * every stroke.
	 *
	 * Envelope points are left out for the same reason - a long envelope has
	 * hundreds of them and nearly nothing asks for them - and are asked for
	 * one envelope at a time with `EnvelopeJson`.
	 *
	 * @param Map The version to describe.
	 *
	 * @return The JSON text, which is an object.
	 */
	std::string StructureJson(const CMapState &Map);

	/**
	 * The points of one envelope, for the panel that draws it.
	 *
	 * They go out as whole numbers and come back as whole numbers, because
	 * that is what they are: a time is whole milliseconds and a value is the
	 * map's own 22.10 fixed point. Nothing is turned into a fraction on the
	 * way, so nothing is lost on the way back - which is what an editor that
	 * writes the file again needs.
	 *
	 * What a value means is a question about the channels: four are a colour,
	 * three a place and a turn, one a volume. The panel knows that; the
	 * document holds what the file holds.
	 *
	 * @param Map The version to read.
	 * @param Index Which envelope of it.
	 *
	 * @return The JSON text, which is an object, or `null` where there is no
	 * such envelope.
	 */
	std::string EnvelopeJson(const CMapState &Map, size_t Index);

	/**
	 * The history as the history panel shows it: what was done, when, where
	 * in it the map stands, and what it all costs.
	 *
	 * @param Document The document whose history to describe.
	 *
	 * @return The JSON text, which is an object.
	 */
	std::string HistoryJson(const CDocument &Document);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_REPORT_H
