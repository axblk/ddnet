#ifndef GAME_CLIENT_SESSION_GAME_CONFIG_H
#define GAME_CLIENT_SESSION_GAME_CONFIG_H

#include <engine/console.h>
#include <engine/shared/config.h>

#include <memory>
#include <vector>

// The global configuration with the game settings of a session's map applied.
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
