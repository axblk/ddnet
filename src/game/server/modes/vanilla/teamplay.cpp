#include "teamplay.h"

#include <base/str.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/mode/game_services.h>
#include <game/server/player.h>

#include <algorithm>

namespace
{
	constexpr int MIN_UNBALANCED_TEAM_DIFFERENCE = 2;
}

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

std::array<int, NUM_TEAMS> CGameControllerVanillaTeamplay::TeamSizes(int ExceptClientId) const
{
	std::array<int, NUM_TEAMS> aTeamSizes{};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		if(ClientId == ExceptClientId || !pPlayer)
			continue;
		const int Team = pPlayer->GetTeam();
		if(Team >= TEAM_RED && Team <= TEAM_BLUE)
			aTeamSizes[Team]++;
	}
	return aTeamSizes;
}

void CGameControllerVanillaTeamplay::Tick()
{
	CGameControllerVanillaPvP::Tick();
	UpdateTeamBalance(Server()->Tick());
}

void CGameControllerVanillaTeamplay::UpdateTeamBalance(int Tick)
{
	if(g_Config.m_SvTeambalanceTime == 0)
	{
		m_UnbalancedSinceTick = -1;
		return;
	}

	const std::array<int, NUM_TEAMS> aTeamSizes = TeamSizes();
	if(absolute(aTeamSizes[TEAM_RED] - aTeamSizes[TEAM_BLUE]) < MIN_UNBALANCED_TEAM_DIFFERENCE)
	{
		m_UnbalancedSinceTick = -1;
		return;
	}

	if(m_UnbalancedSinceTick < 0)
	{
		m_UnbalancedSinceTick = Tick;
		return;
	}
	if(Tick <= m_UnbalancedSinceTick + g_Config.m_SvTeambalanceTime * Server()->TickSpeed() * 60)
		return;

	std::array<float, MAX_CLIENTS> aPlayerScores{};
	std::array<float, NUM_TEAMS> aTeamScores{};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CPlayer *pPlayer = Services().Player(ClientId);
		if(!pPlayer || pPlayer->GetTeam() < TEAM_RED || pPlayer->GetTeam() > TEAM_BLUE)
			continue;
		const int ScoreStartTick = std::max(pPlayer->m_JoinTick, m_RoundStartTick);
		const int ScoreTicks = std::max(1, Tick - ScoreStartTick);
		aPlayerScores[ClientId] = VanillaPlayer(ClientId)->m_Score * Server()->TickSpeed() * 60.0f / ScoreTicks;
		aTeamScores[pPlayer->GetTeam()] += aPlayerScores[ClientId];
	}

	const int BiggerTeam = aTeamSizes[TEAM_RED] > aTeamSizes[TEAM_BLUE] ? TEAM_RED : TEAM_BLUE;
	const int SmallerTeam = BiggerTeam ^ 1;
	int NumBalance = absolute(aTeamSizes[TEAM_RED] - aTeamSizes[TEAM_BLUE]) / MIN_UNBALANCED_TEAM_DIFFERENCE;
	while(NumBalance-- > 0)
	{
		CPlayer *pBestPlayer = nullptr;
		float BestScoreDifference = 0.0f;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			CPlayer *pPlayer = Services().Player(ClientId);
			if(!pPlayer || pPlayer->GetTeam() != BiggerTeam)
				continue;
			const float ScoreDifference = absolute((aTeamScores[SmallerTeam] + aPlayerScores[ClientId]) - (aTeamScores[BiggerTeam] - aPlayerScores[ClientId]));
			if(!pBestPlayer || ScoreDifference < BestScoreDifference)
			{
				pBestPlayer = pPlayer;
				BestScoreDifference = ScoreDifference;
			}
		}
		if(!pBestPlayer)
			break;

		const int ClientId = pBestPlayer->GetCid();
		const int LastActionTick = pBestPlayer->m_LastActionTick;
		DoTeamChange(pBestPlayer, SmallerTeam, true);
		pBestPlayer->m_LastActionTick = LastActionTick;
		pBestPlayer->Respawn();
		aTeamScores[BiggerTeam] -= aPlayerScores[ClientId];
		aTeamScores[SmallerTeam] += aPlayerScores[ClientId];
		Services().SendGameMessage7(protocol7::GAMEMSG_TEAM_BALANCE_VICTIM, {SmallerTeam}, ClientId);
	}

	m_UnbalancedSinceTick = -1;
	Services().SendGameMessage7(protocol7::GAMEMSG_TEAM_BALANCE);
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
	const std::array<int, NUM_TEAMS> aTeamSizes = TeamSizes(NotThisId);
	const int Team = aTeamSizes[TEAM_RED] > aTeamSizes[TEAM_BLUE] ? TEAM_BLUE : TEAM_RED;
	return CanJoinTeam(Team, NotThisId, nullptr, 0) ? Team : TEAM_SPECTATORS;
}

bool CGameControllerVanillaTeamplay::CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize)
{
	if(!IsValidTeam(Team) || !IGameController::CanJoinTeam(Team, NotThisId, pErrorReason, ErrorReasonSize))
		return false;
	if(Team == TEAM_SPECTATORS || g_Config.m_SvTeambalanceTime == 0)
		return true;

	std::array<int, NUM_TEAMS> aTeamSizes = TeamSizes(NotThisId);
	aTeamSizes[Team]++;
	if(aTeamSizes[Team] - aTeamSizes[Team ^ 1] < MIN_UNBALANCED_TEAM_DIFFERENCE)
		return true;
	if(pErrorReason)
		str_copy(pErrorReason, "Teams must remain balanced", ErrorReasonSize);
	return false;
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
