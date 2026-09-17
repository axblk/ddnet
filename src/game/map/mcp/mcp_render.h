#ifndef GAME_MAP_MCP_MCP_RENDER_H
#define GAME_MAP_MCP_MCP_RENDER_H

#include <game/map/mcp/mcp_core.h>

#include <memory>
#include <string>

namespace map_mcp
{
	/**
	 * A renderer that draws into a surface on no screen.
	 *
	 * The window is opened at the first picture and kept, so that the first
	 * picture pays for the driver and the ones after it do not. Everything
	 * about it - the graphics, the textures, the version being drawn - lives
	 * on the thread that asked for the first picture, which has to be the
	 * one that asks for all the others.
	 *
	 * @param NumArgs The program's arguments, for finding the game's data.
	 * @param ppArguments The same.
	 * @param pError Why there is no renderer, if there is none.
	 *
	 * @return The renderer, or null.
	 */
	std::unique_ptr<IRenderer> CreateHeadlessRenderer(int NumArgs, const char **ppArguments, std::string *pError);
} // namespace map_mcp

#endif // GAME_MAP_MCP_MCP_RENDER_H
