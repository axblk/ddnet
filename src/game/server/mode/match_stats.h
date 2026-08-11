#ifndef GAME_SERVER_MODE_MATCH_STATS_H
#define GAME_SERVER_MODE_MATCH_STATS_H

#include "match_events.h"

#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <array>

struct CMatchPlayerStats
{
	std::array<int, NUM_WEAPONS> m_aShotsFired{};
	std::array<int, NUM_WEAPONS> m_aWeaponHits{};
	std::array<int, NUM_WEAPONS> m_aDamageDealt{};
	std::array<int, NUM_WEAPONS> m_aKills{};
	int m_ShotsFired = 0;
	int m_WeaponHits = 0;
	int m_DamageDealt = 0;
	int m_DamageTaken = 0;
	int m_Kills = 0;
	int m_Deaths = 0;
	int m_Suicides = 0;
	int m_Spawns = 0;
	int m_TeamChanges = 0;
	int m_FlagGrabs = 0;
	int m_FlagDrops = 0;
	int m_FlagReturns = 0;
	int m_FlagCaptures = 0;
};

struct CMatchFlagStats
{
	int m_Grabs = 0;
	int m_Drops = 0;
	int m_Returns = 0;
	int m_Captures = 0;
};

class CMatchStats
{
	std::array<CMatchPlayerStats, MAX_CLIENTS> m_aPlayers{};
	std::array<CMatchFlagStats, NUM_TEAMS> m_aFlags{};
	int m_RoundsStarted = 0;
	int m_RoundsEnded = 0;

public:
	void OnEvent(const CMatchEvent &Event);
	const CMatchPlayerStats &Player(int ClientId) const;
	const CMatchFlagStats &Flag(int Team) const;
	int RoundsStarted() const { return m_RoundsStarted; }
	int RoundsEnded() const { return m_RoundsEnded; }
};

#endif // GAME_SERVER_MODE_MATCH_STATS_H
