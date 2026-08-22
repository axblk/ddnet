// The predicted half of the parity harness.
//
// Same synthetic map, same spawns and the same tape as the authoritative half
// in scripts/vanilla_golden/current_runner.cpp, so that every line the two
// write can be compared directly. Nothing here may reimplement a scenario: the
// tape and the line format come from parity_trace.h, which both sides include.

#include <base/logger.h>
#include <base/net.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#define private public
#define protected public
#include <engine/storage.h>

#include <game/client/game_state.h>
#include <game/client/map_context.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/collision.h>
#include <game/mapitems.h>
#undef protected
#undef private

#include "parity_trace.h"

[[noreturn]] static void Fail(const std::string &Message)
{
	std::cerr << Message << '\n';
	std::exit(1);
}

static int WeaponFromName(const std::string &Name)
{
	if(Name == "hammer")
		return WEAPON_HAMMER;
	if(Name == "gun")
		return WEAPON_GUN;
	if(Name == "shotgun")
		return WEAPON_SHOTGUN;
	if(Name == "grenade")
		return WEAPON_GRENADE;
	if(Name == "laser")
		return WEAPON_LASER;
	if(Name == "ninja")
		return WEAPON_NINJA;
	return -1;
}

// The snapshot the predicted character is built from. This is the seam the
// client really uses: it never constructs a character, it adopts one.
static CNetObj_Character SeedCharacter(int PosX, int PosY, int Weapon)
{
	CNetObj_Character Net;
	mem_zero(&Net, sizeof(Net));
	Net.m_X = PosX;
	Net.m_Y = PosY;
	Net.m_Weapon = Weapon;
	Net.m_AmmoCount = 10;
	Net.m_Health = 10;
	Net.m_Armor = 0;
	Net.m_Tick = 0;
	Net.m_HookedPlayer = -1;
	return Net;
}

static int CountEntities(CGameWorld &World, int Type)
{
	int Count = 0;
	for(CEntity *pEntity = World.FindFirst(Type); pEntity != nullptr; pEntity = pEntity->TypeNext())
		++Count;
	return Count;
}

static void RunParity(const std::string &Name)
{
	CMapContext MapContext;
	MapContext.Init();
	std::array<CTile, ParityMap::WIDTH * ParityMap::HEIGHT> aTiles{};
	ParityMap::Install(*MapContext.Collision(), aTiles);

	CGameState State(CGameStateId(1), CStreamId(1));
	CGameInfo Info;
	Info.m_PredictVanilla = true;
	State.SetCoreGameInfo(Info);
	State.InitPrediction(MapContext);
	CGameWorld &World = State.GameWorld();
	// The configuration in which the client predicts anything at all. Weapon
	// prediction hangs off cl_antiping plus cl_antiping_weapons and is off by
	// default (gameclient.cpp:4685); with it off the predicted world fires
	// nothing and comparing it to the server would prove only that.
	World.m_WorldConfig.m_PredictWeapons = true;

	const int Weapon = WeaponFromName(Name.substr(0, 4) == "fire" ? Name.substr(5) : "gun");
	if(Weapon < 0)
		Fail("unknown parity scenario: " + Name);

	CNetObj_Character SeedOne = SeedCharacter(160, 304, Weapon);
	CNetObj_Character SeedTwo = SeedCharacter(195, 304, WEAPON_GUN);
	auto *pChar = new CCharacter(&World, 0, &SeedOne);
	World.InsertEntity(pChar);
	auto *pOther = new CCharacter(&World, 1, &SeedTwo);
	World.InsertEntity(pOther);
	pChar->SetWeaponGot(Weapon, true);
	pChar->SetWeaponAmmo(Weapon, 10);
	pChar->m_Core.m_ActiveWeapon = Weapon;
	pChar->m_ReloadTimer = 0;

	for(int Tick = 0; Tick <= ParityTicks(); ++Tick)
	{
		if(Tick > 0)
		{
			CNetObj_PlayerInput Input;
			ParityInput(Name.c_str(), Tick, &Input);
			pChar->OnDirectInput(&Input);
			pChar->OnPredictedInput(&Input);
			World.m_GameTick = Tick;
			World.Tick();
		}
		// The predicted character has no health, no armor and no death: the
		// client does not model damage at all, it only shows what the snapshot
		// last said. Reporting -1 keeps the two lines the same shape and leaves
		// the gap for the ledger to name instead of hiding it in a blank.
		ParityPrintCharacter(std::cout, Name.c_str(), Tick, 0, *pChar->Core(),
			pChar->GetActiveWeapon(), pChar->GetWeaponAmmo(pChar->GetActiveWeapon()),
			-1, -1, pChar->m_ReloadTimer);
		ParityPrintCounts(std::cout, Name.c_str(), Tick,
			CountEntities(World, CGameWorld::ENTTYPE_PROJECTILE),
			CountEntities(World, CGameWorld::ENTTYPE_LASER),
			CountEntities(World, CGameWorld::ENTTYPE_PICKUP));
	}
}

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		std::cerr << "usage: parity-client SCENARIOS\n";
		return 2;
	}
	std::ifstream Input(argv[1]);
	if(!Input)
	{
		std::cerr << "cannot open scenarios: " << argv[1] << '\n';
		return 2;
	}
	log_set_global_logger(log_logger_noop().release());
	net_init();
	std::string Line;
	while(std::getline(Input, Line))
	{
		const std::string::size_type First = Line.find_first_not_of(" \t\r");
		if(First == std::string::npos || Line[First] == '#')
			continue;
		std::istringstream Fields(Line.substr(First));
		std::string Kind;
		std::string Name;
		Fields >> Kind >> Name;
		if(Kind != "parity")
			Fail("the predicted side only runs parity scenarios: " + Line);
		RunParity(Name);
	}
	return 0;
}
