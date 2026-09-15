/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "gameclient.h"

#include "components/background.h"
#include "components/binds.h"
#include "components/broadcast.h"
#include "components/camera.h"
#include "components/chat.h"
#include "components/controls.h"
#include "components/countryflags.h"
#include "components/damageind.h"
#include "components/debughud.h"
#include "components/effects.h"
#include "components/emoticon.h"
#include "components/freezebars.h"
#include "components/ghost.h"
#include "components/hud.h"
#include "components/infomessages.h"
#include "components/items.h"
#include "components/mapimages.h"
#include "components/maplayers.h"
#include "components/mapsounds.h"
#include "components/motd.h"
#include "components/nameplates.h"
#include "components/particles.h"
#include "components/players.h"
#include "components/race_demo.h"
#include "components/scoreboard.h"
#include "components/skins.h"
#include "components/skins7.h"
#include "components/sounds.h"
#include "components/spectator.h"
#include "components/statboard.h"
#include "components/voting.h"
#include "frontend.h"
#include "game_prediction.h"
#include "lineinput.h"
#include "prediction/entities/character.h"
#include "prediction/entities/projectile.h"
#include "race.h"
#include "render.h"

#include <base/dbg.h>
#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client/checksum.h>
#include <engine/client/enums.h>
#include <engine/client/render_trace.h>
#include <engine/demo.h>
#include <engine/discord.h>
#include <engine/editor.h>
#include <engine/engine.h>
#include <engine/favorites.h>
#include <engine/friends.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/map.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/csv.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/video.h>
#include <engine/sound.h>
#include <engine/storage.h>
#include <engine/textrender.h>
#include <engine/updater.h>

#include <generated/client_data.h>
#include <generated/client_data7.h>
#include <generated/protocol.h>
#include <generated/protocol7.h>
#include <generated/protocolglue.h>

#include <game/client/projectile_data.h>
#include <game/localization.h>
#include <game/mapitems.h>
#include <game/teamscore.h>
#include <game/version.h>

#include <chrono>
#include <limits>
#include <utility>

using namespace std::chrono_literals;

namespace
{
	bool UnpackUuid(CUnpacker *pUnpacker, CUuid &Uuid)
	{
		const unsigned char *pData = pUnpacker->GetRaw(sizeof(Uuid));
		if(pData == nullptr)
			return false;
		mem_copy(&Uuid, pData, sizeof(Uuid));
		return true;
	}

	// what the server's game type says, lowercase and without anything but letters and digits; empty for a race
	std::string ClientObservedModeId(const CGameInfo &GameInfo, const char *pGameType)
	{
		if(GameInfo.m_Race)
			return {};
		std::string ModeId;
		for(const char *pCharacter = pGameType; *pCharacter != '\0' && ModeId.size() < (size_t)MatchReportLimits::MAX_ID_LENGTH; ++pCharacter)
		{
			const char Character = *pCharacter >= 'A' && *pCharacter <= 'Z' ? *pCharacter - 'A' + 'a' : *pCharacter;
			if((Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9'))
				ModeId.push_back(Character);
		}
		return ModeId;
	}

	bool UsePredictedEnvelopeTime(const CGameTickInfo &Time, const CGameView &View)
	{
		return !Time.m_IsDemoPlayback && g_Config.m_ClPredict && (!View.IsSpectating() || View.SpectatorId() == SPEC_FREEVIEW);
	}
}

const char *CGameClient::Version() const { return GAME_VERSION; }
const char *CGameClient::NetVersion() const { return GAME_NETVERSION; }
const char *CGameClient::NetVersion7() const { return GAME_NETVERSION7; }
int CGameClient::DDNetVersion() const { return DDNET_VERSION_NUMBER; }
const char *CGameClient::DDNetVersionStr() const { return m_aDDNetVersionStr; }
int CGameClient::ClientVersion7() const { return CLIENT_VERSION7; }
const char *CGameClient::GetItemName(int Type) const { return m_NetObjHandler.GetObjName(Type); }

CLocalSeats CGameClient::Seats() const
{
	if(ClientNetwork() == nullptr)
		return CLocalSeats();
	return CLocalSeats(ClientNetwork()->NetworkSessionId(), ClientNetwork()->DummySessionId());
}

bool CGameClient::AudioForState(const CGameState &State, bool &Offline) const
{
	Offline = false;
	// Only the seat that is played is heard. The other one shows the same world,
	// and would play every sound a second time.
	if(State.m_SessionId != PlayedSessionId(ContextSessionId(State.m_SessionId)))
		return false;
	return AudioForSession(State.m_SessionId, Offline);
}

CGameSessionContext *CGameClient::FindSessionContext(CSessionId SessionId) const
{
	for(const auto &pContext : m_vpSessionContexts)
	{
		if(pContext->Contains(SessionId))
			return pContext.get();
	}
	return nullptr;
}

CGameSessionContext &CGameClient::SessionContext(CSessionId SessionId) const
{
	CGameSessionContext *pContext = FindSessionContext(SessionId);
	dbg_assert(pContext != nullptr, "missing session context");
	return *pContext;
}

CSessionPresentation &CGameClient::SessionPresentation(CSessionId SessionId) const
{
	CSessionPresentation *pPresentation = m_SessionPresentations.Find(ContextSessionId(SessionId));
	dbg_assert(pPresentation != nullptr, "missing session presentation");
	return *pPresentation;
}

void CGameClient::ResetInfoMessages(CSessionId SessionId)
{
	SessionContext(SessionId).m_InfoMessages.Reset();
	m_InfoMessages.ResetPresentation(SessionId);
}

void CGameClient::ResetChat(CSessionId SessionId)
{
	m_Chat.ResetSession(SessionId);
}

void CGameClient::AddChatLine(CSessionId SessionId, int ClientId, int Team, const char *pText)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	m_Chat.AddLine(Session, Session.GameState(SessionId), SessionMessageTime(SessionId), Sessions()->SessionType(SessionId) == ESessionSourceType::DEMO, Session.Id() == Sessions()->FocusedSessionId(), ClientId, Team, pText);
}

int64_t CGameClient::SessionMessageTime(CSessionId SessionId) const
{
	return Sessions()->SessionType(SessionId) == ESessionSourceType::DEMO ? Sessions()->DemoPlaybackTime(SessionId) : time_get();
}

bool CGameClient::AudioForSession(CSessionId SessionId, bool &Offline) const
{
	Offline = false;
	// The dummy is heard with the server it plays on.
	SessionId = ContextSessionId(SessionId);
#if defined(CONF_VIDEORECORDER)
	const IVideo *pVideo = IVideo::Current();
	if(pVideo != nullptr && pVideo->HasAudio())
	{
		if(Client()->VideoSessionId() == SessionId)
		{
			Offline = Client()->VideoUsesOfflineAudio();
			return true;
		}
		if(!Client()->VideoUsesOfflineAudio())
			return false;
	}
#endif
	// The server and a demo beside it are never heard both at once, but
	// either of them can be the one that is heard.
	CSessionId Heard = Sessions()->FocusedSessionId();
	if(g_Config.m_ClPictureInPictureSound && (g_Config.m_ClPictureInPicture || g_Config.m_ClDummySplitScreen))
	{
		const CSessionId Other = OtherShownSessionId();
		if(Other.IsValid())
			Heard = Other;
	}
	return Heard == SessionId;
}

CSessionId CGameClient::OtherShownSessionId() const
{
	for(const auto &pContext : m_vpSessionContexts)
	{
		if(pContext->Id() != Sessions()->FocusedSessionId() && Sessions()->IsSessionShowable(pContext->Id()))
			return pContext->Id();
	}
	return CSessionId();
}

CGameView &CGameClient::GameView(CSessionId SessionId)
{
	// A demo keeps a view of its own wherever it is shown, so its camera
	// neither jumps nor is handed to the server when the focus moves.
	if(SessionId == InputSessionId() && IsNetworkSeat(SessionId) && !g_Config.m_ClDummySplitScreen)
	{
		TargetView(m_InputView, SessionId);
		return m_InputView;
	}
	CGameView &View = m_aPaneViews[IsNetworkSeat(SessionId) ? SeatOf(SessionId) : PANE_DEMO];
	View.SetTarget(SessionId);
	return View;
}

CGameView &CGameClient::ViewOf(CSessionId SessionId)
{
#if defined(CONF_VIDEORECORDER)
	if(SessionId == Client()->VideoSessionId() && (SessionId != Sessions()->FocusedSessionId() || Client()->VideoUsesOfflineAudio()))
		return m_VideoView;
#endif
	return GameView(SessionId);
}

float CGameClient::ViewLocalTime(CSessionId SessionId) const
{
#if defined(CONF_VIDEORECORDER)
	if(SessionId == Client()->VideoSessionId() && (SessionId != Sessions()->FocusedSessionId() || Client()->VideoUsesOfflineAudio()))
		return Sessions()->DemoPlaybackLocalTime(SessionId);
#endif
	return Client()->LocalTime();
}

void CGameClient::TargetView(CGameView &View, CSessionId SessionId) const
{
	const CGameSessionContext *pShown = FindSessionContext(View.SessionId());
	if(pShown != nullptr && pShown->Contains(SessionId))
		View.SwitchSeat(SessionId);
	else
		View.SetTarget(SessionId);
}

// The programs that only show a demo register none of these, and everything
// that uses one copes with its absence. The game registers all of them, so
// there a missing one is an assert at startup.
template<class TInterface>
static TInterface *ToolOptionalInterface(IKernel *pKernel)
{
#if defined(CONF_DEMO_RENDER_TOOL) || defined(CONF_DEMO_PLAYER_TOOL)
	return pKernel->TryGetInterface<TInterface>();
#else
	return pKernel->RequestInterface<TInterface>();
#endif
}

void CGameClient::OnConsoleInit()
{
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pHttp = ToolOptionalInterface<IHttp>(Kernel());
	const size_t MaxConcurrentAssetJobs = std::clamp(m_pEngine->JobThreadCount(), size_t{2}, size_t{16});
	m_AssetLoader.Init(m_pEngine, MaxConcurrentAssetJobs, m_pHttp);
	m_pClient = Kernel()->RequestInterface<IClient>();
	m_pSessions = Kernel()->RequestInterface<ISessions>();
	m_pClientNetwork = ToolOptionalInterface<IClientNetwork>(Kernel());
	m_pPrediction = ToolOptionalInterface<IGamePrediction>(Kernel());
	m_pRenderTrace = m_pClient->RenderTrace();
	// A program without a connection has no network session, and one that
	// renders a demo has no second one to export from in the background.
	if(NetworkSessionId().IsValid())
		m_vpSessionContexts.push_back(std::make_unique<CGameSessionContext>(NetworkSessionId(), DummySessionId()));
	m_vpSessionContexts.push_back(std::make_unique<CGameSessionContext>(Sessions()->DemoSessionId()));
#if defined(CONF_VIDEORECORDER)
	if(Sessions()->VideoExportSessionId().IsValid())
		m_vpSessionContexts.push_back(std::make_unique<CGameSessionContext>(Sessions()->VideoExportSessionId()));
#endif
	m_InputView.SetTarget(InputSessionId());
	m_Camera.BindState(m_InputView.m_Camera);
	m_pTextRender = Kernel()->RequestInterface<ITextRender>();
	m_pSound = Kernel()->RequestInterface<ISound>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = m_pConfigManager->Values();
	m_pInput = Kernel()->RequestInterface<IInput>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pDemoPlayer = Kernel()->RequestInterface<IDemoPlayer>();
	m_pServerBrowser = ToolOptionalInterface<IServerBrowser>(Kernel());
	m_pEditor = ToolOptionalInterface<IEditor>(Kernel());
	m_pFavorites = ToolOptionalInterface<IFavorites>(Kernel());
	m_pFriends = Kernel()->RequestInterface<IFriends>();
	m_pFoes = Client()->Foes();
	m_pDiscord = ToolOptionalInterface<IDiscord>(Kernel());
#if defined(CONF_AUTOUPDATE)
	m_pUpdater = ToolOptionalInterface<IUpdater>(Kernel());
#endif
	m_pFrontend = ToolOptionalInterface<IGameFrontend>(Kernel());

	// make a list of all the systems, make sure to add them in the correct render order
	m_vpAll.insert(m_vpAll.end(), {&m_Skins,
					      &m_Skins7,
					      &m_CountryFlags,
					      &m_MapImages,
					      &m_Effects, // updated explicitly before component rendering
					      &m_Binds,
					      &m_Binds.m_SpecialBinds,
					      &m_Controls,
					      &m_Camera,
					      &m_Sounds,
					      &m_Voting,
					      &m_Particles, // initialized as a component and updated explicitly
					      &m_RaceDemo,
					      &m_Censor,
					      &m_Background,
					      &m_Backdrop,
					      &m_Particles.m_RenderTrail,
					      &m_Particles.m_RenderTrailExtra,
					      &m_Items,
					      &m_Ghost,
					      &m_Players,
					      &m_Particles.m_RenderExplosions,
					      &m_NamePlates,
					      &m_Particles.m_RenderExtra,
					      &m_Particles.m_RenderGeneral,
					      &m_FreezeBars,
					      &m_DamageInd,
					      &m_Hud,
					      &m_Spectator,
					      &m_Emoticon,
					      &m_InfoMessages,
					      &m_Chat,
					      &m_Broadcast,
					      &m_ImportantAlert,
					      &m_DebugHud,
					      &m_TouchControls,
					      &m_Scoreboard,
					      &m_Statboard,
					      &m_Motd,
					      &m_Tooltips});
	// The front end comes after the game, and takes input where it says.
	auto AddFrontendInput = [this](IGameFrontend::ESlot Slot) {
		if(m_pFrontend != nullptr)
			m_vpInput.insert(m_vpInput.end(), m_pFrontend->Slot(Slot).begin(), m_pFrontend->Slot(Slot).end());
	};
	if(m_pFrontend != nullptr)
	{
		for(const IGameFrontend::CComponentInfo &Info : m_pFrontend->Components())
			m_vpAll.push_back(Info.m_pComponent);
	}

	// build the input stack
	AddFrontendInput(IGameFrontend::ESlot::INPUT_FIRST);
	m_vpInput.push_back(&m_Binds.m_SpecialBinds);
	AddFrontendInput(IGameFrontend::ESlot::INPUT_BEFORE_CHAT);
	m_vpInput.insert(m_vpInput.end(), {&m_Chat, // chat has higher prio, due to that you can quit it by pressing esc
						  &m_Scoreboard,
						  &m_Motd, // for pressing esc to remove it
						  &m_Spectator,
						  &m_Emoticon,
						  &m_ImportantAlert});
	AddFrontendInput(IGameFrontend::ESlot::INPUT_BEFORE_CONTROLS);
	m_vpInput.insert(m_vpInput.end(), {&m_Controls,
						  &m_TouchControls,
						  &m_Binds});

	// initialize client data
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CClientData &Client = m_aClients[ClientId];
		Client.m_pGameClient = this;
		Client.m_ClientId = ClientId;
	}

	// add basic console commands
	Console()->Register("team", "i[team-id]", CFGFLAG_CLIENT, ConTeam, this, "Switch team");
	Console()->Register("kill", "", CFGFLAG_CLIENT, ConKill, this, "Kill yourself to restart");
	Console()->Register("ready_change", "", CFGFLAG_CLIENT, ConReadyChange7, this, "Change ready state (0.7 only)");

	// register game commands to allow the client prediction to load settings from the map
	Console()->Register("tune", "s[tuning] ?f[value]", CFGFLAG_GAME, ConTuneParam, this, "Tune variable to value");
	Console()->Register("tune_zone", "i[zone] s[tuning] f[value]", CFGFLAG_GAME, ConTuneZone, this, "Tune in zone a variable to value");
	Console()->Register("mapbug", "s[mapbug]", CFGFLAG_GAME, ConMapbug, this, "Enable map compatibility mode using the specified bug (example: grenade-doubleexplosion@ddnet.tw)");

	for(auto &pComponent : m_vpAll)
		pComponent->OnInterfacesInit(this);
	m_SessionPresentations.OnInterfacesInit(this);
	for(const auto &pContext : m_vpSessionContexts)
		m_SessionPresentations.Create(pContext->Id());

	// let all the other components register their console commands
	for(auto &pComponent : m_vpAll)
		pComponent->OnConsoleInit();

	Console()->Chain("cl_languagefile", ConchainLanguageUpdate, this);

	Console()->Chain("player_name", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_clan", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_country", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_use_custom_color", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_color_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_color_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player_skin", ConchainSpecialInfoupdate, this);

	Console()->Chain("player7_skin", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_marking", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_decoration", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_hands", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_skin_eyes", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_marking", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_decoration", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_hands", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_color_eyes", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_body", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_marking", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_decoration", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_hands", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_feet", ConchainSpecialInfoupdate, this);
	Console()->Chain("player7_use_custom_color_eyes", ConchainSpecialInfoupdate, this);

	Console()->Chain("dummy_name", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_clan", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_country", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_use_custom_color", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_color_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_color_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy_skin", ConchainSpecialDummyInfoupdate, this);

	Console()->Chain("dummy7_skin", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_marking", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_decoration", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_hands", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_skin_eyes", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_marking", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_decoration", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_hands", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_color_eyes", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_body", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_marking", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_decoration", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_hands", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_feet", ConchainSpecialDummyInfoupdate, this);
	Console()->Chain("dummy7_use_custom_color_eyes", ConchainSpecialDummyInfoupdate, this);

	Console()->Chain("cl_skin_download_url", ConchainRefreshSkins, this);
	Console()->Chain("cl_skin_community_download_url", ConchainRefreshSkins, this);
	Console()->Chain("cl_skin_prefix", ConchainRefreshSkins, this);
	Console()->Chain("cl_download_skins", ConchainRefreshSkins, this);
	Console()->Chain("cl_download_community_skins", ConchainRefreshSkins, this);
	Console()->Chain("cl_vanilla_skins_only", ConchainRefreshSkins, this);
	Console()->Chain("events", ConchainRefreshEventSkins, this);

	Console()->Chain("cl_dummy", ConchainSpecialDummy, this);
}

void CGameClient::InitializeLanguage()
{
	m_LanguageIndexResource = m_AssetLoader.LoadFile(Storage(), "languages/index.txt", IStorage::TYPE_ALL);
}

void CGameClient::UpdateLanguageLoads()
{
	if(m_LanguageIndexResource && m_LanguageIndexResource.IsFinished())
	{
		if(m_LanguageIndexResource.IsReady())
			g_Localization.ParseIndex(m_LanguageIndexResource.Result().Text());
		else
			log_error("localization", "Couldn't open index file '%s'", m_LanguageIndexResource.Path());
		m_LanguageIndexResource.Reset();
		if(g_Config.m_ClShowWelcome)
		{
			g_Localization.SelectDefaultLanguage(Console(), g_Config.m_ClLanguagefile, sizeof(g_Config.m_ClLanguagefile));
			TextRender()->SetFontLanguageVariant(g_Config.m_ClLanguagefile);
		}
		if(g_Config.m_ClLanguagefile[0] != '\0')
			m_LanguageResource = m_AssetLoader.LoadFile(Storage(), g_Config.m_ClLanguagefile, IStorage::TYPE_ALL);
	}
	if(m_LanguageResource && m_LanguageResource.IsFinished())
	{
		if(m_LanguageResource.IsReady())
		{
			g_Localization.ParseLanguage(m_LanguageResource.Result().Text(), m_LanguageResource.Path());
			// Clear all text containers
			Client()->OnWindowResize();
		}
		else
		{
			log_error("localization", "Couldn't load language file '%s'", m_LanguageResource.Path());
		}
		m_LanguageResource.Reset();
	}
}

void CGameClient::ForceUpdateConsoleRemoteCompletionSuggestions()
{
	if(m_pFrontend != nullptr)
		m_pFrontend->OnRconCommandsChanged();
}

void CGameClient::OnInit()
{
	const int64_t OnInitStart = time_get();
	m_StartupAssetsPending = true;
	m_StartupAssetsStart = OnInitStart;

	Client()->SetLoadingCallback([this](IClient::ELoadingCallbackDetail Detail) {
		const char *pTitle;
		if(Detail == IClient::LOADING_CALLBACK_DETAIL_DEMO || DemoPlayer()->IsPlaying())
		{
			pTitle = Localize("Preparing demo playback");
		}
		else
		{
			pTitle = Localize("Connected");
		}

		const char *pMessage;
		switch(Detail)
		{
		case IClient::LOADING_CALLBACK_DETAIL_MAP:
			pMessage = Localize("Loading map file from storage");
			break;
		case IClient::LOADING_CALLBACK_DETAIL_DEMO:
			pMessage = Localize("Loading demo file from storage");
			break;
		default:
			dbg_assert_failed("Invalid callback loading detail");
		}
		RenderLoading(pTitle, pMessage, 0);
	});

	m_pGraphics = Kernel()->RequestInterface<IGraphics>();
	m_pWindow = Kernel()->RequestInterface<IGraphicsWindow>();

	// The two zones everything is drawn in first, so that they come first.
	m_GpuZoneWorld = Graphics()->RegisterGpuRenderZone("world");
	m_GpuZoneInterface = Graphics()->RegisterGpuRenderZone("interface");
	m_RenderComponentInfo.clear();
	for(const CComponent *pComponent : m_vpAll)
		m_RenderComponentInfo.emplace(pComponent, RenderComponentInfo(pComponent));
	// The map layers belong to the session presentations, not to the components.
	m_MapBackgroundRenderInfo = {"world/map_background", Graphics()->RegisterGpuRenderZone("map_background")};
	m_MapForegroundRenderInfo = {"world/map_foreground", Graphics()->RegisterGpuRenderZone("map_foreground")};

	// propagate pointers
	m_UI.Init(Kernel(), &m_RenderTools);
	// A popup over a menu sits on the menu, not on the scene: the backdrop there
	// would cut the scene into the menu instead of blurring what is behind it.
	m_UI.SetRenderPopupMenuBackdropCallback([this](const CUIRect &Rect, int Corners, float Rounding) {
		if(!MenuActive())
			m_Backdrop.RenderRegion(Rect, Corners, Rounding);
	});
	m_RenderTools.Init(Graphics(), TextRender());
	m_RenderMap.Init(Graphics(), TextRender());

	if(GIT_SHORTREV_HASH)
	{
		str_format(m_aDDNetVersionStr, sizeof(m_aDDNetVersionStr), "%s %s (%s)", GAME_NAME, GAME_RELEASE_VERSION, GIT_SHORTREV_HASH);
	}
	else
	{
		str_format(m_aDDNetVersionStr, sizeof(m_aDDNetVersionStr), "%s %s", GAME_NAME, GAME_RELEASE_VERSION);
	}

	// TODO: this should be different
	// setup item sizes
	for(int i = 0; i < NUM_NETOBJTYPES; i++)
		Sessions()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));
	// HACK: only set static size for items, which were available in the first 0.7 release
	// so new items don't break the snapshot delta
	static const int OLD_NUM_NETOBJTYPES = 23;
	for(int i = 0; i < OLD_NUM_NETOBJTYPES; i++)
		Sessions()->SnapSetStaticsize7(i, m_NetObjHandler7.GetObjSize(i));

	if(!TextRender()->WaitForFonts([this]() { m_AssetLoader.Update(); }))
	{
		Client()->AddWarning(SWarning(Localize("Some fonts could not be loaded. Check the local console for details.")));
	}
	TextRender()->SetFontLanguageVariant(g_Config.m_ClLanguagefile);

	// update and swap after font loading, they are quite huge
	Client()->UpdateAndSwap();

	const char *pLoadingDDNetCaption = Localize("Loading DDNet Client");
	const char *pLoadingMessageComponents = Localize("Initializing components");
	const char *pLoadingMessageComponentsSpecial = Localize("Why are you slowmo replaying to read this?");
	char aLoadingMessage[256];
	StartLoadingCoreImages();

	// init all components
	int SkippedComps = 1;
	int CompCounter = 1;
	const int NumComponents = ComponentCount();
	for(int i = NumComponents - 1; i >= 0; --i)
	{
		m_vpAll[i]->OnInit();
		m_AssetLoader.Update();
		// try to render a frame after each component, also flushes GPU uploads
		if(m_pFrontend != nullptr && m_pFrontend->LoadingScreenReady())
		{
			str_format(aLoadingMessage, std::size(aLoadingMessage), "%s [%d/%d]", CompCounter == NumComponents ? pLoadingMessageComponentsSpecial : pLoadingMessageComponents, CompCounter, NumComponents);
			m_pFrontend->RenderLoading(pLoadingDDNetCaption, aLoadingMessage, SkippedComps, true);
			SkippedComps = 1;
		}
		else
		{
			++SkippedComps;
		}
		++CompCounter;
	}

	FinishLoadingCoreImages();
	m_InitComplete = true;

	OnSessionClosed(Sessions()->FocusedSessionId());

	// Set free binds to DDRace binds if it's active
	m_Binds.SetDDRaceBinds(true);

	// Aggressively try to grab window again since some Windows users report
	// window not being focused after starting client.
	Window()->SetWindowGrab(true);

	// Only a server asks for the checksum.
	if(ClientNetwork() != nullptr)
	{
		CChecksumData *pChecksum = ClientNetwork()->ChecksumData();
		pChecksum->m_SizeofGameClient = sizeof(*this);
		pChecksum->m_NumComponents = m_vpAll.size();
		for(size_t i = 0; i < m_vpAll.size(); i++)
		{
			if(i >= std::size(pChecksum->m_aComponentsChecksum))
			{
				break;
			}
			int Size = m_vpAll[i]->Sizeof();
			pChecksum->m_aComponentsChecksum[i] = Size;
		}
	}

	log_trace("gameclient", "initialization finished after %.2fms", (time_get() - OnInitStart) * 1000.0f / (float)time_freq());
}

void CGameClient::OnUpdate()
{
	m_AssetLoader.Update();
	if(TextRender()->Update())
	{
		// Text containers keep the glyphs they were built with
		Client()->OnWindowResize();
	}
	if(m_CoreImagesPending)
	{
		FinishLoadingCoreImages();
		if(m_CoreImagesPending)
			return;
	}
	UpdateAssetPackLoads();
	UpdateLanguageLoads();
	HandleLanguageChanged();
	RequestLiveStats();

	CUIElementBase::Init(Ui()); // update static pointer because game and editor use separate UI

	// handle mouse movement
	float x = 0.0f, y = 0.0f;
	IInput::ECursorType CursorType = Input()->CursorRelative(&x, &y);
	if(CursorType != IInput::CURSOR_NONE)
	{
		for(auto &pComponent : m_vpInput)
		{
			if(pComponent->OnCursorMove(x, y, CursorType))
				break;
		}
	}

	// handle touch events
	std::vector<IInput::CTouchFingerState> vTouchFingerStates = Input()->TouchFingerStates();
	bool TouchHandled = false;
	for(auto &pComponent : m_vpInput)
	{
		if(pComponent == &m_TouchControls)
		{
			if(m_TouchControls.UpdateController(InputView(), vTouchFingerStates, !TouchHandled) && !TouchHandled)
			{
				Input()->ClearTouchDeltas();
				TouchHandled = true;
				vTouchFingerStates.clear();
			}
			continue;
		}
		if(pComponent->OnTouchState(vTouchFingerStates))
		{
			Input()->ClearTouchDeltas();
			TouchHandled = true;
		}
	}

	// handle key presses
	Input()->ConsumeEvents([&](const IInput::CEvent &Event) {
		OnInput(Event);
	});

	if(g_Config.m_ClSubTickAiming && m_Binds.m_MouseOnAction)
	{
		CGameState::CInputState &Input = m_Controls.ActiveInput();
		Input.m_MousePosOnAction = Input.m_MousePos;
		m_Binds.m_MouseOnAction = false;
	}
	for(const auto &pContext : m_vpSessionContexts)
		pContext->m_Vote.Expire(SessionMessageTime(pContext->Id()), time_freq());

	for(auto &pComponent : m_vpAll)
	{
		pComponent->OnUpdate();
	}
	TryFinishStartupAssets();

	UpdateNetworkPlayerInfo();
	m_NewTick = false;
	m_NewPredictedTick = false;
	UpdateManagedTeeRenderInfos();
}

void CGameClient::UpdateNetworkPlayerInfo()
{
	if(Sessions()->FocusedSessionId() == Sessions()->DemoSessionId())
		return;

	const int MainLocalId = SessionContext().SeatState(IClient::CONN_MAIN).LocalClientId();
	const int DummyLocalId = SessionContext().SeatState(IClient::CONN_DUMMY).LocalClientId();
	CGameState::CRuntimeState &MainRuntime = SessionContext().SeatState(IClient::CONN_MAIN).m_Runtime;
	CGameState::CRuntimeState &DummyRuntime = SessionContext().SeatState(IClient::CONN_DUMMY).m_Runtime;
	if(MainLocalId < 0 || !Client()->IsOnline() || MenuActive() || !m_NewTick)
		return;

	if(MainRuntime.m_CheckInfo == 0)
	{
		if(m_pSessions->IsSixup(NetworkSessionId()))
		{
			if(!GotWantedSkin7(IClient::CONN_MAIN))
				SendSkinChange7(IClient::CONN_MAIN);
			else
				MainRuntime.m_CheckInfo = -1;
		}
		else
		{
			if(
				str_comp(m_aClients[MainLocalId].m_aName, PlayerName()) ||
				str_comp(m_aClients[MainLocalId].m_aClan, g_Config.m_PlayerClan) ||
				m_aClients[MainLocalId].m_Country != g_Config.m_PlayerCountry ||
				str_comp(m_aClients[MainLocalId].m_aSkinName, g_Config.m_ClPlayerSkin) ||
				m_aClients[MainLocalId].m_UseCustomColor != g_Config.m_ClPlayerUseCustomColor ||
				m_aClients[MainLocalId].m_ColorBody != (int)g_Config.m_ClPlayerColorBody ||
				m_aClients[MainLocalId].m_ColorFeet != (int)g_Config.m_ClPlayerColorFeet)
				SendInfo(false);
			else
				MainRuntime.m_CheckInfo = -1;
		}
	}

	if(MainRuntime.m_CheckInfo > 0)
		MainRuntime.m_CheckInfo -= std::min(Sessions()->GameTick(NetworkSessionId()) - Sessions()->PrevGameTick(NetworkSessionId()), MainRuntime.m_CheckInfo);

	if(DummyLocalId < 0)
		return;
	if(DummyRuntime.m_CheckInfo == 0)
	{
		if(m_pSessions->IsSixup(NetworkSessionId()))
		{
			if(!GotWantedSkin7(IClient::CONN_DUMMY))
				SendSkinChange7(IClient::CONN_DUMMY);
			else
				DummyRuntime.m_CheckInfo = -1;
		}
		else
		{
			if(
				str_comp(m_aClients[DummyLocalId].m_aName, DummyName()) ||
				str_comp(m_aClients[DummyLocalId].m_aClan, g_Config.m_ClDummyClan) ||
				m_aClients[DummyLocalId].m_Country != g_Config.m_ClDummyCountry ||
				str_comp(m_aClients[DummyLocalId].m_aSkinName, g_Config.m_ClDummySkin) ||
				m_aClients[DummyLocalId].m_UseCustomColor != g_Config.m_ClDummyUseCustomColor ||
				m_aClients[DummyLocalId].m_ColorBody != (int)g_Config.m_ClDummyColorBody ||
				m_aClients[DummyLocalId].m_ColorFeet != (int)g_Config.m_ClDummyColorFeet)
				SendDummyInfo(false);
			else
				DummyRuntime.m_CheckInfo = -1;
		}
	}

	if(DummyRuntime.m_CheckInfo > 0)
		DummyRuntime.m_CheckInfo -= std::min(Sessions()->GameTick(DummySessionId()) - Sessions()->PrevGameTick(DummySessionId()), DummyRuntime.m_CheckInfo);
}

bool CGameClient::MenuActive() const
{
	return m_pFrontend != nullptr && m_pFrontend->MenuActive();
}

bool CGameClient::ConsoleActive() const
{
	return m_pFrontend != nullptr && m_pFrontend->ConsoleActive();
}

void CGameClient::SetMenuActive(bool Active)
{
	if(m_pFrontend != nullptr)
		m_pFrontend->SetMenuActive(Active);
}

void CGameClient::RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter, bool UpdateAndSwap)
{
	if(m_pFrontend != nullptr)
		m_pFrontend->RenderLoading(pCaption, pContent, IncreaseCounter, UpdateAndSwap);
}

bool CGameClient::SceneBackdropWanted() const
{
	return MenuActive() || m_Scoreboard.IsActive() || m_Statboard.IsActive() || m_Motd.IsActive();
}

bool CGameClient::BackdropWanted() const
{
	return SceneBackdropWanted() || ConsoleActive();
}

void CGameClient::DemoSeekTick(IDemoPlayer::ETickOffset TickOffset)
{
	m_SuppressEvents = true;
	DemoPlayer()->SeekTick(TickOffset);
	m_SuppressEvents = false;
	DemoPlayer()->Pause();
}

void CGameClient::OnInput(const IInput::CEvent &Event)
{
	for(auto &pComponent : m_vpInput)
	{
		// Events with flag `FLAG_RELEASE` must always be forwarded to all components so keys being
		// released can be handled in all components also after some components have been disabled.
		if(pComponent->OnInput(Event) && (Event.m_Flags & ~IInput::FLAG_RELEASE) != 0)
			break;
	}
}

void CGameClient::OnDummySwap()
{
	CGameSessionContext &Session = SessionContext(NetworkSessionId());
	const int ActiveSeat = g_Config.m_ClDummy;
	InputView();
	m_Camera.UpdateCamera();
	for(CClientData &Client : m_aClients)
		Client.UpdateSkinInfo(Session.SeatState(ActiveSeat));
	if(g_Config.m_ClDummyResetOnSwitch)
		m_Controls.ResetInput(g_Config.m_ClDummyResetOnSwitch == 2 ? ActiveSeat : ActiveSeat ^ 1);
	// Only the seat that gets the input is predicted: the other one is drawn
	// from its snapshots until it gets the input back.
	CGameState &PreviousState = Session.SeatState(ActiveSeat ^ 1);
	PreviousState.ClearPrediction();
	m_PreviousInputSessionId = PreviousState.m_SessionId;
	UpdateInputRoutes(Session);
}

void CGameClient::UpdateInputRoutes(CGameSessionContext &Session) const
{
	const bool AcceptControls = Session.Id() == Sessions()->FocusedSessionId();
	const int ActiveSeat = InputSeat();
	for(const CGameState &State : Session.GameStates())
	{
		CInputRoute &Route = Session.m_aInputRoutes[State.m_Seat];
		Route.m_Policy = EInputPolicy::DIRECT;
		if(AcceptControls && State.m_Seat != ActiveSeat && g_Config.m_ClDummyHammer)
			Route.m_Policy = EInputPolicy::HAMMER;
		else if(AcceptControls && State.m_Seat != ActiveSeat && g_Config.m_ClDummyCopyMoves)
			Route.m_Policy = EInputPolicy::COPY_MOVES;
		Route.m_Source = Route.m_Policy == EInputPolicy::DIRECT ? State.m_Seat : ActiveSeat;
	}
}

int CGameClient::OnSnapInput(CSessionId SessionId, int *pData, bool Force)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	UpdateInputRoutes(Session);
	CGameState &TargetState = Session.GameState(SessionId);
	CInputRoute &Route = Session.m_aInputRoutes[TargetState.m_Seat];
	const EInputPolicy Policy = Route.m_Policy;
	CNetObj_PlayerInput &TargetInput = TargetState.Input().m_InputData;
	const bool Focused = Sessions()->FocusedSessionId() == Session.Id();
	if(Policy != EInputPolicy::HAMMER)
		Route.FinishHammering(TargetInput);
	if(Policy == EInputPolicy::DIRECT && SessionId == InputSessionId())
	{
		return m_Controls.SnapInput(pData);
	}
	if(TargetState.LocalClientId() < 0)
	{
		return 0;
	}

	if(Policy != EInputPolicy::HAMMER)
	{
		if(!Force && Focused && (!TargetInput.m_Direction && !TargetInput.m_Jump && !TargetInput.m_Hook))
		{
			return 0;
		}

		mem_copy(pData, &TargetInput, sizeof(TargetInput));
		return sizeof(TargetInput);
	}
	else
	{
		if(!Route.AdvanceHammer())
			return 0;

		Route.m_HammerInput.m_Fire = (Route.m_HammerInput.m_Fire + 1) | 1;
		Route.m_HammerInput.m_WantedWeapon = WEAPON_HAMMER + 1;
		if(!g_Config.m_ClDummyRestoreWeapon)
		{
			TargetInput.m_WantedWeapon = WEAPON_HAMMER + 1;
		}

		const vec2 Dir = m_LocalCharacterPos - m_aClients[TargetState.LocalClientId()].m_Predicted.m_Pos;
		Route.m_HammerInput.m_TargetX = (int)Dir.x;
		Route.m_HammerInput.m_TargetY = (int)Dir.y;

		mem_copy(pData, &Route.m_HammerInput, sizeof(Route.m_HammerInput));
		return sizeof(Route.m_HammerInput);
	}
}

bool CGameClient::ShareLoadedMap(CSessionId SessionId, const char *pName, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	// A map is never loaded into data another session still plays.
	Session.m_MapContext.Unload();
	for(const auto &pOther : m_vpSessionContexts)
	{
		const IMap *pMap = pOther->m_MapContext.Map();
		if(pOther.get() == &Session || !pMap->IsLoaded() || str_comp(pMap->BaseName(), fs_filename(pName)) != 0)
			continue;
		if(WantedSha256.has_value() ? pMap->Sha256() == WantedSha256.value() : pMap->Crc() == WantedCrc)
		{
			Session.m_MapContext.Share(pOther->m_MapContext);
			return true;
		}
	}
	return false;
}

void CGameClient::OnConnected(CSessionId SessionId)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	CMapContext &MapContext = Session.m_MapContext;
	const bool Focused = SessionId == Sessions()->FocusedSessionId();
	const char *pConnectCaption = SessionId == Sessions()->DemoSessionId() ? Localize("Preparing demo playback") : Localize("Connected");
	const char *pLoadMapContent = Localize("Initializing map logic");
	if(Focused)
		RenderLoading(pConnectCaption, pLoadMapContent, 0);
	MapContext.Data()->InitLayers();
	MapContext.Collision()->Init(MapContext.Layers());
	Session.SetDescriptor(MapContext.Map()->BaseName(), Sessions()->IsSixup(SessionId) ? EGameProtocol::SIXUP : EGameProtocol::SIX);
	Session.SetServerCapAnyPlayerFlag(Sessions()->SessionType(SessionId) == ESessionSourceType::NETWORK && ClientNetwork()->ServerCapAnyPlayerFlag(SessionId));
	MapContext.Load(*Config());
	for(CGameState &SessionState : Session.GameStates())
		SessionState.InitPrediction(MapContext, m_pPrediction);
	CSessionPresentation &Presentation = SessionPresentation(SessionId);
	Presentation.Load(Session, m_SessionPresentations.MapPresentation(MapContext.Data(), Sessions()->IsSixup(SessionId)));

	// The map images are fetched asynchronously. Their layers were built with
	// texture coordinates and would draw untextured until they arrive, so the
	// world is only entered once they are all there - here, where the loading
	// screen is still up. A session loading in the background has no loading
	// screen to hold and picks its images up per frame instead.
	while(Focused && Presentation.UpdateMapImages())
	{
		// the loader only starts the next jobs when it is updated
		m_AssetLoader.Update();
		RenderLoading(pConnectCaption, Localize("Loading map images"), 0);
		// a program without a loading screen would spin here, and in a browser
		// the fetches only finish while the page has its turn
		thread_sleep_idle(std::chrono::milliseconds(1));
	}

	if(SessionId == NetworkSessionId())
	{
		if(Focused)
		{
			Client()->SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_GETTING_READY);
			RenderLoading(pConnectCaption, Localize("Sending initial client info"), 0);
		}
		SendInfo(true);
		ClientNetwork()->Rcon("crashmeplx");
	}
	if(m_pFrontend != nullptr)
		m_pFrontend->OnSessionConnected(SessionId);

	if(!Focused)
		return;

	m_RaceHelper.Init(this);
	m_SessionPresentations.SetAudible(SessionId);

	// render loading before going through all components
	RenderLoading(pConnectCaption, pLoadMapContent, 0);
	for(auto &pComponent : m_vpAll)
	{
		pComponent->OnMapLoad();
		pComponent->OnReset();
	}
}

void CGameClient::StoreMatch(CSessionId SessionId, const CStoredMatch &Match, const CStoredMatch *pReplacedObserved)
{
	if(Sessions()->SessionType(SessionId) != ESessionSourceType::NETWORK || m_pFrontend == nullptr)
		return;
	m_pFrontend->StoreMatch(Match, pReplacedObserved);
}

void CGameClient::FinalizeObservedMatch(CSessionId SessionId, CGameSessionContext &Session, const CGameState &State, EMatchTermination Termination)
{
	const CServerInfo &ServerInfo = Sessions()->ServerInfo(SessionId);
	const std::string ModeId = ClientObservedModeId(State.CoreGameInfo(), ServerInfo.m_aGameType);
	const IMap *pMap = Map(SessionId);
	if(ModeId.empty() || pMap == nullptr)
		return;
	CObservedMatchMetadata Metadata;
	Metadata.m_OriginId = ServerInfo.m_aAddress;
	Metadata.m_ModeId = ModeId;
	Metadata.m_MapName = ServerInfo.m_aMap;
	Metadata.m_MapSha256 = pMap->Sha256();
	Metadata.m_EndTimeUtc = time_timestamp();
	Metadata.m_TickRate = Sessions()->GameTickSpeed();
	Metadata.m_Termination = Termination;
	if(Session.m_Stats.FinalizeObservedMatch(Metadata, State, Sessions()->GameTick(SessionId)))
		StoreMatch(SessionId, *Session.m_Stats.LatestMatch(), nullptr);
}

void CGameClient::OnSessionClosed(CSessionId SessionId)
{
	++m_SessionChanges;
	CGameSessionContext &Session = SessionContext(SessionId);
	if(Sessions()->SessionType(SessionId) == ESessionSourceType::NETWORK)
	{
		FinalizeObservedMatch(SessionId, Session, Session.SeatState(IClient::CONN_MAIN), EMatchTermination::ABORTED);
		PersistLiveStatsOnDisconnect(SessionId, Session);
	}
	for(CGameState &SessionState : Session.GameStates())
		SessionState.Reset();
	Session.m_Broadcast.Reset();
	Session.m_MapMetadata.Reset();
	Session.m_Vote.Reset();
	ResetInfoMessages(SessionId);
	ResetChat(SessionId);
	Session.m_Stats.Reset();
	Session.m_MatchReportAssembler.Reset();
	Session.m_LastLiveStatsRequest = 0;
	for(CInputRoute &Route : Session.m_aInputRoutes)
		Route.Reset();
	Session.m_DemoSpecId = SPEC_FOLLOW;
	m_SessionPresentations.Unload(SessionId);
#if defined(CONF_VIDEORECORDER)
	if(SessionId == Client()->VideoSessionId() && Client()->VideoUsesOfflineAudio())
	{
		Sound()->StopAll(true);
		m_Sounds.ClearQueue(true);
	}
#endif
	Session.m_MapContext.Unload();
	if(SessionId == NetworkSessionId())
	{
		m_RaceDemo.OnNetworkSessionClosed();
		m_ActiveRecordings.reset();
	}

	if(SessionId != Sessions()->FocusedSessionId())
		return;

	InvalidateSnapshot(SessionId);

	m_EditorMovementDelay = 5;

	// m_aDDNetVersionStr is initialized once in OnInit

	m_SuppressEvents = false;
	m_NewTick = false;
	m_NewPredictedTick = false;

	m_LocalCharacterPos = vec2(0.0f, 0.0f);

	m_PredictedPrevChar.Reset();
	m_PredictedChar.Reset();

	// Snap() was cleared in InvalidateSnapshot

	for(auto &Client : m_aClients)
		Client.Reset();

	m_vSnapEntities.clear();

	m_PreviousInputSessionId = CSessionId();

	// Map bugs and tunings are reset when the map context is loaded.

	m_aCameraSent = {};

	MultiView().Reset();

	auto ResetSpectator = [](CGameView &View) {
		View.SetSpectator(false);
		View.m_SpectatorCursor.Reset();
	};
	ResetSpectator(m_InputView);
	for(CGameView &View : m_aPaneViews)
		ResetSpectator(View);

	for(auto &pComponent : m_vpAll)
		pComponent->OnReset();

	if(Editor() != nullptr)
	{
		Editor()->ResetMentions();
		Editor()->ResetIngameMoved();
	}
}

void CGameClient::PersistLiveStatsOnDisconnect(CSessionId SessionId, CGameSessionContext &Session)
{
	if(const CStoredMatch *pLive = Session.m_Stats.LiveStatsToPersist())
		StoreMatch(SessionId, *pLive, Session.m_Stats.ObservedMatchReplacedBy(*pLive));
}

void CGameClient::HandleMatchReportMessage(CSessionId SessionId, int MsgId, CUnpacker *pUnpacker)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	CMatchReportAssembler &Assembler = Session.m_MatchReportAssembler;
	CUuid MatchId;
	if(!UnpackUuid(pUnpacker, MatchId))
		return;
	if(MsgId == NETMSG_MATCH_REPORT_START)
	{
		const bool Live = pUnpacker->GetInt() != 0;
		const bool PersistOnDisconnect = pUnpacker->GetInt() != 0;
		const int LocalParticipantId = pUnpacker->GetInt();
		const int Size = pUnpacker->GetInt();
		if(pUnpacker->Error() || !Assembler.Start(MatchId, Live, PersistOnDisconnect, LocalParticipantId, Size))
			log_error("match-report", "invalid match report announcement");
		return;
	}
	const int ChunkIndex = pUnpacker->GetInt();
	const int ChunkSize = pUnpacker->GetInt();
	const unsigned char *pChunk = pUnpacker->Error() ? nullptr : pUnpacker->GetRaw(ChunkSize);
	if(pChunk == nullptr || !Assembler.AddChunk(MatchId, ChunkIndex, pChunk, ChunkSize))
	{
		log_error("match-report", "invalid match report chunk");
		return;
	}
	if(!Assembler.IsComplete())
		return;

	const bool Live = Assembler.IsLive();
	const bool PersistOnDisconnect = Assembler.PersistOnDisconnect();
	CStoredMatch Match;
	std::string Error;
	if(!Assembler.Finish(Match, &Error))
	{
		log_error("match-report", "%s", Error.c_str());
		return;
	}
	Match.m_OriginId = Sessions()->ServerInfo(SessionId).m_aAddress;
	CSessionStatsState &Stats = Session.m_Stats;
	if(Live)
	{
		Stats.SetLiveStats(std::move(Match), PersistOnDisconnect);
		return;
	}
	const bool CurrentMatch = Stats.IsCurrentServerMatch(Match.m_Report);
	const CStoredMatch *pObserved = Stats.ObservedMatchReplacedBy(Match);
	if(!CurrentMatch && !pObserved)
		return;
	StoreMatch(SessionId, Match, pObserved);
	if(CurrentMatch)
		Stats.SetLatestServerMatch(std::move(Match));
	else
		Stats.ClearPreviousObservedMatch();
}

void CGameClient::RequestLiveStats() const
{
	// the statboard shows them, and they are kept when the connection breaks
	const CSessionId SessionId = NetworkSessionId();
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	const int64_t Now = time_get();
	if(!pSession || Sessions()->SessionState(SessionId) != ESessionState::READY || (pSession->m_LastLiveStatsRequest != 0 && Now - pSession->m_LastLiveStatsRequest < time_freq() * 10))
		return;
	CMsgPacker Request(NETMSG_LIVE_STATS_REQUEST, false);
	if(ClientNetwork()->SendMsg(IClient::CONN_MAIN, &Request, MSGFLAG_VITAL) >= 0)
		pSession->m_LastLiveStatsRequest = Now;
}

const CStoredMatch *CGameClient::LiveStats(CSessionId SessionId) const
{
	const CGameSessionContext *pSession = FindSessionContext(SessionId);
	return pSession && pSession->m_Stats.LiveStats().has_value() ? &*pSession->m_Stats.LiveStats() : nullptr;
}

void CGameClient::OnSessionFocused(CSessionId SessionId)
{
	dbg_assert(SessionId == Sessions()->FocusedSessionId(), "focused game session mismatch");
	CGameSessionContext &Session = SessionContext(SessionId);
	for(const auto &pBackgroundSession : m_vpSessionContexts)
	{
		if(pBackgroundSession->Id() == SessionId)
			continue;
		for(CGameState &SessionState : pBackgroundSession->GameStates())
		{
			SessionState.Input().ReleaseGameplay();
			SessionState.ClearPrediction();
		}
	}
	InvalidateSnapshot(SessionId);
	++m_SessionChanges;
	InputView();
	m_SessionPresentations.SetAudible(SessionId);
	if(!Session.m_MapContext.Map()->IsLoaded())
		return;
	m_RaceHelper.Init(this);
	for(auto &pComponent : m_vpAll)
		pComponent->OnMapLoad();
}

void CGameClient::AimView(const CGameSessionContext &Session, const CGameState &State, CGameView &View) const
{
	const CGameState::CSnapState &Snap = State.m_Snap;
	View.SetSpectator(Snap.m_SpecInfo.m_Active, Snap.m_SpecInfo.m_SpectatorId);
	if(Sessions()->SessionType(Session.Id()) == ESessionSourceType::DEMO)
	{
		// A demo is watched the way whoever opened it chose to watch it. A demo
		// being rendered to video in the background is watched the way it was
		// recorded unless the export was told whom to follow, which is what
		// carries the zoom the server sent into the exported frames.
		View.SetSpectatorMode(Session.m_DemoSpecId);
	}
}

// Only the view that takes input owns the client-wide local character position.
void CGameClient::UpdatePositions(CGameState &State, CGameView &View, const CGameTickInfo &Time, float LocalTime, bool Interactive)
{
	CGameView::CMultiViewState &MultiViewState = View.m_MultiView;
	CGameState::CSnapState &Snap = State.m_Snap;
	const int SpectatorMode = View.SpectatorMode();
	// local character position
	const int LocalClientId = State.LocalClientId();
	if(Interactive && in_range(LocalClientId, MAX_CLIENTS - 1) && State.RenderedClient(LocalClientId).m_Active)
		m_LocalCharacterPos = State.RenderedClient(LocalClientId).m_Position;

	// spectator position
	if(Snap.m_SpecInfo.m_Active)
	{
		if(MultiViewState.m_Active)
		{
			HandleMultiView(State, LocalTime);
		}
		else if(Time.m_IsDemoPlayback && SpectatorMode != SPEC_FOLLOW && Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
		{
			Snap.m_SpecInfo.m_Position = mix(
				vec2(Snap.m_aCharacters[Snap.m_SpecInfo.m_SpectatorId].m_Prev.m_X, Snap.m_aCharacters[Snap.m_SpecInfo.m_SpectatorId].m_Prev.m_Y),
				vec2(Snap.m_aCharacters[Snap.m_SpecInfo.m_SpectatorId].m_Cur.m_X, Snap.m_aCharacters[Snap.m_SpecInfo.m_SpectatorId].m_Cur.m_Y),
				Time.m_IntraGameTick);
			Snap.m_SpecInfo.m_UsePosition = true;
		}
		else if(Snap.m_pSpectatorInfo && ((Time.m_IsDemoPlayback && SpectatorMode == SPEC_FOLLOW) || (!Time.m_IsDemoPlayback && Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)))
		{
			if(Snap.m_pPrevSpectatorInfo && Snap.m_pPrevSpectatorInfo->m_SpectatorId == Snap.m_pSpectatorInfo->m_SpectatorId)
				Snap.m_SpecInfo.m_Position = mix(vec2(Snap.m_pPrevSpectatorInfo->m_X, Snap.m_pPrevSpectatorInfo->m_Y),
					vec2(Snap.m_pSpectatorInfo->m_X, Snap.m_pSpectatorInfo->m_Y), Time.m_IntraGameTick);
			else
				Snap.m_SpecInfo.m_Position = vec2(Snap.m_pSpectatorInfo->m_X, Snap.m_pSpectatorInfo->m_Y);
			Snap.m_SpecInfo.m_UsePosition = true;
		}
	}

	if(!MultiViewState.m_Active && MultiViewState.m_IsInit)
		ResetMultiView();
}

CVisibleWorldRect CGameClient::VisibleWorldRectFor(const CGameView &View) const
{
	const CViewport &Viewport = View.Viewport();
	const float Aspect = Viewport.m_Width > 0 && Viewport.m_Height > 0 ? Viewport.m_Width / static_cast<float>(Viewport.m_Height) : Graphics()->ScreenAspect();
	const CScreenRect ScreenRect = Graphics()->MapScreenToWorld(
		View.CameraPosition().x, View.CameraPosition().y, 100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Aspect, View.Zoom());
	return CVisibleWorldRect(ScreenRect.m_TopLeft, ScreenRect.m_BottomRight);
}

const CGameClient::CPreparedRenderEntry &CGameClient::InputRenderEntry() const
{
	const auto It = std::find_if(m_vPreparedRenderEntries.begin(), m_vPreparedRenderEntries.end(), [](const CPreparedRenderEntry &Entry) { return Entry.m_Active; });
	dbg_assert(It != m_vPreparedRenderEntries.end(), "missing render entry that takes input");
	return *It;
}

const CGameClient::CPreparedRenderEntry *CGameClient::FindAudibleRenderEntry() const
{
	const auto It = std::find_if(m_vPreparedRenderEntries.begin(), m_vPreparedRenderEntries.end(), [](const CPreparedRenderEntry &Entry) { return Entry.m_Audible; });
	return It == m_vPreparedRenderEntries.end() ? nullptr : &*It;
}

CGameClient::SRenderComponentInfo CGameClient::RenderComponentInfo(const CComponent *pComponent)
{
	// The trace name, and the GPU zone the component is timed in, if any.
	struct SEntry
	{
		const CComponent *m_pComponent;
		const char *m_pTraceName;
		const char *m_pGpuZone;
	};
	const std::array<SEntry, 39> aEntries = {{
		{&m_Skins, "game/skins", nullptr},
		{&m_Skins7, "game/skins7", nullptr},
		{&m_CountryFlags, "game/country_flags", nullptr},
		{&m_MapImages, "game/map_images", nullptr},
		{&m_Effects, "game/effects", nullptr},
		{&m_Binds, "game/binds", nullptr},
		{&m_Binds.m_SpecialBinds, "game/special_binds", nullptr},
		{&m_Controls, "game/controls", nullptr},
		{&m_Camera, "game/camera", nullptr},
		{&m_Sounds, "game/sounds", nullptr},
		{&m_Voting, "game/voting", nullptr},
		{&m_Particles, "game/particles_update", nullptr},
		{&m_RaceDemo, "game/race_demo", nullptr},
		{&m_Censor, "game/censor", nullptr},
		{&m_Background, "world/background", "map_background"},
		{&m_Particles.m_RenderTrail, "world/particles_trail", "particles"},
		{&m_Particles.m_RenderTrailExtra, "world/particles_trail_extra", "particles"},
		{&m_Items, "world/items", "items"},
		{&m_Ghost, "world/ghost", "ghost"},
		{&m_Players, "world/players", "players"},
		{&m_Particles.m_RenderExplosions, "world/particles_explosions", "particles"},
		{&m_NamePlates, "world/nameplates", "nameplates"},
		{&m_Particles.m_RenderExtra, "world/particles_extra", "particles"},
		{&m_Particles.m_RenderGeneral, "world/particles_general", "particles"},
		{&m_FreezeBars, "world/freezebars", "freezebars"},
		{&m_DamageInd, "world/damage_indicators", "damage_indicators"},
		{&m_Hud, "ui/hud", "hud"},
		{&m_Spectator, "ui/spectator", "spectator"},
		{&m_Emoticon, "ui/emoticon", "emoticon"},
		{&m_InfoMessages, "ui/info_messages", "info_messages"},
		{&m_Chat, "ui/chat", "chat"},
		{&m_Broadcast, "ui/broadcast", "broadcast"},
		{&m_ImportantAlert, "ui/important_alert", "important_alert"},
		{&m_DebugHud, "ui/debug_hud", "debug_hud"},
		{&m_TouchControls, "ui/touch_controls", "touch_controls"},
		{&m_Scoreboard, "ui/scoreboard", "scoreboard"},
		{&m_Statboard, "ui/statboard", "statboard"},
		{&m_Motd, "ui/motd", "motd"},
		{&m_Tooltips, "ui/tooltips", "tooltips"},
	}};
	for(const SEntry &Entry : aEntries)
	{
		if(Entry.m_pComponent == pComponent)
			return {Entry.m_pTraceName, Entry.m_pGpuZone == nullptr ? IGraphics::CGpuRenderZone() : Graphics()->RegisterGpuRenderZone(Entry.m_pGpuZone)};
	}
	if(m_pFrontend != nullptr)
	{
		for(const IGameFrontend::CComponentInfo &Info : m_pFrontend->Components())
		{
			if(Info.m_pComponent == pComponent)
				return {Info.m_pTraceName, Info.m_pGpuZone == nullptr ? IGraphics::CGpuRenderZone() : Graphics()->RegisterGpuRenderZone(Info.m_pGpuZone)};
		}
	}
	return {"game/component", IGraphics::CGpuRenderZone()};
}

const CGameClient::SRenderComponentInfo &CGameClient::RenderInfo(const CComponent *pComponent) const
{
	static const SRenderComponentInfo s_Unknown = {"game/component", IGraphics::CGpuRenderZone()};
	const auto It = m_RenderComponentInfo.find(pComponent);
	return It == m_RenderComponentInfo.end() ? s_Unknown : It->second;
}

void CGameClient::OnRender()
{
	if(m_CoreImagesPending)
	{
		RenderLoading(Localize("Loading DDNet Client"), Localize("Initializing assets"), 0, false);
		return;
	}
	// A video export that is not the session on the screen has no view that
	// takes input: its one entry is the whole frame.
	const CPreparedRenderEntry &PrimaryEntry = m_PreparedIsolatedVideoOutput ? m_vPreparedRenderEntries.front() : InputRenderEntry();
	// What is heard need not be what takes input: the demo in the corner can
	// be heard instead of the server.
	const CPreparedRenderEntry *pAudibleEntry = m_PreparedIsolatedVideoOutput ? &PrimaryEntry : FindAudibleRenderEntry();
	const bool IsVideoOutput = m_PreparedVideoOutput;
	const ColorRGBA ClearColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClOverlayEntities ? g_Config.m_ClBackgroundEntitiesColor : g_Config.m_ClBackgroundColor));
	const bool NoGame = Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK;
	if(m_PreparedIsolatedVideoOutput || !m_Backdrop.Begin(ClearColor, NoGame || BackdropWanted()))
		Graphics()->Clear(ClearColor.r, ClearColor.g, ClearColor.b);
	auto RenderInView = [this](const CViewport &Viewport, const auto &Render) {
		const bool CustomViewport = Viewport.m_Width > 0 && Viewport.m_Height > 0;
		if(CustomViewport)
			Graphics()->UpdateViewport(Viewport.m_X, Viewport.m_Y, Viewport.m_Width, Viewport.m_Height, false);
		Render();
		if(CustomViewport)
			Graphics()->UpdateViewport(0, 0, Graphics()->ScreenWidth(), Graphics()->ScreenHeight(), false);
	};
	CRenderTrace *pTrace = m_pRenderTrace;
	auto RenderTraced = [this, pTrace](const SRenderComponentInfo &Info, const auto &Render) {
		CRenderTraceScope TraceScope(pTrace, Info.m_pTraceName, Info.m_GpuZone);
		Graphics()->GpuRenderZoneBegin(Info.m_GpuZone);
		Render();
		Graphics()->GpuRenderZoneEnd(Info.m_GpuZone);
	};

	if(m_PreparedOfflineVideoAudio)
	{
		// The export records into the offline mixer on the demo's clock; the
		// live mixer is left to whatever is on the screen.
		if(pAudibleEntry != nullptr && pAudibleEntry->m_Audible && pAudibleEntry->m_Time.m_IsGameActive)
		{
			m_Sounds.Update(pAudibleEntry->m_pView->CameraPosition(), pAudibleEntry->m_Time.m_PresentationTime, true);
			SessionPresentation(pAudibleEntry->m_pSession->Id()).UpdateMapSounds(*pAudibleEntry->m_pState, pAudibleEntry->m_Time, *pAudibleEntry->m_pView, UsePredictedEnvelopeTime(pAudibleEntry->m_Time, *pAudibleEntry->m_pView), true);
		}
	}
	else
	{
		const bool Audible = pAudibleEntry != nullptr && pAudibleEntry->m_Audible && pAudibleEntry->m_Time.m_IsGameActive;
		m_Sounds.Update(Audible ? std::optional(pAudibleEntry->m_pView->CameraPosition()) : std::nullopt, time_get());
		m_SessionPresentations.SetAudible(Audible ? pAudibleEntry->m_pSession->Id() : CSessionId());
		if(Audible)
			SessionPresentation(pAudibleEntry->m_pSession->Id()).UpdateMapSounds(*pAudibleEntry->m_pState, pAudibleEntry->m_Time, *pAudibleEntry->m_pView, UsePredictedEnvelopeTime(pAudibleEntry->m_Time, *pAudibleEntry->m_pView));
	}

	std::vector<CRenderContext> vContexts;
	vContexts.reserve(m_vPreparedRenderEntries.size());
	for(const CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
	{
		const CPresentationContext Context(*Entry.m_pSession, *Entry.m_pState, Entry.m_Time, {&Entry.m_VisibleWorldRect, 1}, Entry.m_Playback, Entry.m_Audible ? EPresentationAudio::AUDIBLE : EPresentationAudio::MUTED);
		SessionPresentation(Context.m_Session.Id()).UpdateClients(Context);
		m_Effects.Update(Context);
		m_Particles.Update(Context);
		m_DamageInd.Update(Context);
		m_Items.UpdatePresentation(Context);
		m_Ghost.UpdatePresentation(Context);
		m_Players.UpdatePresentation(Context);
		vContexts.emplace_back(*Entry.m_pSession, *Entry.m_pState, *Entry.m_pView, Entry.m_Time, Entry.m_VisibleWorldRect, IsVideoOutput, m_PreparedVideoSettings);
	}

	auto RenderWorld = [&](const CRenderContext &Context) {
		const bool UsePredictedTime = UsePredictedEnvelopeTime(Context.m_Time, Context.m_View);
		CSessionPresentation &Presentation = SessionPresentation(Context.m_Session.Id());
		if(Context.m_Time.m_IsGameActive)
			Presentation.PrepareRender(Context, UsePredictedTime);
		if(!m_Background.UsesCurrentMap())
			m_Background.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
		const std::array<std::pair<CComponent *, const SRenderComponentInfo *>, 13> aWorldComponents = {{
			{Presentation.MapLayersBackground(), &m_MapBackgroundRenderInfo},
			{&m_Particles.m_RenderTrail, &RenderInfo(&m_Particles.m_RenderTrail)},
			{&m_Particles.m_RenderTrailExtra, &RenderInfo(&m_Particles.m_RenderTrailExtra)},
			{&m_Items, &RenderInfo(&m_Items)},
			{&m_Ghost, &RenderInfo(&m_Ghost)},
			{&m_Players, &RenderInfo(&m_Players)},
			{Presentation.MapLayersForeground(), &m_MapForegroundRenderInfo},
			{&m_Particles.m_RenderExplosions, &RenderInfo(&m_Particles.m_RenderExplosions)},
			{&m_NamePlates, &RenderInfo(&m_NamePlates)},
			{&m_Particles.m_RenderExtra, &RenderInfo(&m_Particles.m_RenderExtra)},
			{&m_Particles.m_RenderGeneral, &RenderInfo(&m_Particles.m_RenderGeneral)},
			{&m_FreezeBars, &RenderInfo(&m_FreezeBars)},
			{&m_DamageInd, &RenderInfo(&m_DamageInd)},
		}};
		RenderInView(Context.m_View.Viewport(), [&]() {
			if(Context.m_View.IsInset())
			{
				// The screen was cleared once, before the views under this one
				// were drawn, so an inset clears its own rectangle.
				Graphics()->MapScreenToSize(1.0f, 1.0f);
				Graphics()->TextureClear();
				Graphics()->QuadsBegin();
				Graphics()->SetColor(ClearColor);
				const IGraphics::CQuadItem Quad(0.0f, 0.0f, 1.0f, 1.0f);
				Graphics()->QuadsDrawTL(&Quad, 1);
				Graphics()->QuadsEnd();
			}
			if(g_Config.m_ClOverlayEntities == 100)
			{
				RenderTraced(RenderInfo(&m_Background), [&]() {
					if(!m_Background.UsesCurrentMap())
						m_Background.OnRender(Context);
					else if(Presentation.IsLoaded())
						Presentation.MapLayersBackgroundForce()->OnRender(Context);
				});
			}
			for(const auto &Component : aWorldComponents)
			{
				if(Component.first != nullptr)
					RenderTraced(*Component.second, [&]() { Component.first->OnRender(Context); });
			}
			if(Context.m_View.IsInset())
			{
				// A frame, so that the picture does not run into what it covers.
				const vec2 Size = Graphics()->ViewportSize();
				const float Border = std::max(1.0f, std::round(Size.y / 150.0f));
				Graphics()->MapScreenToSize(Size.x, Size.y);
				Graphics()->TextureClear();
				Graphics()->QuadsBegin();
				Graphics()->SetColor(1.0f, 1.0f, 1.0f, 0.5f);
				const IGraphics::CQuadItem aFrame[] = {
					{0.0f, 0.0f, Size.x, Border},
					{0.0f, Size.y - Border, Size.x, Border},
					{0.0f, Border, Border, Size.y - 2 * Border},
					{Size.x - Border, Border, Border, Size.y - 2 * Border}};
				Graphics()->QuadsDrawTL(aFrame, std::size(aFrame));
				Graphics()->QuadsEnd();
			}
		});
	};
	// An inset is drawn later, over the views below it.
	auto RenderInsets = [&]() {
		Graphics()->GpuRenderZoneEnd(m_GpuZoneInterface);
		Graphics()->GpuRenderZoneBegin(m_GpuZoneWorld);
		for(const CRenderContext &Context : vContexts)
		{
			if(Context.m_View.IsInset())
				RenderWorld(Context);
		}
		Graphics()->GpuRenderZoneEnd(m_GpuZoneWorld);
		Graphics()->GpuRenderZoneBegin(m_GpuZoneInterface);
	};
	const int SessionChangesAtStart = m_SessionChanges;
	Graphics()->GpuRenderZoneBegin(m_GpuZoneWorld);
	for(const CRenderContext &Context : vContexts)
	{
		if(!Context.m_View.IsInset())
			RenderWorld(Context);
	}
	// The HUD is the first thing drawn over the world.
	Graphics()->GpuRenderZoneEnd(m_GpuZoneWorld);
	Graphics()->GpuRenderZoneBegin(m_GpuZoneInterface);

	// An inset shows the world alone, without the HUD and boards that belong
	// to the view it sits on.
	auto RenderComponents = [&](std::initializer_list<CComponent *> vpComponents) {
		for(const CRenderContext &Context : vContexts)
		{
			if(Context.m_View.IsInset())
				continue;
			RenderInView(Context.m_View.Viewport(), [&]() {
				for(CComponent *pComponent : vpComponents)
					RenderTraced(RenderInfo(pComponent), [&]() { pComponent->OnRender(Context); });
			});
		}
	};
	// The overlays that only exist once are drawn in the view that takes input.
	// An isolated export has none: it shows what the demo shows and nothing of
	// the client that renders it.
	const bool Interactive = !m_PreparedIsolatedVideoOutput;
	const CRenderContext InputContext(*PrimaryEntry.m_pSession, *PrimaryEntry.m_pState, *PrimaryEntry.m_pView, PrimaryEntry.m_Time, PrimaryEntry.m_VisibleWorldRect, IsVideoOutput, m_PreparedVideoSettings);
	const CViewport &InputViewport = InputContext.m_View.Viewport();

	RenderComponents({&m_InfoMessages, &m_Hud});
	if(Interactive)
	{
		RenderInView(InputViewport, [&]() {
			RenderTraced(RenderInfo(&m_Spectator), [&]() { m_Spectator.OnRender(InputContext); });
			RenderTraced(RenderInfo(&m_Emoticon), [&]() { m_Emoticon.OnRender(InputContext); });
			RenderTraced(RenderInfo(&m_Chat), [&]() { m_Chat.RenderApplicationOverlay(InputContext); });
		});
	}
	RenderComponents({&m_Chat});
	RenderComponents({&m_Broadcast, &m_DebugHud});
	// Over the HUD and chat of the views below, but still part of the scene
	// the menu blurs and the boards cover.
	if(!m_PreparedMenuPreview)
		RenderInsets();
	if(Interactive)
	{
		RenderInView(InputViewport, [&]() {
			RenderTraced(RenderInfo(&m_ImportantAlert), [&]() { m_ImportantAlert.OnRender(InputContext); });
			RenderTraced(RenderInfo(&m_TouchControls), [&]() {
				m_TouchControls.OnRender(InputContext);
				m_TouchControls.RenderApplicationOverlay();
			});
		});
		if(m_Backdrop.DrawingScene())
		{
			// Without a game the menus show their own background, blurred
			// like the game would be.
			if(NoGame && m_pFrontend != nullptr)
				m_pFrontend->RenderSceneBackground();
			// The console blurs its own picture later, so a frame where it
			// is the only thing over the scene does not need the scene
			// blurred at all.
			m_Backdrop.Finish(NoGame || SceneBackdropWanted());
		}
	}
	m_Scoreboard.BeginRenderFrame();
	RenderComponents({&m_Scoreboard});
	if(Interactive)
	{
		RenderInView(InputViewport, [&]() {
			RenderTraced(RenderInfo(&m_Scoreboard), [&]() { m_Scoreboard.RenderApplicationOverlay(InputContext); });
		});
	}
	RenderComponents({&m_Statboard, &m_Motd});
	if(!Interactive)
	{
		Graphics()->GpuRenderZoneEnd(m_GpuZoneInterface);
		return;
	}
	// After the backdrop, so that opening the scoreboard does not smear the
	// cursor along with the scene behind it, and after the boards that blur
	// it, because a crosshair that is aimed through has to be on top of what
	// it is aimed through. The menu and the console still cover it: they
	// take the mouse over and bring their own pointer.
	for(const CRenderContext &Context : vContexts)
	{
		if(!Context.m_View.IsInset())
			RenderInView(Context.m_View.Viewport(), [&]() { m_Hud.RenderCursor(Context); });
	}
	auto RenderOverlays = [&](IGameFrontend::ESlot Slot) {
		if(m_pFrontend == nullptr)
			return;
		for(CComponent *pComponent : m_pFrontend->Slot(Slot))
			RenderTraced(RenderInfo(pComponent), [&]() { pComponent->OnRenderApplicationOverlay(); });
	};
	RenderOverlays(IGameFrontend::ESlot::OVERLAY_BELOW_TOOLTIPS);
	// The demo browser's picture goes over the menu. A click in the menu may
	// have moved the focus or closed the demo since the frame was prepared,
	// and the views no longer show what they were prepared for then.
	if(m_PreparedMenuPreview && SessionChangesAtStart == m_SessionChanges)
		RenderInsets();
	RenderTraced(RenderInfo(&m_Tooltips), [&]() { m_Tooltips.OnRenderApplicationOverlay(); });
	RenderOverlays(IGameFrontend::ESlot::OVERLAY_ABOVE_TOOLTIPS);

	// Nothing captured what was drawn over the scene, so it goes to the screen
	// as it is.
	m_Backdrop.Present();

	{
		CRenderTraceScope TraceScope(pTrace, "ui/line_input");
		CLineInput::RenderCandidates();
	}
	Graphics()->GpuRenderZoneEnd(m_GpuZoneInterface);
}

void CGameClient::FillPreparedRenderEntry(CPreparedRenderEntry &Entry, int64_t PresentationTime) const
{
	const CSessionId SessionId = Entry.m_pSession->Id();
	const CSessionId StateId = Entry.m_pState->m_SessionId;
	const bool DemoPlayback = Sessions()->SessionType(SessionId) == ESessionSourceType::DEMO;
	const bool WorldPaused = Entry.m_pState->HasGameInfo() && (Entry.m_pState->GameInfo().m_GameStateFlags & (GAMESTATEFLAG_GAMEOVER | GAMESTATEFLAG_PAUSED)) != 0;
	const bool DemoPaused = DemoPlayback && Sessions()->DemoPlaybackPaused(SessionId);
	CGameTickInfo &Time = Entry.m_Time;
	Time.m_PrevGameTick = Sessions()->PrevGameTick(StateId);
	Time.m_GameTick = Sessions()->GameTick(StateId);
	Time.m_PredGameTick = Sessions()->PredGameTick(StateId);
	Time.m_PredictionTick = Sessions()->GetPredictionTick(StateId);
	Time.m_IntraGameTick = Sessions()->IntraGameTick(StateId);
	Time.m_IntraGameTickSincePrev = Sessions()->IntraGameTickSincePrev(StateId);
	Time.m_PredIntraGameTick = Sessions()->PredIntraGameTick(StateId);
	Time.m_GameTickTime = Sessions()->GameTickTime(StateId);
	Time.m_FrameTimeAverage = Client()->FrameTimeAverage();
	Time.m_GameTickSpeed = Sessions()->GameTickSpeed();
	Time.m_PredictionTime = Sessions()->GetPredictionTime(StateId);
	Time.m_PresentationTime = PresentationTime;
	Time.m_PresentationTimeFrequency = time_freq();
	Time.m_AnimationPlaybackSpeed = WorldPaused || DemoPaused ? 0.0f : DemoPlayback ? Sessions()->DemoPlaybackSpeed(SessionId) :
											  1.0f;
	Time.m_IsGameActive = Sessions()->SessionState(StateId) == ESessionState::READY;
	Time.m_IsDemoPlayback = DemoPlayback;
	Time.m_IsDemoPlaybackPaused = DemoPaused;
	Time.m_ConnectionProblems = ClientNetwork() != nullptr && ClientNetwork()->ConnectionProblems(StateId);
	Entry.m_Playback = Time.m_AnimationPlaybackSpeed > 0.0f ? EPresentationPlayback::PLAYING : EPresentationPlayback::PAUSED;
}

void CGameClient::PrepareScreenRender(bool VideoOutput)
{
	m_PreparedVideoOutput = VideoOutput;
	m_PreparedIsolatedVideoOutput = false;
	m_PreparedOfflineVideoAudio = false;
	m_vPreparedRenderEntries.clear();
	if(m_CoreImagesPending)
		return;
	m_vPreparedRenderEntries.reserve(3);

	CGameSessionContext &ActiveSession = SessionContext();
	CGameState &ActiveState = InputState();
	CGameView &View = InputView();

	for(const auto &pContext : m_vpSessionContexts)
		for(CGameState &State : pContext->GameStates())
			State.SetShown(false);
	auto AddEntry = [&](CGameSessionContext &Session, CGameState &State, bool Inset) {
		State.SetShown(true);
		CPreparedRenderEntry Entry;
		Entry.m_pSession = &Session;
		Entry.m_pState = &State;
		Entry.m_pView = &GameView(State.m_SessionId);
		Entry.m_Active = Entry.m_pView == &View;
		bool OfflineAudio;
		Entry.m_Audible = AudioForState(State, OfflineAudio);
		Entry.m_Inset = Inset;
		m_vPreparedRenderEntries.push_back(Entry);
	};
	// The split screen shows the dummy beside the player. An inset only ever
	// shows the seat that is played.
	const bool SplitScreen = g_Config.m_ClDummySplitScreen != 0 && !VideoOutput;
	auto AddSession = [&](CGameSessionContext &Session, bool Inset) {
		if(!Inset && SplitScreen && Session.Id() == NetworkSessionId() && DummyConnected())
		{
			AddEntry(Session, Session.SeatState(IClient::CONN_MAIN), false);
			AddEntry(Session, Session.SeatState(IClient::CONN_DUMMY), false);
		}
		else
		{
			AddEntry(Session, Session.GameState(PlayedSessionId(Session.Id())), Inset);
		}
	};

	// The demo browser shows a demo that plays out of sight in a picture of
	// its own, drawn over the menu.
	const CSessionId DemoId = Sessions()->DemoSessionId();
	const CUIRect MenuPreview = m_pFrontend != nullptr ? m_pFrontend->TakeDemoPreview() : CUIRect{0.0f, 0.0f, 0.0f, 0.0f};
	m_PreparedMenuPreview = !VideoOutput && MenuActive() && MenuPreview.w > 0.0f && ActiveSession.Id() != DemoId && Sessions()->IsSessionShowable(DemoId);
	// The next session with something to show is shown too: beside the
	// focused one on the split screen, or in a corner of it. Sessions keep the
	// order they were opened in, so moving the focus between them does not
	// move them around.
	const bool PictureInPicture = (g_Config.m_ClPictureInPicture != 0 || m_PreparedMenuPreview) && !VideoOutput;
	CGameSessionContext *pOther = m_PreparedMenuPreview ? &SessionContext(DemoId) : nullptr;
	if(pOther == nullptr && (SplitScreen || PictureInPicture))
		pOther = FindSessionContext(OtherShownSessionId());
	const bool OtherBeside = pOther != nullptr && !PictureInPicture;
	for(const auto &pContext : m_vpSessionContexts)
	{
		if(pContext.get() == &ActiveSession || (OtherBeside && pContext.get() == pOther))
			AddSession(*pContext, false);
	}
	if(pOther != nullptr && PictureInPicture)
		AddSession(*pOther, true);

	const int ScreenWidth = Graphics()->ScreenWidth();
	const int ScreenHeight = Graphics()->ScreenHeight();
	const int NumColumns = std::count_if(m_vPreparedRenderEntries.begin(), m_vPreparedRenderEntries.end(), [](const CPreparedRenderEntry &Entry) { return !Entry.m_Inset; });
	const CUIRect &UiScreen = *Ui()->Screen();
	m_PreparedInset = {0.0f, 0.0f, 0.0f, 0.0f};
	int Column = 0;
	for(CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
	{
		if(Entry.m_Inset)
		{
			CViewport Inset;
			if(m_PreparedMenuPreview)
			{
				Inset = {(int)((MenuPreview.x - UiScreen.x) * ScreenWidth / UiScreen.w), (int)((MenuPreview.y - UiScreen.y) * ScreenHeight / UiScreen.h),
					(int)(MenuPreview.w * ScreenWidth / UiScreen.w), (int)(MenuPreview.h * ScreenHeight / UiScreen.h)};
			}
			else
			{
				// In the shape of the screen, in the corner furthest from the
				// chat, the HUD and the kill messages.
				const int Width = ScreenWidth * g_Config.m_ClPictureInPictureSize / 100;
				const int Height = Width * ScreenHeight / std::max(ScreenWidth, 1);
				const int Margin = ScreenHeight / 50;
				Inset = {ScreenWidth - Width - Margin, ScreenHeight - Height - Margin, Width, Height};
			}
			Entry.m_pView->SetViewport(Inset, true);
			m_PreparedInset = {UiScreen.x + Inset.m_X * UiScreen.w / ScreenWidth, UiScreen.y + Inset.m_Y * UiScreen.h / ScreenHeight,
				Inset.m_Width * UiScreen.w / ScreenWidth, Inset.m_Height * UiScreen.h / ScreenHeight};
		}
		else
		{
			const int Left = ScreenWidth * Column / NumColumns;
			const int Right = ScreenWidth * (Column + 1) / NumColumns;
			Entry.m_pView->SetViewport(NumColumns > 1 ? CViewport{Left, 0, Right - Left, ScreenHeight} : CViewport{});
			++Column;
		}
	}

	// A recording shows the focused session on the clock of its video.
	const int64_t PresentationTime = VideoOutput ? Sessions()->DemoPlaybackTime(ActiveSession.Id()) : time_get();
	const int64_t Now = time_get();
	for(CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
		FillPreparedRenderEntry(Entry, PresentationTime);

	// Views are aimed before the controllers run, they read where a view looks.
	for(CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
		AimView(*Entry.m_pSession, *Entry.m_pState, *Entry.m_pView);

	const CGameTickInfo &ActiveTime = InputRenderEntry().m_Time;
	m_ControllerLocalTime = VideoOutput ? Sessions()->DemoPlaybackLocalTime(ActiveSession.Id()) : Client()->LocalTime();
	const CRenderContext ControllerContext(ActiveSession, ActiveState, View, ActiveTime, CVisibleWorldRect(vec2(), vec2()));
	m_Spectator.UpdateController(View, ControllerContext, m_ControllerLocalTime);
	m_Emoticon.UpdateController(View, ControllerContext);
	m_Chat.UpdateController(ControllerContext);
	m_Statboard.UpdateController();
	m_Scoreboard.PrepareApplicationOverlay(ControllerContext);

	for(CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
		UpdateRenderedClients(*Entry.m_pSession, *Entry.m_pState, Now, Entry.m_Time, Entry.m_Playback);

	CGameView::CMultiViewState &MultiViewState = MultiView();
	if(!MultiViewState.m_IsInit && MultiViewState.m_Active)
	{
		int TeamId = 0;
		if(Snap().m_SpecInfo.m_SpectatorId >= 0)
			TeamId = ActiveState.Teams().Team(Snap().m_SpecInfo.m_SpectatorId);
		if(TeamId > MAX_CLIENTS || TeamId < 0)
			TeamId = 0;
		if(!InitMultiView(ActiveState, TeamId))
			ResetMultiView();
	}

	// Only the view that takes input may move the mouse or run the controls.
	for(CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
	{
		m_Camera.BindTarget(*Entry.m_pSession, *Entry.m_pState, *Entry.m_pView, Entry.m_Active, m_ControllerLocalTime);
		UpdatePositions(*Entry.m_pState, *Entry.m_pView, Entry.m_Time, m_ControllerLocalTime, Entry.m_Active);
		m_Camera.UpdateCamera();
		if(Entry.m_Active)
			m_Controls.Update();
		m_Camera.UpdatePosition();
	}
	m_Camera.BindTarget(ActiveSession, ActiveState, View, true, m_ControllerLocalTime);
	UpdateSpectatorCursor(ActiveState, ActiveTime);

	for(CPreparedRenderEntry &Entry : m_vPreparedRenderEntries)
		Entry.m_VisibleWorldRect = VisibleWorldRectFor(*Entry.m_pView);

	if(m_pFrontend != nullptr && m_pFrontend->CanDisplayWarning())
	{
		std::optional<SWarning> Warning = Graphics()->CurrentWarning();
		if(!Warning.has_value())
			Warning = Client()->CurrentWarning();
		if(Warning.has_value())
		{
			const SWarning &TheWarning = Warning.value();
			m_pFrontend->PopupWarning(TheWarning.m_aWarningTitle[0] == '\0' ? Localize("Warning") : TheWarning.m_aWarningTitle, TheWarning.m_aWarningMsg, Localize("Ok"), TheWarning.m_AutoHide ? 10s : 0s);
		}
	}
}

void CGameClient::OnRenderPrepare()
{
	PrepareScreenRender(false);
}

#if defined(CONF_VIDEORECORDER)
void CGameClient::OnRenderVideoPrepare(CSessionId SessionId, const CVideoExportSettings &Settings)
{
	m_PreparedVideoSettings = Settings;
	if(SessionId == Sessions()->FocusedSessionId() && !Client()->VideoUsesOfflineAudio())
	{
		PrepareScreenRender(true);
		return;
	}
	m_PreparedVideoOutput = true;
	m_PreparedIsolatedVideoOutput = SessionId != Sessions()->FocusedSessionId();
	m_PreparedOfflineVideoAudio = Client()->VideoUsesOfflineAudio();
	m_vPreparedRenderEntries.clear();
	if(m_CoreImagesPending)
		return;

	CGameSessionContext &Session = SessionContext(SessionId);
	CGameView &View = m_VideoView;
	View.SetTarget(SessionId);
	View.SetViewport({});
	CPreparedRenderEntry Entry;
	Entry.m_pSession = &Session;
	Entry.m_pState = &Session.GameState(SessionId);
	Entry.m_pView = &View;
	bool OfflineAudio;
	Entry.m_Audible = AudioForSession(SessionId, OfflineAudio);
	dbg_assert(!Entry.m_Audible || OfflineAudio == Client()->VideoUsesOfflineAudio(), "video audio routed to wrong mixer");
	FillPreparedRenderEntry(Entry, Sessions()->DemoPlaybackTime(SessionId));
	UpdateRenderedClients(Session, *Entry.m_pState, time_get(), Entry.m_Time, Entry.m_Playback);

	// The export gets the same camera as any other view, on the demo's clock
	// rather than the wall clock the frames take to write. It takes no input, so
	// it never moves a mouse position or runs the controls.
	const float LocalTime = Sessions()->DemoPlaybackLocalTime(SessionId);
	AimView(Session, *Entry.m_pState, View);
	m_Camera.BindTarget(Session, *Entry.m_pState, View, false, LocalTime);
	UpdatePositions(*Entry.m_pState, View, Entry.m_Time, LocalTime, false);
	m_Camera.UpdateCamera();
	m_Camera.UpdatePosition();
	Entry.m_VisibleWorldRect = VisibleWorldRectFor(View);
	m_vPreparedRenderEntries.push_back(Entry);

	// Leave the camera on the view that takes input, so a console command or a
	// question about the zoom between frames does not land on the export.
	CGameSessionContext &FocusedSession = SessionContext();
	m_Camera.BindTarget(FocusedSession, FocusedSession.GameState(InputSessionId()), InputView(), true, Client()->LocalTime());
}
#endif

void CGameClient::OnRenderFinalize()
{
	if(!m_PreparedIsolatedVideoOutput && !m_vPreparedRenderEntries.empty())
	{
		CGameView &View = *InputRenderEntry().m_pView;
		m_Spectator.CommitController(View, m_ControllerLocalTime);
	}
	m_vPreparedRenderEntries.clear();
	if(!m_PreparedIsolatedVideoOutput)
		Input()->Clear();
	m_PreparedVideoOutput = false;
	m_PreparedVideoSettings = {};
	m_PreparedIsolatedVideoOutput = false;
	m_PreparedOfflineVideoAudio = false;
}

#if defined(CONF_VIDEORECORDER)
bool CGameClient::OnRenderVideoProgress(bool Overlay)
{
	return m_pFrontend != nullptr && m_pFrontend->RenderVideoProgress(Overlay);
}
#endif

void CGameClient::OnDummyDisconnect()
{
	GameState(DummySessionId()).Reset();
}

int CGameClient::LastRaceTick() const
{
	return InputState().m_Runtime.m_LastRaceTick;
}

int CGameClient::CurrentRaceTime() const
{
	const int RaceTick = LastRaceTick();
	if(RaceTick < 0)
	{
		return 0;
	}
	return (Sessions()->GameTick(InputSessionId()) - RaceTick) / Sessions()->GameTickSpeed();
}

bool CGameClient::ReceivedDDNetPlayer() const
{
	return InputState().m_Runtime.m_ReceivedDDNetPlayer;
}

bool CGameClient::IsTeamPlay() const
{
	return Snap().m_pGameInfoObj &&
	       (Snap().m_pGameInfoObj->m_GameFlags & GAMEFLAG_TEAMS) != 0;
}

int CGameClient::MinTeamSize() const
{
	// old servers only expose it if the map settings happen to contain it
	return FocusedGameInfo().m_MinTeamSize != 0 ? FocusedGameInfo().m_MinTeamSize : GameConfig()->m_SvMinTeamSize;
}

int CGameClient::MaxTeamSize() const
{
	// old servers only expose it if the map settings happen to contain it
	return FocusedGameInfo().m_MaxTeamSize != 0 ? FocusedGameInfo().m_MaxTeamSize : GameConfig()->m_SvMaxTeamSize;
}

bool CGameClient::IsWorldPaused() const
{
	return Snap().m_pGameInfoObj &&
	       (Snap().m_pGameInfoObj->m_GameStateFlags & (GAMESTATEFLAG_GAMEOVER | GAMESTATEFLAG_PAUSED)) != 0;
}

bool CGameClient::IsDemoPlaybackPaused() const
{
	return Client()->IsDemoPlayback() &&
	       DemoPlayer()->BaseInfo()->m_Paused;
}

int CGameClient::AntiPingPlayers() const
{
	if(g_Config.m_ClAntiPing &&
		g_Config.m_ClAntiPingPlayers &&
		!Snap().m_SpecInfo.m_Active &&
		!Client()->IsDemoPlayback())
	{
		return g_Config.m_ClAntiPingPlayers;
	}
	return 0;
}

bool CGameClient::AntiPingGrenade() const
{
	return g_Config.m_ClAntiPing &&
	       g_Config.m_ClAntiPingGrenade &&
	       !Snap().m_SpecInfo.m_Active &&
	       !Client()->IsDemoPlayback();
}

bool CGameClient::AntiPingWeapons() const
{
	return g_Config.m_ClAntiPing &&
	       g_Config.m_ClAntiPingWeapons &&
	       !Snap().m_SpecInfo.m_Active &&
	       !Client()->IsDemoPlayback();
}

bool CGameClient::Predict() const
{
	return g_Config.m_ClPredict &&
	       !IsWorldPaused() &&
	       !Client()->IsDemoPlayback() &&
	       !Snap().m_SpecInfo.m_Active &&
	       Snap().m_pLocalCharacter;
}

bool CGameClient::PredictDummy(const CGameState &OtherState) const
{
	if(!g_Config.m_ClPredictDummy || !DummyConnected() || Snap().m_LocalClientId < 0)
		return false;
	const int OtherLocalClientId = OtherState.LocalClientId();
	if(OtherLocalClientId < 0)
		return false;
	const CGameState::CClientSnapshot &OtherLocalClient = OtherState.Client(OtherLocalClientId);
	return !OtherLocalClient.m_HasDDNetPlayer || (OtherLocalClient.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_PAUSED) == 0;
}

ColorRGBA CGameClient::GetDDTeamColor(int DDTeam, float Lightness) const
{
	// Use golden angle to generate unique colors with distinct adjacent colors.
	// The first DDTeam (team 1) gets angle 0°, i.e. red hue.
	const float Hue = std::fmod((DDTeam - 1) * normalized_golden_angle, 1.0f);
	return color_cast<ColorRGBA>(ColorHSLA(Hue, 1.0f, Lightness));
}

void CGameClient::FormatClientId(int ClientId, char (&aClientId)[16], EClientIdFormat Format) const
{
	if(Format == EClientIdFormat::NO_INDENT)
	{
		str_format(aClientId, sizeof(aClientId), "%d", ClientId);
	}
	else
	{
		const int HighestClientId = Format == EClientIdFormat::INDENT_AUTO ? Snap().m_HighestClientId : 64;
		FormatClientId(ClientId, aClientId, HighestClientId);
		return;
	}
	str_append(aClientId, ": ");
}

void CGameClient::FormatClientId(int ClientId, char (&aClientId)[16], int HighestClientId) const
{
	const char *pFigureSpace = " ";
	char aNumber[8];
	str_format(aNumber, sizeof(aNumber), "%d", ClientId);
	aClientId[0] = '\0';
	if(ClientId < 100 && HighestClientId >= 100)
		str_append(aClientId, pFigureSpace);
	if(ClientId < 10 && HighestClientId >= 10)
		str_append(aClientId, pFigureSpace);
	str_append(aClientId, aNumber);
	str_append(aClientId, ": ");
}

void CGameClient::OnRelease()
{
	// release all systems
	for(auto &pComponent : m_vpAll)
		pComponent->OnRelease();
}

void CGameClient::OnMessage(CSessionId SessionId, int MsgId, CUnpacker *pUnpacker)
{
	CGameSessionContext &MessageSession = SessionContext(SessionId);
	CGameState &MessageState = MessageSession.GameState(SessionId);
	// What belongs to the server is taken from the player's seat only.
	const bool DummyConnection = MessageState.m_Seat != IClient::CONN_MAIN;
	const CSessionId ContextId = MessageSession.Id();
	const bool Focused = ContextId == Sessions()->FocusedSessionId();
	const bool SuppressEvents = m_SuppressEvents && ContextId == Sessions()->DemoSessionId();
	const int64_t MessageTime = SessionMessageTime(ContextId);
	if(MsgId == NETMSG_MATCH_REPORT_START || MsgId == NETMSG_MATCH_REPORT_CHUNK)
	{
		if(!DummyConnection && Sessions()->SessionType(SessionId) == ESessionSourceType::NETWORK)
			HandleMatchReportMessage(SessionId, MsgId, pUnpacker);
		return;
	}

	// special messages
	static_assert((int)NETMSGTYPE_SV_TUNEPARAMS == (int)protocol7::NETMSGTYPE_SV_TUNEPARAMS, "0.6 and 0.7 tune message id do not match");
	if(MsgId == NETMSGTYPE_SV_TUNEPARAMS)
	{
		// unpack the new tuning
		CTuningParams NewTuning;

		// No jetpack on DDNet incompatible servers,
		// jetpack strength will be received by tune params
		NewTuning.m_JetpackStrength = 0;

		int *pParams = NewTuning.NetworkArray();
		for(int i = 0; i < CTuningParams::Num(); i++)
		{
			static_assert(offsetof(CTuningParams, m_LaserDamage) / sizeof(CTuneParam) == 30);
			if(i == 30 && Sessions()->IsSixup(SessionId)) // laser_damage was removed in 0.7
			{
				continue;
			}

			const int Value = pUnpacker->GetInt();

			// check for unpacking errors
			if(pUnpacker->Error())
				break;

			pParams[i] = Value;
		}

		MessageState.m_Runtime.m_ServerMode = CGameState::SERVERMODE_PURE;

		MessageState.m_Runtime.m_ReceivedTuning = true;
		// apply new tuning
		MessageState.ApplyTuning(NewTuning);
		return;
	}

	void *pRawMsg = TranslateGameMsg(SessionId, &MsgId, pUnpacker);

	if(!pRawMsg)
	{
		// the 0.7 version of this error message is printed on translation
		// in sixup/translate_game.cpp
		if(!Sessions()->IsSixup(SessionId))
		{
			log_debug("client", "dropped weird message '%s' (%d), failed on '%s'",
				m_NetObjHandler.GetMsgName(MsgId), MsgId, m_NetObjHandler.FailedMsgOn());
		}
		return;
	}
	if(MsgId == NETMSGTYPE_SV_CHANGEINFOCOOLDOWN)
	{
		CNetMsg_Sv_ChangeInfoCooldown *pMsg = (CNetMsg_Sv_ChangeInfoCooldown *)pRawMsg;
		MessageState.m_Runtime.m_NextChangeInfo = pMsg->m_WaitUntil;
		return;
	}

	if(MsgId == NETMSGTYPE_SV_DDRACETIME || MsgId == NETMSGTYPE_SV_DDRACETIMELEGACY)
	{
		const CNetMsg_Sv_DDRaceTime *pMsg = static_cast<const CNetMsg_Sv_DDRaceTime *>(pRawMsg);
		MessageState.m_RaceMessages.ApplyDDRaceTime(pMsg->m_Time, pMsg->m_Check, pMsg->m_Finish != 0, Sessions()->GameTick(SessionId));
	}
	else if(MsgId == NETMSGTYPE_SV_RECORD || MsgId == NETMSGTYPE_SV_RECORDLEGACY)
	{
		const CNetMsg_Sv_Record *pMsg = static_cast<const CNetMsg_Sv_Record *>(pRawMsg);
		if(MsgId == NETMSGTYPE_SV_RECORDLEGACY && MessageState.CoreGameInfo().m_DDRaceRecordMessage)
		{
			MessageState.m_RaceMessages.ApplyLegacyRecord(pMsg->m_ServerTimeBest, pMsg->m_PlayerTimeBest, Sessions()->GameTick(SessionId));
		}
		else if(MsgId == NETMSGTYPE_SV_RECORD || MessageState.CoreGameInfo().m_RaceRecordMessage)
		{
			// Ignore m_ServerTimeBest, it is handled below for the focused connection.
			MessageState.m_Runtime.m_PlayerRecord = pMsg->m_PlayerTimeBest / 100.0f;
		}
	}

	if(MsgId == NETMSGTYPE_SV_TEAMSSTATE || MsgId == NETMSGTYPE_SV_TEAMSSTATELEGACY)
	{
		unsigned int i;
		for(i = 0; i < MAX_CLIENTS; i++)
		{
			const int Team = pUnpacker->GetInt();
			MessageState.SetTeam(i, !pUnpacker->Error() && Team >= TEAM_FLOCK && Team < NUM_DDRACE_TEAMS ? Team : TEAM_FLOCK);
			if(pUnpacker->Error() || Team < TEAM_FLOCK || Team >= NUM_DDRACE_TEAMS)
				break;
		}
		if(i <= VANILLA_MAX_CLIENTS)
			MessageState.SetNumDDRaceTeams(VANILLA_MAX_CLIENTS + 1);
		if(DummyConnection)
			return;

		if(Focused)
		{
			m_Ghost.m_AllowRestart = true;
			m_RaceDemo.m_AllowRestart = true;
		}
		return;
	}
	// Keep prediction ordering current for every connection before inactive messages are filtered below.
	if(MsgId == NETMSGTYPE_SV_KILLMSG)
	{
		const CNetMsg_Sv_KillMsg *pMsg = static_cast<const CNetMsg_Sv_KillMsg *>(pRawMsg);
		if(!(MessageState.CoreGameInfo().m_PredictFNG && pMsg->m_Weapon == WEAPON_LASER))
			MessageState.m_Runtime.m_CharOrder.GiveWeak(pMsg->m_Victim);
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSGTEAM)
	{
		const CNetMsg_Sv_KillMsgTeam *pMsg = static_cast<const CNetMsg_Sv_KillMsgTeam *>(pRawMsg);
		CGameState &State = MessageState;
		std::vector<std::pair<int, int>> vStrongWeakSorted;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(State.Teams().Team(ClientId) != pMsg->m_Team)
				continue;
			if(const CCharacter *pCharacter = State.m_GameWorld.GetCharacterById(ClientId))
				vStrongWeakSorted.emplace_back(ClientId, pMsg->m_First == ClientId ? MAX_CLIENTS : pCharacter->GetStrongWeakId());
		}
		std::stable_sort(vStrongWeakSorted.begin(), vStrongWeakSorted.end(), [](const auto &Left, const auto &Right) { return Left.second > Right.second; });
		for(const auto &Id : vStrongWeakSorted)
			State.m_Runtime.m_CharOrder.GiveWeak(Id.first);
	}

	if(MsgId == NETMSGTYPE_SV_BROADCAST)
	{
		if(!DummyConnection)
		{
			const CNetMsg_Sv_Broadcast *pMsg = static_cast<const CNetMsg_Sv_Broadcast *>(pRawMsg);
			m_Broadcast.DoBroadcast(MessageSession.m_Broadcast, pMsg->m_pMessage, Sessions()->GameTick(SessionId), Sessions()->GameTickSpeed());
		}
		return;
	}
	if(MsgId == NETMSGTYPE_SV_MOTD)
	{
		if(!DummyConnection && Sessions()->SessionType(SessionId) != ESessionSourceType::DEMO)
		{
			const CNetMsg_Sv_Motd *pMsg = static_cast<const CNetMsg_Sv_Motd *>(pRawMsg);
			m_Motd.DoMotd(MessageSession, pMsg->m_pMessage, Focused);
		}
		return;
	}
	switch(MsgId)
	{
	case NETMSGTYPE_SV_VOTESET:
	case NETMSGTYPE_SV_VOTESTATUS:
	case NETMSGTYPE_SV_VOTECLEAROPTIONS:
	case NETMSGTYPE_SV_VOTEOPTIONLISTADD:
	case NETMSGTYPE_SV_VOTEOPTIONADD:
	case NETMSGTYPE_SV_VOTEOPTIONREMOVE:
	case NETMSGTYPE_SV_YOURVOTE:
	case NETMSGTYPE_SV_VOTEOPTIONGROUPSTART:
	case NETMSGTYPE_SV_VOTEOPTIONGROUPEND:
		if(!DummyConnection && Sessions()->SessionType(SessionId) != ESessionSourceType::DEMO)
			m_Voting.HandleMessage(MessageSession.m_Vote, MessageTime, time_freq(), Focused && ClientNetwork()->RconAuthed(), MsgId, pRawMsg);
		return;
	}
	if(MsgId == NETMSGTYPE_SV_EMOTICON)
	{
		const CNetMsg_Sv_Emoticon *pMsg = static_cast<const CNetMsg_Sv_Emoticon *>(pRawMsg);
		MessageState.ApplyEmoticon(pMsg->m_ClientId, pMsg->m_Emoticon, Sessions()->GameTick(SessionId), Sessions()->IntraGameTickSincePrev(SessionId));
	}

	if(DummyConnection)
	{
		const CGameState &MainState = MessageSession.SeatState(IClient::CONN_MAIN);
		const CGameState &DummyState = MessageSession.SeatState(IClient::CONN_DUMMY);
		const int MainLocalId = MainState.LocalClientId();
		const int DummyLocalId = DummyState.LocalClientId();
		if(MsgId == NETMSGTYPE_SV_CHAT && MainLocalId >= 0 && DummyLocalId >= 0)
		{
			CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

			const CTeamsCore &Teams = MessageState.Teams();
			const int MainTeam = MainState.Client(MainLocalId).m_HasPlayerInfo ? MainState.Client(MainLocalId).m_PlayerInfo.m_Team : TEAM_SPECTATORS;
			const int DummyTeam = DummyState.Client(DummyLocalId).m_HasPlayerInfo ? DummyState.Client(DummyLocalId).m_PlayerInfo.m_Team : TEAM_SPECTATORS;
			if((pMsg->m_Team == 1 && (MainTeam != DummyTeam || Teams.Team(MainLocalId) != Teams.Team(DummyLocalId))) || pMsg->m_Team > 1)
			{
				m_Chat.HandleMessage(MessageSession, MessageState, MessageTime, SuppressEvents, Sessions()->SessionType(SessionId) == ESessionSourceType::DEMO, Focused, MsgId, pRawMsg);
			}
		}
		return; // no need of all that stuff for the dummy
	}
	m_Chat.HandleMessage(MessageSession, MessageState, MessageTime, SuppressEvents, Sessions()->SessionType(SessionId) == ESessionSourceType::DEMO, Focused, MsgId, pRawMsg);
	m_InfoMessages.HandleMessage(MessageSession.m_InfoMessages, MessageSession, MessageState, Sessions()->GameTick(SessionId), SuppressEvents, MsgId, pRawMsg);
	MessageSession.m_Stats.HandleMessage(MessageState, SuppressEvents, MsgId, pRawMsg);
	if(MsgId == NETMSGTYPE_SV_RECORD || MsgId == NETMSGTYPE_SV_RECORDLEGACY)
	{
		const CNetMsg_Sv_Record *pMsg = static_cast<const CNetMsg_Sv_Record *>(pRawMsg);
		MessageSession.m_MapMetadata.ApplyRecordBestTime(pMsg->m_ServerTimeBest);
	}
	else if(MsgId == NETMSGTYPE_SV_MAPINFO)
	{
		const CNetMsg_Sv_MapInfo *pMsg = static_cast<const CNetMsg_Sv_MapInfo *>(pRawMsg);
		MessageSession.m_MapMetadata.SetDescription(pMsg->m_pDescription);
	}
	else if(MsgId == NETMSGTYPE_SV_READYTOENTER)
	{
		// A demo can carry the message too, but only a server waits for the answer.
		if(Sessions()->SessionType(SessionId) == ESessionSourceType::NETWORK && ClientNetwork() != nullptr)
			ClientNetwork()->EnterGame(SeatOf(SessionId));
		return;
	}
	else if(MsgId == NETMSGTYPE_SV_MAPSOUNDGLOBAL)
	{
		bool OfflineAudio;
		if(!SuppressEvents && g_Config.m_SndGame && AudioForSession(SessionId, OfflineAudio))
		{
			const CNetMsg_Sv_MapSoundGlobal *pMsg = static_cast<const CNetMsg_Sv_MapSoundGlobal *>(pRawMsg);
			SessionPresentation(SessionId).MapSounds().Play(CSounds::CHN_GLOBAL, pMsg->m_SoundId, OfflineAudio);
		}
		return;
	}
	else if(MsgId == NETMSGTYPE_SV_SOUNDGLOBAL)
	{
		bool OfflineAudio;
		if(SuppressEvents || !g_Config.m_SndGame || !AudioForSession(SessionId, OfflineAudio))
			return;

		const CNetMsg_Sv_SoundGlobal *pMsg = static_cast<const CNetMsg_Sv_SoundGlobal *>(pRawMsg);
		if(pMsg->m_SoundId == SOUND_CTF_DROP || pMsg->m_SoundId == SOUND_CTF_RETURN ||
			pMsg->m_SoundId == SOUND_CTF_CAPTURE || pMsg->m_SoundId == SOUND_CTF_GRAB_EN ||
			pMsg->m_SoundId == SOUND_CTF_GRAB_PL)
			m_Sounds.Enqueue(CSounds::CHN_GLOBAL, pMsg->m_SoundId, OfflineAudio);
		else
			m_Sounds.Play(CSounds::CHN_GLOBAL, pMsg->m_SoundId, 1.0f, OfflineAudio);
		return;
	}
	else if(MsgId == NETMSGTYPE_SV_SAVECODE)
	{
		const CNetMsg_Sv_SaveCode *pMsg = static_cast<const CNetMsg_Sv_SaveCode *>(pRawMsg);
		OnSaveCodeNetMessage(MessageSession, MessageState, pMsg);
		return;
	}

	if(!Focused)
		return;

	// TODO: this should be done smarter
	for(auto &pComponent : m_vpAll)
		pComponent->OnMessage(MsgId, pRawMsg);

	if(MsgId == NETMSGTYPE_SV_KILLMSG)
	{
		CNetMsg_Sv_KillMsg *pMsg = (CNetMsg_Sv_KillMsg *)pRawMsg;
		const CGameState &State = InputState();
		const CGameState::CClientSnapshot &Victim = State.Client(pMsg->m_Victim);
		const bool VictimIsSpec = Victim.m_HasDDNetPlayer && (Victim.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0;
		CGameView::CMultiViewState &MultiViewState = MultiView();
		// reset character prediction
		if(!(State.CoreGameInfo().m_PredictFNG && pMsg->m_Weapon == WEAPON_LASER))
		{
			if(CCharacter *pChar = GameWorld().GetCharacterById(pMsg->m_Victim))
				pChar->ResetPrediction();
			GameWorld().ReleaseHooked(pMsg->m_Victim);
		}

		// if we are spectating a static id set (team 0) and somebody killed, and its not a guy in solo, we remove them from the list
		// never remove players from the list if it is a pvp server
		if(IsMultiViewIdSet() && MultiViewState.m_Team == 0 && MultiViewState.m_aSelected[pMsg->m_Victim] && !VictimIsSpec && !MultiViewState.m_Solo && !State.CoreGameInfo().m_Pvp)
		{
			MultiViewState.m_aSelected[pMsg->m_Victim] = false;

			// if everyone of a team killed, we have no ids to spectate anymore, so we disable multi view
			if(!IsMultiViewIdSet())
			{
				ResetMultiView();
			}
			else
			{
				// the "main" tee killed, search a new one
				if(Snap().m_SpecInfo.m_SpectatorId == pMsg->m_Victim)
				{
					int NewClientId = FindFirstMultiViewId();
					if(NewClientId < MAX_CLIENTS && NewClientId >= 0)
					{
						CleanMultiViewId(NewClientId);
						MultiViewState.m_aSelected[NewClientId] = true;
						m_Spectator.Spectate(NewClientId);
					}
				}
			}
		}
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSGTEAM)
	{
		CNetMsg_Sv_KillMsgTeam *pMsg = (CNetMsg_Sv_KillMsgTeam *)pRawMsg;

		// reset prediction
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(FocusedTeams().Team(i) == pMsg->m_Team)
			{
				if(CCharacter *pChar = GameWorld().GetCharacterById(i))
				{
					pChar->ResetPrediction();
				}
				GameWorld().ReleaseHooked(i);
			}
		}
	}
	else if(MsgId == NETMSGTYPE_SV_PREINPUT)
	{
		CNetMsg_Sv_PreInput *pMsg = (CNetMsg_Sv_PreInput *)pRawMsg;
		m_aClients[pMsg->m_Owner].m_aPreInputs[pMsg->m_IntendedTick % 200] = *pMsg;
	}
}

void CGameClient::OnStateChange(int NewState, int OldState)
{
	for(auto &pComponent : m_vpAll)
		pComponent->OnStateChange(NewState, OldState);
}

void CGameClient::OnShutdown()
{
	m_AssetLoader.Shutdown();
	for(auto &pComponent : m_vpAll)
		pComponent->OnShutdown();
	m_SessionPresentations.UnloadAll();
}

void CGameClient::OnEnterGame(CSessionId SessionId)
{
	(void)SessionId;
}

void CGameClient::OnGameOver()
{
	if(!Client()->IsDemoPlayback() && g_Config.m_ClEditor == 0)
		ClientNetwork()->AutoScreenshot_Start();
}

void CGameClient::OnStartGame()
{
	if(!Client()->IsDemoPlayback() && !g_Config.m_ClAutoDemoOnConnect)
		ClientNetwork()->DemoRecorder_HandleAutoStart();
	m_Statboard.OnReset();
}

void CGameClient::OnStartRound()
{
	// In GamePaused or GameOver state RoundStartTick is updated on each tick
	// hence no need to reset stats until player leaves GameOver
	// and it would be a mistake to reset stats after or during the pause
	m_Statboard.OnReset();

	// Restart automatic race demo recording
	m_RaceDemo.OnReset();
}

void CGameClient::OnWindowResize()
{
	for(auto &pComponent : m_vpAll)
		pComponent->OnWindowResize();

	Ui()->OnWindowResize();
}

void CGameClient::OnLanguageChange()
{
	// The actual language change is delayed because it
	// might require clearing the text render font atlas,
	// which would invalidate text that is currently drawn.
	m_LanguageChanged = true;
}

void CGameClient::HandleLanguageChanged()
{
	if(!m_LanguageChanged)
		return;
	m_LanguageChanged = false;

	g_Localization.Load(g_Config.m_ClLanguagefile, Storage(), Console());
	TextRender()->SetFontLanguageVariant(g_Config.m_ClLanguagefile);

	// Clear all text containers
	Client()->OnWindowResize();
}

void CGameClient::RenderShutdownMessage()
{
	const char *pMessage = nullptr;
	if(Client()->State() == IClient::STATE_QUITTING)
		pMessage = Localize("Quitting. Please wait…");
	else if(Client()->State() == IClient::STATE_RESTARTING)
		pMessage = Localize("Restarting. Please wait…");
	else
		dbg_assert_failed("Invalid client state for quitting message");

	// This function only gets called after the render loop has already terminated, so we have to call Swap manually.
	Graphics()->Clear(0.0f, 0.0f, 0.0f);
	Ui()->MapScreen();
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	Ui()->DoLabel(Ui()->Screen(), pMessage, 16.0f, TEXTALIGN_MC);
	Graphics()->Swap();
	Graphics()->Clear(0.0f, 0.0f, 0.0f);
}

void CGameClient::ProcessDemoSnapshot(CSnapshot *pSnap)
{
	for(int Index = 0; Index < pSnap->NumItems(); Index++)
	{
		const CSnapshotItem *pItem = pSnap->GetItem(Index);
		int ItemType = pSnap->GetItemType(Index);

		if(ItemType == NETOBJTYPE_PROJECTILE)
		{
			// for antiping: if the projectile netobjects from the server contains extra data, this is removed and the original content restored before recording demo
			CNetObj_Projectile *pProj = (CNetObj_Projectile *)((void *)pItem->Data());
			DemoObjectRemoveExtraProjectileInfo(pProj);
		}
		else if(ItemType == NETOBJTYPE_DDNETSPECTATORINFO)
		{
			// always record local camera info as follow mode
			CNetObj_DDNetSpectatorInfo *pDDNetSpectatorInfo = (CNetObj_DDNetSpectatorInfo *)((void *)pItem->Data());
			pDDNetSpectatorInfo->m_HasCameraInfo = true;
			pDDNetSpectatorInfo->m_Zoom = (m_Camera.IsZooming() ? m_Camera.ZoomSmoothingTarget() : m_Camera.Zoom()) * 1000.0f;
			pDDNetSpectatorInfo->m_Deadzone = m_Camera.Deadzone();
			pDDNetSpectatorInfo->m_FollowFactor = m_Camera.FollowFactor();
		}
	}
}

void CGameClient::OnRconType(bool UsernameReq)
{
	if(m_pFrontend != nullptr)
		m_pFrontend->OnRconType(UsernameReq);
}

void CGameClient::OnRconLine(const char *pLine)
{
	if(m_pFrontend != nullptr)
		m_pFrontend->OnRconLine(pLine);
}

void CGameClient::ProcessEvents(CSessionId SessionId)
{
	if(m_SuppressEvents)
		return;

	const int SnapType = ISessions::SNAP_CURRENT;
	CGameSessionContext &Session = SessionContext(SessionId);
	CGameState &State = Session.GameState(SessionId);
	bool OfflineAudio;
	const bool AudioActive = AudioForState(State, OfflineAudio);
	const int Num = Sessions()->SnapNumItems(SessionId, SnapType);
	for(int Index = 0; Index < Num; Index++)
	{
		const ISessions::CSnapItem Item = Sessions()->SnapGetItem(SessionId, SnapType, Index);

		// TODO: We don't have enough info about us, others, to know a correct alpha or volume value.
		const float Alpha = 1.0f;
		const float Volume = 1.0f;

		if(Item.m_Type == NETEVENTTYPE_DAMAGEIND)
		{
			const CNetEvent_DamageInd *pEvent = (const CNetEvent_DamageInd *)Item.m_pData;

			vec2 DamageIndPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!State.m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, DamageIndPos, -1, Sessions()->GameTick(SessionId), pEvent->m_Angle)))
			{
				m_Effects.DamageIndicator(State, vec2(pEvent->m_X, pEvent->m_Y), direction(pEvent->m_Angle / 256.0f), -1, Alpha);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_EXPLOSION)
		{
			const CNetEvent_Explosion *pEvent = (const CNetEvent_Explosion *)Item.m_pData;

			vec2 ExplosionPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!State.m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, ExplosionPos, -1, Sessions()->GameTick(SessionId))))
			{
				m_Effects.Explosion(State, *Session.m_MapContext.Collision(), ExplosionPos, Alpha);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_HAMMERHIT)
		{
			const CNetEvent_HammerHit *pEvent = (const CNetEvent_HammerHit *)Item.m_pData;

			vec2 HammerHitPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!State.m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, HammerHitPos, -1, Sessions()->GameTick(SessionId))))
			{
				m_Effects.HammerHit(State, HammerHitPos, Alpha, Volume);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_BIRTHDAY)
		{
			const CNetEvent_Birthday *pEvent = (const CNetEvent_Birthday *)Item.m_pData;
			m_Effects.Confetti(State, vec2(pEvent->m_X, pEvent->m_Y), Alpha);
		}
		else if(Item.m_Type == NETEVENTTYPE_FINISH)
		{
			const CNetEvent_Finish *pEvent = (const CNetEvent_Finish *)Item.m_pData;
			m_Effects.Confetti(State, vec2(pEvent->m_X, pEvent->m_Y), Alpha);
		}
		else if(Item.m_Type == NETEVENTTYPE_SPAWN)
		{
			const CNetEvent_Spawn *pEvent = (const CNetEvent_Spawn *)Item.m_pData;
			m_Effects.PlayerSpawn(State, vec2(pEvent->m_X, pEvent->m_Y), Alpha, Volume);
		}
		else if(Item.m_Type == NETEVENTTYPE_DEATH)
		{
			const CNetEvent_Death *pEvent = (const CNetEvent_Death *)Item.m_pData;
			m_Effects.PlayerDeath(SessionId, State, vec2(pEvent->m_X, pEvent->m_Y), pEvent->m_ClientId, Alpha);
		}
		else if(Item.m_Type == NETEVENTTYPE_SOUNDWORLD)
		{
			const CNetEvent_SoundWorld *pEvent = (const CNetEvent_SoundWorld *)Item.m_pData;
			if(!Config()->m_SndGame)
				continue;

			if(State.CoreGameInfo().m_RaceSounds && ((pEvent->m_SoundId == SOUND_GUN_FIRE && !g_Config.m_SndGun) || (pEvent->m_SoundId == SOUND_PLAYER_PAIN_LONG && !g_Config.m_SndLongPain)))
				continue;

			vec2 SoundPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!State.m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, SoundPos, -1, Sessions()->GameTick(SessionId), pEvent->m_SoundId)))
			{
				if(AudioActive)
					m_Sounds.PlayAt(CSounds::CHN_WORLD, pEvent->m_SoundId, 1.0f, SoundPos, OfflineAudio);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_MAPSOUNDWORLD)
		{
			CNetEvent_MapSoundWorld *pEvent = (CNetEvent_MapSoundWorld *)Item.m_pData;
			if(!Config()->m_SndGame)
				continue;

			if(AudioActive)
				SessionPresentation(SessionId).MapSounds().PlayAt(CSounds::CHN_WORLD, pEvent->m_SoundId, vec2(pEvent->m_X, pEvent->m_Y), OfflineAudio);
		}
	}
}

static CGameInfo GetGameInfo(const CNetObj_GameInfoEx *pInfoEx, int InfoExSize, const CServerInfo *pFallbackServerInfo)
{
	int Version = -1;
	if(InfoExSize >= 12)
	{
		Version = pInfoEx->m_Version;
	}
	else if(InfoExSize >= 8)
	{
		Version = std::min(pInfoEx->m_Version, 4);
	}
	else if(InfoExSize >= 4)
	{
		Version = 0;
	}
	int Flags = 0;
	if(Version >= 0)
	{
		Flags = pInfoEx->m_Flags;
	}
	int Flags2 = 0;
	if(Version >= 5)
	{
		Flags2 = pInfoEx->m_Flags2;
	}
	bool Race;
	bool FastCap;
	bool FNG;
	bool DDRace;
	bool DDNet;
	bool BlockWorlds;
	bool City;
	bool Vanilla;
	bool Plus;
	bool FDDrace;
	if(Version < 1)
	{
		// The game type is intentionally only available inside this
		// `if`. Game type sniffing should be avoided and ideally not
		// extended. Mods should set the relevant game flags instead.
		const char *pGameType = pFallbackServerInfo->m_aGameType;
		Race = str_find_nocase(pGameType, "race") || str_find_nocase(pGameType, "fastcap");
		FastCap = str_find_nocase(pGameType, "fastcap");
		FNG = str_find_nocase(pGameType, "fng");
		DDRace = str_find_nocase(pGameType, "ddrace") || str_find_nocase(pGameType, "mkrace");
		DDNet = str_find_nocase(pGameType, "ddracenet") || str_find_nocase(pGameType, "ddnet");
		BlockWorlds = str_startswith(pGameType, "bw  ") || str_comp_nocase(pGameType, "bw") == 0;
		City = str_find_nocase(pGameType, "city");
		Vanilla = str_comp(pGameType, "DM") == 0 || str_comp(pGameType, "TDM") == 0 || str_comp(pGameType, "CTF") == 0;
		Plus = str_find(pGameType, "+");
		FDDrace = false;
	}
	else
	{
		Race = Flags & GAMEINFOFLAG_GAMETYPE_RACE;
		FastCap = Flags & GAMEINFOFLAG_GAMETYPE_FASTCAP;
		FNG = Flags & GAMEINFOFLAG_GAMETYPE_FNG;
		DDRace = Flags & GAMEINFOFLAG_GAMETYPE_DDRACE;
		DDNet = Flags & GAMEINFOFLAG_GAMETYPE_DDNET;
		BlockWorlds = Flags & GAMEINFOFLAG_GAMETYPE_BLOCK_WORLDS;
		Vanilla = Flags & GAMEINFOFLAG_GAMETYPE_VANILLA;
		Plus = Flags & GAMEINFOFLAG_GAMETYPE_PLUS;
		City = Version >= 5 && Flags2 & GAMEINFOFLAG2_GAMETYPE_CITY;
		FDDrace = Version >= 6 && Flags2 & GAMEINFOFLAG2_GAMETYPE_FDDRACE;

		// Ensure invariants upheld by the server info parsing business.
		DDRace = DDRace || DDNet || FDDrace;
		Race = Race || FastCap || DDRace;
	}

	CGameInfo Info;
	// Anything that sends the extended game info also knows Cl_ShowDistance;
	// both are DDNet extensions and no server has one without the other.
	Info.m_ClipsToShowDistance = Version >= 0;
	Info.m_DeclaresRuleset = Version >= 2;
	Info.m_FlagStartsRace = FastCap;
	Info.m_TimeScore = Race;
	Info.m_UnlimitedAmmo = Race;
	Info.m_DDRaceRecordMessage = DDRace && !DDNet;
	Info.m_RaceRecordMessage = DDNet || (Race && !DDRace);
	Info.m_RaceSounds = DDRace || FNG || BlockWorlds;
	Info.m_AllowEyeWheel = DDRace || BlockWorlds || City || Plus;
	Info.m_AllowHookColl = DDRace;
	Info.m_AllowZoom = Race || BlockWorlds || City;
	Info.m_BugDDRaceGhost = DDRace;
	Info.m_BugDDRaceInput = DDRace;
	Info.m_BugFNGLaserRange = FNG;
	Info.m_BugVanillaBounce = Vanilla;
	Info.m_PredictFNG = FNG;
	Info.m_PredictDDRace = DDRace;
	Info.m_PredictDDRaceTiles = DDRace && !BlockWorlds;
	Info.m_PredictVanilla = Vanilla || FastCap;
	Info.m_EntitiesDDNet = DDNet;
	Info.m_EntitiesDDRace = DDRace;
	Info.m_EntitiesRace = Race;
	Info.m_EntitiesFNG = FNG;
	Info.m_EntitiesVanilla = Vanilla;
	Info.m_EntitiesBW = BlockWorlds;
	Info.m_Race = Race;
	Info.m_Pvp = !Race;
	Info.m_DontMaskEntities = !DDNet;
	Info.m_AllowXSkins = false;
	Info.m_EntitiesFDDrace = FDDrace;
	Info.m_HudHealthArmor = true;
	Info.m_HudAmmo = true;
	Info.m_HudDDRace = false;
	Info.m_NoWeakHookAndBounce = false;
	Info.m_NoSkinChangeForFrozen = false;
	Info.m_DDRaceTeam = false;
	Info.m_PredictEvents = Vanilla;
	Info.m_MinTeamSize = 0;
	Info.m_MaxTeamSize = 0;
	Info.m_NumDDRaceTeams = 65; // `TEAM_SUPER + 1`, fallback for ddrace64 servers
	Info.m_OldLaser = false;
	Info.m_OldLaserKnown = false;

	if(Version >= 0)
	{
		Info.m_TimeScore = Flags & GAMEINFOFLAG_TIMESCORE;
	}
	if(Version >= 2)
	{
		Info.m_FlagStartsRace = Flags & GAMEINFOFLAG_FLAG_STARTS_RACE;
		Info.m_UnlimitedAmmo = Flags & GAMEINFOFLAG_UNLIMITED_AMMO;
		Info.m_DDRaceRecordMessage = Flags & GAMEINFOFLAG_DDRACE_RECORD_MESSAGE;
		Info.m_RaceRecordMessage = Flags & GAMEINFOFLAG_RACE_RECORD_MESSAGE;
		Info.m_AllowEyeWheel = Flags & GAMEINFOFLAG_ALLOW_EYE_WHEEL;
		Info.m_AllowHookColl = Flags & GAMEINFOFLAG_ALLOW_HOOK_COLL;
		Info.m_AllowZoom = Flags & GAMEINFOFLAG_ALLOW_ZOOM;
		Info.m_BugDDRaceGhost = Flags & GAMEINFOFLAG_BUG_DDRACE_GHOST;
		Info.m_BugDDRaceInput = Flags & GAMEINFOFLAG_BUG_DDRACE_INPUT;
		Info.m_BugFNGLaserRange = Flags & GAMEINFOFLAG_BUG_FNG_LASER_RANGE;
		Info.m_BugVanillaBounce = Flags & GAMEINFOFLAG_BUG_VANILLA_BOUNCE;
		Info.m_PredictFNG = Flags & GAMEINFOFLAG_PREDICT_FNG;
		Info.m_PredictDDRace = Flags & GAMEINFOFLAG_PREDICT_DDRACE;
		Info.m_PredictDDRaceTiles = Flags & GAMEINFOFLAG_PREDICT_DDRACE_TILES;
		Info.m_PredictVanilla = Flags & GAMEINFOFLAG_PREDICT_VANILLA;
		Info.m_EntitiesDDNet = Flags & GAMEINFOFLAG_ENTITIES_DDNET;
		Info.m_EntitiesDDRace = Flags & GAMEINFOFLAG_ENTITIES_DDRACE;
		Info.m_EntitiesRace = Flags & GAMEINFOFLAG_ENTITIES_RACE;
		Info.m_EntitiesFNG = Flags & GAMEINFOFLAG_ENTITIES_FNG;
		Info.m_EntitiesVanilla = Flags & GAMEINFOFLAG_ENTITIES_VANILLA;
	}
	if(Version >= 3)
	{
		Info.m_Race = Flags & GAMEINFOFLAG_RACE;
		Info.m_DontMaskEntities = Flags & GAMEINFOFLAG_DONT_MASK_ENTITIES;
	}
	if(Version >= 4)
	{
		Info.m_EntitiesBW = Flags & GAMEINFOFLAG_ENTITIES_BW;
	}
	if(Version >= 5)
	{
		Info.m_AllowXSkins = Flags2 & GAMEINFOFLAG2_ALLOW_X_SKINS;
	}
	if(Version >= 6)
	{
		Info.m_EntitiesFDDrace = Flags2 & GAMEINFOFLAG2_ENTITIES_FDDRACE;
	}
	if(Version >= 7)
	{
		Info.m_HudHealthArmor = Flags2 & GAMEINFOFLAG2_HUD_HEALTH_ARMOR;
		Info.m_HudAmmo = Flags2 & GAMEINFOFLAG2_HUD_AMMO;
		Info.m_HudDDRace = Flags2 & GAMEINFOFLAG2_HUD_DDRACE;
	}
	if(Version >= 8)
	{
		Info.m_NoWeakHookAndBounce = Flags2 & GAMEINFOFLAG2_NO_WEAK_HOOK;
	}
	if(Version >= 9)
	{
		Info.m_NoSkinChangeForFrozen = Flags2 & GAMEINFOFLAG2_NO_SKIN_CHANGE_FOR_FROZEN;
	}
	if(Version >= 10)
	{
		Info.m_DDRaceTeam = Flags2 & GAMEINFOFLAG2_DDRACE_TEAM;
	}
	if(Version >= 11)
	{
		Info.m_PredictEvents = Flags2 & GAMEINFOFLAG2_PREDICT_EVENTS;
	}
	if(Version >= 12)
	{
		Info.m_MinTeamSize = pInfoEx->m_MinTeamSize;
		Info.m_MaxTeamSize = pInfoEx->m_MaxTeamSize;
		// Servers from before this field was added send 0, and this client cannot represent more
		// teams than it was compiled with. Fall back to our own count for anything out of range.
		const int NumDDRaceTeams = pInfoEx->m_NumDDRaceTeams;
		Info.m_NumDDRaceTeams = NumDDRaceTeams > TEAM_FLOCK + 1 && NumDDRaceTeams <= NUM_DDRACE_TEAMS ? NumDDRaceTeams : NUM_DDRACE_TEAMS;
		Info.m_OldLaser = Flags2 & GAMEINFOFLAG2_OLD_LASER;
		Info.m_OldLaserKnown = true;
	}

	return Info;
}

void CGameClient::InvalidateSnapshot(CSessionId SessionId)
{
	if(SessionId != Sessions()->FocusedSessionId())
		return;
	// clear all pointers
	mem_zero(&Snap(), sizeof(Snap()));
	Snap().m_SpecInfo.m_Zoom = 1.0f;
	Snap().m_LocalClientId = -1;
	m_vSnapEntities.clear();
}

void CGameClient::OnNewSnapshot(CSessionId SessionId)
{
	CGameInfo GameInfo = GetGameInfo(nullptr, 0, &Sessions()->ServerInfo(SessionId));
	const int NumItems = Sessions()->SnapNumItems(SessionId, ISessions::SNAP_CURRENT);
	for(int i = 0; i < NumItems; i++)
	{
		const ISessions::CSnapItem Item = Sessions()->SnapGetItem(SessionId, ISessions::SNAP_CURRENT, i);
		if(Item.m_Type == NETOBJTYPE_GAMEINFOEX)
		{
			GameInfo = GetGameInfo(static_cast<const CNetObj_GameInfoEx *>(Item.m_pData), Item.m_DataSize, &Sessions()->ServerInfo(SessionId));
			break;
		}
	}
	CGameSessionContext &Session = SessionContext(SessionId);
	CGameState &State = Session.GameState(SessionId);
	// Only the session that gets the input is fully predicted.
	const bool Active = SessionId == InputSessionId();
	State.SetFullyPredicted(Active);
	State.SetCoreGameInfo(GameInfo);
	State.ApplySnapshot(*Sessions());
	BuildSnapState(SessionId);
	bool EnteredGameOver = false;
	if(State.m_Seat == IClient::CONN_MAIN)
		EnteredGameOver = Session.m_Stats.UpdateSnapshot(State, Sessions()->GameTick(SessionId));
	bool ProcessedEvents = false;
	if(Active)
	{
		ProcessSnapshot(SessionId);
		ProcessedEvents = true;
	}
#if defined(CONF_VIDEORECORDER)
	else if(SessionId == Client()->VideoSessionId())
	{
		// A session rendered to video in the background is not processed as a
		// whole, but what it plays and shows of its events is recorded.
		ProcessEvents(SessionId);
		ProcessedEvents = true;
	}
#endif
	else if(State.IsShown())
	{
		// Explosions, hits and deaths are events rather than objects, so a pane
		// that does not take input only shows them if they are processed for its
		// state as well. Sounds stay with the state that is played.
		ProcessEvents(SessionId);
		ProcessedEvents = true;
	}
	if(ProcessedEvents)
		ProcessAirJumpEffects(SessionId);
	if(EnteredGameOver)
		FinalizeObservedMatch(SessionId, Session, State, EMatchTermination::COMPLETED);
}

void CGameClient::ProcessAirJumpEffects(CSessionId SessionId)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	CGameState &State = Session.GameState(SessionId);
	const CGameState::CSnapState &Snap = State.m_Snap;
	const bool NetworkSource = Sessions()->SessionType(SessionId) == ESessionSourceType::NETWORK;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const auto &Character = Snap.m_aCharacters[i];
		if(!Character.m_Active || !(Character.m_Cur.m_Jumped & 2) || (Character.m_Prev.m_Jumped & 2))
			continue;

		const CGameState *pOtherState = NetworkSource ? &Session.SeatState(State.m_Seat ^ 1) : nullptr;
		const bool IsDummy = pOtherState != nullptr && DummyConnected() && i == pOtherState->LocalClientId();
		const bool IsLocalPlayer = i == Snap.m_LocalClientId;
		if(Predict() && (IsLocalPlayer || AntiPingPlayers()) && (IsLocalPlayer || IsDummy))
			continue;

		const vec2 PreviousPosition(Character.m_Prev.m_X, Character.m_Prev.m_Y);
		if(Session.m_MapContext.Collision()->IsOnGround(PreviousPosition, CCharacterCore::PhysicalSize()))
			continue;
		const vec2 Position = mix(PreviousPosition, vec2(Character.m_Cur.m_X, Character.m_Cur.m_Y), Sessions()->IntraGameTick(SessionId));
		m_Effects.AirJump(State, Position, i, 1.0f, 1.0f); // TODO snd_game_volume_others
	}
}

// Who is playing, where they are and which camera the server asked for, for
// every game state. What only the focused one needs stays in ProcessSnapshot.
void CGameClient::BuildSnapState(CSessionId SessionId)
{
	CGameSessionContext &Session = SessionContext(SessionId);
	CGameState &ActiveState = Session.GameState(SessionId);
	CGameState::CRuntimeState &Runtime = ActiveState.m_Runtime;
	CGameState::CSnapState &Snap = ActiveState.m_Snap;
	const CPhysicsRules PhysicsRules = PredictedPhysicsRules(Session, ActiveState);
	auto &&Evolve = [&Session, &PhysicsRules](CNetObj_Character *pCharacter, int Tick) {
		CWorldCore TempWorld;
		TempWorld.m_PhysicsRules = PhysicsRules;
		CCharacterCore TempCore = CCharacterCore();
		CTeamsCore TempTeams = CTeamsCore();
		TempCore.Init(&TempWorld, Session.m_MapContext.Collision(), &TempTeams);
		TempCore.Read(pCharacter);
		TempCore.m_ActiveWeapon = pCharacter->m_Weapon;

		while(pCharacter->m_Tick < Tick)
		{
			pCharacter->m_Tick++;
			TempCore.Tick(false);
			TempCore.Move();
			TempCore.Quantize();
		}

		TempCore.Write(pCharacter);
	};

	// clear all pointers
	mem_zero(&Snap, sizeof(Snap));
	Snap.m_SpecInfo.m_Zoom = 1.0f;
	Snap.m_LocalClientId = -1;

	const CServerInfo &ServerInfo = Sessions()->ServerInfo(SessionId);

	bool GotSwitchStateTeam = false;
	Runtime.m_SwitchStateTeam = -1;

	// go through all the items in the snapshot and gather the info we want
	{
		Snap.m_aTeamSize[TEAM_RED] = Snap.m_aTeamSize[TEAM_BLUE] = 0;

		const int Num = Sessions()->SnapNumItems(SessionId, ISessions::SNAP_CURRENT);
		for(int i = 0; i < Num; i++)
		{
			const ISessions::CSnapItem Item = Sessions()->SnapGetItem(SessionId, ISessions::SNAP_CURRENT, i);

			if(Item.m_Type == NETOBJTYPE_PLAYERINFO)
			{
				const CNetObj_PlayerInfo *pInfo = (const CNetObj_PlayerInfo *)Item.m_pData;

				if(pInfo->m_ClientId < MAX_CLIENTS && pInfo->m_ClientId == Item.m_Id)
				{
					Snap.m_apPlayerInfos[pInfo->m_ClientId] = pInfo;
					Snap.m_apPrevPlayerInfos[pInfo->m_ClientId] = static_cast<const CNetObj_PlayerInfo *>(Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, Item.m_Type, pInfo->m_ClientId));
					Snap.m_NumPlayers++;

					if(pInfo->m_Local)
					{
						Snap.m_LocalClientId = pInfo->m_ClientId;
						Snap.m_pLocalInfo = pInfo;

						if(pInfo->m_Team == TEAM_SPECTATORS)
						{
							Snap.m_SpecInfo.m_Active = true;
						}
					}

					Snap.m_HighestClientId = std::max(Snap.m_HighestClientId, pInfo->m_ClientId);

					// calculate team-balance
					if(pInfo->m_Team != TEAM_SPECTATORS)
					{
						Snap.m_aTeamSize[pInfo->m_Team]++;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_CHARACTER)
			{
				if(Item.m_Id < MAX_CLIENTS)
				{
					const void *pOld = Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, NETOBJTYPE_CHARACTER, Item.m_Id);
					Snap.m_aCharacters[Item.m_Id].m_Cur = *((const CNetObj_Character *)Item.m_pData);
					if(pOld)
					{
						Snap.m_aCharacters[Item.m_Id].m_Active = true;
						Snap.m_aCharacters[Item.m_Id].m_Prev = *((const CNetObj_Character *)pOld);

						// limit evolving to 3 seconds
						bool EvolvePrev = Sessions()->PrevGameTick(SessionId) - Snap.m_aCharacters[Item.m_Id].m_Prev.m_Tick <= 3 * Sessions()->GameTickSpeed();
						bool EvolveCur = Sessions()->GameTick(SessionId) - Snap.m_aCharacters[Item.m_Id].m_Cur.m_Tick <= 3 * Sessions()->GameTickSpeed();

						// reuse the result from the previous evolve if the snapped character didn't change since the previous snapshot
						if(EvolveCur && ActiveState.EvolvedCharacter(Item.m_Id).m_Evolved.m_Tick == Sessions()->PrevGameTick(SessionId))
						{
							if(mem_comp(&Snap.m_aCharacters[Item.m_Id].m_Prev, &ActiveState.EvolvedCharacter(Item.m_Id).m_Snapped, sizeof(CNetObj_Character)) == 0)
								Snap.m_aCharacters[Item.m_Id].m_Prev = ActiveState.EvolvedCharacter(Item.m_Id).m_Evolved;
							if(mem_comp(&Snap.m_aCharacters[Item.m_Id].m_Cur, &ActiveState.EvolvedCharacter(Item.m_Id).m_Snapped, sizeof(CNetObj_Character)) == 0)
								Snap.m_aCharacters[Item.m_Id].m_Cur = ActiveState.EvolvedCharacter(Item.m_Id).m_Evolved;
						}

						if(EvolvePrev && Snap.m_aCharacters[Item.m_Id].m_Prev.m_Tick)
							Evolve(&Snap.m_aCharacters[Item.m_Id].m_Prev, Sessions()->PrevGameTick(SessionId));
						if(EvolveCur && Snap.m_aCharacters[Item.m_Id].m_Cur.m_Tick)
							Evolve(&Snap.m_aCharacters[Item.m_Id].m_Cur, Sessions()->GameTick(SessionId));

						ActiveState.EvolvedCharacter(Item.m_Id).m_Snapped = *((const CNetObj_Character *)Item.m_pData);
						ActiveState.EvolvedCharacter(Item.m_Id).m_Evolved = Snap.m_aCharacters[Item.m_Id].m_Cur;
					}
					else
					{
						ActiveState.EvolvedCharacter(Item.m_Id).m_Evolved.m_Tick = -1;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_DDNETCHARACTER)
			{
				const CNetObj_DDNetCharacter *pCharacterData = (const CNetObj_DDNetCharacter *)Item.m_pData;

				if(Item.m_Id < MAX_CLIENTS)
				{
					Snap.m_aCharacters[Item.m_Id].m_ExtendedData = *pCharacterData;
					Snap.m_aCharacters[Item.m_Id].m_pPrevExtendedData = (const CNetObj_DDNetCharacter *)Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, NETOBJTYPE_DDNETCHARACTER, Item.m_Id);
					Snap.m_aCharacters[Item.m_Id].m_HasExtendedData = true;
					Snap.m_aCharacters[Item.m_Id].m_HasExtendedDisplayInfo = false;
					if(pCharacterData->m_JumpedTotal != -1)
					{
						Snap.m_aCharacters[Item.m_Id].m_HasExtendedDisplayInfo = true;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_SPECTATORINFO)
			{
				Snap.m_pSpectatorInfo = (const CNetObj_SpectatorInfo *)Item.m_pData;
				Snap.m_pPrevSpectatorInfo = (const CNetObj_SpectatorInfo *)Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, NETOBJTYPE_SPECTATORINFO, Item.m_Id);

				// needed for 0.7 survival
				// to auto spec players when dead
				if(Sessions()->IsSixup(SessionId))
					Snap.m_SpecInfo.m_Active = true;
				Snap.m_SpecInfo.m_SpectatorId = Snap.m_pSpectatorInfo->m_SpectatorId;
			}
			else if(Item.m_Type == NETOBJTYPE_SPECTATORCOUNT)
			{
				Snap.m_pSpectatorCount = (const CNetObj_SpectatorCount *)Item.m_pData;
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEINFO)
			{
				Snap.m_pGameInfoObj = (const CNetObj_GameInfo *)Item.m_pData;
				const bool CurrentTickGameOver = (Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) != 0;
				const bool CurrentTickGamePaused = (Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED) != 0;
				if(!Runtime.m_GameOver && CurrentTickGameOver)
					OnGameOver();
				else if(Runtime.m_GameOver && !CurrentTickGameOver)
					OnStartGame();
				// Handle case that a new round is started (RoundStartTick changed)
				// New round is usually started after `restart` on server
				if(Snap.m_pGameInfoObj->m_RoundStartTick != Runtime.m_LastRoundStartTick && !(CurrentTickGameOver || CurrentTickGamePaused || Runtime.m_GamePaused))
					OnStartRound();
				Runtime.m_LastRoundStartTick = Snap.m_pGameInfoObj->m_RoundStartTick;
				Runtime.m_GameOver = CurrentTickGameOver;
				Runtime.m_GamePaused = CurrentTickGamePaused;
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEDATA)
			{
				Snap.m_pGameDataObj = static_cast<const CNetObj_GameData *>(Item.m_pData);
				Snap.m_pPrevGameDataObj = static_cast<const CNetObj_GameData *>(Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, Item.m_Type, Item.m_Id));
				if(Snap.m_pGameDataObj->m_FlagCarrierRed == FLAG_TAKEN)
				{
					if(Runtime.m_aFlagDropTick[TEAM_RED] == 0)
						Runtime.m_aFlagDropTick[TEAM_RED] = Sessions()->GameTick(SessionId);
				}
				else
				{
					Runtime.m_aFlagDropTick[TEAM_RED] = 0;
				}
				if(Snap.m_pGameDataObj->m_FlagCarrierBlue == FLAG_TAKEN)
				{
					if(Runtime.m_aFlagDropTick[TEAM_BLUE] == 0)
						Runtime.m_aFlagDropTick[TEAM_BLUE] = Sessions()->GameTick(SessionId);
				}
				else
				{
					Runtime.m_aFlagDropTick[TEAM_BLUE] = 0;
				}
				Runtime.m_LastFlagCarrierRed = Snap.m_pGameDataObj->m_FlagCarrierRed;
				Runtime.m_LastFlagCarrierBlue = Snap.m_pGameDataObj->m_FlagCarrierBlue;
			}
			else if(Item.m_Type == NETOBJTYPE_FLAG)
			{
				const CNetObj_Flag *pPrevFlag = static_cast<const CNetObj_Flag *>(Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, Item.m_Type, Item.m_Id));
				if(pPrevFlag == nullptr)
				{
					continue;
				}
				Snap.m_apFlags[Snap.m_NumFlags] = static_cast<const CNetObj_Flag *>(Item.m_pData);
				Snap.m_apPrevFlags[Snap.m_NumFlags] = pPrevFlag;
				++Snap.m_NumFlags;
			}
			else if(Item.m_Type == NETOBJTYPE_SWITCHSTATE)
			{
				if(Item.m_DataSize < 36)
				{
					continue;
				}
				const CNetObj_SwitchState *pSwitchStateData = (const CNetObj_SwitchState *)Item.m_pData;
				int Team = std::clamp(Item.m_Id, (int)TEAM_FLOCK, NUM_DDRACE_TEAMS - 1);

				int HighestSwitchNumber = std::clamp(std::max(pSwitchStateData->m_HighestSwitchNumber, Session.m_MapContext.Collision()->m_HighestSwitchNumber), 0, 255);
				if(HighestSwitchNumber != std::max(0, (int)Switchers().size() - 1))
				{
					GameWorld().m_Core.InitSwitchers(HighestSwitchNumber);
					Session.m_MapContext.Collision()->m_HighestSwitchNumber = HighestSwitchNumber;
				}

				for(int j = 0; j < (int)Switchers().size(); j++)
				{
					Switchers()[j].m_aStatus[Team] = (pSwitchStateData->m_aStatus[j / 32] >> (j % 32)) & 1;
				}

				if(Item.m_DataSize >= 68)
				{
					// update the endtick of up to four timed switchers
					for(int j = 0; j < (int)std::size(pSwitchStateData->m_aEndTicks); j++)
					{
						int SwitchNumber = pSwitchStateData->m_aSwitchNumbers[j];
						int EndTick = pSwitchStateData->m_aEndTicks[j];
						if(EndTick > 0 && SwitchNumber >= 0 && SwitchNumber < (int)Switchers().size())
						{
							Switchers()[SwitchNumber].m_aEndTick[Team] = EndTick;
						}
					}
				}

				// update switch types
				for(auto &Switcher : Switchers())
				{
					if(Switcher.m_aStatus[Team])
						Switcher.m_aType[Team] = Switcher.m_aEndTick[Team] ? TILE_SWITCHTIMEDOPEN : TILE_SWITCHOPEN;
					else
						Switcher.m_aType[Team] = Switcher.m_aEndTick[Team] ? TILE_SWITCHTIMEDCLOSE : TILE_SWITCHCLOSE;
				}

				if(!GotSwitchStateTeam)
					Runtime.m_SwitchStateTeam = Team;
				else
					Runtime.m_SwitchStateTeam = -1;
				GotSwitchStateTeam = true;
			}
			else if(Item.m_Type == NETOBJTYPE_MAPBESTTIME)
			{
				const CNetObj_MapBestTime *pMapBestTimeData = static_cast<const CNetObj_MapBestTime *>(Item.m_pData);
				Session.m_MapMetadata.ApplyBestTime(pMapBestTimeData->m_MapBestTimeSeconds, pMapBestTimeData->m_MapBestTimeMillis);
			}
		}
	}
	if(ActiveState.HasDDNetSpectatorInfo())
	{
		const CNetObj_DDNetSpectatorInfo &DDNetSpectatorInfo = ActiveState.DDNetSpectatorInfo();
		Snap.m_SpecInfo.m_HasCameraInfo = DDNetSpectatorInfo.m_HasCameraInfo;
		Snap.m_SpecInfo.m_Zoom = DDNetSpectatorInfo.m_Zoom / 1000.0f;
		Snap.m_SpecInfo.m_Deadzone = DDNetSpectatorInfo.m_Deadzone;
		Snap.m_SpecInfo.m_FollowFactor = DDNetSpectatorInfo.m_FollowFactor;
	}
	if(Snap.m_LocalClientId >= 0)
	{
		const CGameState::CClientSnapshot &LocalClient = ActiveState.Client(Snap.m_LocalClientId);
		if(LocalClient.m_HasDDNetPlayer && (LocalClient.m_DDNetPlayer.m_Flags & (EXPLAYERFLAG_PAUSED | EXPLAYERFLAG_SPEC)) != 0)
			Snap.m_SpecInfo.m_Active = true;
	}

	// setup local pointers
	if(Snap.m_LocalClientId >= 0)
	{
		ActiveState.SetLocalClientId(Snap.m_LocalClientId);

		CGameState::CSnapState::CCharacterInfo *pChr = &Snap.m_aCharacters[Snap.m_LocalClientId];
		if(pChr->m_Active)
		{
			if(!Snap.m_SpecInfo.m_Active)
			{
				Snap.m_pLocalCharacter = &pChr->m_Cur;
				Snap.m_pLocalPrevCharacter = &pChr->m_Prev;
			}
		}
		else if(Sessions()->SnapFindItem(SessionId, ISessions::SNAP_PREV, NETOBJTYPE_CHARACTER, Snap.m_LocalClientId))
		{
			// player died
			ActiveState.Input().m_aAmmoCount.fill(0);
		}
	}
	if(Sessions()->SessionType(SessionId) == ESessionSourceType::DEMO)
	{
		int &DemoSpecId = Session.m_DemoSpecId;
		// A server demo has nobody to follow, so the demo being watched goes
		// to the free view once. Snapshots without players, as while seeking,
		// say nothing about that.
		if(SessionId == Sessions()->DemoSessionId() && Snap.m_NumPlayers > 0 && Snap.m_LocalClientId == -1 && DemoSpecId == SPEC_FOLLOW)
		{
			// TODO: can this be done in the translation layer?
			if(!Sessions()->IsSixup(SessionId))
				DemoSpecId = SPEC_FREEVIEW;
		}
		if(DemoSpecId != SPEC_FOLLOW)
		{
			Snap.m_SpecInfo.m_Active = true;
			if(DemoSpecId > SPEC_FREEVIEW && Snap.m_aCharacters[DemoSpecId].m_Active)
				Snap.m_SpecInfo.m_SpectatorId = DemoSpecId;
			else
				Snap.m_SpecInfo.m_SpectatorId = SPEC_FREEVIEW;
		}
	}
	// sort player infos by name
	std::array<std::array<char, MAX_NAME_LENGTH>, MAX_CLIENTS> aaNames = {};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CGameState::CClientIdentityState &Identity = ActiveState.ClientIdentity(ClientId);
		if(!Identity.m_Active || !IntsToStr(Identity.m_ClientInfo.m_aName, std::size(Identity.m_ClientInfo.m_aName), aaNames[ClientId].data(), aaNames[ClientId].size()))
			str_copy(aaNames[ClientId].data(), "nameless tee", aaNames[ClientId].size());
	}
	mem_copy(Snap.m_apInfoByName, Snap.m_apPlayerInfos, sizeof(Snap.m_apInfoByName));
	std::stable_sort(Snap.m_apInfoByName, Snap.m_apInfoByName + MAX_CLIENTS,
		[&aaNames](const CNetObj_PlayerInfo *pPlayer1, const CNetObj_PlayerInfo *pPlayer2) -> bool {
			if(!pPlayer2)
				return static_cast<bool>(pPlayer1);
			if(!pPlayer1)
				return false;
			return str_comp_nocase(aaNames[pPlayer1->m_ClientId].data(), aaNames[pPlayer2->m_ClientId].data()) < 0;
		});

	bool TimeScore = ActiveState.CoreGameInfo().m_TimeScore;
	bool Race7 = Sessions()->IsSixup(SessionId) && Snap.m_pGameInfoObj && Snap.m_pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE;

	// sort player infos by score
	mem_copy(Snap.m_apInfoByScore, Snap.m_apInfoByName, sizeof(Snap.m_apInfoByScore));
	const bool ReceivedFinishTimes = Runtime.m_ReceivedDDNetPlayerFinishTimes;
	auto TimeComparator = CGameClient::GetScoreComparator(TimeScore, ReceivedFinishTimes, Race7);
	auto SortByTimeScore = [TimeComparator, ReceivedFinishTimes, &ActiveState](const CNetObj_PlayerInfo *pPlayer1, const CNetObj_PlayerInfo *pPlayer2) -> bool {
		if(!pPlayer2)
			return static_cast<bool>(pPlayer1);
		if(!pPlayer1)
			return false;
		if(ReceivedFinishTimes)
		{
			const CGameState::CClientSnapshot &Player1 = ActiveState.Client(pPlayer1->m_ClientId);
			const CGameState::CClientSnapshot &Player2 = ActiveState.Client(pPlayer2->m_ClientId);
			return TimeComparator(
				Player1.m_HasDDNetPlayer ? Player1.m_DDNetPlayer.m_FinishTimeSeconds : FinishTime::UNSET,
				Player2.m_HasDDNetPlayer ? Player2.m_DDNetPlayer.m_FinishTimeSeconds : FinishTime::UNSET,
				Player1.m_HasDDNetPlayer ? Player1.m_DDNetPlayer.m_FinishTimeMillis : 0,
				Player2.m_HasDDNetPlayer ? Player2.m_DDNetPlayer.m_FinishTimeMillis : 0);
		}
		return TimeComparator(pPlayer1->m_Score, pPlayer2->m_Score, 0, 0);
	};
	std::stable_sort(Snap.m_apInfoByScore, Snap.m_apInfoByScore + MAX_CLIENTS, SortByTimeScore);

	// sort player infos by DDRace Team (and score between)
	int Index = 0;
	for(int Team = TEAM_FLOCK; Team < NUM_DDRACE_TEAMS; ++Team)
	{
		for(int i = 0; i < MAX_CLIENTS && Index < MAX_CLIENTS; ++i)
		{
			if(Snap.m_apInfoByScore[i] && ActiveState.Teams().Team(Snap.m_apInfoByScore[i]->m_ClientId) == Team)
				Snap.m_apInfoByDDTeamScore[Index++] = Snap.m_apInfoByScore[i];
		}
	}

	// sort player infos by DDRace Team (and name between)
	Index = 0;
	for(int Team = TEAM_FLOCK; Team < NUM_DDRACE_TEAMS; ++Team)
	{
		for(int i = 0; i < MAX_CLIENTS && Index < MAX_CLIENTS; ++i)
		{
			if(Snap.m_apInfoByName[i] && ActiveState.Teams().Team(Snap.m_apInfoByName[i]->m_ClientId) == Team)
				Snap.m_apInfoByDDTeamName[Index++] = Snap.m_apInfoByName[i];
		}
	}

	if(ServerInfo.m_aGameType[0] != '0')
	{
		// Vanilla servers send laser_bounce_num 1, DDNet has laser_bounce_num 1000 since ~2014
		CTuningParams VanillaTuning;
		VanillaTuning.m_LaserBounceNum = 1;
		const CGameInfo &GameInfo = ActiveState.CoreGameInfo();
		if(str_comp(ServerInfo.m_aGameType, "DM") != 0 && str_comp(ServerInfo.m_aGameType, "TDM") != 0 && str_comp(ServerInfo.m_aGameType, "CTF") != 0)
			Runtime.m_ServerMode = CGameState::SERVERMODE_MOD;
		// A server that states its ruleset is taken at its word, tuning commands and
		// all. Only the ones that state nothing are measured against the vanilla
		// tuning, because a mod calling itself DM is what this check is here to spot.
		else if(GameInfo.m_DeclaresRuleset ? GameInfo.m_PredictVanilla : mem_comp(&VanillaTuning, &Runtime.m_CurrentTuning, 33 * sizeof(CTuneParam)) == 0)
			Runtime.m_ServerMode = CGameState::SERVERMODE_PURE;
		else
			Runtime.m_ServerMode = CGameState::SERVERMODE_PUREMOD;
	}
}

void CGameClient::ProcessSnapshot(CSessionId SessionId)
{
	dbg_assert(SessionId == InputSessionId(), "legacy snapshot must belong to the session that gets the input");
	CGameSessionContext &Session = SessionContext(SessionId);
	CGameState &ActiveState = Session.GameState(SessionId);
	CGameState::CRuntimeState &Runtime = ActiveState.m_Runtime;
	CGameState::CSnapState &Snap = ActiveState.m_Snap;
	const bool NetworkSource = Sessions()->SessionType(SessionId) == ESessionSourceType::NETWORK;

	m_vSnapEntities.clear();
	m_NewTick = true;

	ProcessEvents(SessionId);

	if(g_Config.m_DbgStress)
	{
		if(NetworkSource && (Sessions()->GameTick(SessionId) % 100) == 0)
		{
			char aMessage[64];
			int MsgLen = rand() % (sizeof(aMessage) - 1);
			for(int i = 0; i < MsgLen; i++)
				aMessage[i] = (char)('a' + (rand() % ('z' - 'a')));
			aMessage[MsgLen] = 0;

			m_Chat.SendChat(rand() & 1, aMessage);
		}
	}

	// The legacy client data is filled from the game state, only for the focused one.
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CClientData &Client = m_aClients[ClientId];
		const CGameState::CClientSnapshot &SnapshotClient = ActiveState.Client(ClientId);
		if(SnapshotClient.m_HasClientInfo)
		{
			const CNetObj_ClientInfo &Info = SnapshotClient.m_ClientInfo;
			if(!IntsToStr(Info.m_aName, std::size(Info.m_aName), Client.m_aName, std::size(Client.m_aName)))
			{
				str_copy(Client.m_aName, "nameless tee");
			}
			IntsToStr(Info.m_aClan, std::size(Info.m_aClan), Client.m_aClan, std::size(Client.m_aClan));
			Client.m_Country = Info.m_Country;
			if(!in_range(Client.m_Country, CountryCode::MINIMUM, CountryCode::MAXIMUM))
			{
				Client.m_Country = CountryCode::DEFAULT;
			}

			IntsToStr(Info.m_aSkin, std::size(Info.m_aSkin), Client.m_aSkinName, std::size(Client.m_aSkinName));
			if(!CSkin::IsValidName(Client.m_aSkinName) ||
				(!ActiveState.CoreGameInfo().m_AllowXSkins && CSkins::IsSpecialSkin(Client.m_aSkinName)))
			{
				str_copy(Client.m_aSkinName, "default");
			}

			Client.m_UseCustomColor = Info.m_UseCustomColor;
			Client.m_ColorBody = Info.m_ColorBody;
			Client.m_ColorFeet = Info.m_ColorFeet;
		}
		if(SnapshotClient.m_HasPlayerInfo && SnapshotClient.m_PlayerInfo.m_ClientId == ClientId)
		{
			Client.m_Team = SnapshotClient.m_PlayerInfo.m_Team;
			Client.m_Active = true;
		}
		if(SnapshotClient.m_HasExtendedCharacter)
		{
			Client.m_Predicted.ReadDDNet(&SnapshotClient.m_ExtendedCharacter);
		}
	}

	for(CClientData &Client : m_aClients)
	{
		Client.UpdateSkinInfo(ActiveState);
	}

	// clear out unneeded client data
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(!Snap.m_apPlayerInfos[i] && m_aClients[i].m_Active)
		{
			m_aClients[i].Reset();
		}
	}

	if(NetworkSource)
	{
		m_pDiscord->UpdatePlayerCount(Snap.m_NumPlayers);
	}

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		// update friend state
		m_aClients[i].m_Friend = !(i == Snap.m_LocalClientId || !Snap.m_apPlayerInfos[i] || !Friends()->IsFriend(m_aClients[i].m_aName, m_aClients[i].m_aClan, true));

		// update foe state
		m_aClients[i].m_Foe = !(i == Snap.m_LocalClientId || !Snap.m_apPlayerInfos[i] || !Foes()->IsFriend(m_aClients[i].m_aName, m_aClients[i].m_aClan, true));
	}

	if(Snap.m_pLocalCharacter != nullptr)
	{
		m_LocalCharacterPos = vec2(Snap.m_pLocalCharacter->m_X, Snap.m_pLocalCharacter->m_Y);
	}

	const int Seat = ActiveState.m_Seat;
	if(Session.Id() == NetworkSessionId())
	{
		// add tuning to demo when new recording was started, because server tune message was already received before
		std::bitset<RECORDER_MAX> CurrentRecordings;
		for(int i = 0; i < RECORDER_MAX; i++)
		{
			if(DemoRecorder(i)->IsRecording())
			{
				CurrentRecordings.set(i);
			}
		}
		const bool HasNewRecordings = (CurrentRecordings & ~m_ActiveRecordings).any();
		m_ActiveRecordings = CurrentRecordings;
		if(HasNewRecordings)
		{
			CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
			int *pParams = (int *)&Runtime.m_CurrentTuning;
			for(unsigned i = 0; i < sizeof(Runtime.m_CurrentTuning) / sizeof(int); i++)
				Msg.AddInt(pParams[i]);
			ClientNetwork()->SendMsg(Seat, &Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND);
		}

		for(CGameState &SessionState : Session.GameStates())
		{
			if(SessionState.m_Runtime.m_DDRaceMsgSent || !Snap.m_pLocalInfo)
				continue;
			if(SessionState.m_Seat == IClient::CONN_DUMMY && !DummyConnected())
				continue;
			CMsgPacker Msg(NETMSGTYPE_CL_ISDDNETLEGACY, false);
			Msg.AddInt(DDNetVersion());
			ClientNetwork()->SendMsg(SessionState.m_Seat, &Msg, MSGFLAG_VITAL);
			SessionState.m_Runtime.m_DDRaceMsgSent = true;
		}

		if(Snap.m_SpecInfo.m_Active && MultiView().m_Active)
		{
			// dont show other teams while spectating in multi view
			CNetMsg_Cl_ShowOthers Msg;
			Msg.m_Show = SHOW_OTHERS_ONLY_TEAM;
			ClientNetwork()->SendPackMsg(Seat, &Msg, MSGFLAG_VITAL);

			// update state
			Runtime.m_ShowOthers = SHOW_OTHERS_ONLY_TEAM;
		}
		else if(Runtime.m_ShowOthers == SHOW_OTHERS_NOT_SET || Runtime.m_ShowOthers != g_Config.m_ClShowOthers)
		{
			CNetMsg_Cl_ShowOthers Msg;
			Msg.m_Show = g_Config.m_ClShowOthers;
			ClientNetwork()->SendPackMsg(Seat, &Msg, MSGFLAG_VITAL);

			// update state
			Runtime.m_ShowOthers = g_Config.m_ClShowOthers;
		}

		CGameState *pMainState = &Session.SeatState(IClient::CONN_MAIN);
		CGameState *pDummyState = &Session.SeatState(IClient::CONN_DUMMY);
		dbg_assert(pMainState != nullptr && pDummyState != nullptr, "missing Network game states");
		CGameState::CRuntimeState &MainRuntime = pMainState->m_Runtime;
		if(MainRuntime.m_EnableSpectatorCount == -1 || MainRuntime.m_EnableSpectatorCount != g_Config.m_ClShowhudSpectatorCount)
		{
			CNetMsg_Cl_EnableSpectatorCount Msg;
			Msg.m_Enable = g_Config.m_ClShowhudSpectatorCount;
			ClientNetwork()->SendPackMsg(IClient::CONN_MAIN, &Msg, MSGFLAG_VITAL);
			MainRuntime.m_EnableSpectatorCount = g_Config.m_ClShowhudSpectatorCount;
		}
		CGameState::CRuntimeState &DummyRuntime = pDummyState->m_Runtime;
		if(DummyConnected() && (DummyRuntime.m_EnableSpectatorCount == -1 || DummyRuntime.m_EnableSpectatorCount != g_Config.m_ClShowhudSpectatorCount))
		{
			CNetMsg_Cl_EnableSpectatorCount Msg;
			Msg.m_Enable = g_Config.m_ClShowhudSpectatorCount;
			ClientNetwork()->SendPackMsg(IClient::CONN_DUMMY, &Msg, MSGFLAG_VITAL);
			DummyRuntime.m_EnableSpectatorCount = g_Config.m_ClShowhudSpectatorCount;
		}

		// Each seat is told what the view it is shown in covers. On a single
		// screen that is the view the player looks through, for both of them as
		// in DDNet. Side by side, the dummy has a pane of its own, with its own
		// zoom and a narrower shape than the whole screen.
		for(int ViewSeat = IClient::CONN_MAIN; ViewSeat < NUM_DUMMIES; ++ViewSeat)
		{
			CCameraSent &Sent = m_aCameraSent[ViewSeat];
			if(ViewSeat == IClient::CONN_DUMMY && !DummyConnected())
			{
				Sent = {};
				continue;
			}
			const CGameView &SeatView = g_Config.m_ClDummySplitScreen ? GameView(SeatSessionId(ViewSeat)) : InputView();
			const CGameView::CCameraState &Camera = SeatView.m_Camera;
			float ShowDistanceZoom = Camera.m_Zoom;
			float Zoom = Camera.m_Zoom;
			if(Camera.m_Zooming)
			{
				if(Camera.m_ZoomSmoothingTarget > Camera.m_Zoom) // Zooming out
					ShowDistanceZoom = Camera.m_ZoomSmoothingTarget;
				else if(Camera.m_ZoomSmoothingTarget < Camera.m_Zoom && Sent.m_ShowDistanceZoom > 0) // Zooming in
					ShowDistanceZoom = Sent.m_ShowDistanceZoom;

				Zoom = Camera.m_ZoomSmoothingTarget;
			}

			float Deadzone = m_Camera.Deadzone();
			float FollowFactor = m_Camera.FollowFactor();
			const CGameState &SeatState = Session.SeatState(ViewSeat);
			if(SeatState.m_Snap.m_SpecInfo.m_Active && Sent.m_Sent)
			{
				// don't send camera information when spectating
				Zoom = Sent.m_Zoom;
				Deadzone = Sent.m_Deadzone;
				FollowFactor = Sent.m_FollowFactor;
			}

			// The size itself decides, not what went into it: the zoom, the screen
			// and the setting for wide screens all move it, and the server only
			// cares that it clips to what is on screen.
			const CViewport &Viewport = SeatView.Viewport();
			const float Aspect = Viewport.m_Width > 0 && Viewport.m_Height > 0 ? Viewport.m_Width / (float)Viewport.m_Height : Graphics()->ScreenAspect();
			float ShowDistanceX, ShowDistanceY;
			Graphics()->CalcScreenParams(Aspect, ShowDistanceZoom, &ShowDistanceX, &ShowDistanceY);
			if(!Sent.m_Sent || ShowDistanceX != Sent.m_ShowDistance.x || ShowDistanceY != Sent.m_ShowDistance.y)
			{
				CNetMsg_Cl_ShowDistance Msg;
				Msg.m_X = ShowDistanceX;
				Msg.m_Y = ShowDistanceY;
				if(ViewSeat == IClient::CONN_MAIN)
					ClientNetwork()->ChecksumData()->m_Zoom = ShowDistanceZoom;
				CMsgPacker Packer(&Msg);
				Msg.Pack(&Packer);
				ClientNetwork()->SendMsg(ViewSeat, &Packer, MSGFLAG_VITAL);
			}

			if(!Sent.m_Sent || Zoom != Sent.m_Zoom || Deadzone != Sent.m_Deadzone || FollowFactor != Sent.m_FollowFactor)
			{
				CNetMsg_Cl_CameraInfo Msg;
				Msg.m_Zoom = round_truncate(Zoom * 1000.f);
				Msg.m_Deadzone = Deadzone;
				Msg.m_FollowFactor = FollowFactor;
				CMsgPacker Packer(&Msg);
				Msg.Pack(&Packer);
				ClientNetwork()->SendMsg(ViewSeat, &Packer, MSGFLAG_VITAL);
			}

			Sent.m_Sent = true;
			Sent.m_ShowDistanceZoom = ShowDistanceZoom;
			Sent.m_ShowDistance = vec2(ShowDistanceX, ShowDistanceY);
			Sent.m_Zoom = Zoom;
			Sent.m_Deadzone = Deadzone;
			Sent.m_FollowFactor = FollowFactor;
		}
	}

	for(auto &pComponent : m_vpAll)
		pComponent->OnNewSnapshot();

	// notify editor when local character moved
	UpdateEditorIngameMoved();

	if(g_Config.m_ClFreezeStars && !m_SuppressEvents)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			auto &Character = Snap.m_aCharacters[ClientId];
			if(Character.m_Active && Character.m_HasExtendedData && Character.m_pPrevExtendedData)
			{
				int FreezeTimeNow = Character.m_ExtendedData.m_FreezeEnd - Sessions()->GameTick(SessionId);
				int FreezeTimePrev = Character.m_pPrevExtendedData->m_FreezeEnd - Sessions()->PrevGameTick(SessionId);
				vec2 Pos = vec2(Character.m_Cur.m_X, Character.m_Cur.m_Y);
				int StarsNow = (FreezeTimeNow + 1) / Sessions()->GameTickSpeed();
				int StarsPrev = (FreezeTimePrev + 1) / Sessions()->GameTickSpeed();
				if(StarsNow < StarsPrev || (StarsPrev == 0 && StarsNow > 0))
				{
					int Amount = StarsNow + 1;
					float Mid = 3 * pi / 2;
					float Min = Mid - pi / 3;
					float Max = Mid + pi / 3;
					for(int j = 0; j < Amount; j++)
					{
						float Angle = mix(Min, Max, (j + 1) / (float)(Amount + 2));
						m_Effects.DamageIndicator(ActiveState, Pos, direction(Angle), ClientId, 1.0f);
					}
				}
			}
		}
	}

	// Record m_LastRaceTick for g_Config.m_ClConfirmDisconnect/QuitTime
	if(ActiveState.CoreGameInfo().m_Race &&
		NetworkSource &&
		Snap.m_pGameInfoObj &&
		!Snap.m_SpecInfo.m_Active &&
		Snap.m_pLocalCharacter &&
		Snap.m_pLocalPrevCharacter)
	{
		const bool RaceFlag = Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME;
		Runtime.m_LastRaceTick = RaceFlag ? -Snap.m_pGameInfoObj->m_WarmupTimer : -1;
	}

	SnapCollectEntities(SessionId); // creates a collection that associates EntityEx snap items with the entities they belong to

	UpdateLocalTuning(SessionId, Session, ActiveState);
	m_PreviousInputSessionId = CSessionId();
	if(NetworkSource && m_pPrediction != nullptr)
		m_pPrediction->OnNewSnapshot(*this);
}

std::function<bool(int, int, int, int)> CGameClient::GetScoreComparator(bool TimeScore, bool ReceivedMillisecondFinishTimes, bool Race7)
{
	// 0.7 race score
	if(Race7)
	{
		auto CompareTimeMillis07 = [](int TimeMillis1, int TimeMillis2, int, int) {
			TimeMillis1 = TimeMillis1 == protocol7::FinishTime::NOT_FINISHED ? std::numeric_limits<int>::max() : TimeMillis1;
			TimeMillis2 = TimeMillis2 == protocol7::FinishTime::NOT_FINISHED ? std::numeric_limits<int>::max() : TimeMillis2;
			return TimeMillis1 < TimeMillis2;
		};
		return CompareTimeMillis07;
	}

	// normal scores (like points), biggest score is highest in scoreboard
	if(!TimeScore)
	{
		auto CompareScore = [](int Score1, int Score2, int, int) {
			return Score1 > Score2;
		};
		return CompareScore;
	}

	// 'classical' times, times are send negative, so biggest value has shortest time
	if(!ReceivedMillisecondFinishTimes)
	{
		auto CompareTimeScore = [](int TimeScore1, int TimeScore2, int, int) {
			TimeScore1 = TimeScore1 == FinishTime::NOT_FINISHED_TIMESCORE ? std::numeric_limits<int>::min() : TimeScore1;
			TimeScore2 = TimeScore2 == FinishTime::NOT_FINISHED_TIMESCORE ? std::numeric_limits<int>::min() : TimeScore2;
			return TimeScore1 > TimeScore2;
		};
		return CompareTimeScore;
	}

	// long precise times, smallest value first, subsorting by milliseconds
	auto CompareTimeMillis = [](int TimeSeconds1, int TimeSeconds2, int TimeMillis1, int TimeMillis2) {
		TimeSeconds1 = TimeSeconds1 == FinishTime::UNSET || TimeSeconds1 == FinishTime::NOT_FINISHED_MILLIS ? std::numeric_limits<int>::max() : TimeSeconds1;
		TimeSeconds2 = TimeSeconds2 == FinishTime::UNSET || TimeSeconds2 == FinishTime::NOT_FINISHED_MILLIS ? std::numeric_limits<int>::max() : TimeSeconds2;
		if(TimeSeconds1 == TimeSeconds2)
			return TimeMillis1 < TimeMillis2;
		return TimeSeconds1 < TimeSeconds2;
	};
	return CompareTimeMillis;
}

void CGameClient::UpdateEditorIngameMoved()
{
	const bool LocalCharacterMoved = Snap().m_pLocalCharacter && Snap().m_pLocalPrevCharacter && (Snap().m_pLocalCharacter->m_X != Snap().m_pLocalPrevCharacter->m_X || Snap().m_pLocalCharacter->m_Y != Snap().m_pLocalPrevCharacter->m_Y);
	if(!g_Config.m_ClEditor)
	{
		m_EditorMovementDelay = 5;
	}
	else if(m_EditorMovementDelay > 0 && !LocalCharacterMoved)
	{
		--m_EditorMovementDelay;
	}
	if(m_EditorMovementDelay == 0 && LocalCharacterMoved && Editor() != nullptr)
	{
		Editor()->OnIngameMoved();
	}
}

void CGameClient::OnPredict(CSessionId SessionId)
{
	if(m_pPrediction != nullptr)
		m_pPrediction->Predict(*this, SessionId);
}

void CGameClient::OnActivateEditor()
{
	OnRelease();
}

void CGameClient::CClientData::UpdateSkinInfo(const CGameState &State)
{
	const CGameState::CProtocol7ClientState &Protocol7Client = State.Protocol7Client(ClientId());
	const CSkinDescriptor SkinDescriptor = ToSkinDescriptor(State);
	if(SkinDescriptor.m_Flags == 0)
	{
		return;
	}

	const auto &&ApplySkinProperties = [&]() {
		if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SIX)
		{
			m_pSkinInfo->TeeRenderInfo().ApplyColors(m_UseCustomColor, m_ColorBody, m_ColorFeet);
		}
		if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SEVEN)
		{
			CTeeRenderInfo::CSixup &SixupSkinInfo = m_pSkinInfo->TeeRenderInfo().m_Sixup;
			for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
			{
				m_pGameClient->m_Skins7.ApplyColorTo(SixupSkinInfo, Protocol7Client.m_aUseCustomColors[Part], Protocol7Client.m_aSkinPartColors[Part], Part);
			}
			UpdateSkin7HatSprite(Protocol7Client);
			UpdateSkin7BotDecoration(Protocol7Client);
		}
		m_pSkinInfo->TeeRenderInfo().m_Size = 64.0f;
	};

	if(m_pSkinInfo == nullptr)
	{
		CTeeRenderInfo TeeRenderInfo;
		m_pSkinInfo = m_pGameClient->CreateManagedTeeRenderInfo(TeeRenderInfo, SkinDescriptor);
		m_pSkinInfo->SetRefreshCallback([&]() { UpdateRenderInfo(); });
		ApplySkinProperties();
		m_pSkinInfo->m_RefreshCallback();
	}
	else if(m_pSkinInfo->SkinDescriptor() != SkinDescriptor)
	{
		m_pSkinInfo->m_SkinDescriptor = SkinDescriptor;
		m_pGameClient->RefreshSkin(m_pSkinInfo);
		ApplySkinProperties();
	}
	else
	{
		ApplySkinProperties();
		m_pSkinInfo->m_RefreshCallback();
	}
}

void CGameClient::CClientData::UpdateRenderInfo()
{
	m_RenderInfo = m_pSkinInfo->TeeRenderInfo();

	// force team colors
	if(m_pGameClient->IsTeamPlay())
	{
		m_RenderInfo.m_CustomColoredSkin = true;
		std::fill(std::begin(m_RenderInfo.m_Sixup.m_aUseCustomColors), std::end(m_RenderInfo.m_Sixup.m_aUseCustomColors), true);

		if(m_Team >= TEAM_RED && m_Team <= TEAM_BLUE)
		{
			const int aTeamColors[2] = {65461, 10223541};
			m_RenderInfo.m_ColorBody = color_cast<ColorRGBA>(ColorHSLA(aTeamColors[m_Team]));
			m_RenderInfo.m_ColorFeet = color_cast<ColorRGBA>(ColorHSLA(aTeamColors[m_Team]));

			// 0.7
			CTeeRenderInfo::CSixup &Sixup = m_RenderInfo.m_Sixup;
			const ColorRGBA aTeamColorsSixup[2] = {
				ColorRGBA(0.753f, 0.318f, 0.318f, 1.0f),
				ColorRGBA(0.318f, 0.471f, 0.753f, 1.0f)};
			const ColorRGBA aMarkingColorsSixup[2] = {
				ColorRGBA(0.824f, 0.345f, 0.345f, 1.0f),
				ColorRGBA(0.345f, 0.514f, 0.824f, 1.0f)};
			float MarkingAlpha = Sixup.m_aColors[protocol7::SKINPART_MARKING].a;
			for(auto &Color : Sixup.m_aColors)
			{
				Color = aTeamColorsSixup[m_Team];
			}
			if(MarkingAlpha > 0.1f)
			{
				Sixup.m_aColors[protocol7::SKINPART_MARKING] = aMarkingColorsSixup[m_Team];
			}
		}
		else
		{
			m_RenderInfo.m_ColorBody = color_cast<ColorRGBA>(ColorHSLA(12829350));
			m_RenderInfo.m_ColorFeet = color_cast<ColorRGBA>(ColorHSLA(12829350));
			for(auto &Color : m_RenderInfo.m_Sixup.m_aColors)
			{
				Color = color_cast<ColorRGBA>(ColorHSLA(12829350));
			}
		}
	}
}

void CGameClient::CClientData::Reset()
{
	m_UseCustomColor = 0;
	m_ColorBody = 0;
	m_ColorFeet = 0;

	m_aName[0] = '\0';
	m_aClan[0] = '\0';
	m_Country = CountryCode::DEFAULT;
	str_copy(m_aSkinName, "default");

	m_Team = 0;

	m_Predicted.Reset();
	m_PrevPredicted.Reset();

	if(m_pSkinInfo != nullptr)
	{
		// Make sure other `shared_ptr`s to this skin info will not use the refresh callback that refers to this reset client data
		m_pSkinInfo->SetRefreshCallback(nullptr);
		m_pSkinInfo = nullptr;
	}
	m_RenderInfo.Reset();

	m_Angle = 0.0f;
	m_Active = false;
	m_Friend = false;
	m_Foe = false;

	std::fill(std::begin(m_aSwitchStates), std::end(m_aSwitchStates), 0);

	m_Snapped.m_Tick = -1;
	m_Evolved.m_Tick = -1;

	for(auto &PreInput : m_aPreInputs)
	{
		PreInput.m_IntendedTick = -1;
	}
}

CSkinDescriptor CGameClient::CClientData::ToSkinDescriptor(const CGameState &State) const
{
	CSkinDescriptor SkinDescriptor;
	const CGameState::CProtocol7ClientState &Protocol7Client = State.Protocol7Client(ClientId());

	if(m_Active && !Protocol7Client.m_Active)
	{
		SkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SIX;
		str_copy(SkinDescriptor.m_aSkinName, m_aSkinName);
	}
	else if(Protocol7Client.m_Active)
	{
		SkinDescriptor.m_Flags |= CSkinDescriptor::FLAG_SEVEN;
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
		{
			str_copy(SkinDescriptor.m_Sixup.m_aaSkinPartNames[Part], Protocol7Client.m_aaSkinPartNames[Part]);
		}
		SkinDescriptor.m_Sixup.m_XmasHat = time_season() == ETimeSeason::XMAS;
		SkinDescriptor.m_Sixup.m_BotDecoration = (Protocol7Client.m_PlayerFlags & protocol7::PLAYERFLAG_BOT) != 0;
	}

	return SkinDescriptor;
}

void CGameClient::SendSwitchTeam(int Team) const
{
	if(Sessions()->FocusedSessionId() != NetworkSessionId())
		return;
	CNetMsg_Cl_SetTeam Msg;
	Msg.m_Team = Team;
	ClientNetwork()->SendPackMsg(InputSeat(), &Msg, MSGFLAG_VITAL);
}

void CGameClient::SendStartInfo7(int Conn)
{
	const CLocalPlayerProfile Profile = PlayerProfile(Conn);
	protocol7::CNetMsg_Cl_StartInfo Msg;
	Msg.m_pName = Profile.m_Name.c_str();
	Msg.m_pClan = Profile.m_Clan.c_str();
	Msg.m_Country = Profile.m_Country;
	for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = CSkins7::ms_apSkinVariables[Conn][p];
		Msg.m_aUseCustomColors[p] = *CSkins7::ms_apUCCVariables[Conn][p];
		Msg.m_aSkinPartColors[p] = *CSkins7::ms_apColorVariables[Conn][p];
	}
	CMsgPacker Packer(&Msg, false, true);
	if(Msg.Pack(&Packer))
		return;
	ClientNetwork()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	SessionContext(NetworkSessionId()).SeatState(Conn).m_Runtime.m_CheckInfo = -1;
}

CLocalPlayerProfile CGameClient::PlayerProfile(int Conn) const
{
	const bool UseDummyProfile = Conn == IClient::CONN_DUMMY;
	return CLocalPlayerProfile::FromLegacyConfig(*Config(), UseDummyProfile, UseDummyProfile ? DummyName() : PlayerName());
}

void CGameClient::SendSkinChange7(int Conn) const
{
	protocol7::CNetMsg_Cl_SkinChange Msg;
	for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = CSkins7::ms_apSkinVariables[Conn][p];
		Msg.m_aUseCustomColors[p] = *CSkins7::ms_apUCCVariables[Conn][p];
		Msg.m_aSkinPartColors[p] = *CSkins7::ms_apColorVariables[Conn][p];
	}
	CMsgPacker Packer(&Msg, false, true);
	if(Msg.Pack(&Packer))
		return;
	ClientNetwork()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	SessionContext(NetworkSessionId()).SeatState(Conn).m_Runtime.m_CheckInfo = Sessions()->GameTickSpeed();
}

bool CGameClient::GotWantedSkin7(int Conn) const
{
	// validate the wanted skinparts before comparison
	// because the skin parts we compare against are also validated
	// otherwise it tries to resend the skin info when the eyes are set to "negative"
	// in team based modes
	char aSkinParts[protocol7::NUM_SKINPARTS][protocol7::MAX_SKIN_ARRAY_SIZE];
	char *apSkinPartsPtr[protocol7::NUM_SKINPARTS];
	int aUCCVars[protocol7::NUM_SKINPARTS];
	int aColorVars[protocol7::NUM_SKINPARTS];
	for(int SkinPart = 0; SkinPart < protocol7::NUM_SKINPARTS; SkinPart++)
	{
		str_copy(aSkinParts[SkinPart], CSkins7::ms_apSkinVariables[Conn][SkinPart], protocol7::MAX_SKIN_ARRAY_SIZE);
		apSkinPartsPtr[SkinPart] = aSkinParts[SkinPart];
		aUCCVars[SkinPart] = *CSkins7::ms_apUCCVariables[Conn][SkinPart];
		aColorVars[SkinPart] = *CSkins7::ms_apColorVariables[Conn][SkinPart];
	}
	m_Skins7.ValidateSkinParts(apSkinPartsPtr, aUCCVars, aColorVars, Sessions()->TranslationContext(NetworkSessionId()).m_GameFlags);

	const CGameSessionContext &Session = SessionContext(NetworkSessionId());
	const CGameState &State = Session.SeatState(Conn);
	const int LocalClientId = State.LocalClientId();
	if(LocalClientId < 0 || LocalClientId >= MAX_CLIENTS)
		return false;
	const CGameState::CProtocol7ClientState &Protocol7Client = State.Protocol7Client(LocalClientId);
	for(int SkinPart = 0; SkinPart < protocol7::NUM_SKINPARTS; SkinPart++)
	{
		if(str_comp(Protocol7Client.m_aaSkinPartNames[SkinPart], apSkinPartsPtr[SkinPart]))
			return false;
		if(Protocol7Client.m_aUseCustomColors[SkinPart] != aUCCVars[SkinPart])
			return false;
		if(Protocol7Client.m_aSkinPartColors[SkinPart] != aColorVars[SkinPart])
			return false;
	}

	// TODO: add name change ddnet extension to 0.7 protocol
	// if(str_comp(m_aClients[LocalClientId].m_aName, ProfileIndex ? DummyName() : PlayerName()))
	// 	return false;
	// if(str_comp(m_aClients[LocalClientId].m_aClan, ProfileIndex ? g_Config.m_ClDummyClan : g_Config.m_PlayerClan))
	// 	return false;
	// if(m_aClients[LocalClientId].m_Country != (ProfileIndex ? g_Config.m_ClDummyCountry : g_Config.m_PlayerCountry))
	// 	return false;

	return true;
}

void CGameClient::SendInfo(bool Start)
{
	SendConnectionInfo(IClient::CONN_MAIN, Start);
}

void CGameClient::SendDummyInfo(bool Start)
{
	SendConnectionInfo(IClient::CONN_DUMMY, Start);
}

void CGameClient::SendConnectionInfo(int Conn, bool Start)
{
	if(m_pSessions->IsSixup(NetworkSessionId()))
	{
		if(Start)
			SendStartInfo7(Conn);
		else
			SendSkinChange7(Conn);
		return;
	}
	CGameState &State = SessionContext(NetworkSessionId()).SeatState(Conn);
	const CLocalPlayerProfile Profile = PlayerProfile(Conn);
	if(Start)
	{
		CNetMsg_Cl_StartInfo Msg;
		Msg.m_pName = Profile.m_Name.c_str();
		Msg.m_pClan = Profile.m_Clan.c_str();
		Msg.m_Country = Profile.m_Country;
		Msg.m_pSkin = Profile.m_Skin.c_str();
		Msg.m_UseCustomColor = Profile.m_UseCustomColor;
		Msg.m_ColorBody = Profile.m_ColorBody;
		Msg.m_ColorFeet = Profile.m_ColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		ClientNetwork()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
		State.m_Runtime.m_CheckInfo = -1;
	}
	else
	{
		CNetMsg_Cl_ChangeInfo Msg;
		Msg.m_pName = Profile.m_Name.c_str();
		Msg.m_pClan = Profile.m_Clan.c_str();
		Msg.m_Country = Profile.m_Country;
		Msg.m_pSkin = Profile.m_Skin.c_str();
		Msg.m_UseCustomColor = Profile.m_UseCustomColor;
		Msg.m_ColorBody = Profile.m_ColorBody;
		Msg.m_ColorFeet = Profile.m_ColorFeet;
		CMsgPacker Packer(&Msg);
		Msg.Pack(&Packer);
		ClientNetwork()->SendMsg(Conn, &Packer, MSGFLAG_VITAL);
		State.m_Runtime.m_CheckInfo = Sessions()->GameTickSpeed();
	}
}

void CGameClient::SendKill() const
{
	if(Sessions()->FocusedSessionId() != NetworkSessionId())
		return;
	const int ActiveSeat = InputSeat();
	CNetMsg_Cl_Kill Msg;
	ClientNetwork()->SendPackMsg(ActiveSeat, &Msg, MSGFLAG_VITAL);

	if(g_Config.m_ClDummyCopyMoves)
	{
		CMsgPacker MsgP(NETMSGTYPE_CL_KILL, false);
		ClientNetwork()->SendMsg(ActiveSeat ^ 1, &MsgP, MSGFLAG_VITAL);
	}
}

void CGameClient::SendReadyChange7() // NOLINT(readability-make-member-function-const)
{
	if(Sessions()->FocusedSessionId() != NetworkSessionId())
		return;
	if(!Sessions()->IsSixup(NetworkSessionId()))
	{
		log_error("client", "You have to be connected to a 0.7 server to use 'ready_change'");
		return;
	}
	protocol7::CNetMsg_Cl_ReadyChange Msg;
	ClientNetwork()->SendPackMsg(InputSeat(), &Msg, MSGFLAG_VITAL, true);
}

void CGameClient::ConTeam(IConsole::IResult *pResult, void *pUserData)
{
	((CGameClient *)pUserData)->SendSwitchTeam(pResult->GetInteger(0));
}

void CGameClient::ConKill(IConsole::IResult *pResult, void *pUserData)
{
	((CGameClient *)pUserData)->SendKill();
}

void CGameClient::ConReadyChange7(IConsole::IResult *pResult, void *pUserData)
{
	CGameClient *pClient = static_cast<CGameClient *>(pUserData);
	if(pClient->Client()->IsOnline())
		pClient->SendReadyChange7();
}

void CGameClient::ConchainLanguageUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	const bool Changed = pThis->Client()->GlobalTime() && pResult->NumArguments() && str_comp(pResult->GetString(0), g_Config.m_ClLanguagefile) != 0;
	pfnCallback(pResult, pCallbackUserData);
	if(Changed)
	{
		pThis->OnLanguageChange();
	}
}

void CGameClient::ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		CGameClient *pSelf = (CGameClient *)pUserData;
		pSelf->SendInfo(false);
	}
}

void CGameClient::ConchainSpecialDummyInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
		((CGameClient *)pUserData)->SendDummyInfo(false);
}

void CGameClient::ConchainSpecialDummy(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments())
	{
		auto *pSelf = static_cast<CGameClient *>(pUserData);
		if(g_Config.m_ClDummy && !pSelf->DummyConnected())
			g_Config.m_ClDummy = 0;
	}
}

IGameClient *CreateGameClient()
{
	return new CGameClient();
}

ColorRGBA CalculateNameColor(ColorHSLA TextColorHSL)
{
	return color_cast<ColorRGBA>(ColorHSLA(TextColorHSL.h, TextColorHSL.s * 0.68f, TextColorHSL.l * 0.81f));
}

void CGameClient::UpdateLocalTuning(CSessionId SessionId, CGameSessionContext &Session, CGameState &State)
{
	CGameState::CRuntimeState &Runtime = State.m_Runtime;
	const CGameState *pPreviousFocusedState = Session.FindGameState(m_PreviousInputSessionId);
	GameWorld().m_WorldConfig.m_UseTuneZones = State.CoreGameInfo().m_PredictDDRaceTiles;

	// always update default tune zone, even without character
	if(!GameWorld().m_WorldConfig.m_UseTuneZones)
		GameWorld().TuningList()[0] = Runtime.m_CurrentTuning;

	if(!Snap().m_pLocalCharacter && !Snap().m_pSpectatorInfo)
		return;

	vec2 LocalPos = Snap().m_pLocalCharacter ? vec2(Snap().m_pLocalCharacter->m_X, Snap().m_pLocalCharacter->m_Y) : vec2(Snap().m_pSpectatorInfo->m_X, Snap().m_pSpectatorInfo->m_Y);

	// update the tuning at the local position with the latest tunings received before the new snapshot
	if(GameWorld().m_WorldConfig.m_UseTuneZones)
	{
		int TuneZone =
			Snap().m_aCharacters[Snap().m_LocalClientId].m_HasExtendedData &&
					Snap().m_aCharacters[Snap().m_LocalClientId].m_ExtendedData.m_TuneZoneOverride != TuneZone::OVERRIDE_NONE ?
				Snap().m_aCharacters[Snap().m_LocalClientId].m_ExtendedData.m_TuneZoneOverride :
				Session.m_MapContext.Collision()->IsTune(Session.m_MapContext.Collision()->GetMapIndex(LocalPos));

		if(TuneZone != Runtime.m_LocalTuneZone)
		{
			// our tunezone changed, expecting tuning message
			Runtime.m_LocalTuneZone = Runtime.m_ExpectingTuningForZone = TuneZone;
			Runtime.m_ExpectingTuningSince = 0;
		}

		// tunezone could have changed, send dummy tuning to demo
		if(m_ActiveRecordings.any() && pPreviousFocusedState && Runtime.m_LocalTuneZone != pPreviousFocusedState->m_Runtime.m_LocalTuneZone)
		{
			CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
			int *pParams = (int *)&Runtime.m_CurrentTuning;
			for(unsigned i = 0; i < sizeof(Runtime.m_CurrentTuning) / sizeof(int); i++)
				Msg.AddInt(pParams[i]);
			ClientNetwork()->SendMsg(State.m_Seat, &Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND);
		}

		if(Runtime.m_ExpectingTuningForZone >= 0)
		{
			if(Runtime.m_ReceivedTuning)
			{
				Session.m_MapContext.TuningList()[Runtime.m_ExpectingTuningForZone] = Runtime.m_CurrentTuning;
				GameWorld().TuningList()[Runtime.m_ExpectingTuningForZone] = Runtime.m_CurrentTuning;
				Runtime.m_ReceivedTuning = false;
				Runtime.m_ExpectingTuningForZone = -1;
			}
			else if(Runtime.m_ExpectingTuningSince >= 5)
			{
				// if we are expecting tuning for more than 10 snaps (less than a quarter of a second)
				// it is probably dropped or it was received out of order
				// or applied to another tunezone.
				// we need to fallback to current tuning to fix ourselves.
				Runtime.m_ExpectingTuningForZone = -1;
				Runtime.m_ExpectingTuningSince = 0;
				Runtime.m_ReceivedTuning = false;
				log_debug("tunezone", "the tuning was missed");
			}
			else
			{
				// if we are expecting tuning and have not received one yet.
				// do not update any tuning, so we don't apply it to the wrong tunezone.
				log_debug("tunezone", "waiting for tuning for zone %d", Runtime.m_ExpectingTuningForZone);
				Runtime.m_ExpectingTuningSince++;
			}
		}
		else
		{
			// if we have processed what we need, and the tuning is still wrong due to out of order message
			// fix our tuning by using the current one
			GameWorld().TuningList()[TuneZone] = Runtime.m_CurrentTuning;
			Runtime.m_ExpectingTuningSince = 0;
			Runtime.m_ReceivedTuning = false;
		}
	}
}

CPhysicsRules CGameClient::PredictedPhysicsRules(const CGameSessionContext &Session, const CGameState &State) const
{
	return ::PredictedPhysicsRules(State.CoreGameInfo(), Session.m_MapContext.GameConfig().Values());
}

void CGameClient::UpdateRenderedClients(const CGameSessionContext &Session, CGameState &State, int64_t Now, const CGameTickInfo &Time, EPresentationPlayback Playback)
{
	const int LocalClientId = State.LocalClientId();
	const CGameState::CClientSnapshot *pLocalClient = in_range(LocalClientId, MAX_CLIENTS - 1) ? &State.Client(LocalClientId) : nullptr;
	const bool LocalSpectating = pLocalClient == nullptr ||
				     (pLocalClient->m_HasPlayerInfo && pLocalClient->m_PlayerInfo.m_Team == TEAM_SPECTATORS) ||
				     (pLocalClient->m_HasDDNetPlayer && (pLocalClient->m_DDNetPlayer.m_Flags & (EXPLAYERFLAG_PAUSED | EXPLAYERFLAG_SPEC)) != 0);
	const bool Predict = g_Config.m_ClPredict && Playback == EPresentationPlayback::PLAYING && !Time.m_IsDemoPlayback && !LocalSpectating && pLocalClient != nullptr && pLocalClient->m_HasCharacter;
	const int AntiPingPlayers = g_Config.m_ClAntiPing && g_Config.m_ClAntiPingPlayers ? g_Config.m_ClAntiPingPlayers : 0;
	const bool AntiPingGunfire = g_Config.m_ClAntiPing && g_Config.m_ClAntiPingGrenade && g_Config.m_ClAntiPingWeapons && g_Config.m_ClAntiPingGunfire;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		bool IsSecondaryLocal = false;
		for(const CGameState &SessionState : Session.GameStates())
		{
			if(&SessionState == &State || SessionState.LocalClientId() != ClientId)
				continue;
			const CGameState::CClientSnapshot &SessionLocalClient = SessionState.Client(ClientId);
			IsSecondaryLocal = !SessionLocalClient.m_HasDDNetPlayer || (SessionLocalClient.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_PAUSED) == 0;
			break;
		}
		const bool PredictedLocal = ClientId == LocalClientId || (g_Config.m_ClPredictDummy && IsSecondaryLocal);
		const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
		const CGameState::CPredictedClient &PredictedClient = State.PredictedClient(ClientId);
		const CCharacter *pCharacter = State.m_PredictedWorld.GetCharacterById(ClientId);
		const bool AntiPingPlayer = AntiPingPlayers == 1 || (AntiPingPlayers >= 2 && (PredictedLocal || (pCharacter != nullptr && pCharacter->IsInterfering())));
		const bool UsePredicted = Predict && PredictedClient.m_HasPrev && PredictedClient.m_HasCurrent && pCharacter != nullptr &&
					  (ClientId == LocalClientId || (AntiPingPlayer && !State.IsOtherTeamFromLocalPlayer(ClientId)));
		State.UpdateRenderedClient(ClientId, UsePredicted, PredictedLocal, Time.m_IntraGameTick, Time.m_PredIntraGameTick);
		CGameState::CRenderedClient &RenderedClient = State.RenderedClient(ClientId);
		if(!RenderedClient.m_IsPredicted)
			continue;

		if(RenderedClient.m_IsPredictedLocal && AntiPingGunfire &&
			((pCharacter->m_NinjaJetpack && pCharacter->m_FreezeTime == 0) || SnapshotClient.m_Character.m_Weapon != WEAPON_NINJA || SnapshotClient.m_Character.m_Weapon == PredictedClient.m_Current.m_ActiveWeapon))
		{
			RenderedClient.m_Cur.m_AttackTick = pCharacter->GetAttackTick();
			if(SnapshotClient.m_Character.m_Weapon != WEAPON_NINJA && !(pCharacter->m_NinjaJetpack && pCharacter->Core()->m_ActiveWeapon == WEAPON_GUN))
				RenderedClient.m_Cur.m_Weapon = PredictedClient.m_Current.m_ActiveWeapon;
		}
		else if(!RenderedClient.m_IsPredictedLocal)
		{
			RenderedClient.m_Prev.m_Angle = SnapshotClient.m_PrevCharacter.m_Angle;
			RenderedClient.m_Cur.m_Angle = SnapshotClient.m_Character.m_Angle;
			if(g_Config.m_ClAntiPingSmooth)
				RenderedClient.m_Position = GetSmoothPos(State.m_SessionId, State, ClientId, Now, PredictedClient.m_Prev, PredictedClient.m_Current);
		}
	}
}

void CGameClient::UpdateSpectatorCursor(const CGameState &State, const CGameTickInfo &Time)
{
	CGameView &View = InputView();
	CGameView::CSpectatorCursorState &Cursor = View.m_SpectatorCursor;
	using CCursorState = CGameView::CSpectatorCursorState;
	const int CursorOwnerId = View.IsSpectating() ? View.SpectatorId() : State.LocalClientId();

	if(CursorOwnerId != Cursor.m_CursorOwnerId)
	{
		// reset cursor sample count upon changing spectating character
		Cursor.m_NumSamples = 0;
		Cursor.m_CursorOwnerId = CursorOwnerId;
	}

	if(MultiView().m_Active || CursorOwnerId < 0 || CursorOwnerId >= MAX_CLIENTS)
	{
		// do not show spec cursor in multi-view
		Cursor.m_Available = false;
		Cursor.m_NumSamples = 0;
		return;
	}

	const CGameState::CClientSnapshot &CursorOwner = State.Client(CursorOwnerId);
	const CGameState::CRenderedClient &RenderedClient = State.RenderedClient(CursorOwnerId);
	const bool HasExtendedDisplayInfo = CursorOwner.m_HasExtendedCharacter && CursorOwner.m_ExtendedCharacter.m_JumpedTotal != -1;
	const bool CursorOwnerPaused = CursorOwner.m_HasDDNetPlayer && (CursorOwner.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_PAUSED) != 0;
	if(!HasExtendedDisplayInfo || !RenderedClient.m_Active || (!g_Config.m_Debug && CursorOwnerPaused))
	{
		// hide cursor when the spectating player is paused
		Cursor.m_Available = false;
		Cursor.m_NumSamples = 0;
		return;
	}

	Cursor.m_Available = true;
	Cursor.m_Position = RenderedClient.m_Position;
	Cursor.m_Weapon = CursorOwner.m_Character.m_Weapon;

	const vec2 Target = vec2(CursorOwner.m_ExtendedCharacter.m_TargetX, CursorOwner.m_ExtendedCharacter.m_TargetY);

	if(Time.m_IsDemoPlaybackPaused)
	{
		Cursor.m_CursorOwnerId = -1;
		Cursor.m_NumSamples = 0;
		const vec2 TargetNew = vec2(CursorOwner.m_ExtendedCharacter.m_TargetX, CursorOwner.m_ExtendedCharacter.m_TargetY);
		if(CursorOwner.m_HasPrevExtendedCharacter)
		{
			const vec2 TargetOld = vec2(CursorOwner.m_PrevExtendedTargetX, CursorOwner.m_PrevExtendedTargetY);
			Cursor.m_Target = mix(TargetOld, TargetNew, Time.m_IntraGameTick);
		}
		else
		{
			Cursor.m_Target = TargetNew;
		}
	}
	else
	{
		// interpolate cursor positions
		const double Tick = Time.m_GameTick;

		const bool HasSample = Cursor.m_NumSamples > 0;
		const vec2 LastInput = HasSample ? Cursor.m_aTargetSamplesData[Cursor.m_NumSamples - 1] : vec2(0.0f, 0.0f);
		const double LastTime = HasSample ? Cursor.m_aTargetSamplesTime[Cursor.m_NumSamples - 1] : 0.0;
		bool NewSample = LastInput != Target || LastTime + CCursorState::REST_THRESHOLD < Tick;

		if(LastTime > Tick)
		{
			// clear samples when time flows backwards
			Cursor.m_NumSamples = 0;
			NewSample = true;
		}

		if(Cursor.m_NumSamples == 0)
		{
			Cursor.m_aTargetSamplesTime[0] = Tick - CCursorState::INTERP_DELAY;
			Cursor.m_aTargetSamplesData[0] = Target;
		}

		if(NewSample)
		{
			if(Cursor.m_NumSamples == CCursorState::CURSOR_SAMPLES)
			{
				Cursor.m_NumSamples--;
				mem_move(Cursor.m_aTargetSamplesTime, Cursor.m_aTargetSamplesTime + 1, Cursor.m_NumSamples * sizeof(double));
				mem_move(Cursor.m_aTargetSamplesData, Cursor.m_aTargetSamplesData + 1, Cursor.m_NumSamples * sizeof(vec2));
			}
			Cursor.m_aTargetSamplesTime[Cursor.m_NumSamples] = Tick;
			Cursor.m_aTargetSamplesData[Cursor.m_NumSamples] = Target;
			Cursor.m_NumSamples++;
		}

		// using double to avoid precision loss when converting int tick to decimal type
		const double DisplayTime = Tick - CCursorState::INTERP_DELAY + double(Time.m_IntraGameTickSincePrev);
		double aTime[CCursorState::SAMPLE_FRAME_WINDOW];
		vec2 aData[CCursorState::SAMPLE_FRAME_WINDOW];

		// find the available sample timing
		int Index = Cursor.m_NumSamples;
		for(int i = 0; i < Cursor.m_NumSamples; i++)
		{
			if(Cursor.m_aTargetSamplesTime[i] > DisplayTime)
			{
				Index = i;
				break;
			}
		}

		for(int i = 0; i < CCursorState::SAMPLE_FRAME_WINDOW; i++)
		{
			const int Offset = i - CCursorState::SAMPLE_FRAME_OFFSET;
			const int SampleIndex = Index + Offset;
			if(SampleIndex < 0)
			{
				aTime[i] = Cursor.m_aTargetSamplesTime[0] + CCursorState::REST_THRESHOLD * Offset;
				aData[i] = Cursor.m_aTargetSamplesData[0];
			}
			else if(SampleIndex >= Cursor.m_NumSamples)
			{
				// The seeded sample above is only counted once something is
				// appended to it, so a cursor that has not moved yet has none.
				const int LastSample = std::max(Cursor.m_NumSamples - 1, 0);
				aTime[i] = Cursor.m_aTargetSamplesTime[LastSample] + CCursorState::REST_THRESHOLD * (Offset + 1);
				aData[i] = Cursor.m_aTargetSamplesData[LastSample];
			}
			else
			{
				aTime[i] = Cursor.m_aTargetSamplesTime[SampleIndex];
				aData[i] = Cursor.m_aTargetSamplesData[SampleIndex];
			}
		}

		Cursor.m_Target = mix_polynomial(aTime, aData, CCursorState::SAMPLE_FRAME_WINDOW, DisplayTime, vec2(0.0f, 0.0f));
	}

	vec2 TargetCameraOffset(0, 0);
	float l = length(Cursor.m_Target);

	if(l > 0.0001f) // make sure that this isn't 0
	{
		float OffsetAmount = std::max(l - Snap().m_SpecInfo.m_Deadzone, 0.0f) * (Snap().m_SpecInfo.m_FollowFactor / 100.0f);
		TargetCameraOffset = normalize(Cursor.m_Target) * OffsetAmount;
	}

	// if we are in auto spec mode, use camera zoom to smooth out cursor transitions
	const float Zoom = (m_Camera.IsZooming() && m_Camera.IsAutoSpecCameraZooming()) ? m_Camera.Zoom() : Snap().m_SpecInfo.m_Zoom;
	Cursor.m_WorldTarget = Cursor.m_Position + (Cursor.m_Target - TargetCameraOffset) * Zoom + TargetCameraOffset;
}

void CGameClient::HandlePredictedEvents(const int Tick)
{
	const float Alpha = 1.0f;
	const float Volume = 1.0f;

	auto EventsIterator = PredictedWorld().m_PredictedEvents.begin();
	while(EventsIterator != PredictedWorld().m_PredictedEvents.end())
	{
		if(!EventsIterator->m_Handled && EventsIterator->m_Tick <= Tick)
		{
			if(EventsIterator->m_EventId == NETEVENTTYPE_SOUNDWORLD)
			{
				if(FocusedGameInfo().m_RaceSounds && ((EventsIterator->m_ExtraInfo == SOUND_GUN_FIRE && !g_Config.m_SndGun) || (EventsIterator->m_ExtraInfo == SOUND_PLAYER_PAIN_LONG && !g_Config.m_SndLongPain)))
				{
					EventsIterator = PredictedWorld().m_PredictedEvents.erase(EventsIterator);
					continue;
				}
				m_Sounds.PlayAt(CSounds::CHN_WORLD, EventsIterator->m_ExtraInfo, 1.0f, EventsIterator->m_Pos);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_EXPLOSION)
			{
				m_Effects.Explosion(InputState(), *Collision(), EventsIterator->m_Pos, Alpha);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_HAMMERHIT)
			{
				m_Effects.HammerHit(InputState(), EventsIterator->m_Pos, Alpha, Volume);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_DAMAGEIND)
			{
				m_Effects.DamageIndicator(InputState(), EventsIterator->m_Pos, direction(EventsIterator->m_ExtraInfo / 256.0f), -1, Alpha);
			}

			EventsIterator->m_Handled = true;
			++EventsIterator;
			continue;
		}
		else if(Tick - EventsIterator->m_Tick > 3 * Sessions()->GameTickSpeed()) // 3 seconds
		{
			// remove too old events
			EventsIterator = PredictedWorld().m_PredictedEvents.erase(EventsIterator);
		}
		else
		{
			++EventsIterator;
		}
	}
}

void CGameClient::DetectStrongHook(CGameState::CRuntimeState &Runtime) const
{
	const CSessionId SessionId = InputSessionId();
	CTeamsCore Teams = FocusedTeams();
	// attempt to detect strong/weak between players
	for(int FromPlayer = 0; FromPlayer < MAX_CLIENTS; FromPlayer++)
	{
		if(!Snap().m_aCharacters[FromPlayer].m_Active)
			continue;
		int ToPlayer = Snap().m_aCharacters[FromPlayer].m_Prev.m_HookedPlayer;
		if(ToPlayer < 0 || ToPlayer >= MAX_CLIENTS || !Snap().m_aCharacters[ToPlayer].m_Active || ToPlayer != Snap().m_aCharacters[FromPlayer].m_Cur.m_HookedPlayer)
			continue;
		if(absolute(std::min(Runtime.m_aStrongHookLastUpdateTick[ToPlayer], Runtime.m_aStrongHookLastUpdateTick[FromPlayer]) - Sessions()->GameTick(SessionId)) < Sessions()->GameTickSpeed() / 4)
			continue;
		if(Snap().m_aCharacters[FromPlayer].m_Prev.m_Direction != Snap().m_aCharacters[FromPlayer].m_Cur.m_Direction || Snap().m_aCharacters[ToPlayer].m_Prev.m_Direction != Snap().m_aCharacters[ToPlayer].m_Cur.m_Direction)
			continue;

		CCharacter *pFromCharWorld = GameWorld().GetCharacterById(FromPlayer);
		CCharacter *pToCharWorld = GameWorld().GetCharacterById(ToPlayer);
		if(!pFromCharWorld || !pToCharWorld)
			continue;

		Runtime.m_aStrongHookLastUpdateTick[ToPlayer] = Runtime.m_aStrongHookLastUpdateTick[FromPlayer] = Sessions()->GameTick(SessionId);

		float aPredictErr[2];
		CCharacterCore ToCharCur;
		ToCharCur.Read(&Snap().m_aCharacters[ToPlayer].m_Cur);

		CWorldCore World;

		for(int Direction = 0; Direction < 2; Direction++)
		{
			CCharacterCore ToChar = pFromCharWorld->GetCore();
			ToChar.Init(&World, Collision(), &Teams);
			World.m_apCharacters[ToPlayer] = &ToChar;
			ToChar.Read(&Snap().m_aCharacters[ToPlayer].m_Prev);

			CCharacterCore FromChar = pFromCharWorld->GetCore();
			FromChar.Init(&World, Collision(), &Teams);
			World.m_apCharacters[FromPlayer] = &FromChar;
			FromChar.Read(&Snap().m_aCharacters[FromPlayer].m_Prev);

			for(int Tick = Sessions()->PrevGameTick(SessionId); Tick < Sessions()->GameTick(SessionId); Tick++)
			{
				if(Direction == 0)
				{
					FromChar.Tick(false);
					ToChar.Tick(false);
				}
				else
				{
					ToChar.Tick(false);
					FromChar.Tick(false);
				}
				FromChar.Move();
				FromChar.Quantize();
				ToChar.Move();
				ToChar.Quantize();
			}
			aPredictErr[Direction] = distance(ToChar.m_Vel, ToCharCur.m_Vel);
		}
		const float LOW = 0.0001f;
		const float HIGH = 0.07f;
		if(aPredictErr[1] < LOW && aPredictErr[0] > HIGH)
		{
			if(Runtime.m_CharOrder.HasStrongAgainst(ToPlayer, FromPlayer))
			{
				if(ToPlayer != Snap().m_LocalClientId)
					Runtime.m_CharOrder.GiveWeak(ToPlayer);
				else
					Runtime.m_CharOrder.GiveStrong(FromPlayer);
			}
		}
		else if(aPredictErr[0] < LOW && aPredictErr[1] > HIGH)
		{
			if(Runtime.m_CharOrder.HasStrongAgainst(FromPlayer, ToPlayer))
			{
				if(ToPlayer != Snap().m_LocalClientId)
					Runtime.m_CharOrder.GiveStrong(ToPlayer);
				else
					Runtime.m_CharOrder.GiveWeak(FromPlayer);
			}
		}
	}
}

vec2 CGameClient::GetSmoothPos(CSessionId SessionId, const CGameState &State, int ClientId, int64_t Now, const CCharacterCore &Prev, const CCharacterCore &Current) const
{
	vec2 Pos = mix(Prev.m_Pos, Current.m_Pos, Sessions()->PredIntraGameTick(SessionId));
	const CGameState::CClientPredictionHistory &PredictionHistory = State.PredictionHistory(ClientId);
	for(int i = 0; i < 2; i++)
	{
		int64_t Len = std::clamp(PredictionHistory.m_aSmoothLen[i], (int64_t)1, time_freq());
		int64_t TimePassed = Now - PredictionHistory.m_aSmoothStart[i];
		if(in_range(TimePassed, (int64_t)0, Len - 1))
		{
			float MixAmount = 1.f - std::pow(1.f - TimePassed / (float)Len, 1.2f);
			int SmoothTick;
			float SmoothIntra;
			Sessions()->GetSmoothTick(SessionId, Now, &SmoothTick, &SmoothIntra, MixAmount);
			if(SmoothTick > 0 && PredictionHistory.m_aPredTick[(SmoothTick - 1) % 200] >= Sessions()->PrevGameTick(SessionId) && PredictionHistory.m_aPredTick[SmoothTick % 200] <= Sessions()->PredGameTick(SessionId))
				Pos[i] = mix(PredictionHistory.m_aPredPos[(SmoothTick - 1) % 200][i], PredictionHistory.m_aPredPos[SmoothTick % 200][i], SmoothIntra);
		}
	}
	return Pos;
}

void CGameClient::Echo(const char *pString)
{
	m_Chat.Echo(pString);
}

bool CGameClient::IsOtherTeam(int ClientId) const
{
	bool Local = Snap().m_LocalClientId == ClientId;

	if(Snap().m_LocalClientId < 0)
	{
		return false;
	}
	else if((Snap().m_SpecInfo.m_Active && Snap().m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW) || ClientId < 0)
	{
		return false;
	}
	else if(Snap().m_SpecInfo.m_Active && Snap().m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
	{
		if(FocusedTeams().Team(ClientId) == FocusedTeams().TeamSuper() || FocusedTeams().Team(Snap().m_SpecInfo.m_SpectatorId) == FocusedTeams().TeamSuper())
			return false;
		return FocusedTeams().Team(ClientId) != FocusedTeams().Team(Snap().m_SpecInfo.m_SpectatorId);
	}
	else
	{
		const CGameState &State = InputState();
		const CNetObj_DDNetCharacter *pLocalExtended = State.ExtendedCharacter(Snap().m_LocalClientId);
		const CNetObj_DDNetCharacter *pClientExtended = State.ExtendedCharacter(ClientId);
		const bool LocalSolo = pLocalExtended != nullptr && (pLocalExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
		const bool ClientSolo = pClientExtended != nullptr && (pClientExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
		if((LocalSolo || ClientSolo) && !Local)
			return true;
	}

	if(FocusedTeams().Team(ClientId) == FocusedTeams().TeamSuper() || FocusedTeams().Team(Snap().m_LocalClientId) == FocusedTeams().TeamSuper())
		return false;

	return FocusedTeams().Team(ClientId) != FocusedTeams().Team(Snap().m_LocalClientId);
}

void CGameClient::RefreshSkin(const std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo)
{
	CTeeRenderInfo &TeeInfo = pManagedTeeRenderInfo->TeeRenderInfo();
	const CSkinDescriptor &SkinDescriptor = pManagedTeeRenderInfo->SkinDescriptor();

	if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SIX)
	{
		TeeInfo.Apply(m_Skins.Find(SkinDescriptor.m_aSkinName));
	}

	if(SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SEVEN)
	{
		for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
			m_Skins7.FindSkinPart(Part, SkinDescriptor.m_Sixup.m_aaSkinPartNames[Part], true)->ApplyTo(TeeInfo.m_Sixup);

		if(SkinDescriptor.m_Sixup.m_XmasHat)
			TeeInfo.m_Sixup.m_HatTexture = m_Skins7.XmasHatTexture();
		else
			TeeInfo.m_Sixup.m_HatTexture.Invalidate();

		if(SkinDescriptor.m_Sixup.m_BotDecoration)
			TeeInfo.m_Sixup.m_BotTexture = m_Skins7.BotDecorationTexture();
		else
			TeeInfo.m_Sixup.m_BotTexture.Invalidate();
	}

	if(SkinDescriptor.m_Flags != 0 && pManagedTeeRenderInfo->m_RefreshCallback)
	{
		pManagedTeeRenderInfo->m_RefreshCallback();
	}
}

void CGameClient::RefreshSkins(int SkinDescriptorFlags)
{
	dbg_assert(SkinDescriptorFlags != 0, "SkinDescriptorFlags invalid");

	const auto SkinStartLoadTime = time_get_nanoseconds();
	const auto &&ProgressCallback = [&]() {
		// if skin refreshing takes to long, swap to a loading screen
		if(time_get_nanoseconds() - SkinStartLoadTime > 500ms)
		{
			RenderLoading(Localize("Loading skin files"), "", 0);
		}
	};
	if(SkinDescriptorFlags & CSkinDescriptor::FLAG_SIX)
	{
		m_Skins.Refresh(ProgressCallback);
	}
	if(SkinDescriptorFlags & CSkinDescriptor::FLAG_SEVEN)
	{
		m_Skins7.Refresh(ProgressCallback);
	}

	for(std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo : m_vpManagedTeeRenderInfos)
	{
		if(!(pManagedTeeRenderInfo->SkinDescriptor().m_Flags & SkinDescriptorFlags))
		{
			continue;
		}
		RefreshSkin(pManagedTeeRenderInfo);
	}
}

void CGameClient::OnSkinUpdate(const char *pSkinName)
{
	// If the refreshed skin's name starts with the current skin prefix, we also have to
	// refresh skins matching the unprefixed skin name, e.g. if "santa_cammo" is refreshed
	// with prefix "santa" we need to refresh both "santa_cammo" and "cammo".
	const char *pSkinPrefix = m_Skins.SkinPrefix();
	const int SkinPrefixLength = str_length(pSkinPrefix);
	char aSkinNameWithoutPrefix[MAX_SKIN_LENGTH];
	if(SkinPrefixLength > 0 &&
		str_comp_num(pSkinName, pSkinPrefix, SkinPrefixLength) == 0 &&
		pSkinName[SkinPrefixLength] == '_' &&
		pSkinName[SkinPrefixLength + 1] != '\0')
	{
		str_copy(aSkinNameWithoutPrefix, &pSkinName[SkinPrefixLength + 1]);
	}
	else
	{
		aSkinNameWithoutPrefix[0] = '\0';
	}
	const auto &&NameMatches = [&](const char *pCheckName) {
		if(str_comp(pCheckName, pSkinName) == 0)
		{
			return true;
		}
		if(aSkinNameWithoutPrefix[0] != '\0' &&
			str_comp(pCheckName, aSkinNameWithoutPrefix) == 0)
		{
			return true;
		}
		return false;
	};

	// Tees without their own skin wear the default one, which is fetched late,
	// so its arrival concerns them too.
	const bool IsDefault = str_comp(pSkinName, "default") == 0;

	for(std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo : m_vpManagedTeeRenderInfos)
	{
		if(!(pManagedTeeRenderInfo->SkinDescriptor().m_Flags & CSkinDescriptor::FLAG_SIX) ||
			!(IsDefault || NameMatches(pManagedTeeRenderInfo->SkinDescriptor().m_aSkinName)))
		{
			continue;
		}
		RefreshSkin(pManagedTeeRenderInfo);
	}
}

std::shared_ptr<CManagedTeeRenderInfo> CGameClient::CreateManagedTeeRenderInfo(const CTeeRenderInfo &TeeRenderInfo, const CSkinDescriptor &SkinDescriptor)
{
	std::shared_ptr<CManagedTeeRenderInfo> pManagedTeeRenderInfo = std::make_shared<CManagedTeeRenderInfo>(TeeRenderInfo, SkinDescriptor);
	RefreshSkin(pManagedTeeRenderInfo);
	m_vpManagedTeeRenderInfos.emplace_back(pManagedTeeRenderInfo);
	return pManagedTeeRenderInfo;
}

std::shared_ptr<CManagedTeeRenderInfo> CGameClient::CreateManagedTeeRenderInfo(const CClientData &Client)
{
	return CreateManagedTeeRenderInfo(Client.m_RenderInfo, Client.ToSkinDescriptor(InputState()));
}

void CGameClient::UpdateManagedTeeRenderInfos()
{
	while(!m_vpManagedTeeRenderInfos.empty())
	{
		auto UnusedInfo = std::find_if(m_vpManagedTeeRenderInfos.begin(), m_vpManagedTeeRenderInfos.end(), [&](const auto &pItem) {
			return pItem.use_count() <= 1;
		});
		if(UnusedInfo == m_vpManagedTeeRenderInfos.end())
		{
			break;
		}
		m_vpManagedTeeRenderInfos.erase(UnusedInfo);
	}
}

void CGameClient::CollectManagedTeeRenderInfos(const std::function<void(const char *pSkinName)> &ActiveSkinAcceptor)
{
	for(const std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo : m_vpManagedTeeRenderInfos)
	{
		if(pManagedTeeRenderInfo->m_SkinDescriptor.m_Flags & CSkinDescriptor::FLAG_SIX)
		{
			ActiveSkinAcceptor(pManagedTeeRenderInfo->m_SkinDescriptor.m_aSkinName);
		}
	}
}

void CGameClient::ConchainRefreshSkins(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pThis->m_InitComplete)
	{
		pThis->RefreshSkins(CSkinDescriptor::FLAG_SIX);
	}
}

void CGameClient::ConchainRefreshEventSkins(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pThis->m_InitComplete)
	{
		pThis->m_Skins.RefreshEventSkins();
		pThis->RefreshSkins(CSkinDescriptor::FLAG_SIX);
	}
}

void CGameClient::ConTuneParam(IConsole::IResult *pResult, void *pUserData)
{
	if(pResult->NumArguments() == 2)
		static_cast<CGameClient *>(pUserData)->MapContext().SetTuning(0, pResult->GetString(0), pResult->GetFloat(1));
}

void CGameClient::ConTuneZone(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CGameClient *>(pUserData)->MapContext().SetTuning(pResult->GetInteger(0), pResult->GetString(1), pResult->GetFloat(2));
}

void CGameClient::ConMapbug(IConsole::IResult *pResult, void *pUserData)
{
	static_cast<CGameClient *>(pUserData)->MapContext().EnableMapBug(pResult->GetString(0));
}

void CGameClient::DummyResetInput()
{
	if(!DummyConnected())
		return;

	CGameSessionContext &Session = SessionContext(NetworkSessionId());
	CGameState &State = Session.SeatState(g_Config.m_ClDummy ^ 1);
	CNetObj_PlayerInput &Input = State.Input().m_InputData;
	int Fire = Input.m_Fire;
	if((Fire & 1) != 0)
		Fire++;

	State.Input().ReleaseGameplay();
	Input.m_Hook = 0;
	Input.m_Fire = Fire;
}

bool CGameClient::CanDisplayWarning() const
{
	return m_pFrontend != nullptr && m_pFrontend->CanDisplayWarning();
}

CNetObjHandler *CGameClient::GetNetObjHandler()
{
	return &m_NetObjHandler;
}

protocol7::CNetObjHandler *CGameClient::GetNetObjHandler7()
{
	return &m_NetObjHandler7;
}

void CGameClient::SnapCollectEntities(CSessionId SessionId)
{
	const int NumSnapItems = Sessions()->SnapNumItems(SessionId, ISessions::SNAP_CURRENT);

	std::vector<CSnapEntities> vItemData;
	std::vector<CSnapEntities> vItemEx;

	for(int Index = 0; Index < NumSnapItems; Index++)
	{
		const ISessions::CSnapItem Item = Sessions()->SnapGetItem(SessionId, ISessions::SNAP_CURRENT, Index);
		if(Item.m_Type == NETOBJTYPE_ENTITYEX)
			vItemEx.push_back({Item, nullptr});
		else if(Item.m_Type == NETOBJTYPE_PICKUP || Item.m_Type == NETOBJTYPE_DDNETPICKUP || Item.m_Type == NETOBJTYPE_LASER || Item.m_Type == NETOBJTYPE_DDNETLASER || Item.m_Type == NETOBJTYPE_PROJECTILE || Item.m_Type == NETOBJTYPE_DDRACEPROJECTILE || Item.m_Type == NETOBJTYPE_DDNETPROJECTILE)
			vItemData.push_back({Item, nullptr});
	}

	// sort by id
	class CEntComparer
	{
	public:
		bool operator()(const CSnapEntities &Lhs, const CSnapEntities &Rhs) const
		{
			return Lhs.m_Item.m_Id < Rhs.m_Item.m_Id;
		}
	};

	std::sort(vItemData.begin(), vItemData.end(), CEntComparer());
	std::sort(vItemEx.begin(), vItemEx.end(), CEntComparer());

	// merge extended items with items they belong to
	m_vSnapEntities.clear();

	size_t IndexEx = 0;
	for(const CSnapEntities &Ent : vItemData)
	{
		while(IndexEx < vItemEx.size() && vItemEx[IndexEx].m_Item.m_Id < Ent.m_Item.m_Id)
			IndexEx++;

		const CNetObj_EntityEx *pDataEx = nullptr;
		if(IndexEx < vItemEx.size() && vItemEx[IndexEx].m_Item.m_Id == Ent.m_Item.m_Id)
			pDataEx = (const CNetObj_EntityEx *)vItemEx[IndexEx].m_Item.m_pData;

		m_vSnapEntities.push_back({Ent.m_Item, pDataEx});
	}
}

void CGameClient::OnSaveCodeNetMessage(CGameSessionContext &Session, const CGameState &GameState, const CNetMsg_Sv_SaveCode *pMsg)
{
	char aBuf[512];
	auto AddLine = [&](const char *pText) {
		m_Chat.AddLine(Session, GameState, SessionMessageTime(Session.Id()), Sessions()->SessionType(Session.Id()) == ESessionSourceType::DEMO, Session.Id() == Sessions()->FocusedSessionId(), -1, TEAM_ALL, pText);
	};
	if(pMsg->m_pError[0] != '\0')
		AddLine(pMsg->m_pError);

	int State = pMsg->m_State;
	if(State == SAVESTATE_PENDING)
	{
		if(pMsg->m_pCode[0] == '\0')
		{
			str_format(aBuf, sizeof(aBuf),
				Localize("Team save in progress. You'll be able to load with '/load %s'"),
				pMsg->m_pGeneratedCode);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf),
				Localize("Team save in progress. You'll be able to load with '/load %s' if save is successful or with '/load %s' if it fails"),
				pMsg->m_pCode,
				pMsg->m_pGeneratedCode);
		}
		AddLine(aBuf);
	}
	else if(State == SAVESTATE_DONE)
	{
		if(pMsg->m_pServerName[0] == '\0')
		{
			str_format(aBuf, sizeof(aBuf),
				"Team successfully saved by %s. Use '/load %s' to continue",
				pMsg->m_pSaveRequester,
				pMsg->m_pCode[0] ? pMsg->m_pCode : pMsg->m_pGeneratedCode);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf),
				"Team successfully saved by %s. Use '/load %s' on %s to continue",
				pMsg->m_pSaveRequester,
				pMsg->m_pCode[0] ? pMsg->m_pCode : pMsg->m_pGeneratedCode,
				pMsg->m_pServerName);
		}
		AddLine(aBuf);
	}
	else if(State == SAVESTATE_FALLBACKFILE)
	{
		if(pMsg->m_pServerName[0] == '\0')
		{
			str_format(aBuf, sizeof(aBuf),
				Localize("Team successfully saved by %s. The database connection failed, using generated save code instead to avoid collisions. Use '/load %s' to continue"),
				pMsg->m_pSaveRequester,
				pMsg->m_pGeneratedCode);
		}
		else
		{
			str_format(aBuf, sizeof(aBuf),
				Localize("Team successfully saved by %s. The database connection failed, using generated save code instead to avoid collisions. Use '/load %s' on %s to continue"),
				pMsg->m_pSaveRequester,
				pMsg->m_pGeneratedCode,
				pMsg->m_pServerName);
		}
		AddLine(aBuf);
	}
	else if(State == SAVESTATE_ERROR)
	{
		AddLine(Localize("Save failed!"));
	}

	if(State != SAVESTATE_PENDING && State != SAVESTATE_ERROR && Sessions()->SessionType(Session.Id()) != ESessionSourceType::DEMO)
	{
		StoreSave(Session, pMsg->m_pTeamMembers, pMsg->m_pCode[0] ? pMsg->m_pCode : pMsg->m_pGeneratedCode);
	}
}

void CGameClient::StoreSave(const CGameSessionContext &Session, const char *pTeamMembers, const char *pGeneratedCode) const
{
	static constexpr const char *SAVES_HEADER[] = {
		"Time",
		"Players",
		"Map",
		"Code",
	};

	char aTimestamp[20];
	str_timestamp_format(aTimestamp, sizeof(aTimestamp), TimestampFormat::SPACE);

	const bool SavesFileExists = Storage()->FileExists(SAVES_FILE, IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(SAVES_FILE, IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
	{
		log_error("saves", "Failed to open the saves file '%s'", SAVES_FILE);
		return;
	}

	const char *apColumns[std::size(SAVES_HEADER)] = {
		aTimestamp,
		pTeamMembers,
		Session.MapName(),
		pGeneratedCode,
	};

	if(!SavesFileExists)
	{
		CsvWrite(File, std::size(SAVES_HEADER), SAVES_HEADER);
	}
	CsvWrite(File, std::size(SAVES_HEADER), apColumns);
	io_close(File);
}

void CGameClient::StopMapSounds()
{
	m_SessionPresentations.StopMapSounds();
}
