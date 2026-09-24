// The predicted half of the parity harness, see scripts/parity_check.py. It
// runs the client's prediction without a CGameClient: the characters are
// adopted from snapshot objects the way the client adopts them.

#include <memory>
#include <sstream>
#include <string>

#define private public
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#undef private

#include <game/client/game_state.h>
#include <game/client/map_context.h>
#include <game/client/prediction/gameworld.h>

#include "parity_trace.h"

static CNetObj_Character SeedCharacter(int PosX, int PosY, int Weapon)
{
	CNetObj_Character Net = {};
	Net.m_X = PosX;
	Net.m_Y = PosY;
	Net.m_Weapon = Weapon;
	Net.m_AmmoCount = 10;
	Net.m_Health = 10;
	Net.m_HookedPlayer = -1;
	return Net;
}

// A DDNet server always sends the extension. Without it the client infers a
// jetpack from the tuning, which makes the gun full auto.
static CNetObj_DDNetCharacter SeedExtended(int Weapon)
{
	static const int s_aWeaponFlags[NUM_WEAPONS] = {
		CHARACTERFLAG_WEAPON_HAMMER,
		CHARACTERFLAG_WEAPON_GUN,
		CHARACTERFLAG_WEAPON_SHOTGUN,
		CHARACTERFLAG_WEAPON_GRENADE,
		CHARACTERFLAG_WEAPON_LASER,
		CHARACTERFLAG_WEAPON_NINJA,
	};
	CNetObj_DDNetCharacter Net = {};
	Net.m_Flags = CHARACTERFLAG_WEAPON_HAMMER | CHARACTERFLAG_WEAPON_GUN | s_aWeaponFlags[Weapon];
	Net.m_Jumps = 2;
	Net.m_JumpedTotal = -1;
	Net.m_NinjaActivationTick = -1;
	Net.m_FreezeStart = -1;
	Net.m_TuneZoneOverride = TuneZone::OVERRIDE_NONE;
	return Net;
}

static void RunParity(const std::string &Name)
{
	const int Weapon = ParityWeapon(Name);
	if(Weapon < 0)
		Fail("invalid parity scenario: " + Name);

	CMapContext MapContext;
	ParityMap::CTiles aTiles{};
	ParityMap::Install(*MapContext.Collision(), aTiles);

	const auto pState = std::make_unique<CGameState>();
	CGameInfo Info;
	Info.m_PredictVanilla = true;
	pState->SetCoreGameInfo(Info);
	pState->InitPrediction(MapContext);
	CGameWorld &World = pState->m_GameWorld;
	// what cl_antiping_weapons turns on; without it the predicted world fires nothing
	World.m_WorldConfig.m_PredictWeapons = true;

	CNetObj_Character SeedOne = SeedCharacter(160, 304, Weapon);
	CNetObj_Character SeedTwo = SeedCharacter(195, 304, WEAPON_GUN);
	CNetObj_DDNetCharacter ExtendedOne = SeedExtended(Weapon);
	CNetObj_DDNetCharacter ExtendedTwo = SeedExtended(WEAPON_GUN);
	auto *pChar = new CCharacter(&World, 0, &SeedOne, &ExtendedOne);
	World.InsertEntity(pChar);
	World.InsertEntity(new CCharacter(&World, 1, &SeedTwo, &ExtendedTwo));
	pChar->SetWeaponGot(Weapon, true);
	pChar->SetWeaponAmmo(Weapon, 10);
	pChar->m_Core.m_ActiveWeapon = Weapon;
	pChar->m_ReloadTimer = 0;

	for(int Tick = 0; Tick <= PARITY_TICKS; ++Tick)
	{
		if(Tick > 0)
		{
			CNetObj_PlayerInput Input;
			ParityInput(Name, Tick, &Input);
			pChar->OnDirectInput(&Input);
			pChar->OnPredictedInput(&Input);
			World.m_GameTick = Tick;
			World.Tick();
		}
		// the server keeps every laser-like entity in its one laser bucket
		int Lasers = 0;
		for(int Type = CGameWorld::ENTTYPE_LASER; Type <= CGameWorld::ENTTYPE_PLASMA; Type++)
			Lasers += CountEntities(World, Type);
		ParityPrintCharacter(Name, Tick, 0, *pChar->Core(), pChar->GetWeaponAmmo(pChar->GetActiveWeapon()), pChar->m_ReloadTimer);
		ParityPrintCounts(Name, Tick, CountEntities(World, CGameWorld::ENTTYPE_PROJECTILE), Lasers, CountEntities(World, CGameWorld::ENTTYPE_PICKUP));
	}
}

int main(int argc, char **argv)
{
	return RunScenarioFile(argc, argv, "parity-client", [](const std::string &Line) {
		std::istringstream Fields(Line);
		std::string Kind;
		std::string Name;
		Fields >> Kind >> Name;
		if(Kind != "parity")
			Fail("the predicted side only runs parity scenarios: " + Line);
		RunParity(Name);
	});
}
