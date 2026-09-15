#include <game/map/document/tile_store.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

using namespace map_document;

// The tiles of a layer are held so that a version of the map costs what
// changed and not what it holds, so what is tested here is mostly what is
// *shared*: that a copy is a copy of nothing, and that writing to one of two
// versions leaves the other one alone.

namespace
{
	CTile Tile(int Index, int Flags = 0)
	{
		CTile Result = {};
		Result.m_Index = (unsigned char)Index;
		Result.m_Flags = (unsigned char)Flags;
		return Result;
	}

	bool operator==(const CTile &One, const CTile &Other)
	{
		return One.m_Index == Other.m_Index && One.m_Flags == Other.m_Flags && One.m_Skip == Other.m_Skip && One.m_MustBe0 == Other.m_MustBe0;
	}
} // namespace

TEST(TileStore, EmptyIsAir)
{
	const CTileStore<CTile> Store(100, 50);
	EXPECT_EQ(Store.Width(), 100);
	EXPECT_EQ(Store.Height(), 50);
	EXPECT_EQ(Store.ChunksAcross(), 2);
	EXPECT_EQ(Store.ChunksDown(), 1);
	EXPECT_EQ(Store.UsedChunks(), 0);
	EXPECT_EQ(Store.Bytes(), 0u);
	EXPECT_TRUE(Store.Get(0, 0) == Tile(0));
	EXPECT_EQ(Store.Chunk(0, 0), nullptr);
}

TEST(TileStore, OutsideIsAirAndSwallowsWrites)
{
	CTileStore<CTile> Store(10, 10);
	Store.Set(-1, 0, Tile(1));
	Store.Set(0, -1, Tile(1));
	Store.Set(10, 0, Tile(1));
	Store.Set(0, 10, Tile(1));
	EXPECT_EQ(Store.UsedChunks(), 0);
	EXPECT_TRUE(Store.Get(-1, 0) == Tile(0));
	EXPECT_TRUE(Store.Get(10, 10) == Tile(0));
}

TEST(TileStore, WritesAndReads)
{
	CTileStore<CTile> Store(200, 200);
	Store.Set(0, 0, Tile(1));
	Store.Set(63, 63, Tile(2));
	Store.Set(64, 64, Tile(3, TILEFLAG_XFLIP));
	Store.Set(199, 199, Tile(4));
	EXPECT_TRUE(Store.Get(0, 0) == Tile(1));
	EXPECT_TRUE(Store.Get(63, 63) == Tile(2));
	EXPECT_TRUE(Store.Get(64, 64) == Tile(3, TILEFLAG_XFLIP));
	EXPECT_TRUE(Store.Get(199, 199) == Tile(4));
	EXPECT_TRUE(Store.Get(1, 1) == Tile(0));
	// Four tiles in three different blocks of 64.
	EXPECT_EQ(Store.UsedChunks(), 3);
}

TEST(TileStore, AirCostsNoBlock)
{
	CTileStore<CTile> Store(64, 64);
	Store.Set(10, 10, Tile(0));
	EXPECT_EQ(Store.UsedChunks(), 0);
	// Writing a tile and taking it away again leaves the block behind, which
	// is the price of not looking at the whole block after every tile.
	Store.Set(10, 10, Tile(1));
	Store.Set(10, 10, Tile(0));
	EXPECT_EQ(Store.UsedChunks(), 1);
	EXPECT_TRUE(Store.Get(10, 10) == Tile(0));
}

TEST(TileStore, CopyShares)
{
	CTileStore<CTile> First(256, 256);
	for(int i = 0; i < 4; ++i)
		First.Set(i * 64, 0, Tile(1 + i));
	EXPECT_EQ(First.UsedChunks(), 4);

	const CTileStore<CTile> Second = First;
	for(int i = 0; i < 4; ++i)
		EXPECT_EQ(First.ChunkId(i, 0), Second.ChunkId(i, 0));

	// One tile in one block: that block parts company, the other three do not.
	First.Set(0, 0, Tile(9));
	EXPECT_NE(First.ChunkId(0, 0), Second.ChunkId(0, 0));
	for(int i = 1; i < 4; ++i)
		EXPECT_EQ(First.ChunkId(i, 0), Second.ChunkId(i, 0));
	EXPECT_TRUE(First.Get(0, 0) == Tile(9));
	EXPECT_TRUE(Second.Get(0, 0) == Tile(1));
}

TEST(TileStore, WritingTwiceTakesTheBlockApartOnce)
{
	CTileStore<CTile> First(128, 128);
	First.Set(0, 0, Tile(1));
	const CTileStore<CTile> Second = First;
	First.Set(1, 0, Tile(2));
	const void *pAfterFirstWrite = First.ChunkId(0, 0);
	First.Set(2, 0, Tile(3));
	EXPECT_EQ(First.ChunkId(0, 0), pAfterFirstWrite);
	EXPECT_NE(First.ChunkId(0, 0), Second.ChunkId(0, 0));
}

TEST(TileStore, SameTileIsNoChange)
{
	CTileStore<CTile> First(64, 64);
	First.Set(0, 0, Tile(1));
	const CTileStore<CTile> Second = First;
	First.Set(0, 0, Tile(1));
	EXPECT_EQ(First.ChunkId(0, 0), Second.ChunkId(0, 0));
}

TEST(TileStore, Equality)
{
	CTileStore<CTile> First(128, 64);
	CTileStore<CTile> Second(128, 64);
	EXPECT_TRUE(First == Second);
	First.Set(5, 5, Tile(1));
	EXPECT_TRUE(First != Second);
	Second.Set(5, 5, Tile(1));
	EXPECT_TRUE(First == Second);
	// A block that was written to and emptied again still equals no block.
	First.Set(70, 5, Tile(1));
	First.Set(70, 5, Tile(0));
	EXPECT_EQ(First.UsedChunks(), 2);
	EXPECT_EQ(Second.UsedChunks(), 1);
	EXPECT_TRUE(First == Second);
	const CTileStore<CTile> Other(64, 64);
	EXPECT_TRUE(First != Other);
}

TEST(TileStore, ResetEmpties)
{
	CTileStore<CTile> Store(64, 64);
	Store.Set(0, 0, Tile(1));
	Store.Reset(32, 32);
	EXPECT_EQ(Store.Width(), 32);
	EXPECT_EQ(Store.UsedChunks(), 0);
	EXPECT_TRUE(Store.Get(0, 0) == Tile(0));
}

TEST(TileStore, ChunkIsReadStraightOut)
{
	CTileStore<CTile> Store(64, 64);
	Store.Set(3, 2, Tile(7));
	const CTile *pChunk = Store.Chunk(0, 0);
	ASSERT_NE(pChunk, nullptr);
	EXPECT_TRUE(pChunk[2 * CTileStore<CTile>::CHUNK_SIZE + 3] == Tile(7));
	EXPECT_TRUE(pChunk[0] == Tile(0));
}

TEST(TileStore, OtherTileTypes)
{
	CTileStore<CTeleTile> Store(64, 64);
	CTeleTile Tele = {};
	Tele.m_Number = 5;
	Tele.m_Type = TILE_TELEIN;
	Store.Set(1, 1, Tele);
	EXPECT_EQ(Store.Get(1, 1).m_Number, 5);
	EXPECT_EQ(Store.Get(0, 0).m_Number, 0);
	EXPECT_EQ(Store.UsedChunks(), 1);
}

TEST(TileStore, AVersionCostsTheBlockItPaintedAndNotTheMap)
{
	// Big enough that the list of blocks runs over several pages - the whole
	// point of paging it is that a version does not carry a copy of the list.
	constexpr int CHUNK = CTileStore<CTile>::CHUNK_SIZE;
	constexpr int ACROSS = 4 * CTileStore<CTile>::CHUNKS_PER_PAGE;
	CTileStore<CTile> First(ACROSS * CHUNK, CHUNK);
	for(int i = 0; i < ACROSS; ++i)
	{
		First.Set(i * CHUNK, 0, Tile(1));
	}
	ASSERT_EQ(First.UsedChunks(), ACROSS);

	CTileStore<CTile> Second = First;
	Second.Set(0, 0, Tile(2));

	std::unordered_set<const void *> Seen;
	const uint64_t Both = First.BytesOnce(Seen) + Second.BytesOnce(Seen);
	const uint64_t Block = (uint64_t)CTileStore<CTile>::TILES_PER_CHUNK * sizeof(CTile);
	// The second version is one block, one page and one list of pages more
	// than the first - a few kilobytes on top of the block, and nothing that
	// grows with how big the layer is.
	EXPECT_GT(Both, First.Bytes() + Block);
	EXPECT_LT(Both, First.Bytes() + Block + 8 * 1024);
	// All the other blocks are still the same blocks.
	for(int i = 1; i < ACROSS; ++i)
	{
		EXPECT_EQ(First.ChunkId(i, 0), Second.ChunkId(i, 0));
	}
	EXPECT_NE(First.ChunkId(0, 0), Second.ChunkId(0, 0));
}
