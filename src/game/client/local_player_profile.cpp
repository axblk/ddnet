#include "local_player_profile.h"

#include <engine/shared/config.h>

CLocalPlayerProfile CLocalPlayerProfile::FromLegacyConfig(const CConfig &Config, bool UseDummyProfile, const char *pResolvedName)
{
	CLocalPlayerProfile Profile;
	Profile.m_Name = pResolvedName ? pResolvedName : "";
	Profile.m_Clan = UseDummyProfile ? Config.m_ClDummyClan : Config.m_PlayerClan;
	Profile.m_Country = UseDummyProfile ? Config.m_ClDummyCountry : Config.m_PlayerCountry;
	Profile.m_Skin = UseDummyProfile ? Config.m_ClDummySkin : Config.m_ClPlayerSkin;
	Profile.m_UseCustomColor = UseDummyProfile ? Config.m_ClDummyUseCustomColor : Config.m_ClPlayerUseCustomColor;
	Profile.m_ColorBody = UseDummyProfile ? Config.m_ClDummyColorBody : Config.m_ClPlayerColorBody;
	Profile.m_ColorFeet = UseDummyProfile ? Config.m_ClDummyColorFeet : Config.m_ClPlayerColorFeet;
	return Profile;
}
