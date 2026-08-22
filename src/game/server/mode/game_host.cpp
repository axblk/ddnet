#include "game_host.h"

#include <base/dbg.h>
#include <base/log.h>

#include <game/server/gamecontroller.h>
#include <game/server/mode/game_mode_map_reload_state.h>
#include <game/server/mode/game_mode_registry.h>

CGameHost::CGameHost(CGameContext *pGameServer) :
	m_Services(pGameServer)
{
}

CGameHost::~CGameHost() = default;

bool CGameHost::Select(const char *pName)
{
	std::unique_ptr<IGameController> pController = CreateGameController(pName, m_Services);
	if(!pController)
		return false;
	Select(std::move(pController));
	return true;
}

void CGameHost::Select(std::unique_ptr<IGameController> pController)
{
	dbg_assert(!m_pController, "a mode is already selected");
	m_pController = std::move(pController);
	log_info("game", "selected game type '%s'", m_pController->Info().m_pGameType);
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
