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

class CGameClient;
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
	// Which session the export runs through. The same one that is being
	// watched unless somebody made a second one for it, which is what a viewer
	// does so that watching goes on while a video is written.
	CSessionId m_VideoSessionId;
	CVideoExportSettings m_Settings;
	char m_aDemoPath[IO_MAX_PATH_LENGTH] = "";
	// Who to follow once the demo names them, empty when nobody was asked for
	// by name or the one who was has been found.
	char m_aPendingSpectateName[MAX_NAME_LENGTH] = "";
	char m_aVideoPath[IO_MAX_PATH_LENGTH] = "";
	char m_aError[256] = "";
	int64_t m_LastRenderTime = 0;

	/**
	 * The game client as what it is, rather than as the interface the engine
	 * talks to it through.
	 *
	 * A program built on this links the whole game client and is the only
	 * thing steering it, so what it asks of the camera and of the spectating
	 * it asks directly instead of widening an interface for one caller.
	 */
	CGameClient *Game() const;

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
	const char *PlayDemo() { return PlayDemo(m_DemoSessionId); }
	/**
	 * The same for a session other than the one being watched, which is how an
	 * export gets a demo of its own to run through.
	 */
	const char *PlayDemo(CSessionId SessionId);
	void StopDemoSession(const char *pReason) { StopDemoSession(m_DemoSessionId, pReason); }
	void StopDemoSession(CSessionId SessionId, const char *pReason);
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
	/**
	 * Who the demo is watched over the shoulder of: a client id, `SPEC_FREEVIEW`
	 * for a camera of one's own, or `SPEC_FOLLOW` for whoever the demo was
	 * recorded by. A demo of a server has nobody to follow, so it starts in the
	 * free view and this is how one of the players is picked instead.
	 */
	int Spectating() const;
	void SetSpectate(int SpectatorId);
	/**
	 * Moves on to the next player there is, or the one before, and past the end
	 * of them back to the free view. What a bar with two buttons on it offers,
	 * and what the keys do.
	 *
	 * @param Direction 1 for the next, -1 for the one before.
	 */
	void SpectateStep(int Direction);
	/**
	 * Follows whoever is called this, as soon as the demo has named them. A
	 * name is what somebody knows before the demo is open; which client id it
	 * belongs to is only in the demo.
	 */
	void SetSpectateName(const char *pName);
	/**
	 * Looks again for a player that was asked for by name. Does nothing once
	 * one was found, or when none was asked for.
	 */
	void UpdatePendingSpectate();
	/**
	 * What the player of a client id is called, or `nullptr` where the demo has
	 * no such player.
	 */
	const char *SpectatePlayerName(int ClientId) const;

	/**
	 * Whether a server recorded this demo rather than a client. A client's
	 * demo is of whoever recorded it, so there is somebody to follow; a
	 * server's is of everybody who happened to be there and of nobody in
	 * particular, so one of them has to be picked.
	 */
	bool ServerDemo() const;

	/**
	 * Moves the free view, in world units. Does nothing while a player is being
	 * followed, because then the view is theirs.
	 */
	void MoveFreeView(vec2 Offset);
	/**
	 * Multiplies how much of the world is in the window, which a wheel or a pair
	 * of buttons does one notch at a time.
	 */
	void ScaleZoom(float Factor);
	float Zoom() const;
	/**
	 * How wide one pixel of the window is in the world, so that what is dragged
	 * moves with the pointer at any zoom.
	 */
	float WorldPerPixel() const;

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
	CSessionId VideoSessionId() const override { return m_VideoSessionId; }
	// Where the export reads from a session of its own, the sound it writes is
	// mixed for it alone: what comes out of the speakers is the demo being
	// watched, and the two have nothing to do with each other.
	bool VideoUsesOfflineAudio() const override { return m_VideoSessionId != m_DemoSessionId; }
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
