/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "sounds.h"

#include <base/log.h>
#include <base/mem.h>
#include <base/time.h>

#include <engine/engine.h>
#include <engine/shared/config.h>
#include <engine/sound.h>

#include <generated/client_data.h>

#include <game/client/components/camera.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

CSoundAssetJob::CSoundAssetJob(ISound *pSound, IStorage *pStorage, const char *pPath) :
	CAssetJob(pStorage, pPath, IStorage::TYPE_ALL),
	m_pSound(pSound)
{
}

CSoundAssetJob::~CSoundAssetJob()
{
	if(m_SampleId != -1)
		m_pSound->UnloadSample(m_SampleId);
}

bool CSoundAssetJob::Process()
{
	m_SampleId = m_pSound->LoadWVFromMem(Data().data(), static_cast<unsigned>(Data().size()), false, Path());
	return m_SampleId != -1;
}

int CSoundAssetJob::TakeSample()
{
	const int SampleId = m_SampleId;
	m_SampleId = -1;
	return SampleId;
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
	for(int SetId = 0; SetId < g_pData->m_NumSounds; ++SetId)
	{
		for(int SoundId = 0; SoundId < g_pData->m_aSounds[SetId].m_NumSounds; ++SoundId)
			g_pData->m_aSounds[SetId].m_aSounds[SoundId].m_Id = -1;
	}

	// load sounds
	if(g_Config.m_ClThreadsoundloading)
	{
		if(Sound()->IsSoundEnabled())
		{
			for(int SetId = 0; SetId < g_pData->m_NumSounds; ++SetId)
			{
				for(int SoundId = 0; SoundId < g_pData->m_aSounds[SetId].m_NumSounds; ++SoundId)
				{
					const char *pFilename = g_pData->m_aSounds[SetId].m_aSounds[SoundId].m_pFilename;
					m_vSoundLoads.push_back({SetId, SoundId, GameClient()->AssetLoader().Load(std::make_shared<CSoundAssetJob>(Sound(), Storage(), pFilename), EAssetPriority::BACKGROUND)});
				}
			}
		}
		m_WaitForSoundJob = !m_vSoundLoads.empty();
		GameClient()->RenderLoading(Localize("Loading DDNet Client"), Localize("Loading sound files"), 0);
	}
	else
	{
		for(int SetId = 0; SetId < g_pData->m_NumSounds; ++SetId)
		{
			for(int SoundId = 0; SoundId < g_pData->m_aSounds[SetId].m_NumSounds; ++SoundId)
				g_pData->m_aSounds[SetId].m_aSounds[SoundId].m_Id = Sound()->LoadWV(g_pData->m_aSounds[SetId].m_aSounds[SoundId].m_pFilename);
			GameClient()->RenderLoading(Localize("Loading DDNet Client"), Localize("Loading sound files"), 1);
		}
		m_WaitForSoundJob = false;
	}
}

void CSounds::OnShutdown()
{
	m_vSoundLoads.clear();
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

void CSounds::OnUpdate()
{
	FinishSoundLoads();
}

void CSounds::FinishSoundLoads()
{
	if(m_WaitForSoundJob)
	{
		m_WaitForSoundJob = false;
		for(auto &Load : m_vSoundLoads)
		{
			if(!Load.m_Resource.IsFinished())
			{
				m_WaitForSoundJob = true;
				continue;
			}
			if(Load.m_Resource.IsReady())
				g_pData->m_aSounds[Load.m_SetId].m_aSounds[Load.m_SoundId].m_Id = Load.m_Resource.Result().TakeSample();
			else if(Load.m_Resource.IsFailed())
				log_error("sound", "Failed to load sound file '%s'", Load.m_Resource.Path());
		}
		if(!m_WaitForSoundJob)
			m_vSoundLoads.clear();
	}
}

void CSounds::Update(std::optional<vec2> ListenerPosition, int64_t Now, bool Offline)
{
	if(ListenerPosition.has_value())
		Sound()->SetListenerPosition(*ListenerPosition, Offline);
	UpdateChannels();

	// play sound from queue
	CQueue &Queue = m_aQueues[Offline];
	if(Queue.m_Pos > 0 && Queue.m_WaitTime <= Now)
	{
		Play(Queue.m_aEntries[0].m_Channel, Queue.m_aEntries[0].m_SetId, 1.0f, Offline);
		Queue.m_WaitTime = Now + time_freq() * 3 / 10; // wait 300ms before playing the next one
		if(--Queue.m_Pos > 0)
			mem_move(Queue.m_aEntries, Queue.m_aEntries + 1, Queue.m_Pos * sizeof(CQueueEntry));
	}
}

void CSounds::ClearQueue(bool Offline)
{
	CQueue &Queue = m_aQueues[Offline];
	mem_zero(Queue.m_aEntries, sizeof(Queue.m_aEntries));
	Queue.m_Pos = 0;
	// an export's timeline starts at zero
	Queue.m_WaitTime = Offline ? 0 : time();
}

void CSounds::Enqueue(int Channel, int SetId, bool Offline)
{
	if(GameClient()->m_SuppressEvents)
		return;
	CQueue &Queue = m_aQueues[Offline];
	if(Queue.m_Pos >= QUEUE_SIZE)
		return;
	if(!Offline && Channel != CHN_MUSIC && g_Config.m_ClEditor)
		return;

	Queue.m_aEntries[Queue.m_Pos].m_Channel = Channel;
	Queue.m_aEntries[Queue.m_Pos++].m_SetId = SetId;
}

void CSounds::PlayAndRecord(int Channel, int SetId, float Volume, vec2 Position)
{
	// TODO: Volume and position are currently not recorded for sounds played with this function
	// TODO: This also causes desync sounds during demo playback of demos recorded on high ping servers:
	//       https://github.com/ddnet/ddnet/issues/1282
	CNetMsg_Sv_SoundGlobal Msg;
	Msg.m_SoundId = SetId;
	ClientNetwork()->SendPackMsg(g_Config.m_ClDummy, &Msg, MSGFLAG_NOSEND | MSGFLAG_RECORD);

	PlayAt(Channel, SetId, Volume, Position);
}

void CSounds::Play(int Channel, int SetId, float Volume, bool Offline)
{
	PlaySample(Channel, GetSampleId(SetId), 0, Volume, Offline);
}

void CSounds::PlayAt(int Channel, int SetId, float Volume, vec2 Position, bool Offline)
{
	PlaySampleAt(Channel, GetSampleId(SetId), 0, Volume, Position, Offline);
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

ISound::CVoiceHandle CSounds::PlaySample(int Channel, int SampleId, int Flags, float Volume, bool Offline)
{
	if(GameClient()->m_SuppressEvents || (Channel == CHN_MUSIC && !g_Config.m_SndMusic) || SampleId == -1)
		return ISound::CVoiceHandle();

	if(Channel == CHN_MUSIC)
		Flags |= ISound::FLAG_LOOP;

	return Sound()->Play(Channel, SampleId, Flags, Volume, Offline);
}

ISound::CVoiceHandle CSounds::PlaySampleAt(int Channel, int SampleId, int Flags, float Volume, vec2 Position, bool Offline)
{
	if(GameClient()->m_SuppressEvents || (Channel == CHN_MUSIC && !g_Config.m_SndMusic) || SampleId == -1)
		return ISound::CVoiceHandle();

	if(Channel == CHN_MUSIC)
		Flags |= ISound::FLAG_LOOP;

	return Sound()->PlayAt(Channel, SampleId, Flags, Volume, Position, Offline);
}
