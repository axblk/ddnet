#include "interactions.h"

#include <engine/shared/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/player.h>

void CInteractions::Init(int OwnerId, uint32_t UniqueOwnerId)
{
	m_OwnerId = OwnerId;
	m_UniqueOwnerId = UniqueOwnerId;
}

void CInteractions::FillOwnerConnected(
	int DDRaceTeam,
	bool Solo,
	bool NoHitOthers,
	bool NoHitSelf)
{
	m_DDRaceTeam = DDRaceTeam;
	m_Solo = Solo;
	m_NoHitOthers = NoHitOthers;
	m_NoHitSelf = NoHitSelf;
}

void CInteractions::FillOwnerDisconnected()
{
	m_OwnerId = -1;
	// m_UniqueOwnerId = 0; // TODO: yes no maybe?
}

bool CInteractions::CanSee(const CGameContext *pGameServer, int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "invalid client id %d", ClientId);
	return pGameServer->m_apPlayers[ClientId] && pGameServer->m_pController->CanSeeInteraction(*this, ClientId);
}

bool CInteractions::CanHit(const CGameContext *pGameServer, int ClientId) const
{
	dbg_assert(ClientId >= 0 && ClientId < MAX_CLIENTS, "invalid client id %d", ClientId);
	return pGameServer->m_apPlayers[ClientId] && pGameServer->m_pController->CanHitInteraction(*this, ClientId);
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

CClientMask CInteractions::CanHitMask(const CGameContext *pGameServer) const
{
	if(m_DDRaceTeam == TEAM_SUPER)
	{
		return CClientMask().set();
	}

	CClientMask Mask;
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(CanHit(pGameServer, i))
		{
			Mask.set(i);
		}
	}
	return Mask;
}

const CCharacter *CInteractions::OwnerCharacter(const CGameContext *pGameServer) const
{
	const CCharacter *pChr = pGameServer->GetPlayerChar(m_OwnerId);
	if(!pChr)
		return nullptr;
	if(pChr->GetPlayer()->GetUniqueCid() != m_UniqueOwnerId)
		return nullptr;
	return pChr;
}
