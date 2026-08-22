#include "interactions.h"

#include <engine/shared/protocol.h>

#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>

void CInteractions::Init(int OwnerId, uint32_t UniqueOwnerId)
{
	m_OwnerId = OwnerId;
	m_UniqueOwnerId = UniqueOwnerId;
}

void CInteractions::FillOwnerConnected(
	int DDRaceTeam,
	bool Solo,
	bool NoHitOthers,
	bool NoHitSelf,
	bool RestrictToDDRaceTeam)
{
	m_DDRaceTeam = DDRaceTeam;
	m_Solo = Solo;
	m_NoHitOthers = NoHitOthers;
	m_NoHitSelf = NoHitSelf;
	m_RestrictToDDRaceTeam = RestrictToDDRaceTeam;
}

void CInteractions::FillOwnerDisconnected()
{
	m_OwnerId = -1;
	// m_UniqueOwnerId = 0; // TODO: yes no maybe?
}

bool CInteractions::CanSee(const CGameContext *pGameServer, int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "invalid client id %d", ClientId);
	return pGameServer->m_apPlayers[ClientId] && pGameServer->GameHost().Controller()->CanSeeInteraction(*this, ClientId);
}

bool CInteractions::CanHit(const CGameContext *pGameServer, int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "invalid client id %d", ClientId);
	return pGameServer->m_apPlayers[ClientId] && pGameServer->GameHost().Controller()->CanHitInteraction(*this, ClientId);
}

CClientMask CInteractions::CanSeeMask(const CGameContext *pGameServer) const
{
	if(m_DDRaceTeam == TEAM_SUPER)
	{
		return CClientMask().set();
	}

	CClientMask Mask;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(CanSee(pGameServer, i))
		{
			Mask.set(i);
		}
	}
	return Mask;
}
