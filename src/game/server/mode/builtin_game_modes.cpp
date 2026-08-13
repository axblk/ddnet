#include "builtin_game_modes.h"

#include "game_mode_registry.h"

#include <generated/protocol7.h>

#include <game/server/gamemodes/ddnet.h>
#include <game/server/gamemodes/ddrace.h>
#include <game/server/modes/insta/game_modes.h>
#include <game/server/modes/vanilla/game_modes.h>
#include <game/server/modes/zcatch/game_modes.h>

bool RegisterBuiltInGameModes(CGameModeRegistry &Registry)
{
	const int BothProtocols = GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN;
	return Registry.Register(
		       {"ddnet", "DDNet", "DDraceNetwork", "TestDDraceNetwork", EGameModeScoreKind::TIME, protocol7::GAMEFLAG_RACE, BothProtocols, 0, true, EPhysicsRuleset::DDNET},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerDDNet>(Services, Info); }) &&
	       Registry.Register(
		       {"mod", "Mod", "Mod", "TestMod", EGameModeScoreKind::TIME, 0, BothProtocols, 0, true, EPhysicsRuleset::DDNET},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerDDRace>(Services, Info); }) &&
	       RegisterVanillaGameModes(Registry) &&
	       RegisterInstagibGameModes(Registry) &&
	       RegisterZCatchGameModes(Registry);
}
