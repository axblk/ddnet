#include "game_services.h"

#include <base/dbg.h>

#include <game/server/gamecontext.h>
#include <game/server/player.h>

CGameServices::CGameServices(CGameContext *pGameServer) :
	m_pGameServer(pGameServer)
{
	dbg_assert(m_pGameServer, "game services require a game server");
}

IServer *CGameServices::Server() const
{
	return m_pGameServer->Server();
}

CCollision *CGameServices::Collision() const
{
	return m_pGameServer->Collision();
}

CGameWorld &CGameServices::World()
{
	return m_pGameServer->m_World;
}

CPlayer *CGameServices::Player(int ClientId) const
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS ? m_pGameServer->m_apPlayers[ClientId] : nullptr;
}

void CGameServices::SetGlobalTuning(const CTuningParams &Tuning)
{
	*m_pGameServer->GlobalTuning() = Tuning;
	m_pGameServer->SendTuningParams(-1);
}

void CGameServices::ResetTuningZones(const CTuningParams &Tuning)
{
	for(int Zone = 0; Zone < TuneZone::NUM; Zone++)
	{
		m_pGameServer->TuningList()[Zone] = Tuning;
		m_pGameServer->m_aaZoneEnterMsg[Zone][0] = 0;
		m_pGameServer->m_aaZoneLeaveMsg[Zone][0] = 0;
	}
}

void CGameServices::CreateDamageInd(vec2 Position, float Angle, int Amount, CClientMask Mask)
{
	m_pGameServer->CreateDamageInd(Position, Angle, Amount, Mask);
}

void CGameServices::CreateSound(vec2 Position, int Sound, CClientMask Mask)
{
	m_pGameServer->CreateSound(Position, Sound, Mask);
}

void CGameServices::CreateLegacySoundGlobal(int Sound, int Target) const
{
	m_pGameServer->CreateSoundGlobal(Sound, Target, CGameContext::FLAG_SIX);
}

void CGameServices::SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters, int Target) const
{
	m_pGameServer->SendGameMessage7(GameMessageId, Parameters, Target);
}

void CGameServices::SendWeaponPickup(int ClientId, int Weapon) const
{
	m_pGameServer->SendWeaponPickup(ClientId, Weapon);
}

CNetObj_PlayerInput CGameServices::LastPlayerInput(int ClientId) const
{
	return m_pGameServer->GetLastPlayerInput(ClientId);
}
