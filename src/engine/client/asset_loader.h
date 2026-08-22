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
#include <type_traits>
#include <vector>

class IEngine;
class IStorage;
class CImageAssetJob;
class CImageResource;
template<typename TJob>
class CTypedAssetResource;

enum class EAssetType
{
	IMAGE,
	SOUND,
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

protected:
	CAssetJob(EAssetType Type, const char *pPath, int OwnerId, uint64_t Generation);

public:
	virtual bool Success() const { return State() == STATE_DONE; }
	EAssetType Type() const { return m_Type; }
	const char *Path() const { return m_Path.c_str(); }
	int OwnerId() const { return m_OwnerId; }
	uint64_t Generation() const { return m_Generation; }
	uint64_t RequestId() const { return m_RequestId; }
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
	std::deque<std::shared_ptr<CAssetJob>> m_vpPendingJobs;
	std::vector<std::shared_ptr<CAssetJob>> m_vpRunningJobs;

	uint64_t Submit(std::shared_ptr<CAssetJob> pJob);
	void StartPendingJobs();

public:
	void Init(IEngine *pEngine, size_t MaxConcurrentJobs);
	template<typename TJob>
	CTypedAssetResource<TJob> Load(std::shared_ptr<TJob> pJob);
	CImageResource LoadImageFile(IStorage *pStorage, const char *pPath, int StorageType, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {});
	CImageResource LoadImageData(std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation, std::function<bool(CImageInfo &)> Postprocess = {});
	void Update();
	void AbortOwnerBeforeGeneration(int OwnerId, uint64_t Generation);
	void Shutdown();

	bool Idle() const { return m_vpPendingJobs.empty() && m_vpRunningJobs.empty(); }
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

template<typename TJob>
CTypedAssetResource<TJob> CAssetLoader::Load(std::shared_ptr<TJob> pJob)
{
	static_assert(std::is_base_of_v<CAssetJob, TJob>);
	Submit(pJob);
	return CTypedAssetResource<TJob>(std::move(pJob));
}

#endif
