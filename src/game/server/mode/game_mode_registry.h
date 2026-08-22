#ifndef GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
#define GAME_SERVER_MODE_GAME_MODE_REGISTRY_H

#include <memory>

class CGameServices;
class IGameController;

struct CGameModeInfo
{
	// What sv_gametype selects the mode by, compared case-insensitively
	const char *m_pName;
	// What the server advertises, with "Test" in front while sv_test_cmds is on
	const char *m_pGameType;
	int m_GameFlags;
	// DDRace rules: tune zones, and a time instead of points on the scoreboard
	bool m_DDRace;
};

using FCreateGameController = std::unique_ptr<IGameController> (*)(CGameServices &Services, const CGameModeInfo &Info);

template<typename TController>
std::unique_ptr<IGameController> NewGameController(CGameServices &Services, const CGameModeInfo &Info)
{
	return std::make_unique<TController>(Services, Info);
}

// A mode registers itself with a static one of these next to its controller:
//   static const CGameModeRegistration gs_Mode({"name", "Type", 0, false}, NewGameController<CMyController>);
// The server sources are built as an object library, so the linker keeps every registration.
class CGameModeRegistration
{
public:
	CGameModeRegistration(const CGameModeInfo &Info, FCreateGameController pfnCreate);

	const CGameModeInfo m_Info;
	const FCreateGameController m_pfnCreate;
	const CGameModeRegistration *const m_pNext;
};

const CGameModeInfo *FindGameMode(const char *pName);
std::unique_ptr<IGameController> CreateGameController(const char *pName, CGameServices &Services);

#endif // GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
