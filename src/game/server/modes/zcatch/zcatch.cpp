// The rules and the texts follow the zCatch of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).
#include "catches.h"
#include "rules.h"

#include <base/log.h>
#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/match_report.h>
#include <game/server/entities/character.h>
#include <game/server/modes/insta/instagib.h>
#include <game/server/modes/pvp/pvp.h>
#include <game/server/modes/vanilla/dead_spectators.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/player.h>
#include <game/server/teeinfo.h>

#include <array>

namespace
{
	using CZCatchBase = CGameControllerSpawnWeaponInstagib<CGameControllerPvP<CGameControllerDeadSpectators<CGameControllerVanillaDM>>>;

	/**
	 * zCatch: whoever is hit waits until the one who hit them dies, and the
	 * last one standing wins the round.
	 *
	 * With fewer than sv_zcatch_min_players in the game (or with
	 * sv_release_game) it is a release game, a warmup in which who is hit
	 * comes back right away. The kill key of a catcher lets the one caught
	 * last go, a catcher without anybody caught kills themself. Who joins a
	 * running round is caught by the leader. The body colour shows the kills
	 * that count, over the player's own skin.
	 */
	class CGameControllerZCatch final : public CZCatchBase
	{
		CCatches m_Catches;
		// what a caught player asked for: to go to the spectators once free instead of playing
		std::array<bool, MAX_CLIENTS> m_aWantsSpectators{};
		// since when a player is caught, -1 while free
		std::array<int, MAX_CLIENTS> m_aCaughtTick;
		// the reason why the one hit came back was told to the player, once a round
		std::array<bool, MAX_CLIENTS> m_aToldRelease{};

		int KillsToWin() const { return ZCatch::KillsToWin(g_Config.m_SvZcatchMinPlayers); }

		int NumPlaying() const
		{
			int Num = 0;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				Num += IsPlaying(ClientId);
			return Num;
		}

		int NumAlive() const
		{
			int Num = 0;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				Num += IsAlive(ClientId);
			return Num;
		}

		bool HasEnoughPlayers() const
		{
			return !g_Config.m_SvReleaseGame && NumPlaying() >= g_Config.m_SvZcatchMinPlayers;
		}

		// the time caught so far goes to the match report
		void CountCaughtTime(int ClientId)
		{
			if(m_aCaughtTick[ClientId] < 0)
				return;
			if(CPlayer *pPlayer = Services().Player(ClientId))
				AddMatchMetric(pPlayer, "caught_ticks", Server()->Tick() - m_aCaughtTick[ClientId]);
			m_aCaughtTick[ClientId] = m_Catches.IsCaught(ClientId) ? Server()->Tick() : -1;
		}

		void Catch(CPlayer *pVictim, int CatcherId, bool Counts)
		{
			const int VictimId = pVictim->GetCid();
			m_Catches.Catch(VictimId, CatcherId, Counts);
			SetRespawnLocked(VictimId, true);
			pVictim->SetSpectatorId(CatcherId);
			m_aCaughtTick[VictimId] = Server()->Tick();
			m_aWantsSpectators[VictimId] = false;
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "You are spectator until '%s' dies", Server()->ClientName(CatcherId));
			Services().SendChatTarget(VictimId, aBuf);
		}

		// back into the game, or to the spectators if the player asked for it; the catch is gone already
		void Free(int ClientId, const char *pMessage)
		{
			CountCaughtTime(ClientId);
			SetRespawnLocked(ClientId, false);
			CPlayer *pPlayer = Services().Player(ClientId);
			if(!pPlayer)
				return;
			Services().SendChatTarget(ClientId, pMessage);
			if(m_aWantsSpectators[ClientId])
			{
				m_aWantsSpectators[ClientId] = false;
				DoTeamChange(pPlayer, TEAM_SPECTATORS, true);
			}
			else
				pPlayer->Respawn();
		}

		// why the ones the catcher held go free, which they are told with the catcher's name
		enum class EFreedBecause
		{
			DIED,
			RELEASED,
			LEFT,
		};

		void FreeAll(const std::vector<int> &vClientIds, EFreedBecause Because, int CatcherId)
		{
			const char *pName = Server()->ClientName(CatcherId);
			char aBuf[128] = "";
			switch(Because)
			{
			case EFreedBecause::DIED: str_format(aBuf, sizeof(aBuf), "You respawned because '%s' died", pName); break;
			case EFreedBecause::RELEASED: str_format(aBuf, sizeof(aBuf), "You were released by '%s'", pName); break;
			case EFreedBecause::LEFT: str_format(aBuf, sizeof(aBuf), "You respawned because '%s' left", pName); break;
			}
			for(const int ClientId : vClientIds)
				Free(ClientId, aBuf);
		}

		void FreeEverybody()
		{
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(m_Catches.IsCaught(ClientId))
				{
					m_Catches.Leave(ClientId);
					Free(ClientId, "You were released because the round is over");
				}
			}
			m_Catches.Clear();
		}

		// who joins a running round waits for the leader to die
		void CatchByLeader(CPlayer *pPlayer)
		{
			const int ClientId = pPlayer->GetCid();
			const int LeaderId = m_Catches.LeaderId();
			if(!Match().IsRunning() || LeaderId == CCatches::NONE || LeaderId == ClientId || !IsAlive(LeaderId))
				return;
			Catch(pPlayer, LeaderId, false);
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "'%s' is now spectating you (selfkill to release them)", Server()->ClientName(ClientId));
			Services().SendChatTarget(LeaderId, aBuf);
		}

		void UpdateColors(CPlayer *pPlayer)
		{
			// the winner keeps the colour until the next round
			if(Match().IsGameOver())
				return;
			if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			{
				pPlayer->SetTeeInfoOverride(std::nullopt);
				return;
			}
			const int Color = ZCatch::BodyColor(ZCatch::ParseColors(g_Config.m_SvZcatchColors), m_Catches.KillsThatCount(pPlayer->GetCid()));
			pPlayer->SetTeeInfoOverride(CTeeInfoOverride::Colors(Color));
		}

		void StartReleaseGame(const char *pMessage)
		{
			Services().SendChatTarget(-1, pMessage);
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				CountCaughtTime(ClientId);
			AbortMatchReport(EMatchTermination::ABORTED);
			FreeEverybody();
			Match().WaitForPlayers();
		}

		void DoWincheckRound()
		{
			int LastId = -1;
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(IsAlive(ClientId))
					LastId = ClientId;
			}
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				CountCaughtTime(ClientId);
			if(LastId < 0 || m_Catches.KillsThatCount(LastId) < KillsToWin())
			{
				// nobody caught enough to win, as when the others left before they were caught
				Services().SendChatTarget(-1, "Nobody won. Starting a new round.");
				log_info("zcatch", "nobody won the round");
				AbortMatchReport(EMatchTermination::ABORTED);
				StartRound();
				return;
			}
			CPlayer *pWinner = Services().Player(LastId);
			const int Points = ZCatch::WinPoints(m_Catches.KillsThatCount(LastId));
			VanillaPlayer(LastId)->m_Score += Points;
			AddMatchMetric(pWinner, "win_points", Points);
			SetMatchWinners({pWinner});
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "'%s' won the round and gained %d points.", Server()->ClientName(LastId), Points);
			Services().SendChat(-1, TEAM_ALL, aBuf);
			log_info("zcatch", "'%s' won with %d kills that count and gained %d points", Server()->ClientName(LastId), m_Catches.KillsThatCount(LastId), Points);
			EndRound();
		}

	public:
		CGameControllerZCatch(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
			CZCatchBase(Services, GameModeInfo)
		{
			m_aCaughtTick.fill(-1);
		}

		void OnCharacterDeath(const CGameCharacterDeathContext &Context) override
		{
			CPlayer *pVictim = Context.m_pVictim->GetPlayer();
			CPlayer *pKiller = Context.m_pKiller;
			const int VictimId = pVictim->GetCid();
			if(pKiller && Context.m_Weapon != WEAPON_GAME && !Match().IsGameOver())
			{
				// one more for catching somebody who had caught others since the spawn (ddnet-insta issue #274), three less for dying on one's own
				if(pKiller != pVictim && Spree(VictimId) > 0)
					VanillaPlayer(pKiller->GetCid())->m_Score++;
				else if(pKiller == pVictim)
					VanillaPlayer(VictimId)->m_Score -= 2;
			}
			CZCatchBase::OnCharacterDeath(Context);

			// the winner leaving at the end of the round frees nobody
			if(Match().IsGameOver())
				return;
			FreeAll(m_Catches.ReleaseAll(VictimId), EFreedBecause::DIED, VictimId);
			UpdateColors(pVictim);
			if(!pKiller || pKiller == pVictim || Context.m_Weapon == WEAPON_GAME)
				return;

			const int KillerId = pKiller->GetCid();
			if(Match().IsWarmup())
			{
				if(!m_aToldRelease[KillerId])
				{
					Services().SendChatTarget(KillerId, g_Config.m_SvReleaseGame ? "Kill respawned because this is a release game." : "Kill respawned because there are not enough players.");
					m_aToldRelease[KillerId] = true;
				}
				return;
			}
			// a shot of somebody who is gone or caught in the meantime catches nobody
			if(!pKiller->GetCharacter() || m_Catches.IsCaught(KillerId))
				return;
			Catch(pVictim, KillerId, true);
			AddMatchMetric(pKiller, "catches");
			UpdateColors(pKiller);
		}

		void OnPlayerKillKey(CPlayer *pPlayer) override
		{
			const int ClientId = pPlayer->GetCid();
			if(m_Catches.VictimIds(ClientId).empty())
			{
				CZCatchBase::OnPlayerKillKey(pPlayer);
				return;
			}
			// letting somebody go takes no kill delay, as in ddnet-insta
			const std::vector<int> vReleased = m_Catches.ReleaseLast(ClientId);
			AddMatchMetric(pPlayer, "releases", vReleased.size());
			FreeAll(vReleased, EFreedBecause::RELEASED, ClientId);
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "You released '%s' (%d players left)", Server()->ClientName(vReleased.front()), (int)vReleased.size() - 1);
			Services().SendChatTarget(ClientId, aBuf);
			if(m_Catches.KillsThatCount(ClientId) == 0)
			{
				if(vReleased.size() == 1)
					str_copy(aBuf, "You released all players. The next selfkill will kill you!");
				else
					str_format(aBuf, sizeof(aBuf), "You released %d remaining spectators because your kill count reached 0.", (int)vReleased.size() - 1);
				Services().SendChatTarget(ClientId, aBuf);
			}
			UpdateColors(pPlayer);
		}

		void OnPlayerSetTeam(int ClientId, int Team) override
		{
			const int CatcherId = m_Catches.CatcherId(ClientId);
			if(CatcherId == CCatches::NONE)
			{
				CZCatchBase::OnPlayerSetTeam(ClientId, Team);
				return;
			}
			// A caught player cannot get away by spectating (0.7 asks for it, 0.6 sees itself as a spectator
			// and asks for the game), the choice only says where the player goes once free.
			m_aWantsSpectators[ClientId] = Team == TEAM_SPECTATORS;
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), m_aWantsSpectators[ClientId] ? "You will join the spectators once '%s' dies" : "You will join the game once '%s' dies", Server()->ClientName(CatcherId));
			Services().SendBroadcast(aBuf, ClientId);
		}

		void OnPlayerConnect(CPlayer *pPlayer) override
		{
			const int ClientId = pPlayer->GetCid();
			m_aWantsSpectators[ClientId] = false;
			m_aCaughtTick[ClientId] = -1;
			m_aToldRelease[ClientId] = false;
			CZCatchBase::OnPlayerConnect(pPlayer);
			if(pPlayer->GetTeam() != TEAM_SPECTATORS)
				CatchByLeader(pPlayer);
			UpdateColors(pPlayer);
			if(Match().IsWaitingForPlayers() && !HasEnoughPlayers())
				Services().SendChatTarget(ClientId, g_Config.m_SvReleaseGame ? "This is a release game." : "Waiting for more players to start the round.");
		}

		void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override
		{
			const int ClientId = pPlayer->GetCid();
			CountCaughtTime(ClientId);
			FreeAll(m_Catches.Leave(ClientId), EFreedBecause::LEFT, ClientId);
			m_aCaughtTick[ClientId] = -1;
			CZCatchBase::OnPlayerDisconnect(pPlayer, pReason);
		}

		void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override
		{
			const int ClientId = pPlayer->GetCid();
			const int OldTeam = pPlayer->GetTeam();
			CZCatchBase::DoTeamChange(pPlayer, Team, DoChatMsg);
			if(pPlayer->GetTeam() == OldTeam)
				return;
			if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			{
				CountCaughtTime(ClientId);
				FreeAll(m_Catches.Leave(ClientId), EFreedBecause::LEFT, ClientId);
				m_aCaughtTick[ClientId] = -1;
				SetRespawnLocked(ClientId, false);
			}
			else
				CatchByLeader(pPlayer);
			UpdateColors(pPlayer);
		}

		void DoWarmup(int Seconds) override
		{
			CZCatchBase::DoWarmup(Seconds);
			if(!Match().IsWarmup() && !HasEnoughPlayers())
				Match().WaitForPlayers();
		}

		void StartRound() override
		{
			FreeEverybody();
			m_aToldRelease.fill(false);
			if(!HasEnoughPlayers())
				Match().WaitForPlayers();
			CZCatchBase::StartRound();
		}

		void TickMatch() override
		{
			if(Match().IsWaitingForPlayers())
			{
				if(HasEnoughPlayers())
				{
					Services().SendChatTarget(-1, "Enough players connected. Starting game!");
					Match().SetWarmupTicks(0);
					StartRound();
				}
				return;
			}
			if(!Match().IsRunning() || IsGamePaused() || Services().World().ResetRequested())
				return;

			if(!HasEnoughPlayers())
			{
				// the round goes on as long as the leader can still catch enough to win
				const int LeaderId = m_Catches.LeaderId();
				if(g_Config.m_SvReleaseGame || LeaderId == CCatches::NONE || m_Catches.KillsThatCount(LeaderId) + NumAlive() < KillsToWin())
				{
					StartReleaseGame("Not enough players connected anymore. Starting release game.");
					return;
				}
			}
			if(NumAlive() <= 1)
				DoWincheckRound();
		}

		void Tick() override
		{
			CZCatchBase::Tick();
			// also after sv_zcatch_colors changed
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				if(CPlayer *pPlayer = Services().Player(ClientId))
					UpdateColors(pPlayer);
			}
		}

		// the round ends with the last one standing
		int ScoreLimit() const override { return 0; }
		int TimeLimit() const override { return 0; }
	};
}

static const CGameModeRegistration gs_ZCatch({"zcatch", "zCatch", 0, false}, NewGameController<CGameControllerZCatch>);
