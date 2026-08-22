#include "session_game_config.h"

#include <utility>

CSessionGameConfig::CSessionGameConfig(const CConfig &BaseValues) :
	m_pConsole(CreateConsole(CFGFLAG_GAME))
{
	const auto AddInt = [this](const char *pName, int *pValue, int Default, int Min, int Max) {
		auto pVariable = std::make_unique<SIntConfigVariable>(m_pConsole.get(), pName, SConfigVariable::VAR_INT, CFGFLAG_GAME, "Session-local game setting", pValue, Default, Min, Max);
		pVariable->Register();
		m_vpVariables.push_back(std::move(pVariable));
	};
	AddInt("sv_hit", &m_SvHit, DefaultConfig::SvHit, 0, 1);
	AddInt("sv_deepfly", &m_SvDeepfly, DefaultConfig::SvDeepfly, 0, 1);
	AddInt("sv_freeze_delay", &m_SvFreezeDelay, DefaultConfig::SvFreezeDelay, 1, 30);
	AddInt("sv_dragger_range", &m_SvDraggerRange, DefaultConfig::SvDraggerRange, 1, 99999);
	AddInt("sv_old_laser", &m_SvOldLaser, DefaultConfig::SvOldLaser, 0, 1);
	AddInt("sv_team", &m_SvTeam, DefaultConfig::SvTeam, 0, 3);
	AddInt("sv_min_team_size", &m_SvMinTeamSize, DefaultConfig::SvMinTeamSize, 1, SERVER_MAX_CLIENTS);
	AddInt("sv_max_team_size", &m_SvMaxTeamSize, DefaultConfig::SvMaxTeamSize, 1, SERVER_MAX_CLIENTS);
	AddInt("sv_solo_server", &m_SvSoloServer, DefaultConfig::SvSoloServer, 0, 1);

	m_pConsole->SetUnknownCommandCallback([](const char *, void *) { return true; }, nullptr);
	Reset(BaseValues);
}

void CSessionGameConfig::Reset(const CConfig &BaseValues)
{
	m_SvHit = BaseValues.m_SvHit;
	m_SvDeepfly = BaseValues.m_SvDeepfly;
	m_SvFreezeDelay = BaseValues.m_SvFreezeDelay;
	m_SvDraggerRange = BaseValues.m_SvDraggerRange;
	m_SvOldLaser = BaseValues.m_SvOldLaser;
	m_SvTeam = BaseValues.m_SvTeam;
	m_SvMinTeamSize = BaseValues.m_SvMinTeamSize;
	m_SvMaxTeamSize = BaseValues.m_SvMaxTeamSize;
	m_SvSoloServer = BaseValues.m_SvSoloServer;
}

void CSessionGameConfig::ExecuteLine(const char *pLine)
{
	m_pConsole->ExecuteLine(pLine, IConsole::CLIENT_ID_GAME);
}
