#ifndef GAME_MAP_DOCUMENT_MAP_FILE_H
#define GAME_MAP_DOCUMENT_MAP_FILE_H

#include <game/map/document/map_state.h>

#include <string>
#include <vector>

class CDataFileReader;

namespace map_document
{
	/**
	 * Reads a map file into one version of a document.
	 *
	 * What is read is what the file holds, not what it means: the tiles of a
	 * tele layer are the tiles the file has for it, which in a file written
	 * by the editor of today are all air, because the indexes that are drawn
	 * are worked out from the second plane when the layer is drawn. Reading
	 * it any other way would make the map that was read a different map from
	 * the one on disk.
	 *
	 * A file that is broken in a way that can be lived with is read as far as
	 * it goes, and every place it was stood in for is put in `vWarnings`; a
	 * file that cannot be read at all answers false. The state is left half
	 * built in that case and should be thrown away.
	 */
	bool ReadMapState(CDataFileReader &File, CMapState *pState, std::vector<std::string> *pvWarnings);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_MAP_FILE_H
