#include "rules.h"

#include <generated/protocol7.h>

#include <game/server/modes/pvp/pvp.h>
#include <game/server/modes/vanilla/ctf.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/tdm.h>

static constexpr int TEAMS = protocol7::GAMEFLAG_TEAMS;
static constexpr int FLAGS = protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS;

static const CGameModeRegistration gs_IDM({"idm", "iDM", 0, false}, NewGameController<CGameControllerLaserInstagib<CGameControllerPvP<CGameControllerVanillaDM>>>);
static const CGameModeRegistration gs_ITDM({"itdm", "iTDM", TEAMS, false}, NewGameController<CGameControllerLaserInstagib<CGameControllerPvP<CGameControllerVanillaTDM>>>);
static const CGameModeRegistration gs_ICTF({"ictf", "iCTF", FLAGS, false}, NewGameController<CGameControllerLaserInstagib<CGameControllerPvP<CGameControllerVanillaCTF>>>);
static const CGameModeRegistration gs_GDM({"gdm", "gDM", 0, false}, NewGameController<CGameControllerGrenadeInstagib<CGameControllerPvP<CGameControllerVanillaDM>>>);
static const CGameModeRegistration gs_GTDM({"gtdm", "gTDM", TEAMS, false}, NewGameController<CGameControllerGrenadeInstagib<CGameControllerPvP<CGameControllerVanillaTDM>>>);
static const CGameModeRegistration gs_GCTF({"gctf", "gCTF", FLAGS, false}, NewGameController<CGameControllerGrenadeInstagib<CGameControllerPvP<CGameControllerVanillaCTF>>>);
