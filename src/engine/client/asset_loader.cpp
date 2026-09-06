/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "asset_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/http.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
	enum class EAssetLoadError
	{
		NONE,
		ABORTED,
		NOT_FOUND,
		READ,
		TOO_LARGE,
		DECODE,
	};
}

class CImageAssetJob final : public CHttpAssetJob
{
	enum class ESource
	{
		NONE,
		STORAGE,
		PNG_DATA,
		RAW_DATA,
	};

	ESource m_Source;
	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::vector<uint8_t> m_vData;
	CDataFileRawData m_RawData;
	CImageInfo m_Image;
	std::function<bool(CImageInfo &)> m_Postprocess;
	EAssetLoadError m_Error = EAssetLoadError::NONE;
	int m_PngliteIncompatible = 0;
	std::chrono::nanoseconds m_ReadTime{};
	std::chrono::nanoseconds m_DecodeTime{};

	bool LoadStorage();
	bool LoadPngData();
	bool LoadRawData();

protected:
	void Run() override;
	void OnRequestFinished(EHttpAssetSource Source, std::vector<uint8_t> vData) override;

public:
	CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess);
	CImageAssetJob(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess);
	CImageAssetJob(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess);
	CImageAssetJob(std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess);

	bool Success() const override { return m_Error == EAssetLoadError::NONE; }
	int PngliteIncompatible() const { return m_PngliteIncompatible; }
	std::chrono::nanoseconds ReadTime() const { return m_ReadTime; }
	std::chrono::nanoseconds DecodeTime() const { return m_DecodeTime; }
	CImageInfo TakeImage();
};

CAssetJob::CAssetJob(EAssetType Type, const char *pPath, int OwnerId, uint64_t Generation) :
	m_Type(Type),
	m_Path(pPath != nullptr ? pPath : ""),
	m_OwnerId(OwnerId),
	m_Generation(Generation)
{
	dbg_assert(pPath != nullptr, "Asset path must not be null");
	Abortable(true);
}

void CAssetJob::SetPath(const char *pPath)
{
	dbg_assert(pPath != nullptr, "Asset path must not be null");
	dbg_assert(State() == IJob::STATE_QUEUED, "Asset path can only be set before the job is submitted");
	m_Path = pPath;
}

CHttpAssetDestination::CHttpAssetDestination(IStorage *pStorage, const char *pPath, int StorageType, bool UseOnError) :
	m_pStorage(pStorage),
	m_Path(pPath),
	m_StorageType(StorageType),
	m_UseOnError(UseOnError)
{
	dbg_assert(pStorage != nullptr, "Asset destination storage must not be null");
}

CHttpAssetJob::CHttpAssetJob(EAssetType Type, std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, const char *pPath, int OwnerId, uint64_t Generation) :
	CAssetJob(Type, pPath, OwnerId, Generation),
	m_pRequest(std::move(pRequest)),
	m_Destination(std::move(Destination))
{
}

bool CHttpAssetJob::Abort()
{
	if(!CAssetJob::Abort())
		return false;
	if(m_pRequest != nullptr)
		m_pRequest->Abort();
	return true;
}

void CAssetLoader::Init(IEngine *pEngine, size_t MaxConcurrentJobs)
{
	dbg_assert(m_pEngine == nullptr, "Asset loader already initialized");
	dbg_assert(pEngine != nullptr, "Asset loader engine must not be null");
	dbg_assert(MaxConcurrentJobs > 0, "Asset loader needs at least one concurrent job");
	m_pEngine = pEngine;
	m_MaxConcurrentJobs = MaxConcurrentJobs;
}

uint64_t CAssetLoader::Submit(std::shared_ptr<CAssetJob> pJob)
{
	dbg_assert(m_pEngine != nullptr, "Asset loader not initialized");
	dbg_assert(pJob != nullptr, "Asset job must not be null");
	dbg_assert(pJob->RequestId() == 0, "Asset job was already submitted");
	if(m_Shutdown)
	{
		pJob->Abort();
		return 0;
	}

	dbg_assert(m_NextRequestId != 0, "Asset request ID overflow");
	const uint64_t RequestId = m_NextRequestId++;
	pJob->m_RequestId = RequestId;
	m_vpPendingJobs.push_back(std::move(pJob));
	StartPendingJobs();
	return RequestId;
}

uint64_t CAssetLoader::SubmitHttp(IHttp *pHttp, std::shared_ptr<CHttpAssetJob> pJob)
{
	dbg_assert(m_pEngine != nullptr, "Asset loader not initialized");
	dbg_assert(pHttp != nullptr, "Asset HTTP interface must not be null");
	dbg_assert(pJob != nullptr, "Asset job must not be null");
	dbg_assert(pJob->m_pRequest != nullptr, "Downloaded asset job must have a request");
	dbg_assert(pJob->RequestId() == 0, "Asset job was already submitted");
	if(m_Shutdown)
	{
		pJob->Abort();
		return 0;
	}

	dbg_assert(m_NextRequestId != 0, "Asset request ID overflow");
	const uint64_t RequestId = m_NextRequestId++;
	pJob->m_RequestId = RequestId;
	std::shared_ptr<IHttpRequest> pRequest = pJob->m_pRequest;
	m_vpWaitingJobs.push_back(std::move(pJob));
	pHttp->Run(std::move(pRequest));
	return RequestId;
}

void CAssetLoader::UpdateWaitingJobs()
{
	for(auto It = m_vpWaitingJobs.begin(); It != m_vpWaitingJobs.end();)
	{
		std::shared_ptr<CHttpAssetJob> pJob = *It;
		if(!pJob->Done() && !pJob->m_pRequest->Done())
		{
			++It;
			continue;
		}
		It = m_vpWaitingJobs.erase(It);
		if(pJob->Done())
		{
			// The job was aborted while it was waiting for the request
			continue;
		}

		const IHttpRequest &Request = *pJob->m_pRequest;
		const bool Success = Request.State() == EHttpState::DONE && Request.StatusCode() < 400;
		std::vector<uint8_t> vData;
		if(Success && Request.StatusCode() != 304 && Request.WritesToMemory())
		{
			unsigned char *pResult;
			size_t ResultSize;
			Request.Result(&pResult, &ResultSize);
			if(ResultSize > 0)
				vData.assign(pResult, pResult + ResultSize);
			pJob->m_HttpSource = EHttpAssetSource::RESPONSE;
		}
		else if((Success || pJob->m_Destination.m_UseOnError) &&
			pJob->m_Destination.m_pStorage != nullptr &&
			pJob->m_Destination.m_pStorage->FileExists(pJob->m_Destination.m_Path.c_str(), pJob->m_Destination.m_StorageType))
		{
			pJob->m_HttpSource = EHttpAssetSource::DESTINATION;
		}
		pJob->OnRequestFinished(pJob->m_HttpSource, std::move(vData));
		m_vpPendingJobs.push_back(std::move(pJob));
	}
}

CImageResource CAssetLoader::LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(pStorage, pPath, StorageType, OwnerId, Generation, std::move(Postprocess));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageData(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(vData), pContextName, OwnerId, Generation, std::move(Postprocess));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(RawData), Width, Height, Format, pContextName, OwnerId, Generation, std::move(Postprocess));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageHttp(IHttp *pHttp, std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(pRequest), std::move(Destination), pContextName, OwnerId, Generation, std::move(Postprocess));
	SubmitHttp(pHttp, pJob);
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
	UpdateWaitingJobs();
	for(const auto &pJob : m_vpRunningJobs)
	{
		if(pJob->State() != IJob::STATE_DONE || pJob->Type() != EAssetType::IMAGE)
			continue;
		const auto *pImageJob = static_cast<const CImageAssetJob *>(pJob.get());
		if(pImageJob->PngliteIncompatible() != 0)
			log_warn("asset_loader", "PNG is incompatible with pnglite: path='%s' flags=0x%x", pImageJob->Path(), pImageJob->PngliteIncompatible());
	}
	m_vpRunningJobs.erase(
		std::remove_if(m_vpRunningJobs.begin(), m_vpRunningJobs.end(), [](const auto &pJob) { return pJob->Done(); }),
		m_vpRunningJobs.end());
	StartPendingJobs();
}

void CAssetLoader::AbortOwnerBeforeGeneration(int OwnerId, uint64_t Generation)
{
	const auto AbortStaleJob = [OwnerId, Generation](const std::shared_ptr<CAssetJob> &pJob) {
		if(pJob->OwnerId() == OwnerId && pJob->Generation() < Generation)
			pJob->Abort();
	};
	for(const auto &pJob : m_vpWaitingJobs)
		AbortStaleJob(pJob);
	for(const auto &pJob : m_vpPendingJobs)
		AbortStaleJob(pJob);
	for(const auto &pJob : m_vpRunningJobs)
		AbortStaleJob(pJob);
	Update();
}

void CAssetLoader::Shutdown()
{
	if(m_pEngine == nullptr || m_Shutdown)
		return;
	m_Shutdown = true;
	for(const auto &pJob : m_vpWaitingJobs)
		pJob->Abort();
	for(const auto &pJob : m_vpPendingJobs)
		pJob->Abort();
	for(const auto &pJob : m_vpRunningJobs)
		pJob->Abort();
	m_vpWaitingJobs.clear();
	m_vpPendingJobs.clear();
	m_vpRunningJobs.clear();
}

CImageAssetJob::CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess) :
	CHttpAssetJob(EAssetType::IMAGE, nullptr, CHttpAssetDestination(), pPath, OwnerId, Generation),
	m_Source(ESource::STORAGE),
	m_pStorage(pStorage),
	m_StorageType(StorageType),
	m_Postprocess(std::move(Postprocess))
{
	dbg_assert(pStorage != nullptr, "Image asset storage must not be null");
}

CImageAssetJob::CImageAssetJob(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess) :
	CHttpAssetJob(EAssetType::IMAGE, nullptr, CHttpAssetDestination(), pContextName, OwnerId, Generation),
	m_Source(ESource::PNG_DATA),
	m_vData(std::move(vData)),
	m_Postprocess(std::move(Postprocess))
{
}

CImageAssetJob::CImageAssetJob(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess) :
	CHttpAssetJob(EAssetType::IMAGE, nullptr, CHttpAssetDestination(), pContextName, OwnerId, Generation),
	m_Source(ESource::RAW_DATA),
	m_RawData(std::move(RawData)),
	m_Postprocess(std::move(Postprocess))
{
	dbg_assert(Format != CImageInfo::FORMAT_UNDEFINED, "Raw image format must be defined");
	m_Image.m_Width = Width;
	m_Image.m_Height = Height;
	m_Image.m_Format = Format;
}

CImageAssetJob::CImageAssetJob(std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess) :
	CHttpAssetJob(EAssetType::IMAGE, std::move(pRequest), std::move(Destination), pContextName, OwnerId, Generation),
	m_Source(ESource::NONE),
	m_Postprocess(std::move(Postprocess))
{
}

void CImageAssetJob::OnRequestFinished(EHttpAssetSource Source, std::vector<uint8_t> vData)
{
	switch(Source)
	{
	case EHttpAssetSource::NONE:
		break;
	case EHttpAssetSource::RESPONSE:
		m_Source = ESource::PNG_DATA;
		m_vData = std::move(vData);
		break;
	case EHttpAssetSource::DESTINATION:
		m_Source = ESource::STORAGE;
		m_pStorage = Destination().m_pStorage;
		m_StorageType = Destination().m_StorageType;
		// The path describes what was loaded, so failures name the file
		SetPath(Destination().m_Path.c_str());
		break;
	}
}

bool CImageAssetJob::LoadStorage()
{
	IOHANDLE File = m_pStorage->OpenFile(Path(), IOFLAG_READ, m_StorageType);
	if(!File)
	{
		m_Error = EAssetLoadError::NOT_FOUND;
		return false;
	}
	if(!CImageLoader::LoadPngTimed(File, Path(), m_Image, m_PngliteIncompatible, m_ReadTime, m_DecodeTime, false))
	{
		m_Error = EAssetLoadError::DECODE;
		return false;
	}
	return true;
}

bool CImageAssetJob::LoadPngData()
{
	CByteBufferReader Reader(m_vData.data(), m_vData.size());
	const auto DecodeStart = time_get_nanoseconds();
	const bool Success = CImageLoader::LoadPng(Reader, Path(), m_Image, m_PngliteIncompatible, false);
	m_DecodeTime = time_get_nanoseconds() - DecodeStart;
	if(!Success)
	{
		m_Error = EAssetLoadError::DECODE;
		return false;
	}
	return true;
}

bool CImageAssetJob::LoadRawData()
{
	const auto DecodeStart = time_get_nanoseconds();
	const std::unique_ptr<uint8_t[]> pData = m_RawData.Uncompress();
	m_DecodeTime = time_get_nanoseconds() - DecodeStart;
	if(pData == nullptr || m_RawData.UncompressedSize() < m_Image.DataSize() || !m_Image.TryAllocate())
	{
		m_Image.Free();
		m_Error = EAssetLoadError::READ;
		return false;
	}
	mem_copy(m_Image.m_pData, pData.get(), m_Image.DataSize());
	return true;
}

void CImageAssetJob::Run()
{
	if(State() == IJob::STATE_ABORTED)
	{
		m_Error = EAssetLoadError::ABORTED;
		return;
	}
	switch(m_Source)
	{
	case ESource::NONE:
		m_Error = EAssetLoadError::NOT_FOUND;
		return;
	case ESource::STORAGE:
		if(!LoadStorage())
			return;
		break;
	case ESource::PNG_DATA:
		if(!LoadPngData())
			return;
		break;
	case ESource::RAW_DATA:
		if(!LoadRawData())
			return;
		break;
	}
	if(m_Postprocess && !m_Postprocess(m_Image))
	{
		m_Image.Free();
		m_Error = EAssetLoadError::DECODE;
		return;
	}
	if(State() == IJob::STATE_ABORTED)
	{
		m_Image.Free();
		m_Error = EAssetLoadError::ABORTED;
	}
}

CImageInfo CImageAssetJob::TakeImage()
{
	dbg_assert(State() == IJob::STATE_DONE, "Cannot take image from unfinished asset job");
	dbg_assert(Success(), "Cannot take image from failed asset job");
	return std::move(m_Image);
}

CAssetResource::CAssetResource(std::shared_ptr<CAssetJob> pJob) :
	m_pJob(std::move(pJob))
{
	dbg_assert(m_pJob != nullptr, "Asset resource job must not be null");
}

CAssetJob *CAssetResource::Job()
{
	return m_pJob.get();
}

const CAssetJob *CAssetResource::Job() const
{
	return m_pJob.get();
}

bool CAssetResource::IsFinished() const
{
	return m_pJob != nullptr && m_pJob->Done();
}

bool CAssetResource::IsReady(uint64_t CurrentGeneration) const
{
	return m_pJob != nullptr && m_pJob->State() == IJob::STATE_DONE && m_pJob->Generation() == CurrentGeneration && m_pJob->Success();
}

bool CAssetResource::IsFailed(uint64_t CurrentGeneration) const
{
	return m_pJob != nullptr && m_pJob->State() == IJob::STATE_DONE && m_pJob->Generation() == CurrentGeneration && !m_pJob->Success();
}

bool CAssetResource::IsStale(uint64_t CurrentGeneration) const
{
	return IsFinished() && m_pJob->Generation() != CurrentGeneration;
}

bool CAssetResource::Abort()
{
	return m_pJob != nullptr && m_pJob->Abort();
}

void CAssetResource::Reset()
{
	m_pJob.reset();
}

EAssetType CAssetResource::Type() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no type");
	return m_pJob->Type();
}

const char *CAssetResource::Path() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no path");
	return m_pJob->Path();
}

EHttpAssetSource CAssetResource::HttpSource() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no source");
	return m_pJob->HttpSource();
}

int CAssetResource::OwnerId() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no owner");
	return m_pJob->OwnerId();
}

uint64_t CAssetResource::Generation() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no generation");
	return m_pJob->Generation();
}

uint64_t CAssetResource::RequestId() const
{
	dbg_assert(m_pJob != nullptr, "Empty asset resource has no request ID");
	return m_pJob->RequestId();
}

CImageResource::CImageResource(std::shared_ptr<CImageAssetJob> pJob) :
	CAssetResource(std::move(pJob))
{
}

CImageAssetJob *CImageResource::ImageJob()
{
	return static_cast<CImageAssetJob *>(Job());
}

const CImageAssetJob *CImageResource::ImageJob() const
{
	return static_cast<const CImageAssetJob *>(Job());
}

std::chrono::nanoseconds CImageResource::ReadTime() const
{
	dbg_assert(ImageJob() != nullptr, "Empty image resource has no read time");
	return ImageJob()->ReadTime();
}

std::chrono::nanoseconds CImageResource::DecodeTime() const
{
	dbg_assert(ImageJob() != nullptr, "Empty image resource has no decode time");
	return ImageJob()->DecodeTime();
}

CImageInfo CImageResource::TakeImage()
{
	dbg_assert(ImageJob() != nullptr, "Cannot take an image from an empty resource");
	return ImageJob()->TakeImage();
}
