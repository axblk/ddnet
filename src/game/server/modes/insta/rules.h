#ifndef GAME_SERVER_MODES_INSTA_RULES_H
#define GAME_SERVER_MODES_INSTA_RULES_H

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/player.h>

struct CLaserInstagibRules
{
	static constexpr int Weapon = WEAPON_LASER;

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
	static constexpr int Weapon = WEAPON_GRENADE;
	static constexpr int MinimumKillDamage = 4;

	static void AdjustDamage(bool SelfDamage, int &Damage, bool &CanDamage)
	{
		if(SelfDamage || Damage < MinimumKillDamage)
			CanDamage = false;
		else if(CanDamage)
			Damage = 20;
	}
};

template<typename TBase, typename TRules>
class CGameControllerInstagib : public TBase
{
public:
	using TBase::TBase;

	bool OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number) override
	{
		if(Index >= ENTITY_ARMOR_1 && Index <= ENTITY_WEAPON_LASER)
			return false;
		return TBase::OnEntity(Index, x, y, Layer, Flags, Initial, Number);
	}

	void OnCharacterSpawn(CCharacter *pCharacter) override
	{
		TBase::OnCharacterSpawn(pCharacter);
		pCharacter->GiveWeapon(WEAPON_HAMMER, true);
		pCharacter->GiveWeapon(WEAPON_GUN, true);
		pCharacter->GiveWeapon(TRules::Weapon);
		pCharacter->SetWeapon(TRules::Weapon);
	}

	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam) override
	{
		if(Weapon == TRules::Weapon)
			TRules::AdjustDamage(From == pVictim->GetPlayer()->GetCid(), Damage, CanDamage);
		return TBase::OnCharacterTakeDamage(pVictim, Force, Damage, From, Weapon, CanDamage, AttackerTeam);
	}
};

template<typename TBase>
using CGameControllerLaserInstagib = CGameControllerInstagib<TBase, CLaserInstagibRules>;

template<typename TBase>
using CGameControllerGrenadeInstagib = CGameControllerInstagib<TBase, CGrenadeInstagibRules>;

#endif // GAME_SERVER_MODES_INSTA_RULES_H
