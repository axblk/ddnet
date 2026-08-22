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

class CClient : public IClient, public CDemoPlayer::IListener
{
	struct CQuicKnownHost
	{
		char m_aHost[128];
		int m_Port;
		SHA256_DIGEST m_IdentityFingerprint;
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
	NETADDR m_NetworkBindAddr = NETADDR_ZEROED;
	bool m_NetworkInitialized = false;
	std::array<std::vector<std::pair<int, int>>, 2> m_avSnapshotStaticSizes;
	CQuicTransport m_QuicTransport;
	CQuicSessionId m_QuicSession;
	NETADDR m_QuicServerAddress = {};
	bool m_UseQuic = false;
	bool m_UseWebTransport = false;
	bool m_QuicConnected = false;
	// When the last message arrived over QUIC, for the connection warning
	int64_t m_QuicLastRecvTime = 0;
	std::vector<CQuicKnownHost> m_vQuicKnownHosts;
	char m_aQuicTrustHost[128] = {};
	int m_QuicTrustPort = 0;
	SHA256_DIGEST m_QuicExpectedIdentity = {};
	bool m_QuicIdentityRequired = false;
	bool m_QuicIdentityKnown = false;
	bool m_QuicRememberIdentity = false;
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

	// graphs
	CGraph m_FpsGraph;

	CSnapshotDelta *SnapshotDelta();
	CSessionSourceBase &SessionSource(CSessionId SessionId)
	{
		CGameSession *pSession = m_SessionManager.Find(SessionId);
		dbg_assert(pSession != nullptr, "invalid game session");
		return static_cast<CSessionSourceBase &>(pSession->Source());
	}
	const CSessionSourceBase &SessionSource(CSessionId SessionId) const
	{
		const CGameSession *pSession = m_SessionManager.Find(SessionId);
		dbg_assert(pSession != nullptr, "invalid game session");
		return static_cast<const CSessionSourceBase &>(pSession->Source());
	}
	CNetworkSessionSource &NetworkSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::NETWORK, "game session is not a Network source");
		return static_cast<CNetworkSessionSource &>(Source);
	}
	const CNetworkSessionSource &NetworkSource(CSessionId SessionId) const
	{
		const CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::NETWORK, "game session is not a Network source");
		return static_cast<const CNetworkSessionSource &>(Source);
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
		return NetworkSource(SessionId).ConnectionAt(Conn);
	}
	const CConnection &Connection(CSessionId SessionId, int Conn) const
	{
		if(SessionId == m_DemoSessionId)
		{
			dbg_assert(Conn == CONN_MAIN, "invalid demo stream");
			return m_pDemoSessionSource->Connection();
		}
		return NetworkSource(SessionId).ConnectionAt(Conn);
	}
	CConnection &Connection(CSessionId SessionId, CStreamId StreamId)
	{
		if(SessionId == m_DemoSessionId)
		{
			dbg_assert(StreamId == SessionSource(SessionId).PrimaryStreamId(), "invalid demo stream");
			return m_pDemoSessionSource->Connection();
		}
		CConnection *pConnection = NetworkSource(SessionId).Connection(StreamId);
		dbg_assert(pConnection != nullptr, "invalid Network stream");
		return *pConnection;
	}
	const CConnection &Connection(CSessionId SessionId, CStreamId StreamId) const
	{
		if(SessionId == m_DemoSessionId)
		{
			dbg_assert(StreamId == SessionSource(SessionId).PrimaryStreamId(), "invalid demo stream");
			return m_pDemoSessionSource->Connection();
		}
		const CConnection *pConnection = NetworkSource(SessionId).Connection(StreamId);
		dbg_assert(pConnection != nullptr, "invalid Network stream");
		return *pConnection;
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
	// Storage for UpdateNetworkSession, kept so that advancing the streams of a
	// session does not allocate on every frame. One buffer serves every session,
	// which only holds while UpdateNetworkSession is not nested: the session
	// manager updates one session at a time and nothing it calls updates another.
	// Nesting it needs a buffer per session instead.
	std::vector<CStreamId> m_vRepredict;
	void UpdateNetworkSession(CSessionId SessionId);
	void StopDemoSession(const char *pReason);
	void StopNetworkSession(CSessionId SessionId, const char *pReason);
	int MaxLatencyTicks(CSessionId SessionId) const;
	int PredictionMargin(CSessionId SessionId) const;

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
	using IClient::EnterGame;
	using IClient::GameTick;
	using IClient::GameTickTime;
	using IClient::GetInput;
	using IClient::GetPredictionTick;
	using IClient::GetPredictionTime;
	using IClient::GetSmoothTick;
	using IClient::IntraGameTick;
	using IClient::IntraGameTickSincePrev;
	using IClient::PredGameTick;
	using IClient::PredIntraGameTick;
	using IClient::PrevGameTick;
	using IClient::SendMsg;
	using IClient::ServerInfo;
	using IClient::SnapFindItem;
	using IClient::SnapGetItem;
	using IClient::SnapNumItems;

	CSessionId FocusedSessionId() const override { return m_SessionManager.FocusedId(); }
	void SetActiveConnection(int Conn) override
	{
		IClient::SetActiveConnection(Conn);
		if(m_pNetworkSessionSource != nullptr)
			m_pNetworkSessionSource->SetActiveStream(m_pNetworkSessionSource->StreamIdAt(Conn));
	}
	CSessionId NetworkSessionId() const override { return m_NetworkSessionId; }
	CSessionId DemoSessionId() const override { return m_DemoSessionId; }
	std::vector<CSessionId> SessionIds() const override { return m_SessionManager.SessionIds(); }
	ESessionSourceType SessionType(CSessionId SessionId) const override { return SessionSource(SessionId).Type(); }
	std::vector<CStreamId> StreamIds(CSessionId SessionId) const override { return SessionSource(SessionId).StreamIds(); }
	CStreamId PrimaryStreamId(CSessionId SessionId) const override { return SessionSource(SessionId).PrimaryStreamId(); }
	CStreamId ActiveStreamId(CSessionId SessionId) const override { return SessionSource(SessionId).ActiveStreamId(); }
	CStreamId StreamId(CSessionId SessionId, int LegacyConnection) const override
	{
		if(SessionId == m_DemoSessionId)
			return LegacyConnection == CONN_MAIN ? SessionSource(SessionId).PrimaryStreamId() : CStreamId{};
		return NetworkSource(SessionId).StreamIdAt(LegacyConnection);
	}
	int StreamIndex(CSessionId SessionId, CStreamId StreamId) const override
	{
		if(SessionId == m_DemoSessionId)
			return StreamId == SessionSource(SessionId).PrimaryStreamId() ? CONN_MAIN : -1;
		return NetworkSource(SessionId).StreamIndex(StreamId);
	}
	ESessionState SessionState(CSessionId SessionId) const override { return SessionSource(SessionId).State(); }
	int PrevGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_PrevGameTick; }
	int GameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_CurGameTick; }
	int PredGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_PredTick; }
	float IntraGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_GameIntraTick; }
	float PredIntraGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_PredIntraTick; }
	float IntraGameTickSincePrev(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_GameIntraTickSincePrev; }
	float GameTickTime(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_GameTickTime; }

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
	void SendInfo(CSessionId SessionId, CStreamId StreamId);
	void SendEnterGame(int Conn);
	void SendEnterGame(CSessionId SessionId, CStreamId StreamId);
	void SendReady(int Conn);
	void SendReady(CSessionId SessionId, CStreamId StreamId);
	void SendMapRequest(CSessionId SessionId);

	bool RconAuthed() const override { return Connection(ActiveConnection()).m_RconAuthed != 0; }
	bool UseTempRconCommands() const override { return m_pNetworkSessionSource->m_UseTempRconCommands != 0; }
	void RconAuth(int Conn, const char *pName, const char *pPassword) override;
	void Rcon(const char *pCmd) override;
	bool ReceivingRconCommands() const override { return m_pNetworkSessionSource->m_ExpectedRconCommands > 0; }
	float GotRconCommandsPercentage() const override;
	bool ReceivingMaplist() const override { return m_pNetworkSessionSource->m_ExpectedMaplistEntries > 0; }
	float GotMaplistPercentage() const override;
	const std::vector<std::string> &MaplistEntries() const override { return m_pNetworkSessionSource->m_vMaplistEntries; }

	bool ConnectionProblems(CSessionId SessionId, CStreamId StreamId) const override;

	IGraphics::CTextureHandle GetDebugFont() const override { return m_DebugFont; }

	void SendInput(CSessionId SessionId);

	// TODO: OPT: do this a lot smarter!
	int *GetInput(CSessionId SessionId, CStreamId StreamId, int Tick) const override;

	const char *LatestVersion() const override;
	int64_t ReconnectTime() const override { return m_pNetworkSessionSource->ReconnectTime(); }
	void CancelReconnect() override { m_pNetworkSessionSource->CancelReconnect(); }

	// ------ state handling -----
	void SetState(EClientState State);
	void SetFocusedState(EClientState State, bool ResetSession);
	void FocusSession(CSessionId SessionId);
	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	// called when the map is loaded and we should init for a new round
	void OnEnterGame(int Conn);
	void EnterGame(CSessionId SessionId, CStreamId StreamId) override;

	// called once after being ingame for 1 second
	void OnPostConnect(int Conn);

	void Connect(const char *pAddress, const char *pPassword = nullptr) override;
	CSessionId CreateNetworkSession();
	CStreamId ConnectAdditionalStream(CSessionId SessionId);
	bool DestroyNetworkStream(CSessionId SessionId, CStreamId StreamId);
	bool DestroyNetworkSession(CSessionId SessionId);
	void ConnectSession(CSessionId SessionId, const char *pAddress, const char *pPassword);
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
	void RequestServerInfo(CSessionId SessionId);
	void SetSessionServerInfo(CSessionId SessionId, const CServerInfo &ServerInfo);

	void LoadDebugFont();

	// ---

	int GetPredictionTime(CSessionId SessionId, CStreamId StreamId) override;
	CSnapItem SnapGetItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Index) const override;
	int GetPredictionTick(CSessionId SessionId, CStreamId StreamId) override;
	const void *SnapFindItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Type, int Id) const override;
	int SnapNumItems(CSessionId SessionId, CStreamId StreamId, int SnapId) const override;
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

	int TranslateSysMsg(CSessionId SessionId, int *pMsgId, bool System, CUnpacker *pUnpacker, CPacker *pPacker, const NETADDR *pPeerAddress, bool *pIsExMsg);

	bool PreprocessConnlessPacket7(CNetChunk *pPacket);
	void ProcessConnlessPacket(CNetChunk *pPacket);
	void ProcessServerInfo(int Type, NETADDR *pFrom, const void *pData, int DataSize);
	void ProcessServerPacket(CSessionId SessionId, CStreamId StreamId, CNetChunk *pPacket);
	void ClearQuicTrust();
	const CQuicKnownHost *FindQuicKnownHost(const char *pHost, int Port) const;
	bool AddQuicKnownHost(const char *pHost, int Port, SHA256_DIGEST IdentityFingerprint);
	void StartLegacyConnection(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs, bool Sixup);
	const NETADDR &SessionServerAddress(CSessionId SessionId) const;

	int UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo);

	void ResetMapDownload(CSessionId SessionId, bool ResetActive);
	void FinishMapDownload(CSessionId SessionId);

	EInfoState InfoState() const override { return m_InfoState; }
	void RequestDDNetInfo() override;
	void ResetDDNetInfoTask();
	void LoadDDNetInfo();

	bool IsSixup(CSessionId SessionId) const override { return SessionSource(SessionId).IsSixup(); }
	CTranslationContext &TranslationContext(CSessionId SessionId) override { return SessionSource(SessionId).TranslationContext(); }
	const CTranslationContext &TranslationContext(CSessionId SessionId) const override { return SessionSource(SessionId).TranslationContext(); }

	const NETADDR &ServerAddress() const override { return SessionServerAddress(m_NetworkSessionId); }
	int ConnectNetTypes() const override;
	const char *ConnectAddressString() const override { return m_pNetworkSessionSource->m_ConnectAddress.c_str(); }
	const char *MapDownloadName() const override { return m_pNetworkSessionSource->m_aMapdownloadName; }
	int MapDownloadAmount() const override { return !m_pNetworkSessionSource->m_pMapdownloadTask ? m_pNetworkSessionSource->m_MapdownloadAmount : (int)m_pNetworkSessionSource->m_pMapdownloadTask->Current(); }
	int MapDownloadTotalsize() const override { return !m_pNetworkSessionSource->m_pMapdownloadTask ? m_pNetworkSessionSource->m_MapdownloadTotalsize : (int)m_pNetworkSessionSource->m_pMapdownloadTask->Size(); }

	void PumpNetwork(CSessionId SessionId);
	int SendMsg(CSessionId SessionId, CStreamId StreamId, CMsgPacker *pMsg, int Flags) override;

	void OnDemoPlayerSnapshot(void *pData, int Size) override;
	void OnDemoPlayerMessage(void *pData, int Size) override;

	void Update();

	void RegisterInterfaces();
	void InitInterfaces();

	void Run();

	bool InitNetworkClient(char *pError, size_t ErrorSize);
	bool InitNetworkClientImpl(NETADDR BindAddr, int Conn, char *pError, size_t ErrorSize);
	bool InitNetworkStream(NETADDR BindAddr, CNetClient &NetClient, int &Port, const char *pName, char *pError, size_t ErrorSize);
	bool CtrlShiftKey(int Key, bool &Last);

	static void Con_Connect(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgConnectSession(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgConnectStream(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDestroyStream(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDestroySession(IConsole::IResult *pResult, void *pUserData);
	static void Con_DbgDumpSessions(IConsole::IResult *pResult, void *pUserData);
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
	static void Con_QuicReconnect(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicKnownHost(IConsole::IResult *pResult, void *pUserData);
	static void Con_QuicForgetHost(IConsole::IResult *pResult, void *pUserData);
	static void QuicKnownHostsConfigSaveCallback(IConfigManager *pConfigManager, void *pUserData);
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
	virtual int HandleChecksum(CSessionId SessionId, CStreamId StreamId, CUuid Uuid, CUnpacker *pUnpacker);

	// gfx
	void Notify(const char *pTitle, const char *pMessage) override;
	void OnWindowResize() override;
	void BenchmarkQuit(int Seconds, const char *pFilename);

	void UpdateAndSwap() override;

	// DDRace

	void GenerateTimeoutSeed() override;
	void GenerateTimeoutCodes(CSessionId SessionId, const NETADDR *pAddrs, int NumAddrs);

	void RaceRecord_Start(const char *pFilename) override;
	void RaceRecord_Stop() override;
	bool RaceRecord_IsRecording() override;

	void DemoSliceBegin() override;
	void DemoSliceEnd() override;
	void DemoSlice(const char *pDstPath, CLIENTFUNC_FILTER pfnFilter, void *pUser) override;
	virtual void SaveReplay(int Length, const char *pFilename = "");

	bool EditorHasUnsavedData() const override { return m_pEditor->HasUnsavedData(); }

	IFriends *Foes() override { return &m_Foes; }

	void GetSmoothTick(CSessionId SessionId, CStreamId StreamId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) override;

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
