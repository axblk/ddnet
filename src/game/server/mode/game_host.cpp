#include "game_host.h"

#include <base/dbg.h>
#include <base/log.h>

#include <game/server/gamecontroller.h>
#include <game/server/mode/game_mode_map_reload_state.h>

CGameHost::CGameHost(CGameContext *pGameServer) :
	m_Services(pGameServer)
{
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
	m_pController->Init(pDbPool);
}

void CGameHost::Shutdown()
{
	m_pController.reset();
}

void CGameHost::PrepareMapReloadState(std::unique_ptr<IGameModeMapReloadState> pState)
{
	m_pMapReloadState = std::move(pState);
	m_MapReloadStateFromPreviousContext = false;
}

std::unique_ptr<IGameModeMapReloadState> CGameHost::TakeMapReloadState()
{
	if(m_MapReloadStateFromPreviousContext)
	{
		m_pMapReloadState.reset();
		return nullptr;
	}
	return std::move(m_pMapReloadState);
}

void CGameHost::RestoreMapReloadState(std::unique_ptr<IGameModeMapReloadState> pState)
{
	m_pMapReloadState = std::move(pState);
	m_MapReloadStateFromPreviousContext = m_pMapReloadState != nullptr;
}

void CGameHost::DiscardMapReloadState(int ClientId)
{
	if(m_pMapReloadState)
		m_pMapReloadState->DiscardClient(ClientId);
}
