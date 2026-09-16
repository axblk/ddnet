#include "proof.h"

#include "structure.h"

#include <engine/graphics.h>

#include <game/mapitems.h>

#include <variant>

namespace map_document
{
	CProofRect ProofScreen(vec2 Center, float Aspect, float Zoom)
	{
		float Width, Height;
		CalcViewSize(Aspect, Zoom, PROOF_MAX_ASPECT / 100.0f, &Width, &Height);
		// A view at parallax 100 is the world at its own size, so the size the
		// game computes is the size on the map - no scaling comes after it.
		return CProofRect{
			vec2(Center.x - Width / 2.0f, Center.y - Height / 2.0f),
			vec2(Center.x + Width / 2.0f, Center.y + Height / 2.0f)};
	}

	std::vector<CMenuPosition> MenuPositions(const CMapState &Map)
	{
		std::vector<CMenuPosition> vPositions;
		const std::optional<CLayerAddress> Game = FindGameLayer(Map);
		if(!Game.has_value())
			return vPositions;
		const CTileLayer *pGame = std::get_if<CTileLayer>(Map.Layer(Game->m_Group, Game->m_Layer));
		if(pGame == nullptr)
			return vPositions;

		for(int y = 0; y < pGame->Height(); ++y)
		{
			for(int x = 0; x < pGame->Width(); ++x)
			{
				const int Index = pGame->m_Tiles.Get(x, y).m_Index;
				if(Index < TILE_TIME_CHECKPOINT_FIRST || Index > TILE_TIME_CHECKPOINT_LAST)
					continue;
				// The camera stands in the middle of the tile, not on its
				// corner, which is where a tee standing on it would be.
				vPositions.push_back(CMenuPosition{
					Index - TILE_TIME_CHECKPOINT_FIRST,
					vec2(x * 32.0f + 16.0f, y * 32.0f + 16.0f)});
			}
		}
		return vPositions;
	}
} // namespace map_document
