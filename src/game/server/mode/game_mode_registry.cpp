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

const CGameModeRegistry::CEntry *CGameModeRegistry::FindEntry(const char *pId) const
{
	if(!pId)
		return nullptr;

	for(const CEntry &Entry : m_vEntries)
	{
		if(str_comp(Entry.m_Info.m_pId, pId) == 0)
			return &Entry;
	}
	return nullptr;
}

const CGameModeRegistry::CEntry *CGameModeRegistry::ResolveEntry(const char *pId) const
{
	if(const CEntry *pExact = FindEntry(pId))
		return pExact;
	if(!pId)
		return nullptr;

	// `sv_gametype ctf` is how a server has been configured since long before
	// the modes had ids of their own, so the name a mode advertises is an alias
	// for it. A mod may register another mode under a name that is already
	// taken; the alias then means the one that was registered first, and the
	// mod's own id still selects it unambiguously.
	for(const CEntry &Entry : m_vEntries)
	{
		if(str_comp_nocase(Entry.m_Info.m_pGameType, pId) == 0)
			return &Entry;
	}
	return nullptr;
}

const CGameModeInfo *CGameModeRegistry::Find(const char *pId) const
{
	const CEntry *pEntry = FindEntry(pId);
	return pEntry == nullptr ? nullptr : &pEntry->m_Info;
}

const CGameModeInfo *CGameModeRegistry::Resolve(const char *pId) const
{
	const CEntry *pEntry = ResolveEntry(pId);
	return pEntry == nullptr ? nullptr : &pEntry->m_Info;
}

std::unique_ptr<IGameController> CGameModeRegistry::Create(const char *pId, CGameServices &Services) const
{
	const CEntry *pEntry = ResolveEntry(pId);
	return pEntry == nullptr ? nullptr : pEntry->m_pfnCreateController(Services, pEntry->m_Info);
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
