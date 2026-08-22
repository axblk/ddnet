#include <engine/client/backend/gpu_timestamp.h>

#include <gtest/gtest.h>

#include <vector>

// The Vulkan and the WebGPU backend hand out timestamp queries through this,
// and a query written at the wrong offset shows up as another zone's time
// rather than as an error, so the offsets are checked here instead.

TEST(GpuTimestampZones, IntervalsFollowTheFrameQueries)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::WORLD, Query));
	EXPECT_EQ(Query, 2u);
	ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::WORLD, Query));
	EXPECT_EQ(Query, 3u);
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::HUD, Query));
	EXPECT_EQ(Query, 4u);
	ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::HUD, Query));
	EXPECT_EQ(Query, 5u);
	EXPECT_EQ(Zones.IntervalCount(), 2u);
	EXPECT_EQ(Zones.WrittenQueryCount(), 6u);
	EXPECT_EQ(Zones.ZoneMask(), (1U << static_cast<size_t>(IGraphics::EGpuRenderZone::WORLD)) | (1U << static_cast<size_t>(IGraphics::EGpuRenderZone::HUD)));
}

TEST(GpuTimestampZones, ANestedZoneKeepsItsOwnInterval)
{
	CGpuTimestampZones Zones;
	uint32_t Outer = 0;
	uint32_t Inner = 0;
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::WORLD, Outer));
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::PLAYERS, Inner));
	EXPECT_EQ(Outer, 2u);
	EXPECT_EQ(Inner, 4u);
	ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::PLAYERS, Inner));
	EXPECT_EQ(Inner, 5u);
	ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::WORLD, Outer));
	EXPECT_EQ(Outer, 3u);
}

TEST(GpuTimestampZones, AZoneOpenedTwiceIsIgnoredUntilItCloses)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::MENUS, Query));
	EXPECT_FALSE(Zones.Begin(IGraphics::EGpuRenderZone::MENUS, Query));
	ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::MENUS, Query));
	EXPECT_FALSE(Zones.End(IGraphics::EGpuRenderZone::MENUS, Query));
	EXPECT_EQ(Zones.IntervalCount(), 1u);
}

TEST(GpuTimestampZones, AFrameThatRunsOutOfIntervalsDropsTheZone)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	for(uint32_t Interval = 0; Interval < GPU_TIMESTAMP_MAX_INTERVALS; ++Interval)
	{
		ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::WORLD, Query));
		ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::WORLD, Query));
	}
	EXPECT_FALSE(Zones.Begin(IGraphics::EGpuRenderZone::WORLD, Query));
	EXPECT_EQ(Zones.IntervalCount(), GPU_TIMESTAMP_MAX_INTERVALS);
	EXPECT_EQ(Zones.ZoneMask(), 0u) << "a zone that lost an interval must not be reported";
}

TEST(GpuTimestampZones, AZoneLeftOpenIsClosedButNotReported)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::CHAT, Query));
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::CONSOLE, Query));
	ASSERT_TRUE(Zones.End(IGraphics::EGpuRenderZone::CONSOLE, Query));
	std::vector<uint32_t> vClosing;
	Zones.CloseOpenZones([&](uint32_t ClosingQuery) { vClosing.push_back(ClosingQuery); });
	EXPECT_EQ(vClosing, std::vector<uint32_t>{3u});
	EXPECT_EQ(Zones.ZoneMask(), 1U << static_cast<size_t>(IGraphics::EGpuRenderZone::CONSOLE));
	vClosing.clear();
	Zones.CloseOpenZones([&](uint32_t ClosingQuery) { vClosing.push_back(ClosingQuery); });
	EXPECT_TRUE(vClosing.empty()) << "a zone is closed only once";
}

TEST(GpuTimestampZones, ResetStartsTheNextFrameClean)
{
	CGpuTimestampZones Zones;
	uint32_t Query = 0;
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::WORLD, Query));
	Zones.Reset();
	EXPECT_EQ(Zones.IntervalCount(), 0u);
	EXPECT_EQ(Zones.ZoneMask(), 0u);
	ASSERT_TRUE(Zones.Begin(IGraphics::EGpuRenderZone::WORLD, Query));
	EXPECT_EQ(Query, 2u);
}
