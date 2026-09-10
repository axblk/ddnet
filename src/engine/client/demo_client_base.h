/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_CLIENT_BASE_H
#define ENGINE_CLIENT_DEMO_CLIENT_BASE_H

#include "client_offline.h"
#include "friends.h"
#include "ghost.h"
#include "render_trace.h"

#include <engine/graphics.h>
#include <engine/shared/video.h>

#include <memory>

class IEngineGraphics;
class IEngineGraphicsWindow;
class IEngineSound;
class IEngineTextRender;

/**
 * What a program that only watches a demo is made of: it opens a demo, loads
 * its map, draws the game the way the client draws it, and can hand what it
 * drew to an encoder.
 *
 * It is not a smaller game client. It has no connection, no server browser, no
 * editor and no menu, and it does not link any of them. What it does have is
 * what a picture of a demo needs: the assets, the map, the game state the demo
 * carries and the drawing of it.
 *
 * Two programs are built on it: the render tool, which draws into a surface
 * without a window and encodes every frame as fast as it can, and the viewer,
 * which draws into a window at the speed of a clock.
 */
class CDemoClientBase : public CClientWithoutConnection
{
	IEngineGraphicsWindow *m_pWindow = nullptr;
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

protected:
	std::unique_ptr<IVideo> m_pVideo;
	CVideoExportSettings m_Settings;
	char m_aDemoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aVideoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aError[256] = "";
	int64_t m_LastRenderTime = 0;

	IEngineGraphics *Graphics() { return m_pGraphics; }
	IEngineSound *Sound() { return m_pSound; }
	IEngineTextRender *TextRender() { return m_pTextRender; }
	IEngineGraphicsWindow *Window() { return m_pWindow; }

	/**
	 * Registers the window and the graphics with the kernel and opens them.
	 *
	 * The caller brings the window, because a program that encodes draws into
	 * a surface without one and a program that is watched draws into a real
	 * one.
	 *
	 * @param pWindow The window to draw into. Taken over by the kernel.
	 *
	 * @return `false` when the graphics could not be opened, which has already
	 * been logged.
	 */
	bool InitGraphics(IEngineGraphicsWindow *pWindow);
	/**
	 * Opens the text render. It draws with the graphics, so it cannot be part
	 * of `InitInterfaces`, which runs before there are any.
	 */
	void InitTextRender();
	/**
	 * Opens the demo at `m_aDemoPath` and gets the game ready to be drawn.
	 *
	 * @return an error message, or `nullptr` when the demo plays.
	 */
	const char *PlayDemo();
	void StopDemoSession(const char *pReason);
	/**
	 * Draws one frame into the running export and hands it to the encoder.
	 * Does nothing while no export is running.
	 */
	void RenderExportFrame();
	/**
	 * Called after every frame that went into the export, so that a program
	 * can say how far it has come.
	 */
	virtual void OnExportFrame() {}

public:
	CDemoClientBase();
	~CDemoClientBase() override;

	/**
	 * Hands the kernel what this program brings besides itself: the demo player
	 * it plays with, and the ghost files the game reads.
	 */
	void RegisterInterfaces();
	void InitInterfaces();

	/**
	 * Starts encoding what is drawn into `m_aVideoPath` with `m_Settings`.
	 *
	 * @return an error message, or `nullptr` when the export runs.
	 */
	const char *StartVideo();

	// ----- what a program that draws answers -----
	CRenderTrace *RenderTrace() override { return &m_RenderTrace; }
	// Only the editor draws with it, and neither of these has an editor.
	IGraphics::CTextureHandle GetDebugFont() const override { return IGraphics::CTextureHandle(); }
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

	const char *DemoPlayer_Play(const char *pFilename, int StorageType) override;
	CSessionId VideoSessionId() const override { return m_DemoSessionId; }
	bool VideoUsesOfflineAudio() const override { return false; }
	bool DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const override;

	// The command line or the controls settled these before the export
	// started, so they are what this program was asked for.
	CVideoExportSettings DefaultVideoExportSettings() override { return m_Settings; }

	// A render queue is something the menu offers, and there is no menu here:
	// one demo is opened and that is the one that is drawn.
	const char *DemoPlayer_Render(const char *pFilename, int StorageType, const char *pVideoName, const CVideoExportSettings &Settings, int SpeedIndex, bool StartQueue) override { return "A demo program renders one demo."; }
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

#endif // ENGINE_CLIENT_DEMO_CLIENT_BASE_H
