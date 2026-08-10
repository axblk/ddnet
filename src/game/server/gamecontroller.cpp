/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "gamecontroller.h"

#include "entities/character.h"
#include "entities/door.h"
#include "entities/dragger.h"
#include "entities/gun.h"
#include "entities/laser.h"
#include "entities/light.h"
#include "entities/pickup.h"
#include "entities/projectile.h"
#include "gamecontext.h"
#include "player.h"

#include <base/log.h>

#include <engine/antibot.h>
#include <engine/config.h>
#include <engine/shared/config.h>
#include <engine/shared/protocolglue.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/server/score.h>
#include <game/teamscore.h>

IGameController::IGameController(class CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	m_Teams(pGameServer), m_GameModeInfo(GameModeInfo), m_pLoadBestTimeResult(nullptr)
{
	m_pGameServer = pGameServer;
	m_pConfig = m_pGameServer->Config();
	m_pServer = m_pGameServer->Server();
	m_pGameType = g_Config.m_SvTestingCommands ? m_GameModeInfo.m_pTestingGameType : m_GameModeInfo.m_pGameType;

	//
	m_Warmup = 0;
	m_GameOverTick = -1;
	m_SuddenDeath = 0;
	m_RoundStartTick = Server()->Tick();
	m_RoundCount = 0;
	m_GameFlags = m_GameModeInfo.m_GameFlags;
	m_aMapWish[0] = 0;

	m_CurrentRecord.reset();
}

IGameController::~IGameController() = default;

void IGameController::Init()
{
	InitGameSettings();
	m_pGameType = g_Config.m_SvTestingCommands ? m_GameModeInfo.m_pTestingGameType : m_GameModeInfo.m_pGameType;
	DoWarmup(g_Config.m_SvWarmup);
	m_Teams.Reset();
}

void IGameController::LoadGameSettings()
{
	GameServer()->ConfigManager()->SetGameSettingsReadOnly(false);
	GameServer()->ConfigManager()->SetReadOnly("sv_gametype", true);
	GameServer()->Console()->ExecuteFile(g_Config.m_SvResetFile, IConsole::CLIENT_ID_UNSPECIFIED);
	GameServer()->LoadMapSettings();
	GameServer()->ConfigManager()->SetReadOnly("sv_gametype", false);
	GameServer()->ConfigManager()->SetGameSettingsReadOnly(true);
}

void IGameController::InitGameSettings()
{
	for(int i = 0; i < TuneZone::NUM; i++)
	{
		GameServer()->TuningList()[i] = CTuningParams::DEFAULT;
		GameServer()->m_aaZoneEnterMsg[i][0] = 0;
		GameServer()->m_aaZoneLeaveMsg[i][0] = 0;
	}
	if(g_Config.m_SvTuneReset)
		ResetTuning();

	LoadGameSettings();
}

void IGameController::ResetTuning()
{
	*GameServer()->GlobalTuning() = CTuningParams::DEFAULT;
	GameServer()->SendTuningParams(-1);
}

void IGameController::DoActivityCheck()
{
	if(g_Config.m_SvInactiveKickTime == 0)
		return;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS && !Server()->IsRconAuthed(i))
		{
			if(Server()->Tick() > GameServer()->m_apPlayers[i]->m_LastActionTick + g_Config.m_SvInactiveKickTime * Server()->TickSpeed() * 60)
			{
				switch(g_Config.m_SvInactiveKick)
				{
				case 0:
				{
					// move player to spectator
					DoTeamChange(GameServer()->m_apPlayers[i], TEAM_SPECTATORS, true);
				}
				break;
				case 1:
				{
					// move player to spectator if the reserved slots aren't filled yet, kick them otherwise
					int Spectators = 0;
					for(auto &pPlayer : GameServer()->m_apPlayers)
						if(pPlayer && pPlayer->GetTeam() == TEAM_SPECTATORS)
							++Spectators;
					if(Spectators >= g_Config.m_SvSpectatorSlots)
						Server()->Kick(i, "Kicked for inactivity");
					else
						DoTeamChange(GameServer()->m_apPlayers[i], TEAM_SPECTATORS, true);
				}
				break;
				case 2:
				{
					// kick the player
					Server()->Kick(i, "Kicked for inactivity");
				}
				}
			}
		}
	}
}

float IGameController::EvaluateSpawnPos(CSpawnEval *pEval, vec2 Pos, int ClientId)
{
	float Score = 0.0f;
	CCharacter *pC = static_cast<CCharacter *>(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_CHARACTER));
	for(; pC; pC = (CCharacter *)pC->TypeNext())
	{
		if(!pC->CanCollide(ClientId))
			continue;

		const float TeamWeight = pEval->m_FriendlyTeam != -1 && pC->GetPlayer()->GetTeam() == pEval->m_FriendlyTeam ? 0.5f : 1.0f;
		const float d = distance(Pos, pC->m_Pos);
		Score += TeamWeight * (d == 0 ? 1000000000.0f : 1.0f / d);
	}

	return Score;
}

void IGameController::EvaluateSpawnType(CSpawnEval *pEval, ESpawnType SpawnType, int ClientId)
{
	const bool PlayerCollision = GameServer()->GlobalTuning()->m_PlayerCollision;

	bool PlayerCollisionDisabled = false;
	CCharacter *pPlayerCharacter = GameServer()->GetPlayerChar(ClientId);
	if(pPlayerCharacter)
		PlayerCollisionDisabled = pPlayerCharacter->GetCore().m_CollisionDisabled;

	// make sure players keep spawning at the same tile
	// on race maps no matter what
	if(!PlayerCollision && pEval->m_Got)
		return;

	// j == 0: Find an empty slot, j == 1: Take any slot if no empty one found
	for(int j = 0; j < 2; j++)
	{
		// get spawn point
		for(const vec2 &SpawnPoint : m_avSpawnPoints[SpawnType])
		{
			vec2 P = SpawnPoint;
			if(j == 0)
			{
				// check if the position is occupado
				CEntity *apEnts[MAX_CLIENTS];
				int Num = GameServer()->m_World.FindEntities(SpawnPoint, 64, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
				vec2 aPositions[5] = {vec2(0.0f, 0.0f), vec2(-32.0f, 0.0f), vec2(0.0f, -32.0f), vec2(32.0f, 0.0f), vec2(0.0f, 32.0f)}; // start, left, up, right, down
				int Result = -1;
				for(int Index = 0; Index < 5 && Result == -1; ++Index)
				{
					Result = Index;
					if(!PlayerCollision || PlayerCollisionDisabled)
						break;
					for(int c = 0; c < Num; ++c)
					{
						CCharacter *pChr = static_cast<CCharacter *>(apEnts[c]);
						const bool CanCollide = pChr->CanCollide(ClientId) && !pChr->GetCore().m_CollisionDisabled;

						if(GameServer()->Collision()->CheckPoint(SpawnPoint + aPositions[Index]) ||
							(CanCollide && distance(pChr->m_Pos, SpawnPoint + aPositions[Index]) <= pChr->GetProximityRadius()))
						{
							Result = -1;
							break;
						}
					}
				}
				if(Result == -1)
					continue; // try next spawn point

				P += aPositions[Result];
			}

			float S = EvaluateSpawnPos(pEval, P, ClientId);
			if(!pEval->m_Got || (j == 0 && pEval->m_Score > S))
			{
				pEval->m_Got = true;
				pEval->m_Score = S;
				pEval->m_Pos = P;
			}
		}
	}
}

bool IGameController::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	// spectators can't spawn
	if(Team == TEAM_SPECTATORS)
		return false;

	CSpawnEval Eval;
	EvaluateSpawnType(&Eval, SPAWNTYPE_DEFAULT, ClientId);
	EvaluateSpawnType(&Eval, SPAWNTYPE_RED, ClientId);
	EvaluateSpawnType(&Eval, SPAWNTYPE_BLUE, ClientId);

	*pOutPos = Eval.m_Pos;
	return Eval.m_Got;
}

bool IGameController::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	dbg_assert(Index >= 0, "Invalid entity index");

	const vec2 Pos(x * 32.0f + 16.0f, y * 32.0f + 16.0f);

	int aSides[8];
	aSides[0] = GameServer()->Collision()->Entity(x, y + 1, Layer);
	aSides[1] = GameServer()->Collision()->Entity(x + 1, y + 1, Layer);
	aSides[2] = GameServer()->Collision()->Entity(x + 1, y, Layer);
	aSides[3] = GameServer()->Collision()->Entity(x + 1, y - 1, Layer);
	aSides[4] = GameServer()->Collision()->Entity(x, y - 1, Layer);
	aSides[5] = GameServer()->Collision()->Entity(x - 1, y - 1, Layer);
	aSides[6] = GameServer()->Collision()->Entity(x - 1, y, Layer);
	aSides[7] = GameServer()->Collision()->Entity(x - 1, y + 1, Layer);

	if(Index >= ENTITY_SPAWN && Index <= ENTITY_SPAWN_BLUE && Initial)
	{
		const int SpawnType = Index - ENTITY_SPAWN;
		m_avSpawnPoints[SpawnType].push_back(Pos);
	}
	else if(Index == ENTITY_DOOR)
	{
		for(int i = 0; i < 8; i++)
		{
			if(aSides[i] >= ENTITY_LASER_SHORT && aSides[i] <= ENTITY_LASER_LONG)
			{
				new CDoor(
					&GameServer()->m_World, //GameWorld
					Pos, //Pos
					pi / 4 * i, //Rotation
					32 * 3 + 32 * (aSides[i] - ENTITY_LASER_SHORT) * 3, //Length
					Number //Number
				);
			}
		}
	}
	else if(Index == ENTITY_CRAZY_SHOTGUN_EX)
	{
		int Dir;
		if(!Flags)
			Dir = 0;
		else if(Flags == ROTATION_90)
			Dir = 1;
		else if(Flags == ROTATION_180)
			Dir = 2;
		else
			Dir = 3;
		float Deg = Dir * (pi / 2);
		CProjectile *pBullet = new CProjectile(
			&GameServer()->m_World,
			WEAPON_SHOTGUN, //Type
			-1, //Owner
			Pos, //Pos
			vec2(std::sin(Deg), std::cos(Deg)), //Dir
			-2, //Span
			true, //Freeze
			true, //Explosive
			(g_Config.m_SvShotgunBulletSound) ? SOUND_GRENADE_EXPLODE : -1, //SoundImpact
			vec2(std::sin(Deg), std::cos(Deg)), // InitDir
			Layer,
			Number);
		pBullet->SetBouncing(2 - (Dir % 2));
	}
	else if(Index == ENTITY_CRAZY_SHOTGUN)
	{
		int Dir;
		if(!Flags)
			Dir = 0;
		else if(Flags == (TILEFLAG_ROTATE))
			Dir = 1;
		else if(Flags == (TILEFLAG_XFLIP | TILEFLAG_YFLIP))
			Dir = 2;
		else
			Dir = 3;
		float Deg = Dir * (pi / 2);
		CProjectile *pBullet = new CProjectile(
			&GameServer()->m_World,
			WEAPON_SHOTGUN, //Type
			-1, //Owner
			Pos, //Pos
			vec2(std::sin(Deg), std::cos(Deg)), //Dir
			-2, //Span
			true, //Freeze
			false, //Explosive
			SOUND_GRENADE_EXPLODE,
			vec2(std::sin(Deg), std::cos(Deg)), // InitDir
			Layer,
			Number);
		pBullet->SetBouncing(2 - (Dir % 2));
	}

	int Type = -1;
	int SubType = 0;

	if(Index == ENTITY_ARMOR_1)
		Type = POWERUP_ARMOR;
	else if(Index == ENTITY_ARMOR_SHOTGUN)
		Type = POWERUP_ARMOR_SHOTGUN;
	else if(Index == ENTITY_ARMOR_GRENADE)
		Type = POWERUP_ARMOR_GRENADE;
	else if(Index == ENTITY_ARMOR_NINJA)
		Type = POWERUP_ARMOR_NINJA;
	else if(Index == ENTITY_ARMOR_LASER)
		Type = POWERUP_ARMOR_LASER;
	else if(Index == ENTITY_HEALTH_1)
		Type = POWERUP_HEALTH;
	else if(Index == ENTITY_WEAPON_SHOTGUN)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_SHOTGUN;
	}
	else if(Index == ENTITY_WEAPON_GRENADE)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_GRENADE;
	}
	else if(Index == ENTITY_WEAPON_LASER)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_LASER;
	}
	else if(Index == ENTITY_POWERUP_NINJA)
	{
		Type = POWERUP_NINJA;
		SubType = WEAPON_NINJA;
	}
	else if(Index >= ENTITY_LASER_FAST_CCW && Index <= ENTITY_LASER_FAST_CW)
	{
		int aSides2[8];
		aSides2[0] = GameServer()->Collision()->Entity(x, y + 2, Layer);
		aSides2[1] = GameServer()->Collision()->Entity(x + 2, y + 2, Layer);
		aSides2[2] = GameServer()->Collision()->Entity(x + 2, y, Layer);
		aSides2[3] = GameServer()->Collision()->Entity(x + 2, y - 2, Layer);
		aSides2[4] = GameServer()->Collision()->Entity(x, y - 2, Layer);
		aSides2[5] = GameServer()->Collision()->Entity(x - 2, y - 2, Layer);
		aSides2[6] = GameServer()->Collision()->Entity(x - 2, y, Layer);
		aSides2[7] = GameServer()->Collision()->Entity(x - 2, y + 2, Layer);

		int Ind = Index - ENTITY_LASER_STOP;
		int M;
		if(Ind < 0)
		{
			Ind = -Ind;
			M = 1;
		}
		else if(Ind == 0)
			M = 0;
		else
			M = -1;

		float AngularSpeed = 0.0f;
		if(Ind == 0)
			AngularSpeed = 0.0f;
		else if(Ind == 1)
			AngularSpeed = pi / 360;
		else if(Ind == 2)
			AngularSpeed = pi / 180;
		else if(Ind == 3)
			AngularSpeed = pi / 90;
		AngularSpeed *= M;

		for(int i = 0; i < 8; i++)
		{
			if(aSides[i] >= ENTITY_LASER_SHORT && aSides[i] <= ENTITY_LASER_LONG)
			{
				CLight *pLight = new CLight(&GameServer()->m_World, Pos, pi / 4 * i, 32 * 3 + 32 * (aSides[i] - ENTITY_LASER_SHORT) * 3, Layer, Number);
				pLight->m_AngularSpeed = AngularSpeed;
				if(aSides2[i] >= ENTITY_LASER_C_SLOW && aSides2[i] <= ENTITY_LASER_C_FAST)
				{
					pLight->m_Speed = 1 + (aSides2[i] - ENTITY_LASER_C_SLOW) * 2;
					pLight->m_CurveLength = pLight->m_Length;
				}
				else if(aSides2[i] >= ENTITY_LASER_O_SLOW && aSides2[i] <= ENTITY_LASER_O_FAST)
				{
					pLight->m_Speed = 1 + (aSides2[i] - ENTITY_LASER_O_SLOW) * 2;
					pLight->m_CurveLength = 0;
				}
				else
					pLight->m_CurveLength = pLight->m_Length;
			}
		}
	}
	else if(Index >= ENTITY_DRAGGER_WEAK && Index <= ENTITY_DRAGGER_STRONG)
	{
		new CDragger(&GameServer()->m_World, Pos, Index - ENTITY_DRAGGER_WEAK + 1, false, Layer, Number);
	}
	else if(Index >= ENTITY_DRAGGER_WEAK_NW && Index <= ENTITY_DRAGGER_STRONG_NW)
	{
		new CDragger(&GameServer()->m_World, Pos, Index - ENTITY_DRAGGER_WEAK_NW + 1, true, Layer, Number);
	}
	else if(Index == ENTITY_PLASMAE)
	{
		new CGun(&GameServer()->m_World, Pos, false, true, Layer, Number);
	}
	else if(Index == ENTITY_PLASMAF)
	{
		new CGun(&GameServer()->m_World, Pos, true, false, Layer, Number);
	}
	else if(Index == ENTITY_PLASMA)
	{
		new CGun(&GameServer()->m_World, Pos, true, true, Layer, Number);
	}
	else if(Index == ENTITY_PLASMAU)
	{
		new CGun(&GameServer()->m_World, Pos, false, false, Layer, Number);
	}

	if(Type != -1) // NOLINT(clang-analyzer-unix.Malloc)
	{
		int PickupFlags = TileFlagsToPickupFlags(Flags);
		CPickup *pPickup = new CPickup(&GameServer()->m_World, Type, SubType, Layer, Number, PickupFlags);
		pPickup->m_Pos = Pos;
		return true; // NOLINT(clang-analyzer-unix.Malloc)
	}

	return false;
}

void IGameController::OnPlayerConnect(CPlayer *pPlayer)
{
	int ClientId = pPlayer->GetCid();
	pPlayer->Respawn();

	if(!Server()->ClientPrevIngame(ClientId))
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' team=%d", ClientId, Server()->ClientName(ClientId), pPlayer->GetTeam());
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	if(Server()->IsSixup(ClientId))
	{
		{
			protocol7::CNetMsg_Sv_GameInfo Msg;
			Msg.m_GameFlags = m_GameFlags;
			Msg.m_MatchCurrent = 1;
			Msg.m_MatchNum = 0;
			Msg.m_ScoreLimit = ScoreLimit();
			Msg.m_TimeLimit = TimeLimit();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}

		// /team is essential
		{
			protocol7::CNetMsg_Sv_CommandInfoRemove Msg;
			Msg.m_pName = "team";
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}
	}
}

void IGameController::OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason)
{
	pPlayer->OnDisconnect();
	int ClientId = pPlayer->GetCid();
	if(Server()->ClientIngame(ClientId))
	{
		char aBuf[512];
		if(pReason && *pReason)
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game (%s)", Server()->ClientName(ClientId), pReason);
		else
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game", Server()->ClientName(ClientId));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf, -1);

		str_format(aBuf, sizeof(aBuf), "leave player='%d:%s'", ClientId, Server()->ClientName(ClientId));
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "game", aBuf);
	}
}

void IGameController::EndRound()
{
	if(m_Warmup) // game can't end when we are running warmup
		return;

	SetGamePaused(true);
	m_GameOverTick = Server()->Tick();
	m_SuddenDeath = 0;
	log_info("game", "end round type='%s'", m_pGameType);
}

void IGameController::ResetGame()
{
	GameServer()->m_World.m_ResetRequested = true;
}

bool IGameController::IsValidTeam(int Team)
{
	return Team == TEAM_SPECTATORS || Team == TEAM_GAME;
}

const char *IGameController::GetTeamName(int Team)
{
	switch(Team)
	{
	case TEAM_SPECTATORS:
		return "spectators";
	case TEAM_GAME:
		return "game";
	default:
		dbg_assert_failed("Invalid Team: %d", Team);
	}
}

void IGameController::SetGamePaused(bool Paused)
{
	// Cannot unpause the game while gameover is active
	if(m_GameOverTick != -1 && !Paused)
	{
		return;
	}
	GameServer()->m_World.m_Paused = Paused;
}

bool IGameController::IsGamePaused() const
{
	return GameServer()->m_World.m_Paused;
}

void IGameController::StartRound()
{
	ResetGame();

	m_RoundStartTick = Server()->Tick();
	m_SuddenDeath = 0;
	m_GameOverTick = -1;
	SetGamePaused(false);
	Server()->DemoRecorder_HandleAutoStart();
	log_info("game", "start round type='%s' teamplay='%d'", m_pGameType, m_GameFlags & GAMEFLAG_TEAMS);
}

void IGameController::ChangeMap(const char *pToMap)
{
	Server()->ChangeMap(pToMap);
}

void IGameController::OnReset()
{
	for(auto &pPlayer : GameServer()->m_apPlayers)
		if(pPlayer)
			pPlayer->Respawn();
}

int IGameController::OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon)
{
	return 0;
}

bool IGameController::OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam)
{
	if(Damage)
		pVictim->SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);

	pVictim->AddVelocity(Force);
	return true;
}

bool IGameController::CanCharacterHitCharacter(CCharacter *pAttacker, CCharacter *pTarget) const
{
	return pTarget->IsAlive() && pAttacker->CanCollide(pTarget->GetPlayer()->GetCid());
}

CWeaponFireResult IGameController::OnCharacterFireWeapon(const CWeaponFireContext &Context)
{
	CCharacter *pCharacter = Context.m_pCharacter;
	if(!pCharacter || Context.m_Weapon < 0 || Context.m_Weapon >= NUM_WEAPONS || !pCharacter->GetWeaponAmmo(Context.m_Weapon))
		return {};

	CWeaponFireResult Result;
	Result.m_Fired = true;
	const int Owner = pCharacter->GetPlayer()->GetCid();

	switch(Context.m_Weapon)
	{
	case WEAPON_HAMMER:
	{
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_HAMMER_FIRE, pCharacter->TeamMask());
		GameServer()->Antibot()->OnHammerFire(Owner);
		if(pCharacter->HammerHitDisabled())
			break;

		CEntity *apEntities[MAX_CLIENTS];
		const int NumEntities = GameServer()->m_World.FindEntities(
			Context.m_ProjectileStartPosition,
			pCharacter->GetProximityRadius() * 0.5f,
			apEntities,
			MAX_CLIENTS,
			CGameWorld::ENTTYPE_CHARACTER);
		int Hits = 0;
		for(int i = 0; i < NumEntities; i++)
		{
			auto *pTarget = static_cast<CCharacter *>(apEntities[i]);
			if(pTarget == pCharacter || !CanCharacterHitCharacter(pCharacter, pTarget))
				continue;

			if(length(pTarget->m_Pos - Context.m_ProjectileStartPosition) > 0.0f)
				GameServer()->CreateHammerHit(pTarget->m_Pos - normalize(pTarget->m_Pos - Context.m_ProjectileStartPosition) * pCharacter->GetProximityRadius() * 0.5f, pCharacter->TeamMask());
			else
				GameServer()->CreateHammerHit(Context.m_ProjectileStartPosition, pCharacter->TeamMask());

			const vec2 Direction = length(pTarget->m_Pos - pCharacter->m_Pos) > 0.0f ? normalize(pTarget->m_Pos - pCharacter->m_Pos) : vec2(0.0f, -1.0f);
			const vec2 VelocityDelta = pTarget->VelocityDeltaAfterClamping(normalize(Direction + vec2(0.0f, -1.1f)) * 10.0f);
			pTarget->TakeDamage(
				(vec2(0.0f, -1.0f) + VelocityDelta) * Context.m_pTuning->m_HammerStrength,
				g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage,
				Owner,
				Context.m_Weapon);
			pTarget->Unfreeze();
			GameServer()->Antibot()->OnHammerHit(Owner, pTarget->GetPlayer()->GetCid());
			Hits++;
		}

		if(Hits)
			Result.m_ReloadTicks = Context.m_pTuning->m_HammerHitFireDelay * Server()->TickSpeed() / 1000;
		break;
	}
	case WEAPON_GUN:
		if(!pCharacter->Core()->m_Jetpack || !pCharacter->GetPlayer()->m_NinjaJetpack || pCharacter->HasTelegunGun())
		{
			new CProjectile(
				pCharacter->GameWorld(),
				WEAPON_GUN,
				Owner,
				Context.m_ProjectileStartPosition,
				Context.m_Direction,
				Server()->TickSpeed() * Context.m_pTuning->m_GunLifetime,
				false,
				false,
				-1,
				Context.m_MouseTarget);
			GameServer()->CreateSound(pCharacter->m_Pos, SOUND_GUN_FIRE, pCharacter->TeamMask());
		}
		break;
	case WEAPON_SHOTGUN:
		new CLaser(pCharacter->GameWorld(), pCharacter->m_Pos, Context.m_Direction, Context.m_pTuning->m_LaserReach, Owner, WEAPON_SHOTGUN);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_SHOTGUN_FIRE, pCharacter->TeamMask());
		break;
	case WEAPON_GRENADE:
		new CProjectile(
			pCharacter->GameWorld(),
			WEAPON_GRENADE,
			Owner,
			Context.m_ProjectileStartPosition,
			Context.m_Direction,
			Server()->TickSpeed() * Context.m_pTuning->m_GrenadeLifetime,
			false,
			true,
			SOUND_GRENADE_EXPLODE,
			Context.m_MouseTarget);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_GRENADE_FIRE, pCharacter->TeamMask());
		break;
	case WEAPON_LASER:
		new CLaser(pCharacter->GameWorld(), pCharacter->m_Pos, Context.m_Direction, Context.m_pTuning->m_LaserReach, Owner, WEAPON_LASER);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_LASER_FIRE, pCharacter->TeamMask());
		break;
	case WEAPON_NINJA:
		pCharacter->ActivateNinja(Context.m_Direction);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_NINJA_FIRE, pCharacter->TeamMask());
		break;
	default:
		return {};
	}

	return Result;
}

CGamePickupResult IGameController::OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position)
{
	bool Sound = false;
	switch(Type)
	{
	case POWERUP_HEALTH:
		if(pCharacter->Freeze())
			GameServer()->CreateSound(Position, SOUND_PICKUP_HEALTH, pCharacter->TeamMask());
		break;
	case POWERUP_ARMOR:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		for(int Weapon = WEAPON_SHOTGUN; Weapon < NUM_WEAPONS; Weapon++)
		{
			if(pCharacter->GetWeaponGot(Weapon))
			{
				pCharacter->SetWeaponGot(Weapon, false);
				pCharacter->SetWeaponAmmo(Weapon, 0);
				Sound = true;
			}
		}
		pCharacter->SetNinjaActivationDir(vec2(0, 0));
		pCharacter->SetNinjaActivationTick(-500);
		pCharacter->SetNinjaCurrentMoveTime(0);
		if(Sound)
		{
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() >= WEAPON_SHOTGUN)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_ARMOR_SHOTGUN:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		if(pCharacter->GetWeaponGot(WEAPON_SHOTGUN))
		{
			pCharacter->SetWeaponGot(WEAPON_SHOTGUN, false);
			pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 0);
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() == WEAPON_SHOTGUN)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_ARMOR_GRENADE:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		if(pCharacter->GetWeaponGot(WEAPON_GRENADE))
		{
			pCharacter->SetWeaponGot(WEAPON_GRENADE, false);
			pCharacter->SetWeaponAmmo(WEAPON_GRENADE, 0);
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() == WEAPON_GRENADE)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_ARMOR_NINJA:
		if(pCharacter->Team() != TEAM_SUPER)
		{
			pCharacter->SetNinjaActivationDir(vec2(0, 0));
			pCharacter->SetNinjaActivationTick(-500);
			pCharacter->SetNinjaCurrentMoveTime(0);
		}
		break;
	case POWERUP_ARMOR_LASER:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		if(pCharacter->GetWeaponGot(WEAPON_LASER))
		{
			pCharacter->SetWeaponGot(WEAPON_LASER, false);
			pCharacter->SetWeaponAmmo(WEAPON_LASER, 0);
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() == WEAPON_LASER)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_WEAPON:
		if(Subtype >= 0 && Subtype < NUM_WEAPONS && (!pCharacter->GetWeaponGot(Subtype) || pCharacter->GetWeaponAmmo(Subtype) != -1))
		{
			pCharacter->GiveWeapon(Subtype);
			if(Subtype == WEAPON_GRENADE)
				GameServer()->CreateSound(Position, SOUND_PICKUP_GRENADE, pCharacter->TeamMask());
			else if(Subtype == WEAPON_SHOTGUN || Subtype == WEAPON_LASER)
				GameServer()->CreateSound(Position, SOUND_PICKUP_SHOTGUN, pCharacter->TeamMask());
			if(pCharacter->GetPlayer())
				GameServer()->SendWeaponPickup(pCharacter->GetPlayer()->GetCid(), Subtype);
		}
		break;
	case POWERUP_NINJA:
		pCharacter->GiveNinja();
		break;
	default:
		break;
	}
	return {};
}

CGameProjectileRules IGameController::ProjectileRules(const CGameProjectileContext &Context) const
{
	const EProjectileOwnerLossAction OwnerLossAction = Context.m_Weapon != WEAPON_GRENADE || g_Config.m_SvDestroyBulletsOnDeath || Context.m_BelongsToPracticeTeam ? EProjectileOwnerLossAction::DESTROY : EProjectileOwnerLossAction::KEEP;
	return {
		Context.m_pOwner ? !Context.m_pOwner->GrenadeHitDisabled() : g_Config.m_SvHit != 0,
		true,
		0.0f,
		OwnerLossAction,
	};
}

void IGameController::OnCharacterSpawn(class CCharacter *pChr)
{
	pChr->SetTeams(&Teams());
	Teams().OnCharacterSpawn(pChr->GetPlayer()->GetCid());

	// default health
	pChr->IncreaseHealth(10);

	// give default weapons
	pChr->GiveWeapon(WEAPON_HAMMER);
	pChr->GiveWeapon(WEAPON_GUN);
}

void IGameController::HandleCharacterTiles(CCharacter *pChr, int MapIndex)
{
	// Do nothing by default
}

void IGameController::DoWarmup(int Seconds)
{
	if(Seconds < 0)
		m_Warmup = 0;
	else
		m_Warmup = Seconds * Server()->TickSpeed();
}

void IGameController::Tick()
{
	// do warmup
	if(m_Warmup)
	{
		m_Warmup--;
		if(!m_Warmup)
			StartRound();
	}

	if(m_GameOverTick != -1)
	{
		// game over.. wait for restart
		if(Server()->Tick() > m_GameOverTick + Server()->TickSpeed() * 10)
		{
			StartRound();
			m_RoundCount++;
		}
	}

	if(m_pLoadBestTimeResult != nullptr && m_pLoadBestTimeResult->m_Completed)
	{
		if(m_pLoadBestTimeResult->m_Success)
		{
			m_CurrentRecord = m_pLoadBestTimeResult->m_CurrentRecord;

			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetClientVersion() >= VERSION_DDRACE)
				{
					GameServer()->SendRecord(i);
				}
			}
		}
		m_pLoadBestTimeResult = nullptr;
	}

	DoActivityCheck();
}

void IGameController::Snap(int SnappingClient)
{
	CNetObj_GameInfo GameInfo = {};

	GameInfo.m_GameFlags = GameFlags_ClampToSix(m_GameFlags);
	GameInfo.m_GameStateFlags = 0;
	if(m_GameOverTick != -1)
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(m_SuddenDeath)
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(IsGamePaused())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	GameInfo.m_RoundStartTick = m_RoundStartTick;
	GameInfo.m_WarmupTimer = m_Warmup;

	GameInfo.m_RoundNum = 0;
	GameInfo.m_RoundCurrent = m_RoundCount + 1;
	UpdateGameInfo(GameInfo, SnappingClient);
	Server()->SnapNewItem(0, GameInfo);

	CNetObj_GameInfoEx GameInfoEx = {};
	GameInfoEx.m_Flags = GameInfoFlags(SnappingClient);
	GameInfoEx.m_Flags2 = GameInfoFlags2(SnappingClient);
	GameInfoEx.m_Version = GAMEINFO_CURVERSION;
	Server()->SnapNewItem(0, GameInfoEx);

	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_GameData GameData = {};
		GameData.m_GameStartTick = m_RoundStartTick;
		GameData.m_GameStateFlags = 0;
		if(m_GameOverTick != -1)
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_GAMEOVER;
		if(m_SuddenDeath)
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_SUDDENDEATH;
		if(IsGamePaused())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_PAUSED;
		GameData.m_GameStateEndTick = TimeLimit() > 0 ? m_RoundStartTick + TimeLimit() * Server()->TickSpeed() * 60 : 0;
		Server()->SnapNewItem(0, GameData);
	}

	SnapMode(SnappingClient);
}

int IGameController::GetAutoTeam(int NotThisId)
{
	int Team = TEAM_GAME;

	if(CanJoinTeam(Team, NotThisId, nullptr, 0))
		return Team;
	return TEAM_SPECTATORS;
}

bool IGameController::CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize)
{
	const CPlayer *pPlayer = GameServer()->m_apPlayers[NotThisId];
	if(pPlayer && pPlayer->IsPaused())
	{
		if(pErrorReason)
			str_copy(pErrorReason, "Use /pause first then you can kill", ErrorReasonSize);
		return false;
	}
	if(Team == TEAM_SPECTATORS || (pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS))
		return true;

	int aNumplayers[2] = {0, 0};
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GameServer()->m_apPlayers[i] && i != NotThisId)
		{
			if(GameServer()->m_apPlayers[i]->GetTeam() >= TEAM_RED && GameServer()->m_apPlayers[i]->GetTeam() <= TEAM_BLUE)
				aNumplayers[GameServer()->m_apPlayers[i]->GetTeam()]++;
		}
	}

	if((aNumplayers[0] + aNumplayers[1]) < Server()->MaxClients() - g_Config.m_SvSpectatorSlots)
		return true;

	if(pErrorReason)
		str_format(pErrorReason, ErrorReasonSize, "Only %d active players are allowed", Server()->MaxClients() - g_Config.m_SvSpectatorSlots);
	return false;
}

CClientMask IGameController::GetMaskForPlayerWorldEvent(int Asker, int ExceptId)
{
	if(Asker == -1)
		return CClientMask().set().reset(ExceptId);

	return Teams().TeamMask(GameServer()->GetDDRaceTeam(Asker), ExceptId, Asker);
}

void IGameController::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(!IsValidTeam(Team))
		return;

	if(Team == pPlayer->GetTeam())
		return;

	pPlayer->SetTeam(Team);
	int ClientId = pPlayer->GetCid();

	char aBuf[128];
	if(DoChatMsg)
	{
		str_format(aBuf, sizeof(aBuf), "'%s' joined the %s", Server()->ClientName(ClientId), GameServer()->m_pController->GetTeamName(Team));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf);
	}

	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' m_Team=%d", ClientId, Server()->ClientName(ClientId), Team);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// OnPlayerInfoChange(pPlayer);
}

int IGameController::TileFlagsToPickupFlags(int TileFlags) const
{
	int PickupFlags = 0;
	if(TileFlags & TILEFLAG_XFLIP)
		PickupFlags |= PICKUPFLAG_XFLIP;
	if(TileFlags & TILEFLAG_YFLIP)
		PickupFlags |= PICKUPFLAG_YFLIP;
	if(TileFlags & TILEFLAG_ROTATE)
		PickupFlags |= PICKUPFLAG_ROTATE;
	return PickupFlags;
}
