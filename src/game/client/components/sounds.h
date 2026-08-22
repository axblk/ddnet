/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SOUNDS_H
#define GAME_CLIENT_COMPONENTS_SOUNDS_H

#include <base/vmath.h>

#include <engine/client/asset_loader.h>
#include <engine/sound.h>

#include <game/client/component.h>

#include <array>
#include <chrono>
#include <optional>
#include <vector>

class CSoundLoading : public CAssetJob
{
	class CResult
	{
	public:
		int m_SetId;
		int m_SoundId;
		int m_SampleId;
	};

	ISound *m_pSound;
	int m_Lane;
	int m_NumLanes;
	bool m_Completed = false;
	int m_NumLoaded = 0;
	std::chrono::nanoseconds m_LoadTime{};
	std::vector<CResult> m_vResults;

public:
	CSoundLoading(ISound *pSound, int Lane, int NumLanes, int OwnerId, uint64_t Generation);
	~CSoundLoading() override;
	void Run() override;
	void Commit();
	int NumLoaded() const { return m_NumLoaded; }
	std::chrono::nanoseconds LoadTime() const { return m_LoadTime; }
};

class CSounds : public CComponent
{
	enum
	{
		QUEUE_SIZE = 32,
	};
	class CQueueEntry
	{
	public:
		int m_Channel;
		int m_SetId;
	};
	CQueueEntry m_aaQueue[2][QUEUE_SIZE] = {};
	int m_aQueuePos[2] = {};
	int64_t m_aQueueWaitTime[2] = {};
	std::array<CTypedAssetResource<CSoundLoading>, 2> m_aSoundResources;
	uint64_t m_LoadGeneration = 1;
	bool m_WaitForSoundJob = false;
	int64_t m_SoundBatchStart = 0;
	int m_NumSoundSamplesLoaded = 0;
	int m_NumSoundJobsFinished = 0;
	std::chrono::nanoseconds m_SoundLoadTime{};

	void UpdateChannels();
	void UpdateQueue(bool Offline, int64_t Now);
	bool UpdateLoadingState();
	int GetSampleId(int SetId);

	float m_GuiSoundVolume = -1.0f;
	float m_GameSoundVolume = -1.0f;
	float m_MapSoundVolume = -1.0f;
	float m_BackgroundMusicVolume = -1.0f;

public:
	// sound channels
	enum
	{
		CHN_GUI = 0,
		CHN_MUSIC,
		CHN_WORLD,
		CHN_GLOBAL,
		CHN_MAPSOUND,
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnShutdown() override;
	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;
	void Update(std::optional<vec2> ListenerPosition);
	void UpdateOffline(vec2 ListenerPosition, int64_t Now);
	bool IsReady() { return UpdateLoadingState(); }

	void ClearQueue();
	void ClearOffline();
	void Enqueue(int Channel, int SetId);
	void EnqueueForAudio(int Channel, int SetId, bool Offline);
	void Play(int Channel, int SetId, float Volume);
	void PlayAt(int Channel, int SetId, float Volume, vec2 Position);
	void PlayForAudio(int Channel, int SetId, float Volume, bool Offline);
	void PlayAtForAudio(int Channel, int SetId, float Volume, vec2 Position, bool Offline);
	void PlayAndRecord(int Channel, int SetId, float Volume, vec2 Position);
	void Stop(int SetId);
	bool IsPlaying(int SetId);
	bool StartupAssetsLoaded() const { return !m_WaitForSoundJob; }

	ISound::CVoiceHandle PlaySample(int Channel, int SampleId, int Flags, float Volume);
	ISound::CVoiceHandle PlaySampleAt(int Channel, int SampleId, int Flags, float Volume, vec2 Position);
	ISound::CVoiceHandle PlaySampleForAudio(int Channel, int SampleId, int Flags, float Volume, bool Offline);
	ISound::CVoiceHandle PlaySampleAtForAudio(int Channel, int SampleId, int Flags, float Volume, vec2 Position, bool Offline);
};

#endif
