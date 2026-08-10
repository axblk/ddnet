#ifndef GAME_SERVER_MODE_GAME_HOST_H
#define GAME_SERVER_MODE_GAME_HOST_H

#include "game_mode_registry.h"

#include <memory>

class CGameContext;
class IGameController;

class CGameHost
{
public:
	explicit CGameHost(CGameContext *pGameServer);

	CGameModeRegistry &Modes() { return m_Modes; }
	const CGameModeRegistry &Modes() const { return m_Modes; }

	bool Select(const char *pModeId);
	void Init();
	void Shutdown();

	IGameController *Controller() const { return m_pController.get(); }

private:
	CGameContext *m_pGameServer;
	CGameModeRegistry m_Modes;
	std::unique_ptr<IGameController> m_pController;
};

#endif // GAME_SERVER_MODE_GAME_HOST_H
