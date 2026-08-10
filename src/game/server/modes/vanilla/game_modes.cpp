#include "game_modes.h"

#include "dm.h"

#include <game/server/mode/game_mode_registry.h>

bool RegisterVanillaGameModes(CGameModeRegistry &Registry)
{
	return Registry.Register(
		{"vanilla.dm", "Vanilla DM", "DM", "TestDM", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN},
		[](CGameContext *pGameServer, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerVanillaDM>(pGameServer, Info); });
}
