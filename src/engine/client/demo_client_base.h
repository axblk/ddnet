/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_CLIENT_BASE_H
#define ENGINE_CLIENT_DEMO_CLIENT_BASE_H

#include "client_core.h"
#include "friends.h"
#include "ghost.h"
#include "render_trace.h"

#include <engine/graphics.h>
#include <engine/shared/video.h>

#include <memory>
#include <vector>

class IConfigManager;
class IEngine;
class IEngineGraphics;
class IEngineGraphicsWindow;
class IEngineSound;
class IEngineTextRender;

/**
 * A program that only shows a demo: it opens one demo, loads its map and draws
 * it the way the client does, and can hand what it drew to a video encoder.
 * It has no connection, server browser, editor or menu and links none of them.
 */
class CDemoClientBase : public CClientCore
{
	IEngine *m_pEngine = nullptr;
	IConfigManager *m_pConfigManager = nullptr;
	IEngineGraphicsWindow *m_pWindow = nullptr;
	IEngineGraphics *m_pGraphics = nullptr;
	IEngineSound *m_pSound = nullptr;
	IEngineTextRender *m_pTextRender = nullptr;

	CRenderTrace m_RenderTrace;
	CGhostRecorder m_GhostRecorder;
	CGhostLoader m_GhostLoader;
	CFriends m_Friends;
	CFriends m_Foes;

protected:
#if defined(CONF_VIDEORECORDER)
	std::unique_ptr<IVideo> m_pVideo;
#endif
	CVideoExportSettings m_Settings;
	char m_aDemoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aVideoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aError[256] = "";
	int64_t m_LastRenderTime = 0;

	IEngine *Engine() { return m_pEngine; }
	IEngineGraphics *Graphics() { return m_pGraphics; }
	IEngineSound *Sound() { return m_pSound; }
	IEngineTextRender *TextRender() { return m_pTextRender; }

	/**
	 * Opens the graphics in the given window, then the sound, the text render
	 * and the game.
	 *
	 * @param pWindow The window to draw into, taken over by the kernel.
	 *
	 * @return `false` when the graphics could not be opened, which has been
	 * logged.
	 */
	bool InitGame(IEngineGraphicsWindow *pWindow);
	/**
	 * Ends a running export, closes the demo and shuts down what `InitGame`
	 * opened.
	 */
	void ShutdownGame();

	/**
	 * Opens the demo at `m_aDemoPath` and starts playing it.
	 *
	 * @return An error message, or `nullptr` when the demo plays.
	 */
	const char *PlayDemo();
	void StopDemoSession(const char *pReason);
	/**
	 * Advances the time, the demo, the sound and the game by one frame.
	 */
	void Update();

#if defined(CONF_VIDEORECORDER)
	/**
	 * Starts encoding what is drawn into `m_aVideoPath` with `m_Settings`.
	 *
	 * @return An error message, or `nullptr` when the export runs.
	 */
	const char *StartVideo();
	bool Exporting() const;
	/**
	 * Draws one frame into the running export.
	 */
	void RenderExportFrame();
	/**
	 * Called after every frame that went into the export.
	 */
	virtual void OnExportFrame() {}
#endif

public:
	CDemoClientBase();
	~CDemoClientBase() override;

	/**
	 * Hands the kernel the demo player, the ghost files and the friends.
	 */
	void RegisterInterfaces();
	void InitInterfaces();

	/**
	 * Takes the arguments this program understands off the command line and
	 * leaves the rest for the console.
	 *
	 * @return An exit code when the program is done already, e.g. after
	 * printing its usage.
	 */
	virtual std::optional<int> ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments) = 0;
	/**
	 * Called once the configuration and the command line were executed.
	 *
	 * @return `false` when the program cannot run, which has been logged.
	 */
	virtual bool Configure() { return true; }
	/**
	 * @return The exit code of the program.
	 */
	virtual int Run() = 0;

	CRenderTrace *RenderTrace() override { return &m_RenderTrace; }
	IGraphics::CTextureHandle GetDebugFont() override { return IGraphics::CTextureHandle(); }
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
	const char *PlayerName() const override { return ""; }
	const char *DummyName() override { return ""; }
	const char *ErrorString() const override { return m_aError; }

	const char *DemoPlayer_Play(const char *pFilename, int StorageType) override;
#if defined(CONF_VIDEORECORDER)
	CSessionId VideoExportSessionId() const override { return {}; }
	CSessionId VideoSessionId() const override { return m_DemoSessionId; }
	bool VideoUsesOfflineAudio() const override { return false; }
	bool DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const override;
	CVideoExportSettings DefaultVideoExportSettings() override { return m_Settings; }

	// There is no menu and so no render queue: the one demo is the one drawn.
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
	bool DemoPlayer_RenderQueueActive() const override;
	const char *DemoPlayer_RenderQueueError() const override { return m_aError; }
#endif
};

/**
 * The entry point of a program built on `CDemoClientBase`: puts together the
 * interfaces the game runs on, reads the configuration and the command line and
 * runs the client.
 *
 * @param pClient The client, taken over.
 * @param ArgumentCount The number of command line arguments.
 * @param ppArguments The command line arguments, the program's name first.
 *
 * @return The exit code of the program.
 */
int DemoClientMain(CDemoClientBase *pClient, int ArgumentCount, const char **ppArguments);

#endif // ENGINE_CLIENT_DEMO_CLIENT_BASE_H
