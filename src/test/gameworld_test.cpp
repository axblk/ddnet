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

#include <generated/protocol.h>

#include <game/server/entities/character.h>
#include <game/server/entities/pickup.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamecontroller.h>
#include <game/server/gameworld.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/player.h>
#include <game/version.h>

#include <gtest/gtest.h>

#include <memory>
#include <thread>

bool IsInterrupted()
{
	return false;
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
	const int StartTeam = GameServer()->m_pController->GetAutoTeam(ClientId);
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
	pChr->m_NinjaJetpack = true;
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
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);

	const CGameModeInfo Info = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM Controller(GameServer(), Info);
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
	IGameController *pPreviousController = GameServer()->m_pController;
	GameServer()->m_pController = &Controller;
	pCharacter->SetActiveWeapon(WEAPON_GUN);
	CNetObj_PlayerInput Input = {};
	Input.m_TargetX = 1;
	pCharacter->OnDirectInput(&Input);
	Input.m_Fire = 1;
	pCharacter->OnDirectInput(&Input);
	GameServer()->m_pController = pPreviousController;
	EXPECT_EQ(pCharacter->GetWeaponAmmo(WEAPON_GUN), 9);
	EXPECT_EQ(CountProjectiles(), BeforeGun + 1);

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
}

TEST_F(GameWorld, VanillaPickup)
{
	constexpr int ClientId = 0;
	GameServer()->CreatePlayer(ClientId, TEAM_GAME, false, -1);
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	pPlayer->ForceSpawn(vec2(0, 0));
	CCharacter *pCharacter = pPlayer->GetCharacter();
	ASSERT_NE(pCharacter, nullptr);

	const CGameModeInfo Info = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM Controller(GameServer(), Info);
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
	IGameController *pPreviousController = GameServer()->m_pController;
	GameServer()->m_pController = &Controller;
	auto *pHealth = new CPickup(&GameServer()->m_World, POWERUP_HEALTH, 0, 0, 0, 0);
	pHealth->m_Pos = pCharacter->m_Pos;
	pHealth->Tick();
	auto *pNinja = new CPickup(&GameServer()->m_World, POWERUP_NINJA, 0, 0, 0, 0);
	GameServer()->m_pController = pPreviousController;
	EXPECT_EQ(pCharacter->GetHealth(), 10);
	EXPECT_FALSE(pHealth->IsActive());
	EXPECT_FALSE(pNinja->IsActive());
}

TEST_F(GameWorld, LegacyPickupRemainsActive)
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
}

TEST_F(GameWorld, VanillaProjectileOwnerLoss)
{
	constexpr int ClientId = 0;
	constexpr int TargetId = 1;
	vec2 SpawnPosition;
	ASSERT_TRUE(GameServer()->m_pController->CanSpawn(TEAM_GAME, &SpawnPosition, ClientId));
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

	const CGameModeInfo Info = {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN};
	CGameControllerVanillaDM Controller(GameServer(), Info);
	const CGameProjectileRules ConnectedRules = Controller.ProjectileRules({WEAPON_GUN, pPlayer->GetCharacter(), true, false});
	EXPECT_TRUE(ConnectedRules.m_HitCharacters);
	EXPECT_FALSE(ConnectedRules.m_RespectCharacterCollision);
	EXPECT_FLOAT_EQ(ConnectedRules.m_DirectImpactForce, 0.001f);
	EXPECT_EQ(ConnectedRules.m_OwnerLossAction, EProjectileOwnerLossAction::KEEP);
	EXPECT_EQ(Controller.ProjectileRules({WEAPON_GUN, nullptr, false, false}).m_OwnerLossAction, EProjectileOwnerLossAction::DETACH);
	EXPECT_EQ(GameServer()->m_pController->ProjectileRules({WEAPON_GUN, nullptr, true, false}).m_OwnerLossAction, EProjectileOwnerLossAction::DESTROY);

	IGameController *pPreviousController = GameServer()->m_pController;
	GameServer()->m_pController = &Controller;
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
	GameServer()->m_pController = pPreviousController;
	EXPECT_EQ(pProjectile->GetOwnerId(), -1);
}
