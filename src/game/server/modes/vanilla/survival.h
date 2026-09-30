#ifndef GAME_SERVER_MODES_VANILLA_SURVIVAL_H
#define GAME_SERVER_MODES_VANILLA_SURVIVAL_H

#include "dead_spectators.h"

#include <base/log.h>
#include <base/str.h>

#include <engine/server.h>

#include <generated/protocol7.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <array>

/**
 * Rounds inside a match, as Teeworlds 0.7 plays LMS and LTS.
 *
 * Everybody starts a round with every weapon, and nobody who dies comes back
 * before the next one: a dead player watches somebody who is still in, of the
 * own team in a team mode (CGameControllerDeadSpectators). The mode decides when a round is over and who
 * scores for it, and the score limit decides when the match is over. Between
 * two rounds the winners are shown for a moment, and a new round counts down
 * before it starts. There are no pickups.
 *
 * 0.7 clients know all of this, a 0.6 client sees a dead player as a
 * spectator and is told in broadcasts what happens with the round.
 */
template<typename TBase>
class CGameControllerSurvival : public CGameControllerDeadSpectators<TBase>
{
	using CBase = CGameControllerDeadSpectators<TBase>;

	static constexpr int ROUND_OVER_SECONDS = 5;
	static constexpr int WAITING_BROADCAST_SECONDS = 8;

	enum class ERoundState
	{
		RUNNING,
		ROUND_OVER,
	};
	ERoundState m_RoundState = ERoundState::RUNNING;
	// when the pause after a round ends
	int m_RoundStateEndTick = 0;
	int m_RoundOverTick = 0;
	// of the match, from 0
	int m_Round = 0;
	int m_NextWaitingBroadcastTick = 0;

protected:
	using CBase::IsAlive;
	using CBase::IsPlaying;
	using CBase::SetRespawnLocked;
	using CBase::UnlockAllRespawns;
	using CBase::UpdateDeadSpectators;
	using TBase::Match;
	using TBase::Server;
	using TBase::Services;

	bool IsRoundTimeUp() const
	{
		return this->TimeLimit() > 0 && Server()->Tick() - Match().RoundStartTick() >= this->TimeLimit() * Server()->TickSpeed() * 60;
	}

	int Round() const { return m_Round; }

	// What the modes decide; pure, so there is nothing to instantiate, which the NOLINT check does not see.
	// whether a match can be played with the players there are
	virtual bool HasEnoughPlayers() const = 0; // NOLINT(portability-template-virtual-member-function)
	// ends the round with EndSurvivalRound once it is decided, and gives the points for it
	virtual void DoWincheckRound() = 0; // NOLINT(portability-template-virtual-member-function)
	// at the end of a round: whether the match is over
	virtual bool DoWincheckMatch() const = 0; // NOLINT(portability-template-virtual-member-function)
	// before the countdown of a round
	virtual void OnRoundBegin() {}

	/**
	 * Ends the round that DoWincheckRound decided, and the match if it is over.
	 *
	 * @param pResult What a 0.6 client is told about how the round ended.
	 */
	void EndSurvivalRound(const char *pResult)
	{
		this->SetMatchMetric("rounds", m_Round + 1);
		log_info("game", "survival round %d over: %s", m_Round + 1, pResult);
		Services().SendLegacyBroadcast(pResult);
		Services().SendLegacyChatGlobal(pResult);
		if(DoWincheckMatch())
		{
			this->EndRound();
			return;
		}
		m_RoundState = ERoundState::ROUND_OVER;
		m_RoundOverTick = Server()->Tick();
		m_RoundStateEndTick = Server()->Tick() + ROUND_OVER_SECONDS * Server()->TickSpeed();
		this->SetGamePaused(true);
	}

	void AddRoundWon(CPlayer *pPlayer)
	{
		this->AddMatchMetric(pPlayer, "rounds_won");
	}

public:
	using CBase::CBase;
	using CBase::IsPlayerDeadSpectator;

	bool OnEntity(const CMapEntityContext &Context) override
	{
		// no pickups, everybody starts with every weapon
		if(Context.m_Index < ENTITY_SPAWN || Context.m_Index > ENTITY_SPAWN_BLUE)
			return false;
		return CBase::OnEntity(Context);
	}

	void OnCharacterSpawn(CCharacter *pCharacter) override
	{
		CBase::OnCharacterSpawn(pCharacter);
		static constexpr std::array<std::pair<int, int>, 3> s_aLoadout = {{{WEAPON_SHOTGUN, 10}, {WEAPON_GRENADE, 10}, {WEAPON_LASER, 5}}};
		for(const auto &[Weapon, Ammo] : s_aLoadout)
		{
			pCharacter->SetWeaponGot(Weapon, true);
			pCharacter->SetWeaponAmmo(Weapon, Ammo);
		}
		SetRespawnLocked(pCharacter->GetPlayer()->GetCid(), RespawnDisabledFromNow());
	}

	void OnCharacterDeath(const CGameCharacterDeathContext &Context) override
	{
		CBase::OnCharacterDeath(Context);
		const int VictimId = Context.m_pVictim->GetPlayer()->GetCid();
		UpdateDeadSpectators();
		if(Context.m_Weapon != WEAPON_GAME && IsPlayerDeadSpectator(VictimId))
			Services().SendLegacyBroadcast("Wait for the next round", VictimId);
	}

	void OnPlayerConnect(CPlayer *pPlayer) override
	{
		const int ClientId = pPlayer->GetCid();
		SetRespawnLocked(ClientId, RespawnDisabledFromNow());
		CBase::OnPlayerConnect(pPlayer);
		if(IsPlayerDeadSpectator(ClientId))
		{
			UpdateDeadSpectators();
			Services().SendLegacyBroadcast("Wait for the next round", ClientId);
		}
	}

	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override
	{
		const int ClientId = pPlayer->GetCid();
		const int OldTeam = pPlayer->GetTeam();
		CBase::DoTeamChange(pPlayer, Team, DoChatMsg);
		if(pPlayer->GetTeam() == OldTeam || pPlayer->GetTeam() == TEAM_SPECTATORS)
			return;
		SetRespawnLocked(ClientId, RespawnDisabledFromNow());
		if(IsPlayerDeadSpectator(ClientId))
		{
			UpdateDeadSpectators();
			Services().SendLegacyBroadcast("Wait for the next round", ClientId);
		}
	}

	void DoWarmup(int Seconds) override
	{
		CBase::DoWarmup(Seconds);
		if(!Match().IsWarmup() && !HasEnoughPlayers())
			Match().WaitForPlayers();
		if(Match().IsWarmup())
			EnterWarmup();
	}

	// the start of a match
	void StartRound() override
	{
		m_Round = 0;
		m_RoundState = ERoundState::RUNNING;
		UnlockAllRespawns();
		if(!HasEnoughPlayers())
			Match().WaitForPlayers();
		// its first round begins with the match
		CBase::StartRound();
		if(Match().IsWaitingForPlayers())
			EnterWarmup();
	}

	// the end of a match
	void EndRound() override
	{
		CBase::EndRound();
		if(!Match().IsGameOver())
			return;
		// the final scores show everybody in their team
		m_RoundState = ERoundState::RUNNING;
		UnlockAllRespawns();
	}

	void Tick() override
	{
		CBase::Tick();
		if(Match().IsWaitingForPlayers())
		{
			if(HasEnoughPlayers())
			{
				Match().SetWarmupTicks(0);
				StartRound();
			}
			else if(Server()->Tick() >= m_NextWaitingBroadcastTick)
			{
				Services().SendLegacyBroadcast("Waiting for more players");
				m_NextWaitingBroadcastTick = Server()->Tick() + WAITING_BROADCAST_SECONDS * Server()->TickSpeed();
			}
			return;
		}
		if(!Match().IsRunning())
			return;

		switch(m_RoundState)
		{
		case ERoundState::ROUND_OVER:
			if(Server()->Tick() >= m_RoundStateEndTick)
				StartNextRound();
			break;
		case ERoundState::RUNNING:
			// as in 0.7, nothing is decided while the game stands still
			if(!Services().World().ResetRequested() && !this->IsGamePaused())
				DoWincheckRound();
			break;
		}
	}

protected:
	void BeginMatch() override { BeginRound(); }

	void FormatCountdown(char *pBuf, int BufSize, int Seconds, bool Start) const override
	{
		if(Start)
			str_format(pBuf, BufSize, "Round %d starts in %d", m_Round + 1, Seconds);
		else
			CBase::FormatCountdown(pBuf, BufSize, Seconds, Start);
	}

	void OnCountdownEnd(bool Start) override
	{
		CBase::OnCountdownEnd(Start);
		if(!Start)
			return;
		log_info("game", "survival round %d starts", m_Round + 1);
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			CPlayer *pPlayer = Services().Player(ClientId);
			if(pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS)
				this->AddMatchMetric(pPlayer, "rounds_played");
		}
	}

	void UpdateGameDataSixup(protocol7::CNetObj_GameData &GameData, int SnappingClient) override
	{
		CBase::UpdateGameDataSixup(GameData, SnappingClient);
		if(!Match().IsRunning())
			return;
		if(m_RoundState == ERoundState::ROUND_OVER)
		{
			// the world stands still, but a 0.7 client would say that the game is paused
			GameData.m_GameStateFlags &= ~protocol7::GAMESTATEFLAG_PAUSED;
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_ROUNDOVER;
			// what the clock shows: how long the round took
			GameData.m_GameStateEndTick = m_RoundOverTick - Match().RoundStartTick();
		}
	}

private:
	// what 0.7 calls the start respawn state: a player who joins now or spawns now does not come back after dying
	bool RespawnDisabledFromNow() const
	{
		return Match().IsRunning() && !this->IsStartCountdown();
	}

	// respawning is fine while there is no round to play
	void EnterWarmup()
	{
		m_RoundState = ERoundState::RUNNING;
		UnlockAllRespawns();
		m_NextWaitingBroadcastTick = Server()->Tick();
		this->SetGamePaused(false);
	}

	void BeginRound()
	{
		m_RoundState = ERoundState::RUNNING;
		Match().SetRoundStartTick(Server()->Tick());
		OnRoundBegin();
		this->StartCountdown(true);
	}

	void StartNextRound()
	{
		UnlockAllRespawns();
		this->ResetGame();
		if(!HasEnoughPlayers())
		{
			// the match ends without a result, a new one starts once there are enough players again
			log_info("game", "survival match aborted, not enough players");
			this->AbortMatchReport(EMatchTermination::ABORTED);
			Match().WaitForPlayers();
			EnterWarmup();
			return;
		}
		m_Round++;
		BeginRound();
	}
};

#endif // GAME_SERVER_MODES_VANILLA_SURVIVAL_H
