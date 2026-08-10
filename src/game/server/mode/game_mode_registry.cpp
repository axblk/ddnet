#include "game_mode_registry.h"

#include <base/str.h>

#include <game/server/gamecontroller.h>

bool CGameModeRegistry::Register(const CGameModeInfo &Info, FCreateController pfnCreateController)
{
	if(!Info.m_pId || !Info.m_pId[0] || !Info.m_pDisplayName || !Info.m_pGameType || !Info.m_pTestingGameType || !pfnCreateController || Find(Info.m_pId))
		return false;

	m_vEntries.push_back({Info, pfnCreateController});
	return true;
}

const CGameModeInfo *CGameModeRegistry::Find(const char *pId) const
{
	if(!pId)
		return nullptr;

	for(const CEntry &Entry : m_vEntries)
	{
		if(str_comp(Entry.m_Info.m_pId, pId) == 0)
			return &Entry.m_Info;
	}
	return nullptr;
}

std::unique_ptr<IGameController> CGameModeRegistry::Create(const char *pId, CGameContext *pGameServer) const
{
	if(!pId || !pGameServer)
		return nullptr;

	for(const CEntry &Entry : m_vEntries)
	{
		if(str_comp(Entry.m_Info.m_pId, pId) == 0)
			return Entry.m_pfnCreateController(pGameServer, Entry.m_Info);
	}
	return nullptr;
}

const char *GameModeScoreKindName(EGameModeScoreKind ScoreKind)
{
	switch(ScoreKind)
	{
	case EGameModeScoreKind::POINTS:
		return "points";
	case EGameModeScoreKind::TIME:
		return "time";
	}
	return "points";
}
