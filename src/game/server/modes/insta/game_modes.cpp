#include "game_modes.h"

#include <generated/protocol7.h>

#include <game/server/mode/game_mode_registry.h>
#include <game/server/modes/insta/rules.h>
#include <game/server/modes/vanilla/ctf.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/tdm.h>

namespace
{
	using CGameControllerInstagibDM = CGameControllerLaserInstagib<CGameControllerVanillaDM>;
	using CGameControllerInstagibTDM = CGameControllerLaserInstagib<CGameControllerVanillaTDM>;
	using CGameControllerInstagibCTF = CGameControllerLaserInstagib<CGameControllerVanillaCTF>;
	using CGameControllerGrenadeDM = CGameControllerGrenadeInstagib<CGameControllerVanillaDM>;
	using CGameControllerGrenadeTDM = CGameControllerGrenadeInstagib<CGameControllerVanillaTDM>;
	using CGameControllerGrenadeCTF = CGameControllerGrenadeInstagib<CGameControllerVanillaCTF>;
}

bool RegisterInstagibGameModes(CGameModeRegistry &Registry)
{
	return Registry.Register(
		       {"insta.idm", "Instagib DM", "iDM", "TestiDM", EGameModeScoreKind::POINTS, 0, 0, false, EPhysicsRuleset::VANILLA, CompetitiveGameModeReport("insta.idm@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerInstagibDM>(Services, Info); }) &&
	       Registry.Register(
		       {"insta.itdm", "Instagib TDM", "iTDM", "TestiTDM", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS, 0, false, EPhysicsRuleset::VANILLA, CompetitiveGameModeReport("insta.itdm@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerInstagibTDM>(Services, Info); }) &&
	       Registry.Register(
		       {"insta.ictf", "Instagib CTF", "iCTF", "TestiCTF", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS, 0, false, EPhysicsRuleset::VANILLA, CompetitiveGameModeReport("insta.ictf@ddnet.org", true)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerInstagibCTF>(Services, Info); }) &&
	       Registry.Register(
		       {"insta.gdm", "Grenade Instagib DM", "gDM", "TestgDM", EGameModeScoreKind::POINTS, 0, 0, false, EPhysicsRuleset::VANILLA, CompetitiveGameModeReport("insta.gdm@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerGrenadeDM>(Services, Info); }) &&
	       Registry.Register(
		       {"insta.gtdm", "Grenade Instagib TDM", "gTDM", "TestgTDM", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS, 0, false, EPhysicsRuleset::VANILLA, CompetitiveGameModeReport("insta.gtdm@ddnet.org", false)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerGrenadeTDM>(Services, Info); }) &&
	       Registry.Register(
		       {"insta.gctf", "Grenade Instagib CTF", "gCTF", "TestgCTF", EGameModeScoreKind::POINTS, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS, 0, false, EPhysicsRuleset::VANILLA, CompetitiveGameModeReport("insta.gctf@ddnet.org", true)},
		       [](CGameServices &Services, const CGameModeInfo &Info) -> std::unique_ptr<IGameController> { return std::make_unique<CGameControllerGrenadeCTF>(Services, Info); });
}
