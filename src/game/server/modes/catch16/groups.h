#ifndef GAME_SERVER_MODES_CATCH16_GROUPS_H
#define GAME_SERVER_MODES_CATCH16_GROUPS_H

#include <engine/shared/protocol.h>

#include <array>
#include <bitset>
#include <vector>

/**
 * The colour groups of catch16, by client id: everybody starts as their
 * own group, and whoever is hit joins the group of the one who hit them.
 * A group is known by the one who founded it, whose slot gives the colours.
 */
class CCatch16Groups
{
	std::array<int, MAX_CLIENTS> m_aGroups;

public:
	CCatch16Groups() { Reset(); }

	int Group(int ClientId) const { return m_aGroups[ClientId]; }
	bool SameGroup(int ClientId, int OtherId) const { return m_aGroups[ClientId] == m_aGroups[OtherId]; }
	// whether the player changed the group
	bool Join(int ClientId, int Group);
	// back into the own group
	void Leave(int ClientId) { m_aGroups[ClientId] = ClientId; }
	/**
	 * The founder of a group left the server: the others in the group go back into their own ones.
	 *
	 * @return Who went back.
	 */
	std::vector<int> Dissolve(int FounderId);
	// everybody in their own group
	void Reset();

	/**
	 * The group all players of Playing are in, -1 if there are several or fewer than two players.
	 */
	int OnlyGroup(const std::bitset<MAX_CLIENTS> &Playing) const;
	// the groups of Playing with the most members, if they have more than one; empty otherwise
	std::vector<int> BiggestGroups(const std::bitset<MAX_CLIENTS> &Playing) const;
};

namespace Catch16
{
	// the colours of a group, packed HSL for body and feet, after catch64
	struct CColors
	{
		int m_Body;
		int m_Feet;
	};
	CColors GroupColors(int FounderId);
	// the name of the colour of a group, like "cherry" or "black-cyan"
	void GroupColorName(int FounderId, char *pBuf, int BufSize);
}

#endif // GAME_SERVER_MODES_CATCH16_GROUPS_H
