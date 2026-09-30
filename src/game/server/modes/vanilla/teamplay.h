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
	// balances the teams once they have been uneven for sv_teambalance_time minutes
	virtual void UpdateTeamBalance(int Tick);
	// moves players from the bigger team to the smaller one right away, if the teams are uneven
	void BalanceTeams(int Tick);
	virtual bool CanBeMovedOnBalance(const CPlayer *pPlayer) const;
	// by the server, not the player: no chat message and no activity
	void MoveSilently(CPlayer *pPlayer, int Team);

public:
	CGameControllerVanillaTeamplay(CGameServices &Services, const CGameModeInfo &GameModeInfo);

	bool OnCharacterTakeDamage(CCharacter *pVictim, const CGameDamageContext &Context) override;
	void Tick() override;
	void StartRound() override;
	bool CanSpawn(int Team, vec2 *pOutPos, int ClientId) override;
	bool IsValidTeam(int Team) override;
	const char *GetTeamName(int Team) override;
	int GetAutoTeam(int NotThisId) override;
	bool CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize) override;
	int TeamScore(int Team) const override;
	// also after a match with sv_match_swap
	void SwapTeams() override;
	void ShuffleTeams() override;
	void ForceTeamBalance() override;

protected:
	void SnapTeamData(int SnappingClient, int FlagCarrierRed, int FlagCarrierBlue, int FlagDropTickRed = 0, int FlagDropTickBlue = 0, bool SnapFlags = false);
};

#endif // GAME_SERVER_MODES_VANILLA_TEAMPLAY_H
