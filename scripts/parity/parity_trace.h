#ifndef SCRIPTS_PARITY_PARITY_TRACE_H
#define SCRIPTS_PARITY_PARITY_TRACE_H

// What both halves of the parity harness share: the map, the tape, the line
// format and the scenario file loop. Included by
// scripts/vanilla_golden/current_runner.cpp (authoritative world) and
// scripts/parity/client_runner.cpp (predicted world).

#include <base/logger.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

[[noreturn]] inline void Fail(const std::string &Message)
{
	std::cerr << Message << '\n';
	std::exit(1);
}

inline int WeaponFromName(const std::string &Name)
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

// The whole bucket rather than a radius query, so an entity that flew far away
// is still counted on both sides.
template<typename TWorld>
int CountEntities(TWorld &World, int Type)
{
	int Count = 0;
	for(auto *pEntity = World.FindFirst(Type); pEntity != nullptr; pEntity = pEntity->TypeNext())
		++Count;
	return Count;
}

// A floor, a ceiling, the outer walls and one pillar, built in code so neither
// side depends on a map file.
namespace ParityMap {
enum
{
	WIDTH = 24,
	HEIGHT = 14,
};

using CTiles = std::array<CTile, WIDTH * HEIGHT>;

inline void Solid(CCollision &Collision, int X, int Y)
{
	Collision.m_pTiles[Y * WIDTH + X].m_Index = TILE_SOLID;
}

inline void Install(CCollision &Collision, CTiles &aTiles)
{
	Collision.Unload();
	aTiles.fill({});
	Collision.m_Width = WIDTH;
	Collision.m_Height = HEIGHT;
	Collision.m_pTiles = aTiles.data();
	for(int x = 0; x < WIDTH; ++x)
	{
		Solid(Collision, x, 0);
		Solid(Collision, x, 10);
		Solid(Collision, x, HEIGHT - 1);
	}
	for(int y = 0; y < HEIGHT; ++y)
	{
		Solid(Collision, 0, y);
		Solid(Collision, WIDTH - 1, y);
		Solid(Collision, 16, y);
	}
}
} // namespace ParityMap

inline int ParityWeapon(const std::string &Scenario)
{
	return WeaponFromName(Scenario.substr(0, 5) == "fire_" ? Scenario.substr(5) : "gun");
}

inline void ParityInput(const std::string &Scenario, int Tick, CNetObj_PlayerInput *pInput)
{
	mem_zero(pInput, sizeof(*pInput));
	pInput->m_TargetX = 100;
	pInput->m_TargetY = 0;
	if(Scenario == "move")
	{
		pInput->m_Direction = Tick <= 30 ? 1 : 0;
		pInput->m_Jump = Tick == 10 || Tick == 40 ? 1 : 0;
	}
	else if(Scenario == "hook")
	{
		pInput->m_Direction = 1;
		pInput->m_Hook = Tick >= 2 && Tick <= 60 ? 1 : 0;
	}
	else
	{
		pInput->m_Direction = Tick <= 20 ? 1 : 0;
		pInput->m_Fire = Tick % 4 == 1 ? 1 : 0;
	}
}

constexpr int PARITY_TICKS = 120;

// Both sides quantize before writing, so every value is an integer and the
// comparison needs no tolerance.
inline void ParityPrintCharacter(const std::string &Scenario, int Tick, int Id, const CCharacterCore &Core, int Ammo, int ReloadTimer)
{
	CNetObj_CharacterCore Net = {};
	Core.Write(&Net);
	std::cout << "char " << Scenario << " tick=" << Tick << " id=" << Id
		  << " pos=" << Net.m_X << ',' << Net.m_Y
		  << " vel=" << Net.m_VelX << ',' << Net.m_VelY
		  << " angle=" << Net.m_Angle
		  << " jumped=" << Net.m_Jumped
		  << " hook=" << Net.m_HookState << ',' << Net.m_HookedPlayer
		  << ',' << Net.m_HookX << ',' << Net.m_HookY
		  << " weapon=" << Core.m_ActiveWeapon << " ammo=" << Ammo
		  << " reload=" << ReloadTimer << '\n';
}

inline void ParityPrintCounts(const std::string &Scenario, int Tick, int Projectiles, int Lasers, int Pickups)
{
	std::cout << "world " << Scenario << " tick=" << Tick
		  << " projectiles=" << Projectiles
		  << " lasers=" << Lasers
		  << " pickups=" << Pickups << '\n';
}

// Runs every non-empty, non-comment line of the scenario file given on the
// command line through Dispatch.
template<typename TDispatch>
int RunScenarioFile(int Argc, char **ppArgv, const char *pProgram, TDispatch &&Dispatch)
{
	if(Argc != 2)
	{
		std::cerr << "usage: " << pProgram << " SCENARIOS\n";
		return 2;
	}
	std::ifstream Input(ppArgv[1]);
	if(!Input)
	{
		std::cerr << "cannot open scenarios: " << ppArgv[1] << '\n';
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
		Dispatch(Line.substr(First));
	}
	return 0;
}

#endif
