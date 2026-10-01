/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_ASSET_LOADER_H
#define ENGINE_CLIENT_ASSET_LOADER_H

#include <base/dbg.h>
#include <base/hash.h>
#include <base/lock.h>
#include <base/sphore.h>

#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/shared/jobs.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

class CDataFileRawData;
class IEngine;
class IHttp;
class IHttpRequest;
class IStorage;
class CAssetResource;
class CImageAssetJob;
class CImageResource;
class CFileAssetJob;
template<typename TJob>
class CTypedAssetResource;

/**
 * How badly an asset is wanted.
 */
enum class EAssetPriority
{
	/**
	 * What is shown next waits for it: it is fetched as soon as it is
	 * submitted, and read and made ahead of every queued asset that is not
	 * urgent. See also `CAssetLoader::Prioritize`.
	 */
	URGENT,
	/**
	 * Somebody wants it soon. Only a few assets that are not urgent are
	 * fetched at a time, so that they do not take every connection a browser
	 * opens to a host and the urgent ones go first; these are next.
	 */
	NORMAL,
	/**
	 * Nobody waits for it: fetched after the others that are not urgent.
	 */
	BACKGROUND,
};

/**
 * Job that prepares an asset from its bytes. The loader gets the bytes from a
 * file or a request, or the job brings them itself, then `Process` runs on the
 * job pool.
 */
class CAssetJob : public IJob
{
	friend class CAssetLoader;
	friend class CAssetResource;

	std::string m_Path;

	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::shared_ptr<IHttpRequest> m_pRequest;
	// Read the file if the request failed
	bool m_UseFileOnError = false;
	// The request is the loader's, for a file the storage has an address for
	bool m_FetchedByLoader = false;
	int m_HttpStatus = 0;
	// The bytes are the response, owned by the request
	bool m_UseResponse = false;
	std::vector<uint8_t> m_vData;
	bool m_ReadFailed = false;
	bool m_Success = false;
	EAssetPriority m_Priority = EAssetPriority::NORMAL;
	// Takes one of the few fetches of assets that are not urgent
	bool m_CountedFetch = false;

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
	 * Takes over the bytes of a job that read a file. The bytes of a fetched
	 * response stay with the request, so the caller gets a copy of them.
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
 * Loads assets without blocking the main thread. Files are read one after the
 * other by a reader thread, or fetched where the storage gives them an
 * address, then at most `MaxConcurrentJobs` jobs run on the job pool.
 */
class CAssetLoader
{
	class CDeferredFetch
	{
	public:
		std::shared_ptr<CAssetJob> m_pJob;
		std::string m_Url;
	};

	IEngine *m_pEngine = nullptr;
	IHttp *m_pHttp = nullptr;
	size_t m_MaxConcurrentJobs = 0;
	bool m_Shutdown = false;
	std::vector<std::shared_ptr<CAssetJob>> m_vpFetchingJobs;
	// Jobs that are not urgent waiting for their turn to be fetched, the
	// normal ones ahead of the background ones
	std::deque<CDeferredFetch> m_vDeferredFetches;
	size_t m_CountedFetches = 0;
	std::deque<std::shared_ptr<CAssetJob>> m_vpPendingJobs;
	std::vector<std::shared_ptr<CAssetJob>> m_vpRunningJobs;

	// What arrives while a report of late assets runs, see `ReportLateAssets`
	std::string m_LateWhat;
	int64_t m_LateStart = 0;
	int64_t m_LateEnd = 0;
	std::vector<std::pair<std::string, int64_t>> m_vLateAssets;
	void NoteFinishedJob(const CAssetJob &Job);
	void FinishLateReport();

	CLock m_ReaderLock;
	CSemaphore m_ReaderSemaphore;
	std::deque<std::shared_ptr<CAssetJob>> m_vpUnreadJobs GUARDED_BY(m_ReaderLock);
	std::vector<std::shared_ptr<CAssetJob>> m_vpReadJobs GUARDED_BY(m_ReaderLock);
	void *m_pReaderThread = nullptr;
	std::atomic<bool> m_ReaderShutdown{false};

	static void ReaderThread(void *pUser);
	// Puts an urgent job behind the urgent ones already queued, any other at the end
	static void Queue(std::deque<std::shared_ptr<CAssetJob>> &vpJobs, std::shared_ptr<CAssetJob> pJob);
	// Moves a job that became urgent ahead in a queue, whether it is in there
	static bool Requeue(std::deque<std::shared_ptr<CAssetJob>> &vpJobs, const std::shared_ptr<CAssetJob> &pJob);
	void ReadLoop() NO_THREAD_SAFETY_ANALYSIS;
	void Submit(std::shared_ptr<CAssetJob> pJob, EAssetPriority Priority = EAssetPriority::NORMAL) REQUIRES(!m_ReaderLock);
	void Enqueue(std::shared_ptr<CAssetJob> pJob) REQUIRES(!m_ReaderLock);
	bool StartFetching(const std::shared_ptr<CAssetJob> &pJob);
	void Fetch(const std::shared_ptr<CAssetJob> &pJob, const char *pUrl);
	void StartDeferredFetches();
	void UpdateFetchingJobs() REQUIRES(!m_ReaderLock);
	void UpdateReadJobs() REQUIRES(!m_ReaderLock);
	void StartPendingJobs();

public:
	~CAssetLoader() NO_THREAD_SAFETY_ANALYSIS { Shutdown(); }

	/**
	 * @param pEngine Engine whose job pool makes the assets.
	 * @param MaxConcurrentJobs How many assets are made at once.
	 * @param pHttp Fetches the files the storage has an address for, which
	 * are those of the browser's data directory. Without it every file is read.
	 */
	void Init(IEngine *pEngine, size_t MaxConcurrentJobs, IHttp *pHttp = nullptr);
	template<typename TJob>
	CTypedAssetResource<TJob> Load(std::shared_ptr<TJob> pJob, EAssetPriority Priority = EAssetPriority::NORMAL) REQUIRES(!m_ReaderLock);
	CTypedAssetResource<CFileAssetJob> LoadFile(IStorage *pStorage, const char *pPath, int StorageType) REQUIRES(!m_ReaderLock);
	CImageResource LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, std::function<bool(CImageInfo &)> Postprocess = {}, EAssetPriority Priority = EAssetPriority::NORMAL) REQUIRES(!m_ReaderLock);
	CImageResource LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, std::function<bool(CImageInfo &)> Postprocess = {}, EAssetPriority Priority = EAssetPriority::NORMAL) REQUIRES(!m_ReaderLock);
	/**
	 * Runs the request and loads the image from the response. The image is
	 * read from `pPath` instead if the response is not in memory, if the
	 * status is 304 Not Modified, or if the request failed and
	 * `UseFileOnError` is set.
	 */
	CImageResource LoadImageHttp(IHttp *pHttp, std::shared_ptr<IHttpRequest> pRequest, IStorage *pStorage, const char *pPath, int StorageType, bool UseFileOnError, std::function<bool(CImageInfo &)> Postprocess = {}) REQUIRES(!m_ReaderLock);
	void Update() REQUIRES(!m_ReaderLock);
	void Shutdown() REQUIRES(!m_ReaderLock);
	/**
	 * Makes a load urgent, see `EAssetPriority::URGENT`, wherever it stands:
	 * one that waits for its turn to be fetched is fetched now, and a queued
	 * one moves ahead of those that are not urgent. For what somebody
	 * started to load early and now waits for, see
	 * `CCriticalAssets::Prioritize`.
	 */
	void Prioritize(const CAssetResource &Resource) REQUIRES(!m_ReaderLock);

	/**
	 * Notes every asset that finishes from now on for a while, and logs them
	 * in one line when the while is over. Called when something is first
	 * drawn: an asset that arrives after it can pop in. A report that still
	 * runs is logged first.
	 *
	 * @param pWhat What was drawn, for the log.
	 * @param Window For how long assets are noted.
	 */
	void ReportLateAssets(const char *pWhat, std::chrono::nanoseconds Window);
	/**
	 * Logs a report of late assets that still runs, when something else
	 * begins to load: what arrives from then on is what that loads.
	 */
	void EndLateReport();
};

/**
 * Handle of a load. Dropping or replacing it aborts a load that has not
 * finished, and a finished result goes with it.
 */
class CAssetResource
{
	friend class CAssetLoader;

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
	/**
	 * The SHA-256 of the bytes a ready asset was made from, so that nobody
	 * reads the file again to hash it, which in a browser fetches it once
	 * more. Only for an asset that was read or fetched.
	 */
	SHA256_DIGEST SourceSha256() const;
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

class CFileAssetJob final : public CAssetJob
{
protected:
	bool Process() override { return true; }

public:
	CFileAssetJob(IStorage *pStorage, const char *pPath, int StorageType);

	std::vector<uint8_t> TakeBytes();
	std::string_view Text() const;
};

/**
 * Textures of files that are loaded when they are first used rather than
 * ahead, such as the pictures of a tool that only some of its views draw.
 * Where the storage reads a file, it is loaded at once, as
 * `IGraphics::LoadTexture` does; where it fetches the file (see
 * `IStorage::FetchUrl`), it is fetched through the asset loader, and the
 * texture stays as it was until the image arrives - invalid for a new one.
 */
class COnDemandTextures
{
	class CLoad
	{
	public:
		int m_Flags = 0;
		CImageResource m_Resource;
	};
	IGraphics *m_pGraphics = nullptr;
	IStorage *m_pStorage = nullptr;
	CAssetLoader *m_pLoader = nullptr;
	// Only for the textures that are fetched. One that failed keeps its
	// entry, so that it is not fetched again every time it is used.
	std::map<IGraphics::CTextureHandle *, CLoad> m_Loads;

public:
	void Init(IGraphics *pGraphics, IStorage *pStorage, CAssetLoader *pLoader);
	/**
	 * Loads a file into a texture, unless that is done or under way.
	 *
	 * @param Texture The texture, which has to outlive its load.
	 * @param pPath The file, of any storage type.
	 * @param Flags How to load it, see `IGraphics::TEXLOAD_*`.
	 *
	 * @return The texture, not valid while it is fetched.
	 */
	IGraphics::CTextureHandle Get(IGraphics::CTextureHandle &Texture, const char *pPath, int Flags = 0);
	/**
	 * Loads another file into a texture. One that is fetched replaces the
	 * texture when it arrives.
	 */
	void Replace(IGraphics::CTextureHandle &Texture, const char *pPath, int Flags = 0);
	/**
	 * Stops loading into a texture, which is set another way.
	 */
	void Forget(IGraphics::CTextureHandle &Texture);
	/**
	 * Uploads the images that arrived. `Get` does so for its own texture as
	 * well.
	 */
	void Update();
	/**
	 * Whether a texture is fetched and has nothing to show meanwhile.
	 */
	bool IsWaiting() const;
};

template<typename TJob>
CTypedAssetResource<TJob> CAssetLoader::Load(std::shared_ptr<TJob> pJob, EAssetPriority Priority)
{
	static_assert(std::is_base_of_v<CAssetJob, TJob>);
	Submit(pJob, Priority);
	return CTypedAssetResource<TJob>(std::move(pJob));
}

#endif
