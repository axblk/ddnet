#include "teamplay.h"

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/mode/game_services.h>
#include <game/server/player.h>

CGameControllerVanillaTeamplay::CGameControllerVanillaTeamplay(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	CGameControllerVanillaPvP(Services, GameModeInfo)
{
}

bool CGameControllerVanillaTeamplay::OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam)
{
	const int VictimId = pVictim->GetPlayer()->GetCid();
	if(CPlayer *pAttacker = Services().Player(From))
		AttackerTeam = pAttacker->GetTeam();
	if(!g_Config.m_SvTeamdamage && From != VictimId && AttackerTeam >= TEAM_RED && AttackerTeam <= TEAM_BLUE && AttackerTeam == pVictim->GetPlayer()->GetTeam())
	{
		pVictim->AddVelocity(Force);
		return true;
	}
	return CGameControllerVanillaPvP::OnCharacterTakeDamage(pVictim, Force, Damage, From, Weapon, CanDamage, AttackerTeam);
}

void CGameControllerVanillaTeamplay::StartRound()
{
	m_aTeamScores.fill(0);
	CGameControllerVanillaPvP::StartRound();
}

bool CGameControllerVanillaTeamplay::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	CPlayerVanilla *pPlayer = VanillaPlayer(ClientId);
	if(!pPlayer || Server()->Tick() < pPlayer->m_EarliestRespawnTick || !IsValidTeam(Team) || Team == TEAM_SPECTATORS)
		return false;

	CSpawnEval Eval;
	Eval.m_FriendlyTeam = Team;
	EvaluateSpawnType(&Eval, Team == TEAM_RED ? SPAWNTYPE_RED : SPAWNTYPE_BLUE, ClientId);
	if(!Eval.m_Got)
	{
		EvaluateSpawnType(&Eval, SPAWNTYPE_DEFAULT, ClientId);
		if(!Eval.m_Got)
			EvaluateSpawnType(&Eval, Team == TEAM_RED ? SPAWNTYPE_BLUE : SPAWNTYPE_RED, ClientId);
	}
	*pOutPos = Eval.m_Pos;
	return Eval.m_Got;
}

bool CGameControllerVanillaTeamplay::IsValidTeam(int Team)
{
	return Team == TEAM_SPECTATORS || Team == TEAM_RED || Team == TEAM_BLUE;
}

const char *CGameControllerVanillaTeamplay::GetTeamName(int Team)
{
	switch(Team)
	{
	case TEAM_SPECTATORS: return "spectators";
	case TEAM_RED: return "red team";
	case TEAM_BLUE: return "blue team";
	default: dbg_assert_failed("Invalid Team: %d", Team);
	}
}

int CGameControllerVanillaTeamplay::GetAutoTeam(int NotThisId)
{
	int aTeamSize[NUM_TEAMS] = {0, 0};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CPlayer *pPlayer = Services().Player(ClientId);
		if(ClientId == NotThisId || !pPlayer)
			continue;
		const int Team = pPlayer->GetTeam();
		if(Team >= TEAM_RED && Team <= TEAM_BLUE)
			aTeamSize[Team]++;
	}
	const int Team = aTeamSize[TEAM_RED] > aTeamSize[TEAM_BLUE] ? TEAM_BLUE : TEAM_RED;
	return CanJoinTeam(Team, NotThisId, nullptr, 0) ? Team : TEAM_SPECTATORS;
}

bool CGameControllerVanillaTeamplay::CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize)
{
	return IsValidTeam(Team) && IGameController::CanJoinTeam(Team, NotThisId, pErrorReason, ErrorReasonSize);
}

int CGameControllerVanillaTeamplay::TeamScore(int Team) const
{
	return Team >= TEAM_RED && Team <= TEAM_BLUE ? m_aTeamScores[Team] : 0;
}

void CGameControllerVanillaTeamplay::SnapTeamData(int SnappingClient, int FlagCarrierRed, int FlagCarrierBlue, int FlagDropTickRed, int FlagDropTickBlue, bool SnapFlags)
{
	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_GameDataTeam GameDataTeam = {};
		GameDataTeam.m_TeamscoreRed = m_aTeamScores[TEAM_RED];
		GameDataTeam.m_TeamscoreBlue = m_aTeamScores[TEAM_BLUE];
		Server()->SnapNewItem(0, GameDataTeam);

		if(SnapFlags)
		{
			protocol7::CNetObj_GameDataFlag GameDataFlag = {};
			GameDataFlag.m_FlagCarrierRed = FlagCarrierRed;
			GameDataFlag.m_FlagCarrierBlue = FlagCarrierBlue;
			GameDataFlag.m_FlagDropTickRed = FlagDropTickRed;
			GameDataFlag.m_FlagDropTickBlue = FlagDropTickBlue;
			Server()->SnapNewItem(0, GameDataFlag);
		}
	}
	else
	{
		CNetObj_GameData GameData = {};
		GameData.m_TeamscoreRed = m_aTeamScores[TEAM_RED];
		GameData.m_TeamscoreBlue = m_aTeamScores[TEAM_BLUE];
		GameData.m_FlagCarrierRed = FlagCarrierRed;
		GameData.m_FlagCarrierBlue = FlagCarrierBlue;
		Server()->SnapNewItem(0, GameData);
	}
}
