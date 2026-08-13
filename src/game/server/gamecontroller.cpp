/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "gamecontroller.h"

#include "entities/character.h"
#include "entities/laser.h"
#include "entities/pickup.h"
#include "entities/projectile.h"
#include "gamecontext.h"
#include "mode/game_services.h"
#include "player.h"

#include <base/log.h>
#include <base/net.h>
#include <base/time.h>

#include <engine/antibot.h>
#include <engine/config.h>
#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/protocolglue.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/teamscore.h>

#include <algorithm>

IGameController::IGameController(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	m_GameModeInfo(GameModeInfo),
	m_MatchLifecycle(Services.Server()->Tick())
{
	RegisterMapEntityFactory(CreateCommonMapEntity);
	m_pServices = &Services;
	m_pServer = Services.Server();
	m_pGameType = g_Config.m_SvTestingCommands ? m_GameModeInfo.m_pTestingGameType : m_GameModeInfo.m_pGameType;
}

CGameContext *IGameController::GameServer() const
{
	return m_pServices->GameServer();
}

IGameController::~IGameController()
{
	GameServer()->Console()->DeregisterOwner(this);
}

void IGameController::Init(CDbConnectionPool *)
{
	Services().World().m_Core.m_PhysicsRuleset = Info().m_PhysicsRuleset;
	RegisterCommands();
	InitGameSettings();
	Services().World().m_Core.m_OldTeleportHook = Info().m_PhysicsRuleset == EPhysicsRuleset::DDNET && g_Config.m_SvOldTeleportHook;
	Services().World().m_Core.m_OldTeleportWeapons = Info().m_PhysicsRuleset == EPhysicsRuleset::DDNET && g_Config.m_SvOldTeleportWeapons;
	m_pGameType = g_Config.m_SvTestingCommands ? m_GameModeInfo.m_pTestingGameType : m_GameModeInfo.m_pGameType;
	DoWarmup(g_Config.m_SvWarmup);
	m_TeamsCore.Reset();
	if(Match().IsRunning())
		PublishMatchEvent(CMatchEventRoundStarted{});
}

int IGameController::TuningZoneAt(vec2 Position) const
{
	if(!Info().m_UseTuneZones)
		return 0;
	const int MapIndex = GameServer()->Collision()->GetMapIndex(Position);
	return GameServer()->Collision()->IsTune(MapIndex);
}

CPlayer *IGameController::CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team)
{
	return new(ClientId) CPlayer(Services(), UniqueClientId, ClientId, Team);
}

CCharacter *IGameController::CreateCharacter(CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	return new(ClientId) CCharacter(&Services().World(), Services().LastPlayerInput(ClientId));
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

void IGameController::OnPlayerSetTeam(int ClientId, int Team)
{
	if(IsGamePaused())
		return;

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->GetTeam() == Team)
		return;
	if(g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > Server()->Tick())
		return;

	if(pPlayer->m_TeamChangeTick > Server()->Tick())
	{
		pPlayer->m_LastSetTeam = Server()->Tick();
		const int TimeLeft = (pPlayer->m_TeamChangeTick - Server()->Tick()) / Server()->TickSpeed();
		char aTime[32];
		str_time((int64_t)TimeLeft * 100, ETimeFormat::HOURS, aTime, sizeof(aTime));
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Time to wait before changing team: %s", aTime);
		GameServer()->SendBroadcast(aBuf, ClientId);
		return;
	}

	char aTeamJoinError[512];
	if(CanJoinTeam(Team, ClientId, aTeamJoinError, sizeof(aTeamJoinError)))
	{
		if(pPlayer->GetTeam() == TEAM_SPECTATORS || Team == TEAM_SPECTATORS)
			GameServer()->m_VoteUpdate = true;
		DoTeamChange(pPlayer, Team, true);
		pPlayer->m_TeamChangeTick = Server()->Tick();
	}
	else
		GameServer()->SendBroadcast(aTeamJoinError, ClientId);
}

void IGameController::OnPlayerKill(int ClientId)
{
	if(IsGamePaused())
		return;

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->m_LastKill && pPlayer->m_LastKill + Server()->TickSpeed() * g_Config.m_SvKillDelay > Server()->Tick())
		return;
	if(pPlayer->IsPaused() || !pPlayer->GetCharacter())
		return;

	pPlayer->m_LastKill = Server()->Tick();
	pPlayer->KillCharacter(WEAPON_SELF);
	pPlayer->Respawn();
}

void IGameController::OnPlayerCallKickVote(int ClientId, int TargetId, const char *pReason)
{
	if(g_Config.m_SvVoteKickMin)
	{
		int NumPlayers = 0;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!GameServer()->m_apPlayers[i] || GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS)
				continue;

			++NumPlayers;
			for(int j = 0; j < i; ++j)
			{
				if(GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->GetTeam() != TEAM_SPECTATORS &&
					!net_addr_comp_noport(Server()->ClientAddr(i), Server()->ClientAddr(j)))
				{
					--NumPlayers;
					break;
				}
			}
		}

		if(NumPlayers < g_Config.m_SvVoteKickMin)
		{
			char aMessage[128];
			str_format(aMessage, sizeof(aMessage), "Kick voting requires %d players", g_Config.m_SvVoteKickMin);
			GameServer()->SendChatTarget(ClientId, aMessage);
			return;
		}
	}

	char aChatMessage[512];
	str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to kick '%s' (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
	char aSixupDescription[VOTE_DESC_LENGTH];
	str_format(aSixupDescription, sizeof(aSixupDescription), "%2d: %s", TargetId, Server()->ClientName(TargetId));
	char aCommand[VOTE_CMD_LENGTH];
	char aDescription[VOTE_DESC_LENGTH];
	if(!g_Config.m_SvVoteKickBantime)
	{
		str_format(aCommand, sizeof(aCommand), "kick %d Kicked by vote", TargetId);
		str_format(aDescription, sizeof(aDescription), "Kick '%s'", Server()->ClientName(TargetId));
	}
	else
	{
		str_format(aCommand, sizeof(aCommand), "ban %s %d Banned by vote", Server()->ClientAddrString(TargetId, false), g_Config.m_SvVoteKickBantime);
		str_format(aDescription, sizeof(aDescription), "Ban '%s'", Server()->ClientName(TargetId));
	}

	GameServer()->m_apPlayers[ClientId]->m_LastKickVote = time_get();
	GameServer()->m_VoteType = CGameContext::VOTE_TYPE_KICK;
	GameServer()->m_VoteVictim = TargetId;
	GameServer()->CallVote(ClientId, aDescription, aCommand, pReason, aChatMessage, aSixupDescription);
}

void IGameController::OnPlayerCallSpectateVote(int ClientId, int TargetId, const char *pReason)
{
	char aChatMessage[512];
	str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to move '%s' to spectators (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
	char aDescription[VOTE_DESC_LENGTH];
	str_format(aDescription, sizeof(aDescription), "Move '%s' to spectators", Server()->ClientName(TargetId));
	char aSixupDescription[VOTE_DESC_LENGTH];
	str_format(aSixupDescription, sizeof(aSixupDescription), "%2d: %s", TargetId, Server()->ClientName(TargetId));
	char aCommand[VOTE_CMD_LENGTH];
	str_format(aCommand, sizeof(aCommand), "set_team %d -1 %d", TargetId, g_Config.m_SvVoteSpectateRejoindelay);

	GameServer()->m_VoteType = CGameContext::VOTE_TYPE_SPECTATE;
	GameServer()->m_VoteVictim = TargetId;
	GameServer()->CallVote(ClientId, aDescription, aCommand, pReason, aChatMessage, aSixupDescription);
}

bool IGameController::CanPlayerVoteOnTargetVote(int, int VoterId) const
{
	return GameServer()->m_apPlayers[VoterId]->GetTeam() != TEAM_SPECTATORS;
}

int IGameController::PlayerVetoActivityStartTick(int ClientId) const
{
	return GameServer()->m_apPlayers[ClientId]->m_JoinTick;
}

int IGameController::PlayerTeamGroup(int ClientId) const
{
	return GameServer()->m_apPlayers[ClientId]->GetTeam();
}

bool IGameController::CanPlayerReceivePreInput(int, int) const
{
	return true;
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
	return m_EntityRegistry.CreateMapEntity(*this, {Index, x, y, Layer, Flags, Initial, Number});
}

bool IGameController::CreateCommonMapEntity(IGameController &Controller, const CMapEntityContext &Context)
{
	const vec2 Pos(Context.m_X * 32.0f + 16.0f, Context.m_Y * 32.0f + 16.0f);

	if(Context.m_Index >= ENTITY_SPAWN && Context.m_Index <= ENTITY_SPAWN_BLUE)
	{
		if(Context.m_Initial)
		{
			const int SpawnType = Context.m_Index - ENTITY_SPAWN;
			Controller.m_avSpawnPoints[SpawnType].push_back(Pos);
		}
		return true;
	}

	int Type = -1;
	int SubType = 0;

	if(Context.m_Index == ENTITY_ARMOR_1)
		Type = POWERUP_ARMOR;
	else if(Context.m_Index == ENTITY_ARMOR_SHOTGUN)
		Type = POWERUP_ARMOR_SHOTGUN;
	else if(Context.m_Index == ENTITY_ARMOR_GRENADE)
		Type = POWERUP_ARMOR_GRENADE;
	else if(Context.m_Index == ENTITY_ARMOR_NINJA)
		Type = POWERUP_ARMOR_NINJA;
	else if(Context.m_Index == ENTITY_ARMOR_LASER)
		Type = POWERUP_ARMOR_LASER;
	else if(Context.m_Index == ENTITY_HEALTH_1)
		Type = POWERUP_HEALTH;
	else if(Context.m_Index == ENTITY_WEAPON_SHOTGUN)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_SHOTGUN;
	}
	else if(Context.m_Index == ENTITY_WEAPON_GRENADE)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_GRENADE;
	}
	else if(Context.m_Index == ENTITY_WEAPON_LASER)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_LASER;
	}
	else if(Context.m_Index == ENTITY_POWERUP_NINJA)
	{
		Type = POWERUP_NINJA;
		SubType = WEAPON_NINJA;
	}
	if(Type != -1) // NOLINT(clang-analyzer-unix.Malloc)
	{
		const int PickupFlags = Controller.TileFlagsToPickupFlags(Context.m_Flags);
		CPickup *pPickup = new CPickup(&Controller.GameServer()->m_World, Type, SubType, Context.m_Layer, Context.m_Number, PickupFlags);
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
			Msg.m_GameFlags = Info().m_GameFlags;
			Msg.m_MatchCurrent = 1;
			Msg.m_MatchNum = 0;
			Msg.m_ScoreLimit = ScoreLimit();
			Msg.m_TimeLimit = TimeLimit();
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}

		// Override Sixup's built-in /team only when the active mode provides its own command.
		if(GameServer()->Console()->GetCommandInfo("team", CFGFLAG_CHAT, false))
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

int IGameController::PlayerAutoRespawnTick(const CPlayer *pPlayer) const
{
	return pPlayer->m_DieTick + 2;
}

IGameModeMapReloadState *IGameController::MapReloadState() const
{
	return GameServer()->GameHost().MapReloadState();
}

void IGameController::DiscardMapReloadState(int ClientId)
{
	GameServer()->GameHost().DiscardMapReloadState(ClientId);
}

void IGameController::RestoreCharacterAfterMapReload(CCharacter *pCharacter)
{
	DiscardMapReloadState(pCharacter->GetPlayer()->GetCid());
}

void IGameController::EndRound()
{
	if(!Match().EndRound(Server()->Tick()))
		return;

	SetGamePaused(true);
	log_info("game", "end round type='%s'", m_pGameType);
	PublishMatchEvent(CMatchEventRoundEnded{});
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
	if(Match().IsGameOver() && !Paused)
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

	Match().StartRound(Server()->Tick());
	SetGamePaused(false);
	Server()->DemoRecorder_HandleAutoStart();
	log_info("game", "start round type='%s' teamplay='%d'", m_pGameType, Info().m_GameFlags & GAMEFLAG_TEAMS);
	PublishMatchEvent(CMatchEventRoundStarted{});
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

void IGameController::FinalizeCharacterDeath(const CGameCharacterDeathContext &Context, int ModeSpecial)
{
	Context.m_pVictim->FinalizeDeath(Context.m_Killer, Context.m_Weapon, Context.m_SendKillMessage, ModeSpecial);
	if(Context.m_Weapon == WEAPON_GAME)
		return;
	const int VictimId = Context.m_pVictim->GetPlayer()->GetCid();
	if(Context.m_Killer == VictimId)
		PublishMatchEvent(CMatchEventSuicide{VictimId, Context.m_Weapon});
	else
		PublishMatchEvent(CMatchEventKill{Context.m_Killer, VictimId, Context.m_Weapon});
}

void IGameController::OnCharacterDeath(const CGameCharacterDeathContext &Context)
{
	FinalizeCharacterDeath(Context);
}

bool IGameController::OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam)
{
	if(Damage)
		pVictim->SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);

	pVictim->AddVelocity(Force);
	return true;
}

bool IGameController::CanCharacterHitCharacter(CCharacter *, CCharacter *pTarget) const
{
	return pTarget->IsAlive();
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

CGamePickupResult IGameController::OnCharacterPickup(CCharacter *, int, int, vec2)
{
	return {};
}

CGameProjectileRules IGameController::ProjectileRules(const CGameProjectileContext &Context) const
{
	return {true, false, 0.001f, Context.m_OwnerConnected ? EProjectileOwnerLossAction::KEEP : EProjectileOwnerLossAction::DETACH};
}

void IGameController::OnExplosion(const CGameExplosionContext &Context)
{
	GameServer()->CreateExplosionEvent(Context.m_Position, Context.m_Mask);
	if(Context.m_NoDamage)
		return;

	CEntity *apEntities[MAX_CLIENTS];
	constexpr float Radius = 135.0f;
	constexpr float InnerRadius = 48.0f;
	const int Num = GameServer()->m_World.FindEntities(Context.m_Position, Radius, apEntities, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; i++)
	{
		auto *pCharacter = static_cast<CCharacter *>(apEntities[i]);
		const vec2 Difference = pCharacter->m_Pos - Context.m_Position;
		const float Distance = length(Difference);
		const vec2 ForceDirection = Distance > 0.0f ? normalize(Difference) : vec2(0.0f, 1.0f);
		const float Falloff = 1.0f - std::clamp((Distance - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		const float Damage = GameServer()->GlobalTuning()->m_ExplosionStrength * Falloff;
		if((int)Damage == 0)
			continue;
		pCharacter->TakeDamage(ForceDirection * Damage * 2.0f, (int)Damage, Context.m_Owner, Context.m_Weapon, true, Context.m_AttackerTeam);
	}
}

void IGameController::OnCharacterSpawn(class CCharacter *pChr)
{
	pChr->SetTeamsCore(&TeamsCore());

	// default health
	pChr->IncreaseHealth(10);

	// give default weapons
	pChr->GiveWeapon(WEAPON_HAMMER);
	pChr->GiveWeapon(WEAPON_GUN);
}

void IGameController::TickCharacterPostCore(CCharacter *pCharacter)
{
	if(pCharacter->GameLayerClipped(pCharacter->m_Pos) || pCharacter->IsOnDeathTile())
		pCharacter->Die(pCharacter->GetPlayer()->GetCid(), WEAPON_WORLD);
}

void IGameController::DoWarmup(int Seconds)
{
	Match().SetWarmupTicks(Seconds < 0 ? 0 : Seconds * Server()->TickSpeed());
}

void IGameController::Tick()
{
	if(Match().TickWarmup())
		StartRound();

	if(Match().ShouldRestartRound(Server()->Tick(), Server()->TickSpeed() * 10))
	{
		StartRound();
		Match().AdvanceRound();
	}

	DoActivityCheck();
}

void IGameController::Snap(int SnappingClient)
{
	CNetObj_GameInfo GameInfo = {};

	GameInfo.m_GameFlags = GameFlags_ClampToSix(Info().m_GameFlags);
	GameInfo.m_GameStateFlags = 0;
	if(Match().IsGameOver())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(Match().IsSuddenDeath())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(IsGamePaused())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	GameInfo.m_RoundStartTick = Match().RoundStartTick();
	GameInfo.m_WarmupTimer = Match().WarmupTicks();

	GameInfo.m_RoundNum = 0;
	GameInfo.m_RoundCurrent = Match().RoundCount() + 1;
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
		GameData.m_GameStartTick = Match().RoundStartTick();
		GameData.m_GameStateFlags = 0;
		if(Match().IsGameOver())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_GAMEOVER;
		if(Match().IsSuddenDeath())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_SUDDENDEATH;
		if(IsGamePaused())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_PAUSED;
		GameData.m_GameStateEndTick = TimeLimit() > 0 ? Match().RoundStartTick() + TimeLimit() * Server()->TickSpeed() * 60 : 0;
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

int IGameController::ActivePlayerSlots() const
{
	const int ConfiguredSlots = std::max(0, Server()->MaxClients() - g_Config.m_SvSpectatorSlots);
	if(m_GameModeInfo.m_ActivePlayerLimit <= 0)
		return ConfiguredSlots;
	return std::min(ConfiguredSlots, m_GameModeInfo.m_ActivePlayerLimit);
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

	if((aNumplayers[0] + aNumplayers[1]) < ActivePlayerSlots())
		return true;

	if(pErrorReason)
		str_format(pErrorReason, ErrorReasonSize, "Only %d active players are allowed", ActivePlayerSlots());
	return false;
}

CClientMask IGameController::GetMaskForPlayerWorldEvent(int, int ExceptId)
{
	CClientMask Mask = CClientMask().set();
	if(ExceptId != -1)
		Mask.reset(ExceptId);
	return Mask;
}

void IGameController::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(!IsValidTeam(Team))
		return;

	if(Team == pPlayer->GetTeam())
		return;

	const int OldTeam = pPlayer->GetTeam();
	pPlayer->SetTeam(Team);
	int ClientId = pPlayer->GetCid();

	char aBuf[128];
	if(DoChatMsg)
	{
		str_format(aBuf, sizeof(aBuf), "'%s' joined the %s", Server()->ClientName(ClientId), GetTeamName(Team));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf);
	}

	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' m_Team=%d", ClientId, Server()->ClientName(ClientId), Team);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	PublishMatchEvent(CMatchEventTeamChanged{ClientId, OldTeam, Team});

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
