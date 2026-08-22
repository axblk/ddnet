#include "mapsounds.h"

#include <base/log.h>

#include <engine/sound.h>

#include <game/client/components/envelope_state.h>
#include <game/client/components/sounds.h>
#include <game/client/game_state.h>
#include <game/client/gameclient.h>
#include <game/layers.h>
#include <game/localization.h>
#include <game/mapitems.h>

#include <atomic>

namespace
{
	std::atomic<int> gs_NextMapSoundAssetOwner{-1};
}

class CMapSounds::CMapSoundLoading final : public CAssetJob
{
	ISound *m_pSound;
	std::vector<uint8_t> m_vData;
	bool m_FromMemory;
	int m_SampleId = -1;

protected:
	void Run() override
	{
		m_SampleId = m_FromMemory ? m_pSound->LoadOpusFromMem(m_vData.data(), static_cast<unsigned>(m_vData.size()), false, Path()) : m_pSound->LoadOpus(Path());
	}

public:
	CMapSoundLoading(ISound *pSound, const char *pPath, int OwnerId, uint64_t Generation) :
		CAssetJob(EAssetType::SOUND, pPath, OwnerId, Generation),
		m_pSound(pSound),
		m_FromMemory(false)
	{
	}

	CMapSoundLoading(ISound *pSound, std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation) :
		CAssetJob(EAssetType::SOUND, pContextName, OwnerId, Generation),
		m_pSound(pSound),
		m_vData(std::move(vData)),
		m_FromMemory(true)
	{
	}

	~CMapSoundLoading() override
	{
		if(m_SampleId >= 0)
			m_pSound->UnloadSample(m_SampleId);
	}

	int TakeSample()
	{
		const int SampleId = m_SampleId;
		m_SampleId = -1;
		return SampleId;
	}
};

CMapSounds::CMapSounds() :
	m_AssetOwnerId(gs_NextMapSoundAssetOwner.fetch_sub(1, std::memory_order_relaxed))
{
	std::fill(std::begin(m_aSounds), std::end(m_aSounds), -1);
	m_Count = 0;
	m_Time = 0.0f;
}

void CMapSounds::Play(int Channel, int SoundId)
{
	if(!m_Audible || SoundId < 0 || SoundId >= m_Count || m_aSounds[SoundId] < 0)
		return;

	PlayForAudio(Channel, SoundId, m_Offline);
}

void CMapSounds::PlayAt(int Channel, int SoundId, vec2 Position)
{
	if(!m_Audible || SoundId < 0 || SoundId >= m_Count || m_aSounds[SoundId] < 0)
		return;

	PlayAtForAudio(Channel, SoundId, Position, m_Offline);
}

void CMapSounds::PlayForAudio(int Channel, int SoundId, bool Offline)
{
	if(SoundId < 0 || SoundId >= m_Count || m_aSounds[SoundId] < 0)
		return;
	GameClient()->m_Sounds.PlaySampleForAudio(Channel, m_aSounds[SoundId], 0, 1.0f, Offline);
}

void CMapSounds::PlayAtForAudio(int Channel, int SoundId, vec2 Position, bool Offline)
{
	if(SoundId < 0 || SoundId >= m_Count || m_aSounds[SoundId] < 0)
		return;
	GameClient()->m_Sounds.PlaySampleAtForAudio(Channel, m_aSounds[SoundId], 0, 1.0f, Position, Offline);
}

void CMapSounds::Load(IMap *pMap, CLayers *pLayers)
{
	Unload();

	if(!Sound()->IsSoundEnabled())
		return;

	// load samples
	int Start;
	pMap->GetType(MAPITEMTYPE_SOUND, &Start, &m_Count);

	m_Count = std::clamp<int>(m_Count, 0, MAX_MAPSOUNDS);

	// load new samples
	m_LoadWarning = false;
	for(int i = 0; i < m_Count; i++)
	{
		CMapItemSound *pSound = (CMapItemSound *)pMap->GetItem(Start + i);
		const char *pName = pMap->GetDataString(pSound->m_SoundName);
		if(pName == nullptr || pName[0] == '\0')
		{
			if(pSound->m_External)
			{
				log_error("mapsounds", "Failed to load map sound %d: failed to load name.", i);
				m_LoadWarning = true;
				continue;
			}
			pName = "(error)";
		}

		if(pSound->m_External)
		{
			char aBuf[IO_MAX_PATH_LENGTH];
			str_format(aBuf, sizeof(aBuf), "mapres/%s.opus", pName);
			m_vSoundLoads.push_back({i, GameClient()->AssetLoader().Load(std::make_shared<CMapSoundLoading>(Sound(), aBuf, m_AssetOwnerId, m_LoadGeneration))});
			pMap->UnloadData(pSound->m_SoundName);
		}
		else
		{
			const void *pData = pMap->GetData(pSound->m_SoundData);
			if(pData == nullptr)
			{
				log_error("mapsounds", "Failed to load map sound %d: failed to load data.", i);
				m_LoadWarning = true;
				continue;
			}
			const int SoundDataSize = pMap->GetDataSize(pSound->m_SoundData);
			if(SoundDataSize <= 0)
			{
				log_error("mapsounds", "Failed to load map sound %d: invalid data size.", i);
				m_LoadWarning = true;
				pMap->UnloadData(pSound->m_SoundData);
				continue;
			}
			const auto *pBytes = static_cast<const uint8_t *>(pData);
			std::vector<uint8_t> vData(pBytes, pBytes + SoundDataSize);
			m_vSoundLoads.push_back({i, GameClient()->AssetLoader().Load(std::make_shared<CMapSoundLoading>(Sound(), std::move(vData), pName, m_AssetOwnerId, m_LoadGeneration))});
			pMap->UnloadData(pSound->m_SoundData);
		}
	}

	// enqueue sound sources
	for(int GroupIndex = 0; GroupIndex < pLayers->NumGroups(); GroupIndex++)
	{
		const CMapItemGroup *pGroup = pLayers->GetGroup(GroupIndex);
		if(!pGroup)
			continue;

		for(int LayerIndex = 0; LayerIndex < pGroup->m_NumLayers; LayerIndex++)
		{
			const CMapItemLayer *pLayer = pLayers->GetLayer(pGroup->m_StartLayer + LayerIndex);
			if(!pLayer)
				continue;
			if(pLayer->m_Type != LAYERTYPE_SOUNDS)
				continue;

			const CMapItemLayerSounds *pSoundLayer = reinterpret_cast<const CMapItemLayerSounds *>(pLayer);
			if(pSoundLayer->m_Version < 1 || pSoundLayer->m_Version > 2)
				continue;
			if(pSoundLayer->m_Sound < 0 || pSoundLayer->m_Sound >= m_Count)
				continue;

			const CSoundSource *pSources = static_cast<CSoundSource *>(pLayers->Map()->GetDataSwapped(pSoundLayer->m_Data));
			if(!pSources)
				continue;

			const size_t NumSources = std::min((size_t)pSoundLayer->m_NumSources, (size_t)pLayers->Map()->GetDataSize(pSoundLayer->m_Data) / sizeof(CSoundSource));
			for(size_t SourceIndex = 0; SourceIndex < NumSources; SourceIndex++)
			{
				CSourceQueueEntry Source;
				Source.m_Sound = pSoundLayer->m_Sound;
				Source.m_HighDetail = pLayer->m_Flags & LAYERFLAG_DETAIL;
				Source.m_pGroup = pGroup;
				Source.m_pSource = &pSources[SourceIndex];
				m_vSourceQueue.push_back(Source);
			}
		}
	}
}

void CMapSounds::FinishSoundLoads()
{
	for(auto It = m_vSoundLoads.begin(); It != m_vSoundLoads.end();)
	{
		if(!It->m_Resource.IsFinished())
		{
			++It;
			continue;
		}
		if(It->m_Resource.IsReady(m_LoadGeneration))
		{
			const int SampleId = It->m_Resource.Result().TakeSample();
			if(SampleId >= 0)
				m_aSounds[It->m_Sound] = SampleId;
			else
				m_LoadWarning = true;
		}
		It = m_vSoundLoads.erase(It);
	}
	if(m_vSoundLoads.empty() && m_LoadWarning)
	{
		Client()->AddWarning(SWarning(Localize("Some map sounds could not be loaded. Check the local console for details.")));
		m_LoadWarning = false;
	}
}

void CMapSounds::Update(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool DemoPlayerPaused, const CEnvelopeState &EnvEvaluator, bool Offline)
{
	FinishSoundLoads();
	SetAudio(true, Offline);
	if(!m_Audible)
		return;

	if(State.HasGameInfo())
	{
		m_Time = mix((Time.m_PrevGameTick - State.GameInfo().m_RoundStartTick) / (float)Time.m_GameTickSpeed,
			(Time.m_GameTick - State.GameInfo().m_RoundStartTick) / (float)Time.m_GameTickSpeed,
			Time.m_IntraGameTick);
	}

	// enqueue sounds
	for(auto &Source : m_vSourceQueue)
	{
		if(m_aSounds[Source.m_Sound] < 0)
			continue;
		float Offset = m_Time - Source.m_pSource->m_TimeDelay;
		if(!DemoPlayerPaused && Offset >= 0.0f && g_Config.m_SndEnable && (g_Config.m_GfxHighDetail || !Source.m_HighDetail))
		{
			if(Source.m_Voice.IsValid())
			{
				// currently playing, set offset
				Sound()->SetVoiceTimeOffset(Source.m_Voice, Offset);
			}
			else
			{
				// need to enqueue
				int Flags = 0;
				if(Source.m_pSource->m_Loop)
					Flags |= ISound::FLAG_LOOP;
				if(!Source.m_pSource->m_Pan)
					Flags |= ISound::FLAG_NO_PANNING;

				Source.m_Voice = GameClient()->m_Sounds.PlaySampleAtForAudio(CSounds::CHN_MAPSOUND, m_aSounds[Source.m_Sound], Flags, 1.0f, vec2(fx2f(Source.m_pSource->m_Position.x), fx2f(Source.m_pSource->m_Position.y)), m_Offline);
				Sound()->SetVoiceTimeOffset(Source.m_Voice, Offset);
				Sound()->SetVoiceFalloff(Source.m_Voice, Source.m_pSource->m_Falloff / 255.0f);
				switch(Source.m_pSource->m_Shape.m_Type)
				{
				case CSoundShape::SHAPE_CIRCLE:
				{
					Sound()->SetVoiceCircle(Source.m_Voice, Source.m_pSource->m_Shape.m_Circle.m_Radius);
					break;
				}

				case CSoundShape::SHAPE_RECTANGLE:
				{
					Sound()->SetVoiceRectangle(Source.m_Voice, fx2f(Source.m_pSource->m_Shape.m_Rectangle.m_Width), fx2f(Source.m_pSource->m_Shape.m_Rectangle.m_Height));
					break;
				}
				};
			}
		}
		else
		{
			// stop voice
			Sound()->StopVoice(Source.m_Voice);
			Source.m_Voice = ISound::CVoiceHandle();
		}
	}

	for(const auto &Source : m_vSourceQueue)
	{
		if(!Source.m_Voice.IsValid())
			continue;

		ColorRGBA Position = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
		EnvEvaluator.EnvelopeEval(Source.m_pSource->m_PosEnvOffset, Source.m_pSource->m_PosEnv, Position, 2);

		float x = fx2f(Source.m_pSource->m_Position.x) + Position.r;
		float y = fx2f(Source.m_pSource->m_Position.y) + Position.g;

		x += ListenerPosition.x * (1.0f - Source.m_pGroup->m_ParallaxX / 100.0f);
		y += ListenerPosition.y * (1.0f - Source.m_pGroup->m_ParallaxY / 100.0f);

		x -= Source.m_pGroup->m_OffsetX;
		y -= Source.m_pGroup->m_OffsetY;

		Sound()->SetVoicePosition(Source.m_Voice, vec2(x, y));

		ColorRGBA Volume = ColorRGBA(1.0f, 0.0f, 0.0f, 0.0f);
		EnvEvaluator.EnvelopeEval(Source.m_pSource->m_SoundEnvOffset, Source.m_pSource->m_SoundEnv, Volume, 1);

		Sound()->SetVoiceVolume(Source.m_Voice, std::clamp(Volume.r, 0.0f, 1.0f));
	}
}

void CMapSounds::StopVoices()
{
	for(auto &Source : m_vSourceQueue)
	{
		Sound()->StopVoice(Source.m_Voice);
		Source.m_Voice = ISound::CVoiceHandle();
	}
	for(int i = 0; i < m_Count; i++)
	{
		if(m_aSounds[i] >= 0)
			Sound()->Stop(m_aSounds[i]);
	}
}

void CMapSounds::SetAudible(bool Audible)
{
	SetAudio(Audible, false);
}

void CMapSounds::SetAudio(bool Audible, bool Offline)
{
	if(m_Audible == Audible && m_Offline == Offline)
		return;
	if(m_Audible)
		StopVoices();
	m_Audible = Audible;
	m_Offline = Offline;
}

void CMapSounds::Unload()
{
	SetAudible(false);
	++m_LoadGeneration;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(m_AssetOwnerId, m_LoadGeneration);
	m_vSoundLoads.clear();
	m_LoadWarning = false;
	// unload all samples
	m_vSourceQueue.clear();
	m_Time = 0.0f;
	for(int i = 0; i < m_Count; i++)
	{
		Sound()->UnloadSample(m_aSounds[i]);
		m_aSounds[i] = -1;
	}
	m_Count = 0;
}
