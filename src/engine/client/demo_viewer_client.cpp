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

bool CDemoViewerClient::StartExport(int Width, int Height, int Fps, bool Audio)
{
	if(m_pVideo != nullptr)
	{
		return false;
	}
	m_Settings.m_Width = std::max(Width, 1);
	m_Settings.m_Height = std::max(Height, 1);
	m_Settings.m_FPS = std::clamp(Fps, 1, 240);
	m_Settings.m_Audio = Audio;
	// The name of the demo, so that whoever ends up with the file knows what it
	// is a video of. It is written where the user's own files go and handed to
	// the browser from there.
	char aName[IO_MAX_PATH_LENGTH];
	fs_split_file_extension(fs_filename(m_aDemoPath), aName, sizeof(aName));
	str_format(m_aVideoPath, sizeof(m_aVideoPath), "videos/%s.mp4", aName);
	const char *pError = StartVideo();
	if(pError != nullptr)
	{
		log_error("videorecorder", "%s", pError);
		m_aError[0] = '\0';
		m_aVideoPath[0] = '\0';
		return false;
	}
	return true;
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
	// Everything this frame brought has been read. Nothing carries over to the
	// next one, and the events pile up until they are let go of.
	Input()->Clear();
	return true;
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
	Graphics()->Swap();
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
			// A demo that has run out pauses on its last frame, which is what
			// somebody watching wants: they can seek back into it. Nobody
			// watches a surface without a window, so there it is the end.
			if(m_Surfaceless && DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_CurrentTick >= DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo()->m_LastTick)
			{
				break;
			}
			set_new_tick();
			m_SessionManager.Update();
			Sound()->Update();
			GameClient()->OnUpdate();
			// An export takes every frame in its own time, so while one runs
			// it decides what is drawn and the window only shows the result.
			if(m_pVideo != nullptr && IVideo::Current() == m_pVideo.get())
			{
				RenderExportFrame();
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

EMSCRIPTEN_KEEPALIVE void DemoViewerQuit()
{
	if(g_pDemoViewer != nullptr)
		g_pDemoViewer->Quit();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerStartExport(int Width, int Height, int Fps, int Audio)
{
	return g_pDemoViewer != nullptr && g_pDemoViewer->StartExport(Width, Height, Fps, Audio != 0) ? 1 : 0;
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
