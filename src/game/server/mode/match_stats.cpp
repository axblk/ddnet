#include "match_stats.h"

#include <base/dbg.h>

namespace
{
	bool ValidClientId(int ClientId)
	{
		return ClientId >= 0 && ClientId < MAX_CLIENTS;
	}

	bool ValidWeapon(int Weapon)
	{
		return Weapon >= 0 && Weapon < NUM_WEAPONS;
	}

	bool ValidFlagTeam(int Team)
	{
		return Team >= TEAM_RED && Team <= TEAM_BLUE;
	}
}

void CMatchStats::OnEvent(const CMatchEvent &Event)
{
	if(const auto *pShotFired = std::get_if<CMatchEventShotFired>(&Event))
	{
		if(ValidClientId(pShotFired->m_ClientId) && ValidWeapon(pShotFired->m_Weapon))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pShotFired->m_ClientId];
			Stats.m_ShotsFired++;
			Stats.m_aShotsFired[pShotFired->m_Weapon]++;
		}
	}
	else if(const auto *pWeaponHit = std::get_if<CMatchEventWeaponHit>(&Event))
	{
		if(ValidClientId(pWeaponHit->m_AttackerId) && ValidWeapon(pWeaponHit->m_Weapon))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pWeaponHit->m_AttackerId];
			Stats.m_WeaponHits++;
			Stats.m_aWeaponHits[pWeaponHit->m_Weapon]++;
		}
	}
	else if(const auto *pDamage = std::get_if<CMatchEventDamage>(&Event))
	{
		if(pDamage->m_Amount <= 0)
			return;
		if(ValidClientId(pDamage->m_AttackerId))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pDamage->m_AttackerId];
			Stats.m_DamageDealt += pDamage->m_Amount;
			if(ValidWeapon(pDamage->m_Weapon))
				Stats.m_aDamageDealt[pDamage->m_Weapon] += pDamage->m_Amount;
		}
		if(ValidClientId(pDamage->m_VictimId))
			m_aPlayers[pDamage->m_VictimId].m_DamageTaken += pDamage->m_Amount;
	}
	else if(const auto *pKill = std::get_if<CMatchEventKill>(&Event))
	{
		if(ValidClientId(pKill->m_KillerId))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pKill->m_KillerId];
			Stats.m_Kills++;
			if(ValidWeapon(pKill->m_Weapon))
				Stats.m_aKills[pKill->m_Weapon]++;
		}
		if(ValidClientId(pKill->m_VictimId))
			m_aPlayers[pKill->m_VictimId].m_Deaths++;
	}
	else if(const auto *pSuicide = std::get_if<CMatchEventSuicide>(&Event))
	{
		if(ValidClientId(pSuicide->m_ClientId))
		{
			m_aPlayers[pSuicide->m_ClientId].m_Suicides++;
			m_aPlayers[pSuicide->m_ClientId].m_Deaths++;
		}
	}
	else if(const auto *pSpawn = std::get_if<CMatchEventSpawn>(&Event))
	{
		if(ValidClientId(pSpawn->m_ClientId))
			m_aPlayers[pSpawn->m_ClientId].m_Spawns++;
	}
	else if(const auto *pTeamChanged = std::get_if<CMatchEventTeamChanged>(&Event))
	{
		if(ValidClientId(pTeamChanged->m_ClientId))
			m_aPlayers[pTeamChanged->m_ClientId].m_TeamChanges++;
	}
	else if(std::holds_alternative<CMatchEventRoundStarted>(Event))
	{
		m_RoundsStarted++;
	}
	else if(std::holds_alternative<CMatchEventRoundEnded>(Event))
	{
		m_RoundsEnded++;
	}
	else if(const auto *pFlagGrab = std::get_if<CMatchEventFlagGrab>(&Event))
	{
		if(ValidClientId(pFlagGrab->m_ClientId))
			m_aPlayers[pFlagGrab->m_ClientId].m_FlagGrabs++;
		if(ValidFlagTeam(pFlagGrab->m_FlagTeam))
			m_aFlags[pFlagGrab->m_FlagTeam].m_Grabs++;
	}
	else if(const auto *pFlagDrop = std::get_if<CMatchEventFlagDrop>(&Event))
	{
		if(ValidClientId(pFlagDrop->m_ClientId))
			m_aPlayers[pFlagDrop->m_ClientId].m_FlagDrops++;
		if(ValidFlagTeam(pFlagDrop->m_FlagTeam))
			m_aFlags[pFlagDrop->m_FlagTeam].m_Drops++;
	}
	else if(const auto *pFlagReturn = std::get_if<CMatchEventFlagReturn>(&Event))
	{
		if(ValidClientId(pFlagReturn->m_ClientId))
			m_aPlayers[pFlagReturn->m_ClientId].m_FlagReturns++;
		if(ValidFlagTeam(pFlagReturn->m_FlagTeam))
			m_aFlags[pFlagReturn->m_FlagTeam].m_Returns++;
	}
	else if(const auto *pFlagCapture = std::get_if<CMatchEventFlagCapture>(&Event))
	{
		if(ValidClientId(pFlagCapture->m_ClientId))
			m_aPlayers[pFlagCapture->m_ClientId].m_FlagCaptures++;
		if(ValidFlagTeam(pFlagCapture->m_FlagTeam))
			m_aFlags[pFlagCapture->m_FlagTeam].m_Captures++;
	}
}

const CMatchPlayerStats &CMatchStats::Player(int ClientId) const
{
	dbg_assert(ValidClientId(ClientId), "invalid match stats ClientId: %d", ClientId);
	return m_aPlayers[ClientId];
}

const CMatchFlagStats &CMatchStats::Flag(int Team) const
{
	dbg_assert(ValidFlagTeam(Team), "invalid match stats flag team: %d", Team);
	return m_aFlags[Team];
}
