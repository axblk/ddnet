#ifndef GAME_CLIENT_MAP_CONTEXT_H
#define GAME_CLIENT_MAP_CONTEXT_H

#include "session_game_config.h"

#include <engine/map.h>

#include <game/collision.h>
#include <game/gamecore.h>
#include <game/layers.h>
#include <game/mapbugs.h>

#include <memory>

class CMapContext
{
	std::unique_ptr<IMap> m_pMap;
	CLayers m_Layers;
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

	void Init();
	void Load(const CConfig &BaseConfig);
	void Unload();

	void SetTuning(int TuneZone, const char *pName, float Value);
	void EnableMapBug(const char *pName);

	IMap *Map() { return m_pMap.get(); }
	const IMap *Map() const { return m_pMap.get(); }
	CLayers *Layers() { return &m_Layers; }
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
