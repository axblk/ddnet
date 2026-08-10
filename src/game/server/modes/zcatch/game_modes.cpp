#include "game_modes.h"

#include <game/server/gamecontext.h>
#include <game/server/mode/game_mode_registry.h>
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
				if(GameServer()->m_apPlayers[ClientId])
					GameServer()->m_apPlayers[ClientId]->Respawn();
			}
		}

		int LeadingCatcherId(int ExcludedId) const
		{
			int BestId = -1;
			int BestCount = 0;
			for(int CandidateId = 0; CandidateId < MAX_CLIENTS; CandidateId++)
			{
				const CPlayer *pCandidate = GameServer()->m_apPlayers[CandidateId];
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
		CGameControllerZCatch(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
			CBase(pGameServer, GameModeInfo)
		{
			m_aCatcherIds.fill(-1);
		}

		int OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon) override
		{
			const int VictimId = pVictim->GetPlayer()->GetCid();
			const int KillerId = pKiller ? pKiller->GetCid() : -1;
			const int Result = CBase::OnCharacterDeath(pVictim, pKiller, Weapon);

			ReleaseCaughtBy(VictimId);
			if(Weapon != WEAPON_GAME && KillerId >= 0 && KillerId < MAX_CLIENTS && KillerId != VictimId && !IsCaught(KillerId) && pKiller->GetTeam() != TEAM_SPECTATORS)
			{
				m_aCatcherIds[VictimId] = KillerId;
				pVictim->GetPlayer()->SetSpectatorId(KillerId);
			}
			return Result;
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

			if(pPlayer->GetTeam() == TEAM_SPECTATORS || m_GameOverTick != -1 || m_Warmup)
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

		void Tick() override
		{
			CBase::Tick();
			if(m_GameOverTick != -1 || m_Warmup)
				return;

			int Contestants = 0;
			int Remaining = 0;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				const CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
				if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
					continue;
				Contestants++;
				if(!IsCaught(ClientId))
					Remaining++;
			}
			if(Contestants >= 2 && Remaining <= 1)
				EndRound();
		}
	};
}

bool RegisterZCatchGameModes(CGameModeRegistry &Registry)
{
	return Registry.Register(
		{"zcatch.laser", "Laser zCatch", "zCatch", "TestZCatch", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN},
		[](CGameContext *pGameServer, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerZCatch>(pGameServer, Info); });
}
