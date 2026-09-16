#include "game_modes.h"

#include "ctf.h"
#include "dm.h"
#include "tdm.h"

#include <generated/protocol7.h>

#include <game/server/mode/game_mode_registry.h>

bool RegisterVanillaGameModes(CGameModeRegistry &Registry)
{
	return Registry.Register(
		       {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(Services, Info); }) &&
	       Registry.Register(
		       {"vanilla.1on1", "Vanilla 1on1", "1on1", "Test1on1", EGameModeScoreKind::POINTS, 0, 2},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(Services, Info); }) &&
	       Registry.Register(
		       {"vanilla.tdm", "Vanilla TDM", "TDM", "TestTDM", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaTDM>(Services, Info); }) &&
	       Registry.Register(
		       {"vanilla.ctf", "Vanilla CTF", "CTF", "TestCTF", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaCTF>(Services, Info); });
}
