#include "game_modes.h"

#include "ctf.h"
#include "dm.h"
#include "tdm.h"

#include <generated/protocol7.h>

#include <game/server/mode/game_mode_registry.h>

bool RegisterVanillaGameModes(CGameModeRegistry &Registry)
{
	return Registry.Register(
		       {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, 0, false, CPhysicsRules::Vanilla(), CompetitiveGameModeReport("vanilla.dm@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(Services, Info); }) &&
	       Registry.Register(
		       {"vanilla.1on1", "Vanilla 1on1", "1on1", "Test1on1", EGameModeScoreKind::POINTS, 0, 2, false, CPhysicsRules::Vanilla(), CompetitiveGameModeReport("vanilla.1on1@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(Services, Info); }) &&
	       Registry.Register(
		       {"vanilla.tdm", "Vanilla TDM", "TDM", "TestTDM", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS, 0, false, CPhysicsRules::Vanilla(), CompetitiveGameModeReport("vanilla.tdm@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaTDM>(Services, Info); }) &&
	       Registry.Register(
		       {"vanilla.ctf", "Vanilla CTF", "CTF", "TestCTF", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS, 0, false, CPhysicsRules::Vanilla(), CompetitiveGameModeReport("vanilla.ctf@ddnet.org", true)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaCTF>(Services, Info); });
}
