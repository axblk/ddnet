#ifndef GAME_SERVER_MODES_INSTA_RULES_H
#define GAME_SERVER_MODES_INSTA_RULES_H

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/player.h>

struct CLaserInstagibRules
{
	static constexpr int WEAPON = WEAPON_LASER;

	static void AdjustDamage(bool SelfDamage, int &Damage, bool &CanDamage)
	{
		if(SelfDamage)
			CanDamage = false;
		else if(CanDamage)
			Damage = 20;
	}
};

struct CGrenadeInstagibRules
{
	static constexpr int WEAPON = WEAPON_GRENADE;
	static constexpr int MINIMUM_KILL_DAMAGE = 4;

	static void AdjustDamage(bool SelfDamage, int &Damage, bool &CanDamage)
	{
		if(SelfDamage || Damage < MINIMUM_KILL_DAMAGE)
			CanDamage = false;
		else if(CanDamage)
			Damage = 20;
	}
};

template<typename TBase, typename TRules>
class CGameControllerInstagib : public TBase
{
public:
	CGameControllerInstagib(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
		TBase(Services, GameModeInfo)
	{
		this->IgnoreMapEntityRange(ENTITY_ARMOR_1, ENTITY_WEAPON_LASER);
	}

	void OnCharacterSpawn(CCharacter *pCharacter) override
	{
		TBase::OnCharacterSpawn(pCharacter);
		pCharacter->GiveWeapon(WEAPON_HAMMER, true);
		pCharacter->GiveWeapon(WEAPON_GUN, true);
		pCharacter->GiveWeapon(TRules::WEAPON);
		pCharacter->SetWeapon(TRules::WEAPON);
	}

	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam) override
	{
		if(Weapon == TRules::WEAPON)
			TRules::AdjustDamage(From == pVictim->GetPlayer()->GetCid(), Damage, CanDamage);
		return TBase::OnCharacterTakeDamage(pVictim, Force, Damage, From, Weapon, CanDamage, AttackerTeam);
	}
};

template<typename TBase>
using CGameControllerLaserInstagib = CGameControllerInstagib<TBase, CLaserInstagibRules>;

template<typename TBase>
using CGameControllerGrenadeInstagib = CGameControllerInstagib<TBase, CGrenadeInstagibRules>;

#endif // GAME_SERVER_MODES_INSTA_RULES_H
