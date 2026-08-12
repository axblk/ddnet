#include "test.h"

#include <base/logger.h>
#include <base/types.h>

#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/server/databases/connection.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/register.h>
#include <engine/server/server.h>
#include <engine/server/server_logger.h>
#include <engine/shared/assertion_logger.h>
#include <engine/shared/config.h>
#include <engine/shared/jsonwriter.h>

#include <generated/protocol.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/dragger.h>
#include <game/server/entities/dragger_beam.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/server/gamemodes/ddrace_character.h>
#include <game/server/gamemodes/mod.h>
#include <game/server/gameworld.h>
#include <game/server/interactions.h>
#include <game/server/mode/game_mode_registry.h>
#include <game/server/mode/game_services.h>
#include <game/server/mode/match_stats.h>
#include <game/server/modes/vanilla/ctf.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/flag.h>
#include <game/server/modes/vanilla/tdm.h>
#include <game/server/player.h>
#include <game/server/save.h>
#include <game/server/score.h>
#include <game/server/teams.h>
#include <game/version.h>

#include <gtest/gtest.h>

#include <limits>
#include <memory>
#include <thread>

bool IsInterrupted()
{
	return false;
}

TEST(MatchStats, AggregatesTypedEvents)
{
	CMatchStats Stats;
	Stats.OnEvent(CMatchEventShotFired{0, WEAPON_GUN});
	Stats.OnEvent(CMatchEventWeaponHit{0, 1, WEAPON_GUN});
	Stats.OnEvent(CMatchEventDamage{0, 1, WEAPON_GUN, 4});
	Stats.OnEvent(CMatchEventKill{0, 1, WEAPON_GUN});
	Stats.OnEvent(CMatchEventSuicide{1, WEAPON_SELF});
	Stats.OnEvent(CMatchEventSpawn{0, TEAM_RED});
	Stats.OnEvent(CMatchEventTeamChanged{0, TEAM_RED, TEAM_BLUE});
	Stats.OnEvent(CMatchEventRoundStarted{});
	Stats.OnEvent(CMatchEventRoundEnded{});
	Stats.OnEvent(CMatchEventFlagGrab{0, TEAM_BLUE});
	Stats.OnEvent(CMatchEventFlagDrop{0, TEAM_BLUE});
	Stats.OnEvent(CMatchEventFlagReturn{1, TEAM_BLUE});
	Stats.OnEvent(CMatchEventFlagCapture{0, TEAM_BLUE, 42});
	Stats.OnEvent(CMatchEventFlagReturn{-1, TEAM_RED});

	const CMatchPlayerStats &Attacker = Stats.Player(0);
	EXPECT_EQ(Attacker.m_ShotsFired, 1);
	EXPECT_EQ(Attacker.m_aShotsFired[WEAPON_GUN], 1);
	EXPECT_EQ(Attacker.m_WeaponHits, 1);
	EXPECT_EQ(Attacker.m_aWeaponHits[WEAPON_GUN], 1);
	EXPECT_EQ(Attacker.m_DamageDealt, 4);
	EXPECT_EQ(Attacker.m_aDamageDealt[WEAPON_GUN], 4);
	EXPECT_EQ(Attacker.m_Kills, 1);
	EXPECT_EQ(Attacker.m_aKills[WEAPON_GUN], 1);
	EXPECT_EQ(Attacker.m_Spawns, 1);
	EXPECT_EQ(Attacker.m_TeamChanges, 1);
	EXPECT_EQ(Attacker.m_FlagGrabs, 1);
	EXPECT_EQ(Attacker.m_FlagDrops, 1);
	EXPECT_EQ(Attacker.m_FlagCaptures, 1);

	const CMatchPlayerStats &Victim = Stats.Player(1);
	EXPECT_EQ(Victim.m_DamageTaken, 4);
	EXPECT_EQ(Victim.m_Deaths, 2);
	EXPECT_EQ(Victim.m_Suicides, 1);
	EXPECT_EQ(Victim.m_FlagReturns, 1);
	EXPECT_EQ(Stats.Flag(TEAM_BLUE).m_Grabs, 1);
	EXPECT_EQ(Stats.Flag(TEAM_BLUE).m_Drops, 1);
	EXPECT_EQ(Stats.Flag(TEAM_BLUE).m_Returns, 1);
	EXPECT_EQ(Stats.Flag(TEAM_BLUE).m_Captures, 1);
	EXPECT_EQ(Stats.Flag(TEAM_RED).m_Returns, 1);
	EXPECT_EQ(Stats.RoundsStarted(), 1);
	EXPECT_EQ(Stats.RoundsEnded(), 1);
}

#if defined(CONF_PLATFORM_ANDROID)
std::vector<std::string> FetchAndroidServerCommandQueue()
{
	return {};
}
#endif

class GameWorld : public ::testing::Test // NOLINT(readability-identifier-naming)
{
public:
	IGameServer *m_pGameServer = nullptr;
	CServer *m_pServer = nullptr;
	std::unique_ptr<IKernel> m_pKernel;
	CTestInfo m_TestInfo;
	std::unique_ptr<IStorage> m_pStorage;

	CGameContext *GameServer() // NOLINT(readability-make-member-function-const)
	{
		return (CGameContext *)m_pGameServer;
	}

	CGameServices &GameServices() // NOLINT(readability-make-member-function-const)
	{
		return GameServer()->GameHost().Services();
	}

	IGameController *GameController() // NOLINT(readability-make-member-function-const)
	{
		return GameServer()->GameHost().Controller();
	}

	void DeletePlayers()
	{
		for(CPlayer *&pPlayer : GameServer()->m_apPlayers)
		{
			delete pPlayer;
			pPlayer = nullptr;
		}
	}

	void SelectGameMode(const char *pModeId)
	{
		for(const CPlayer *pPlayer : GameServer()->m_apPlayers)
			ASSERT_EQ(pPlayer, nullptr) << "cannot switch modes with live players";
		GameServer()->GameHost().Shutdown();
		ASSERT_TRUE(GameServer()->GameHost().Select(pModeId));
		GameServer()->GameHost().Init(m_pServer->DbPool());
	}

	CGameControllerDDRace *RaceControllerOrNull() // NOLINT(readability-make-member-function-const)
	{
		return dynamic_cast<CGameControllerDDRace *>(GameController());
	}

	CGameControllerDDRace &RaceController() // NOLINT(readability-make-member-function-const)
	{
		return *RaceControllerOrNull();
	}

	CGameTeams &RaceTeams() // NOLINT(readability-make-member-function-const)
	{
		return RaceController().RaceTeams();
	}

	CScore &RaceScore() // NOLINT(readability-make-member-function-const)
	{
		return RaceController().RaceScore();
	}

	GameWorld()
	{
		CServer *pServer = CreateServer();
		m_pServer = pServer;

		m_pKernel = std::unique_ptr<IKernel>(IKernel::Create());
		m_pKernel->RegisterInterface(m_pServer);

		IEngine *pEngine = CreateTestEngine(GAME_NAME);
		m_pKernel->RegisterInterface(pEngine);

		m_TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_TestInfo.CreateTestStorage();
		EXPECT_NE(m_pStorage, nullptr);
		m_pKernel->RegisterInterface(m_pStorage.get(), false);

		IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON).release();
		m_pKernel->RegisterInterface(pConsole);

		IConfigManager *pConfigManager = CreateConfigManager();
		m_pKernel->RegisterInterface(pConfigManager);

		IEngineHttp *pEngineHttp = CreateEngineHttp();
		m_pKernel->RegisterInterface(pEngineHttp); // IEngineHttp
		m_pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);

		IEngineAntibot *pEngineAntibot = CreateEngineAntibot();
		m_pKernel->RegisterInterface(pEngineAntibot);
		m_pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);

		m_pGameServer = CreateGameServer();
		m_pKernel->RegisterInterface(m_pGameServer);

		pEngine->Init();
		pConsole->Init();
		pConfigManager->Init();

		m_pServer->RegisterCommands();

		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		m_pServer->m_RunServer = CServer::RUNNING;

		m_pServer->m_AuthManager.Init();

		{
			int Size = GameServer()->PersistentClientDataSize();
			for(auto &Client : m_pServer->m_aClients)
			{
				Client.m_HasPersistentData = false;
				Client.m_pPersistentData = malloc(Size);
			}
		}
		m_pServer->m_pPersistentData = malloc(GameServer()->PersistentDataSize());
		EXPECT_NE(m_pServer->LoadMap("coverage"), 0);

		EXPECT_TRUE(pEngineHttp->Init(std::chrono::seconds{2})) << "Failed to initialize the HTTP client";

		pServer->m_NetServer.SetCallbacks(
			CServer::NewClientCallback,
			CServer::NewClientNoAuthCallback,
			CServer::ClientRejoinCallback,
			CServer::DelClientCallback, pServer);

		pServer->m_Econ.Init(pServer->Config(), pServer->Console(), &pServer->m_ServerBan);

		pServer->m_Fifo.Init(pServer->Console(), pServer->Config()->m_SvInputFifo, CFGFLAG_SERVER);
		m_pServer->Antibot()->Init();
		GameServer()->OnInit(nullptr);
		pServer->ReadAnnouncementsFile();
		pServer->InitMaplist();
	}

	~GameWorld() override
	{
		m_pServer->m_Econ.Shutdown();
		m_pServer->m_Fifo.Shutdown();
		m_pGameServer->OnShutdown(nullptr);
		m_pServer->DbPool()->OnShutdown();
	}
};

namespace
{
	class CFactoryPlayer final : public CPlayerVanilla
	{
		int *m_pDestructions;

	public:
		CFactoryPlayer(CGameServices &Services, uint32_t UniqueClientId, int ClientId, int Team, int *pDestructions) :
			CPlayerVanilla(Services, UniqueClientId, ClientId, Team),
			m_pDestructions(pDestructions)
		{
		}

		~CFactoryPlayer() override
		{
			(*m_pDestructions)++;
		}
	};

	class CFactoryCharacter final : public CCharacter
	{
		int *m_pDestructions;

	public:
		CFactoryCharacter(CGameWorld *pWorld, CNetObj_PlayerInput LastInput, int *pDestructions) :
			CCharacter(pWorld, LastInput),
			m_pDestructions(pDestructions)
		{
		}

		~CFactoryCharacter() override
		{
			(*m_pDestructions)++;
		}
	};

	class CFactoryController final : public CGameControllerVanillaDM
	{
		int *m_pPlayerDestructions;
		int *m_pCharacterDestructions;

	public:
		CFactoryController(CGameServices &Services, const CGameModeInfo &GameModeInfo, int *pPlayerDestructions, int *pCharacterDestructions) :
			CGameControllerVanillaDM(Services, GameModeInfo),
			m_pPlayerDestructions(pPlayerDestructions),
			m_pCharacterDestructions(pCharacterDestructions)
		{
		}

		CPlayer *CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team) override
		{
			return new(ClientId) CFactoryPlayer(Services(), UniqueClientId, ClientId, Team, m_pPlayerDestructions);
		}

		CCharacter *CreateCharacter(CPlayer *pPlayer) override
		{
			const int ClientId = pPlayer->GetCid();
			return new(ClientId) CFactoryCharacter(&Services().World(), Services().LastPlayerInput(ClientId), m_pCharacterDestructions);
		}

		bool CanSpawn(int, vec2 *pOutPos, int) override
		{
			*pOutPos = vec2(64.0f, 96.0f);
			return true;
		}
	};

	class CServicesFactoryController final : public IGameController
	{
	public:
		CGameServices *m_pConstructionServices;

		CServicesFactoryController(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
			IGameController(Services, GameModeInfo),
			m_pConstructionServices(&Services)
		{
		}

		bool IsValidTeam(int Team) override
		{
			return Team == TEAM_RED || Team == TEAM_BLUE || Team == TEAM_SPECTATORS;
		}
	};

	class CTestVanillaTDM final : public CGameControllerVanillaTDM
	{
	public:
		using CGameControllerVanillaTDM::CGameControllerVanillaTDM;
		using CGameControllerVanillaTeamplay::UpdateTeamBalance;
	};

	std::unique_ptr<IGameController> CreateServicesFactoryController(CGameServices &Services, const CGameModeInfo &GameModeInfo)
	{
		return std::make_unique<CServicesFactoryController>(Services, GameModeInfo);
	}
}

TEST_F(GameWorld, ClosestCharacter)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChr1 = new(0) CCharacter(&GameServer()->m_World, Input);
	pChr1->m_Pos = vec2(0, 0);
	GameServer()->m_World.InsertEntity(pChr1);

	CCharacter *pChr2 = new(1) CCharacter(&GameServer()->m_World, Input);
	pChr2->m_Pos = vec2(10, 10);
	GameServer()->m_World.InsertEntity(pChr2);

	CCharacter *pClosest = GameServer()->m_World.ClosestCharacter(vec2(1, 1), 20, nullptr);
	EXPECT_EQ(pClosest, pChr1);
}

TEST_F(GameWorld, IntersectEntity)
{
	CNetObj_PlayerInput Input = {};
	CCharacter *pChrLeft = new(0) CCharacter(&GameServer()->m_World, Input);
	pChrLeft->m_Pos = vec2(15, 10);
	GameServer()->m_World.InsertEntity(pChrLeft);

	CCharacter *pChrRight = new(1) CCharacter(&GameServer()->m_World, Input);
	pChrRight->m_Pos = vec2(16, 10);
	GameServer()->m_World.InsertEntity(pChrRight);

	float Radius = 5.0f;
	vec2 IntersectAt;
	CCharacter *pIntersectedChar;

	// both tees are exactly on the line
	// if we go intersect left to right we find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// if we intersect right to left we find the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);

	// but not if we ignore the right one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrRight, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// or we force find the left one

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrLeft);

	// pNotThis == pThisOnly => nullptr

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(20, 10), // intersect from
		vec2(10, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		pChrLeft, // pNotThis
		-1, // CollideWith
		pChrLeft /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, nullptr);

	// the tee closer to the start of the intersection line
	// will not be matched if it is further than Radius away
	// from the line

	vec2 CloserToFromButTooFarFromLine = vec2(11, 11 + Radius + pChrLeft->GetProximityRadius());
	pChrLeft->SetPosition(CloserToFromButTooFarFromLine);
	pChrLeft->m_Pos = CloserToFromButTooFarFromLine;

	pIntersectedChar = (CCharacter *)GameServer()->m_World.IntersectEntity(
		vec2(10, 10), // intersect from
		vec2(20, 10), // intersect to
		Radius,
		CGameWorld::ENTTYPE_CHARACTER,
		IntersectAt,
		nullptr, // pNotThis
		-1, // CollideWith
		nullptr /* pThisOnly */);
	EXPECT_EQ(pIntersectedChar, pChrRight);
}

TEST_F(GameWorld, BasicTick)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	const int StartTeam = GameController()->GetAutoTeam(ClientId);
	GameServer()->CreatePlayer(ClientId, StartTeam, Afk, LastWhisperTo);

	GameServer()->OnTick();
}

TEST_F(GameWorld, CharacterEmote)
{
	int ClientId = 0;
	bool Afk = true;
	int LastWhisperTo = -1;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, Afk, LastWhisperTo);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pChr = pPlayer->GetCharacter();
	ASSERT_NE(pChr, nullptr);

	// afk
	pPlayer->SetAfk(true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// not afk
	pPlayer->SetAfk(false);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_BLINK);

	// frozen and paused
	pPlayer->Pause(CPlayer::PAUSE_PAUSED, true);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_NORMAL);

	// ninja jetpack
	pPlayer->Pause(CPlayer::PAUSE_NONE, true);
	pChr->Unfreeze();
	pPlayer->m_NinjaJetpack = true;
	pChr->SetJetpack(true);
	pChr->SetActiveWeapon(WEAPON_GUN);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_HAPPY);

	// /emote angry 3 chat command
	pChr->SetEmote(EMOTE_ANGRY, GameServer()->Server()->Tick() + GameServer()->Server()->TickSpeed() * 3);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);

	// /emote angry 3 chat command and frozen
	pChr->Freeze(10);
	ASSERT_EQ(pChr->DetermineEyeEmote(), EMOTE_ANGRY);
}

TEST_F(GameWorld, VanillaWeaponFire)
{
	constexpr int ClientId = 0;
	SelectGameMode("vanilla.dm");
	auto &Controller = *dynamic_cast<CGameControllerVanillaDM *>(GameController());
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);
	EXPECT_EQ(Controller.MatchStats().Player(ClientId).m_Spawns, 1);

	const CTuningParams Tuning = CGameControllerVanillaDM::DefaultTuning();
	auto CountProjectiles = [this]() {
		int Count = 0;
		for(CEntity *pEntity = GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_PROJECTILE); pEntity; pEntity = pEntity->TypeNext())
			Count++;
		return Count;
	};

	pCharacter->SetWeaponAmmo(WEAPON_GUN, 10);
	CWeaponFireContext Context = {pCharacter, WEAPON_GUN, vec2(1, 0), vec2(1, 0), pCharacter->m_Pos, &Tuning};
	const int BeforeGun = CountProjectiles();
	pCharacter->SetActiveWeapon(WEAPON_GUN);
	CNetObj_PlayerInput Input = {};
	Input.m_TargetX = 1;
	pCharacter->OnDirectInput(&Input);
	Input.m_Fire = 1;
	pCharacter->OnDirectInput(&Input);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_GUN), 9);
	EXPECT_EQ(CountProjectiles(), BeforeGun + 1);
	EXPECT_EQ(Controller.MatchStats().Player(ClientId).m_ShotsFired, 1);
	EXPECT_EQ(Controller.MatchStats().Player(ClientId).m_aShotsFired[WEAPON_GUN], 1);

	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 10);
	Context.m_Weapon = WEAPON_SHOTGUN;
	const int BeforeShotgun = CountProjectiles();
	const CWeaponFireResult ShotgunResult = Controller.OnCharacterFireWeapon(Context);
	EXPECT_TRUE(ShotgunResult.m_Fired);
	EXPECT_TRUE(ShotgunResult.m_ConsumeAmmo);
	EXPECT_EQ(CountProjectiles(), BeforeShotgun + 5);

	pCharacter->SetWeaponAmmo(WEAPON_GUN, 0);
	Context.m_Weapon = WEAPON_GUN;
	const int BeforeNoAmmo = CountProjectiles();
	const CWeaponFireResult NoAmmoResult = Controller.OnCharacterFireWeapon(Context);
	EXPECT_FALSE(NoAmmoResult.m_Fired);
	EXPECT_GT(NoAmmoResult.m_ReloadTicks, 0);
	EXPECT_EQ(CountProjectiles(), BeforeNoAmmo);
	EXPECT_EQ(Controller.MatchStats().Player(ClientId).m_ShotsFired, 1);
	const int RoundsStartedBefore = Controller.MatchStats().RoundsStarted();
	const int RoundsEndedBefore = Controller.MatchStats().RoundsEnded();
	Controller.StartRound();
	Controller.EndRound();
	EXPECT_EQ(Controller.MatchStats().RoundsStarted(), RoundsStartedBefore + 1);
	EXPECT_EQ(Controller.MatchStats().RoundsEnded(), RoundsEndedBefore + 1);
}

TEST_F(GameWorld, MapEntitySetsAreExplicit)
{
	auto CountEntities = [this](int Type) {
		int Count = 0;
		for(CEntity *pEntity = GameServer()->m_World.FindFirst(Type); pEntity; pEntity = pEntity->TypeNext())
			Count++;
		return Count;
	};

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	const int BeforeVanilla = CountEntities(CGameWorld::ENTTYPE_PROJECTILE);
	EXPECT_FALSE(VanillaController.OnEntity(ENTITY_CRAZY_SHOTGUN, 10, 10, LAYER_GAME, 0, true, 0));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PROJECTILE), BeforeVanilla);
	EXPECT_TRUE(VanillaController.OnEntity(ENTITY_SPAWN, 10, 10, LAYER_GAME, 0, true, 0));
	EXPECT_TRUE(VanillaController.OnEntity(ENTITY_SPAWN, 10, 10, LAYER_GAME, 0, false, 0));
	const int BeforePickups = CountEntities(CGameWorld::ENTTYPE_PICKUP);
	EXPECT_TRUE(VanillaController.OnEntity(ENTITY_HEALTH_1, 10, 10, LAYER_GAME, 0, true, 0));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PICKUP), BeforePickups + 1);

	const CGameModeInfo DDNetInfo = {"ddnet", "DDNet", "DDraceNetwork", "TestDDraceNetwork", EGameModeScoreKind::TIME, protocol7::GAMEFLAG_RACE, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerDDNet DDNetController(GameServices(), DDNetInfo);
	EXPECT_TRUE(DDNetController.OnEntity(ENTITY_CRAZY_SHOTGUN, 10, 10, LAYER_GAME, 0, true, 0));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PROJECTILE), BeforeVanilla + 1);

	const CGameModeInfo ModInfo = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerMod ModController(GameServices(), ModInfo);
	EXPECT_TRUE(ModController.OnEntity(ENTITY_CRAZY_SHOTGUN, 10, 10, LAYER_GAME, 0, true, 0));
	EXPECT_EQ(CountEntities(CGameWorld::ENTTYPE_PROJECTILE), BeforeVanilla + 2);
}

TEST_F(GameWorld, DDRaceMapSettingsAreModeOwned)
{
	ASSERT_GE(GameServer()->Collision()->GetWidth() * GameServer()->Collision()->GetHeight(), 6);
	const CGameModeInfo Info = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN, 0, true};
	CGameControllerMod Controller(GameServices(), Info);
	const CTuningParams PreviousTuning = *GameServer()->GlobalTuning();
	const int PreviousOldLaser = g_Config.m_SvOldLaser;
	const int PreviousEndlessDrag = g_Config.m_SvEndlessDrag;
	const int PreviousHit = g_Config.m_SvHit;
	int aPreviousTiles[5];
	const int aMapSettings[] = {TILE_OLDLASER, TILE_NPC, TILE_EHOOK, TILE_NOHIT, TILE_NPH};
	for(int i = 0; i < 5; i++)
	{
		aPreviousTiles[i] = GameServer()->Collision()->GameLayer()[i + 1].m_Index;
		GameServer()->Collision()->SetCollisionAt((i + 1) * 32.0f, 0.0f, aMapSettings[i]);
	}

	g_Config.m_SvOldLaser = 0;
	g_Config.m_SvEndlessDrag = 0;
	g_Config.m_SvHit = 1;
	Controller.OnReset();

	EXPECT_EQ(g_Config.m_SvOldLaser, 1);
	EXPECT_EQ(g_Config.m_SvEndlessDrag, 1);
	EXPECT_EQ(g_Config.m_SvHit, 0);
	EXPECT_EQ(GameServer()->GlobalTuning()->m_PlayerCollision, 0);
	EXPECT_EQ(GameServer()->GlobalTuning()->m_PlayerHooking, 0);

	for(int i = 0; i < 5; i++)
		GameServer()->Collision()->SetCollisionAt((i + 1) * 32.0f, 0.0f, aPreviousTiles[i]);
	*GameServer()->GlobalTuning() = PreviousTuning;
	g_Config.m_SvOldLaser = PreviousOldLaser;
	g_Config.m_SvEndlessDrag = PreviousEndlessDrag;
	g_Config.m_SvHit = PreviousHit;
}

TEST_F(GameWorld, CharacterTickPhasesAreModeOwned)
{
	vec2 SpawnPos;
	bool FoundSpawn = false;
	for(int y = 1; y < GameServer()->Collision()->GetHeight() - 1 && !FoundSpawn; y++)
	{
		for(int x = 1; x < GameServer()->Collision()->GetWidth() - 1; x++)
		{
			const vec2 Candidate(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			if(!GameServer()->Collision()->TestBox(Candidate, CCharacterCore::PhysicalSizeVec2()))
			{
				SpawnPos = Candidate;
				FoundSpawn = true;
				break;
			}
		}
	}
	ASSERT_TRUE(FoundSpawn);

	const int PreviousNoWeakHook = g_Config.m_SvNoWeakHook;
	g_Config.m_SvNoWeakHook = 0;

	SelectGameMode("vanilla.dm");
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CCharacter *pVanillaCharacter = GameServer()->m_apPlayers[0]->ForceSpawn(SpawnPos);
	EXPECT_NE(pVanillaCharacter, nullptr);
	if(!pVanillaCharacter)
	{
		delete GameServer()->m_apPlayers[0];
		GameServer()->m_apPlayers[0] = nullptr;
		g_Config.m_SvNoWeakHook = PreviousNoWeakHook;
		return;
	}
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	EXPECT_TRUE(pVanillaCharacter->Freeze(2));
	const int VanillaFreezeBefore = pVanillaCharacter->m_FreezeTime;
	pVanillaCharacter->Tick();
	EXPECT_EQ(pVanillaCharacter->m_FreezeTime, VanillaFreezeBefore);

	const vec2 ClippedPos(-10000.0f, -10000.0f);
	pVanillaCharacter->SetPosition(ClippedPos);
	pVanillaCharacter->m_Pos = ClippedPos;
	pVanillaCharacter->Tick();
	EXPECT_FALSE(pVanillaCharacter->IsAlive());
	delete GameServer()->m_apPlayers[0];
	GameServer()->m_apPlayers[0] = nullptr;

	SelectGameMode("ddnet");
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CCharacter *pRaceCharacter = GameServer()->m_apPlayers[0]->ForceSpawn(SpawnPos);
	EXPECT_NE(pRaceCharacter, nullptr);
	if(!pRaceCharacter)
	{
		g_Config.m_SvNoWeakHook = PreviousNoWeakHook;
		return;
	}
	EXPECT_NE(dynamic_cast<CCharacterDDRace *>(pRaceCharacter), nullptr);
	EXPECT_TRUE(pRaceCharacter->Freeze(2));
	const int RaceFreezeBefore = pRaceCharacter->m_FreezeTime;
	pRaceCharacter->Tick();
	EXPECT_EQ(pRaceCharacter->m_FreezeTime, RaceFreezeBefore - 1);

	delete GameServer()->m_apPlayers[0];
	GameServer()->m_apPlayers[0] = nullptr;
	SelectGameMode("mod");
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CCharacter *pModCharacter = GameServer()->m_apPlayers[0]->ForceSpawn(SpawnPos);
	ASSERT_NE(dynamic_cast<CCharacterDDRace *>(pModCharacter), nullptr);
	EXPECT_TRUE(pModCharacter->Freeze(2));
	const int ModFreezeBefore = pModCharacter->m_FreezeTime;
	pModCharacter->Tick();
	EXPECT_EQ(pModCharacter->m_FreezeTime, ModFreezeBefore - 1);

	g_Config.m_SvNoWeakHook = PreviousNoWeakHook;
}

TEST_F(GameWorld, CharacterSpawnInitializationIsModeOwned)
{
	vec2 TunePosition;
	int MapTuneZone = 0;
	for(int y = 0; y < GameServer()->Collision()->GetHeight() && MapTuneZone == 0; y++)
	{
		for(int x = 0; x < GameServer()->Collision()->GetWidth(); x++)
		{
			MapTuneZone = GameServer()->Collision()->IsTune(y * GameServer()->Collision()->GetWidth() + x);
			if(MapTuneZone > 0)
			{
				TunePosition = vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
				break;
			}
		}
	}
	ASSERT_GT(MapTuneZone, 0);
	const float GlobalGravity = GameServer()->TuningList()[0].m_Gravity;
	const float ZoneGravity = GlobalGravity + 1.0f;
	GameServer()->TuningList()[MapTuneZone].m_Gravity = ZoneGravity;

	const int PreviousEndlessDrag = g_Config.m_SvEndlessDrag;
	const int PreviousTeamMode = g_Config.m_SvTeam;
	g_Config.m_SvEndlessDrag = 1;
	g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;

	SelectGameMode("vanilla.dm");
	GameServer()->TuningList()[MapTuneZone].m_Gravity = ZoneGravity;
	EXPECT_EQ(GameController()->TuningZoneAt(TunePosition), 0);
	CPlayer *pVanillaPlayer = GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CCharacter *pVanillaCharacter = pVanillaPlayer->ForceSpawn(TunePosition);
	EXPECT_NE(pVanillaCharacter, nullptr);
	if(!pVanillaCharacter)
	{
		delete GameServer()->m_apPlayers[0];
		GameServer()->m_apPlayers[0] = nullptr;
		g_Config.m_SvEndlessDrag = PreviousEndlessDrag;
		g_Config.m_SvTeam = PreviousTeamMode;
		return;
	}
	EXPECT_FALSE(pVanillaCharacter->GetCore().m_EndlessHook);
	EXPECT_FALSE(pVanillaCharacter->GetCore().m_Solo);
	EXPECT_EQ(pVanillaCharacter->TuningZone(), 0);
	EXPECT_FLOAT_EQ(pVanillaCharacter->GetCore().m_Tuning.m_Gravity, GlobalGravity);
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_INGAME;
	pVanillaPlayer->Tick();
	EXPECT_EQ(pVanillaPlayer->m_TuneZone, 0);
	auto *pVanillaProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, 0, TunePosition, vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_EQ(pVanillaProjectile->NetInfo().m_TuneZone, 0);
	delete pVanillaProjectile;
	pVanillaCharacter->SetInvincible(true);
	EXPECT_TRUE(pVanillaCharacter->GetCore().m_Invincible);
	EXPECT_FALSE(pVanillaCharacter->GetCore().m_Super);
	pVanillaCharacter->PreTick();
	EXPECT_TRUE(pVanillaCharacter->IsAlive());
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	delete GameServer()->m_apPlayers[0];
	GameServer()->m_apPlayers[0] = nullptr;
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_EMPTY;

	SelectGameMode("ddnet");
	g_Config.m_SvEndlessDrag = 1;
	g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;
	GameServer()->TuningList()[MapTuneZone].m_Gravity = ZoneGravity;
	EXPECT_EQ(GameController()->TuningZoneAt(TunePosition), MapTuneZone);
	GameServer()->CreatePlayer(1, TEAM_GAME, false, -1);
	CCharacter *pDDNetCharacter = GameServer()->m_apPlayers[1]->ForceSpawn(TunePosition);
	g_Config.m_SvEndlessDrag = PreviousEndlessDrag;
	g_Config.m_SvTeam = PreviousTeamMode;
	EXPECT_NE(pDDNetCharacter, nullptr);
	if(!pDDNetCharacter)
		return;
	EXPECT_TRUE(pDDNetCharacter->GetCore().m_EndlessHook);
	EXPECT_TRUE(pDDNetCharacter->GetCore().m_Solo);
	CCharacterDDRace *pDDRaceCharacter = dynamic_cast<CCharacterDDRace *>(pDDNetCharacter);
	ASSERT_NE(pDDRaceCharacter, nullptr);
	EXPECT_EQ(pDDRaceCharacter->TuningZone(), MapTuneZone);
	EXPECT_FLOAT_EQ(pDDRaceCharacter->GetCore().m_Tuning.m_Gravity, ZoneGravity);
	EXPECT_EQ(pDDRaceCharacter->m_TuneZoneOld, -1);
	m_pServer->m_aClients[1].m_State = CServer::CClient::STATE_INGAME;
	GameServer()->m_apPlayers[1]->Tick();
	EXPECT_EQ(GameServer()->m_apPlayers[1]->m_TuneZone, MapTuneZone);
	auto *pDDNetProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, 1, TunePosition, vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_EQ(pDDNetProjectile->NetInfo().m_TuneZone, MapTuneZone);
	delete pDDNetProjectile;
	EXPECT_TRUE(pDDRaceCharacter->HasRaceTeams());
	pDDNetCharacter->Pause(true);
	pDDRaceCharacter->m_StartTime = GameServer()->Server()->Tick() + 1;
	pDDNetCharacter->PreTick();
	EXPECT_FALSE(pDDNetCharacter->IsAlive());
}

TEST_F(GameWorld, DDRaceStartWarningIsCharacterOwned)
{
	CPlayer *pPlayer = GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);

	EXPECT_TRUE(pCharacter->TryStartWarning());
	EXPECT_FALSE(pCharacter->TryStartWarning());
}

TEST_F(GameWorld, DDRaceSaveUsesPlayerNinjaJetpack)
{
	CPlayer *pPlayer = GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);

	pPlayer->m_NinjaJetpack = true;
	CSaveTee SavedTee;
	SavedTee.Save(pCharacter, false);
	pPlayer->m_NinjaJetpack = false;
	ASSERT_TRUE(SavedTee.Load(pCharacter));
	EXPECT_TRUE(pPlayer->m_NinjaJetpack);
}

TEST_F(GameWorld, DDRaceSaveIsBlockedByDraggerBeam)
{
	constexpr int ClientId = 0;
	constexpr int Team = 1;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	ASSERT_NE(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	RaceTeams().SetForceCharacterTeam(ClientId, Team);
	RaceTeams().ChangeTeamState(Team, ETeamState::STARTED);

	auto *pDragger = new CDragger(&GameServer()->m_World, vec2(64.0f, 64.0f), 1.0f, false);
	new CDraggerBeam(&GameServer()->m_World, pDragger, pDragger->GetPos(), 1.0f, false, ClientId, 0, 0);
	CSaveTeam SavedTeam;
	EXPECT_EQ(SavedTeam.Save(GameServer(), &RaceTeams(), Team, true, false), ESaveResult::DRAGGER_ACTIVE);
}

TEST_F(GameWorld, DDRaceRescueStateIsCharacterOwned)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);

	const int PreviousRescue = g_Config.m_SvRescue;
	const int PreviousRescueDelay = g_Config.m_SvRescueDelay;
	g_Config.m_SvRescue = 1;
	g_Config.m_SvRescueDelay = 0;

	const vec2 AutoPosition = pCharacter->m_Pos;
	const bool AutoRescueSet = pCharacter->TrySetRescue(RESCUEMODE_AUTO);
	pCharacter->m_Pos = vec2(72.0f, 96.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	const vec2 ManualPosition = pCharacter->m_Pos;
	const bool ManualRescueSet = pCharacter->TrySetRescue(RESCUEMODE_MANUAL);

	if(!AutoRescueSet || !ManualRescueSet)
	{
		g_Config.m_SvRescue = PreviousRescue;
		g_Config.m_SvRescueDelay = PreviousRescueDelay;
		FAIL() << "Failed to establish deterministic rescue positions";
	}

	auto &PlayerState = RaceTeams().PlayerState(ClientId);
	PlayerState.m_RescueMode = RESCUEMODE_AUTO;
	pCharacter->m_Pos = vec2(128.0f, 128.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	CCharacterCore Core = pCharacter->GetCore();
	Core.m_Vel = vec2(4.0f, 5.0f);
	Core.m_HookState = HOOK_GRABBED;
	pCharacter->SetCore(Core);
	pCharacter->m_StartTime = 1234;
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	EXPECT_TRUE(pCharacter->Rescue());
	EXPECT_EQ(pCharacter->m_Pos, AutoPosition);
	EXPECT_EQ(pCharacter->GetCore().m_Vel, vec2(0.0f, 0.0f));
	EXPECT_EQ(pCharacter->GetCore().m_HookState, HOOK_IDLE);
	EXPECT_EQ(pCharacter->m_StartTime, 1234);
	EXPECT_EQ(pCharacter->m_DDRaceState, ERaceState::STARTED);

	PlayerState.m_RescueMode = RESCUEMODE_MANUAL;
	pCharacter->m_Pos = vec2(160.0f, 128.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	EXPECT_TRUE(pCharacter->Rescue());
	EXPECT_EQ(pCharacter->m_Pos, ManualPosition);
	g_Config.m_SvRescueDelay = 1;
	const vec2 CooldownPosition(192.0f, 128.0f);
	pCharacter->m_Pos = CooldownPosition;
	pCharacter->SetPosition(pCharacter->m_Pos);
	EXPECT_FALSE(pCharacter->Rescue());
	EXPECT_EQ(pCharacter->m_Pos, CooldownPosition);

	g_Config.m_SvRescue = PreviousRescue;
	g_Config.m_SvRescueDelay = PreviousRescueDelay;
}

TEST_F(GameWorld, DDRaceSuperStateIsCharacterOwned)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);

	const int OriginalTeam = pCharacter->Team();
	pCharacter->SetInvincible(true);
	pCharacter->SetSuper(true);
	EXPECT_TRUE(pCharacter->IsSuper());
	EXPECT_FALSE(pCharacter->GetCore().m_Invincible);
	EXPECT_EQ(pCharacter->Team(), TEAM_SUPER);
	EXPECT_EQ(pCharacter->TeamBeforeSuper(), OriginalTeam);
	EXPECT_EQ(pCharacter->m_DDRaceState, ERaceState::CHEATED);

	pCharacter->SetInvincible(true);
	EXPECT_FALSE(pCharacter->IsSuper());
	EXPECT_TRUE(pCharacter->GetCore().m_Invincible);
	EXPECT_EQ(pCharacter->Team(), OriginalTeam);
}

TEST_F(GameWorld, ModeFactoriesOwnConcretePlayerAndCharacterTypes)
{
	constexpr int ClientId = 0;
	const CGameModeInfo Info = {"factory.test", "Factory Test", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	int PlayerDestructions = 0;
	int CharacterDestructions = 0;
	CFactoryController Controller(GameServices(), Info, &PlayerDestructions, &CharacterDestructions);

	CPlayer *pPlayer = Controller.CreatePlayer(1, ClientId, TEAM_GAME);
	ASSERT_NE(dynamic_cast<CFactoryPlayer *>(pPlayer), nullptr);
	EXPECT_GT(sizeof(CFactoryPlayer), sizeof(CPlayer));

	CCharacter *pCharacter = Controller.CreateCharacter(pPlayer);
	ASSERT_NE(dynamic_cast<CFactoryCharacter *>(pCharacter), nullptr);
	EXPECT_GT(sizeof(CFactoryCharacter), sizeof(CCharacter));

	delete pCharacter;
	EXPECT_EQ(CharacterDestructions, 1);
	EXPECT_EQ(PlayerDestructions, 0);
	delete pPlayer;
	EXPECT_EQ(PlayerDestructions, 1);
}

TEST_F(GameWorld, CharacterDeathTransactionIsModeOwned)
{
	constexpr int VictimId = 0;
	constexpr int OtherId = 1;
	CPlayer *pVictimPlayer = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	CPlayer *pOtherPlayer = GameServer()->CreatePlayer(OtherId, TEAM_GAME, false, -1);
	ASSERT_NE(pVictimPlayer, nullptr);
	ASSERT_NE(pOtherPlayer, nullptr);
	CCharacterDDRace *pVictim = dynamic_cast<CCharacterDDRace *>(pVictimPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pVictim, nullptr);
	const int PreviousRescue = g_Config.m_SvRescue;
	g_Config.m_SvRescue = 1;
	const bool RescueSet = pVictim->TrySetRescue(RESCUEMODE_AUTO);
	g_Config.m_SvRescue = PreviousRescue;
	ASSERT_TRUE(RescueSet);
	const vec2 RescuePosition = pVictim->GetLastRescueTeeRef().GetPos();

	auto &VictimState = RaceTeams().PlayerState(VictimId);
	auto &OtherState = RaceTeams().PlayerState(OtherId);
	VictimState.m_SwapTargetClientId = OtherId;
	OtherState.m_SwapTargetClientId = VictimId;
	pVictim->Die(VictimId, WEAPON_SELF, false);

	EXPECT_FALSE(pVictim->IsAlive());
	EXPECT_EQ(GameServer()->m_World.m_Core.m_apCharacters[VictimId], nullptr);
	ASSERT_TRUE(VictimState.m_LastDeath.has_value());
	EXPECT_EQ(VictimState.m_LastDeath->GetPos(), RescuePosition);
	EXPECT_EQ(VictimState.m_SwapTargetClientId, -1);
	EXPECT_EQ(OtherState.m_SwapTargetClientId, -1);
}

TEST_F(GameWorld, RegistryFactoriesReceiveHostServices)
{
	const CGameModeInfo Info = {"services.test", "Services Test", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameModeRegistry Registry;
	ASSERT_TRUE(Registry.Register(Info, CreateServicesFactoryController));

	std::unique_ptr<IGameController> pController = Registry.Create(Info.m_pId, GameServices());
	ASSERT_NE(pController, nullptr);
	auto *pServicesController = dynamic_cast<CServicesFactoryController *>(pController.get());
	ASSERT_NE(pServicesController, nullptr);
	EXPECT_EQ(pServicesController->m_pConstructionServices, &GameServer()->GameHost().Services());
	EXPECT_STREQ(pController->Info().m_pId, Info.m_pId);
	EXPECT_EQ(Registry.Create("missing", GameServices()), nullptr);

	ASSERT_FALSE(GameController()->IsValidTeam(TEAM_BLUE));
	ASSERT_TRUE(pController->IsValidTeam(TEAM_BLUE));
	std::unique_ptr<CPlayer> pPlayer(pController->CreatePlayer(1, 0, TEAM_BLUE));
	ASSERT_NE(pPlayer, nullptr);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_BLUE);
}

TEST_F(GameWorld, PlayerAutoRespawnPolicyIsModeOwned)
{
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[0];
	ASSERT_NE(pPlayer, nullptr);
	pPlayer->m_PreviousDieTick = 100;
	pPlayer->m_DieTick = 101;

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	EXPECT_EQ(VanillaController.PlayerAutoRespawnTick(pPlayer), pPlayer->m_DieTick + 2);

	const int DDNetRespawnTick = pPlayer->m_PreviousDieTick + GameServer()->Server()->TickSpeed() * 3 + 2;
	const CGameModeInfo DDNetInfo = {"ddnet", "DDNet", "DDraceNetwork", "TestDDraceNetwork", EGameModeScoreKind::TIME, protocol7::GAMEFLAG_RACE, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerDDNet DDNetController(GameServices(), DDNetInfo);
	EXPECT_EQ(DDNetController.PlayerAutoRespawnTick(pPlayer), DDNetRespawnTick);

	const CGameModeInfo ModInfo = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerMod ModController(GameServices(), ModInfo);
	EXPECT_EQ(ModController.PlayerAutoRespawnTick(pPlayer), DDNetRespawnTick);
}

TEST_F(GameWorld, PlayerSetTeamOperationIsModeOwned)
{
	constexpr int ClientId = 0;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	const CGameModeInfo DDNetInfo = {"ddnet", "DDNet", "DDraceNetwork", "TestDDraceNetwork", EGameModeScoreKind::TIME, protocol7::GAMEFLAG_RACE, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerDDNet DDNetController(GameServices(), DDNetInfo);
	const CGameModeInfo ModInfo = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerMod ModController(GameServices(), ModInfo);

	const int PreviousKillProtection = g_Config.m_SvKillProtection;
	const int PreviousSpamProtection = g_Config.m_SvSpamprotection;
	g_Config.m_SvKillProtection = 1;
	g_Config.m_SvSpamprotection = 0;

	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60;
	VanillaController.OnPlayerSetTeam(ClientId, TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_SPECTATORS);

	pPlayer->SetTeam(TEAM_GAME, false);
	pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	if(!pCharacter)
	{
		g_Config.m_SvKillProtection = PreviousKillProtection;
		g_Config.m_SvSpamprotection = PreviousSpamProtection;
		FAIL() << "failed to respawn test character";
		return;
	}
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60;
	DDNetController.OnPlayerSetTeam(ClientId, TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_GAME);

	ModController.OnPlayerSetTeam(ClientId, TEAM_SPECTATORS);
	EXPECT_EQ(pPlayer->GetTeam(), TEAM_GAME);

	g_Config.m_SvKillProtection = PreviousKillProtection;
	g_Config.m_SvSpamprotection = PreviousSpamProtection;
}

TEST_F(GameWorld, PlayerKillOperationIsModeOwned)
{
	constexpr int ClientId = 0;
	const int PreviousKillProtection = g_Config.m_SvKillProtection;
	const int PreviousKillDelay = g_Config.m_SvKillDelay;
	g_Config.m_SvKillProtection = 1;
	g_Config.m_SvKillDelay = 0;

	SelectGameMode("vanilla.dm");
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacter *pVanillaCharacter = pPlayer->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pVanillaCharacter, nullptr);
	GameController()->OnPlayerKill(ClientId);
	EXPECT_EQ(pPlayer->GetCharacter(), nullptr);

	delete GameServer()->m_apPlayers[ClientId];
	GameServer()->m_apPlayers[ClientId] = nullptr;
	SelectGameMode("mod");
	pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pRaceCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pRaceCharacter, nullptr);
	pRaceCharacter->m_DDRaceState = ERaceState::STARTED;
	pRaceCharacter->m_StartTime = GameServer()->Server()->Tick() - GameServer()->Server()->TickSpeed() * 60;
	GameController()->OnPlayerKill(ClientId);
	EXPECT_EQ(pPlayer->GetCharacter(), pRaceCharacter);
	EXPECT_TRUE(pRaceCharacter->IsAlive());

	g_Config.m_SvKillProtection = PreviousKillProtection;
	g_Config.m_SvKillDelay = PreviousKillDelay;
}

TEST_F(GameWorld, TargetVoteOperationsAreModeOwned)
{
	constexpr int CallerId = 0;
	constexpr int TargetId = 1;
	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	const CGameModeInfo ModInfo = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerMod ModController(GameServices(), ModInfo);

	const int PreviousVoteKickMin = g_Config.m_SvVoteKickMin;
	const int PreviousVoteKickBantime = g_Config.m_SvVoteKickBantime;
	const int PreviousPauseable = g_Config.m_SvPauseable;
	const int PreviousVotePause = g_Config.m_SvVotePause;
	const int PreviousVoteSpectateRejoinDelay = g_Config.m_SvVoteSpectateRejoindelay;
	g_Config.m_SvVoteKickMin = 0;
	g_Config.m_SvVoteKickBantime = 0;
	g_Config.m_SvPauseable = 0;
	g_Config.m_SvVotePause = 0;
	g_Config.m_SvVoteSpectateRejoindelay = 3;

	CPlayer *pCaller = GameServer()->CreatePlayer(CallerId, TEAM_GAME, false, -1);
	CPlayer *pTarget = GameServer()->CreatePlayer(TargetId, TEAM_GAME, false, -1);
	ASSERT_NE(pCaller, nullptr);
	ASSERT_NE(pTarget, nullptr);

	VanillaController.OnPlayerCallKickVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_KICK);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "kick 1 Kicked by vote");
	GameServer()->EndVote();

	VanillaController.OnPlayerCallSpectateVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_SPECTATE);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "set_team 1 -1 3");
	GameServer()->EndVote();

	CCharacter *pCallerCharacter = pCaller->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pTargetCharacter = pTarget->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pCallerCharacter, nullptr);
	ASSERT_NE(pTargetCharacter, nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(CallerId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(TargetId, 1);

	ModController.OnPlayerCallKickVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_KICK);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "uninvite 1 1; set_team_ddr 1 0");
	GameServer()->EndVote();

	ModController.OnPlayerCallSpectateVote(CallerId, TargetId, "test");
	EXPECT_EQ(GameServer()->m_VoteType, CGameContext::VOTE_TYPE_SPECTATE);
	EXPECT_EQ(GameServer()->m_VoteVictim, TargetId);
	EXPECT_STREQ(GameServer()->m_aVoteCommand, "uninvite 1 1; set_team 1 -1 3");
	GameServer()->EndVote();

	g_Config.m_SvVoteKickMin = PreviousVoteKickMin;
	g_Config.m_SvVoteKickBantime = PreviousVoteKickBantime;
	g_Config.m_SvPauseable = PreviousPauseable;
	g_Config.m_SvVotePause = PreviousVotePause;
	g_Config.m_SvVoteSpectateRejoindelay = PreviousVoteSpectateRejoinDelay;
}

TEST_F(GameWorld, TargetVoteAudienceIsModeOwned)
{
	constexpr int CreatorId = 0;
	constexpr int VoterId = 1;
	constexpr int SpectatorId = 2;
	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	const CGameModeInfo ModInfo = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerMod ModController(GameServices(), ModInfo);

	CPlayer *pCreator = GameServer()->CreatePlayer(CreatorId, TEAM_GAME, false, -1);
	CPlayer *pVoter = GameServer()->CreatePlayer(VoterId, TEAM_GAME, false, -1);
	CPlayer *pSpectator = GameServer()->CreatePlayer(SpectatorId, TEAM_SPECTATORS, false, -1);
	ASSERT_NE(pCreator, nullptr);
	ASSERT_NE(pVoter, nullptr);
	ASSERT_NE(pSpectator, nullptr);
	CCharacter *pCreatorCharacter = pCreator->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVoterCharacter = pVoter->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pCreatorCharacter, nullptr);
	ASSERT_NE(pVoterCharacter, nullptr);

	ModController.RaceTeams().SetForceCharacterTeam(CreatorId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(VoterId, 2);

	EXPECT_TRUE(VanillaController.CanPlayerVoteOnTargetVote(CreatorId, VoterId));
	EXPECT_FALSE(VanillaController.CanPlayerVoteOnTargetVote(CreatorId, SpectatorId));
	EXPECT_FALSE(ModController.CanPlayerVoteOnTargetVote(CreatorId, VoterId));
	EXPECT_FALSE(ModController.CanPlayerVoteOnTargetVote(CreatorId, SpectatorId));

	ModController.RaceTeams().SetForceCharacterTeam(VoterId, 1);
	EXPECT_TRUE(ModController.CanPlayerVoteOnTargetVote(CreatorId, VoterId));
}

TEST_F(GameWorld, PlayerVetoActivityIsModeOwned)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	const CGameModeInfo ModInfo = {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerMod ModController(GameServices(), ModInfo);

	const int Now = GameServer()->Server()->Tick();
	pPlayer->m_JoinTick = Now - GameServer()->Server()->TickSpeed() * 5 * 60;
	pCharacter->m_DDRaceState = ERaceState::STARTED;
	pCharacter->m_StartTime = Now - GameServer()->Server()->TickSpeed() * 30 * 60;

	EXPECT_EQ(VanillaController.PlayerVetoActivityStartTick(ClientId), pPlayer->m_JoinTick);
	EXPECT_EQ(ModController.PlayerVetoActivityStartTick(ClientId), pCharacter->m_StartTime);

	pCharacter->m_DDRaceState = ERaceState::NONE;
	EXPECT_EQ(ModController.PlayerVetoActivityStartTick(ClientId), pPlayer->m_JoinTick);
}

TEST_F(GameWorld, PlayerVisibilityPolicyIsModeOwned)
{
	constexpr int TargetId = 0;
	constexpr int ViewerId = 1;
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerMod *>(GameController());
	GameServer()->CreatePlayer(TargetId, TEAM_GAME, false, -1);
	GameServer()->CreatePlayer(ViewerId, TEAM_GAME, false, -1);
	CCharacterDDRace *pTarget = dynamic_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[TargetId]->ForceSpawn(vec2(64.0f, 96.0f)));
	CCharacterDDRace *pViewer = dynamic_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[ViewerId]->ForceSpawn(vec2(96.0f, 96.0f)));
	ASSERT_NE(pTarget, nullptr);
	ASSERT_NE(pViewer, nullptr);

	const int PreviousShowOthers = g_Config.m_SvShowOthers;
	const int PreviousShowOthersDefault = g_Config.m_SvShowOthersDefault;
	g_Config.m_SvShowOthers = 1;
	g_Config.m_SvShowOthersDefault = SHOW_OTHERS_ONLY_TEAM;

	ModController.RaceTeams().SetForceCharacterTeam(TargetId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 2);

	g_Config.m_SvShowOthersDefault = SHOW_OTHERS_OFF;
	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_OFF;
	EXPECT_FALSE(pTarget->CanSnapCharacter(ViewerId));
	ModController.OnPlayerShowOthers(ViewerId, SHOW_OTHERS_ON);
	EXPECT_TRUE(pTarget->CanSnapCharacter(ViewerId));

	g_Config.m_SvShowOthers = PreviousShowOthers;
	g_Config.m_SvShowOthersDefault = PreviousShowOthersDefault;
}

TEST_F(GameWorld, EntityInteractionPolicyIsModeOwned)
{
	constexpr int OwnerId = 0;
	constexpr int ViewerId = 1;
	constexpr int SpectatorId = 2;
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerMod *>(GameController());
	CPlayer *pOwner = GameServer()->CreatePlayer(OwnerId, TEAM_GAME, false, -1);
	ASSERT_NE(pOwner, nullptr);
	CPlayer *pViewer = GameServer()->CreatePlayer(ViewerId, TEAM_GAME, false, -1);
	CPlayer *pSpectator = GameServer()->CreatePlayer(SpectatorId, TEAM_SPECTATORS, false, -1);
	ASSERT_NE(pViewer, nullptr);
	ASSERT_NE(pSpectator, nullptr);
	CCharacter *pOwnerCharacter = pOwner->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pViewerCharacter = pViewer->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pOwnerCharacter, nullptr);
	ASSERT_NE(pViewerCharacter, nullptr);

	CInteractions Interaction;
	Interaction.Init(OwnerId, pOwner->GetUniqueCid());
	Interaction.FillOwnerConnected(1, false, false, false);

	ModController.RaceTeams().SetForceCharacterTeam(OwnerId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 2);
	ModController.RaceTeams().SetForceCharacterTeam(SpectatorId, 2);
	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_OFF;

	EXPECT_FALSE(ModController.CanCharacterHitCharacter(pOwnerCharacter, pViewerCharacter));
	EXPECT_FALSE(Interaction.CanSee(GameServer(), ViewerId));
	EXPECT_FALSE(Interaction.CanHit(GameServer(), ViewerId));
	EXPECT_FALSE(Interaction.CanSeeMask(GameServer()).test(ViewerId));
	EXPECT_FALSE(Interaction.CanHitMask(GameServer()).test(ViewerId));
	EXPECT_TRUE(Interaction.CanSee(GameServer(), OwnerId));
	auto *pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, OwnerId, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_FALSE(pProjectile->CanCollide(ViewerId));
	auto *pOwnerlessProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, -1, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 10, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_FALSE(pOwnerlessProjectile->CanCollide(ViewerId));

	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_ONLY_TEAM;
	EXPECT_FALSE(Interaction.CanSee(GameServer(), ViewerId));
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 1);
	EXPECT_TRUE(ModController.CanCharacterHitCharacter(pOwnerCharacter, pViewerCharacter));
	EXPECT_TRUE(Interaction.CanSee(GameServer(), ViewerId));
	EXPECT_TRUE(pProjectile->CanCollide(ViewerId));
	EXPECT_FALSE(pOwnerlessProjectile->CanCollide(ViewerId));
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, TEAM_FLOCK);
	EXPECT_TRUE(pOwnerlessProjectile->CanCollide(ViewerId));
	ModController.RaceTeams().SetForceCharacterTeam(ViewerId, 1);
	ModController.RaceTeams().PlayerState(ViewerId).m_ShowOthers = SHOW_OTHERS_OFF;
	pViewerCharacter->SetSolo(true);
	EXPECT_FALSE(Interaction.CanSee(GameServer(), ViewerId));
	pViewerCharacter->SetSolo(false);

	ModController.RaceTeams().PlayerState(SpectatorId).m_SpecTeam = true;
	EXPECT_FALSE(Interaction.CanSee(GameServer(), SpectatorId));
	pSpectator->SetSpectatorId(OwnerId);
	EXPECT_TRUE(Interaction.CanSee(GameServer(), SpectatorId));
	pSpectator->SetSpectatorId(ViewerId);
	EXPECT_TRUE(Interaction.CanSee(GameServer(), SpectatorId));
	pViewer->KillCharacter();
	EXPECT_FALSE(Interaction.CanSee(GameServer(), SpectatorId));

	CInteractions NoHitOthers;
	NoHitOthers.Init(OwnerId, pOwner->GetUniqueCid());
	NoHitOthers.FillOwnerConnected(0, false, true, false);
	EXPECT_TRUE(NoHitOthers.CanHit(GameServer(), OwnerId));
	EXPECT_FALSE(NoHitOthers.CanHit(GameServer(), ViewerId));
	CInteractions NoHitSelf;
	NoHitSelf.Init(OwnerId, pOwner->GetUniqueCid());
	NoHitSelf.FillOwnerConnected(0, false, false, true);
	EXPECT_FALSE(NoHitSelf.CanHit(GameServer(), OwnerId));
	EXPECT_TRUE(NoHitSelf.CanHit(GameServer(), ViewerId));

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	EXPECT_TRUE(VanillaController.CanCharacterHitCharacter(pOwnerCharacter, pOwnerCharacter));
	EXPECT_TRUE(VanillaController.CanSeeInteraction(Interaction, ViewerId));
	EXPECT_TRUE(VanillaController.CanHitInteraction(Interaction, ViewerId));

	delete pProjectile;
	delete pOwnerlessProjectile;
}

TEST_F(GameWorld, PlayerTeamGroupIsModeOwned)
{
	constexpr int RedOne = 0;
	constexpr int RedTwo = 1;
	constexpr int Blue = 2;
	SelectGameMode("vanilla.tdm");
	auto &TdmController = *dynamic_cast<CGameControllerVanillaTDM *>(GameController());

	CPlayer *pRedOne = GameServer()->CreatePlayer(RedOne, TEAM_RED, false, -1);
	CPlayer *pRedTwo = GameServer()->CreatePlayer(RedTwo, TEAM_RED, false, -1);
	CPlayer *pBlue = GameServer()->CreatePlayer(Blue, TEAM_BLUE, false, -1);
	ASSERT_NE(pRedOne, nullptr);
	ASSERT_NE(pRedTwo, nullptr);
	ASSERT_NE(pBlue, nullptr);
	CCharacter *pRedOneCharacter = pRedOne->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pRedTwoCharacter = pRedTwo->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pBlueCharacter = pBlue->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pRedOneCharacter, nullptr);
	ASSERT_NE(pRedTwoCharacter, nullptr);
	ASSERT_NE(pBlueCharacter, nullptr);

	EXPECT_EQ(TdmController.PlayerTeamGroup(RedOne), TEAM_RED);
	EXPECT_EQ(TdmController.PlayerTeamGroup(RedTwo), TEAM_RED);
	EXPECT_EQ(TdmController.PlayerTeamGroup(Blue), TEAM_BLUE);
	CJsonStringWriter TdmServerInfoWriter;
	TdmServerInfoWriter.BeginObject();
	GameServer()->OnUpdatePlayerServerInfo(&TdmServerInfoWriter, Blue);
	TdmServerInfoWriter.EndObject();
	const std::string TdmServerInfo = TdmServerInfoWriter.GetOutputString();
	EXPECT_NE(TdmServerInfo.find("\"team\": 1"), std::string::npos);

	DeletePlayers();
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerMod *>(GameController());
	pRedOne = GameServer()->CreatePlayer(RedOne, TEAM_GAME, false, -1);
	pRedTwo = GameServer()->CreatePlayer(RedTwo, TEAM_GAME, false, -1);
	pBlue = GameServer()->CreatePlayer(Blue, TEAM_GAME, false, -1);
	ASSERT_NE(pRedOne->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(pRedTwo->ForceSpawn(vec2(96.0f, 96.0f)), nullptr);
	ASSERT_NE(pBlue->ForceSpawn(vec2(128.0f, 96.0f)), nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(RedOne, 1);
	ModController.RaceTeams().SetForceCharacterTeam(RedTwo, 1);
	ModController.RaceTeams().SetForceCharacterTeam(Blue, 2);
	EXPECT_EQ(ModController.PlayerTeamGroup(RedOne), 1);
	EXPECT_EQ(ModController.PlayerTeamGroup(RedTwo), 1);
	EXPECT_EQ(ModController.PlayerTeamGroup(Blue), 2);
	CJsonStringWriter RaceServerInfoWriter;
	RaceServerInfoWriter.BeginObject();
	GameServer()->OnUpdatePlayerServerInfo(&RaceServerInfoWriter, Blue);
	RaceServerInfoWriter.EndObject();
	const std::string RaceServerInfo = RaceServerInfoWriter.GetOutputString();
	EXPECT_NE(RaceServerInfo.find("\"team\": 2"), std::string::npos);
}

TEST_F(GameWorld, WorldEventAudienceIsModeOwned)
{
	constexpr int Red = 0;
	constexpr int Blue = 1;
	SelectGameMode("vanilla.tdm");

	CPlayer *pRed = GameServer()->CreatePlayer(Red, TEAM_RED, false, -1);
	CPlayer *pBlue = GameServer()->CreatePlayer(Blue, TEAM_BLUE, false, -1);
	ASSERT_NE(pRed, nullptr);
	ASSERT_NE(pBlue, nullptr);
	CCharacter *pRedCharacter = pRed->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pBlueCharacter = pBlue->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pRedCharacter, nullptr);
	ASSERT_NE(pBlueCharacter, nullptr);

	EXPECT_TRUE(pRedCharacter->TeamMask().test(Red));
	EXPECT_TRUE(pRedCharacter->TeamMask().test(Blue));

	DeletePlayers();
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerMod *>(GameController());
	pRed = GameServer()->CreatePlayer(Red, TEAM_GAME, false, -1);
	pBlue = GameServer()->CreatePlayer(Blue, TEAM_GAME, false, -1);
	pRedCharacter = pRed->ForceSpawn(vec2(64.0f, 96.0f));
	pBlueCharacter = pBlue->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pRedCharacter, nullptr);
	ASSERT_NE(pBlueCharacter, nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(Red, 1);
	ModController.RaceTeams().SetForceCharacterTeam(Blue, 2);
	ModController.RaceTeams().PlayerState(Blue).m_ShowOthers = SHOW_OTHERS_OFF;

	EXPECT_TRUE(pRedCharacter->TeamMask().test(Red));
	EXPECT_FALSE(pRedCharacter->TeamMask().test(Blue));
}

TEST_F(GameWorld, PreInputAudienceIsModeOwned)
{
	constexpr int SenderId = 0;
	constexpr int AllyId = 1;
	constexpr int OpponentId = 2;
	SelectGameMode("vanilla.tdm");

	CPlayer *pSender = GameServer()->CreatePlayer(SenderId, TEAM_RED, false, -1);
	CPlayer *pAlly = GameServer()->CreatePlayer(AllyId, TEAM_RED, false, -1);
	CPlayer *pOpponent = GameServer()->CreatePlayer(OpponentId, TEAM_BLUE, false, -1);
	ASSERT_NE(pSender, nullptr);
	ASSERT_NE(pAlly, nullptr);
	ASSERT_NE(pOpponent, nullptr);
	CCharacter *pSenderCharacter = pSender->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pAllyCharacter = pAlly->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pOpponentCharacter = pOpponent->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pSenderCharacter, nullptr);
	ASSERT_NE(pAllyCharacter, nullptr);
	ASSERT_NE(pOpponentCharacter, nullptr);

	const int PreviousAllyState = m_pServer->m_aClients[AllyId].m_State;
	const int PreviousAllyVersion = m_pServer->m_aClients[AllyId].m_DDNetVersion;
	const bool PreviousAllyVersionSettled = m_pServer->m_aClients[AllyId].m_DDNetVersionSettled;
	const int PreviousOpponentState = m_pServer->m_aClients[OpponentId].m_State;
	const int PreviousOpponentVersion = m_pServer->m_aClients[OpponentId].m_DDNetVersion;
	const bool PreviousOpponentVersionSettled = m_pServer->m_aClients[OpponentId].m_DDNetVersionSettled;
	m_pServer->m_aClients[AllyId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->m_aClients[OpponentId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->SetClientDDNetVersion(AllyId, VERSION_DDNET_PREINPUT);
	m_pServer->SetClientDDNetVersion(OpponentId, VERSION_DDNET_PREINPUT);
	bool aTdmClients[MAX_CLIENTS] = {};
	GameServer()->PreInputClients(SenderId, aTdmClients);
	EXPECT_TRUE(aTdmClients[AllyId]);
	EXPECT_TRUE(aTdmClients[OpponentId]);

	DeletePlayers();
	SelectGameMode("mod");
	auto &ModController = *dynamic_cast<CGameControllerMod *>(GameController());
	pSender = GameServer()->CreatePlayer(SenderId, TEAM_GAME, false, -1);
	pAlly = GameServer()->CreatePlayer(AllyId, TEAM_GAME, false, -1);
	pOpponent = GameServer()->CreatePlayer(OpponentId, TEAM_GAME, false, -1);
	ASSERT_NE(pSender->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	ASSERT_NE(pAlly->ForceSpawn(vec2(96.0f, 96.0f)), nullptr);
	ASSERT_NE(pOpponent->ForceSpawn(vec2(128.0f, 96.0f)), nullptr);
	ModController.RaceTeams().SetForceCharacterTeam(SenderId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(AllyId, 1);
	ModController.RaceTeams().SetForceCharacterTeam(OpponentId, 2);
	ModController.RaceTeams().PlayerState(AllyId).m_ShowOthers = SHOW_OTHERS_ON;
	ModController.RaceTeams().PlayerState(OpponentId).m_ShowOthers = SHOW_OTHERS_ON;

	bool aRaceClients[MAX_CLIENTS] = {};
	GameServer()->PreInputClients(SenderId, aRaceClients);
	EXPECT_TRUE(aRaceClients[AllyId]);
	EXPECT_FALSE(aRaceClients[OpponentId]);

	m_pServer->m_aClients[AllyId].m_State = PreviousAllyState;
	m_pServer->m_aClients[AllyId].m_DDNetVersion = PreviousAllyVersion;
	m_pServer->m_aClients[AllyId].m_DDNetVersionSettled = PreviousAllyVersionSettled;
	m_pServer->m_aClients[OpponentId].m_State = PreviousOpponentState;
	m_pServer->m_aClients[OpponentId].m_DDNetVersion = PreviousOpponentVersion;
	m_pServer->m_aClients[OpponentId].m_DDNetVersionSettled = PreviousOpponentVersionSettled;
}

TEST_F(GameWorld, PlayerSnapshotContributionsAreModeOwned)
{
	constexpr int ClientId = 0;
	SelectGameMode("vanilla.dm");
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);

	const int PreviousClientState = m_pServer->m_aClients[ClientId].m_State;
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;

	m_pServer->m_SnapshotBuilder.Init();
	pPlayer->Snap(ClientId);
	CSnapshotBuffer VanillaBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaBuffer);
	const CSnapshot *pVanillaSnapshot = VanillaBuffer.AsSnapshot();
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_CLIENTINFO, ClientId), nullptr);
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_PLAYERINFO, ClientId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETPLAYER, ClientId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETSPECTATORINFO, ClientId), nullptr);

	DeletePlayers();
	SelectGameMode("mod");
	pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	m_pServer->m_SnapshotBuilder.Init();
	pPlayer->Snap(ClientId);
	CSnapshotBuffer RaceBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceBuffer);
	const CSnapshot *pRaceSnapshot = RaceBuffer.AsSnapshot();
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_CLIENTINFO, ClientId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_PLAYERINFO, ClientId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETPLAYER, ClientId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETSPECTATORINFO, ClientId), nullptr);

	m_pServer->m_aClients[ClientId].m_State = PreviousClientState;
}

TEST_F(GameWorld, CharacterSnapshotContributionsAreModeOwned)
{
	constexpr int ClientId = 0;
	SelectGameMode("vanilla.dm");
	const int PreviousClientState = m_pServer->m_aClients[ClientId].m_State;
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;

	CPlayer *pVanillaPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CCharacter *pVanillaCharacter = pVanillaPlayer->ForceSpawn(vec2(64.0f, 96.0f));
	EXPECT_NE(pVanillaCharacter, nullptr);
	if(!pVanillaCharacter)
	{
		delete GameServer()->m_apPlayers[ClientId];
		GameServer()->m_apPlayers[ClientId] = nullptr;
		m_pServer->m_aClients[ClientId].m_State = PreviousClientState;
		return;
	}
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	m_pServer->m_SnapshotBuilder.Init();
	pVanillaCharacter->Snap(ClientId);
	CSnapshotBuffer VanillaBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaBuffer);
	const CSnapshot *pVanillaSnapshot = VanillaBuffer.AsSnapshot();
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_CHARACTER, ClientId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETCHARACTER, ClientId), nullptr);

	delete GameServer()->m_apPlayers[ClientId];
	GameServer()->m_apPlayers[ClientId] = nullptr;
	SelectGameMode("ddnet");
	CPlayer *pRacePlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CCharacter *pRaceCharacter = pRacePlayer->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pRaceCharacter, nullptr);
	EXPECT_NE(dynamic_cast<CCharacterDDRace *>(pRaceCharacter), nullptr);
	m_pServer->m_SnapshotBuilder.Init();
	pRaceCharacter->Snap(ClientId);
	CSnapshotBuffer RaceBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceBuffer);
	const CSnapshot *pRaceSnapshot = RaceBuffer.AsSnapshot();
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_CHARACTER, ClientId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETCHARACTER, ClientId), nullptr);

	m_pServer->m_aClients[ClientId].m_State = PreviousClientState;
}

TEST_F(GameWorld, EntitySnapshotFormatsAreModeOwned)
{
	constexpr int ClientId = 0;
	constexpr int LaserId = 100;
	constexpr int PickupId = 101;
	SelectGameMode("vanilla.dm");
	ASSERT_NE(GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1), nullptr);
	auto *pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, -1, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 100, false, false, -1, vec2(1.0f, 0.0f));
	ASSERT_TRUE(pProjectile->GetId().has_value());
	const int ProjectileId = pProjectile->GetId().value();

	const int PreviousClientState = m_pServer->m_aClients[ClientId].m_State;
	const int PreviousClientVersion = m_pServer->m_aClients[ClientId].m_DDNetVersion;
	const bool PreviousClientVersionSettled = m_pServer->m_aClients[ClientId].m_DDNetVersionSettled;
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;
	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_ENTITY_NETOBJS);
	const CSnapContext SnapContext(VERSION_DDNET_ENTITY_NETOBJS, false);

	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->SnapLaserObject(SnapContext, LaserId, vec2(32.0f, 32.0f), vec2(64.0f, 32.0f), 1);
	GameServer()->SnapPickup(SnapContext, PickupId, vec2(32.0f, 64.0f), POWERUP_WEAPON, WEAPON_SHOTGUN, 0, 0);
	pProjectile->Snap(ClientId);
	CSnapshotBuffer VanillaBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaBuffer);
	const CSnapshot *pVanillaSnapshot = VanillaBuffer.AsSnapshot();
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_LASER, LaserId), nullptr);
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_PICKUP, PickupId), nullptr);
	EXPECT_NE(pVanillaSnapshot->FindItem(NETOBJTYPE_PROJECTILE, ProjectileId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETLASER, LaserId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETPICKUP, PickupId), nullptr);
	EXPECT_EQ(pVanillaSnapshot->FindItem(NETOBJTYPE_DDNETPROJECTILE, ProjectileId), nullptr);

	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_MSG_LEGACY);
	m_pServer->m_SnapshotBuilder.Init();
	pProjectile->Snap(ClientId);
	CSnapshotBuffer VanillaLegacyBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&VanillaLegacyBuffer);
	const CSnapshot *pVanillaLegacySnapshot = VanillaLegacyBuffer.AsSnapshot();
	EXPECT_NE(pVanillaLegacySnapshot->FindItem(NETOBJTYPE_PROJECTILE, ProjectileId), nullptr);
	EXPECT_EQ(pVanillaLegacySnapshot->FindItem(NETOBJTYPE_DDRACEPROJECTILE, ProjectileId), nullptr);

	delete pProjectile;
	DeletePlayers();
	SelectGameMode("mod");
	ASSERT_NE(GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1), nullptr);
	pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, -1, vec2(64.0f, 96.0f), vec2(1.0f, 0.0f), 100, false, false, -1, vec2(1.0f, 0.0f));
	ASSERT_TRUE(pProjectile->GetId().has_value());
	const int RaceProjectileId = pProjectile->GetId().value();
	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_ENTITY_NETOBJS);
	m_pServer->m_SnapshotBuilder.Init();
	GameServer()->SnapLaserObject(SnapContext, LaserId, vec2(32.0f, 32.0f), vec2(64.0f, 32.0f), 1);
	GameServer()->SnapPickup(SnapContext, PickupId, vec2(32.0f, 64.0f), POWERUP_WEAPON, WEAPON_SHOTGUN, 0, 0);
	pProjectile->Snap(ClientId);
	CSnapshotBuffer RaceBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceBuffer);
	const CSnapshot *pRaceSnapshot = RaceBuffer.AsSnapshot();
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETLASER, LaserId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETPICKUP, PickupId), nullptr);
	EXPECT_NE(pRaceSnapshot->FindItem(NETOBJTYPE_DDNETPROJECTILE, RaceProjectileId), nullptr);

	m_pServer->SetClientDDNetVersion(ClientId, VERSION_DDNET_MSG_LEGACY);
	m_pServer->m_SnapshotBuilder.Init();
	pProjectile->Snap(ClientId);
	CSnapshotBuffer RaceLegacyBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&RaceLegacyBuffer);
	const CSnapshot *pRaceLegacySnapshot = RaceLegacyBuffer.AsSnapshot();
	EXPECT_NE(pRaceLegacySnapshot->FindItem(NETOBJTYPE_DDRACEPROJECTILE, RaceProjectileId), nullptr);

	m_pServer->m_aClients[ClientId].m_State = PreviousClientState;
	m_pServer->m_aClients[ClientId].m_DDNetVersion = PreviousClientVersion;
	m_pServer->m_aClients[ClientId].m_DDNetVersionSettled = PreviousClientVersionSettled;
}

TEST_F(GameWorld, HotReloadStateIsModeOwned)
{
	GameServer()->CreatePlayer(0, TEAM_GAME, false, -1);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[0]->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);
	EXPECT_NE(dynamic_cast<CCharacterDDRace *>(pCharacter), nullptr);

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	EXPECT_FALSE(VanillaController.SaveStateForMapReload());
	EXPECT_EQ(GameServer()->GameHost().MapReloadState(), nullptr);

	const CGameModeInfo DDNetInfo = {"ddnet", "DDNet", "DDraceNetwork", "TestDDraceNetwork", EGameModeScoreKind::TIME, protocol7::GAMEFLAG_RACE, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerDDNet DDNetController(GameServices(), DDNetInfo);
	auto &PlayerState = RaceTeams().PlayerState(0);
	EXPECT_FALSE(PlayerState.m_LastTeleTee.has_value());
	EXPECT_FALSE(PlayerState.m_LastDeath.has_value());
	const vec2 SavedPosition = pCharacter->m_Pos;
	std::unique_ptr<IGameModeMapReloadState> pState = DDNetController.SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));

	PlayerState.m_LastTeleTee.emplace();
	PlayerState.m_LastDeath.emplace();
	pCharacter->m_Pos = vec2(320.0f, 320.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	DDNetController.RestoreCharacterAfterMapReload(pCharacter);
	EXPECT_EQ(pCharacter->m_Pos, SavedPosition);
	EXPECT_FALSE(PlayerState.m_LastTeleTee.has_value());
	EXPECT_FALSE(PlayerState.m_LastDeath.has_value());

	RaceTeams().SaveLastTeleport(pCharacter);
	PlayerState.m_LastDeath = PlayerState.m_LastTeleTee;
	ASSERT_TRUE(PlayerState.m_LastTeleTee.has_value());
	ASSERT_TRUE(PlayerState.m_LastDeath.has_value());
	const vec2 SavedTeleportPosition = PlayerState.m_LastTeleTee->GetPos();
	const vec2 SavedDeathPosition = PlayerState.m_LastDeath->GetPos();
	pState = DDNetController.SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	PlayerState.m_LastTeleTee.reset();
	PlayerState.m_LastDeath.reset();
	DDNetController.RestoreCharacterAfterMapReload(pCharacter);
	ASSERT_TRUE(PlayerState.m_LastTeleTee.has_value());
	ASSERT_TRUE(PlayerState.m_LastDeath.has_value());
	EXPECT_EQ(PlayerState.m_LastTeleTee->GetPos(), SavedTeleportPosition);
	EXPECT_EQ(PlayerState.m_LastDeath->GetPos(), SavedDeathPosition);

	pState = DDNetController.SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	pCharacter->m_Pos = vec2(384.0f, 384.0f);
	pCharacter->SetPosition(pCharacter->m_Pos);
	VanillaController.RestoreCharacterAfterMapReload(pCharacter);
	DDNetController.RestoreCharacterAfterMapReload(pCharacter);
	EXPECT_EQ(pCharacter->m_Pos, vec2(384.0f, 384.0f));
}

TEST_F(GameWorld, MapReloadStateSurvivesContextReconstruction)
{
	constexpr int ClientId = 0;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);
	RaceTeams().SaveLastTeleport(pCharacter);
	RaceTeams().PlayerState(ClientId).m_LastDeath = RaceTeams().PlayerState(ClientId).m_LastTeleTee;
	const vec2 SavedPosition = pCharacter->m_Pos;
	const vec2 SavedTeleportPosition = RaceTeams().PlayerState(ClientId).m_LastTeleTee->GetPos();

	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	GameServer()->OnShutdown(m_pServer->m_pPersistentData);
	m_pKernel->ReregisterInterface(m_pGameServer);
	GameServer()->OnInit(m_pServer->m_pPersistentData);

	ASSERT_NE(GameServer()->GameHost().MapReloadState(), nullptr);
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CCharacter *pRestoredCharacter = GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(320.0f, 320.0f));
	ASSERT_NE(pRestoredCharacter, nullptr);
	EXPECT_EQ(pRestoredCharacter->m_Pos, SavedPosition);
	const auto &RestoredPlayerState = RaceTeams().PlayerState(ClientId);
	ASSERT_TRUE(RestoredPlayerState.m_LastTeleTee.has_value());
	ASSERT_TRUE(RestoredPlayerState.m_LastDeath.has_value());
	EXPECT_EQ(RestoredPlayerState.m_LastTeleTee->GetPos(), SavedTeleportPosition);
	EXPECT_EQ(RestoredPlayerState.m_LastDeath->GetPos(), SavedTeleportPosition);
}

TEST_F(GameWorld, MapReloadStateExpiresAfterOneContextHandoff)
{
	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));

	std::unique_ptr<IGameModeMapReloadState> pTransferredState = GameServer()->GameHost().TakeMapReloadState();
	ASSERT_NE(pTransferredState, nullptr);
	GameServer()->GameHost().RestoreMapReloadState(std::move(pTransferredState));
	ASSERT_NE(GameServer()->GameHost().MapReloadState(), nullptr);

	EXPECT_FALSE(GameServer()->GameHost().TakeMapReloadState());
	EXPECT_EQ(GameServer()->GameHost().MapReloadState(), nullptr);
}

TEST_F(GameWorld, MapReloadDisconnectKeepsSharedTeamState)
{
	constexpr int Team = 5;
	for(int ClientId = 0; ClientId < 2; ClientId++)
	{
		GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
		ASSERT_NE(GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(64.0f + ClientId * 32.0f, 96.0f)), nullptr);
		RaceTeams().SetForceCharacterTeam(ClientId, Team);
	}
	RaceTeams().SetPractice(Team, true);

	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_AUTH;
	m_pServer->m_aClients[0].m_DebugDummy = true;
	GameServer()->OnClientDrop(0, "test disconnect before map reload");
	m_pServer->m_aClients[0].m_State = CServer::CClient::STATE_EMPTY;
	m_pServer->m_aClients[0].m_DebugDummy = false;
	RaceTeams().SetPractice(Team, false);

	GameController()->RestoreCharacterAfterMapReload(GameServer()->GetPlayerChar(1));
	EXPECT_TRUE(RaceTeams().IsPractice(Team));
}

TEST_F(GameWorld, PreparingMapReloadReplacesTeamState)
{
	constexpr int ClientId = 0;
	constexpr int Team = 5;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CCharacter *pCharacter = GameServer()->m_apPlayers[ClientId]->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pCharacter, nullptr);
	RaceTeams().SetForceCharacterTeam(ClientId, Team);
	RaceTeams().SetPractice(Team, true);

	std::unique_ptr<IGameModeMapReloadState> pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	RaceTeams().SetPractice(Team, false);
	pState = GameController()->SaveStateForMapReload();
	ASSERT_NE(pState, nullptr);
	GameServer()->GameHost().PrepareMapReloadState(std::move(pState));
	RaceTeams().SetPractice(Team, true);

	GameController()->RestoreCharacterAfterMapReload(pCharacter);
	EXPECT_FALSE(RaceTeams().IsPractice(Team));
}

TEST_F(GameWorld, ModeOwnedCommandsFollowControllerLifetime)
{
	IConsole *pConsole = GameServer()->Console();
	const char *const apModeChatCommands[] = {"rank", "team", "practice", "tp", "hitothers", "save", "settings", "pause", "timer"};
	const char *const apModeAdminCommands[] = {"tele", "set_team_ddr", "save_dry", "random_map", "random_unfinished_map"};
	auto ExpectModeCommands = [&](bool Registered) {
		for(const char *pName : apModeChatCommands)
			EXPECT_EQ(pConsole->GetCommandInfo(pName, CFGFLAG_CHAT, false) != nullptr, Registered) << pName;
		for(const char *pName : apModeAdminCommands)
			EXPECT_EQ(pConsole->GetCommandInfo(pName, CFGFLAG_SERVER, false) != nullptr, Registered) << pName;
	};

	ExpectModeCommands(true);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	RaceScore().SetCurrentRecord(12.5f);
	ASSERT_TRUE(RaceScore().CurrentRecord().has_value());
	EXPECT_FLOAT_EQ(RaceScore().CurrentRecord().value(), 12.5f);
	ASSERT_NE(pConsole->GetCommandInfo("help", CFGFLAG_CHAT, false), nullptr);
	ASSERT_NE(pConsole->GetCommandInfo("showall", CFGFLAG_CHAT, false), nullptr);
	ASSERT_NE(pConsole->GetCommandInfo("kill_pl", CFGFLAG_SERVER, false), nullptr);

	GameServer()->GameHost().Shutdown();
	ExpectModeCommands(false);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("help", CFGFLAG_CHAT, false), nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("showall", CFGFLAG_CHAT, false), nullptr);
	EXPECT_NE(pConsole->GetCommandInfo("kill_pl", CFGFLAG_SERVER, false), nullptr);

	SelectGameMode("vanilla.dm");
	ExpectModeCommands(false);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	EXPECT_FALSE(RaceControllerOrNull() != nullptr);
	const int VanillaClientId = 0;
	CPlayer *pVanillaPlayer = GameServer()->CreatePlayer(VanillaClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pVanillaPlayer, nullptr);
	EXPECT_NE(dynamic_cast<CPlayerVanilla *>(pVanillaPlayer), nullptr);
	CCharacter *pVanillaCharacter = pVanillaPlayer->ForceSpawn(vec2(64.0f, 96.0f));
	ASSERT_NE(pVanillaCharacter, nullptr);
	EXPECT_EQ(dynamic_cast<CCharacterDDRace *>(pVanillaCharacter), nullptr);
	auto *pVanillaLaser = new CLaser(&GameServer()->m_World, pVanillaCharacter->m_Pos, vec2(1.0f, 0.0f), 64.0f, VanillaClientId, WEAPON_LASER);
	pVanillaLaser->Tick();
	delete pVanillaLaser;
	auto *pVanillaProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, VanillaClientId, pVanillaCharacter->m_Pos, vec2(1.0f, 0.0f), 100, false, false, -1, vec2(1.0f, 0.0f));
	EXPECT_TRUE(pVanillaProjectile->CanCollide(VanillaClientId));
	delete pVanillaProjectile;
	const int PreviousPlayerDemoRecord = g_Config.m_SvPlayerDemoRecord;
	g_Config.m_SvPlayerDemoRecord = 1;
	EXPECT_TRUE(m_pStorage->CreateFolder("demos", IStorage::TYPE_SAVE));
	m_pServer->StartRecord(VanillaClientId);
	EXPECT_TRUE(m_pServer->IsRecording(VanillaClientId));
	pVanillaCharacter->StopRecording();
	EXPECT_FALSE(m_pServer->IsRecording(VanillaClientId));
	g_Config.m_SvPlayerDemoRecord = PreviousPlayerDemoRecord;
	m_pServer->m_aClients[VanillaClientId].m_State = CServer::CClient::STATE_READY;
	m_pServer->SetClientScore(VanillaClientId, 17);
	GameController()->OnPlayerNameChanged(VanillaClientId);
	EXPECT_EQ(m_pServer->m_aClients[VanillaClientId].m_Score, 17);
	const int PreviousTestingCommands = g_Config.m_SvTestingCommands;
	const int PreviousPracticeByDefault = g_Config.m_SvPracticeByDefault;
	g_Config.m_SvTestingCommands = 1;
	g_Config.m_SvPracticeByDefault = 0;
	pConsole->ExecuteLine("sv_practice_by_default 1", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_EQ(g_Config.m_SvPracticeByDefault, 1);

	delete GameServer()->m_apPlayers[VanillaClientId];
	GameServer()->m_apPlayers[VanillaClientId] = nullptr;
	SelectGameMode("ddnet");
	ExpectModeCommands(true);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	EXPECT_TRUE(RaceControllerOrNull() != nullptr);
	EXPECT_FALSE(RaceScore().CurrentRecord().has_value());
	g_Config.m_SvTestingCommands = 1;
	g_Config.m_SvPracticeByDefault = 0;
	RaceTeams().Reset();
	EXPECT_FALSE(RaceTeams().PracticeByDefault());
	EXPECT_FALSE(RaceTeams().IsPractice(TEAM_FLOCK));
	pConsole->ExecuteLine("sv_practice_by_default 1", IConsole::CLIENT_ID_UNSPECIFIED, true);
	EXPECT_TRUE(RaceTeams().PracticeByDefault());
	EXPECT_TRUE(RaceTeams().IsPractice(TEAM_FLOCK));
	g_Config.m_SvTestingCommands = PreviousTestingCommands;
	g_Config.m_SvPracticeByDefault = PreviousPracticeByDefault;
}

TEST_F(GameWorld, DDRaceTeamCommandUsesModeOwnedState)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	ASSERT_NE(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)), nullptr);
	EXPECT_EQ(RaceTeams().m_Core.Team(ClientId), TEAM_FLOCK);

	GameServer()->Console()->ExecuteLine("team 1", ClientId);
	EXPECT_EQ(RaceTeams().m_Core.Team(ClientId), 1);
}

TEST_F(GameWorld, DDRaceRecordingPolicyIsCharacterOwned)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	CCharacterDDRace *pCharacter = dynamic_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);
	const int PreviousPlayerDemoRecord = g_Config.m_SvPlayerDemoRecord;
	g_Config.m_SvPlayerDemoRecord = 1;
	EXPECT_TRUE(m_pStorage->CreateFolder("demos", IStorage::TYPE_SAVE));
	m_pServer->StartRecord(ClientId);
	ASSERT_TRUE(m_pServer->IsRecording(ClientId));

	CPlayerData *pData = RaceScore().PlayerData(ClientId);
	pData->m_RecordStopTick = GameServer()->Server()->Tick() + GameServer()->Server()->TickSpeed();
	pData->m_RecordFinishTime = 12.5f;
	pCharacter->StopRecording();

	EXPECT_FALSE(m_pServer->IsRecording(ClientId));
	EXPECT_EQ(pData->m_RecordStopTick, -1);
	g_Config.m_SvPlayerDemoRecord = PreviousPlayerDemoRecord;
}

TEST_F(GameWorld, DDRacePlayerCommandUsesModeOwnedState)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);

	const int PreviousShowOthers = g_Config.m_SvShowOthers;
	g_Config.m_SvShowOthers = 1;
	GameServer()->Console()->ExecuteLine("showothers 2", ClientId);
	EXPECT_EQ(RaceTeams().PlayerState(ClientId).m_ShowOthers, 2);
	GameServer()->Console()->ExecuteLine("ninjajetpack 1", ClientId);
	EXPECT_TRUE(pPlayer->m_NinjaJetpack);
	g_Config.m_SvShowOthers = PreviousShowOthers;
}

TEST_F(GameWorld, DDRaceAdminAndPracticeCommandsUseModeOwnedState)
{
	constexpr int ClientId = 0;
	constexpr int Team = 1;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);
	auto *pCharacter = static_cast<CCharacterDDRace *>(pPlayer->ForceSpawn(vec2(64.0f, 96.0f)));
	ASSERT_NE(pCharacter, nullptr);
	m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_READY;
	const int AuthKey = m_pServer->m_AuthManager.AddKey("gameworld-admin", "test", RoleName::ADMIN);
	ASSERT_GE(AuthKey, 0);
	m_pServer->m_aClients[ClientId].m_AuthKey = AuthKey;

	const int PreviousTestingCommands = g_Config.m_SvTestingCommands;
	g_Config.m_SvTestingCommands = 1;
	GameServer()->Console()->ExecuteLine("super", ClientId);
	EXPECT_TRUE(pCharacter->IsSuper());
	GameServer()->Console()->ExecuteLine("unsuper", ClientId);
	EXPECT_FALSE(pCharacter->IsSuper());

	RaceTeams().SetForceCharacterTeam(ClientId, Team);
	RaceTeams().SetPractice(Team, true);
	GameServer()->Console()->ExecuteLineFlag("hitothers all", CFGFLAG_CHAT, ClientId);
	EXPECT_TRUE(pCharacter->HammerHitDisabled());
	EXPECT_TRUE(pCharacter->ShotgunHitDisabled());
	EXPECT_TRUE(pCharacter->GrenadeHitDisabled());
	EXPECT_TRUE(pCharacter->LaserHitDisabled());
	g_Config.m_SvTestingCommands = PreviousTestingCommands;
}

TEST_F(GameWorld, RaceScorePlayerStateFollowsPlayerIdentity)
{
	constexpr int ClientId = 0;
	CScore *pScore = &RaceScore();
	CPlayer *pFirstPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pFirstPlayer, nullptr);
	const uint32_t FirstUniqueClientId = pFirstPlayer->GetUniqueCid();
	pScore->ResetPlayer(ClientId);

	float aTimeCp[NUM_CHECKPOINTS] = {};
	pScore->PlayerData(ClientId)->Set(12.5f, aTimeCp);
	ASSERT_TRUE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	pScore->BeginFinishEligibilityCheck(ClientId);
	EXPECT_TRUE(pScore->FinishEligibilityCheckActive(ClientId));
	pScore->SetNotEligibleForFinish(ClientId);
	EXPECT_TRUE(pScore->NotEligibleForFinish(ClientId));
	EXPECT_FALSE(pScore->FinishEligibilityCheckActive(ClientId));

	CPlayer *pSecondPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pSecondPlayer, nullptr);
	ASSERT_NE(FirstUniqueClientId, pSecondPlayer->GetUniqueCid());
	pScore->Tick();
	EXPECT_FALSE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	EXPECT_FALSE(pScore->NotEligibleForFinish(ClientId));
	EXPECT_FALSE(pScore->FinishEligibilityCheckActive(ClientId));

	pScore->PlayerData(ClientId)->Set(13.5f, aTimeCp);
	pScore->SetNotEligibleForFinish(ClientId);
	GameController()->OnPlayerNameChanged(ClientId);
	EXPECT_FALSE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	EXPECT_FALSE(pScore->NotEligibleForFinish(ClientId));

	pScore->PlayerData(ClientId)->Set(14.5f, aTimeCp);
	pScore->SetNotEligibleForFinish(ClientId);
	GameController()->OnPlayerDisconnect(pSecondPlayer, "test");
	EXPECT_FALSE(pScore->PlayerData(ClientId)->m_BestTime.has_value());
	EXPECT_FALSE(pScore->NotEligibleForFinish(ClientId));
}

TEST_F(GameWorld, RaceFinishEligibilityIsModeOwned)
{
	constexpr int ClientId = 0;
	CPlayer *pPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pPlayer, nullptr);

	GameController()->OnPlayerEnter(pPlayer);
	EXPECT_TRUE(RaceScore().FinishEligibilityCheckActive(ClientId));
	EXPECT_FALSE(GameController()->OnPlayerChatMessage(ClientId, "hello", 0));
	EXPECT_TRUE(GameController()->OnPlayerChatMessage(ClientId, "xd sure chillerbot.png is lyfe", 0));
	EXPECT_TRUE(RaceScore().NotEligibleForFinish(ClientId));

	const CGameModeInfo VanillaInfo = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM VanillaController(GameServices(), VanillaInfo);
	EXPECT_FALSE(VanillaController.OnPlayerChatMessage(ClientId, "xd sure chillerbot.png is lyfe", 0));
}

TEST_F(GameWorld, RaceTeamPlayerStateFollowsPlayerIdentity)
{
	constexpr int ClientId = 0;
	CPlayer *pFirstPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pFirstPlayer, nullptr);
	GameController()->OnPlayerConnect(pFirstPlayer);
	CCharacterDDRace *pFirstCharacter = dynamic_cast<CCharacterDDRace *>(pFirstPlayer->ForceSpawn(vec2(0.0f, 32.0f)));
	ASSERT_NE(pFirstCharacter, nullptr);
	EXPECT_FALSE(RaceTeams().LoadLastTeleport(pFirstCharacter));
	RaceTeams().SaveLastTeleport(pFirstCharacter);
	pFirstCharacter->SetPosition(vec2(64.0f, 64.0f));
	EXPECT_TRUE(RaceTeams().LoadLastTeleport(pFirstCharacter));
	EXPECT_EQ(pFirstCharacter->m_Pos.x, 0.0f);
	EXPECT_EQ(pFirstCharacter->m_Pos.y, 32.0f);

	auto SetState = [this] {
		auto &State = RaceTeams().PlayerState(ClientId);
		State.m_TeeStarted = true;
		State.m_TeeFinished = true;
		State.m_LastChat = 1;
		State.m_LastSwap = 2;
		State.m_LastInvited = 3;
		State.m_LastTeamChange = 4;
		State.m_VotedForPractice = true;
		State.m_SwapTargetClientId = 1;
		State.m_RescueMode = RESCUEMODE_MANUAL;
		State.m_LastTeleTee.emplace();
		State.m_LastDeath.emplace();
		State.m_ShowOthers = SHOW_OTHERS_OFF;
		State.m_SpecTeam = true;
	};
	auto ExpectReset = [this](int ShowOthers) {
		const auto &State = RaceTeams().PlayerState(ClientId);
		EXPECT_FALSE(State.m_TeeStarted);
		EXPECT_FALSE(State.m_TeeFinished);
		EXPECT_EQ(State.m_LastChat, 0);
		EXPECT_EQ(State.m_LastSwap, 0);
		EXPECT_EQ(State.m_LastInvited, 0);
		EXPECT_FALSE(State.m_LastTeamChange.has_value());
		EXPECT_FALSE(State.m_VotedForPractice);
		EXPECT_EQ(State.m_SwapTargetClientId, -1);
		EXPECT_EQ(State.m_RescueMode, RESCUEMODE_AUTO);
		EXPECT_FALSE(State.m_LastTeleTee.has_value());
		EXPECT_FALSE(State.m_LastDeath.has_value());
		EXPECT_EQ(State.m_ShowOthers, ShowOthers);
		EXPECT_FALSE(State.m_SpecTeam);
	};

	SetState();
	RaceTeams().Reset();
	const auto &RoundResetState = RaceTeams().PlayerState(ClientId);
	EXPECT_FALSE(RoundResetState.m_TeeStarted);
	EXPECT_FALSE(RoundResetState.m_LastTeleTee.has_value());
	EXPECT_EQ(RoundResetState.m_ShowOthers, SHOW_OTHERS_OFF);
	EXPECT_TRUE(RoundResetState.m_SpecTeam);

	SetState();
	CPlayer *pSecondPlayer = GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	ASSERT_NE(pSecondPlayer, nullptr);
	GameController()->OnPlayerConnect(pSecondPlayer);
	ExpectReset(g_Config.m_SvShowOthersDefault);

	SetState();
	GameController()->OnPlayerDisconnect(pSecondPlayer, "test");
	ExpectReset(SHOW_OTHERS_ON);
}

TEST_F(GameWorld, VanillaTDMTeamDamage)
{
	SelectGameMode("vanilla.tdm");
	auto &Controller = *dynamic_cast<CGameControllerVanillaTDM *>(GameController());

	GameServer()->CreatePlayer(0, TEAM_RED, false, -1);
	GameServer()->CreatePlayer(1, TEAM_RED, false, -1);
	GameServer()->CreatePlayer(2, TEAM_BLUE, false, -1);
	CCharacter *pAttacker = GameServer()->m_apPlayers[0]->ForceSpawn(vec2(-64, 0));
	CCharacter *pTeammate = GameServer()->m_apPlayers[1]->ForceSpawn(vec2(0, 0));
	CCharacter *pEnemy = GameServer()->m_apPlayers[2]->ForceSpawn(vec2(64, 0));
	pTeammate->SetHealth(10);
	pEnemy->SetHealth(10);
	const int PreviousTeamDamage = g_Config.m_SvTeamdamage;
	g_Config.m_SvTeamdamage = 0;

	Controller.OnCharacterTakeDamage(pTeammate, vec2(2, 0), 1, pAttacker->GetPlayer()->GetCid(), WEAPON_GUN, true, TEAM_SPECTATORS);
	Controller.OnCharacterTakeDamage(pEnemy, vec2(2, 0), 1, pAttacker->GetPlayer()->GetCid(), WEAPON_GUN, true, TEAM_SPECTATORS);
	const int TeammateHealth = pTeammate->GetHealth();
	const int EnemyHealth = pEnemy->GetHealth();
	const float TeammateVelocityX = pTeammate->GetCore().m_Vel.x;
	auto *pFriendlyProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, pAttacker->GetPlayer()->GetCid(), pTeammate->m_Pos, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	auto *pEnemyProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, pAttacker->GetPlayer()->GetCid(), pEnemy->m_Pos, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	Controller.DoTeamChange(pAttacker->GetPlayer(), TEAM_BLUE, false);
	EXPECT_EQ(Controller.MatchStats().Player(0).m_TeamChanges, 1);
	const int BlueTeamAfterForceSpawn = pEnemy->GetPlayer()->GetTeam();
	const int FriendlyProjectileOwner = pFriendlyProjectile->GetOwnerId();
	const int EnemyProjectileOwner = pEnemyProjectile->GetOwnerId();

	g_Config.m_SvTeamdamage = PreviousTeamDamage;
	EXPECT_EQ(TeammateHealth, 10);
	EXPECT_GT(TeammateVelocityX, 0.0f);
	EXPECT_EQ(EnemyHealth, 9);
	EXPECT_EQ(BlueTeamAfterForceSpawn, TEAM_BLUE);
	EXPECT_EQ(FriendlyProjectileOwner, -1);
	EXPECT_EQ(EnemyProjectileOwner, -1);
}

TEST_F(GameWorld, VanillaTeamBalanceRunsAfterConfiguredDelay)
{
	const int PreviousTeamBalanceTime = g_Config.m_SvTeambalanceTime;
	g_Config.m_SvTeambalanceTime = 1;
	const CGameModeInfo Info = {"vanilla.tdm", "Vanilla TDM", "TDM", "TestTDM", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CTestVanillaTDM Controller(GameServices(), Info);

	for(int ClientId = 0; ClientId < 4; ClientId++)
		GameServer()->m_apPlayers[ClientId] = Controller.CreatePlayer(ClientId + 1, ClientId, ClientId == 3 ? TEAM_BLUE : TEAM_RED);
	auto *pRedZero = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[0]);
	auto *pRedBestFit = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[1]);
	auto *pRedTen = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[2]);
	auto *pBlue = static_cast<CPlayerVanilla *>(GameServer()->m_apPlayers[3]);
	pRedZero->m_Score = 0;
	pRedBestFit->m_Score = 2;
	pRedTen->m_Score = 10;
	pBlue->m_Score = 3;
	pRedBestFit->m_LastActionTick = 123;

	char aError[64];
	EXPECT_FALSE(Controller.CanJoinTeam(TEAM_RED, 4, aError, sizeof(aError)));
	EXPECT_STREQ(aError, "Teams must remain balanced");
	const int Now = GameServer()->Server()->Tick();
	Controller.UpdateTeamBalance(Now);
	Controller.UpdateTeamBalance(Now + GameServer()->Server()->TickSpeed() * 60);
	EXPECT_EQ(pRedBestFit->GetTeam(), TEAM_RED);
	Controller.UpdateTeamBalance(Now + GameServer()->Server()->TickSpeed() * 60 + 1);

	EXPECT_EQ(pRedZero->GetTeam(), TEAM_RED);
	EXPECT_EQ(pRedBestFit->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pRedTen->GetTeam(), TEAM_RED);
	EXPECT_EQ(pBlue->GetTeam(), TEAM_BLUE);
	EXPECT_EQ(pRedBestFit->m_LastActionTick, 123);
	EXPECT_EQ(Controller.MatchStats().Player(1).m_TeamChanges, 1);

	g_Config.m_SvTeambalanceTime = PreviousTeamBalanceTime;
}

TEST_F(GameWorld, InstagibDMVerticalSlice)
{
	SelectGameMode("insta.idm");

	constexpr int AttackerId = 0;
	constexpr int VictimId = 1;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	EXPECT_EQ(GameController()->MatchStats().Player(AttackerId).m_Spawns, 1);
	EXPECT_EQ(GameController()->MatchStats().Player(VictimId).m_Spawns, 1);

	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_GUN));
	EXPECT_TRUE(pAttackerCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_EQ(pAttackerCharacter->GetWeaponAmmo(WEAPON_LASER), -1);
	EXPECT_EQ(pAttackerCharacter->GetActiveWeapon(), WEAPON_LASER);

	pAttackerCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	EXPECT_TRUE(pAttackerCharacter->IsAlive());
	EXPECT_EQ(pAttackerCharacter->GetHealth(), 10);

	pVictimCharacter->SetArmor(10);
	pVictimCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	EXPECT_FALSE(pVictimCharacter->IsAlive());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 1);
	const CMatchPlayerStats &AttackerStats = GameController()->MatchStats().Player(AttackerId);
	const CMatchPlayerStats &VictimStats = GameController()->MatchStats().Player(VictimId);
	EXPECT_EQ(AttackerStats.m_WeaponHits, 2);
	EXPECT_EQ(AttackerStats.m_aWeaponHits[WEAPON_LASER], 2);
	EXPECT_EQ(AttackerStats.m_DamageDealt, 20);
	EXPECT_EQ(AttackerStats.m_aDamageDealt[WEAPON_LASER], 20);
	EXPECT_EQ(AttackerStats.m_Kills, 1);
	EXPECT_EQ(AttackerStats.m_aKills[WEAPON_LASER], 1);
	EXPECT_EQ(VictimStats.m_DamageTaken, 20);
	EXPECT_EQ(VictimStats.m_Deaths, 1);

	pAttackerCharacter->Die(AttackerId, WEAPON_SELF);
	EXPECT_EQ(AttackerStats.m_Suicides, 1);
	EXPECT_EQ(AttackerStats.m_Deaths, 1);

	EXPECT_TRUE(GameController()->OnEntity(ENTITY_SPAWN, 1, 1, LAYER_GAME, 0, true));
	EXPECT_FALSE(GameController()->OnEntity(ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true));
	EXPECT_FALSE(GameController()->OnEntity(ENTITY_WEAPON_LASER, 1, 1, LAYER_GAME, 0, true));
}

TEST_F(GameWorld, Vanilla1on1IsTwoSlotDMProfile)
{
	SelectGameMode("vanilla.1on1");

	EXPECT_NE(dynamic_cast<CGameControllerVanillaDM *>(GameController()), nullptr);
	EXPECT_EQ(GameController()->ActivePlayerSlots(), 2);

	ASSERT_NE(GameServer()->CreatePlayer(0, TEAM_GAME, false, -1), nullptr);
	ASSERT_NE(GameServer()->CreatePlayer(1, TEAM_GAME, false, -1), nullptr);
	char aError[128];
	EXPECT_EQ(GameController()->GetAutoTeam(2), TEAM_SPECTATORS);
	EXPECT_FALSE(GameController()->CanJoinTeam(TEAM_GAME, 2, aError, sizeof(aError)));
	EXPECT_STREQ(aError, "Only 2 active players are allowed");
	EXPECT_TRUE(GameController()->CanJoinTeam(TEAM_SPECTATORS, 2, aError, sizeof(aError)));
}

TEST_F(GameWorld, InstagibTDMReusesTeamplay)
{
	SelectGameMode("insta.itdm");

	constexpr int AttackerId = 0;
	constexpr int TeammateId = 1;
	constexpr int EnemyId = 2;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_RED, false, -1);
	CPlayer *pTeammate = GameServer()->CreatePlayer(TeammateId, TEAM_RED, false, -1);
	CPlayer *pEnemy = GameServer()->CreatePlayer(EnemyId, TEAM_BLUE, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pTeammate, nullptr);
	ASSERT_NE(pEnemy, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pTeammateCharacter = pTeammate->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pEnemyCharacter = pEnemy->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pTeammateCharacter, nullptr);
	ASSERT_NE(pEnemyCharacter, nullptr);

	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_GUN));
	EXPECT_TRUE(pAttackerCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_EQ(pAttackerCharacter->GetWeaponAmmo(WEAPON_LASER), -1);

	const int PreviousTeamDamage = g_Config.m_SvTeamdamage;
	g_Config.m_SvTeamdamage = 0;
	pAttackerCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	pTeammateCharacter->TakeDamage(vec2(2.0f, 0.0f), 0, AttackerId, WEAPON_LASER);
	pEnemyCharacter->SetArmor(10);
	pEnemyCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);
	g_Config.m_SvTeamdamage = PreviousTeamDamage;

	EXPECT_TRUE(pAttackerCharacter->IsAlive());
	EXPECT_TRUE(pTeammateCharacter->IsAlive());
	EXPECT_EQ(pTeammateCharacter->GetHealth(), 10);
	EXPECT_GT(pTeammateCharacter->GetCore().m_Vel.x, 0.0f);
	EXPECT_FALSE(pEnemyCharacter->IsAlive());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 1);
	const auto *pTDM = dynamic_cast<CGameControllerVanillaTDM *>(GameController());
	ASSERT_NE(pTDM, nullptr);
	EXPECT_EQ(pTDM->TeamScore(TEAM_RED), 1);
	EXPECT_FALSE(GameController()->OnEntity(ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true));
}

TEST_F(GameWorld, InstagibCTFReusesFlagLifecycle)
{
	SelectGameMode("insta.ictf");

	EXPECT_FALSE(GameController()->OnEntity(ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true));
	EXPECT_TRUE(GameController()->OnEntity(ENTITY_FLAGSTAND_RED, 1, 1, LAYER_GAME, 0, true));
	EXPECT_TRUE(GameController()->OnEntity(ENTITY_FLAGSTAND_BLUE, 2, 1, LAYER_GAME, 0, true));
	const auto *pCTF = dynamic_cast<CGameControllerVanillaCTF *>(GameController());
	ASSERT_NE(pCTF, nullptr);
	ASSERT_NE(pCTF->Flag(TEAM_RED), nullptr);
	ASSERT_NE(pCTF->Flag(TEAM_BLUE), nullptr);

	constexpr int AttackerId = 0;
	constexpr int VictimId = 1;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_RED, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_BLUE, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	EXPECT_TRUE(pVictimCharacter->GetWeaponGot(WEAPON_LASER));
	EXPECT_FALSE(pVictimCharacter->GetWeaponGot(WEAPON_GUN));

	pCTF->Flag(TEAM_RED)->Grab(pVictimCharacter);
	ASSERT_EQ(pCTF->Flag(TEAM_RED)->Carrier(), pVictimCharacter);
	pVictimCharacter->SetArmor(10);
	pVictimCharacter->TakeDamage(vec2(), 0, AttackerId, WEAPON_LASER);

	EXPECT_FALSE(pVictimCharacter->IsAlive());
	EXPECT_EQ(pCTF->Flag(TEAM_RED)->Carrier(), nullptr);
	EXPECT_FALSE(pCTF->Flag(TEAM_RED)->IsAtStand());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 2);
}

TEST_F(GameWorld, GrenadeInstagibDMVerticalSlice)
{
	SelectGameMode("insta.gdm");

	constexpr int AttackerId = 0;
	constexpr int VictimId = 1;
	CPlayer *pAttacker = GameServer()->CreatePlayer(AttackerId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pAttacker, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pAttackerCharacter = pAttacker->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pAttackerCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);

	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_HAMMER));
	EXPECT_FALSE(pAttackerCharacter->GetWeaponGot(WEAPON_GUN));
	EXPECT_TRUE(pAttackerCharacter->GetWeaponGot(WEAPON_GRENADE));
	EXPECT_EQ(pAttackerCharacter->GetWeaponAmmo(WEAPON_GRENADE), -1);
	EXPECT_EQ(pAttackerCharacter->GetActiveWeapon(), WEAPON_GRENADE);

	pAttackerCharacter->TakeDamage(vec2(2.0f, 0.0f), 5, AttackerId, WEAPON_GRENADE);
	EXPECT_TRUE(pAttackerCharacter->IsAlive());
	EXPECT_EQ(pAttackerCharacter->GetHealth(), 10);
	EXPECT_GT(pAttackerCharacter->GetCore().m_Vel.x, 0.0f);

	pVictimCharacter->TakeDamage(vec2(2.0f, 0.0f), 3, AttackerId, WEAPON_GRENADE);
	EXPECT_TRUE(pVictimCharacter->IsAlive());
	EXPECT_EQ(pVictimCharacter->GetHealth(), 10);
	EXPECT_GT(pVictimCharacter->GetCore().m_Vel.x, 0.0f);

	pVictimCharacter->SetArmor(10);
	pVictimCharacter->TakeDamage(vec2(), 4, AttackerId, WEAPON_GRENADE);
	EXPECT_FALSE(pVictimCharacter->IsAlive());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pAttacker), 1);

	EXPECT_TRUE(GameController()->OnEntity(ENTITY_SPAWN, 1, 1, LAYER_GAME, 0, true));
	EXPECT_FALSE(GameController()->OnEntity(ENTITY_HEALTH_1, 1, 1, LAYER_GAME, 0, true));
	EXPECT_FALSE(GameController()->OnEntity(ENTITY_WEAPON_GRENADE, 1, 1, LAYER_GAME, 0, true));
}

TEST_F(GameWorld, ZCatchReleasesOwnershipOnCatcherDeathAndDisconnect)
{
	SelectGameMode("zcatch.laser");

	constexpr int FirstCatcherId = 0;
	constexpr int VictimId = 1;
	constexpr int SecondCatcherId = 2;
	constexpr int StaleProjectileVictimId = 3;
	CPlayer *pFirstCatcher = GameServer()->CreatePlayer(FirstCatcherId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	CPlayer *pSecondCatcher = GameServer()->CreatePlayer(SecondCatcherId, TEAM_GAME, false, -1);
	CPlayer *pStaleProjectileVictim = GameServer()->CreatePlayer(StaleProjectileVictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pFirstCatcher, nullptr);
	ASSERT_NE(pVictim, nullptr);
	ASSERT_NE(pSecondCatcher, nullptr);
	ASSERT_NE(pStaleProjectileVictim, nullptr);
	CCharacter *pFirstCatcherCharacter = pFirstCatcher->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pSecondCatcherCharacter = pSecondCatcher->ForceSpawn(vec2(128.0f, 96.0f));
	CCharacter *pStaleProjectileVictimCharacter = pStaleProjectileVictim->ForceSpawn(vec2(160.0f, 96.0f));
	ASSERT_NE(pFirstCatcherCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	ASSERT_NE(pSecondCatcherCharacter, nullptr);
	ASSERT_NE(pStaleProjectileVictimCharacter, nullptr);

	pVictimCharacter->TakeDamage(vec2(), 0, FirstCatcherId, WEAPON_LASER);
	EXPECT_EQ(GameController()->PlayerAutoRespawnTick(pVictim), std::numeric_limits<int>::max());
	pFirstCatcherCharacter->TakeDamage(vec2(), 0, SecondCatcherId, WEAPON_LASER);
	EXPECT_NE(GameController()->PlayerAutoRespawnTick(pVictim), std::numeric_limits<int>::max());
	EXPECT_EQ(GameController()->PlayerAutoRespawnTick(pFirstCatcher), std::numeric_limits<int>::max());
	pStaleProjectileVictimCharacter->TakeDamage(vec2(), 0, FirstCatcherId, WEAPON_LASER);
	EXPECT_NE(GameController()->PlayerAutoRespawnTick(pStaleProjectileVictim), std::numeric_limits<int>::max());

	GameController()->OnPlayerDisconnect(pSecondCatcher, "test");
	EXPECT_NE(GameController()->PlayerAutoRespawnTick(pFirstCatcher), std::numeric_limits<int>::max());
}

TEST_F(GameWorld, ZCatchEndsRoundForLastPlayerStanding)
{
	SelectGameMode("zcatch.laser");

	constexpr int CatcherId = 0;
	constexpr int FirstVictimId = 1;
	constexpr int SecondVictimId = 2;
	CPlayer *pCatcher = GameServer()->CreatePlayer(CatcherId, TEAM_GAME, false, -1);
	CPlayer *pFirstVictim = GameServer()->CreatePlayer(FirstVictimId, TEAM_GAME, false, -1);
	CPlayer *pSecondVictim = GameServer()->CreatePlayer(SecondVictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pCatcher, nullptr);
	ASSERT_NE(pFirstVictim, nullptr);
	ASSERT_NE(pSecondVictim, nullptr);
	CCharacter *pCatcherCharacter = pCatcher->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pFirstVictimCharacter = pFirstVictim->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pSecondVictimCharacter = pSecondVictim->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pCatcherCharacter, nullptr);
	ASSERT_NE(pFirstVictimCharacter, nullptr);
	ASSERT_NE(pSecondVictimCharacter, nullptr);

	pFirstVictimCharacter->TakeDamage(vec2(), 0, CatcherId, WEAPON_LASER);
	pSecondVictimCharacter->TakeDamage(vec2(), 0, CatcherId, WEAPON_LASER);
	EXPECT_EQ(GameController()->PlayerAutoRespawnTick(pFirstVictim), std::numeric_limits<int>::max());
	EXPECT_EQ(GameController()->PlayerAutoRespawnTick(pSecondVictim), std::numeric_limits<int>::max());
	EXPECT_FALSE(GameController()->IsGamePaused());

	GameController()->Tick();
	EXPECT_TRUE(GameController()->IsGamePaused());
	EXPECT_EQ(GameController()->SnapPlayerScore(SERVER_DEMO_CLIENT, pCatcher), 2);
}

TEST_F(GameWorld, ZCatchDeadSpectatorPresentationIsProtocolAware)
{
	SelectGameMode("zcatch.laser");

	constexpr int VictimId = 0;
	constexpr int CatcherId = 1;
	CPlayer *pCatcher = GameServer()->CreatePlayer(CatcherId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pCatcher, nullptr);
	ASSERT_NE(pVictim, nullptr);
	CCharacter *pCatcherCharacter = pCatcher->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(96.0f, 96.0f));
	ASSERT_NE(pCatcherCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	const int PreviousClientState = m_pServer->m_aClients[VictimId].m_State;
	const bool PreviousSixup = m_pServer->m_aClients[VictimId].m_Sixup;
	m_pServer->m_aClients[VictimId].m_State = CServer::CClient::STATE_INGAME;

	pVictimCharacter->TakeDamage(vec2(), 0, CatcherId, WEAPON_LASER);
	EXPECT_EQ(pVictim->GetTeam(), TEAM_GAME);
	EXPECT_EQ(pVictim->SpectatorId(), CatcherId);

	m_pServer->m_aClients[VictimId].m_Sixup = false;
	m_pServer->m_SnapshotBuilder.Init(false);
	pVictim->Snap(VictimId);
	CSnapshotBuffer SixBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&SixBuffer);
	const CSnapshot *pSixSnapshot = SixBuffer.AsSnapshot();
	const auto *pSixPlayerInfo = static_cast<const CNetObj_PlayerInfo *>(pSixSnapshot->FindItem(NETOBJTYPE_PLAYERINFO, VictimId));
	const auto *pSixSpectatorInfo = static_cast<const CNetObj_SpectatorInfo *>(pSixSnapshot->FindItem(NETOBJTYPE_SPECTATORINFO, VictimId));
	ASSERT_NE(pSixPlayerInfo, nullptr);
	ASSERT_NE(pSixSpectatorInfo, nullptr);
	EXPECT_EQ(pSixPlayerInfo->m_Team, TEAM_SPECTATORS);
	EXPECT_EQ(pSixSpectatorInfo->m_SpectatorId, CatcherId);

	m_pServer->m_aClients[VictimId].m_Sixup = true;
	m_pServer->m_SnapshotBuilder.Init(true);
	pVictim->Snap(VictimId);
	CSnapshotBuffer SevenBuffer;
	m_pServer->m_SnapshotBuilder.Finish(&SevenBuffer);
	const CSnapshot *pSevenSnapshot = SevenBuffer.AsSnapshot();
	const auto *pSevenPlayerInfo = static_cast<const protocol7::CNetObj_PlayerInfo *>(pSevenSnapshot->FindItem(protocol7::NETOBJTYPE_PLAYERINFO, VictimId));
	const auto *pSevenSpectatorInfo = static_cast<const protocol7::CNetObj_SpectatorInfo *>(pSevenSnapshot->FindItem(protocol7::NETOBJTYPE_SPECTATORINFO, VictimId));
	ASSERT_NE(pSevenPlayerInfo, nullptr);
	ASSERT_NE(pSevenSpectatorInfo, nullptr);
	EXPECT_NE(pSevenPlayerInfo->m_PlayerFlags & protocol7::PLAYERFLAG_DEAD, 0);
	EXPECT_EQ(pSevenSpectatorInfo->m_SpecMode, protocol7::SPEC_PLAYER);
	EXPECT_EQ(pSevenSpectatorInfo->m_SpectatorId, CatcherId);

	m_pServer->m_aClients[VictimId].m_State = PreviousClientState;
	m_pServer->m_aClients[VictimId].m_Sixup = PreviousSixup;
}

TEST_F(GameWorld, ZCatchLateJoinFollowsLeadingCatcher)
{
	SelectGameMode("zcatch.laser");

	constexpr int LeaderId = 0;
	constexpr int VictimId = 1;
	constexpr int ContenderId = 2;
	constexpr int EarlyJoinId = 3;
	constexpr int LateJoinId = 4;
	constexpr int SpectatorId = 5;
	CPlayer *pLeader = GameServer()->CreatePlayer(LeaderId, TEAM_GAME, false, -1);
	CPlayer *pVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	CPlayer *pContender = GameServer()->CreatePlayer(ContenderId, TEAM_GAME, false, -1);
	ASSERT_NE(pLeader, nullptr);
	ASSERT_NE(pVictim, nullptr);
	ASSERT_NE(pContender, nullptr);
	CCharacter *pLeaderCharacter = pLeader->ForceSpawn(vec2(64.0f, 96.0f));
	CCharacter *pVictimCharacter = pVictim->ForceSpawn(vec2(96.0f, 96.0f));
	CCharacter *pContenderCharacter = pContender->ForceSpawn(vec2(128.0f, 96.0f));
	ASSERT_NE(pLeaderCharacter, nullptr);
	ASSERT_NE(pVictimCharacter, nullptr);
	ASSERT_NE(pContenderCharacter, nullptr);
	CPlayer *pEarlyJoin = GameServer()->CreatePlayer(EarlyJoinId, TEAM_GAME, false, -1);
	ASSERT_NE(pEarlyJoin, nullptr);
	GameController()->OnPlayerConnect(pEarlyJoin);
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(EarlyJoinId));
	pVictimCharacter->TakeDamage(vec2(), 0, LeaderId, WEAPON_LASER);

	CPlayer *pLateJoin = GameServer()->CreatePlayer(LateJoinId, TEAM_GAME, false, -1);
	ASSERT_NE(pLateJoin, nullptr);
	GameController()->OnPlayerConnect(pLateJoin);
	EXPECT_EQ(pLateJoin->GetTeam(), TEAM_GAME);
	EXPECT_TRUE(GameController()->IsPlayerDeadSpectator(LateJoinId));
	EXPECT_EQ(pLateJoin->SpectatorId(), LeaderId);
	EXPECT_EQ(GameController()->PlayerAutoRespawnTick(pLateJoin), std::numeric_limits<int>::max());

	CPlayer *pSpectator = GameServer()->CreatePlayer(SpectatorId, TEAM_SPECTATORS, false, -1);
	ASSERT_NE(pSpectator, nullptr);
	GameController()->OnPlayerConnect(pSpectator);
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(SpectatorId));

	pLeaderCharacter->TakeDamage(vec2(), 0, ContenderId, WEAPON_LASER);
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(VictimId));
	EXPECT_FALSE(GameController()->IsPlayerDeadSpectator(LateJoinId));
}

TEST_F(GameWorld, VanillaCTFFlagLifecycle)
{
	SelectGameMode("vanilla.ctf");
	auto &Controller = *dynamic_cast<CGameControllerVanillaCTF *>(GameController());

	int RedFlagX = -1;
	int RedFlagY = -1;
	int BlueFlagX = -1;
	int BlueFlagY = -1;
	for(int y = 1; y < GameServer()->Collision()->GetHeight() - 1; y++)
	{
		for(int x = 1; x < GameServer()->Collision()->GetWidth() - 1; x++)
		{
			const vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
			if(GameServer()->Collision()->TestBox(Pos, vec2(28.0f, 28.0f)))
				continue;
			if(RedFlagX == -1)
			{
				RedFlagX = x;
				RedFlagY = y;
			}
			else if(distance(Pos, vec2(RedFlagX * 32.0f + 16.0f, RedFlagY * 32.0f + 16.0f)) > 128.0f)
			{
				BlueFlagX = x;
				BlueFlagY = y;
				break;
			}
		}
		if(BlueFlagX != -1)
			break;
	}
	ASSERT_NE(RedFlagX, -1);
	ASSERT_NE(BlueFlagX, -1);
	const vec2 RedStand(RedFlagX * 32.0f + 16.0f, RedFlagY * 32.0f + 16.0f);
	const vec2 BlueStand(BlueFlagX * 32.0f + 16.0f, BlueFlagY * 32.0f + 16.0f);
	EXPECT_TRUE(Controller.OnEntity(ENTITY_FLAGSTAND_RED, RedFlagX, RedFlagY, LAYER_GAME, 0, true, 0));
	EXPECT_TRUE(Controller.OnEntity(ENTITY_FLAGSTAND_BLUE, BlueFlagX, BlueFlagY, LAYER_GAME, 0, true, 0));
	CFlag *pRedFlag = Controller.Flag(TEAM_RED);
	EXPECT_TRUE(Controller.OnEntity(ENTITY_FLAGSTAND_RED, RedFlagX, RedFlagY, LAYER_GAME, 0, true, 0));
	EXPECT_EQ(Controller.Flag(TEAM_RED), pRedFlag);
	ASSERT_NE(Controller.Flag(TEAM_RED), nullptr);
	ASSERT_NE(Controller.Flag(TEAM_BLUE), nullptr);
	EXPECT_EQ(Controller.Flag(TEAM_RED)->StandPosition(), RedStand);
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->StandPosition(), BlueStand);

	GameServer()->CreatePlayer(0, TEAM_RED, false, -1);
	GameServer()->CreatePlayer(1, TEAM_BLUE, false, -1);
	CCharacter *pRedCarrier = GameServer()->m_apPlayers[0]->ForceSpawn(BlueStand);
	CCharacter *pBlueReturner = GameServer()->m_apPlayers[1]->ForceSpawn(vec2(BlueStand.x + 128.0f, BlueStand.y));
	ASSERT_NE(pRedCarrier, nullptr);
	ASSERT_NE(pBlueReturner, nullptr);
	EXPECT_EQ(Controller.MatchStats().Player(0).m_Spawns, 1);
	EXPECT_EQ(Controller.MatchStats().Player(1).m_Spawns, 1);
	const int PreviousScoreLimit = g_Config.m_SvScorelimit;
	const int PreviousTimeLimit = g_Config.m_SvTimelimit;
	g_Config.m_SvScorelimit = 0;
	g_Config.m_SvTimelimit = 0;

	Controller.Tick();
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), pRedCarrier);
	EXPECT_EQ(Controller.TeamScore(TEAM_RED), 1);
	EXPECT_EQ(Controller.SnapPlayerScore(-1, pRedCarrier->GetPlayer()), 1);

	pRedCarrier->SetPosition(RedStand);
	pRedCarrier->m_Pos = RedStand;
	Controller.Tick();
	EXPECT_TRUE(Controller.Flag(TEAM_RED)->IsAtStand());
	EXPECT_TRUE(Controller.Flag(TEAM_BLUE)->IsAtStand());
	EXPECT_EQ(Controller.TeamScore(TEAM_RED), 101);
	EXPECT_EQ(Controller.SnapPlayerScore(-1, pRedCarrier->GetPlayer()), 6);
	EXPECT_EQ(Controller.MatchStats().Player(0).m_FlagGrabs, 1);
	EXPECT_EQ(Controller.MatchStats().Player(0).m_FlagCaptures, 1);
	EXPECT_EQ(Controller.MatchStats().Flag(TEAM_BLUE).m_Grabs, 1);
	EXPECT_EQ(Controller.MatchStats().Flag(TEAM_BLUE).m_Captures, 1);

	pRedCarrier->SetPosition(BlueStand);
	pRedCarrier->m_Pos = BlueStand;
	Controller.Tick();
	ASSERT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), pRedCarrier);
	pRedCarrier->Die(pBlueReturner->GetPlayer()->GetCid(), WEAPON_GUN);
	EXPECT_EQ(Controller.Flag(TEAM_BLUE)->Carrier(), nullptr);
	EXPECT_FALSE(Controller.Flag(TEAM_BLUE)->IsAtStand());
	pBlueReturner->SetPosition(BlueStand);
	pBlueReturner->m_Pos = BlueStand;
	Controller.Tick();
	EXPECT_TRUE(Controller.Flag(TEAM_BLUE)->IsAtStand());
	EXPECT_EQ(Controller.SnapPlayerScore(-1, pBlueReturner->GetPlayer()), 3);
	EXPECT_EQ(Controller.MatchStats().Player(0).m_FlagGrabs, 2);
	EXPECT_EQ(Controller.MatchStats().Player(0).m_FlagDrops, 1);
	EXPECT_EQ(Controller.MatchStats().Player(1).m_FlagReturns, 1);
	EXPECT_EQ(Controller.MatchStats().Flag(TEAM_BLUE).m_Grabs, 2);
	EXPECT_EQ(Controller.MatchStats().Flag(TEAM_BLUE).m_Drops, 1);
	EXPECT_EQ(Controller.MatchStats().Flag(TEAM_BLUE).m_Returns, 1);
	EXPECT_EQ(Controller.MatchStats().Flag(TEAM_BLUE).m_Captures, 1);

	g_Config.m_SvScorelimit = PreviousScoreLimit;
	g_Config.m_SvTimelimit = PreviousTimeLimit;
}

TEST_F(GameWorld, VanillaPickup)
{
	constexpr int ClientId = 0;
	SelectGameMode("vanilla.dm");
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);

	auto &Controller = *dynamic_cast<CGameControllerVanillaDM *>(GameController());
	pCharacter->SetHealth(9);
	const CGamePickupResult HealthResult = Controller.OnCharacterPickup(pCharacter, POWERUP_HEALTH, 0, pCharacter->m_Pos);
	EXPECT_TRUE(HealthResult.m_Picked);
	EXPECT_EQ(HealthResult.m_RespawnSeconds, 15);
	EXPECT_EQ(pCharacter->GetHealth(), 10);
	EXPECT_FALSE(Controller.OnCharacterPickup(pCharacter, POWERUP_HEALTH, 0, pCharacter->m_Pos).m_Picked);

	pCharacter->SetWeaponGot(WEAPON_SHOTGUN, false);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 0);
	const CGamePickupResult WeaponResult = Controller.OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos);
	EXPECT_TRUE(WeaponResult.m_Picked);
	EXPECT_EQ(WeaponResult.m_RespawnSeconds, 15);
	EXPECT_EQ(WeaponResult.m_RespawnSound, SOUND_WEAPON_SPAWN);
	EXPECT_TRUE(pCharacter->GetWeaponGot(WEAPON_SHOTGUN));
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), 10);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 4);
	EXPECT_TRUE(Controller.OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos).m_Picked);
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), 10);
	EXPECT_FALSE(Controller.OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos).m_Picked);
	EXPECT_EQ(Controller.PickupInitialSpawnDelaySeconds(POWERUP_HEALTH, 0), 0);
	EXPECT_EQ(Controller.PickupInitialSpawnDelaySeconds(POWERUP_NINJA, 0), 90);

	pCharacter->SetHealth(9);
	auto *pHealth = new CPickup(&GameServer()->m_World, POWERUP_HEALTH, 0, 0, 0, 0);
	pHealth->m_Pos = pCharacter->m_Pos;
	pHealth->Tick();
	auto *pNinja = new CPickup(&GameServer()->m_World, POWERUP_NINJA, 0, 0, 0, 0);
	EXPECT_EQ(pCharacter->GetHealth(), 10);
	EXPECT_FALSE(pHealth->IsActive());
	EXPECT_FALSE(pNinja->IsActive());
}

TEST_F(GameWorld, DDRacePickupPolicyIsModeOwned)
{
	constexpr int ClientId = 0;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);

	auto *pHealth = new CPickup(&GameServer()->m_World, POWERUP_HEALTH, 0, 0, 0, 0);
	pHealth->m_Pos = pCharacter->m_Pos;
	pHealth->Tick();
	EXPECT_GT(pCharacter->m_FreezeTime, 0);
	EXPECT_TRUE(pHealth->IsActive());

	pCharacter->Unfreeze();
	pCharacter->SetWeaponGot(WEAPON_SHOTGUN, true);
	pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, -1);
	pCharacter->SetActiveWeapon(WEAPON_SHOTGUN);
	RaceController().OnCharacterPickup(pCharacter, POWERUP_ARMOR_SHOTGUN, 0, pCharacter->m_Pos);
	EXPECT_FALSE(pCharacter->GetWeaponGot(WEAPON_SHOTGUN));
	EXPECT_EQ(pCharacter->GetActiveWeapon(), WEAPON_HAMMER);

	RaceController().OnCharacterPickup(pCharacter, POWERUP_WEAPON, WEAPON_SHOTGUN, pCharacter->m_Pos);
	EXPECT_TRUE(pCharacter->GetWeaponGot(WEAPON_SHOTGUN));
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_SHOTGUN), -1);
}

TEST_F(GameWorld, VanillaProjectileOwnerLoss)
{
	constexpr int ClientId = 0;
	constexpr int TargetId = 1;
	EXPECT_EQ(GameController()->ProjectileRules({WEAPON_GUN, nullptr, true, false}).m_OwnerLossAction, EProjectileOwnerLossAction::DESTROY);

	vec2 SpawnPosition;
	ASSERT_TRUE(GameController()->CanSpawn(TEAM_GAME, &SpawnPosition, ClientId));
	SelectGameMode("vanilla.dm");
	auto &Controller = *dynamic_cast<CGameControllerVanillaDM *>(GameController());
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(SpawnPosition);
	ASSERT_NE(pPlayer->GetCharacter(), nullptr);
	GameServer()->CreatePlayer(TargetId, TEAM_GAME, false, -1);
	CPlayer *pTargetPlayer = GameServer()->m_apPlayers[TargetId];
	pTargetPlayer->ForceSpawn(SpawnPosition);
	CCharacter *pTarget = pTargetPlayer->GetCharacter();
	ASSERT_NE(pTarget, nullptr);
	pTarget->SetHealth(10);

	const CGameProjectileRules ConnectedRules = Controller.ProjectileRules({WEAPON_GUN, pPlayer->GetCharacter(), true, false});
	EXPECT_TRUE(ConnectedRules.m_HitCharacters);
	EXPECT_FALSE(ConnectedRules.m_RespectCharacterCollision);
	EXPECT_FLOAT_EQ(ConnectedRules.m_DirectImpactForce, 0.001f);
	EXPECT_EQ(ConnectedRules.m_OwnerLossAction, EProjectileOwnerLossAction::KEEP);
	EXPECT_EQ(Controller.ProjectileRules({WEAPON_GUN, nullptr, false, false}).m_OwnerLossAction, EProjectileOwnerLossAction::DETACH);
	auto *pImpactProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, ClientId, SpawnPosition, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	pImpactProjectile->Tick();
	EXPECT_EQ(pTarget->GetHealth(), 9);

	auto *pProjectile = new CProjectile(&GameServer()->m_World, WEAPON_GUN, ClientId, SpawnPosition, vec2(1, 0), 100, false, false, -1, vec2(1, 0));
	pPlayer->KillCharacter();
	pProjectile->Tick();
	EXPECT_EQ(pProjectile->GetOwnerId(), ClientId);
	GameServer()->m_apPlayers[ClientId] = nullptr;
	pProjectile->Tick();
	GameServer()->m_apPlayers[ClientId] = pPlayer;
	EXPECT_EQ(pProjectile->GetOwnerId(), -1);
}

TEST_F(GameWorld, ExplosionPolicyIsModeOwned)
{
	constexpr int OwnerId = 0;
	constexpr int VictimId = 1;
	const int PreviousHit = g_Config.m_SvHit;

	ASSERT_TRUE(RaceControllerOrNull() != nullptr);
	CPlayer *pRaceOwner = GameServer()->CreatePlayer(OwnerId, TEAM_GAME, false, -1);
	CPlayer *pRaceVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pRaceOwner, nullptr);
	ASSERT_NE(pRaceVictim, nullptr);
	ASSERT_NE(pRaceOwner->ForceSpawn(vec2(-64.0f, 0.0f)), nullptr);
	CCharacter *pRaceVictimCharacter = pRaceVictim->ForceSpawn(vec2(0.0f, 0.0f));
	ASSERT_NE(pRaceVictimCharacter, nullptr);
	pRaceVictimCharacter->SetHealth(10);
	pRaceOwner->KillCharacter();
	g_Config.m_SvHit = 0;
	GameServer()->CreateExplosion(pRaceVictimCharacter->m_Pos, OwnerId, WEAPON_GRENADE, false, -1);
	const int RaceVictimHealth = pRaceVictimCharacter->GetHealth();
	g_Config.m_SvHit = PreviousHit;
	EXPECT_EQ(RaceVictimHealth, 10);

	delete GameServer()->m_apPlayers[OwnerId];
	GameServer()->m_apPlayers[OwnerId] = nullptr;
	delete GameServer()->m_apPlayers[VictimId];
	GameServer()->m_apPlayers[VictimId] = nullptr;
	SelectGameMode("vanilla.dm");
	ASSERT_FALSE(RaceControllerOrNull() != nullptr);
	CPlayer *pVanillaOwner = GameServer()->CreatePlayer(OwnerId, TEAM_GAME, false, -1);
	CPlayer *pVanillaVictim = GameServer()->CreatePlayer(VictimId, TEAM_GAME, false, -1);
	ASSERT_NE(pVanillaOwner, nullptr);
	ASSERT_NE(pVanillaVictim, nullptr);
	ASSERT_NE(pVanillaOwner->ForceSpawn(vec2(-64.0f, 0.0f)), nullptr);
	CCharacter *pVanillaVictimCharacter = pVanillaVictim->ForceSpawn(vec2(0.0f, 0.0f));
	ASSERT_NE(pVanillaVictimCharacter, nullptr);
	pVanillaVictimCharacter->SetHealth(10);
	pVanillaOwner->KillCharacter();
	g_Config.m_SvHit = 0;
	GameServer()->CreateExplosion(pVanillaVictimCharacter->m_Pos, OwnerId, WEAPON_GRENADE, false, -1);
	const int VanillaVictimHealth = pVanillaVictimCharacter->GetHealth();
	g_Config.m_SvHit = PreviousHit;
	EXPECT_LT(VanillaVictimHealth, 10);

	delete GameServer()->m_apPlayers[OwnerId];
	GameServer()->m_apPlayers[OwnerId] = nullptr;
	delete GameServer()->m_apPlayers[VictimId];
	GameServer()->m_apPlayers[VictimId] = nullptr;
	SelectGameMode("ddnet");
}
