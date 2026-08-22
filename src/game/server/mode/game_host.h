#ifndef GAME_SERVER_MODE_GAME_HOST_H
#define GAME_SERVER_MODE_GAME_HOST_H

#include "game_mode_registry.h"
#include "game_services.h"

#include <memory>

class CGameContext;
class CDbConnectionPool;
class IGameController;
class IGameModeMapReloadState;

class CGameHost
{
public:
	explicit CGameHost(CGameContext *pGameServer);
	~CGameHost();

	CGameModeRegistry &Modes() { return m_Modes; }
	const CGameModeRegistry &Modes() const { return m_Modes; }

	bool Select(const char *pModeId);
	void Init(CDbConnectionPool *pDbPool);
	void Shutdown();

	IGameController *Controller() const { return m_pController.get(); }
	CGameServices &Services() { return m_Services; }
	const CGameServices &Services() const { return m_Services; }
	IGameModeMapReloadState *MapReloadState() const { return m_pMapReloadState.get(); }
	void PrepareMapReloadState(std::unique_ptr<IGameModeMapReloadState> pState);
	std::unique_ptr<IGameModeMapReloadState> TakeMapReloadState();
	void RestoreMapReloadState(std::unique_ptr<IGameModeMapReloadState> pState);
	void DiscardMapReloadState(int ClientId);

private:
	CGameContext *m_pGameServer;
	CGameServices m_Services;
	CGameModeRegistry m_Modes;
	std::unique_ptr<IGameController> m_pController;
	std::unique_ptr<IGameModeMapReloadState> m_pMapReloadState;
	bool m_MapReloadStateFromPreviousContext = false;
};

#endif // GAME_SERVER_MODE_GAME_HOST_H
