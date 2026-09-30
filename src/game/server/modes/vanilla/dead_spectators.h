#ifndef GAME_SERVER_MODES_VANILLA_DEAD_SPECTATORS_H
#define GAME_SERVER_MODES_VANILLA_DEAD_SPECTATORS_H

#include <engine/server.h>

#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <array>
#include <limits>

/**
 * Players who are in the game but may not respawn until the mode lets them.
 *
 * Meanwhile they watch somebody who is still in, and they do not count as
 * inactive. Which condition brings them back is up to the mode: the next
 * round in LMS and LTS, the death of the catcher in zCatch.
 *
 * 0.7 clients show such a player as dead (PLAYERFLAG_DEAD), 0.6 and DDNet
 * clients as a spectator; the player stays in their team on the server.
 */
template<typename TBase>
class CGameControllerDeadSpectators : public TBase
{
	std::array<bool, MAX_CLIENTS> m_aRespawnLocked{};

protected:
	using TBase::Server;
	using TBase::Services;

	bool IsRespawnLocked(int ClientId) const { return m_aRespawnLocked[ClientId]; }
	void SetRespawnLocked(int ClientId, bool Locked) { m_aRespawnLocked[ClientId] = Locked; }
	void UnlockAllRespawns() { m_aRespawnLocked.fill(false); }

	// in the game, not still loading the map
	bool IsPlaying(int ClientId) const
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		return pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS && Server()->ClientIngame(ClientId);
	}

	// the one who plays still counts as alive while waiting to spawn
	bool IsAlive(int ClientId) const
	{
		return IsPlaying(ClientId) && (!m_aRespawnLocked[ClientId] || Services().Player(ClientId)->GetCharacter());
	}

	/**
	 * Whom a dead spectator may watch: by default somebody else of the own team who is still in.
	 *
	 * @param ClientId The dead spectator.
	 * @param TargetId The player to watch.
	 */
	virtual bool CanDeadSpectatorFollow(int ClientId, int TargetId) const // NOLINT(portability-template-virtual-member-function)
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		const CPlayer *pTarget = Services().Player(TargetId);
		return pPlayer && pTarget && TargetId != ClientId && IsAlive(TargetId) && pTarget->GetTeam() == pPlayer->GetTeam();
	}

	// a dead spectator watches somebody who is still in, the one watched so far if possible
	void UpdateDeadSpectators()
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			CPlayer *pPlayer = Services().Player(ClientId);
			if(!pPlayer || !IsPlayerDeadSpectator(ClientId) || CanDeadSpectatorFollow(ClientId, pPlayer->SpectatorId()))
				continue;
			int Follow = SPEC_FREEVIEW;
			for(int Candidate = 0; Candidate < MAX_CLIENTS; Candidate++)
			{
				if(CanDeadSpectatorFollow(ClientId, Candidate))
				{
					Follow = Candidate;
					break;
				}
			}
			if(Follow != pPlayer->SpectatorId())
				pPlayer->SetSpectatorId(Follow);
		}
	}

public:
	using TBase::TBase;

	bool IsPlayerDeadSpectator(int ClientId) const override
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		return pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS && m_aRespawnLocked[ClientId] && !pPlayer->GetCharacter();
	}

	bool CanPlayerSpectate(int ClientId, int SpectatorId) const override
	{
		return !IsPlayerDeadSpectator(ClientId) || CanDeadSpectatorFollow(ClientId, SpectatorId);
	}

	bool CanSpawn(int Team, vec2 *pOutPos, int ClientId) override
	{
		return !m_aRespawnLocked[ClientId] && TBase::CanSpawn(Team, pOutPos, ClientId);
	}

	int PlayerAutoRespawnTick(const CPlayer *pPlayer) const override
	{
		return m_aRespawnLocked[pPlayer->GetCid()] ? std::numeric_limits<int>::max() : TBase::PlayerAutoRespawnTick(pPlayer);
	}

	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override
	{
		TBase::OnPlayerDisconnect(pPlayer, pReason);
		m_aRespawnLocked[pPlayer->GetCid()] = false;
	}

	void Tick() override
	{
		TBase::Tick();
		UpdateDeadSpectators();
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			// waiting to come back is not being inactive
			if(IsPlayerDeadSpectator(ClientId))
				Services().Player(ClientId)->m_LastActionTick++;
		}
	}
};

#endif // GAME_SERVER_MODES_VANILLA_DEAD_SPECTATORS_H
