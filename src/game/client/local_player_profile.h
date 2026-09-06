#ifndef GAME_CLIENT_LOCAL_PLAYER_PROFILE_H
#define GAME_CLIENT_LOCAL_PLAYER_PROFILE_H

#include <string>

class CConfig;

class CLocalPlayerProfile
{
public:
	std::string m_Name;
	std::string m_Clan;
	int m_Country = -1;
	std::string m_Skin;
	bool m_UseCustomColor = false;
	unsigned m_ColorBody = 0;
	unsigned m_ColorFeet = 0;

	static CLocalPlayerProfile FromLegacyConfig(const CConfig &Config, bool UseDummyProfile, const char *pResolvedName);
};

#endif // GAME_CLIENT_LOCAL_PLAYER_PROFILE_H
