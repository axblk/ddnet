#include <game/map/document/map_state.h>

#include <gtest/gtest.h>

// A version of the map has to cost what changed and not what the map holds,
// so what is tested here is what two versions have in *common*: the groups,
// the layers and the blocks of tiles that an edit did not touch have to be
// the same nodes afterwards, not copies of them.

namespace
{
	CTile Tile(int Index)
	{
		CTile Result = {};
		Result.m_Index = (unsigned char)Index;
		return Result;
	}

	// Three groups of three layers, each layer four blocks wide and two down,
	// so that there is something around every edit to be left alone.
	CMapState ThreeByThree()
	{
		CMapState State;
		for(int g = 0; g < 3; ++g)
		{
			CGroup Group;
			Group.m_Name = "group";
			for(int l = 0; l < 3; ++l)
			{
				CTileLayer Layer(ETileLayerKind::TILES, 4 * CTileStore<CTile>::CHUNK_SIZE, 2 * CTileStore<CTile>::CHUNK_SIZE);
				Layer.m_Name = "layer";
				// One tile in every block, so that no block is missing merely
				// because it is air.
				for(int cy = 0; cy < Layer.m_Tiles.ChunksDown(); ++cy)
				{
					for(int cx = 0; cx < Layer.m_Tiles.ChunksAcross(); ++cx)
					{
						Layer.m_Tiles.Set(cx * CTileStore<CTile>::CHUNK_SIZE, cy * CTileStore<CTile>::CHUNK_SIZE, Tile(1));
					}
				}
				Group.m_vpLayers.push_back(std::make_shared<const CTileLayer>(std::move(Layer)));
			}
			State.AddGroup(std::move(Group));
		}
		return State;
	}
} // namespace

TEST(MapState, EmptyIsEmpty)
{
	const CMapState State;
	EXPECT_EQ(State.NumGroups(), 0u);
	EXPECT_EQ(State.Bytes(), 0u);
}

TEST(MapState, EditReplacesOneLayerAndOneGroup)
{
	const CMapState Before = ThreeByThree();
	CMapState After = Before;

	CTileLayer Changed = *After.Layer(1, 2);
	Changed.m_Tiles.Set(5, 5, Tile(7));
	After.ReplaceLayer(1, 2, std::move(Changed));

	EXPECT_EQ(Before.Layer(1, 2)->m_Tiles.Get(5, 5).m_Index, 0);
	EXPECT_EQ(After.Layer(1, 2)->m_Tiles.Get(5, 5).m_Index, 7);

	// The group that holds it and the layer itself are new nodes.
	EXPECT_NE(Before.Group(1), After.Group(1));
	EXPECT_NE(Before.Layer(1, 2), After.Layer(1, 2));

	// Nothing else is.
	EXPECT_EQ(Before.Group(0), After.Group(0));
	EXPECT_EQ(Before.Group(2), After.Group(2));
	EXPECT_EQ(Before.Layer(1, 0), After.Layer(1, 0));
	EXPECT_EQ(Before.Layer(1, 1), After.Layer(1, 1));
}

TEST(MapState, EditSharesEveryBlockItDidNotTouch)
{
	const CMapState Before = ThreeByThree();
	CMapState After = Before;

	CTileLayer Changed = *After.Layer(0, 0);
	Changed.m_Tiles.Set(5, 5, Tile(7));
	After.ReplaceLayer(0, 0, std::move(Changed));

	const CTileStore<CTile> &Old = Before.Layer(0, 0)->m_Tiles;
	const CTileStore<CTile> &New = After.Layer(0, 0)->m_Tiles;
	int Shared = 0, Taken = 0;
	for(int cy = 0; cy < Old.ChunksDown(); ++cy)
	{
		for(int cx = 0; cx < Old.ChunksAcross(); ++cx)
		{
			if(Old.ChunkId(cx, cy) == New.ChunkId(cx, cy))
				++Shared;
			else
				++Taken;
		}
	}
	EXPECT_EQ(Taken, 1);
	EXPECT_EQ(Shared, Old.ChunksAcross() * Old.ChunksDown() - 1);
}

TEST(MapState, ChangingAPropertySharesTheTiles)
{
	const CMapState Before = ThreeByThree();
	CMapState After = Before;

	CTileLayer Renamed = *After.Layer(2, 1);
	Renamed.m_Name = "renamed";
	After.ReplaceLayer(2, 1, std::move(Renamed));

	EXPECT_EQ(Before.Layer(2, 1)->m_Name, "layer");
	EXPECT_EQ(After.Layer(2, 1)->m_Name, "renamed");
	EXPECT_NE(Before.Layer(2, 1), After.Layer(2, 1));

	// A name is not a tile: every block came along.
	const CTileStore<CTile> &Old = Before.Layer(2, 1)->m_Tiles;
	const CTileStore<CTile> &New = After.Layer(2, 1)->m_Tiles;
	for(int cy = 0; cy < Old.ChunksDown(); ++cy)
	{
		for(int cx = 0; cx < Old.ChunksAcross(); ++cx)
		{
			EXPECT_EQ(Old.ChunkId(cx, cy), New.ChunkId(cx, cy));
		}
	}
}

TEST(MapState, ChangingAGroupSharesItsLayers)
{
	const CMapState Before = ThreeByThree();
	CMapState After = Before;

	CGroup Moved = *After.Group(0);
	Moved.m_OffsetX = 32;
	After.ReplaceGroup(0, std::move(Moved));

	EXPECT_EQ(Before.Group(0)->m_OffsetX, 0);
	EXPECT_EQ(After.Group(0)->m_OffsetX, 32);
	EXPECT_NE(Before.Group(0), After.Group(0));
	for(size_t l = 0; l < Before.NumLayers(0); ++l)
	{
		EXPECT_EQ(Before.Layer(0, l), After.Layer(0, l));
	}
}

TEST(MapState, PhysicsLayerCarriesItsSecondPlane)
{
	const CTileLayer Tiles(ETileLayerKind::TILES, 64, 64);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(Tiles.m_ExtraTiles));

	CTileLayer Switch(ETileLayerKind::SWITCH, 64, 64);
	ASSERT_TRUE(std::holds_alternative<CTileStore<CSwitchTile>>(Switch.m_ExtraTiles));
	auto &Numbers = std::get<CTileStore<CSwitchTile>>(Switch.m_ExtraTiles);
	EXPECT_EQ(Numbers.Width(), 64);

	CSwitchTile Number = {};
	Number.m_Number = 3;
	Numbers.Set(1, 1, Number);
	EXPECT_EQ(std::get<CTileStore<CSwitchTile>>(Switch.m_ExtraTiles).Get(1, 1).m_Number, 3);

	// Both planes count towards what the layer holds.
	Switch.m_Tiles.Set(1, 1, Tile(1));
	EXPECT_GT(Switch.m_Tiles.Bytes(), 64u * 64u * sizeof(CTile) - 1u);
	EXPECT_GT(Numbers.Bytes(), 64u * 64u * sizeof(CSwitchTile) - 1u);
	EXPECT_EQ(Switch.Bytes(), Switch.m_Tiles.Bytes() + Numbers.Bytes());
}

TEST(MapState, VersionCostsOneBlock)
{
	const CMapState Before = ThreeByThree();
	CMapState After = Before;

	CTileLayer Changed = *After.Layer(1, 1);
	Changed.m_Tiles.Set(5, 5, Tile(7));
	After.ReplaceLayer(1, 1, std::move(Changed));

	// Both versions hold the same amount, because they hold the same blocks
	// but for one - what the second version added is that one block.
	EXPECT_EQ(Before.Bytes(), After.Bytes());
	const uint64_t Block = (uint64_t)CTileStore<CTile>::TILES_PER_CHUNK * sizeof(CTile);
	EXPECT_EQ(After.Layer(1, 1)->Bytes(), Before.Layer(1, 1)->Bytes());
	EXPECT_GT(Before.Bytes(), Block);
}
