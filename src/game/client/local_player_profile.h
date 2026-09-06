#ifndef GAME_CLIENT_LOCAL_PLAYER_PROFILE_H
#define GAME_CLIENT_LOCAL_PLAYER_PROFILE_H

#include <engine/client/stream.h>

#include <string>
#include <vector>

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

class CLocalPlayerProfileBindings
{
public:
	class CBinding
	{
	public:
		CStreamId m_StreamId;
		CLocalPlayerProfile m_Profile;
	};

private:
	std::vector<CBinding> m_vBindings;

public:
	bool Set(CStreamId StreamId, CLocalPlayerProfile Profile);
	CLocalPlayerProfile *Find(CStreamId StreamId);
	const CLocalPlayerProfile *Find(CStreamId StreamId) const;
	bool Remove(CStreamId StreamId);
	size_t NumProfiles() const { return m_vBindings.size(); }
};

#endif // GAME_CLIENT_LOCAL_PLAYER_PROFILE_H
