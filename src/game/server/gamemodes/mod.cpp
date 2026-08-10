#include "mod.h"

CGameControllerMod::CGameControllerMod(class CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	IGameController(pGameServer, GameModeInfo)
{
}

CGameControllerMod::~CGameControllerMod() = default;

void CGameControllerMod::Tick()
{
	// this is the main part of the gamemode, this function is run every tick

	IGameController::Tick();
}
