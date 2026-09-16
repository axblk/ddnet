#include <game/map/document/edit.h>

#include <gtest/gtest.h>

using namespace map_document;

// What a tool does to a map: it stamps a brush into a layer, fills a
// rectangle with it, takes a piece back out again, and turns what it holds.
// The point of every test here is that the layer ends up holding what the
// file would hold - a physics layer says what it means in its own plane, and
// a tile that the game reads does not get flags it cannot have.

namespace
{
	CTile Tile(int Index, int Flags = 0)
	{
		CTile Result = {};
		Result.m_Index = (unsigned char)Index;
		Result.m_Flags = (unsigned char)Flags;
		return Result;
	}

	CBrush TileBrush(int Width, int Height, int FirstIndex)
	{
		CBrush Brush(ETileLayerKind::TILES, Width, Height);
		for(int y = 0; y < Height; ++y)
		{
			for(int x = 0; x < Width; ++x)
				Brush.m_Tiles.Set(x, y, Tile(FirstIndex + y * Width + x));
		}
		return Brush;
	}

	int IndexAt(const CTileLayer &Layer, int x, int y) { return Layer.m_Tiles.Get(x, y).m_Index; }

	CTeleTile TeleTile(int Type, int Number)
	{
		CTeleTile Result = {};
		Result.m_Type = (unsigned char)Type;
		Result.m_Number = (unsigned char)Number;
		return Result;
	}

	const CTileStore<CTeleTile> &TeleTiles(const CTileLayer &Layer)
	{
		return std::get<CTileStore<CTeleTile>>(Layer.m_ExtraTiles);
	}

	CMapState OneLayer(ETileLayerKind Kind = ETileLayerKind::TILES, int Width = 16, int Height = 8)
	{
		CMapState State;
		CGroup Group;
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(CTileLayer(Kind, Width, Height)));
		State.AddGroup(std::move(Group));
		return State;
	}
} // namespace

TEST(Edit, AStampPutsTheBrushWhereItWasPut)
{
	CTileLayer Layer(ETileLayerKind::TILES, 16, 8);
	StampTiles(Layer, 3, 2, TileBrush(2, 2, 10));

	EXPECT_EQ(IndexAt(Layer, 3, 2), 10);
	EXPECT_EQ(IndexAt(Layer, 4, 2), 11);
	EXPECT_EQ(IndexAt(Layer, 3, 3), 12);
	EXPECT_EQ(IndexAt(Layer, 4, 3), 13);
	// Everything beside it is still air.
	EXPECT_EQ(IndexAt(Layer, 2, 2), 0);
	EXPECT_EQ(IndexAt(Layer, 5, 3), 0);
}

TEST(Edit, WhatFallsOutsideTheLayerIsLeftOff)
{
	CTileLayer Layer(ETileLayerKind::TILES, 16, 8);
	StampTiles(Layer, -1, 7, TileBrush(2, 2, 10));

	// Only the corner of the brush that is inside the layer is drawn, and
	// nothing outside it is touched - there is nothing there to touch.
	EXPECT_EQ(IndexAt(Layer, 0, 7), 11);
	EXPECT_EQ(IndexAt(Layer, 1, 7), 0);
}

TEST(Edit, FillingRepeatsTheBrush)
{
	CTileLayer Layer(ETileLayerKind::TILES, 16, 8);
	FillTiles(Layer, 0, 0, 5, 1, TileBrush(2, 1, 10));

	EXPECT_EQ(IndexAt(Layer, 0, 0), 10);
	EXPECT_EQ(IndexAt(Layer, 1, 0), 11);
	EXPECT_EQ(IndexAt(Layer, 2, 0), 10);
	EXPECT_EQ(IndexAt(Layer, 3, 0), 11);
	EXPECT_EQ(IndexAt(Layer, 4, 0), 10);
	EXPECT_EQ(IndexAt(Layer, 5, 0), 0);
}

TEST(Edit, ErasingPutsAirBack)
{
	CTileLayer Layer(ETileLayerKind::TILES, 16, 8);
	FillTiles(Layer, 0, 0, 16, 8, TileBrush(1, 1, 7));
	EraseTiles(Layer, 2, 2, 3, 3);

	EXPECT_EQ(IndexAt(Layer, 2, 2), 0);
	EXPECT_EQ(IndexAt(Layer, 4, 4), 0);
	EXPECT_EQ(IndexAt(Layer, 5, 4), 7);
	EXPECT_EQ(IndexAt(Layer, 1, 2), 7);
}

TEST(Edit, WhatIsGrabbedIsWhatIsStampedBack)
{
	CTileLayer Layer(ETileLayerKind::TILES, 16, 8);
	FillTiles(Layer, 0, 0, 16, 8, TileBrush(3, 3, 20));

	const CBrush Brush = GrabTiles(Layer, 4, 1, 3, 2);
	ASSERT_EQ(Brush.Width(), 3);
	ASSERT_EQ(Brush.Height(), 2);

	CTileLayer Other(ETileLayerKind::TILES, 16, 8);
	StampTiles(Other, 0, 0, Brush);
	for(int y = 0; y < 2; ++y)
	{
		for(int x = 0; x < 3; ++x)
			EXPECT_EQ(IndexAt(Other, x, y), IndexAt(Layer, 4 + x, 1 + y));
	}
}

TEST(Edit, AGrabIsClippedToTheLayer)
{
	CTileLayer Layer(ETileLayerKind::TILES, 16, 8);
	const CBrush Brush = GrabTiles(Layer, 14, 6, 4, 4);
	EXPECT_EQ(Brush.Width(), 2);
	EXPECT_EQ(Brush.Height(), 2);
}

TEST(Edit, APhysicsLayerIsPaintedInItsOwnPlane)
{
	CTileLayer Layer(ETileLayerKind::TELE, 16, 8);
	CBrush Brush(ETileLayerKind::TELE, 1, 1);
	std::get<CTileStore<CTeleTile>>(Brush.m_ExtraTiles).Set(0, 0, TeleTile(TILE_TELEIN, 3));

	StampTiles(Layer, 5, 5, Brush);

	EXPECT_EQ(TeleTiles(Layer).Get(5, 5).m_Type, TILE_TELEIN);
	EXPECT_EQ(TeleTiles(Layer).Get(5, 5).m_Number, 3);
	// The plane the file keeps for a tele layer stays the plane of air it is.
	EXPECT_EQ(IndexAt(Layer, 5, 5), 0);
}

TEST(Edit, ThePlainKindsTakeEachOthersTilesAndThePhysicsLayersDoNot)
{
	EXPECT_TRUE(CanStamp(ETileLayerKind::GAME, ETileLayerKind::TILES));
	EXPECT_TRUE(CanStamp(ETileLayerKind::TILES, ETileLayerKind::FRONT));
	EXPECT_TRUE(CanStamp(ETileLayerKind::TELE, ETileLayerKind::TELE));
	EXPECT_FALSE(CanStamp(ETileLayerKind::TELE, ETileLayerKind::TILES));
	EXPECT_FALSE(CanStamp(ETileLayerKind::TILES, ETileLayerKind::SWITCH));
}

TEST(Edit, AMirroredBrushIsTheBrushTheOtherWayRound)
{
	CBrush Brush = TileBrush(3, 2, 1);
	FlipBrushX(Brush);

	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Index, 3);
	EXPECT_EQ(Brush.m_Tiles.Get(2, 0).m_Index, 1);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 1).m_Index, 6);

	FlipBrushY(Brush);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Index, 6);
	EXPECT_EQ(Brush.m_Tiles.Get(2, 1).m_Index, 1);
}

TEST(Edit, AMirroredTileIsDrawnMirrored)
{
	CBrush Brush(ETileLayerKind::TILES, 1, 1);
	Brush.m_Tiles.Set(0, 0, Tile(5, 0));
	FlipBrushX(Brush);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Flags, TILEFLAG_XFLIP);

	// A tile that is already turned is mirrored the other way round, because
	// what mirrors it after the turn is the other flag.
	Brush.m_Tiles.Set(0, 0, Tile(5, TILEFLAG_ROTATE));
	FlipBrushX(Brush);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Flags, TILEFLAG_ROTATE | TILEFLAG_YFLIP);
}

TEST(Edit, ATileTheGameReadsLosesFlagsItCannotHave)
{
	CBrush Brush(ETileLayerKind::GAME, 1, 1);
	Brush.m_Tiles.Set(0, 0, Tile(TILE_SOLID, 0));
	FlipBrushX(Brush);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Flags, 0);

	// One that may be turned keeps its flags in a game layer too - a stopper
	// is the whole point of turning a tile the game reads.
	ASSERT_TRUE(IsRotatableTile(TILE_STOP));
	Brush.m_Tiles.Set(0, 0, Tile(TILE_STOP, 0));
	FlipBrushX(Brush);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Flags, TILEFLAG_XFLIP);
}

TEST(Edit, ATurnedBrushSwapsItsSides)
{
	CBrush Brush = TileBrush(3, 2, 1);
	// 1 2 3     4 1
	// 4 5 6  ->  5 2
	//            6 3
	RotateBrush(Brush);

	ASSERT_EQ(Brush.Width(), 2);
	ASSERT_EQ(Brush.Height(), 3);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 0).m_Index, 4);
	EXPECT_EQ(Brush.m_Tiles.Get(1, 0).m_Index, 1);
	EXPECT_EQ(Brush.m_Tiles.Get(0, 2).m_Index, 6);
	EXPECT_EQ(Brush.m_Tiles.Get(1, 2).m_Index, 3);
}

TEST(Edit, FourTurnsAreNoTurnAtAll)
{
	const CBrush Was = TileBrush(3, 2, 1);
	CBrush Brush = Was;
	for(int i = 0; i < 4; ++i)
		RotateBrush(Brush);
	EXPECT_EQ(Brush, Was);
}

TEST(Edit, ASpeedupIsTurnedWithTheBrush)
{
	CBrush Brush(ETileLayerKind::SPEEDUP, 1, 1);
	CSpeedupTile Speedup = {};
	Speedup.m_Type = TILE_SPEED_BOOST;
	Speedup.m_Force = 10;
	Speedup.m_Angle = 0; // pushing to the right
	std::get<CTileStore<CSpeedupTile>>(Brush.m_ExtraTiles).Set(0, 0, Speedup);

	FlipBrushX(Brush);
	EXPECT_EQ(std::get<CTileStore<CSpeedupTile>>(Brush.m_ExtraTiles).Get(0, 0).m_Angle, 180);

	RotateBrush(Brush);
	EXPECT_EQ(std::get<CTileStore<CSpeedupTile>>(Brush.m_ExtraTiles).Get(0, 0).m_Angle, 270);
}

TEST(Edit, ATeleTileKeepsItsNumberWhenTheBrushIsTurned)
{
	CBrush Brush(ETileLayerKind::TELE, 2, 1);
	auto &Tele = std::get<CTileStore<CTeleTile>>(Brush.m_ExtraTiles);
	Tele.Set(0, 0, TeleTile(TILE_TELEIN, 7));
	Tele.Set(1, 0, TeleTile(TILE_TELEOUT, 8));

	FlipBrushX(Brush);
	const auto &Turned = std::get<CTileStore<CTeleTile>>(Brush.m_ExtraTiles);
	EXPECT_EQ(Turned.Get(0, 0).m_Type, TILE_TELEOUT);
	EXPECT_EQ(Turned.Get(0, 0).m_Number, 8);
	EXPECT_EQ(Turned.Get(1, 0).m_Number, 7);
}

TEST(Edit, AStrokeIsOneEntryHoweverManyStampsItTook)
{
	CDocument Document(OneLayer());
	const CBrush Brush = TileBrush(1, 1, 5);

	Document.Begin("Paint");
	for(int x = 0; x < 8; ++x)
		PaintTiles(Document, 0, 0, x, 0, Brush);
	Document.Commit();

	EXPECT_EQ(Document.History().NumEntries(), 2u);
	EXPECT_EQ(IndexAt(*Document.Map().TileLayer(0, 0), 7, 0), 5);

	ASSERT_TRUE(Document.Undo());
	EXPECT_EQ(IndexAt(*Document.Map().TileLayer(0, 0), 7, 0), 0);
	EXPECT_EQ(IndexAt(*Document.Map().TileLayer(0, 0), 0, 0), 0);
}

TEST(Edit, PaintingTheTileThatIsAlreadyThereIsNoChange)
{
	CDocument Document(OneLayer());
	const CBrush Air(ETileLayerKind::TILES, 1, 1);

	Document.Begin("Paint");
	PaintTiles(Document, 0, 0, 0, 0, Air);
	Document.Commit();

	// The layer is a new node either way - what says that nothing happened is
	// that it holds the same tiles, and the transaction looks.
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_FALSE(Document.CanUndo());
}

TEST(Edit, AStrokeOnlyTakesApartTheBlocksItTouched)
{
	CDocument Document(OneLayer(ETileLayerKind::TILES, 4 * CTileStore<CTile>::CHUNK_SIZE, CTileStore<CTile>::CHUNK_SIZE));
	const CTileLayer *pBefore = Document.Map().TileLayer(0, 0);
	const void *pFarBlock = pBefore->m_Tiles.ChunkId(3, 0);

	Document.Begin("Paint");
	PaintTiles(Document, 0, 0, 1, 1, TileBrush(1, 1, 9));
	Document.Commit();

	const CTileLayer *pAfter = Document.Map().TileLayer(0, 0);
	EXPECT_NE(pAfter->m_Tiles.ChunkId(0, 0), pBefore->m_Tiles.ChunkId(0, 0));
	EXPECT_EQ(pAfter->m_Tiles.ChunkId(3, 0), pFarBlock);
}

// The numbers beside a physics tile belong to the brush: a number is chosen
// and then tiles are put down with it. What is air keeps none of them, or the
// file would carry a tile that does nothing with a number beside it.
TEST(Edit, TheNumbersGoOnEveryTileOfTheBrushThatIsNotAir)
{
	CBrush Brush(ETileLayerKind::TELE, 2, 1);
	CTileStore<CTeleTile> &Tele = std::get<CTileStore<CTeleTile>>(Brush.m_ExtraTiles);
	Tele.Set(0, 0, TeleTile(TILE_TELEIN, 0));
	// The second one stays air.

	CBrushNumbers Numbers;
	Numbers.m_Number = 42;
	SetBrushNumbers(Brush, Numbers);

	EXPECT_EQ(Tele.Get(0, 0).m_Number, 42);
	EXPECT_EQ(Tele.Get(1, 0).m_Type, 0);
	EXPECT_EQ(Tele.Get(1, 0).m_Number, 0);
}

TEST(Edit, ASwitchCarriesItsDelayAndASpeedupItsForce)
{
	CBrush Switch(ETileLayerKind::SWITCH, 1, 1);
	CSwitchTile Tile = {};
	Tile.m_Type = TILE_SWITCHOPEN;
	std::get<CTileStore<CSwitchTile>>(Switch.m_ExtraTiles).Set(0, 0, Tile);
	CBrushNumbers Numbers;
	Numbers.m_Number = 7;
	Numbers.m_Delay = 3;
	SetBrushNumbers(Switch, Numbers);
	EXPECT_EQ(std::get<CTileStore<CSwitchTile>>(Switch.m_ExtraTiles).Get(0, 0).m_Number, 7);
	EXPECT_EQ(std::get<CTileStore<CSwitchTile>>(Switch.m_ExtraTiles).Get(0, 0).m_Delay, 3);

	CBrush Speedup(ETileLayerKind::SPEEDUP, 1, 1);
	CSpeedupTile Push = {};
	Push.m_Type = TILE_SPEED_BOOST;
	std::get<CTileStore<CSpeedupTile>>(Speedup.m_ExtraTiles).Set(0, 0, Push);
	CBrushNumbers Pushing;
	Pushing.m_Force = 20;
	Pushing.m_MaxSpeed = 30;
	// More than a turn round is the same way round.
	Pushing.m_Angle = 450;
	SetBrushNumbers(Speedup, Pushing);
	const CSpeedupTile Written = std::get<CTileStore<CSpeedupTile>>(Speedup.m_ExtraTiles).Get(0, 0);
	EXPECT_EQ(Written.m_Force, 20);
	EXPECT_EQ(Written.m_MaxSpeed, 30);
	EXPECT_EQ(Written.m_Angle, 90);
}

TEST(Edit, WhatABrushCarriesCanBeReadBackOffIt)
{
	CBrush Brush(ETileLayerKind::TELE, 2, 1);
	CTileStore<CTeleTile> &Tele = std::get<CTileStore<CTeleTile>>(Brush.m_ExtraTiles);
	// Air first, so that reading has to walk past it.
	Tele.Set(1, 0, TeleTile(TILE_TELEOUT, 12));
	EXPECT_EQ(BrushNumbers(Brush).m_Number, 12);

	// A brush of a kind that has no numbers carries none, and is not changed
	// by being given some.
	CBrush Plain = TileBrush(2, 2, 5);
	const CBrush Was = Plain;
	CBrushNumbers Numbers;
	Numbers.m_Number = 9;
	SetBrushNumbers(Plain, Numbers);
	EXPECT_EQ(Plain, Was);
	EXPECT_EQ(BrushNumbers(Plain).m_Number, 0);
}

TEST(Edit, TheNextFreeNumberSkipsWhatIsUsed)
{
	CTileLayer Layer(ETileLayerKind::TELE, 8, 4);
	CTileStore<CTeleTile> &Tele = std::get<CTileStore<CTeleTile>>(Layer.m_ExtraTiles);
	Tele.Set(0, 0, TeleTile(TILE_TELEIN, 1));
	Tele.Set(1, 0, TeleTile(TILE_TELEOUT, 2));
	EXPECT_EQ(NextFreeNumber(Layer), 3);

	// The checkpoints keep their own count: two is free among them even
	// though a teleporter is using it.
	Tele.Set(2, 0, TeleTile(TILE_TELECHECK, 1));
	EXPECT_EQ(NextFreeNumber(Layer, true), 2);
	EXPECT_EQ(NextFreeNumber(Layer, false), 3);

	// A layer that has no numbers at all starts at one.
	CTileLayer Plain(ETileLayerKind::TILES, 4, 4);
	EXPECT_EQ(NextFreeNumber(Plain), 1);
}

TEST(Edit, WhereANumberIsUsedComesOutOnePlacePerCluster)
{
	CTileLayer Layer(ETileLayerKind::TELE, 40, 8);
	CTileStore<CTeleTile> &Tele = std::get<CTileStore<CTeleTile>>(Layer.m_ExtraTiles);
	// Three tiles side by side are one teleporter, not three.
	Tele.Set(2, 1, TeleTile(TILE_TELEIN, 7));
	Tele.Set(3, 1, TeleTile(TILE_TELEIN, 7));
	Tele.Set(4, 1, TeleTile(TILE_TELEIN, 7));
	// Far enough away to be somewhere else.
	Tele.Set(30, 1, TeleTile(TILE_TELEOUT, 7));
	// And a different number is not this one.
	Tele.Set(20, 5, TeleTile(TILE_TELEIN, 8));

	const std::vector<ivec2> vPlaces = NumberPlaces(Layer, 7);
	ASSERT_EQ(vPlaces.size(), 2);
	EXPECT_EQ(vPlaces[0], ivec2(2, 1));
	EXPECT_EQ(vPlaces[1], ivec2(30, 1));

	EXPECT_TRUE(NumberPlaces(Layer, 9).empty());
	// Zero is no number rather than a number nothing uses.
	EXPECT_TRUE(NumberPlaces(Layer, 0).empty());
}
