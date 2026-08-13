/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_H
#define ENGINE_CLIENT_CLIENT_H

#include "graph.h"
#include "session_sources.h"
#include "smooth_time.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/client.h>
#include <engine/client/checksum.h>
#include <engine/client/friends.h>
#include <engine/client/ghost.h>
#include <engine/client/serverbrowser.h>
#include <engine/client/updater.h>
#include <engine/editor.h>
#include <engine/graphics.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/shared/demo.h>
#include <engine/shared/fifo.h>
#include <engine/shared/network.h>
#include <engine/textrender.h>
#include <engine/warning.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>

class CDemoEdit;
class IDemoRecorder;
class CMsgPacker;
class CUnpacker;
class IConfigManager;
class IDiscord;
class IEngine;
class IEngineInput;
class IEngineSound;
class IFriends;
class ILogger;
class ISteam;
class INotifications;
class IStorage;
class IUpdater;

class CServerCapabilities
{
public:
	bool m_ChatTimeoutCode = false;
	bool m_AnyPlayerFlag = false;
	bool m_PingEx = false;
	bool m_AllowDummy = false;
	bool m_SyncWeaponInput = false;
};

class CClient : public IClient, public CDemoPlayer::IListener
{
	// needed interfaces
	IConfigManager *m_pConfigManager = nullptr;
	CConfig *m_pConfig = nullptr;
	IConsole *m_pConsole = nullptr;
	IDiscord *m_pDiscord = nullptr;
	IEditor *m_pEditor = nullptr;
	IEngine *m_pEngine = nullptr;
	IFavorites *m_pFavorites = nullptr;
	IGameClient *m_pGameClient = nullptr;
	IEngineGraphics *m_pGraphics = nullptr;
	IEngineHttp *m_pHttp = nullptr;
	IEngineInput *m_pInput = nullptr;
	IEngineSound *m_pSound = nullptr;
	ISteam *m_pSteam = nullptr;
	INotifications *m_pNotifications = nullptr;
	IStorage *m_pStorage = nullptr;
	IEngineTextRender *m_pTextRender = nullptr;
	IUpdater *m_pUpdater = nullptr;

	CSessionManager m_SessionManager;
	CSessionId m_NetworkSessionId;
	CSessionId m_DemoSessionId;
	CNetworkSessionSource *m_pNetworkSessionSource = nullptr;
	CDemoSessionSource *m_pDemoSessionSource = nullptr;
	CNetClient m_ContactNetClient;
	CDemoRecorder m_aDemoRecorders[RECORDER_MAX];
	CDemoRecorder m_aDemoRecordersSixup[RECORDER_MAX];
	CDemoEditor m_DemoEditor;
	CGhostRecorder m_GhostRecorder;
	CGhostLoader m_GhostLoader;
	CServerBrowser m_ServerBrowser;
	CUpdater m_Updater;
	CFriends m_Friends;
	CFriends m_Foes;

	char m_aConnectAddressStr[MAX_SERVER_ADDRESSES * NETADDR_MAXSTRSIZE] = "";

	CUuid m_ConnectionId = UUID_ZEROED;
	bool m_HaveGlobalTcpAddr = false;
	NETADDR m_GlobalTcpAddr = NETADDR_ZEROED;

	int64_t m_LocalStartTime = 0;
	int64_t m_GlobalStartTime = 0;

	IGraphics::CTextureHandle m_DebugFont;

	int64_t m_LastRenderTime;

	int m_SnapCrcErrors = 0;
	bool m_AutoScreenshotRecycle = false;
	bool m_AutoStatScreenshotRecycle = false;
	bool m_AutoCSVRecycle = false;
	bool m_EditorActive = false;

	char m_aRconUsername[64] = "";
	char m_aRconPassword[sizeof(g_Config.m_SvRconPassword)] = "";
	int m_UseTempRconCommands = 0;
	int m_ExpectedRconCommands = -1;
	int m_GotRconCommands = 0;
	char m_aPassword[sizeof(g_Config.m_Password)] = "";
	bool m_SendPassword = false;

	int m_ExpectedMaplistEntries = -1;
	std::vector<std::string> m_vMaplistEntries;

	// version-checking
	char m_aVersionStr[10] = "0";

	// pinging
	int64_t m_PingStartTime = 0;

	bool m_GenerateTimeoutSeed = true;

	char m_aCmdConnect[256] = "";
	char m_aCmdPlayDemo[IO_MAX_PATH_LENGTH] = "";
	char m_aCmdEditMap[IO_MAX_PATH_LENGTH] = "";

	// map download
	char m_aMapDownloadUrl[256] = "";
	std::shared_ptr<IHttpRequest> m_pMapdownloadTask = nullptr;
	char m_aMapdownloadFilename[256] = "";
	char m_aMapdownloadFilenameTemp[256] = "";
	char m_aMapdownloadName[256] = "";
	IOHANDLE m_MapdownloadFileTemp = nullptr;
	int m_MapdownloadChunk = 0;
	int m_MapdownloadCrc = 0;
	int m_MapdownloadAmount = -1;
	int m_MapdownloadTotalsize = -1;
	std::optional<SHA256_DIGEST> m_MapdownloadSha256;

	class CMapDetails
	{
	public:
		char m_aName[256];
		int m_Size;
		int m_Crc;
		SHA256_DIGEST m_Sha256;
		char m_aUrl[256];
	};
	std::optional<CMapDetails> m_MapDetails;

	EInfoState m_InfoState = EInfoState::ERROR;
	std::shared_ptr<IHttpRequest> m_pDDNetInfoTask = nullptr;

	// time
	int m_LastActiveConnection = CONN_MAIN;
	bool m_DummySendConnInfo = false;
	bool m_DummyConnecting = false;
	bool m_DummyConnected = false;
	float m_LastDummyConnectTime = 0.0f;
	bool m_DummyReconnectOnReload = false;
	bool m_DummyDeactivateOnReconnect = false;

	// graphs
	CGraph m_FpsGraph;

	CSnapshotDelta *SnapshotDelta();
	CSessionSourceBase &SessionSource(CSessionId SessionId)
	{
		if(SessionId == m_DemoSessionId)
			return *m_pDemoSessionSource;
		dbg_assert(SessionId == m_NetworkSessionId, "invalid game session");
		return *m_pNetworkSessionSource;
	}
	const CSessionSourceBase &SessionSource(CSessionId SessionId) const
	{
		if(SessionId == m_DemoSessionId)
			return *m_pDemoSessionSource;
		dbg_assert(SessionId == m_NetworkSessionId, "invalid game session");
		return *m_pNetworkSessionSource;
	}
	CConnection &Connection(int Conn)
	{
		return m_pNetworkSessionSource->ConnectionAt(Conn);
	}
	const CConnection &Connection(int Conn) const
	{
		return m_pNetworkSessionSource->ConnectionAt(Conn);
	}
	CConnection &Connection(CSessionId SessionId, int Conn)
	{
		if(SessionId == m_DemoSessionId)
		{
			dbg_assert(Conn == CONN_MAIN, "invalid demo stream");
			return m_pDemoSessionSource->Connection();
		}
		dbg_assert(SessionId == m_NetworkSessionId, "invalid game session");
		return Connection(Conn);
	}
	const CConnection &Connection(CSessionId SessionId, int Conn) const
	{
		if(SessionId == m_DemoSessionId)
		{
			dbg_assert(Conn == CONN_MAIN, "invalid demo stream");
			return m_pDemoSessionSource->Connection();
		}
		dbg_assert(SessionId == m_NetworkSessionId, "invalid game session");
		return Connection(Conn);
	}
	CDemoPlayer &DemoPlayer() { return m_pDemoSessionSource->DemoPlayer(); }
	const CDemoPlayer &DemoPlayer() const { return m_pDemoSessionSource->DemoPlayer(); }
	CNetClient &NetClient(int Conn)
	{
		dbg_assert(Conn >= CONN_MAIN && Conn < NUM_CONNS, "invalid network connection");
		return Conn == CONN_CONTACT ? m_ContactNetClient : Connection(Conn).m_NetClient;
	}
	const CNetClient &NetClient(int Conn) const
	{
		dbg_assert(Conn >= CONN_MAIN && Conn < NUM_CONNS, "invalid network connection");
		return Conn == CONN_CONTACT ? m_ContactNetClient : Connection(Conn).m_NetClient;
	}

	std::deque<std::shared_ptr<CDemoEdit>> m_EditJobs;

	//
	bool m_CanReceiveServerCapabilities = false;
	bool m_ServerSentCapabilities = false;
	CServerCapabilities m_ServerCapabilities;

	bool ServerCapAnyPlayerFlag() const override { return m_ServerCapabilities.m_AnyPlayerFlag; }

	int64_t m_CurrentServerInfoRequestTime = -1; // >= 0 should request, == -1 got info

	int m_CurrentServerPingInfoType = -1;
	int m_CurrentServerPingBasicToken = -1;
	int m_CurrentServerPingToken = -1;
	CUuid m_CurrentServerPingUuid = UUID_ZEROED;
	int64_t m_CurrentServerCurrentPingTime = -1; // >= 0 request running
	int64_t m_CurrentServerNextPingTime = -1; // >= 0 should request

	// version info
	struct CVersionInfo
	{
		enum
		{
			STATE_INIT = 0,
			STATE_START,
			STATE_READY,
		};

		int m_State = STATE_INIT;
	} m_VersionInfo;

	std::mutex m_WarningsMutex;
	std::vector<SWarning> m_vWarnings;
	std::vector<SWarning> m_vQuittingWarnings;

	CFifo m_Fifo;

	IOHANDLE m_BenchmarkFile = nullptr;
	int64_t m_BenchmarkStopTime = 0;

	CChecksum m_Checksum;
	int64_t m_OwnExecutableSize = 0;
	IOHANDLE m_OwnExecutable = nullptr;

	// favorite command handling
	bool m_FavoritesGroup = false;
	bool m_FavoritesGroupAllowPing = false;
	int m_FavoritesGroupNum = 0;
	NETADDR m_aFavoritesGroupAddresses[MAX_SERVER_ADDRESSES];

	void UpdateDemoIntraTimers();
	void UpdateDemoSession();
	void UpdateNetworkSession();
	void StopDemoSession(const char *pReason);
	void StopNetworkSession(const char *pReason);
	int MaxLatencyTicks() const;
	int PredictionMargin() const;

	std::shared_ptr<ILogger> m_pFileLogger = nullptr;
	std::shared_ptr<ILogger> m_pStdoutLogger = nullptr;

	// For RenderDebug function
	NETSTATS m_NetstatsPrev = {};
	NETSTATS m_NetstatsCurrent = {};
	std::chrono::nanoseconds m_NetstatsLastUpdate = std::chrono::nanoseconds(0);

	// For DummyName function
	char m_aAutomaticDummyName[MAX_NAME_LENGTH];

public:
	using IClient::ConnectionProblems;
	using IClient::GameTick;
	using IClient::GameTickTime;
	using IClient::GetPredictionTick;
	using IClient::GetPredictionTime;
	using IClient::GetSmoothTick;
	using IClient::IntraGameTick;
	using IClient::IntraGameTickSincePrev;
	using IClient::PredGameTick;
	using IClient::PredIntraGameTick;
	using IClient::PrevGameTick;
	using IClient::ServerInfo;
	using IClient::SnapFindItem;
	using IClient::SnapGetItem;
	using IClient::SnapNumItems;

	CSessionId FocusedSessionId() const override { return m_SessionManager.FocusedId(); }
	CSessionId NetworkSessionId() const override { return m_NetworkSessionId; }
	CSessionId DemoSessionId() const override { return m_DemoSessionId; }
	ESessionState SessionState(CSessionId SessionId) const override { return SessionSource(SessionId).State(); }
	int PrevGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_PrevGameTick; }
	int GameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_CurGameTick; }
	int PredGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_PredTick; }
	float IntraGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_GameIntraTick; }
	float PredIntraGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_PredIntraTick; }
	float IntraGameTickSincePrev(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_GameIntraTickSincePrev; }
	float GameTickTime(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_GameTickTime; }

	IConfigManager *ConfigManager() { return m_pConfigManager; }
	CConfig *Config() { return m_pConfig; }
	IDiscord *Discord() { return m_pDiscord; }
	IEngine *Engine() { return m_pEngine; }
	IGameClient *GameClient() { return m_pGameClient; }
	const IGameClient *GameClient() const { return m_pGameClient; }
	IEngineGraphics *Graphics() { return m_pGraphics; }
	IEngineInput *Input() { return m_pInput; }
	IEngineSound *Sound() { return m_pSound; }
	ISteam *Steam() { return m_pSteam; }
	INotifications *Notifications() { return m_pNotifications; }
	IStorage *Storage() { return m_pStorage; }
	IEngineTextRender *TextRender() { return m_pTextRender; }
	IUpdater *Updater() { return m_pUpdater; }
	IHttp *Http() { return m_pHttp; }

	CClient();

	// ----- send functions -----
	int SendMsg(int Conn, CMsgPacker *pMsg, int Flags) override;

	void SendInfo(int Conn);
	void SendEnterGame(int Conn);
	void SendReady(int Conn);
	void SendMapRequest();

	bool RconAuthed() const override { return Connection(ActiveConnection()).m_RconAuthed != 0; }
	bool UseTempRconCommands() const override { return m_UseTempRconCommands != 0; }
	void RconAuth(int Conn, const char *pName, const char *pPassword) override;
	void Rcon(const char *pCmd) override;
	bool ReceivingRconCommands() const override { return m_ExpectedRconCommands > 0; }
	float GotRconCommandsPercentage() const override;
	bool ReceivingMaplist() const override { return m_ExpectedMaplistEntries > 0; }
	float GotMaplistPercentage() const override;
	const std::vector<std::string> &MaplistEntries() const override { return m_vMaplistEntries; }

	bool ConnectionProblems(CSessionId SessionId, int Conn) const override;

	IGraphics::CTextureHandle GetDebugFont() const override { return m_DebugFont; }

	void SendInput();

	// TODO: OPT: do this a lot smarter!
	int *GetInput(int Conn, int Tick) const override;

	const char *LatestVersion() const override;

	// ------ state handling -----
	void SetState(EClientState State);
	void SetFocusedState(EClientState State, bool ResetSession);
	void FocusSession(CSessionId SessionId);
	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	// called when the map is loaded and we should init for a new round
	void OnEnterGame(int Conn);
	void EnterGame(CSessionId SessionId, int Conn) override;

	// called once after being ingame for 1 second
	void OnPostConnect(int Conn);

	void Connect(const char *pAddress, const char *pPassword = nullptr) override;
	void DisconnectWithReason(const char *pReason);
	void DisconnectDemoWithReason(const char *pReason);
	void Disconnect() override;

	void DummyDisconnect(const char *pReason) override;
	void DummyConnect() override;
	bool DummyConnected() const override;
	bool DummyConnecting() const override;
	bool DummyConnectingDelayed() const override;
	bool DummyAllowed() const override;

	const CServerInfo &ServerInfo(CSessionId SessionId) const override { return SessionSource(SessionId).ServerInfo(); }
	void ServerInfoRequest();
	void SetCurrentServerInfo(const CServerInfo &ServerInfo);

	void LoadDebugFont();

	// ---

	int GetPredictionTime(CSessionId SessionId, int Conn) override;
	CSnapItem SnapGetItem(CSessionId SessionId, int Conn, int SnapId, int Index) const override;
	int GetPredictionTick(CSessionId SessionId, int Conn) override;
	const void *SnapFindItem(CSessionId SessionId, int Conn, int SnapId, int Type, int Id) const override;
	int SnapNumItems(CSessionId SessionId, int Conn, int SnapId) const override;
	void SnapSetStaticsize(int ItemType, int Size) override;
	void SnapSetStaticsize7(int ItemType, int Size) override;

	void Render();
	void RenderDebug();
	void RenderGraphs();

	void Restart() override;
	void Quit() override;
	void ResetSocket();

	const char *PlayerName() const override;
	const char *DummyName() override;
	const char *ErrorString() const override;

	const char *LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc);

	int TranslateSysMsg(int *pMsgId, bool System, CUnpacker *pUnpacker, CPacker *pPacker, CNetChunk *pPacket, bool *pIsExMsg);

	bool PreprocessConnlessPacket7(CNetChunk *pPacket);
	void ProcessConnlessPacket(CNetChunk *pPacket);
	void ProcessServerInfo(int Type, NETADDR *pFrom, const void *pData, int DataSize);
	void ProcessServerPacket(CNetChunk *pPacket, int Conn);

	int UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo);

	void ResetMapDownload(bool ResetActive);
	void FinishMapDownload();

	EInfoState InfoState() const override { return m_InfoState; }
	void RequestDDNetInfo() override;
	void ResetDDNetInfoTask();
	void LoadDDNetInfo();

	bool IsSixup(CSessionId SessionId) const override { return SessionSource(SessionId).IsSixup(); }
	CTranslationContext &TranslationContext(CSessionId SessionId) override { return SessionSource(SessionId).TranslationContext(); }
	const CTranslationContext &TranslationContext(CSessionId SessionId) const override { return SessionSource(SessionId).TranslationContext(); }

	const NETADDR &ServerAddress() const override { return *NetClient(CONN_MAIN).ServerAddress(); }
	int ConnectNetTypes() const override;
	const char *ConnectAddressString() const override { return m_aConnectAddressStr; }
	const char *MapDownloadName() const override { return m_aMapdownloadName; }
	int MapDownloadAmount() const override { return !m_pMapdownloadTask ? m_MapdownloadAmount : (int)m_pMapdownloadTask->Current(); }
	int MapDownloadTotalsize() const override { return !m_pMapdownloadTask ? m_MapdownloadTotalsize : (int)m_pMapdownloadTask->Size(); }

	void PumpNetwork();

	void OnDemoPlayerSnapshot(void *pData, int Size) override;
	void OnDemoPlayerMessage(void *pData, int Size) override;

	void Update();

	void RegisterInterfaces();
	void InitInterfaces();

	void Run();

	bool InitNetworkClient(char *pError, size_t ErrorSize);
	bool InitNetworkClientImpl(NETADDR BindAddr, int Conn, char *pError, size_t ErrorSize);
	bool CtrlShiftKey(int Key, bool &Last);

	static void Con_Connect(IConsole::IResult *pResult, void *pUserData);
	static void Con_Disconnect(IConsole::IResult *pResult, void *pUserData);

	static void Con_DummyConnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyDisconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyResetInput(IConsole::IResult *pResult, void *pUserData);

	static void Con_Quit(IConsole::IResult *pResult, void *pUserData);
	static void Con_Restart(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoPlay(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoSpeed(IConsole::IResult *pResult, void *pUserData);
	static void Con_Minimize(IConsole::IResult *pResult, void *pUserData);
	static void Con_Ping(IConsole::IResult *pResult, void *pUserData);
	static void ConNetReset(IConsole::IResult *pResult, void *pUserData);
	static void Con_Screenshot(IConsole::IResult *pResult, void *pUserData);

#if defined(CONF_VIDEORECORDER)
	void StartVideo(const char *pFilename, bool WithTimestamp);
	static void Con_StartVideo(IConsole::IResult *pResult, void *pUserData);
	static void Con_StopVideo(IConsole::IResult *pResult, void *pUserData);
	const char *DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, int SpeedIndex, bool StartPaused = false) override;
#endif

	static void Con_Rcon(IConsole::IResult *pResult, void *pUserData);
	static void Con_RconAuth(IConsole::IResult *pResult, void *pUserData);
	static void Con_RconLogin(IConsole::IResult *pResult, void *pUserData);
	static void Con_BeginFavoriteGroup(IConsole::IResult *pResult, void *pUserData);
	static void Con_EndFavoriteGroup(IConsole::IResult *pResult, void *pUserData);
	static void Con_AddFavorite(IConsole::IResult *pResult, void *pUserData);
	static void Con_RemoveFavorite(IConsole::IResult *pResult, void *pUserData);
	static void Con_Play(IConsole::IResult *pResult, void *pUserData);
	static void Con_Record(IConsole::IResult *pResult, void *pUserData);
	static void Con_StopRecord(IConsole::IResult *pResult, void *pUserData);
	static void Con_AddDemoMarker(IConsole::IResult *pResult, void *pUserData);
	static void Con_BenchmarkQuit(IConsole::IResult *pResult, void *pUserData);
	static void ConchainServerBrowserUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainFullscreen(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainWindowBordered(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainWindowScreen(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainWindowVSync(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainWindowResize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainTimeoutSeed(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainPassword(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainReplays(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainInputFifo(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainNetReset(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainLoglevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainStdoutOutputLevel(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

	static void Con_DemoSlice(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoSliceBegin(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoSliceEnd(IConsole::IResult *pResult, void *pUserData);
	static void Con_SaveReplay(IConsole::IResult *pResult, void *pUserData);

	void RegisterCommands();

	const char *DemoPlayer_Play(const char *pFilename, int StorageType) override;
	void DemoRecorder_Start(const char *pFilename, bool WithTimestamp, int Recorder) override;
	void DemoRecorder_HandleAutoStart() override;
	void DemoRecorder_UpdateReplayRecorder() override;
	void DemoRecorder_AddDemoMarker(int Recorder);
	IDemoRecorder *DemoRecorder(int Recorder) override;
	CDemoRecorder (&DemoRecorders())[RECORDER_MAX];

	void AutoScreenshot_Start() override;
	void AutoStatScreenshot_Start() override;
	void AutoScreenshot_Cleanup();
	void AutoStatScreenshot_Cleanup();

	void AutoCSV_Start() override;
	void AutoCSV_Cleanup();

	void ServerBrowserUpdate() override;

	void HandleConnectAddress(const NETADDR *pAddr);
	void HandleConnectLink(const char *pLink);
	void HandleDemoPath(const char *pPath);
	void HandleMapPath(const char *pPath);

	virtual void InitChecksum();
	virtual int HandleChecksum(int Conn, CUuid Uuid, CUnpacker *pUnpacker);

	// gfx
	void Notify(const char *pTitle, const char *pMessage) override;
	void OnWindowResize() override;
	void BenchmarkQuit(int Seconds, const char *pFilename);

	void UpdateAndSwap() override;

	// DDRace

	void GenerateTimeoutSeed() override;
	void GenerateTimeoutCodes(const NETADDR *pAddrs, int NumAddrs);

	void RaceRecord_Start(const char *pFilename) override;
	void RaceRecord_Stop() override;
	bool RaceRecord_IsRecording() override;

	void DemoSliceBegin() override;
	void DemoSliceEnd() override;
	void DemoSlice(const char *pDstPath, CLIENTFUNC_FILTER pfnFilter, void *pUser) override;
	virtual void SaveReplay(int Length, const char *pFilename = "");

	bool EditorHasUnsavedData() const override { return m_pEditor->HasUnsavedData(); }

	IFriends *Foes() override { return &m_Foes; }

	void GetSmoothTick(CSessionId SessionId, int Conn, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) override;

	void AddWarning(const SWarning &Warning) override;
	std::optional<SWarning> CurrentWarning() override;
	std::vector<SWarning> &&QuittingWarnings() { return std::move(m_vQuittingWarnings); }

	CChecksumData *ChecksumData() override { return &m_Checksum.m_Data; }
	int UdpConnectivity(int NetType) override;

	bool ViewLink(const char *pLink) override;
	bool ViewFile(const char *pFilename) override;

#if defined(CONF_FAMILY_WINDOWS)
	void ShellRegister() override;
	void ShellUnregister() override;
#endif

	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override;
	void GetGpuInfoString(char (&aGpuInfo)[512]) override;
	void SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger);
};

#endif
