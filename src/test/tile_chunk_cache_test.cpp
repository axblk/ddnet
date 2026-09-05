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

TEST(TileChunkCache, EvictionKeepsNothingWhenUnderBudget)
{
	using CUsage = CTileChunkCache::CChunkUsage;
	const std::vector<CUsage> vUsage = {{1024, 1}, {1024, 2}};
	EXPECT_TRUE(CTileChunkCache::ChunksToEvict(vUsage, 2048, 3).empty());
	// Exactly at the budget is not over it.
	EXPECT_TRUE(CTileChunkCache::ChunksToEvict(vUsage, CTileChunkCache::MEMORY_BUDGET, 3).empty());
}

TEST(TileChunkCache, EvictionGivesUpTheLeastRecentlyDrawnFirst)
{
	using CUsage = CTileChunkCache::CChunkUsage;
	const uint64_t Budget = CTileChunkCache::MEMORY_BUDGET;
	// Three chunks of a quarter budget each, none of them drawn in this render.
	const uint64_t Quarter = Budget / 4;
	const std::vector<CUsage> vUsage = {{Quarter, 30}, {Quarter, 10}, {Quarter, 20}};
	// Just over: the oldest alone brings the total below the target.
	const std::vector<size_t> vOne = CTileChunkCache::ChunksToEvict(vUsage, Budget + 1, 40);
	ASSERT_EQ(vOne.size(), 1u);
	EXPECT_EQ(vOne[0], 1u);
	// A quarter over: the oldest brings it to the budget, which is still above
	// the target, so the second oldest goes too - oldest first.
	const std::vector<size_t> vTwo = CTileChunkCache::ChunksToEvict(vUsage, Budget + Quarter, 40);
	ASSERT_EQ(vTwo.size(), 2u);
	EXPECT_EQ(vTwo[0], 1u);
	EXPECT_EQ(vTwo[1], 2u);
}

TEST(TileChunkCache, EvictionStopsAtTheTargetBelowTheBudget)
{
	using CUsage = CTileChunkCache::CChunkUsage;
	const uint64_t Budget = CTileChunkCache::MEMORY_BUDGET;
	const uint64_t Target = CTileChunkCache::EVICTION_TARGET;
	EXPECT_LT(Target, Budget);
	// Many small chunks: exactly as many go as it takes to reach the target,
	// not the budget, and not one more.
	const uint64_t Small = (Budget - Target) / 4;
	std::vector<CUsage> vUsage;
	for(uint64_t Tick = 1; Tick <= 16; ++Tick)
		vUsage.push_back({Small, Tick});
	const uint64_t Total = Budget + Small;
	const std::vector<size_t> vEvict = CTileChunkCache::ChunksToEvict(vUsage, Total, 100);
	// Budget + Small down to Target is five Smalls.
	ASSERT_EQ(vEvict.size(), 5u);
	for(size_t Index = 0; Index < vEvict.size(); ++Index)
		EXPECT_EQ(vEvict[Index], Index);
}

TEST(TileChunkCache, EvictionKeepsWhatIsOnScreenAndChunksWithoutABuffer)
{
	using CUsage = CTileChunkCache::CChunkUsage;
	const uint64_t Budget = CTileChunkCache::MEMORY_BUDGET;
	// Everything this cache holds is being drawn right now, so there is nothing
	// it can give up however far over the budget the total is.
	const std::vector<CUsage> vVisible = {{Budget, 7}, {Budget, 7}};
	EXPECT_TRUE(CTileChunkCache::ChunksToEvict(vVisible, 4 * Budget, 7).empty());
	// A chunk that holds no buffer costs nothing and is not a candidate.
	const std::vector<CUsage> vEmpty = {{0, 1}, {Budget, 2}, {0, 3}};
	const std::vector<size_t> vEvict = CTileChunkCache::ChunksToEvict(vEmpty, 4 * Budget, 9);
	ASSERT_EQ(vEvict.size(), 1u);
	EXPECT_EQ(vEvict[0], 1u);
}
