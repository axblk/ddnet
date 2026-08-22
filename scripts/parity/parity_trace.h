#ifndef SCRIPTS_PARITY_PARITY_TRACE_H
#define SCRIPTS_PARITY_PARITY_TRACE_H

// What the two sides of the parity harness agree on: the inputs a scenario
// means and the shape of the line it writes. Both live here and nowhere else,
// because a harness whose two halves can disagree about either one reports
// divergences it invented itself.
//
// Included by scripts/vanilla_golden/current_runner.cpp for the authoritative
// world and by scripts/parity/client_runner.cpp for the predicted one.

#include <base/mem.h>
#include <base/str.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>

#include <array>
#include <ostream>

// The map. Twenty-four by fourteen tiles built in code rather than loaded, so
// neither side depends on a file and both walk into the same walls: a floor, a
// ceiling, the outer walls and one pillar down the middle.
namespace ParityMap {
enum
{
	WIDTH = 24,
	HEIGHT = 14,
};

inline void Solid(CCollision &Collision, int X, int Y)
{
	Collision.m_pTiles[Y * WIDTH + X].m_Index = TILE_SOLID;
}

inline void Install(CCollision &Collision, std::array<CTile, WIDTH * HEIGHT> &aTiles)
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

// The tape. A scenario is a function from tick to input and nothing else, so
// neither side can drift by feeding itself something the other did not.
inline void ParityInput(const char *pScenario, int Tick, CNetObj_PlayerInput *pInput)
{
	mem_zero(pInput, sizeof(*pInput));
	pInput->m_TargetX = 100;
	pInput->m_TargetY = 0;
	if(str_comp(pScenario, "move") == 0)
	{
		pInput->m_Direction = Tick <= 30 ? 1 : 0;
		pInput->m_Jump = Tick == 10 || Tick == 40 ? 1 : 0;
	}
	else if(str_comp(pScenario, "hook") == 0)
	{
		pInput->m_Direction = 1;
		pInput->m_Hook = Tick >= 2 && Tick <= 60 ? 1 : 0;
	}
	else
	{
		// The firing scenarios. Ammunition is the headline divergence: the
		// server spends it, the predicted character never does, so a tape that
		// keeps firing separates the two within a few ticks.
		pInput->m_Direction = Tick <= 20 ? 1 : 0;
		pInput->m_Fire = Tick % 4 == 1 ? 1 : 0;
	}
}

inline int ParityTicks()
{
	return 120;
}

// One line per character per tick. Quantize has run on both sides by the time
// this is called, so every number here is an integer and the comparison needs
// no tolerance.
inline void ParityPrintCharacter(std::ostream &Out, const char *pScenario, int Tick, int Id,
	const CCharacterCore &Core, int Weapon, int Ammo, int Health, int Armor, int ReloadTimer)
{
	// Write covers fourteen of the fifteen fields and leaves m_Tick alone,
	// so the struct has to start zeroed rather than as stack garbage.
	CNetObj_CharacterCore Net = {};
	Core.Write(&Net);
	Out << "char " << pScenario << " tick=" << Tick << " id=" << Id
	    << " pos=" << Net.m_X << ',' << Net.m_Y
	    << " vel=" << Net.m_VelX << ',' << Net.m_VelY
	    << " angle=" << Net.m_Angle
	    << " jumped=" << Net.m_Jumped
	    << " hook=" << Net.m_HookState << ',' << Net.m_HookedPlayer
	    << ',' << Net.m_HookX << ',' << Net.m_HookY
	    << " weapon=" << Weapon << " ammo=" << Ammo
	    << " health=" << Health << " armor=" << Armor
	    << " reload=" << ReloadTimer << '\n';
}

// The entity population, counted rather than identified. The two sides give an
// entity its identity in different ways, so matching them one to one is a step
// of its own; a count that drifts already names the tick to look at.
inline void ParityPrintCounts(std::ostream &Out, const char *pScenario, int Tick, int Projectiles, int Lasers, int Pickups)
{
	Out << "world " << pScenario << " tick=" << Tick
	    << " projectiles=" << Projectiles
	    << " lasers=" << Lasers
	    << " pickups=" << Pickups << '\n';
}

#endif
