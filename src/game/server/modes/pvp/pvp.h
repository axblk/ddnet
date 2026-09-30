#ifndef GAME_SERVER_MODES_PVP_PVP_H
#define GAME_SERVER_MODES_PVP_PVP_H

#include "anticamper.h"
#include "killing_spree.h"

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <array>

/**
 * What the PvP modes share on top of a vanilla mode, as ddnet-insta has it
 * for all of its modes: protection after spawning, killing sprees and
 * multikills, the anticamper, zoom and a freeze the characters run down.
 *
 * Everything is off by default, see modes/pvp/config_variables.h. The plain
 * vanilla modes do not use it.
 */
template<typename TBase>
class CGameControllerPvP : public TBase
{
	class CPlayerState
	{
	public:
		CKillingSpree m_Spree;
		// the longest spree of the match so far, for the match report
		int m_BestSpree = 0;
		CMultiKill m_MultiKill;
		CAnticamper m_Anticamper;
	};
	std::array<CPlayerState, MAX_CLIENTS> m_aStates;

	// a shot held down since the spawn does not go off in the first half second, a new click does
	static constexpr int FIRST_SHOT_TICKS_DIVISOR = 2;

protected:
	using TBase::Match;
	using TBase::Server;
	using TBase::Services;

	/**
	 * Whether a death ends the killing spree of the one who died.
	 *
	 * Deaths by the game, like a team change, never do.
	 */
	virtual bool DeathEndsSpree(const CGameCharacterDeathContext &Context) const { return true; } // NOLINT(portability-template-virtual-member-function)

	// a kill that counts towards a multikill, which kills do is up to the mode
	void AddMultiKill(CPlayer *pKiller)
	{
		const int Kills = m_aStates[pKiller->GetCid()].m_MultiKill.Add(Server()->Tick(), Server()->TickSpeed());
		if(Kills < 2)
			return;
		char aBuf[128];
		CMultiKill::Format(aBuf, sizeof(aBuf), Server()->ClientName(pKiller->GetCid()), Kills);
		Services().SendChat(-1, TEAM_ALL, aBuf);
	}

	int Spree(int ClientId) const { return m_aStates[ClientId].m_Spree.Kills(); }

	// the character spawned less than sv_respawn_protection_ms ago
	bool IsFreshlySpawned(const CCharacter *pCharacter) const
	{
		return pCharacter && (int64_t)(Server()->Tick() - pCharacter->m_SpawnTick) * 1000 < (int64_t)g_Config.m_SvRespawnProtectionMs * Server()->TickSpeed();
	}

public:
	using TBase::TBase;

	bool OnCharacterTakeDamage(CCharacter *pVictim, const CGameDamageContext &Context) override
	{
		// neither a freshly spawned victim nor a freshly spawned attacker can hurt or be hurt, pushing is fine
		const int VictimId = pVictim->GetPlayer()->GetCid();
		if(Context.m_CanDamage && Context.m_From != VictimId && (IsFreshlySpawned(pVictim) || IsFreshlySpawned(Services().Character(Context.m_From))))
		{
			CGameDamageContext Protected = Context;
			Protected.m_CanDamage = false;
			return TBase::OnCharacterTakeDamage(pVictim, Protected);
		}
		return TBase::OnCharacterTakeDamage(pVictim, Context);
	}

	CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context) override
	{
		// the click that respawned a player must not fire right away (ddnet-insta issue #289)
		if(Context.m_Weapon != WEAPON_GUN && !Context.m_Pressed && Server()->Tick() - Context.m_pCharacter->m_SpawnTick <= Server()->TickSpeed() / FIRST_SHOT_TICKS_DIVISOR)
			return {};
		return TBase::OnCharacterFireWeapon(Context);
	}

	void OnCharacterSpawn(CCharacter *pCharacter) override
	{
		TBase::OnCharacterSpawn(pCharacter);
		m_aStates[pCharacter->GetPlayer()->GetCid()].m_Anticamper.Reset();
	}

	void OnCharacterDeath(const CGameCharacterDeathContext &Context) override
	{
		CCharacter *pVictim = Context.m_pVictim;
		const int VictimId = pVictim->GetPlayer()->GetCid();
		if(Context.m_pKiller && Context.m_Weapon != WEAPON_GAME)
		{
			const int KillerId = Context.m_pKiller->GetCid();
			if(KillerId != VictimId && Context.m_pKiller->GetCharacter())
				AddSpree(Context.m_pKiller);
			if(DeathEndsSpree(Context))
				EndSpree(pVictim, KillerId);
		}
		TBase::OnCharacterDeath(Context);
	}

	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override
	{
		TBase::OnPlayerDisconnect(pPlayer, pReason);
		m_aStates[pPlayer->GetCid()] = CPlayerState();
	}

	void StartRound() override
	{
		for(CPlayerState &State : m_aStates)
			State.m_BestSpree = 0;
		TBase::StartRound();
	}

	void EndRound() override
	{
		TBase::EndRound();
		if(g_Config.m_SvKillingspreeResetOnRoundEnd && Match().IsGameOver())
		{
			for(CPlayerState &State : m_aStates)
				State.m_Spree.End();
		}
	}

	void TickCharacterPreCore(CCharacter *pCharacter) override
	{
		TBase::TickCharacterPreCore(pCharacter);
		pCharacter->TickFreeze();
	}

	void Tick() override
	{
		TBase::Tick();
		// the clock starts anew after the game stood still, which it does as long as a pause waits for the players to be ready
		if(!g_Config.m_SvAnticamper || this->IsGamePaused())
		{
			for(CPlayerState &State : m_aStates)
				State.m_Anticamper.Reset();
			return;
		}
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			CCharacter *pCharacter = Services().Character(ClientId);
			CAnticamper &Anticamper = m_aStates[ClientId].m_Anticamper;
			if(!pCharacter || pCharacter->m_FreezeTime > 0)
			{
				Anticamper.Reset();
				continue;
			}
			switch(Anticamper.Tick(pCharacter->m_Pos, Server()->Tick(), Server()->TickSpeed(), g_Config.m_SvAnticamperTime, g_Config.m_SvAnticamperRange))
			{
			case CAnticamper::EAction::NONE:
				break;
			case CAnticamper::EAction::WARN:
				Services().SendBroadcast("ANTICAMPER: Move or die", ClientId);
				break;
			case CAnticamper::EAction::PUNISH:
				if(g_Config.m_SvAnticamperFreeze > 0)
				{
					pCharacter->Freeze(g_Config.m_SvAnticamperFreeze);
					Services().CreateSound(pCharacter->m_Pos, SOUND_PLAYER_PAIN_LONG);
				}
				else
					pCharacter->Die(ClientId, WEAPON_WORLD);
				break;
			}
		}
	}

	int GameInfoFlags(int SnappingClient) const override
	{
		const int Flags = TBase::GameInfoFlags(SnappingClient);
		return g_Config.m_SvAllowZoom ? Flags | GAMEINFOFLAG_ALLOW_ZOOM : Flags;
	}

private:
	void AddSpree(CPlayer *pKiller)
	{
		CPlayerState &State = m_aStates[pKiller->GetCid()];
		const bool Step = State.m_Spree.Add(g_Config.m_SvKillingspreeKills);
		if(State.m_Spree.Kills() > State.m_BestSpree)
		{
			this->AddMatchMetric(pKiller, "best_spree", State.m_Spree.Kills() - State.m_BestSpree);
			State.m_BestSpree = State.m_Spree.Kills();
		}
		if(!Step)
			return;
		char aBuf[128];
		CKillingSpree::FormatStep(aBuf, sizeof(aBuf), Server()->ClientName(pKiller->GetCid()), State.m_Spree.Kills(), g_Config.m_SvKillingspreeKills);
		Services().SendChat(-1, TEAM_ALL, aBuf);
	}

	void EndSpree(CCharacter *pVictim, int KillerId)
	{
		const int VictimId = pVictim->GetPlayer()->GetCid();
		const int Kills = m_aStates[VictimId].m_Spree.End();
		if(!CKillingSpree::IsEndWorthTelling(Kills, g_Config.m_SvKillingspreeKills))
			return;
		// a bang without damage where the spree ended
		Services().CreateExplosionEvent(pVictim->m_Pos);
		Services().CreateSound(pVictim->m_Pos, SOUND_GRENADE_EXPLODE);
		char aBuf[128];
		CKillingSpree::FormatEnd(aBuf, sizeof(aBuf), Server()->ClientName(VictimId), Kills, Server()->ClientName(KillerId));
		Services().SendChat(-1, TEAM_ALL, aBuf);
	}
};

#endif // GAME_SERVER_MODES_PVP_PVP_H
