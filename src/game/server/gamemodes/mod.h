#ifndef GAME_SERVER_GAMEMODES_MOD_H
#define GAME_SERVER_GAMEMODES_MOD_H

#include "ddrace.h"

class CGameControllerMod : public CGameControllerDDRace
{
public:
	CGameControllerMod(CGameServices &Services, const CGameModeInfo &GameModeInfo);
	~CGameControllerMod() override;
};
#endif // GAME_SERVER_GAMEMODES_MOD_H
