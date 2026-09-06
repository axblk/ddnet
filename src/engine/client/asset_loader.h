/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_ASSET_LOADER_H
#define ENGINE_CLIENT_ASSET_LOADER_H

#include <base/dbg.h>

#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/shared/jobs.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

class CDataFileRawData;
class IEngine;
class IHttp;
class IHttpRequest;
class IStorage;
class CImageAssetJob;
class CImageResource;
template<typename TJob>
class CTypedAssetResource;

/**
 * Job that prepares an asset from its bytes. The bytes come from a file or a
 * request, or the job brings them itself, then `Process` runs on the job pool.
 */
class CAssetJob : public IJob
{
	friend class CAssetLoader;

	std::string m_Path;

	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::shared_ptr<IHttpRequest> m_pRequest;
	// Read the file if the request failed
	bool m_UseFileOnError = false;
	int m_HttpStatus = 0;
	// The bytes are the response, owned by the request
	bool m_UseResponse = false;
	std::vector<uint8_t> m_vData;
	bool m_ReadFailed = false;
	bool m_Success = false;

	static bool ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData);

protected:
	CAssetJob(IStorage *pStorage, const char *pPath, int StorageType);
	// For jobs that hold their input themselves
	explicit CAssetJob(const char *pContextName);

	/**
	 * Called on a job thread, not called if the file could not be read.
	 */
	virtual bool Process() = 0;

	/**
	 * The bytes to process. They live as long as the job.
	 */
	std::span<const uint8_t> Data() const;
	/**
	 * Takes over the bytes of a job that read a file.
	 */
	std::vector<uint8_t> TakeData();

public:
	void Run() final;
	bool Abort() override;

	bool Success() const { return m_Success; }
	const char *Path() const { return m_Path.c_str(); }
	int HttpStatus() const { return m_HttpStatus; }
};

/**
 * Loads assets without blocking the main thread. At most `MaxConcurrentJobs`
 * jobs run on the job pool.
 */
class CAssetLoader
{
	IEngine *m_pEngine = nullptr;
	size_t m_MaxConcurrentJobs = 0;
	bool m_Shutdown = false;
	std::vector<std::shared_ptr<CAssetJob>> m_vpFetchingJobs;
	std::deque<std::shared_ptr<CAssetJob>> m_vpPendingJobs;
	std::vector<std::shared_ptr<CAssetJob>> m_vpRunningJobs;

	void Submit(std::shared_ptr<CAssetJob> pJob);
	void Enqueue(std::shared_ptr<CAssetJob> pJob);
	void UpdateFetchingJobs();
	void StartPendingJobs();

public:
	~CAssetLoader() { Shutdown(); }

	void Init(IEngine *pEngine, size_t MaxConcurrentJobs);
	template<typename TJob>
	CTypedAssetResource<TJob> Load(std::shared_ptr<TJob> pJob);
	CImageResource LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess = {});
	CImageResource LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess = {});
	/**
	 * Runs the request and loads the image from the response. The image is
	 * read from `pPath` instead if the response is not in memory, if the
	 * status is 304 Not Modified, or if the request failed and
	 * `UseFileOnError` is set.
	 */
	CImageResource LoadImageHttp(IHttp *pHttp, std::shared_ptr<IHttpRequest> pRequest, IStorage *pStorage, const char *pPath, int StorageType, bool UseFileOnError, std::function<bool(CImageInfo &)> Postprocess = {});
	void Update();
	void Shutdown();
};

/**
 * Handle of a load. Dropping or replacing it aborts a load that has not
 * finished, and a finished result goes with it.
 */
class CAssetResource
{
	std::shared_ptr<CAssetJob> m_pJob;

protected:
	explicit CAssetResource(std::shared_ptr<CAssetJob> pJob);
	CAssetJob *Job() { return m_pJob.get(); }
	const CAssetJob *Job() const { return m_pJob.get(); }

public:
	CAssetResource() = default;
	CAssetResource(CAssetResource &&Other) noexcept = default;
	CAssetResource &operator=(CAssetResource &&Other) noexcept;
	~CAssetResource() { Reset(); }

	explicit operator bool() const { return m_pJob != nullptr; }
	bool IsFinished() const;
	bool IsReady() const;
	bool IsFailed() const;
	bool Abort();
	void Reset();
	const char *Path() const;
	/**
	 * @return Status code of the request, `0` if there was none or it failed.
	 */
	int HttpStatus() const;
};

template<typename TJob>
class CTypedAssetResource final : public CAssetResource
{
	friend class CAssetLoader;

	explicit CTypedAssetResource(std::shared_ptr<TJob> pJob) :
		CAssetResource(std::move(pJob))
	{
	}

public:
	CTypedAssetResource() = default;

	TJob &Result()
	{
		dbg_assert(Job() != nullptr && Job()->State() == IJob::STATE_DONE && Job()->Success(), "Asset resource result is not ready");
		return *static_cast<TJob *>(Job());
	}

	const TJob &Result() const
	{
		dbg_assert(Job() != nullptr && Job()->State() == IJob::STATE_DONE && Job()->Success(), "Asset resource result is not ready");
		return *static_cast<const TJob *>(Job());
	}
};

class CImageResource final : public CAssetResource
{
	friend class CAssetLoader;

	explicit CImageResource(std::shared_ptr<CImageAssetJob> pJob);

public:
	CImageResource() = default;

	CImageInfo TakeImage();

	/**
	 * Replaces `Texture` once the image is loaded, logs errors and resets the
	 * resource.
	 *
	 * @return `true` if the resource finished.
	 */
	bool FinishTexture(IGraphics *pGraphics, IGraphics::CTextureHandle &Texture, int Flags = 0);
};

template<typename TJob>
CTypedAssetResource<TJob> CAssetLoader::Load(std::shared_ptr<TJob> pJob)
{
	static_assert(std::is_base_of_v<CAssetJob, TJob>);
	Submit(pJob);
	return CTypedAssetResource<TJob>(std::move(pJob));
}

#endif
