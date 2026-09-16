#include <game/map/document_source.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>

using namespace map_document;

// The document keeps what a map file holds, and a physics layer holds air
// where the tiles it draws would be. What it draws is worked out from its
// second plane, and that is what is tested here - without a graphics card,
// because the source is asked for its tiles the same way the cache asks.

namespace
{
	class CDrawn
	{
	public:
		unsigned char m_Index = 0;
		unsigned char m_Flags = 0;
		int m_Angle = 0;
	};

	CDrawn Read(const CTileChunkCache::CLayerSource &Source, int x, int y)
	{
		CDrawn Drawn;
		Source.m_ReadTile(x, y, &Drawn.m_Index, &Drawn.m_Flags, &Drawn.m_Angle);
		return Drawn;
	}

	std::shared_ptr<const CLayer> Wrap(CTileLayer Layer)
	{
		return std::make_shared<const CLayer>(std::move(Layer));
	}
}

TEST(DocumentSource, DrawsTheTilesOfALayerThatHasThem)
{
	CTileLayer Layer(ETileLayerKind::TILES, 64, 64);
	Layer.m_Image = 2;
	CTile Tile = {};
	Tile.m_Index = 7;
	Tile.m_Flags = TILEFLAG_XFLIP;
	Layer.m_Tiles.Set(1, 2, Tile);

	const CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(std::move(Layer)));
	EXPECT_EQ(Source.m_Width, 64);
	EXPECT_EQ(Source.m_Height, 64);
	EXPECT_TRUE(Source.m_Textured);
	EXPECT_FALSE(Source.m_FillSpeedup);

	EXPECT_EQ(Read(Source, 1, 2).m_Index, 7);
	EXPECT_EQ(Read(Source, 1, 2).m_Flags, TILEFLAG_XFLIP);
	EXPECT_EQ(Read(Source, 0, 0).m_Index, 0);
}

TEST(DocumentSource, ALayerWithoutAnImageIsNotTextured)
{
	const CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(CTileLayer(ETileLayerKind::TILES, 16, 16)));
	EXPECT_FALSE(Source.m_Textured);
}

TEST(DocumentSource, APhysicsLayerIsTexturedByTheEntitiesSheet)
{
	// No picture of its own, and drawn out of one all the same: uploaded
	// without texture coordinates, the graphics would drop its draw the moment
	// the entities sheet is bound.
	for(const ETileLayerKind Kind : {ETileLayerKind::GAME, ETileLayerKind::FRONT, ETileLayerKind::TELE, ETileLayerKind::SPEEDUP, ETileLayerKind::SWITCH, ETileLayerKind::TUNE})
		EXPECT_TRUE(DocumentLayerSource(Wrap(CTileLayer(Kind, 16, 16))).m_Textured);
}

TEST(DocumentSource, DrawsATeleLayerFromItsSecondPlane)
{
	CTileLayer Layer(ETileLayerKind::TELE, 64, 64);
	auto &Tele = std::get<CTileStore<CTeleTile>>(Layer.m_ExtraTiles);
	CTeleTile In = {};
	In.m_Type = TILE_TELEIN;
	In.m_Number = 3;
	Tele.Set(1, 1, In);
	CTeleTile Nonsense = {};
	Nonsense.m_Type = 123;
	Tele.Set(2, 1, Nonsense);

	const CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(std::move(Layer)));
	EXPECT_EQ(Read(Source, 1, 1).m_Index, TILE_TELEIN);
	// A number that is no tele tile draws nothing rather than something else.
	EXPECT_EQ(Read(Source, 2, 1).m_Index, 0);
	EXPECT_EQ(Read(Source, 5, 5).m_Index, 0);
}

TEST(DocumentSource, DrawsASpeedupLayerWithItsAngle)
{
	CTileLayer Layer(ETileLayerKind::SPEEDUP, 64, 64);
	auto &Speedup = std::get<CTileStore<CSpeedupTile>>(Layer.m_ExtraTiles);
	CSpeedupTile Pushes = {};
	Pushes.m_Type = TILE_SPEED_BOOST;
	Pushes.m_Force = 20;
	Pushes.m_Angle = 90;
	Speedup.Set(1, 1, Pushes);
	CSpeedupTile Limp = {};
	Limp.m_Type = TILE_SPEED_BOOST;
	Limp.m_Force = 0;
	Limp.m_Angle = 45;
	Speedup.Set(2, 1, Limp);

	const CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(std::move(Layer)));
	EXPECT_TRUE(Source.m_FillSpeedup);
	EXPECT_EQ(Read(Source, 1, 1).m_Index, TILE_SPEED_BOOST);
	EXPECT_EQ(Read(Source, 1, 1).m_Angle, 90);
	// A speedup that pushes with no force pushes nothing and is not drawn.
	EXPECT_EQ(Read(Source, 2, 1).m_Index, 0);
}

TEST(DocumentSource, DrawsASwitchLayerByWhatItSwitches)
{
	CTileLayer Layer(ETileLayerKind::SWITCH, 64, 64);
	auto &Switch = std::get<CTileStore<CSwitchTile>>(Layer.m_ExtraTiles);
	CSwitchTile Door = {};
	Door.m_Type = ENTITY_DOOR + ENTITY_OFFSET;
	Door.m_Flags = TILEFLAG_YFLIP;
	Door.m_Number = 4;
	Switch.Set(1, 1, Door);
	CSwitchTile Timed = {};
	Timed.m_Type = TILE_SWITCHTIMEDOPEN;
	Switch.Set(2, 1, Timed);
	CSwitchTile DrawnByTheGame = {};
	DrawnByTheGame.m_Type = ENTITY_CRAZY_SHOTGUN + ENTITY_OFFSET + 1;
	Switch.Set(3, 1, DrawnByTheGame);

	const CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(std::move(Layer)));
	EXPECT_EQ(Read(Source, 1, 1).m_Index, ENTITY_DOOR + ENTITY_OFFSET);
	EXPECT_EQ(Read(Source, 1, 1).m_Flags, TILEFLAG_YFLIP);
	EXPECT_EQ(Read(Source, 2, 1).m_Index, TILE_SWITCHTIMEDOPEN);
	// The entities in that range are drawn by the game, not from the tileset.
	EXPECT_EQ(Read(Source, 3, 1).m_Index, 0);
}

TEST(DocumentSource, DrawsATuneLayerFromItsSecondPlane)
{
	CTileLayer Layer(ETileLayerKind::TUNE, 64, 64);
	auto &Tune = std::get<CTileStore<CTuneTile>>(Layer.m_ExtraTiles);
	CTuneTile Zone = {};
	Zone.m_Type = TILE_TUNE;
	Zone.m_Number = 2;
	Tune.Set(1, 1, Zone);

	const CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(std::move(Layer)));
	EXPECT_EQ(Read(Source, 1, 1).m_Index, TILE_TUNE);
	EXPECT_EQ(Read(Source, 0, 0).m_Index, 0);
}

TEST(DocumentSource, TheSourceHoldsTheVersionItDraws)
{
	CTileLayer Layer(ETileLayerKind::TILES, 64, 64);
	CTile Tile = {};
	Tile.m_Index = 9;
	Layer.m_Tiles.Set(1, 1, Tile);

	CTileChunkCache::CLayerSource Source = DocumentLayerSource(Wrap(std::move(Layer)));
	// Whoever made the source is gone, and the layer it draws is still there:
	// a version that is being drawn cannot be edited away under the renderer.
	EXPECT_EQ(Read(Source, 1, 1).m_Index, 9);
}
