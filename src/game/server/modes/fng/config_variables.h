// This file can be included several times.

// The settings of the FNG family (modes/fng), names and defaults as in ddnet-insta.

MACRO_CONFIG_INT(SvHitFreezeDelay, sv_hit_freeze_delay, 10, 1, 30, CFGFLAG_SAVE | CFGFLAG_SERVER, "Seconds a hit freezes in FNG")
MACRO_CONFIG_INT(SvPlayerScoreSpikeNormal, sv_player_score_normal, 3, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the player who put a frozen enemy into normal spikes in FNG")
MACRO_CONFIG_INT(SvPlayerScoreSpikeGold, sv_player_score_gold, 6, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the player who put a frozen enemy into golden spikes in FNG")
MACRO_CONFIG_INT(SvPlayerScoreSpikeGreen, sv_player_score_green, 8, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the player who put a frozen enemy into green spikes in FNG")
MACRO_CONFIG_INT(SvPlayerScoreSpikePurple, sv_player_score_purple, 10, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the player who put a frozen enemy into purple spikes in FNG")
MACRO_CONFIG_INT(SvPlayerScoreSpikeTeam, sv_player_score_team, 5, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the player who put a frozen enemy into the spikes of the own team in FNG, lost for the other team's")
MACRO_CONFIG_INT(SvTeamScoreSpikeNormal, sv_team_score_normal, 5, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the team whose player put a frozen enemy into normal spikes in FNG")
MACRO_CONFIG_INT(SvTeamScoreSpikeGold, sv_team_score_gold, 12, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the team whose player put a frozen enemy into golden spikes in FNG")
MACRO_CONFIG_INT(SvTeamScoreSpikeGreen, sv_team_score_green, 15, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the team whose player put a frozen enemy into green spikes in FNG")
MACRO_CONFIG_INT(SvTeamScoreSpikePurple, sv_team_score_purple, 18, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the team whose player put a frozen enemy into purple spikes in FNG")
MACRO_CONFIG_INT(SvTeamScoreSpikeTeam, sv_team_score_team, 10, 0, 100, CFGFLAG_SAVE | CFGFLAG_SERVER, "Points for the team whose player put a frozen enemy into the spikes of the own team in FNG")
MACRO_CONFIG_INT(SvWrongSpikeFreeze, sv_wrong_spike_freeze, 10, 0, 30, CFGFLAG_SAVE | CFGFLAG_SERVER, "Seconds the player is frozen who put a frozen enemy into the spikes of the other team in FNG (0 not at all)")
MACRO_CONFIG_INT(SvFngHammer, sv_fng_hammer, 0, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "The hammer of FNG pushes as sv_hammer_scale_x/y and sv_melt_hammer_scale_x/y say")
MACRO_CONFIG_INT(SvHammerScaleX, sv_hammer_scale_x, 320, 1, 1000, CFGFLAG_SAVE | CFGFLAG_SERVER, "Percent of the sideways push of the FNG hammer on enemies and team mates who are not frozen (needs sv_fng_hammer)")
MACRO_CONFIG_INT(SvHammerScaleY, sv_hammer_scale_y, 120, 1, 1000, CFGFLAG_SAVE | CFGFLAG_SERVER, "Percent of the upward push of the FNG hammer on enemies and team mates who are not frozen (needs sv_fng_hammer)")
MACRO_CONFIG_INT(SvMeltHammerScaleX, sv_melt_hammer_scale_x, 50, 1, 1000, CFGFLAG_SAVE | CFGFLAG_SERVER, "Percent of the sideways push of the FNG hammer on frozen team mates (needs sv_fng_hammer)")
MACRO_CONFIG_INT(SvMeltHammerScaleY, sv_melt_hammer_scale_y, 50, 1, 1000, CFGFLAG_SAVE | CFGFLAG_SERVER, "Percent of the upward push of the FNG hammer on frozen team mates (needs sv_fng_hammer)")
MACRO_CONFIG_INT(SvAnnounceSteals, sv_announce_steals, 1, 0, 1, CFGFLAG_SAVE | CFGFLAG_SERVER, "Tell everybody when somebody put a player into the spikes whom another had frozen, in FNG")
