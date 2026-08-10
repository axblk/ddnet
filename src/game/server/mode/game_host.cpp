#include "game_host.h"

#include <base/dbg.h>
#include <base/log.h>

#include <game/server/gamecontroller.h>

CGameHost::CGameHost(CGameContext *pGameServer) :
	m_pGameServer(pGameServer)
{
	dbg_assert(m_pGameServer, "game host requires a game server");
}

bool CGameHost::Select(const char *pModeId)
{
	if(m_pController)
		return false;

	std::unique_ptr<IGameController> pController = m_Modes.Create(pModeId, m_pGameServer);
	if(!pController)
		return false;

	m_pController = std::move(pController);
	log_info("game", "selected mode id='%s' name='%s'", m_pController->Info().m_pId, m_pController->Info().m_pDisplayName);
	return true;
}

void CGameHost::Init()
{
	dbg_assert(m_pController, "cannot initialize game host without a selected mode");
	m_pController->Init();
}

void CGameHost::Shutdown()
{
	m_pController.reset();
}
