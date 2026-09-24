/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_CLIENT_BASE_H
#define ENGINE_CLIENT_DEMO_CLIENT_BASE_H

#include "client_core.h"
#include "friends.h"
#include "ghost.h"
#include "render_trace.h"

#include <engine/graphics.h>
#include <engine/shared/protocol.h>
#include <engine/shared/video.h>
#include <engine/view_control.h>

#include <memory>
#include <vector>

class IConfigManager;
class IEngine;
class IEngineGraphics;
class IEngineGraphicsWindow;
class IEngineInput;
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
	// The ticks the running export writes, -1 for the rest of the demo.
	int m_VideoFirstTick = -1;
	int m_VideoLastTick = -1;
	char m_aError[256] = "";
	int64_t m_LastRenderTime = 0;
	// Who to follow once the demo names them, see `SetSpectateName`.
	char m_aPendingSpectateName[MAX_NAME_LENGTH] = "";

	IEngine *Engine() { return m_pEngine; }
	IEngineGraphicsWindow *Window() { return m_pWindow; }
	IEngineGraphics *Graphics() { return m_pGraphics; }
	IEngineSound *Sound() { return m_pSound; }
	IEngineTextRender *TextRender() { return m_pTextRender; }
	/**
	 * What the game lets this program steer of the views of its demos.
	 */
	IViewControl *ViewControl() { return GameClient()->ViewControl(); }

	/**
	 * Opens the graphics in the given window, then the sound, the text render,
	 * the input and the game.
	 *
	 * @param pWindow The window to draw into, taken over by the kernel.
	 * @param pInput The input to open, `nullptr` when nobody is at the keyboard.
	 *
	 * @return `false` when the graphics could not be opened, which has been
	 * logged.
	 */
	bool InitGame(IEngineGraphicsWindow *pWindow, IEngineInput *pInput);
	/**
	 * Ends a running export, closes the demo and shuts down what `InitGame`
	 * opened.
	 */
	void ShutdownGame();

	/**
	 * Opens the demo at `m_aDemoPath` in a session and starts playing it.
	 *
	 * @param SessionId The session to play it in: the demo being watched, or
	 * one it is read again in beside it, as an export may.
	 *
	 * @return An error message, or `nullptr` when the demo plays.
	 */
	const char *PlayDemo(CSessionId SessionId);
	const char *PlayDemo() { return PlayDemo(m_DemoSessionId); }
	/**
	 * Stops the demo of a session.
	 *
	 * @param SessionId The session whose demo stops.
	 * @param pReason Why, which ends the program with an error when it is the
	 * demo being watched. `nullptr` or empty when nothing went wrong.
	 */
	void StopDemoSession(CSessionId SessionId, const char *pReason);
	void StopDemoSession(const char *pReason) { StopDemoSession(m_DemoSessionId, pReason); }
	/**
	 * Follows a player of the demo being watched: a client id,
	 * `SPEC_FREEVIEW`, or `SPEC_FOLLOW` for whoever recorded it. Replaces a
	 * name `SetSpectateName` still waits for.
	 */
	void Spectate(int SpectatorId);
	/**
	 * Follows whoever is called this in the demo being watched, as soon as
	 * the demo has named them.
	 */
	void SetSpectateName(const char *pName);
	/**
	 * Advances the time, the demos, the sound and the game by one frame.
	 */
	void Update();

#if defined(CONF_VIDEORECORDER)
	/**
	 * Starts encoding the demo of `VideoSessionId()` into `m_aVideoPath` with
	 * `m_Settings`, up to `m_VideoLastTick`.
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
	void UpdateAndSwap() override {}
	void OnWindowResize() override;
	void Notify(const char *pTitle, const char *pMessage) override {}

	IFriends *Foes() override { return &m_Foes; }

	/**
	 * Ends the program after the current frame.
	 */
	void Quit();

#if defined(CONF_VIDEORECORDER)
	CSessionId VideoExportSessionId() const override { return {}; }
	CSessionId VideoSessionId() const override { return m_DemoSessionId; }
	bool VideoUsesOfflineAudio() const override { return false; }
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
