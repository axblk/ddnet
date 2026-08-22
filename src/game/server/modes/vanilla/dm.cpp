#include "dm.h"

#include <engine/server.h>

#include <game/server/entities/character.h>
#include <game/server/mode/game_services.h>
#include <game/server/player.h>

#include <limits>

CGameControllerVanillaDM::CGameControllerVanillaDM(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	CGameControllerVanillaPvP(Services, GameModeInfo)
{
}

void CGameControllerVanillaDM::OnCharacterDeath(const CGameCharacterDeathContext &Context)
{
	CCharacter *pVictim = Context.m_pVictim;
	CPlayer *pKiller = Context.m_pKiller;
	const int VictimId = pVictim->GetPlayer()->GetCid();
	SetRespawnDelay(VictimId, Context.m_Weapon);
	const int KillerId = pKiller ? pKiller->GetCid() : -1;
	if(CPlayerVanilla *pKillerPlayer = VanillaPlayer(KillerId))
		pKillerPlayer->m_Score += DeathScoreDelta(VictimId, KillerId, Context.m_Weapon);
	FinalizeCharacterDeath(Context);
}

void CGameControllerVanillaDM::Tick()
{
	IGameController::Tick();
	TickMatch();
}

void CGameControllerVanillaDM::TickMatch()
{
	if(!Match().IsRunning())
		return;

	int TopScore = std::numeric_limits<int>::min();
	int NumTopScores = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;

		const int Score = VanillaPlayer(ClientId)->m_Score;
		if(Score > TopScore)
		{
			TopScore = Score;
			NumTopScores = 1;
		}
		else if(Score == TopScore)
		{
			NumTopScores++;
		}
	}

	const bool ScoreLimitReached = ScoreLimit() > 0 && TopScore >= ScoreLimit();
	const bool TimeLimitReached = TimeLimit() > 0 && Server()->Tick() - Match().RoundStartTick() >= TimeLimit() * Server()->TickSpeed() * 60;
	const EMatchResult MatchResult = EvaluateMatch(NumTopScores, ScoreLimitReached || TimeLimitReached, Match().IsSuddenDeath());
	if(MatchResult == EMatchResult::END_ROUND)
		EndRound();
	else if(MatchResult == EMatchResult::SUDDEN_DEATH)
		Match().BeginSuddenDeath();
}

bool CGameControllerVanillaDM::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	CPlayerVanilla *pPlayer = VanillaPlayer(ClientId);
	if(!pPlayer || Server()->Tick() < pPlayer->m_EarliestRespawnTick)
		return false;
	return IGameController::CanSpawn(Team, pOutPos, ClientId);
}
