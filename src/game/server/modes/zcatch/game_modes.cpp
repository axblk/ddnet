#include "game_modes.h"

#include <game/server/mode/game_mode_registry.h>
#include <game/server/mode/game_services.h>
#include <game/server/modes/insta/rules.h>
#include <game/server/modes/vanilla/dm.h>

#include <array>
#include <limits>

namespace
{
	class CGameControllerZCatch final : public CGameControllerLaserInstagib<CGameControllerVanillaDM>
	{
		using CBase = CGameControllerLaserInstagib<CGameControllerVanillaDM>;

		std::array<int, MAX_CLIENTS> m_aCatcherIds;

		bool IsCaught(int ClientId) const
		{
			return ClientId >= 0 && ClientId < MAX_CLIENTS && m_aCatcherIds[ClientId] >= 0;
		}

		void ReleaseCaughtBy(int CatcherId)
		{
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(m_aCatcherIds[ClientId] != CatcherId)
					continue;
				m_aCatcherIds[ClientId] = -1;
				if(CPlayer *pPlayer = Services().Player(ClientId))
					pPlayer->Respawn();
			}
		}

		int LeadingCatcherId(int ExcludedId) const
		{
			int BestId = -1;
			int BestCount = 0;
			for(int CandidateId = 0; CandidateId < MAX_CLIENTS; CandidateId++)
			{
				const CPlayer *pCandidate = Services().Player(CandidateId);
				if(CandidateId == ExcludedId || !pCandidate || pCandidate->GetTeam() == TEAM_SPECTATORS || IsCaught(CandidateId))
					continue;

				int Count = 0;
				for(const int CatcherId : m_aCatcherIds)
					Count += CatcherId == CandidateId;
				if(Count > BestCount)
				{
					BestId = CandidateId;
					BestCount = Count;
				}
			}
			return BestId;
		}

	public:
		CGameControllerZCatch(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
			CBase(Services, GameModeInfo)
		{
			m_aCatcherIds.fill(-1);
		}

		void OnCharacterDeath(const CGameCharacterDeathContext &Context) override
		{
			CCharacter *pVictim = Context.m_pVictim;
			CPlayer *pKiller = Context.m_pKiller;
			const int VictimId = pVictim->GetPlayer()->GetCid();
			const int KillerId = pKiller ? pKiller->GetCid() : -1;

			ReleaseCaughtBy(VictimId);
			if(Context.m_Weapon != WEAPON_GAME && KillerId >= 0 && KillerId < MAX_CLIENTS && KillerId != VictimId && !IsCaught(KillerId) && pKiller->GetTeam() != TEAM_SPECTATORS)
			{
				m_aCatcherIds[VictimId] = KillerId;
				pVictim->GetPlayer()->SetSpectatorId(KillerId);
				AddParticipantMatchMetric(pKiller, "catches", 1);
			}
			CBase::OnCharacterDeath(Context);
		}

		bool IsPlayerDeadSpectator(int ClientId) const override
		{
			return IsCaught(ClientId);
		}

		bool CanSpawn(int Team, vec2 *pOutPos, int ClientId) override
		{
			return !IsCaught(ClientId) && CBase::CanSpawn(Team, pOutPos, ClientId);
		}

		int PlayerAutoRespawnTick(const CPlayer *pPlayer) const override
		{
			return IsCaught(pPlayer->GetCid()) ? std::numeric_limits<int>::max() : CBase::PlayerAutoRespawnTick(pPlayer);
		}

		void StartRound() override
		{
			m_aCatcherIds.fill(-1);
			CBase::StartRound();
		}

		void OnPlayerConnect(CPlayer *pPlayer) override
		{
			const int ClientId = pPlayer->GetCid();
			m_aCatcherIds[ClientId] = -1;
			CBase::OnPlayerConnect(pPlayer);

			if(pPlayer->GetTeam() == TEAM_SPECTATORS || !Match().IsRunning())
				return;
			const int CatcherId = LeadingCatcherId(ClientId);
			if(CatcherId == -1)
				return;
			m_aCatcherIds[ClientId] = CatcherId;
			pPlayer->SetSpectatorId(CatcherId);
		}

		void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override
		{
			if(Team == TEAM_SPECTATORS)
			{
				ReleaseCaughtBy(pPlayer->GetCid());
				m_aCatcherIds[pPlayer->GetCid()] = -1;
			}
			CBase::DoTeamChange(pPlayer, Team, DoChatMsg);
		}

		void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override
		{
			const int ClientId = pPlayer->GetCid();
			ReleaseCaughtBy(ClientId);
			m_aCatcherIds[ClientId] = -1;
			CBase::OnPlayerDisconnect(pPlayer, pReason);
		}

		void TickMatch() override
		{
			if(!Match().IsRunning())
				return;

			int Contestants = 0;
			int Remaining = 0;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				const CPlayer *pPlayer = Services().Player(ClientId);
				if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
					continue;
				Contestants++;
				if(!IsCaught(ClientId))
					Remaining++;
			}
			if(Contestants >= 2 && Remaining <= 1)
				EndRound();
		}

		int ScoreLimit() const override { return 0; }
		int TimeLimit() const override { return 0; }
	};
}

bool RegisterZCatchGameModes(CGameModeRegistry &Registry)
{
	CGameModeInfo Info = {"zcatch.laser", "Laser zCatch", "zCatch", "TestZCatch", EGameModeScoreKind::POINTS, 0};
	Info.m_Report = CompetitiveGameModeReport("zcatch.laser@ddnet.org", false);
	Info.m_Report.m_vMetrics.push_back({"zcatch.laser@ddnet.org/catches", "Catches", EGameModeMetricCategory::OBJECTIVES, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM, static_cast<int>(Info.m_Report.m_vMetrics.size())});
	return Registry.Register(
		Info,
		[](CGameServices &Services, const CGameModeInfo &GameModeInfo) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerZCatch>(Services, GameModeInfo); });
}
