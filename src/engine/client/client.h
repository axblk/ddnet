/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_H
#define ENGINE_CLIENT_CLIENT_H

#include "client_net.h"
#include "graph.h"
#include "render_trace.h"
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

class CClient : public CClientWithConnection
{
	// needed interfaces
	IDiscord *m_pDiscord = nullptr;
	IEditor *m_pEditor = nullptr;
	IFavorites *m_pFavorites = nullptr;
	IEngineGraphics *m_pGraphics = nullptr;
	IEngineInput *m_pInput = nullptr;
	IEngineSound *m_pSound = nullptr;
	ISteam *m_pSteam = nullptr;
	INotifications *m_pNotifications = nullptr;
	IEngineTextRender *m_pTextRender = nullptr;
	IUpdater *m_pUpdater = nullptr;

#if defined(CONF_VIDEORECORDER)
	CSessionId m_VideoExportSessionId;
	CDemoSessionSource *m_pVideoExportSessionSource = nullptr;
#endif
	CDemoRecorder m_aDemoRecorders[RECORDER_MAX];
	CDemoRecorder m_aDemoRecordersSixup[RECORDER_MAX];
	CDemoEditor m_DemoEditor;
	CGhostRecorder m_GhostRecorder;
	CGhostLoader m_GhostLoader;
	CServerBrowser m_ServerBrowser;
	CUpdater m_Updater;
	CFriends m_Friends;
	CFriends m_Foes;

	IGraphics::CTextureHandle m_DebugFont;

	int64_t m_LastRenderTime;

	bool m_AutoScreenshotRecycle = false;
	bool m_AutoStatScreenshotRecycle = false;
	bool m_AutoCSVRecycle = false;
	bool m_EditorActive = false;

	// version-checking
	char m_aVersionStr[10] = "0";

	char m_aCmdConnect[256] = "";
	char m_aCmdPlayDemo[IO_MAX_PATH_LENGTH] = "";
	char m_aCmdEditMap[IO_MAX_PATH_LENGTH] = "";

	// map download
	char m_aMapDownloadUrl[256] = "";

	EInfoState m_InfoState = EInfoState::ERROR;
	std::shared_ptr<IHttpRequest> m_pDDNetInfoTask = nullptr;

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
	bool m_HiddenWindow = false;

	CSnapshotDelta *SnapshotDelta();
	std::deque<std::shared_ptr<CDemoEdit>> m_EditJobs;

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

	void StopDemoSession(CSessionId SessionId, const char *pReason);

	// For RenderDebug function
	NETSTATS m_NetstatsPrev = {};
	NETSTATS m_NetstatsCurrent = {};
	std::chrono::nanoseconds m_NetstatsLastUpdate = std::chrono::nanoseconds(0);

public:
	IDiscord *Discord() { return m_pDiscord; }
	IEngineGraphics *Graphics() { return m_pGraphics; }
	IEngineInput *Input() { return m_pInput; }
	IEngineSound *Sound() { return m_pSound; }
	ISteam *Steam() { return m_pSteam; }
	INotifications *Notifications() { return m_pNotifications; }
	IEngineTextRender *TextRender() { return m_pTextRender; }
	IUpdater *Updater() { return m_pUpdater; }

	CClient();
	~CClient() override;

	IGraphics::CTextureHandle GetDebugFont() const override { return m_DebugFont; }

	// ------ state handling -----
	void FocusSessionForState(EClientState State) override;
	void OnStateChanged(EClientState State, EClientState OldState) override;
	void OnMapLoadStarted(CSessionId SessionId) override;

	void DisconnectDemoWithReason(CSessionId SessionId, const char *pReason);
	void LoadDebugFont();

	// ---

	void Render();
	void RenderScreen();
	void RenderDebug();
	void RenderGraphs();

	void Restart() override;
	void Quit() override;

	const char *PlayerName() const override;
	const char *LatestVersion() const override;

	bool PreprocessConnlessPacket7(CNetChunk *pPacket) override;
	void ProcessConnlessPacket(CNetChunk *pPacket) override;
	void ProcessServerInfo(int Type, NETADDR *pFrom, const void *pData, int DataSize);

	EInfoState InfoState() const override { return m_InfoState; }
	void RequestDDNetInfo() override;
	void ResetDDNetInfoTask();
	void LoadDDNetInfo();

	void Update();

	void RegisterInterfaces();
	void InitInterfaces();

	void Run();

	bool CtrlShiftKey(int Key, bool &Last);

	static void Con_Quit(IConsole::IResult *pResult, void *pUserData);
	static void Con_Restart(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoPlay(IConsole::IResult *pResult, void *pUserData);
	static void Con_DemoSpeed(IConsole::IResult *pResult, void *pUserData);
	static void Con_Minimize(IConsole::IResult *pResult, void *pUserData);
	static void Con_Screenshot(IConsole::IResult *pResult, void *pUserData);

#if defined(CONF_VIDEORECORDER)
	CVideoExportSettings DefaultVideoExportSettings();
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
	/**
	 * Sets up the video export that the command line asked for.
	 *
	 * @return `false` when the output file cannot be written, which has already
	 * been logged.
	 */
	bool ConfigureCommandLineVideoExport(const CCommandLineVideoExport &Export);
	int CommandLineExitCode() const { return m_CommandLineExitCode; }
#endif

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
	static void ConchainReplays(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainInputFifo(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
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

	void AutoScreenshot_Start() override;
	void AutoStatScreenshot_Start() override;
	void AutoScreenshot_Cleanup();
	void AutoStatScreenshot_Cleanup();

	void AutoCSV_Start() override;
	void AutoCSV_Cleanup();

	void ServerBrowserUpdate() override;

	const CServerInfo *KnownServerInfo(const NETADDR &Address) override;
	void RequestServerInfoRefresh(const NETADDR &Address) override;
	void RequestServerInfoWithToken(const NETADDR &Address, int *pBasicToken, int *pToken) override;
	void OnCurrentServerPing(const NETADDR &Address, int LatencyMs) override;
	const char *MapDownloadUrl() const override { return m_aMapDownloadUrl; }
	void RecordMessage(const void *pData, int Size) override;
	void RecordSnapshot(int Tick, const CSnapshot *pData, int Size) override;
	void DisconnectDemoWithReason(const char *pReason) override { DisconnectDemoWithReason(m_DemoSessionId, pReason); }

	void HandleConnectAddress(const NETADDR *pAddr);
	void HandleConnectLink(const char *pLink) override;
	void HandleDemoPath(const char *pPath);
	void HandleMapPath(const char *pPath);

	void InitChecksum();
	int HandleChecksum(CSessionId SessionId, CStreamId StreamId, CUuid Uuid, CUnpacker *pUnpacker) override;

	// gfx
	void Notify(const char *pTitle, const char *pMessage) override;
	void OnWindowResize() override;
	void BenchmarkQuit(int Seconds, const char *pFilename);
	CRenderTrace *RenderTrace() override { return &m_RenderTrace; }
	void StopRenderTrace();

	void UpdateAndSwap() override;

	// DDRace

	void RaceRecord_Start(const char *pFilename) override;
	void RaceRecord_Stop() override;
	bool RaceRecord_IsRecording() override;

	void DemoSliceBegin() override;
	void DemoSliceEnd() override;
	void DemoSlice(const char *pDstPath, CLIENTFUNC_FILTER pfnFilter, void *pUser) override;
	virtual void SaveReplay(int Length, const char *pFilename = "");

	bool EditorHasUnsavedData() const override { return m_pEditor != nullptr && m_pEditor->HasUnsavedData(); }

	IFriends *Foes() override { return &m_Foes; }

	CChecksumData *ChecksumData() override { return &m_Checksum.m_Data; }

	bool ViewLink(const char *pLink) override;
	bool ViewFile(const char *pFilename) override;

#if defined(CONF_FAMILY_WINDOWS)
	void ShellRegister() override;
	void ShellUnregister() override;
#endif

	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override;
	void GetGpuInfoString(char (&aGpuInfo)[512]) override;
};

/**
 * Creates the client that an entry point then runs, either the game's or the
 * demo render tool's.
 */
CClient *CreateClient();

#endif
