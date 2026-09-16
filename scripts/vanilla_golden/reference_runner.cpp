#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#define private public
#define protected public
#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/gamecore.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/entities/laser.h>
#include <game/server/entities/projectile.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemodes/dm.h>
#include <game/server/player.h>
#undef protected
#undef private

class CReferenceServer : public IServer
{
public:
	CReferenceServer()
	{
		m_CurrentGameTick = 0;
		m_TickSpeed = 50;
	}

	void SetTick(int Tick) { m_CurrentGameTick = Tick; }
	const char *ClientName(int ClientID) const override { return ClientID == 0 ? "alpha" : "beta"; }
	const char *ClientClan(int ClientID) const override { return ""; }
	int ClientCountry(int ClientID) const override { return -1; }
	bool ClientIngame(int ClientID) const override { return ClientID >= 0 && ClientID < 2; }
	int GetClientInfo(int ClientID, CClientInfo *pInfo) const override
	{
		pInfo->m_pName = ClientName(ClientID);
		pInfo->m_Latency = 0;
		return 1;
	}
	void GetClientAddr(int ClientID, char *pAddrStr, int Size) const override { str_copy(pAddrStr, "local", Size); }
	int GetClientVersion(int ClientID) const override { return 0x0705; }
	int SendMsg(CMsgPacker *pMsg, int Flags, int ClientID) override { return 0; }
	void SetClientName(int ClientID, const char *pName) override {}
	void SetClientClan(int ClientID, const char *pClan) override {}
	void SetClientCountry(int ClientID, int Country) override {}
	void SetClientScore(int ClientID, int Score) override {}
	int SnapNewID() override { return 1; }
	void SnapFreeID(int ID) override {}
	void *SnapNewItem(int Type, int ID, int Size) override
	{
		m_aSnapshot.resize(Size);
		return m_aSnapshot.data();
	}
	void SnapSetStaticsize(int ItemType, int Size) override {}
	void SetRconCID(int ClientID) override {}
	bool IsAuthed(int ClientID) const override { return false; }
	bool IsBanned(int ClientID) override { return false; }
	void Kick(int ClientID, const char *pReason) override {}
	void DemoRecorder_HandleAutoStart() override {}
	bool DemoRecorder_IsRecording() override { return false; }

private:
	std::vector<unsigned char> m_aSnapshot;
};

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
		mem_zero(m_aTiles, sizeof(m_aTiles));
		for(int x = 0; x < WIDTH; ++x)
		{
			Solid(x, 0);
			Solid(x, 10);
			Solid(x, HEIGHT - 1);
		}
		for(int y = 0; y < HEIGHT; ++y)
		{
			Solid(0, y);
			Solid(WIDTH - 1, y);
			Solid(16, y);
		}
		m_Collision.m_pTiles = m_aTiles;
		m_Collision.m_Width = WIDTH;
		m_Collision.m_Height = HEIGHT;
		m_Collision.m_pLayers = 0;
	}

	void Install(CCollision *pCollision)
	{
		pCollision->m_pTiles = m_aTiles;
		pCollision->m_Width = WIDTH;
		pCollision->m_Height = HEIGHT;
		pCollision->m_pLayers = 0;
	}

	CCollision m_Collision;

private:
	void Solid(int X, int Y) { m_aTiles[Y * WIDTH + X].m_Index = TILE_SOLID; }
	CTile m_aTiles[WIDTH * HEIGHT];
};

static void PrintCore(const char *pName, int Tick, CCharacterCore &Core)
{
	CNetObj_CharacterCore Net;
	Core.Write(&Net);
	std::cout << "core " << pName << " tick=" << Tick
		  << " pos=" << Net.m_X << "," << Net.m_Y
		  << " vel=" << Net.m_VelX << "," << Net.m_VelY
		  << " jumped=" << Net.m_Jumped
		  << " hook=" << Net.m_HookState << "," << Net.m_HookedPlayer
		  << "," << Net.m_HookX << "," << Net.m_HookY << '\n';
}

static void RunCore(const std::string &Name, int Ticks)
{
	CCollisionFixture Collision;
	CWorldCore World;
	CCharacterCore Core;
	Core.Init(&World, &Collision.m_Collision);
	Core.Reset();
	Core.m_Pos = vec2(160.0f, 304.0f);
	World.m_apCharacters[0] = &Core;

	CCharacterCore Other;
	if(Name == "hook_player")
	{
		Other.Init(&World, &Collision.m_Collision);
		Other.Reset();
		Other.m_Pos = vec2(300.0f, 304.0f);
		World.m_apCharacters[1] = &Other;
	}

	PrintCore(Name.c_str(), 0, Core);
	for(int Tick = 1; Tick <= Ticks; ++Tick)
	{
		mem_zero(&Core.m_Input, sizeof(Core.m_Input));
		Core.m_Input.m_TargetX = 400;
		Core.m_Input.m_TargetY = 0;
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
		PrintCore(Name.c_str(), Tick, Core);
	}
}

class CGameFixture
{
public:
	CGameFixture()
	{
		mem_zero(&m_Config, sizeof(m_Config));
		m_Config.m_SvMaxClients = MAX_CLIENTS;
		m_Config.m_SvPlayerSlots = MAX_CLIENTS;
		m_Config.m_SvScorelimit = 2;
		m_Config.m_SvTimelimit = 0;
		m_Config.m_SvWarmup = 0;
		m_Config.m_SvTeamdamage = 0;
		m_Game.m_pServer = &m_Server;
		m_Game.m_pConfig = &m_Config;
		m_Game.m_pConsole = CreateConsole(CFGFLAG_SERVER);
		m_Collision.Install(&m_Game.m_Collision);
		m_Game.m_World.SetGameServer(&m_Game);
		m_Game.m_pController = new CGameControllerDM(&m_Game);
		m_Game.m_pController->m_GameState = IGameController::IGS_GAME_RUNNING;
		m_Game.m_pController->m_GameStateTimer = IGameController::TIMER_INFINITE;
		m_Game.m_pController->m_aTeamSize[TEAM_RED] = 2;
		m_Game.m_World.m_Paused = false;
		Spawn(0, vec2(160.0f, 304.0f));
		Spawn(1, vec2(195.0f, 304.0f));
	}

	~CGameFixture()
	{
		delete m_Game.m_pController;
		m_Game.m_pController = 0;
		delete m_Game.m_pConsole;
		m_Game.m_pConsole = 0;
	}

	CCharacter *Character(int ClientID) { return m_Game.m_apPlayers[ClientID]->m_pCharacter; }

	CReferenceServer m_Server;
	CConfig m_Config;
	CCollisionFixture m_Collision;
	CGameContext m_Game;

private:
	void Spawn(int ClientID, vec2 Pos)
	{
		CPlayer *pPlayer = new(ClientID) CPlayer(&m_Game, ClientID, false, false);
		m_Game.m_apPlayers[ClientID] = pPlayer;
		CCharacter *pCharacter = new(ClientID) CCharacter(&m_Game.m_World);
		pPlayer->m_pCharacter = pCharacter;
		pCharacter->Spawn(pPlayer, Pos);
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
	const int Projectiles = Fixture.m_Game.m_World.FindEntities(
		vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	int MinLife = -1;
	int MaxLife = -1;
	for(int i = 0; i < Projectiles; ++i)
	{
		const int Life = static_cast<CProjectile *>(apEntities[i])->m_LifeSpan;
		MinLife = MinLife < 0 ? Life : std::min(MinLife, Life);
		MaxLife = std::max(MaxLife, Life);
	}
	const int Lasers = Fixture.m_Game.m_World.FindEntities(
		vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_LASER);
	int LaserEnergy = -1;
	int LaserBounces = -1;
	if(Lasers)
	{
		CLaser *pLaser = static_cast<CLaser *>(apEntities[0]);
		LaserEnergy = round_to_int(pLaser->m_Energy * 256.0f);
		LaserBounces = pLaser->m_Bounces;
	}
	std::cout << "weapon_tick " << Name << " tick=" << Tick
		  << " ammo=" << pAttacker->m_aWeapons[Weapon].m_Ammo
		  << " reload=" << pAttacker->m_ReloadTimer
		  << " victim=" << pVictim->m_Health << ',' << pVictim->m_Armor
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
		pAttacker->GiveWeapon(Weapon, 10);
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
	const int Projectiles = Fixture.m_Game.m_World.FindEntities(
		vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	const int Lasers = Fixture.m_Game.m_World.FindEntities(
		vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_LASER);
	std::vector<std::string> ProjectileData;
	if(Projectiles)
	{
		Fixture.m_Game.m_World.FindEntities(
			vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
		for(int i = 0; i < Projectiles; ++i)
		{
			CNetObj_Projectile Net;
			static_cast<CProjectile *>(apEntities[i])->FillInfo(&Net);
			std::ostringstream Item;
			Item << Net.m_X << ',' << Net.m_Y << ',' << Net.m_VelX << ',' << Net.m_VelY << ',' << Net.m_Type;
			ProjectileData.push_back(Item.str());
		}
		std::sort(ProjectileData.begin(), ProjectileData.end());
	}
	const int InitialProjectiles = Projectiles;
	const int InitialLasers = Lasers;
	PrintWeaponTick(Fixture, Name, 0, pAttacker, pVictim, Weapon);
	for(int Tick = 1; Tick <= 120; ++Tick)
	{
		Fixture.m_Server.SetTick(Tick);
		Fixture.m_Game.m_World.Tick();
		PrintWeaponTick(Fixture, Name, Tick, pAttacker, pVictim, Weapon);
	}
	const int FinalProjectiles = Fixture.m_Game.m_World.FindEntities(
		vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_PROJECTILE);
	const int FinalLasers = Fixture.m_Game.m_World.FindEntities(
		vec2(200.0f, 304.0f), 1000.0f, apEntities, 32, CGameWorld::ENTTYPE_LASER);
	std::cout << "weapon " << Name
		  << " ammo=" << pAttacker->m_aWeapons[Weapon].m_Ammo
		  << " reload=" << pAttacker->m_ReloadTimer
		  << " projectiles=" << InitialProjectiles << ',' << FinalProjectiles
		  << " lasers=" << InitialLasers << ',' << FinalLasers
		  << " victim=" << pVictim->m_Health << ',' << pVictim->m_Armor
		  << " victim_vel=" << round_to_int(pVictim->m_Core.m_Vel.x * 256.0f)
		  << ',' << round_to_int(pVictim->m_Core.m_Vel.y * 256.0f);
	for(const std::string &Item : ProjectileData)
		std::cout << " projectile=" << Item;
	std::cout << '\n';
}

static void RunDamage(const std::string &Name, int Amount)
{
	CGameFixture Fixture;
	CCharacter *pVictim = Fixture.Character(1);
	pVictim->m_Health = 10;
	pVictim->m_Armor = 5;
	int Weapon = WeaponFromName(Name);
	int From = 0;
	if(Name == "grenade_self")
	{
		Weapon = WEAPON_GRENADE;
		From = 1;
	}
	if(Name == "grenade_explosion")
	{
		Fixture.m_Game.CreateExplosion(pVictim->m_Pos - vec2(32.0f, 0.0f), 0, WEAPON_GRENADE, Amount);
	}
	else
		pVictim->TakeDamage(vec2(2.0f, -3.0f), vec2(-1.0f, 0.0f), Amount, From, Weapon);
	std::cout << "damage " << Name << " amount=" << Amount
		  << " health=" << pVictim->m_Health << " armor=" << pVictim->m_Armor
		  << " vel=" << round_to_int(pVictim->m_Core.m_Vel.x * 256.0f)
		  << ',' << round_to_int(pVictim->m_Core.m_Vel.y * 256.0f) << '\n';
}

static void RunMatch(const std::string &Name)
{
	CGameFixture Fixture;
	CPlayer *pAlpha = Fixture.m_Game.m_apPlayers[0];
	CPlayer *pBeta = Fixture.m_Game.m_apPlayers[1];
	IGameController *pController = Fixture.m_Game.m_pController;
	int RespawnBefore = -1;
	int RespawnAt = -1;
	if(Name == "kill")
		Fixture.Character(1)->Die(0, WEAPON_GUN);
	else if(Name == "suicide")
		Fixture.Character(0)->Die(0, WEAPON_GRENADE);
	else if(Name == "respawn_normal" || Name == "respawn_self")
	{
		pController->m_aaSpawnPoints[0][0] = vec2(256.0f, 304.0f);
		pController->m_aNumSpawnPoints[0] = 1;
		if(Name == "respawn_normal")
		{
			Fixture.Character(0)->Die(1, WEAPON_GUN);
			Fixture.m_Server.SetTick(1);
			pAlpha->Tick();
		}
		else
			pAlpha->KillCharacter(WEAPON_SELF);
		pAlpha->Respawn();
		const int BeforeTick = Name == "respawn_normal" ? 24 : 149;
		const int AtTick = Name == "respawn_normal" ? 25 : 150;
		Fixture.m_Server.SetTick(BeforeTick);
		pAlpha->Tick();
		RespawnBefore = pAlpha->GetCharacter() != 0;
		Fixture.m_Server.SetTick(AtTick);
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
		throw std::runtime_error("unknown match scenario: " + Name);
	const bool Running = pController->m_GameState == IGameController::IGS_GAME_RUNNING;
	const bool Warmup = pController->m_GameState == IGameController::IGS_WARMUP_GAME ||
			    pController->m_GameState == IGameController::IGS_WARMUP_USER;
	const bool Paused = pController->m_GameState == IGameController::IGS_GAME_PAUSED ||
			    pController->m_GameState == IGameController::IGS_START_COUNTDOWN;
	const bool MatchEnded = pController->m_GameState == IGameController::IGS_END_MATCH;
	std::cout << "match " << Name
		  << " score=" << pAlpha->m_Score << ',' << pBeta->m_Score
		  << " sudden_death=" << pController->m_SuddenDeath
		  << " running=" << Running << " warmup=" << Warmup
		  << " paused=" << Paused << " match_ended=" << MatchEnded
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
			throw std::runtime_error("invalid core scenario: " + Line);
		RunCore(Name, Ticks);
	}
	else if(Kind == "weapon")
	{
		if(WeaponFromName(Name) < 0)
			throw std::runtime_error("invalid weapon scenario: " + Line);
		RunWeapon(Name);
	}
	else if(Kind == "damage")
	{
		int Amount;
		if(!(Input >> Amount) || (WeaponFromName(Name) < 0 && Name != "grenade_self" && Name != "grenade_explosion"))
			throw std::runtime_error("invalid damage scenario: " + Line);
		RunDamage(Name, Amount);
	}
	else if(Kind == "match")
		RunMatch(Name);
	else
		throw std::runtime_error("unknown scenario: " + Line);
	std::string Extra;
	if(Input >> Extra)
		throw std::runtime_error("trailing scenario input: " + Line);
}

int main(int argc, char **argv)
{
	if(argc != 2)
	{
		std::cerr << "usage: vanilla-golden-reference SCENARIOS\n";
		return 2;
	}
	std::ifstream Input(argv[1]);
	if(!Input)
	{
		std::cerr << "cannot open scenarios: " << argv[1] << '\n';
		return 2;
	}
	try
	{
		std::string Line;
		while(std::getline(Input, Line))
		{
			const std::string::size_type First = Line.find_first_not_of(" \t\r");
			if(First == std::string::npos || Line[First] == '#')
				continue;
			Dispatch(Line.substr(First));
		}
	}
	catch(const std::exception &Error)
	{
		std::cerr << Error.what() << '\n';
		return 1;
	}
	return 0;
}
