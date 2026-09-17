#ifndef GAME_MAP_MCP_MCP_ABI_H
#define GAME_MAP_MCP_MCP_ABI_H

/*
 * The map tools behind a C interface, for whatever speaks the protocol.
 *
 * Strings in, strings out: every argument is UTF-8 JSON or plain text, every
 * answer is a JSON text that the caller frees with `ddnet_map_mcp_free`. The
 * answers of the calls that can fail are `{"result": ...}` or
 * `{"error": {"code": -32602, "message": "..."}}`, the codes being the
 * JSON-RPC ones the protocol uses.
 *
 * One server holds the open maps. Its calls may come from any thread and at
 * the same time: they are queued to one worker thread that owns the maps and
 * the renderer, and run one after the other - the document is single-threaded,
 * and so is the graphics backend the renderer draws with.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ddnet_map_mcp ddnet_map_mcp; // NOLINT(readability-identifier-naming)

/**
 * Starts a server.
 *
 * @param pOptionsJson An object: `root` (directory, required), `historyMb`,
 * `historyEntries`, `maxMaps`, `sequentialHandles`, `idleSeconds`, `render`
 * (whether to draw pictures; needs the game's data and a Vulkan driver, which
 * may be a software one) and `args` (the program's arguments, for finding the
 * game's data directory the way every program does).
 * @param ppErrorOut Where a message goes when nothing is returned; to be freed
 * with `ddnet_map_mcp_free`. May be null.
 *
 * @return The server, or null.
 */
ddnet_map_mcp *ddnet_map_mcp_create(const char *pOptionsJson, char **ppErrorOut);

/** Stops the server and closes every map, saved or not. */
void ddnet_map_mcp_destroy(ddnet_map_mcp *pServer);

/** `{"name", "version", "instructions"}`. */
char *ddnet_map_mcp_info(ddnet_map_mcp *pServer);

/** The tools as `tools/list` lists them: a JSON array. */
char *ddnet_map_mcp_tools(ddnet_map_mcp *pServer);

/**
 * One tool call.
 *
 * @param pServer The server.
 * @param pName Which tool.
 * @param pArgumentsJson Its arguments, a JSON object, or null for none.
 * @param pMetaJson The request's `_meta`, or null; kept for what a later
 * version may want to know, unread today.
 *
 * @return `{"result": {"content": [...], "structuredContent": ..., "isError": ...}}`,
 * or `{"error": ...}` for a tool that does not exist.
 */
char *ddnet_map_mcp_call(ddnet_map_mcp *pServer, const char *pName, const char *pArgumentsJson, const char *pMetaJson);

/** The resources, a JSON array. */
char *ddnet_map_mcp_resources(ddnet_map_mcp *pServer);

/** The resource templates, a JSON array. */
char *ddnet_map_mcp_resource_templates(ddnet_map_mcp *pServer);

/** `{"result": {"contents": [...]}}` or `{"error": ...}`. */
char *ddnet_map_mcp_read_resource(ddnet_map_mcp *pServer, const char *pUri);

/** The prompts, a JSON array. */
char *ddnet_map_mcp_prompts(ddnet_map_mcp *pServer);

/** `{"result": {"description": ..., "messages": [...]}}` or `{"error": ...}`. */
char *ddnet_map_mcp_get_prompt(ddnet_map_mcp *pServer, const char *pName, const char *pArgumentsJson);

/** Closes the maps nobody has used for longer than `idleSeconds`. */
void ddnet_map_mcp_close_idle(ddnet_map_mcp *pServer);

/** Frees any string this interface handed out. */
void ddnet_map_mcp_free(char *pText);

#ifdef __cplusplus
}
#endif

#endif // GAME_MAP_MCP_MCP_ABI_H
