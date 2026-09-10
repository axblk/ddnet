/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_render_client.h"

#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics_window.h>
#include <engine/shared/video.h>
#include <engine/storage.h>

#include <algorithm>
#include <cinttypes>

std::optional<int> CDemoRenderClient::ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments)
{
	CCommandLineVideoExport VideoExport;
	if(!VideoExport.ParseArguments(ArgumentCount, ppArguments, vArguments, "ddnet-demo-render", true))
		return -1;
	if(VideoExport.m_ListCodecs)
	{
		for(const CVideoEncoder &Encoder : VideoEncoders())
			log_info("videorecorder", "%-20s %s", Encoder.m_aName[0] == '\0' ? "(default)" : Encoder.m_aName, Encoder.m_aDisplayName);
	}
	if(VideoExport.m_Help || VideoExport.m_ListCodecs)
		return 0;
	if(!VideoExport.m_Export)
	{
		PrintVideoExportUsage("ddnet-demo-render");
		return -1;
	}
	str_copy(m_aDemoPath, VideoExport.m_aDemoPath);
	str_copy(m_aVideoPath, VideoExport.m_aVideoPath);
	if(!str_endswith(m_aVideoPath, ".mp4"))
		str_append(m_aVideoPath, ".mp4");
	return std::nullopt;
}

bool CDemoRenderClient::Configure()
{
	m_Settings = CCommandLineVideoExport::Settings();
	// The finished file is moved into place without replacing anything, so an
	// existing output would only be found out about after the whole render.
	if(Storage()->FileExists(m_aVideoPath, IStorage::TYPE_SAVE_OR_ABSOLUTE))
	{
		log_error("videorecorder", "Output file '%s' already exists.", m_aVideoPath);
		return false;
	}
	return true;
}

void CDemoRenderClient::OnExportFrame()
{
	const std::chrono::nanoseconds Now = time_get_nanoseconds();
	if(Now - m_LastProgressLog < std::chrono::seconds(1))
		return;
	m_LastProgressLog = Now;
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	const int TotalTicks = std::max(pInfo->m_LastTick - pInfo->m_FirstTick, 0);
	const int CurrentTicks = std::clamp(pInfo->m_CurrentTick - pInfo->m_FirstTick, 0, TotalTicks);
	const float Progress = TotalTicks == 0 ? 0.0f : CurrentTicks / static_cast<float>(TotalTicks);
	const CVideoExportStatus Status = m_pVideo->Status();
	log_info("videorecorder", "Rendering %.1f%% (%" PRIu64 " / %" PRIu64 " frames encoded, %.0f per second)",
		Progress * 100.0f, Status.m_EncodedFrames, Status.m_SubmittedFrames, Status.m_FramesPerSecond);
}

int CDemoRenderClient::Run()
{
	int ExitCode = 1;
	if(InitGame(CreateOffscreenGraphicsWindow(), nullptr))
	{
		const char *pError = PlayDemo();
		if(pError == nullptr)
			pError = StartVideo();
		if(pError != nullptr)
			log_error("videorecorder", "%s", pError);
		else
		{
			// Nobody is at a keyboard here, so an interrupt is the only way out.
			// It has to reach the encoder, which removes the unfinished file.
			CatchVideoExportInterrupt();
			while(m_State != IClient::STATE_QUITTING && SessionState(m_DemoSessionId) == ESessionState::READY)
			{
				if(VideoExportInterrupted())
				{
					if(Exporting())
						m_pVideo->Cancel();
					StopDemoSession("Video rendering interrupted.");
					continue;
				}
				Update();
				RenderExportFrame();
			}
			if(m_aError[0] != '\0')
				log_error("videorecorder", "%s", m_aError);
			else
			{
				ExitCode = 0;
				log_info("videorecorder", "Export completed");
			}
		}
	}
	ShutdownGame();
	return ExitCode;
}

int main(int argc, const char **argv)
{
	return DemoClientMain(new CDemoRenderClient, argc, argv);
}
