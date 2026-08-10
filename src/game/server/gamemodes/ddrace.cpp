#include "ddrace.h"

#include "ddrace_map_entities.h"

#include <engine/shared/config.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <algorithm>

CGameControllerDDRace::CGameControllerDDRace(class CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	IGameController(pGameServer, GameModeInfo)
{
}

void CGameControllerDDRace::RegisterCommands()
{
	GameServer()->RegisterDDRaceAdminCommands(this);
	GameServer()->RegisterDDRacePlayerCommands(this);
	GameServer()->RegisterDDRacePracticeCommands(this);
	GameServer()->RegisterDDRaceScoreCommands(this);
	GameServer()->RegisterDDRaceTeamCommands(this);
}

void CGameControllerDDRace::OnCharacterSpawn(CCharacter *pCharacter)
{
	IGameController::OnCharacterSpawn(pCharacter);
	pCharacter->DDRaceInit();
}

void CGameControllerDDRace::TickCharacterPreCore(CCharacter *pCharacter)
{
	pCharacter->DDRaceTick();
}

void CGameControllerDDRace::TickCharacterPostCore(CCharacter *pCharacter)
{
	pCharacter->DDRacePostCoreTick();
}

int CGameControllerDDRace::PlayerAutoRespawnTick(const CPlayer *pPlayer) const
{
	return std::max(pPlayer->m_DieTick, pPlayer->m_PreviousDieTick + Server()->TickSpeed() * 3) + 2;
}

bool CGameControllerDDRace::SaveStateForHotReload()
{
	GameServer()->SaveDDRaceStateForHotReload();
	return true;
}

void CGameControllerDDRace::RestoreCharacterAfterHotReload(CCharacter *pCharacter)
{
	GameServer()->RestoreDDRaceCharacterAfterHotReload(pCharacter);
}

bool CGameControllerDDRace::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	return IGameController::OnEntity(Index, x, y, Layer, Flags, Initial, Number) ||
	       CreateDDRaceMapEntity(GameServer(), Index, x, y, Layer, Flags, Number);
}

void CGameControllerDDRace::OnPlayerSetTeam(int ClientId, int Team)
{
	// Keep the cheap common rejection checks ahead of the DDRace-specific policy.
	// The neutral implementation repeats them before applying the operation.
	if(IsGamePaused())
		return;

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->GetTeam() == Team)
		return;
	if(g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > Server()->Tick())
		return;

	CCharacter *pCharacter = pPlayer->GetCharacter();
	if(pCharacter)
	{
		const int CurrentTime = (Server()->Tick() - pCharacter->m_StartTime) / Server()->TickSpeed();
		if(g_Config.m_SvKillProtection != 0 && CurrentTime >= 60 * g_Config.m_SvKillProtection && pCharacter->m_DDRaceState == ERaceState::STARTED)
		{
			GameServer()->SendChatTarget(ClientId, "Kill Protection enabled. If you really want to join the spectators, first type /kill");
			return;
		}
	}

	IGameController::OnPlayerSetTeam(ClientId, Team);
}
