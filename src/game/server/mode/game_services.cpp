#include "game_services.h"

#include <game/server/gamecontext.h>

CGameWorld &CGameServices::World() const
{
	return m_pGameServer->m_World;
}

CCollision *CGameServices::Collision() const
{
	return m_pGameServer->Collision();
}

CPlayer *CGameServices::Player(int ClientId) const
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS ? m_pGameServer->m_apPlayers[ClientId] : nullptr;
}

void CGameServices::CreateSound(vec2 Position, int Sound, CClientMask Mask) const
{
	m_pGameServer->CreateSound(Position, Sound, Mask);
}

void CGameServices::CreateDamageInd(vec2 Position, float Angle, int Amount, CClientMask Mask) const
{
	m_pGameServer->CreateDamageInd(Position, Angle, Amount, Mask);
}

void CGameServices::CreateLegacySoundGlobal(int Sound, int Target) const
{
	m_pGameServer->CreateSoundGlobal(Sound, Target, CGameContext::FLAG_SIX);
}

void CGameServices::SendLegacyChatGlobal(const char *pText) const
{
	m_pGameServer->SendChatTarget(-1, pText, CGameContext::FLAG_SIX);
}

void CGameServices::SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters, int Target) const
{
	m_pGameServer->SendGameMessage7(GameMessageId, Parameters, Target);
}

void CGameServices::SendWeaponPickup(int ClientId, int Weapon) const
{
	m_pGameServer->SendWeaponPickup(ClientId, Weapon);
}
