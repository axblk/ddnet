#include <engine/client/backend/gpu_timestamp.h>

#include <gtest/gtest.h>

#include <vector>

namespace
{
	constexpr IGraphics::CGpuRenderZone ZoneA(0);
	constexpr IGraphics::CGpuRenderZone ZoneB(1);
}

TEST(GpuTimestampZones, IntervalsFollowTheFrameQueries)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(ZoneA, Query));
	EXPECT_EQ(Query, 2u);
	ASSERT_TRUE(Zones.End(ZoneA, Query));
	EXPECT_EQ(Query, 3u);
	ASSERT_TRUE(Zones.Begin(ZoneB, Query));
	EXPECT_EQ(Query, 4u);
	ASSERT_TRUE(Zones.End(ZoneB, Query));
	EXPECT_EQ(Query, 5u);
	EXPECT_EQ(Zones.IntervalCount(), 2u);
	EXPECT_EQ(Zones.WrittenQueryCount(), 6u);
	EXPECT_EQ(Zones.ZoneMask(), (1U << 0) | (1U << 1));
}

TEST(GpuTimestampZones, ANestedZoneKeepsItsOwnInterval)
{
	CGpuTimestampZones Zones;
	uint32_t Outer = 0;
	uint32_t Inner = 0;
	ASSERT_TRUE(Zones.Begin(ZoneA, Outer));
	ASSERT_TRUE(Zones.Begin(ZoneB, Inner));
	EXPECT_EQ(Outer, 2u);
	EXPECT_EQ(Inner, 4u);
	ASSERT_TRUE(Zones.End(ZoneB, Inner));
	EXPECT_EQ(Inner, 5u);
	ASSERT_TRUE(Zones.End(ZoneA, Outer));
	EXPECT_EQ(Outer, 3u);
}

TEST(GpuTimestampZones, AZoneOpenedTwiceIsIgnoredUntilItCloses)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(ZoneA, Query));
	EXPECT_FALSE(Zones.Begin(ZoneA, Query));
	ASSERT_TRUE(Zones.End(ZoneA, Query));
	EXPECT_FALSE(Zones.End(ZoneA, Query));
	EXPECT_EQ(Zones.IntervalCount(), 1u);
}

TEST(GpuTimestampZones, AFrameThatRunsOutOfIntervalsDropsTheZone)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	for(uint32_t Interval = 0; Interval < GPU_TIMESTAMP_MAX_INTERVALS; ++Interval)
	{
		ASSERT_TRUE(Zones.Begin(ZoneA, Query));
		ASSERT_TRUE(Zones.End(ZoneA, Query));
	}
	EXPECT_FALSE(Zones.Begin(ZoneA, Query));
	EXPECT_EQ(Zones.IntervalCount(), GPU_TIMESTAMP_MAX_INTERVALS);
	EXPECT_EQ(Zones.ZoneMask(), 0u) << "a zone that lost an interval must not be reported";
}

TEST(GpuTimestampZones, AZoneLeftOpenIsClosedButNotReported)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(ZoneA, Query));
	ASSERT_TRUE(Zones.Begin(ZoneB, Query));
	ASSERT_TRUE(Zones.End(ZoneB, Query));
	std::vector<uint32_t> vClosing;
	Zones.CloseOpenZones([&](uint32_t ClosingQuery) { vClosing.push_back(ClosingQuery); });
	EXPECT_EQ(vClosing, std::vector<uint32_t>{3u});
	EXPECT_EQ(Zones.ZoneMask(), 1U << 1);
	vClosing.clear();
	Zones.CloseOpenZones([&](uint32_t ClosingQuery) { vClosing.push_back(ClosingQuery); });
	EXPECT_TRUE(vClosing.empty()) << "a zone is closed only once";
}

TEST(GpuTimestampZones, NoZoneIsNotTimed)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	EXPECT_FALSE(Zones.Begin(IGraphics::CGpuRenderZone(), Query));
	EXPECT_FALSE(Zones.Begin(IGraphics::CGpuRenderZone(IGraphics::MAX_GPU_RENDER_ZONES), Query));
	EXPECT_EQ(Zones.IntervalCount(), 0u);
}

TEST(GpuTimestampZones, ResetStartsTheNextFrameClean)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(ZoneA, Query));
	Zones.Reset();
	EXPECT_EQ(Zones.IntervalCount(), 0u);
	EXPECT_EQ(Zones.ZoneMask(), 0u);
	ASSERT_TRUE(Zones.Begin(ZoneA, Query));
	EXPECT_EQ(Query, 2u);
}
