/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_ASSET_LOADER_H
#define ENGINE_CLIENT_ASSET_LOADER_H

#include <base/dbg.h>

#include <engine/image.h>
#include <engine/shared/jobs.h>

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

	// Reading a file is the same work whatever the bytes turn out to be, so it
	// happens here for every asset and nowhere else. Only what is made of the
	// bytes afterwards depends on the kind of asset, and that is Process.
	IStorage *m_pStorage = nullptr;
	int m_StorageType = 0;
	std::vector<uint8_t> m_vData;
	bool m_ReadFailed = false;
	std::chrono::nanoseconds m_ReadTime{};

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
	 * Reads the file, if there is one, and then makes the asset out of the
	 * bytes. The caller that has no job threads to hand the job to runs it
	 * itself.
	 */
	void Run() final;

	/**
	 * Reads a whole file into memory. The one place in the asset pipeline that
	 * reads from storage, so that every asset is read the same way. The read
	 * time, if it is wanted, is added to whatever the caller already counted.
	 */
	static bool ReadFile(IStorage *pStorage, const char *pPath, int StorageType, std::vector<uint8_t> &vData, std::chrono::nanoseconds *pReadTime = nullptr);

	virtual bool Success() const { return State() == STATE_DONE; }
	/**
	 * How long reading the file took. Zero for a job that brought its bytes.
	 */
	std::chrono::nanoseconds ReadTime() const { return m_ReadTime; }
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
 * Limits how many asset jobs are submitted to the engine job pool at once.
 *
 * The loader does not own committed resources and never calls asset owners.
 * Owners keep typed resource handles, poll completion and reject stale generations.
 */
class CAssetLoader
{
	IEngine *m_pEngine = nullptr;
	size_t m_MaxConcurrentJobs = 0;
	uint64_t m_NextRequestId = 1;
	bool m_Shutdown = false;
	std::vector<std::shared_ptr<CHttpAssetJob>> m_vpWaitingJobs;
	std::deque<std::shared_ptr<CAssetJob>> m_vpPendingJobs;
	std::vector<std::shared_ptr<CAssetJob>> m_vpRunningJobs;

	uint64_t Submit(std::shared_ptr<CAssetJob> pJob);
	uint64_t SubmitHttp(IHttp *pHttp, std::shared_ptr<CHttpAssetJob> pJob);
	void UpdateWaitingJobs();
	void StartPendingJobs();

public:
	void Init(IEngine *pEngine, size_t MaxConcurrentJobs);
	template<typename TJob>
	CTypedAssetResource<TJob> Load(std::shared_ptr<TJob> pJob);
	/**
	 * Downloads and prepares an asset. The request is run immediately, the job
	 * is only submitted to the job pool when the request finished.
	 */
	template<typename TJob>
	CTypedAssetResource<TJob> LoadHttp(IHttp *pHttp, std::shared_ptr<TJob> pJob);
	CImageResource LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {});
	CImageResource LoadImageData(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {});
	/**
	 * Loads an image from the raw data of a datafile item, which contains the
	 * uncompressed pixels of the image instead of an encoded image file. The
	 * data is uncompressed on the job thread.
	 */
	CImageResource LoadImageRawData(CDataFileRawData RawData, size_t Width, size_t Height, CImageInfo::EImageFormat Format, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {});
	/**
	 * Loads an image that is downloaded with the given request, either from
	 * the response or from the file that the request downloads to.
	 *
	 * @see LoadHttp
	 */
	CImageResource LoadImageHttp(IHttp *pHttp, std::shared_ptr<IHttpRequest> pRequest, CHttpAssetDestination Destination, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {});
	/**
	 * Reads a text file - a description, a piece of configuration, a piece of
	 * JSON - off the main thread. What the text means stays with the caller,
	 * which is the only place that knows it.
	 */
	CTypedAssetResource<CTextAssetJob> LoadTextFile(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation);
	void Update();
	void AbortOwnerBeforeGeneration(int OwnerId, uint64_t Generation);
	void Shutdown();

	bool Idle() const { return m_vpWaitingJobs.empty() && m_vpPendingJobs.empty() && m_vpRunningJobs.empty(); }
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

	std::chrono::nanoseconds ReadTime() const;
	std::chrono::nanoseconds DecodeTime() const;
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

template<typename TJob>
CTypedAssetResource<TJob> CAssetLoader::Load(std::shared_ptr<TJob> pJob)
{
	static_assert(std::is_base_of_v<CAssetJob, TJob>);
	Submit(pJob);
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
