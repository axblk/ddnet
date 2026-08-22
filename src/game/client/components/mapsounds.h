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

public:
	CMapSounds();

	void Play(int Channel, int SoundId, bool Offline = false);
	void PlayAt(int Channel, int SoundId, vec2 Position, bool Offline = false);
	// Stops every playing map sound voice, for when sound is switched off.
	void StopVoices();

	void Load(IMap *pMap, CLayers *pLayers);
	void SetAudible(bool Audible);
	void Unload();
	/**
	 * Starts, moves and stops the map's sound sources for the moment shown.
	 *
	 * @param State The game state whose round the sources are timed by.
	 * @param Time The ticks of the moment shown.
	 * @param ListenerPosition Where the sources are heard from.
	 * @param DemoPlayerPaused Whether a paused demo keeps new sources from starting.
	 * @param EnvEvaluator What evaluates the envelopes the sources move and fade by.
	 * @param Offline Whether the sources play into the offline mix of a video
	 * export, which is always audible while it runs.
	 */
	void Update(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool DemoPlayerPaused, const CEnvelopeState &EnvEvaluator, bool Offline = false);
};

#endif // GAME_CLIENT_COMPONENTS_MAPSOUNDS_H
