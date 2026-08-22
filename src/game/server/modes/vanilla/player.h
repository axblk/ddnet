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
	// How many damage indicators the tee has collected in quick succession, so
	// that repeated hits fan out instead of printing on top of each other
	int m_DamageTaken = 0;
	int m_DamageTakenTick = 0;
};

#endif // GAME_SERVER_MODES_VANILLA_PLAYER_H
