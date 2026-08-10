#ifndef GAME_SERVER_MODE_GAME_SERVICES_H
#define GAME_SERVER_MODE_GAME_SERVICES_H

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <initializer_list>

class CCollision;
class CGameContext;
class CGameHost;
class CGameWorld;
class CPlayer;
class CTuningParams;
class IGameController;
class IServer;

class CGameServices
{
	friend class CGameHost;
	friend class IGameController;
	friend class CPlayer;

	CGameContext *m_pGameServer;

	explicit CGameServices(CGameContext *pGameServer);
	CGameContext *GameServer() const { return m_pGameServer; }

public:
	IServer *Server() const;
	CCollision *Collision() const;
	CGameWorld &World();
	CPlayer *Player(int ClientId) const;
	CNetObj_PlayerInput LastPlayerInput(int ClientId) const;

	void SetGlobalTuning(const CTuningParams &Tuning);
	void ResetTuningZones(const CTuningParams &Tuning);

	void CreateDamageInd(vec2 Position, float Angle, int Amount, CClientMask Mask = CClientMask().set());
	void CreateSound(vec2 Position, int Sound, CClientMask Mask = CClientMask().set());
	void CreateLegacySoundGlobal(int Sound, int Target = -1) const;
	void SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters = {}, int Target = -1) const;
	void SendWeaponPickup(int ClientId, int Weapon) const;
};

#endif // GAME_SERVER_MODE_GAME_SERVICES_H
