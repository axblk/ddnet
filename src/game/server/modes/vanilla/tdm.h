#ifndef GAME_SERVER_MODES_VANILLA_TDM_H
#define GAME_SERVER_MODES_VANILLA_TDM_H

#include "teamplay.h"

class CGameControllerVanillaTDM : public CGameControllerVanillaTeamplay
{
public:
	CGameControllerVanillaTDM(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo);

	static void ApplyTeamDeathScore(std::array<int, NUM_TEAMS> &aTeamScores, int VictimTeam, int KillerTeam, int Weapon, bool SelfKill);
	int OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon) override;
	void Tick() override;

protected:
	void SnapMode(int SnappingClient) override;
};

#endif // GAME_SERVER_MODES_VANILLA_TDM_H
