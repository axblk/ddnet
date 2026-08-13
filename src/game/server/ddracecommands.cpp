/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#include "gamecontext.h"
#include "gamemodes/ddrace.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/time.h>

#include <engine/antibot.h>
#include <engine/shared/config.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/server/gamemodes/ddrace_character.h>
#include <game/server/player.h>
#include <game/server/save.h>
#include <game/server/teams.h>

#include <algorithm>
#include <cmath>

namespace
{
	CGameControllerDDRace &RaceController(CGameContext *pGameServer)
	{
		return *static_cast<CGameControllerDDRace *>(pGameServer->GameHost().Controller());
	}

	CGameTeams *RaceTeams(CGameContext *pGameServer)
	{
		return &RaceController(pGameServer).RaceTeams();
	}

	struct CCommandRegistration
	{
		const char *m_pName;
		const char *m_pParams;
		int m_Flags;
		IConsole::FCommandCallback m_pfnCallback;
		const char *m_pHelp;
	};
}

static void MoveCharacter(CGameContext *pGameServer, int ClientId, int X, int Y, bool Raw = false);
static void ModifyWeapons(IConsole::IResult *pResult, void *pUserData, int Weapon, bool Remove);
static void Teleport(CCharacterDDRace *pCharacter, vec2 Pos);
static CCharacter *GetPracticeCharacter(CGameContext *pGameServer, IConsole::IResult *pResult);

static void ConGoLeft(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Tiles = pResult->NumArguments() == 1 ? pResult->GetInteger(0) : 1;

	if(!CheckClientId(pResult->m_ClientId))
		return;
	MoveCharacter(pSelf, pResult->m_ClientId, -1 * Tiles, 0);
}

static void ConGoRight(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Tiles = pResult->NumArguments() == 1 ? pResult->GetInteger(0) : 1;

	if(!CheckClientId(pResult->m_ClientId))
		return;
	MoveCharacter(pSelf, pResult->m_ClientId, Tiles, 0);
}

static void ConGoDown(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Tiles = pResult->NumArguments() == 1 ? pResult->GetInteger(0) : 1;

	if(!CheckClientId(pResult->m_ClientId))
		return;
	MoveCharacter(pSelf, pResult->m_ClientId, 0, Tiles);
}

static void ConGoUp(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Tiles = pResult->NumArguments() == 1 ? pResult->GetInteger(0) : 1;

	if(!CheckClientId(pResult->m_ClientId))
		return;
	MoveCharacter(pSelf, pResult->m_ClientId, 0, -1 * Tiles);
}

static void ConMove(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	MoveCharacter(pSelf, pResult->m_ClientId, pResult->GetInteger(0),
		pResult->GetInteger(1));
}

static void ConMoveRaw(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	MoveCharacter(pSelf, pResult->m_ClientId, pResult->GetInteger(0),
		pResult->GetInteger(1), true);
}

static void MoveCharacter(CGameContext *pGameServer, int ClientId, int X, int Y, bool Raw)
{
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pGameServer->GetPlayerChar(ClientId));

	if(!pChr)
		return;

	pChr->Move(vec2((Raw ? 1 : 32) * X, (Raw ? 1 : 32) * Y));
	pChr->ResetVelocity();
	pChr->m_DDRaceState = ERaceState::CHEATED;
}

void CGameContext::ConKillPlayer(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	int Victim = pResult->GetVictim();

	if(pSelf->m_apPlayers[Victim])
	{
		pSelf->m_apPlayers[Victim]->KillCharacter(WEAPON_GAME);
		char aBuf[512];
		if(pResult->NumArguments() == 2)
			str_format(aBuf, sizeof(aBuf), "%s was killed by authorized player (%s)",
				pSelf->Server()->ClientName(Victim),
				pResult->GetString(1));
		else
			str_format(aBuf, sizeof(aBuf), "%s was killed by authorized player",
				pSelf->Server()->ClientName(Victim));
		pSelf->SendChat(-1, TEAM_ALL, aBuf);
	}
}

static void ConNinja(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_NINJA, false);
}

static void ConUnNinja(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_NINJA, true);
}

static void ConEndlessHook(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
	{
		pChr->SetEndlessHook(true);
	}
}

static void ConUnEndlessHook(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
	{
		pChr->SetEndlessHook(false);
	}
}

static void ConSuper(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(pResult->m_ClientId));
	if(pChr && !pChr->IsSuper())
	{
		pChr->SetSuper(true);
		pChr->Unfreeze();
	}
}

static void ConUnSuper(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(pResult->m_ClientId));
	if(pChr && pChr->IsSuper())
	{
		pChr->SetSuper(false);
	}
}

static void ConToggleInvincible(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(pResult->m_ClientId));
	if(pChr)
		pChr->SetInvincible(pResult->NumArguments() == 0 ? !pChr->Core()->m_Invincible : pResult->GetInteger(0));
}

static void ConSolo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetSolo(true);
}

static void ConUnSolo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetSolo(false);
}

static void ConFreeze(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->Freeze();
}

static void ConUnfreeze(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->Unfreeze();
}

static void ConDeep(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetDeepFrozen(true);
}

static void ConUnDeep(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
	{
		pChr->SetDeepFrozen(false);
		pChr->Unfreeze();
	}
}

static void ConLiveFreeze(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetLiveFrozen(true);
}

static void ConUnLiveFreeze(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetLiveFrozen(false);
}

static void ConShotgun(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_SHOTGUN, false);
}

static void ConGrenade(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_GRENADE, false);
}

static void ConLaser(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_LASER, false);
}

static void ConJetpack(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetJetpack(true);
}

static void ConEndlessJump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetEndlessJump(true);
}

static void ConSetJumps(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetJumps(pResult->GetInteger(0));
}

static void ConWeapons(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, -1, false);
}

static void ConUnShotgun(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_SHOTGUN, true);
}

static void ConUnGrenade(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_GRENADE, true);
}

static void ConUnLaser(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, WEAPON_LASER, true);
}

static void ConUnJetpack(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetJetpack(false);
}

static void ConUnEndlessJump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(pChr)
		pChr->SetEndlessJump(false);
}

static void ConSetSwitch(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacter *pChr = pSelf->GetPlayerChar(pResult->m_ClientId);
	if(!pChr)
	{
		log_info("chatresp", "You can't set switch while you are dead/a spectator.");
		return;
	}
	const int Team = pChr->Team();
	const int Switch = pResult->GetInteger(0);
	if(!in_range(Switch, (int)pSelf->Switchers().size() - 1))
	{
		log_info("chatresp", "Invalid switch ID");
		return;
	}
	const bool State = pResult->NumArguments() == 1 ? !pSelf->Switchers()[Switch].m_aStatus[Team] : pResult->GetInteger(1) != 0;
	const int EndTick = pResult->NumArguments() == 3 ? pSelf->Server()->Tick() + 1 + pResult->GetInteger(2) * pSelf->Server()->TickSpeed() : 0;
	pSelf->Switchers()[Switch].m_aStatus[Team] = State;
	pSelf->Switchers()[Switch].m_aEndTick[Team] = EndTick;
	if(State)
		pSelf->Switchers()[Switch].m_aType[Team] = EndTick ? TILE_SWITCHTIMEDOPEN : TILE_SWITCHOPEN;
	else
		pSelf->Switchers()[Switch].m_aType[Team] = EndTick ? TILE_SWITCHTIMEDCLOSE : TILE_SWITCHCLOSE;
}

static void ConUnWeapons(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, -1, true);
}

static void ConAddWeapon(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, pResult->GetInteger(0), false);
}

static void ConRemoveWeapon(IConsole::IResult *pResult, void *pUserData)
{
	ModifyWeapons(pResult, pUserData, pResult->GetInteger(0), true);
}

static void ModifyWeapons(IConsole::IResult *pResult, void *pUserData,
	int Weapon, bool Remove)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(pResult->m_ClientId));
	if(!pChr)
		return;

	if(std::clamp(Weapon, -1, NUM_WEAPONS - 1) != Weapon)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "info",
			"invalid weapon id");
		return;
	}

	if(Weapon == -1)
	{
		pChr->GiveWeapon(WEAPON_SHOTGUN, Remove);
		pChr->GiveWeapon(WEAPON_GRENADE, Remove);
		pChr->GiveWeapon(WEAPON_LASER, Remove);
	}
	else
	{
		pChr->GiveWeapon(Weapon, Remove);
	}

	pChr->m_DDRaceState = ERaceState::CHEATED;
}

static void Teleport(CCharacterDDRace *pChr, vec2 Pos)
{
	pChr->SetPosition(Pos);
	pChr->m_Pos = Pos;
	pChr->m_PrevPos = Pos;
	pChr->m_DDRaceState = ERaceState::CHEATED;
}

static void ConToTeleporter(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	unsigned int TeleTo = pResult->GetInteger(0);

	if(!pSelf->Collision()->TeleOuts(TeleTo - 1).empty())
	{
		CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(pResult->m_ClientId));
		if(pChr)
		{
			int TeleOut = pSelf->m_World.m_Core.RandomOr0(pSelf->Collision()->TeleOuts(TeleTo - 1).size());
			Teleport(pChr, pSelf->Collision()->TeleOuts(TeleTo - 1)[TeleOut]);
		}
	}
}

static void ConToCheckTeleporter(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	unsigned int TeleTo = pResult->GetInteger(0);

	if(!pSelf->Collision()->TeleCheckOuts(TeleTo - 1).empty())
	{
		CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(pResult->m_ClientId));
		if(pChr)
		{
			int TeleOut = pSelf->m_World.m_Core.RandomOr0(pSelf->Collision()->TeleCheckOuts(TeleTo - 1).size());
			Teleport(pChr, pSelf->Collision()->TeleCheckOuts(TeleTo - 1)[TeleOut]);
			pChr->m_TeleCheckpoint = TeleTo;
		}
	}
}

static void ConTeleport(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	int Tele = pResult->NumArguments() == 2 ? pResult->GetInteger(0) : pResult->m_ClientId;
	int TeleTo = pResult->NumArguments() ? pResult->GetInteger(pResult->NumArguments() - 1) : pResult->m_ClientId;
	int AuthLevel = pSelf->Server()->GetAuthedState(pResult->m_ClientId);

	if(Tele != pResult->m_ClientId && AuthLevel < g_Config.m_SvTeleOthersAuthLevel)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "tele", "you aren't allowed to tele others");
		return;
	}

	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pSelf->GetPlayerChar(Tele));
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];

	if(pChr && pPlayer && pSelf->GetPlayerChar(TeleTo))
	{
		// default to view pos when character is not available
		vec2 Pos = pPlayer->m_ViewPos;
		if(pResult->NumArguments() == 0 && !pPlayer->IsPaused() && pChr->IsAlive())
		{
			vec2 Target = vec2(pChr->Core()->m_Input.m_TargetX, pChr->Core()->m_Input.m_TargetY);
			Pos = pPlayer->m_CameraInfo.ConvertTargetToWorld(pChr->GetPos(), Target);
		}
		Teleport(pChr, Pos);
		pChr->ResetJumps();
		pChr->Unfreeze();
		pChr->SetVelocity(vec2(0, 0));
	}
}

void CGameContext::ConKill(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];

	if(!pPlayer || (pPlayer->m_LastKill && pPlayer->m_LastKill + pSelf->Server()->TickSpeed() * g_Config.m_SvKillDelay > pSelf->Server()->Tick()))
		return;

	pPlayer->m_LastKill = pSelf->Server()->Tick();
	pPlayer->KillCharacter(WEAPON_SELF);
}

static void ConForcePause(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int Victim = pResult->GetVictim();
	int Seconds = 0;
	if(pResult->NumArguments() > 1)
		Seconds = std::clamp(pResult->GetInteger(1), 0, 360);

	CPlayer *pPlayer = pSelf->m_apPlayers[Victim];
	if(!pPlayer)
		return;

	pPlayer->ForcePause(Seconds);
}

void CGameContext::ConModerate(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	bool HadModerator = pSelf->PlayerModerating();

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	pPlayer->m_Moderating = !pPlayer->m_Moderating;

	if(!HadModerator && pPlayer->m_Moderating)
		pSelf->SendChat(-1, TEAM_ALL, "Server kick/spec votes will now be actively moderated.", 0);

	if(!pSelf->PlayerModerating())
		pSelf->SendChat(-1, TEAM_ALL, "Server kick/spec votes are no longer actively moderated.", 0);

	if(pPlayer->m_Moderating)
		pSelf->SendChatTarget(pResult->m_ClientId, "Active moderator mode enabled for you.");
	else
		pSelf->SendChatTarget(pResult->m_ClientId, "Active moderator mode disabled for you.");
}

static void ConSetDDRTeam(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CGameTeams &Teams = *RaceTeams(pSelf);

	if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
	{
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "join",
			"Teams are disabled");
		return;
	}

	const int Target = pResult->GetVictim();
	CPlayer *pPlayer = pSelf->m_apPlayers[Target];
	if(!pPlayer)
		return;

	const int Team = pResult->GetInteger(1);
	if(!Teams.IsValidTeamNumber(Team))
		return;

	CCharacter *pChr = pSelf->GetPlayerChar(Target);

	if((Teams.m_Core.Team(Target) && Teams.GetDDRaceState(pPlayer) == ERaceState::STARTED) || (pChr && Teams.IsPractice(pChr->Team())))
		pPlayer->KillCharacter(WEAPON_GAME);

	Teams.SetForceCharacterTeam(Target, Team);
	Teams.SetTeamLock(Team, true);
}

static void ConUninvite(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	const int Target = pResult->GetVictim();
	if(!pSelf->m_apPlayers[Target])
		return;

	RaceTeams(pSelf)->SetClientInvited(pResult->GetInteger(1), Target, false);
}

void CGameContext::ConVoteNo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	pSelf->ForceVote(false);
}

static void ConDrySave(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer || !pSelf->Server()->IsRconAuthedAdmin(pResult->m_ClientId))
		return;

	CSaveTeam SavedTeam;
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	ESaveResult Result = SavedTeam.Save(pSelf, RaceTeams(pSelf), Team, true);
	if(CSaveTeam::HandleSaveError(Result, pResult->m_ClientId, pSelf))
		return;

	char aTimestamp[32];
	str_timestamp(aTimestamp, sizeof(aTimestamp));
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "%s_%s_%s.save", pSelf->Map()->BaseName(), aTimestamp, pSelf->Server()->GetAuthName(pResult->m_ClientId));
	IOHANDLE File = pSelf->Storage()->OpenFile(aBuf, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;

	int Len = str_length(SavedTeam.GetString());
	io_write(File, SavedTeam.GetString(), Len);
	io_close(File);
}

static void ConPractice(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	if(pSelf->ProcessSpamProtection(pResult->m_ClientId, false))
		return;

	if(!g_Config.m_SvPractice)
	{
		log_info("chatresp", "Practice mode is disabled");
		return;
	}

	CGameTeams &Teams = *RaceTeams(pSelf);

	int Team = Teams.m_Core.Team(pResult->m_ClientId);

	if(!Teams.IsValidTeamNumber(Team) || (Team == TEAM_FLOCK && g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO))
	{
		log_info("chatresp", "Join a team to enable practice mode, which means you can use /r, but can't earn a rank.");
		return;
	}

	if(Teams.TeamFlock(Team))
	{
		log_info("chatresp", "Practice mode can't be enabled in team 0 mode.");
		return;
	}

	if(Teams.GetSaving(Team))
	{
		log_info("chatresp", "Practice mode can't be enabled while team save or load is in progress");
		return;
	}

	if(Teams.IsPractice(Team))
	{
		log_info("chatresp", "Team is already in practice mode");
		return;
	}

	bool VotedForPractice = pResult->NumArguments() == 0 || pResult->GetInteger(0);

	if(VotedForPractice == Teams.PlayerState(pResult->m_ClientId).m_VotedForPractice)
		return;

	Teams.PlayerState(pResult->m_ClientId).m_VotedForPractice = VotedForPractice;

	int NumCurrentVotes = 0;
	int TeamSize = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(Teams.m_Core.Team(i) == Team)
		{
			CPlayer *pPlayer2 = pSelf->m_apPlayers[i];
			if(pPlayer2 && Teams.PlayerState(i).m_VotedForPractice)
				NumCurrentVotes++;
			TeamSize++;
		}
	}

	int NumRequiredVotes = TeamSize / 2 + 1;

	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "'%s' voted to %s /practice mode for your team, which means you can use practice commands, but you can't earn a rank. Type /practice to vote (%d/%d required votes)", pSelf->Server()->ClientName(pResult->m_ClientId), VotedForPractice ? "enable" : "disable", NumCurrentVotes, NumRequiredVotes);
	pSelf->SendChatTeam(Team, aBuf);

	if(NumCurrentVotes >= NumRequiredVotes)
	{
		Teams.SetPractice(Team, true);
		pSelf->SendChatTeam(Team, "Practice mode enabled for your team, happy practicing!");
		pSelf->SendChatTeam(Team, "See /practicecmdlist for a list of all available practice commands. Most commonly used ones are /telecursor, /lasttp and /rescue");
	}
}

static void ConUnPractice(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;

	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	if(pSelf->ProcessSpamProtection(pResult->m_ClientId, false))
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);

	int Team = Teams.m_Core.Team(pResult->m_ClientId);

	if(g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO && Team == TEAM_FLOCK)
	{
		log_info("chatresp", "Practice mode can't be disabled for team 0");
		return;
	}

	if(!Teams.IsPractice(Team))
	{
		log_info("chatresp", "Team isn't in practice mode");
		return;
	}

	if(Teams.GetSaving(Team))
	{
		log_info("chatresp", "Practice mode can't be disabled while team save or load is in progress");
		return;
	}

	if(Teams.TeamSize(Team) > g_Config.m_SvMaxTeamSize && RaceTeams(pSelf)->TeamLocked(Team))
	{
		log_info("chatresp", "Can't disable practice. This team exceeds the maximum allowed size of %d players for regular team", g_Config.m_SvMaxTeamSize);
		return;
	}

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(Teams.m_Core.Team(i) == Team)
		{
			CPlayer *pPlayer2 = pSelf->m_apPlayers[i];
			if(pPlayer2)
			{
				if(Teams.PlayerState(i).m_VotedForPractice)
					Teams.PlayerState(i).m_VotedForPractice = false;

				if(!g_Config.m_SvPauseable && pPlayer2->IsPaused() == -1 * CPlayer::PAUSE_SPEC)
					pPlayer2->Pause(CPlayer::PAUSE_PAUSED, true);
			}
		}
	}

	// send before kill, in case team isn't locked
	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "'%s' disabled practice mode for your team", pSelf->Server()->ClientName(pResult->m_ClientId));
	pSelf->SendChatTeam(Team, aBuf);

	Teams.KillCharacterOrTeam(pResult->m_ClientId, Team);
	Teams.SetPractice(Team, false);
	pPlayer->Respawn(); // set spawn as strong hook
}

static void ConPracticeCmdList(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;

	char aPracticeCommands[256] = "Available practice commands: ";
	for(const IConsole::ICommandInfo *pCmd = pSelf->Console()->FirstCommandInfo(pResult->m_ClientId, CMDFLAG_PRACTICE);
		pCmd; pCmd = pSelf->Console()->NextCommandInfo(pCmd, pResult->m_ClientId, CMDFLAG_PRACTICE))
	{
		char aCommand[64];

		str_format(aCommand, sizeof(aCommand), "/%s%s", pCmd->Name(), pSelf->Console()->NextCommandInfo(pCmd, pResult->m_ClientId, CMDFLAG_PRACTICE) ? ", " : "");

		if(str_length(aCommand) + str_length(aPracticeCommands) > 255)
		{
			pSelf->SendChatTarget(pResult->m_ClientId, aPracticeCommands);
			aPracticeCommands[0] = '\0';
		}
		str_append(aPracticeCommands, aCommand);
	}
	pSelf->SendChatTarget(pResult->m_ClientId, aPracticeCommands);
}

static void ConRescue(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pPlayer->GetCharacter());
	if(!pChr)
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!g_Config.m_SvRescue && !Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "Rescue is not enabled on this server and you're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}

	bool GoRescue = true;
	auto &PlayerState = Teams.PlayerState(pPlayer->GetCid());

	if(PlayerState.m_RescueMode == RESCUEMODE_MANUAL)
	{
		// if character can't set their rescue state then we should rescue them instead
		GoRescue = !pChr->TrySetRescue(RESCUEMODE_MANUAL);
	}

	if(GoRescue)
	{
		if(pChr->Rescue())
			pChr->Unfreeze();
	}
}

static void ConRescueMode(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);
	auto &PlayerState = Teams.PlayerState(pPlayer->GetCid());
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!g_Config.m_SvRescue && !Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "Rescue is not enabled on this server and you're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}

	if(str_comp_nocase(pResult->GetString(0), "auto") == 0)
	{
		if(PlayerState.m_RescueMode != RESCUEMODE_AUTO)
		{
			PlayerState.m_RescueMode = RESCUEMODE_AUTO;

			pSelf->SendChatTarget(pPlayer->GetCid(), "Rescue mode changed to auto.");
		}

		return;
	}

	if(str_comp_nocase(pResult->GetString(0), "manual") == 0)
	{
		if(PlayerState.m_RescueMode != RESCUEMODE_MANUAL)
		{
			PlayerState.m_RescueMode = RESCUEMODE_MANUAL;

			pSelf->SendChatTarget(pPlayer->GetCid(), "Rescue mode changed to manual.");
		}

		return;
	}

	if(str_comp_nocase(pResult->GetString(0), "list") == 0)
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "Available rescue modes: auto, manual");
	}
	else if(str_comp_nocase(pResult->GetString(0), "") == 0)
	{
		char aBuf[64];
		str_format(aBuf, sizeof(aBuf), "Current rescue mode: %s.", PlayerState.m_RescueMode == RESCUEMODE_MANUAL ? "manual" : "auto");
		pSelf->SendChatTarget(pPlayer->GetCid(), aBuf);
	}
	else
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "Unknown argument. Check '/rescuemode list'");
	}
}

static void ConBack(IConsole::IResult *pResult, void *pUserData)
{
	auto *pSelf = static_cast<CGameContext *>(pUserData);
	if(auto *pChr = static_cast<CCharacterDDRace *>(GetPracticeCharacter(pSelf, pResult)))
	{
		auto *pPlayer = pChr->GetPlayer();
		auto &PlayerState = RaceTeams(pSelf)->PlayerState(pPlayer->GetCid());
		if(!PlayerState.m_LastDeath.has_value())
		{
			pSelf->SendChatTarget(pPlayer->GetCid(), "There is nowhere to go back to.");
			return;
		}
		pChr->GetLastRescueTeeRef(PlayerState.m_RescueMode) = PlayerState.m_LastDeath.value();
		if(pChr->Rescue())
			pChr->Unfreeze();
	}
}

static void ConTeleTo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pCallingPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pCallingPlayer)
		return;
	CCharacterDDRace *pCallingCharacter = static_cast<CCharacterDDRace *>(pCallingPlayer->GetCharacter());
	if(!pCallingCharacter)
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pCallingPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}

	vec2 Pos = {};

	if(pResult->NumArguments() == 0)
	{
		// Set calling tee's position to the origin of its spectating viewport
		Pos = pCallingPlayer->m_ViewPos;
	}
	else
	{
		const CPlayer *pDestPlayer = pSelf->FindPlayerByName(pResult->GetString(0));
		if(!pDestPlayer)
		{
			pSelf->SendChatTarget(pCallingPlayer->GetCid(), "No player with this name found.");
			return;
		}
		const CCharacter *pDestCharacter = pDestPlayer->GetCharacter();
		if(!pDestCharacter)
			return;

		// Set calling tee's position to that of the destination tee
		Pos = pDestCharacter->m_Pos;
	}

	// Teleport tee
	Teleport(pCallingCharacter, Pos);
	pCallingCharacter->ResetJumps();
	pCallingCharacter->Unfreeze();
	pCallingCharacter->ResetVelocity();
	Teams.SaveLastTeleport(pCallingCharacter);
}

static void ConTeleXY(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pCallingPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pCallingPlayer)
		return;
	CCharacterDDRace *pCallingCharacter = static_cast<CCharacterDDRace *>(pCallingPlayer->GetCharacter());
	if(!pCallingCharacter)
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pCallingPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}

	vec2 Pos = {};

	if(pResult->NumArguments() != 2)
	{
		pSelf->SendChatTarget(pCallingPlayer->GetCid(), "Can't recognize specified arguments. Usage: /tpxy x y, e.g. /tpxy 9 3.");
		return;
	}
	else
	{
		float BaseX = 0.f, BaseY = 0.f;

		CMapItemLayerTilemap *pGameLayer = pSelf->Layers()->GameLayer();
		constexpr float OuterKillTileBoundaryDistance = 201 * 32.f;
		float MapWidth = (pGameLayer->m_Width * 32) + (OuterKillTileBoundaryDistance * 2.f), MapHeight = (pGameLayer->m_Height * 32) + (OuterKillTileBoundaryDistance * 2.f);

		const auto DetermineCoordinateRelativity = [](const char *pInString, const float AbsoluteDefaultValue, float &OutFloat) -> bool {
			// mode 0 = abs, 1 = sub, 2 = add

			// Relative?
			const char *pStrDelta = str_startswith(pInString, "~");

			float d;
			if(!str_tofloat(pStrDelta ? pStrDelta : pInString, &d))
				return false;

			// Is the number valid?
			if(std::isnan(d) || std::isinf(d))
				return false;

			// Convert our gleaned 'display' coordinate to an actual map coordinate
			d *= 32.f;

			OutFloat = (pStrDelta ? AbsoluteDefaultValue : 0) + d;
			return true;
		};

		if(!DetermineCoordinateRelativity(pResult->GetString(0), pCallingPlayer->m_ViewPos.x, BaseX))
		{
			pSelf->SendChatTarget(pCallingPlayer->GetCid(), "Invalid X coordinate.");
			return;
		}
		if(!DetermineCoordinateRelativity(pResult->GetString(1), pCallingPlayer->m_ViewPos.y, BaseY))
		{
			pSelf->SendChatTarget(pCallingPlayer->GetCid(), "Invalid Y coordinate.");
			return;
		}

		Pos = {std::clamp(BaseX, (-OuterKillTileBoundaryDistance) + 1.f, (-OuterKillTileBoundaryDistance) + MapWidth - 1.f), std::clamp(BaseY, (-OuterKillTileBoundaryDistance) + 1.f, (-OuterKillTileBoundaryDistance) + MapHeight - 1.f)};
	}

	// Teleport tee
	Teleport(pCallingCharacter, Pos);
	pCallingCharacter->ResetJumps();
	pCallingCharacter->Unfreeze();
	pCallingCharacter->ResetVelocity();
	Teams.SaveLastTeleport(pCallingCharacter);
}

static void ConTeleCursor(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pPlayer->GetCharacter());
	if(!pChr)
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}

	// default to view pos when character is not available
	vec2 Pos = pPlayer->m_ViewPos;
	if(pResult->NumArguments() == 0 && !pPlayer->IsPaused() && pPlayer->GetCharacter() && pPlayer->GetCharacter()->IsAlive())
	{
		vec2 Target = vec2(pChr->Core()->m_Input.m_TargetX, pChr->Core()->m_Input.m_TargetY);
		Pos = pPlayer->m_CameraInfo.ConvertTargetToWorld(pPlayer->GetCharacter()->GetPos(), Target);
	}
	else if(pResult->NumArguments() > 0)
	{
		const CPlayer *pPlayerTo = pSelf->FindPlayerByName(pResult->GetString(0));
		if(!pPlayerTo)
		{
			pSelf->SendChatTarget(pPlayer->GetCid(), "No player with this name found.");
			return;
		}
		const CCharacter *pChrTo = pPlayerTo->GetCharacter();
		if(!pChrTo)
			return;
		Pos = pChrTo->m_Pos;
	}
	Teleport(pChr, Pos);
	pChr->ResetJumps();
	pChr->Unfreeze();
	pChr->ResetVelocity();
	Teams.SaveLastTeleport(pChr);
}

static void ConLastTele(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(pPlayer->GetCharacter());
	if(!pChr)
		return;

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}
	if(!Teams.LoadLastTeleport(pChr))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "You haven't previously teleported. Use /tp before using this command.");
		return;
	}
	pPlayer->Pause(CPlayer::PAUSE_NONE, true);
}

static CCharacter *GetPracticeCharacter(CGameContext *pGameServer, IConsole::IResult *pResult)
{
	if(!CheckClientId(pResult->m_ClientId))
		return nullptr;
	CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return nullptr;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr)
		return nullptr;

	CGameTeams &Teams = *RaceTeams(pGameServer);
	int Team = RaceTeams(pGameServer)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pGameServer->SendChatTarget(pPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return nullptr;
	}
	return pChr;
}

static void ConPracticeToTeleporter(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(GetPracticeCharacter(pSelf, pResult));
	if(pChr)
	{
		if(pSelf->Collision()->TeleOuts(pResult->GetInteger(0) - 1).empty())
		{
			pSelf->SendChatTarget(pChr->GetPlayer()->GetCid(), "There is no teleporter with that index on the map.");
			return;
		}

		ConToTeleporter(pResult, pUserData);
		pChr->ResetJumps();
		pChr->Unfreeze();
		pChr->ResetVelocity();
		RaceTeams(pSelf)->SaveLastTeleport(pChr);
	}
}

static void ConPracticeToCheckTeleporter(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	CCharacterDDRace *pChr = static_cast<CCharacterDDRace *>(GetPracticeCharacter(pSelf, pResult));
	if(pChr)
	{
		if(pSelf->Collision()->TeleCheckOuts(pResult->GetInteger(0) - 1).empty())
		{
			pSelf->SendChatTarget(pChr->GetPlayer()->GetCid(), "There is no checkpoint teleporter with that index on the map.");
			return;
		}

		ConToCheckTeleporter(pResult, pUserData);
		pChr->ResetJumps();
		pChr->Unfreeze();
		pChr->ResetVelocity();
		RaceTeams(pSelf)->SaveLastTeleport(pChr);
	}
}

static void ConPracticeUnSolo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr)
		return;

	if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "Command is not available on solo servers");
		return;
	}

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}
	pChr->SetSolo(false);
}

static void ConPracticeSolo(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(!CheckClientId(pResult->m_ClientId))
		return;
	CPlayer *pPlayer = pSelf->m_apPlayers[pResult->m_ClientId];
	if(!pPlayer)
		return;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr)
		return;

	if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "Command is not available on solo servers");
		return;
	}

	CGameTeams &Teams = *RaceTeams(pSelf);
	int Team = RaceTeams(pSelf)->m_Core.Team(pResult->m_ClientId);
	if(!Teams.IsPractice(Team))
	{
		pSelf->SendChatTarget(pPlayer->GetCid(), "You're not in a team with /practice turned on. Note that you can't earn a rank with practice enabled.");
		return;
	}
	pChr->SetSolo(true);
}

static void ConPracticeUnDeep(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	pChr->SetDeepFrozen(false);
	pChr->Unfreeze();
}

static void ConPracticeDeep(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	pChr->SetDeepFrozen(true);
}

static void ConPracticeUnLiveFreeze(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	pChr->SetLiveFrozen(false);
}

static void ConPracticeLiveFreeze(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	pChr->SetLiveFrozen(true);
}

static void ConPracticeShotgun(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConShotgun(pResult, pUserData);
}

static void ConPracticeGrenade(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConGrenade(pResult, pUserData);
}

static void ConPracticeLaser(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConLaser(pResult, pUserData);
}

static void ConPracticeJetpack(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConJetpack(pResult, pUserData);
}

static void ConPracticeEndlessJump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConEndlessJump(pResult, pUserData);
}

static void ConPracticeSetJumps(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConSetJumps(pResult, pUserData);
}

static void ConPracticeWeapons(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConWeapons(pResult, pUserData);
}

static void ConPracticeUnShotgun(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnShotgun(pResult, pUserData);
}

static void ConPracticeUnGrenade(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnGrenade(pResult, pUserData);
}

static void ConPracticeUnLaser(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnLaser(pResult, pUserData);
}

static void ConPracticeUnJetpack(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnJetpack(pResult, pUserData);
}

static void ConPracticeUnEndlessJump(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnEndlessJump(pResult, pUserData);
}

static void ConPracticeUnWeapons(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnWeapons(pResult, pUserData);
}

static void ConPracticeNinja(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConNinja(pResult, pUserData);
}

static void ConPracticeUnNinja(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnNinja(pResult, pUserData);
}

static void ConPracticeEndlessHook(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConEndlessHook(pResult, pUserData);
}

static void ConPracticeUnEndlessHook(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConUnEndlessHook(pResult, pUserData);
}

static void ConPracticeSetSwitch(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConSetSwitch(pResult, pUserData);
}

static void ConPracticeToggleInvincible(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConToggleInvincible(pResult, pUserData);
}

static void ConPracticeToggleCollision(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	pChr->SetCollisionDisabled(!pChr->Core()->m_CollisionDisabled);
}

static void ConPracticeToggleHookCollision(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	pChr->SetHookHitDisabled(!pChr->Core()->m_HookHitDisabled);
}

static void ConPracticeToggleHitOthers(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	auto *pChr = GetPracticeCharacter(pSelf, pResult);
	if(!pChr)
		return;

	if(pResult->NumArguments() == 0 || str_comp(pResult->GetString(0), "all") == 0)
	{
		bool IsEnabled = (pChr->HammerHitDisabled() && pChr->ShotgunHitDisabled() &&
				  pChr->GrenadeHitDisabled() && pChr->LaserHitDisabled());
		pChr->SetHammerHitDisabled(!IsEnabled);
		pChr->SetShotgunHitDisabled(!IsEnabled);
		pChr->SetGrenadeHitDisabled(!IsEnabled);
		pChr->SetLaserHitDisabled(!IsEnabled);
		return;
	}

	if(str_comp(pResult->GetString(0), "hammer") == 0)
		pChr->SetHammerHitDisabled(!pChr->HammerHitDisabled());
	else if(str_comp(pResult->GetString(0), "shotgun") == 0)
		pChr->SetShotgunHitDisabled(!pChr->ShotgunHitDisabled());
	else if(str_comp(pResult->GetString(0), "grenade") == 0)
		pChr->SetGrenadeHitDisabled(!pChr->GrenadeHitDisabled());
	else if(str_comp(pResult->GetString(0), "laser") == 0)
		pChr->SetLaserHitDisabled(!pChr->LaserHitDisabled());
}

static void ConPracticeAddWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConAddWeapon(pResult, pUserData);
}

static void ConPracticeRemoveWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	if(GetPracticeCharacter(pSelf, pResult))
		ConRemoveWeapon(pResult, pUserData);
}

static void ConchainPracticeByDefaultUpdate(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	const int OldValue = g_Config.m_SvPracticeByDefault;
	pfnCallback(pResult, pCallbackUserData);

	if(!pResult->NumArguments() || !g_Config.m_SvTestingCommands)
		return;

	CGameContext *pSelf = (CGameContext *)pUserData;
	const int Enable = pResult->GetInteger(0);
	if(Enable == OldValue)
		return;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "Practice is %s by default.", Enable ? "enabled" : "disabled");
	if(Enable)
		str_append(aBuf, " Join a team and /unpractice to turn it off for your team.");

	pSelf->SendChat(-1, TEAM_ALL, aBuf);

	CGameTeams &Teams = *RaceTeams(pSelf);
	for(int Team = 0; Team < NUM_DDRACE_TEAMS; Team++)
	{
		if(Team == TEAM_FLOCK || Teams.TeamSize(Team) == 0)
			Teams.SetPractice(Team, Enable);
	}
}

void CGameControllerDDRace::RegisterAdminCommands()
{
	static const CCommandRegistration s_aCommands[] = {
		{"totele", "i[number]", CFGFLAG_SERVER | CMDFLAG_TEST, ConToTeleporter, "Teleports you to teleporter i"},
		{"totelecp", "i[number]", CFGFLAG_SERVER | CMDFLAG_TEST, ConToCheckTeleporter, "Teleports you to checkpoint teleporter i"},
		{"tele", "?i[id] ?i[id]", CFGFLAG_SERVER | CMDFLAG_TEST, ConTeleport, "Teleports player i (or you) to player i (or you to where you look at)"},
		{"addweapon", "i[weapon-id]", CFGFLAG_SERVER | CMDFLAG_TEST, ConAddWeapon, "Gives weapon with id i to you (all = -1, hammer = 0, gun = 1, shotgun = 2, grenade = 3, laser = 4, ninja = 5)"},
		{"removeweapon", "i[weapon-id]", CFGFLAG_SERVER | CMDFLAG_TEST, ConRemoveWeapon, "removes weapon with id i from you (all = -1, hammer = 0, gun = 1, shotgun = 2, grenade = 3, laser = 4, ninja = 5)"},
		{"shotgun", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConShotgun, "Gives a shotgun to you"},
		{"grenade", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConGrenade, "Gives a grenade launcher to you"},
		{"laser", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConLaser, "Gives a laser to you"},
		{"rifle", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConLaser, "Gives a laser to you"},
		{"jetpack", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConJetpack, "Gives jetpack to you"},
		{"setjumps", "i[jumps]", CFGFLAG_SERVER | CMDFLAG_TEST, ConSetJumps, "Gives you as many jumps as you specify"},
		{"weapons", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConWeapons, "Gives all weapons to you"},
		{"unshotgun", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnShotgun, "Removes the shotgun from you"},
		{"ungrenade", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnGrenade, "Removes the grenade launcher from you"},
		{"unlaser", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnLaser, "Removes the laser from you"},
		{"unrifle", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnLaser, "Removes the laser from you"},
		{"unjetpack", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnJetpack, "Removes the jetpack from you"},
		{"unweapons", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnWeapons, "Removes all weapons from you"},
		{"ninja", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConNinja, "Makes you a ninja"},
		{"unninja", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnNinja, "Removes ninja from you"},
		{"super", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConSuper, "Makes you super"},
		{"unsuper", "", CFGFLAG_SERVER, ConUnSuper, "Removes super from you"},
		{"invincible", "?i['0'|'1']", CFGFLAG_SERVER | CMDFLAG_TEST, ConToggleInvincible, "Toggles invincible mode"},
		{"infinite_jump", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConEndlessJump, "Gives you infinite jump"},
		{"uninfinite_jump", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnEndlessJump, "Removes infinite jump from you"},
		{"endless_hook", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConEndlessHook, "Gives you endless hook"},
		{"unendless_hook", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnEndlessHook, "Removes endless hook from you"},
		{"setswitch", "i[switch] ?i['0'|'1'] ?i[seconds]", CFGFLAG_SERVER | CMDFLAG_TEST, ConSetSwitch, "Toggle or set the switch on or off for the specified time (or indefinitely by default)"},
		{"solo", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConSolo, "Puts you into solo part"},
		{"unsolo", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnSolo, "Puts you out of solo part"},
		{"freeze", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConFreeze, "Puts you into freeze"},
		{"unfreeze", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnfreeze, "Puts you out of freeze"},
		{"deep", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConDeep, "Puts you into deep freeze"},
		{"undeep", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnDeep, "Puts you out of deep freeze"},
		{"livefreeze", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConLiveFreeze, "Makes you live frozen"},
		{"unlivefreeze", "", CFGFLAG_SERVER | CMDFLAG_TEST, ConUnLiveFreeze, "Puts you out of live freeze"},
		{"left", "?i[tiles]", CFGFLAG_SERVER | CMDFLAG_TEST, ConGoLeft, "Makes you move 1 tile left"},
		{"right", "?i[tiles]", CFGFLAG_SERVER | CMDFLAG_TEST, ConGoRight, "Makes you move 1 tile right"},
		{"up", "?i[tiles]", CFGFLAG_SERVER | CMDFLAG_TEST, ConGoUp, "Makes you move 1 tile up"},
		{"down", "?i[tiles]", CFGFLAG_SERVER | CMDFLAG_TEST, ConGoDown, "Makes you move 1 tile down"},
		{"move", "i[x] i[y]", CFGFLAG_SERVER | CMDFLAG_TEST, ConMove, "Moves to the tile with x/y-number ii"},
		{"move_raw", "i[x] i[y]", CFGFLAG_SERVER | CMDFLAG_TEST, ConMoveRaw, "Moves to the point with x/y-coordinates ii"},
		{"force_pause", "v[id] i[seconds]", CFGFLAG_SERVER, ConForcePause, "Force i to pause for i seconds"},
		{"force_unpause", "v[id]", CFGFLAG_SERVER, ConForcePause, "Set force-pause timer of i to 0."},
		{"set_team_ddr", "v[id] i[team]", CFGFLAG_SERVER, ConSetDDRTeam, "Set ddrace team for a player"},
		{"uninvite", "v[id] i[team]", CFGFLAG_SERVER, ConUninvite, "Uninvite player from team"},
		{"save_dry", "", CFGFLAG_SERVER, ConDrySave, "Dump the current savestring"},
	};

	for(const CCommandRegistration &Command : s_aCommands)
	{
		dbg_assert(GameServer()->Console()->RegisterOwned(Command.m_pName, Command.m_pParams, Command.m_Flags, Command.m_pfnCallback, GameServer(), Command.m_pHelp, this), "duplicate mode command '%s'", Command.m_pName);
	}
}

void CGameControllerDDRace::RegisterPracticeCommands()
{
	static const CCommandRegistration s_aCommands[] = {
		{"practice", "?i['0'|'1']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConPractice, "Enable cheats for your current team's run, but you can't earn a rank"},
		{"unpractice", "", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConUnPractice, "Kills team and disables practice mode"},
		{"practicecmdlist", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConPracticeCmdList, "List all commands that are available in practice mode"},
		{"r", "", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConRescue, "Teleport yourself out of freeze if auto rescue mode is enabled, otherwise it will set position for rescuing if grounded and teleport you out of freeze if not (use sv_rescue 1 to enable this feature)"},
		{"rescue", "", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConRescue, "Teleport yourself out of freeze if auto rescue mode is enabled, otherwise it will set position for rescuing if grounded and teleport you out of freeze if not (use sv_rescue 1 to enable this feature)"},
		{"back", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConBack, "Teleport yourself to the last auto rescue position before you died (use sv_rescue 1 to enable this feature)"},
		{"rescuemode", "?r['auto'|'manual']", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConRescueMode, "Sets one of the two rescue modes (auto or manual). Prints current mode if no arguments provided"},
		{"tp", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConTeleTo, "Depending on the number of supplied arguments, teleport yourself to; (0.) where you are spectating or aiming; (1.) the specified player name"},
		{"teleport", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConTeleTo, "Depending on the number of supplied arguments, teleport yourself to; (0.) where you are spectating or aiming; (1.) the specified player name"},
		{"tpxy", "s[x] s[y]", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConTeleXY, "Teleport yourself to the specified coordinates. A tilde (~) can be used to denote your current position, e.g. '/tpxy ~1 ~' to teleport one tile to the right"},
		{"lasttp", "", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConLastTele, "Teleport yourself to the last location you teleported to"},
		{"tc", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConTeleCursor, "Teleport yourself to player or to where you are spectating/or looking if no player name is given"},
		{"telecursor", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER | CMDFLAG_PRACTICE, ConTeleCursor, "Teleport yourself to player or to where you are spectating/or looking if no player name is given"},
		{"totele", "i[number]", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeToTeleporter, "Teleports you to teleporter i"},
		{"totelecp", "i[number]", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeToCheckTeleporter, "Teleports you to checkpoint teleporter i"},
		{"unsolo", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnSolo, "Puts you out of solo part"},
		{"solo", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeSolo, "Puts you into solo part"},
		{"undeep", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnDeep, "Puts you out of deep freeze"},
		{"deep", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeDeep, "Puts you into deep freeze"},
		{"unlivefreeze", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnLiveFreeze, "Puts you out of live freeze"},
		{"livefreeze", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeLiveFreeze, "Makes you live frozen"},
		{"addweapon", "i[weapon-id]", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeAddWeapon, "Gives weapon with id i to you (all = -1, hammer = 0, gun = 1, shotgun = 2, grenade = 3, laser = 4, ninja = 5)"},
		{"removeweapon", "i[weapon-id]", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeRemoveWeapon, "removes weapon with id i from you (all = -1, hammer = 0, gun = 1, shotgun = 2, grenade = 3, laser = 4, ninja = 5)"},
		{"shotgun", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeShotgun, "Gives a shotgun to you"},
		{"grenade", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeGrenade, "Gives a grenade launcher to you"},
		{"laser", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeLaser, "Gives a laser to you"},
		{"rifle", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeLaser, "Gives a laser to you"},
		{"jetpack", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeJetpack, "Gives jetpack to you"},
		{"setjumps", "i[jumps]", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeSetJumps, "Gives you as many jumps as you specify"},
		{"weapons", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeWeapons, "Gives all weapons to you"},
		{"unshotgun", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnShotgun, "Removes the shotgun from you"},
		{"ungrenade", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnGrenade, "Removes the grenade launcher from you"},
		{"unlaser", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnLaser, "Removes the laser from you"},
		{"unrifle", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnLaser, "Removes the laser from you"},
		{"unjetpack", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnJetpack, "Removes the jetpack from you"},
		{"unweapons", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnWeapons, "Removes all weapons from you"},
		{"ninja", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeNinja, "Makes you a ninja"},
		{"unninja", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnNinja, "Removes ninja from you"},
		{"infjump", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeEndlessJump, "Gives you infinite jump"},
		{"uninfjump", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnEndlessJump, "Removes infinite jump from you"},
		{"endless", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeEndlessHook, "Gives you endless hook"},
		{"unendless", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeUnEndlessHook, "Removes endless hook from you"},
		{"setswitch", "i[switch] ?i['0'|'1'] ?i[seconds]", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeSetSwitch, "Toggle or set the switch on or off for the specified time (or indefinitely by default)"},
		{"invincible", "?i['0'|'1']", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeToggleInvincible, "Toggles invincible mode"},
		{"collision", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeToggleCollision, "Toggles collision"},
		{"hookcollision", "", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeToggleHookCollision, "Toggles hook collision"},
		{"hitothers", "?s['all'|'hammer'|'shotgun'|'grenade'|'laser']", CFGFLAG_CHAT | CMDFLAG_PRACTICE, ConPracticeToggleHitOthers, "Toggles hit others"},
	};

	for(const CCommandRegistration &Command : s_aCommands)
	{
		dbg_assert(GameServer()->Console()->RegisterOwned(Command.m_pName, Command.m_pParams, Command.m_Flags, Command.m_pfnCallback, GameServer(), Command.m_pHelp, this), "duplicate mode command '%s'", Command.m_pName);
	}

	dbg_assert(GameServer()->Console()->ChainOwned("sv_practice_by_default", ConchainPracticeByDefaultUpdate, GameServer(), this), "failed to chain mode config 'sv_practice_by_default'");
}

void CGameContext::ConReloadCensorlist(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->ReadCensorList();
}

void CGameContext::ConDumpAntibot(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->Antibot()->ConsoleCommand("dump");
}

void CGameContext::ConAntibot(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	pSelf->Antibot()->ConsoleCommand(pResult->GetString(0));
}

void CGameContext::ConDumpLog(IConsole::IResult *pResult, void *pUserData)
{
	CGameContext *pSelf = (CGameContext *)pUserData;
	int LimitSecs = MAX_LOG_SECONDS;
	if(pResult->NumArguments() > 0)
		LimitSecs = pResult->GetInteger(0);

	if(LimitSecs < 0)
		return;

	int Iterator = pSelf->m_LatestLog;
	for(int i = 0; i < MAX_LOGS; i++)
	{
		CLog *pEntry = &pSelf->m_aLogs[Iterator];
		Iterator = (Iterator + 1) % MAX_LOGS;

		if(!pEntry->m_Timestamp)
			continue;

		int Seconds = (time_get() - pEntry->m_Timestamp) / time_freq();
		if(Seconds > LimitSecs)
			continue;

		char aBuf[sizeof(pEntry->m_aDescription) + 128];
		if(pEntry->m_FromServer)
			str_format(aBuf, sizeof(aBuf), "%s, %d seconds ago", pEntry->m_aDescription, Seconds);
		else
			str_format(aBuf, sizeof(aBuf), "%s, %d seconds ago < addr=<{%s}> name='%s' client=%d",
				pEntry->m_aDescription, Seconds, pEntry->m_aClientAddrStr, pEntry->m_aClientName, pEntry->m_ClientVersion);
		pSelf->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "log", aBuf);
	}
}

void CGameContext::LogEvent(const char *Description, int ClientId)
{
	CLog *pNewEntry = &m_aLogs[m_LatestLog];
	m_LatestLog = (m_LatestLog + 1) % MAX_LOGS;

	pNewEntry->m_Timestamp = time_get();
	str_copy(pNewEntry->m_aDescription, Description);
	pNewEntry->m_FromServer = ClientId < 0;
	if(!pNewEntry->m_FromServer)
	{
		pNewEntry->m_ClientVersion = Server()->GetClientVersion(ClientId);
		str_copy(pNewEntry->m_aClientAddrStr, Server()->ClientAddrString(ClientId, false));
		str_copy(pNewEntry->m_aClientName, Server()->ClientName(ClientId));
	}
}
