// This file can be included several times.

// The settings of the instagib rules (CGameControllerInstagib), names and defaults as in ddnet-insta.

MACRO_CONFIG_INT(SvDamageNeededForKill, sv_damage_needed_for_kill, 4, 0, 5, CFGFLAG_SAVE | CFGFLAG_SERVER, "Damage a grenade has to do to kill in the instagib modes, less only pushes")
MACRO_CONFIG_INT(SvOnlyWallshotKills, sv_only_wallshot_kills, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Only a laser that bounced off a wall kills in the instagib modes")
MACRO_CONFIG_INT(SvSprayprotection, sv_sprayprotection, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "A grenade only reaches the players its shooter could see when firing, in the instagib modes")
MACRO_CONFIG_INT(SvGrenadeAmmoRegen, sv_grenade_ammo_regen, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Limited grenades that come back over time in the instagib modes, as on gCTF servers (0 endless grenades)")
MACRO_CONFIG_INT(SvGrenadeAmmoRegenTime, sv_grenade_ammo_regen_time, 128, 1, 9000, CFGFLAG_SAVE | CFGFLAG_SERVER, "Milliseconds for a grenade to come back")
MACRO_CONFIG_INT(SvGrenadeAmmoRegenNum, sv_grenade_ammo_regen_num, 6, 1, 10, CFGFLAG_SAVE | CFGFLAG_SERVER, "Grenades a player can have")
MACRO_CONFIG_INT(SvGrenadeAmmoRegenSpeed, sv_grenade_ammo_regen_speed, 1, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "A grenade that pushes its shooter comes back")
MACRO_CONFIG_INT(SvGrenadeAmmoRegenOnKill, sv_grenade_ammo_regen_on_kill, 2, 0, 2, CFGFLAG_SAVE | CFGFLAG_SERVER, "Grenades a hit brings back (0 none, 1 one, 2 all)")
