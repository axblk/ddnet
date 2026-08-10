#include "mod.h"

CGameControllerMod::CGameControllerMod(class CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	CGameControllerDDRace(pGameServer, GameModeInfo)
{
}

CGameControllerMod::~CGameControllerMod() = default;
