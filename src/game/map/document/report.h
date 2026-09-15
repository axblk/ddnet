#ifndef GAME_MAP_DOCUMENT_REPORT_H
#define GAME_MAP_DOCUMENT_REPORT_H

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
	 * Envelope points are left out for the same reason and a second one: they
	 * are floating-point, and a number that goes through text loses exactly
	 * the bits an envelope editor would put back. The envelope panel asks for
	 * its points as numbers.
	 *
	 * @param Map The version to describe.
	 *
	 * @return The JSON text, which is an object.
	 */
	std::string StructureJson(const CMapState &Map);

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
