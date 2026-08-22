#ifndef GAME_CLIENT_COMPONENTS_MAPSOUNDS_H
#define GAME_CLIENT_COMPONENTS_MAPSOUNDS_H

#include <engine/client/asset_loader.h>
#include <engine/sound.h>

#include <game/client/component.h>
#include <game/mapitems.h>

#include <vector>

class CMapSounds : public CComponent
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
	void Clear();
	bool SoundEnabled();

public:
	CMapSounds();
	int Sizeof() const override { return sizeof(*this); }

	void Play(int Channel, int SoundId);
	void PlayAt(int Channel, int SoundId, vec2 Position);
	void StopAll();

	void OnMapLoad() override;
	void OnRender() override;
	void OnStateChange(int NewState, int OldState) override;
};

#endif // GAME_CLIENT_COMPONENTS_MAPSOUNDS_H
