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

#include <algorithm>
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

CGameClient *CDemoClientBase::Game() const
{
	// The game client this asks things of is the same one whether this is
	// const or not: what is const here is the program around it, not the game
	// it is showing.
	return const_cast<CGameClient *>(static_cast<const CGameClient *>(GameClient()));
}

int CDemoClientBase::Spectating() const
{
	return Game()->m_DemoSpecId;
}

void CDemoClientBase::SetSpectate(int SpectatorId)
{
	// Whoever says which player to watch has said it, so a name that was
	// waiting for the demo to turn up is no longer what is wanted.
	m_aPendingSpectateName[0] = '\0';
	SpectatorId = std::clamp(SpectatorId, (int)SPEC_FOLLOW, MAX_CLIENTS - 1);
	CGameClient *pGame = Game();
	if(pGame->m_DemoSpecId == SpectatorId)
	{
		return;
	}
	pGame->m_DemoSpecId = SpectatorId;
	// What is watched is taken out of here while a tick is drawn, so a demo
	// that stands still has to be shown one for the change to be seen. The
	// events of that tick are not new, they are the ones already heard.
	CDemoPlayer &Player = DemoSource(m_DemoSessionId).DemoPlayer();
	if(Player.BaseInfo()->m_Paused)
	{
		pGame->m_SuppressEvents = true;
		Player.SeekTick(IDemoPlayer::TICK_CURRENT);
		pGame->m_SuppressEvents = false;
		Player.Pause();
	}
}

void CDemoClientBase::SpectateStep(int Direction)
{
	const int Current = Spectating();
	for(int Offset = 1; Offset <= MAX_CLIENTS; ++Offset)
	{
		// Past the last player and past the first one lies the free view, which
		// is why the round trip is one longer than there are players.
		const int Candidate = ((Current < 0 ? (Direction > 0 ? -1 : MAX_CLIENTS) : Current) + Direction * Offset + MAX_CLIENTS + 1) % (MAX_CLIENTS + 1);
		const int SpectatorId = Candidate == MAX_CLIENTS ? SPEC_FREEVIEW : Candidate;
		if(SpectatorId == SPEC_FREEVIEW || SpectatePlayerName(SpectatorId) != nullptr)
		{
			SetSpectate(SpectatorId);
			return;
		}
	}
}

void CDemoClientBase::SetSpectateName(const char *pName)
{
	str_copy(m_aPendingSpectateName, pName);
	UpdatePendingSpectate();
}

void CDemoClientBase::UpdatePendingSpectate()
{
	if(m_aPendingSpectateName[0] == '\0')
	{
		return;
	}
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const char *pName = SpectatePlayerName(ClientId);
		if(pName != nullptr && str_comp(pName, m_aPendingSpectateName) == 0)
		{
			SetSpectate(ClientId);
			return;
		}
	}
}

const char *CDemoClientBase::SpectatePlayerName(int ClientId) const
{
	if(!in_range(ClientId, 0, MAX_CLIENTS - 1))
	{
		return nullptr;
	}
	const CGameClient::CClientData &Client = Game()->m_aClients[ClientId];
	return Client.m_Active ? Client.m_aName : nullptr;
}

void CDemoClientBase::MoveFreeView(vec2 Offset)
{
	// Only the free view is anybody's to move. While a player is followed the
	// camera is on them, and the move would be undone by the next frame.
	if(Offset != vec2(0.0f, 0.0f) && Spectating() == SPEC_FREEVIEW)
	{
		Game()->m_Camera.SetViewPos(Offset, true);
	}
}

void CDemoClientBase::ScaleZoom(float Factor)
{
	CCamera &Camera = Game()->m_Camera;
	if(Camera.ZoomAllowed())
	{
		Camera.ScaleZoom(Factor);
	}
}

float CDemoClientBase::Zoom() const
{
	return Game()->m_Camera.Zoom();
}

float CDemoClientBase::WorldPerPixel() const
{
	const int ScreenWidth = m_pGraphics->ScreenWidth();
	if(ScreenWidth <= 0)
	{
		return 0.0f;
	}
	float ViewWidth, ViewHeight;
	m_pGraphics->CalcScreenParams(m_pGraphics->ScreenAspect(), Zoom(), &ViewWidth, &ViewHeight);
	return ViewWidth / ScreenWidth;
}

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
	// A demo says how long it is before a frame of it is drawn, so the file
	// can say so too - from its first fragment, rather than only once it is
	// closed. What is left of the demo at the speed it is played at, which is
	// how long the export will take to run through it.
	const IDemoPlayer::CInfo *pInfo = Player.BaseInfo();
	const int RemainingTicks = std::max(pInfo->m_LastTick - pInfo->m_CurrentTick, 0);
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
