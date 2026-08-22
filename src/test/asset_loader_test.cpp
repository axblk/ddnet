#include "test.h"

#include <base/io.h>
#include <base/thread.h>

#include <engine/client/asset_loader.h>
#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
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

		void Run() override
		{
			const int Running = m_Running.fetch_add(1) + 1;
			int MaxRunning = m_MaxRunning.load();
			while(Running > MaxRunning && !m_MaxRunning.compare_exchange_weak(MaxRunning, Running))
			{
			}
			while(!m_Release.load() && State() != STATE_ABORTED)
				thread_yield();
			m_Running.fetch_sub(1);
		}

	public:
		CBlockingAssetJob(int OwnerId, uint64_t Generation, std::atomic<int> &Running, std::atomic<int> &MaxRunning, std::atomic<bool> &Release) :
			CAssetJob(EAssetType::SOUND, "test", OwnerId, Generation),
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
}

TEST(AssetLoader, LimitsConcurrencyAndPublishesMetadata)
{
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 2);
	std::atomic<int> Running{0};
	std::atomic<int> MaxRunning{0};
	std::atomic<bool> Release{false};
	std::vector<CTypedAssetResource<CBlockingAssetJob>> vResources;
	for(int i = 0; i < 4; ++i)
	{
		auto pJob = std::make_shared<CBlockingAssetJob>(7, 11, Running, MaxRunning, Release);
		auto Resource = Loader.Load(std::move(pJob));
		EXPECT_EQ(Resource.RequestId(), static_cast<uint64_t>(i) + 1);
		vResources.push_back(std::move(Resource));
	}

	// The high water mark is written after the counter it is taken from, so
	// waiting for the counter can see two jobs before either wrote the mark.
	WaitForRunningJobs(Loader, MaxRunning, 2);
	EXPECT_EQ(Running.load(), 2);
	EXPECT_EQ(MaxRunning.load(), 2);
	EXPECT_EQ(Loader.RunningCount(), 2U);
	EXPECT_EQ(Loader.PendingCount(), 2U);
	EXPECT_EQ(vResources[0].Type(), EAssetType::SOUND);
	EXPECT_STREQ(vResources[0].Path(), "test");
	EXPECT_EQ(vResources[0].OwnerId(), 7);
	EXPECT_EQ(vResources[0].Generation(), 11U);

	Release = true;
	for(int i = 0; i < 100000 && !Loader.Idle(); ++i)
	{
		Loader.Update();
		thread_yield();
	}
	EXPECT_TRUE(Loader.Idle());
	EXPECT_LE(MaxRunning.load(), 2);
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}

TEST(AssetLoader, AbortsStaleGenerations)
{
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	std::atomic<int> Running{0};
	std::atomic<int> MaxRunning{0};
	std::atomic<bool> Release{false};
	auto pRunningJob = std::make_shared<CBlockingAssetJob>(1, 1, Running, MaxRunning, Release);
	auto pOldJob = std::make_shared<CBlockingAssetJob>(3, 4, Running, MaxRunning, Release);
	auto pCurrentJob = std::make_shared<CBlockingAssetJob>(3, 5, Running, MaxRunning, Release);
	auto RunningResource = Loader.Load(pRunningJob);
	auto OldResource = Loader.Load(pOldJob);
	auto CurrentResource = Loader.Load(pCurrentJob);
	WaitForRunningJobs(Loader, Running, 1);
	EXPECT_FALSE(pRunningJob->Abort());
	EXPECT_EQ(pRunningJob->State(), IJob::STATE_RUNNING);
	Loader.AbortOwnerBeforeGeneration(3, 5);
	EXPECT_FALSE(RunningResource.IsFinished());
	EXPECT_TRUE(OldResource.IsFinished());
	EXPECT_FALSE(CurrentResource.IsFinished());
	EXPECT_EQ(pOldJob->State(), IJob::STATE_ABORTED);
	EXPECT_EQ(pCurrentJob->State(), IJob::STATE_QUEUED);

	Release = true;
	for(int i = 0; i < 100000 && !Loader.Idle(); ++i)
	{
		Loader.Update();
		thread_yield();
	}
	EXPECT_TRUE(Loader.Idle());
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
	auto pJob = std::make_shared<CBlockingAssetJob>(2, 3, Running, MaxRunning, Release);
	auto Resource = Loader.Load(pJob);
	ASSERT_EQ(pJob->State(), IJob::STATE_QUEUED);
	Loader.AbortOwnerBeforeGeneration(2, 4);
	EXPECT_TRUE(Resource.IsFinished());
	EXPECT_EQ(pJob->State(), IJob::STATE_ABORTED);
	EXPECT_TRUE(Loader.Idle());
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
	CImageResource Resource = Loader.LoadImage(std::move(vPng), "memory.png", 4, 7, [](CImageInfo &Image) {
		Image.m_pData[0] = 42;
		return true;
	});
	EXPECT_STREQ(Resource.Path(), "memory.png");
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady(7));
	EXPECT_FALSE(Resource.IsFailed(7));
	EXPECT_TRUE(Resource.IsStale(8));
	CImageInfo Result = Resource.TakeImage();
	EXPECT_EQ(Result.m_pData[0], 42);
	Resource.Reset();
	EXPECT_FALSE(Resource);

	Loader.Update();
	EXPECT_TRUE(Loader.Idle());
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}
