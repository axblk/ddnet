/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_client_base.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/http.h>
#include <engine/input.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/version.h>

#include <algorithm>
#include <memory>
#include <utility>

CDemoClientBase::CDemoClientBase()
{
	auto pDemoSource = std::make_unique<CDemoSessionSource>(true, [this]() { UpdateDemoIntraTimers(m_DemoSessionId); });
	m_pDemoSessionSource = pDemoSource.get();
	m_DemoSessionId = m_SessionManager.Create(std::move(pDemoSource));
	m_DemoListener = CDemoListener(this, m_DemoSessionId);
	m_pDemoSessionSource->m_DemoPlayer.SetListener(&m_DemoListener);
	m_SessionManager.SetFocused(m_DemoSessionId);
	m_StateStartTime = time_get();
	m_LastRenderTime = time_get();
}

CDemoClientBase::~CDemoClientBase() = default;

void CDemoClientBase::RegisterInterfaces()
{
	Kernel()->RegisterInterface(static_cast<IDemoPlayer *>(&DemoPlayer()), false);
	Kernel()->RegisterInterface(static_cast<IGhostRecorder *>(&m_GhostRecorder), false);
	Kernel()->RegisterInterface(static_cast<IGhostLoader *>(&m_GhostLoader), false);
	Kernel()->RegisterInterface(static_cast<IFriends *>(&m_Friends), false);
	Kernel()->ReregisterInterface(static_cast<IFriends *>(&m_Foes));
}

void CDemoClientBase::InitInterfaces()
{
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pGameClient = Kernel()->RequestInterface<IGameClient>();
	m_pSound = Kernel()->RequestInterface<IEngineSound>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_Friends.Init();
	m_Foes.Init(true);
	m_GhostRecorder.Init(m_pStorage);
	m_GhostLoader.Init(m_pStorage);
}

bool CDemoClientBase::InitGame(IEngineGraphicsWindow *pWindow, IEngineInput *pInput)
{
	m_LocalStartTime = m_GlobalStartTime = time_get();

	// The kernel shuts interfaces down in reverse order, and the window has to
	// outlive the graphics that draw into it.
	m_pWindow = pWindow;
	Kernel()->RegisterInterface(m_pWindow); // IEngineGraphicsWindow
	Kernel()->RegisterInterface(static_cast<IGraphicsWindow *>(m_pWindow), false);
	m_pGraphics = CreateEngineGraphicsThreaded();
	Kernel()->RegisterInterface(m_pGraphics); // IEngineGraphics
	Kernel()->RegisterInterface(static_cast<IGraphics *>(m_pGraphics), false);
	IGraphicsBackend *pBackend = m_pWindow->Open(false);
	if(pBackend == nullptr || m_pGraphics->Init(pBackend, m_pWindow->Surface()) != 0)
	{
		log_error("client", "Failed to initialize the graphics (see details above)");
		return false;
	}

	GameClient()->InitializeLanguage();
	if(Sound()->Init() != 0 && m_Settings.m_Audio)
	{
		log_warn("client", "The audio device could not be initialised, continuing without sound.");
		m_Settings.m_Audio = false;
	}
#if defined(CONF_VIDEORECORDER)
	InitVideoBackend();
#endif
	m_pTextRender = Kernel()->RequestInterface<IEngineTextRender>();
	m_pTextRender->Init();
	if(pInput != nullptr)
		pInput->Init();
	Graphics()->AddWindowResizeListener([this] { OnWindowResize(); });
	GameClient()->OnInit();
	return true;
}

void CDemoClientBase::ShutdownGame()
{
	// The text render is the last thing started before the game client, so
	// without it the graphics failed and there is nothing to end here. The
	// game client must not hear of a state it was never initialised for; the
	// kernel shuts the graphics down.
	if(m_pTextRender == nullptr)
		return;
	SetState(IClient::STATE_QUITTING);
#if defined(CONF_VIDEORECORDER)
	// A demo read again for an export goes first, because it hands the encoder
	// its last frames as it stops.
	const CSessionId ExportSessionId = VideoExportSessionId();
	if(ExportSessionId.IsValid() && ExportSessionId != m_DemoSessionId && SessionState(ExportSessionId) != ESessionState::OFFLINE)
		StopDemoSession(ExportSessionId, nullptr);
	if(m_pVideo != nullptr)
	{
		if(IVideo::Current() == m_pVideo.get())
			m_pVideo->Stop();
		m_pVideo.reset();
	}
#endif
	if(SessionState(m_DemoSessionId) != ESessionState::OFFLINE)
		StopDemoSession(nullptr);
	// The jobs load assets into the graphics and the sound, so neither may be
	// shut down while one is still running.
	Engine()->ShutdownJobs();
	GameClient()->OnShutdown();
	m_pTextRender->Shutdown();
	m_pGraphics->Shutdown();
}

void CDemoClientBase::Quit()
{
	SetState(IClient::STATE_QUITTING);
}

void CDemoClientBase::OnWindowResize()
{
	GameClient()->OnWindowResize();
	TextRender()->OnWindowResize();
}

const char *CDemoClientBase::PlayDemo(CSessionId SessionId)
{
	CDemoSessionSource &Source = DemoSource(SessionId);
	// Only the demo being watched is the program loading; another one is
	// opened beside it.
	const bool Watched = SessionId == m_DemoSessionId;
	if(Watched)
		SetState(IClient::STATE_LOADING);
	else
		Source.SetState(ESessionState::LOADING_MAP);
	if(const char *pError = LoadDemo(SessionId, m_aDemoPath, IStorage::TYPE_ALL_OR_ABSOLUTE))
		return pError;
	if(Watched)
		SetState(IClient::STATE_DEMOPLAYBACK);
	else
		Source.SetState(ESessionState::READY);
	GameClient()->OnConnected(SessionId);
	Source.PrepareSnapshots();
	Source.m_DemoPlayer.Play();
	GameClient()->OnEnterGame(SessionId);
	return nullptr;
}

void CDemoClientBase::StopDemoSession(CSessionId SessionId, const char *pReason)
{
	if(SessionId == m_DemoSessionId && pReason != nullptr && pReason[0] != '\0' && m_aError[0] == '\0')
		str_copy(m_aError, pReason);
	CDemoSessionSource &Source = DemoSource(SessionId);
	Source.m_DemoPlayer.Stop(pReason == nullptr ? "" : pReason);
	if(State() < IClient::STATE_QUITTING)
		GameClient()->OnSessionClosed(SessionId);
	Source.SetState(ESessionState::OFFLINE);
	Source.m_Connection.ResetSnapshots();
	Source.ResetMetadata();
}

void CDemoClientBase::Spectate(int SpectatorId)
{
	m_aPendingSpectateName[0] = '\0';
	ViewControl()->SetSpectatorId(m_DemoSessionId, SpectatorId);
}

void CDemoClientBase::SetSpectateName(const char *pName)
{
	str_copy(m_aPendingSpectateName, pName);
}

void CDemoClientBase::Update()
{
	set_new_tick();
	if(SessionState(m_DemoSessionId) == ESessionState::READY && !UpdateDemoPlayer(m_DemoSessionId))
		StopDemoSession(DemoPlayer().ErrorMessage());
#if defined(CONF_VIDEORECORDER)
	// A demo read again for an export stops when it runs out, which is how its
	// export learns that it is done.
	const CSessionId ExportSessionId = VideoExportSessionId();
	if(ExportSessionId.IsValid() && ExportSessionId != m_DemoSessionId && SessionState(ExportSessionId) == ESessionState::READY && !UpdateDemoPlayer(ExportSessionId))
		StopDemoSession(ExportSessionId, nullptr);
#endif
	// A player asked for by name shows up a snapshot or two in.
	if(m_aPendingSpectateName[0] != '\0' && SessionState(m_DemoSessionId) == ESessionState::READY)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			char aName[MAX_NAME_LENGTH];
			if(ViewControl()->PlayerName(m_DemoSessionId, ClientId, aName, sizeof(aName)) && str_comp(aName, m_aPendingSpectateName) == 0)
			{
				Spectate(ClientId);
				break;
			}
		}
	}
	Sound()->Update();
	GameClient()->OnUpdate();
}

#if defined(CONF_VIDEORECORDER)
const char *CDemoClientBase::StartVideo()
{
	Graphics()->WaitForIdle();
	const int StorageType = fs_is_relative_path(m_aVideoPath) ? IStorage::TYPE_SAVE : IStorage::TYPE_ABSOLUTE;
	m_pVideo = CreateVideo(Graphics(), Sound(), Storage(), m_Settings, m_LocalStartTime, m_aVideoPath, StorageType, false, true);
	CDemoPlayer &Player = DemoSource(VideoSessionId()).m_DemoPlayer;
	// The file can say how long it will be from its first fragment: what is
	// left of the demo at the speed it plays at.
	const IDemoPlayer::CInfo *pInfo = Player.BaseInfo();
	const int EndTick = m_VideoLastTick >= 0 ? m_VideoLastTick : pInfo->m_LastTick;
	const int RemainingTicks = std::max(EndTick - pInfo->m_CurrentTick, 0);
	const float Speed = pInfo->m_Speed > 0.0f ? pInfo->m_Speed : 1.0f;
	m_pVideo->SetExpectedDuration(RemainingTicks / (float)SERVER_TICK_SPEED / Speed);
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

bool CDemoClientBase::Exporting() const
{
	return m_pVideo != nullptr && IVideo::Current() == m_pVideo.get();
}

void CDemoClientBase::RenderExportFrame()
{
	const int64_t Now = time_get();
	m_RenderFrameTime = (Now - m_LastRenderTime) / (float)time_freq();
	m_FrameTimeAverage = m_FrameTimeAverage * 0.9f + m_RenderFrameTime * 0.1f;
	m_LastRenderTime = Now;

	IVideo *pVideo = IVideo::Current();
	if(pVideo == nullptr || !pVideo->BeginVideoFrameRender())
		return;
	GameClient()->OnRenderPrepare();
	GameClient()->OnRenderVideoPrepare(VideoSessionId(), pVideo->Settings());
	GameClient()->OnRender();
	if(pVideo->HasAudio())
	{
		// An export in a session of its own is mixed apart from the speakers.
		const bool Offline = VideoUsesOfflineAudio();
		pVideo->NextAudioFrameTimeline([this, Offline](short *pFinalOut, unsigned Frames) { Sound()->Mix(pFinalOut, Frames, Offline); });
	}
	GameClient()->OnRenderFinalize();
	pVideo->EndVideoFrameRender();
	OnExportFrame();
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}
#endif

int DemoClientMain(CDemoClientBase *pClient, int ArgumentCount, const char **ppArguments)
{
	CCmdlineFix CmdlineFix(&ArgumentCount, &ppArguments);

	std::shared_ptr<ILogger> pStdoutLogger = std::shared_ptr<ILogger>(log_logger_stdout());
	std::shared_ptr<CFutureLogger> pFutureConsoleLogger = std::make_shared<CFutureLogger>();
	std::shared_ptr<CFutureLogger> pFutureAssertionLogger = std::make_shared<CFutureLogger>();
	// A command line program reports on its output, so this only gives the
	// console commands that configure a log file something to write to.
	std::shared_ptr<CFutureLogger> pFutureFileLogger = std::make_shared<CFutureLogger>();
	pFutureFileLogger->Set(log_logger_noop());
	log_set_global_logger(log_logger_collection({pStdoutLogger, pFutureConsoleLogger, pFutureAssertionLogger, pFutureFileLogger}).release());
	// Setting the game up and reading its configuration prints a lot and
	// complains about every client command this program does not have. Until
	// the configuration says what was asked for, only warnings and errors go
	// out.
	pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(-1)});

	pClient->SetLoggers(std::shared_ptr<ILogger>(pFutureFileLogger), std::shared_ptr<ILogger>(pStdoutLogger));

	IKernel *pKernel = IKernel::Create();
	pKernel->RegisterInterface(static_cast<IClient *>(pClient), false);
	pKernel->RegisterInterface(static_cast<ISessions *>(pClient), false);
	pClient->RegisterInterfaces();

	IEngine *pEngine = CreateEngine(GAME_NAME, pFutureConsoleLogger);
	pKernel->RegisterInterface(pEngine, false);

	// The engine goes before the graphics, and the client before the kernel,
	// which owns the graphics the client still points to.
	const auto Cleanup = [&]() {
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		if(IEngineHttp *pHttp = pKernel->TryGetInterface<IEngineHttp>(); pHttp != nullptr)
			pHttp->Shutdown();
#endif
		delete pEngine;
		pKernel->Shutdown();
		delete pClient;
		delete pKernel;
	};

	IStorage *pStorage = CreateStorage(IStorage::EInitializationType::CLIENT, ArgumentCount, ppArguments);
	if(pStorage == nullptr)
	{
		log_error("client", "Failed to initialize the storage location (see details above)");
		Cleanup();
		return -1;
	}
	pKernel->RegisterInterface(pStorage);
	pFutureAssertionLogger->Set(CreateAssertionLogger(pStorage, GAME_NAME));

	IConsole *pConsole = CreateConsole(CFGFLAG_CLIENT).release();
	pKernel->RegisterInterface(pConsole);

	IConfigManager *pConfigManager = CreateConfigManager();
	pKernel->RegisterInterface(pConfigManager);

	IEngineSound *pEngineSound = CreateEngineSound();
	pKernel->RegisterInterface(pEngineSound); // IEngineSound
	pKernel->RegisterInterface(static_cast<ISound *>(pEngineSound), false);

	IEngineInput *pEngineInput = CreateEngineInput();
	pKernel->RegisterInterface(pEngineInput); // IEngineInput
	pKernel->RegisterInterface(static_cast<IInput *>(pEngineInput), false);

	IEngineTextRender *pEngineTextRender = CreateEngineTextRender();
	pKernel->RegisterInterface(pEngineTextRender); // IEngineTextRender
	pKernel->RegisterInterface(static_cast<ITextRender *>(pEngineTextRender), false);

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// In the browser the demo's assets are fetched beside each other.
	IEngineHttp *pEngineHttp = CreateEngineHttp();
	pKernel->RegisterInterface(pEngineHttp); // IEngineHttp
	pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);
#endif

	pKernel->RegisterInterface(CreateGameClient());

	pEngine->Init();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	if(!pEngineHttp->Init(std::chrono::seconds{1}))
	{
		log_error("client", "Failed to initialize the HTTP client");
		Cleanup();
		return -1;
	}
#endif
	pConsole->Init();
	pConfigManager->Init();
	pKernel->RequestInterface<IGameClient>()->OnConsoleInit();
	pClient->InitInterfaces();

	// The settings come from the configuration file, so a demo looks the way
	// it looks in the client. Nothing is written back to it.
	if(pStorage->FileExists(CONFIG_FILE, IStorage::TYPE_ALL) && !pConsole->ExecuteFile(CONFIG_FILE, IConsole::CLIENT_ID_UNSPECIFIED))
		log_warn("client", "Failed to load config from '" CONFIG_FILE "', continuing with the default settings.");
	if(g_Config.m_ClConfigVersion < 1 && g_Config.m_ClAntiPing == 0)
	{
		g_Config.m_ClAntiPingPlayers = 1;
		g_Config.m_ClAntiPingGrenade = 1;
		g_Config.m_ClAntiPingWeapons = 1;
	}
	pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_StdoutOutputLevel)});

	std::vector<const char *> vArguments;
	const std::optional<int> EarlyExitCode = pClient->ParseArguments(ArgumentCount, ppArguments, vArguments);
	if(EarlyExitCode.has_value())
	{
		Cleanup();
		return EarlyExitCode.value();
	}

	// What the program did not take configures the client the way it does in
	// the client itself, so `gfx_backend` and the like work here.
	pConsole->ParseArguments(ArgumentCount - 1, &ppArguments[1]);
	pStdoutLogger->SetFilter(CLogFilter{IConsole::ToLogLevelFilter(g_Config.m_StdoutOutputLevel)});

	if(!pClient->Configure())
	{
		Cleanup();
		return -1;
	}

	const int ExitCode = pClient->Run();
	for(const SWarning &Warning : pClient->QuittingWarnings())
		log_warn("client", "%s: %s", Warning.m_aWarningTitle, Warning.m_aWarningMsg);
	Cleanup();
	return ExitCode;
}
