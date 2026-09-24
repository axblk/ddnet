/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_H
#define ENGINE_CLIENT_H
#include "graphics.h"
#include "kernel.h"
#include "message.h"

#include <base/hash.h>

#include <engine/client/enums.h>
#include <engine/client/session.h>
#include <engine/friends.h>
#include <engine/shared/translation_context.h>
#if defined(CONF_VIDEORECORDER)
#include <engine/shared/video.h>
#endif

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <functional>
#include <optional>
#include <vector>

#define CONNECTLINK_DOUBLE_SLASH "ddnet://"
#define CONNECTLINK_NO_SLASH "ddnet:"
#define QUIC_CONNECTLINK_DOUBLE_SLASH "ddnet+quic://"
#define QUIC_CONNECTLINK7_DOUBLE_SLASH "tw-0.7+quic://"
#define WT_CONNECTLINK_DOUBLE_SLASH "ddnet+wt://"
#define WT_CONNECTLINK7_DOUBLE_SLASH "tw-0.7+wt://"

class CSnapshot;
class CSnapshotBuffer;
class CRenderTrace;
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
	ELoadingStateDetail m_LoadingStateDetail = LOADING_STATE_DETAIL_INITIAL;
	int64_t m_StateStartTime;

	float m_LocalTime = 0.0f;
	float m_GlobalTime = 0.0f;
	float m_RenderFrameTime = 0.0001f;
	float m_FrameTimeAverage = 0.0001f;

	TLoadingCallback m_LoadingCallback = nullptr;

public:
	enum
	{
		CONN_MAIN = 0,
		CONN_DUMMY,
		CONN_CONTACT,
		NUM_CONNS,
	};

	//
	/**
	 * The state of the session in focus, or that the client is quitting or
	 * restarting.
	 */
	virtual EClientState State() const = 0;
	virtual bool IsOnline() const = 0;
	virtual bool IsDemoPlayback() const = 0;
	ELoadingStateDetail LoadingStateDetail() const { return m_LoadingStateDetail; }
	int64_t StateStartTime() const { return m_StateStartTime; }
	void SetLoadingStateDetail(ELoadingStateDetail LoadingStateDetail) { m_LoadingStateDetail = LoadingStateDetail; }

	void SetLoadingCallback(TLoadingCallback &&Func) { m_LoadingCallback = std::move(Func); }

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
	virtual CRenderTrace *RenderTrace() = 0;

#if defined(CONF_VIDEORECORDER)
	virtual CSessionId VideoSessionId() const = 0;
	virtual bool VideoUsesOfflineAudio() const = 0;
#endif
	// gfx
	virtual void Notify(const char *pTitle, const char *pMessage) = 0;
	virtual void OnWindowResize() = 0;

	virtual void UpdateAndSwap() = 0;

	virtual void AddWarning(const SWarning &Warning) = 0;
	virtual std::optional<SWarning> CurrentWarning() = 0;

	virtual IFriends *Foes() = 0;
};

/**
 * What only a client that connects to servers does: the connection and the
 * dummy, the remote console, recording what is played and the server browser.
 */
class IClientNetwork : public IInterface
{
	MACRO_INTERFACE("clientnetwork")
public:
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

	// actions
	virtual void Connect(const char *pAddress, const char *pPassword = nullptr) = 0;
	virtual void Disconnect() = 0;

	/**
	 * The session the server is played in.
	 */
	virtual CSessionId NetworkSessionId() const = 0;
	/**
	 * The session the dummy plays in, on the server of the network session.
	 */
	virtual CSessionId DummySessionId() const = 0;
	virtual const char *PlayerName() const = 0;
	virtual const char *DummyName() = 0;

	// dummy
	virtual void DummyDisconnect(const char *pReason) = 0;
	virtual void DummyConnect() = 0;
	virtual bool DummyConnected() const = 0;
	virtual bool DummyConnecting() const = 0;
	virtual bool DummyConnectingDelayed() const = 0;
	virtual bool DummyAllowed() const = 0;

	virtual void DemoRecorder_Start(const char *pFilename, bool WithTimestamp, int Recorder) = 0;
	virtual void DemoRecorder_HandleAutoStart() = 0;
	virtual void DemoRecorder_UpdateReplayRecorder() = 0;
	virtual class IDemoRecorder *DemoRecorder(int Recorder) = 0;
	virtual void AutoScreenshot_Start() = 0;
	virtual void AutoStatScreenshot_Start() = 0;
	virtual void AutoCSV_Start() = 0;
	virtual void ServerBrowserUpdate() = 0;

	// networking
	virtual void EnterGame(int Conn) = 0;

	//
	virtual const NETADDR &ServerAddress() const = 0;
	virtual int ConnectNetTypes() const = 0;
	virtual const char *ConnectAddressString() const = 0;
	virtual const char *MapDownloadName() const = 0;
	virtual int MapDownloadAmount() const = 0;
	virtual int MapDownloadTotalsize() const = 0;

	// remote console
	virtual void RconAuth(const char *pUsername, const char *pPassword, bool Dummy) = 0;
	virtual bool RconAuthed() const = 0;
	virtual bool UseTempRconCommands() const = 0;
	virtual void Rcon(const char *pLine) = 0;
	virtual bool ReceivingRconCommands() const = 0;
	virtual float GotRconCommandsPercentage() const = 0;
	virtual bool ReceivingMaplist() const = 0;
	virtual float GotMaplistPercentage() const = 0;
	virtual const std::vector<std::string> &MaplistEntries() const = 0;

	// server info
	virtual bool ServerCapAnyPlayerFlag(CSessionId SessionId) const = 0;

	virtual int SendMsg(int Conn, CMsgPacker *pMsg, int Flags) = 0;
	virtual int SendMsgActive(CMsgPacker *pMsg, int Flags) = 0;

	template<class T>
	int SendPackMsgActive(T *pMsg, int Flags, bool NoTranslate = false)
	{
		CMsgPacker Packer(T::ms_MsgId, false, NoTranslate);
		if(pMsg->Pack(&Packer))
			return -1;
		return SendMsgActive(&Packer, Flags);
	}

	template<class T>
	int SendPackMsg(int Conn, T *pMsg, int Flags, bool NoTranslate = false)
	{
		CMsgPacker Packer(T::ms_MsgId, false, NoTranslate);
		if(pMsg->Pack(&Packer))
			return -1;
		return SendMsg(Conn, &Packer, Flags);
	}

	virtual const char *LatestVersion() const = 0;
	virtual bool ConnectionProblems(CSessionId SessionId) const = 0;

	// DDRace

	virtual int64_t ReconnectTime() const = 0;
	virtual void CancelReconnect() = 0;

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

	virtual CChecksumData *ChecksumData() = 0;
	virtual int UdpConnectivity(int NetType) = 0;

#if defined(CONF_FAMILY_WINDOWS)
	virtual void ShellRegister() = 0;
	virtual void ShellUnregister() = 0;
#endif
};

/**
 * What only the front end asks of the client: quitting and restarting it,
 * playing and exporting demos from the demo browser, why the last connection
 * ended, news and points from the info server, and opening links and files.
 * Only the full client registers it; a program without a front end has none.
 */
class IClientFrontend : public IInterface
{
	MACRO_INTERFACE("clientfrontend")
public:
	virtual void Restart() = 0;
	virtual void Quit() = 0;
	virtual const char *DemoPlayer_Play(const char *pFilename, int StorageType) = 0;
#if defined(CONF_VIDEORECORDER)
	/**
	 * The export settings as the configuration has them, for every caller that
	 * does not build its own. There is only one set of them, so a demo rendered
	 * from the console comes out like one rendered from the dialog.
	 */
	virtual CVideoExportSettings DefaultVideoExportSettings() = 0;
	virtual const char *DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue) = 0;
	virtual void DemoPlayer_StartRenderQueue() = 0;
	virtual void DemoPlayer_ClearRenderQueue() = 0;
	virtual size_t DemoPlayer_RenderQueueSize() const = 0;
	/**
	 * Number of queued exports that have not been started yet.
	 */
	virtual size_t DemoPlayer_RenderQueuePending() const = 0;
	/**
	 * Demo of the pending export at @p Index, which must be less than
	 * `DemoPlayer_RenderQueuePending()`. That is what the queue was filled
	 * with and what a name in it should say; the video name is derived from
	 * it and says the same thing twice.
	 */
	virtual const char *DemoPlayer_RenderQueueName(size_t Index) const = 0;
	/**
	 * Demo of the export that is running, or an empty string when none is.
	 */
	virtual const char *DemoPlayer_ActiveRenderName() const = 0;
	/**
	 * Removes the pending export at @p Index, which must be less than
	 * `DemoPlayer_RenderQueuePending()`. The active export is not affected.
	 */
	virtual void DemoPlayer_RenderQueueErase(size_t Index) = 0;
	/**
	 * Moves the pending export at @p Index one position towards the front or
	 * back of the queue. Both @p Index and the resulting position must be less
	 * than `DemoPlayer_RenderQueuePending()`.
	 */
	virtual void DemoPlayer_RenderQueueMove(size_t Index, bool Up) = 0;
	/**
	 * Aborts the export that is currently running. Pending exports are kept and
	 * the next one is started afterwards.
	 */
	virtual void DemoPlayer_CancelActiveRender() = 0;
	virtual bool DemoPlayer_RenderQueueActive() const = 0;
	virtual const char *DemoPlayer_RenderQueueError() const = 0;
	virtual bool DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const = 0;
#endif

	virtual const char *ErrorString() const = 0;

	virtual IGraphics::CTextureHandle GetDebugFont() = 0; // TODO: remove this function

	// DDRace

	virtual const char *News() const = 0;
	virtual int Points() const = 0;

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
	 *
	 * @remark On iOS the file or directory is shown in the Files app.
	 */
	virtual bool ViewFile(const char *pFilename) = 0;
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
	virtual void OnNewSnapshot(CSessionId SessionId) = 0;
	virtual void OnEnterGame(CSessionId SessionId) = 0;
	virtual void OnShutdown() = 0;
	virtual void OnRenderPrepare() = 0;
#if defined(CONF_VIDEORECORDER)
	virtual void OnRenderVideoPrepare(CSessionId SessionId, const CVideoExportSettings &Settings) = 0;
#endif
	virtual void OnRender() = 0;
	virtual void OnRenderFinalize() = 0;
#if defined(CONF_VIDEORECORDER)
	virtual bool OnRenderVideoProgress(bool Overlay) = 0;
#endif
	virtual void OnUpdate() = 0;
	virtual void OnStateChange(int NewState, int OldState) = 0;
	virtual void OnConnected(CSessionId SessionId) = 0;
	virtual void OnSessionClosed(CSessionId SessionId) = 0;
	virtual void OnSessionFocused(CSessionId SessionId) = 0;
	virtual void OnMessage(CSessionId SessionId, int MsgId, CUnpacker *pUnpacker) = 0;
	virtual void OnPredict(CSessionId SessionId) = 0;
	virtual void OnActivateEditor() = 0;
	virtual void OnWindowResize() = 0;
	virtual bool IsSoundReady() = 0;

	virtual int OnSnapInput(CSessionId SessionId, int *pData, bool Force) = 0;
	virtual void OnDummySwap() = 0;
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

	virtual void ApplySkin7InfoFromSnapObj(CSessionId SessionId, const protocol7::CNetObj_De_ClientInfo *pObj, int ClientId) = 0;
	virtual int OnDemoRecSnap7(CSessionId SessionId, CSnapshot *pFrom, CSnapshotBuffer *pTo) = 0;
	virtual int TranslateSnap(CSessionId SessionId, CSnapshotBuffer *pSnapDstSix, CSnapshot *pSnapSrcSeven) = 0;
	virtual void ProcessDemoSnapshot(CSnapshot *pSnap) = 0;

	virtual void InitializeLanguage() = 0;

	virtual void ForceUpdateConsoleRemoteCompletionSuggestions() = 0;
};

extern IGameClient *CreateGameClient();
#endif
