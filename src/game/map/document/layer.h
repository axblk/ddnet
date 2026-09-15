#ifndef GAME_MAP_DOCUMENT_LAYER_H
#define GAME_MAP_DOCUMENT_LAYER_H

#include <game/map/document/tile_store.h>
#include <game/mapitems.h>

#include <cstdint>
#include <string>
#include <variant>

/**
 * What kind of tiles a layer holds.
 *
 * Only `TILES` is a layer that is merely drawn. The rest are the physics
 * layers, and each of them carries a second plane of tiles next to the one
 * that is drawn: where a switch is, which tele it leads to, how hard a
 * speedup pushes. The file format keeps them apart the same way, and so does
 * the editor that exists today.
 */
enum class ETileLayerKind
{
	TILES,
	GAME,
	FRONT,
	TELE,
	SPEEDUP,
	SWITCH,
	TUNE,
};

/**
 * One tile layer, as a node that nobody writes to once it is shared.
 *
 * A version of the map is a version of every layer, so a layer has to be
 * copyable for the price of its pointers: what a copy costs is the table of
 * blocks, and the blocks themselves are shared until somebody writes to one
 * of the two copies - see `CTileStore`. Changing a layer is therefore
 * copying it, writing to the copy, and putting the copy in place of the old
 * one; the old one stays valid for as long as a version of the map still
 * names it, which is what makes undo a pointer swap.
 *
 * The fields are plain because a shared layer is held as `const` and a layer
 * that is being changed belongs to whoever is changing it. Hiding them behind
 * setters would suggest that a layer can be edited in place, which is exactly
 * what must not happen.
 */
class CTileLayer
{
public:
	/**
	 * The second plane of a physics layer, and nothing at all for a layer
	 * that is only drawn.
	 */
	using CExtraTiles = std::variant<
		std::monostate,
		CTileStore<CTeleTile>,
		CTileStore<CSpeedupTile>,
		CTileStore<CSwitchTile>,
		CTileStore<CTuneTile>>;

	CTileLayer() = default;

	CTileLayer(ETileLayerKind Kind, int Width, int Height) :
		m_Kind(Kind), m_Tiles(Width, Height)
	{
		switch(Kind)
		{
		case ETileLayerKind::TELE: m_ExtraTiles.emplace<CTileStore<CTeleTile>>(Width, Height); break;
		case ETileLayerKind::SPEEDUP: m_ExtraTiles.emplace<CTileStore<CSpeedupTile>>(Width, Height); break;
		case ETileLayerKind::SWITCH: m_ExtraTiles.emplace<CTileStore<CSwitchTile>>(Width, Height); break;
		case ETileLayerKind::TUNE: m_ExtraTiles.emplace<CTileStore<CTuneTile>>(Width, Height); break;
		case ETileLayerKind::TILES:
		case ETileLayerKind::GAME:
		case ETileLayerKind::FRONT: break;
		}
	}

	ETileLayerKind m_Kind = ETileLayerKind::TILES;
	std::string m_Name;
	bool m_Detail = false;

	// Only a layer that is drawn as an image has these; a physics layer takes
	// its picture from the entities of the game, not from the map.
	int m_Image = -1;
	CColor m_Color = {255, 255, 255, 255};
	int m_ColorEnvelope = -1;
	int m_ColorEnvelopeOffset = 0;

	CTileStore<CTile> m_Tiles;
	CExtraTiles m_ExtraTiles;

	int Width() const { return m_Tiles.Width(); }
	int Height() const { return m_Tiles.Height(); }

	/**
	 * What this layer holds, with a block it shares with another version of
	 * itself counted as its share - see `CTileStore::Bytes`.
	 */
	uint64_t Bytes() const
	{
		uint64_t Total = m_Tiles.Bytes();
		std::visit([&Total](const auto &Extra) {
			if constexpr(!std::is_same_v<std::decay_t<decltype(Extra)>, std::monostate>)
			{
				Total += Extra.Bytes();
			}
		},
			m_ExtraTiles);
		return Total;
	}
};

#endif // GAME_MAP_DOCUMENT_LAYER_H
