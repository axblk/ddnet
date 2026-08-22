/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SOUNDS_H
#define GAME_CLIENT_COMPONENTS_SOUNDS_H

#include <base/vmath.h>

#include <engine/shared/jobs.h>
#include <engine/sound.h>

#include <game/client/component.h>

#include <optional>

class CSoundLoading : public IJob
{
	CGameClient *m_pGameClient;
	bool m_Render;

public:
	CSoundLoading(CGameClient *pGameClient, bool Render);
	void Run() override;
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
	std::shared_ptr<CSoundLoading> m_pSoundJob;
	bool m_WaitForSoundJob;

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

	ISound::CVoiceHandle PlaySample(int Channel, int SampleId, int Flags, float Volume);
	ISound::CVoiceHandle PlaySampleAt(int Channel, int SampleId, int Flags, float Volume, vec2 Position);
	ISound::CVoiceHandle PlaySampleForAudio(int Channel, int SampleId, int Flags, float Volume, bool Offline);
	ISound::CVoiceHandle PlaySampleAtForAudio(int Channel, int SampleId, int Flags, float Volume, vec2 Position, bool Offline);
};

#endif
