/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_render_client.h"

#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cinttypes>

bool CDemoRenderClient::Configure(const CCommandLineVideoExport &Export)
{
	m_Settings = Export.Settings();
	str_copy(m_aDemoPath, Export.m_aDemoPath);
	str_copy(m_aVideoPath, Export.m_aVideoPath);
	if(!str_endswith(m_aVideoPath, ".mp4"))
		str_append(m_aVideoPath, ".mp4");
	// The finished file is moved into place without replacing anything, so an
	// output that already exists would only be found out about after the demo
	// was decoded for minutes.
	if(Storage()->FileExists(m_aVideoPath, IStorage::TYPE_SAVE_OR_ABSOLUTE))
	{
		log_error("videorecorder", "Output file '%s' already exists.", m_aVideoPath);
		return false;
	}
	m_ExitCode = 1;
	return true;
}

void CDemoRenderClient::OnExportFrame()
{
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Now - m_LastProgressLog < std::chrono::seconds(1))
		return;
	m_LastProgressLog = Now;
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
	const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
	const int CurrentTicks = std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks);
	const float Progress = TotalTicks == 0 ? 0.0f : CurrentTicks / static_cast<float>(TotalTicks);
	const CVideoExportStatus Status = m_pVideo->Status();
	log_info("videorecorder", "Rendering %.1f%% (%" PRIu64 " / %" PRIu64 " frames encoded, %.0f per second)",
		Progress * 100.0f, Status.m_EncodedFrames, Status.m_SubmittedFrames, Status.m_FramesPerSecond);
}

void CDemoRenderClient::Run()
{
	m_LocalStartTime = m_GlobalStartTime = time_get();

	// There is no window system here, so this is the window-less surface the
	// export draws into.
	if(!InitGraphics(CreateOffscreenGraphicsWindow()))
	{
		m_ExitCode = 1;
		return;
	}

	GameClient()->InitializeLanguage();
	if(Sound()->Init() != 0 && m_Settings.m_Audio)
	{
		log_warn("videorecorder", "The audio device could not be initialised, rendering without sound.");
		m_Settings.m_Audio = false;
	}
	InitVideoBackend();

	InitTextRender();
	Graphics()->AddWindowResizeListener([this] { OnWindowResize(); });
	GameClient()->OnInit();

	const char *pError = PlayDemo();
	if(pError == nullptr)
		pError = StartVideo();
	if(pError != nullptr)
	{
		log_error("videorecorder", "%s", pError);
		m_ExitCode = 1;
	}
	else
	{
		// The export writes its file as it goes, so an interrupt has to reach the
		// encoder: it throws the unfinished file away instead of leaving it
		// behind. There is nobody at a keyboard here otherwise, so this is the
		// only way out of the render.
		CatchVideoExportInterrupt();
		while(m_State != IClient::STATE_QUITTING && SessionState(m_DemoSessionId) == ESessionState::READY)
		{
			if(VideoExportInterrupted())
			{
				// Cancelling throws the unfinished file away, and ending the
				// demo takes the loop through the same exit an export that ran
				// out of demo takes.
				if(m_pVideo != nullptr && IVideo::Current() == m_pVideo.get())
					m_pVideo->Cancel();
				StopDemoSession("Video rendering interrupted.");
				continue;
			}
			set_new_tick();
			m_SessionManager.Update();
			Sound()->Update();
			GameClient()->OnUpdate();
			RenderExportFrame();
		}
		if(m_aError[0] != '\0')
		{
			log_error("videorecorder", "%s", m_aError);
			m_ExitCode = 1;
		}
		else
		{
			m_ExitCode = 0;
			log_info("videorecorder", "Export completed");
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
}
