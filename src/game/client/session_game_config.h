#ifndef GAME_CLIENT_SESSION_GAME_CONFIG_H
#define GAME_CLIENT_SESSION_GAME_CONFIG_H

#include <engine/console.h>
#include <engine/shared/config.h>

#include <memory>
#include <vector>

class CSessionGameConfig
{
	std::unique_ptr<IConsole> m_pConsole;
	std::vector<std::unique_ptr<SConfigVariable>> m_vpVariables;

public:
	int m_SvHit = DefaultConfig::SvHit;
	int m_SvDeepfly = DefaultConfig::SvDeepfly;
	int m_SvFreezeDelay = DefaultConfig::SvFreezeDelay;
	int m_SvDraggerRange = DefaultConfig::SvDraggerRange;
	int m_SvOldLaser = DefaultConfig::SvOldLaser;
	int m_SvTeam = DefaultConfig::SvTeam;
	int m_SvMinTeamSize = DefaultConfig::SvMinTeamSize;
	int m_SvMaxTeamSize = DefaultConfig::SvMaxTeamSize;
	int m_SvSoloServer = DefaultConfig::SvSoloServer;

	explicit CSessionGameConfig(const CConfig &BaseValues);

	CSessionGameConfig(const CSessionGameConfig &) = delete;
	CSessionGameConfig &operator=(const CSessionGameConfig &) = delete;

	void Reset(const CConfig &BaseValues);
	void ExecuteLine(const char *pLine);
	IConsole *Console() { return m_pConsole.get(); }
};

#endif // GAME_CLIENT_SESSION_GAME_CONFIG_H
