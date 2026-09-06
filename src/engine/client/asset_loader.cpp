/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "asset_loader.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/log.h>
#include <base/mem.h>

#include <engine/engine.h>
#include <engine/gfx/image_loader.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <algorithm>
#include <limits>
#include <utility>

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
	CImageAssetJob(std::vector<uint8_t> vData, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess);
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

CAssetJob::CAssetJob(std::vector<uint8_t> vData, const char *pContextName) :
	m_Path(pContextName),
	m_vData(std::move(vData))
{
	Abortable(true);
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
	if(m_pStorage != nullptr && !ReadFile(m_pStorage, Path(), m_StorageType, m_vData))
		m_ReadFailed = true;
	m_Success = !m_ReadFailed && Process();
}

void CAssetLoader::Init(IEngine *pEngine, size_t MaxConcurrentJobs)
{
	dbg_assert(m_pEngine == nullptr, "Asset loader already initialized");
	dbg_assert(MaxConcurrentJobs > 0, "Asset loader needs at least one concurrent job");
	m_pEngine = pEngine;
	m_MaxConcurrentJobs = MaxConcurrentJobs;
}

void CAssetLoader::Submit(std::shared_ptr<CAssetJob> pJob)
{
	dbg_assert(m_pEngine != nullptr, "Asset loader not initialized");
	if(m_Shutdown)
	{
		pJob->Abort();
		return;
	}
	m_vpPendingJobs.push_back(std::move(pJob));
	StartPendingJobs();
}

CImageResource CAssetLoader::LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(pStorage, pPath, StorageType, std::move(Postprocess));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageData(std::vector<uint8_t> vData, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(vData), pContextName, std::move(Postprocess));
	Submit(pJob);
	return CImageResource(std::move(pJob));
}

CImageResource CAssetLoader::LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess)
{
	auto pJob = std::make_shared<CImageAssetJob>(std::move(RawData), Width, Height, Format, pContextName, std::move(Postprocess));
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
	m_vpRunningJobs.erase(
		std::remove_if(m_vpRunningJobs.begin(), m_vpRunningJobs.end(), [](const auto &pJob) { return pJob->Done(); }),
		m_vpRunningJobs.end());
	StartPendingJobs();
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

CImageAssetJob::CImageAssetJob(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(pStorage, pPath, StorageType),
	m_Postprocess(std::move(Postprocess))
{
}

CImageAssetJob::CImageAssetJob(std::vector<uint8_t> vData, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(std::move(vData), pContextName),
	m_Postprocess(std::move(Postprocess))
{
}

CImageAssetJob::CImageAssetJob(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess) :
	CAssetJob(std::vector<uint8_t>(), pContextName),
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
