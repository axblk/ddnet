/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_PLAYER_CLIENT_H
#define ENGINE_CLIENT_DEMO_PLAYER_CLIENT_H

#include "demo_client_base.h"
#include "viewer_gestures.h"

#if !defined(CONF_WEB_PLATFORM)
#include "viewer_controls.h"
#endif

#include <chrono>
#include <functional>
#include <string>
#include <vector>

class IEngineInput;

/**
 * The client of the demo player: it shows one demo in a window, in real time,
 * with the keys and a bar of a video player to steer it. A page around the
 * canvas can steer it as well, and can have it write a video of the demo in a
 * session of its own while the demo goes on playing.
 */
class CDemoPlayerClient : public CDemoClientBase
{
public:
	/**
	 * How the last export that was asked for went. A page polls this.
	 */
	enum class EExportState
	{
		IDLE,
		RUNNING,
		FINISHED,
		FAILED,
	};

private:
	IEngineInput *m_pInput = nullptr;
	bool m_Surfaceless = false;
	// When the next frame is due. Drawing faster than the screen shows is only
	// heat, and in a browser the wait is when the page gets its turn.
	std::chrono::nanoseconds m_NextFrameTime{0};
	// During an export the window is drawn on a clock of its own.
	int64_t m_LastWindowRenderTime = 0;
	std::chrono::nanoseconds m_LastExportScreenRender{0};
#if !defined(CONF_WEB_PLATFORM)
	// In a browser the page draws the controls, next to the canvas.
	CViewerControls m_Controls;
#endif
	CViewerGestures m_Gestures;
	bool m_ShowControls = true;
	bool m_ZoomEnabled = true;
	// Whether Tab and `=` show the scoreboard and the statboard while held,
	// and whether they are held.
	bool m_OverlaysEnabled = true;
	bool m_ScoreboardShown = false;
	bool m_StatboardShown = false;
	// What the keys and the pointer did this frame, for the bar.
	bool m_KeyPressed = false;
	bool m_MouseClicked = false;
	// Tells a page that asked for another demo when it is there.
	int m_LoadCount = 0;
	// A demo of a server follows nobody, so its export waits for a choice.
	bool m_SpectateChosen = false;
	// The volume to go back to when unmuted, zero while not muted.
	int m_VolumeBeforeMute = 0;
	// What `PlayerName` returned last.
	char m_aPlayerName[MAX_NAME_LENGTH] = "";
	// How the view draws, the same for the demo and its export. Somebody
	// watching a demo wants the map as it was made, so detail is on and the
	// key presses are off.
	CViewRenderOptions m_RenderOptions;
	// See `SetStartTime`.
	float m_StartTime = -1.0f;
	float m_StartSpeed = 0.0f;
	bool m_StartPaused = false;
	// See `SetClip`. A negative end means nothing is marked.
	float m_ClipStart = 0.0f;
	float m_ClipEnd = -1.0f;
	// Dragging the seek bar pauses the demo, and it goes on afterwards only
	// if it was going before.
	bool m_Seeking = false;
	bool m_PausedBeforeSeeking = false;
	// Dragging moves the free view.
	vec2 m_LastMousePos = vec2(0.0f, 0.0f);
	bool m_Dragging = false;
	// What `Players` returned last.
	std::string m_Players;
	// See `FromPage`.
	std::vector<std::function<void()>> m_vPageActions;
#if defined(CONF_VIDEORECORDER)
	// An export reads the demo again in a session of its own, so that
	// watching goes on beside it.
	CSessionId m_ExportSessionId;
	CDemoListener m_ExportDemoListener;
	// The session the running export reads, the watched one otherwise.
	CSessionId m_VideoSessionId;
	EExportState m_ExportState = EExportState::IDLE;
	// When the running export was asked for, to say how long it still has.
	std::chrono::nanoseconds m_ExportStartTime{0};
	// Kept after it was logged, for a page to ask.
	char m_aExportError[256] = "";
	// What the bar's export menu was last set to.
	bool m_ExportAudio = true;
	int m_ExportFps = 60;
#endif

	/**
	 * Answers the keys, the wheel, the fingers and the pointer.
	 *
	 * @return `false` when the window was closed.
	 */
	bool HandleInput();
	/**
	 * Shows the scoreboard and the statboard while their keys are held.
	 */
	void UpdateOverlays();
	/**
	 * How wide one pixel of the window is in the world.
	 */
	float WorldPerPixel() const;
	void RenderWindowFrame();
#if !defined(CONF_WEB_PLATFORM)
	/**
	 * Draws the bar over the demo and does what was pressed in it.
	 */
	void RenderControls();
#endif
	/**
	 * Opens the demo dropped on the window in place of the one that plays.
	 */
	void OpenDroppedDemo(const char *pPath);
	/**
	 * Does what a page asked for while the program could not.
	 */
	void RunPageActions();
#if defined(CONF_VIDEORECORDER)
	/**
	 * Starts writing a video of the demo, or of the marked piece of it, in a
	 * session of its own.
	 *
	 * @param Settings How to encode it.
	 * @param SpectatorId Whom the video follows, as in `Spectate`.
	 *
	 * @return `true` when the export started.
	 */
	bool StartExport(const CVideoExportSettings &Settings, int SpectatorId);
	/**
	 * Throws away the export that is running, along with its unfinished file.
	 */
	void CancelExport();
	/**
	 * Closes the file of an export that reached its end and goes back to
	 * showing the demo alone.
	 */
	void FinishExport();
	/**
	 * Draws the next frame of the running export, and the window beside it
	 * often enough to stay usable.
	 */
	void UpdateExport();
#endif

public:
	CDemoPlayerClient();

	std::optional<int> ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments) override;
	int Run() override;

	// The window is drawn into, so what was drawn has to reach it.
	void UpdateAndSwap() override;

#if defined(CONF_VIDEORECORDER)
	CSessionId VideoExportSessionId() const override { return m_ExportSessionId; }
	CSessionId VideoSessionId() const override { return m_VideoSessionId; }
	// An export in a session of its own mixes its sound apart from the speakers.
	bool VideoUsesOfflineAudio() const override { return m_VideoSessionId != m_DemoSessionId; }
#endif

	/**
	 * Whether the player draws its own bar over the demo. A page with
	 * controls of its own turns it off.
	 */
	void SetShowControls(bool Show) { m_ShowControls = Show; }
	bool ShowControls() const { return m_ShowControls; }
	/**
	 * Whether the wheel, the zoom keys and a pinch zoom the view. A page that
	 * wants the wheel for scrolling turns it off; `ScaleZoom` works either way.
	 */
	void SetZoomEnabled(bool Enabled) { m_ZoomEnabled = Enabled; }
	bool ZoomEnabled() const { return m_ZoomEnabled; }
	/**
	 * Whether Tab and `=` show the scoreboard and the statboard while held. A
	 * page may want Tab for moving the focus instead.
	 */
	void SetOverlaysEnabled(bool Enabled) { m_OverlaysEnabled = Enabled; }
	bool OverlaysEnabled() const { return m_OverlaysEnabled; }
	/**
	 * How many demos were opened so far.
	 */
	int LoadCount() const { return m_LoadCount; }

	/**
	 * Where in the demo to start, how fast, and whether paused, applied before
	 * the first frame so that a link opens at its place. A negative time and a
	 * speed of zero mean nothing was asked for.
	 */
	void SetStartTime(float Seconds) { m_StartTime = Seconds; }
	void SetStartSpeed(float Speed) { m_StartSpeed = Speed; }
	void SetStartPaused(bool Paused) { m_StartPaused = Paused; }

	/**
	 * How big to draw, in window units, for a player in a box of a page's
	 * own: nothing tells the window when the box changes shape.
	 */
	void SetSize(int Width, int Height);

	/**
	 * Does what a page asked for now, or in the loop before the next frame
	 * when the program is unwound in one of its waits (`web_unwound`), where
	 * waiting again would take it down. Queries do not wait and are answered
	 * at once, so they show a change only a frame later.
	 */
	void FromPage(std::function<void()> &&Action);

	void SetPaused(bool Paused);
	/**
	 * Starts playing, from the beginning - of the marked piece, if there is
	 * one - when at the end, like a video.
	 */
	void Play();
	/**
	 * Play if it stands still, stop if it runs.
	 */
	void TogglePause();
	/**
	 * Whether it stands at the end of the marked piece, or of the demo.
	 */
	bool AtEnd() const;
	void SeekPercent(float Percent);
	/**
	 * Seeks to a time counted from the beginning of the demo.
	 */
	void SeekToTime(float Seconds);
	/**
	 * Goes to the beginning of the marked piece, or of the demo.
	 */
	void SeekStart();
	void SetSpeed(float Speed);
	bool Paused() const;
	float Progress() const;
	float Speed() const;
	float Length() const;

	/**
	 * Marks out a piece of the demo, in seconds from its beginning. Playback
	 * stops at its end, starting over goes to its beginning, and an export
	 * writes only the piece.
	 *
	 * @param Start Where it begins. Below zero is the beginning of the demo.
	 * @param End Where it ends. Anything at or before `Start` clears the mark,
	 * which is what a whole demo is.
	 */
	void SetClip(float Start, float End);
	/**
	 * Marks the place the demo stands at as the one end of the piece or the
	 * other, and leaves the other end where it was.
	 */
	void MarkClip(bool AsStart);
	float ClipStart() const { return m_ClipStart; }
	/**
	 * Where the marked piece ends, or a negative number where nothing is
	 * marked.
	 */
	float ClipEnd() const { return m_ClipEnd; }
	bool HasClip() const { return m_ClipEnd > m_ClipStart; }

	/**
	 * Whom the demo is watched from: a client id, `SPEC_FREEVIEW`, or
	 * `SPEC_FOLLOW` for whoever recorded it.
	 */
	int Spectating();
	/**
	 * `Spectate` for a choice somebody made, which a server demo's export
	 * waits for.
	 */
	void ChooseSpectate(int SpectatorId);
	using CDemoClientBase::SetSpectateName;
	/**
	 * Moves on to the next player of the demo, or the one before, and past the
	 * last to the free view.
	 *
	 * @param Direction 1 for the next, -1 for the one before.
	 */
	void SpectateStep(int Direction);
	/**
	 * Whether a server recorded the demo, which then follows nobody by
	 * itself.
	 */
	bool ServerDemo() const;
	/**
	 * The name of a player of the demo, or `nullptr` where there is none.
	 * Valid until the next call.
	 */
	const char *PlayerName(int ClientId);
	/**
	 * The players the demo has named so far, as a JSON array of objects with
	 * an `id` and a `name`. Valid until the next call.
	 */
	const char *Players();

	/**
	 * `snd_volume` between 0 and 1; while muted, the volume before.
	 */
	float Volume() const;
	void SetVolume(float Volume);
	/**
	 * Muting remembers the volume, so that unmuting goes back to it.
	 */
	bool Muted() const;
	void SetMuted(bool Muted);

	/**
	 * The zoom, the free view and the camera the demo brought, of the demo
	 * being watched. See `IViewControl`.
	 */
	void MoveFreeView(vec2 Offset) { ViewControl()->MoveFreeView(m_DemoSessionId, Offset); }
	void ScaleZoom(float Factor) { ViewControl()->ScaleZoom(m_DemoSessionId, Factor); }
	float Zoom() { return ViewControl()->Zoom(m_DemoSessionId); }
	void ResetZoom() { ViewControl()->ResetZoom(m_DemoSessionId); }
	bool ZoomChanged() { return ViewControl()->ZoomChanged(m_DemoSessionId); }
	bool RecordedCameraAvailable() { return ViewControl()->RecordedCameraAvailable(m_DemoSessionId); }
	bool RecordedCamera() { return ViewControl()->RecordedCamera(m_DemoSessionId); }
	void SetRecordedCamera(bool Use) { ViewControl()->SetRecordedCamera(m_DemoSessionId, Use); }

	/**
	 * How the demo and its export are drawn, without touching the
	 * configuration.
	 */
	const CViewRenderOptions &RenderOptions() const { return m_RenderOptions; }
	void SetRenderOptions(const CViewRenderOptions &Options);

	/**
	 * Asks for a video of the demo, or of the marked piece of it. It is
	 * started before the next frame; `ExportState` says how it went.
	 *
	 * @param Settings How to encode it. The size, the frame rate and the
	 * quality are brought into range here, so a page may pass on whatever
	 * was typed into its form.
	 * @param SpectatorId Whom the video follows. A demo of a server has
	 * nobody to follow by itself, so there it has to be a player or
	 * `SPEC_FREEVIEW`.
	 *
	 * @return `true` when there was no export running already.
	 */
	bool RequestExport(const CVideoExportSettings &Settings, int SpectatorId);
	/**
	 * Asks for a video of the given size, with the sound and the rate the
	 * export menu was last set to. What the bar's export items do.
	 */
	void ExportFromControls(int Width, int Height);
	/**
	 * Asks for the export that is running to be thrown away, along with its
	 * unfinished file. Done before the next frame, as with `RequestExport`.
	 */
	void RequestCancelExport();
	/**
	 * How far the export has come, between 0 and 1.
	 */
	float ExportProgress() const;
	/**
	 * How much longer the export has to run, in seconds, or a negative
	 * number when there is no telling yet.
	 */
	float ExportSecondsLeft() const;
	EExportState ExportState() const;
	/**
	 * Why the export that was last asked for failed, or an empty string when
	 * none has.
	 */
	const char *ExportError() const;
};

#endif // ENGINE_CLIENT_DEMO_PLAYER_CLIENT_H
