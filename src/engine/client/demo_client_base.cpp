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
#include <engine/input.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/version.h>

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

bool CDemoClientBase::InitGame(IEngineGraphicsWindow *pWindow)
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

void CDemoClientBase::GetGpuInfoString(char (&aGpuInfo)[512])
{
	str_format(aGpuInfo, sizeof(aGpuInfo), "%s\n%s\n%s",
		Graphics()->GetVendorString(), Graphics()->GetRendererString(), Graphics()->GetVersionString());
}

const char *CDemoClientBase::DemoPlayer_Play(const char *pFilename, int StorageType)
{
	str_copy(m_aDemoPath, pFilename);
	return PlayDemo();
}

const char *CDemoClientBase::PlayDemo()
{
	CDemoSessionSource &Source = DemoSource(m_DemoSessionId);
	SetState(IClient::STATE_LOADING);
	if(const char *pError = LoadDemo(m_DemoSessionId, m_aDemoPath, IStorage::TYPE_ALL_OR_ABSOLUTE))
		return pError;
	SetState(IClient::STATE_DEMOPLAYBACK);
	GameClient()->OnConnected(m_DemoSessionId);
	Source.PrepareSnapshots();
	Source.m_DemoPlayer.Play();
	GameClient()->OnEnterGame(m_DemoSessionId);
	return nullptr;
}

void CDemoClientBase::StopDemoSession(const char *pReason)
{
	if(pReason != nullptr && pReason[0] != '\0' && m_aError[0] == '\0')
		str_copy(m_aError, pReason);
	CDemoSessionSource &Source = DemoSource(m_DemoSessionId);
	Source.m_DemoPlayer.Stop(pReason == nullptr ? "" : pReason);
	if(m_State < IClient::STATE_QUITTING)
		GameClient()->OnSessionClosed(m_DemoSessionId);
	Source.SetState(ESessionState::OFFLINE);
	Source.m_Connection.ResetSnapshots();
	Source.ResetMetadata();
}

void CDemoClientBase::Update()
{
	set_new_tick();
	if(SessionState(m_DemoSessionId) == ESessionState::READY && !UpdateDemoPlayer(m_DemoSessionId))
		StopDemoSession(DemoPlayer().ErrorMessage());
	Sound()->Update();
	GameClient()->OnUpdate();
}

#if defined(CONF_VIDEORECORDER)
const char *CDemoClientBase::StartVideo()
{
	Graphics()->WaitForIdle();
	const int StorageType = fs_is_relative_path(m_aVideoPath) ? IStorage::TYPE_SAVE : IStorage::TYPE_ABSOLUTE;
	m_pVideo = CreateVideo(Graphics(), Sound(), Storage(), m_Settings, m_LocalStartTime, m_aVideoPath, StorageType, false, true);
	DemoPlayer().SetVideo(m_pVideo.get());
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

bool CDemoClientBase::DemoPlayer_RenderQueueActive() const
{
	return m_pVideo != nullptr;
}

bool CDemoClientBase::DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const
{
	if(m_pVideo == nullptr)
		return false;
	const IDemoPlayer::CInfo *pInfo = DemoPlayer().BaseInfo();
	*pFirstTick = pInfo->m_FirstTick;
	*pCurrentTick = pInfo->m_CurrentTick;
	*pLastTick = pInfo->m_LastTick;
	return true;
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
	GameClient()->OnRenderVideoPrepare(m_DemoSessionId, pVideo->Settings());
	GameClient()->OnRender();
	if(pVideo->HasAudio())
		pVideo->NextAudioFrameTimeline([this](short *pFinalOut, unsigned Frames) { Sound()->Mix(pFinalOut, Frames, false); });
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
	pClient->RegisterInterfaces();

	IEngine *pEngine = CreateEngine(GAME_NAME, pFutureConsoleLogger);
	pKernel->RegisterInterface(pEngine, false);

	// The engine goes before the graphics, and the client before the kernel,
	// which owns the graphics the client still points to.
	const auto Cleanup = [&]() {
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

	pKernel->RegisterInterface(CreateGameClient());

	pEngine->Init();
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
