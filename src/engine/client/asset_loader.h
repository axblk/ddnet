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
#include <string>
#include <type_traits>
#include <vector>

class IEngine;
class IStorage;
class CImageAssetJob;
class CImageResource;
template<typename TJob>
class CTypedAssetResource;

/**
 * Job that prepares an asset from its bytes. The bytes come from a file or
 * the caller, then `Process` runs on the job pool.
 */
class CAssetJob : public IJob
{
	friend class CAssetLoader;

	std::string m_Path;

	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::vector<uint8_t> m_vData;
	bool m_ReadFailed = false;
	bool m_Success = false;

	static bool ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData);

protected:
	CAssetJob(IStorage *pStorage, const char *pPath, int StorageType);
	CAssetJob(std::vector<uint8_t> vData, const char *pContextName);

	/**
	 * Called on a job thread, not called if the file could not be read.
	 */
	virtual bool Process() = 0;

	std::vector<uint8_t> &Data() { return m_vData; }
	const std::vector<uint8_t> &Data() const { return m_vData; }

public:
	void Run() final;

	bool Success() const { return m_Success; }
	const char *Path() const { return m_Path.c_str(); }
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
	std::deque<std::shared_ptr<CAssetJob>> m_vpPendingJobs;
	std::vector<std::shared_ptr<CAssetJob>> m_vpRunningJobs;

	void Submit(std::shared_ptr<CAssetJob> pJob);
	void StartPendingJobs();

public:
	~CAssetLoader() { Shutdown(); }

	void Init(IEngine *pEngine, size_t MaxConcurrentJobs);
	template<typename TJob>
	CTypedAssetResource<TJob> Load(std::shared_ptr<TJob> pJob);
	CImageResource LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess = {});
	CImageResource LoadImageData(std::vector<uint8_t> vData, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess = {});
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
