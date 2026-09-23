#ifndef GAME_SERVER_GAMEMODES_DDRACE_PLAYER_H
#define GAME_SERVER_GAMEMODES_DDRACE_PLAYER_H

#include <game/server/player.h>

class CPlayerDDRace : public CPlayer
{
public:
	CPlayerDDRace(CGameServices &Services, uint32_t UniqueClientId, int ClientId, int Team);

	enum
	{
		TIMERTYPE_DEFAULT = -1,
		TIMERTYPE_GAMETIMER,
		TIMERTYPE_BROADCAST,
		TIMERTYPE_GAMETIMER_AND_BROADCAST,
		TIMERTYPE_SIXUP,
		TIMERTYPE_NONE,
	};

	// false if the client cannot show that timer
	bool SetTimerType(int TimerType);

	int m_TimerType;
	bool m_NinjaJetpack = false;
};

#endif // GAME_SERVER_GAMEMODES_DDRACE_PLAYER_H
