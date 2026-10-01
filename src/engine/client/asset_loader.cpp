/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "asset_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/http.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace
{
	// How many files that are not urgent are fetched at once. A browser opens
	// six connections to a host and queues the rest in the order they were
	// asked for, so more than a few at a time keep the urgent ones waiting.
	constexpr size_t MAX_CONCURRENT_FETCHES = 6;
} // namespace

class CImageAssetJob final : public CAssetJob
{
	// Compressed pixels of a map image
	CDataFileRawData m_RawData;
	bool m_FromRawData = false;
	CImageInfo m_Image;
	std::function<bool(CImageInfo &)> m_Postprocess;

	bool UncompressRawData();

protected:
	bool Process() override;

public:
	CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess);
	CImageAssetJob(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess);

	CImageInfo TakeImage();
};

CAssetJob::CAssetJob(IStorage *pStorage, const char *pPath, int StorageType) :
	m_Path(pPath),
	m_pStorage(pStorage),
	m_StorageType(StorageType)
{
	dbg_assert(pStorage != nullptr, "Asset storage must not be null");
	Abortable(true);
}

CAssetJob::CAssetJob(const char *pContextName) :
	m_Path(pContextName)
{
	Abortable(true);
}

std::span<const uint8_t> CAssetJob::Data() const
{
	if(!m_UseResponse)
		return m_vData;
	unsigned char *pResult;
	size_t ResultSize;
	m_pRequest->Result(&pResult, &ResultSize);
	return {pResult, ResultSize};
}

std::vector<uint8_t> CAssetJob::TakeData()
{
	if(!m_UseResponse)
		return std::move(m_vData);
	// The request owns the response, so the caller gets a copy
	const std::span<const uint8_t> Response = Data();
	return std::vector<uint8_t>(Response.begin(), Response.end());
}

bool CAssetJob::ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData)
{
	IOHANDLE File = pStorage->OpenFile(pPath, IOFLAG_READ, StorageType);
	if(!File)
		return false;
	const int64_t Length = io_length(File);
	bool Success = Length >= 0 && (uint64_t)Length <= std::numeric_limits<unsigned>::max();
	if(Success)
	{
		vData.resize(Length);
		Success = io_read(File, vData.data(), Length) == (unsigned)Length;
	}
	io_close(File);
	if(!Success)
		vData.clear();
	return Success;
}

void CAssetJob::Run()
{
	if(State() == IJob::STATE_ABORTED)
		return;
	m_Success = !m_ReadFailed && Process();
	if(m_pRequest != nullptr && m_HttpStatus != 0 && m_HttpStatus < 400 && m_pRequest->ValidatesBeforeOverwrite())
		m_pRequest->OnValidation(m_Success);
}

bool CAssetJob::Abort()
{
	if(!IJob::Abort())
		return false;
	if(m_pRequest != nullptr)
		m_pRequest->Abort();
	return true;
}

void CAssetLoader::Init(IEngine *pEngine, size_t MaxConcurrentJobs, IHttp *pHttp)
{
	dbg_assert(m_pEngine == nullptr, "Asset loader already initialized");
	dbg_assert(MaxConcurrentJobs > 0, "Asset loader needs at least one concurrent job");
	m_pEngine = pEngine;
	m_MaxConcurrentJobs = MaxConcurrentJobs;
	m_pHttp = pHttp;
}

void CAssetLoader::ReaderThread(void *pUser)
{
	static_cast<CAssetLoader *>(pUser)->ReadLoop();
}

void CAssetLoader::ReadLoop()
{
	while(true)
	{
		m_ReaderSemaphore.Wait();
		if(m_ReaderShutdown)
			return;
		std::shared_ptr<CAssetJob> pJob;
		{
			const CLockScope LockScope(m_ReaderLock);
			if(m_vpUnreadJobs.empty())
				continue;
			pJob = std::move(m_vpUnreadJobs.front());
			m_vpUnreadJobs.pop_front();
		}
		if(pJob->State() != IJob::STATE_ABORTED && !CAssetJob::ReadFile(pJob->m_pStorage, pJob->Path(), pJob->m_StorageType, pJob->m_vData))
			pJob->m_ReadFailed = true;
		const CLockScope LockScope(m_ReaderLock);
		m_vpReadJobs.push_back(std::move(pJob));
	}
}

void CAssetLoader::Submit(std::shared_ptr<CAssetJob> pJob, EAssetPriority Priority)
{
	dbg_assert(m_pEngine != nullptr, "Asset loader not initialized");
	if(m_Shutdown)
	{
		pJob->Abort();
		return;
	}
	pJob->m_Priority = Priority;
	if(pJob->m_pRequest != nullptr)
		m_vpFetchingJobs.push_back(std::move(pJob));
	else if(!StartFetching(pJob))
		Enqueue(std::move(pJob));
}

bool CAssetLoader::StartFetching(const std::shared_ptr<CAssetJob> &pJob)
{
	if(m_pHttp == nullptr || pJob->m_pStorage == nullptr)
		return false;
	char aUrl[512];
	if(!pJob->m_pStorage->FetchUrl(pJob->Path(), pJob->m_StorageType, aUrl, sizeof(aUrl)))
		return false;
	if(pJob->m_Priority == EAssetPriority::URGENT || m_CountedFetches < MAX_CONCURRENT_FETCHES)
	{
		Fetch(pJob, aUrl);
		return true;
	}
	auto It = m_vDeferredFetches.end();
	if(pJob->m_Priority == EAssetPriority::NORMAL)
		It = std::find_if(m_vDeferredFetches.begin(), m_vDeferredFetches.end(), [](const CDeferredFetch &Deferred) { return Deferred.m_pJob->m_Priority == EAssetPriority::BACKGROUND; });
	m_vDeferredFetches.insert(It, {pJob, aUrl});
	return true;
}

void CAssetLoader::Fetch(const std::shared_ptr<CAssetJob> &pJob, const char *pUrl)
{
	std::shared_ptr<IHttpRequest> pRequest = m_pHttp->CreateRequest(pUrl);
	// A file that is not there answers 404, which is how a program that
	// cannot list the data directory learns it, see `IStorage::FetchUrl`: not
	// an error of the request, so the loader says what went wrong itself.
	pRequest->LogProgress(HTTPLOG::NONE);
	pRequest->FailOnErrorStatus(false);
	pJob->m_pRequest = pRequest;
	pJob->m_FetchedByLoader = true;
	pJob->m_CountedFetch = pJob->m_Priority != EAssetPriority::URGENT;
	if(pJob->m_CountedFetch)
		++m_CountedFetches;
	m_vpFetchingJobs.push_back(pJob);
	m_pHttp->Run(std::move(pRequest));
}

void CAssetLoader::StartDeferredFetches()
{
	while(!m_vDeferredFetches.empty() && m_CountedFetches < MAX_CONCURRENT_FETCHES)
	{
		CDeferredFetch Deferred = std::move(m_vDeferredFetches.front());
		m_vDeferredFetches.pop_front();
		if(!Deferred.m_pJob->Done())
			Fetch(Deferred.m_pJob, Deferred.m_Url.c_str());
	}
}

void CAssetLoader::Enqueue(std::shared_ptr<CAssetJob> pJob)
{
	if(pJob->m_pStorage == nullptr)
	{
		Queue(m_vpPendingJobs, std::move(pJob));
		StartPendingJobs();
		return;
	}
	if(m_pReaderThread == nullptr)
		m_pReaderThread = thread_init(ReaderThread, this, "asset reader");
	{
		const CLockScope LockScope(m_ReaderLock);
		Queue(m_vpUnreadJobs, std::move(pJob));
	}
	m_ReaderSemaphore.Signal();
}

void CAssetLoader::Queue(std::deque<std::shared_ptr<CAssetJob>> &vpJobs, std::shared_ptr<CAssetJob> pJob)
{
	auto It = vpJobs.end();
	if(pJob->m_Priority == EAssetPriority::URGENT)
		It = std::find_if(vpJobs.begin(), vpJobs.end(), [](const std::shared_ptr<CAssetJob> &pQueued) { return pQueued->m_Priority != EAssetPriority::URGENT; });
	vpJobs.insert(It, std::move(pJob));
}

bool CAssetLoader::Requeue(std::deque<std::shared_ptr<CAssetJob>> &vpJobs, const std::shared_ptr<CAssetJob> &pJob)
{
	const auto It = std::find(vpJobs.begin(), vpJobs.end(), pJob);
	if(It == vpJobs.end())
		return false;
	vpJobs.erase(It);
	Queue(vpJobs, pJob);
	return true;
}

void CAssetLoader::Prioritize(const CAssetResource &Resource)
{
	const std::shared_ptr<CAssetJob> &pJob = Resource.m_pJob;
	if(pJob == nullptr || pJob->m_Priority == EAssetPriority::URGENT || pJob->Done())
		return;
	pJob->m_Priority = EAssetPriority::URGENT;
	const auto Deferred = std::find_if(m_vDeferredFetches.begin(), m_vDeferredFetches.end(), [&](const CDeferredFetch &Entry) { return Entry.m_pJob == pJob; });
	if(Deferred != m_vDeferredFetches.end())
	{
		const std::string Url = std::move(Deferred->m_Url);
		m_vDeferredFetches.erase(Deferred);
		Fetch(pJob, Url.c_str());
		return;
	}
	if(Requeue(m_vpPendingJobs, pJob))
		return;
	const CLockScope LockScope(m_ReaderLock);
	Requeue(m_vpUnreadJobs, pJob);
}

void CAssetLoader::UpdateFetchingJobs()
{
	for(auto It = m_vpFetchingJobs.begin(); It != m_vpFetchingJobs.end();)
	{
		const std::shared_ptr<CAssetJob> pJob = *It;
		const IHttpRequest &Request = *pJob->m_pRequest;
		if(!pJob->Done() && !Request.Done())
		{
			++It;
			continue;
		}
		It = m_vpFetchingJobs.erase(It);
		if(pJob->m_CountedFetch)
		{
			dbg_assert(m_CountedFetches > 0, "Fetch count underflow");
			--m_CountedFetches;
			pJob->m_CountedFetch = false;
		}
		if(pJob->Done())
			continue;

		if(Request.State() == EHttpState::DONE)
			pJob->m_HttpStatus = Request.StatusCode();
		const bool Success = pJob->m_HttpStatus != 0 && pJob->m_HttpStatus < 400;
		if(!Success && pJob->m_FetchedByLoader)
		{
			if(pJob->m_HttpStatus == 404)
				log_debug("asset_loader", "'%s' is not there", pJob->Path());
			else
				log_error("asset_loader", "Could not fetch '%s' (status %d)", pJob->Path(), pJob->m_HttpStatus);
		}
		if(Success && pJob->m_HttpStatus != 304 && Request.WritesToMemory())
		{
			pJob->m_UseResponse = true;
			pJob->m_pStorage = nullptr;
		}
		else if(!Success && !pJob->m_UseFileOnError)
		{
			pJob->m_pStorage = nullptr;
			pJob->m_ReadFailed = true;
		}
		Enqueue(pJob);
	}
}

void CAssetLoader::UpdateReadJobs()
{
	std::vector<std::shared_ptr<CAssetJob>> vpRead;
	{
		const CLockScope LockScope(m_ReaderLock);
		vpRead.swap(m_vpReadJobs);
	}
	for(auto &pJob : vpRead)
		Queue(m_vpPendingJobs, std::move(pJob));
}

CTypedAssetResource<CFileAssetJob> CAssetLoader::LoadFile(IStorage *pStorage, const char *pPath, int StorageType)
{
	return Load(std::make_shared<CFileAssetJob>(pStorage, pPath, StorageType));
}

CImageResource CAssetLoader::LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess, EAssetPriority Priority)
{
	auto pJob = std::make_shared<CImageAssetJob>(pStorage, pPath, StorageType, std::move(Postprocess));
	Submit(pJob, Priority);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess, EAssetPriority Priority)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(RawData), Width, Height, Format, pContextName, std::move(Postprocess));
	Submit(pJob, Priority);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageHttp(IHttp *pHttp, std::shared_ptr<IHttpRequest> pRequest, IStorage *pStorage, const char *pPath, int StorageType, bool UseFileOnError, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(pStorage, pPath, StorageType, std::move(Postprocess));
	pJob->m_pRequest = pRequest;
	pJob->m_UseFileOnError = UseFileOnError;
	pHttp->Run(std::move(pRequest));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

void CAssetLoader::StartPendingJobs()
{
	while(m_vpRunningJobs.size() < m_MaxConcurrentJobs && !m_vpPendingJobs.empty())
	{
		std::shared_ptr<CAssetJob> pJob = std::move(m_vpPendingJobs.front());
		m_vpPendingJobs.pop_front();
		if(pJob->Done())
			continue;
		m_vpRunningJobs.push_back(pJob);
		m_pEngine->AddJob(std::move(pJob));
	}
}

void CAssetLoader::Update()
{
	dbg_assert(m_pEngine != nullptr, "Asset loader not initialized");
	UpdateFetchingJobs();
	StartDeferredFetches();
	UpdateReadJobs();
	m_vpRunningJobs.erase(
		std::remove_if(m_vpRunningJobs.begin(), m_vpRunningJobs.end(), [this](const auto &pJob) {
			if(!pJob->Done())
				return false;
			NoteFinishedJob(*pJob);
			return true;
		}),
		m_vpRunningJobs.end());
	StartPendingJobs();
	if(!m_LateWhat.empty() && time_get() >= m_LateEnd)
		FinishLateReport();
}

void CAssetLoader::ReportLateAssets(const char *pWhat, std::chrono::nanoseconds Window)
{
	if(!m_LateWhat.empty())
		FinishLateReport();
	m_LateWhat = pWhat;
	m_LateStart = time_get();
	// In milliseconds, nanoseconds times the tick frequency overflow after a few seconds.
	m_LateEnd = m_LateStart + std::chrono::duration_cast<std::chrono::milliseconds>(Window).count() * time_freq() / 1'000;
	m_vLateAssets.clear();
}

void CAssetLoader::NoteFinishedJob(const CAssetJob &Job)
{
	// An aborted asset was not wanted any more, so it cannot pop in.
	if(m_LateWhat.empty() || Job.State() != IJob::STATE_DONE)
		return;
	m_vLateAssets.emplace_back(Job.Path(), time_get() - m_LateStart);
}

void CAssetLoader::EndLateReport()
{
	if(!m_LateWhat.empty())
		FinishLateReport();
}

void CAssetLoader::FinishLateReport()
{
	// Cut short when something else began to load, see `EndLateReport`.
	const int64_t WindowMs = (std::min(time_get(), m_LateEnd) - m_LateStart) * 1000 / time_freq();
	if(m_vLateAssets.empty())
	{
		log_debug("asset_loader", "No asset arrived in the %" PRId64 " ms after the %s was first drawn", WindowMs, m_LateWhat.c_str());
	}
	else
	{
		// The first line counts them by folder, so that logs compare at a
		// glance; the lines after it name them all.
		std::map<std::string, int> Folders;
		for(const auto &[Path, Time] : m_vLateAssets)
		{
			const size_t Slash = Path.find('/');
			++Folders[Slash == std::string::npos ? std::string(".") : Path.substr(0, Slash)];
		}
		std::string Counts;
		for(const auto &[Folder, Count] : Folders)
			Counts += (Counts.empty() ? "" : ", ") + std::to_string(Count) + " " + Folder;
		log_debug("asset_loader", "%" PRIzu " assets arrived in the %" PRId64 " ms after the %s was first drawn: %s", m_vLateAssets.size(), WindowMs, m_LateWhat.c_str(), Counts.c_str());
		char aList[1024] = "";
		for(size_t i = 0; i < m_vLateAssets.size(); ++i)
		{
			const auto &[Path, Time] = m_vLateAssets[i];
			char aEntry[IO_MAX_PATH_LENGTH + 32];
			str_format(aEntry, sizeof(aEntry), "%s%s (+%" PRId64 " ms)", aList[0] == '\0' ? "" : ", ", Path.c_str(), Time * 1000 / time_freq());
			str_append(aList, aEntry);
			if(i + 1 == m_vLateAssets.size() || str_length(aList) + IO_MAX_PATH_LENGTH + 32 >= (int)sizeof(aList))
			{
				log_debug("asset_loader", "Arrived after the %s was first drawn: %s", m_LateWhat.c_str(), aList);
				aList[0] = '\0';
			}
		}
	}
	m_LateWhat.clear();
	m_vLateAssets.clear();
}

void CAssetLoader::Shutdown()
{
	if(m_pEngine == nullptr || m_Shutdown)
		return;
	m_Shutdown = true;
	// Stop the reader first, so no other thread touches jobs
	{
		const CLockScope LockScope(m_ReaderLock);
		for(const auto &pJob : m_vpUnreadJobs)
			pJob->Abort();
		for(const auto &pJob : m_vpReadJobs)
			pJob->Abort();
	}
	if(m_pReaderThread != nullptr)
	{
		m_ReaderShutdown = true;
		m_ReaderSemaphore.Signal();
		thread_wait(m_pReaderThread);
		m_pReaderThread = nullptr;
	}
	{
		const CLockScope LockScope(m_ReaderLock);
		for(const auto &pJob : m_vpReadJobs)
			pJob->Abort();
		m_vpUnreadJobs.clear();
		m_vpReadJobs.clear();
	}
	for(const auto &pJob : m_vpFetchingJobs)
		pJob->Abort();
	for(const auto &Deferred : m_vDeferredFetches)
		Deferred.m_pJob->Abort();
	for(const auto &pJob : m_vpPendingJobs)
		pJob->Abort();
	for(const auto &pJob : m_vpRunningJobs)
		pJob->Abort();
	m_vpFetchingJobs.clear();
	m_vDeferredFetches.clear();
	m_CountedFetches = 0;
	m_vpPendingJobs.clear();
	m_vpRunningJobs.clear();
}

CImageAssetJob::CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(pStorage, pPath, StorageType),
	m_Postprocess(std::move(Postprocess))
{
}

CImageAssetJob::CImageAssetJob(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(pContextName),
	m_RawData(std::move(RawData)),
	m_FromRawData(true),
	m_Postprocess(std::move(Postprocess))
{
	dbg_assert(Format != CImageInfo::FORMAT_UNDEFINED, "Raw image format must be defined");
	m_Image.m_Width = Width;
	m_Image.m_Height = Height;
	m_Image.m_Format = Format;
}

bool CImageAssetJob::UncompressRawData()
{
	if(m_RawData.UncompressedSize() >= m_Image.DataSize() && m_Image.TryAllocate())
	{
		if(m_RawData.UncompressedSize() == m_Image.DataSize())
		{
			if(m_RawData.UncompressTo(m_Image.m_pData))
				return true;
		}
		else if(const std::unique_ptr<uint8_t[]> pData = m_RawData.Uncompress())
		{
			// The data item is larger than the image, only its start is used
			mem_copy(m_Image.m_pData, pData.get(), m_Image.DataSize());
			return true;
		}
		m_Image.Free();
	}
	log_error("asset_loader", "Invalid image data in '%s'", Path());
	return false;
}

bool CImageAssetJob::Process()
{
	if(m_FromRawData)
	{
		if(!UncompressRawData())
			return false;
	}
	else
	{
		CByteBufferReader Reader(Data().data(), Data().size());
		int PngliteIncompatible;
		if(!CImageLoader::LoadPng(Reader, Path(), m_Image, PngliteIncompatible))
			return false;
	}
	if(m_Postprocess && !m_Postprocess(m_Image))
	{
		m_Image.Free();
		return false;
	}
	return true;
}

CImageInfo CImageAssetJob::TakeImage()
{
	dbg_assert(State() == IJob::STATE_DONE && Success(), "Cannot take image from unfinished or failed asset job");
	return std::move(m_Image);
}

CFileAssetJob::CFileAssetJob(IStorage *pStorage, const char *pPath, int StorageType) :
	CAssetJob(pStorage, pPath, StorageType)
{
}

std::vector<uint8_t> CFileAssetJob::TakeBytes()
{
	dbg_assert(State() == IJob::STATE_DONE && Success(), "Cannot take bytes from unfinished or failed asset job");
	return TakeData();
}

std::string_view CFileAssetJob::Text() const
{
	dbg_assert(State() == IJob::STATE_DONE && Success(), "Cannot read text of unfinished or failed asset job");
	const std::span<const uint8_t> Bytes = Data();
	return std::string_view(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
}

SHA256_DIGEST CAssetResource::SourceSha256() const
{
	dbg_assert(IsReady(), "Asset resource is not ready");
	const std::span<const uint8_t> Bytes = m_pJob->Data();
	return sha256(Bytes.data(), Bytes.size());
}

CAssetResource::CAssetResource(std::shared_ptr<CAssetJob> pJob) :
	m_pJob(std::move(pJob))
{
}

CAssetResource &CAssetResource::operator=(CAssetResource &&Other) noexcept
{
	if(this != &Other)
	{
		Reset();
		m_pJob = std::move(Other.m_pJob);
	}
	return *this;
}

bool CAssetResource::IsFinished() const
{
	return m_pJob != nullptr && m_pJob->Done();
}

bool CAssetResource::IsReady() const
{
	return m_pJob != nullptr && m_pJob->State() == IJob::STATE_DONE && m_pJob->Success();
}

bool CAssetResource::IsFailed() const
{
	return m_pJob != nullptr && m_pJob->State() == IJob::STATE_DONE && !m_pJob->Success();
}

bool CAssetResource::Abort()
{
	return m_pJob != nullptr && m_pJob->Abort();
}

void CAssetResource::Reset()
{
	if(m_pJob != nullptr && !m_pJob->Done())
		m_pJob->Abort();
	m_pJob.reset();
}

const char *CAssetResource::Path() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no path");
	return m_pJob->Path();
}

int CAssetResource::HttpStatus() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no request");
	return m_pJob->HttpStatus();
}

CImageResource::CImageResource(std::shared_ptr<CImageAssetJob> pJob) :
	CAssetResource(std::move(pJob))
{
}

CImageInfo CImageResource::TakeImage()
{
	dbg_assert(Job() != nullptr, "Cannot take an image from an empty resource");
	return static_cast<CImageAssetJob *>(Job())->TakeImage();
}

bool CImageResource::FinishTexture(IGraphics *pGraphics, IGraphics::CTextureHandle &Texture, int Flags)
{
	if(!IsFinished())
		return false;
	if(IsReady())
	{
		CImageInfo Image = TakeImage();
		IGraphics::CTextureHandle NewTexture = pGraphics->LoadTextureRawMove(Image, Flags, Path());
		if(NewTexture.IsValid())
		{
			pGraphics->UnloadTexture(&Texture);
			Texture = NewTexture;
		}
		else
		{
			log_error("asset_loader", "Failed to upload '%s'", Path());
		}
	}
	else if(IsFailed())
	{
		log_error("asset_loader", "Failed to load '%s'", Path());
	}
	Reset();
	return true;
}

void COnDemandTextures::Init(IGraphics *pGraphics, IStorage *pStorage, CAssetLoader *pLoader)
{
	m_pGraphics = pGraphics;
	m_pStorage = pStorage;
	m_pLoader = pLoader;
}

IGraphics::CTextureHandle COnDemandTextures::Get(IGraphics::CTextureHandle &Texture, const char *pPath, int Flags)
{
	if(Texture.IsValid())
		return Texture;
	const auto Load = m_Loads.find(&Texture);
	if(Load == m_Loads.end())
		Replace(Texture, pPath, Flags);
	else if(Load->second.m_Resource)
		Load->second.m_Resource.FinishTexture(m_pGraphics, Texture, Load->second.m_Flags);
	return Texture;
}

void COnDemandTextures::Replace(IGraphics::CTextureHandle &Texture, const char *pPath, int Flags)
{
	dbg_assert(m_pGraphics != nullptr, "COnDemandTextures::Init was not called");
	char aUrl[IO_MAX_PATH_LENGTH * 2];
	if(m_pStorage->FetchUrl(pPath, IStorage::TYPE_ALL, aUrl, sizeof(aUrl)))
	{
		CLoad &Load = m_Loads[&Texture];
		Load.m_Flags = Flags;
		Load.m_Resource = m_pLoader->LoadImageFile(m_pStorage, pPath, IStorage::TYPE_ALL, {}, EAssetPriority::URGENT);
		return;
	}
	m_Loads.erase(&Texture);
	m_pGraphics->UnloadTexture(&Texture);
	Texture = m_pGraphics->LoadTexture(pPath, IStorage::TYPE_ALL, Flags);
}

void COnDemandTextures::Forget(IGraphics::CTextureHandle &Texture)
{
	m_Loads.erase(&Texture);
}

void COnDemandTextures::Update()
{
	for(auto &[pTexture, Load] : m_Loads)
	{
		if(Load.m_Resource)
			Load.m_Resource.FinishTexture(m_pGraphics, *pTexture, Load.m_Flags);
	}
}

bool COnDemandTextures::IsWaiting() const
{
	return std::any_of(m_Loads.begin(), m_Loads.end(), [](const auto &Entry) { return Entry.second.m_Resource && !Entry.first->IsValid(); });
}
