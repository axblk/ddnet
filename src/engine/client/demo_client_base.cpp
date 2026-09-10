/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "demo_client_base.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/shared/config.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <game/client/gameclient.h>

#include <memory>
#include <utility>

CDemoClientBase::CDemoClientBase()
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
	m_pConfig = m_pConfigManager->Values();
	m_Friends.Init();
	m_Foes.Init(true);
}

void CDemoClientBase::InitTextRender()
{
	m_pTextRender = Kernel()->RequestInterface<IEngineTextRender>();
	m_pTextRender->Init();
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

bool CDemoClientBase::InitGraphics(IEngineGraphicsWindow *pWindow)
{
	// The window comes first and is registered first: the kernel shuts
	// interfaces down in reverse order, and the window has to outlive the
	// graphics that draw into it.
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
	return true;
}

const char *CDemoClientBase::DemoPlayer_Play(const char *pFilename, int StorageType)
{
	str_copy(m_aDemoPath, pFilename);
	return PlayDemo();
}

const char *CDemoClientBase::PlayDemo()
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

const char *CDemoClientBase::StartVideo()
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

void CDemoClientBase::StopDemoSession(const char *pReason)
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

bool CDemoClientBase::DemoPlayer_RenderInfo(int *pFirstTick, int *pCurrentTick, int *pLastTick) const
{
	if(m_pVideo == nullptr)
		return false;
	const IDemoPlayer::CInfo *pInfo = DemoSource(m_DemoSessionId).DemoPlayer().BaseInfo();
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
	GameClient()->OnRenderVideoPrepare(m_DemoSessionId, pVideo->Settings());
	GameClient()->OnRender();
	if(pVideo->HasAudio())
	{
		pVideo->NextAudioFrameTimeline([this](short *pFinalOut, unsigned Frames) { Sound()->Mix(pFinalOut, Frames); });
	}
	GameClient()->OnRenderFinalize();
	pVideo->EndVideoFrameRender();
	OnExportFrame();
	m_GlobalTime = (time_get() - m_GlobalStartTime) / (float)time_freq();
}
