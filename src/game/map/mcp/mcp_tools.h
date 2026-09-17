#ifndef GAME_MAP_MCP_MCP_TOOLS_H
#define GAME_MAP_MCP_MCP_TOOLS_H

#include <cstddef>

namespace map_mcp
{
	/** What `ddnet://docs/encodings` says, which tiles.read points at too. */
	const char *EncodingsDoc();

	/**
	 * The width and height out of a PNG's header.
	 *
	 * @param pData The file.
	 * @param Size How long it is.
	 * @param pWidth Where the width goes.
	 * @param pHeight Where the height goes.
	 *
	 * @return Whether the file starts like a PNG.
	 */
	bool ReadPngSize(const unsigned char *pData, size_t Size, int *pWidth, int *pHeight);
} // namespace map_mcp

#endif // GAME_MAP_MCP_MCP_TOOLS_H
