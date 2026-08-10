#include "local_player_profile.h"

#include <engine/shared/config.h>

#include <algorithm>
#include <utility>

CLocalPlayerProfile CLocalPlayerProfile::FromLegacyConfig(const CConfig &Config, bool Dummy, const char *pResolvedName)
{
	CLocalPlayerProfile Profile;
	Profile.m_Name = pResolvedName ? pResolvedName : "";
	Profile.m_Clan = Dummy ? Config.m_ClDummyClan : Config.m_PlayerClan;
	Profile.m_Country = Dummy ? Config.m_ClDummyCountry : Config.m_PlayerCountry;
	Profile.m_Skin = Dummy ? Config.m_ClDummySkin : Config.m_ClPlayerSkin;
	Profile.m_UseCustomColor = Dummy ? Config.m_ClDummyUseCustomColor : Config.m_ClPlayerUseCustomColor;
	Profile.m_ColorBody = Dummy ? Config.m_ClDummyColorBody : Config.m_ClPlayerColorBody;
	Profile.m_ColorFeet = Dummy ? Config.m_ClDummyColorFeet : Config.m_ClPlayerColorFeet;
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
