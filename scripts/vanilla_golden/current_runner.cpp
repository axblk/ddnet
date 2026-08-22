#include <base/logger.h>
#include <base/net.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#define private public
#define protected public
#include <engine/engine.h>
#include <engine/http.h>
#include <engine/kernel.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/server/server.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/mode/game_services.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/player.h>
#include <game/server/player.h>
#include <game/version.h>
#undef protected
#undef private

bool IsInterrupted()
{
	return false;
}

[[noreturn]] static void Fail(const std::string &Message)
{
	std::cerr << Message << '\n';
	std::exit(1);
}

static void Check(bool Condition, const char *pMessage)
{
	if(!Condition)
		Fail(pMessage);
}

class CCollisionFixture
{
public:
	enum
	{
		WIDTH = 24,
		HEIGHT = 14,
	};

	CCollisionFixture()
	{
		Install(m_Collision, m_aTiles);
	}

	static void Install(CCollision &Collision, std::array<CTile, WIDTH * HEIGHT> &aTiles)
	{
		Collision.Unload();
		aTiles.fill({});
		Collision.m_Width = WIDTH;
		Collision.m_Height = HEIGHT;
		Collision.m_pTiles = aTiles.data();
		for(int x = 0; x < WIDTH; ++x)
		{
			Solid(Collision, x, 0);
			Solid(Collision, x, 10);
			Solid(Collision, x, HEIGHT - 1);
		}
		for(int y = 0; y < HEIGHT; ++y)
		{
			Solid(Collision, 0, y);
			Solid(Collision, WIDTH - 1, y);
			Solid(Collision, 16, y);
		}
	}

	std::array<CTile, WIDTH * HEIGHT> m_aTiles{};
	CCollision m_Collision;

private:
	static void Solid(CCollision &Collision, int X, int Y) { Collision.m_pTiles[Y * WIDTH + X].m_Index = TILE_SOLID; }
};

static void PrintCore(const std::string &Name, int Tick, const CCharacterCore &Core)
{
	CNetObj_CharacterCore Net;
	Core.Write(&Net);
	std::cout << "core " << Name << " tick=" << Tick
		  << " pos=" << Net.m_X << ',' << Net.m_Y
		  << " vel=" << Net.m_VelX << ',' << Net.m_VelY
		  << " jumped=" << Net.m_Jumped
		  << " hook=" << Net.m_HookState << ',' << Net.m_HookedPlayer
		  << ',' << Net.m_HookX << ',' << Net.m_HookY << '\n';
}

static void RunCore(const std::string &Name, int Ticks)
{
	CCollisionFixture Collision;
	CWorldCore World;
	CTeamsCore Teams;
	CCharacterCore Core;
	Core.Init(&World, &Collision.m_Collision, &Teams);
	Core.Reset();
	Core.m_Id = 0;
	Core.m_Pos = vec2(160.0f, 304.0f);
	World.m_apCharacters[0] = &Core;

	CCharacterCore Other;
	if(Name == "hook_player")
	{
		Other.Init(&World, &Collision.m_Collision, &Teams);
		Other.Reset();
		Other.m_Id = 1;
		Other.m_Pos = vec2(300.0f, 304.0f);
		World.m_apCharacters[1] = &Other;
	}

	PrintCore(Name, 0, Core);
	for(int Tick = 1; Tick <= Ticks; ++Tick)
	{
		mem_zero(&Core.m_Input, sizeof(Core.m_Input));
		Core.m_Input.m_TargetX = 400;
		if(Name == "run")
			Core.m_Input.m_Direction = Tick <= 30 ? 1 : 0;
		else if(Name == "jump")
		{
			Core.m_Input.m_Direction = 1;
			Core.m_Input.m_Jump = Tick == 2 || Tick == 28;
		}
		else
			Core.m_Input.m_Hook = Tick <= 55;
		Core.Tick(true);
		Core.Move();
		Core.Quantize();
		PrintCore(Name, Tick, Core);
	}
}

class CGameFixture
{
public:
	CGameFixture()
	{
		m_pServer = CreateServer();
		Check(m_pServer != nullptr, "server creation failed");
		m_pKernel.reset(IKernel::Create());
		m_pKernel->RegisterInterface(m_pServer);

		IEngine *pEngine = CreateTestEngine(GAME_NAME);
		m_pKernel->RegisterInterface(pEngine);
		const char *apStorageArgs[] = {"vanilla-golden-current"};
		m_pStorage.reset(CreateStorage(IStorage::EInitializationType::BASIC, std::size(apStorageArgs), apStorageArgs));
		Check(m_pStorage != nullptr, "storage creation failed");
		m_pKernel->RegisterInterface(m_pStorage.get(), false);

		IConsole *pConsole = CreateConsole(CFGFLAG_SERVER | CFGFLAG_ECON).release();
		m_pKernel->RegisterInterface(pConsole);
		IConfigManager *pConfigManager = CreateConfigManager();
		m_pKernel->RegisterInterface(pConfigManager);
		IEngineHttp *pEngineHttp = CreateEngineHttp();
		m_pKernel->RegisterInterface(pEngineHttp);
		m_pKernel->RegisterInterface(static_cast<IHttp *>(pEngineHttp), false);
		IEngineAntibot *pEngineAntibot = CreateEngineAntibot();
		m_pKernel->RegisterInterface(pEngineAntibot);
		m_pKernel->RegisterInterface(static_cast<IAntibot *>(pEngineAntibot), false);

		m_pGameServer = static_cast<CGameContext *>(CreateGameServer());
		m_pKernel->RegisterInterface(m_pGameServer);
		pEngine->Init();
		pConsole->Init();
		pConfigManager->Init();
		m_pServer->RegisterCommands();
		Check(m_pServer->LoadMap("coverage") != 0, "coverage map load failed");
		m_pServer->m_RunServer = CServer::RUNNING;
		m_pServer->m_AuthManager.Init();
		for(auto &Client : m_pServer->m_aClients)
		{
			Client.m_HasPersistentData = false;
			Client.m_pPersistentData = malloc(m_pGameServer->PersistentClientDataSize());
		}
		m_pServer->m_pPersistentData = malloc(m_pGameServer->PersistentDataSize());
		Check(m_pServer->LoadMap("coverage") != 0, "persistent coverage map load failed");
		Check(pEngineHttp->Init(std::chrono::seconds{2}), "HTTP init failed");
		m_pServer->m_NetServer.SetCallbacks(
			CServer::NewClientCallback,
			CServer::NewClientNoAuthCallback,
			CServer::ClientRejoinCallback,
			CServer::DelClientCallback,
			m_pServer);
		m_pServer->m_Econ.Init(m_pServer->Config(), m_pServer->Console(), &m_pServer->m_ServerBan);
		m_pServer->m_Fifo.Init(m_pServer->Console(), m_pServer->Config()->m_SvInputFifo, CFGFLAG_SERVER);
		m_pServer->Antibot()->Init();
		m_pGameServer->OnInit(nullptr);
		m_pServer->ReadAnnouncementsFile();
		m_pServer->InitMaplist();

		m_pGameServer->GameHost().Shutdown();
		Check(m_pGameServer->GameHost().Select("vanilla.dm"), "vanilla.dm selection failed");
		m_pGameServer->GameHost().Init(m_pServer->DbPool());
		m_pGameServer->m_World.m_ResetRequested = true;
		m_pGameServer->m_World.Tick();
		CCollisionFixture::Install(*m_pGameServer->Collision(), m_aTiles);
		g_Config.m_SvScorelimit = 2;
		g_Config.m_SvTimelimit = 0;
		g_Config.m_SvWarmup = 0;
		Spawn(0, vec2(160.0f, 304.0f));
		Spawn(1, vec2(195.0f, 304.0f));
	}

	~CGameFixture()
	{
		m_pServer->m_Econ.Shutdown();
		m_pServer->m_Fifo.Shutdown();
		m_pGameServer->OnShutdown(nullptr);
		m_pServer->DbPool()->OnShutdown();
	}

	void SetTick(int Tick) { m_pServer->m_CurrentGameTick = Tick; }
	CCharacter *Character(int ClientId) { return m_pGameServer->m_apPlayers[ClientId]->GetCharacter(); }
	CPlayerVanilla *Player(int ClientId) { return static_cast<CPlayerVanilla *>(m_pGameServer->m_apPlayers[ClientId]); }

	std::array<CTile, CCollisionFixture::WIDTH * CCollisionFixture::HEIGHT> m_aTiles{};
	CServer *m_pServer;
	CGameContext *m_pGameServer;
	std::unique_ptr<IKernel> m_pKernel;
	std::unique_ptr<IStorage> m_pStorage;

private:
	void Spawn(int ClientId, vec2 Position)
	{
		m_pServer->m_aClients[ClientId].m_State = CServer::CClient::STATE_INGAME;
		CPlayer *pPlayer = m_pGameServer->CreatePlayer(ClientId, TEAM_GAME, false, -1);
		Check(pPlayer != nullptr, "player creation failed");
		Check(pPlayer->ForceSpawn(Position) != nullptr, "character spawn failed");
	}
};

static int WeaponFromName(const std::string &Name)
{
	if(Name == "hammer")
		return WEAPON_HAMMER;
	if(Name == "gun")
		return WEAPON_GUN;
	if(Name == "shotgun")
		return WEAPON_SHOTGUN;
	if(Name == "grenade")
		return WEAPON_GRENADE;
	if(Name == "laser")
		return WEAPON_LASER;
	if(Name == "ninja")
		return WEAPON_NINJA;
	return -1;
}

static void PrintWeaponTick(CGameFixture &Fixture, const std::string &Name, int Tick, CCharacter *pAttacker, CCharacter *pVictim, int Weapon)
{
	CEntity *apEntities[32];
	const int Projectiles = Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	int MinLife = -1;
	int MaxLife = -1;
	for(int i = 0; i < Projectiles; ++i)
	{
		const int Life = static_cast<CProjectile *>(apEntities[i])->m_LifeSpan;
		MinLife = MinLife < 0 ? Life : std::min(MinLife, Life);
		MaxLife = std::max(MaxLife, Life);
	}
	const int Lasers = Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_LASER);
	int LaserEnergy = -1;
	int LaserBounces = -1;
	if(Lasers)
	{
		const CLaser *pLaser = static_cast<CLaser *>(apEntities[0]);
		LaserEnergy = round_to_int(pLaser->m_Energy * 256.0f);
		LaserBounces = pLaser->m_Bounces;
	}
	std::cout << "weapon_tick " << Name << " tick=" << Tick
		  << " ammo=" << pAttacker->GetWeaponAmmo(Weapon)
		  << " reload=" << pAttacker->m_ReloadTimer
		  << " victim=" << pVictim->GetHealth() << ',' << pVictim->GetArmor()
		  << " projectiles=" << Projectiles << " life=" << MinLife << ',' << MaxLife
		  << " lasers=" << Lasers << " laser=" << LaserEnergy << ',' << LaserBounces << '\n';
}

static void RunWeapon(const std::string &Name)
{
	CGameFixture Fixture;
	CCharacter *pAttacker = Fixture.Character(0);
	CCharacter *pVictim = Fixture.Character(1);
	const int Weapon = WeaponFromName(Name);
	if(Weapon == WEAPON_NINJA)
		pAttacker->GiveNinja();
	else if(Weapon != WEAPON_HAMMER)
	{
		pAttacker->GiveWeapon(Weapon);
		pAttacker->SetWeaponAmmo(Weapon, 10);
	}
	pAttacker->SetWeapon(Weapon);
	pAttacker->m_ReloadTimer = 0;
	mem_zero(&pAttacker->m_LatestPrevInput, sizeof(pAttacker->m_LatestPrevInput));
	mem_zero(&pAttacker->m_LatestInput, sizeof(pAttacker->m_LatestInput));
	pAttacker->m_LatestInput.m_TargetX = 100;
	pAttacker->m_LatestInput.m_Fire = 1;
	pAttacker->FireWeapon();
	pAttacker->m_LatestPrevInput.m_Fire = 0;
	pAttacker->m_LatestInput.m_Fire = 0;

	CEntity *apEntities[32];
	const int InitialProjectiles = Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	const int InitialLasers = Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_LASER);
	std::vector<std::string> ProjectileData;
	Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	for(int i = 0; i < InitialProjectiles; ++i)
	{
		const CNetObj_Projectile Net = static_cast<CProjectile *>(apEntities[i])->NetInfoVanilla();
		std::ostringstream Item;
		Item << Net.m_X << ',' << Net.m_Y << ',' << Net.m_VelX << ',' << Net.m_VelY << ',' << Net.m_Type;
		ProjectileData.push_back(Item.str());
	}
	std::sort(ProjectileData.begin(), ProjectileData.end());
	PrintWeaponTick(Fixture, Name, 0, pAttacker, pVictim, Weapon);
	for(int Tick = 1; Tick <= 120; ++Tick)
	{
		Fixture.SetTick(Tick);
		Fixture.m_pGameServer->m_World.Tick();
		PrintWeaponTick(Fixture, Name, Tick, pAttacker, pVictim, Weapon);
	}
	const int FinalProjectiles = Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	const int FinalLasers = Fixture.m_pGameServer->m_World.FindEntities(vec2(200, 304), 1000, apEntities, 32, CGameWorld::ENTTYPE_LASER);
	std::cout << "weapon " << Name
		  << " ammo=" << pAttacker->GetWeaponAmmo(Weapon)
		  << " reload=" << pAttacker->m_ReloadTimer
		  << " projectiles=" << InitialProjectiles << ',' << FinalProjectiles
		  << " lasers=" << InitialLasers << ',' << FinalLasers
		  << " victim=" << pVictim->GetHealth() << ',' << pVictim->GetArmor()
		  << " victim_vel=" << round_to_int(pVictim->Core()->m_Vel.x * 256.0f)
		  << ',' << round_to_int(pVictim->Core()->m_Vel.y * 256.0f);
	for(const std::string &Item : ProjectileData)
		std::cout << " projectile=" << Item;
	std::cout << '\n';
}

static void RunDamage(const std::string &Name, int Amount)
{
	CGameFixture Fixture;
	CCharacter *pVictim = Fixture.Character(1);
	pVictim->SetHealth(10);
	pVictim->SetArmor(5);
	int Weapon = WeaponFromName(Name);
	int From = 0;
	if(Name == "grenade_self")
	{
		Weapon = WEAPON_GRENADE;
		From = 1;
	}
	if(Name == "grenade_explosion")
		Fixture.m_pGameServer->CreateExplosion(pVictim->m_Pos - vec2(32, 0), 0, WEAPON_GRENADE, false, -1);
	else
		pVictim->TakeDamage(vec2(2, -3), Amount, From, Weapon);
	std::cout << "damage " << Name << " amount=" << Amount
		  << " health=" << pVictim->GetHealth() << " armor=" << pVictim->GetArmor()
		  << " vel=" << round_to_int(pVictim->Core()->m_Vel.x * 256.0f)
		  << ',' << round_to_int(pVictim->Core()->m_Vel.y * 256.0f) << '\n';
}

static void RunMatch(const std::string &Name)
{
	CGameFixture Fixture;
	CPlayerVanilla *pAlpha = Fixture.Player(0);
	CPlayerVanilla *pBeta = Fixture.Player(1);
	auto *pController = static_cast<CGameControllerVanillaDM *>(Fixture.m_pGameServer->GameHost().Controller());
	int RespawnBefore = -1;
	int RespawnAt = -1;
	if(Name == "kill")
		Fixture.Character(1)->Die(0, WEAPON_GUN);
	else if(Name == "suicide")
		Fixture.Character(0)->Die(0, WEAPON_GRENADE);
	else if(Name == "respawn_normal" || Name == "respawn_self")
	{
		pController->m_avSpawnPoints[IGameController::SPAWNTYPE_DEFAULT].clear();
		pController->m_avSpawnPoints[IGameController::SPAWNTYPE_DEFAULT].push_back(vec2(256.0f, 304.0f));
		if(Name == "respawn_normal")
		{
			Fixture.Character(0)->Die(1, WEAPON_GUN);
			Fixture.SetTick(1);
			pAlpha->Tick();
		}
		else
			pAlpha->KillCharacter(WEAPON_SELF);
		pAlpha->Respawn();
		const int BeforeTick = Name == "respawn_normal" ? 24 : 149;
		const int AtTick = Name == "respawn_normal" ? 25 : 150;
		Fixture.SetTick(BeforeTick);
		pAlpha->Tick();
		RespawnBefore = pAlpha->GetCharacter() != nullptr;
		Fixture.SetTick(AtTick);
		pAlpha->Tick();
		RespawnAt = pAlpha->GetCharacter() && pAlpha->GetCharacter()->IsAlive();
	}
	else if(Name == "sudden_death")
	{
		pAlpha->m_Score = 2;
		pBeta->m_Score = 2;
		pController->Tick();
	}
	else if(Name == "score_limit")
	{
		pAlpha->m_Score = 2;
		pBeta->m_Score = 1;
		pController->Tick();
	}
	else
		Fail("unknown match scenario: " + Name);
	const bool MatchEnded = pController->Match().IsGameOver();
	const bool Warmup = pController->Match().IsWarmup();
	const bool Paused = !MatchEnded && pController->IsGamePaused();
	std::cout << "match " << Name
		  << " score=" << pAlpha->m_Score << ',' << pBeta->m_Score
		  << " sudden_death=" << pController->Match().IsSuddenDeath()
		  << " running=" << (!MatchEnded && !Warmup && !Paused)
		  << " warmup=" << Warmup << " paused=" << Paused << " match_ended=" << MatchEnded
		  << " alive=" << (pAlpha->GetCharacter() && pAlpha->GetCharacter()->IsAlive())
		  << " respawn_before=" << RespawnBefore << " respawn_at=" << RespawnAt << '\n';
}

static void Dispatch(const std::string &Line)
{
	std::istringstream Input(Line);
	std::string Kind;
	std::string Name;
	Input >> Kind >> Name;
	if(Kind == "core")
	{
		int Ticks;
		if(!(Input >> Ticks) || (Name != "run" && Name != "jump" && Name != "hook_wall" && Name != "hook_player"))
			Fail("invalid core scenario: " + Line);
		RunCore(Name, Ticks);
	}
	else if(Kind == "weapon")
	{
		if(WeaponFromName(Name) < 0)
			Fail("invalid weapon scenario: " + Line);
		RunWeapon(Name);
	}
	else if(Kind == "damage")
	{
		int Amount;
		if(!(Input >> Amount) || (WeaponFromName(Name) < 0 && Name != "grenade_self" && Name != "grenade_explosion"))
			Fail("invalid damage scenario: " + Line);
		RunDamage(Name, Amount);
	}
	else if(Kind == "match")
		RunMatch(Name);
	else
		Fail("unknown scenario: " + Line);
	std::string Extra;
	if(Input >> Extra)
		Fail("trailing scenario input: " + Line);
}

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		std::cerr << "usage: vanilla-golden-current SCENARIOS\n";
		return 2;
	}
	std::ifstream Input(argv[1]);
	if(!Input)
	{
		std::cerr << "cannot open scenarios: " << argv[1] << '\n';
		return 2;
	}
	log_set_global_logger(log_logger_noop().release());
	net_init();
	std::string Line;
	while(std::getline(Input, Line))
	{
		const std::string::size_type First = Line.find_first_not_of(" \t\r");
		if(First == std::string::npos || Line[First] == '#')
			continue;
		Dispatch(Line.substr(First));
	}
	return 0;
}
