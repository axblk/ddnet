#include "tdm.h"

#include <engine/server.h>
#include <engine/shared/config.h>

#include <generated/protocol7.h>

#include <game/server/entities/character.h>
#include <game/server/player.h>

#include <algorithm>

static const CGameModeRegistration gs_TDM({"tdm", "TDM", protocol7::GAMEFLAG_TEAMS, false}, NewGameController<CGameControllerVanillaTDM>);

CGameControllerVanillaTDM::CGameControllerVanillaTDM(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	CGameControllerVanillaTeamplay(Services, GameModeInfo)
{
}

void CGameControllerVanillaTDM::OnCharacterDeath(const CGameCharacterDeathContext &Context)
{
	CCharacter *pVictim = Context.m_pVictim;
	CPlayer *pKiller = Context.m_pKiller;
	const int VictimId = pVictim->GetPlayer()->GetCid();
	const int VictimTeam = pVictim->GetPlayer()->GetTeam();
	const int KillerId = pKiller ? pKiller->GetCid() : -1;
	const int KillerTeam = pKiller ? pKiller->GetTeam() : TEAM_SPECTATORS;
	const bool SelfKill = KillerId == VictimId;
	const bool TeamKill = !SelfKill && KillerTeam == VictimTeam;

	SetRespawnDelay(VictimId, Context.m_Weapon);
	CPlayerVanilla *pVictimPlayer = VanillaPlayer(VictimId);
	pVictimPlayer->m_EarliestRespawnTick = std::max(pVictimPlayer->m_EarliestRespawnTick, Server()->Tick() + Server()->TickSpeed() * g_Config.m_SvRespawnDelayTDM);
	if(CPlayerVanilla *pKillerPlayer = VanillaPlayer(KillerId))
		pKillerPlayer->m_Score += DeathScoreDelta(VictimId, KillerId, Context.m_Weapon, TeamKill);
	if(KillerTeam >= TEAM_RED && KillerTeam <= TEAM_BLUE && VictimTeam >= TEAM_RED && VictimTeam <= TEAM_BLUE && Context.m_Weapon != WEAPON_GAME)
		m_aTeamScores[KillerTeam] += SelfKill || TeamKill ? -1 : 1;
	FinalizeCharacterDeath(Context);
}

void CGameControllerVanillaTDM::Tick()
{
	CGameControllerVanillaTeamplay::Tick();
	if(!Match().IsRunning())
		return;

	CheckMatchEnd(std::max(m_aTeamScores[TEAM_RED], m_aTeamScores[TEAM_BLUE]), m_aTeamScores[TEAM_RED] == m_aTeamScores[TEAM_BLUE]);
}

void CGameControllerVanillaTDM::SnapMode(int SnappingClient)
{
	SnapTeamData(SnappingClient, FLAG_MISSING, FLAG_MISSING);
}
