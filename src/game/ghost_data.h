/* (c) Rajh, Redix and Sushi. */
#ifndef GAME_GHOST_DATA_H
#define GAME_GHOST_DATA_H

#include <base/mem.h>

#include <generated/protocol.h>

/**
 * What a ghost file is made of: a skin, the character of every tick, and the
 * tick the run started on. The numbers are what they are on the wire, so a
 * ghost is a race written down in the same units a snapshot carries.
 *
 * Here rather than with the client's ghost component, because a program that
 * only reads a ghost - to make a demo of it - has no business bringing the
 * client along for these four structures.
 */
enum
{
	GHOSTDATA_TYPE_SKIN = 0,
	GHOSTDATA_TYPE_CHARACTER_NO_TICK,
	GHOSTDATA_TYPE_CHARACTER,
	GHOSTDATA_TYPE_START_TICK
};

struct CGhostSkin
{
	int m_aSkin[6];
	int m_UseCustomColor;
	int m_ColorBody;
	int m_ColorFeet;
};

struct CGhostCharacter_NoTick
{
	int m_X;
	int m_Y;
	int m_VelX;
	int m_VelY;
	int m_Angle;
	int m_Direction;
	int m_Weapon;
	int m_HookState;
	int m_HookX;
	int m_HookY;
	int m_AttackTick;
};

struct CGhostCharacter : public CGhostCharacter_NoTick
{
	int m_Tick;
};

/**
 * The character of one tick as a snapshot carries it.
 *
 * @param pChar Where it is written.
 * @param pGhostChar What the ghost recorded.
 */
inline void GhostCharacterToNetObj(CNetObj_Character *pChar, const CGhostCharacter *pGhostChar)
{
	mem_zero(pChar, sizeof(CNetObj_Character));
	pChar->m_X = pGhostChar->m_X;
	pChar->m_Y = pGhostChar->m_Y;
	pChar->m_VelX = pGhostChar->m_VelX;
	pChar->m_VelY = 0;
	pChar->m_Angle = pGhostChar->m_Angle;
	pChar->m_Direction = pGhostChar->m_Direction;
	pChar->m_Weapon = pGhostChar->m_Weapon;
	pChar->m_HookState = pGhostChar->m_HookState;
	pChar->m_HookX = pGhostChar->m_HookX;
	pChar->m_HookY = pGhostChar->m_HookY;
	pChar->m_AttackTick = pGhostChar->m_AttackTick;
	pChar->m_HookedPlayer = -1;
	pChar->m_Tick = pGhostChar->m_Tick;
}

/**
 * The other direction, as a ghost records it: a frozen tee is written down as
 * one holding a ninja, which is what it looks like.
 *
 * @param pGhostChar Where it is written.
 * @param pChar The character of this tick.
 * @param pDDnetChar What else is known about it, or `nullptr`.
 */
inline void NetObjToGhostCharacter(CGhostCharacter *pGhostChar, const CNetObj_Character *pChar, const CNetObj_DDNetCharacter *pDDnetChar)
{
	pGhostChar->m_X = pChar->m_X;
	pGhostChar->m_Y = pChar->m_Y;
	pGhostChar->m_VelX = pChar->m_VelX;
	pGhostChar->m_VelY = 0;
	pGhostChar->m_Angle = pChar->m_Angle;
	pGhostChar->m_Direction = pChar->m_Direction;
	int Weapon = pChar->m_Weapon;
	if(pDDnetChar != nullptr && pDDnetChar->m_FreezeEnd != 0)
	{
		Weapon = WEAPON_NINJA;
	}
	pGhostChar->m_Weapon = Weapon;
	pGhostChar->m_HookState = pChar->m_HookState;
	pGhostChar->m_HookX = pChar->m_HookX;
	pGhostChar->m_HookY = pChar->m_HookY;
	pGhostChar->m_AttackTick = pChar->m_AttackTick;
	pGhostChar->m_Tick = pChar->m_Tick;
}

#endif // GAME_GHOST_DATA_H
