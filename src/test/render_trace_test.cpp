#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/str.h>

#include <engine/client/render_trace.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

TEST(RenderTrace, RecordsAndSaves)
{
	CTestInfo Info;
	const std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), "-trace.json");

	CRenderTrace Trace;
	ASSERT_TRUE(Trace.Start(pStorage.get(), 60, aFilename));
	Trace.BeginFrame();
	{
		CRenderTraceScope Scope(&Trace, "test/zone");
	}
	CRenderTrace::CFrame Frame;
	Frame.m_FrametimeNanoseconds = 8000000;
	Frame.m_Render.m_GpuTimingSupported = true;
	Frame.m_Render.m_GpuTimeNanoseconds = 250000;
	Frame.m_Render.m_aGpuRenderZoneNanoseconds[static_cast<size_t>(IGraphics::EGpuRenderZone::WORLD)] = 150000;
	Frame.m_Render.m_GpuRenderZoneMask = 1;
	Trace.RecordFrame(Frame);
	ASSERT_TRUE(Trace.Stop());
	EXPECT_FALSE(Trace.Enabled());

	IOHANDLE File = io_open(aFilename, IOFLAG_READ);
	ASSERT_NE(File, nullptr);
	char *pJson = io_read_all_str(File);
	io_close(File);
	ASSERT_NE(pJson, nullptr);
	EXPECT_NE(str_find(pJson, "\"format\": \"ddnet-render-trace\""), nullptr);
	EXPECT_NE(str_find(pJson, "\"test/zone\""), nullptr);
	EXPECT_NE(str_find(pJson, "\"gpu_time_ns\": 250000"), nullptr);
	EXPECT_NE(str_find(pJson, "\"gpu_world_ns\": 150000"), nullptr);
	EXPECT_NE(str_find(pJson, "\"gpu_zone_names\""), nullptr);
	EXPECT_NE(str_find(pJson, "\"map_background\""), nullptr);
	EXPECT_NE(str_find(pJson, "\"gpu_zones_ns\""), nullptr);
	EXPECT_NE(str_find(pJson, "\"gpu_zone_mask\": 1"), nullptr);
	free(pJson);
	EXPECT_FALSE(fs_remove(aFilename));
}
