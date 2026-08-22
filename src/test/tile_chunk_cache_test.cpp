#include <game/map/tile_chunk_cache.h>

#include <gtest/gtest.h>

using CChunkRange = CTileChunkCache::CChunkRange;

static constexpr int CHUNK = CTileChunkCache::CHUNK_SIZE;

TEST(TileChunkCache, ChunkRangeCoversTheTilesItIsGiven)
{
	// One tile is one chunk, and the last tile of a chunk still belongs to it.
	const CChunkRange Single = CTileChunkCache::ChunkRange(0, 0, 1, 1, 4 * CHUNK, 4 * CHUNK);
	EXPECT_EQ(Single.m_FirstX, 0);
	EXPECT_EQ(Single.m_LastX, 0);
	EXPECT_EQ(Single.m_FirstY, 0);
	EXPECT_EQ(Single.m_LastY, 0);

	const CChunkRange LastOfChunk = CTileChunkCache::ChunkRange(CHUNK - 1, CHUNK - 1, 1, 1, 4 * CHUNK, 4 * CHUNK);
	EXPECT_EQ(LastOfChunk.m_FirstX, 0);
	EXPECT_EQ(LastOfChunk.m_LastX, 0);

	const CChunkRange FirstOfNext = CTileChunkCache::ChunkRange(CHUNK, CHUNK, 1, 1, 4 * CHUNK, 4 * CHUNK);
	EXPECT_EQ(FirstOfNext.m_FirstX, 1);
	EXPECT_EQ(FirstOfNext.m_LastX, 1);

	// A rectangle that ends on a chunk border must not pull in the chunk behind it.
	const CChunkRange UpToBorder = CTileChunkCache::ChunkRange(0, 0, CHUNK, CHUNK, 4 * CHUNK, 4 * CHUNK);
	EXPECT_EQ(UpToBorder.m_LastX, 0);
	EXPECT_EQ(UpToBorder.m_LastY, 0);

	const CChunkRange OverBorder = CTileChunkCache::ChunkRange(0, 0, CHUNK + 1, CHUNK + 1, 4 * CHUNK, 4 * CHUNK);
	EXPECT_EQ(OverBorder.m_LastX, 1);
	EXPECT_EQ(OverBorder.m_LastY, 1);
}

TEST(TileChunkCache, ChunkRangeClampsToTheLayer)
{
	// A layer that is not a multiple of the chunk size still has its last chunk.
	const CChunkRange Whole = CTileChunkCache::ChunkRange(0, 0, 100000, 100000, CHUNK + 1, CHUNK + 1);
	EXPECT_EQ(Whole.m_FirstX, 0);
	EXPECT_EQ(Whole.m_LastX, 1);
	EXPECT_EQ(Whole.m_FirstY, 0);
	EXPECT_EQ(Whole.m_LastY, 1);

	// Negative positions are the editor moving a selection off the layer.
	const CChunkRange Negative = CTileChunkCache::ChunkRange(-10, -10, 12, 12, 4 * CHUNK, 4 * CHUNK);
	EXPECT_FALSE(Negative.IsEmpty());
	EXPECT_EQ(Negative.m_FirstX, 0);
	EXPECT_EQ(Negative.m_LastX, 0);
}

TEST(TileChunkCache, ChunkRangeIsEmptyOutsideTheLayer)
{
	EXPECT_TRUE(CTileChunkCache::ChunkRange(-100, 0, 50, 50, 4 * CHUNK, 4 * CHUNK).IsEmpty());
	EXPECT_TRUE(CTileChunkCache::ChunkRange(4 * CHUNK, 0, 50, 50, 4 * CHUNK, 4 * CHUNK).IsEmpty());
	EXPECT_TRUE(CTileChunkCache::ChunkRange(0, 0, 0, 10, 4 * CHUNK, 4 * CHUNK).IsEmpty());
	EXPECT_TRUE(CTileChunkCache::ChunkRange(0, 0, 10, -5, 4 * CHUNK, 4 * CHUNK).IsEmpty());
	EXPECT_TRUE(CTileChunkCache::ChunkRange(0, 0, 10, 10, 0, 0).IsEmpty());
}
