// The colours and their names come from catch64 (AssassinTee, zlib licence, https://github.com/AssassinTee/catch64),
// the rules follow catch16 by Clarus.
#include "groups.h"

#include <base/str.h>

#include <algorithm>

bool CCatch16Groups::Join(int ClientId, int Group)
{
	if(m_aGroups[ClientId] == Group)
		return false;
	m_aGroups[ClientId] = Group;
	return true;
}

std::vector<int> CCatch16Groups::Dissolve(int FounderId)
{
	std::vector<int> vMembers;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(m_aGroups[ClientId] == FounderId && ClientId != FounderId)
		{
			m_aGroups[ClientId] = ClientId;
			vMembers.push_back(ClientId);
		}
	}
	m_aGroups[FounderId] = FounderId;
	return vMembers;
}

void CCatch16Groups::Reset()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		m_aGroups[ClientId] = ClientId;
}

int CCatch16Groups::OnlyGroup(const std::bitset<MAX_CLIENTS> &Playing) const
{
	if(Playing.count() < 2)
		return -1;
	int Group = -1;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(!Playing.test(ClientId))
			continue;
		if(Group == -1)
			Group = m_aGroups[ClientId];
		else if(m_aGroups[ClientId] != Group)
			return -1;
	}
	return Group;
}

std::vector<int> CCatch16Groups::BiggestGroups(const std::bitset<MAX_CLIENTS> &Playing) const
{
	std::array<int, MAX_CLIENTS> aMembers{};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(Playing.test(ClientId))
			aMembers[m_aGroups[ClientId]]++;
	}
	const int Most = *std::max_element(aMembers.begin(), aMembers.end());
	std::vector<int> vGroups;
	if(Most < 2)
		return vGroups;
	for(int Group = 0; Group < MAX_CLIENTS; Group++)
	{
		if(aMembers[Group] == Most)
			vGroups.push_back(Group);
	}
	return vGroups;
}

// 16 hues spread so that neighbouring slots differ most, and the names catch64 gives them
static constexpr int HUE_STEPS = 16;
static constexpr std::array<int, HUE_STEPS> gs_aHueSteps = {0, 8, 4, 12, 2, 6, 10, 14, 1, 3, 5, 7, 9, 11, 13, 15};
static constexpr std::array<const char *, HUE_STEPS> gs_apColorNames = {"cherry", "cyan", "emerald", "grape", "sunflower", "mint", "orchid", "lavender", "tangerine", "lime", "avocado", "pool", "sky", "eggplant", "violet", "pink"};

// as 0.6 sends it, whose lightness starts at a half: 0 is the pure colour, and black is a grey
static int PackHsl(int Hue, int Saturation, int Lightness)
{
	return (Hue << 16) | (Saturation << 8) | Lightness;
}

Catch16::CColors Catch16::GroupColors(int FounderId)
{
	const int Color = PackHsl(gs_aHueSteps[FounderId % HUE_STEPS] * 16, 255, 0);
	const int White = PackHsl(0, 255, 255);
	const int Black = PackHsl(0, 0, 0);
	// the first 16 in the colour, then with white feet, then with a black and a white body
	switch(FounderId / HUE_STEPS)
	{
	case 0: return {Color, Color};
	case 1: return {Color, White};
	case 2: return {Black, Color};
	default: return {White, Color};
	}
}

void Catch16::GroupColorName(int FounderId, char *pBuf, int BufSize)
{
	const char *pName = gs_apColorNames[FounderId % HUE_STEPS];
	switch(FounderId / HUE_STEPS)
	{
	case 0: str_copy(pBuf, pName, BufSize); break;
	case 1: str_format(pBuf, BufSize, "%s-white", pName); break;
	case 2: str_format(pBuf, BufSize, "black-%s", pName); break;
	default: str_format(pBuf, BufSize, "white-%s", pName); break;
	}
}
