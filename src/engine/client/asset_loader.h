/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_ASSET_LOADER_H
#define ENGINE_CLIENT_ASSET_LOADER_H

#include <base/dbg.h>
#include <base/lock.h>
#include <base/sphore.h>

#include <engine/image.h>
#include <engine/shared/jobs.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

class CDataFileRawData;
class IEngine;
class IHttp;
class IHttpRequest;
class IStorage;
class CImageAssetJob;
class CImageResource;
class CTextAssetJob;
template<typename TJob>
class CTypedAssetResource;

enum class EAssetType
{
	/**
	 * Bytes that are used as they are, such as a map file.
	 */
	DATA,
	FONT,
	IMAGE,
	SOUND,
	TEXT,
};

/**
 * Where the data of a downloaded asset was taken from.
 */
enum class EHttpAssetSource
{
	/**
	 * Nothing could be loaded, because the request failed and the destination
	 * file cannot be used.
	 */
	NONE,
	/**
	 * The response of the request.
	 */
	RESPONSE,
	/**
	 * The file that the request downloads to.
	 */
	DESTINATION,
};

/**
 * The file that a downloaded asset is loaded from when the response does not
 * contain the data, either because the request only writes the data to this
 * file or because the file is still up to date.
 */
class CHttpAssetDestination
{
public:
	CHttpAssetDestination() = default;
	CHttpAssetDestination(IStorage *pStorage, const char *pPath, int StorageType, bool UseOnError);

	IStorage *m_pStorage = nullptr;
	std::string m_Path;
	int m_StorageType = 0;
	/**
	 * Whether the file is also used when the request failed, for example to
	 * fall back to a previously downloaded asset.
	 */
	bool m_UseOnError = false;
};

/**
 * Owner of the asset jobs that the client core starts for itself. The owners
 * of the game client are counted from zero (`CGameClient::EAssetOwner`), so
 * whoever loads from below the game client counts down from here instead.
 */
constexpr int ASSET_OWNER_CLIENT_CORE = -1;

/**
 * How badly an asset is wanted.
 */
enum class EAssetPriority
{
	/**
	 * Somebody is waiting for it: it is fetched as soon as it is submitted.
	 */
	NORMAL,
	/**
	 * Nobody is waiting for it. Only a few of these are fetched at a time, so
	 * that a hundred files that nobody waits for cannot take every connection
	 * a browser opens to a host away from the one file that somebody does.
	 */
	BACKGROUND,
};

/**
 * Base job for asynchronously reading and preparing an asset.
 *
 * Jobs only own CPU-side input and results. Consumers poll the job state and
 * commit successful results on the responsible main, graphics or sound thread.
 */
class CAssetJob : public IJob
{
	friend class CAssetLoader;

	EAssetType m_Type;
	std::string m_Path;
	int m_OwnerId;
	uint64_t m_Generation;
	uint64_t m_RequestId = 0;

	// Where the bytes come from: a file, a request, or the caller's own hands.
	// Fetching them is the same work whatever they turn out to be, so it is a
	// stage of its own before the job pool - see the loader. Only what is made
	// of them afterwards depends on the kind of asset, and that is Process.
	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::vector<uint8_t> m_vData;
	bool m_ReadFailed = false;
	bool m_Background = false;

protected:
	/**
	 * A job whose bytes are read from a file.
	 */
	CAssetJob(EAssetType Type, IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation);
	/**
	 * A job whose bytes the caller already has, or that has no bytes of its
	 * own because it reads several files itself. The context name takes the
	 * place of the path, for logging and for the owner to recognize the job by.
	 */
	CAssetJob(EAssetType Type, std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation);

	/**
	 * Makes the asset out of the bytes in `Data`, on a job thread.
	 *
	 * Only called when there was something to read, or when the job brought
	 * its own bytes.
	 */
	virtual void Process() = 0;

	/**
	 * Called instead of `Process` when the file could not be read, for jobs
	 * that tell that apart from a file they could not make sense of.
	 */
	virtual void OnReadFailed() {}

	std::vector<uint8_t> &Data() { return m_vData; }
	const std::vector<uint8_t> &Data() const { return m_vData; }
	void SetData(std::vector<uint8_t> vData);

	/**
	 * Points the job at a file instead of at bytes it was given, which the
	 * HTTP path decides only once the request finished.
	 *
	 * @remark Must only be called before the job is submitted to the job pool.
	 */
	void SetSourceFile(IStorage *pStorage, const char *pPath, int StorageType);

	/**
	 * Sets the path of the asset when the source of the asset is only decided
	 * after the job was created, so that the path describes what was loaded.
	 *
	 * @remark Must only be called before the job is submitted to the job pool.
	 */
	void SetPath(const char *pPath);

public:
	/**
	 * Makes the asset out of the bytes the job was given. The caller that has
	 * no job threads to hand the job to runs it itself.
	 */
	void Run() final;

	/**
	 * Whether the bytes of this job still have to be read from a file.
	 */
	bool HasFileSource() const { return m_pStorage != nullptr; }

	/**
	 * Fetches the bytes of a job that reads a file, on whichever thread the
	 * loader reads with. Never called by the job pool: how many files are read
	 * at once is the loader's to decide and has nothing to do with how many
	 * assets are being made at once.
	 */
	void ReadSource();

	/**
	 * Reads a whole file into memory. The one place in the asset pipeline that
	 * reads from storage, so that every asset is read the same way. The read
	 * time, if it is wanted, is added to whatever the caller already counted.
	 */
	static bool ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData);

	virtual bool Success() const { return State() == STATE_DONE; }
	/**
	 * How long reading the file took. Zero for a job that brought its bytes.
	 */
	EAssetType Type() const { return m_Type; }
	const char *Path() const { return m_Path.c_str(); }
	int OwnerId() const { return m_OwnerId; }
	uint64_t Generation() const { return m_Generation; }
	uint64_t RequestId() const { return m_RequestId; }
	virtual EHttpAssetSource HttpSource() const { return EHttpAssetSource::NONE; }
};

/**
 * Base job for an asset that is downloaded before it is prepared.
 *
 * The loader runs the request, polls it and only submits the job to the job
 * pool when the request finished, so that no job thread waits for the network.
 * Aborting the job also aborts the request.
 */
class CHttpAssetJob : public CAssetJob
{
	friend class CAssetLoader;

	std::shared_ptr<IHttpRequest> m_pRequest;
	CHttpAssetDestination m_Destination;
	EHttpAssetSource m_HttpSource = EHttpAssetSource::NONE;

protected:
	/**
	 * @param Type Kind of asset that the job prepares.
	 * @param pStorage Storage that the file is read from.
	 * @param pPath Name of the asset, for logging and for the owner to
	 * recognize it by.
	 * @param StorageType Storage type that the file is read from.
	 * @param pRequest Request that downloads the asset. Jobs which are not
	 * loaded with `CAssetLoader::LoadHttp` have no request.
	 * @param Destination File that the asset is loaded from when the response
	 * does not contain the data.
	 * @param OwnerId Owner that the job is aborted with.
	 * @param Generation Generation of the owner that requested the asset.
	 */
	CHttpAssetJob(EAssetType Type, IStorage *pStorage, const char *pPath, int StorageType, std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, int OwnerId, uint64_t Generation);
	CHttpAssetJob(EAssetType Type, std::vector<uint8_t> vData, const char *pContextName, std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, int OwnerId, uint64_t Generation);

	/**
	 * Called on the main thread when the request finished and before the job
	 * is submitted to the job pool, so the job can use the downloaded data.
	 *
	 * @param Source Where the data of the asset must be taken from.
	 * @param vData Data of the response, only set for `EHttpAssetSource::RESPONSE`.
	 */
	virtual void OnRequestFinished(EHttpAssetSource Source, std::vector<uint8_t> vData) = 0;

	const CHttpAssetDestination &Destination() const { return m_Destination; }

public:
	bool Abort() override;
	EHttpAssetSource HttpSource() const override { return m_HttpSource; }
	IHttpRequest *Request() { return m_pRequest.get(); }
};

/**
 * Fetches the bytes of assets and hands them to the engine job pool.
 *
 * Fetching and making sense of what was fetched are two different kinds of
 * work with two different right answers for how many at once, so they are two
 * stages here. A file is read by one thread, one file after the next: a hard
 * disk that is made to seek between two reads is slower than one that is not,
 * and a solid state disk gains next to nothing from being asked twice over. A
 * request is the opposite - `IHttp` runs it without a thread of ours, in the
 * browser through the browser's own fetch, so any number can be in flight and
 * the network is used for what it is. Bytes the caller already holds need no
 * stage at all. Only what is made of the bytes afterwards goes to the job
 * pool, and that is what `MaxConcurrentJobs` counts.
 *
 * The loader does not own committed resources and never calls asset owners.
 * Owners keep typed resource handles, poll completion and reject stale generations.
 */
class CAssetLoader
{
	IEngine *m_pEngine = nullptr;
	IHttp *m_pHttp = nullptr;
	size_t m_MaxConcurrentJobs = 0;
	uint64_t m_NextRequestId = 1;
	bool m_Shutdown = false;
	std::vector<std::shared_ptr<CHttpAssetJob>> m_vpWaitingJobs;
	std::deque<std::shared_ptr<CAssetJob>> m_vpPendingJobs;
	std::vector<std::shared_ptr<CAssetJob>> m_vpRunningJobs;

	// A job whose file can be fetched instead of read, with the request that
	// fetches it. There is no limit on how many of these are in flight: the
	// request costs no thread of ours, and the platform where files are
	// fetched is the one where waiting for one at a time hurts most.
	class CFetchingJob
	{
	public:
		std::shared_ptr<CAssetJob> m_pJob;
		std::shared_ptr<IHttpRequest> m_pRequest;
	};
	std::vector<CFetchingJob> m_vFetchingJobs;
	// Background jobs whose turn to be fetched has not come yet, in the order
	// they were submitted in.
	std::deque<std::shared_ptr<CAssetJob>> m_vpDeferredFetchJobs;
	size_t m_BackgroundFetchCount = 0;

	// The one reader, started when the first file is asked for. It takes jobs
	// off the front of the queue, reads them one at a time and puts them back
	// for `Update` to pass on to the job pool. A thread of its own and not a
	// job, because a job would be queued behind the very work it feeds.
	CLock m_ReaderLock;
	CSemaphore m_ReaderSemaphore;
	std::deque<std::shared_ptr<CAssetJob>> m_vpUnreadJobs GUARDED_BY(m_ReaderLock);
	std::vector<std::shared_ptr<CAssetJob>> m_vpReadJobs GUARDED_BY(m_ReaderLock);
	void *m_pReaderThread = nullptr;
	std::atomic<bool> m_ReaderShutdown{false};

	static void ReaderThread(void *pUser);
	void ReadLoop() NO_THREAD_SAFETY_ANALYSIS;
	void Enqueue(std::shared_ptr<CAssetJob> pJob) REQUIRES(!m_ReaderLock);
	bool StartFetching(const std::shared_ptr<CAssetJob> &pJob);
	void Fetch(const std::shared_ptr<CAssetJob> &pJob, const char *pUrl);
	void StartDeferredFetches();
	void UpdateFetchingJobs() REQUIRES(!m_ReaderLock);
	void UpdateReadJobs() REQUIRES(!m_ReaderLock);

	uint64_t Submit(std::shared_ptr<CAssetJob> pJob, EAssetPriority Priority) REQUIRES(!m_ReaderLock);
	uint64_t SubmitHttp(IHttp *pHttp, std::shared_ptr<CHttpAssetJob> pJob);
	void UpdateWaitingJobs() REQUIRES(!m_ReaderLock);
	void StartPendingJobs();

public:
	~CAssetLoader() NO_THREAD_SAFETY_ANALYSIS { Shutdown(); }

	/**
	 * @param pEngine Engine whose job pool makes the assets.
	 * @param MaxConcurrentJobs How many assets are made at once.
	 * @param pHttp Used where a file can be fetched rather than read, which is
	 * the browser. Without it every file is read, which still works there and
	 * is simply slower.
	 */
	void Init(IEngine *pEngine, size_t MaxConcurrentJobs, IHttp *pHttp = nullptr);
	template<typename TJob>
	CTypedAssetResource<TJob> Load(std::shared_ptr<TJob> pJob, EAssetPriority Priority = EAssetPriority::NORMAL) REQUIRES(!m_ReaderLock);
	/**
	 * Downloads and prepares an asset. The request is run immediately, the job
	 * is only submitted to the job pool when the request finished.
	 */
	template<typename TJob>
	CTypedAssetResource<TJob> LoadHttp(IHttp *pHttp, std::shared_ptr<TJob> pJob) REQUIRES(!m_ReaderLock);
	CImageResource LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {}) REQUIRES(!m_ReaderLock);
	CImageResource LoadImageData(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {}) REQUIRES(!m_ReaderLock);
	/**
	 * Loads an image from the raw data of a datafile item, which contains the
	 * uncompressed pixels of the image instead of an encoded image file. The
	 * data is uncompressed on the job thread.
	 */
	CImageResource LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {}) REQUIRES(!m_ReaderLock);
	/**
	 * Loads an image that is downloaded with the given request, either from
	 * the response or from the file that the request downloads to.
	 *
	 * @see LoadHttp
	 */
	CImageResource LoadImageHttp(IHttp *pHttp, std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {}) REQUIRES(!m_ReaderLock);
	/**
	 * Reads a text file - a description, a piece of configuration, a piece of
	 * JSON - off the main thread. What the text means stays with the caller,
	 * which is the only place that knows it.
	 */
	CTypedAssetResource<CTextAssetJob> LoadTextFile(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation) REQUIRES(!m_ReaderLock);
	void Update() REQUIRES(!m_ReaderLock);
	void AbortOwnerBeforeGeneration(int OwnerId, uint64_t Generation) REQUIRES(!m_ReaderLock);
	void Shutdown() REQUIRES(!m_ReaderLock);

	bool Idle() const REQUIRES(!m_ReaderLock) { return m_vpWaitingJobs.empty() && m_vFetchingJobs.empty() && m_vpDeferredFetchJobs.empty() && m_vpPendingJobs.empty() && m_vpRunningJobs.empty() && ReadingCount() == 0; }
	size_t ReadingCount() const REQUIRES(!m_ReaderLock);
	size_t WaitingCount() const { return m_vpWaitingJobs.size(); }
	size_t PendingCount() const { return m_vpPendingJobs.size(); }
	size_t RunningCount() const { return m_vpRunningJobs.size(); }
	size_t MaxConcurrentJobs() const { return m_MaxConcurrentJobs; }
};

class CAssetResource
{
	std::shared_ptr<CAssetJob> m_pJob;

protected:
	explicit CAssetResource(std::shared_ptr<CAssetJob> pJob);
	CAssetJob *Job();
	const CAssetJob *Job() const;

public:
	CAssetResource() = default;

	explicit operator bool() const { return m_pJob != nullptr; }
	bool IsFinished() const;
	bool IsReady(uint64_t CurrentGeneration) const;
	bool IsFailed(uint64_t CurrentGeneration) const;
	bool IsStale(uint64_t CurrentGeneration) const;
	bool Abort();
	void Reset();
	EAssetType Type() const;
	const char *Path() const;
	/**
	 * Returns where the data of a downloaded asset was taken from.
	 */
	EHttpAssetSource HttpSource() const;
	int OwnerId() const;
	uint64_t Generation() const;
	uint64_t RequestId() const;
};

/**
 * Stable handle for a consumer-specific asset job.
 */
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

/**
 * Stable handle for one asynchronously loaded image.
 *
 * Callers only observe resource state and consume the decoded image when it is
 * ready. The underlying engine job stays an implementation detail.
 */
class CImageResource final : public CAssetResource
{
	friend class CAssetLoader;

	explicit CImageResource(std::shared_ptr<CImageAssetJob> pJob);
	CImageAssetJob *ImageJob();
	const CImageAssetJob *ImageJob() const;

public:
	CImageResource() = default;

	CImageInfo TakeImage();
};

/**
 * Job that reads a whole text file into memory.
 *
 * Reading a file is what costs, and it is the same work whatever the text
 * turns out to mean, so the job stops at the text. Interpreting it - parsing
 * JSON, resolving what it names - happens where the meaning is known.
 */
class CTextAssetJob final : public CAssetJob
{
	bool m_Ok = false;

protected:
	// The bytes that were read are the text, so there is nothing left to make
	// of them. What the text means is the caller's to decide.
	void Process() override { m_Ok = true; }

public:
	CTextAssetJob(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation);

	bool Success() const override { return m_Ok; }
	/**
	 * The text that was read. Only valid once the job succeeded.
	 */
	std::string_view Text() const;
};

/**
 * Job for a file whose bytes are the asset, such as a map.
 *
 * The point of loading one through here is not what is made of the bytes -
 * nothing is - but where they come from: the loader reads them off the main
 * thread, and in the browser it fetches them the same way as every other
 * asset.
 */
class CDataAssetJob final : public CAssetJob
{
	bool m_Ok = false;

protected:
	// The bytes that were read are the asset, so there is nothing left to make
	// of them.
	void Process() override { m_Ok = true; }

public:
	CDataAssetJob(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation);

	bool Success() const override { return m_Ok; }
	/**
	 * The bytes that were read. Only valid once the job succeeded.
	 */
	const std::vector<uint8_t> &Bytes() const;
};

template<typename TJob>
CTypedAssetResource<TJob> CAssetLoader::Load(std::shared_ptr<TJob> pJob, EAssetPriority Priority)
{
	static_assert(std::is_base_of_v<CAssetJob, TJob>);
	Submit(pJob, Priority);
	return CTypedAssetResource<TJob>(std::move(pJob));
}

template<typename TJob>
CTypedAssetResource<TJob> CAssetLoader::LoadHttp(IHttp *pHttp, std::shared_ptr<TJob> pJob)
{
	static_assert(std::is_base_of_v<CHttpAssetJob, TJob>);
	SubmitHttp(pHttp, pJob);
	return CTypedAssetResource<TJob>(std::move(pJob));
}

#endif
