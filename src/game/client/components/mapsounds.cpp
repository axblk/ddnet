#include "mapsounds.h"

#include <base/log.h>

#include <engine/demo.h>
#include <engine/shared/config.h>
#include <engine/sound.h>

#include <game/client/components/camera.h>
#include <game/client/components/sounds.h>
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
	int m_SampleId = -1;

protected:
	// Whether the sound came out of a file next to the map or out of the map
	// itself, by here it is bytes either way.
	void Process() override
	{
		m_SampleId = m_pSound->LoadOpusFromMem(Data().data(), static_cast<unsigned>(Data().size()), false, Path());
	}

	void OnReadFailed() override
	{
		log_error("mapsounds", "Failed to open/read sound file '%s'", Path());
	}

public:
	CMapSoundLoading(ISound *pSound, IStorage *pStorage, const char *pPath, int OwnerId, uint64_t Generation) :
		CAssetJob(EAssetType::SOUND, pStorage, pPath, IStorage::TYPE_ALL, OwnerId, Generation),
		m_pSound(pSound)
	{
	}

	CMapSoundLoading(ISound *pSound, std::vector<uint8_t> vData, const char *pContextName, int OwnerId, uint64_t Generation) :
		CAssetJob(EAssetType::SOUND, std::move(vData), pContextName, OwnerId, Generation),
		m_pSound(pSound)
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
}

void CMapSounds::Play(int Channel, int SoundId)
{
	if(!SoundEnabled())
		return;
	if(SoundId < 0 || SoundId >= m_Count)
		return;

	GameClient()->m_Sounds.PlaySample(Channel, m_aSounds[SoundId], 0, 1.0f);
}

void CMapSounds::PlayAt(int Channel, int SoundId, vec2 Position)
{
	if(!SoundEnabled())
		return;
	if(SoundId < 0 || SoundId >= m_Count)
		return;

	GameClient()->m_Sounds.PlaySampleAt(Channel, m_aSounds[SoundId], 0, 1.0f, Position);
}

void CMapSounds::StopAll()
{
	for(auto &Source : m_vSourceQueue)
	{
		Sound()->StopVoice(Source.m_Voice);
		Source.m_Voice = ISound::CVoiceHandle();
	}
}

void CMapSounds::OnMapLoad()
{
	IMap *pMap = GameClient()->Map();

	Clear();

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
			m_vSoundLoads.push_back({i, GameClient()->AssetLoader().Load(std::make_shared<CMapSoundLoading>(Sound(), Storage(), aBuf, m_AssetOwnerId, m_LoadGeneration))});
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
	for(int GroupIndex = 0; GroupIndex < Layers()->NumGroups(); GroupIndex++)
	{
		const CMapItemGroup *pGroup = Layers()->GetGroup(GroupIndex);
		if(!pGroup)
			continue;

		for(int LayerIndex = 0; LayerIndex < pGroup->m_NumLayers; LayerIndex++)
		{
			const CMapItemLayer *pLayer = Layers()->GetLayer(pGroup->m_StartLayer + LayerIndex);
			if(!pLayer)
				continue;
			if(pLayer->m_Type != LAYERTYPE_SOUNDS)
				continue;

			const CMapItemLayerSounds *pSoundLayer = reinterpret_cast<const CMapItemLayerSounds *>(pLayer);
			if(pSoundLayer->m_Version < 1 || pSoundLayer->m_Version > 2)
				continue;
			if(pSoundLayer->m_Sound < 0 || pSoundLayer->m_Sound >= m_Count)
				continue;

			const CSoundSource *pSources = static_cast<CSoundSource *>(Layers()->Map()->GetDataSwapped(pSoundLayer->m_Data));
			if(!pSources)
				continue;

			const size_t NumSources = std::min((size_t)pSoundLayer->m_NumSources, (size_t)Layers()->Map()->GetDataSize(pSoundLayer->m_Data) / sizeof(CSoundSource));
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

void CMapSounds::OnRender()
{
	FinishSoundLoads();
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	const bool DemoPlayerPaused = GameClient()->IsDemoPlaybackPaused();

	// enqueue sounds
	for(auto &Source : m_vSourceQueue)
	{
		if(m_aSounds[Source.m_Sound] < 0)
			continue;
		static float s_Time = 0.0f;
		if(GameClient()->m_Snap.m_pGameInfoObj)
		{
			s_Time = mix((Client()->PrevGameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / (float)Client()->GameTickSpeed(),
				(Client()->GameTick(g_Config.m_ClDummy) - GameClient()->m_Snap.m_pGameInfoObj->m_RoundStartTick) / (float)Client()->GameTickSpeed(),
				Client()->IntraGameTick(g_Config.m_ClDummy));
		}
		float Offset = s_Time - Source.m_pSource->m_TimeDelay;
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

				Source.m_Voice = GameClient()->m_Sounds.PlaySampleAt(CSounds::CHN_MAPSOUND, m_aSounds[Source.m_Sound], Flags, 1.0f, vec2(fx2f(Source.m_pSource->m_Position.x), fx2f(Source.m_pSource->m_Position.y)));
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

	const vec2 Center = GameClient()->m_Camera.m_Center;
	for(const auto &Source : m_vSourceQueue)
	{
		if(!Source.m_Voice.IsValid())
			continue;

		ColorRGBA Position = ColorRGBA(0.0f, 0.0f, 0.0f, 0.0f);
		CEnvelopeState &EnvEvaluator = GameClient()->m_MapLayersBackground.EnvEvaluator();
		EnvEvaluator.EnvelopeEval(Source.m_pSource->m_PosEnvOffset, Source.m_pSource->m_PosEnv, Position, 2);

		float x = fx2f(Source.m_pSource->m_Position.x) + Position.r;
		float y = fx2f(Source.m_pSource->m_Position.y) + Position.g;

		x += Center.x * (1.0f - Source.m_pGroup->m_ParallaxX / 100.0f);
		y += Center.y * (1.0f - Source.m_pGroup->m_ParallaxY / 100.0f);

		x -= Source.m_pGroup->m_OffsetX;
		y -= Source.m_pGroup->m_OffsetY;

		Sound()->SetVoicePosition(Source.m_Voice, vec2(x, y));

		ColorRGBA Volume = ColorRGBA(1.0f, 0.0f, 0.0f, 0.0f);
		EnvEvaluator.EnvelopeEval(Source.m_pSource->m_SoundEnvOffset, Source.m_pSource->m_SoundEnv, Volume, 1);

		Sound()->SetVoiceVolume(Source.m_Voice, std::clamp(Volume.r, 0.0f, 1.0f));
	}
}

void CMapSounds::Clear()
{
	++m_LoadGeneration;
	GameClient()->AssetLoader().AbortOwnerBeforeGeneration(m_AssetOwnerId, m_LoadGeneration);
	m_vSoundLoads.clear();
	m_LoadWarning = false;
	// unload all samples
	m_vSourceQueue.clear();
	for(int i = 0; i < m_Count; i++)
	{
		Sound()->UnloadSample(m_aSounds[i]);
		m_aSounds[i] = -1;
	}
	m_Count = 0;
}

void CMapSounds::OnStateChange(int NewState, int OldState)
{
	if(NewState < IClient::STATE_ONLINE)
		Clear();
}

bool CMapSounds::SoundEnabled()
{
	return g_Config.m_SndGame && g_Config.m_SndEnable;
}
