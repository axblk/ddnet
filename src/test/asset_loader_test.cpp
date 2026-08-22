#include "test.h"

#include <base/io.h>
#include <base/thread.h>

#include <engine/client/asset_loader.h>
#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/http.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
	class CBlockingAssetJob final : public CAssetJob
	{
		std::atomic<int> &m_Running;
		std::atomic<int> &m_MaxRunning;
		std::atomic<bool> &m_Release;

		void Process() override
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
			CAssetJob(EAssetType::SOUND, std::vector<uint8_t>(), "test", OwnerId, Generation),
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

	class CTestHttpRequest final : public IHttpRequest
	{
	public:
		explicit CTestHttpRequest(const char *pUrl = "http://localhost/test.png") :
			IHttpRequest(pUrl)
		{
		}

		void Header(const char *pNameColonValue) override { (void)pNameColonValue; }

		void Finish(EHttpState State, int StatusCode, const std::vector<uint8_t> &vData)
		{
			m_StatusCode = StatusCode;
			if(!vData.empty())
			{
				EXPECT_EQ(OnData(reinterpret_cast<const char *>(vData.data()), vData.size()), vData.size());
			}
			OnCompletionInternal(State);
		}
	};

	class CTestHttp final : public IHttp
	{
	public:
		std::vector<std::shared_ptr<IHttpRequest>> m_vpRequests;

		void Run(std::shared_ptr<IHttpRequest> pRequest) override { m_vpRequests.push_back(std::move(pRequest)); }
		std::unique_ptr<IHttpRequest> CreateRequest(const char *pUrl) override { return std::make_unique<CTestHttpRequest>(pUrl); }
		bool HasIpresolveBug() const override { return false; }
	};

	std::vector<uint8_t> TestPng(uint8_t Color)
	{
		CImageInfo Image;
		Image.m_Width = 1;
		Image.m_Height = 1;
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		Image.AllocateFillZero();
		Image.m_pData[0] = Color;
		CByteBufferWriter Writer;
		EXPECT_TRUE(CImageLoader::SavePng(Writer, Image));
		Image.Free();
		return std::vector<uint8_t>(Writer.Data(), Writer.Data() + Writer.Size());
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
	EXPECT_EQ(pRunningJob->State(), IJob::STATE_RUNNING);
	Loader.AbortOwnerBeforeGeneration(3, 5);
	EXPECT_FALSE(RunningResource.IsFinished());
	EXPECT_TRUE(OldResource.IsFinished());
	EXPECT_FALSE(CurrentResource.IsFinished());
	EXPECT_EQ(pOldJob->State(), IJob::STATE_ABORTED);
	EXPECT_EQ(pCurrentJob->State(), IJob::STATE_QUEUED);

	// A job a worker has already picked up is aborted as well, so a component
	// that drops its stale work does not wait for a result nobody collects.
	Loader.AbortOwnerBeforeGeneration(1, 2);
	EXPECT_EQ(pRunningJob->State(), IJob::STATE_ABORTED);

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
	CImageResource Resource = Loader.LoadImageData(std::move(vPng), "memory.png", 4, 7, [](CImageInfo &Image) {
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

TEST(AssetLoader, UncompressesRawMapImageData)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	CTestInfo Info;

	constexpr size_t Width = 3;
	constexpr size_t Height = 2;
	std::array<uint8_t, Width * Height * 4> aPixels;
	for(size_t i = 0; i < aPixels.size(); ++i)
	{
		aPixels[i] = static_cast<uint8_t>(i * 7);
	}

	{
		CDataFileWriter Writer;
		ASSERT_TRUE(Writer.Open(pStorage.get(), Info.m_aFilename));
		EXPECT_EQ(Writer.AddData(aPixels.size(), aPixels.data()), 0);
		EXPECT_EQ(Writer.AddData(3, "abc"), 1);
		Writer.Finish();
	}

	CDataFileReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage.get(), Info.m_aFilename, IStorage::TYPE_ALL));

	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);

	CDataFileRawData RawData;
	ASSERT_TRUE(Reader.GetRawData(0, RawData));
	EXPECT_EQ(RawData.UncompressedSize(), aPixels.size());
	CImageResource Resource = Loader.LoadImageRawData(std::move(RawData), Width, Height, CImageInfo::FORMAT_RGBA, "embedded: test", 1, 2, [](CImageInfo &Image) {
		Image.m_pData[0] = 42;
		return true;
	});
	EXPECT_STREQ(Resource.Path(), "embedded: test");
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady(2));
	CImageInfo Image = Resource.TakeImage();
	EXPECT_EQ(Image.m_Width, Width);
	EXPECT_EQ(Image.m_Height, Height);
	EXPECT_EQ(Image.m_Format, CImageInfo::FORMAT_RGBA);
	ASSERT_EQ(Image.DataSize(), aPixels.size());
	EXPECT_EQ(Image.m_pData[0], 42);
	EXPECT_TRUE(std::equal(aPixels.begin() + 1, aPixels.end(), Image.m_pData + 1));
	Image.Free();

	// A data block that is too small for the image is rejected instead of
	// being read beyond its end.
	CDataFileRawData ShortRawData;
	ASSERT_TRUE(Reader.GetRawData(1, ShortRawData));
	EXPECT_EQ(ShortRawData.UncompressedSize(), 3U);
	CImageResource ShortResource = Loader.LoadImageRawData(std::move(ShortRawData), Width, Height, CImageInfo::FORMAT_RGBA, "embedded: short", 1, 2);
	WaitForResource(Loader, ShortResource);
	EXPECT_TRUE(ShortResource.IsFailed(2));

	// Corrupt data cannot be uncompressed.
	CImageResource CorruptResource = Loader.LoadImageRawData(CDataFileRawData({0x12, 0x34, 0x56, 0x78}, Width * Height * 4, true), Width, Height, CImageInfo::FORMAT_RGBA, "embedded: corrupt", 1, 2);
	WaitForResource(Loader, CorruptResource);
	EXPECT_TRUE(CorruptResource.IsFailed(2));

	Loader.Update();
	EXPECT_TRUE(Loader.Idle());
	Loader.Shutdown();
	pEngine->ShutdownJobs();

	Reader.Close();
	if(!HasFailure())
	{
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
	}
}

TEST(AssetLoader, ReadsTextFilesAndReportsMissingOnes)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	CTestInfo Info;
	const std::string Content = "{\"skin\": {\"body\": {\"filename\": \"standard\"}}}";
	{
		IOHANDLE File = pStorage->OpenFile(Info.m_aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		ASSERT_TRUE(File);
		EXPECT_EQ(io_write(File, Content.data(), Content.size()), Content.size());
		EXPECT_EQ(io_close(File), 0);
	}

	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);

	CTypedAssetResource<CTextAssetJob> Resource = Loader.LoadTextFile(pStorage.get(), Info.m_aFilename, IStorage::TYPE_SAVE, 1, 2);
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady(2));
	EXPECT_EQ(Resource.Type(), EAssetType::TEXT);
	EXPECT_STREQ(Resource.Path(), Info.m_aFilename);
	EXPECT_EQ(Resource.Result().Text(), Content);

	// A file that is not there fails the resource instead of the loader, the
	// same way a missing image does.
	CTypedAssetResource<CTextAssetJob> MissingResource = Loader.LoadTextFile(pStorage.get(), "asset_loader_test_missing.json", IStorage::TYPE_SAVE, 1, 2);
	WaitForResource(Loader, MissingResource);
	EXPECT_TRUE(MissingResource.IsFailed(2));

	Loader.Shutdown();
	pEngine->ShutdownJobs();
	pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
}

TEST(AssetLoader, DecodesHttpResponseWhenRequestFinished)
{
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CTestHttp Http;
	auto pRequest = std::make_shared<CTestHttpRequest>();
	CImageResource Resource = Loader.LoadImageHttp(&Http, pRequest, CHttpAssetDestination(), "downloaded.png", 1, 2);

	// The request is run immediately, but no job is submitted for it before
	// the download finished, so no job thread waits for the network.
	ASSERT_EQ(Http.m_vpRequests.size(), 1U);
	EXPECT_EQ(Http.m_vpRequests[0], pRequest);
	EXPECT_EQ(Loader.WaitingCount(), 1U);
	Loader.Update();
	EXPECT_EQ(Loader.WaitingCount(), 1U);
	EXPECT_EQ(Loader.PendingCount(), 0U);
	EXPECT_EQ(Loader.RunningCount(), 0U);
	EXPECT_FALSE(Resource.IsFinished());

	pRequest->Finish(EHttpState::DONE, 200, TestPng(42));
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady(2));
	EXPECT_EQ(Resource.HttpSource(), EHttpAssetSource::RESPONSE);
	EXPECT_STREQ(Resource.Path(), "downloaded.png");
	CImageInfo Image = Resource.TakeImage();
	EXPECT_EQ(Image.m_pData[0], 42);
	Image.Free();

	Loader.Update();
	EXPECT_TRUE(Loader.Idle());
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}

TEST(AssetLoader, LoadsHttpDestinationFileWithoutResponse)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	CTestInfo Info;
	const std::vector<uint8_t> vPng = TestPng(11);
	{
		IOHANDLE File = pStorage->OpenFile(Info.m_aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		ASSERT_TRUE(File);
		EXPECT_EQ(io_write(File, vPng.data(), vPng.size()), vPng.size());
		EXPECT_EQ(io_close(File), 0);
	}

	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CTestHttp Http;

	// A request that did not return the asset falls back to the file it
	// downloads to, so a cached asset is used when it is still up to date.
	auto pNotModifiedRequest = std::make_shared<CTestHttpRequest>();
	pNotModifiedRequest->Finish(EHttpState::DONE, 304, {});
	CImageResource NotModifiedResource = Loader.LoadImageHttp(&Http, pNotModifiedRequest, CHttpAssetDestination(pStorage.get(), Info.m_aFilename, IStorage::TYPE_SAVE, false), "downloaded.png", 1, 2);
	WaitForResource(Loader, NotModifiedResource);
	ASSERT_TRUE(NotModifiedResource.IsReady(2));
	EXPECT_EQ(NotModifiedResource.HttpSource(), EHttpAssetSource::DESTINATION);
	EXPECT_STREQ(NotModifiedResource.Path(), Info.m_aFilename);
	CImageInfo Image = NotModifiedResource.TakeImage();
	EXPECT_EQ(Image.m_pData[0], 11);
	Image.Free();

	// A failed request only uses the file when the asset allows it
	auto pFailedRequest = std::make_shared<CTestHttpRequest>();
	pFailedRequest->Finish(EHttpState::ERROR, 0, {});
	CImageResource FailedResource = Loader.LoadImageHttp(&Http, pFailedRequest, CHttpAssetDestination(pStorage.get(), Info.m_aFilename, IStorage::TYPE_SAVE, false), "downloaded.png", 1, 2);
	WaitForResource(Loader, FailedResource);
	EXPECT_TRUE(FailedResource.IsFailed(2));
	EXPECT_EQ(FailedResource.HttpSource(), EHttpAssetSource::NONE);

	auto pFallbackRequest = std::make_shared<CTestHttpRequest>();
	pFallbackRequest->Finish(EHttpState::ERROR, 0, {});
	CImageResource FallbackResource = Loader.LoadImageHttp(&Http, pFallbackRequest, CHttpAssetDestination(pStorage.get(), Info.m_aFilename, IStorage::TYPE_SAVE, true), "downloaded.png", 1, 2);
	WaitForResource(Loader, FallbackResource);
	ASSERT_TRUE(FallbackResource.IsReady(2));
	EXPECT_EQ(FallbackResource.HttpSource(), EHttpAssetSource::DESTINATION);
	FallbackResource.TakeImage().Free();

	// A missing file cannot be used either
	auto pMissingRequest = std::make_shared<CTestHttpRequest>();
	pMissingRequest->Finish(EHttpState::DONE, 404, {});
	CImageResource MissingResource = Loader.LoadImageHttp(&Http, pMissingRequest, CHttpAssetDestination(pStorage.get(), "asset_loader_test_missing.png", IStorage::TYPE_SAVE, true), "downloaded.png", 1, 2);
	WaitForResource(Loader, MissingResource);
	EXPECT_TRUE(MissingResource.IsFailed(2));
	EXPECT_EQ(MissingResource.HttpSource(), EHttpAssetSource::NONE);

	Loader.Update();
	EXPECT_TRUE(Loader.Idle());
	Loader.Shutdown();
	pEngine->ShutdownJobs();

	if(!HasFailure())
	{
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
	}
}

TEST(AssetLoader, AbortsUnfinishedHttpRequest)
{
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CTestHttp Http;
	auto pRequest = std::make_shared<CTestHttpRequest>();
	CImageResource Resource = Loader.LoadImageHttp(&Http, pRequest, CHttpAssetDestination(), "downloaded.png", 1, 2);
	EXPECT_FALSE(pRequest->IsAbortRequested());

	// Dropping the asset also cancels the download it is waiting for
	Loader.AbortOwnerBeforeGeneration(1, 3);
	EXPECT_TRUE(pRequest->IsAbortRequested());
	EXPECT_TRUE(Resource.IsFinished());
	EXPECT_FALSE(Resource.IsReady(2));
	EXPECT_TRUE(Loader.Idle());
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}
