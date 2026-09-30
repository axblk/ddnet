#ifndef GAME_SERVER_MODES_INSTA_INSTAGIB_H
#define GAME_SERVER_MODES_INSTA_INSTAGIB_H

// The rules follow the instagib modes of ddnet-insta (zlib licence, https://github.com/ddnet-insta/ddnet-insta).

#include "grenade_ammo.h"

#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

#include <array>

/**
 * Instagib: everyone has the one weapon of the mode and a hit of it decides,
 * as in the instagib modes of ddnet-insta. Laser (iDM, iTDM, iCTF) or
 * grenade (gDM, gTDM, gCTF); zCatch and catch16 choose it per round.
 *
 * There are no pickups and no damage indicators, and the own weapon only
 * pushes. The grenade can be limited as on gCTF servers (grenade_ammo.h),
 * reach only who its shooter saw (sv_sprayprotection) and needs some damage
 * to kill (sv_damage_needed_for_kill); the laser can be made to kill only
 * after a bounce (sv_only_wallshot_kills). Settings: modes/insta/config_variables.h.
 */
template<typename TBase>
class CGameControllerInstagib : public TBase
{
	std::array<CGrenadeAmmo::CRegen, MAX_CLIENTS> m_aGrenadeRegen{};

	CGrenadeAmmo::CSettings GrenadeAmmo() const { return CGrenadeAmmo::CSettings::FromConfig(Server()->TickSpeed()); }

protected:
	using TBase::Server;
	using TBase::Services;

	// the one weapon everyone has, asked at each spawn
	virtual int InstagibWeapon() const = 0; // NOLINT(portability-template-virtual-member-function)

	/**
	 * A hit of the instagib weapon on somebody else that passed the rules of
	 * the mode, like those about friendly fire. The hit sound and the grenades
	 * a hit brings back are done already.
	 *
	 * By default the victim dies of it.
	 *
	 * @return false if the victim died of it.
	 */
	virtual bool OnInstagibHit(CCharacter *pVictim, const CGameDamageContext &Context) // NOLINT(portability-template-virtual-member-function)
	{
		CPlayer *pAttacker = Services().Player(Context.m_From);
		this->AddMatchDamage(pAttacker, pVictim->GetPlayer(), Context.m_Weapon, pVictim->GetHealth() + pVictim->GetArmor());
		pVictim->SetHealth(0);
		pVictim->SetArmor(0);
		pVictim->Die(Context.m_From, Context.m_Weapon);
		if(pAttacker && pAttacker->GetCharacter())
			pAttacker->GetCharacter()->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
		return false;
	}

	bool OnCharacterHurt(CCharacter *pVictim, const CGameDamageContext &Context) override
	{
		const int VictimId = pVictim->GetPlayer()->GetCid();
		if(Context.m_Weapon != InstagibWeapon() || Context.m_From == VictimId)
			return TBase::OnCharacterHurt(pVictim, Context);

		// no damage indicators, a hit decides anyway
		CPlayer *pAttacker = Services().Player(Context.m_From);
		if(pAttacker)
		{
			this->CreateHitSound(Context.m_From);
			if(Context.m_Weapon == WEAPON_LASER && Context.m_Bounces > 0)
				this->AddMatchMetric(pAttacker, "wallshots");
			if(CCharacter *pAttackerCharacter = pAttacker->GetCharacter(); pAttackerCharacter && Context.m_Weapon == WEAPON_GRENADE)
				pAttackerCharacter->SetWeaponAmmo(WEAPON_GRENADE, CGrenadeAmmo::AfterHit(pAttackerCharacter->GetWeaponAmmo(WEAPON_GRENADE), GrenadeAmmo()));
		}
		return OnInstagibHit(pVictim, Context);
	}

public:
	using TBase::TBase;

	bool OnEntity(const CMapEntityContext &Context) override
	{
		// no pickups, everyone has the one weapon that counts
		if(Context.m_Index >= ENTITY_ARMOR_1 && Context.m_Index <= ENTITY_WEAPON_LASER)
			return false;
		return TBase::OnEntity(Context);
	}

	void OnCharacterSpawn(CCharacter *pCharacter) override
	{
		TBase::OnCharacterSpawn(pCharacter);
		const int Weapon = InstagibWeapon();
		pCharacter->GiveWeapon(WEAPON_HAMMER, true);
		pCharacter->GiveWeapon(WEAPON_GUN, true);
		pCharacter->GiveWeapon(Weapon);
		pCharacter->SetWeaponAmmo(Weapon, Weapon == WEAPON_GRENADE ? CGrenadeAmmo::Spawn(GrenadeAmmo()) : -1);
		pCharacter->SetWeapon(Weapon);
		m_aGrenadeRegen[pCharacter->GetPlayer()->GetCid()] = {};
	}

	bool OnCharacterTakeDamage(CCharacter *pVictim, const CGameDamageContext &Context) override
	{
		if(Context.m_Weapon != InstagibWeapon())
			return TBase::OnCharacterTakeDamage(pVictim, Context);

		CGameDamageContext Hit = Context;
		if(Context.m_From == pVictim->GetPlayer()->GetCid())
		{
			// the own weapon only pushes, and a grenade jump gives the grenade back
			Hit.m_CanDamage = false;
			if(Context.m_Weapon == WEAPON_GRENADE)
				pVictim->SetWeaponAmmo(WEAPON_GRENADE, CGrenadeAmmo::AfterSelfPush(pVictim->GetWeaponAmmo(WEAPON_GRENADE), GrenadeAmmo()));
		}
		else if(Context.m_Weapon == WEAPON_GRENADE && Context.m_Damage < g_Config.m_SvDamageNeededForKill)
		{
			// the edge of an explosion only pushes
			Hit.m_CanDamage = false;
		}
		else if(Context.m_Weapon == WEAPON_LASER && Context.m_Bounces == 0 && g_Config.m_SvOnlyWallshotKills)
		{
			return true;
		}
		return TBase::OnCharacterTakeDamage(pVictim, Hit);
	}

	CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context) override
	{
		const CWeaponFireResult Result = TBase::OnCharacterFireWeapon(Context);
		if(Result.m_Fired && Context.m_Weapon == WEAPON_GRENADE)
		{
			const int ReloadTicks = Result.m_ReloadTicks > 0 ? Result.m_ReloadTicks : (int)(Context.m_pTuning->GetWeaponFireDelay(Context.m_Weapon) * Server()->TickSpeed());
			m_aGrenadeRegen[Context.m_pCharacter->GetPlayer()->GetCid()].OnFire(ReloadTicks);
		}
		return Result;
	}

	void OnProjectileCreated(CProjectile *pProjectile) override
	{
		TBase::OnProjectileCreated(pProjectile);
		const int OwnerId = pProjectile->GetOwnerId();
		if(!g_Config.m_SvSprayprotection || pProjectile->Type() != WEAPON_GRENADE || !Services().Player(OwnerId))
			return;
		// the grenade only reaches who its shooter could see when firing
		CClientMask Mask;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			const CCharacter *pCharacter = Services().Character(ClientId);
			if(pCharacter && pCharacter->IsAlive() && !pCharacter->NetworkClipped(OwnerId))
				Mask.set(ClientId);
		}
		pProjectile->SetAffectMask(Mask);
	}

	void TickCharacterPostCore(CCharacter *pCharacter) override
	{
		TBase::TickCharacterPostCore(pCharacter);
		if(!pCharacter->IsAlive() || pCharacter->GetActiveWeapon() != WEAPON_GRENADE)
			return;
		const int Ammo = pCharacter->GetWeaponAmmo(WEAPON_GRENADE);
		const int NewAmmo = m_aGrenadeRegen[pCharacter->GetPlayer()->GetCid()].Tick(Ammo, GrenadeAmmo());
		if(NewAmmo != Ammo)
			pCharacter->SetWeaponAmmo(WEAPON_GRENADE, NewAmmo);
	}

	int GameInfoFlags(int SnappingClient) const override
	{
		// only for switching away from an empty weapon, the DDNet client predicts the ammo it is sent
		return TBase::GameInfoFlags(SnappingClient) | GAMEINFOFLAG_UNLIMITED_AMMO;
	}

	int GameInfoFlags2(int SnappingClient) const override
	{
		// a hit decides, health and armour say nothing
		return TBase::GameInfoFlags2(SnappingClient) & ~GAMEINFOFLAG2_HUD_HEALTH_ARMOR;
	}
};

// instagib with a weapon fixed for the mode
template<typename TBase, int Weapon>
class CGameControllerFixedInstagib : public CGameControllerInstagib<TBase>
{
protected:
	int InstagibWeapon() const override { return Weapon; }

public:
	using CGameControllerInstagib<TBase>::CGameControllerInstagib;
};

// the weapon sv_spawn_weapons names: laser (or rifle, as ddnet-insta also calls it) or else grenade
inline int InstagibSpawnWeapon(const char *pSetting)
{
	return str_comp_nocase(pSetting, "laser") == 0 || str_comp_nocase(pSetting, "rifle") == 0 ? WEAPON_LASER : WEAPON_GRENADE;
}

// instagib with the weapon the server chooses in sv_spawn_weapons, from the next spawn on
template<typename TBase>
class CGameControllerSpawnWeaponInstagib : public CGameControllerInstagib<TBase>
{
protected:
	int InstagibWeapon() const override { return InstagibSpawnWeapon(g_Config.m_SvSpawnWeapons); }

public:
	using CGameControllerInstagib<TBase>::CGameControllerInstagib;
};

template<typename TBase>
using CGameControllerLaserInstagib = CGameControllerFixedInstagib<TBase, WEAPON_LASER>;

template<typename TBase>
using CGameControllerGrenadeInstagib = CGameControllerFixedInstagib<TBase, WEAPON_GRENADE>;

#endif // GAME_SERVER_MODES_INSTA_INSTAGIB_H
