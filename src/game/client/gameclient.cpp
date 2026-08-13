/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "gameclient.h"

#include "components/background.h"
#include "components/binds.h"
#include "components/broadcast.h"
#include "components/camera.h"
#include "components/chat.h"
#include "components/console.h"
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
#include "components/menu_background.h"
#include "components/menus.h"
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
#include "lineinput.h"
#include "prediction/entities/character.h"
#include "prediction/entities/projectile.h"
#include "race.h"
#include "render.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client/checksum.h>
#include <engine/client/enums.h>
#include <engine/demo.h>
#include <engine/discord.h>
#include <engine/editor.h>
#include <engine/engine.h>
#include <engine/favorites.h>
#include <engine/friends.h>
#include <engine/graphics.h>
#include <engine/map.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/csv.h>
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

using namespace std::chrono_literals;

namespace
{
	int LegacyProfileIndex(int Conn)
	{
		dbg_assert(Conn == IClient::CONN_MAIN || Conn == IClient::CONN_DUMMY, "legacy profile stream must be main or dummy");
		return Conn == IClient::CONN_DUMMY;
	}

	int64_t CurrentPresentationTime()
	{
#if defined(CONF_VIDEORECORDER)
		if(IVideo::Current())
			return IVideo::Current()->Time();
#endif
		return time_get();
	}

	bool UsePredictedEnvelopeTime(const CGameTickInfo &Time, const CGameView &View)
	{
		return !Time.m_IsDemoPlayback && g_Config.m_ClPredict && (!View.IsSpectating() || View.SpectatorId() == SPEC_FREEVIEW);
	}

	class CScreenRenderOutput final : public CRenderOutput
	{
		IGraphics &m_Graphics;
		ColorRGBA m_ClearColor;
		bool m_Cleared = false;
		bool m_CustomViewport = false;

	public:
		CScreenRenderOutput(IGraphics &Graphics, ColorRGBA ClearColor) :
			m_Graphics(Graphics),
			m_ClearColor(ClearColor)
		{
		}

		bool IsVideoOutput() const override
		{
#if defined(CONF_VIDEORECORDER)
			return IVideo::Current() != nullptr;
#else
			return false;
#endif
		}

		void BeginView(const CViewport &Viewport, vec2 CameraPosition, float Zoom) override
		{
			if(!m_Cleared)
			{
				m_Graphics.Clear(m_ClearColor.r, m_ClearColor.g, m_ClearColor.b);
				m_Cleared = true;
			}
			m_CustomViewport = Viewport.m_Width > 0 && Viewport.m_Height > 0;
			if(m_CustomViewport)
				m_Graphics.UpdateViewport(Viewport.m_X, Viewport.m_Y, Viewport.m_Width, Viewport.m_Height, false);
		}

		void DrawCharacter(int ClientId, vec2 Position, bool Local) override {}
		void DrawSpectatorCharacter(int ClientId, vec2 Position, bool OtherTeam) override {}

		void EndView() override
		{
			if(m_CustomViewport)
				m_Graphics.UpdateViewport(0, 0, m_Graphics.ScreenWidth(), m_Graphics.ScreenHeight(), false);
			m_CustomViewport = false;
		}
	};
}

const char *CGameClient::Version() const { return GAME_VERSION; }
const char *CGameClient::NetVersion() const { return GAME_NETVERSION; }
const char *CGameClient::NetVersion7() const { return GAME_NETVERSION7; }
int CGameClient::DDNetVersion() const { return DDNET_VERSION_NUMBER; }
const char *CGameClient::DDNetVersionStr() const { return m_aDDNetVersionStr; }
int CGameClient::ClientVersion7() const { return CLIENT_VERSION7; }
const char *CGameClient::GetItemName(int Type) const { return m_NetObjHandler.GetObjName(Type); }

CGameClient::CGameClient()
{
}

CGameSessionContext &CGameClient::SessionContext()
{
	CGameSessionContext *pContext = m_SessionContexts.Find(Client()->FocusedSessionId());
	dbg_assert(pContext != nullptr, "missing focused game session context");
	return *pContext;
}

const CGameSessionContext &CGameClient::SessionContext() const
{
	const CGameSessionContext *pContext = m_SessionContexts.Find(Client()->FocusedSessionId());
	dbg_assert(pContext != nullptr, "missing focused game session context");
	return *pContext;
}

CSessionPresentation &CGameClient::SessionPresentation(CSessionId SessionId)
{
	CSessionPresentation *pPresentation = m_SessionPresentations.Find(SessionId);
	dbg_assert(pPresentation != nullptr, "missing session presentation");
	return *pPresentation;
}

const CSessionPresentation &CGameClient::SessionPresentation(CSessionId SessionId) const
{
	const CSessionPresentation *pPresentation = m_SessionPresentations.Find(SessionId);
	dbg_assert(pPresentation != nullptr, "missing session presentation");
	return *pPresentation;
}

void CGameClient::ResetInfoMessages(CSessionId SessionId)
{
	CGameSessionContext *pContext = m_SessionContexts.Find(SessionId);
	dbg_assert(pContext != nullptr, "missing info message session context");
	pContext->InfoMessages().Reset();
	m_InfoMessages.ResetPresentation(SessionId);
}

void CGameClient::ResetChat(CSessionId SessionId)
{
	m_Chat.ResetSession(SessionId);
}

void CGameClient::AddChatLine(CSessionId SessionId, int Conn, int ClientId, int Team, const char *pText)
{
	CGameSessionContext *pSession = m_SessionContexts.Find(SessionId);
	CGameState *pState = pSession != nullptr ? pSession->GameStates().FindByStream(CStreamId(Conn + 1)) : nullptr;
	if(pState != nullptr)
		m_Chat.AddLine(*pSession, *pState, SessionMessageTime(SessionId), SessionId == Client()->DemoSessionId(), SessionId == Client()->FocusedSessionId(), ClientId, Team, pText);
}

int64_t CGameClient::SessionMessageTime(CSessionId SessionId) const
{
	return SessionId == Client()->DemoSessionId() ? CurrentPresentationTime() : time_get();
}

CGameState &CGameClient::GameState(int Conn)
{
	CGameState *pState = SessionContext().GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing game state for connection");
	return *pState;
}

const CGameState &CGameClient::GameState(int Conn) const
{
	const CGameState *pState = SessionContext().GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing game state for connection");
	return *pState;
}

CGameView &CGameClient::LegacyGameView()
{
	CGameView *pView = m_GameViews.Find(m_LegacyGameViewId);
	dbg_assert(pView != nullptr, "missing legacy game view");
	CGameSessionContext &Session = SessionContext();
	const int Conn = Session.Id() == Client()->DemoSessionId() ? IClient::CONN_MAIN : ActiveConnection();
	CGameState *pState = Session.GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing focused game state");
	pView->SetTarget(Session.Id(), pState->Id());
	return *pView;
}

void CGameClient::OnConsoleInit()
{
	m_pEngine = Kernel()->RequestInterface<IEngine>();
	m_pClient = Kernel()->RequestInterface<IClient>();
	CGameSessionContext *pNetworkContext = m_SessionContexts.Create(m_pClient->NetworkSessionId(), "", EGameProtocol::SIX, {CStreamId(IClient::CONN_MAIN + 1), CStreamId(IClient::CONN_DUMMY + 1)});
	CGameSessionContext *pDemoContext = m_SessionContexts.Create(m_pClient->DemoSessionId(), "", EGameProtocol::SIX, {CStreamId(IClient::CONN_MAIN + 1)});
	dbg_assert(pNetworkContext != nullptr && pDemoContext != nullptr, "failed to create game session contexts");
	m_LegacyGameViewId = m_GameViews.Create(pNetworkContext->Id(), pNetworkContext->GameStates().States().front()->Id());
	dbg_assert(m_LegacyGameViewId.IsValid(), "failed to create legacy game view");
	const CGameState *pDummyState = pNetworkContext->GameStates().FindByStream(CStreamId(IClient::CONN_DUMMY + 1));
	dbg_assert(pDummyState != nullptr, "missing dummy game state");
	m_SecondaryGameViewId = m_GameViews.Create(pNetworkContext->Id(), pDummyState->Id());
	dbg_assert(m_SecondaryGameViewId.IsValid(), "failed to create secondary game view");
	const CGameState *pMainState = pNetworkContext->GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
	dbg_assert(pMainState != nullptr, "missing main game state");
	m_TertiaryGameViewId = m_GameViews.Create(pNetworkContext->Id(), pMainState->Id());
	dbg_assert(m_TertiaryGameViewId.IsValid(), "failed to create tertiary game view");
	CGameView *pLegacyView = m_GameViews.Find(m_LegacyGameViewId);
	dbg_assert(pLegacyView != nullptr, "missing legacy game view");
	m_Camera.BindState(pLegacyView->Camera());
	m_pTextRender = Kernel()->RequestInterface<ITextRender>();
	m_pSound = Kernel()->RequestInterface<ISound>();
	m_pConfigManager = Kernel()->RequestInterface<IConfigManager>();
	m_pConfig = m_pConfigManager->Values();
	m_pInput = Kernel()->RequestInterface<IInput>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pStorage = Kernel()->RequestInterface<IStorage>();
	m_pDemoPlayer = Kernel()->RequestInterface<IDemoPlayer>();
	m_pServerBrowser = Kernel()->RequestInterface<IServerBrowser>();
	m_pEditor = Kernel()->RequestInterface<IEditor>();
	m_pFavorites = Kernel()->RequestInterface<IFavorites>();
	m_pFriends = Kernel()->RequestInterface<IFriends>();
	m_pFoes = Client()->Foes();
	m_pDiscord = Kernel()->RequestInterface<IDiscord>();
#if defined(CONF_AUTOUPDATE)
	m_pUpdater = Kernel()->RequestInterface<IUpdater>();
#endif
	m_pHttp = Kernel()->RequestInterface<IHttp>();
	for(const auto &pContext : m_SessionContexts.Contexts())
		pContext->MapContext().Init();

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
					      &m_Menus,
					      &m_Tooltips,
					      &m_KeyBinder,
					      &m_GameConsole,
					      &m_MenuBackground});

	// build the input stack
	m_vpInput.insert(m_vpInput.end(), {&m_KeyBinder, // this will take over all input when we want to bind a key
						  &m_Binds.m_SpecialBinds,
						  &m_GameConsole,
						  &m_Chat, // chat has higher prio, due to that you can quit it by pressing esc
						  &m_Scoreboard,
						  &m_Motd, // for pressing esc to remove it
						  &m_Spectator,
						  &m_Emoticon,
						  &m_ImportantAlert,
						  &m_Menus,
						  &m_Controls,
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
	for(const auto &pContext : m_SessionContexts.Contexts())
		dbg_assert(m_SessionPresentations.Create(pContext->Id()) != nullptr, "failed to create session presentation");

	m_LocalServer.OnInterfacesInit(this);

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

	Console()->Chain("cl_menu_map", ConchainMenuMap, this);
}

void CGameClient::InitializeLanguage()
{
	// set the language
	g_Localization.LoadIndexfile(Storage(), Console());
	if(g_Config.m_ClShowWelcome)
		g_Localization.SelectDefaultLanguage(Console(), g_Config.m_ClLanguagefile, sizeof(g_Config.m_ClLanguagefile));
	g_Localization.Load(g_Config.m_ClLanguagefile, Storage(), Console());
}

void CGameClient::ForceUpdateConsoleRemoteCompletionSuggestions()
{
	m_GameConsole.ForceUpdateRemoteCompletionSuggestions();
}

void CGameClient::OnInit()
{
	const int64_t OnInitStart = time_get();

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
		m_Menus.RenderLoading(pTitle, pMessage, 0);
	});

	m_pGraphics = Kernel()->RequestInterface<IGraphics>();

	// propagate pointers
	m_UI.Init(Kernel());
	m_UI.SetOnBackButtonPressedCallback([this]() {
		m_BackButtonHandledKeyBind = m_KeyBinder.HasPendingKeyReader();
		if(m_BackButtonHandledKeyBind)
			m_KeyBinder.AbortPendingKey();
	});
	m_UI.SetDispatchInputCallback([this](const IInput::CEvent &Event) {
		if(m_BackButtonHandledKeyBind)
		{
			if(Event.m_Flags & IInput::FLAG_RELEASE)
				m_BackButtonHandledKeyBind = false;
			return;
		}
		OnInput(Event);
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
		Client()->SnapSetStaticsize(i, m_NetObjHandler.GetObjSize(i));
	// HACK: only set static size for items, which were available in the first 0.7 release
	// so new items don't break the snapshot delta
	static const int OLD_NUM_NETOBJTYPES = 23;
	for(int i = 0; i < OLD_NUM_NETOBJTYPES; i++)
		Client()->SnapSetStaticsize7(i, m_NetObjHandler7.GetObjSize(i));

	if(!TextRender()->LoadFonts())
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

	// init all components
	int SkippedComps = 1;
	int CompCounter = 1;
	const int NumComponents = ComponentCount();
	for(int i = NumComponents - 1; i >= 0; --i)
	{
		m_vpAll[i]->OnInit();
		// try to render a frame after each component, also flushes GPU uploads
		if(m_Menus.IsInit())
		{
			str_format(aLoadingMessage, std::size(aLoadingMessage), "%s [%d/%d]", CompCounter == NumComponents ? pLoadingMessageComponentsSpecial : pLoadingMessageComponents, CompCounter, NumComponents);
			m_Menus.RenderLoading(pLoadingDDNetCaption, aLoadingMessage, SkippedComps);
			SkippedComps = 1;
		}
		else
		{
			++SkippedComps;
		}
		++CompCounter;
	}

	m_GameSkinLoaded = false;
	m_ParticlesSkinLoaded = false;
	m_EmoticonsSkinLoaded = false;
	m_HudSkinLoaded = false;

	// setup load amount, load textures
	const char *pLoadingMessageAssets = Localize("Initializing assets");
	for(int i = 0; i < g_pData->m_NumImages; i++)
	{
		if(i == IMAGE_GAME)
			LoadGameSkin(g_Config.m_ClAssetGame);
		else if(i == IMAGE_EMOTICONS)
			LoadEmoticonsSkin(g_Config.m_ClAssetEmoticons);
		else if(i == IMAGE_PARTICLES)
			LoadParticlesSkin(g_Config.m_ClAssetParticles);
		else if(i == IMAGE_HUD)
			LoadHudSkin(g_Config.m_ClAssetHud);
		else if(i == IMAGE_EXTRAS)
			LoadExtrasSkin(g_Config.m_ClAssetExtras);
		else if(g_pData->m_aImages[i].m_pFilename[0] == '\0') // handle special null image without filename
			g_pData->m_aImages[i].m_Id = IGraphics::CTextureHandle();
		else
			g_pData->m_aImages[i].m_Id = Graphics()->LoadTexture(g_pData->m_aImages[i].m_pFilename, IStorage::TYPE_ALL);
		m_Menus.RenderLoading(pLoadingDDNetCaption, pLoadingMessageAssets, 1);
	}

	m_GameWorld.Init(Collision(), TuningList(), MapContext().MapBugs(), GameConfig());
	OnSessionClosed(Client()->FocusedSessionId());

	// Set free binds to DDRace binds if it's active
	m_Binds.SetDDRaceBinds(true);

	// Aggressively try to grab window again since some Windows users report
	// window not being focused after starting client.
	Graphics()->SetWindowGrab(true);

	CChecksumData *pChecksum = Client()->ChecksumData();
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

	m_Menus.FinishLoading();
	log_trace("gameclient", "initialization finished after %.2fms", (time_get() - OnInitStart) * 1000.0f / (float)time_freq());
}

void CGameClient::OnUpdate()
{
	HandleLanguageChanged();

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
	const std::vector<IInput::CTouchFingerState> &vTouchFingerStates = Input()->TouchFingerStates();
	const int TouchConnection = ActiveConnection();
	CGameSessionContext &TouchSession = SessionContext();
	CGameState &TouchState = GameState(TouchConnection);
	CGameView &TouchView = LegacyGameView();
	const CViewport &TouchViewport = TouchView.Viewport();
	const float TouchAspectRatio = TouchViewport.m_Width > 0 && TouchViewport.m_Height > 0 ? TouchViewport.m_Width / (float)TouchViewport.m_Height : Graphics()->ScreenAspect();
	CTouchControllerContext TouchContext{
		TouchSession,
		TouchState,
		TouchView,
		*TouchSession.MapContext().Collision(),
		TouchState.StreamId(),
		Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK,
		Client()->IsDemoPlayback(),
		Client()->DummyAllowed(),
		Client()->DummyConnected(),
		Client()->RconAuthed(),
		TouchAspectRatio,
		time_get_nanoseconds(),
	};
	bool TouchHandled = false;
	for(auto &pComponent : m_vpInput)
	{
		if(pComponent == &m_TouchControls)
		{
			if(m_TouchControls.UpdateController(TouchContext, vTouchFingerStates, !TouchHandled) && !TouchHandled)
			{
				Input()->ClearTouchDeltas();
				TouchHandled = true;
			}
			continue;
		}
		if(TouchHandled)
		{
			// Also update inactive components so they can handle touch fingers being released.
			pComponent->OnTouchState({});
		}
		else if(pComponent->OnTouchState(vTouchFingerStates))
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
		CGameState::CInputState &Input = GameState(ActiveConnection()).Input();
		Input.m_MousePosOnAction = Input.m_MousePos;
		m_Binds.m_MouseOnAction = false;
	}
	for(const auto &pContext : m_SessionContexts.Contexts())
		pContext->Vote().Expire(SessionMessageTime(pContext->Id()), time_freq());

	for(auto &pComponent : m_vpAll)
	{
		pComponent->OnUpdate();
	}

	UpdateNetworkPlayerInfo();
	m_NewTick = false;
	m_NewPredictedTick = false;
	UpdateManagedTeeRenderInfos();
}

void CGameClient::UpdateNetworkPlayerInfo()
{
	if(Client()->FocusedSessionId() == Client()->DemoSessionId())
		return;

	const int MainLocalId = GameState(IClient::CONN_MAIN).LocalClientId();
	const int DummyLocalId = GameState(IClient::CONN_DUMMY).LocalClientId();
	CGameState::CRuntimeState &MainRuntime = GameState(IClient::CONN_MAIN).Runtime();
	CGameState::CRuntimeState &DummyRuntime = GameState(IClient::CONN_DUMMY).Runtime();
	if(MainLocalId < 0 || !Client()->IsOnline() || m_Menus.IsActive() || !m_NewTick)
		return;

	if(MainRuntime.m_CheckInfo == 0)
	{
		if(m_pClient->IsSixup(Client()->NetworkSessionId()))
		{
			if(!GotWantedSkin7(IClient::CONN_MAIN))
				SendSkinChange7(Client()->NetworkSessionId(), IClient::CONN_MAIN);
			else
				MainRuntime.m_CheckInfo = -1;
		}
		else
		{
			if(
				str_comp(m_aClients[MainLocalId].m_aName, Client()->PlayerName()) ||
				str_comp(m_aClients[MainLocalId].m_aClan, g_Config.m_PlayerClan) ||
				m_aClients[MainLocalId].m_Country != g_Config.m_PlayerCountry ||
				str_comp(m_aClients[MainLocalId].m_aSkinName, g_Config.m_ClPlayerSkin) ||
				m_aClients[MainLocalId].m_UseCustomColor != g_Config.m_ClPlayerUseCustomColor ||
				m_aClients[MainLocalId].m_ColorBody != (int)g_Config.m_ClPlayerColorBody ||
				m_aClients[MainLocalId].m_ColorFeet != (int)g_Config.m_ClPlayerColorFeet)
				SendInfo(Client()->NetworkSessionId(), false);
			else
				MainRuntime.m_CheckInfo = -1;
		}
	}

	if(MainRuntime.m_CheckInfo > 0)
		MainRuntime.m_CheckInfo -= std::min(Client()->GameTick(Client()->NetworkSessionId(), IClient::CONN_MAIN) - Client()->PrevGameTick(Client()->NetworkSessionId(), IClient::CONN_MAIN), MainRuntime.m_CheckInfo);

	if(DummyLocalId < 0)
		return;
	if(DummyRuntime.m_CheckInfo == 0)
	{
		if(m_pClient->IsSixup(Client()->NetworkSessionId()))
		{
			if(!GotWantedSkin7(IClient::CONN_DUMMY))
				SendSkinChange7(Client()->NetworkSessionId(), IClient::CONN_DUMMY);
			else
				DummyRuntime.m_CheckInfo = -1;
		}
		else
		{
			if(
				str_comp(m_aClients[DummyLocalId].m_aName, Client()->DummyName()) ||
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
		DummyRuntime.m_CheckInfo -= std::min(Client()->GameTick(Client()->NetworkSessionId(), IClient::CONN_DUMMY) - Client()->PrevGameTick(Client()->NetworkSessionId(), IClient::CONN_DUMMY), DummyRuntime.m_CheckInfo);
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

void CGameClient::OnConnectionFocusChanged(CSessionId SessionId, int PreviousConn, int Conn)
{
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing focus-change game session context");
	CGameState *pPreviousState = pSession->GameStates().FindByStream(CStreamId(PreviousConn + 1));
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pPreviousState != nullptr && pState != nullptr, "missing focus-change game state");
	(void)LegacyGameView();
	m_Camera.UpdateCamera();
	for(CClientData &Client : m_aClients)
		Client.UpdateSkinInfo(*pState);
	if(g_Config.m_ClDummyResetOnSwitch)
	{
		const CStreamId FocusedStream = pState->StreamId();
		for(const auto &pSessionState : pSession->GameStates().States())
		{
			const bool IsFocused = pSessionState->StreamId() == FocusedStream;
			if(IsFocused != (g_Config.m_ClDummyResetOnSwitch == 2))
				continue;
			m_Controls.ResetInput(pSessionState->StreamId());
		}
	}
	m_PreviousFocusedStream = pPreviousState->StreamId();
	UpdateInputRoutes(SessionId);
}

void CGameClient::UpdateInputRoutes(CSessionId SessionId)
{
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing input game session context");
	const bool AcceptControls = SessionId == Client()->FocusedSessionId();
	const CStreamId FocusedStream(ActiveConnection() + 1);
	CGameStateManager &GameStates = pSession->GameStates();
	CStreamInputRouter &Router = pSession->InputRouter();
	std::vector<CStreamId> vRemovedStreams;
	for(const CStreamInputRoute &Route : Router.Routes())
	{
		if(!GameStates.FindByStream(Route.m_Target))
			vRemovedStreams.push_back(Route.m_Target);
		if(!GameStates.FindByStream(Route.m_Source))
			vRemovedStreams.push_back(Route.m_Source);
	}
	for(CStreamId StreamId : vRemovedStreams)
		Router.Remove(StreamId);

	for(const auto &pState : GameStates.States())
	{
		const CStreamId Target = pState->StreamId();
		const EStreamInputPolicy Policy = !AcceptControls || Target == FocusedStream ? EStreamInputPolicy::DIRECT : g_Config.m_ClDummyHammer ? EStreamInputPolicy::HAMMER :
														    g_Config.m_ClDummyCopyMoves      ? EStreamInputPolicy::COPY_MOVES :
																		       EStreamInputPolicy::DIRECT;
		const CStreamId Source = Policy == EStreamInputPolicy::DIRECT ? Target : FocusedStream;
		const bool RouteSet = Router.Set(Target, Source, Policy);
		dbg_assert(RouteSet, "failed to set input route");
	}
}

int CGameClient::OnSnapInput(CSessionId SessionId, int *pData, int Conn, bool Force)
{
	const int ActiveConn = Client()->ActiveConnection();
	UpdateInputRoutes(SessionId);
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing input game session context");
	CGameState *pTargetState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pTargetState != nullptr, "missing input game state");
	CGameState &TargetState = *pTargetState;
	const CStreamId Target = TargetState.StreamId();
	CStreamInputRouter &Router = pSession->InputRouter();
	CStreamInputRoute &Route = *Router.Find(Target);
	const EStreamInputPolicy Policy = Route.m_Policy;
	CNetObj_PlayerInput &TargetInput = TargetState.Input().m_InputData;
	if(Policy != EStreamInputPolicy::HAMMER)
		Route.FinishHammering(TargetInput);
	if(Policy == EStreamInputPolicy::DIRECT && Conn == ActiveConn && SessionId == Client()->FocusedSessionId())
	{
		return m_Controls.SnapInput(pData);
	}
	if(TargetState.LocalClientId() < 0)
	{
		return 0;
	}

	if(Policy != EStreamInputPolicy::HAMMER)
	{
		if(!Force && SessionId == Client()->FocusedSessionId() && (!TargetInput.m_Direction && !TargetInput.m_Jump && !TargetInput.m_Hook))
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

void CGameClient::BindLegacyWorld(CGameSessionContext &Session)
{
	CMapContext &MapContext = Session.MapContext();
	m_GameWorld.Clear();
	m_GameWorld.Init(MapContext.Collision(), MapContext.TuningList(), MapContext.MapBugs(), &MapContext.GameConfig());
	m_GameWorld.m_WorldConfig = {};
	m_GameWorld.m_WorldConfig.m_InfiniteAmmo = true;
	m_GameWorld.m_Core.InitSwitchers(MapContext.Collision()->m_HighestSwitchNumber);
	m_GameWorld.m_PredictedEvents.clear();
	m_PredictedWorld.CopyWorld(&m_GameWorld);
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);
}

void CGameClient::OnConnected(CSessionId SessionId)
{
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing connected game session context");
	CMapContext &MapContext = pSession->MapContext();
	const bool Focused = SessionId == Client()->FocusedSessionId();
	const char *pConnectCaption = SessionId == Client()->DemoSessionId() ? Localize("Preparing demo playback") : Localize("Connected");
	const char *pLoadMapContent = Localize("Initializing map logic");
	if(Focused)
		m_Menus.RenderLoading(pConnectCaption, pLoadMapContent, 0);
	MapContext.Layers()->Init(MapContext.Map(), false, true);
	MapContext.Collision()->Init(MapContext.Layers());
	pSession->SetDescriptor(MapContext.Map()->BaseName(), Client()->IsSixup(SessionId) ? EGameProtocol::SIXUP : EGameProtocol::SIX);
	pSession->SetServerCapAnyPlayerFlag(SessionId == Client()->NetworkSessionId() && Client()->ServerCapAnyPlayerFlag());
	MapContext.Load(*Config());
	for(const auto &pGameState : pSession->GameStates().States())
		pGameState->InitPrediction(MapContext);
	SessionPresentation(SessionId).Load(*pSession);

	if(SessionId == Client()->NetworkSessionId())
	{
		if(Focused)
		{
			Client()->SetLoadingStateDetail(IClient::LOADING_STATE_DETAIL_GETTING_READY);
			m_Menus.RenderLoading(pConnectCaption, Localize("Sending initial client info"), 0);
		}
		SendInfo(SessionId, true);
		Client()->Rcon("crashmeplx");
		m_LocalServer.RconAuthIfPossible();
	}

	if(!Focused)
		return;

	BindLegacyWorld(*pSession);
	m_RaceHelper.Init(this);
	m_SessionPresentations.SetAudible(SessionId);

	// render loading before going through all components
	m_Menus.RenderLoading(pConnectCaption, pLoadMapContent, 0);
	for(auto &pComponent : m_vpAll)
	{
		pComponent->OnMapLoad();
		pComponent->OnReset();
	}
}

void CGameClient::OnSessionClosed(CSessionId SessionId)
{
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing closed game session context");
	for(const auto &pGameState : pSession->GameStates().States())
		pGameState->Reset();
	pSession->Broadcast().Reset();
	pSession->MapMetadata().Reset();
	pSession->Vote().Reset();
	ResetInfoMessages(SessionId);
	ResetChat(SessionId);
	pSession->Stats().Reset();
	pSession->InputRouter().Reset();
	m_SessionPresentations.Unload(SessionId);
	pSession->MapContext().Unload();
	pSession->MapContext().Map()->Unload();
	if(SessionId == Client()->NetworkSessionId())
	{
		m_RaceDemo.OnNetworkSessionClosed();
		m_ActiveRecordings.reset();
	}

	if(SessionId != Client()->FocusedSessionId())
		return;

	InvalidateSnapshot(SessionId);

	m_EditorMovementDelay = 5;

	// m_aDDNetVersionStr is initialized once in OnInit

	m_SuppressEvents = false;
	m_NewTick = false;
	m_NewPredictedTick = false;

	m_DemoSpecId = SPEC_FOLLOW;
	m_LocalCharacterPos = vec2(0.0f, 0.0f);

	m_PredictedPrevChar.Reset();
	m_PredictedChar.Reset();

	// m_Snap was cleared in InvalidateSnapshot

	for(auto &Client : m_aClients)
		Client.Reset();

	m_GameWorld.Clear();
	m_GameWorld.m_WorldConfig.m_InfiniteAmmo = true;
	m_PredictedWorld.CopyWorld(&m_GameWorld);
	m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);

	m_vSnapEntities.clear();

	m_PreviousFocusedStream.reset();

	// Map bugs and tunings are reset when the map context is loaded.

	m_LastShowDistanceZoom = 0.0f;
	m_LastZoom = 0.0f;
	m_LastScreenAspect = 0.0f;
	m_LastDeadzone = 0.0f;
	m_LastFollowFactor = 0.0f;
	m_LastDummyConnected = false;

	MultiView().Reset();

	LegacyGameView().SetSpectator(false);
	LegacyGameView().SpectatorCursor().Reset();
	CGameView *pSecondaryView = m_GameViews.Find(m_SecondaryGameViewId);
	dbg_assert(pSecondaryView != nullptr, "missing secondary game view");
	pSecondaryView->SetSpectator(false);
	pSecondaryView->SpectatorCursor().Reset();
	CGameView *pTertiaryView = m_GameViews.Find(m_TertiaryGameViewId);
	dbg_assert(pTertiaryView != nullptr, "missing tertiary game view");
	pTertiaryView->SetSpectator(false);
	pTertiaryView->SpectatorCursor().Reset();

	for(auto &pComponent : m_vpAll)
		pComponent->OnReset();

	Editor()->ResetMentions();
	Editor()->ResetIngameMoved();
}

void CGameClient::OnSessionFocused(CSessionId SessionId)
{
	dbg_assert(SessionId == Client()->FocusedSessionId(), "focused game session mismatch");
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing focused game session context");
	if(SessionId == Client()->DemoSessionId())
	{
		CGameSessionContext *pNetworkSession = FindSessionContext(Client()->NetworkSessionId());
		dbg_assert(pNetworkSession != nullptr, "missing Network game session context");
		for(const auto &pState : pNetworkSession->GameStates().States())
			pState->Input().ReleaseGameplay();
	}
	InvalidateSnapshot(SessionId);
	LegacyGameView();
	m_SessionPresentations.SetAudible(SessionId);
	if(!pSession->MapContext().Map()->IsLoaded())
		return;
	BindLegacyWorld(*pSession);
	m_RaceHelper.Init(this);
	for(auto &pComponent : m_vpAll)
		pComponent->OnMapLoad();
}

void CGameClient::UpdatePositions(const CGameState &State)
{
	const CSessionId SessionId = SessionContext().Id();
	const int Conn = ActiveConnection();
	CGameView::CMultiViewState &MultiViewState = MultiView();
	// local character position
	const int LocalClientId = State.LocalClientId();
	if(in_range(LocalClientId, MAX_CLIENTS - 1) && State.RenderedClient(LocalClientId).m_Active)
		m_LocalCharacterPos = State.RenderedClient(LocalClientId).m_Position;

	// spectator position
	if(m_Snap.m_SpecInfo.m_Active)
	{
		if(MultiViewState.m_Active)
		{
			HandleMultiView(State);
		}
		else if(Client()->IsDemoPlayback() && m_DemoSpecId != SPEC_FOLLOW && m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
		{
			m_Snap.m_SpecInfo.m_Position = mix(
				vec2(m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Prev.m_X, m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Prev.m_Y),
				vec2(m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Cur.m_X, m_Snap.m_aCharacters[m_Snap.m_SpecInfo.m_SpectatorId].m_Cur.m_Y),
				Client()->IntraGameTick(SessionId, Conn));
			m_Snap.m_SpecInfo.m_UsePosition = true;
		}
		else if(m_Snap.m_pSpectatorInfo && ((Client()->IsDemoPlayback() && m_DemoSpecId == SPEC_FOLLOW) || (!Client()->IsDemoPlayback() && m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)))
		{
			if(m_Snap.m_pPrevSpectatorInfo && m_Snap.m_pPrevSpectatorInfo->m_SpectatorId == m_Snap.m_pSpectatorInfo->m_SpectatorId)
				m_Snap.m_SpecInfo.m_Position = mix(vec2(m_Snap.m_pPrevSpectatorInfo->m_X, m_Snap.m_pPrevSpectatorInfo->m_Y),
					vec2(m_Snap.m_pSpectatorInfo->m_X, m_Snap.m_pSpectatorInfo->m_Y), Client()->IntraGameTick(SessionId, Conn));
			else
				m_Snap.m_SpecInfo.m_Position = vec2(m_Snap.m_pSpectatorInfo->m_X, m_Snap.m_pSpectatorInfo->m_Y);
			m_Snap.m_SpecInfo.m_UsePosition = true;
		}
	}

	if(!MultiViewState.m_Active && MultiViewState.m_IsInit)
		ResetMultiView();
}

void CGameClient::OnRender()
{
	CGameSessionContext &ActiveSession = SessionContext();
	const bool FocusedDemo = ActiveSession.Id() == Client()->DemoSessionId();
	const int ActiveConn = FocusedDemo ? IClient::CONN_MAIN : ActiveConnection();
	CGameState *pActiveState = ActiveSession.GameStates().FindByStream(CStreamId(ActiveConn + 1));
	dbg_assert(pActiveState != nullptr, "missing active game state");
	CGameState &ActiveState = *pActiveState;
	CGameView &View = LegacyGameView();
	CGameView::CMultiViewState &MultiViewState = MultiView();
	CGameView *pSecondaryView = m_GameViews.Find(m_SecondaryGameViewId);
	CGameView *pTertiaryView = m_GameViews.Find(m_TertiaryGameViewId);
	dbg_assert(pSecondaryView != nullptr && pTertiaryView != nullptr, "missing auxiliary game views");

	struct CRenderEntry
	{
		CGameSessionContext *m_pSession;
		CGameState *m_pState;
		CGameView *m_pView;
		int m_Conn;
		bool m_Audible;
		CGameTickInfo m_Time;
		EPresentationPlayback m_Playback = EPresentationPlayback::PLAYING;
	};
	std::vector<CRenderEntry> vEntries;
	vEntries.reserve(3);
	auto AddEntry = [&](CGameSessionContext &Session, CGameState &State, CGameView &RenderView, int Conn, bool Audible) {
		RenderView.SetTarget(Session.Id(), State.Id());
		vEntries.push_back({&Session, &State, &RenderView, Conn, Audible});
	};
	auto AddNetworkEntries = [&](CGameSessionContext &NetworkSession, CGameView &MainView, CGameView *pDummyView) {
		CGameState *pMainState = NetworkSession.GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
		dbg_assert(pMainState != nullptr, "missing Network main state");
		AddEntry(NetworkSession, *pMainState, MainView, IClient::CONN_MAIN, &MainView == &View);
		if(pDummyView != nullptr && Client()->DummyConnected())
		{
			CGameState *pDummyState = NetworkSession.GameStates().FindByStream(CStreamId(IClient::CONN_DUMMY + 1));
			dbg_assert(pDummyState != nullptr, "missing Network dummy state");
			AddEntry(NetworkSession, *pDummyState, *pDummyView, IClient::CONN_DUMMY, pDummyView == &View);
		}
	};

	const bool MultiGameScreen = g_Config.m_ClDummySplitScreen != 0;
	if(MultiGameScreen && FocusedDemo && Client()->SessionState(Client()->NetworkSessionId()) == ESessionState::READY)
	{
		CGameSessionContext *pNetworkSession = FindSessionContext(Client()->NetworkSessionId());
		dbg_assert(pNetworkSession != nullptr, "missing Network session context");
		AddNetworkEntries(*pNetworkSession, *pSecondaryView, pTertiaryView);
		AddEntry(ActiveSession, ActiveState, View, ActiveConn, true);
	}
	else if(MultiGameScreen && !FocusedDemo)
	{
		if(ActiveConn == IClient::CONN_MAIN)
			AddNetworkEntries(ActiveSession, View, pSecondaryView);
		else
			AddNetworkEntries(ActiveSession, *pSecondaryView, &View);
		if(Client()->SessionState(Client()->DemoSessionId()) == ESessionState::READY)
		{
			CGameSessionContext *pDemoSession = FindSessionContext(Client()->DemoSessionId());
			dbg_assert(pDemoSession != nullptr, "missing Demo session context");
			CGameState *pDemoState = pDemoSession->GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
			dbg_assert(pDemoState != nullptr, "missing Demo game state");
			AddEntry(*pDemoSession, *pDemoState, *pTertiaryView, IClient::CONN_MAIN, false);
		}
	}
	else
	{
		AddEntry(ActiveSession, ActiveState, View, ActiveConn, true);
	}

	const int64_t PresentationTime = CurrentPresentationTime();
	const int64_t Now = time_get();
	const int64_t PresentationTimeFrequency = time_freq();
	auto FreezeTime = [&](const CRenderEntry &Entry) {
		const CSessionId SessionId = Entry.m_pSession->Id();
		const bool DemoPlayback = SessionId == Client()->DemoSessionId();
		const bool WorldPaused = Entry.m_pState->HasGameInfo() && (Entry.m_pState->GameInfo().m_GameStateFlags & (GAMESTATEFLAG_GAMEOVER | GAMESTATEFLAG_PAUSED)) != 0;
		const bool DemoPaused = DemoPlayback && DemoPlayer()->BaseInfo()->m_Paused;
		const float AnimationPlaybackSpeed = WorldPaused || DemoPaused ? 0.0f : DemoPlayback ? DemoPlayer()->BaseInfo()->m_Speed :
												       1.0f;
		CGameTickInfo Time;
		Time.m_PrevGameTick = Client()->PrevGameTick(SessionId, Entry.m_Conn);
		Time.m_GameTick = Client()->GameTick(SessionId, Entry.m_Conn);
		Time.m_PredGameTick = Client()->PredGameTick(SessionId, Entry.m_Conn);
		Time.m_PredictionTick = Client()->GetPredictionTick(SessionId, Entry.m_Conn);
		Time.m_IntraGameTick = Client()->IntraGameTick(SessionId, Entry.m_Conn);
		Time.m_IntraGameTickSincePrev = Client()->IntraGameTickSincePrev(SessionId, Entry.m_Conn);
		Time.m_PredIntraGameTick = Client()->PredIntraGameTick(SessionId, Entry.m_Conn);
		Time.m_GameTickTime = Client()->GameTickTime(SessionId, Entry.m_Conn);
		Time.m_FrameTimeAverage = Client()->FrameTimeAverage();
		Time.m_GameTickSpeed = Client()->GameTickSpeed();
		Time.m_PredictionTime = Client()->GetPredictionTime(SessionId, Entry.m_Conn);
		Time.m_PresentationTime = PresentationTime;
		Time.m_PresentationTimeFrequency = PresentationTimeFrequency;
		Time.m_AnimationPlaybackSpeed = AnimationPlaybackSpeed;
		Time.m_IsGameActive = Client()->SessionState(SessionId) == ESessionState::READY;
		Time.m_IsDemoPlayback = DemoPlayback;
		Time.m_IsDemoPlaybackPaused = DemoPaused;
		Time.m_ConnectionProblems = Client()->ConnectionProblems(SessionId, Entry.m_Conn);
		return Time;
	};
	for(CRenderEntry &Entry : vEntries)
	{
		Entry.m_Time = FreezeTime(Entry);
		Entry.m_Playback = Entry.m_Time.m_AnimationPlaybackSpeed > 0.0f ? EPresentationPlayback::PLAYING : EPresentationPlayback::PAUSED;
		UpdateRenderedClients(*Entry.m_pSession, *Entry.m_pState, Entry.m_Conn, Now, Entry.m_Time, Entry.m_Playback);
	}
	const auto ActiveEntryIt = std::find_if(vEntries.begin(), vEntries.end(), [&](const CRenderEntry &Entry) { return Entry.m_pView == &View; });
	dbg_assert(ActiveEntryIt != vEntries.end(), "missing active render entry");
	const CGameTickInfo &GameTickInfo = ActiveEntryIt->m_Time;
	const ColorRGBA ClearColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClOverlayEntities ? g_Config.m_ClBackgroundEntitiesColor : g_Config.m_ClBackgroundColor));

	// check if multi view got activated
	if(!MultiViewState.m_IsInit && MultiViewState.m_Active)
	{
		int TeamId = 0;
		if(m_Snap.m_SpecInfo.m_SpectatorId >= 0)
			TeamId = ActiveState.Teams().Team(m_Snap.m_SpecInfo.m_SpectatorId);

		if(TeamId > MAX_CLIENTS || TeamId < 0)
			TeamId = 0;

		if(!InitMultiView(ActiveState, TeamId))
		{
			ResetMultiView();
		}
	}

	// update the local character and spectate position
	UpdatePositions(ActiveState);

	// display warnings
	if(m_Menus.CanDisplayWarning())
	{
		std::optional<SWarning> Warning = Graphics()->CurrentWarning();
		if(!Warning.has_value())
		{
			Warning = Client()->CurrentWarning();
		}
		if(Warning.has_value())
		{
			const SWarning &TheWarning = Warning.value();
			m_Menus.PopupWarning(TheWarning.m_aWarningTitle[0] == '\0' ? Localize("Warning") : TheWarning.m_aWarningTitle, TheWarning.m_aWarningMsg, Localize("Ok"), TheWarning.m_AutoHide ? 10s : 0s);
		}
	}

	// Update camera data before controls so the focused input target can compensate using the current camera data.
	m_Camera.UpdateCamera();
	m_Controls.Update();
	m_Camera.UpdatePosition();

	UpdateSpectatorCursor(ActiveState, GameTickInfo);
	if(vEntries.size() > 1)
	{
		for(size_t i = 0; i < vEntries.size(); ++i)
		{
			CRenderEntry &Entry = vEntries[i];
			const int Left = Graphics()->ScreenWidth() * static_cast<int>(i) / static_cast<int>(vEntries.size());
			const int Right = Graphics()->ScreenWidth() * static_cast<int>(i + 1) / static_cast<int>(vEntries.size());
			Entry.m_pView->SetViewport({Left, 0, Right - Left, Graphics()->ScreenHeight()});
			if(Entry.m_pView == &View)
				continue;
			const int LocalId = Entry.m_pState->LocalClientId();
			const CGameState::CClientSnapshot *pLocalClient = in_range(LocalId, MAX_CLIENTS - 1) ? &Entry.m_pState->Client(LocalId) : nullptr;
			const bool LocalPaused = pLocalClient != nullptr && pLocalClient->m_HasDDNetPlayer && (pLocalClient->m_DDNetPlayer.m_Flags & (EXPLAYERFLAG_PAUSED | EXPLAYERFLAG_SPEC)) != 0;
			const bool HasSpectatorInfo = Entry.m_pState->HasSpectatorInfo();
			const int SpectatorId = HasSpectatorInfo ? Entry.m_pState->SpectatorInfo().m_SpectatorId : LocalPaused ? LocalId :
																 SPEC_FREEVIEW;
			Entry.m_pView->SetSpectator(HasSpectatorInfo || LocalPaused, SpectatorId);
			if(HasSpectatorInfo && SpectatorId == SPEC_FREEVIEW)
				Entry.m_pView->SetCameraPosition(vec2(Entry.m_pState->SpectatorInfo().m_X, Entry.m_pState->SpectatorInfo().m_Y));
			else if(Entry.m_pView->IsSpectating() && in_range(SpectatorId, MAX_CLIENTS - 1) && Entry.m_pState->RenderedClient(SpectatorId).m_Active)
				Entry.m_pView->SetCameraPosition(Entry.m_pState->RenderedClient(SpectatorId).m_Position);
			else if(in_range(LocalId, MAX_CLIENTS - 1) && Entry.m_pState->RenderedClient(LocalId).m_Active)
				Entry.m_pView->SetCameraPosition(Entry.m_pState->RenderedClient(LocalId).m_Position);
		}
	}
	else
	{
		View.SetViewport({});
	}

	auto VisibleWorldRectFor = [this](const CGameView &RenderView) {
		const CViewport &Viewport = RenderView.Viewport();
		const float Aspect = Viewport.m_Width > 0 && Viewport.m_Height > 0 ? Viewport.m_Width / static_cast<float>(Viewport.m_Height) : Graphics()->ScreenAspect();
		const CScreenRect ScreenRect = Graphics()->MapScreenToWorld(
			RenderView.CameraPosition().x, RenderView.CameraPosition().y, 100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Aspect, RenderView.Zoom());
		return CVisibleWorldRect(ScreenRect.m_TopLeft, ScreenRect.m_BottomRight);
	};
	const CVisibleWorldRect VisibleWorldRect = VisibleWorldRectFor(View);
	CScreenRenderOutput ScreenOutput(*Graphics(), ClearColor);
	std::vector<CGameRenderRequest> vRenderRequests;
	vRenderRequests.reserve(vEntries.size());
	for(const CRenderEntry &Entry : vEntries)
	{
		vRenderRequests.emplace_back(
			*Entry.m_pSession,
			*Entry.m_pState,
			*Entry.m_pView,
			Entry.m_Time,
			VisibleWorldRectFor(*Entry.m_pView),
			Entry.m_Playback,
			Entry.m_Audible ? EPresentationAudio::AUDIBLE : EPresentationAudio::MUTED,
			ScreenOutput);
	}
	const CGameRenderRequest *pAudibleRequest = FindAudibleRenderRequest(vRenderRequests);
	m_Sounds.Update(pAudibleRequest != nullptr ? std::optional(pAudibleRequest->m_View.CameraPosition()) : std::nullopt);
	if(pAudibleRequest != nullptr)
	{
		m_SessionPresentations.SetAudible(pAudibleRequest->m_Session.Id());
		SessionPresentation(pAudibleRequest->m_Session.Id()).UpdateMapSounds(pAudibleRequest->m_State, pAudibleRequest->m_Time, pAudibleRequest->m_View.CameraPosition(), UsePredictedEnvelopeTime(pAudibleRequest->m_Time, pAudibleRequest->m_View));
	}
	else
	{
		m_SessionPresentations.SetAudible(CSessionId());
	}
	CGameRenderScheduler Scheduler;
	Scheduler.Run(
		vRenderRequests,
		[this](const CPresentationContext &Context) {
			SessionPresentation(Context.m_Session.Id()).UpdateClients(Context);
			m_Effects.Update(Context);
			m_Particles.Update(Context);
			m_DamageInd.Update(Context);
			m_Items.UpdatePresentation(Context);
			m_Ghost.UpdatePresentation(Context);
			m_Players.UpdatePresentation(Context);
		},
		[this](const CRenderContext &Context, CRenderOutput &Output) {
			const bool UsePredictedTime = UsePredictedEnvelopeTime(Context.m_Time, Context.m_View);
			CSessionPresentation &Presentation = SessionPresentation(Context.m_Session.Id());
			if(Context.m_Time.m_IsGameActive)
				Presentation.PrepareRender(Context, UsePredictedTime);
			if(!m_Background.UsesCurrentMap())
				m_Background.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
			const std::array<CComponent *, 13> apWorldComponents = {
				&Presentation.MapLayersBackground(),
				&m_Particles.m_RenderTrail,
				&m_Particles.m_RenderTrailExtra,
				&m_Items,
				&m_Ghost,
				&m_Players,
				&Presentation.MapLayersForeground(),
				&m_Particles.m_RenderExplosions,
				&m_NamePlates,
				&m_Particles.m_RenderExtra,
				&m_Particles.m_RenderGeneral,
				&m_FreezeBars,
				&m_DamageInd,
			};
			Output.BeginView(Context.m_View.Viewport(), Context.m_View.CameraPosition(), Context.m_View.Zoom());
			if(g_Config.m_ClOverlayEntities == 100)
			{
				if(m_Background.UsesCurrentMap())
					Presentation.MapLayersBackgroundForce().OnRender(Context);
				else
					m_Background.OnRender(Context);
			}
			for(CComponent *pComponent : apWorldComponents)
				pComponent->OnRender(Context);
			Output.EndView();
		});

	const CRenderContext CompatibilityContext(ActiveSession, ActiveState, View, GameTickInfo, VisibleWorldRect, ScreenOutput.PresentationCacheKey(), ScreenOutput.IsVideoOutput());

	const std::array<CComponent *, 2> apRequestOverlaysBeforeChat = {
		&m_InfoMessages,
		&m_Hud,
	};
	const std::array<CComponent *, 2> apCompatibilityOverlaysBeforeChat = {
		&m_Spectator,
		&m_Emoticon,
	};
	const std::array<CComponent *, 2> apRequestOverlaysAfterChat = {
		&m_Broadcast,
		&m_DebugHud,
	};
	const std::array<CComponent *, 2> apCompatibilityOverlaysAfterChat = {
		&m_ImportantAlert,
		&m_TouchControls,
	};
	const std::array<CComponent *, 2> apRequestOverlaysAfterScoreboard = {
		&m_Statboard,
		&m_Motd,
	};
	const std::array<CComponent *, 3> apApplicationOverlays = {
		&m_Menus,
		&m_Tooltips,
		&m_GameConsole,
	};
	auto RenderRequestComponents = [&](std::span<CComponent *const> vpComponents) {
		Scheduler.Run(
			vRenderRequests,
			[](const CPresentationContext &) {},
			[vpComponents](const CRenderContext &Context, CRenderOutput &Output) {
				Output.BeginView(Context.m_View.Viewport(), Context.m_View.CameraPosition(), Context.m_View.Zoom());
				for(CComponent *pComponent : vpComponents)
					pComponent->OnRender(Context);
				Output.EndView();
			});
	};
	RenderRequestComponents(apRequestOverlaysBeforeChat);
	ScreenOutput.BeginView(View.Viewport(), View.CameraPosition(), View.Zoom());
	for(CComponent *pComponent : apCompatibilityOverlaysBeforeChat)
		pComponent->OnRender(CompatibilityContext);
	m_Chat.RenderApplicationOverlay(CompatibilityContext);
	ScreenOutput.EndView();
	Scheduler.Run(
		vRenderRequests,
		[](const CPresentationContext &) {},
		[this](const CRenderContext &Context, CRenderOutput &Output) {
			Output.BeginView(Context.m_View.Viewport(), Context.m_View.CameraPosition(), Context.m_View.Zoom());
			m_Chat.OnRender(Context);
			Output.EndView();
		});
	RenderRequestComponents(apRequestOverlaysAfterChat);
	ScreenOutput.BeginView(View.Viewport(), View.CameraPosition(), View.Zoom());
	for(CComponent *pComponent : apCompatibilityOverlaysAfterChat)
		pComponent->OnRender(CompatibilityContext);
	m_TouchControls.RenderApplicationOverlay();
	ScreenOutput.EndView();
	m_Scoreboard.BeginRenderFrame();
	Scheduler.Run(
		vRenderRequests,
		[](const CPresentationContext &) {},
		[this](const CRenderContext &Context, CRenderOutput &Output) {
			Output.BeginView(Context.m_View.Viewport(), Context.m_View.CameraPosition(), Context.m_View.Zoom());
			m_Scoreboard.OnRender(Context);
			Output.EndView();
		});
	ScreenOutput.BeginView(View.Viewport(), View.CameraPosition(), View.Zoom());
	m_Scoreboard.RenderApplicationOverlay(CompatibilityContext);
	ScreenOutput.EndView();
	RenderRequestComponents(apRequestOverlaysAfterScoreboard);
	for(CComponent *pComponent : apApplicationOverlays)
		pComponent->OnRender();

	CLineInput::RenderCandidates();
}

void CGameClient::OnRenderPrepare()
{
	CGameSessionContext &Session = SessionContext();
	const int Conn = ActiveConnection();
	CGameState *pState = Session.GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing controller game state");
	CGameView &View = LegacyGameView();
	CGameTickInfo Time;
	Time.m_GameTickSpeed = Client()->GameTickSpeed();
	Time.m_PresentationTime = CurrentPresentationTime();
	Time.m_PresentationTimeFrequency = time_freq();
	Time.m_IsGameActive = Client()->SessionState(Session.Id()) == ESessionState::READY;
	Time.m_IsDemoPlayback = Session.Id() == Client()->DemoSessionId();
	const CRenderContext Context(Session, *pState, View, Time, CVisibleWorldRect(vec2(), vec2()));
	m_ControllerLocalTime = Client()->LocalTime();
	m_Spectator.UpdateController(View, Context, m_ControllerLocalTime);
	m_Emoticon.UpdateController(View, Context);
	m_Chat.UpdateController(Context);
	m_Statboard.UpdateController();
	m_Scoreboard.PrepareApplicationOverlay(Context);
}

void CGameClient::OnRenderFinalize()
{
	CGameView &View = LegacyGameView();
	m_Spectator.CommitController(View, View.SessionId(), View.StateId(), m_ControllerLocalTime);
	Input()->Clear();
}

void CGameClient::OnDummyDisconnect()
{
	CGameSessionContext *pSession = FindSessionContext(Client()->NetworkSessionId());
	dbg_assert(pSession != nullptr, "missing Network game session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(IClient::CONN_DUMMY + 1));
	dbg_assert(pState != nullptr, "missing Network dummy game state");
	pState->Reset();
}

int CGameClient::LastRaceTick() const
{
	return GameState(ActiveConnection()).Runtime().m_LastRaceTick;
}

int CGameClient::CurrentRaceTime() const
{
	const int RaceTick = LastRaceTick();
	if(RaceTick < 0)
	{
		return 0;
	}
	return (Client()->GameTick(SessionContext().Id(), ActiveConnection()) - RaceTick) / Client()->GameTickSpeed();
}

int CGameClient::FlagDropTick(int Team) const
{
	return GameState(ActiveConnection()).Runtime().m_aFlagDropTick[Team];
}

bool CGameClient::ReceivedDDNetPlayer() const
{
	return GameState(ActiveConnection()).Runtime().m_ReceivedDDNetPlayer;
}

bool CGameClient::ReceivedDDNetPlayerFinishTimes() const
{
	return GameState(ActiveConnection()).Runtime().m_ReceivedDDNetPlayerFinishTimes;
}

bool CGameClient::ReceivedDDNetPlayerFinishTimesMillis() const
{
	return GameState(ActiveConnection()).Runtime().m_ReceivedDDNetPlayerFinishTimesMillis;
}

bool CGameClient::IsTeamPlay() const
{
	return m_Snap.m_pGameInfoObj &&
	       (m_Snap.m_pGameInfoObj->m_GameFlags & GAMEFLAG_TEAMS) != 0;
}

bool CGameClient::IsWorldPaused() const
{
	return m_Snap.m_pGameInfoObj &&
	       (m_Snap.m_pGameInfoObj->m_GameStateFlags & (GAMESTATEFLAG_GAMEOVER | GAMESTATEFLAG_PAUSED)) != 0;
}

bool CGameClient::IsDemoPlaybackPaused() const
{
	return Client()->IsDemoPlayback() &&
	       DemoPlayer()->BaseInfo()->m_Paused;
}

float CGameClient::GetAnimationPlaybackSpeed() const
{
	if(IsWorldPaused() || IsDemoPlaybackPaused())
	{
		return 0.0f;
	}
	if(Client()->IsDemoPlayback())
	{
		return DemoPlayer()->BaseInfo()->m_Speed;
	}
	return 1.0f;
}

int CGameClient::AntiPingPlayers() const
{
	if(g_Config.m_ClAntiPing &&
		g_Config.m_ClAntiPingPlayers &&
		!m_Snap.m_SpecInfo.m_Active &&
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
	       !m_Snap.m_SpecInfo.m_Active &&
	       !Client()->IsDemoPlayback();
}

bool CGameClient::AntiPingWeapons() const
{
	return g_Config.m_ClAntiPing &&
	       g_Config.m_ClAntiPingWeapons &&
	       !m_Snap.m_SpecInfo.m_Active &&
	       !Client()->IsDemoPlayback();
}

bool CGameClient::AntiPingGunfire() const
{
	return AntiPingGrenade() &&
	       AntiPingWeapons() &&
	       g_Config.m_ClAntiPingGunfire;
}

bool CGameClient::Predict() const
{
	return g_Config.m_ClPredict &&
	       !IsWorldPaused() &&
	       !Client()->IsDemoPlayback() &&
	       !m_Snap.m_SpecInfo.m_Active &&
	       m_Snap.m_pLocalCharacter;
}

bool CGameClient::PredictDummy(const CGameState &OtherState) const
{
	if(!g_Config.m_ClPredictDummy || !Client()->DummyConnected() || m_Snap.m_LocalClientId < 0)
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
		const int HighestClientId = Format == EClientIdFormat::INDENT_AUTO ? m_Snap.m_HighestClientId : 64;
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

void CGameClient::OnMessage(CSessionId SessionId, int MsgId, CUnpacker *pUnpacker, int Conn)
{
	const bool AdditionalStream = SessionId == Client()->NetworkSessionId() && Conn != IClient::CONN_MAIN;
	CGameSessionContext *pMessageSession = m_SessionContexts.Find(SessionId);
	dbg_assert(pMessageSession != nullptr, "missing message session context");
	CGameState *pMessageState = pMessageSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pMessageState != nullptr, "missing message game state");
	const bool Focused = SessionId == Client()->FocusedSessionId();
	const bool SuppressEvents = m_SuppressEvents && SessionId == Client()->DemoSessionId();
	const int64_t MessageTime = SessionMessageTime(SessionId);

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
			if(i == 30 && Client()->IsSixup(SessionId)) // laser_damage was removed in 0.7
			{
				continue;
			}

			const int Value = pUnpacker->GetInt();

			// check for unpacking errors
			if(pUnpacker->Error())
				break;

			pParams[i] = Value;
		}

		pMessageState->Runtime().m_ServerMode = CGameState::SERVERMODE_PURE;

		pMessageState->Runtime().m_ReceivedTuning = true;
		// apply new tuning
		pMessageState->ApplyTuning(NewTuning);
		return;
	}

	void *pRawMsg = TranslateGameMsg(SessionId, &MsgId, pUnpacker, Conn);

	if(!pRawMsg)
	{
		// the 0.7 version of this error message is printed on translation
		// in sixup/translate_game.cpp
		if(!Client()->IsSixup(SessionId))
		{
			log_debug("client", "dropped weird message '%s' (%d), failed on '%s'",
				m_NetObjHandler.GetMsgName(MsgId), MsgId, m_NetObjHandler.FailedMsgOn());
		}
		return;
	}
	if(MsgId == NETMSGTYPE_SV_CHANGEINFOCOOLDOWN)
	{
		CNetMsg_Sv_ChangeInfoCooldown *pMsg = (CNetMsg_Sv_ChangeInfoCooldown *)pRawMsg;
		pMessageState->Runtime().m_NextChangeInfo = pMsg->m_WaitUntil;
		return;
	}

	if(MsgId == NETMSGTYPE_SV_DDRACETIME || MsgId == NETMSGTYPE_SV_DDRACETIMELEGACY)
	{
		const CNetMsg_Sv_DDRaceTime *pMsg = static_cast<const CNetMsg_Sv_DDRaceTime *>(pRawMsg);
		pMessageState->RaceMessages().ApplyDDRaceTime(pMsg->m_Time, pMsg->m_Check, pMsg->m_Finish != 0, Client()->GameTick(SessionId, Conn));
	}
	else if(MsgId == NETMSGTYPE_SV_RECORD || MsgId == NETMSGTYPE_SV_RECORDLEGACY)
	{
		const CNetMsg_Sv_Record *pMsg = static_cast<const CNetMsg_Sv_Record *>(pRawMsg);
		if(MsgId == NETMSGTYPE_SV_RECORDLEGACY && pMessageState->CoreGameInfo().m_DDRaceRecordMessage)
		{
			pMessageState->RaceMessages().ApplyLegacyRecord(pMsg->m_ServerTimeBest, pMsg->m_PlayerTimeBest, Client()->GameTick(SessionId, Conn));
		}
		else if(MsgId == NETMSGTYPE_SV_RECORD || pMessageState->CoreGameInfo().m_RaceRecordMessage)
		{
			// Ignore m_ServerTimeBest, it is handled below for the focused connection.
			pMessageState->Runtime().m_PlayerRecord = pMsg->m_PlayerTimeBest / 100.0f;
		}
	}

	if(MsgId == NETMSGTYPE_SV_TEAMSSTATE || MsgId == NETMSGTYPE_SV_TEAMSSTATELEGACY)
	{
		unsigned int i;
		for(i = 0; i < MAX_CLIENTS; i++)
		{
			const int Team = pUnpacker->GetInt();
			pMessageState->SetTeam(i, !pUnpacker->Error() && Team >= TEAM_FLOCK && Team < NUM_DDRACE_TEAMS ? Team : TEAM_FLOCK);
			if(pUnpacker->Error() || Team < TEAM_FLOCK || Team >= NUM_DDRACE_TEAMS)
				break;
		}
		pMessageState->SetDDrace16(i <= 16);
		if(AdditionalStream)
			return;

		if(Focused)
		{
			m_Ghost.m_AllowRestart = true;
			m_RaceDemo.m_AllowRestart = true;
		}
		return;
	}
	// Keep prediction ordering current for every stream before inactive messages are filtered below.
	if(MsgId == NETMSGTYPE_SV_KILLMSG)
	{
		const CNetMsg_Sv_KillMsg *pMsg = static_cast<const CNetMsg_Sv_KillMsg *>(pRawMsg);
		if(!(pMessageState->CoreGameInfo().m_PredictFNG && pMsg->m_Weapon == WEAPON_LASER))
			pMessageState->Runtime().m_CharOrder.GiveWeak(pMsg->m_Victim);
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSGTEAM)
	{
		const CNetMsg_Sv_KillMsgTeam *pMsg = static_cast<const CNetMsg_Sv_KillMsgTeam *>(pRawMsg);
		CGameState &State = *pMessageState;
		std::vector<std::pair<int, int>> vStrongWeakSorted;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(State.Teams().Team(ClientId) != pMsg->m_Team)
				continue;
			if(const CCharacter *pCharacter = State.GameWorld().GetCharacterById(ClientId))
				vStrongWeakSorted.emplace_back(ClientId, pMsg->m_First == ClientId ? MAX_CLIENTS : pCharacter->GetStrongWeakId());
		}
		std::stable_sort(vStrongWeakSorted.begin(), vStrongWeakSorted.end(), [](const auto &Left, const auto &Right) { return Left.second > Right.second; });
		for(const auto &Id : vStrongWeakSorted)
			State.Runtime().m_CharOrder.GiveWeak(Id.first);
	}

	if(MsgId == NETMSGTYPE_SV_BROADCAST)
	{
		if(!AdditionalStream)
		{
			const CNetMsg_Sv_Broadcast *pMsg = static_cast<const CNetMsg_Sv_Broadcast *>(pRawMsg);
			m_Broadcast.DoBroadcast(pMessageSession->Broadcast(), pMsg->m_pMessage, Client()->GameTick(SessionId, Conn), Client()->GameTickSpeed());
		}
		return;
	}
	if(MsgId == NETMSGTYPE_SV_MOTD)
	{
		if(!AdditionalStream && SessionId != Client()->DemoSessionId())
		{
			const CNetMsg_Sv_Motd *pMsg = static_cast<const CNetMsg_Sv_Motd *>(pRawMsg);
			m_Motd.DoMotd(*pMessageSession, pMsg->m_pMessage, Focused);
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
		if(!AdditionalStream && SessionId != Client()->DemoSessionId())
			m_Voting.HandleMessage(pMessageSession->Vote(), MessageTime, time_freq(), Focused && Client()->RconAuthed(), MsgId, pRawMsg);
		return;
	}
	if(MsgId == NETMSGTYPE_SV_EMOTICON)
	{
		const CNetMsg_Sv_Emoticon *pMsg = static_cast<const CNetMsg_Sv_Emoticon *>(pRawMsg);
		pMessageState->ApplyEmoticon(pMsg->m_ClientId, pMsg->m_Emoticon, Client()->GameTick(SessionId, Conn), Client()->IntraGameTickSincePrev(SessionId, Conn));
	}

	if(AdditionalStream)
	{
		const CGameState *pMainState = pMessageSession->GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
		const CGameState *pDummyState = pMessageSession->GameStates().FindByStream(CStreamId(IClient::CONN_DUMMY + 1));
		if(pMainState == nullptr || pDummyState == nullptr)
			return;
		const int MainLocalId = pMainState->LocalClientId();
		const int DummyLocalId = pDummyState->LocalClientId();
		if(MsgId == NETMSGTYPE_SV_CHAT && MainLocalId >= 0 && DummyLocalId >= 0)
		{
			CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

			const CTeamsCore &Teams = pMessageState->Teams();
			const int MainTeam = pMainState->Client(MainLocalId).m_HasPlayerInfo ? pMainState->Client(MainLocalId).m_PlayerInfo.m_Team : TEAM_SPECTATORS;
			const int DummyTeam = pDummyState->Client(DummyLocalId).m_HasPlayerInfo ? pDummyState->Client(DummyLocalId).m_PlayerInfo.m_Team : TEAM_SPECTATORS;
			if((pMsg->m_Team == 1 && (MainTeam != DummyTeam || Teams.Team(MainLocalId) != Teams.Team(DummyLocalId))) || pMsg->m_Team > 1)
			{
				m_Chat.HandleMessage(*pMessageSession, *pMessageState, MessageTime, SuppressEvents, SessionId == Client()->DemoSessionId(), Focused, MsgId, pRawMsg);
			}
		}
		return; // no need of all that stuff for the dummy
	}
	m_Chat.HandleMessage(*pMessageSession, *pMessageState, MessageTime, SuppressEvents, SessionId == Client()->DemoSessionId(), Focused, MsgId, pRawMsg);
	m_InfoMessages.HandleMessage(pMessageSession->InfoMessages(), *pMessageSession, *pMessageState, Client()->GameTick(SessionId, Conn), SuppressEvents, MsgId, pRawMsg);
	m_Statboard.HandleMessage(pMessageSession->Stats(), *pMessageState, SuppressEvents, MsgId, pRawMsg);
	if(MsgId == NETMSGTYPE_SV_RECORD || MsgId == NETMSGTYPE_SV_RECORDLEGACY)
	{
		const CNetMsg_Sv_Record *pMsg = static_cast<const CNetMsg_Sv_Record *>(pRawMsg);
		pMessageSession->MapMetadata().ApplyRecordBestTime(pMsg->m_ServerTimeBest);
	}
	else if(MsgId == NETMSGTYPE_SV_MAPINFO)
	{
		const CNetMsg_Sv_MapInfo *pMsg = static_cast<const CNetMsg_Sv_MapInfo *>(pRawMsg);
		pMessageSession->MapMetadata().SetDescription(pMsg->m_pDescription);
	}
	else if(MsgId == NETMSGTYPE_SV_READYTOENTER)
	{
		Client()->EnterGame(SessionId, Conn);
		return;
	}
	else if(MsgId == NETMSGTYPE_SV_MAPSOUNDGLOBAL)
	{
		if(!SuppressEvents && g_Config.m_SndGame)
		{
			const CNetMsg_Sv_MapSoundGlobal *pMsg = static_cast<const CNetMsg_Sv_MapSoundGlobal *>(pRawMsg);
			SessionPresentation(SessionId).MapSounds().Play(CSounds::CHN_GLOBAL, pMsg->m_SoundId);
		}
		return;
	}
	else if(MsgId == NETMSGTYPE_SV_SAVECODE)
	{
		const CNetMsg_Sv_SaveCode *pMsg = static_cast<const CNetMsg_Sv_SaveCode *>(pRawMsg);
		OnSaveCodeNetMessage(*pMessageSession, *pMessageState, pMsg);
		return;
	}

	if(!Focused)
		return;

	// TODO: this should be done smarter
	for(auto &pComponent : m_vpAll)
		pComponent->OnMessage(MsgId, pRawMsg);

	if(MsgId == NETMSGTYPE_SV_SOUNDGLOBAL)
	{
		if(SuppressEvents)
			return;

		// don't enqueue pseudo-global sounds from demos (created by PlayAndRecord)
		CNetMsg_Sv_SoundGlobal *pMsg = (CNetMsg_Sv_SoundGlobal *)pRawMsg;
		if(pMsg->m_SoundId == SOUND_CTF_DROP || pMsg->m_SoundId == SOUND_CTF_RETURN ||
			pMsg->m_SoundId == SOUND_CTF_CAPTURE || pMsg->m_SoundId == SOUND_CTF_GRAB_EN ||
			pMsg->m_SoundId == SOUND_CTF_GRAB_PL)
		{
			if(g_Config.m_SndGame)
				m_Sounds.Enqueue(CSounds::CHN_GLOBAL, pMsg->m_SoundId);
		}
		else
		{
			if(g_Config.m_SndGame)
				m_Sounds.Play(CSounds::CHN_GLOBAL, pMsg->m_SoundId, 1.0f);
		}
	}
	else if(MsgId == NETMSGTYPE_SV_KILLMSG)
	{
		CNetMsg_Sv_KillMsg *pMsg = (CNetMsg_Sv_KillMsg *)pRawMsg;
		const CGameState &State = GameState(ActiveConnection());
		const CGameState::CClientSnapshot &Victim = State.Client(pMsg->m_Victim);
		const bool VictimIsSpec = Victim.m_HasDDNetPlayer && (Victim.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0;
		CGameView::CMultiViewState &MultiViewState = MultiView();
		// reset character prediction
		if(!(State.CoreGameInfo().m_PredictFNG && pMsg->m_Weapon == WEAPON_LASER))
		{
			if(CCharacter *pChar = m_GameWorld.GetCharacterById(pMsg->m_Victim))
				pChar->ResetPrediction();
			m_GameWorld.ReleaseHooked(pMsg->m_Victim);
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
				if(m_Snap.m_SpecInfo.m_SpectatorId == pMsg->m_Victim)
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
				if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
				{
					pChar->ResetPrediction();
				}
				m_GameWorld.ReleaseHooked(i);
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
	for(auto &pComponent : m_vpAll)
		pComponent->OnShutdown();
	m_SessionPresentations.UnloadAll();

	m_LocalServer.KillServer();
}

void CGameClient::OnEnterGame(CSessionId SessionId)
{
	(void)SessionId;
}

void CGameClient::OnGameOver()
{
	if(!Client()->IsDemoPlayback() && g_Config.m_ClEditor == 0)
		Client()->AutoScreenshot_Start();
}

void CGameClient::OnStartGame()
{
	if(!Client()->IsDemoPlayback() && !g_Config.m_ClAutoDemoOnConnect)
		Client()->DemoRecorder_HandleAutoStart();
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
	m_GameConsole.RequireUsername(UsernameReq);
}

void CGameClient::OnRconLine(const char *pLine)
{
	m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, pLine);
}

void CGameClient::ProcessEvents(CSessionId SessionId, int Conn)
{
	if(m_SuppressEvents)
		return;

	const int SnapType = IClient::SNAP_CURRENT;
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing event session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing event game state");
	CGameState &State = *pState;
	const int Num = Client()->SnapNumItems(SessionId, Conn, SnapType);
	for(int Index = 0; Index < Num; Index++)
	{
		const IClient::CSnapItem Item = Client()->SnapGetItem(SessionId, Conn, SnapType, Index);

		// TODO: We don't have enough info about us, others, to know a correct alpha or volume value.
		const float Alpha = 1.0f;
		const float Volume = 1.0f;

		if(Item.m_Type == NETEVENTTYPE_DAMAGEIND)
		{
			const CNetEvent_DamageInd *pEvent = (const CNetEvent_DamageInd *)Item.m_pData;

			vec2 DamageIndPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, DamageIndPos, -1, Client()->GameTick(SessionId, Conn), pEvent->m_Angle)))
			{
				m_Effects.DamageIndicator(State, vec2(pEvent->m_X, pEvent->m_Y), direction(pEvent->m_Angle / 256.0f), -1, Alpha);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_EXPLOSION)
		{
			const CNetEvent_Explosion *pEvent = (const CNetEvent_Explosion *)Item.m_pData;

			vec2 ExplosionPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, ExplosionPos, -1, Client()->GameTick(SessionId, Conn))))
			{
				m_Effects.Explosion(State, ExplosionPos, Alpha);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_HAMMERHIT)
		{
			const CNetEvent_HammerHit *pEvent = (const CNetEvent_HammerHit *)Item.m_pData;

			vec2 HammerHitPos = vec2(pEvent->m_X, pEvent->m_Y);
			if(!m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, HammerHitPos, -1, Client()->GameTick(SessionId, Conn))))
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
			if(!m_PredictedWorld.CheckPredictedEventHandled(CGameWorld::CPredictedEvent(Item.m_Type, SoundPos, -1, Client()->GameTick(SessionId, Conn), pEvent->m_SoundId)))
			{
				m_Sounds.PlayAt(CSounds::CHN_WORLD, pEvent->m_SoundId, 1.0f, SoundPos);
			}
		}
		else if(Item.m_Type == NETEVENTTYPE_MAPSOUNDWORLD)
		{
			CNetEvent_MapSoundWorld *pEvent = (CNetEvent_MapSoundWorld *)Item.m_pData;
			if(!Config()->m_SndGame)
				continue;

			SessionPresentation(SessionId).MapSounds().PlayAt(CSounds::CHN_WORLD, pEvent->m_SoundId, vec2(pEvent->m_X, pEvent->m_Y));
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
	Info.m_Supports128Teams = false;

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
		Info.m_Supports128Teams = Flags2 & GAMEINFOFLAG2_SUPPORTS_128_TEAMS;
	}

	return Info;
}

void CGameClient::InvalidateSnapshot(CSessionId SessionId)
{
	if(SessionId != Client()->FocusedSessionId())
		return;
	// clear all pointers
	mem_zero(&m_Snap, sizeof(m_Snap));
	m_Snap.m_SpecInfo.m_Zoom = 1.0f;
	m_Snap.m_LocalClientId = -1;
	m_vSnapEntities.clear();
}

void CGameClient::OnNewSnapshot(CSessionId SessionId, int Conn)
{
	CGameInfo GameInfo = GetGameInfo(nullptr, 0, &Client()->ServerInfo(SessionId));
	const int NumItems = Client()->SnapNumItems(SessionId, Conn, IClient::SNAP_CURRENT);
	for(int i = 0; i < NumItems; i++)
	{
		const IClient::CSnapItem Item = Client()->SnapGetItem(SessionId, Conn, IClient::SNAP_CURRENT, i);
		if(Item.m_Type == NETOBJTYPE_GAMEINFOEX)
		{
			GameInfo = GetGameInfo(static_cast<const CNetObj_GameInfoEx *>(Item.m_pData), Item.m_DataSize, &Client()->ServerInfo(SessionId));
			break;
		}
	}
	CGameSessionContext *pSession = m_SessionContexts.Find(SessionId);
	dbg_assert(pSession != nullptr, "missing snapshot session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing snapshot game state");
	CGameState &State = *pState;
	State.SetCoreGameInfo(GameInfo);
	State.ApplySnapshot(*Client(), SessionId, Conn);
	if(Conn == IClient::CONN_MAIN)
		pSession->Stats().UpdateSnapshot(State, Client()->GameTick(SessionId, Conn));
	if(SessionId == Client()->FocusedSessionId() && Conn == ActiveConnection())
		ProcessSnapshot(SessionId, Conn);
}

void CGameClient::ProcessSnapshot(CSessionId SessionId, int Conn)
{
	dbg_assert(SessionId == Client()->FocusedSessionId(), "legacy snapshot must belong to focused session");
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing legacy snapshot session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing legacy snapshot game state");
	CGameSessionContext &Session = *pSession;
	CGameState &ActiveState = *pState;
	CGameState::CRuntimeState &Runtime = ActiveState.Runtime();
	const bool NetworkSource = SessionId == Client()->NetworkSessionId();
	auto &&Evolve = [this, &Session](CNetObj_Character *pCharacter, int Tick) {
		CWorldCore TempWorld;
		CCharacterCore TempCore = CCharacterCore();
		CTeamsCore TempTeams = CTeamsCore();
		TempCore.Init(&TempWorld, Session.MapContext().Collision(), &TempTeams);
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

	InvalidateSnapshot(SessionId);

	m_NewTick = true;

	ProcessEvents(SessionId, Conn);

	if(g_Config.m_DbgStress)
	{
		if(NetworkSource && (Client()->GameTick(SessionId, Conn) % 100) == 0)
		{
			char aMessage[64];
			int MsgLen = rand() % (sizeof(aMessage) - 1);
			for(int i = 0; i < MsgLen; i++)
				aMessage[i] = (char)('a' + (rand() % ('z' - 'a')));
			aMessage[MsgLen] = 0;

			m_Chat.SendChat(rand() & 1, aMessage);
		}
	}

	const CServerInfo &ServerInfo = Client()->ServerInfo(SessionId);

	bool GotSwitchStateTeam = false;
	Runtime.m_SwitchStateTeam = -1;

	// go through all the items in the snapshot and gather the info we want
	{
		m_Snap.m_aTeamSize[TEAM_RED] = m_Snap.m_aTeamSize[TEAM_BLUE] = 0;

		const int Num = Client()->SnapNumItems(SessionId, Conn, IClient::SNAP_CURRENT);
		for(int i = 0; i < Num; i++)
		{
			const IClient::CSnapItem Item = Client()->SnapGetItem(SessionId, Conn, IClient::SNAP_CURRENT, i);

			if(Item.m_Type == NETOBJTYPE_CLIENTINFO)
			{
				const CNetObj_ClientInfo *pInfo = (const CNetObj_ClientInfo *)Item.m_pData;
				int ClientId = Item.m_Id;
				if(ClientId < MAX_CLIENTS)
				{
					CClientData *pClient = &m_aClients[ClientId];

					if(!IntsToStr(pInfo->m_aName, std::size(pInfo->m_aName), pClient->m_aName, std::size(pClient->m_aName)))
					{
						str_copy(pClient->m_aName, "nameless tee");
					}
					IntsToStr(pInfo->m_aClan, std::size(pInfo->m_aClan), pClient->m_aClan, std::size(pClient->m_aClan));
					pClient->m_Country = pInfo->m_Country;
					if(!in_range(pClient->m_Country, CountryCode::MINIMUM, CountryCode::MAXIMUM))
					{
						pClient->m_Country = CountryCode::DEFAULT;
					}

					IntsToStr(pInfo->m_aSkin, std::size(pInfo->m_aSkin), pClient->m_aSkinName, std::size(pClient->m_aSkinName));
					if(!CSkin::IsValidName(pClient->m_aSkinName) ||
						(!ActiveState.CoreGameInfo().m_AllowXSkins && CSkins::IsSpecialSkin(pClient->m_aSkinName)))
					{
						str_copy(pClient->m_aSkinName, "default");
					}

					pClient->m_UseCustomColor = pInfo->m_UseCustomColor;
					pClient->m_ColorBody = pInfo->m_ColorBody;
					pClient->m_ColorFeet = pInfo->m_ColorFeet;
				}
			}
			else if(Item.m_Type == NETOBJTYPE_PLAYERINFO)
			{
				const CNetObj_PlayerInfo *pInfo = (const CNetObj_PlayerInfo *)Item.m_pData;

				if(pInfo->m_ClientId < MAX_CLIENTS && pInfo->m_ClientId == Item.m_Id)
				{
					m_aClients[pInfo->m_ClientId].m_Team = pInfo->m_Team;
					m_aClients[pInfo->m_ClientId].m_Active = true;
					m_Snap.m_apPlayerInfos[pInfo->m_ClientId] = pInfo;
					m_Snap.m_apPrevPlayerInfos[pInfo->m_ClientId] = static_cast<const CNetObj_PlayerInfo *>(Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, Item.m_Type, pInfo->m_ClientId));
					m_Snap.m_NumPlayers++;

					if(pInfo->m_Local)
					{
						m_Snap.m_LocalClientId = pInfo->m_ClientId;
						m_Snap.m_pLocalInfo = pInfo;

						if(pInfo->m_Team == TEAM_SPECTATORS)
						{
							m_Snap.m_SpecInfo.m_Active = true;
						}
					}

					m_Snap.m_HighestClientId = std::max(m_Snap.m_HighestClientId, pInfo->m_ClientId);

					// calculate team-balance
					if(pInfo->m_Team != TEAM_SPECTATORS)
					{
						m_Snap.m_aTeamSize[pInfo->m_Team]++;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_CHARACTER)
			{
				if(Item.m_Id < MAX_CLIENTS)
				{
					const void *pOld = Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, NETOBJTYPE_CHARACTER, Item.m_Id);
					m_Snap.m_aCharacters[Item.m_Id].m_Cur = *((const CNetObj_Character *)Item.m_pData);
					if(pOld)
					{
						m_Snap.m_aCharacters[Item.m_Id].m_Active = true;
						m_Snap.m_aCharacters[Item.m_Id].m_Prev = *((const CNetObj_Character *)pOld);

						// limit evolving to 3 seconds
						bool EvolvePrev = Client()->PrevGameTick(SessionId, Conn) - m_Snap.m_aCharacters[Item.m_Id].m_Prev.m_Tick <= 3 * Client()->GameTickSpeed();
						bool EvolveCur = Client()->GameTick(SessionId, Conn) - m_Snap.m_aCharacters[Item.m_Id].m_Cur.m_Tick <= 3 * Client()->GameTickSpeed();

						// reuse the result from the previous evolve if the snapped character didn't change since the previous snapshot
						if(EvolveCur && m_aClients[Item.m_Id].m_Evolved.m_Tick == Client()->PrevGameTick(SessionId, Conn))
						{
							if(mem_comp(&m_Snap.m_aCharacters[Item.m_Id].m_Prev, &m_aClients[Item.m_Id].m_Snapped, sizeof(CNetObj_Character)) == 0)
								m_Snap.m_aCharacters[Item.m_Id].m_Prev = m_aClients[Item.m_Id].m_Evolved;
							if(mem_comp(&m_Snap.m_aCharacters[Item.m_Id].m_Cur, &m_aClients[Item.m_Id].m_Snapped, sizeof(CNetObj_Character)) == 0)
								m_Snap.m_aCharacters[Item.m_Id].m_Cur = m_aClients[Item.m_Id].m_Evolved;
						}

						if(EvolvePrev && m_Snap.m_aCharacters[Item.m_Id].m_Prev.m_Tick)
							Evolve(&m_Snap.m_aCharacters[Item.m_Id].m_Prev, Client()->PrevGameTick(SessionId, Conn));
						if(EvolveCur && m_Snap.m_aCharacters[Item.m_Id].m_Cur.m_Tick)
							Evolve(&m_Snap.m_aCharacters[Item.m_Id].m_Cur, Client()->GameTick(SessionId, Conn));

						m_aClients[Item.m_Id].m_Snapped = *((const CNetObj_Character *)Item.m_pData);
						m_aClients[Item.m_Id].m_Evolved = m_Snap.m_aCharacters[Item.m_Id].m_Cur;
					}
					else
					{
						m_aClients[Item.m_Id].m_Evolved.m_Tick = -1;
					}
				}
			}
			else if(Item.m_Type == NETOBJTYPE_DDNETCHARACTER)
			{
				const CNetObj_DDNetCharacter *pCharacterData = (const CNetObj_DDNetCharacter *)Item.m_pData;

				if(Item.m_Id < MAX_CLIENTS)
				{
					m_Snap.m_aCharacters[Item.m_Id].m_ExtendedData = *pCharacterData;
					m_Snap.m_aCharacters[Item.m_Id].m_pPrevExtendedData = (const CNetObj_DDNetCharacter *)Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, NETOBJTYPE_DDNETCHARACTER, Item.m_Id);
					m_Snap.m_aCharacters[Item.m_Id].m_HasExtendedData = true;
					m_Snap.m_aCharacters[Item.m_Id].m_HasExtendedDisplayInfo = false;
					if(pCharacterData->m_JumpedTotal != -1)
					{
						m_Snap.m_aCharacters[Item.m_Id].m_HasExtendedDisplayInfo = true;
					}
					m_aClients[Item.m_Id].m_Predicted.ReadDDNet(pCharacterData);
				}
			}
			else if(Item.m_Type == NETOBJTYPE_SPECTATORINFO)
			{
				m_Snap.m_pSpectatorInfo = (const CNetObj_SpectatorInfo *)Item.m_pData;
				m_Snap.m_pPrevSpectatorInfo = (const CNetObj_SpectatorInfo *)Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, NETOBJTYPE_SPECTATORINFO, Item.m_Id);

				// needed for 0.7 survival
				// to auto spec players when dead
				if(Client()->IsSixup(SessionId))
					m_Snap.m_SpecInfo.m_Active = true;
				m_Snap.m_SpecInfo.m_SpectatorId = m_Snap.m_pSpectatorInfo->m_SpectatorId;
			}
			else if(Item.m_Type == NETOBJTYPE_DDNETSPECTATORINFO)
			{
				const CNetObj_DDNetSpectatorInfo *pDDNetSpecInfo = (const CNetObj_DDNetSpectatorInfo *)Item.m_pData;
				m_Snap.m_SpecInfo.m_HasCameraInfo = pDDNetSpecInfo->m_HasCameraInfo;
				m_Snap.m_SpecInfo.m_Zoom = pDDNetSpecInfo->m_Zoom / 1000.0f;
				m_Snap.m_SpecInfo.m_Deadzone = pDDNetSpecInfo->m_Deadzone;
				m_Snap.m_SpecInfo.m_FollowFactor = pDDNetSpecInfo->m_FollowFactor;
			}
			else if(Item.m_Type == NETOBJTYPE_SPECTATORCOUNT)
			{
				m_Snap.m_pSpectatorCount = (const CNetObj_SpectatorCount *)Item.m_pData;
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEINFO)
			{
				m_Snap.m_pGameInfoObj = (const CNetObj_GameInfo *)Item.m_pData;
				const bool CurrentTickGameOver = (m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) != 0;
				const bool CurrentTickGamePaused = (m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED) != 0;
				if(!Runtime.m_GameOver && CurrentTickGameOver)
					OnGameOver();
				else if(Runtime.m_GameOver && !CurrentTickGameOver)
					OnStartGame();
				// Handle case that a new round is started (RoundStartTick changed)
				// New round is usually started after `restart` on server
				if(m_Snap.m_pGameInfoObj->m_RoundStartTick != Runtime.m_LastRoundStartTick && !(CurrentTickGameOver || CurrentTickGamePaused || Runtime.m_GamePaused))
					OnStartRound();
				Runtime.m_LastRoundStartTick = m_Snap.m_pGameInfoObj->m_RoundStartTick;
				Runtime.m_GameOver = CurrentTickGameOver;
				Runtime.m_GamePaused = CurrentTickGamePaused;
			}
			else if(Item.m_Type == NETOBJTYPE_GAMEDATA)
			{
				m_Snap.m_pGameDataObj = static_cast<const CNetObj_GameData *>(Item.m_pData);
				m_Snap.m_pPrevGameDataObj = static_cast<const CNetObj_GameData *>(Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, Item.m_Type, Item.m_Id));
				if(m_Snap.m_pGameDataObj->m_FlagCarrierRed == FLAG_TAKEN)
				{
					if(Runtime.m_aFlagDropTick[TEAM_RED] == 0)
						Runtime.m_aFlagDropTick[TEAM_RED] = Client()->GameTick(SessionId, Conn);
				}
				else
				{
					Runtime.m_aFlagDropTick[TEAM_RED] = 0;
				}
				if(m_Snap.m_pGameDataObj->m_FlagCarrierBlue == FLAG_TAKEN)
				{
					if(Runtime.m_aFlagDropTick[TEAM_BLUE] == 0)
						Runtime.m_aFlagDropTick[TEAM_BLUE] = Client()->GameTick(SessionId, Conn);
				}
				else
				{
					Runtime.m_aFlagDropTick[TEAM_BLUE] = 0;
				}
				Runtime.m_LastFlagCarrierRed = m_Snap.m_pGameDataObj->m_FlagCarrierRed;
				Runtime.m_LastFlagCarrierBlue = m_Snap.m_pGameDataObj->m_FlagCarrierBlue;
			}
			else if(Item.m_Type == NETOBJTYPE_FLAG)
			{
				const CNetObj_Flag *pPrevFlag = static_cast<const CNetObj_Flag *>(Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, Item.m_Type, Item.m_Id));
				if(pPrevFlag == nullptr)
				{
					continue;
				}
				m_Snap.m_apFlags[m_Snap.m_NumFlags] = static_cast<const CNetObj_Flag *>(Item.m_pData);
				m_Snap.m_apPrevFlags[m_Snap.m_NumFlags] = pPrevFlag;
				++m_Snap.m_NumFlags;
			}
			else if(Item.m_Type == NETOBJTYPE_SWITCHSTATE)
			{
				if(Item.m_DataSize < 36)
				{
					continue;
				}
				const CNetObj_SwitchState *pSwitchStateData = (const CNetObj_SwitchState *)Item.m_pData;
				int Team = std::clamp(Item.m_Id, (int)TEAM_FLOCK, NUM_DDRACE_TEAMS - 1);

				int HighestSwitchNumber = std::clamp(std::max(pSwitchStateData->m_HighestSwitchNumber, Session.MapContext().Collision()->m_HighestSwitchNumber), 0, 255);
				if(HighestSwitchNumber != std::max(0, (int)Switchers().size() - 1))
				{
					m_GameWorld.m_Core.InitSwitchers(HighestSwitchNumber);
					Session.MapContext().Collision()->m_HighestSwitchNumber = HighestSwitchNumber;
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
				Session.MapMetadata().ApplyBestTime(pMapBestTimeData->m_MapBestTimeSeconds, pMapBestTimeData->m_MapBestTimeMillis);
			}
		}
	}
	if(m_Snap.m_LocalClientId >= 0)
	{
		const CGameState::CClientSnapshot &LocalClient = ActiveState.Client(m_Snap.m_LocalClientId);
		if(LocalClient.m_HasDDNetPlayer && (LocalClient.m_DDNetPlayer.m_Flags & (EXPLAYERFLAG_PAUSED | EXPLAYERFLAG_SPEC)) != 0)
			m_Snap.m_SpecInfo.m_Active = true;
	}

	for(CClientData &Client : m_aClients)
	{
		Client.UpdateSkinInfo(ActiveState);
	}

	// setup local pointers
	if(m_Snap.m_LocalClientId >= 0)
	{
		ActiveState.SetLocalClientId(m_Snap.m_LocalClientId);

		CSnapState::CCharacterInfo *pChr = &m_Snap.m_aCharacters[m_Snap.m_LocalClientId];
		if(pChr->m_Active)
		{
			if(!m_Snap.m_SpecInfo.m_Active)
			{
				m_Snap.m_pLocalCharacter = &pChr->m_Cur;
				m_Snap.m_pLocalPrevCharacter = &pChr->m_Prev;
				m_LocalCharacterPos = vec2(m_Snap.m_pLocalCharacter->m_X, m_Snap.m_pLocalCharacter->m_Y);
			}
		}
		else if(Client()->SnapFindItem(SessionId, Conn, IClient::SNAP_PREV, NETOBJTYPE_CHARACTER, m_Snap.m_LocalClientId))
		{
			// player died
			ActiveState.Input().m_aAmmoCount.fill(0);
		}
	}
	if(SessionId == Client()->DemoSessionId())
	{
		if(m_Snap.m_LocalClientId == -1 && m_DemoSpecId == SPEC_FOLLOW)
		{
			// TODO: can this be done in the translation layer?
			if(!Client()->IsSixup(SessionId))
				m_DemoSpecId = SPEC_FREEVIEW;
		}
		if(m_DemoSpecId != SPEC_FOLLOW)
		{
			m_Snap.m_SpecInfo.m_Active = true;
			if(m_DemoSpecId > SPEC_FREEVIEW && m_Snap.m_aCharacters[m_DemoSpecId].m_Active)
				m_Snap.m_SpecInfo.m_SpectatorId = m_DemoSpecId;
			else
				m_Snap.m_SpecInfo.m_SpectatorId = SPEC_FREEVIEW;
		}
	}
	LegacyGameView().SetSpectator(m_Snap.m_SpecInfo.m_Active, m_Snap.m_SpecInfo.m_SpectatorId);
	if(SessionId == Client()->DemoSessionId())
		LegacyGameView().SetSpectatorMode(m_DemoSpecId);

	// clear out unneeded client data
	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(!m_Snap.m_apPlayerInfos[i] && m_aClients[i].m_Active)
		{
			m_aClients[i].Reset();
		}
	}

	if(NetworkSource)
	{
		m_pDiscord->UpdatePlayerCount(m_Snap.m_NumPlayers);
	}

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		// update friend state
		m_aClients[i].m_Friend = !(i == m_Snap.m_LocalClientId || !m_Snap.m_apPlayerInfos[i] || !Friends()->IsFriend(m_aClients[i].m_aName, m_aClients[i].m_aClan, true));

		// update foe state
		m_aClients[i].m_Foe = !(i == m_Snap.m_LocalClientId || !m_Snap.m_apPlayerInfos[i] || !Foes()->IsFriend(m_aClients[i].m_aName, m_aClients[i].m_aClan, true));
	}

	// sort player infos by name
	mem_copy(m_Snap.m_apInfoByName, m_Snap.m_apPlayerInfos, sizeof(m_Snap.m_apInfoByName));
	std::stable_sort(m_Snap.m_apInfoByName, m_Snap.m_apInfoByName + MAX_CLIENTS,
		[this](const CNetObj_PlayerInfo *pPlayer1, const CNetObj_PlayerInfo *pPlayer2) -> bool {
			if(!pPlayer2)
				return static_cast<bool>(pPlayer1);
			if(!pPlayer1)
				return false;
			return str_comp_nocase(m_aClients[pPlayer1->m_ClientId].m_aName, m_aClients[pPlayer2->m_ClientId].m_aName) < 0;
		});

	bool TimeScore = ActiveState.CoreGameInfo().m_TimeScore;
	bool Race7 = Client()->IsSixup(SessionId) && m_Snap.m_pGameInfoObj && m_Snap.m_pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE;

	// sort player infos by score
	mem_copy(m_Snap.m_apInfoByScore, m_Snap.m_apInfoByName, sizeof(m_Snap.m_apInfoByScore));
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
	std::stable_sort(m_Snap.m_apInfoByScore, m_Snap.m_apInfoByScore + MAX_CLIENTS, SortByTimeScore);

	// sort player infos by DDRace Team (and score between)
	int Index = 0;
	for(int Team = TEAM_FLOCK; Team < NUM_DDRACE_TEAMS; ++Team)
	{
		for(int i = 0; i < MAX_CLIENTS && Index < MAX_CLIENTS; ++i)
		{
			if(m_Snap.m_apInfoByScore[i] && ActiveState.Teams().Team(m_Snap.m_apInfoByScore[i]->m_ClientId) == Team)
				m_Snap.m_apInfoByDDTeamScore[Index++] = m_Snap.m_apInfoByScore[i];
		}
	}

	// sort player infos by DDRace Team (and name between)
	Index = 0;
	for(int Team = TEAM_FLOCK; Team < NUM_DDRACE_TEAMS; ++Team)
	{
		for(int i = 0; i < MAX_CLIENTS && Index < MAX_CLIENTS; ++i)
		{
			if(m_Snap.m_apInfoByName[i] && ActiveState.Teams().Team(m_Snap.m_apInfoByName[i]->m_ClientId) == Team)
				m_Snap.m_apInfoByDDTeamName[Index++] = m_Snap.m_apInfoByName[i];
		}
	}

	if(ServerInfo.m_aGameType[0] != '0')
	{
		if(str_comp(ServerInfo.m_aGameType, "DM") != 0 && str_comp(ServerInfo.m_aGameType, "TDM") != 0 && str_comp(ServerInfo.m_aGameType, "CTF") != 0)
			Runtime.m_ServerMode = CGameState::SERVERMODE_MOD;
		else if(mem_comp(&CTuningParams::DEFAULT, &Runtime.m_CurrentTuning, 33) == 0)
			Runtime.m_ServerMode = CGameState::SERVERMODE_PURE;
		else
			Runtime.m_ServerMode = CGameState::SERVERMODE_PUREMOD;
	}

	if(NetworkSource)
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
			Client()->SendMsg(Conn, &Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND);
		}

		for(const auto &pSessionState : Session.GameStates().States())
		{
			const int SessionConn = (int)pSessionState->StreamId().Value() - 1;
			if(pSessionState->Runtime().m_DDRaceMsgSent || !m_Snap.m_pLocalInfo)
				continue;
			if(SessionConn == IClient::CONN_DUMMY && !Client()->DummyConnected())
				continue;
			CMsgPacker Msg(NETMSGTYPE_CL_ISDDNETLEGACY, false);
			Msg.AddInt(DDNetVersion());
			Client()->SendMsg(SessionConn, &Msg, MSGFLAG_VITAL);
			pSessionState->Runtime().m_DDRaceMsgSent = true;
		}

		if(m_Snap.m_SpecInfo.m_Active && MultiView().m_Active)
		{
			// dont show other teams while spectating in multi view
			CNetMsg_Cl_ShowOthers Msg;
			Msg.m_Show = SHOW_OTHERS_ONLY_TEAM;
			Client()->SendPackMsg(Conn, &Msg, MSGFLAG_VITAL);

			// update state
			Runtime.m_ShowOthers = SHOW_OTHERS_ONLY_TEAM;
		}
		else if(Runtime.m_ShowOthers == SHOW_OTHERS_NOT_SET || Runtime.m_ShowOthers != g_Config.m_ClShowOthers)
		{
			CNetMsg_Cl_ShowOthers Msg;
			Msg.m_Show = g_Config.m_ClShowOthers;
			Client()->SendPackMsg(Conn, &Msg, MSGFLAG_VITAL);

			// update state
			Runtime.m_ShowOthers = g_Config.m_ClShowOthers;
		}

		CGameState *pMainState = Session.GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
		CGameState *pDummyState = Session.GameStates().FindByStream(CStreamId(IClient::CONN_DUMMY + 1));
		dbg_assert(pMainState != nullptr && pDummyState != nullptr, "missing Network game states");
		CGameState::CRuntimeState &MainRuntime = pMainState->Runtime();
		if(MainRuntime.m_EnableSpectatorCount == -1 || MainRuntime.m_EnableSpectatorCount != g_Config.m_ClShowhudSpectatorCount)
		{
			CNetMsg_Cl_EnableSpectatorCount Msg;
			Msg.m_Enable = g_Config.m_ClShowhudSpectatorCount;
			Client()->SendPackMsg(IClient::CONN_MAIN, &Msg, MSGFLAG_VITAL);
			MainRuntime.m_EnableSpectatorCount = g_Config.m_ClShowhudSpectatorCount;
		}
		CGameState::CRuntimeState &DummyRuntime = pDummyState->Runtime();
		if(Client()->DummyConnected() && (DummyRuntime.m_EnableSpectatorCount == -1 || DummyRuntime.m_EnableSpectatorCount != g_Config.m_ClShowhudSpectatorCount))
		{
			CNetMsg_Cl_EnableSpectatorCount Msg;
			Msg.m_Enable = g_Config.m_ClShowhudSpectatorCount;
			Client()->SendPackMsg(IClient::CONN_DUMMY, &Msg, MSGFLAG_VITAL);
			DummyRuntime.m_EnableSpectatorCount = g_Config.m_ClShowhudSpectatorCount;
		}

		float ShowDistanceZoom = m_Camera.Zoom();
		float Zoom = m_Camera.Zoom();
		if(m_Camera.IsZooming())
		{
			if(m_Camera.ZoomSmoothingTarget() > m_Camera.Zoom()) // Zooming out
				ShowDistanceZoom = m_Camera.ZoomSmoothingTarget();
			else if(m_Camera.ZoomSmoothingTarget() < m_Camera.Zoom() && m_LastShowDistanceZoom > 0) // Zooming in
				ShowDistanceZoom = m_LastShowDistanceZoom;

			Zoom = m_Camera.ZoomSmoothingTarget();
		}

		float Deadzone = m_Camera.Deadzone();
		float FollowFactor = m_Camera.FollowFactor();

		if(m_Snap.m_SpecInfo.m_Active)
		{
			// don't send camera information when spectating
			Zoom = m_LastZoom;
			Deadzone = m_LastDeadzone;
			FollowFactor = m_LastFollowFactor;
		}

		// initialize dummy vital when first connected
		if(Client()->DummyConnected() && !m_LastDummyConnected)
		{
			{
				CNetMsg_Cl_ShowDistance Msg;
				float x, y;
				Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), ShowDistanceZoom, &x, &y);
				Msg.m_X = x;
				Msg.m_Y = y;
				CMsgPacker Packer(&Msg);
				Msg.Pack(&Packer);
				Client()->SendMsg(IClient::CONN_DUMMY, &Packer, MSGFLAG_VITAL);
			}
			{
				CNetMsg_Cl_CameraInfo Msg;
				Msg.m_Zoom = round_truncate(Zoom * 1000.f);
				Msg.m_Deadzone = Deadzone;
				Msg.m_FollowFactor = FollowFactor;
				CMsgPacker Packer(&Msg);
				Msg.Pack(&Packer);
				Client()->SendMsg(IClient::CONN_DUMMY, &Packer, MSGFLAG_VITAL);
			}
		}

		// send show distance
		if(ShowDistanceZoom != m_LastShowDistanceZoom || Graphics()->ScreenAspect() != m_LastScreenAspect)
		{
			CNetMsg_Cl_ShowDistance Msg;
			float x, y;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), ShowDistanceZoom, &x, &y);
			Msg.m_X = x;
			Msg.m_Y = y;
			Client()->ChecksumData()->m_Zoom = ShowDistanceZoom;
			CMsgPacker Packer(&Msg);
			Msg.Pack(&Packer);

			Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL);
			if(Client()->DummyConnected() && m_LastDummyConnected)
				Client()->SendMsg(IClient::CONN_DUMMY, &Packer, MSGFLAG_VITAL);
		}

		// send camera info
		if(Zoom != m_LastZoom || Deadzone != m_LastDeadzone || FollowFactor != m_LastFollowFactor)
		{
			CNetMsg_Cl_CameraInfo Msg;
			Msg.m_Zoom = round_truncate(Zoom * 1000.f);
			Msg.m_Deadzone = Deadzone;
			Msg.m_FollowFactor = FollowFactor;
			CMsgPacker Packer(&Msg);
			Msg.Pack(&Packer);

			Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL);
			if(Client()->DummyConnected() && m_LastDummyConnected)
				Client()->SendMsg(IClient::CONN_DUMMY, &Packer, MSGFLAG_VITAL);
		}

		m_LastShowDistanceZoom = ShowDistanceZoom;
		m_LastZoom = Zoom;
		m_LastScreenAspect = Graphics()->ScreenAspect();
		m_LastDeadzone = Deadzone;
		m_LastFollowFactor = FollowFactor;
		m_LastDummyConnected = Client()->DummyConnected();
	}

	for(auto &pComponent : m_vpAll)
		pComponent->OnNewSnapshot();

	// notify editor when local character moved
	UpdateEditorIngameMoved();

	// detect air jump for other players
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_Snap.m_aCharacters[i].m_Active && (m_Snap.m_aCharacters[i].m_Cur.m_Jumped & 2) && !(m_Snap.m_aCharacters[i].m_Prev.m_Jumped & 2))
		{
			const CGameState *pOtherState = NetworkSource ? Session.GameStates().FindByStream(CStreamId((Conn == IClient::CONN_MAIN ? IClient::CONN_DUMMY : IClient::CONN_MAIN) + 1)) : nullptr;
			bool IsDummy = pOtherState != nullptr && Client()->DummyConnected() && i == pOtherState->LocalClientId();
			bool IsLocalPlayer = i == m_Snap.m_LocalClientId;

			if(!Predict() || (!IsLocalPlayer && !AntiPingPlayers()) || (!IsLocalPlayer && !IsDummy))
			{
				vec2 Pos = mix(vec2(m_Snap.m_aCharacters[i].m_Prev.m_X, m_Snap.m_aCharacters[i].m_Prev.m_Y),
					vec2(m_Snap.m_aCharacters[i].m_Cur.m_X, m_Snap.m_aCharacters[i].m_Cur.m_Y),
					Client()->IntraGameTick(SessionId, Conn));
				const float Volume = 1.0f; // TODO snd_game_volume_others

				const bool Grounded = Session.MapContext().Collision()->IsOnGround(vec2(m_Snap.m_aCharacters[i].m_Prev.m_X, m_Snap.m_aCharacters[i].m_Prev.m_Y), CCharacterCore::PhysicalSize());
				if(!Grounded)
				{
					m_Effects.AirJump(ActiveState, Pos, i, 1.0f, Volume);
				}
			}
		}
	}

	if(g_Config.m_ClFreezeStars && !m_SuppressEvents)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			auto &Character = m_Snap.m_aCharacters[ClientId];
			if(Character.m_Active && Character.m_HasExtendedData && Character.m_pPrevExtendedData)
			{
				int FreezeTimeNow = Character.m_ExtendedData.m_FreezeEnd - Client()->GameTick(SessionId, Conn);
				int FreezeTimePrev = Character.m_pPrevExtendedData->m_FreezeEnd - Client()->PrevGameTick(SessionId, Conn);
				vec2 Pos = vec2(Character.m_Cur.m_X, Character.m_Cur.m_Y);
				int StarsNow = (FreezeTimeNow + 1) / Client()->GameTickSpeed();
				int StarsPrev = (FreezeTimePrev + 1) / Client()->GameTickSpeed();
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
		m_Snap.m_pGameInfoObj &&
		!m_Snap.m_SpecInfo.m_Active &&
		m_Snap.m_pLocalCharacter &&
		m_Snap.m_pLocalPrevCharacter)
	{
		const bool RaceFlag = m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME;
		Runtime.m_LastRaceTick = RaceFlag ? -m_Snap.m_pGameInfoObj->m_WarmupTimer : -1;
	}

	SnapCollectEntities(SessionId, Conn); // creates a collection that associates EntityEx snap items with the entities they belong to

	UpdateLocalTuning(SessionId, Session, ActiveState, Conn);
	m_PreviousFocusedStream.reset();
	if(NetworkSource)
		UpdatePrediction();
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
	const bool LocalCharacterMoved = m_Snap.m_pLocalCharacter && m_Snap.m_pLocalPrevCharacter && (m_Snap.m_pLocalCharacter->m_X != m_Snap.m_pLocalPrevCharacter->m_X || m_Snap.m_pLocalCharacter->m_Y != m_Snap.m_pLocalPrevCharacter->m_Y);
	if(!g_Config.m_ClEditor)
	{
		m_EditorMovementDelay = 5;
	}
	else if(m_EditorMovementDelay > 0 && !LocalCharacterMoved)
	{
		--m_EditorMovementDelay;
	}
	if(m_EditorMovementDelay == 0 && LocalCharacterMoved)
	{
		Editor()->OnIngameMoved();
	}
}

void CGameClient::ApplyPreInputs(int Tick, bool Direct, CGameWorld &GameWorld)
{
	if(!g_Config.m_ClAntiPingPreInput)
		return;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(CCharacter *pChar = GameWorld.GetCharacterById(ClientId))
		{
			if(ClientId == GameState(IClient::CONN_MAIN).LocalClientId() || (Client()->DummyConnected() && ClientId == GameState(IClient::CONN_DUMMY).LocalClientId()))
				continue;

			const CNetMsg_Sv_PreInput PreInput = m_aClients[ClientId].m_aPreInputs[Tick % 200];
			if(PreInput.m_IntendedTick != Tick)
				continue;

			//convert preinput to input
			CNetObj_PlayerInput Input = {0};
			Input.m_Direction = PreInput.m_Direction;
			Input.m_TargetX = PreInput.m_TargetX;
			Input.m_TargetY = PreInput.m_TargetY;
			Input.m_Jump = PreInput.m_Jump;
			Input.m_Fire = PreInput.m_Fire;
			Input.m_Hook = PreInput.m_Hook;
			Input.m_WantedWeapon = PreInput.m_WantedWeapon;
			Input.m_NextWeapon = PreInput.m_NextWeapon;
			Input.m_PrevWeapon = PreInput.m_PrevWeapon;

			if(Direct)
			{
				pChar->OnDirectInput(&Input);
			}
			else
			{
				pChar->OnPredictedInput(&Input);
			}
		}
	}
}

void CGameClient::OnPredict(CSessionId SessionId, int Conn)
{
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing prediction game session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing prediction game state");
	pState->Predict(*Client(), SessionId, Conn);
	if(SessionId == Client()->FocusedSessionId() && Conn == ActiveConnection())
		ProcessPrediction();
}

void CGameClient::ProcessPrediction()
{
	const CSessionId SessionId = Client()->NetworkSessionId();
	const int PredictionConnection = ActiveConnection();
	CGameState &ActiveState = GameState(PredictionConnection);
	CGameState::CRuntimeState &Runtime = ActiveState.Runtime();
	// store the previous values so we can detect prediction errors
	CCharacterCore BeforePrevChar = m_PredictedPrevChar;
	CCharacterCore BeforeChar = m_PredictedChar;

	// we can't predict without our own id or own character
	if(m_Snap.m_LocalClientId == -1 || !m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_Active)
		return;

	// don't predict anything if we are paused
	if(m_Snap.m_pGameInfoObj && m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED)
	{
		if(m_Snap.m_pLocalCharacter)
		{
			m_PredictedChar.Read(m_Snap.m_pLocalCharacter);
			m_PredictedChar.m_ActiveWeapon = m_Snap.m_pLocalCharacter->m_Weapon;
		}
		if(m_Snap.m_pLocalPrevCharacter)
		{
			m_PredictedPrevChar.Read(m_Snap.m_pLocalPrevCharacter);
			m_PredictedPrevChar.m_ActiveWeapon = m_Snap.m_pLocalPrevCharacter->m_Weapon;
		}
		return;
	}

	vec2 aBeforeRender[MAX_CLIENTS];
	const int64_t SmoothNow = time_get();
	for(int i = 0; i < MAX_CLIENTS; i++)
		aBeforeRender[i] = GetSmoothPos(SessionId, ActiveState, PredictionConnection, i, SmoothNow, m_aClients[i].m_PrevPredicted, m_aClients[i].m_Predicted);

	// init
	const int OtherPredictionConnection = PredictionConnection == IClient::CONN_MAIN ? IClient::CONN_DUMMY : IClient::CONN_MAIN;
	const CGameState &OtherState = GameState(OtherPredictionConnection);

	// PredictedEvents are only handled in predicted world, so update them here
	m_GameWorld.m_PredictedEvents = m_PredictedWorld.m_PredictedEvents;
	m_PredictedWorld.CopyWorld(&m_GameWorld);

	// don't predict inactive players, or entities from other teams
	for(int i = 0; i < MAX_CLIENTS; i++)
		if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
			if((!m_Snap.m_aCharacters[i].m_Active && pChar->m_SnapTicks > 10) || IsOtherTeam(i))
				pChar->Destroy();

	CProjectile *pProjNext = nullptr;
	for(CProjectile *pProj = (CProjectile *)m_PredictedWorld.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pProj; pProj = pProjNext)
	{
		pProjNext = (CProjectile *)pProj->TypeNext();
		if(IsOtherTeam(pProj->GetOwner()))
		{
			pProj->Destroy();
		}
	}

	CCharacter *pLocalChar = m_PredictedWorld.GetCharacterById(m_Snap.m_LocalClientId);
	if(!pLocalChar)
		return;
	CCharacter *pDummyChar = nullptr;
	if(PredictDummy(OtherState))
		pDummyChar = m_PredictedWorld.GetCharacterById(OtherState.LocalClientId());

	int PredictionTick = Client()->GetPredictionTick(SessionId, PredictionConnection);
	// predict
	for(int Tick = Client()->GameTick(SessionId, PredictionConnection) + 1; Tick <= Client()->PredGameTick(SessionId, PredictionConnection); Tick++)
	{
		// fetch the previous characters
		if(Tick == PredictionTick)
		{
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
					m_aClients[i].m_PrevPredicted = pChar->GetCore();
		}

		if(Tick == Client()->PredGameTick(SessionId, PredictionConnection))
		{
			m_PredictedPrevChar = pLocalChar->GetCore();
			m_aClients[m_Snap.m_LocalClientId].m_PrevPredicted = pLocalChar->GetCore();

			if(pDummyChar)
				m_aClients[OtherState.LocalClientId()].m_PrevPredicted = pDummyChar->GetCore();
		}

		// optionally allow some movement in freeze by not predicting freeze the last one to two ticks
		if(g_Config.m_ClPredictFreeze == 2 && Client()->PredGameTick(SessionId, PredictionConnection) - 1 - Client()->PredGameTick(SessionId, PredictionConnection) % 2 <= Tick)
			pLocalChar->m_CanMoveInFreeze = true;

		// apply inputs and tick
		CNetObj_PlayerInput *pInputData = (CNetObj_PlayerInput *)Client()->GetInput(PredictionConnection, Tick);
		CNetObj_PlayerInput *pDummyInputData = !pDummyChar ? nullptr : (CNetObj_PlayerInput *)Client()->GetInput(OtherPredictionConnection, Tick);
		bool DummyFirst = pInputData && pDummyInputData && pDummyChar->GetCid() < pLocalChar->GetCid();

		if(DummyFirst)
			pDummyChar->OnDirectInput(pDummyInputData);
		if(pInputData)
			pLocalChar->OnDirectInput(pInputData);
		if(pDummyInputData && !DummyFirst)
			pDummyChar->OnDirectInput(pDummyInputData);

		ApplyPreInputs(Tick, true, m_PredictedWorld);

		m_PredictedWorld.m_GameTick = Tick;
		if(pInputData)
			pLocalChar->OnPredictedInput(pInputData);
		if(pDummyInputData)
			pDummyChar->OnPredictedInput(pDummyInputData);

		ApplyPreInputs(Tick, false, m_PredictedWorld);

		m_PredictedWorld.Tick();

		// fetch the current characters
		if(Tick == PredictionTick)
		{
			m_PrevPredictedWorld.CopyWorld(&m_PredictedWorld);

			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
					m_aClients[i].m_Predicted = pChar->GetCore();
		}

		if(Tick == Client()->PredGameTick(SessionId, PredictionConnection))
		{
			m_PredictedChar = pLocalChar->GetCore();
			m_aClients[m_Snap.m_LocalClientId].m_Predicted = pLocalChar->GetCore();

			if(pDummyChar)
				m_aClients[OtherState.LocalClientId()].m_Predicted = pDummyChar->GetCore();
		}

		for(int i = 0; i < MAX_CLIENTS; i++)
			if(CCharacter *pChar = m_PredictedWorld.GetCharacterById(i))
			{
				ActiveState.PredictionHistory(i).m_aPredPos[Tick % 200] = pChar->Core()->m_Pos;
				ActiveState.PredictionHistory(i).m_aPredTick[Tick % 200] = Tick;
			}

		// check if we want to trigger effects
		if(Tick > Runtime.m_LastNewPredictedTick)
		{
			Runtime.m_LastNewPredictedTick = Tick;
			m_NewPredictedTick = true;
			vec2 Pos = pLocalChar->Core()->m_Pos;
			int Events = pLocalChar->Core()->m_TriggeredEvents;
			if(g_Config.m_ClPredict && !m_SuppressEvents)
				if(Events & COREEVENT_AIR_JUMP)
					m_Effects.AirJump(ActiveState, Pos, pLocalChar->GetCid(), 1.0f, 1.0f);
			if(g_Config.m_SndGame && !m_SuppressEvents)
			{
				if(Events & COREEVENT_GROUND_JUMP)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_PLAYER_JUMP, 1.0f, Pos);
				if(Events & COREEVENT_HOOK_ATTACH_GROUND)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_HOOK_ATTACH_GROUND, 1.0f, Pos);
				if(Events & COREEVENT_HOOK_HIT_NOHOOK)
					m_Sounds.PlayAndRecord(CSounds::CHN_WORLD, SOUND_HOOK_NOATTACH, 1.0f, Pos);
				if(Events & COREEVENT_HOOK_ATTACH_PLAYER)
				{
					m_PredictedWorld.CreatePredictedSound(Pos, SOUND_HOOK_ATTACH_PLAYER, pLocalChar->GetCid());
				}
			}
		}

		// check if we want to trigger predicted airjump for dummy
		if(AntiPingPlayers() && pDummyChar && Tick > GameState(OtherPredictionConnection).Runtime().m_LastNewPredictedTick)
		{
			GameState(OtherPredictionConnection).Runtime().m_LastNewPredictedTick = Tick;
			vec2 Pos = pDummyChar->Core()->m_Pos;
			int Events = pDummyChar->Core()->m_TriggeredEvents;
			if(g_Config.m_ClPredict && !m_SuppressEvents)
				if(Events & COREEVENT_AIR_JUMP)
					m_Effects.AirJump(ActiveState, Pos, pDummyChar->GetCid(), 1.0f, 1.0f);
		}

		HandlePredictedEvents(Tick);
	}

	// detect mispredictions of other players and make corrections smoother when possible
	if(g_Config.m_ClAntiPingSmooth && Predict() && AntiPingPlayers() && m_NewTick && Runtime.m_LegacyPredictedTick >= MIN_TICK && absolute(Runtime.m_LegacyPredictedTick - Client()->PredGameTick(SessionId, PredictionConnection)) <= 1 && absolute(Client()->GameTick(SessionId, PredictionConnection) - Client()->PrevGameTick(SessionId, PredictionConnection)) <= 2)
	{
		int PredTime = std::clamp(Client()->GetPredictionTime(SessionId, PredictionConnection), 0, 800);
		float SmoothPace = 4 - 1.5f * PredTime / 800.f; // smoothing pace (a lower value will make the smoothing quicker)
		int64_t Len = 1000 * PredTime * SmoothPace;

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!m_Snap.m_aCharacters[i].m_Active || i == m_Snap.m_LocalClientId || !Runtime.m_aLastPredictedActive[i])
				continue;
			vec2 NewPos = (Runtime.m_LegacyPredictedTick == Client()->PredGameTick(SessionId, PredictionConnection)) ? m_aClients[i].m_Predicted.m_Pos : m_aClients[i].m_PrevPredicted.m_Pos;
			vec2 PredErr = (Runtime.m_aLastPredictedPosition[i] - NewPos) / (float)std::min(Client()->GetPredictionTime(SessionId, PredictionConnection), 200);
			if(in_range(length(PredErr), 0.05f, 5.f))
			{
				vec2 PredPos = mix(m_aClients[i].m_PrevPredicted.m_Pos, m_aClients[i].m_Predicted.m_Pos, Client()->PredIntraGameTick(SessionId, PredictionConnection));
				vec2 CurPos = mix(
					vec2(m_Snap.m_aCharacters[i].m_Prev.m_X, m_Snap.m_aCharacters[i].m_Prev.m_Y),
					vec2(m_Snap.m_aCharacters[i].m_Cur.m_X, m_Snap.m_aCharacters[i].m_Cur.m_Y),
					Client()->IntraGameTick(SessionId, PredictionConnection));
				vec2 RenderDiff = PredPos - aBeforeRender[i];
				vec2 PredDiff = PredPos - CurPos;

				float aMixAmount[2];
				for(int j = 0; j < 2; j++)
				{
					aMixAmount[j] = 1.0f;
					if(absolute(PredErr[j]) > 0.05f)
					{
						aMixAmount[j] = 0.0f;
						if(absolute(RenderDiff[j]) > 0.01f)
						{
							aMixAmount[j] = 1.f - std::clamp(RenderDiff[j] / PredDiff[j], 0.f, 1.f);
							aMixAmount[j] = 1.f - std::pow(1.f - aMixAmount[j], 1 / 1.2f);
						}
					}
					CGameState::CClientPredictionHistory &PredictionHistory = ActiveState.PredictionHistory(i);
					int64_t TimePassed = time_get() - PredictionHistory.m_aSmoothStart[j];
					if(in_range(TimePassed, (int64_t)0, Len - 1))
						aMixAmount[j] = std::min(aMixAmount[j], (float)(TimePassed / (double)Len));
				}
				for(int j = 0; j < 2; j++)
					if(absolute(RenderDiff[j]) < 0.01f && absolute(PredDiff[j]) < 0.01f && absolute(m_aClients[i].m_PrevPredicted.m_Pos[j] - m_aClients[i].m_Predicted.m_Pos[j]) < 0.01f && aMixAmount[j] > aMixAmount[j ^ 1])
						aMixAmount[j] = aMixAmount[j ^ 1];
				for(int j = 0; j < 2; j++)
				{
					// don't smooth for longer than 700ms, or more than 300ms longer along one axis than the other axis
					int64_t Remaining = std::min({
						(1.f - aMixAmount[j]) * Len,
						time_freq() * 0.700f,
						(1.f - aMixAmount[j ^ 1]) * Len + time_freq() * 0.300f,
					});
					int64_t Start = time_get() - (Len - Remaining);
					CGameState::CClientPredictionHistory &PredictionHistory = ActiveState.PredictionHistory(i);
					if(!in_range(Start + Len, PredictionHistory.m_aSmoothStart[j], PredictionHistory.m_aSmoothStart[j] + Len))
					{
						PredictionHistory.m_aSmoothStart[j] = Start;
						PredictionHistory.m_aSmoothLen[j] = Len;
					}
				}
			}
		}
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(m_Snap.m_aCharacters[i].m_Active)
		{
			CGameState::CPredictedClient &PredictedClient = ActiveState.PredictedClient(i);
			PredictedClient.m_HasPrev = true;
			PredictedClient.m_Prev = m_aClients[i].m_PrevPredicted;
			PredictedClient.m_HasCurrent = true;
			PredictedClient.m_Current = m_aClients[i].m_Predicted;
			Runtime.m_aLastPredictedPosition[i] = m_aClients[i].m_Predicted.m_Pos;
			Runtime.m_aLastPredictedActive[i] = true;
		}
		else
		{
			Runtime.m_aLastPredictedActive[i] = false;
		}
	}

	if(g_Config.m_Debug && g_Config.m_ClPredict && Runtime.m_LegacyPredictedTick == Client()->PredGameTick(SessionId, PredictionConnection))
	{
		CNetObj_CharacterCore Before = {0}, Now = {0}, BeforePrev = {0}, NowPrev = {0};
		BeforeChar.Write(&Before);
		BeforePrevChar.Write(&BeforePrev);
		m_PredictedChar.Write(&Now);
		m_PredictedPrevChar.Write(&NowPrev);

		if(mem_comp(&Before, &Now, sizeof(CNetObj_CharacterCore)) != 0)
		{
			log_trace("client", "prediction error");
			for(unsigned i = 0; i < sizeof(CNetObj_CharacterCore) / sizeof(int); i++)
			{
				if(((int *)&Before)[i] != ((int *)&Now)[i])
				{
					log_trace("client", "	%d %d %d (%d %d)", i, ((int *)&Before)[i], ((int *)&Now)[i], ((int *)&BeforePrev)[i], ((int *)&NowPrev)[i]);
				}
			}
		}
	}

	Runtime.m_LegacyPredictedTick = Client()->PredGameTick(SessionId, PredictionConnection);

	if(m_NewPredictedTick)
		m_Ghost.OnNewPredictedSnapshot();
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
	if(Client()->FocusedSessionId() != Client()->NetworkSessionId())
		return;
	CNetMsg_Cl_SetTeam Msg;
	Msg.m_Team = Team;
	Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_VITAL);
}

void CGameClient::SendStartInfo7(CSessionId SessionId, int Conn)
{
	const int ProfileIndex = LegacyProfileIndex(Conn);
	const CLocalPlayerProfile &Profile = RefreshLegacyPlayerProfile(SessionId, Conn);
	protocol7::CNetMsg_Cl_StartInfo Msg;
	Msg.m_pName = Profile.m_Name.c_str();
	Msg.m_pClan = Profile.m_Clan.c_str();
	Msg.m_Country = Profile.m_Country;
	for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = CSkins7::ms_apSkinVariables[ProfileIndex][p];
		Msg.m_aUseCustomColors[p] = *CSkins7::ms_apUCCVariables[ProfileIndex][p];
		Msg.m_aSkinPartColors[p] = *CSkins7::ms_apColorVariables[ProfileIndex][p];
	}
	CMsgPacker Packer(&Msg, false, true);
	if(Msg.Pack(&Packer))
		return;
	Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing start-info game session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing start-info game state");
	pState->Runtime().m_CheckInfo = -1;
}

const CLocalPlayerProfile &CGameClient::RefreshLegacyPlayerProfile(CSessionId SessionId, int Conn)
{
	const bool UseDummyProfile = LegacyProfileIndex(Conn);
	const CStreamId StreamId(Conn + 1);
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing player-profile game session context");
	pSession->LocalPlayerProfiles().Set(StreamId, CLocalPlayerProfile::FromLegacyConfig(*Config(), UseDummyProfile, UseDummyProfile ? Client()->DummyName() : Client()->PlayerName()));
	return *pSession->LocalPlayerProfiles().Find(StreamId);
}

void CGameClient::SendSkinChange7(CSessionId SessionId, int Conn)
{
	const int ProfileIndex = LegacyProfileIndex(Conn);
	protocol7::CNetMsg_Cl_SkinChange Msg;
	for(int p = 0; p < protocol7::NUM_SKINPARTS; p++)
	{
		Msg.m_apSkinPartNames[p] = CSkins7::ms_apSkinVariables[ProfileIndex][p];
		Msg.m_aUseCustomColors[p] = *CSkins7::ms_apUCCVariables[ProfileIndex][p];
		Msg.m_aSkinPartColors[p] = *CSkins7::ms_apColorVariables[ProfileIndex][p];
	}
	CMsgPacker Packer(&Msg, false, true);
	if(Msg.Pack(&Packer))
		return;
	Client()->SendMsg(Conn, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing skin-change game session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing skin-change game state");
	pState->Runtime().m_CheckInfo = Client()->GameTickSpeed();
}

bool CGameClient::GotWantedSkin7(int Conn)
{
	const int ProfileIndex = LegacyProfileIndex(Conn);
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
		str_copy(aSkinParts[SkinPart], CSkins7::ms_apSkinVariables[ProfileIndex][SkinPart], protocol7::MAX_SKIN_ARRAY_SIZE);
		apSkinPartsPtr[SkinPart] = aSkinParts[SkinPart];
		aUCCVars[SkinPart] = *CSkins7::ms_apUCCVariables[ProfileIndex][SkinPart];
		aColorVars[SkinPart] = *CSkins7::ms_apColorVariables[ProfileIndex][SkinPart];
	}
	m_Skins7.ValidateSkinParts(apSkinPartsPtr, aUCCVars, aColorVars, Client()->TranslationContext(Client()->NetworkSessionId()).m_GameFlags);

	const CGameSessionContext *pSession = FindSessionContext(Client()->NetworkSessionId());
	dbg_assert(pSession != nullptr, "missing wanted-skin game session context");
	const CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing wanted-skin game state");
	const int LocalClientId = pState->LocalClientId();
	if(LocalClientId < 0 || LocalClientId >= MAX_CLIENTS)
		return false;
	const CGameState::CProtocol7ClientState &Protocol7Client = pState->Protocol7Client(LocalClientId);
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
	// if(str_comp(m_aClients[LocalClientId].m_aName, ProfileIndex ? Client()->DummyName() : Client()->PlayerName()))
	// 	return false;
	// if(str_comp(m_aClients[LocalClientId].m_aClan, ProfileIndex ? g_Config.m_ClDummyClan : g_Config.m_PlayerClan))
	// 	return false;
	// if(m_aClients[LocalClientId].m_Country != (ProfileIndex ? g_Config.m_ClDummyCountry : g_Config.m_PlayerCountry))
	// 	return false;

	return true;
}

void CGameClient::SendInfo(CSessionId SessionId, bool Start)
{
	if(m_pClient->IsSixup(SessionId))
	{
		if(Start)
			SendStartInfo7(SessionId, IClient::CONN_MAIN);
		else
			SendSkinChange7(SessionId, IClient::CONN_MAIN);
		return;
	}
	const CLocalPlayerProfile &Profile = RefreshLegacyPlayerProfile(SessionId, IClient::CONN_MAIN);
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
		Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL | MSGFLAG_FLUSH);
		CGameSessionContext *pSession = FindSessionContext(SessionId);
		dbg_assert(pSession != nullptr, "missing info game session context");
		CGameState *pState = pSession->GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
		dbg_assert(pState != nullptr, "missing info game state");
		pState->Runtime().m_CheckInfo = -1;
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
		Client()->SendMsg(IClient::CONN_MAIN, &Packer, MSGFLAG_VITAL);
		CGameSessionContext *pSession = FindSessionContext(SessionId);
		dbg_assert(pSession != nullptr, "missing info game session context");
		CGameState *pState = pSession->GameStates().FindByStream(CStreamId(IClient::CONN_MAIN + 1));
		dbg_assert(pState != nullptr, "missing info game state");
		pState->Runtime().m_CheckInfo = Client()->GameTickSpeed();
	}
}

void CGameClient::SendDummyInfo(bool Start)
{
	const CSessionId SessionId = Client()->NetworkSessionId();
	CGameSessionContext *pSession = FindSessionContext(SessionId);
	dbg_assert(pSession != nullptr, "missing Network game session context");
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(IClient::CONN_DUMMY + 1));
	dbg_assert(pState != nullptr, "missing Network dummy game state");
	if(m_pClient->IsSixup(SessionId))
	{
		if(Start)
			SendStartInfo7(SessionId, IClient::CONN_DUMMY);
		else
			SendSkinChange7(SessionId, IClient::CONN_DUMMY);
		return;
	}
	const CLocalPlayerProfile &Profile = RefreshLegacyPlayerProfile(SessionId, IClient::CONN_DUMMY);
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
		Client()->SendMsg(IClient::CONN_DUMMY, &Packer, MSGFLAG_VITAL);
		pState->Runtime().m_CheckInfo = -1;
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
		Client()->SendMsg(IClient::CONN_DUMMY, &Packer, MSGFLAG_VITAL);
		pState->Runtime().m_CheckInfo = Client()->GameTickSpeed();
	}
}

void CGameClient::SendKill() const
{
	if(Client()->FocusedSessionId() != Client()->NetworkSessionId())
		return;
	const int ActiveConn = Client()->ActiveConnection();
	CNetMsg_Cl_Kill Msg;
	Client()->SendPackMsg(ActiveConn, &Msg, MSGFLAG_VITAL);

	if(g_Config.m_ClDummyCopyMoves)
	{
		CMsgPacker MsgP(NETMSGTYPE_CL_KILL, false);
		Client()->SendMsg(ActiveConn == IClient::CONN_MAIN ? IClient::CONN_DUMMY : IClient::CONN_MAIN, &MsgP, MSGFLAG_VITAL);
	}
}

void CGameClient::SendReadyChange7() // NOLINT(readability-make-member-function-const)
{
	if(Client()->FocusedSessionId() != Client()->NetworkSessionId())
		return;
	if(!Client()->IsSixup(Client()->NetworkSessionId()))
	{
		log_error("client", "You have to be connected to a 0.7 server to use 'ready_change'");
		return;
	}
	protocol7::CNetMsg_Cl_ReadyChange Msg;
	Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_VITAL, true);
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
		pSelf->SendInfo(pSelf->Client()->NetworkSessionId(), false);
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
		if(g_Config.m_ClDummy && !pSelf->Client()->DummyConnected())
			g_Config.m_ClDummy = 0;
		pSelf->Client()->SetActiveConnection(g_Config.m_ClDummy);
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

void CGameClient::UpdateLocalTuning(CSessionId SessionId, CGameSessionContext &Session, CGameState &State, int Conn)
{
	CGameState::CRuntimeState &Runtime = State.Runtime();
	const CGameState *pPreviousFocusedState = m_PreviousFocusedStream.has_value() ? Session.GameStates().FindByStream(*m_PreviousFocusedStream) : nullptr;
	m_GameWorld.m_WorldConfig.m_UseTuneZones = State.CoreGameInfo().m_PredictDDRaceTiles;

	// always update default tune zone, even without character
	if(!m_GameWorld.m_WorldConfig.m_UseTuneZones)
		m_GameWorld.TuningList()[0] = Runtime.m_CurrentTuning;

	if(!m_Snap.m_pLocalCharacter && !m_Snap.m_pSpectatorInfo)
		return;

	vec2 LocalPos = m_Snap.m_pLocalCharacter ? vec2(m_Snap.m_pLocalCharacter->m_X, m_Snap.m_pLocalCharacter->m_Y) : vec2(m_Snap.m_pSpectatorInfo->m_X, m_Snap.m_pSpectatorInfo->m_Y);

	// update the tuning at the local position with the latest tunings received before the new snapshot
	if(m_GameWorld.m_WorldConfig.m_UseTuneZones)
	{
		int TuneZone =
			m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_HasExtendedData &&
					m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_ExtendedData.m_TuneZoneOverride != TuneZone::OVERRIDE_NONE ?
				m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_ExtendedData.m_TuneZoneOverride :
				Session.MapContext().Collision()->IsTune(Session.MapContext().Collision()->GetMapIndex(LocalPos));

		if(TuneZone != Runtime.m_LocalTuneZone)
		{
			// our tunezone changed, expecting tuning message
			Runtime.m_LocalTuneZone = Runtime.m_ExpectingTuningForZone = TuneZone;
			Runtime.m_ExpectingTuningSince = 0;
		}

		// tunezone could have changed, send dummy tuning to demo
		if(SessionId == Client()->NetworkSessionId() && m_ActiveRecordings.any() && pPreviousFocusedState && Runtime.m_LocalTuneZone != pPreviousFocusedState->Runtime().m_LocalTuneZone)
		{
			CMsgPacker Msg(NETMSGTYPE_SV_TUNEPARAMS);
			int *pParams = (int *)&Runtime.m_CurrentTuning;
			for(unsigned i = 0; i < sizeof(Runtime.m_CurrentTuning) / sizeof(int); i++)
				Msg.AddInt(pParams[i]);
			Client()->SendMsg(Conn, &Msg, MSGFLAG_RECORD | MSGFLAG_NOSEND);
		}

		if(Runtime.m_ExpectingTuningForZone >= 0)
		{
			if(Runtime.m_ReceivedTuning)
			{
				Session.MapContext().TuningList()[Runtime.m_ExpectingTuningForZone] = Runtime.m_CurrentTuning;
				m_GameWorld.TuningList()[Runtime.m_ExpectingTuningForZone] = Runtime.m_CurrentTuning;
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
			m_GameWorld.TuningList()[TuneZone] = Runtime.m_CurrentTuning;
			Runtime.m_ExpectingTuningSince = 0;
			Runtime.m_ReceivedTuning = false;
		}
	}
}

void CGameClient::UpdatePrediction()
{
	const CSessionId SessionId = Client()->NetworkSessionId();
	const int PredictionConnection = ActiveConnection();
	const int OtherPredictionConnection = PredictionConnection == IClient::CONN_MAIN ? IClient::CONN_DUMMY : IClient::CONN_MAIN;
	CGameState &ActiveState = GameState(PredictionConnection);
	const CGameState &OtherState = GameState(OtherPredictionConnection);
	CGameState::CRuntimeState &Runtime = ActiveState.Runtime();
	m_GameWorld.m_WorldConfig.m_IsVanilla = FocusedGameInfo().m_PredictVanilla;
	m_GameWorld.m_WorldConfig.m_IsDDRace = FocusedGameInfo().m_PredictDDRace;
	m_GameWorld.m_WorldConfig.m_IsFNG = FocusedGameInfo().m_PredictFNG;
	m_GameWorld.m_WorldConfig.m_PredictDDRace = FocusedGameInfo().m_PredictDDRace;
	m_GameWorld.m_WorldConfig.m_PredictTiles = FocusedGameInfo().m_PredictDDRace && FocusedGameInfo().m_PredictDDRaceTiles;
	m_GameWorld.m_WorldConfig.m_PredictFreeze = g_Config.m_ClPredictFreeze;
	m_GameWorld.m_WorldConfig.m_PredictWeapons = AntiPingWeapons();
	m_GameWorld.m_WorldConfig.m_BugDDRaceInput = FocusedGameInfo().m_BugDDRaceInput;
	m_GameWorld.m_WorldConfig.m_NoWeakHookAndBounce = FocusedGameInfo().m_NoWeakHookAndBounce;
	m_GameWorld.m_WorldConfig.m_PredictEvents = FocusedGameInfo().m_PredictEvents;

	if(!m_Snap.m_pLocalCharacter)
	{
		if(CCharacter *pLocalChar = m_GameWorld.GetCharacterById(m_Snap.m_LocalClientId))
			pLocalChar->Destroy();
		return;
	}

	if(m_Snap.m_pLocalCharacter->m_AmmoCount > 0 && m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		m_GameWorld.m_WorldConfig.m_InfiniteAmmo = false;
	const CTuningParams &CurrentTuning = Runtime.m_CurrentTuning;
	m_GameWorld.m_WorldConfig.m_IsSolo = !m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_HasExtendedData && !CurrentTuning.m_PlayerCollision && !CurrentTuning.m_PlayerHooking;

	CCharacter *pLocalChar = m_GameWorld.GetCharacterById(m_Snap.m_LocalClientId);
	CCharacter *pDummyChar = nullptr;
	const bool PredictOther = PredictDummy(OtherState);
	if(PredictOther)
		pDummyChar = m_GameWorld.GetCharacterById(OtherState.LocalClientId());

	// update strong and weak hook
	if(pLocalChar && !m_Snap.m_SpecInfo.m_Active && !Client()->IsDemoPlayback() && (CurrentTuning.m_PlayerCollision || CurrentTuning.m_PlayerHooking))
	{
		if(m_Snap.m_aCharacters[m_Snap.m_LocalClientId].m_HasExtendedData)
		{
			int aIds[MAX_CLIENTS];
			for(int &Id : aIds)
				Id = -1;
			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
					aIds[pChar->GetStrongWeakId()] = i;
			for(int Id : aIds)
				if(Id >= 0)
					Runtime.m_CharOrder.GiveStrong(Id);
		}
		else
		{
			// manual detection
			DetectStrongHook(Runtime);
		}
		for(int i : Runtime.m_CharOrder.m_Ids)
		{
			if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
			{
				m_GameWorld.RemoveEntity(pChar);
				m_GameWorld.InsertEntity(pChar);
			}
		}
	}

	// advance the gameworld to the current gametick
	if(pLocalChar && absolute(m_GameWorld.GameTick() - Client()->GameTick(SessionId, PredictionConnection)) < Client()->GameTickSpeed())
	{
		for(int Tick = m_GameWorld.GameTick() + 1; Tick <= Client()->GameTick(SessionId, PredictionConnection); Tick++)
		{
			CNetObj_PlayerInput *pInput = (CNetObj_PlayerInput *)Client()->GetInput(PredictionConnection, Tick);
			CNetObj_PlayerInput *pDummyInput = nullptr;
			if(pDummyChar)
				pDummyInput = (CNetObj_PlayerInput *)Client()->GetInput(OtherPredictionConnection, Tick);
			if(pInput)
				pLocalChar->OnDirectInput(pInput);
			if(pDummyInput)
				pDummyChar->OnDirectInput(pDummyInput);

			ApplyPreInputs(Tick, true, m_GameWorld);

			m_GameWorld.m_GameTick = Tick;
			if(pInput)
				pLocalChar->OnPredictedInput(pInput);
			if(pDummyInput)
				pDummyChar->OnPredictedInput(pDummyInput);

			ApplyPreInputs(Tick, false, m_GameWorld);

			m_GameWorld.Tick();

			for(int i = 0; i < MAX_CLIENTS; i++)
				if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
				{
					ActiveState.PredictionHistory(i).m_aPredPos[Tick % 200] = pChar->Core()->m_Pos;
					ActiveState.PredictionHistory(i).m_aPredTick[Tick % 200] = Tick;
				}
		}
	}
	else
	{
		// skip to current gametick
		m_GameWorld.m_GameTick = Client()->GameTick(SessionId, PredictionConnection);
		if(pLocalChar)
			if(CNetObj_PlayerInput *pInput = (CNetObj_PlayerInput *)Client()->GetInput(PredictionConnection, Client()->GameTick(SessionId, PredictionConnection)))
				pLocalChar->SetInput(pInput);
		if(pDummyChar)
			if(CNetObj_PlayerInput *pInput = (CNetObj_PlayerInput *)Client()->GetInput(OtherPredictionConnection, Client()->GameTick(SessionId, PredictionConnection)))
				pDummyChar->SetInput(pInput);
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(CCharacter *pChar = m_GameWorld.GetCharacterById(i))
		{
			ActiveState.PredictionHistory(i).m_aPredPos[Client()->GameTick(SessionId, PredictionConnection) % 200] = pChar->Core()->m_Pos;
			ActiveState.PredictionHistory(i).m_aPredTick[Client()->GameTick(SessionId, PredictionConnection) % 200] = Client()->GameTick(SessionId, PredictionConnection);
		}

	// update the local gameworld with the new snapshot
	m_GameWorld.NetObjBegin(FocusedTeams(), m_Snap.m_LocalClientId);

	for(int i = 0; i < MAX_CLIENTS; i++)
		if(m_Snap.m_aCharacters[i].m_Active)
		{
			bool IsLocal = i == m_Snap.m_LocalClientId || (PredictOther && i == OtherState.LocalClientId());
			int GameTeam = IsTeamPlay() ? m_aClients[i].m_Team : i;
			m_GameWorld.NetCharAdd(i, &m_Snap.m_aCharacters[i].m_Cur,
				m_Snap.m_aCharacters[i].m_HasExtendedData ? &m_Snap.m_aCharacters[i].m_ExtendedData : nullptr,
				GameTeam, IsLocal);
		}

	for(const CSnapEntities &EntData : SnapEntities())
		m_GameWorld.NetObjAdd(EntData.m_Item.m_Id, EntData.m_Item.m_Type, EntData.m_Item.m_pData, EntData.m_pDataEx);

	m_GameWorld.NetObjEnd();
}

void CGameClient::UpdateRenderedClients(const CGameSessionContext &Session, CGameState &State, int Conn, int64_t Now, const CGameTickInfo &Time, EPresentationPlayback Playback)
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
		for(const auto &pSessionState : Session.GameStates().States())
		{
			if(pSessionState.get() == &State || pSessionState->LocalClientId() != ClientId)
				continue;
			const CGameState::CClientSnapshot &SessionLocalClient = pSessionState->Client(ClientId);
			IsSecondaryLocal = !SessionLocalClient.m_HasDDNetPlayer || (SessionLocalClient.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_PAUSED) == 0;
			break;
		}
		const bool PredictedLocal = ClientId == LocalClientId || (g_Config.m_ClPredictDummy && IsSecondaryLocal);
		const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
		const CGameState::CPredictedClient &PredictedClient = State.PredictedClient(ClientId);
		const CCharacter *pCharacter = State.PredictedWorld().GetCharacterById(ClientId);
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
				RenderedClient.m_Position = GetSmoothPos(Session.Id(), State, Conn, ClientId, Now, PredictedClient.m_Prev, PredictedClient.m_Current);
		}
	}
}

void CGameClient::UpdateSpectatorCursor(const CGameState &State, const CGameTickInfo &Time)
{
	CGameView &View = LegacyGameView();
	CGameView::CSpectatorCursorState &Cursor = View.SpectatorCursor();
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

	if(IsDemoPlaybackPaused())
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
				aTime[i] = Cursor.m_aTargetSamplesTime[Cursor.m_NumSamples - 1] + CCursorState::REST_THRESHOLD * (Offset + 1);
				aData[i] = Cursor.m_aTargetSamplesData[Cursor.m_NumSamples - 1];
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
		float OffsetAmount = std::max(l - m_Snap.m_SpecInfo.m_Deadzone, 0.0f) * (m_Snap.m_SpecInfo.m_FollowFactor / 100.0f);
		TargetCameraOffset = normalize(Cursor.m_Target) * OffsetAmount;
	}

	// if we are in auto spec mode, use camera zoom to smooth out cursor transitions
	const float Zoom = (m_Camera.IsZooming() && m_Camera.IsAutoSpecCameraZooming()) ? m_Camera.Zoom() : m_Snap.m_SpecInfo.m_Zoom;
	Cursor.m_WorldTarget = Cursor.m_Position + (Cursor.m_Target - TargetCameraOffset) * Zoom + TargetCameraOffset;
}

void CGameClient::HandlePredictedEvents(const int Tick)
{
	const float Alpha = 1.0f;
	const float Volume = 1.0f;

	auto EventsIterator = m_PredictedWorld.m_PredictedEvents.begin();
	while(EventsIterator != m_PredictedWorld.m_PredictedEvents.end())
	{
		if(!EventsIterator->m_Handled && EventsIterator->m_Tick <= Tick)
		{
			if(EventsIterator->m_EventId == NETEVENTTYPE_SOUNDWORLD)
			{
				if(FocusedGameInfo().m_RaceSounds && ((EventsIterator->m_ExtraInfo == SOUND_GUN_FIRE && !g_Config.m_SndGun) || (EventsIterator->m_ExtraInfo == SOUND_PLAYER_PAIN_LONG && !g_Config.m_SndLongPain)))
				{
					EventsIterator = m_PredictedWorld.m_PredictedEvents.erase(EventsIterator);
					continue;
				}
				m_Sounds.PlayAt(CSounds::CHN_WORLD, EventsIterator->m_ExtraInfo, 1.0f, EventsIterator->m_Pos);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_EXPLOSION)
			{
				m_Effects.Explosion(GameState(ActiveConnection()), EventsIterator->m_Pos, Alpha);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_HAMMERHIT)
			{
				m_Effects.HammerHit(GameState(ActiveConnection()), EventsIterator->m_Pos, Alpha, Volume);
			}
			else if(EventsIterator->m_EventId == NETEVENTTYPE_DAMAGEIND)
			{
				m_Effects.DamageIndicator(GameState(ActiveConnection()), EventsIterator->m_Pos, direction(EventsIterator->m_ExtraInfo / 256.0f), -1, Alpha);
			}

			EventsIterator->m_Handled = true;
			++EventsIterator;
			continue;
		}
		else if(Tick - EventsIterator->m_Tick > 3 * Client()->GameTickSpeed()) // 3 seconds
		{
			// remove too old events
			EventsIterator = m_PredictedWorld.m_PredictedEvents.erase(EventsIterator);
		}
		else
		{
			++EventsIterator;
		}
	}
}

void CGameClient::DetectStrongHook(CGameState::CRuntimeState &Runtime)
{
	const CSessionId SessionId = Client()->NetworkSessionId();
	const int Conn = ActiveConnection();
	CTeamsCore Teams = FocusedTeams();
	// attempt to detect strong/weak between players
	for(int FromPlayer = 0; FromPlayer < MAX_CLIENTS; FromPlayer++)
	{
		if(!m_Snap.m_aCharacters[FromPlayer].m_Active)
			continue;
		int ToPlayer = m_Snap.m_aCharacters[FromPlayer].m_Prev.m_HookedPlayer;
		if(ToPlayer < 0 || ToPlayer >= MAX_CLIENTS || !m_Snap.m_aCharacters[ToPlayer].m_Active || ToPlayer != m_Snap.m_aCharacters[FromPlayer].m_Cur.m_HookedPlayer)
			continue;
		if(absolute(std::min(Runtime.m_aStrongHookLastUpdateTick[ToPlayer], Runtime.m_aStrongHookLastUpdateTick[FromPlayer]) - Client()->GameTick(SessionId, Conn)) < Client()->GameTickSpeed() / 4)
			continue;
		if(m_Snap.m_aCharacters[FromPlayer].m_Prev.m_Direction != m_Snap.m_aCharacters[FromPlayer].m_Cur.m_Direction || m_Snap.m_aCharacters[ToPlayer].m_Prev.m_Direction != m_Snap.m_aCharacters[ToPlayer].m_Cur.m_Direction)
			continue;

		CCharacter *pFromCharWorld = m_GameWorld.GetCharacterById(FromPlayer);
		CCharacter *pToCharWorld = m_GameWorld.GetCharacterById(ToPlayer);
		if(!pFromCharWorld || !pToCharWorld)
			continue;

		Runtime.m_aStrongHookLastUpdateTick[ToPlayer] = Runtime.m_aStrongHookLastUpdateTick[FromPlayer] = Client()->GameTick(SessionId, Conn);

		float aPredictErr[2];
		CCharacterCore ToCharCur;
		ToCharCur.Read(&m_Snap.m_aCharacters[ToPlayer].m_Cur);

		CWorldCore World;

		for(int Direction = 0; Direction < 2; Direction++)
		{
			CCharacterCore ToChar = pFromCharWorld->GetCore();
			ToChar.Init(&World, Collision(), &Teams);
			World.m_apCharacters[ToPlayer] = &ToChar;
			ToChar.Read(&m_Snap.m_aCharacters[ToPlayer].m_Prev);

			CCharacterCore FromChar = pFromCharWorld->GetCore();
			FromChar.Init(&World, Collision(), &Teams);
			World.m_apCharacters[FromPlayer] = &FromChar;
			FromChar.Read(&m_Snap.m_aCharacters[FromPlayer].m_Prev);

			for(int Tick = Client()->PrevGameTick(SessionId, Conn); Tick < Client()->GameTick(SessionId, Conn); Tick++)
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
				if(ToPlayer != m_Snap.m_LocalClientId)
					Runtime.m_CharOrder.GiveWeak(ToPlayer);
				else
					Runtime.m_CharOrder.GiveStrong(FromPlayer);
			}
		}
		else if(aPredictErr[0] < LOW && aPredictErr[1] > HIGH)
		{
			if(Runtime.m_CharOrder.HasStrongAgainst(FromPlayer, ToPlayer))
			{
				if(ToPlayer != m_Snap.m_LocalClientId)
					Runtime.m_CharOrder.GiveStrong(ToPlayer);
				else
					Runtime.m_CharOrder.GiveWeak(FromPlayer);
			}
		}
	}
}

vec2 CGameClient::GetSmoothPos(CSessionId SessionId, const CGameState &State, int Conn, int ClientId, int64_t Now, const CCharacterCore &Prev, const CCharacterCore &Current)
{
	vec2 Pos = mix(Prev.m_Pos, Current.m_Pos, Client()->PredIntraGameTick(SessionId, Conn));
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
			Client()->GetSmoothTick(SessionId, Conn, Now, &SmoothTick, &SmoothIntra, MixAmount);
			if(SmoothTick > 0 && PredictionHistory.m_aPredTick[(SmoothTick - 1) % 200] >= Client()->PrevGameTick(SessionId, Conn) && PredictionHistory.m_aPredTick[SmoothTick % 200] <= Client()->PredGameTick(SessionId, Conn))
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
	bool Local = m_Snap.m_LocalClientId == ClientId;

	if(m_Snap.m_LocalClientId < 0)
	{
		return false;
	}
	else if((m_Snap.m_SpecInfo.m_Active && m_Snap.m_SpecInfo.m_SpectatorId == SPEC_FREEVIEW) || ClientId < 0)
	{
		return false;
	}
	else if(m_Snap.m_SpecInfo.m_Active && m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
	{
		if(FocusedTeams().Team(ClientId) == FocusedTeams().TeamSuper() || FocusedTeams().Team(m_Snap.m_SpecInfo.m_SpectatorId) == FocusedTeams().TeamSuper())
			return false;
		return FocusedTeams().Team(ClientId) != FocusedTeams().Team(m_Snap.m_SpecInfo.m_SpectatorId);
	}
	else
	{
		const CGameState &State = GameState(ActiveConnection());
		const CNetObj_DDNetCharacter *pLocalExtended = State.ExtendedCharacter(m_Snap.m_LocalClientId);
		const CNetObj_DDNetCharacter *pClientExtended = State.ExtendedCharacter(ClientId);
		const bool LocalSolo = pLocalExtended != nullptr && (pLocalExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
		const bool ClientSolo = pClientExtended != nullptr && (pClientExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
		if((LocalSolo || ClientSolo) && !Local)
			return true;
	}

	if(FocusedTeams().Team(ClientId) == FocusedTeams().TeamSuper() || FocusedTeams().Team(m_Snap.m_LocalClientId) == FocusedTeams().TeamSuper())
		return false;

	return FocusedTeams().Team(ClientId) != FocusedTeams().Team(m_Snap.m_LocalClientId);
}

int CGameClient::SwitchStateTeam() const
{
	if(GameState(ActiveConnection()).Runtime().m_SwitchStateTeam >= 0)
		return GameState(ActiveConnection()).Runtime().m_SwitchStateTeam;
	else if(m_Snap.m_LocalClientId < 0)
		return 0;
	else if(m_Snap.m_SpecInfo.m_Active && m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
		return FocusedTeams().Team(m_Snap.m_SpecInfo.m_SpectatorId);
	return FocusedTeams().Team(m_Snap.m_LocalClientId);
}

bool CGameClient::IsLocalCharSuper() const
{
	if(m_Snap.m_LocalClientId < 0)
		return false;
	const CNetObj_DDNetCharacter *pExtended = GameState(ActiveConnection()).ExtendedCharacter(m_Snap.m_LocalClientId);
	return pExtended != nullptr && (pExtended->m_Flags & CHARACTERFLAG_SUPER) != 0;
}

CGameClient::CImageAsset CGameClient::LoadAssetFromPath(const char *pPath, bool AsDir, int AssetId, const char *pDirectory) const
{
	CImageAsset LoadedAsset;
	LoadedAsset.m_IsDefault = str_comp(pPath, "default") == 0;
	if(LoadedAsset.m_IsDefault)
	{
		str_copy(LoadedAsset.m_aPath, g_pData->m_aImages[AssetId].m_pFilename);
	}
	else if(AsDir)
	{
		str_format(LoadedAsset.m_aPath, sizeof(LoadedAsset.m_aPath), "assets/%s/%s/%s", pDirectory, pPath, g_pData->m_aImages[AssetId].m_pFilename);
	}
	else
	{
		str_format(LoadedAsset.m_aPath, sizeof(LoadedAsset.m_aPath), "assets/%s/%s.png", pDirectory, pPath);
	}

	Graphics()->LoadPng(LoadedAsset.m_ImageInfo, LoadedAsset.m_aPath, IStorage::TYPE_ALL);

	if(!LoadedAsset.m_IsDefault && LoadedAsset.IsLoaded())
	{
		CImageInfo ImgDefaultInfo;
		if(Graphics()->LoadPng(ImgDefaultInfo, g_pData->m_aImages[AssetId].m_pFilename, IStorage::TYPE_ALL))
			LoadedAsset.m_FallbackImageInfo = std::move(ImgDefaultInfo);
	}
	return LoadedAsset;
}

void CGameClient::LoadGameSkin(const char *pPath, bool AsDir)
{
	if(m_GameSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHealthFull);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHealthEmpty);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteArmorFull);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteArmorEmpty);

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammerCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGunCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgunCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenadeCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinjaCursor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaserCursor);

		for(auto &SpriteWeaponCursor : m_GameSkin.m_aSpriteWeaponCursors)
		{
			SpriteWeaponCursor = IGraphics::CTextureHandle();
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookChain);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteHookHead);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammer);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaser);

		for(auto &SpriteWeapon : m_GameSkin.m_aSpriteWeapons)
		{
			SpriteWeapon = IGraphics::CTextureHandle();
		}

		for(auto &SpriteParticle : m_GameSkin.m_aSpriteParticles)
		{
			Graphics()->UnloadTexture(&SpriteParticle);
		}

		for(auto &SpriteStar : m_GameSkin.m_aSpriteStars)
		{
			Graphics()->UnloadTexture(&SpriteStar);
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGunProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponShotgunProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponGrenadeProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponHammerProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponNinjaProjectile);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteWeaponLaserProjectile);

		for(auto &SpriteWeaponProjectile : m_GameSkin.m_aSpriteWeaponProjectiles)
		{
			SpriteWeaponProjectile = IGraphics::CTextureHandle();
		}

		for(int i = 0; i < 3; ++i)
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponGunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i]);
			Graphics()->UnloadTexture(&m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i]);

			for(auto &SpriteWeaponsMuzzle : m_GameSkin.m_aaSpriteWeaponsMuzzles)
			{
				SpriteWeaponsMuzzle[i] = IGraphics::CTextureHandle();
			}
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupHealth);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmor);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorLaser);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupArmorNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupGrenade);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupShotgun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupLaser);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupNinja);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupGun);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpritePickupHammer);

		for(auto &SpritePickupWeapon : m_GameSkin.m_aSpritePickupWeapons)
		{
			SpritePickupWeapon = IGraphics::CTextureHandle();
		}

		for(auto &SpritePickupWeaponArmor : m_GameSkin.m_aSpritePickupWeaponArmor)
		{
			SpritePickupWeaponArmor = IGraphics::CTextureHandle();
		}

		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteFlagBlue);
		Graphics()->UnloadTexture(&m_GameSkin.m_SpriteFlagRed);

		if(m_GameSkin.IsSixup())
		{
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarFullLeft);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarFull);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarEmpty);
			Graphics()->UnloadTexture(&m_GameSkin.m_SpriteNinjaBarEmptyRight);
		}

		m_GameSkinLoaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_GAME, "game");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			LoadGameSkin("default");
		else
			LoadGameSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_HEALTH_FULL].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
	{
		m_GameSkin.m_SpriteHealthFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HEALTH_FULL]);
		m_GameSkin.m_SpriteHealthEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HEALTH_EMPTY]);
		m_GameSkin.m_SpriteArmorFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_ARMOR_FULL]);
		m_GameSkin.m_SpriteArmorEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_ARMOR_EMPTY]);

		m_GameSkin.m_SpriteWeaponHammerCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_HAMMER_CURSOR]);
		m_GameSkin.m_SpriteWeaponGunCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_CURSOR]);
		m_GameSkin.m_SpriteWeaponShotgunCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_CURSOR]);
		m_GameSkin.m_SpriteWeaponGrenadeCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_CURSOR]);
		m_GameSkin.m_SpriteWeaponNinjaCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_CURSOR]);
		m_GameSkin.m_SpriteWeaponLaserCursor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_CURSOR]);

		m_GameSkin.m_aSpriteWeaponCursors[0] = m_GameSkin.m_SpriteWeaponHammerCursor;
		m_GameSkin.m_aSpriteWeaponCursors[1] = m_GameSkin.m_SpriteWeaponGunCursor;
		m_GameSkin.m_aSpriteWeaponCursors[2] = m_GameSkin.m_SpriteWeaponShotgunCursor;
		m_GameSkin.m_aSpriteWeaponCursors[3] = m_GameSkin.m_SpriteWeaponGrenadeCursor;
		m_GameSkin.m_aSpriteWeaponCursors[4] = m_GameSkin.m_SpriteWeaponLaserCursor;
		m_GameSkin.m_aSpriteWeaponCursors[5] = m_GameSkin.m_SpriteWeaponNinjaCursor;

		// weapons and hook
		m_GameSkin.m_SpriteHookChain = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HOOK_CHAIN]);
		m_GameSkin.m_SpriteHookHead = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HOOK_HEAD]);
		m_GameSkin.m_SpriteWeaponHammer = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_HAMMER_BODY]);
		m_GameSkin.m_SpriteWeaponGun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_BODY]);
		m_GameSkin.m_SpriteWeaponShotgun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_BODY]);
		m_GameSkin.m_SpriteWeaponGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_BODY]);
		m_GameSkin.m_SpriteWeaponNinja = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_BODY]);
		m_GameSkin.m_SpriteWeaponLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_BODY]);

		m_GameSkin.m_aSpriteWeapons[0] = m_GameSkin.m_SpriteWeaponHammer;
		m_GameSkin.m_aSpriteWeapons[1] = m_GameSkin.m_SpriteWeaponGun;
		m_GameSkin.m_aSpriteWeapons[2] = m_GameSkin.m_SpriteWeaponShotgun;
		m_GameSkin.m_aSpriteWeapons[3] = m_GameSkin.m_SpriteWeaponGrenade;
		m_GameSkin.m_aSpriteWeapons[4] = m_GameSkin.m_SpriteWeaponLaser;
		m_GameSkin.m_aSpriteWeapons[5] = m_GameSkin.m_SpriteWeaponNinja;

		// particles
		for(int i = 0; i < 9; ++i)
		{
			m_GameSkin.m_aSpriteParticles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART1 + i]);
		}

		// stars
		for(int i = 0; i < 3; ++i)
		{
			m_GameSkin.m_aSpriteStars[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_STAR1 + i]);
		}

		// projectiles
		m_GameSkin.m_SpriteWeaponGunProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_PROJ]);
		m_GameSkin.m_SpriteWeaponShotgunProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_PROJ]);
		m_GameSkin.m_SpriteWeaponGrenadeProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GRENADE_PROJ]);

		// these weapons have no projectiles
		m_GameSkin.m_SpriteWeaponHammerProjectile = IGraphics::CTextureHandle();
		m_GameSkin.m_SpriteWeaponNinjaProjectile = IGraphics::CTextureHandle();

		m_GameSkin.m_SpriteWeaponLaserProjectile = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_LASER_PROJ]);

		m_GameSkin.m_aSpriteWeaponProjectiles[0] = m_GameSkin.m_SpriteWeaponHammerProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[1] = m_GameSkin.m_SpriteWeaponGunProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[2] = m_GameSkin.m_SpriteWeaponShotgunProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[3] = m_GameSkin.m_SpriteWeaponGrenadeProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[4] = m_GameSkin.m_SpriteWeaponLaserProjectile;
		m_GameSkin.m_aSpriteWeaponProjectiles[5] = m_GameSkin.m_SpriteWeaponNinjaProjectile;

		// muzzles
		for(int i = 0; i < 3; ++i)
		{
			m_GameSkin.m_aSpriteWeaponGunMuzzles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_GUN_MUZZLE1 + i]);
			m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_SHOTGUN_MUZZLE1 + i]);
			m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_WEAPON_NINJA_MUZZLE1 + i]);

			m_GameSkin.m_aaSpriteWeaponsMuzzles[1][i] = m_GameSkin.m_aSpriteWeaponGunMuzzles[i];
			m_GameSkin.m_aaSpriteWeaponsMuzzles[2][i] = m_GameSkin.m_aSpriteWeaponShotgunMuzzles[i];
			m_GameSkin.m_aaSpriteWeaponsMuzzles[5][i] = m_GameSkin.m_aaSpriteWeaponNinjaMuzzles[i];
		}

		// pickups
		m_GameSkin.m_SpritePickupHealth = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_HEALTH]);
		m_GameSkin.m_SpritePickupArmor = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR]);
		m_GameSkin.m_SpritePickupHammer = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_HAMMER]);
		m_GameSkin.m_SpritePickupGun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_GUN]);
		m_GameSkin.m_SpritePickupShotgun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_SHOTGUN]);
		m_GameSkin.m_SpritePickupGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_GRENADE]);
		m_GameSkin.m_SpritePickupLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_LASER]);
		m_GameSkin.m_SpritePickupNinja = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_NINJA]);
		m_GameSkin.m_SpritePickupArmorShotgun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_SHOTGUN]);
		m_GameSkin.m_SpritePickupArmorGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_GRENADE]);
		m_GameSkin.m_SpritePickupArmorNinja = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_NINJA]);
		m_GameSkin.m_SpritePickupArmorLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PICKUP_ARMOR_LASER]);

		m_GameSkin.m_aSpritePickupWeapons[0] = m_GameSkin.m_SpritePickupHammer;
		m_GameSkin.m_aSpritePickupWeapons[1] = m_GameSkin.m_SpritePickupGun;
		m_GameSkin.m_aSpritePickupWeapons[2] = m_GameSkin.m_SpritePickupShotgun;
		m_GameSkin.m_aSpritePickupWeapons[3] = m_GameSkin.m_SpritePickupGrenade;
		m_GameSkin.m_aSpritePickupWeapons[4] = m_GameSkin.m_SpritePickupLaser;
		m_GameSkin.m_aSpritePickupWeapons[5] = m_GameSkin.m_SpritePickupNinja;

		m_GameSkin.m_aSpritePickupWeaponArmor[0] = m_GameSkin.m_SpritePickupArmorShotgun;
		m_GameSkin.m_aSpritePickupWeaponArmor[1] = m_GameSkin.m_SpritePickupArmorGrenade;
		m_GameSkin.m_aSpritePickupWeaponArmor[2] = m_GameSkin.m_SpritePickupArmorNinja;
		m_GameSkin.m_aSpritePickupWeaponArmor[3] = m_GameSkin.m_SpritePickupArmorLaser;

		// flags
		m_GameSkin.m_SpriteFlagBlue = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_FLAG_BLUE]);
		m_GameSkin.m_SpriteFlagRed = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_FLAG_RED]);

		// ninja bar (0.7)
		if(!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]) ||
			!Graphics()->IsSpriteTextureFullyTransparent(ImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]))
		{
			m_GameSkin.m_SpriteNinjaBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL_LEFT]);
			m_GameSkin.m_SpriteNinjaBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_FULL]);
			m_GameSkin.m_SpriteNinjaBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY]);
			m_GameSkin.m_SpriteNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &client_data7::g_pData->m_aSprites[client_data7::SPRITE_NINJA_BAR_EMPTY_RIGHT]);
		}

		m_GameSkinLoaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::LoadEmoticonsSkin(const char *pPath, bool AsDir)
{
	if(m_EmoticonsSkinLoaded)
	{
		for(auto &SpriteEmoticon : m_EmoticonsSkin.m_aSpriteEmoticons)
			Graphics()->UnloadTexture(&SpriteEmoticon);

		m_EmoticonsSkinLoaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_EMOTICONS, "emoticons");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			LoadEmoticonsSkin("default");
		else
			LoadEmoticonsSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_OOP].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
	{
		for(int i = 0; i < 16; ++i)
			m_EmoticonsSkin.m_aSpriteEmoticons[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_OOP + i]);

		m_EmoticonsSkinLoaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::LoadParticlesSkin(const char *pPath, bool AsDir)
{
	if(m_ParticlesSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleSlice);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleBall);
		for(auto &SpriteParticleSplat : m_ParticlesSkin.m_aSpriteParticleSplat)
			Graphics()->UnloadTexture(&SpriteParticleSplat);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleSmoke);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleShell);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleExpl);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleAirJump);
		Graphics()->UnloadTexture(&m_ParticlesSkin.m_SpriteParticleHit);

		for(auto &SpriteParticle : m_ParticlesSkin.m_aSpriteParticles)
			SpriteParticle = IGraphics::CTextureHandle();

		m_ParticlesSkinLoaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_PARTICLES, "particles");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			LoadParticlesSkin("default");
		else
			LoadParticlesSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_PART_SLICE].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
	{
		m_ParticlesSkin.m_SpriteParticleSlice = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SLICE]);
		m_ParticlesSkin.m_SpriteParticleBall = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_BALL]);
		for(int i = 0; i < 3; ++i)
			m_ParticlesSkin.m_aSpriteParticleSplat[i] = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SPLAT01 + i]);
		m_ParticlesSkin.m_SpriteParticleSmoke = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SMOKE]);
		m_ParticlesSkin.m_SpriteParticleShell = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SHELL]);
		m_ParticlesSkin.m_SpriteParticleExpl = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_EXPL01]);
		m_ParticlesSkin.m_SpriteParticleAirJump = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_AIRJUMP]);
		m_ParticlesSkin.m_SpriteParticleHit = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_HIT01]);

		m_ParticlesSkin.m_aSpriteParticles[0] = m_ParticlesSkin.m_SpriteParticleSlice;
		m_ParticlesSkin.m_aSpriteParticles[1] = m_ParticlesSkin.m_SpriteParticleBall;
		for(int i = 0; i < 3; ++i)
			m_ParticlesSkin.m_aSpriteParticles[2 + i] = m_ParticlesSkin.m_aSpriteParticleSplat[i];
		m_ParticlesSkin.m_aSpriteParticles[5] = m_ParticlesSkin.m_SpriteParticleSmoke;
		m_ParticlesSkin.m_aSpriteParticles[6] = m_ParticlesSkin.m_SpriteParticleShell;
		m_ParticlesSkin.m_aSpriteParticles[7] = m_ParticlesSkin.m_SpriteParticleExpl;
		m_ParticlesSkin.m_aSpriteParticles[8] = m_ParticlesSkin.m_SpriteParticleAirJump;
		m_ParticlesSkin.m_aSpriteParticles[9] = m_ParticlesSkin.m_SpriteParticleHit;

		m_ParticlesSkinLoaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::LoadHudSkin(const char *pPath, bool AsDir)
{
	if(m_HudSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudAirjump);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudAirjumpEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudSolo);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudCollisionDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudEndlessJump);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudEndlessHook);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudJetpack);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarFullLeft);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarFull);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudFreezeBarEmptyRight);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarFullLeft);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarFull);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarEmpty);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudNinjaBarEmptyRight);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudHookHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudHammerHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudShotgunHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudGrenadeHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLaserHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudGunHitDisabled);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDeepFrozen);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLiveFrozen);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportGrenade);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportGun);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeleportLaser);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudPracticeMode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudLockMode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudTeam0Mode);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDummyHammer);
		Graphics()->UnloadTexture(&m_HudSkin.m_SpriteHudDummyCopy);
		m_HudSkinLoaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_HUD, "hud");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			LoadHudSkin("default");
		else
			LoadHudSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_HUD_AIRJUMP].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
	{
		m_HudSkin.m_SpriteHudAirjump = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_AIRJUMP]);
		m_HudSkin.m_SpriteHudAirjumpEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_AIRJUMP_EMPTY]);
		m_HudSkin.m_SpriteHudSolo = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_SOLO]);
		m_HudSkin.m_SpriteHudCollisionDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_COLLISION_DISABLED]);
		m_HudSkin.m_SpriteHudEndlessJump = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_ENDLESS_JUMP]);
		m_HudSkin.m_SpriteHudEndlessHook = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_ENDLESS_HOOK]);
		m_HudSkin.m_SpriteHudJetpack = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_JETPACK]);
		m_HudSkin.m_SpriteHudFreezeBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_FULL_LEFT]);
		m_HudSkin.m_SpriteHudFreezeBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_FULL]);
		m_HudSkin.m_SpriteHudFreezeBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_EMPTY]);
		m_HudSkin.m_SpriteHudFreezeBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_FREEZE_BAR_EMPTY_RIGHT]);
		m_HudSkin.m_SpriteHudNinjaBarFullLeft = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_FULL_LEFT]);
		m_HudSkin.m_SpriteHudNinjaBarFull = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_FULL]);
		m_HudSkin.m_SpriteHudNinjaBarEmpty = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_EMPTY]);
		m_HudSkin.m_SpriteHudNinjaBarEmptyRight = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_NINJA_BAR_EMPTY_RIGHT]);
		m_HudSkin.m_SpriteHudHookHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_HOOK_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudHammerHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_HAMMER_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudShotgunHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_SHOTGUN_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudGrenadeHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_GRENADE_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudLaserHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LASER_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudGunHitDisabled = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_GUN_HIT_DISABLED]);
		m_HudSkin.m_SpriteHudDeepFrozen = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DEEP_FROZEN]);
		m_HudSkin.m_SpriteHudLiveFrozen = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LIVE_FROZEN]);
		m_HudSkin.m_SpriteHudTeleportGrenade = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_GRENADE]);
		m_HudSkin.m_SpriteHudTeleportGun = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_GUN]);
		m_HudSkin.m_SpriteHudTeleportLaser = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TELEPORT_LASER]);
		m_HudSkin.m_SpriteHudPracticeMode = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_PRACTICE_MODE]);
		m_HudSkin.m_SpriteHudLockMode = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_LOCK_MODE]);
		m_HudSkin.m_SpriteHudTeam0Mode = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_TEAM0_MODE]);
		m_HudSkin.m_SpriteHudDummyHammer = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DUMMY_HAMMER]);
		m_HudSkin.m_SpriteHudDummyCopy = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_HUD_DUMMY_COPY]);

		m_HudSkinLoaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
}

void CGameClient::LoadExtrasSkin(const char *pPath, bool AsDir)
{
	if(m_ExtrasSkinLoaded)
	{
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSnowflake);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteParticleSparkle);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpritePulley);
		Graphics()->UnloadTexture(&m_ExtrasSkin.m_SpriteHectagon);

		for(auto &SpriteParticle : m_ExtrasSkin.m_aSpriteParticles)
			SpriteParticle = IGraphics::CTextureHandle();

		m_ExtrasSkinLoaded = false;
	}

	CImageAsset LoadedAsset = LoadAssetFromPath(pPath, AsDir, IMAGE_EXTRAS, "extras");
	CImageInfo &ImgInfo = LoadedAsset.m_ImageInfo;
	std::optional<CImageInfo> &FallbackImgInfo = LoadedAsset.m_FallbackImageInfo;
	if(!LoadedAsset.IsLoaded() && !LoadedAsset.m_IsDefault)
	{
		if(AsDir)
			LoadExtrasSkin("default");
		else
			LoadExtrasSkin(pPath, true);
	}
	else if(LoadedAsset.IsLoaded() && Graphics()->CheckImageDivisibility(LoadedAsset.m_aPath, ImgInfo, g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridx, g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE].m_pSet->m_Gridy, true) && Graphics()->IsImageFormatRgba(LoadedAsset.m_aPath, ImgInfo))
	{
		m_ExtrasSkin.m_SpriteParticleSnowflake = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SNOWFLAKE]);
		m_ExtrasSkin.m_SpriteParticleSparkle = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_SPARKLE]);
		m_ExtrasSkin.m_SpritePulley = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_PULLEY]);
		m_ExtrasSkin.m_SpriteHectagon = Graphics()->LoadSpriteTexture(ImgInfo, FallbackImgInfo, &g_pData->m_aSprites[SPRITE_PART_HECTAGON]);

		m_ExtrasSkin.m_aSpriteParticles[0] = m_ExtrasSkin.m_SpriteParticleSnowflake;
		m_ExtrasSkin.m_aSpriteParticles[1] = m_ExtrasSkin.m_SpriteParticleSparkle;
		m_ExtrasSkin.m_aSpriteParticles[2] = m_ExtrasSkin.m_SpritePulley;
		m_ExtrasSkin.m_aSpriteParticles[3] = m_ExtrasSkin.m_SpriteHectagon;

		m_ExtrasSkinLoaded = true;
	}
	ImgInfo.Free();
	if(FallbackImgInfo.has_value())
		FallbackImgInfo.value().Free();
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
			m_Menus.RenderLoading(Localize("Loading skin files"), "", 0);
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

	for(std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo : m_vpManagedTeeRenderInfos)
	{
		if(!(pManagedTeeRenderInfo->SkinDescriptor().m_Flags & CSkinDescriptor::FLAG_SIX) ||
			!NameMatches(pManagedTeeRenderInfo->SkinDescriptor().m_aSkinName))
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
	return CreateManagedTeeRenderInfo(Client.m_RenderInfo, Client.ToSkinDescriptor(GameState(ActiveConnection())));
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
	if(pResult->NumArguments() && pThis->m_Menus.IsInit())
	{
		pThis->RefreshSkins(CSkinDescriptor::FLAG_SIX);
	}
}

void CGameClient::ConchainRefreshEventSkins(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameClient *pThis = static_cast<CGameClient *>(pUserData);
	pfnCallback(pResult, pCallbackUserData);
	if(pResult->NumArguments() && pThis->m_Menus.IsInit())
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

void CGameClient::ConchainMenuMap(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameClient *pSelf = (CGameClient *)pUserData;
	if(pResult->NumArguments())
	{
		if(str_comp(g_Config.m_ClMenuMap, pResult->GetString(0)) != 0)
		{
			str_copy(g_Config.m_ClMenuMap, pResult->GetString(0));
			pSelf->m_MenuBackground.LoadMenuBackground();
		}
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

void CGameClient::DummyResetInput()
{
	if(!Client()->DummyConnected())
		return;

	CGameSessionContext *pSession = FindSessionContext(Client()->NetworkSessionId());
	dbg_assert(pSession != nullptr, "missing Network game session context");
	const int Conn = Client()->ActiveConnection() == IClient::CONN_MAIN ? IClient::CONN_DUMMY : IClient::CONN_MAIN;
	CGameState *pState = pSession->GameStates().FindByStream(CStreamId(Conn + 1));
	dbg_assert(pState != nullptr, "missing Network game state");
	CNetObj_PlayerInput &Input = pState->Input().m_InputData;
	int Fire = Input.m_Fire;
	if((Fire & 1) != 0)
		Fire++;

	pState->Input().ReleaseGameplay();
	Input.m_Hook = 0;
	Input.m_Fire = Fire;
}

bool CGameClient::CanDisplayWarning() const
{
	return m_Menus.CanDisplayWarning();
}

CNetObjHandler *CGameClient::GetNetObjHandler()
{
	return &m_NetObjHandler;
}

protocol7::CNetObjHandler *CGameClient::GetNetObjHandler7()
{
	return &m_NetObjHandler7;
}

void CGameClient::SnapCollectEntities(CSessionId SessionId, int Conn)
{
	const int NumSnapItems = Client()->SnapNumItems(SessionId, Conn, IClient::SNAP_CURRENT);

	std::vector<CSnapEntities> vItemData;
	std::vector<CSnapEntities> vItemEx;

	for(int Index = 0; Index < NumSnapItems; Index++)
	{
		const IClient::CSnapItem Item = Client()->SnapGetItem(SessionId, Conn, IClient::SNAP_CURRENT, Index);
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

void CGameClient::HandleMultiView(const CGameState &State)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	bool IsTeamZero = IsMultiViewIdSet();
	bool Init = false;
	vec2 MinPos, MaxPos;
	float SumVel = 0.0f;
	int AmountPlayers = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CNetObj_DDNetCharacter *pExtended = State.ExtendedCharacter(ClientId);
		const bool Frozen = pExtended != nullptr && pExtended->m_FreezeEnd != 0;
		// look at players who are vanished
		if(MultiViewState.m_aVanish[ClientId])
		{
			// not in freeze anymore and the delay is over
			if(MultiViewState.m_aLastFreeze[ClientId] + 6.0f <= Client()->LocalTime() && !Frozen)
			{
				MultiViewState.m_aVanish[ClientId] = false;
				MultiViewState.m_aLastFreeze[ClientId] = 0.0f;
			}
		}

		// we look at team 0 and the player is not in the spec list
		if(IsTeamZero && !MultiViewState.m_aSelected[ClientId])
			continue;

		// player is vanished
		if(MultiViewState.m_aVanish[ClientId])
			continue;

		// the player is not in the team we are spectating
		if(State.Teams().Team(ClientId) != MultiViewState.m_Team)
			continue;

		vec2 PlayerPos;
		if(State.RenderedClient(ClientId).m_Active)
			PlayerPos = State.RenderedClient(ClientId).m_Position;
		else if(const CGameState::CClientSnapshot &Client = State.Client(ClientId);
			Client.m_HasDDNetPlayer && (Client.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0 && Client.m_HasSpecChar)
			PlayerPos = vec2(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
		else
			continue;

		// player is far away and frozen
		if(distance(MultiViewState.m_OldPos, PlayerPos) > 1100 && Frozen)
		{
			// check if the player is frozen for more than 3 seconds, if so vanish them
			if(MultiViewState.m_aLastFreeze[ClientId] == 0.0f)
			{
				MultiViewState.m_aLastFreeze[ClientId] = Client()->LocalTime();
			}
			else if(MultiViewState.m_aLastFreeze[ClientId] + 3.0f <= Client()->LocalTime())
			{
				MultiViewState.m_aVanish[ClientId] = true;
				// player we want to be vanished is our "main" tee, so lets switch the tee
				if(ClientId == m_Snap.m_SpecInfo.m_SpectatorId)
					m_Spectator.Spectate(FindFirstMultiViewId());
			}
		}
		else if(MultiViewState.m_aLastFreeze[ClientId] != 0)
		{
			MultiViewState.m_aLastFreeze[ClientId] = 0;
		}

		// set the minimum and maximum position
		if(!Init)
		{
			MinPos = PlayerPos;
			MaxPos = PlayerPos;
			Init = true;
		}
		else
		{
			MinPos.x = std::min(MinPos.x, PlayerPos.x);
			MaxPos.x = std::max(MaxPos.x, PlayerPos.x);
			MinPos.y = std::min(MinPos.y, PlayerPos.y);
			MaxPos.y = std::max(MaxPos.y, PlayerPos.y);
		}

		// sum up the velocity of all players we are spectating
		const CNetObj_Character &CurrentCharacter = State.RenderedClient(ClientId).m_Cur;
		SumVel += length(vec2(CurrentCharacter.m_VelX / 256.0f, CurrentCharacter.m_VelY / 256.0f)) * 50.0f / 32.0f;
		AmountPlayers++;
	}

	// if we have found no players, we disable multi view
	if(AmountPlayers == 0)
	{
		if(MultiViewState.m_SecondChance == 0.0f)
		{
			MultiViewState.m_SecondChance = Client()->LocalTime() + 0.3f;
		}
		else if(MultiViewState.m_SecondChance < Client()->LocalTime())
		{
			ResetMultiView();
		}
		return;
	}
	else if(MultiViewState.m_SecondChance != 0.0f)
	{
		MultiViewState.m_SecondChance = 0.0f;
	}

	// if we only have one tee that's in the list, we activate solo-mode
	MultiViewState.m_Solo = std::count(std::begin(MultiViewState.m_aSelected), std::end(MultiViewState.m_aSelected), true) == 1;

	vec2 TargetPos = vec2((MinPos.x + MaxPos.x) / 2.0f, (MinPos.y + MaxPos.y) / 2.0f);
	// dont hide the position hud if its only one player
	MultiViewState.m_ShowHud = AmountPlayers == 1;
	// get the average velocity
	float AvgVel = std::clamp(SumVel / AmountPlayers, 0.0f, 1000.0f);

	if(MultiViewState.m_OldPersonalZoom == MultiViewState.m_PersonalZoom)
		m_Camera.SetZoom(CalculateMultiViewZoom(MinPos, MaxPos, AvgVel), g_Config.m_ClMultiViewZoomSmoothness, false);
	else
		m_Camera.SetZoom(CalculateMultiViewZoom(MinPos, MaxPos, AvgVel), 50, false);

	m_Snap.m_SpecInfo.m_Position = MultiViewState.m_OldPos + ((TargetPos - MultiViewState.m_OldPos) * CalculateMultiViewMultiplier(TargetPos));
	MultiViewState.m_OldPos = m_Snap.m_SpecInfo.m_Position;
	m_Snap.m_SpecInfo.m_UsePosition = true;
}

bool CGameClient::InitMultiView(const CGameState &State, int Team)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	float Width, Height;
	CleanMultiViewIds();
	MultiViewState.m_IsInit = true;

	// get the current view coordinates
	Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), m_Camera.Zoom(), &Width, &Height);
	vec2 AxisX = vec2(m_Camera.Center().x - (Width / 2.0f), m_Camera.Center().x + (Width / 2.0f));
	vec2 AxisY = vec2(m_Camera.Center().y - (Height / 2.0f), m_Camera.Center().y + (Height / 2.0f));

	if(Team > 0)
	{
		MultiViewState.m_Team = Team;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			MultiViewState.m_aSelected[ClientId] = State.Teams().Team(ClientId) == Team;
	}
	else
	{
		// we want to allow spectating players in teams directly if there is no other team on screen
		// to do that, -1 is used temporarily for "we don't know which team to spectate yet"
		MultiViewState.m_Team = -1;

		int Count = 0;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			vec2 PlayerPos;

			// get the position of the player
			if(State.RenderedClient(ClientId).m_Active)
				PlayerPos = State.RenderedClient(ClientId).m_Position;
			else if(const CGameState::CClientSnapshot &Client = State.Client(ClientId);
				Client.m_HasDDNetPlayer && (Client.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0 && Client.m_HasSpecChar)
				PlayerPos = vec2(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
			else
				continue;

			if(PlayerPos.x == 0 || PlayerPos.y == 0)
				continue;

			// skip players that aren't in view
			if(PlayerPos.x <= AxisX.x || PlayerPos.x >= AxisX.y || PlayerPos.y <= AxisY.x || PlayerPos.y >= AxisY.y)
				continue;

			if(MultiViewState.m_Team == -1)
			{
				// use the current player's team for now, but it might switch to team 0 if any other team is found
				MultiViewState.m_Team = State.Teams().Team(ClientId);
			}
			else if(MultiViewState.m_Team != 0 && State.Teams().Team(ClientId) != MultiViewState.m_Team)
			{
				// mismatched teams; remove all previously added players again and switch to team 0 instead
				std::fill_n(MultiViewState.m_aSelected, ClientId, false);
				MultiViewState.m_Team = 0;
			}

			MultiViewState.m_aSelected[ClientId] = true;
			Count++;
		}

		// might still be -1 if not a single player was in view; fallback to team 0 in that case
		if(MultiViewState.m_Team == -1)
			MultiViewState.m_Team = 0;

		// we are spectating only one player
		MultiViewState.m_Solo = Count == 1;
	}

	if(IsMultiViewIdSet())
	{
		int SpectatorId = m_Snap.m_SpecInfo.m_SpectatorId;
		int NewSpectatorId = -1;

		vec2 CurPosition(m_Camera.Center());
		if(SpectatorId != SPEC_FREEVIEW)
		{
			CurPosition = State.RenderedClient(SpectatorId).m_Position;
		}

		int ClosestDistance = std::numeric_limits<int>::max();
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
			if(!SnapshotClient.m_HasPlayerInfo || SnapshotClient.m_PlayerInfo.m_Team == TEAM_SPECTATORS || State.Teams().Team(ClientId) != MultiViewState.m_Team)
				continue;

			vec2 PlayerPos;
			if(State.RenderedClient(ClientId).m_Active)
				PlayerPos = State.RenderedClient(ClientId).m_Position;
			else if(const CGameState::CClientSnapshot &Client = State.Client(ClientId);
				Client.m_HasDDNetPlayer && (Client.m_DDNetPlayer.m_Flags & EXPLAYERFLAG_SPEC) != 0 && Client.m_HasSpecChar)
				PlayerPos = vec2(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
			else
				continue;

			int Distance = distance(CurPosition, PlayerPos);
			if(NewSpectatorId == -1 || Distance < ClosestDistance)
			{
				NewSpectatorId = ClientId;
				ClosestDistance = Distance;
			}
		}

		if(NewSpectatorId > -1)
			m_Spectator.Spectate(NewSpectatorId);
	}

	return IsMultiViewIdSet();
}

float CGameClient::CalculateMultiViewMultiplier(vec2 TargetPos)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	float MaxCameraDist = 200.0f;
	float MinCameraDist = 20.0f;
	float MaxVel = g_Config.m_ClMultiViewSensitivity / 150.0f;
	float MinVel = 0.007f;
	float CurrentCameraDistance = distance(MultiViewState.m_OldPos, TargetPos);
	float UpperLimit = 1.0f;

	if(MultiViewState.m_Teleported && CurrentCameraDistance <= 100.0f)
		MultiViewState.m_Teleported = false;

	// somebody got teleported very likely
	if((MultiViewState.m_Teleported || CurrentCameraDistance - MultiViewState.m_OldCameraDistance > 100.0f) && MultiViewState.m_OldCameraDistance != 0.0f)
	{
		UpperLimit = 0.1f; // dont try to compensate it by flickering
		MultiViewState.m_Teleported = true;
	}
	MultiViewState.m_OldCameraDistance = CurrentCameraDistance;

	return std::clamp(MapValue(MaxCameraDist, MinCameraDist, MaxVel, MinVel, CurrentCameraDistance), MinVel, UpperLimit);
}

float CGameClient::CalculateMultiViewZoom(vec2 MinPos, vec2 MaxPos, float Vel)
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	float Ratio = Graphics()->ScreenAspect();
	float ZoomX = 0.0f, ZoomY;

	// only calc two axis if the aspect ratio is not 1:1
	if(Ratio != 1.0f)
		ZoomX = (0.001309f - 0.000328f * Ratio) * (MaxPos.x - MinPos.x) + (0.741413f - 0.032959f * Ratio);

	// calculate the according zoom with linear function
	ZoomY = 0.001309f * (MaxPos.y - MinPos.y) + 0.741413f;
	// choose the highest zoom
	float Zoom = std::max(ZoomX, ZoomY);
	// zoom out to maximum 10 percent of the current zoom for 70 velocity
	float Diff = std::clamp(MapValue(70.0f, 15.0f, Zoom * 0.10f, 0.0f, Vel), 0.0f, Zoom * 0.10f);
	// zoom should stay between 1.1 and 20.0
	Zoom = std::clamp(Zoom + Diff, 1.1f, 20.0f);
	// dont go below default zoom
	Zoom = std::max(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), Zoom);
	// add the user preference
	Zoom -= Zoom * 0.1f * MultiViewState.m_PersonalZoom;
	MultiViewState.m_OldPersonalZoom = MultiViewState.m_PersonalZoom;

	return Zoom;
}

float CGameClient::MapValue(float MaxValue, float MinValue, float MaxRange, float MinRange, float Value)
{
	return (MaxRange - MinRange) / (MaxValue - MinValue) * (Value - MinValue) + MinRange;
}

void CGameClient::ResetMultiView()
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	m_Camera.SetZoom(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), g_Config.m_ClSmoothZoomTime, true);
	MultiViewState.m_PersonalZoom = 0.0f;
	MultiViewState.m_Active = false;
	MultiViewState.m_Solo = false;
	MultiViewState.m_IsInit = false;
	MultiViewState.m_Teleported = false;
	MultiViewState.m_OldCameraDistance = 0.0f;
}

void CGameClient::CleanMultiViewIds()
{
	CGameView::CMultiViewState &MultiViewState = MultiView();
	std::fill(std::begin(MultiViewState.m_aSelected), std::end(MultiViewState.m_aSelected), false);
	std::fill(std::begin(MultiViewState.m_aLastFreeze), std::end(MultiViewState.m_aLastFreeze), 0.0f);
	std::fill(std::begin(MultiViewState.m_aVanish), std::end(MultiViewState.m_aVanish), false);
}

void CGameClient::CleanMultiViewId(int ClientId)
{
	if(ClientId >= MAX_CLIENTS || ClientId < 0)
		return;

	CGameView::CMultiViewState &MultiViewState = MultiView();
	MultiViewState.m_aSelected[ClientId] = false;
	MultiViewState.m_aLastFreeze[ClientId] = 0.0f;
	MultiViewState.m_aVanish[ClientId] = false;
}

bool CGameClient::IsMultiViewIdSet()
{
	const CGameView::CMultiViewState &MultiViewState = MultiView();
	return std::any_of(std::begin(MultiViewState.m_aSelected), std::end(MultiViewState.m_aSelected), [](bool IsSet) { return IsSet; });
}

int CGameClient::FindFirstMultiViewId()
{
	int ClientId = -1;
	const CGameView::CMultiViewState &MultiViewState = MultiView();
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(MultiViewState.m_aSelected[i] && !MultiViewState.m_aVanish[i])
			return i;
	}
	return ClientId;
}

void CGameClient::OnSaveCodeNetMessage(CGameSessionContext &Session, const CGameState &GameState, const CNetMsg_Sv_SaveCode *pMsg)
{
	char aBuf[512];
	auto AddLine = [&](const char *pText) {
		m_Chat.AddLine(Session, GameState, SessionMessageTime(Session.Id()), Session.Id() == Client()->DemoSessionId(), Session.Id() == Client()->FocusedSessionId(), -1, TEAM_ALL, pText);
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

	if(State != SAVESTATE_PENDING && State != SAVESTATE_ERROR && Session.Id() != Client()->DemoSessionId())
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
