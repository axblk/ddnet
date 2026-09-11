/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_viewer_client.h"

#include "window_sdl.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonwriter.h>
#include <engine/sound.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cstdlib>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

using namespace std::chrono_literals;

namespace
{
	// How long a held key waits before it starts repeating, and how often it
	// repeats after that. Seeking is the only thing worth holding down.
	constexpr std::chrono::nanoseconds KEY_REPEAT_DELAY = 400ms;
	constexpr std::chrono::nanoseconds KEY_REPEAT_INTERVAL = 60ms;
	// How far one press of a seek key moves.
	constexpr float SEEK_SECONDS = 5.0f;
	// How much of the world one notch of the wheel or one press of a zoom key
	// adds or takes away. The same step the client zooms in.
	constexpr float ZOOM_STEP = 1.0f;

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// The one viewer there is while the page is open, so that the controls
	// beside the canvas have something to talk to. Nothing else ever runs two.
	CDemoViewerClient *g_pDemoViewer = nullptr;
#endif

	int ControlKeyCode(CDemoViewerClient::EControlKey ControlKey)
	{
		switch(ControlKey)
		{
		case CDemoViewerClient::CONTROL_KEY_PAUSE: return KEY_SPACE;
		case CDemoViewerClient::CONTROL_KEY_SEEK_BACK: return KEY_LEFT;
		case CDemoViewerClient::CONTROL_KEY_SEEK_FORWARD: return KEY_RIGHT;
		case CDemoViewerClient::CONTROL_KEY_SPEED_UP: return KEY_UP;
		case CDemoViewerClient::CONTROL_KEY_SPEED_DOWN: return KEY_DOWN;
		case CDemoViewerClient::CONTROL_KEY_RESTART: return KEY_HOME;
		case CDemoViewerClient::CONTROL_KEY_FREE_VIEW: return KEY_F;
		case CDemoViewerClient::CONTROL_KEY_SPECTATE_NEXT: return KEY_N;
		case CDemoViewerClient::CONTROL_KEY_SPECTATE_PREVIOUS: return KEY_P;
		case CDemoViewerClient::CONTROL_KEY_ZOOM_IN: return KEY_KP_PLUS;
		case CDemoViewerClient::CONTROL_KEY_ZOOM_OUT: return KEY_KP_MINUS;
		case CDemoViewerClient::CONTROL_KEY_QUIT: return KEY_ESCAPE;
		default: dbg_assert_failed("Invalid control key");
		}
	}
} // namespace

void CDemoViewerClient::Configure(const char *pDemoPath, const char *pVideoPath, const CVideoExportSettings &Settings)
{
	str_copy(m_aDemoPath, pDemoPath);
	str_copy(m_aVideoPath, pVideoPath == nullptr ? "" : pVideoPath);
	m_Settings = Settings;
}

void CDemoViewerClient::SetPaused(bool Paused)
{
	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	if(Paused)
		Player.Pause();
	else
		Player.Unpause();
}

void CDemoViewerClient::SeekPercent(float Percent)
{
	DemoSource(m_DemoSessionId).DemoPlayer().SeekPercent(std::clamp(Percent, 0.0f, 1.0f));
}

void CDemoViewerClient::SeekTime(float Seconds)
{
	DemoSource(m_DemoSessionId).DemoPlayer().SeekTime(Seconds);
}

void CDemoViewerClient::SeekStart()
{
	DemoSource(m_DemoSessionId).DemoPlayer().SeekPercent(0.0f);
}

void CDemoViewerClient::SetSpeed(float Speed)
{
	DemoSource(m_DemoSessionId).DemoPlayer().SetSpeed(std::clamp(Speed, 0.01f, 100.0f));
}

bool CDemoViewerClient::RequestExport(const CVideoExportSettings &Settings)
{
	if(m_pVideo != nullptr || m_ExportRequested)
	{
		return false;
	}
	m_RequestedSettings = Settings;
	m_ExportRequested = true;
	m_ExportState = EExportState::RUNNING;
	return true;
}

bool CDemoViewerClient::StartExport(const CVideoExportSettings &Settings)
{
	if(m_pVideo != nullptr)
	{
		return false;
	}
	m_ExportState = EExportState::RUNNING;
	m_Settings = Settings;
	// An encoder takes whole macroblocks, so an odd size is refused rather than
	// rounded. A window is any size the user dragged it to, so the size that
	// comes from one is brought to an even one here instead of being turned
	// away.
	m_Settings.m_Width = std::clamp(Settings.m_Width, 2, 8192) & ~1;
	m_Settings.m_Height = std::clamp(Settings.m_Height, 2, 8192) & ~1;
	m_Settings.m_FPS = std::clamp(Settings.m_FPS, 1, 240);
	m_Settings.m_Crf = std::clamp(Settings.m_Crf, 0, 51);
	// The name of the demo, so that whoever ends up with the file knows what it
	// is a video of. It is written where the user's own files go and handed to
	// the browser from there.
	char aName[IO_MAX_PATH_LENGTH];
	fs_split_file_extension(fs_filename(m_aDemoPath), aName, sizeof(aName));
	str_format(m_aVideoPath, sizeof(m_aVideoPath), "videos/%s.mp4", aName);
	// A demo standing still is written frame after identical frame, which is
	// not what anybody means by exporting from here on.
	SetPaused(false);
	const char *pError = StartVideo();
	if(pError != nullptr)
	{
		log_error("videorecorder", "%s", pError);
		m_aError[0] = '\0';
		m_aVideoPath[0] = '\0';
		m_ExportState = EExportState::FAILED;
		return false;
	}
	return true;
}

void CDemoViewerClient::CancelExport()
{
	if(m_pVideo == nullptr)
	{
		return;
	}
	if(IVideo::Current() == m_pVideo.get())
	{
		m_pVideo->Cancel();
	}
	m_pVideo.reset();
	m_aVideoPath[0] = '\0';
	m_ExportState = EExportState::IDLE;
}

void CDemoViewerClient::FinishExport()
{
	// The demo player has closed it already, which is what wrote the last of
	// it out and handed it over. Anything else is closed here.
	if(IVideo::Current() == m_pVideo.get())
	{
		m_pVideo->Stop();
	}
	const bool Failed = m_pVideo->Status().m_HasError;
	m_pVideo.reset();
	if(Failed)
	{
		m_aError[0] = '\0';
	}
	log_info("videorecorder", Failed ? "Export failed" : "Export completed");
	m_aVideoPath[0] = '\0';
	m_ExportState = Failed ? EExportState::FAILED : EExportState::FINISHED;
	// Writing the demo to a file played it to its end, and that ended the
	// session it was played from. Whoever asked for the video is still sitting
	// in front of it, so the demo is put back on, from the start and standing
	// still.
	const char *pError = PlayDemo();
	if(pError == nullptr)
	{
		SetPaused(true);
	}
	else
	{
		log_error("client", "%s", pError);
	}
}

bool CDemoViewerClient::Paused() const
{
	return DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_Paused;
}

float CDemoViewerClient::Progress() const
{
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
	const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
	if(TotalTicks == 0)
		return 0.0f;
	return std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks) / (float)TotalTicks;
}

float CDemoViewerClient::Speed() const
{
	return DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_Speed;
}

float CDemoViewerClient::Length() const
{
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
	return std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0) / (float)SERVER_TICK_SPEED;
}

bool CDemoViewerClient::Exporting() const
{
	return m_pVideo != nullptr && IVideo::Current() == m_pVideo.get();
}

void CDemoViewerClient::UpdateAndSwap()
{
	Graphics()->Swap();
}

void CDemoViewerClient::DemoPlayer_CancelActiveRender()
{
	if(m_pVideo != nullptr && IVideo::Current() == m_pVideo.get())
	{
		m_pVideo->Cancel();
	}
	m_pVideo.reset();
}

bool CDemoViewerClient::KeyPressed(EControlKey ControlKey, bool Repeats)
{
	const bool Pressed = Input()->KeyIsPressed(ControlKeyCode(ControlKey));
	const bool WasPressed = m_aKeyWasPressed[ControlKey];
	m_aKeyWasPressed[ControlKey] = Pressed;
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Pressed && !WasPressed)
	{
		m_aKeyRepeatTime[ControlKey] = Now + KEY_REPEAT_DELAY;
		return true;
	}
	if(Pressed && Repeats && Now >= m_aKeyRepeatTime[ControlKey])
	{
		m_aKeyRepeatTime[ControlKey] = Now + KEY_REPEAT_INTERVAL;
		return true;
	}
	return false;
}

bool CDemoViewerClient::HandleInput()
{
	// Nobody is at the keyboard of a window that is not there.
	if(m_pInput == nullptr)
	{
		return true;
	}
	if(Input()->Update())
	{
		return false;
	}

	char aDroppedFile[IO_MAX_PATH_LENGTH];
	if(Input()->GetDropFile(aDroppedFile, sizeof(aDroppedFile)))
	{
		// Whatever is playing gives way to what was just dropped, and a viewer
		// that was waiting for a demo has one now.
		if(SessionState(m_DemoSessionId) == ESessionState::READY)
		{
			StopDemoSession(nullptr);
		}
		m_aError[0] = '\0';
		const char *pLoadError = DemoPlayer_Play(aDroppedFile, IStorage::TYPE_ALL_OR_ABSOLUTE);
		if(pLoadError != nullptr)
		{
			log_error("client", "%s", pLoadError);
			m_aError[0] = '\0';
		}
	}

	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	if(KeyPressed(CONTROL_KEY_QUIT, false))
	{
		return false;
	}
	if(KeyPressed(CONTROL_KEY_PAUSE, false))
	{
		if(Player.BaseInfo()->m_Paused)
			Player.Unpause();
		else
			Player.Pause();
	}
	if(KeyPressed(CONTROL_KEY_SEEK_BACK, true))
	{
		Player.SeekTime(-SEEK_SECONDS);
	}
	if(KeyPressed(CONTROL_KEY_SEEK_FORWARD, true))
	{
		Player.SeekTime(SEEK_SECONDS);
	}
	if(KeyPressed(CONTROL_KEY_SPEED_UP, false))
	{
		Player.AdjustSpeedIndex(1);
	}
	if(KeyPressed(CONTROL_KEY_SPEED_DOWN, false))
	{
		Player.AdjustSpeedIndex(-1);
	}
	if(KeyPressed(CONTROL_KEY_RESTART, false))
	{
		Player.SeekPercent(0.0f);
	}
	if(KeyPressed(CONTROL_KEY_FREE_VIEW, false))
	{
		// Back to whoever the demo was recorded by when there is one, so that
		// the same key both leaves a player and comes back to them.
		SetSpectate(Spectating() == SPEC_FREEVIEW ? SPEC_FOLLOW : SPEC_FREEVIEW);
	}
	if(KeyPressed(CONTROL_KEY_SPECTATE_NEXT, false))
	{
		SpectateStep(1);
	}
	if(KeyPressed(CONTROL_KEY_SPECTATE_PREVIOUS, false))
	{
		SpectateStep(-1);
	}
	if(KeyPressed(CONTROL_KEY_ZOOM_IN, true) || Input()->KeyPress(KEY_MOUSE_WHEEL_UP))
	{
		ScaleZoom(CCamera::ZoomStepsToValue(ZOOM_STEP));
	}
	if(KeyPressed(CONTROL_KEY_ZOOM_OUT, true) || Input()->KeyPress(KEY_MOUSE_WHEEL_DOWN))
	{
		ScaleZoom(CCamera::ZoomStepsToValue(-ZOOM_STEP));
	}

	// Dragging moves the free view, the way a map is dragged. In the pixels
	// that are drawn, not the ones the window is measured in, so that on a
	// screen with more of the former the world keeps up with the pointer. A
	// press that landed on the bar belongs to the bar.
	const vec2 MousePos = Input()->NativeMousePos() * Graphics()->ScreenHiDPIScale();
	if(Input()->NativeMousePressed(1) && !m_Controls.Hovered())
	{
		if(m_Dragging)
		{
			MoveFreeView((m_LastMousePos - MousePos) * WorldPerPixel());
		}
		m_Dragging = true;
	}
	else
	{
		m_Dragging = false;
	}
	m_LastMousePos = MousePos;

	// Everything this frame brought has been read. Nothing carries over to the
	// next one, and the events pile up until they are let go of.
	Input()->Clear();
	return true;
}

const char *CDemoViewerClient::Players()
{
	// Written by the JSON writer rather than put together by hand: a name is
	// whatever somebody typed, quotation marks and all.
	CJsonStringWriter Writer;
	Writer.BeginArray();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const char *pName = SpectatePlayerName(ClientId);
		if(pName == nullptr)
		{
			continue;
		}
		Writer.BeginObject();
		Writer.WriteAttribute("id");
		Writer.WriteIntValue(ClientId);
		Writer.WriteAttribute("name");
		Writer.WriteStrValue(pName);
		Writer.EndObject();
	}
	Writer.EndArray();
	m_Players = Writer.GetOutputString();
	return m_Players.c_str();
}

void CDemoViewerClient::RenderControls()
{
	// Nothing to steer, and nobody to steer it: a window that was never opened
	// has no pointer over it, and a demo that is being written to a file is
	// not being watched.
	if(!m_ShowControls || m_pInput == nullptr || Exporting() || SessionState(m_DemoSessionId) != ESessionState::READY)
	{
		return;
	}
	const float Total = Length();
	if(Total <= 0.0f)
	{
		return;
	}

	const auto &&FormatTime = [](char *pBuffer, size_t Size, float Seconds) {
		const int Whole = std::max((int)(Seconds + 0.5f), 0);
		str_format(pBuffer, Size, "%d:%02d", Whole / 60, Whole % 60);
	};
	char aElapsed[16];
	char aLength[16];
	FormatTime(aElapsed, sizeof(aElapsed), Progress() * Total);
	FormatTime(aLength, sizeof(aLength), Total);
	char aTime[40];
	str_format(aTime, sizeof(aTime), "%s / %s", aElapsed, aLength);
	char aSpeed[16];
	str_format(aSpeed, sizeof(aSpeed), "%.2fx", Speed());
	// Who is being watched, cut short: a bar as wide as the longest name
	// somebody chose is a bar that covers the demo.
	char aSpectating[20];
	const char *pSpectating = SpectatePlayerName(Spectating());
	if(pSpectating != nullptr)
		str_copy(aSpectating, pSpectating);
	else
		str_copy(aSpectating, Spectating() == SPEC_FOLLOW ? "Follow" : "Free view");

	// Left to right, the way a video player has it: what it is doing, how fast,
	// how far along, and off on the other side what is being watched.
	enum
	{
		ITEM_SEEK,
		ITEM_PLAY,
		ITEM_RESTART,
		ITEM_SLOWER,
		ITEM_SPEED,
		ITEM_FASTER,
		ITEM_TIME,
		ITEM_SPACER,
		ITEM_SPECTATE,
		NUM_ITEMS,
	};
	CViewerControls::SItem aItems[NUM_ITEMS];
	aItems[ITEM_SEEK].m_Type = CViewerControls::EItem::SLIDER;
	aItems[ITEM_SEEK].m_Value = Progress();
	aItems[ITEM_PLAY].m_Icon = Paused() ? CViewerControls::EIcon::PLAY : CViewerControls::EIcon::PAUSE;
	aItems[ITEM_RESTART].m_Icon = CViewerControls::EIcon::RESTART;
	aItems[ITEM_SLOWER].m_Icon = CViewerControls::EIcon::MINUS;
	aItems[ITEM_SLOWER].m_Optional = true;
	aItems[ITEM_SPEED].m_Type = CViewerControls::EItem::TEXT;
	aItems[ITEM_SPEED].m_pText = aSpeed;
	aItems[ITEM_SPEED].m_Width = 56.0f;
	aItems[ITEM_SPEED].m_Optional = true;
	aItems[ITEM_FASTER].m_Icon = CViewerControls::EIcon::PLUS;
	aItems[ITEM_FASTER].m_Optional = true;
	aItems[ITEM_TIME].m_Type = CViewerControls::EItem::TEXT;
	aItems[ITEM_TIME].m_pText = aTime;
	aItems[ITEM_SPACER].m_Type = CViewerControls::EItem::SPACER;
	aItems[ITEM_SPECTATE].m_Icon = CViewerControls::EIcon::EYE;
	aItems[ITEM_SPECTATE].m_pText = aSpectating;

	CViewerControls::SInput Input;
	Input.m_MousePos = m_pInput->NativeMousePos();
	Input.m_MousePressed = m_pInput->NativeMousePressed(1);
	// From the events rather than from the state, because a frame can take
	// longer than a tap does and the state alone would never see it.
	Input.m_MouseClicked = m_pInput->KeyPress(KEY_MOUSE_1);
	Input.m_KeyPressed = std::any_of(m_aKeyWasPressed.begin(), m_aKeyWasPressed.end(), [](bool Pressed) { return Pressed; });

	float SeekTo = 0.0f;
	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	switch(m_Controls.Render(aItems, NUM_ITEMS, Input, &SeekTo))
	{
	case ITEM_PLAY:
		SetPaused(!Paused());
		break;
	case ITEM_SEEK:
		Player.SeekPercent(SeekTo);
		break;
	case ITEM_SLOWER:
		Player.AdjustSpeedIndex(-1);
		break;
	case ITEM_FASTER:
		Player.AdjustSpeedIndex(1);
		break;
	case ITEM_RESTART:
		SeekStart();
		break;
	case ITEM_SPECTATE:
		SpectateStep(1);
		break;
	default:
		break;
	}
}

void CDemoViewerClient::RenderWindowFrame()
{
	const int64_t Now = time_get();
	m_RenderFrameTime = (Now - m_LastRenderTime) / (float)time_freq();
	m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;
	m_LastRenderTime = Now;

	GameClient()->OnRenderPrepare();
	GameClient()->OnRender();
	GameClient()->OnRenderFinalize();
	// Over everything else, because it is what is in front of the demo, and
	// before the frame goes out.
	RenderControls();
	Graphics()->Swap();
	// The clock everything that moves by itself runs on: the camera easing
	// towards a player it was just put on, and the zoom easing towards what
	// the wheel asked for. Without it they are handed the same instant every
	// frame and never arrive.
	m_LocalTime = (time_get() - m_LocalStartTime) / (float)time_freq();
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}

void CDemoViewerClient::Run()
{
	m_LocalStartTime = m_GlobalStartTime = time_get();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pDemoViewer = this;
#endif

	// The same escape hatch the client has: a machine with no display can still
	// draw, into a surface without a window. It is the only way to try the
	// viewer where there is nothing to show it on, and it is what an export
	// from here wants anyway. Nobody is at the keyboard of a window that is not
	// there, so the keys stay off as well.
	m_Surfaceless = std::getenv("GFX_SURFACELESS") != nullptr;
	if(!InitGraphics(m_Surfaceless ? CreateOffscreenGraphicsWindow() : CreateSdlGraphicsWindow()))
	{
		m_ExitCode = 1;
		return;
	}

	if(!m_Surfaceless)
	{
		m_pInput = Kernel()->RequestInterface<IEngineInput>();
		m_pInput->Init();
		// Nobody aims here. The input takes the pointer when it starts, because
		// a game wants it; a viewer wants it left where it is, so that it can be
		// put on a button and taken out of the window again.
		m_pInput->MouseModeAbsolute();
	}
	GameClient()->InitializeLanguage();
	if(Sound()->Init() != 0)
	{
		log_warn("client", "The audio device could not be initialised, watching without sound.");
	}
	InitVideoBackend();

	InitTextRender();
	m_Controls.Init(Graphics(), TextRender());
	Graphics()->AddWindowResizeListener([this] { OnWindowResize(); });
	GameClient()->OnInit();

	// A viewer opens with whatever it was given, which may be nothing: there it
	// waits for a demo to be dropped on it, the way the map viewer waits for a
	// map. A surface without a window has nobody to drop one, so there a demo is
	// the only reason to be running at all.
	const char *pError = nullptr;
	if(m_aDemoPath[0] != '\0')
	{
		pError = PlayDemo();
		if(pError == nullptr && m_aVideoPath[0] != '\0')
		{
			pError = StartVideo();
		}
	}
	else if(m_Surfaceless)
	{
		pError = "No demo was given, and a surface without a window has nowhere to drop one.";
	}
	if(pError != nullptr)
	{
		log_error("client", "%s", pError);
		m_ExitCode = 1;
	}
	else
	{
		while(m_State != IClient::STATE_QUITTING)
		{
			if(!HandleInput())
			{
				break;
			}
			// What a page asked for, done here where the stack belongs to the
			// viewer: both of these wait for the browser, and what waits gets
			// its stack unwound underneath it.
			if(m_CancelRequested)
			{
				m_CancelRequested = false;
				m_ExportRequested = false;
				CancelExport();
			}
			if(m_ExportRequested)
			{
				m_ExportRequested = false;
				StartExport(m_RequestedSettings);
			}
			// A demo that has run out pauses on its last frame, which is what
			// somebody watching wants: they can seek back into it. Nobody
			// watches a surface without a window, so there it is the end.
			if(m_Surfaceless && DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_CurrentTick >= DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_LastTick)
			{
				break;
			}
			set_new_tick();
			m_SessionManager.Update();
			// A player asked for by name is only found once the demo has named
			// them, which is a snapshot or two in.
			UpdatePendingSpectate();
			Sound()->Update();
			GameClient()->OnUpdate();
			// An export takes every frame in its own time, so while one runs
			// it decides what is drawn and the window only shows the result.
			if(m_pVideo != nullptr && IVideo::Current() == m_pVideo.get())
			{
				RenderExportFrame();
			}
			else if(m_pVideo != nullptr)
			{
				// The demo player closes the video itself when the demo runs
				// out under it, which is what says the export is complete:
				// what was being written is no longer the video being written.
				FinishExport();
			}
			else
			{
				RenderWindowFrame();
				thread_sleep_until_next_frame(m_NextFrameTime, g_Config.m_ClRefreshRate);
			}
		}
		if(m_aError[0] != '\0')
		{
			log_error("client", "%s", m_aError);
			m_ExitCode = 1;
		}
	}

	SetState(IClient::STATE_QUITTING);
	if(m_pVideo != nullptr)
	{
		if(IVideo::Current() == m_pVideo.get())
			m_pVideo->Stop();
		m_pVideo.reset();
	}
	if(SessionState(m_DemoSessionId) != ESessionState::OFFLINE)
	{
		m_SessionManager.Close(m_DemoSessionId);
		m_SessionManager.Update(m_DemoSessionId);
	}
	// The jobs run on their own threads and load assets into the graphics and
	// the sound, so none of the two may be shut down while one is still going.
	Engine()->ShutdownJobs();
	GameClient()->OnShutdown();
	// The text render gives its textures back, which needs the graphics.
	TextRender()->Shutdown();
	Graphics()->Shutdown();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pDemoViewer = nullptr;
#endif
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// What the controls beside the canvas call. A browser shows no key bindings
// and nobody would find them, so the page has buttons and a slider and these
// are the other end of them. Every one of them is safe to call before the
// demo is playing or after it has stopped.
extern "C" {

EMSCRIPTEN_KEEPALIVE void DemoViewerSetPaused(int Paused)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetPaused(Paused != 0);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekPercent(float Percent)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SeekPercent(Percent);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekTime(float Seconds)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SeekTime(Seconds);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpeed(float Speed)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetSpeed(Speed);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekStart()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SeekStart();
}

// Who the demo is watched over the shoulder of. -1 is the free view, -2 is
// whoever recorded it, and everything from 0 up is one of its players.
EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpectate(int SpectatorId)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetSpectate(SpectatorId);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpectateName(const char *pName)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetSpectateName(pName == nullptr ? "" : pName);
}

EMSCRIPTEN_KEEPALIVE int DemoViewerSpectating()
{
	return g_pDemoViewer == nullptr ? SPEC_FREEVIEW : g_pDemoViewer->Spectating();
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSpectateStep(int Direction)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SpectateStep(Direction);
}

// The players the demo has named so far, as JSON. What is pointed at stays
// there until this is called again, which is all a page reading it out needs.
EMSCRIPTEN_KEEPALIVE const char *DemoViewerPlayers()
{
	return g_pDemoViewer == nullptr ? "[]" : g_pDemoViewer->Players();
}

// How much of the world is in the window. A page has no wheel over the canvas
// while the pointer is on a button of its own, so it can ask for this instead.
EMSCRIPTEN_KEEPALIVE void DemoViewerZoomBy(float Factor)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->ScaleZoom(Factor);
}

EMSCRIPTEN_KEEPALIVE float DemoViewerZoom()
{
	return g_pDemoViewer == nullptr ? 1.0f : g_pDemoViewer->Zoom();
}

EMSCRIPTEN_KEEPALIVE void DemoViewerQuit()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->Quit();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerStartExport(int Width, int Height, int Fps, int Audio, int Crf, const char *pCodec, int Hud, int Chat)
{
	if(g_pDemoViewer == nullptr)
	{
		return 0;
	}
	// This only asks. Whether it worked is what `DemoViewerExportState` says,
	// a moment later - a browser is asked for an encoder before there is an
	// answer, and asking it takes a turn of its event loop.
	CVideoExportSettings Settings;
	Settings.m_Width = Width;
	Settings.m_Height = Height;
	Settings.m_FPS = Fps;
	Settings.m_Audio = Audio != 0;
	Settings.m_Crf = Crf;
	str_copy(Settings.m_aVideoCodec, pCodec == nullptr ? "" : pCodec);
	Settings.m_ShowHud = Hud != 0;
	Settings.m_ShowChat = Chat != 0;
	return g_pDemoViewer->RequestExport(Settings) ? 1 : 0;
}

// Whether the viewer draws its own controls. A page with a bar of its own
// beside the canvas says so and gets a bare picture.
EMSCRIPTEN_KEEPALIVE void DemoViewerSetControls(int Show)
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->SetShowControls(Show != 0);
}

EMSCRIPTEN_KEEPALIVE int DemoViewerControls()
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->ShowControls() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void DemoViewerCancelExport()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->RequestCancelExport();
}

// 0 while nothing was ever asked for, 1 while a video is being written, 2 when
// the last one was handed over and 3 when it failed.
EMSCRIPTEN_KEEPALIVE int DemoViewerExportState()
{
	return g_pDemoViewer == nullptr ? 0 : (int)g_pDemoViewer->ExportState();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerPaused()
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->Paused() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float DemoViewerProgress()
{
	return g_pDemoViewer == nullptr ? 0.0f : g_pDemoViewer->Progress();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerSpeed()
{
	return g_pDemoViewer == nullptr ? 1.0f : g_pDemoViewer->Speed();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerLength()
{
	return g_pDemoViewer == nullptr ? 0.0f : g_pDemoViewer->Length();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerExporting()
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->Exporting() ? 1 : 0;
}
}
#endif
