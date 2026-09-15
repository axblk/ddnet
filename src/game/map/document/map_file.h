#ifndef GAME_MAP_DOCUMENT_MAP_FILE_H
#define GAME_MAP_DOCUMENT_MAP_FILE_H

#include <game/map/document/map_state.h>

#include <string>
#include <vector>

class CDataFileReader;
class CDataFileWriter;

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

	/**
	 * Writes a version of a document out as a map file.
	 *
	 * The file is left open: whoever asked for it decides when it is finished
	 * and where it lands, because that is a matter of jobs and temporary
	 * names rather than of the map.
	 *
	 * What comes out is what the state holds, item for item, in the order the
	 * editor has always written them - so a map that was read and written
	 * again is the same map, and a second writing of it is the same bytes.
	 */
	void WriteMapState(CDataFileWriter &File, const CMapState &State);
} // namespace map_document

#endif // GAME_MAP_DOCUMENT_MAP_FILE_H
