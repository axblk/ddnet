#ifndef GAME_SERVER_MODE_MATCH_EVENTS_H
#define GAME_SERVER_MODE_MATCH_EVENTS_H

#include <variant>

struct CMatchEventShotFired
{
	int m_ClientId;
	int m_Weapon;
};

struct CMatchEventWeaponHit
{
	int m_AttackerId;
	int m_VictimId;
	int m_Weapon;
};

struct CMatchEventDamage
{
	int m_AttackerId;
	int m_VictimId;
	int m_Weapon;
	int m_Amount;
};

struct CMatchEventKill
{
	int m_KillerId;
	int m_VictimId;
	int m_Weapon;
};

struct CMatchEventSuicide
{
	int m_ClientId;
	int m_Weapon;
};

struct CMatchEventSpawn
{
	int m_ClientId;
	int m_Team;
};

struct CMatchEventTeamChanged
{
	int m_ClientId;
	int m_OldTeam;
	int m_NewTeam;
};

struct CMatchEventRoundStarted
{
};

struct CMatchEventRoundEnded
{
};

struct CMatchEventFlagGrab
{
	int m_ClientId;
	int m_FlagTeam;
};

struct CMatchEventFlagDrop
{
	int m_ClientId;
	int m_FlagTeam;
};

struct CMatchEventFlagReturn
{
	int m_ClientId;
	int m_FlagTeam;
};

struct CMatchEventFlagCapture
{
	int m_ClientId;
	int m_FlagTeam;
	int m_CaptureTicks;
};

using CMatchEvent = std::variant<
	CMatchEventShotFired,
	CMatchEventWeaponHit,
	CMatchEventDamage,
	CMatchEventKill,
	CMatchEventSuicide,
	CMatchEventSpawn,
	CMatchEventTeamChanged,
	CMatchEventRoundStarted,
	CMatchEventRoundEnded,
	CMatchEventFlagGrab,
	CMatchEventFlagDrop,
	CMatchEventFlagReturn,
	CMatchEventFlagCapture>;

#endif // GAME_SERVER_MODE_MATCH_EVENTS_H
