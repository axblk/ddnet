#include "dm.h"

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <limits>

CGameControllerVanillaDM::CGameControllerVanillaDM(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	CGameControllerVanillaPvP(pGameServer, GameModeInfo)
{
}

int CGameControllerVanillaDM::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	const int VictimId = pVictim->GetPlayer()->GetCid();
	m_aEarliestRespawnTicks[VictimId] = Server()->Tick() + Server()->TickSpeed() / 2;
	ApplyDeathScore(m_aScores, VictimId, pKiller ? pKiller->GetCid() : -1, Weapon);
	return 0;
}

void CGameControllerVanillaDM::Tick()
{
	IGameController::Tick();
	if(m_GameOverTick != -1 || m_Warmup)
		return;

	int TopScore = std::numeric_limits<int>::min();
	int NumTopScores = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;

		if(m_aScores[ClientId] > TopScore)
		{
			TopScore = m_aScores[ClientId];
			NumTopScores = 1;
		}
		else if(m_aScores[ClientId] == TopScore)
		{
			NumTopScores++;
		}
	}

	const bool ScoreLimitReached = ScoreLimit() > 0 && TopScore >= ScoreLimit();
	const bool TimeLimitReached = TimeLimit() > 0 && Server()->Tick() - m_RoundStartTick >= TimeLimit() * Server()->TickSpeed() * 60;
	const EMatchResult MatchResult = EvaluateMatch(NumTopScores, ScoreLimitReached || TimeLimitReached, m_SuddenDeath != 0);
	if(MatchResult == EMatchResult::END_ROUND)
		EndRound();
	else if(MatchResult == EMatchResult::SUDDEN_DEATH)
		m_SuddenDeath = 1;
}

bool CGameControllerVanillaDM::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || Server()->Tick() < m_aEarliestRespawnTicks[ClientId])
		return false;
	return IGameController::CanSpawn(Team, pOutPos, ClientId);
}
