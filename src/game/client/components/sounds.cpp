/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "sounds.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/shared/config.h>
#include <engine/sound.h>

#include <generated/client_data.h>

#include <game/client/components/camera.h>
#include <game/client/components/menus.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

namespace
{
	constexpr int ASSET_OWNER_STARTUP_SOUNDS = 1;
}

CSoundLoading::CSoundLoading(ISound *pSound, int Lane, int NumLanes, int OwnerId, uint64_t Generation) :
	CAssetJob(EAssetType::SOUND, "audio", OwnerId, Generation),
	m_pSound(pSound),
	m_Lane(Lane),
	m_NumLanes(NumLanes)
{
	dbg_assert(pSound != nullptr, "Sound must not be null");
	dbg_assert(Lane >= 0 && Lane < NumLanes, "Invalid sound loading lane");
}

CSoundLoading::~CSoundLoading()
{
	for(const CResult &Result : m_vResults)
	{
		if(Result.m_SampleId != -1)
			m_pSound->UnloadSample(Result.m_SampleId);
	}
}

void CSoundLoading::Run()
{
	for(int SetId = m_Lane; SetId < g_pData->m_NumSounds; SetId += m_NumLanes)
	{
		for(int SoundId = 0; SoundId < g_pData->m_aSounds[SetId].m_NumSounds; SoundId++)
		{
			if(State() == IJob::STATE_ABORTED)
				return;
			const std::chrono::nanoseconds LoadStart = time_get_nanoseconds();
			const int SampleId = m_pSound->LoadWV(g_pData->m_aSounds[SetId].m_aSounds[SoundId].m_pFilename);
			m_LoadTime += time_get_nanoseconds() - LoadStart;
			m_NumLoaded += SampleId != -1;
			m_vResults.push_back({SetId, SoundId, SampleId});
		}
	}
	m_Completed = true;
}

void CSoundLoading::Commit()
{
	dbg_assert(m_Completed, "Cannot commit unfinished sound load job");
	for(CResult &Result : m_vResults)
	{
		g_pData->m_aSounds[Result.m_SetId].m_aSounds[Result.m_SoundId].m_Id = Result.m_SampleId;
		Result.m_SampleId = -1;
	}
}

void CSounds::UpdateChannels()
{
	const float NewGuiSoundVolume = g_Config.m_SndChatVolume / 100.0f;
	if(NewGuiSoundVolume != m_GuiSoundVolume)
	{
		m_GuiSoundVolume = NewGuiSoundVolume;
		Sound()->SetChannel(CSounds::CHN_GUI, m_GuiSoundVolume, 0.0f);
	}

	const float NewGameSoundVolume = g_Config.m_SndGameVolume / 100.0f;
	if(NewGameSoundVolume != m_GameSoundVolume)
	{
		m_GameSoundVolume = NewGameSoundVolume;
		Sound()->SetChannel(CSounds::CHN_WORLD, 0.9f * m_GameSoundVolume, 1.0f);
		Sound()->SetChannel(CSounds::CHN_GLOBAL, m_GameSoundVolume, 0.0f);
	}

	const float NewMapSoundVolume = g_Config.m_SndMapVolume / 100.0f;
	if(NewMapSoundVolume != m_MapSoundVolume)
	{
		m_MapSoundVolume = NewMapSoundVolume;
		Sound()->SetChannel(CSounds::CHN_MAPSOUND, m_MapSoundVolume, 1.0f);
	}

	const float NewBackgroundMusicVolume = g_Config.m_SndBackgroundMusicVolume / 100.0f;
	if(NewBackgroundMusicVolume != m_BackgroundMusicVolume)
	{
		m_BackgroundMusicVolume = NewBackgroundMusicVolume;
		Sound()->SetChannel(CSounds::CHN_MUSIC, m_BackgroundMusicVolume, 1.0f);
	}
}

int CSounds::GetSampleId(int SetId)
{
	if(!g_Config.m_SndEnable || !Sound()->IsSoundEnabled() || SetId < 0 || SetId >= g_pData->m_NumSounds)
		return -1;

	CDataSoundset *pSet = &g_pData->m_aSounds[SetId];
	if(!pSet->m_NumSounds)
		return -1;

	if(pSet->m_NumSounds == 1)
		return pSet->m_aSounds[0].m_Id;

	// return random one
	int Id;
	do
	{
		Id = rand() % pSet->m_NumSounds;
	} while(Id == pSet->m_Last);
	pSet->m_Last = Id;
	return pSet->m_aSounds[Id].m_Id;
}

void CSounds::OnInit()
{
	UpdateChannels();
	ClearQueue();
	m_SoundBatchStart = time_get();
	m_NumSoundSamplesLoaded = 0;
	m_NumSoundJobsFinished = 0;
	m_SoundLoadTime = std::chrono::nanoseconds::zero();
	for(int SetId = 0; SetId < g_pData->m_NumSounds; ++SetId)
	{
		for(int SoundId = 0; SoundId < g_pData->m_aSounds[SetId].m_NumSounds; ++SoundId)
			g_pData->m_aSounds[SetId].m_aSounds[SoundId].m_Id = -1;
	}

	// load sounds
	if(g_Config.m_ClThreadsoundloading)
	{
		for(size_t Lane = 0; Lane < m_aSoundResources.size(); ++Lane)
		{
			m_aSoundResources[Lane] = GameClient()->AssetLoader().Load(std::make_shared<CSoundLoading>(Sound(), static_cast<int>(Lane), static_cast<int>(m_aSoundResources.size()), ASSET_OWNER_STARTUP_SOUNDS, m_LoadGeneration));
		}
		m_WaitForSoundJob = true;
		GameClient()->m_Menus.RenderLoading(Localize("Loading DDNet Client"), Localize("Loading sound files"), 0);
	}
	else
	{
		for(int SetId = 0; SetId < g_pData->m_NumSounds; ++SetId)
		{
			CSoundLoading SoundLoading(Sound(), SetId, g_pData->m_NumSounds, ASSET_OWNER_STARTUP_SOUNDS, m_LoadGeneration);
			SoundLoading.Run();
			SoundLoading.Commit();
			m_NumSoundSamplesLoaded += SoundLoading.NumLoaded();
			m_SoundLoadTime += SoundLoading.LoadTime();
			++m_NumSoundJobsFinished;
			GameClient()->m_Menus.RenderLoading(Localize("Loading DDNet Client"), Localize("Loading sound files"), 1);
		}
		m_WaitForSoundJob = false;
		log_info("asset_loader", "Startup sound batch: jobs=%d loaded=%d wall=%.2fms load=%.2fms", m_NumSoundJobsFinished, m_NumSoundSamplesLoaded,
			(time_get() - m_SoundBatchStart) * 1000.0 / time_freq(), m_SoundLoadTime.count() / 1000000.0);
	}
}

void CSounds::OnShutdown()
{
	++m_LoadGeneration;
	for(auto &Resource : m_aSoundResources)
	{
		Resource.Abort();
		Resource.Reset();
	}
	m_WaitForSoundJob = false;
}

void CSounds::OnReset()
{
	if(Client()->State() >= IClient::STATE_ONLINE)
	{
		Sound()->StopAll();
		ClearQueue();
	}
}

void CSounds::OnStateChange(int NewState, int OldState)
{
	if(NewState == IClient::STATE_ONLINE || NewState == IClient::STATE_DEMOPLAYBACK)
		OnReset();
}

void CSounds::Update(std::optional<vec2> ListenerPosition)
{
	// check for sound initialisation
	if(m_WaitForSoundJob)
	{
		bool Waiting = false;
		for(auto &Resource : m_aSoundResources)
		{
			if(!Resource)
				continue;
			if(Resource.IsReady(m_LoadGeneration))
			{
				CSoundLoading &SoundLoading = Resource.Result();
				m_NumSoundSamplesLoaded += SoundLoading.NumLoaded();
				m_SoundLoadTime += SoundLoading.LoadTime();
				++m_NumSoundJobsFinished;
				SoundLoading.Commit();
				Resource.Reset();
			}
			else if(Resource.IsFinished())
			{
				Resource.Reset();
			}
			else
			{
				Waiting = true;
			}
		}
		m_WaitForSoundJob = Waiting;
		if(!m_WaitForSoundJob)
		{
			log_info("asset_loader", "Startup sound batch: jobs=%d loaded=%d wall=%.2fms load=%.2fms", m_NumSoundJobsFinished, m_NumSoundSamplesLoaded,
				(time_get() - m_SoundBatchStart) * 1000.0 / time_freq(), m_SoundLoadTime.count() / 1000000.0);
		}
	}

	if(ListenerPosition.has_value())
		Sound()->SetListenerPosition(*ListenerPosition);
	UpdateChannels();

	// play sound from queue
	if(m_QueuePos > 0)
	{
		int64_t Now = time();
		if(m_QueueWaitTime <= Now)
		{
			Play(m_aQueue[0].m_Channel, m_aQueue[0].m_SetId, 1.0f);
			m_QueueWaitTime = Now + time_freq() * 3 / 10; // wait 300ms before playing the next one
			if(--m_QueuePos > 0)
				mem_move(m_aQueue, m_aQueue + 1, m_QueuePos * sizeof(CQueueEntry));
		}
	}
}

void CSounds::ClearQueue()
{
	mem_zero(m_aQueue, sizeof(m_aQueue));
	m_QueuePos = 0;
	m_QueueWaitTime = time();
}

void CSounds::Enqueue(int Channel, int SetId)
{
	if(GameClient()->m_SuppressEvents)
		return;
	if(m_QueuePos >= QUEUE_SIZE)
		return;
	if(Channel != CHN_MUSIC && g_Config.m_ClEditor)
		return;

	m_aQueue[m_QueuePos].m_Channel = Channel;
	m_aQueue[m_QueuePos++].m_SetId = SetId;
}

void CSounds::PlayAndRecord(int Channel, int SetId, float Volume, vec2 Position)
{
	// TODO: Volume and position are currently not recorded for sounds played with this function
	// TODO: This also causes desync sounds during demo playback of demos recorded on high ping servers:
	//       https://github.com/ddnet/ddnet/issues/1282
	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundId = SetId;
	Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_NOSEND | MSGFLAG_RECORD);

	PlayAt(Channel, SetId, Volume, Position);
}

void CSounds::Play(int Channel, int SetId, float Volume)
{
	PlaySample(Channel, GetSampleId(SetId), 0, Volume);
}

void CSounds::PlayAt(int Channel, int SetId, float Volume, vec2 Position)
{
	PlaySampleAt(Channel, GetSampleId(SetId), 0, Volume, Position);
}

void CSounds::Stop(int SetId)
{
	if(SetId < 0 || SetId >= g_pData->m_NumSounds)
		return;

	const CDataSoundset *pSet = &g_pData->m_aSounds[SetId];
	for(int i = 0; i < pSet->m_NumSounds; i++)
		if(pSet->m_aSounds[i].m_Id != -1)
			Sound()->Stop(pSet->m_aSounds[i].m_Id);
}

bool CSounds::IsPlaying(int SetId)
{
	if(SetId < 0 || SetId >= g_pData->m_NumSounds)
		return false;

	const CDataSoundset *pSet = &g_pData->m_aSounds[SetId];
	for(int i = 0; i < pSet->m_NumSounds; i++)
		if(pSet->m_aSounds[i].m_Id != -1 && Sound()->IsPlaying(pSet->m_aSounds[i].m_Id))
			return true;
	return false;
}

ISound::CVoiceHandle CSounds::PlaySample(int Channel, int SampleId, int Flags, float Volume)
{
	if(GameClient()->m_SuppressEvents || (Channel == CHN_MUSIC && !g_Config.m_SndMusic) || SampleId == -1)
		return ISound::CVoiceHandle();

	if(Channel == CHN_MUSIC)
		Flags |= ISound::FLAG_LOOP;

	return Sound()->Play(Channel, SampleId, Flags, Volume);
}

ISound::CVoiceHandle CSounds::PlaySampleAt(int Channel, int SampleId, int Flags, float Volume, vec2 Position)
{
	if(GameClient()->m_SuppressEvents || (Channel == CHN_MUSIC && !g_Config.m_SndMusic) || SampleId == -1)
		return ISound::CVoiceHandle();

	if(Channel == CHN_MUSIC)
		Flags |= ISound::FLAG_LOOP;

	return Sound()->PlayAt(Channel, SampleId, Flags, Volume, Position);
}
