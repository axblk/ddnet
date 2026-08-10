#include "tdm.h"

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/player.h>

#include <algorithm>

CGameControllerVanillaTDM::CGameControllerVanillaTDM(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	CGameControllerVanillaTeamplay(Services, GameModeInfo)
{
}

void CGameControllerVanillaTDM::ApplyTeamDeathScore(std::array<int, NUM_TEAMS> &aTeamScores, int VictimTeam, int KillerTeam, int Weapon, bool SelfKill)
{
	if(KillerTeam < TEAM_RED || KillerTeam > TEAM_BLUE || VictimTeam < TEAM_RED || VictimTeam > TEAM_BLUE || Weapon == WEAPON_GAME)
		return;
	aTeamScores[KillerTeam] += SelfKill || KillerTeam == VictimTeam ? -1 : 1;
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

	VanillaPlayer(VictimId)->m_EarliestRespawnTick = Server()->Tick() + Server()->TickSpeed() * g_Config.m_SvRespawnDelayTDM;
	if(CPlayerVanilla *pKillerPlayer = VanillaPlayer(KillerId))
		pKillerPlayer->m_Score += DeathScoreDelta(VictimId, KillerId, Context.m_Weapon, TeamKill);
	ApplyTeamDeathScore(m_aTeamScores, VictimTeam, KillerTeam, Context.m_Weapon, SelfKill);
	FinalizeCharacterDeath(Context);
}

void CGameControllerVanillaTDM::Tick()
{
	IGameController::Tick();
	if(m_GameOverTick != -1 || m_Warmup)
		return;

	const bool ScoresTied = m_aTeamScores[TEAM_RED] == m_aTeamScores[TEAM_BLUE];
	const int TopScore = std::max(m_aTeamScores[TEAM_RED], m_aTeamScores[TEAM_BLUE]);
	const bool ScoreLimitReached = ScoreLimit() > 0 && TopScore >= ScoreLimit();
	const bool TimeLimitReached = TimeLimit() > 0 && Server()->Tick() - m_RoundStartTick >= TimeLimit() * Server()->TickSpeed() * 60;
	const EMatchResult MatchResult = EvaluateMatch(ScoresTied ? 2 : 1, ScoreLimitReached || TimeLimitReached, m_SuddenDeath != 0);
	if(MatchResult == EMatchResult::END_ROUND)
		EndRound();
	else if(MatchResult == EMatchResult::SUDDEN_DEATH)
		m_SuddenDeath = 1;
}

void CGameControllerVanillaTDM::SnapMode(int SnappingClient)
{
	SnapTeamData(SnappingClient, FLAG_MISSING, FLAG_MISSING);
}
