#include "test.h"

#include <base/thread.h>

#include <engine/client/asset_loader.h>
#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

namespace
{
	class CBlockingAssetJob final : public CAssetJob
	{
		std::atomic<int> &m_Running;
		std::atomic<int> &m_MaxRunning;
		std::atomic<bool> &m_Release;

		bool Process() override
		{
			const int Running = m_Running.fetch_add(1) + 1;
			int MaxRunning = m_MaxRunning.load();
			while(Running > MaxRunning && !m_MaxRunning.compare_exchange_weak(MaxRunning, Running))
			{
			}
			while(!m_Release.load() && State() != STATE_ABORTED)
				thread_yield();
			m_Running.fetch_sub(1);
			return true;
		}

	public:
		CBlockingAssetJob(std::atomic<int> &Running, std::atomic<int> &MaxRunning, std::atomic<bool> &Release) :
			CAssetJob(std::vector<uint8_t>(), "test"),
			m_Running(Running),
			m_MaxRunning(MaxRunning),
			m_Release(Release)
		{
		}
	};

	class CQueuedTestEngine final : public IEngine
	{
	public:
		std::vector<std::shared_ptr<IJob>> m_vpJobs;

		void Init() override {}
		void AddJob(std::shared_ptr<IJob> pJob) override { m_vpJobs.push_back(std::move(pJob)); }
		size_t JobThreadCount() const override { return 1; }
		void ShutdownJobs() override {}
		void SetAdditionalLogger(std::shared_ptr<ILogger> &&pLogger) override { (void)pLogger; }
	};

	void WaitForRunningJobs(CAssetLoader &Loader, const std::atomic<int> &Running, int Expected)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while(Running.load() != Expected && std::chrono::steady_clock::now() < Deadline)
		{
			Loader.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	void WaitForResource(CAssetLoader &Loader, const CAssetResource &Resource)
	{
		const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while(!Resource.IsFinished() && std::chrono::steady_clock::now() < Deadline)
		{
			Loader.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	template<typename TResource>
	void WaitForResources(CAssetLoader &Loader, const std::vector<TResource> &vResources)
	{
		for(const auto &Resource : vResources)
			WaitForResource(Loader, Resource);
	}

}

TEST(AssetLoader, LimitsConcurrency)
{
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 2);
	std::atomic<int> Running{0};
	std::atomic<int> MaxRunning{0};
	std::atomic<bool> Release{false};
	std::vector<CTypedAssetResource<CBlockingAssetJob>> vResources;
	vResources.reserve(4);
	for(int i = 0; i < 4; ++i)
		vResources.push_back(Loader.Load(std::make_shared<CBlockingAssetJob>(Running, MaxRunning, Release)));
	EXPECT_STREQ(vResources[0].Path(), "test");

	// The maximum is written after the counter it is taken from
	WaitForRunningJobs(Loader, MaxRunning, 2);
	EXPECT_EQ(Running.load(), 2);
	EXPECT_EQ(MaxRunning.load(), 2);

	Release = true;
	WaitForResources(Loader, vResources);
	for(const auto &Resource : vResources)
		EXPECT_TRUE(Resource.IsReady());
	EXPECT_EQ(MaxRunning.load(), 2);
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}

TEST(AssetLoader, DroppingTheHandleAbortsTheLoad)
{
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	std::atomic<int> Running{0};
	std::atomic<int> MaxRunning{0};
	std::atomic<bool> Release{false};
	auto pRunningJob = std::make_shared<CBlockingAssetJob>(Running, MaxRunning, Release);
	auto pDroppedJob = std::make_shared<CBlockingAssetJob>(Running, MaxRunning, Release);
	auto pKeptJob = std::make_shared<CBlockingAssetJob>(Running, MaxRunning, Release);
	auto RunningResource = Loader.Load(pRunningJob);
	auto DroppedResource = Loader.Load(pDroppedJob);
	auto KeptResource = Loader.Load(pKeptJob);
	WaitForRunningJobs(Loader, Running, 1);
	EXPECT_EQ(pRunningJob->State(), IJob::STATE_RUNNING);
	DroppedResource.Reset();
	EXPECT_EQ(pDroppedJob->State(), IJob::STATE_ABORTED);
	EXPECT_EQ(pKeptJob->State(), IJob::STATE_QUEUED);

	// Replacing the handle aborts a running load as well
	RunningResource = CTypedAssetResource<CBlockingAssetJob>();
	EXPECT_EQ(pRunningJob->State(), IJob::STATE_ABORTED);

	Release = true;
	WaitForResource(Loader, KeptResource);
	EXPECT_TRUE(KeptResource.IsReady());

	// A finished result goes with its handle
	KeptResource.Reset();
	EXPECT_EQ(pKeptJob->State(), IJob::STATE_DONE);
	Loader.Update();
	EXPECT_EQ(pKeptJob.use_count(), 1);
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}

TEST(AssetLoader, ReleasesAbortedEngineQueuedJob)
{
	CQueuedTestEngine Engine;
	CAssetLoader Loader;
	Loader.Init(&Engine, 1);
	std::atomic<int> Running{0};
	std::atomic<int> MaxRunning{0};
	std::atomic<bool> Release{false};
	auto pJob = std::make_shared<CBlockingAssetJob>(Running, MaxRunning, Release);
	auto Resource = Loader.Load(pJob);
	ASSERT_EQ(pJob->State(), IJob::STATE_QUEUED);
	Resource.Reset();
	EXPECT_EQ(pJob->State(), IJob::STATE_ABORTED);
	Loader.Shutdown();
}

TEST(AssetLoader, DecodesOwnedImageBytesAndPostprocesses)
{
	CImageInfo Source;
	Source.m_Width = 1;
	Source.m_Height = 1;
	Source.m_Format = CImageInfo::FORMAT_RGBA;
	ASSERT_TRUE(Source.TryAllocate());
	std::fill_n(Source.m_pData, Source.DataSize(), 0);
	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SavePng(Writer, Source));

	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	std::vector<uint8_t> vPng(Writer.Data(), Writer.Data() + Writer.Size());
	CImageResource Resource = Loader.LoadImageData(std::move(vPng), "memory.png", [](CImageInfo &Image) {
		Image.m_pData[0] = 42;
		return true;
	});
	EXPECT_STREQ(Resource.Path(), "memory.png");
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady());
	EXPECT_FALSE(Resource.IsFailed());
	CImageInfo Result = Resource.TakeImage();
	EXPECT_EQ(Result.m_pData[0], 42);
	Resource.Reset();
	EXPECT_FALSE(Resource);

	Loader.Shutdown();
	pEngine->ShutdownJobs();
}
