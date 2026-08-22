#include "mod.h"

CGameControllerMod::CGameControllerMod(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	CGameControllerDDRace(Services, GameModeInfo)
{
}

CGameControllerMod::~CGameControllerMod() = default;
