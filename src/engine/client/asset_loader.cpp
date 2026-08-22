/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "asset_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/storage.h>

#include <algorithm>
#include <limits>

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

class CImageAssetJob final : public CAssetJob
{
	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::vector<uint8_t> m_vData;
	CImageInfo m_Image;
	std::function<bool(CImageInfo &)> m_Postprocess;
	EAssetLoadError m_Error = EAssetLoadError::NONE;
	int m_PngliteIncompatible = 0;
	std::chrono::nanoseconds m_ReadTime{};
	std::chrono::nanoseconds m_DecodeTime{};

protected:
	void Run() override;

public:
	CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess);
	CImageAssetJob(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess);

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

bool CAssetJob::Abort()
{
	return AbortQueued();
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

CImageResource CAssetLoader::LoadImage(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(pStorage, pPath, StorageType, OwnerId, Generation, std::move(Postprocess));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImage(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(vData), pContextName, OwnerId, Generation, std::move(Postprocess));
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
	for(const auto &pJob : m_vpPendingJobs)
		pJob->Abort();
	for(const auto &pJob : m_vpRunningJobs)
		pJob->Abort();
	m_vpPendingJobs.clear();
	m_vpRunningJobs.clear();
}

CImageAssetJob::CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(EAssetType::IMAGE, pPath, OwnerId, Generation),
	m_pStorage(pStorage),
	m_StorageType(StorageType),
	m_Postprocess(std::move(Postprocess))
{
	dbg_assert(pStorage != nullptr, "Image asset storage must not be null");
}

CImageAssetJob::CImageAssetJob(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(EAssetType::IMAGE, pContextName, OwnerId, Generation),
	m_vData(std::move(vData)),
	m_Postprocess(std::move(Postprocess))
{
}

void CImageAssetJob::Run()
{
	if(State() == IJob::STATE_ABORTED)
	{
		m_Error = EAssetLoadError::ABORTED;
		return;
	}
	if(m_pStorage != nullptr)
	{
		IOHANDLE File = m_pStorage->OpenFile(Path(), IOFLAG_READ, m_StorageType);
		if(!File)
		{
			m_Error = EAssetLoadError::NOT_FOUND;
			return;
		}
		if(!CImageLoader::LoadPngTimed(File, Path(), m_Image, m_PngliteIncompatible, m_ReadTime, m_DecodeTime, false))
		{
			m_Error = EAssetLoadError::DECODE;
			return;
		}
	}
	else
	{
		CByteBufferReader Reader(m_vData.data(), m_vData.size());
		const auto DecodeStart = time_get_nanoseconds();
		const bool Success = CImageLoader::LoadPng(Reader, Path(), m_Image, m_PngliteIncompatible, false);
		m_DecodeTime = time_get_nanoseconds() - DecodeStart;
		if(!Success)
		{
			m_Error = EAssetLoadError::DECODE;
			return;
		}
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
