#ifndef GAME_SERVER_MODE_GAME_SERVICES_H
#define GAME_SERVER_MODE_GAME_SERVICES_H

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <initializer_list>

class CCollision;
class CGameContext;
class CGameWorld;
class CPlayer;

// What a game mode may use of the game server.
class CGameServices
{
	friend class CGameHost;
	friend class IGameController;
	friend class CPlayer;

	CGameContext *m_pGameServer;

	explicit CGameServices(CGameContext *pGameServer) :
		m_pGameServer(pGameServer) {}
	CGameContext *GameServer() const { return m_pGameServer; }

public:
	CGameWorld &World() const;
	CCollision *Collision() const;
	// nullptr for ids that are out of range, like the killer of a world death
	CPlayer *Player(int ClientId) const;

	void CreateSound(vec2 Position, int Sound, CClientMask Mask = CClientMask().set()) const;
	void CreateDamageInd(vec2 Position, float Angle, int Amount, CClientMask Mask = CClientMask().set()) const;
	// to 0.6 clients only, 0.7 clients get a game message instead
	void CreateLegacySoundGlobal(int Sound, int Target = -1) const;
	void SendLegacyChatGlobal(const char *pText) const;
	void SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters = {}, int Target = -1) const;
	void SendWeaponPickup(int ClientId, int Weapon) const;
};

#endif // GAME_SERVER_MODE_GAME_SERVICES_H
