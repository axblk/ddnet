#ifndef GAME_SERVER_MODES_FNG_FNG_H
#define GAME_SERVER_MODES_FNG_FNG_H

// The rules and the texts follow the FNG modes of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).

#include "spikes.h"

#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/collision.h>
#include <game/server/entities/character.h>
#include <game/server/modes/insta/instagib.h>
#include <game/server/modes/pvp/pvp.h>
#include <game/server/modes/vanilla/teamplay.h>
#include <game/server/player.h>

#include <array>
#include <type_traits>

/**
 * FNG: a hit of the instagib weapon freezes instead of killing, and whoever
 * puts a frozen enemy into the spikes scores (modes/fng/spikes.h). A frozen
 * tee cannot move, shoot or kill itself and thaws after sv_hit_freeze_delay;
 * the hammer never freezes, but melts frozen team mates faster.
 *
 * The one who touched a frozen tee last gets the spike kill: by a freeze,
 * a grenade, a hammer or the hook. A touch of a team mate protects.
 *
 * fng and boomfng play it in teams with the laser or the grenade, solofng
 * and bolofng without.
 */
template<typename TBase, int Weapon>
class CGameControllerFng : public CGameControllerFixedInstagib<CGameControllerPvP<TBase>, Weapon>
{
	using CBase = CGameControllerFixedInstagib<CGameControllerPvP<TBase>, Weapon>;
	static constexpr int NOBODY = -1;
	static constexpr std::array<int, MAX_CLIENTS> Nobody()
	{
		std::array<int, MAX_CLIENTS> aIds{};
		aIds.fill(NOBODY);
		return aIds;
	}

	// who touched the player last, for the spike kill
	std::array<int, MAX_CLIENTS> m_aLastToucherIds = Nobody();
	// who froze the player, to tell when another takes the kill
	std::array<int, MAX_CLIENTS> m_aFreezerIds = Nobody();

	bool IsTeamMate(int ClientId, int OtherId) const
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		const CPlayer *pOther = Services().Player(OtherId);
		return this->IsTeamPlay() && pPlayer && pOther && pPlayer->GetTeam() == pOther->GetTeam();
	}

	void AddScore(int ClientId, int Score) { this->VanillaPlayer(ClientId)->m_Score += Score; }

	void AddTeamScore(int Team, int Score)
	{
		if constexpr(std::is_base_of_v<CGameControllerVanillaTeamplay, TBase>)
		{
			if(Team == TEAM_RED || Team == TEAM_BLUE)
				this->m_aTeamScores[Team] += Score;
		}
	}

	void Forget(int ClientId)
	{
		for(int &Id : m_aLastToucherIds)
		{
			if(Id == ClientId)
				Id = NOBODY;
		}
		for(int &Id : m_aFreezerIds)
		{
			if(Id == ClientId)
				Id = NOBODY;
		}
	}

	Fng::ESpike TouchedSpike(const CCharacter *pCharacter) const
	{
		const CCollision *pCollision = Services().Collision();
		if(!pCollision->GameLayer())
			return Fng::ESpike::NONE;
		return Fng::TouchedSpike(pCharacter->m_Pos, pCharacter->GetProximityRadius(), pCollision->GetWidth(), pCollision->GetHeight(), [pCollision](int x, int y) {
			const Fng::ESpike Spike = Fng::SpikeOfTile(pCollision->GetIndex(x, y));
			return Spike != Fng::ESpike::NONE ? Spike : Fng::SpikeOfTile(pCollision->GetFrontIndex(x, y));
		});
	}

protected:
	using CBase::Server;
	using CBase::Services;

	/**
	 * A tee touched the spikes: death, and points for who put it there if it was frozen.
	 */
	void OnSpike(CCharacter *pCharacter, Fng::ESpike Spike)
	{
		const int ClientId = pCharacter->GetPlayer()->GetCid();
		const int KillerId = m_aLastToucherIds[ClientId];
		CPlayer *pKiller = Services().Player(KillerId);
		if(pCharacter->m_FreezeTime <= 0 || !pKiller || IsTeamMate(ClientId, KillerId))
		{
			pCharacter->Die(ClientId, WEAPON_WORLD);
			return;
		}

		// the death gives the killer a point and the team one, as every kill does
		const Fng::CSpikePoints Points = Fng::SpikePoints(Spike, pKiller->GetTeam(), this->IsTeamPlay());
		if(Points.m_Wrong)
		{
			AddScore(KillerId, Points.m_Player - 1);
			AddTeamScore(pKiller->GetTeam(), -1);
			this->AddMatchMetric(pKiller, "wrong_spikes");
			m_aLastToucherIds[KillerId] = NOBODY;
			if(CCharacter *pKillerCharacter = pKiller->GetCharacter(); pKillerCharacter && g_Config.m_SvWrongSpikeFreeze)
				pKillerCharacter->Freeze(g_Config.m_SvWrongSpikeFreeze);
		}
		else
		{
			AddScore(KillerId, Points.m_Player - 1);
			AddTeamScore(pKiller->GetTeam(), Points.m_Team - 1);
			this->AddMatchMetric(pKiller, "spike_kills");
		}
		// yes, wrong spikes count for a multi too
		this->AddMultiKill(pKiller);

		const int FreezerId = m_aFreezerIds[ClientId];
		if(FreezerId != NOBODY && FreezerId != KillerId && g_Config.m_SvAnnounceSteals)
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "'%s' kill was stolen by '%s'.", Server()->ClientName(FreezerId), Server()->ClientName(KillerId));
			Services().SendChat(-1, TEAM_ALL, aBuf);
		}
		this->CreateSoundFor(KillerId, SOUND_CTF_CAPTURE);
		pCharacter->Die(KillerId, WEAPON_NINJA);
	}

	bool OnInstagibHit(CCharacter *pVictim, const CGameDamageContext &Context) override
	{
		// frozen instead of dead, which still is a kill in the kill feed
		const int VictimId = pVictim->GetPlayer()->GetCid();
		CPlayer *pAttacker = Services().Player(Context.m_From);
		if(pAttacker)
		{
			if(IsTeamMate(VictimId, Context.m_From))
				AddScore(Context.m_From, -1);
			else
			{
				AddScore(Context.m_From, 1);
				AddTeamScore(pAttacker->GetTeam(), 1);
			}
			this->AddMatchMetric(pAttacker, "freezes");
		}
		Services().SendKillMessage(Context.m_From, VictimId, Context.m_Weapon);
		Services().CreateDeath(pVictim->m_Pos, VictimId, pVictim->TeamMask());
		m_aLastToucherIds[VictimId] = Context.m_From;
		m_aFreezerIds[VictimId] = Context.m_From;
		pVictim->Freeze(g_Config.m_SvHitFreezeDelay);
		return true;
	}

public:
	using CBase::CBase;

	void OnCharacterSpawn(CCharacter *pCharacter) override
	{
		CBase::OnCharacterSpawn(pCharacter);
		// the hammer to push and to melt, the weapon in the hand
		pCharacter->GiveWeapon(WEAPON_HAMMER);
		pCharacter->SetWeapon(Weapon);
		const int ClientId = pCharacter->GetPlayer()->GetCid();
		m_aLastToucherIds[ClientId] = NOBODY;
		m_aFreezerIds[ClientId] = NOBODY;
	}

	bool OnCharacterTakeDamage(CCharacter *pVictim, const CGameDamageContext &Context) override
	{
		const int VictimId = pVictim->GetPlayer()->GetCid();
		const CPlayer *pAttacker = Context.m_From != VictimId ? Services().Player(Context.m_From) : nullptr;
		CGameDamageContext Hit = Context;
		if(pAttacker && Context.m_Weapon != WEAPON_LASER && Context.m_Weapon != WEAPON_GUN)
		{
			// a push makes the last toucher, one of a team mate protects
			m_aLastToucherIds[VictimId] = IsTeamMate(VictimId, Context.m_From) ? NOBODY : Context.m_From;
		}
		if(pAttacker && Context.m_Weapon == WEAPON_HAMMER)
		{
			// the hammer never freezes, it melts frozen team mates
			Hit.m_CanDamage = false;
			const bool Melt = this->IsTeamPlay() && IsTeamMate(VictimId, Context.m_From) && pVictim->m_FreezeTime > 0;
			if(g_Config.m_SvFngHammer && pAttacker->GetCharacter())
				Hit.m_Force = Fng::HammerForce(pAttacker->GetCharacter()->m_Pos, pVictim->m_Pos, Melt);
			if(Melt)
			{
				bool Thawed;
				pVictim->m_FreezeTime = Fng::MeltFreeze(pVictim->m_FreezeTime, Server()->TickSpeed(), &Thawed);
				if(Thawed)
				{
					AddScore(Context.m_From, 1);
					this->AddMatchMetric(Services().Player(Context.m_From), "melts");
				}
			}
		}
		// a frozen tee is only pushed
		if(pVictim->m_FreezeTime > 0)
			Hit.m_CanDamage = false;
		return CBase::OnCharacterTakeDamage(pVictim, Hit);
	}

	void OnPlayerKillKey(CPlayer *pPlayer) override
	{
		if(pPlayer->GetCharacter() && pPlayer->GetCharacter()->m_FreezeTime > 0)
		{
			Services().SendBroadcast("You can't kill while being frozen", pPlayer->GetCid());
			return;
		}
		CBase::OnPlayerKillKey(pPlayer);
	}

	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override
	{
		// a frozen tee left behind is nobody's kill any more
		Forget(pPlayer->GetCid());
		CBase::OnPlayerDisconnect(pPlayer, pReason);
	}

	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override
	{
		const int OldTeam = pPlayer->GetTeam();
		CBase::DoTeamChange(pPlayer, Team, DoChatMsg);
		if(pPlayer->GetTeam() != OldTeam)
			Forget(pPlayer->GetCid());
	}

	void TickCharacterPostCore(CCharacter *pCharacter) override
	{
		CBase::TickCharacterPostCore(pCharacter);
		if(!pCharacter->IsAlive())
			return;
		// the hook touches too, also by a team mate, which the spikes then do not count
		const int HookedId = pCharacter->Core()->HookedPlayer();
		if(HookedId >= 0 && HookedId < MAX_CLIENTS)
			m_aLastToucherIds[HookedId] = pCharacter->GetPlayer()->GetCid();
		const Fng::ESpike Spike = TouchedSpike(pCharacter);
		if(Spike != Fng::ESpike::NONE)
			OnSpike(pCharacter, Spike);
	}

	int GameInfoFlags(int SnappingClient) const override
	{
		// the FNG overlay of entities, the sounds of FNG, and the hammer the client knows if it is the one it knows
		int Flags = CBase::GameInfoFlags(SnappingClient) | GAMEINFOFLAG_GAMETYPE_FNG | GAMEINFOFLAG_ENTITIES_FNG;
		if(Fng::IsPredictedHammer())
			Flags |= GAMEINFOFLAG_PREDICT_FNG;
		return Flags;
	}
};

#endif // GAME_SERVER_MODES_FNG_FNG_H
