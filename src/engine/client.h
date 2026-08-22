/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_H
#define ENGINE_CLIENT_H
#include "graphics.h"
#include "kernel.h"
#include "message.h"

#include <base/dbg.h>
#include <base/hash.h>

#include <engine/client/enums.h>
#include <engine/client/session.h>
#include <engine/friends.h>
#include <engine/shared/translation_context.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <functional>
#include <optional>
#include <vector>

#define CONNECTLINK_DOUBLE_SLASH "ddnet://"
#define CONNECTLINK_NO_SLASH "ddnet:"

class CSnapshot;
class CSnapshotBuffer;
class IMap;
struct SWarning;

enum
{
	RECORDER_MANUAL = 0,
	RECORDER_AUTO = 1,
	RECORDER_RACE = 2,
	RECORDER_REPLAYS = 3,
	RECORDER_MAX = 4,
};

typedef bool (*CLIENTFUNC_FILTER)(const void *pData, int DataSize, void *pUser);
struct CChecksumData;

class IClient : public IInterface
{
	MACRO_INTERFACE("client")
public:
	/* Constants: Client States
		STATE_OFFLINE - The client is offline.
		STATE_CONNECTING - The client is trying to connect to a server.
		STATE_LOADING - The client has connected to a server and is loading resources.
		STATE_ONLINE - The client is connected to a server and running the game.
		STATE_DEMOPLAYBACK - The client is playing a demo
		STATE_QUITTING - The client is quitting.
	*/

	enum EClientState
	{
		STATE_OFFLINE = 0,
		STATE_CONNECTING,
		STATE_LOADING,
		STATE_ONLINE,
		STATE_DEMOPLAYBACK,
		STATE_QUITTING,
		STATE_RESTARTING,
	};

	/**
	 * More precise state for @see STATE_LOADING
	 * Sets what is actually happening in the client right now
	 */
	enum ELoadingStateDetail
	{
		LOADING_STATE_DETAIL_INITIAL,
		LOADING_STATE_DETAIL_LOADING_MAP,
		LOADING_STATE_DETAIL_LOADING_DEMO,
		LOADING_STATE_DETAIL_SENDING_READY,
		LOADING_STATE_DETAIL_GETTING_READY,
	};

	enum ELoadingCallbackDetail
	{
		LOADING_CALLBACK_DETAIL_MAP,
		LOADING_CALLBACK_DETAIL_DEMO,
	};
	typedef std::function<void(ELoadingCallbackDetail Detail)> TLoadingCallback;

protected:
	// quick access to state of the client
	EClientState m_State = IClient::STATE_OFFLINE;
	ELoadingStateDetail m_LoadingStateDetail = LOADING_STATE_DETAIL_INITIAL;
	int64_t m_StateStartTime;

	float m_LocalTime = 0.0f;
	float m_GlobalTime = 0.0f;
	float m_RenderFrameTime = 0.0001f;
	float m_FrameTimeAverage = 0.0001f;

	TLoadingCallback m_LoadingCallback = nullptr;

	char m_aNews[3000] = "";
	int m_Points = -1;
	int64_t m_ReconnectTime = 0;
	// A connection is one local network endpoint. A stream is its ordered
	// snapshots, messages and ticks. Sessions, game states and views are owned
	// above this engine interface and must pass the connection explicitly.
	int m_ActiveConnection = 0;

public:
	class CSnapItem
	{
	public:
		int m_Type;
		int m_Id;
		const void *m_pData;
		int m_DataSize;
	};

	enum
	{
		CONN_MAIN = 0,
		CONN_DUMMY,
		CONN_CONTACT,
		NUM_CONNS,
	};

	int ActiveConnection() const { return m_ActiveConnection; }
	virtual void SetActiveConnection(int Conn)
	{
		dbg_assert(Conn == CONN_MAIN || Conn == CONN_DUMMY, "invalid active game connection");
		m_ActiveConnection = Conn;
	}
	virtual CSessionId FocusedSessionId() const = 0;
	virtual CSessionId NetworkSessionId() const = 0;
	virtual CSessionId DemoSessionId() const = 0;
	virtual std::vector<CSessionId> SessionIds() const = 0;
	virtual ESessionSourceType SessionType(CSessionId SessionId) const = 0;
	virtual std::vector<CStreamId> StreamIds(CSessionId SessionId) const = 0;
	virtual CStreamId PrimaryStreamId(CSessionId SessionId) const = 0;
	virtual CStreamId ActiveStreamId(CSessionId SessionId) const = 0;
	virtual CStreamId StreamId(CSessionId SessionId, int LegacyConnection) const = 0;
	virtual int StreamIndex(CSessionId SessionId, CStreamId StreamId) const = 0;
	virtual ESessionState SessionState(CSessionId SessionId) const = 0;

	enum
	{
		CONNECTIVITY_UNKNOWN,
		CONNECTIVITY_CHECKING,
		CONNECTIVITY_UNREACHABLE,
		CONNECTIVITY_REACHABLE,
		// Different global IP address has been detected for UDP and
		// TCP connections.
		CONNECTIVITY_DIFFERING_UDP_TCP_IP_ADDRESSES,
	};

	//
	EClientState State() const { return m_State; }
	virtual bool IsOnline() const = 0;
	virtual bool IsDemoPlayback() const = 0;
	ELoadingStateDetail LoadingStateDetail() const { return m_LoadingStateDetail; }
	int64_t StateStartTime() const { return m_StateStartTime; }
	void SetLoadingStateDetail(ELoadingStateDetail LoadingStateDetail) { m_LoadingStateDetail = LoadingStateDetail; }

	void SetLoadingCallback(TLoadingCallback &&Func) { m_LoadingCallback = std::move(Func); }

	// Game time.
	//
	// There are 50 ticks per second, by default we only send snapshot on
	// every second tick.

	/**
	 * Tick of the second to most recently received snapshot (usually 2
	 * less than `GameTick`).
	 */
	virtual int PrevGameTick(CSessionId SessionId, int Conn) const = 0;
	int PrevGameTick(CSessionId SessionId, CStreamId StreamId) const { return PrevGameTick(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * Tick of most recently received snapshot.
	 */
	virtual int GameTick(CSessionId SessionId, int Conn) const = 0;
	int GameTick(CSessionId SessionId, CStreamId StreamId) const { return GameTick(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * The tick we should predict to. Comes from a magic black box called
	 * "smooth time".
	 */
	virtual int PredGameTick(CSessionId SessionId, int Conn) const = 0;
	int PredGameTick(CSessionId SessionId, CStreamId StreamId) const { return PredGameTick(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * Linear interpolation parameter between `PrevGameTick` (0) and
	 * `GameTick` (1). Can be outside the interval [0, 1].
	 */
	virtual float IntraGameTick(CSessionId SessionId, int Conn) const = 0;
	float IntraGameTick(CSessionId SessionId, CStreamId StreamId) const { return IntraGameTick(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * Linear interpolation parameter between `PredGameTick - 1` (0) and
	 * `PredGameTick` (1). Can be outside the interval [0, 1].
	 */
	virtual float PredIntraGameTick(CSessionId SessionId, int Conn) const = 0;
	float PredIntraGameTick(CSessionId SessionId, CStreamId StreamId) const { return PredIntraGameTick(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * (Fractional) ticks since `PrevGameTick`.
	 */
	virtual float IntraGameTickSincePrev(CSessionId SessionId, int Conn) const = 0;
	float IntraGameTickSincePrev(CSessionId SessionId, CStreamId StreamId) const { return IntraGameTickSincePrev(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * Time in seconds since the second to most recently received snapshot.
	 */
	virtual float GameTickTime(CSessionId SessionId, int Conn) const = 0;
	float GameTickTime(CSessionId SessionId, CStreamId StreamId) const { return GameTickTime(SessionId, StreamIndex(SessionId, StreamId)); }
	/**
	 * 50
	 */
	int GameTickSpeed() const { return SERVER_TICK_SPEED; }

	// Other time.

	/**
	 * Time in seconds since a map was joined, or `GlobalTime` if that
	 * hasn't happened yet.
	 */
	float LocalTime() const { return m_LocalTime; }
	/**
	 * Time in seconds since the client was opened.
	 */
	float GlobalTime() const { return m_GlobalTime; }

	// Render statistics.

	/**
	 * Duration in seconds of the previous render cycle.
	 */
	float RenderFrameTime() const { return m_RenderFrameTime; }
	/**
	 * Exponentially weighted average of frame times.
	 */
	float FrameTimeAverage() const { return m_FrameTimeAverage; }

	// actions
	virtual void Connect(const char *pAddress, const char *pPassword = nullptr) = 0;
	virtual void Disconnect() = 0;

	// dummy
	virtual void DummyDisconnect(const char *pReason) = 0;
	virtual void DummyConnect() = 0;
	virtual bool DummyConnected() const = 0;
	virtual bool DummyConnecting() const = 0;
	virtual bool DummyConnectingDelayed() const = 0;
	virtual bool DummyAllowed() const = 0;

	virtual void Restart() = 0;
	virtual void Quit() = 0;
	virtual const char *DemoPlayer_Play(const char *pFilename, int StorageType) = 0;
#if defined(CONF_VIDEORECORDER)
	virtual const char *DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, int SpeedIndex, bool StartPaused = false) = 0;
#endif
	virtual void DemoRecorder_Start(const char *pFilename, bool WithTimestamp, int Recorder) = 0;
	virtual void DemoRecorder_HandleAutoStart() = 0;
	virtual void DemoRecorder_UpdateReplayRecorder() = 0;
	virtual class IDemoRecorder *DemoRecorder(int Recorder) = 0;
	virtual void AutoScreenshot_Start() = 0;
	virtual void AutoStatScreenshot_Start() = 0;
	virtual void AutoCSV_Start() = 0;
	virtual void ServerBrowserUpdate() = 0;

	// gfx
	virtual void Notify(const char *pTitle, const char *pMessage) = 0;
	virtual void OnWindowResize() = 0;

	virtual void UpdateAndSwap() = 0;

	// networking
	virtual void EnterGame(CSessionId SessionId, int Conn) = 0;
	void EnterGame(CSessionId SessionId, CStreamId StreamId) { EnterGame(SessionId, StreamIndex(SessionId, StreamId)); }

	//
	virtual const NETADDR &ServerAddress() const = 0;
	virtual int ConnectNetTypes() const = 0;
	virtual const char *ConnectAddressString() const = 0;
	virtual const char *MapDownloadName() const = 0;
	virtual int MapDownloadAmount() const = 0;
	virtual int MapDownloadTotalsize() const = 0;

	// input
	virtual int *GetInput(int Conn, int Tick) const = 0;
	int *GetInput(CSessionId SessionId, CStreamId StreamId, int Tick) const { return GetInput(StreamIndex(SessionId, StreamId), Tick); }

	// remote console
	virtual void RconAuth(int Conn, const char *pUsername, const char *pPassword) = 0;
	virtual bool RconAuthed() const = 0;
	virtual bool UseTempRconCommands() const = 0;
	virtual void Rcon(const char *pLine) = 0;
	virtual bool ReceivingRconCommands() const = 0;
	virtual float GotRconCommandsPercentage() const = 0;
	virtual bool ReceivingMaplist() const = 0;
	virtual float GotMaplistPercentage() const = 0;
	virtual const std::vector<std::string> &MaplistEntries() const = 0;

	// server info
	virtual const class CServerInfo &ServerInfo(CSessionId SessionId) const = 0;
	virtual bool ServerCapAnyPlayerFlag() const = 0;

	virtual int GetPredictionTime(CSessionId SessionId, int Conn) = 0;
	virtual int GetPredictionTick(CSessionId SessionId, int Conn) = 0;
	int GetPredictionTime(CSessionId SessionId, CStreamId StreamId) { return GetPredictionTime(SessionId, StreamIndex(SessionId, StreamId)); }
	int GetPredictionTick(CSessionId SessionId, CStreamId StreamId) { return GetPredictionTick(SessionId, StreamIndex(SessionId, StreamId)); }

	// snapshot interface

	enum
	{
		SNAP_CURRENT = 0,
		SNAP_PREV = 1,
		NUM_SNAPSHOT_TYPES = 2,
	};

	// TODO: Refactor: should redo this a bit i think, too many virtual calls
	virtual int SnapNumItems(CSessionId SessionId, int Conn, int SnapId) const = 0;
	virtual const void *SnapFindItem(CSessionId SessionId, int Conn, int SnapId, int Type, int Id) const = 0;
	virtual CSnapItem SnapGetItem(CSessionId SessionId, int Conn, int SnapId, int Index) const = 0;
	int SnapNumItems(CSessionId SessionId, CStreamId StreamId, int SnapId) const { return SnapNumItems(SessionId, StreamIndex(SessionId, StreamId), SnapId); }
	const void *SnapFindItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Type, int Id) const { return SnapFindItem(SessionId, StreamIndex(SessionId, StreamId), SnapId, Type, Id); }
	CSnapItem SnapGetItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Index) const { return SnapGetItem(SessionId, StreamIndex(SessionId, StreamId), SnapId, Index); }

	virtual void SnapSetStaticsize(int ItemType, int Size) = 0;
	virtual void SnapSetStaticsize7(int ItemType, int Size) = 0;

	virtual int SendMsg(int Conn, CMsgPacker *pMsg, int Flags) = 0;
	template<class T>
	int SendPackMsg(int Conn, T *pMsg, int Flags, bool NoTranslate = false)
	{
		CMsgPacker Packer(T::ms_MsgId, false, NoTranslate);
		if(pMsg->Pack(&Packer))
			return -1;
		return SendMsg(Conn, &Packer, Flags);
	}

	//
	virtual const char *PlayerName() const = 0;
	virtual const char *DummyName() = 0;
	virtual const char *ErrorString() const = 0;
	virtual const char *LatestVersion() const = 0;
	virtual bool ConnectionProblems(CSessionId SessionId, int Conn) const = 0;
	bool ConnectionProblems(CSessionId SessionId, CStreamId StreamId) const { return ConnectionProblems(SessionId, StreamIndex(SessionId, StreamId)); }

	virtual IGraphics::CTextureHandle GetDebugFont() const = 0; // TODO: remove this function

	// DDRace

	const char *News() const { return m_aNews; }
	int Points() const { return m_Points; }
	int64_t ReconnectTime() const { return m_ReconnectTime; }
	void SetReconnectTime(int64_t ReconnectTime) { m_ReconnectTime = ReconnectTime; }

	virtual bool IsSixup(CSessionId SessionId) const = 0;
	virtual CTranslationContext &TranslationContext(CSessionId SessionId) = 0;
	virtual const CTranslationContext &TranslationContext(CSessionId SessionId) const = 0;

	virtual void RaceRecord_Start(const char *pFilename) = 0;
	virtual void RaceRecord_Stop() = 0;
	virtual bool RaceRecord_IsRecording() = 0;

	virtual void DemoSliceBegin() = 0;
	virtual void DemoSliceEnd() = 0;
	virtual void DemoSlice(const char *pDstPath, CLIENTFUNC_FILTER pfnFilter, void *pUser) = 0;

	enum class EInfoState
	{
		LOADING,
		SUCCESS,
		ERROR,
	};
	virtual EInfoState InfoState() const = 0;
	virtual void RequestDDNetInfo() = 0;
	virtual bool EditorHasUnsavedData() const = 0;

	virtual void GenerateTimeoutSeed() = 0;

	virtual IFriends *Foes() = 0;

	virtual void GetSmoothTick(CSessionId SessionId, int Conn, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) = 0;
	void GetSmoothTick(CSessionId SessionId, CStreamId StreamId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) { GetSmoothTick(SessionId, StreamIndex(SessionId, StreamId), Now, pSmoothTick, pSmoothIntraTick, MixAmount); }

	virtual void AddWarning(const SWarning &Warning) = 0;
	virtual std::optional<SWarning> CurrentWarning() = 0;

	virtual CChecksumData *ChecksumData() = 0;
	virtual int UdpConnectivity(int NetType) = 0;

	/**
	 * Opens a link in the browser.
	 *
	 * @param pLink The link to open in a browser.
	 *
	 * @return `true` on success, `false` on failure.
	 *
	 * @remark This may not be called with untrusted input or it'll result in arbitrary code execution, especially on Windows.
	 */
	virtual bool ViewLink(const char *pLink) = 0;
	/**
	 * Opens a file or directory with the default program.
	 *
	 * @param pFilename The file or folder to open with the default program.
	 *
	 * @return `true` on success, `false` on failure.
	 *
	 * @remark This may not be called with untrusted input or it'll result in arbitrary code execution, especially on Windows.
	 */
	virtual bool ViewFile(const char *pFilename) = 0;

#if defined(CONF_FAMILY_WINDOWS)
	virtual void ShellRegister() = 0;
	virtual void ShellUnregister() = 0;
#endif

	virtual std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) = 0;
	virtual void GetGpuInfoString(char (&aGpuInfo)[512]) = 0;
};

class IGameClient : public IInterface
{
	MACRO_INTERFACE("gameclient")
protected:
public:
	virtual void OnConsoleInit() = 0;

	virtual void OnRconType(bool UsernameReq) = 0;
	virtual void OnRconLine(const char *pLine) = 0;
	virtual void OnInit() = 0;
	virtual void InvalidateSnapshot(CSessionId SessionId) = 0;
	virtual void OnNewSnapshot(CSessionId SessionId, CStreamId StreamId) = 0;
	virtual void OnEnterGame(CSessionId SessionId) = 0;
	virtual void OnShutdown() = 0;
	virtual void OnRenderPrepare() = 0;
	virtual void OnRender() = 0;
	virtual void OnRenderFinalize() = 0;
	virtual void OnUpdate() = 0;
	virtual void OnStateChange(int NewState, int OldState) = 0;
	virtual void OnConnected(CSessionId SessionId) = 0;
	virtual void OnSessionClosed(CSessionId SessionId) = 0;
	virtual void OnSessionFocused(CSessionId SessionId) = 0;
	virtual void OnMessage(CSessionId SessionId, int MsgId, CUnpacker *pUnpacker, CStreamId StreamId) = 0;
	virtual void OnPredict(CSessionId SessionId, CStreamId StreamId) = 0;
	virtual void OnActivateEditor() = 0;
	virtual void OnWindowResize() = 0;

	virtual int OnSnapInput(CSessionId SessionId, int *pData, CStreamId StreamId, bool Force) = 0;
	virtual void OnConnectionFocusChanged(CSessionId SessionId, CStreamId PreviousStreamId, CStreamId StreamId) = 0;
	virtual void SendDummyInfo(bool Start) = 0;

	virtual const char *GetItemName(int Type) const = 0;
	virtual const char *Version() const = 0;
	virtual const char *NetVersion() const = 0;
	virtual const char *NetVersion7() const = 0;
	virtual int DDNetVersion() const = 0;
	virtual const char *DDNetVersionStr() const = 0;

	virtual void OnDummyDisconnect() = 0;
	virtual void DummyResetInput() = 0;
	virtual void Echo(const char *pString) = 0;

	virtual bool CanDisplayWarning() const = 0;
	virtual void RenderShutdownMessage() = 0;

	virtual IMap *Map() = 0;
	virtual const IMap *Map() const = 0;
	virtual IMap *Map(CSessionId SessionId) = 0;
	virtual const IMap *Map(CSessionId SessionId) const = 0;
	virtual CNetObjHandler *GetNetObjHandler() = 0;
	virtual protocol7::CNetObjHandler *GetNetObjHandler7() = 0;

	virtual int ClientVersion7() const = 0;

	virtual void ApplySkin7InfoFromSnapObj(CSessionId SessionId, const protocol7::CNetObj_De_ClientInfo *pObj, int ClientId, CStreamId StreamId) = 0;
	virtual int OnDemoRecSnap7(CSessionId SessionId, CSnapshot *pFrom, CSnapshotBuffer *pTo, CStreamId StreamId) = 0;
	virtual int TranslateSnap(CSessionId SessionId, CSnapshotBuffer *pSnapDstSix, CSnapshot *pSnapSrcSeven, CStreamId StreamId) = 0;
	virtual void ProcessDemoSnapshot(CSnapshot *pSnap) = 0;

	virtual void InitializeLanguage() = 0;

	virtual void ForceUpdateConsoleRemoteCompletionSuggestions() = 0;
};

extern IGameClient *CreateGameClient();
#endif
