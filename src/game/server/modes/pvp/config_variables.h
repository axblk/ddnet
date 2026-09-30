// This file can be included several times.

// The settings of the shared PvP layer (CGameControllerPvP), names and defaults as in ddnet-insta.

MACRO_CONFIG_INT(SvRespawnProtectionMs, sv_respawn_protection_ms, 0, 0, 999999999, CFGFLAG_SAVE | CFGFLAG_SERVER, "Milliseconds after spawning in which a player can neither be hurt nor hurt others in the PvP modes like iCTF or zCatch (0 off)")
MACRO_CONFIG_INT(SvKillingspreeKills, sv_killingspree_kills, 0, 0, 20, CFGFLAG_SAVE | CFGFLAG_SERVER, "Kills in a row that make a killing spree the server tells everybody about in the PvP modes (0 off)")
MACRO_CONFIG_INT(SvKillingspreeResetOnRoundEnd, sv_killingspree_reset_on_round_end, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Whether the killing sprees end when a round ends in the PvP modes")
MACRO_CONFIG_INT(SvAnticamper, sv_anticamper, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Punish players who stay in one spot for too long in the PvP modes")
MACRO_CONFIG_INT(SvAnticamperFreeze, sv_anticamper_freeze, 7, 0, 15, CFGFLAG_SAVE | CFGFLAG_SERVER, "Seconds a camper is frozen for (0 kills the camper instead)")
MACRO_CONFIG_INT(SvAnticamperTime, sv_anticamper_time, 10, 5, 120, CFGFLAG_SAVE | CFGFLAG_SERVER, "Seconds a player may stay in one spot before the anticamper strikes")
MACRO_CONFIG_INT(SvAnticamperRange, sv_anticamper_range, 200, 0, 1000, CFGFLAG_SAVE | CFGFLAG_SERVER, "How far a player has to move to leave the spot, in units along each axis")
MACRO_CONFIG_INT(SvAllowZoom, sv_allow_zoom, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Let DDNet clients zoom in the PvP modes")
