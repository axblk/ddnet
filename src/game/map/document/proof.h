#ifndef GAME_MAP_DOCUMENT_PROOF_H
#define GAME_MAP_DOCUMENT_PROOF_H

#include "map_state.h"

#include <base/vmath.h>

#include <vector>

namespace map_document
{
	/**
	 * What a player would see, as arithmetic.
	 *
	 * A map is drawn in an editor at whatever zoom somebody happens to be at,
	 * on whatever window they happen to have; a map is *played* on a screen
	 * whose shape nobody chose, at a zoom the game decides. Proof mode is the
	 * answer to "is this still on screen for everybody" - the one question an
	 * editor cannot answer by looking.
	 *
	 * The sums are the game's own (`CalcViewSize`), so the rectangle an editor
	 * draws and the view a client ends up with are the same rectangle rather
	 * than two guesses about it.
	 */

	/** A rectangle in world units. */
	class CProofRect
	{
	public:
		vec2 m_TopLeft;
		vec2 m_BottomRight;

		float Width() const { return m_BottomRight.x - m_TopLeft.x; }
		float Height() const { return m_BottomRight.y - m_TopLeft.y; }
	};

	/**
	 * Above this aspect a view only grows sideways rather than trading height
	 * for width - the client's `cl_view_max_aspect`, in hundredths.
	 *
	 * The editor has no settings of its own, so it takes the default. It
	 * matters little here: the shapes proof mode walks through stop just
	 * short of it.
	 */
	constexpr int PROOF_MAX_ASPECT = 178;

	/**
	 * What a screen of one shape covers, in world units, around a place.
	 *
	 * @param Center Where the camera stands, in the game layer's coordinates.
	 * @param Aspect How wide the screen is against how tall.
	 * @param Zoom What the game zooms to - 1 in a game, 0.7 behind a menu.
	 */
	CProofRect ProofScreen(vec2 Center, float Aspect, float Zoom);

	/** Where a menu background stands, and the number that picks it. */
	class CMenuPosition
	{
	public:
		int m_Index;
		vec2 m_Position;
	};

	/**
	 * The places a menu background camera can stand in this map.
	 *
	 * A map says so with time checkpoint tiles in its game layer: tile 35 is
	 * the first place, 36 the second, and so on. Places the map does not name
	 * are not here - where those stand is the client's business, and the
	 * document does not know the client.
	 */
	std::vector<CMenuPosition> MenuPositions(const CMapState &Map);
} // namespace map_document

#endif
