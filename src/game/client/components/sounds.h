/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SOUNDS_H
#define GAME_CLIENT_COMPONENTS_SOUNDS_H

#include <base/vmath.h>

#include <engine/client/asset_loader.h>
#include <engine/sound.h>

#include <game/client/component.h>

#include <optional>
#include <vector>

class CSoundAssetJob final : public CAssetJob
{
	ISound *m_pSound;
	int m_SampleId = -1;

protected:
	bool Process() override;

public:
	CSoundAssetJob(ISound *pSound, IStorage *pStorage, const char *pPath);
	~CSoundAssetJob() override;
	int TakeSample();
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
	class CQueue
	{
	public:
		CQueueEntry m_aEntries[QUEUE_SIZE];
		int m_Pos = 0;
		int64_t m_WaitTime = 0;
	};
	// One for the device and one for the offline mix of a video export.
	CQueue m_aQueues[2];
	class CSoundLoad
	{
	public:
		int m_SetId;
		int m_SoundId;
		CTypedAssetResource<CSoundAssetJob> m_Resource;
	};
	std::vector<CSoundLoad> m_vSoundLoads;
	bool m_WaitForSoundJob = false;

	void UpdateChannels();
	void FinishSoundLoads();
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
	void OnUpdate() override;
	/**
	 * @param ListenerPosition Where the sounds are heard from, or nothing to
	 * leave the listener where it is.
	 * @param Now Presentation time, which paces the queue.
	 * @param Offline Whether this is the offline mix of a video export.
	 */
	void Update(std::optional<vec2> ListenerPosition, int64_t Now, bool Offline = false);

	void ClearQueue(bool Offline = false);
	void Enqueue(int Channel, int SetId, bool Offline = false);
	void Play(int Channel, int SetId, float Volume, bool Offline = false);
	void PlayAt(int Channel, int SetId, float Volume, vec2 Position, bool Offline = false);
	void PlayAndRecord(int Channel, int SetId, float Volume, vec2 Position);
	void Stop(int SetId);
	bool IsPlaying(int SetId);
	bool StartupAssetsLoaded() const { return !m_WaitForSoundJob; }

	ISound::CVoiceHandle PlaySample(int Channel, int SampleId, int Flags, float Volume, bool Offline = false);
	ISound::CVoiceHandle PlaySampleAt(int Channel, int SampleId, int Flags, float Volume, vec2 Position, bool Offline = false);
};

#endif
