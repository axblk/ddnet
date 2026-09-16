#ifndef GAME_SERVER_MODES_VANILLA_DM_H
#define GAME_SERVER_MODES_VANILLA_DM_H

#include "vanilla_pvp.h"

class CGameControllerVanillaDM : public CGameControllerVanillaPvP
{
public:
	CGameControllerVanillaDM(CGameServices &Services, const CGameModeInfo &GameModeInfo);

	void OnCharacterDeath(const CGameCharacterDeathContext &Context) override;
	void Tick() override;
	bool CanSpawn(int Team, vec2 *pOutPos, int ClientId) override;

protected:
	virtual void TickMatch();
};

#endif // GAME_SERVER_MODES_VANILLA_DM_H
