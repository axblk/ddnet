#include "game_mode_registry.h"

#include <base/dbg.h>
#include <base/str.h>

#include <game/server/gamecontroller.h>

// zero-initialized before any registration runs, whatever the order of the static initializers
static const CGameModeRegistration *gs_pFirstGameMode = nullptr;

CGameModeRegistration::CGameModeRegistration(const CGameModeInfo &Info, FCreateGameController pfnCreate) :
	m_Info(Info),
	m_pfnCreate(pfnCreate),
	m_pNext(gs_pFirstGameMode)
{
	gs_pFirstGameMode = this;
}

static const CGameModeRegistration *FindRegistration(const char *pName)
{
	const CGameModeRegistration *pFound = nullptr;
	for(const CGameModeRegistration *pMode = gs_pFirstGameMode; pMode; pMode = pMode->m_pNext)
	{
		if(str_comp_nocase(pMode->m_Info.m_pName, pName) != 0)
			continue;
		dbg_assert(!pFound, "two game modes are called '%s'", pName);
		pFound = pMode;
	}
	return pFound;
}

const CGameModeInfo *FindGameMode(const char *pName)
{
	const CGameModeRegistration *pMode = FindRegistration(pName);
	return pMode ? &pMode->m_Info : nullptr;
}

std::unique_ptr<IGameController> CreateGameController(const char *pName, CGameServices &Services)
{
	const CGameModeRegistration *pMode = FindRegistration(pName);
	return pMode ? pMode->m_pfnCreate(Services, pMode->m_Info) : nullptr;
}
