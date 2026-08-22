#include "dm.h"

#include <engine/server.h>

#include <game/server/entities/character.h>
#include <game/server/player.h>

static const CGameModeRegistration gs_DM({"dm", "DM", 0, false}, NewGameController<CGameControllerVanillaDM>);

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

	// spectators keep their score and count too, like in stock vanilla
	int TopScore = 0;
	int NumTopScores = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CPlayerVanilla *pPlayer = VanillaPlayer(ClientId);
		if(!pPlayer)
			continue;
		if(pPlayer->m_Score > TopScore)
		{
			TopScore = pPlayer->m_Score;
			NumTopScores = 1;
		}
		else if(pPlayer->m_Score == TopScore)
		{
			NumTopScores++;
		}
	}
	CheckMatchEnd(TopScore, NumTopScores != 1);
}

bool CGameControllerVanillaDM::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	CPlayerVanilla *pPlayer = VanillaPlayer(ClientId);
	if(!pPlayer || Server()->Tick() < pPlayer->m_EarliestRespawnTick)
		return false;
	return IGameController::CanSpawn(Team, pOutPos, ClientId);
}
