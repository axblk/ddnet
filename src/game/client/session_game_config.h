#ifndef GAME_CLIENT_SESSION_GAME_CONFIG_H
#define GAME_CLIENT_SESSION_GAME_CONFIG_H

#include <engine/console.h>
#include <engine/shared/config.h>

#include <memory>
#include <vector>

/**
 * The game settings of one session.
 *
 * A map carries settings that both sides have to read the same way, and every
 * session has a map of its own. This holds a copy of the global configuration
 * that the map's settings are applied to, so that one session cannot change
 * what another one predicts.
 *
 * Which settings a map may set is not written down here: it is the set marked
 * `CFGFLAG_GAME` in `config_variables.h`, and that is where this reads it from.
 */
class CSessionGameConfig
{
	std::unique_ptr<IConsole> m_pConsole;
	std::vector<std::unique_ptr<SConfigVariable>> m_vpVariables;
	CConfig m_Values;

public:
	explicit CSessionGameConfig(const CConfig &BaseValues);

	CSessionGameConfig(const CSessionGameConfig &) = delete;
	CSessionGameConfig &operator=(const CSessionGameConfig &) = delete;

	void Reset(const CConfig &BaseValues) { m_Values = BaseValues; }
	void ExecuteLine(const char *pLine);
	IConsole *Console() { return m_pConsole.get(); }
	const CConfig &Values() const { return m_Values; }
};

#endif // GAME_CLIENT_SESSION_GAME_CONFIG_H
