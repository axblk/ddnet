/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "pickup.h"

#include "character.h"

#include <generated/protocol.h>

#include <game/mapitems.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>
#include <game/teamscore.h>

static constexpr int PICKUP_PHYSICS_RADIUS = 14;

CPickup::CPickup(CGameWorld *pGameWorld, int Type, int SubType, int Layer, int Number, int Flags) :
	CEntity(pGameWorld, CGameWorld::ENTTYPE_PICKUP, true, vec2(0, 0), PICKUP_PHYSICS_RADIUS)
{
	m_Core = vec2(0.0f, 0.0f);
	m_Type = Type;
	m_Subtype = SubType;

	m_Layer = Layer;
	m_Number = Number;
	m_Flags = Flags;
	m_RespawnSound = -1;
	const int SpawnDelay = GameServer()->GameHost().Controller()->PickupInitialSpawnDelaySeconds(m_Type, m_Subtype);
	m_SpawnTick = SpawnDelay > 0 ? GameWorld()->GameTick() + GameWorld()->GameTickSpeed() * SpawnDelay : -1;

	GameWorld()->InsertEntity(this);
}

void CPickup::Reset()
{
	m_MarkedForDestroy = true;
}

void CPickup::Tick()
{
	Move();
	if(m_SpawnTick != -1)
	{
		if(GameWorld()->GameTick() > m_SpawnTick)
		{
			m_SpawnTick = -1;
			if(m_RespawnSound >= 0)
				GameServer()->CreateSound(m_Pos, m_RespawnSound);
		}
		else
			return;
	}

	// Check if a player intersected us
	CEntity *apEnts[MAX_CLIENTS];
	int Num = GameWorld()->FindEntities(m_Pos, GetProximityRadius() + ms_CollisionExtraSize, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; ++i)
	{
		auto *pChr = static_cast<CCharacter *>(apEnts[i]);

		if(!pChr || !pChr->IsAlive())
			continue;
		if(m_Layer == LAYER_SWITCH && m_Number > 0 && !Switchers()[m_Number].m_aStatus[pChr->Team()])
			continue;

		const CGamePickupResult Result = GameServer()->GameHost().Controller()->OnCharacterPickup(pChr, m_Type, m_Subtype, m_Pos);
		if(!Result.m_Picked)
			continue;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "pickup player='%d:%s' type=%d subtype=%d", pChr->GetPlayer()->GetCid(), Server()->ClientName(pChr->GetPlayer()->GetCid()), m_Type, m_Subtype);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
		m_RespawnSound = Result.m_RespawnSound;
		if(Result.m_RespawnSeconds >= 0)
			m_SpawnTick = GameWorld()->GameTick() + GameWorld()->GameTickSpeed() * Result.m_RespawnSeconds;
		break;
	}
}

void CPickup::TickPaused()
{
	if(m_SpawnTick != -1)
		m_SpawnTick++;
}

void CPickup::Snap(int SnappingClient)
{
	if(!IsActive() || NetworkClipped(SnappingClient) || GetId() < 0)
		return;

	int SnappingClientVersion = GameServer()->GetClientVersion(SnappingClient);
	bool Sixup = Server()->IsSixup(SnappingClient);

	if(SnappingClientVersion < VERSION_DDNET_ENTITY_NETOBJS)
	{
		CCharacter *pChar = GameServer()->GetPlayerChar(SnappingClient);

		if(SnappingClient != SERVER_DEMO_CLIENT && (GameServer()->m_apPlayers[SnappingClient]->GetTeam() == TEAM_SPECTATORS || GameServer()->m_apPlayers[SnappingClient]->IsPaused()) && GameServer()->m_apPlayers[SnappingClient]->SpectatorId() != SPEC_FREEVIEW)
			pChar = GameServer()->GetPlayerChar(GameServer()->m_apPlayers[SnappingClient]->SpectatorId());

		int Tick = (GameWorld()->GameTick() % GameWorld()->GameTickSpeed()) % 11;
		if(pChar && pChar->IsAlive() && m_Layer == LAYER_SWITCH && m_Number > 0 && !Switchers()[m_Number].m_aStatus[pChar->Team()] && !Tick)
			return;
	}

	GameServer()->SnapPickup(CSnapContext(SnappingClientVersion, Sixup, SnappingClient), GetId(), m_Pos, m_Type, m_Subtype, m_Number, m_Flags);
}

void CPickup::Move()
{
	if(GameWorld()->GameTick() % (int)(GameWorld()->GameTickSpeed() * 0.15f) == 0)
	{
		Collision()->MoverSpeed(m_Pos.x, m_Pos.y, &m_Core);
		m_Pos += m_Core;
	}
}
