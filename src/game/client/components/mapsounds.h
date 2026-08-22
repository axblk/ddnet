#ifndef GAME_CLIENT_COMPONENTS_MAPSOUNDS_H
#define GAME_CLIENT_COMPONENTS_MAPSOUNDS_H

#include <engine/client/asset_loader.h>
#include <engine/sound.h>

#include <game/client/component.h>
#include <game/mapitems.h>

#include <vector>

class CEnvelopeState;
class CGameState;
class CGameTickInfo;
class CLayers;
class IMap;

class CMapSounds : public CComponentInterfaces
{
	class CMapSoundLoading;
	class CMapSoundLoad
	{
	public:
		int m_Sound;
		CTypedAssetResource<CMapSoundLoading> m_Resource;
	};

	int m_aSounds[MAX_MAPSOUNDS];
	int m_Count;
	float m_Time;
	bool m_Audible = false;
	int m_AssetOwnerId;
	uint64_t m_LoadGeneration = 1;
	bool m_LoadWarning = false;
	std::vector<CMapSoundLoad> m_vSoundLoads;

	class CSourceQueueEntry
	{
	public:
		int m_Sound;
		bool m_HighDetail;
		ISound::CVoiceHandle m_Voice;
		const CMapItemGroup *m_pGroup;
		const CSoundSource *m_pSource;
	};
	std::vector<CSourceQueueEntry> m_vSourceQueue;
	void FinishSoundLoads();
	void StopVoices();

public:
	CMapSounds();

	void Play(int Channel, int SoundId);
	void PlayAt(int Channel, int SoundId, vec2 Position);

	void Load(IMap *pMap, CLayers *pLayers);
	void SetAudible(bool Audible);
	void Unload();
	void Update(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool DemoPlayerPaused, const CEnvelopeState &EnvEvaluator);
};

#endif // GAME_CLIENT_COMPONENTS_MAPSOUNDS_H
