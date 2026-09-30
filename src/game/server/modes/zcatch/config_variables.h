// This file can be included several times.

// The settings of zCatch (modes/zcatch), names and defaults as in ddnet-insta where it has them.

MACRO_CONFIG_INT(SvZcatchMinPlayers, sv_zcatch_min_players, 5, 2, MAX_CLIENTS, CFGFLAG_SAVE | CFGFLAG_SERVER, "Players a zCatch round needs to start, with fewer it is a release game")
MACRO_CONFIG_INT(SvReleaseGame, sv_release_game, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Always a release game in zCatch: who is hit comes back right away")
MACRO_CONFIG_STR(SvZcatchColors, sv_zcatch_colors, 16, "teetime", CFGFLAG_SAVE | CFGFLAG_SERVER, "Colours of the kills in zCatch: teetime or savander")
