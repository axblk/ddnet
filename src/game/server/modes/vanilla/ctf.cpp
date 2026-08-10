#include "ctf.h"

#include "flag.h"

#include <base/log.h>

#include <engine/shared/config.h>

#include <game/collision.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <algorithm>

CGameControllerVanillaCTF::CGameControllerVanillaCTF(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo) :
	CGameControllerVanillaTeamplay(pGameServer, GameModeInfo)
{
}

bool CGameControllerVanillaCTF::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	const bool BaseHandled = CGameControllerVanillaTeamplay::OnEntity(Index, x, y, Layer, Flags, Initial, Number);
	const int Team = Index == ENTITY_FLAGSTAND_RED ? TEAM_RED : Index == ENTITY_FLAGSTAND_BLUE ? TEAM_BLUE :
												     TEAM_SPECTATORS;
	if(Team == TEAM_SPECTATORS || m_apFlags[Team])
		return BaseHandled;

	const vec2 StandPosition(x * 32.0f + 16.0f, y * 32.0f + 16.0f);
	m_apFlags[Team] = new CFlag(&GameServer()->m_World, Team, StandPosition);
	log_info("game", "flag_stand team=%d x=%.1f y=%.1f", Team, StandPosition.x, StandPosition.y);
	return true;
}

int CGameControllerVanillaCTF::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	const int VictimId = pVictim->GetPlayer()->GetCid();
	const int KillerId = pKiller ? pKiller->GetCid() : -1;
	const bool TeamKill = pKiller && pKiller != pVictim->GetPlayer() && pKiller->GetTeam() == pVictim->GetPlayer()->GetTeam();
	m_aEarliestRespawnTicks[VictimId] = Server()->Tick() + Server()->TickSpeed() / 2;
	ApplyDeathScore(m_aScores, VictimId, KillerId, Weapon, TeamKill);

	int Result = 0;
	for(CFlag *pFlag : m_apFlags)
	{
		if(!pFlag)
			continue;
		if(pKiller && pKiller->GetCharacter() == pFlag->Carrier())
			Result |= 2;
		if(pFlag->Carrier() != pVictim)
			continue;

		pFlag->Drop();
		log_info("game", "flag_drop player='%d:%s' team=%d", VictimId, Server()->ClientName(VictimId), pVictim->GetPlayer()->GetTeam());
		GameServer()->CreateSoundGlobal(SOUND_CTF_DROP, -1, CGameContext::FLAG_SIX);
		GameServer()->SendGameMessage7(protocol7::GAMEMSG_CTF_DROP);
		if(pKiller && pKiller->GetTeam() != pVictim->GetPlayer()->GetTeam())
			m_aScores[pKiller->GetCid()]++;
		Result |= 1;
	}
	return Result;
}

void CGameControllerVanillaCTF::Tick()
{
	CGameControllerVanillaTeamplay::Tick();
	if(GameServer()->m_World.m_ResetRequested || GameServer()->m_World.m_Paused || m_GameOverTick != -1 || m_Warmup)
		return;

	ProcessFlags();

	const bool ScoresTied = m_aTeamScores[TEAM_RED] / 100 == m_aTeamScores[TEAM_BLUE] / 100;
	const int TopScore = std::max(m_aTeamScores[TEAM_RED], m_aTeamScores[TEAM_BLUE]);
	const bool ScoreLimitReached = ScoreLimit() > 0 && TopScore >= ScoreLimit();
	const bool TimeLimitReached = TimeLimit() > 0 && Server()->Tick() - m_RoundStartTick >= TimeLimit() * Server()->TickSpeed() * 60;
	const EMatchResult MatchResult = EvaluateMatch(ScoresTied ? 2 : 1, ScoreLimitReached || TimeLimitReached, m_SuddenDeath != 0);
	if(MatchResult == EMatchResult::END_ROUND)
		EndRound();
	else if(MatchResult == EMatchResult::SUDDEN_DEATH)
		m_SuddenDeath = 1;
}

void CGameControllerVanillaCTF::ProcessFlags()
{
	for(CFlag *pFlag : m_apFlags)
	{
		if(!pFlag)
			continue;
		if(pFlag->TakeAutomaticReturn())
		{
			log_info("game", "flag_return");
			GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN, -1, CGameContext::FLAG_SIX);
			GameServer()->SendGameMessage7(protocol7::GAMEMSG_CTF_RETURN);
			continue;
		}

		if(pFlag->Carrier())
		{
			CFlag *pOwnFlag = m_apFlags[pFlag->Carrier()->GetPlayer()->GetTeam()];
			if(pOwnFlag && pOwnFlag->IsAtStand() && distance(pFlag->Carrier()->m_Pos, pOwnFlag->StandPosition()) < CFlag::PHYSICAL_SIZE + pFlag->Carrier()->GetProximityRadius())
				FlagCapture(pFlag);
			continue;
		}

		CEntity *apEntities[MAX_CLIENTS];
		const int Num = GameServer()->m_World.FindEntities(pFlag->m_Pos, CFlag::PHYSICAL_SIZE, apEntities, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
		for(int i = 0; i < Num; i++)
		{
			auto *pCharacter = static_cast<CCharacter *>(apEntities[i]);
			if(!pCharacter->IsAlive() || pCharacter->GetPlayer()->GetTeam() == TEAM_SPECTATORS || GameServer()->Collision()->IntersectLine(pFlag->m_Pos, pCharacter->m_Pos, nullptr, nullptr))
				continue;
			if(pCharacter->GetPlayer()->GetTeam() == pFlag->Team())
			{
				if(!pFlag->IsAtStand())
					FlagReturn(pFlag, pCharacter);
			}
			else
			{
				FlagGrab(pFlag, pCharacter);
				break;
			}
		}
	}
}

void CGameControllerVanillaCTF::FlagGrab(CFlag *pFlag, CCharacter *pCarrier)
{
	if(pFlag->IsAtStand())
		m_aTeamScores[pCarrier->GetPlayer()->GetTeam()]++;
	pFlag->Grab(pCarrier);
	m_aScores[pCarrier->GetPlayer()->GetCid()]++;
	log_info("game", "flag_grab player='%d:%s' team=%d", pCarrier->GetPlayer()->GetCid(), Server()->ClientName(pCarrier->GetPlayer()->GetCid()), pCarrier->GetPlayer()->GetTeam());
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(pPlayer)
			GameServer()->CreateSoundGlobal(pPlayer->GetTeam() == pFlag->Team() ? SOUND_CTF_GRAB_EN : SOUND_CTF_GRAB_PL, ClientId, CGameContext::FLAG_SIX);
	}
	GameServer()->SendGameMessage7(protocol7::GAMEMSG_CTF_GRAB, {pFlag->Team()});
}

void CGameControllerVanillaCTF::FlagReturn(CFlag *pFlag, CCharacter *pPlayer)
{
	pFlag->Return();
	m_aScores[pPlayer->GetPlayer()->GetCid()]++;
	log_info("game", "flag_return player='%d:%s' team=%d", pPlayer->GetPlayer()->GetCid(), Server()->ClientName(pPlayer->GetPlayer()->GetCid()), pPlayer->GetPlayer()->GetTeam());
	GameServer()->CreateSoundGlobal(SOUND_CTF_RETURN, -1, CGameContext::FLAG_SIX);
	GameServer()->SendGameMessage7(protocol7::GAMEMSG_CTF_RETURN);
}

void CGameControllerVanillaCTF::FlagCapture(CFlag *pFlag)
{
	CCharacter *pCarrier = pFlag->Carrier();
	if(!pCarrier)
		return;
	const int CarrierId = pCarrier->GetPlayer()->GetCid();
	const int CaptureTicks = Server()->Tick() - pFlag->GrabTick();
	m_aTeamScores[pCarrier->GetPlayer()->GetTeam()] += 100;
	m_aScores[CarrierId] += 5;
	log_info("game", "flag_capture player='%d:%s' team=%d time=%.2f", CarrierId, Server()->ClientName(CarrierId), pCarrier->GetPlayer()->GetTeam(), CaptureTicks / (float)Server()->TickSpeed());
	GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE, -1, CGameContext::FLAG_SIX);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		int TranslatedCarrierId = CarrierId;
		if(GameServer()->m_apPlayers[ClientId] && Server()->IsSixup(ClientId) && Server()->Translate(TranslatedCarrierId, ClientId))
			GameServer()->SendGameMessage7(protocol7::GAMEMSG_CTF_CAPTURE, {pFlag->Team(), TranslatedCarrierId, CaptureTicks}, ClientId);
	}
	for(CFlag *pResetFlag : m_apFlags)
		if(pResetFlag)
			pResetFlag->Return();
}

CFlag *CGameControllerVanillaCTF::Flag(int Team) const
{
	return Team >= TEAM_RED && Team <= TEAM_BLUE ? m_apFlags[Team] : nullptr;
}

int CGameControllerVanillaCTF::FlagCarrierState(const CFlag *pFlag, int SnappingClient) const
{
	if(!pFlag)
		return FLAG_MISSING;
	if(pFlag->IsAtStand())
		return FLAG_ATSTAND;
	if(pFlag->Carrier() && pFlag->Carrier()->GetPlayer())
	{
		int CarrierId = pFlag->Carrier()->GetPlayer()->GetCid();
		return Server()->Translate(CarrierId, SnappingClient) ? CarrierId : FLAG_TAKEN;
	}
	return FLAG_TAKEN;
}

void CGameControllerVanillaCTF::SnapMode(int SnappingClient)
{
	SnapTeamData(
		SnappingClient,
		FlagCarrierState(m_apFlags[TEAM_RED], SnappingClient),
		FlagCarrierState(m_apFlags[TEAM_BLUE], SnappingClient),
		m_apFlags[TEAM_RED] ? m_apFlags[TEAM_RED]->DropTick() : 0,
		m_apFlags[TEAM_BLUE] ? m_apFlags[TEAM_BLUE]->DropTick() : 0,
		true);
}
