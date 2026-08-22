#include "flag.h"

#include <engine/server.h>

#include <generated/protocol7.h>

#include <game/collision.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>

CFlag::CFlag(CGameWorld *pGameWorld, int Team, vec2 StandPos) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_FLAG, false, StandPos, PHYSICAL_SIZE),
	m_Team(Team),
	m_StandPos(StandPos)
{
	GameWorld()->InsertEntity(this);
	Reset();
}

void CFlag::Reset()
{
	m_AtStand = true;
	m_pCarrier = nullptr;
	m_Pos = m_StandPos;
	m_Vel = vec2(0, 0);
	m_GrabTick = 0;
	m_DropTick = 0;
	m_AutomaticReturn = false;
}

void CFlag::Grab(CCharacter *pCarrier)
{
	dbg_assert(pCarrier != nullptr, "flag carrier must exist");
	if(m_AtStand)
		m_GrabTick = Server()->Tick();
	m_pCarrier = pCarrier;
	m_AtStand = false;
	m_Pos = pCarrier->m_Pos;
}

void CFlag::Drop()
{
	if(!m_pCarrier)
		return;
	m_Pos = m_pCarrier->m_Pos;
	m_pCarrier = nullptr;
	m_Vel = vec2(0, 0);
	m_DropTick = Server()->Tick();
}

void CFlag::Return()
{
	Reset();
}

void CFlag::ReturnAutomatically()
{
	Reset();
	m_AutomaticReturn = true;
}

bool CFlag::TakeAutomaticReturn()
{
	const bool Result = m_AutomaticReturn;
	m_AutomaticReturn = false;
	return Result;
}

void CFlag::TickDeferred()
{
	if(m_pCarrier)
	{
		m_Pos = m_pCarrier->m_Pos;
		return;
	}

	if(Collision()->GetCollisionAt(m_Pos.x, m_Pos.y) == TILE_DEATH ||
		Collision()->GetFrontCollisionAt(m_Pos.x, m_Pos.y) == TILE_DEATH ||
		GameLayerClipped(m_Pos))
	{
		ReturnAutomatically();
		return;
	}

	if(m_AtStand)
		return;
	if(Server()->Tick() > m_DropTick + Server()->TickSpeed() * 30)
	{
		ReturnAutomatically();
		return;
	}

	m_Vel.y += GlobalTuning()->m_Gravity;
	Collision()->MoveBox(&m_Pos, &m_Vel, vec2(PHYSICAL_SIZE, PHYSICAL_SIZE), vec2(0.5f, 0.5f));
}

void CFlag::TickPaused()
{
	if(m_DropTick)
		m_DropTick++;
	if(m_GrabTick)
		m_GrabTick++;
}

void CFlag::Snap(int SnappingClient)
{
	if(NetworkClipped(SnappingClient))
		return;

	if(m_pCarrier)
		m_Pos = m_pCarrier->m_Pos;

	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_Flag Flag = {};
		Flag.m_X = round_to_int(m_Pos.x);
		Flag.m_Y = round_to_int(m_Pos.y);
		Flag.m_Team = m_Team;
		Server()->SnapNewItem(m_Team, Flag);
	}
	else
	{
		CNetObj_Flag Flag = {};
		Flag.m_X = round_to_int(m_Pos.x);
		Flag.m_Y = round_to_int(m_Pos.y);
		Flag.m_Team = m_Team;
		Server()->SnapNewItem(m_Team, Flag);
	}
}
