#include "fng.h"

#include <generated/protocol7.h>

#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/tdm.h>

static constexpr int TEAMS = protocol7::GAMEFLAG_TEAMS;

static const CGameModeRegistration gs_Fng({"fng", "fng", TEAMS, false}, NewGameController<CGameControllerFng<CGameControllerVanillaTDM, WEAPON_LASER>>);
static const CGameModeRegistration gs_BoomFng({"boomfng", "boomfng", TEAMS, false}, NewGameController<CGameControllerFng<CGameControllerVanillaTDM, WEAPON_GRENADE>>);
static const CGameModeRegistration gs_SoloFng({"solofng", "solofng", 0, false}, NewGameController<CGameControllerFng<CGameControllerVanillaDM, WEAPON_LASER>>);
static const CGameModeRegistration gs_BoloFng({"bolofng", "bolofng", 0, false}, NewGameController<CGameControllerFng<CGameControllerVanillaDM, WEAPON_GRENADE>>);
