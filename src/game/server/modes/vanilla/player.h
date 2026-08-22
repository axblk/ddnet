#ifndef GAME_SERVER_MODES_VANILLA_PLAYER_H
#define GAME_SERVER_MODES_VANILLA_PLAYER_H

#include <game/server/player.h>

class CPlayerVanilla : public CPlayer
{
public:
	CPlayerVanilla(CGameServices &Services, uint32_t UniqueClientId, int ClientId, int Team) :
		CPlayer(Services, UniqueClientId, ClientId, Team)
	{
	}

	void ResetRoundState(int LastNoAmmoSoundTick)
	{
		m_Score = 0;
		m_EarliestRespawnTick = 0;
		m_LastNoAmmoSoundTick = LastNoAmmoSoundTick;
	}

	int m_Score = 0;
	int m_EarliestRespawnTick = 0;
	int m_LastNoAmmoSoundTick = 0;
};

#endif // GAME_SERVER_MODES_VANILLA_PLAYER_H
