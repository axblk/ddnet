/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_VIEWER_CLIENT_H
#define ENGINE_CLIENT_DEMO_VIEWER_CLIENT_H

#include "demo_client_base.h"
#include "viewer_controls.h"

#include <array>
#include <chrono>

class IEngineInput;

/**
 * The client of the demo viewer: it opens one demo and shows it in a window,
 * at the speed of a clock, with the keyboard to steer it.
 *
 * What it is made of is `CDemoClientBase`; what it adds is a window somebody
 * looks at, the loop that keeps time with them, and the keys that pause, seek
 * and change the speed. It can also run the same export the render tool runs,
 * out of its own loop, and then it draws the export instead of the window.
 */
class CDemoViewerClient : public CDemoClientBase
{
public:
	/**
	 * How an export that somebody asked for is getting on. A browser cannot be
	 * told, it has to ask: what starts an export there returns before the
	 * browser has even been asked for an encoder.
	 */
	enum class EExportState
	{
		IDLE,
		RUNNING,
		FINISHED,
		FAILED,
	};

	/**
	 * The keys the viewer answers to. Held in this order in the state of the
	 * previous frame, which is what tells a press from a hold.
	 */
	enum EControlKey
	{
		CONTROL_KEY_PAUSE,
		CONTROL_KEY_SEEK_BACK,
		CONTROL_KEY_SEEK_FORWARD,
		CONTROL_KEY_SPEED_UP,
		CONTROL_KEY_SPEED_DOWN,
		CONTROL_KEY_RESTART,
		CONTROL_KEY_QUIT,
		NUM_CONTROL_KEYS,
	};

private:
	IEngineInput *m_pInput = nullptr;
	bool m_Surfaceless = false;
	std::array<bool, NUM_CONTROL_KEYS> m_aKeyWasPressed = {};
	std::array<std::chrono::nanoseconds, NUM_CONTROL_KEYS> m_aKeyRepeatTime = {};
	// When the next frame is due. A viewer draws in real time, so drawing
	// faster than the screen shows is only heat - and in a browser waiting is
	// also the moment the page is given back its turn to paint and to answer.
	std::chrono::nanoseconds m_NextFrameTime{};
	int m_ExitCode = 0;
	EExportState m_ExportState = EExportState::IDLE;
	CViewerControls m_Controls;
	bool m_ShowControls = true;
	// What a page asked for, to be done between two frames rather than in the
	// call that asked. Starting or ending an export waits for the browser, and
	// waiting unwinds the stack it is waiting on - which, in a call that came
	// from the page, is not the stack the viewer runs on. The loop has its own,
	// and that one may be unwound.
	bool m_ExportRequested = false;
	bool m_CancelRequested = false;
	CVideoExportSettings m_RequestedSettings;

	IEngineInput *Input() { return m_pInput; }

	/**
	 * Answers the keys that steer the playback.
	 *
	 * @return `false` when the window was closed.
	 */
	bool HandleInput();
	/**
	 * Whether a key counts as pressed this frame: on the frame it goes down,
	 * and again every so often while it stays down for the keys that may
	 * repeat.
	 */
	bool KeyPressed(EControlKey ControlKey, bool Repeats);
	void RenderWindowFrame();
	/**
	 * Draws the viewer's own controls over the demo and does what was pressed
	 * in them.
	 */
	void RenderControls();
	/**
	 * Starts writing a video of the demo from here on.
	 *
	 * @param Settings How to encode it.
	 *
	 * @return `true` when the export started.
	 */
	bool StartExport(const CVideoExportSettings &Settings);
	/**
	 * Throws away the export that is running, along with its unfinished file.
	 */
	void CancelExport();
	/**
	 * Closes the file of an export that has run to the end of the demo and
	 * plays the demo again, so that what was being watched is still there
	 * afterwards.
	 */
	void FinishExport();

public:
	/**
	 * Takes over what the command line asked for.
	 *
	 * @param pDemoPath The demo to show.
	 * @param pVideoPath Where to write a video of it, or `nullptr` to only
	 * show it.
	 * @param Settings How to encode that video.
	 */
	void Configure(const char *pDemoPath, const char *pVideoPath, const CVideoExportSettings &Settings);
	/**
	 * Whether the viewer draws its own bar of controls over the demo. It does
	 * unless it is told otherwise, because a viewer that shows no way to use
	 * it has none. A page that puts its own controls beside the canvas turns
	 * this off.
	 */
	void SetShowControls(bool Show) { m_ShowControls = Show; }
	bool ShowControls() const { return m_ShowControls; }
	void Run();
	int ExitCode() const { return m_ExitCode; }

	// The window is drawn into, so what was drawn has to reach it.
	void UpdateAndSwap() override;
	// Cancelling an export leaves the viewer showing the demo, it does not end
	// the program the way it ends the render tool.
	void DemoPlayer_CancelActiveRender() override;

	/**
	 * What a page around the canvas can ask of the demo. A browser has no
	 * keyboard shortcuts to discover, so the controls are in the page and
	 * these are what they call; on every other platform nothing calls them.
	 */
	void SetPaused(bool Paused);
	void SeekPercent(float Percent);
	void SeekTime(float Seconds);
	void SeekStart();
	void SetSpeed(float Speed);
	/**
	 * Asks for a video of the demo from here on, which the page offers once
	 * and hands to the browser's downloads when it is finished. It is started
	 * before the next frame; `ExportState` says how it went.
	 *
	 * @param Settings How to encode it, the same settings the render tool
	 * takes on its command line. The size, the frame rate and the quality are
	 * brought into range here, so a page may pass on whatever was typed into
	 * its form.
	 * @return `true` when there was no export running already.
	 */
	bool RequestExport(const CVideoExportSettings &Settings);
	/**
	 * Asks for the export that is running to be thrown away, along with its
	 * unfinished file. Done before the next frame, as with `RequestExport`.
	 */
	void RequestCancelExport() { m_CancelRequested = true; }

	bool Paused() const;
	float Progress() const;
	float Speed() const;
	float Length() const;
	bool Exporting() const;
	EExportState ExportState() const { return m_ExportState; }
};

#endif // ENGINE_CLIENT_DEMO_VIEWER_CLIENT_H
