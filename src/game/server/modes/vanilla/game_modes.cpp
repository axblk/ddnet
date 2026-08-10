#include "game_modes.h"

#include "ctf.h"
#include "dm.h"
#include "tdm.h"

#include <game/server/mode/game_mode_registry.h>

bool RegisterVanillaGameModes(CGameModeRegistry &Registry)
{
	return Registry.Register(
		       {"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN},
		       [](CGameContext *pGameServer, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(pGameServer, Info); }) &&
	       Registry.Register(
		       {"vanilla.1on1", "Vanilla 1on1", "1on1", "Test1on1", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN, 2},
		       [](CGameContext *pGameServer, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(pGameServer, Info); }) &&
	       Registry.Register(
		       {"vanilla.tdm", "Vanilla TDM", "TDM", "TestTDM", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN},
		       [](CGameContext *pGameServer, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaTDM>(pGameServer, Info); }) &&
	       Registry.Register(
		       {"vanilla.ctf", "Vanilla CTF", "CTF", "TestCTF", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN},
		       [](CGameContext *pGameServer, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaCTF>(pGameServer, Info); });
}
