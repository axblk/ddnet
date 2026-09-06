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
			CAssetJob("test"),
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
		CTestHttpRequest() :
			IHttpRequest("http://localhost/test.png")
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

	template<typename TResource>
	void WaitForResources(CAssetLoader &Loader, const std::vector<TResource> &vResources)
	{
		for(const auto &Resource : vResources)
			WaitForResource(Loader, Resource);
	}

	void WriteTestFile(IStorage *pStorage, const char *pFilename, const void *pData, size_t Size)
	{
		IOHANDLE File = pStorage->OpenFile(pFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		ASSERT_TRUE(File);
		EXPECT_EQ(io_write(File, pData, Size), Size);
		EXPECT_EQ(io_close(File), 0);
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

TEST(AssetLoader, DecodesImageAndPostprocesses)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	CTestInfo Info;
	const std::vector<uint8_t> vPng = TestPng(0);
	WriteTestFile(pStorage.get(), Info.m_aFilename, vPng.data(), vPng.size());

	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CImageResource Resource = Loader.LoadImageFile(pStorage.get(), Info.m_aFilename, IStorage::TYPE_SAVE, [](CImageInfo &Image) {
		Image.m_pData[0] = 42;
		return true;
	});
	EXPECT_STREQ(Resource.Path(), Info.m_aFilename);
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady());
	EXPECT_FALSE(Resource.IsFailed());
	CImageInfo Result = Resource.TakeImage();
	EXPECT_EQ(Result.m_pData[0], 42);
	Resource.Reset();
	EXPECT_FALSE(Resource);

	Loader.Shutdown();
	pEngine->ShutdownJobs();
	pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
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
	CImageResource Resource = Loader.LoadImageRawData(std::move(RawData), Width, Height, CImageInfo::FORMAT_RGBA, "embedded: test", [](CImageInfo &Image) {
		Image.m_pData[0] = 42;
		return true;
	});
	EXPECT_STREQ(Resource.Path(), "embedded: test");
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady());
	CImageInfo Image = Resource.TakeImage();
	EXPECT_EQ(Image.m_Width, Width);
	EXPECT_EQ(Image.m_Height, Height);
	EXPECT_EQ(Image.m_Format, CImageInfo::FORMAT_RGBA);
	ASSERT_EQ(Image.DataSize(), aPixels.size());
	EXPECT_EQ(Image.m_pData[0], 42);
	EXPECT_TRUE(std::equal(aPixels.begin() + 1, aPixels.end(), Image.m_pData + 1));
	Image.Free();

	// Larger than the image, only its start is used
	CDataFileRawData LargeRawData;
	ASSERT_TRUE(Reader.GetRawData(0, LargeRawData));
	CImageResource LargeResource = Loader.LoadImageRawData(std::move(LargeRawData), Width, Height - 1, CImageInfo::FORMAT_RGBA, "embedded: large");
	WaitForResource(Loader, LargeResource);
	ASSERT_TRUE(LargeResource.IsReady());
	CImageInfo LargeImage = LargeResource.TakeImage();
	ASSERT_EQ(LargeImage.DataSize(), Width * (Height - 1) * 4);
	EXPECT_TRUE(std::equal(aPixels.begin(), aPixels.begin() + LargeImage.DataSize(), LargeImage.m_pData));
	LargeImage.Free();

	// Too small for the image
	CDataFileRawData ShortRawData;
	ASSERT_TRUE(Reader.GetRawData(1, ShortRawData));
	EXPECT_EQ(ShortRawData.UncompressedSize(), 3U);
	CImageResource ShortResource = Loader.LoadImageRawData(std::move(ShortRawData), Width, Height, CImageInfo::FORMAT_RGBA, "embedded: short");
	WaitForResource(Loader, ShortResource);
	EXPECT_TRUE(ShortResource.IsFailed());

	CImageResource CorruptResource = Loader.LoadImageRawData(CDataFileRawData({0x12, 0x34, 0x56, 0x78}, Width * Height * 4, true), Width, Height, CImageInfo::FORMAT_RGBA, "embedded: corrupt");
	WaitForResource(Loader, CorruptResource);
	EXPECT_TRUE(CorruptResource.IsFailed());

	Loader.Shutdown();
	pEngine->ShutdownJobs();

	Reader.Close();
	if(!HasFailure())
	{
		pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
	}
}

TEST(AssetLoader, DecodesHttpResponse)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CTestHttp Http;
	auto pRequest = std::make_shared<CTestHttpRequest>();
	CImageResource Resource = Loader.LoadImageHttp(&Http, pRequest, pStorage.get(), "downloaded.png", IStorage::TYPE_SAVE, false);
	ASSERT_EQ(Http.m_vpRequests.size(), 1U);
	EXPECT_EQ(Http.m_vpRequests[0], pRequest);
	Loader.Update();
	EXPECT_FALSE(Resource.IsFinished());

	pRequest->Finish(EHttpState::DONE, 200, TestPng(42));
	WaitForResource(Loader, Resource);
	ASSERT_TRUE(Resource.IsReady());
	EXPECT_EQ(Resource.HttpStatus(), 200);
	EXPECT_STREQ(Resource.Path(), "downloaded.png");
	CImageInfo Image = Resource.TakeImage();
	EXPECT_EQ(Image.m_pData[0], 42);
	Image.Free();

	Loader.Shutdown();
	pEngine->ShutdownJobs();
}

TEST(AssetLoader, ReadsHttpFileWithoutResponse)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	CTestInfo Info;
	const std::vector<uint8_t> vPng = TestPng(11);
	WriteTestFile(pStorage.get(), Info.m_aFilename, vPng.data(), vPng.size());

	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CTestHttp Http;
	const auto LoadAfter = [&](EHttpState State, int StatusCode, const char *pPath, bool UseFileOnError) {
		auto pRequest = std::make_shared<CTestHttpRequest>();
		pRequest->Finish(State, StatusCode, {});
		CImageResource Resource = Loader.LoadImageHttp(&Http, pRequest, pStorage.get(), pPath, IStorage::TYPE_SAVE, UseFileOnError);
		WaitForResource(Loader, Resource);
		return Resource;
	};

	CImageResource NotModifiedResource = LoadAfter(EHttpState::DONE, 304, Info.m_aFilename, false);
	ASSERT_TRUE(NotModifiedResource.IsReady());
	EXPECT_EQ(NotModifiedResource.HttpStatus(), 304);
	EXPECT_STREQ(NotModifiedResource.Path(), Info.m_aFilename);
	CImageInfo Image = NotModifiedResource.TakeImage();
	EXPECT_EQ(Image.m_pData[0], 11);
	Image.Free();

	CImageResource FailedResource = LoadAfter(EHttpState::ERROR, 0, Info.m_aFilename, false);
	EXPECT_TRUE(FailedResource.IsFailed());
	EXPECT_EQ(FailedResource.HttpStatus(), 0);

	CImageResource FallbackResource = LoadAfter(EHttpState::ERROR, 0, Info.m_aFilename, true);
	ASSERT_TRUE(FallbackResource.IsReady());
	FallbackResource.TakeImage().Free();

	CImageResource MissingResource = LoadAfter(EHttpState::DONE, 404, "asset_loader_test_missing.png", true);
	EXPECT_TRUE(MissingResource.IsFailed());
	EXPECT_EQ(MissingResource.HttpStatus(), 404);

	Loader.Shutdown();
	pEngine->ShutdownJobs();
	pStorage->RemoveFile(Info.m_aFilename, IStorage::TYPE_SAVE);
}

TEST(AssetLoader, AbortsUnfinishedHttpRequest)
{
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr) << "Error creating local storage";
	std::unique_ptr<IEngine> pEngine(CreateTestEngine("asset_loader_test"));
	CAssetLoader Loader;
	Loader.Init(pEngine.get(), 1);
	CTestHttp Http;
	auto pRequest = std::make_shared<CTestHttpRequest>();
	CImageResource Resource = Loader.LoadImageHttp(&Http, pRequest, pStorage.get(), "downloaded.png", IStorage::TYPE_SAVE, false);
	EXPECT_FALSE(pRequest->IsAbortRequested());

	Resource.Reset();
	EXPECT_TRUE(pRequest->IsAbortRequested());
	Loader.Shutdown();
	pEngine->ShutdownJobs();
}
