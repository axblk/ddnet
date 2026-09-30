#ifndef GAME_SERVER_MODES_PVP_ANTICAMPER_H
#define GAME_SERVER_MODES_PVP_ANTICAMPER_H

#include <base/vmath.h>

/**
 * Whether a player camps: stays near one spot for too long.
 *
 * The spot is where the player was when the clock started; moving Range away
 * from it along either axis starts the clock anew. Warned a few seconds
 * before, a camper is punished once the time is up. As in ddnet-insta.
 */
class CAnticamper
{
	vec2 m_Spot = vec2(0.0f, 0.0f);
	// -1 while the clock does not run
	int m_PunishTick = -1;
	bool m_Warned = false;

public:
	static constexpr int WARN_SECONDS = 5;

	enum class EAction
	{
		NONE,
		WARN,
		PUNISH,
	};

	void Reset();
	/**
	 * Watches the player for a tick.
	 *
	 * @param Pos Where the player is.
	 * @param Tick The current tick.
	 * @param TickSpeed The ticks in a second.
	 * @param Seconds How long the player may stay at one spot.
	 * @param Range How far the player has to move away from the spot, along an axis.
	 */
	EAction Tick(vec2 Pos, int Tick, int TickSpeed, int Seconds, int Range);
};

#endif // GAME_SERVER_MODES_PVP_ANTICAMPER_H
