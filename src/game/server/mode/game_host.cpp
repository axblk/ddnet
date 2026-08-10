#include "game_host.h"

#include <base/dbg.h>
#include <base/log.h>

#include <game/server/gamecontroller.h>
#include <game/server/score.h>
#include <game/server/teams.h>

CGameHost::CGameHost(CGameContext *pGameServer) :
	m_pGameServer(pGameServer),
	m_Services(pGameServer)
{
	dbg_assert(m_pGameServer, "game host requires a game server");
}

CGameHost::~CGameHost() = default;

bool CGameHost::Select(const char *pModeId)
{
	if(m_pController)
		return false;

	std::unique_ptr<IGameController> pController = m_Modes.Create(pModeId, m_Services);
	if(!pController)
		return false;

	m_pController = std::move(pController);
	log_info("game", "selected mode id='%s' name='%s'", m_pController->Info().m_pId, m_pController->Info().m_pDisplayName);
	return true;
}

void CGameHost::Init(CDbConnectionPool *pDbPool)
{
	dbg_assert(m_pController, "cannot initialize game host without a selected mode");
	if(m_pController->UsesRaceTeams())
		m_pRaceTeams = std::make_unique<CGameTeams>(m_pGameServer, m_pController->TeamsCore());
	if(m_pController->UsesRaceScore())
	{
		dbg_assert(pDbPool, "race score service requires a database pool");
		m_pRaceScore = std::make_unique<CScore>(m_pGameServer, pDbPool);
	}
	m_pController->Init();
	if(m_pRaceTeams)
		m_pRaceTeams->Reset();
}

void CGameHost::Shutdown()
{
	m_pRaceScore.reset();
	m_pRaceTeams.reset();
	m_pController.reset();
}
