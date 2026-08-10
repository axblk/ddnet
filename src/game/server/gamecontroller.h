/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMECONTROLLER_H
#define GAME_SERVER_GAMECONTROLLER_H

#include <base/dbg.h>
#include <base/vmath.h>

#include <engine/map.h>
#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <game/server/mode/game_mode_registry.h>
#include <game/teamscore.h>

class CCharacter;
class CGameContext;
class CGameControllerDDRace;
class CGameServices;
class CGameTeams;
class CInteractions;
class CPlayer;
class CTuningParams;

struct CWeaponFireContext
{
	CCharacter *m_pCharacter;
	int m_Weapon;
	vec2 m_Direction;
	vec2 m_MouseTarget;
	vec2 m_ProjectileStartPosition;
	const CTuningParams *m_pTuning;
};

struct CWeaponFireResult
{
	bool m_Fired = false;
	bool m_ConsumeAmmo = false;
	int m_ReloadTicks = 0;
};

struct CGamePickupResult
{
	bool m_Picked = false;
	int m_RespawnSeconds = 0;
	int m_RespawnSound = -1;
};

enum class EProjectileOwnerLossAction
{
	KEEP,
	DETACH,
	DESTROY,
};

struct CGameProjectileContext
{
	int m_Weapon;
	CCharacter *m_pOwner;
	bool m_OwnerConnected;
	bool m_BelongsToPracticeTeam;
};

struct CGameProjectileRules
{
	bool m_HitCharacters;
	bool m_RespectCharacterCollision;
	float m_DirectImpactForce;
	EProjectileOwnerLossAction m_OwnerLossAction;
};

struct CGameExplosionContext
{
	vec2 m_Position;
	int m_Owner;
	int m_Weapon;
	bool m_NoDamage;
	int m_ActivatedTeam;
	CClientMask m_Mask;
	int m_AttackerTeam;
};

/*
	Class: Game Controller
		Controls the main game logic. Keeping track of team and player score,
		winning conditions and specific game logic.
*/
class IGameController
{
	friend class CSaveTeam; // need access to GameServer() and Server()
	friend class CGameControllerDDRace;

protected:
	enum ESpawnType
	{
		SPAWNTYPE_DEFAULT = 0,
		SPAWNTYPE_RED,
		SPAWNTYPE_BLUE,

		NUM_SPAWNTYPES
	};

private:
	std::vector<vec2> m_avSpawnPoints[NUM_SPAWNTYPES];

	CGameServices *m_pServices;
	class CConfig *m_pConfig;
	class IServer *m_pServer;

	CTeamsCore m_TeamsCore;
	const CGameModeInfo m_GameModeInfo;
	CGameContext *GameServer() const;

protected:
	CGameServices &Services() const { return *m_pServices; }
	CConfig *Config() { return m_pConfig; }
	IServer *Server() const { return m_pServer; }

	void LoadGameSettings();
	virtual void RegisterCommands() {}
	virtual void InitGameSettings();
	virtual void UpdateGameInfo(CNetObj_GameInfo &GameInfo, int SnappingClient) {}
	virtual int GameInfoFlags(int SnappingClient) const { return 0; }
	virtual int GameInfoFlags2(int SnappingClient) const { return 0; }
	virtual void SnapMode(int SnappingClient) {}
	virtual int ScoreLimit() const { return 0; }
	virtual int TimeLimit() const { return 0; }
	void DoActivityCheck();

	struct CSpawnEval
	{
		CSpawnEval()
		{
			m_Got = false;
			m_FriendlyTeam = -1;
			m_Pos = vec2(100, 100);
		}

		vec2 m_Pos;
		bool m_Got;
		int m_FriendlyTeam;
		float m_Score;
	};

	float EvaluateSpawnPos(CSpawnEval *pEval, vec2 Pos, int ClientId);
	void EvaluateSpawnType(CSpawnEval *pEval, ESpawnType SpawnType, int ClientId);

	void ResetGame();

	char m_aMapWish[MAX_MAP_LENGTH];

	int m_RoundStartTick;
	int m_GameOverTick;
	int m_SuddenDeath;

	int m_Warmup;
	int m_RoundCount;

	int m_GameFlags;

public:
	const char *m_pGameType;

	IGameController(CGameServices &Services, const CGameModeInfo &GameModeInfo);
	virtual ~IGameController();
	void Init();
	const CGameModeInfo &Info() const { return m_GameModeInfo; }
	virtual void ResetTuning();
	virtual CPlayer *CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team);
	virtual CCharacter *CreateCharacter(CPlayer *pPlayer);

	// event
	/*
		Function: OnCharacterDeath
			Called when a CCharacter in the world dies.

		Arguments:
			victim - The CCharacter that died.
			killer - The player that killed it.
			weapon - What weapon that killed it. Can be -1 for undefined
				weapon when switching team or player suicides.
	*/
	virtual int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon);
	virtual bool OnCharacterTakeDamage(class CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam = TEAM_SPECTATORS);
	virtual bool CanCharacterHitCharacter(CCharacter *pAttacker, CCharacter *pTarget) const;
	virtual bool CanSeeInteraction(const CInteractions &, int) const { return true; }
	virtual bool CanHitInteraction(const CInteractions &, int) const { return true; }
	virtual CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context);
	virtual CGamePickupResult OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position);
	virtual int PickupInitialSpawnDelaySeconds(int Type, int Subtype) const { return 0; }
	virtual CGameProjectileRules ProjectileRules(const CGameProjectileContext &Context) const;
	virtual void OnExplosion(const CGameExplosionContext &Context);
	/*
		Function: OnCharacterSpawn
			Called when a CCharacter spawns into the game world.

		Arguments:
			chr - The CCharacter that was spawned.
	*/
	virtual void OnCharacterSpawn(class CCharacter *pChr);
	virtual bool CanSnapCharacter(CCharacter *pCharacter, int SnappingClient) const { return true; }
	virtual void SnapCharacterMode(CCharacter *pCharacter, int SnappingClient, int TranslatedId) {}
	virtual bool UseDDNetEntityNetObjs() const { return false; }
	virtual bool UsesRaceTeams() const { return false; }
	virtual bool UsesRaceScore() const { return false; }
	// Complete mode-owned phases around the shared CharacterCore tick.
	virtual void TickCharacterPreCore(CCharacter *) {}
	virtual void TickCharacterPostCore(CCharacter *pCharacter);

	virtual void HandleCharacterTiles(class CCharacter *pChr, int MapIndex);
	virtual void SetArmorProgress(CCharacter *pCharacter, int Progress) {}

	/*
		Function: OnEntity
			Called when the map is loaded to process an entity
			in the map.

		Arguments:
			index - Entity index.
			pos - Where the entity is located in the world.

		Returns:
			True if the entity index was recognized by this controller.
	*/
	virtual bool OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number = 0);

	virtual void OnPlayerConnect(class CPlayer *pPlayer);
	virtual void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason);
	virtual void OnPlayerNameChanged(int ClientId) {}
	virtual void OnPlayerDDNetVersionKnown(int ClientId) {}
	virtual void OnPlayerSetTeam(int ClientId, int Team);
	virtual void OnPlayerKill(int ClientId);
	virtual void OnPlayerCallKickVote(int ClientId, int TargetId, const char *pReason);
	virtual void OnPlayerCallSpectateVote(int ClientId, int TargetId, const char *pReason);
	virtual bool CanPlayerVoteOnTargetVote(int VoteCreatorId, int VoterId) const;
	virtual int PlayerVetoActivityStartTick(int ClientId) const;
	virtual int PlayerTeamGroup(int ClientId) const;
	virtual bool CanPlayerReceivePreInput(int SenderId, int ReceiverId) const;
	virtual bool IsPlayerDeadSpectator(int ClientId) const { return false; }
	virtual void OnPlayerShowOthers(int ClientId, int Show) {}
	virtual int PlayerAutoRespawnTick(const CPlayer *pPlayer) const;
	virtual bool SaveStateForHotReload() { return false; }
	virtual void RestoreCharacterAfterHotReload(CCharacter *pCharacter);

	virtual void OnReset();

	// game
	virtual void DoWarmup(int Seconds);

	void SetGamePaused(bool Paused);
	bool IsGamePaused() const;
	virtual void StartRound();
	virtual void EndRound();
	void ChangeMap(const char *pToMap);

	/*

	*/
	virtual void Tick();

	virtual void Snap(int SnappingClient);

	/**
	 * Sets the score value that will be shown in the scoreboard.
	 *
	 * @param SnappingClient Client ID of the player that will receive the snapshot.
	 * @param pPlayer Player that is being snapped.
	 *
	 * @return the score value that will be included in the snapshot.
	 */
	virtual int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) { return 0; }
	virtual void SnapPlayerMode(CPlayer *pPlayer, int SnappingClient, int TranslatedId) {}

	class CFinishTime
	{
	public:
		CFinishTime(int Seconds, int Milliseconds) :
			m_Seconds(Seconds), m_Milliseconds(Milliseconds)
		{
			dbg_assert(Seconds >= 0, "Invalid Seconds: %d", Seconds);
			dbg_assert(Milliseconds >= 0 && Milliseconds < 1000, "Invalid Milliseconds: %d", Milliseconds);
		}

		int m_Seconds;
		int m_Milliseconds;

		static CFinishTime Unset() { return CFinishTime(FinishTime::UNSET); }
		static CFinishTime NotFinished() { return CFinishTime(FinishTime::NOT_FINISHED_MILLIS); }

	private:
		CFinishTime(int Type)
		{
			m_Seconds = Type;
			m_Milliseconds = 0;
		}
	};

	/**
	 * Returns the finish time value that will be shown in the scoreboard.
	 *
	 * @param SnappingClient Client ID of the player that will receive the snapshot.
	 * @param pPlayer Player that is being snapped.
	 *
	 * @return The time split into seconds and the milliseconds remainder, use CFinishTime::Unset if you want the server to prefer scores.
	 */
	virtual CFinishTime SnapPlayerTime(int SnappingClient, CPlayer *pPlayer) { return CFinishTime::Unset(); }

	/**
	 * Snaps the current server record / best time of the current map.
	 *
	 * @param SnappingClient Client ID of the player that will receive the snapshot.
	 *
	 * @return The the map best time split into seconds and the milliseconds remainder, use CFinishTime::Unset if you want the server to prefer scores.
	 */
	virtual CFinishTime SnapMapBestTime(int SnappingClient) { return CFinishTime::Unset(); }

	// spawn
	virtual bool CanSpawn(int Team, vec2 *pOutPos, int ClientId);

	virtual void DoTeamChange(class CPlayer *pPlayer, int Team, bool DoChatMsg);

	int TileFlagsToPickupFlags(int TileFlags) const;

	/*

	*/
	virtual bool IsValidTeam(int Team);
	virtual const char *GetTeamName(int Team);
	int ActivePlayerSlots() const;
	virtual int GetAutoTeam(int NotThisId);
	virtual bool CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize);

	virtual CClientMask GetMaskForPlayerWorldEvent(int Asker, int ExceptID = -1);

	bool IsTeamPlay() const { return m_GameFlags & GAMEFLAG_TEAMS; }
	CTeamsCore &TeamsCore() { return m_TeamsCore; }
	const CTeamsCore &TeamsCore() const { return m_TeamsCore; }
	CGameTeams &RaceTeams();
	const CGameTeams &RaceTeams() const;
};

#endif
