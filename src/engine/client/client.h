/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_H
#define ENGINE_CLIENT_CLIENT_H

#include "graph.h"
#include "render_trace.h"
#include "session_sources.h"
#include "smooth_time.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/client.h>
#include <engine/client/checksum.h>
#include <engine/client/connect_target.h>
#include <engine/client/friends.h>
#include <engine/client/ghost.h>
#include <engine/client/serverbrowser.h>
#include <engine/client/updater.h>
#include <engine/editor.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/http.h>
#include <engine/shared/config.h>
#include <engine/shared/demo.h>
#include <engine/shared/fifo.h>
#include <engine/shared/network.h>
#include <engine/shared/quic_transport.h>
#include <engine/textrender.h>
#include <engine/warning.h>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

class CDemoEdit;
class IDemoRecorder;
class CMsgPacker;
class CUnpacker;
#if defined(CONF_VIDEORECORDER)
class CVideo;
#endif
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

class CClient : public IClient
{
	// Hands what the demo player of one session reads to the client, together
	// with the session it belongs to.
	class CDemoListener : public CDemoPlayer::IListener
	{
		CClient *m_pClient = nullptr;
		CSessionId m_SessionId;

	public:
		CDemoListener() = default;
		CDemoListener(CClient *pClient, CSessionId SessionId) :
			m_pClient(pClient), m_SessionId(SessionId) {}
		void OnDemoPlayerSnapshot(void *pData, int Size) override { m_pClient->OnDemoSnapshot(m_SessionId, pData, Size); }
		void OnDemoPlayerMessage(void *pData, int Size) override { m_pClient->OnDemoMessage(m_SessionId, pData, Size); }
	};

	// needed interfaces
	IConfigManager *m_pConfigManager = nullptr;
	CConfig *m_pConfig = nullptr;
	IConsole *m_pConsole = nullptr;
	IDiscord *m_pDiscord = nullptr;
	IEditor *m_pEditor = nullptr;
	IEngine *m_pEngine = nullptr;
	IFavorites *m_pFavorites = nullptr;
	IGameClient *m_pGameClient = nullptr;
	IEngineGraphicsWindow *m_pWindow = nullptr;
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
	CDemoListener m_DemoListener;
#if defined(CONF_VIDEORECORDER)
	// A second demo session that renders queued exports in the background,
	// so that watching a demo and exporting one do not share a player.
	CSessionId m_VideoExportSessionId;
	CDemoSessionSource *m_pVideoExportSessionSource = nullptr;
	CDemoListener m_VideoExportDemoListener;
#endif
	CNetClient m_ContactNetClient;
	CQuicTransport m_QuicTransport;
	CQuicSessionId m_QuicSession;
	NETADDR m_QuicServerAddress = {};
	bool m_UseQuic = false;
	bool m_UseWebTransport = false;
	bool m_QuicConnected = false;
	// When the last message arrived over QUIC, for the connection warning
	int64_t m_QuicLastRecvTime = 0;
	CQuicKnownHosts m_QuicKnownHosts;
	CQuicIdentityCheck m_QuicIdentityCheck;
	CDemoRecorder m_aDemoRecorders[RECORDER_MAX];
	CDemoRecorder m_aDemoRecordersSixup[RECORDER_MAX];
	CDemoEditor m_DemoEditor;
	CGhostRecorder m_GhostRecorder;
	CGhostLoader m_GhostLoader;
	CServerBrowser m_ServerBrowser;
	CUpdater m_Updater;
	CFriends m_Friends;
	CFriends m_Foes;

	bool m_HaveGlobalTcpAddr = false;
	NETADDR m_GlobalTcpAddr = NETADDR_ZEROED;

	int64_t m_LocalStartTime = 0;
	int64_t m_GlobalStartTime = 0;

	IGraphics::CTextureHandle m_DebugFont;

	int64_t m_LastRenderTime;

	bool m_AutoScreenshotRecycle = false;
	bool m_AutoStatScreenshotRecycle = false;
	bool m_AutoCSVRecycle = false;
	bool m_EditorActive = false;

	char m_aRconUsername[64] = "";
	char m_aRconPassword[sizeof(g_Config.m_SvRconPassword)] = "";

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

	EInfoState m_InfoState = EInfoState::ERROR;
	std::shared_ptr<IHttpRequest> m_pDDNetInfoTask = nullptr;

	// time
	bool m_DummySendConnInfo = false;
	bool m_DummyConnecting = false;
	bool m_DummyConnected = false;
	float m_LastDummyConnectTime = 0.0f;
	bool m_DummyReconnectOnReload = false;
	bool m_DummyDeactivateOnReconnect = false;
#if defined(CONF_PLATFORM_IOS)
	bool m_DummyReconnectOnResume = false;
#endif

	// graphs
	CGraph m_FpsGraph;

#if defined(CONF_VIDEORECORDER)
	class CVideoExportJob
	{
	public:
		char m_aDemoPath[IO_MAX_PATH_LENGTH] = {};
		int m_StorageType = 0;
		char m_aVideoName[IO_MAX_PATH_LENGTH] = {};
		CVideoExportSettings m_Settings;
		int m_SpeedIndex = 0;
		bool m_ExactVideoPath = false;
	};

	std::unique_ptr<CVideo> m_pVideo;
	CSessionId m_VideoSessionId;
	bool m_VideoOfflineAudio = false;
	std::deque<CVideoExportJob> m_VideoExportQueue;
	std::optional<CVideoExportJob> m_ActiveVideoExport;
	bool m_VideoExportQueueRunning = false;
	bool m_LoadingQueuedVideoExport = false;
	// When the queue started waiting for the sound assets, so that the wait has
	// an end even if they never arrive.
	int64_t m_VideoExportSoundWaitStart = 0;
	char m_aVideoExportQueueError[256] = {};
	bool m_CommandLineVideoExport = false;
	char m_aCommandLineDemoPath[IO_MAX_PATH_LENGTH] = {};
	char m_aCommandLineVideoPath[IO_MAX_PATH_LENGTH] = {};
	CVideoExportSettings m_CommandLineVideoSettings;
	int m_CommandLineExitCode = 0;
	char m_aVideoError[256] = {};
	std::chrono::nanoseconds m_LastVideoProgressRender{0};
	void UpdateVideoExportQueue();
	const char *QueueVideoExport(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue, bool ExactVideoPath);
	CDemoPlayer *VideoDemoPlayer();
#endif

	CSnapshotDelta *SnapshotDelta();
	CSessionSourceBase &SessionSource(CSessionId SessionId)
	{
		CSessionSource *pSource = m_SessionManager.Find(SessionId);
		dbg_assert(pSource != nullptr, "invalid game session");
		return static_cast<CSessionSourceBase &>(*pSource);
	}
	const CSessionSourceBase &SessionSource(CSessionId SessionId) const
	{
		return const_cast<CClient *>(this)->SessionSource(SessionId);
	}
	CNetworkSessionSource &NetworkSource(CSessionId SessionId)
	{
		dbg_assert(SessionId == m_NetworkSessionId, "game session is not the network session");
		return *m_pNetworkSessionSource;
	}
	const CNetworkSessionSource &NetworkSource(CSessionId SessionId) const
	{
		return const_cast<CClient *>(this)->NetworkSource(SessionId);
	}
	CDemoSessionSource &DemoSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::DEMO, "game session is not a demo");
		return static_cast<CDemoSessionSource &>(Source);
	}
	const CDemoSessionSource &DemoSource(CSessionId SessionId) const
	{
		return const_cast<CClient *>(this)->DemoSource(SessionId);
	}
	CConnection &Connection(int Conn) { return m_pNetworkSessionSource->m_aConnections[Conn]; }
	const CConnection &Connection(int Conn) const { return m_pNetworkSessionSource->m_aConnections[Conn]; }
	CConnection &Connection(CSessionId SessionId, int Conn)
	{
		if(SessionSource(SessionId).Type() == ESessionSourceType::NETWORK)
			return NetworkSource(SessionId).m_aConnections[Conn];
		dbg_assert(Conn == CONN_MAIN, "a demo has only one connection");
		return DemoSource(SessionId).m_Connection;
	}
	const CConnection &Connection(CSessionId SessionId, int Conn) const
	{
		return const_cast<CClient *>(this)->Connection(SessionId, Conn);
	}
	CDemoPlayer &DemoPlayer() { return m_pDemoSessionSource->m_DemoPlayer; }
	const CDemoPlayer &DemoPlayer() const { return m_pDemoSessionSource->m_DemoPlayer; }
	CNetClient &NetClient(int Conn) { return Conn == CONN_CONTACT ? m_ContactNetClient : Connection(Conn).m_NetClient; }
	const CNetClient &NetClient(int Conn) const { return Conn == CONN_CONTACT ? m_ContactNetClient : Connection(Conn).m_NetClient; }

	std::deque<std::shared_ptr<CDemoEdit>> m_EditJobs;

	//
	bool ServerCapAnyPlayerFlag(CSessionId SessionId) const override { return NetworkSource(SessionId).m_ServerCapabilities.m_AnyPlayerFlag; }

	int64_t m_CurrentServerInfoRequestTime = -1; // >= 0 should request, == -1 got info

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
	uint64_t m_RenderWallTimeNanoseconds = 0;
	ITextRender::CTextRenderStats m_BenchmarkPreviousTextRenderStats;
	CRenderTrace m_RenderTrace;
	ITextRender::CTextRenderStats m_RenderTracePreviousTextRenderStats;

	CChecksum m_Checksum;
	int64_t m_OwnExecutableSize = 0;
	IOHANDLE m_OwnExecutable = nullptr;

	// favorite command handling
	bool m_FavoritesGroup = false;
	bool m_FavoritesGroupAllowPing = false;
	int m_FavoritesGroupNum = 0;
	NETADDR m_aFavoritesGroupAddresses[MAX_SERVER_ADDRESSES];

	void UpdateDemoIntraTimers(CSessionId SessionId);
	// The session being updated. A stop requested from within its update is
	// carried out once the update returns.
	CSessionId m_UpdatingSessionId;
	void UpdateSessions();
	void FinishStopSession(CSessionId SessionId);
	void UpdateDemoSession(CSessionId SessionId);
	void UpdateNetworkSession();
	void StopDemoSession(CSessionId SessionId, const char *pReason);
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
	CSessionId FocusedSessionId() const override { return m_SessionManager.FocusedId(); }
	CSessionId NetworkSessionId() const override { return m_NetworkSessionId; }
	CSessionId DemoSessionId() const override { return m_DemoSessionId; }
#if defined(CONF_VIDEORECORDER)
	CSessionId VideoExportSessionId() const override { return m_VideoExportSessionId; }
#endif
	ESessionSourceType SessionType(CSessionId SessionId) const override { return SessionSource(SessionId).Type(); }
	ESessionState SessionState(CSessionId SessionId) const override { return SessionSource(SessionId).State(); }
	bool DemoPlaybackPaused(CSessionId SessionId) const override { return DemoSource(SessionId).m_DemoPlayer.BaseInfo()->m_Paused; }
	float DemoPlaybackSpeed(CSessionId SessionId) const override { return DemoSource(SessionId).m_DemoPlayer.BaseInfo()->m_Speed; }
	int64_t DemoPlaybackTime(CSessionId SessionId) const override;
	float DemoPlaybackLocalTime(CSessionId SessionId) const override;
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
	IEngineGraphicsWindow *Window() { return m_pWindow; }
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
	~CClient() override;

	// ----- send functions -----
	int SendMsg(int Conn, CMsgPacker *pMsg, int Flags) override;
	// Send via the currently active client (main/dummy)
	int SendMsgActive(CMsgPacker *pMsg, int Flags) override;

	void SendInfo(int Conn);
	void SendEnterGame(int Conn);
	void SendReady(int Conn);
	void SendMapRequest();

	bool RconAuthed() const override { return Connection(ActiveConnection()).m_RconAuthed != 0; }
	bool UseTempRconCommands() const override { return m_pNetworkSessionSource->m_UseTempRconCommands != 0; }
	void RconAuth(const char *pName, const char *pPassword, bool Dummy = g_Config.m_ClDummy) override;
	void Rcon(const char *pCmd) override;
	bool ReceivingRconCommands() const override { return m_pNetworkSessionSource->m_ExpectedRconCommands > 0; }
	float GotRconCommandsPercentage() const override;
	bool ReceivingMaplist() const override { return m_pNetworkSessionSource->m_ExpectedMaplistEntries > 0; }
	float GotMaplistPercentage() const override;
	const std::vector<std::string> &MaplistEntries() const override { return m_pNetworkSessionSource->m_vMaplistEntries; }

	bool ConnectionProblems(CSessionId SessionId, int Conn) const override;

	IGraphics::CTextureHandle GetDebugFont() override;

	void SendInput();

	// TODO: OPT: do this a lot smarter!
	int *GetInput(CSessionId SessionId, int Conn, int Tick) const override;

	const char *LatestVersion() const override;
	int64_t ReconnectTime() const override { return m_pNetworkSessionSource->m_ReconnectTime; }
	void CancelReconnect() override { m_pNetworkSessionSource->CancelReconnect(); }

	// ------ state handling -----
	void SetState(EClientState State);
	void SetFocusedState(EClientState State, bool ResetSession);
	void FocusSession(CSessionId SessionId);
	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	// called when the map is loaded and we should init for a new round
	void OnEnterGame(int Conn);
	void EnterGame(int Conn) override;

	// called once after being ingame for 1 second
	void OnPostConnect(int Conn);

	void Connect(const char *pAddress, const char *pPassword = nullptr) override;
	void StopSession(CSessionId SessionId, const char *pReason);
	void DisconnectWithReason(const char *pReason) { StopSession(m_NetworkSessionId, pReason); }
	void Disconnect() override;

	void DummyDisconnect(const char *pReason) override;
	void DummyConnect() override;
	bool DummyConnected() const override;
	bool DummyConnecting() const override;
	bool DummyConnectingDelayed() const override;
	bool DummyAllowed() const override;

	const CServerInfo &ServerInfo(CSessionId SessionId) const override { return SessionSource(SessionId).m_ServerInfo; }
	void ServerInfoRequest();
	void SetCurrentServerInfo(const CServerInfo &ServerInfo);

	// ---

	int GetPredictionTime(CSessionId SessionId, int Conn) override;
	CSnapItem SnapGetItem(CSessionId SessionId, int Conn, int SnapId, int Index) const override;
	int GetPredictionTick(CSessionId SessionId, int Conn) override;
	const void *SnapFindItem(CSessionId SessionId, int Conn, int SnapId, int Type, int Id) const override;
	int SnapNumItems(CSessionId SessionId, int Conn, int SnapId) const override;
	void SnapSetStaticsize(int ItemType, int Size) override;
	void SnapSetStaticsize7(int ItemType, int Size) override;

	void Render();
	void RenderScreen();
	void RenderDebug();
	void RenderGraphs();

	void Restart() override;
	void Quit() override;
	void ResetSocket();
#if defined(CONF_PLATFORM_IOS)
	void RecreateBrokenSockets();
#endif

	const char *PlayerName() const override;
	const char *DummyName() override;
	const char *ErrorString() const override;

	const char *LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc);

	int TranslateSysMsg(int *pMsgId, bool System, CUnpacker *pUnpacker, CPacker *pPacker, const NETADDR *pPeerAddress, bool *pIsExMsg);

	bool PreprocessConnlessPacket7(CNetChunk *pPacket);
	void ProcessConnlessPacket(CNetChunk *pPacket);
	void ProcessServerInfo(int Type, NETADDR *pFrom, const void *pData, int DataSize);
	void ProcessServerPacket(CNetChunk *pPacket, int Conn, bool Dummy);
	bool TryStartModernTransport(const CConnectTarget &Target);
	void StartLegacyConnection(const NETADDR *pAddrs, int NumAddrs, bool Sixup);

	int UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo);

	void ResetMapDownload(bool ResetActive);
	void FinishMapDownload();

	EInfoState InfoState() const override { return m_InfoState; }
	void RequestDDNetInfo() override;
	void ResetDDNetInfoTask();
	void LoadDDNetInfo();

	bool IsSixup(CSessionId SessionId) const override { return SessionSource(SessionId).m_Sixup; }
	CTranslationContext &TranslationContext(CSessionId SessionId) override { return SessionSource(SessionId).m_TranslationContext; }
	const CTranslationContext &TranslationContext(CSessionId SessionId) const override { return SessionSource(SessionId).m_TranslationContext; }

	const NETADDR &ServerAddress() const override { return m_UseQuic ? m_QuicServerAddress : *NetClient(CONN_MAIN).ServerAddress(); }
	int ConnectNetTypes() const override;
	const char *ConnectAddressString() const override { return m_pNetworkSessionSource->m_ConnectAddress.c_str(); }
	const char *MapDownloadName() const override { return m_pNetworkSessionSource->m_aMapdownloadName; }
	int MapDownloadAmount() const override { return !m_pNetworkSessionSource->m_pMapdownloadTask ? m_pNetworkSessionSource->m_MapdownloadAmount : (int)m_pNetworkSessionSource->m_pMapdownloadTask->Current(); }
	int MapDownloadTotalsize() const override { return !m_pNetworkSessionSource->m_pMapdownloadTask ? m_pNetworkSessionSource->m_MapdownloadTotalsize : (int)m_pNetworkSessionSource->m_pMapdownloadTask->Size(); }

	void PumpNetwork();

	void OnDemoSnapshot(CSessionId SessionId, void *pData, int Size);
	void OnDemoMessage(CSessionId SessionId, void *pData, int Size);

	void Update();

	void RegisterInterfaces();
	void InitInterfaces();

	void Run();

	bool InitNetworkClient(char *pError, size_t ErrorSize);
	bool InitNetworkClientImpl(NETADDR BindAddr, int Conn, char *pError, size_t ErrorSize);
	bool CtrlShiftKey(int Key, bool &Last);

	static void Con_Connect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDumpSessions(IConsole::IResult *pResult, void *pUserData);
	static void Con_Disconnect(IConsole::IResult *pResult, void *pUserData);

	static void Con_DummyConnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyDisconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DummyResetInput(IConsole::IResult *pResult, void *pUserData);

	static void Con_Quit(IConsole::IResult *pResult, void *pUserData);
	static void Con_Restart(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoPlay(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoSpeed(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoSeek(IConsole::IResult *pResult, void *pUserData);
	static void Con_Minimize(IConsole::IResult *pResult, void *pUserData);
	static void Con_Ping(IConsole::IResult *pResult, void *pUserData);
	static void ConNetReset(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicReconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicKnownHost(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicForgetHost(IConsole::IResult *pResult, void *pUserData);
	static void QuicKnownHostsConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData);
	static void Con_Screenshot(IConsole::IResult *pResult, void *pUserData);

#if defined(CONF_VIDEORECORDER)
	CVideoExportSettings DefaultVideoExportSettings() override;
	const char *StartVideo(CSessionId SessionId, const char *pFilename, bool WithTimestamp, const CVideoExportSettings &Settings, bool ExactFilename);
	static void Con_StartVideo(IConsole::IResult *pResult, void *pUserData);
	static void Con_RenderDemo(IConsole::IResult *pResult, void *pUserData);
	static void Con_StopVideo(IConsole::IResult *pResult, void *pUserData);
	const char *DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue) override;
	void DemoPlayer_StartRenderQueue() override { m_VideoExportQueueRunning = true; }
	void DemoPlayer_ClearRenderQueue() override
	{
		m_VideoExportQueue.clear();
		if(!m_ActiveVideoExport.has_value())
			m_VideoExportQueueRunning = false;
	}
	size_t DemoPlayer_RenderQueueSize() const override { return m_VideoExportQueue.size() + (m_ActiveVideoExport.has_value() ? 1 : 0); }
	size_t DemoPlayer_RenderQueuePending() const override { return m_VideoExportQueue.size(); }
	const char *DemoPlayer_RenderQueueName(size_t Index) const override
	{
		dbg_assert(Index < m_VideoExportQueue.size(), "render queue index out of bounds");
		return m_VideoExportQueue[Index].m_aDemoPath;
	}
	const char *DemoPlayer_ActiveRenderName() const override
	{
		return m_ActiveVideoExport.has_value() ? m_ActiveVideoExport->m_aDemoPath : "";
	}
	void DemoPlayer_RenderQueueErase(size_t Index) override
	{
		dbg_assert(Index < m_VideoExportQueue.size(), "render queue index out of bounds");
		m_VideoExportQueue.erase(m_VideoExportQueue.begin() + Index);
		if(m_VideoExportQueue.empty() && !m_ActiveVideoExport.has_value())
			m_VideoExportQueueRunning = false;
	}
	void DemoPlayer_RenderQueueMove(size_t Index, bool Up) override
	{
		const size_t Target = Up ? Index - 1 : Index + 1;
		dbg_assert(Index < m_VideoExportQueue.size() && Target < m_VideoExportQueue.size(), "render queue index out of bounds");
		std::swap(m_VideoExportQueue[Index], m_VideoExportQueue[Target]);
	}
	void DemoPlayer_CancelActiveRender() override;
	bool DemoPlayer_RenderQueueActive() const override { return m_ActiveVideoExport.has_value(); }
	const char *DemoPlayer_RenderQueueError() const override { return m_aVideoExportQueueError; }
	bool DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const override;
	CSessionId VideoSessionId() const override { return m_VideoSessionId; }
	bool VideoUsesOfflineAudio() const override { return m_VideoOfflineAudio; }
	void ConfigureCommandLineVideoExport(const char *pDemoPath, const char *pVideoPath, const CVideoExportSettings &Settings);
	int CommandLineExitCode() const { return m_CommandLineExitCode; }
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
	static void Con_RenderTraceStart(IConsole::IResult *pResult, void *pUserData);
	static void Con_RenderTraceStop(IConsole::IResult *pResult, void *pUserData);
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
	const char *DemoPlayer_Play(CSessionId SessionId, const char *pFilename, int StorageType, bool Focus);
	void DemoRecorder_Start(const char *pFilename, bool WithTimestamp, int Recorder) override;
	void DemoRecorder_HandleAutoStart() override;
	void DemoRecorder_UpdateReplayRecorder() override;
	void DemoRecorder_AddDemoMarker(int Recorder);
	IDemoRecorder *DemoRecorder(int Recorder) override;
	CDemoRecorder (&DemoRecorders())[RECORDER_MAX];

	void TakeScreenshot(const char *pFilename);
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
	CRenderTrace *RenderTrace() override { return &m_RenderTrace; }
	void StopRenderTrace();

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
