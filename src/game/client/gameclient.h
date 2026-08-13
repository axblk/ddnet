/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_GAMECLIENT_H
#define GAME_CLIENT_GAMECLIENT_H

#include "game_state.h"
#include "game_view.h"
#include "input_policy.h"
#include "local_player_profile.h"
#include "map_context.h"
#include "render.h"
#include "session_context.h"
#include "session_presentation.h"

#include <base/color.h>
#include <base/types.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/client/enums.h>
#include <engine/console.h>
#include <engine/shared/config.h>
#include <engine/shared/snapshot.h>

#include <generated/protocol7.h>
#include <generated/protocolglue.h>

#include <game/client/prediction/gameworld.h>
#include <game/client/race.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/layers.h>
#include <game/map/render_map.h>
#include <game/mapbugs.h>
#include <game/teamscore.h>

// components
#include "components/background.h"
#include "components/binds.h"
#include "components/broadcast.h"
#include "components/camera.h"
#include "components/censor.h"
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
#include "components/important_alert.h"
#include "components/infomessages.h"
#include "components/items.h"
#include "components/key_binder.h"
#include "components/local_server.h"
#include "components/mapimages.h"
#include "components/maplayers.h"
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
#include "components/tooltips.h"
#include "components/touch_controls.h"
#include "components/voting.h"

#include <array>
#include <memory>
#include <optional>
#include <vector>

class IMap;

class CSnapEntities
{
public:
	IClient::CSnapItem m_Item;
	const CNetObj_EntityEx *m_pDataEx;
};

enum class EClientIdFormat
{
	NO_INDENT,
	INDENT_AUTO,
	INDENT_FORCE, // for rendering settings preview
};

class CGameClient : public IGameClient
{
public:
	// all components
	CInfoMessages m_InfoMessages;
	CCamera m_Camera;
	CChat m_Chat;
	CCensor m_Censor;
	CMotd m_Motd;
	CBroadcast m_Broadcast;
	CGameConsole m_GameConsole;
	CBinds m_Binds;
	CKeyBinder m_KeyBinder;
	CParticles m_Particles;
	CMenus m_Menus;
	CSkins m_Skins;
	CSkins7 m_Skins7;
	CCountryFlags m_CountryFlags;
	CHud m_Hud;
	CImportantAlert m_ImportantAlert;
	CDebugHud m_DebugHud;
	CControls m_Controls;
	CEffects m_Effects;
	CScoreboard m_Scoreboard;
	CStatboard m_Statboard;
	CSounds m_Sounds;
	CEmoticon m_Emoticon;
	CDamageInd m_DamageInd;
	CTouchControls m_TouchControls;
	CVoting m_Voting;
	CSpectator m_Spectator;

	CPlayers m_Players;
	CNamePlates m_NamePlates;
	CFreezeBars m_FreezeBars;
	CItems m_Items;
	CMapImages m_MapImages;
	CSessionPresentationManager m_SessionPresentations{m_MapImages};
	CBackground m_Background;
	CMenuBackground m_MenuBackground;

	CRaceDemo m_RaceDemo;
	CGhost m_Ghost;

	CTooltips m_Tooltips;

	CLocalServer m_LocalServer;

private:
	std::vector<class CComponent *> m_vpAll;
	std::vector<class CComponent *> m_vpInput;
	CNetObjHandler m_NetObjHandler;
	protocol7::CNetObjHandler m_NetObjHandler7;

	class IEngine *m_pEngine;
	class IInput *m_pInput;
	class IGraphics *m_pGraphics;
	class ITextRender *m_pTextRender;
	class IClient *m_pClient;
	class ISound *m_pSound;
	class IConfigManager *m_pConfigManager;
	class CConfig *m_pConfig;
	class IConsole *m_pConsole;
	class IStorage *m_pStorage;
	class IDemoPlayer *m_pDemoPlayer;
	class IFavorites *m_pFavorites;
	class IServerBrowser *m_pServerBrowser;
	class IEditor *m_pEditor;
	class IFriends *m_pFriends;
	class IFriends *m_pFoes;
	class IDiscord *m_pDiscord;
#if defined(CONF_AUTOUPDATE)
	class IUpdater *m_pUpdater;
#endif
	class IHttp *m_pHttp;

	CGameSessionContextManager m_SessionContexts;
	CGameViewManager m_GameViews;
	CGameViewId m_LegacyGameViewId;
	CGameViewId m_SecondaryGameViewId;
	CGameViewId m_TertiaryGameViewId;
	float m_ControllerLocalTime = 0.0f;
	CUi m_UI;
	CRaceHelper m_RaceHelper;

	void ProcessEvents(CSessionId SessionId, int Conn);
	void ProcessSnapshot(CSessionId SessionId, int Conn);
	void ProcessPrediction();
	void UpdatePositions(const CGameState &State);
	void UpdateNetworkPlayerInfo();
	void BindLegacyWorld(CGameSessionContext &Session);
	void AddChatLine(CSessionId SessionId, int Conn, int ClientId, int Team, const char *pText);
	int64_t SessionMessageTime(CSessionId SessionId) const;
	const CLocalPlayerProfile &RefreshLegacyPlayerProfile(CSessionId SessionId, int Conn);

	int m_EditorMovementDelay = 5;
	void UpdateEditorIngameMoved();

	char m_aDDNetVersionStr[64];

	static void ConTeam(IConsole::IResult *pResult, void *pUserData);
	static void ConKill(IConsole::IResult *pResult, void *pUserData);
	static void ConReadyChange7(IConsole::IResult *pResult, void *pUserData);

	static void ConchainLanguageUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainSpecialInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainSpecialDummyInfoupdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainRefreshSkins(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainRefreshEventSkins(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);
	static void ConchainSpecialDummy(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

	static void ConTuneParam(IConsole::IResult *pResult, void *pUserData);
	static void ConTuneZone(IConsole::IResult *pResult, void *pUserData);
	static void ConMapbug(IConsole::IResult *pResult, void *pUserData);

	static void ConchainMenuMap(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

public:
	static std::function<bool(int, int, int, int)> GetScoreComparator(bool TimeScore, bool ReceivedMillisecondFinishTimes, bool Race7);

	IKernel *Kernel() { return IInterface::Kernel(); }
	IEngine *Engine() const { return m_pEngine; }
	class IGraphics *Graphics() const { return m_pGraphics; }
	class IClient *Client() const { return m_pClient; }
	int ActiveConnection() const { return Client()->FocusedSessionId() == Client()->DemoSessionId() ? IClient::CONN_MAIN : Client()->ActiveConnection(); }
	CGameSessionContext &SessionContext();
	const CGameSessionContext &SessionContext() const;
	CGameSessionContext *FindSessionContext(CSessionId SessionId) { return m_SessionContexts.Find(SessionId); }
	const CGameSessionContext *FindSessionContext(CSessionId SessionId) const { return m_SessionContexts.Find(SessionId); }
	CSessionPresentation &SessionPresentation(CSessionId SessionId);
	const CSessionPresentation &SessionPresentation(CSessionId SessionId) const;
	void ResetInfoMessages(CSessionId SessionId);
	void ResetChat(CSessionId SessionId);
	CMapContext &MapContext() { return SessionContext().MapContext(); }
	const CMapContext &MapContext() const { return SessionContext().MapContext(); }
	class CUi *Ui() { return &m_UI; }
	class ISound *Sound() const { return m_pSound; }
	class IInput *Input() const { return m_pInput; }
	class IStorage *Storage() const { return m_pStorage; }
	class IConfigManager *ConfigManager() const { return m_pConfigManager; }
	class CConfig *Config() const { return m_pConfig; }
	CGameState &GameState(int Conn);
	const CGameState &GameState(int Conn) const;
	CGameView &LegacyGameView();
	class IConsole *Console() { return m_pConsole; }
	class ITextRender *TextRender() const { return m_pTextRender; }
	class IDemoPlayer *DemoPlayer() const { return m_pDemoPlayer; }
	class IDemoRecorder *DemoRecorder(int Recorder) const { return Client()->DemoRecorder(Recorder); }
	class IFavorites *Favorites() const { return m_pFavorites; }
	class IServerBrowser *ServerBrowser() const { return m_pServerBrowser; }
	class CRenderTools *RenderTools() { return &m_RenderTools; }
	class CRenderMap *RenderMap() { return &m_RenderMap; }
	class CLayers *Layers() { return MapContext().Layers(); }
	CCollision *Collision() { return MapContext().Collision(); }
	const CCollision *Collision() const { return MapContext().Collision(); }
	const CSessionGameConfig *GameConfig() const { return &MapContext().GameConfig(); }
	const CRaceHelper *RaceHelper() const { return &m_RaceHelper; }
	class IEditor *Editor() { return m_pEditor; }
	class IFriends *Friends() { return m_pFriends; }
	class IFriends *Foes() { return m_pFoes; }
#if defined(CONF_AUTOUPDATE)
	class IUpdater *Updater()
	{
		return m_pUpdater;
	}
#endif
	class IHttp *Http()
	{
		return m_pHttp;
	}

	int NetobjNumCorrections()
	{
		return m_NetObjHandler.NumObjCorrections();
	}
	const char *NetobjCorrectedOn() { return m_NetObjHandler.CorrectedObjOn(); }

	bool m_SuppressEvents;
	bool m_NewTick;
	bool m_NewPredictedTick;

	int m_DemoSpecId;

	vec2 m_LocalCharacterPos;

	/**
	 * Our prediction for the local character at tick
	 * `IClient::PredGameTick(SessionId, Conn) - 1`.
	 */
	CCharacterCore m_PredictedPrevChar;
	/**
	 * Our prediction for the local character at tick
	 * `IClient::PredGameTick(SessionId, Conn)`.
	 */
	CCharacterCore m_PredictedChar;

	// snap pointers
	class CSnapState
	{
	public:
		const CNetObj_Character *m_pLocalCharacter;
		const CNetObj_Character *m_pLocalPrevCharacter;
		const CNetObj_PlayerInfo *m_pLocalInfo;
		const CNetObj_SpectatorInfo *m_pSpectatorInfo;
		const CNetObj_SpectatorInfo *m_pPrevSpectatorInfo;
		const CNetObj_SpectatorCount *m_pSpectatorCount;
		int m_NumFlags;
		const CNetObj_Flag *m_apFlags[CSnapshot::MAX_ITEMS];
		const CNetObj_Flag *m_apPrevFlags[CSnapshot::MAX_ITEMS];
		const CNetObj_GameInfo *m_pGameInfoObj;
		const CNetObj_GameData *m_pGameDataObj;
		const CNetObj_GameData *m_pPrevGameDataObj;

		const CNetObj_PlayerInfo *m_apPlayerInfos[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apPrevPlayerInfos[MAX_CLIENTS];

		const CNetObj_PlayerInfo *m_apInfoByScore[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apInfoByName[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apInfoByDDTeamScore[MAX_CLIENTS];
		const CNetObj_PlayerInfo *m_apInfoByDDTeamName[MAX_CLIENTS];

		int m_LocalClientId;
		int m_NumPlayers;
		int m_aTeamSize[2];
		int m_HighestClientId;

		class CSpectateInfo
		{
		public:
			bool m_Active;
			int m_SpectatorId;
			bool m_UsePosition;
			vec2 m_Position;

			bool m_HasCameraInfo;
			float m_Zoom;
			int m_Deadzone;
			int m_FollowFactor;
		};
		CSpectateInfo m_SpecInfo;

		class CCharacterInfo
		{
		public:
			bool m_Active;

			// snapshots
			CNetObj_Character m_Prev;
			CNetObj_Character m_Cur;

			CNetObj_DDNetCharacter m_ExtendedData;
			const CNetObj_DDNetCharacter *m_pPrevExtendedData;
			bool m_HasExtendedData;
			bool m_HasExtendedDisplayInfo;
		};
		CCharacterInfo m_aCharacters[MAX_CLIENTS];
	};

	CSnapState m_Snap;

	std::bitset<RECORDER_MAX> m_ActiveRecordings;

	// client data
	class CClientData
	{
		friend class CGameClient;
		CGameClient *m_pGameClient;
		int m_ClientId;

	public:
		int m_UseCustomColor;
		int m_ColorBody;
		int m_ColorFeet;

		char m_aName[MAX_NAME_LENGTH];
		char m_aClan[MAX_CLAN_LENGTH];
		/**
		 * Country code in ISO 3166-1 numeric.
		 */
		int m_Country;
		char m_aSkinName[MAX_SKIN_LENGTH];
		int m_Team;

		CCharacterCore m_Predicted;
		CCharacterCore m_PrevPredicted;

		std::shared_ptr<CManagedTeeRenderInfo> m_pSkinInfo = nullptr; // this is what the server reports
		CTeeRenderInfo m_RenderInfo; // this is what we use

		float m_Angle;
		bool m_Active;
		bool m_Friend;
		bool m_Foe;

		// Editor allows 256 switches for now.
		bool m_aSwitchStates[256];

		CNetObj_Character m_Snapped;
		CNetObj_Character m_Evolved;

		CNetMsg_Sv_PreInput m_aPreInputs[200];

		void UpdateSkinInfo(const CGameState &State);
		void UpdateSkin7HatSprite(const CGameState::CProtocol7ClientState &Protocol7Client);
		void UpdateSkin7BotDecoration(const CGameState::CProtocol7ClientState &Protocol7Client);
		void UpdateRenderInfo();
		void Reset();
		CSkinDescriptor ToSkinDescriptor(const CGameState &State) const;

		int ClientId() const { return m_ClientId; }
	};

	CClientData m_aClients[MAX_CLIENTS];

	CRenderTools m_RenderTools;
	CRenderMap m_RenderMap;

	bool m_BackButtonHandledKeyBind = false;

	size_t ComponentCount() const { return m_vpAll.size(); }

	// hooks
	void OnConnected(CSessionId SessionId) override;
	void OnSessionClosed(CSessionId SessionId) override;
	void OnSessionFocused(CSessionId SessionId) override;
	void OnRenderPrepare() override;
	void OnRender() override;
	void OnRenderFinalize() override;
	void OnUpdate() override;
	void OnDummyDisconnect() override;
	virtual void OnRelease();
	void OnInit() override;
	void OnConsoleInit() override;
	void OnStateChange(int NewState, int OldState) override;
	template<typename T>
	void ApplySkin7InfoFromGameMsg(CSessionId SessionId, const T *pMsg, int ClientId, CGameState &State);
	void ApplySkin7InfoFromSnapObj(CSessionId SessionId, const protocol7::CNetObj_De_ClientInfo *pObj, int ClientId, int Conn) override;
	int OnDemoRecSnap7(CSnapshot *pFrom, CSnapshotBuffer *pTo, int Conn) override;
	void *TranslateGameMsg(CSessionId SessionId, int *pMsgId, CUnpacker *pUnpacker, int Conn);
	int TranslateSnap(CSessionId SessionId, CSnapshotBuffer *pSnapDstSix, CSnapshot *pSnapSrcSeven, int Conn) override;
	void OnMessage(CSessionId SessionId, int MsgId, CUnpacker *pUnpacker, int Conn) override;
	void InvalidateSnapshot(CSessionId SessionId) override;
	void OnNewSnapshot(CSessionId SessionId, int Conn) override;
	void OnPredict(CSessionId SessionId, int Conn) override;
	void OnActivateEditor() override;
	void OnConnectionFocusChanged(CSessionId SessionId, int PreviousConn, int Conn) override;
	int OnSnapInput(CSessionId SessionId, int *pData, int Conn, bool Force) override;
	void OnShutdown() override;
	void OnEnterGame(CSessionId SessionId) override;
	void OnRconType(bool UsernameReq) override;
	void OnRconLine(const char *pLine) override;
	virtual void OnGameOver();
	virtual void OnStartGame();
	virtual void OnStartRound();
	void OnWindowResize() override;

	void InitializeLanguage() override;
	bool m_LanguageChanged = false;
	void OnLanguageChange();
	void HandleLanguageChanged();

	void ForceUpdateConsoleRemoteCompletionSuggestions() override;

	void RefreshSkin(const std::shared_ptr<CManagedTeeRenderInfo> &pManagedTeeRenderInfo);
	void RefreshSkins(int SkinDescriptorFlags);
	void OnSkinUpdate(const char *pSkinName);
	std::shared_ptr<CManagedTeeRenderInfo> CreateManagedTeeRenderInfo(const CTeeRenderInfo &TeeRenderInfo, const CSkinDescriptor &SkinDescriptor);
	std::shared_ptr<CManagedTeeRenderInfo> CreateManagedTeeRenderInfo(const CClientData &Client);
	void CollectManagedTeeRenderInfos(const std::function<void(const char *pSkinName)> &ActiveSkinAcceptor);

	void RenderShutdownMessage() override;
	void ProcessDemoSnapshot(CSnapshot *pSnap) override;

	const char *GetItemName(int Type) const override;
	const char *Version() const override;
	const char *NetVersion() const override;
	const char *NetVersion7() const override;
	int DDNetVersion() const override;
	const char *DDNetVersionStr() const override;
	int ClientVersion7() const override;

	void DoTeamChangeMessage7(CSessionId SessionId, int Conn, const CGameState &State, const char *pName, int ClientId, int Team, const char *pPrefix = "");

	// actions
	// TODO: move these
	void SendSwitchTeam(int Team) const;
	void SendStartInfo7(CSessionId SessionId, int Conn);
	void SendSkinChange7(CSessionId SessionId, int Conn);
	// Returns true if the requested skin change got applied by the server
	bool GotWantedSkin7(int Conn);
	void SendInfo(CSessionId SessionId, bool Start);
	void SendDummyInfo(bool Start) override;
	void SendKill() const;
	void SendReadyChange7(); // NOLINT(readability-make-member-function-const)

	void ApplyPreInputs(int Tick, bool Direct, CGameWorld &GameWorld);

	// DDRace

	const CTeamsCore &FocusedTeams() const { return GameState(ActiveConnection()).Teams(); }
	const CGameInfo &FocusedGameInfo() const { return GameState(ActiveConnection()).CoreGameInfo(); }

	int LastRaceTick() const;
	int CurrentRaceTime() const;
	int FlagDropTick(int Team) const;
	bool ReceivedDDNetPlayer() const;
	bool ReceivedDDNetPlayerFinishTimes() const;
	bool ReceivedDDNetPlayerFinishTimesMillis() const;

	bool IsTeamPlay() const;
	bool IsWorldPaused() const;
	bool IsDemoPlaybackPaused() const;
	float GetAnimationPlaybackSpeed() const;

	int AntiPingPlayers() const;
	bool AntiPingGrenade() const;
	bool AntiPingWeapons() const;
	bool AntiPingGunfire() const;
	bool Predict() const;
	bool PredictDummy(const CGameState &OtherState) const;

	const CTuningParams *GetTuning(int i) const { return &MapContext().TuningList()[i]; }
	ColorRGBA GetDDTeamColor(int DDTeam, float Lightness = 0.5f) const;
	void FormatClientId(int ClientId, char (&aClientId)[16], EClientIdFormat Format) const;
	void FormatClientId(int ClientId, char (&aClientId)[16], int HighestClientId) const;

	CGameWorld m_GameWorld;
	CGameWorld m_PredictedWorld;
	CGameWorld m_PrevPredictedWorld;

	std::vector<SSwitchers> &Switchers() { return m_GameWorld.m_Core.m_vSwitchers; }
	std::vector<SSwitchers> &PredSwitchers() { return m_PredictedWorld.m_Core.m_vSwitchers; }

	void DummyResetInput() override;
	void Echo(const char *pString) override;
	bool IsOtherTeam(int ClientId) const;
	int SwitchStateTeam() const;
	bool IsLocalCharSuper() const;
	bool CanDisplayWarning() const override;

	IMap *Map() override { return MapContext().Map(); }
	const IMap *Map() const override { return MapContext().Map(); }
	IMap *Map(CSessionId SessionId) override
	{
		CGameSessionContext *pSession = FindSessionContext(SessionId);
		dbg_assert(pSession != nullptr, "missing game session context");
		return pSession->MapContext().Map();
	}
	const IMap *Map(CSessionId SessionId) const override
	{
		const CGameSessionContext *pSession = FindSessionContext(SessionId);
		dbg_assert(pSession != nullptr, "missing game session context");
		return pSession->MapContext().Map();
	}
	CNetObjHandler *GetNetObjHandler() override;
	protocol7::CNetObjHandler *GetNetObjHandler7() override;

	void LoadGameSkin(const char *pPath, bool AsDir = false);
	void LoadEmoticonsSkin(const char *pPath, bool AsDir = false);
	void LoadParticlesSkin(const char *pPath, bool AsDir = false);
	void LoadHudSkin(const char *pPath, bool AsDir = false);
	void LoadExtrasSkin(const char *pPath, bool AsDir = false);

	struct SClientGameSkin
	{
		// health armor hud
		IGraphics::CTextureHandle m_SpriteHealthFull;
		IGraphics::CTextureHandle m_SpriteHealthEmpty;
		IGraphics::CTextureHandle m_SpriteArmorFull;
		IGraphics::CTextureHandle m_SpriteArmorEmpty;

		// cursors
		IGraphics::CTextureHandle m_SpriteWeaponHammerCursor;
		IGraphics::CTextureHandle m_SpriteWeaponGunCursor;
		IGraphics::CTextureHandle m_SpriteWeaponShotgunCursor;
		IGraphics::CTextureHandle m_SpriteWeaponGrenadeCursor;
		IGraphics::CTextureHandle m_SpriteWeaponNinjaCursor;
		IGraphics::CTextureHandle m_SpriteWeaponLaserCursor;

		IGraphics::CTextureHandle m_aSpriteWeaponCursors[6];

		// weapons and hook
		IGraphics::CTextureHandle m_SpriteHookChain;
		IGraphics::CTextureHandle m_SpriteHookHead;
		IGraphics::CTextureHandle m_SpriteWeaponHammer;
		IGraphics::CTextureHandle m_SpriteWeaponGun;
		IGraphics::CTextureHandle m_SpriteWeaponShotgun;
		IGraphics::CTextureHandle m_SpriteWeaponGrenade;
		IGraphics::CTextureHandle m_SpriteWeaponNinja;
		IGraphics::CTextureHandle m_SpriteWeaponLaser;

		IGraphics::CTextureHandle m_aSpriteWeapons[6];

		// particles
		IGraphics::CTextureHandle m_aSpriteParticles[9];

		// stars
		IGraphics::CTextureHandle m_aSpriteStars[3];

		// projectiles
		IGraphics::CTextureHandle m_SpriteWeaponGunProjectile;
		IGraphics::CTextureHandle m_SpriteWeaponShotgunProjectile;
		IGraphics::CTextureHandle m_SpriteWeaponGrenadeProjectile;
		IGraphics::CTextureHandle m_SpriteWeaponHammerProjectile;
		IGraphics::CTextureHandle m_SpriteWeaponNinjaProjectile;
		IGraphics::CTextureHandle m_SpriteWeaponLaserProjectile;

		IGraphics::CTextureHandle m_aSpriteWeaponProjectiles[6];

		// muzzles
		IGraphics::CTextureHandle m_aSpriteWeaponGunMuzzles[3];
		IGraphics::CTextureHandle m_aSpriteWeaponShotgunMuzzles[3];
		IGraphics::CTextureHandle m_aaSpriteWeaponNinjaMuzzles[3];

		IGraphics::CTextureHandle m_aaSpriteWeaponsMuzzles[6][3];

		// pickups
		IGraphics::CTextureHandle m_SpritePickupHealth;
		IGraphics::CTextureHandle m_SpritePickupArmor;
		IGraphics::CTextureHandle m_SpritePickupArmorShotgun;
		IGraphics::CTextureHandle m_SpritePickupArmorGrenade;
		IGraphics::CTextureHandle m_SpritePickupArmorNinja;
		IGraphics::CTextureHandle m_SpritePickupArmorLaser;
		IGraphics::CTextureHandle m_SpritePickupGrenade;
		IGraphics::CTextureHandle m_SpritePickupShotgun;
		IGraphics::CTextureHandle m_SpritePickupLaser;
		IGraphics::CTextureHandle m_SpritePickupNinja;
		IGraphics::CTextureHandle m_SpritePickupGun;
		IGraphics::CTextureHandle m_SpritePickupHammer;

		IGraphics::CTextureHandle m_aSpritePickupWeapons[6];
		IGraphics::CTextureHandle m_aSpritePickupWeaponArmor[4];

		// flags
		IGraphics::CTextureHandle m_SpriteFlagBlue;
		IGraphics::CTextureHandle m_SpriteFlagRed;

		// ninja bar (0.7)
		IGraphics::CTextureHandle m_SpriteNinjaBarFullLeft;
		IGraphics::CTextureHandle m_SpriteNinjaBarFull;
		IGraphics::CTextureHandle m_SpriteNinjaBarEmpty;
		IGraphics::CTextureHandle m_SpriteNinjaBarEmptyRight;

		bool IsSixup() const
		{
			return m_SpriteNinjaBarFullLeft.IsValid();
		}
	};

	SClientGameSkin m_GameSkin;
	bool m_GameSkinLoaded = false;

	struct SClientParticlesSkin
	{
		IGraphics::CTextureHandle m_SpriteParticleSlice;
		IGraphics::CTextureHandle m_SpriteParticleBall;
		IGraphics::CTextureHandle m_aSpriteParticleSplat[3];
		IGraphics::CTextureHandle m_SpriteParticleSmoke;
		IGraphics::CTextureHandle m_SpriteParticleShell;
		IGraphics::CTextureHandle m_SpriteParticleExpl;
		IGraphics::CTextureHandle m_SpriteParticleAirJump;
		IGraphics::CTextureHandle m_SpriteParticleHit;
		IGraphics::CTextureHandle m_aSpriteParticles[10];
	};

	SClientParticlesSkin m_ParticlesSkin;
	bool m_ParticlesSkinLoaded = false;

	struct SClientEmoticonsSkin
	{
		IGraphics::CTextureHandle m_aSpriteEmoticons[16];
	};

	SClientEmoticonsSkin m_EmoticonsSkin;
	bool m_EmoticonsSkinLoaded = false;

	struct SClientHudSkin
	{
		IGraphics::CTextureHandle m_SpriteHudAirjump;
		IGraphics::CTextureHandle m_SpriteHudAirjumpEmpty;
		IGraphics::CTextureHandle m_SpriteHudSolo;
		IGraphics::CTextureHandle m_SpriteHudCollisionDisabled;
		IGraphics::CTextureHandle m_SpriteHudEndlessJump;
		IGraphics::CTextureHandle m_SpriteHudEndlessHook;
		IGraphics::CTextureHandle m_SpriteHudJetpack;
		IGraphics::CTextureHandle m_SpriteHudFreezeBarFullLeft;
		IGraphics::CTextureHandle m_SpriteHudFreezeBarFull;
		IGraphics::CTextureHandle m_SpriteHudFreezeBarEmpty;
		IGraphics::CTextureHandle m_SpriteHudFreezeBarEmptyRight;
		IGraphics::CTextureHandle m_SpriteHudNinjaBarFullLeft;
		IGraphics::CTextureHandle m_SpriteHudNinjaBarFull;
		IGraphics::CTextureHandle m_SpriteHudNinjaBarEmpty;
		IGraphics::CTextureHandle m_SpriteHudNinjaBarEmptyRight;
		IGraphics::CTextureHandle m_SpriteHudHookHitDisabled;
		IGraphics::CTextureHandle m_SpriteHudHammerHitDisabled;
		IGraphics::CTextureHandle m_SpriteHudShotgunHitDisabled;
		IGraphics::CTextureHandle m_SpriteHudGrenadeHitDisabled;
		IGraphics::CTextureHandle m_SpriteHudLaserHitDisabled;
		IGraphics::CTextureHandle m_SpriteHudGunHitDisabled;
		IGraphics::CTextureHandle m_SpriteHudDeepFrozen;
		IGraphics::CTextureHandle m_SpriteHudLiveFrozen;
		IGraphics::CTextureHandle m_SpriteHudTeleportGrenade;
		IGraphics::CTextureHandle m_SpriteHudTeleportGun;
		IGraphics::CTextureHandle m_SpriteHudTeleportLaser;
		IGraphics::CTextureHandle m_SpriteHudPracticeMode;
		IGraphics::CTextureHandle m_SpriteHudLockMode;
		IGraphics::CTextureHandle m_SpriteHudTeam0Mode;
		IGraphics::CTextureHandle m_SpriteHudDummyHammer;
		IGraphics::CTextureHandle m_SpriteHudDummyCopy;
	};

	SClientHudSkin m_HudSkin;
	bool m_HudSkinLoaded = false;

	struct SClientExtrasSkin
	{
		IGraphics::CTextureHandle m_SpriteParticleSnowflake;
		IGraphics::CTextureHandle m_SpriteParticleSparkle;
		IGraphics::CTextureHandle m_SpritePulley;
		IGraphics::CTextureHandle m_SpriteHectagon;
		IGraphics::CTextureHandle m_aSpriteParticles[4];
	};

	SClientExtrasSkin m_ExtrasSkin;
	bool m_ExtrasSkinLoaded = false;

	const std::vector<CSnapEntities> &SnapEntities() { return m_vSnapEntities; }

	CGameView::CMultiViewState &MultiView() { return LegacyGameView().MultiView(); }

	void ResetMultiView();
	int FindFirstMultiViewId();
	void CleanMultiViewId(int ClientId);

private:
	std::vector<CSnapEntities> m_vSnapEntities;
	void SnapCollectEntities(CSessionId SessionId, int Conn);

	class CImageAsset
	{
	public:
		bool IsLoaded() const { return m_ImageInfo.m_pData != nullptr; }

		char m_aPath[IO_MAX_PATH_LENGTH];
		bool m_IsDefault;
		CImageInfo m_ImageInfo;
		std::optional<CImageInfo> m_FallbackImageInfo;
	};

	CImageAsset LoadAssetFromPath(const char *pPath, bool AsDir, int AssetId, const char *pDirectory) const;

	std::vector<std::shared_ptr<CManagedTeeRenderInfo>> m_vpManagedTeeRenderInfos;
	void UpdateManagedTeeRenderInfos();

	void UpdateInputRoutes(CSessionId SessionId);
	void UpdateLocalTuning(CSessionId SessionId, CGameSessionContext &Session, CGameState &State, int Conn);
	void UpdatePrediction();
	void UpdateRenderedClients(const CGameSessionContext &Session, CGameState &State, int Conn, int64_t Now, const CGameTickInfo &Time, EPresentationPlayback Playback);
	void UpdateSpectatorCursor(const CGameState &State, const CGameTickInfo &Time);
	void HandlePredictedEvents(int Tick);

	void OnInput(const IInput::CEvent &Event);

	void DetectStrongHook(CGameState::CRuntimeState &Runtime);

	vec2 GetSmoothPos(CSessionId SessionId, const CGameState &State, int Conn, int ClientId, int64_t Now, const CCharacterCore &Prev, const CCharacterCore &Current);

	std::optional<CStreamId> m_PreviousFocusedStream;

	CTuningParams *TuningList() { return MapContext().TuningList(); }

	float m_LastShowDistanceZoom;
	float m_LastZoom;
	float m_LastScreenAspect;
	float m_LastDeadzone;
	float m_LastFollowFactor;
	bool m_LastDummyConnected;

	void HandleMultiView(const CGameState &State);
	bool IsMultiViewIdSet();
	void CleanMultiViewIds();
	bool InitMultiView(const CGameState &State, int Team);
	float CalculateMultiViewMultiplier(vec2 TargetPos);
	float CalculateMultiViewZoom(vec2 MinPos, vec2 MaxPos, float Vel);
	float MapValue(float MaxValue, float MinValue, float MaxRange, float MinRange, float Value);

	void OnSaveCodeNetMessage(CGameSessionContext &Session, const CGameState &State, const CNetMsg_Sv_SaveCode *pMsg);
	void StoreSave(const CGameSessionContext &Session, const char *pTeamMembers, const char *pGeneratedCode) const;
};

ColorRGBA CalculateNameColor(ColorHSLA TextColorHSL);

#endif
