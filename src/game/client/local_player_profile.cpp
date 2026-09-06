#include "local_player_profile.h"

#include <engine/shared/config.h>

#include <algorithm>
#include <utility>

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

bool CLocalPlayerProfileBindings::Set(CStreamId StreamId, CLocalPlayerProfile Profile)
{
	if(!StreamId.IsValid())
		return false;
	if(CLocalPlayerProfile *pExisting = Find(StreamId))
	{
		*pExisting = std::move(Profile);
		return true;
	}
	m_vBindings.push_back({StreamId, std::move(Profile)});
	return true;
}

CLocalPlayerProfile *CLocalPlayerProfileBindings::Find(CStreamId StreamId)
{
	const auto Found = std::find_if(m_vBindings.begin(), m_vBindings.end(), [StreamId](const CBinding &Binding) { return Binding.m_StreamId == StreamId; });
	return Found == m_vBindings.end() ? nullptr : &Found->m_Profile;
}

const CLocalPlayerProfile *CLocalPlayerProfileBindings::Find(CStreamId StreamId) const
{
	const auto Found = std::find_if(m_vBindings.begin(), m_vBindings.end(), [StreamId](const CBinding &Binding) { return Binding.m_StreamId == StreamId; });
	return Found == m_vBindings.end() ? nullptr : &Found->m_Profile;
}

bool CLocalPlayerProfileBindings::Remove(CStreamId StreamId)
{
	const auto Found = std::find_if(m_vBindings.begin(), m_vBindings.end(), [StreamId](const CBinding &Binding) { return Binding.m_StreamId == StreamId; });
	if(Found == m_vBindings.end())
		return false;
	m_vBindings.erase(Found);
	return true;
}
