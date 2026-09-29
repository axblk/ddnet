#ifndef ENGINE_SERVER_CONFIG_SCHEMA_H
#define ENGINE_SERVER_CONFIG_SCHEMA_H

#include <string>
#include <vector>

/**
 * When a change of a config variable takes effect on a running server.
 */
enum class EConfigApply
{
	// Read when it is needed, or applied right away by a chain.
	LIVE,
	// Read when a map is loaded, so it applies from the next map change or `reload`.
	MAP_LOAD,
	// Read only when the server starts.
	RESTART,
};

/**
 * When a change of a server variable takes effect, from where the server reads
 * it: every `CFGFLAG_GAME` variable at map load, the variables in the lists of
 * `config_schema.cpp` as listed there, and everything else live.
 *
 * @param pName The name of the variable.
 * @param Flags Its flags.
 */
EConfigApply ConfigApply(const char *pName, int Flags);

/**
 * The names in the lists behind `ConfigApply`, for tests to check that they
 * name variables.
 *
 * @return The names of the variables that apply at map load or at restart.
 */
std::vector<const char *> ConfigApplyListedNames();

/**
 * The words a string variable takes, when it takes nothing else, such as
 * `sv_map_convert`. The schema lists them as its `enum`.
 *
 * @param pName The name of the variable.
 *
 * @return The words, empty for a variable that takes any string.
 */
std::vector<const char *> ConfigEnumValues(const char *pName);

/**
 * Describes what a server binary understands, as a JSON document: its version
 * and build, every server and econ variable with its type, default, range,
 * flags, when a change applies and whether it is chained, every other command
 * with its parameters, flags and default access level, and the game types.
 * A command that a game type adds while it runs, such as the DDRace commands,
 * names the game types that have it in `gametypes`. A command's `access` is
 * what the binary gives it: `access_level` in a config changes it (the
 * shipped `autoexec_server.cfg` lets helpers use `status` and `status_json`),
 * and `access_status` tells what a running server has.
 *
 * The server is built as far as registering its commands, and the controller
 * of each game type as far as registering its own, which opens no socket and
 * reads or writes no file. The output depends only on the binary, lists are
 * sorted by name.
 *
 * @return The document, ending with a line break.
 */
std::string ServerConfigSchema();

#endif
