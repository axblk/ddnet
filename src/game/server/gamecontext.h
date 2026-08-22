/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_SERVER_GAMECONTEXT_H
#define GAME_SERVER_GAMECONTEXT_H

#include "eventhandler.h"
#include "gameworld.h"
#include "mode/game_host.h"
#include "playermapping.h"
#include "teehistorian.h"

#include <base/types.h>

#include <engine/console.h>
#include <engine/server.h>

#include <generated/protocol.h>

#include <game/collision.h>
#include <game/layers.h>
#include <game/mapbugs.h>
#include <game/voting.h>

#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>

/*
	Tick
		Game Context (CGameContext::tick)
			Game World (GAMEWORLD::tick)
				Reset world if requested (GAMEWORLD::reset)
				All entities in the world (ENTITY::tick)
				All entities in the world (ENTITY::tick_deferred)
				Remove entities marked for deletion (GAMEWORLD::remove_entities)
			Game Controller (GAMECONTROLLER::tick)
			All players (CPlayer::tick)


	Snap
		Game Context (CGameContext::snap)
			Game World (GAMEWORLD::snap)
				All entities in the world (ENTITY::snap)
			Game Controller (GAMECONTROLLER::snap)
			Events handler (EVENT_HANDLER::snap)
			All players (CPlayer::snap)

*/

class CCharacter;
class IConfigManager;
class CConfig;
class CHeap;
class CPlayer;
class CUnpacker;
class IAntibot;
class IGameController;
class IMap;
class IStorage;
struct CAntibotRoundData;
struct CSnapContext
{
	CSnapContext(int Version, bool Sixup, int ClientId) :
		m_ClientVersion(Version), m_Sixup(Sixup), m_ClientId(ClientId)
	{
	}

	int GetClientVersion() const { return m_ClientVersion; }
	bool IsSixup() const { return m_Sixup; }
	int ClientId() const { return m_ClientId; }

private:
	int m_ClientVersion;
	bool m_Sixup;
	int m_ClientId;
};

class CMute
{
public:
	int64_t m_Expire;
	bool m_Initialized = false;
	bool m_InitialDelay;
	char m_aReason[128];
	char m_aClientName[MAX_NAME_LENGTH];
	bool m_NameKnown;

	int SecondsLeft() const;
};

class CMutes
{
public:
	CMutes(const char *pSystemName);

	bool Mute(const NETADDR *pAddr, int Seconds, const char *pReason, const char *pClientName, bool InitialDelay);
	void UnmuteIndex(int Index);
	void UnmuteAddr(const NETADDR *pAddr);
	void UnmuteExpired();
	std::optional<CMute> IsMuted(const NETADDR *pAddr, bool RespectInitialDelay) const;
	void Print(int Page) const;

private:
	const char *m_pSystemName;
	std::map<NETADDR, CMute> m_Mutes;
};

class CGameContext : public IGameServer
{
	IServer *m_pServer;
	IConfigManager *m_pConfigManager;
	CConfig *m_pConfig;
	IConsole *m_pConsole;
	IStorage *m_pStorage;
	IAntibot *m_pAntibot;
	std::unique_ptr<IMap> m_pMap;
	CLayers m_Layers;
	CCollision m_Collision;
	protocol7::CNetObjHandler m_NetObjHandler7;
	CNetObjHandler m_NetObjHandler;
	CTuningParams m_aTuningList[TuneZone::NUM];
	std::vector<std::string> m_vCensorlist;

	bool m_TeeHistorianActive;
	CTeeHistorian m_TeeHistorian;
	ASYNCIO *m_pTeeHistorianFile;
	CUuid m_GameUuid;
	CMapBugs m_MapBugs;
	CPrng m_Prng;
	CGameHost m_GameHost;

	bool m_Resetting;

	static std::optional<std::vector<int>> ClientsForVictim(int ClientId, const char *pVictim, void *pUser);
	static void CommandCallback(int ClientId, int FlagMask, const char *pCmd, IConsole::IResult *pResult, void *pUser);
	static void TeeHistorianWrite(const void *pData, int DataSize, void *pUser);

	static void ConTuneParam(IConsole::IResult *pResult, void *pUserData);
	static void ConToggleTuneParam(IConsole::IResult *pResult, void *pUserData);
	static void ConTuneReset(IConsole::IResult *pResult, void *pUserData);
	static void ConTunes(IConsole::IResult *pResult, void *pUserData);
	static void ConMapbug(IConsole::IResult *pResult, void *pUserData);
	static void ConPause(IConsole::IResult *pResult, void *pUserData);
	static void ConChangeMap(IConsole::IResult *pResult, void *pUserData);
	static void ConRestart(IConsole::IResult *pResult, void *pUserData);
	static void ConServerAlert(IConsole::IResult *pResult, void *pUserData);
	static void ConModAlert(IConsole::IResult *pResult, void *pUserData);
	static void ConBroadcast(IConsole::IResult *pResult, void *pUserData);
	static void ConSay(IConsole::IResult *pResult, void *pUserData);
	static void ConSetTeam(IConsole::IResult *pResult, void *pUserData);
	static void ConSetTeamAll(IConsole::IResult *pResult, void *pUserData);
	static void ConHotReload(IConsole::IResult *pResult, void *pUserData);
	static void ConAddVote(IConsole::IResult *pResult, void *pUserData);
	static void ConRemoveVote(IConsole::IResult *pResult, void *pUserData);
	static void ConForceVote(IConsole::IResult *pResult, void *pUserData);
	static void ConClearVotes(IConsole::IResult *pResult, void *pUserData);
	static void ConAddMapVotes(IConsole::IResult *pResult, void *pUserData);
	static void ConVote(IConsole::IResult *pResult, void *pUserData);
	static void ConVotes(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteNo(IConsole::IResult *pResult, void *pUserData);
	static void ConDumpAntibot(IConsole::IResult *pResult, void *pUserData);
	static void ConAntibot(IConsole::IResult *pResult, void *pUserData);
	static void ConchainSpecialMotdupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainSettingUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConDumpLog(IConsole::IResult *pResult, void *pUserData);

	void AddVote(const char *pDescription, const char *pCommand);
	static int MapScan(const char *pName, int IsDir, int DirType, void *pUserData);

	class CPersistentData
	{
	public:
		CUuid m_PrevGameUuid;
	};

	class CPersistentClientData
	{
	public:
		bool m_IsSpectator;
		bool m_IsAfk;
		int m_LastWhisperTo;
	};

public:
	IServer *Server() const { return m_pServer; }
	IConfigManager *ConfigManager() const { return m_pConfigManager; }
	CConfig *Config() { return m_pConfig; }
	IConsole *Console() { return m_pConsole; }
	IStorage *Storage() { return m_pStorage; }
	IMap *Map() override { return m_pMap.get(); }
	const IMap *Map() const override { return m_pMap.get(); }
	CCollision *Collision() { return &m_Collision; }
	CTuningParams *GlobalTuning() { return &m_aTuningList[0]; }
	CTuningParams *TuningList() { return m_aTuningList; }
	IAntibot *Antibot() { return m_pAntibot; }
	CTeeHistorian *TeeHistorian() { return &m_TeeHistorian; }
	bool TeeHistorianActive() const { return m_TeeHistorianActive; }
	CNetObjHandler *GetNetObjHandler() override { return &m_NetObjHandler; }
	protocol7::CNetObjHandler *GetNetObjHandler7() override { return &m_NetObjHandler7; }

	CGameContext(bool Resetting = false);
	~CGameContext() override;

	void Clear();

	CEventHandler m_Events;
	CPlayer *m_apPlayers[MAX_CLIENTS];
	// keep last input to always apply when none is sent
	CNetObj_PlayerInput m_aLastPlayerInput[MAX_CLIENTS];
	bool m_aPlayerHasInput[MAX_CLIENTS];

	// returns last input if available otherwise nulled PlayerInput object
	// ClientId has to be valid
	CNetObj_PlayerInput GetLastPlayerInput(int ClientId) const;

	CGameHost &GameHost() { return m_GameHost; }
	const CGameHost &GameHost() const { return m_GameHost; }
	CGameWorld m_World;
	CPlayerMapping m_PlayerMapping;

	// helper functions
	CCharacter *GetPlayerChar(int ClientId);
	const CCharacter *GetPlayerChar(int ClientId) const;
	const CPlayer *FindPlayerByName(const char *pName) const;
	// Returns `nullptr` if no player is found.
	CPlayer *FindPlayerByName(const char *pName);
	std::optional<int> FindClientIdByName(const char *pName) const;
	bool EmulateBug(int Bug) const;
	std::vector<SSwitchers> &Switchers() { return m_World.m_Core.m_vSwitchers; }

	// voting
	void StartVote(const char *pDesc, const char *pCommand, const char *pReason, const char *pSixupDesc);
	void EndVote();
	void SendVoteSet(int ClientId);
	void SendVoteStatus(int ClientId, int Total, int Yes, int No);
	void AbortVoteKickOnDisconnect(int ClientId);

	int m_VoteCreator;
	int m_VoteType;
	int64_t m_VoteCloseTime;
	bool m_VoteUpdate;
	int m_VotePos;
	char m_aVoteDescription[VOTE_DESC_LENGTH];
	char m_aSixupVoteDescription[VOTE_DESC_LENGTH];
	char m_aVoteCommand[VOTE_CMD_LENGTH];
	char m_aVoteReason[VOTE_REASON_LENGTH];
	int m_NumVoteOptions;
	int m_VoteEnforce;
	char m_aaZoneEnterMsg[TuneZone::NUM][256]; // 0 is used for switching from or to area without tunings
	char m_aaZoneLeaveMsg[TuneZone::NUM][256];

	void CreateAllEntities(bool Initial);
	CPlayer *CreatePlayer(int ClientId, int StartTeam, bool Afk, int LastWhisperTo);

	char m_aDeleteTempfile[128];
	void DeleteTempfile();

	enum
	{
		VOTE_ENFORCE_UNKNOWN = 0,
		VOTE_ENFORCE_NO,
		VOTE_ENFORCE_YES,
		VOTE_ENFORCE_NO_ADMIN,
		VOTE_ENFORCE_YES_ADMIN,
		VOTE_ENFORCE_ABORT,
		VOTE_ENFORCE_CANCEL,
	};
	CHeap *m_pVoteOptionHeap;
	CVoteOptionServer *m_pVoteOptionFirst;
	CVoteOptionServer *m_pVoteOptionLast;

	// helper functions
	void CreateDamageInd(vec2 Pos, float AngleMod, int Amount, CClientMask Mask = CClientMask().set());
	void CreateExplosion(vec2 Pos, int Owner, int Weapon, bool NoDamage, int ActivatedTeam, CClientMask Mask = CClientMask().set(), int AttackerTeam = TEAM_SPECTATORS);
	void CreateExplosionEvent(vec2 Pos, CClientMask Mask = CClientMask().set());
	void CreateHammerHit(vec2 Pos, CClientMask Mask = CClientMask().set());
	void CreatePlayerSpawn(vec2 Pos, CClientMask Mask = CClientMask().set());
	void CreateDeath(vec2 Pos, int ClientId, CClientMask Mask = CClientMask().set());
	void CreateBirthdayEffect(vec2 Pos, CClientMask Mask = CClientMask().set());
	void CreateFinishEffect(vec2 Pos, CClientMask Mask = CClientMask().set());
	void CreateSound(vec2 Pos, int Sound, CClientMask Mask = CClientMask().set());
	void CreateSoundGlobal(int Sound, int Target = -1) const;

	void SnapLaserObject(const CSnapContext &Context, int SnapId, const vec2 &To, const vec2 &From, int StartTick, int Owner = -1, int LaserType = -1, int Subtype = -1, int SwitchNumber = -1) const;
	void SnapPickup(const CSnapContext &Context, int SnapId, const vec2 &Pos, int Type, int SubType, int SwitchNumber, int Flags) const;

	enum
	{
		FLAG_SIX = 1 << 0,
		FLAG_SIXUP = 1 << 1,
	};

	// network
	void CreateSoundGlobal(int Sound, int Target, int VersionFlags) const;
	void SendGameMessage7(int GameMessageId, std::initializer_list<int> Parameters = {}, int Target = -1) const;
	void CallVote(int ClientId, const char *pDesc, const char *pCmd, const char *pReason, const char *pChatmsg, const char *pSixupDesc = nullptr);
	void SendChatTarget(int To, const char *pText, int VersionFlags = FLAG_SIX | FLAG_SIXUP) const;
	void SendChatTeam(int Team, const char *pText) const;
	void SendChat(int ClientId, int Team, const char *pText, int SpamProtectionClientId = -1, int VersionFlags = FLAG_SIX | FLAG_SIXUP);
	void SendEmoticon(int ClientId, int Emoticon, int TargetClientId) const;
	void SendWeaponPickup(int ClientId, int Weapon) const;
	void SendMotd(int ClientId) const;
	void SendSettings(int ClientId) const;
	void SendServerAlert(const char *pMessage);
	void SendModeratorAlert(int ToClientId, const char *pMessage);
	void SendBroadcast(const char *pText, int ClientId, bool IsImportant = true);

	/**
	 * The 0.7 protocol does not support renaming connected clients (or changing clan/country).
	 * But the 0.6 protocol does allow that. And the server supports both.
	 * So when a 0.6 client renames we update the state for 0.7 clients
	 * by reconnecting the renamed client. This is abstracted away by this method.
	 * During the reconnect also other properties than name are being resent and potentially
	 * updated. Those are: name, country, clan, team and skin
	 *
	 * @param ClientId This is the id of the client that will be updated. Not the id that will receive the message. The message gets broadcasted to all 0.7 clients.
	 */
	void SendRename7(int ClientId);
	void SendSkinChange7(int ClientId);

	void List(int ClientId, const char *pFilter);

	void SendTuningParams(int ClientId, int Zone = 0);

	void ProgressVoteOptions(int ClientId);

	//
	void LoadMapSettings();

	// engine events
	void OnInit(const void *pPersistentData) override;
	void OnConsoleInit() override;
	void RegisterModerationCommands();
	void RegisterChatCommands();
	[[nodiscard]] bool OnMapChange(char *pNewMapName, int MapNameSize) override;
	void OnShutdown(void *pPersistentData) override;

	void OnTick() override;
	void OnSnap(int ClientId, bool GlobalSnap, bool RecordingDemo) override;
	void OnPostGlobalSnap() override;

	void *PreProcessMsg(int *pMsgId, CUnpacker *pUnpacker, int ClientId);
	void CensorMessage(char *pCensoredMessage, const char *pMessage, int Size);
	void OnMessage(int MsgId, CUnpacker *pUnpacker, int ClientId) override;
	void OnSayNetMessage(const CNetMsg_Cl_Say *pMsg, int ClientId, const CUnpacker *pUnpacker);
	void OnCallVoteNetMessage(const CNetMsg_Cl_CallVote *pMsg, int ClientId);
	void OnVoteNetMessage(const CNetMsg_Cl_Vote *pMsg, int ClientId);
	void OnIsDDNetLegacyNetMessage(const CNetMsg_Cl_IsDDNetLegacy *pMsg, int ClientId, CUnpacker *pUnpacker);
	void OnShowDistanceNetMessage(const CNetMsg_Cl_ShowDistance *pMsg, int ClientId);
	void OnCameraInfoNetMessage(const CNetMsg_Cl_CameraInfo *pMsg, int ClientId);
	void OnSetSpectatorModeNetMessage(const CNetMsg_Cl_SetSpectatorMode *pMsg, int ClientId);
	void OnChangeInfoNetMessage(const CNetMsg_Cl_ChangeInfo *pMsg, int ClientId);
	void OnEmoticonNetMessage(const CNetMsg_Cl_Emoticon *pMsg, int ClientId);
	void OnEnableSpectatorCountNetMessage(const CNetMsg_Cl_EnableSpectatorCount *pMsg, int ClientId);
	void OnStartInfoNetMessage(const CNetMsg_Cl_StartInfo *pMsg, int ClientId);

	bool OnClientDataPersist(int ClientId, void *pData) override;
	void OnClientConnected(int ClientId, void *pData) override;
	void OnClientEnter(int ClientId) override;
	void OnClientDrop(int ClientId, const char *pReason) override;
	void OnClientInfoChange(int ClientId) override;
	void OnClientPrepareInput(int ClientId, void *pInput) override;
	void OnClientDirectInput(int ClientId, const void *pInput) override;
	void OnClientPredictedInput(int ClientId, const void *pInput) override;
	void OnClientPredictedEarlyInput(int ClientId, const void *pInput) override;

	void PreInputClients(int ClientId, bool *pClients) override;

	void TeehistorianRecordAntibot(const void *pData, int DataSize) override;
	void TeehistorianRecordPlayerJoin(int ClientId, bool Sixup) override;
	void TeehistorianRecordPlayerDrop(int ClientId, const char *pReason) override;
	void TeehistorianRecordPlayerRejoin(int ClientId) override;
	void TeehistorianRecordPlayerName(int ClientId, const char *pName) override;
	void TeehistorianRecordPlayerFinish(int ClientId, int TimeTicks) override;
	void TeehistorianRecordTeamFinish(int TeamId, int TimeTicks) override;
	void TeehistorianRecordAuthLogin(int ClientId, int Level, const char *pAuthName) override;

	bool IsClientReady(int ClientId) const override;
	bool IsClientPlayer(int ClientId) const override;
	// Whether the client is allowed to have high bandwidth.
	bool IsClientHighBandwidth(int ClientId) const override;
	int PersistentDataSize() const override { return sizeof(CPersistentData); }
	int PersistentClientDataSize() const override { return sizeof(CPersistentClientData); }

	CUuid GameUuid() const override;
	const char *GameType() const override;
	const char *ClientScoreKind() const override;
	char m_aVersionString[32];
	const char *Version() const override;
	const char *NetVersion() const override;

	void OnPreTickTeehistorian() override;
	bool OnClientDDNetVersionKnown(int ClientId);
	void FillAntibot(CAntibotRoundData *pData) override;
	bool ProcessSpamProtection(int ClientId, bool RespectChatInitialDelay = true);
	// Describes the time when the first player joined the server.
	int64_t m_NonEmptySince;
	int64_t m_LastMapVote;
	int GetClientVersion(int ClientId) const;
	CClientMask ClientsMaskExcludeClientVersionAndHigher(int Version) const;
	bool PlayerExists(int ClientId) const override { return m_apPlayers[ClientId]; }
	// Returns true if someone is actively moderating.
	bool PlayerModerating() const;
	void ForceVote(bool Success);

	// Checks if player can vote and notify them about the reason
	bool RateLimitPlayerVote(int ClientId);
	bool RateLimitPlayerMapVote(int ClientId) const;

	void OnUpdatePlayerServerInfo(CJsonWriter *pJsonWriter, int ClientId) override;
	void ReadCensorList();

private:
	// starting 1 to make 0 the special value "no client id"
	uint32_t m_NextUniqueClientId = 1;
	bool m_VoteWillPass;
	static void ConKillPlayer(IConsole::IResult *pResult, void *pUserData);
	static void ConDamagePlayer(IConsole::IResult *pResult, void *pUserData);

	static void ConHelp(IConsole::IResult *pResult, void *pUserData);
	static void ConRules(IConsole::IResult *pResult, void *pUserData);
	static void ConKill(IConsole::IResult *pResult, void *pUserData);
	static void ConDND(IConsole::IResult *pResult, void *pUserData);
	static void ConWhispers(IConsole::IResult *pResult, void *pUserData);
	static void ConTimeout(IConsole::IResult *pResult, void *pUserData);
	static void ConWhisper(IConsole::IResult *pResult, void *pUserData);
	static void ConConverse(IConsole::IResult *pResult, void *pUserData);
	static void ConSetEyeEmote(IConsole::IResult *pResult, void *pUserData);
	static void ConEyeEmote(IConsole::IResult *pResult, void *pUserData);
	static void ConShowAll(IConsole::IResult *pResult, void *pUserData);

	static void ConModerate(IConsole::IResult *pResult, void *pUserData);

	static void ConList(IConsole::IResult *pResult, void *pUserData);

	static void ConReloadCensorlist(IConsole::IResult *pResult, void *pUserData);

	CMutes m_Mutes;
	CMutes m_VoteMutes;
	void MuteWithMessage(const NETADDR *pAddr, int Seconds, const char *pReason, const char *pDisplayName);
	void VoteMuteWithMessage(const NETADDR *pAddr, int Seconds, const char *pReason, const char *pDisplayName);

	static void ConMute(IConsole::IResult *pResult, void *pUserData);
	static void ConMuteId(IConsole::IResult *pResult, void *pUserData);
	static void ConMuteIp(IConsole::IResult *pResult, void *pUserData);
	static void ConUnmute(IConsole::IResult *pResult, void *pUserData);
	static void ConUnmuteId(IConsole::IResult *pResult, void *pUserData);
	static void ConUnmuteIp(IConsole::IResult *pResult, void *pUserData);
	static void ConMutes(IConsole::IResult *pResult, void *pUserData);

	static void ConVoteMute(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteMuteId(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteMuteIp(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteUnmute(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteUnmuteId(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteUnmuteIp(IConsole::IResult *pResult, void *pUserData);
	static void ConVoteMutes(IConsole::IResult *pResult, void *pUserData);

	void Whisper(int ClientId, char *pStr);
	int WhisperRecordFlag(int ClientId) const;
	void WhisperId(int ClientId, int VictimId, const char *pMessage);
	void Converse(int ClientId, char *pStr);
	bool IsVersionBanned(int Version);
	enum
	{
		MAX_LOG_SECONDS = 600,
		MAX_LOGS = 512,
	};
	struct CLog
	{
		int64_t m_Timestamp;
		bool m_FromServer;
		char m_aDescription[256 + 8];
		int m_ClientVersion;
		char m_aClientName[MAX_NAME_LENGTH];
		char m_aClientAddrStr[NETADDR_MAXSTRSIZE];
	};
	CLog m_aLogs[MAX_LOGS];
	int m_LatestLog;

	void LogEvent(const char *Description, int ClientId);

public:
	CLayers *Layers() { return &m_Layers; }
	enum
	{
		VOTE_TYPE_UNKNOWN = 0,
		VOTE_TYPE_OPTION,
		VOTE_TYPE_KICK,
		VOTE_TYPE_SPECTATE,
	};
	int m_VoteVictim;

	bool IsOptionVote() const { return m_VoteType == VOTE_TYPE_OPTION; }
	bool IsKickVote() const { return m_VoteType == VOTE_TYPE_KICK; }
	bool IsSpecVote() const { return m_VoteType == VOTE_TYPE_SPECTATE; }

	bool IsRunningVote(int ClientId) const;
	bool IsRunningKickOrSpecVote(int ClientId) const;

	void OnSetAuthed(int ClientId, int Level) override;
	void ReinitPlayerMap(int ClientId, bool Timeout) override;
};

static inline bool CheckClientId(int ClientId)
{
	return ClientId >= 0 && ClientId < MAX_CLIENTS;
}

#endif
