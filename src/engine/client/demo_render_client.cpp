/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_render_client.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

#include <algorithm>
#include <cinttypes>

CDemoRenderClient::CDemoRenderClient()
{
	auto pDemoSource = std::make_unique<CDemoSessionSource>(true, [this](CDemoPlayer &DemoPlayer) { UpdateDemoIntraTimers(DemoPlayer); });
	m_pDemoSessionSource = pDemoSource.get();
	m_DemoSessionId = m_SessionManager.Create(std::move(pDemoSource));
	m_pDemoSessionSource->SetLifecycleCallbacks(
		[this]() { UpdateDemoSession(m_DemoSessionId); },
		[this](const char *pReason) { StopDemoSession(pReason); });
	m_SessionManager.SetFocused(m_DemoSessionId);
	m_StateStartTime = time_get();
	m_LastRenderTime = time_get();
}

CDemoRenderClient::~CDemoRenderClient() = default;

void CDemoRenderClient::RegisterInterfaces()
{
	Kernel()->RegisterInterface(static_cast<IDemoPlayer *>(&DemoPlayer()), false);
	Kernel()->RegisterInterface(static_cast<IGhostRecorder *>(&m_GhostRecorder), false);
	Kernel()->RegisterInterface(static_cast<IGhostLoader *>(&m_GhostLoader), false);
	Kernel()->RegisterInterface(static_cast<IFriends *>(&m_Friends), false);
	Kernel()->ReregisterInterface(static_cast<IFriends *>(&m_Foes));
}

void CDemoRenderClient::InitInterfaces()
{
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pGameClient = Kernel()->RequestInterface<IGameClient>();
	m_pSound = Kernel()->RequestInterface<IEngineSound>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = m_pConfigManager->Values();
	m_Friends.Init();
	m_Foes.Init(true);
}

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

void CDemoRenderClient::Quit()
{
	SetState(IClient::STATE_QUITTING);
}

void CDemoRenderClient::OnWindowResize()
{
	GameClient()->OnWindowResize();
	TextRender()->OnWindowResize();
}

void CDemoRenderClient::GetGpuInfoString(char (&aGpuInfo)[512])
{
	str_format(aGpuInfo, sizeof(aGpuInfo), "%s\n%s\n%s",
		Graphics()->GetVendorString(), Graphics()->GetRendererString(), Graphics()->GetVersionString());
}

bool CDemoRenderClient::InitGraphics()
{
	m_pGraphics = CreateEngineGraphicsThreaded(EGraphicsBackendMode::OFFSCREEN, false);
	Kernel()->RegisterInterface(m_pGraphics); // IEngineGraphics
	Kernel()->RegisterInterface(static_cast<IGraphics *>(m_pGraphics), false);
	if(m_pGraphics->Init() != 0)
	{
		log_error("videorecorder", "Failed to initialize the graphics (see details above)");
		return false;
	}
	return true;
}

const char *CDemoRenderClient::DemoPlayer_Play(const char *pFilename, int StorageType)
{
	str_copy(m_aDemoPath, pFilename);
	return PlayDemo();
}

const char *CDemoRenderClient::PlayDemo()
{
	CDemoSessionSource &Source = DemoSource(m_DemoSessionId);
	CDemoPlayer &Player = Source.DemoPlayer();
	Player.SetListener(this);
	SetState(IClient::STATE_LOADING);
	if(Player.Load(Storage(), m_pConsole, m_aDemoPath, IStorage::TYPE_ALL_OR_ABSOLUTE))
		return Player.ErrorMessage();
	Source.SetSixup(Player.IsSixup());

	const CMapInfo *pMapInfo = Player.GetMapInfo();
	const char *pError = LoadMapSearch(m_DemoSessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc);
	if(pError != nullptr)
	{
		// A demo may carry its map, which is the only copy of it a machine that
		// only renders demos is going to have.
		if(!Player.ExtractMap(Storage()))
			return pError;
		pError = LoadMapSearch(m_DemoSessionId, pMapInfo->m_aName, pMapInfo->m_Sha256, pMapInfo->m_Crc);
		if(pError != nullptr)
			return pError;
	}

	CServerInfo &DemoServerInfo = Source.ServerInfo();
	DemoServerInfo = {};
	str_copy(DemoServerInfo.m_aMap, pMapInfo->m_aName);
	DemoServerInfo.m_MapCrc = pMapInfo->m_Crc;
	DemoServerInfo.m_MapSize = pMapInfo->m_Size;

	SetState(IClient::STATE_DEMOPLAYBACK);
	GameClient()->OnConnected(m_DemoSessionId);
	Source.PrepareSnapshots();
	Player.Play();
	GameClient()->OnEnterGame(m_DemoSessionId);
	return nullptr;
}

const char *CDemoRenderClient::StartVideo()
{
	Graphics()->WaitForIdle();
	const int StorageType = fs_is_relative_path(m_aVideoPath) ? IStorage::TYPE_SAVE : IStorage::TYPE_ABSOLUTE;
	m_pVideo = CreateVideo(Graphics(), Sound(), Storage(), m_Settings, m_LocalStartTime, m_aVideoPath, StorageType, false, true);
	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	Player.SetVideo(m_pVideo.get());
	if(!m_pVideo->Start())
	{
		const CVideoExportStatus Status = m_pVideo->Status();
		str_copy(m_aError, Status.m_aError[0] == '\0' ? "Failed to start video recording." : Status.m_aError);
		return m_aError;
	}
	log_info("videorecorder", "Rendering '%s' to '%s'", m_aDemoPath, m_aVideoPath);
	return nullptr;
}

void CDemoRenderClient::StopDemoSession(const char *pReason)
{
	if(pReason != nullptr && pReason[0] != '\0' && m_aError[0] == '\0')
		str_copy(m_aError, pReason);
	CDemoSessionSource &Source = DemoSource(m_DemoSessionId);
	char aReason[256];
	str_copy(aReason, pReason == nullptr ? "" : pReason);
	Source.DemoPlayer().Stop(aReason);
	if(m_State < IClient::STATE_QUITTING)
		GameClient()->OnSessionClosed(m_DemoSessionId);
	Source.SetState(ESessionState::OFFLINE);
	Connection(m_DemoSessionId, CONN_MAIN).ResetSnapshots();
	Source.ResetMetadata();
}

bool CDemoRenderClient::DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const
{
	if(m_pVideo == nullptr)
		return false;
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
	*pFirstTick = pInfo->m_FirstTick;
	*pCurrentTick = pInfo->m_CurrentTick;
	*pLastTick = pInfo->m_LastTick;
	return true;
}

void CDemoRenderClient::LogProgress()
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

void CDemoRenderClient::RenderFrame()
{
	const int64_t Now = time_get();
	m_RenderFrameTime = (Now - m_LastRenderTime) / (float)time_freq();
	m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;
	m_LastRenderTime = Now;

	IVideo *pVideo = IVideo::Current();
	if(pVideo == nullptr || !pVideo->BeginVideoFrameRender())
		return;
	GameClient()->OnRenderVideoPrepare(m_DemoSessionId, pVideo->Settings());
	GameClient()->OnRender();
	if(pVideo->HasAudio())
	{
		pVideo->NextAudioFrameTimeline([this](short *pFinalOut, unsigned Frames) { Sound()->Mix(pFinalOut, Frames); });
	}
	GameClient()->OnRenderFinalize();
	pVideo->EndVideoFrameRender();
	LogProgress();
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}

void CDemoRenderClient::Run()
{
	m_LocalStartTime = m_GlobalStartTime = time_get();

	if(!InitGraphics())
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

	m_pTextRender = Kernel()->RequestInterface<IEngineTextRender>();
	m_pTextRender->Init();
	m_DebugFont = Graphics()->LoadTexture("debug_font.png", IStorage::TYPE_ALL);
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
		while(m_State != IClient::STATE_QUITTING && SessionState(m_DemoSessionId) == ESessionState::READY)
		{
			set_new_tick();
			m_SessionManager.Update();
			Sound()->Update();
			GameClient()->OnUpdate();
			RenderFrame();
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
