#ifndef GAME_CLIENT_MAP_CONTEXT_H
#define GAME_CLIENT_MAP_CONTEXT_H

#include "session_game_config.h"

#include <engine/map.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/layers.h>
#include <game/mapbugs.h>

#include <memory>

/**
 * The map file and its layers, which every session playing that map reads the
 * same way. Sessions share one through a shared pointer, so it lives as long
 * as the last of them.
 */
class CMapData
{
	std::unique_ptr<IMap> m_pMap;
	CLayers m_Layers;
	bool m_LayersInitialized = false;

public:
	CMapData();
	~CMapData();
	CMapData(const CMapData &) = delete;
	CMapData &operator=(const CMapData &) = delete;

	/**
	 * Once per map, whichever session gets there first.
	 */
	void InitLayers();

	IMap *Map() { return m_pMap.get(); }
	const IMap *Map() const { return m_pMap.get(); }
	CLayers *Layers() { return &m_Layers; }
};

class CMapContext
{
	std::shared_ptr<CMapData> m_pData;
	// Doors are stamped into it, the switch count is written from snapshots
	// and predicted lasers write tiles, so each session keeps its own.
	CCollision m_Collision;
	CMapBugs m_MapBugs;
	CTuningParams m_aTuningList[TuneZone::NUM];
	CSessionGameConfig m_GameConfig;

	static void ConTuneParam(IConsole::IResult *pResult, void *pUserData);
	static void ConTuneZone(IConsole::IResult *pResult, void *pUserData);
	static void ConMapbug(IConsole::IResult *pResult, void *pUserData);
	void ResetSettings(const CConfig &BaseConfig);

public:
	CMapContext();

	void Load(const CConfig &BaseConfig);
	/**
	 * Leaves the map to whoever else plays it and starts over with an empty
	 * one.
	 */
	void Unload();
	/**
	 * Plays a map another session already loaded instead of loading it again.
	 */
	void Share(const CMapContext &Other);
	const std::shared_ptr<CMapData> &Data() const { return m_pData; }

	void SetTuning(int TuneZone, const char *pName, float Value);
	void EnableMapBug(const char *pName);

	IMap *Map() { return m_pData->Map(); }
	const IMap *Map() const { return m_pData->Map(); }
	CLayers *Layers() { return m_pData->Layers(); }
	CCollision *Collision() { return &m_Collision; }
	const CCollision *Collision() const { return &m_Collision; }
	CMapBugs *MapBugs() { return &m_MapBugs; }
	const CMapBugs *MapBugs() const { return &m_MapBugs; }
	CTuningParams *TuningList() { return m_aTuningList; }
	const CTuningParams *TuningList() const { return m_aTuningList; }
	CSessionGameConfig &GameConfig() { return m_GameConfig; }
	const CSessionGameConfig &GameConfig() const { return m_GameConfig; }
};

#endif // GAME_CLIENT_MAP_CONTEXT_H
