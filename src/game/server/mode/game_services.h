#ifndef GAME_SERVER_MODE_GAME_SERVICES_H
#define GAME_SERVER_MODE_GAME_SERVICES_H

#include <base/vmath.h>

#include <engine/shared/protocol.h>
#include <engine/shared/uuid_manager.h>

#include <generated/protocol.h>

#include <initializer_list>
#include <optional>
#include <vector>

class CCharacter;
class CCollision;
class CGameContext;
class CGameWorld;
class CLayers;
class CPlayer;
class CPlayerMapping;
class CTeeHistorian;
class CTuningParams;
class IAntibot;
class IConsole;
class IMap;
class IServer;
class IStorage;
struct SSwitchers;

// The votes of the server, as far as a mode takes part in them.
class CGameVotes
{
	friend class CGameServices;

	CGameContext *m_pGameServer;

	explicit CGameVotes(CGameContext *pGameServer) :
		m_pGameServer(pGameServer) {}

public:
	enum class EType
	{
		OPTION,
		KICK,
		SPECTATE,
	};

	// Victim is the player a kick or spectate vote is about, -1 for an option vote
	void Call(EType Type, int Victim, int ClientId, const char *pDesc, const char *pCmd, const char *pReason, const char *pChatmsg, const char *pSixupDesc = nullptr) const;
	void Force(bool Success) const;
	// the player a running kick or spectate vote is about, -1 if there is none
	int KickOrSpecVictim() const;
	bool IsRunningKickOrSpecVote(int ClientId) const;
	// who called the vote whose command is being run
	int Creator() const;
	bool Moderating() const;
	// true if the player may not call a vote now, and tells them so
	bool RateLimit(int ClientId) const;
	bool RateLimitMap(int ClientId) const;
	// the map vote cooldown starts when a map vote is called and is lifted when finding the map failed
	void SetLastMapVote(int64_t Time) const;
};

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
	enum
	{
		FLAG_SIX = 1 << 0,
		FLAG_SIXUP = 1 << 1,
	};

	IServer *Server() const;
	IConsole *Console() const;
	IStorage *Storage() const;
	IMap *Map() const;
	IAntibot *Antibot() const;

	CGameWorld &World() const;
	CCollision *Collision() const;
	CLayers *Layers() const;
	std::vector<SSwitchers> &Switchers() const;
	CPlayerMapping &PlayerMapping() const;
	CGameVotes Votes() const { return CGameVotes(m_pGameServer); }
	// nullptr while no teehistorian is recorded
	CTeeHistorian *TeeHistorian() const;
	CUuid GameUuid() const;

	// nullptr for ids that are out of range, like the killer of a world death
	CPlayer *Player(int ClientId) const;
	CCharacter *Character(int ClientId) const;
	CPlayer *FindPlayerByName(const char *pName) const;
	std::optional<int> FindClientIdByName(const char *pName) const;
	int ClientVersion(int ClientId) const;
	CClientMask ClientsMaskExcludeClientVersionAndHigher(int Version) const;
	CNetObj_PlayerInput LastPlayerInput(int ClientId) const;

	CTuningParams *GlobalTuning() const;
	// one per tune zone, zone 0 is the global tuning
	CTuningParams *TuningList() const;
	void SendTuningParams(int ClientId, int Zone = 0) const;
	// what a player is told on entering or leaving a tune zone, empty for nothing
	const char *ZoneMessage(int Zone, bool Enter) const;
	void SetZoneMessage(int Zone, bool Enter, const char *pText) const;

	void CreateSound(vec2 Position, int Sound, CClientMask Mask = CClientMask().set()) const;
	void CreateDamageInd(vec2 Position, float Angle, int Amount, CClientMask Mask = CClientMask().set()) const;
	void CreateDeath(vec2 Position, int ClientId, CClientMask Mask = CClientMask().set()) const;
	void CreateExplosionEvent(vec2 Position, CClientMask Mask = CClientMask().set()) const;
	void CreateFinishEffect(vec2 Position, CClientMask Mask = CClientMask().set()) const;
	void CreateBirthdayEffect(vec2 Position, CClientMask Mask = CClientMask().set()) const;
	// to 0.6 clients only, 0.7 clients get a game message instead
	void CreateLegacySoundGlobal(int Sound, int Target = -1) const;
	void SendLegacyChatGlobal(const char *pText) const;
	void SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters = {}, int Target = -1) const;
	void SendWeaponPickup(int ClientId, int Weapon) const;

	void SendChatTarget(int To, const char *pText, int VersionFlags = FLAG_SIX | FLAG_SIXUP) const;
	void SendChatTeam(int Team, const char *pText) const;
	void SendChat(int ClientId, int Team, const char *pText, int SpamProtectionClientId = -1, int VersionFlags = FLAG_SIX | FLAG_SIXUP) const;
	void SendBroadcast(const char *pText, int ClientId, bool IsImportant = true) const;
	// true if the player is muted or sends too much, and tells them so
	bool ProcessSpamProtection(int ClientId, bool RespectChatInitialDelay = true) const;
};

#endif // GAME_SERVER_MODE_GAME_SERVICES_H
