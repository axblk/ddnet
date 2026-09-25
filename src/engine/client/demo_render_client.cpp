/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_render_client.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/graphics_window.h>
#include <engine/shared/video.h>
#include <engine/sound.h>
#include <engine/storage.h>

#include <algorithm>
#include <cinttypes>
#include <cstdlib>

#if defined(CONF_WEB_PLATFORM)
#include "web/audio_web.h"
#include "web/window_web.h"

#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <memory>
#endif

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>

// How far the render has come, for a page, with the numbers the log carries.
// clang-format off
EM_JS(void, BrowserRenderProgress, (float Progress, double EncodedFrames, double SubmittedFrames, float FramesPerSecond), {
	if(typeof Module.ddnetRenderProgress === 'function')
		Module.ddnetRenderProgress({progress: Progress, encodedFrames: EncodedFrames, submittedFrames: SubmittedFrames, framesPerSecond: FramesPerSecond});
});
// clang-format on

namespace
{
	struct SRenderProgress
	{
		float m_Progress;
		double m_EncodedFrames;
		double m_SubmittedFrames;
		float m_FramesPerSecond;
	};

	void ReportRenderProgress(const SRenderProgress &Progress)
	{
#if defined(CONF_WEB_PLATFORM)
		// The page's hook is on the page's thread, which is not waited for.
		if(!emscripten_is_main_runtime_thread())
		{
			emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(), [](void *pUser) {
				const std::unique_ptr<SRenderProgress> pProgress(static_cast<SRenderProgress *>(pUser));
				ReportRenderProgress(*pProgress); }, new SRenderProgress(Progress));
			return;
		}
#endif
		BrowserRenderProgress(Progress.m_Progress, Progress.m_EncodedFrames, Progress.m_SubmittedFrames, Progress.m_FramesPerSecond);
	}
} // namespace
#endif

namespace
{
	// What `rand()` starts with in a program that never seeds it.
	constexpr unsigned VIDEO_RANDOM_SEED = 1;
} // namespace

std::optional<int> CDemoRenderClient::ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments)
{
	// Whom to follow is this program's own argument. It is taken off first,
	// so that its value is not mistaken for the demo.
	vArguments.assign(ppArguments, ppArguments + ArgumentCount);
	for(auto It = vArguments.begin() + 1; It != vArguments.end();)
	{
		if(str_comp(*It, "--follow") != 0)
		{
			++It;
			continue;
		}
		if(It + 1 == vArguments.end() || (*(It + 1))[0] == '\0' || str_length(*(It + 1)) >= static_cast<int>(sizeof(m_aFollow)))
		{
			log_error("videorecorder", "Invalid value for --follow.");
			return -1;
		}
		str_copy(m_aFollow, *(It + 1));
		It = vArguments.erase(It, It + 2);
	}
	ArgumentCount = static_cast<int>(vArguments.size());
	ppArguments = vArguments.data();

	CCommandLineVideoExport VideoExport;
	if(!VideoExport.ParseArguments(ArgumentCount, ppArguments, vArguments, "ddnet-demo-render", true, true))
		return -1;
	if(VideoExport.m_Help)
	{
		log_info("videorecorder", "  --follow <player>  Follow a player, by client id or by name. A demo a server");
		log_info("videorecorder", "                     recorded has nobody to follow without this.");
	}
	if(VideoExport.m_ListCodecs)
	{
		for(const CVideoEncoder &Encoder : VideoEncoders())
			log_info("videorecorder", "%-20s %s", Encoder.m_aName[0] == '\0' ? "(default)" : Encoder.m_aName, Encoder.m_aDisplayName);
	}
	if(VideoExport.m_Help || VideoExport.m_ListCodecs)
		return 0;
	if(!VideoExport.m_Export)
	{
		PrintVideoExportUsage("ddnet-demo-render", true);
		return -1;
	}
	// One demo goes into the file named, several into the directory named,
	// each into a video called after it.
	for(const std::string &DemoPath : VideoExport.m_vDemoPaths)
	{
		char aVideoPath[IO_MAX_PATH_LENGTH];
		if(VideoExport.m_vDemoPaths.size() == 1)
			str_copy(aVideoPath, VideoExport.m_aVideoPath);
		else
		{
			char aName[IO_MAX_PATH_LENGTH];
			fs_split_file_extension(fs_filename(DemoPath.c_str()), aName, sizeof(aName));
			const int Length = str_length(VideoExport.m_aVideoPath);
			const bool Separator = Length > 0 && (VideoExport.m_aVideoPath[Length - 1] == '/' || VideoExport.m_aVideoPath[Length - 1] == '\\');
			str_format(aVideoPath, sizeof(aVideoPath), "%s%s%s", VideoExport.m_aVideoPath, Separator ? "" : "/", aName);
		}
		if(!str_endswith(aVideoPath, ".mp4"))
			str_append(aVideoPath, ".mp4");
		const bool Taken = std::any_of(m_vJobs.begin(), m_vJobs.end(), [&](const SJob &Job) { return Job.m_VideoPath == aVideoPath; });
		if(Taken)
		{
			log_error("videorecorder", "Two demos would be rendered into '%s'.", aVideoPath);
			return -1;
		}
		m_vJobs.push_back({DemoPath, aVideoPath});
	}
	return std::nullopt;
}

bool CDemoRenderClient::Configure()
{
	m_Settings = CCommandLineVideoExport::Settings();
	// The finished file is moved into place without replacing anything, so an
	// existing output would only be found out about after the whole render.
	return std::all_of(m_vJobs.begin(), m_vJobs.end(), [&](const SJob &Job) {
		if(!Storage()->FileExists(Job.m_VideoPath.c_str(), IStorage::TYPE_SAVE_OR_ABSOLUTE))
			return true;
		log_error("videorecorder", "Output file '%s' already exists.", Job.m_VideoPath.c_str());
		return false;
	});
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
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	ReportRenderProgress({Progress, (double)Status.m_EncodedFrames, (double)Status.m_SubmittedFrames, Status.m_FramesPerSecond});
#endif
}

bool CDemoRenderClient::RenderDemo(const SJob &Job)
{
	str_copy(m_aDemoPath, Job.m_DemoPath.c_str());
	str_copy(m_aVideoPath, Job.m_VideoPath.c_str());
	const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
	const char *pError = PlayDemo();
	if(pError == nullptr)
	{
		// A number is a client id, anything else a name, followed once the
		// demo names it.
		if(m_aFollow[0] != '\0')
		{
			int ClientId;
			if(str_toint(m_aFollow, &ClientId))
				Spectate(ClientId);
			else
				SetSpectateName(m_aFollow);
		}
		WaitUntilReadyToRender();
		pError = StartVideo();
	}
	if(pError != nullptr)
	{
		log_error("videorecorder", "%s", pError);
		EndDemo();
		return false;
	}
	// The effects draw on `rand()`, and so does loading. Every video starts
	// it at the same place, so that a demo makes the same video whatever was
	// loaded or rendered before.
	srand(VIDEO_RANDOM_SEED);
	while(State() != IClient::STATE_QUITTING && SessionState(m_DemoSessionId) == ESessionState::READY)
	{
		if(VideoExportInterrupted())
		{
			m_Interrupted = true;
			if(Exporting())
				m_pVideo->Cancel();
			StopDemoSession("Video rendering interrupted.");
			continue;
		}
		// The first frame shows the demo where it starts, so a frame is
		// drawn before the demo and the video clock move on to the next.
		// The client's export begins the same way.
		RenderExportFrame();
		Update();
	}
	const bool Succeeded = m_aError[0] == '\0';
	if(Succeeded)
		log_info("videorecorder", "Export completed: '%s' in %.1f s", m_aVideoPath, std::chrono::duration<float>(time_get_nanoseconds() - StartTime).count());
	else
		log_error("videorecorder", "%s", m_aError);
	EndDemo();
	return Succeeded;
}

void CDemoRenderClient::EndDemo()
{
	if(SessionState(m_DemoSessionId) != ESessionState::OFFLINE)
		StopDemoSession(nullptr);
	if(m_pVideo != nullptr)
	{
		if(IVideo::Current() == m_pVideo.get())
			m_pVideo->Stop();
		m_pVideo.reset();
	}
	// The next demo starts where a program that rendered nothing starts.
	Sound()->StopAll();
	m_aError[0] = '\0';
	m_LastProgressLog = std::chrono::nanoseconds(0);
}

int CDemoRenderClient::Run()
{
	int Failed = 0;
#if defined(CONF_WEB_PLATFORM)
	// In a browser the renderer is the demo player without a page: its own
	// canvas, no input and nothing to play the sound on.
	CWebAudioOutput::SetWanted(false);
	IEngineGraphicsWindow *pWindow = CreateWebOffscreenGraphicsWindow();
#else
	IEngineGraphicsWindow *pWindow = CreateOffscreenGraphicsWindow();
#endif
	if(InitGame(pWindow, nullptr))
	{
		// Nobody is at a keyboard here, so an interrupt is the only way out.
		// It has to reach the encoder, which removes the unfinished file.
		CatchVideoExportInterrupt();
		for(const SJob &Job : m_vJobs)
		{
			if(m_Interrupted || State() == IClient::STATE_QUITTING)
			{
				++Failed;
				continue;
			}
			if(!RenderDemo(Job))
				++Failed;
		}
		if(m_vJobs.size() > 1)
			log_info("videorecorder", "Rendered %d of %d demos", static_cast<int>(m_vJobs.size()) - Failed, static_cast<int>(m_vJobs.size()));
	}
	else
		Failed = static_cast<int>(m_vJobs.size());
	ShutdownGame();
	return Failed == 0 ? 0 : 1;
}

// The web demo player starts it, see its main().
#if !defined(CONF_WEB_PLATFORM)
int main(int argc, const char **argv)
{
	return DemoClientMain(new CDemoRenderClient, argc, argv);
}
#endif
