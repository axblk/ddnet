/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_viewer_client.h"

#include "window_sdl.h"

#include <base/log.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include <algorithm>
#include <cstdlib>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

namespace
{
	// How far one press of a seek key moves.
	constexpr float SEEK_SECONDS = 5.0f;

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// The one viewer of the page, for the controls beside the canvas.
	CDemoViewerClient *gs_pDemoViewer = nullptr;
#endif
} // namespace

std::optional<int> CDemoViewerClient::ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments)
{
	// The demo is the first argument that names one, everything else goes to
	// the console.
	vArguments.push_back(ppArguments[0]);
	for(int Index = 1; Index < ArgumentCount; Index++)
	{
		const char *pArgument = ppArguments[Index];
		if(str_comp(pArgument, "--help") == 0 || str_comp(pArgument, "-h") == 0)
		{
			log_info("client", "Usage: ddnet-demo-viewer <demo> [console commands]");
			log_info("client", "Space pauses, left and right seek, up and down change the speed, Home");
			log_info("client", "starts over and Escape closes the window.");
			return 0;
		}
		if(m_aDemoPath[0] == '\0' && str_endswith(pArgument, ".demo"))
			str_copy(m_aDemoPath, pArgument);
		else
			vArguments.push_back(pArgument);
	}
	if(m_aDemoPath[0] == '\0')
	{
		log_error("client", "Usage: ddnet-demo-viewer <demo> [console commands]");
		return -1;
	}
	ArgumentCount = static_cast<int>(vArguments.size());
	ppArguments = vArguments.data();
	return std::nullopt;
}

bool CDemoViewerClient::Configure()
{
#if defined(CONF_VIDEORECORDER)
	m_Settings = CCommandLineVideoExport::Settings();
#endif
	return true;
}

void CDemoViewerClient::UpdateAndSwap()
{
	Graphics()->Swap();
}

void CDemoViewerClient::SetPaused(bool Paused)
{
	if(Paused)
		DemoPlayer().Pause();
	else
		DemoPlayer().Unpause();
}

void CDemoViewerClient::SeekPercent(float Percent)
{
	DemoPlayer().SeekPercent(std::clamp(Percent, 0.0f, 1.0f));
}

void CDemoViewerClient::SeekTime(float Seconds)
{
	DemoPlayer().SeekTime(Seconds);
}

void CDemoViewerClient::SetSpeed(float Speed)
{
	DemoPlayer().SetSpeed(std::clamp(Speed, 0.01f, 100.0f));
}

bool CDemoViewerClient::Paused() const
{
	return DemoPlayer().BaseInfo()->m_Paused;
}

float CDemoViewerClient::Progress() const
{
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
	if(TotalTicks == 0)
		return 0.0f;
	return std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks) / (float)TotalTicks;
}

float CDemoViewerClient::Speed() const
{
	return DemoPlayer().BaseInfo()->m_Speed;
}

float CDemoViewerClient::Length() const
{
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	return std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0) / (float)SERVER_TICK_SPEED;
}

bool CDemoViewerClient::HandleInput()
{
	if(m_pInput == nullptr)
		return true;
	if(m_pInput->Update())
		return false;

	bool Quit = false;
	CDemoPlayer &Player = DemoPlayer();
	m_pInput->ConsumeEvents([&](const IInput::CEvent &Event) {
		if((Event.m_Flags & IInput::FLAG_PRESS) == 0)
			return;
		// Only seeking repeats while a key is held down.
		const bool Repeated = (Event.m_Flags & IInput::FLAG_REPEAT) != 0;
		switch(Event.m_Key)
		{
		case KEY_LEFT: Player.SeekTime(-SEEK_SECONDS); break;
		case KEY_RIGHT: Player.SeekTime(SEEK_SECONDS); break;
		default:
			if(Repeated)
				break;
			switch(Event.m_Key)
			{
			case KEY_ESCAPE: Quit = true; break;
			case KEY_SPACE: SetPaused(!Paused()); break;
			case KEY_UP: Player.AdjustSpeedIndex(1); break;
			case KEY_DOWN: Player.AdjustSpeedIndex(-1); break;
			case KEY_HOME: Player.SeekPercent(0.0f); break;
			default: break;
			}
		}
	});
	return !Quit;
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

int CDemoViewerClient::Run()
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	gs_pDemoViewer = this;
#endif
	// A machine without a display can still draw into a surface without a
	// window, the same way the client can. Nobody is at its keyboard.
	m_Surfaceless = std::getenv("GFX_SURFACELESS") != nullptr;
	if(!m_Surfaceless)
		m_pInput = Kernel()->RequestInterface<IEngineInput>();
	int ExitCode = 1;
	if(InitGame(m_Surfaceless ? CreateOffscreenGraphicsWindow() : CreateSdlGraphicsWindow(), m_pInput))
	{
		if(const char *pError = PlayDemo())
			log_error("client", "%s", pError);
		else
		{
			while(m_State != IClient::STATE_QUITTING && SessionState(m_DemoSessionId) == ESessionState::READY)
			{
				if(!HandleInput())
					break;
				// A demo that has run out pauses on its last frame, so the
				// viewer can seek back into it. Without a window it is the end.
				if(m_Surfaceless && DemoPlayer().BaseInfo()->m_CurrentTick >= DemoPlayer().BaseInfo()->m_LastTick)
					break;
				Update();
				RenderWindowFrame();
				thread_sleep_until_next_frame(m_NextFrameTime, g_Config.m_ClRefreshRate);
			}
			if(m_aError[0] != '\0')
				log_error("client", "%s", m_aError);
			else
				ExitCode = 0;
		}
	}
	ShutdownGame();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	gs_pDemoViewer = nullptr;
#endif
	return ExitCode;
}

int main(int argc, const char **argv)
{
	return DemoClientMain(new CDemoViewerClient, argc, argv);
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// What the controls beside the canvas call. Safe to call before the demo plays
// and after it stopped.
extern "C" {

EMSCRIPTEN_KEEPALIVE void DemoViewerSetPaused(int Paused)
{
	if(gs_pDemoViewer != nullptr)
		gs_pDemoViewer->SetPaused(Paused != 0);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekPercent(float Percent)
{
	if(gs_pDemoViewer != nullptr)
		gs_pDemoViewer->SeekPercent(Percent);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSeekTime(float Seconds)
{
	if(gs_pDemoViewer != nullptr)
		gs_pDemoViewer->SeekTime(Seconds);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerSetSpeed(float Speed)
{
	if(gs_pDemoViewer != nullptr)
		gs_pDemoViewer->SetSpeed(Speed);
}

EMSCRIPTEN_KEEPALIVE void DemoViewerQuit()
{
	if(gs_pDemoViewer != nullptr)
		gs_pDemoViewer->Quit();
}

EMSCRIPTEN_KEEPALIVE int DemoViewerPaused()
{
	return gs_pDemoViewer != nullptr && gs_pDemoViewer->Paused() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float DemoViewerProgress()
{
	return gs_pDemoViewer == nullptr ? 0.0f : gs_pDemoViewer->Progress();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerSpeed()
{
	return gs_pDemoViewer == nullptr ? 1.0f : gs_pDemoViewer->Speed();
}

EMSCRIPTEN_KEEPALIVE float DemoViewerLength()
{
	return gs_pDemoViewer == nullptr ? 0.0f : gs_pDemoViewer->Length();
}
}
#endif
