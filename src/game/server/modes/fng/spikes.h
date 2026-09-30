#ifndef GAME_SERVER_MODES_FNG_SPIKES_H
#define GAME_SERVER_MODES_FNG_SPIKES_H

#include <base/vmath.h>

#include <functional>

// The spikes of FNG maps and what they are worth, as ddnet-insta has them.
namespace Fng
{
	enum class ESpike
	{
		NONE,
		NORMAL,
		GOLD,
		GREEN,
		PURPLE,
		// of a team: points for putting an enemy into the own team's, a loss for the other's
		RED,
		BLUE,
	};

	// the spike of a tile index of the game or the front layer: gold 7, normal 8, red 9, blue 10, green 14, purple 15
	ESpike SpikeOfTile(int Index);

	/**
	 * The spike a tee touches: the tiles under the corners of a square of a
	 * third of its size around its centre, the nearest one of them.
	 *
	 * @param Pos The centre of the tee.
	 * @param ProximityRadius The radius of the tee.
	 * @param Width The width of the map in tiles.
	 * @param Height The height of the map in tiles.
	 * @param TileAt The spike at a tile position, of the game or the front layer.
	 */
	ESpike TouchedSpike(vec2 Pos, float ProximityRadius, int Width, int Height, const std::function<ESpike(int x, int y)> &TileAt);

	class CSpikePoints
	{
	public:
		int m_Player = 0;
		int m_Team = 0;
		// the spikes of the other team: the player loses m_Player and is frozen
		bool m_Wrong = false;
	};
	// from the settings; without teams every spike of a team is right
	CSpikePoints SpikePoints(ESpike Spike, int KillerTeam, bool TeamPlay);

	/**
	 * The push of the FNG hammer, the one the DDNet client predicts with PREDICT_FNG.
	 *
	 * @param From Where the hammer is swung from.
	 * @param To Where the tee is that it hits.
	 * @param Melt Whether it hits a frozen team mate.
	 */
	vec2 HammerForce(vec2 From, vec2 To, bool Melt);
	// whether the hammer settings are the ones the DDNet client predicts
	bool IsPredictedHammer();

	/**
	 * A hammer on a frozen team mate: three seconds less.
	 *
	 * @param FreezeTicks The ticks of freeze the team mate has left.
	 * @param TickSpeed The ticks in a second.
	 * @param pThawed Whether it thaws the team mate, which gives a point.
	 * @return The freeze left, at least the tick that thaws.
	 */
	int MeltFreeze(int FreezeTicks, int TickSpeed, bool *pThawed);
}

#endif // GAME_SERVER_MODES_FNG_SPIKES_H
