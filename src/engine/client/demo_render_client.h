/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
#define ENGINE_CLIENT_DEMO_RENDER_CLIENT_H

#include "client_offline.h"
#include "friends.h"
#include "ghost.h"
#include "render_trace.h"

#include <engine/graphics.h>
#include <engine/shared/video.h>

#include <memory>

class CVideo;
class IEngineGraphics;
class IEngineSound;
class IEngineTextRender;

/**
 * The client of the demo render tool: it opens a demo, loads its map, draws
 * the game the way the client draws it and encodes what it drew.
 *
 * It is not a smaller game client. It has no connection, no server browser, no
 * editor, no input and no menu, and it does not link any of them. What it does
 * have is what a picture of a demo needs: the assets, the map, the game state
 * the demo carries and the drawing of it.
 */
class CDemoRenderClient : public CClientWithoutConnection
{
	IEngineGraphics *m_pGraphics = nullptr;
	IEngineSound *m_pSound = nullptr;
	IEngineTextRender *m_pTextRender = nullptr;

	CRenderTrace m_RenderTrace;
	CGhostRecorder m_GhostRecorder;
	CGhostLoader m_GhostLoader;
	// Who a player marked as a friend is written in the configuration, not
	// asked of anyone, so a demo names them the way the game does.
	CFriends m_Friends;
	CFriends m_Foes;
	IGraphics::CTextureHandle m_DebugFont;

	std::unique_ptr<CVideo> m_pVideo;
	CVideoExportSettings m_Settings;
	char m_aDemoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aVideoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aError[256] = "";
	int m_ExitCode = 0;
	int64_t m_LastRenderTime = 0;
	std::chrono::nanoseconds m_LastProgressLog{0};

	IEngineGraphics *Graphics() { return m_pGraphics; }
	IEngineSound *Sound() { return m_pSound; }
	IEngineTextRender *TextRender() { return m_pTextRender; }

	bool InitGraphics();
	const char *PlayDemo();
	const char *StartVideo();
	void StopDemoSession(const char *pReason);
	void RenderFrame();
	void LogProgress();

public:
	CDemoRenderClient();
	~CDemoRenderClient() override;

	/**
	 * Hands the kernel what this program brings besides itself: the demo player
	 * it plays with, and the ghost files the game reads.
	 */
	void RegisterInterfaces();
	void InitInterfaces();
	/**
	 * Takes over what the command line asked for.
	 *
	 * @return `false` when the output file cannot be written, which has already
	 * been logged.
	 */
	bool Configure(const CCommandLineVideoExport &Export);
	void Run();
	int ExitCode() const { return m_ExitCode; }

	// ----- what a program that draws answers -----
	CRenderTrace *RenderTrace() override { return &m_RenderTrace; }
	IGraphics::CTextureHandle GetDebugFont() const override { return m_DebugFont; }
	void UpdateAndSwap() override {}
	void OnWindowResize() override;
	void Notify(const char *pTitle, const char *pMessage) override {}
	std::optional<int> ShowMessageBox(const IGraphics::CMessageBox &MessageBox) override { return std::nullopt; }
	void GetGpuInfoString(char (&aGpuInfo)[512]) override;
	bool ViewLink(const char *pLink) override { return false; }
	bool ViewFile(const char *pFilename) override { return false; }

	IFriends *Foes() override { return &m_Foes; }

	void Quit() override;
	void Restart() override { Quit(); }
	const char *PlayerName() const override { return "(demo)"; }
	const char *DummyName() override { return "(demo)"; }
	const char *ErrorString() const override { return m_aError; }

	// ----- the render this tool exists for -----
	const char *DemoPlayer_Play(const char *pFilename, int StorageType) override;
	CSessionId VideoSessionId() const override { return m_DemoSessionId; }
	bool VideoUsesOfflineAudio() const override { return false; }
	bool DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const override;

	// A render queue is something the menu offers, and there is no menu here:
	// the tool renders the one demo it was given and stops.
	const char *DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue) override { return "The demo render tool renders one demo."; }
	void DemoPlayer_StartRenderQueue() override {}
	void DemoPlayer_ClearRenderQueue() override {}
	size_t DemoPlayer_RenderQueueSize() const override { return 0; }
	size_t DemoPlayer_RenderQueuePending() const override { return 0; }
	const char *DemoPlayer_RenderQueueName(size_t Index) const override { return ""; }
	const char *DemoPlayer_ActiveRenderName() const override { return m_aDemoPath; }
	void DemoPlayer_RenderQueueErase(size_t Index) override {}
	void DemoPlayer_RenderQueueMove(size_t Index, bool Up) override {}
	void DemoPlayer_CancelActiveRender() override { Quit(); }
	bool DemoPlayer_RenderQueueActive() const override { return m_pVideo != nullptr; }
	const char *DemoPlayer_RenderQueueError() const override { return m_aError; }
};

#endif // ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
