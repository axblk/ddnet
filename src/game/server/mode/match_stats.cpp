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
	if(const auto *pEvent = std::get_if<CMatchEventShotFired>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId) && ValidWeapon(pEvent->m_Weapon))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pEvent->m_ClientId];
			Stats.m_ShotsFired++;
			Stats.m_aShotsFired[pEvent->m_Weapon]++;
		}
	}
	else if(const auto *pEvent = std::get_if<CMatchEventWeaponHit>(&Event))
	{
		if(ValidClientId(pEvent->m_AttackerId) && ValidWeapon(pEvent->m_Weapon))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pEvent->m_AttackerId];
			Stats.m_WeaponHits++;
			Stats.m_aWeaponHits[pEvent->m_Weapon]++;
		}
	}
	else if(const auto *pEvent = std::get_if<CMatchEventDamage>(&Event))
	{
		if(pEvent->m_Amount <= 0)
			return;
		if(ValidClientId(pEvent->m_AttackerId))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pEvent->m_AttackerId];
			Stats.m_DamageDealt += pEvent->m_Amount;
			if(ValidWeapon(pEvent->m_Weapon))
				Stats.m_aDamageDealt[pEvent->m_Weapon] += pEvent->m_Amount;
		}
		if(ValidClientId(pEvent->m_VictimId))
			m_aPlayers[pEvent->m_VictimId].m_DamageTaken += pEvent->m_Amount;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventKill>(&Event))
	{
		if(ValidClientId(pEvent->m_KillerId))
		{
			CMatchPlayerStats &Stats = m_aPlayers[pEvent->m_KillerId];
			Stats.m_Kills++;
			if(ValidWeapon(pEvent->m_Weapon))
				Stats.m_aKills[pEvent->m_Weapon]++;
		}
		if(ValidClientId(pEvent->m_VictimId))
			m_aPlayers[pEvent->m_VictimId].m_Deaths++;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventSuicide>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
		{
			m_aPlayers[pEvent->m_ClientId].m_Suicides++;
			m_aPlayers[pEvent->m_ClientId].m_Deaths++;
		}
	}
	else if(const auto *pEvent = std::get_if<CMatchEventSpawn>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
			m_aPlayers[pEvent->m_ClientId].m_Spawns++;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventTeamChanged>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
			m_aPlayers[pEvent->m_ClientId].m_TeamChanges++;
	}
	else if(std::holds_alternative<CMatchEventRoundStarted>(Event))
	{
		m_RoundsStarted++;
	}
	else if(std::holds_alternative<CMatchEventRoundEnded>(Event))
	{
		m_RoundsEnded++;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventFlagGrab>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
			m_aPlayers[pEvent->m_ClientId].m_FlagGrabs++;
		if(ValidFlagTeam(pEvent->m_FlagTeam))
			m_aFlags[pEvent->m_FlagTeam].m_Grabs++;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventFlagDrop>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
			m_aPlayers[pEvent->m_ClientId].m_FlagDrops++;
		if(ValidFlagTeam(pEvent->m_FlagTeam))
			m_aFlags[pEvent->m_FlagTeam].m_Drops++;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventFlagReturn>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
			m_aPlayers[pEvent->m_ClientId].m_FlagReturns++;
		if(ValidFlagTeam(pEvent->m_FlagTeam))
			m_aFlags[pEvent->m_FlagTeam].m_Returns++;
	}
	else if(const auto *pEvent = std::get_if<CMatchEventFlagCapture>(&Event))
	{
		if(ValidClientId(pEvent->m_ClientId))
			m_aPlayers[pEvent->m_ClientId].m_FlagCaptures++;
		if(ValidFlagTeam(pEvent->m_FlagTeam))
			m_aFlags[pEvent->m_FlagTeam].m_Captures++;
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
