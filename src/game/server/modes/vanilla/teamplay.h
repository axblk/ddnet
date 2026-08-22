#ifndef GAME_SERVER_MODES_VANILLA_TEAMPLAY_H
#define GAME_SERVER_MODES_VANILLA_TEAMPLAY_H

#include "vanilla_pvp.h"

#include <array>

class CGameControllerVanillaTeamplay : public CGameControllerVanillaPvP
{
protected:
	std::array<int, NUM_TEAMS> m_aTeamScores{};
	int m_UnbalancedSinceTick = -1;

	std::array<int, NUM_TEAMS> TeamSizes(int ExceptClientId = -1) const;
	void UpdateTeamBalance(int Tick);

public:
	CGameControllerVanillaTeamplay(CGameServices &Services, const CGameModeInfo &GameModeInfo);

	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam) override;
	void Tick() override;
	void StartRound() override;
	bool CanSpawn(int Team, vec2 *pOutPos, int ClientId) override;
	bool IsValidTeam(int Team) override;
	const char *GetTeamName(int Team) override;
	int GetAutoTeam(int NotThisId) override;
	bool CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize) override;
	int TeamScore(int Team) const;

protected:
	void SnapTeamData(int SnappingClient, int FlagCarrierRed, int FlagCarrierBlue, int FlagDropTickRed = 0, int FlagDropTickBlue = 0, bool SnapFlags = false);
};

#endif // GAME_SERVER_MODES_VANILLA_TEAMPLAY_H
