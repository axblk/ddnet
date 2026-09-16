#include <base/dbg.h>

#include <game/map/document_source.h>
#include <game/mapitems.h>

#include <variant>

using namespace map_document;

namespace
{
	// What a physics layer draws where it has a tile of its own, and air where it
	// has none. The rules are the ones the editor has always drawn these by.
	void ReadTele(const CTileStore<CTeleTile> &Tele, int x, int y, unsigned char *pIndex, unsigned char *pFlags)
	{
		const CTeleTile Tile = Tele.Get(x, y);
		*pIndex = IsValidTeleTile(Tile.m_Type) ? Tile.m_Type : 0;
		*pFlags = 0;
	}

	void ReadSpeedup(const CTileStore<CSpeedupTile> &Speedup, int x, int y, unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate)
	{
		const CSpeedupTile Tile = Speedup.Get(x, y);
		// A speedup that pushes with no force pushes nothing, and is not drawn.
		*pIndex = IsValidSpeedupTile(Tile.m_Type) && Tile.m_Force > 0 ? Tile.m_Type : 0;
		*pFlags = 0;
		*pAngleRotate = Tile.m_Angle;
	}

	void ReadSwitch(const CTileStore<CSwitchTile> &Switch, int x, int y, unsigned char *pIndex, unsigned char *pFlags)
	{
		const CSwitchTile Tile = Switch.Get(x, y);
		*pIndex = 0;
		*pFlags = 0;
		// The entities between these two are drawn by the game rather than from
		// the tileset, so the switch layer leaves their place empty.
		if((Tile.m_Type > ENTITY_CRAZY_SHOTGUN + ENTITY_OFFSET && Tile.m_Type < ENTITY_DRAGGER_WEAK + ENTITY_OFFSET) ||
			Tile.m_Type == ENTITY_LASER_O_FAST + 1 + ENTITY_OFFSET)
			return;
		// A switched pickup or door keeps the picture of the entity it switches.
		if((Tile.m_Type >= ENTITY_ARMOR_1 + ENTITY_OFFSET && Tile.m_Type <= ENTITY_DOOR + ENTITY_OFFSET) ||
			IsValidSwitchTile(Tile.m_Type))
		{
			*pIndex = Tile.m_Type;
			*pFlags = Tile.m_Flags;
		}
	}

	void ReadTune(const CTileStore<CTuneTile> &Tune, int x, int y, unsigned char *pIndex, unsigned char *pFlags)
	{
		const CTuneTile Tile = Tune.Get(x, y);
		*pIndex = IsValidTuneTile(Tile.m_Type) ? Tile.m_Type : 0;
		*pFlags = 0;
	}
} // namespace

CTileChunkCache::CLayerSource DocumentLayerSource(const std::shared_ptr<const CLayer> &pLayer)
{
	dbg_assert(pLayer != nullptr && std::holds_alternative<CTileLayer>(*pLayer), "Only a tile layer can be drawn as one");
	// Points at the tile layer while holding on to the layer it is part of,
	// so the version being drawn stays whole for as long as it is drawn.
	const std::shared_ptr<const CTileLayer> pTiles(pLayer, &std::get<CTileLayer>(*pLayer));

	CTileChunkCache::CLayerSource Source;
	Source.m_Width = pTiles->Width();
	Source.m_Height = pTiles->Height();
	Source.m_Textured = pTiles->m_Image >= 0;
	Source.m_FillSpeedup = pTiles->m_Kind == ETileLayerKind::SPEEDUP;
	Source.m_ReadTile = [pTiles](int x, int y, unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate) {
		switch(pTiles->m_Kind)
		{
		case ETileLayerKind::TILES:
		case ETileLayerKind::GAME:
		case ETileLayerKind::FRONT:
		{
			// These draw the tiles they hold, and the front layer holds its
			// own - the file keeps them apart, the drawing does not.
			const CTile Tile = pTiles->m_Tiles.Get(x, y);
			*pIndex = Tile.m_Index;
			*pFlags = Tile.m_Flags;
			break;
		}
		case ETileLayerKind::TELE:
			ReadTele(std::get<CTileStore<CTeleTile>>(pTiles->m_ExtraTiles), x, y, pIndex, pFlags);
			break;
		case ETileLayerKind::SPEEDUP:
			ReadSpeedup(std::get<CTileStore<CSpeedupTile>>(pTiles->m_ExtraTiles), x, y, pIndex, pFlags, pAngleRotate);
			break;
		case ETileLayerKind::SWITCH:
			ReadSwitch(std::get<CTileStore<CSwitchTile>>(pTiles->m_ExtraTiles), x, y, pIndex, pFlags);
			break;
		case ETileLayerKind::TUNE:
			ReadTune(std::get<CTileStore<CTuneTile>>(pTiles->m_ExtraTiles), x, y, pIndex, pFlags);
			break;
		}
	};
	return Source;
}
