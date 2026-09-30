#include "survival.h"
#include "teamplay.h"

#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/protocol.h>

#include <generated/protocol7.h>

#include <game/server/entities/character.h>
#include <game/server/player.h>

#include <array>

namespace
{
	// Last Team Standing: the team that is left scores the round
	class CGameControllerLTS final : public CGameControllerSurvival<CGameControllerVanillaTeamplay>
	{
		using CBase = CGameControllerSurvival<CGameControllerVanillaTeamplay>;

		std::array<int, NUM_TEAMS> AliveCounts() const
		{
			std::array<int, NUM_TEAMS> aCounts{};
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(IsAlive(ClientId))
					aCounts[Services().Player(ClientId)->GetTeam()]++;
			}
			return aCounts;
		}

		void WinRound(int Team)
		{
			m_aTeamScores[Team]++;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				CPlayer *pPlayer = Services().Player(ClientId);
				if(pPlayer && pPlayer->GetTeam() == Team)
					AddRoundWon(pPlayer);
			}
		}

	public:
		using CBase::CBase;

		void OnCharacterDeath(const CGameCharacterDeathContext &Context) override
		{
			// a kill scores for the player only, the team scores for the round
			CCharacter *pVictim = Context.m_pVictim;
			CPlayer *pKiller = Context.m_pKiller;
			const int VictimId = pVictim->GetPlayer()->GetCid();
			const int KillerId = pKiller ? pKiller->GetCid() : -1;
			const bool TeamKill = pKiller && KillerId != VictimId && pKiller->GetTeam() == pVictim->GetPlayer()->GetTeam();
			SetRespawnDelay(VictimId, Context.m_Weapon);
			if(CPlayerVanilla *pKillerPlayer = VanillaPlayer(KillerId))
				pKillerPlayer->m_Score += DeathScoreDelta(VictimId, KillerId, Context.m_Weapon, TeamKill);
			CBase::OnCharacterDeath(Context);
		}

		// as in 0.7, not even on command, only when a round begins
		void ForceTeamBalance() override {}

	protected:
		// teams are balanced when a round begins, not in the middle of one
		void UpdateTeamBalance(int Tick) override {}
		void OnRoundBegin() override { BalanceTeams(Server()->Tick()); }

		void SnapMode(int SnappingClient) override
		{
			SnapTeamData(SnappingClient, FLAG_MISSING, FLAG_MISSING);
		}

		bool HasEnoughPlayers() const override
		{
			std::array<int, NUM_TEAMS> aTeamSizes{};
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(IsPlaying(ClientId))
					aTeamSizes[Services().Player(ClientId)->GetTeam()]++;
			}
			return aTeamSizes[TEAM_RED] > 0 && aTeamSizes[TEAM_BLUE] > 0;
		}

		void DoWincheckRound() override
		{
			const std::array<int, NUM_TEAMS> aAlive = AliveCounts();
			char aResult[128];
			if(aAlive[TEAM_RED] + aAlive[TEAM_BLUE] == 0 || IsRoundTimeUp())
			{
				WinRound(TEAM_RED);
				WinRound(TEAM_BLUE);
				str_format(aResult, sizeof(aResult), "Round %d is a draw", Round() + 1);
			}
			else if(aAlive[TEAM_RED] == 0)
			{
				WinRound(TEAM_BLUE);
				str_format(aResult, sizeof(aResult), "The blue team wins round %d", Round() + 1);
			}
			else if(aAlive[TEAM_BLUE] == 0)
			{
				WinRound(TEAM_RED);
				str_format(aResult, sizeof(aResult), "The red team wins round %d", Round() + 1);
			}
			else
				return;
			EndSurvivalRound(aResult);
		}

		bool DoWincheckMatch() const override
		{
			// unlike in TDM, a tie ends the match as well
			return (ScoreLimit() > 0 && (m_aTeamScores[TEAM_RED] >= ScoreLimit() || m_aTeamScores[TEAM_BLUE] >= ScoreLimit())) || IsRoundTimeUp();
		}
	};
}

static const CGameModeRegistration gs_LTS({"lts", "LTS", protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_SURVIVAL, false}, NewGameController<CGameControllerLTS>);
