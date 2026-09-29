#include <base/str.h>

#include <generated/protocol7.h>

#include <game/server/mode/game_mode_registry.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

TEST(GameModeRegistry, FindsByName)
{
	const struct
	{
		const char *m_pName;
		const char *m_pGameType;
		int m_GameFlags;
		bool m_DDRace;
	} aExpected[] = {
		{"ddnet", "DDraceNetwork", protocol7::GAMEFLAG_RACE, true},
		{"DM", "DM", 0, false},
		{"tdm", "TDM", protocol7::GAMEFLAG_TEAMS, false},
		{"Ctf", "CTF", protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS, false},
		{"ictf", "iCTF", protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS, false},
		{"lms", "LMS", protocol7::GAMEFLAG_SURVIVAL, false},
		{"LTS", "LTS", protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_SURVIVAL, false},
	};
	for(const auto &Expected : aExpected)
	{
		const CGameModeInfo *pInfo = FindGameMode(Expected.m_pName);
		ASSERT_NE(pInfo, nullptr) << Expected.m_pName;
		EXPECT_STREQ(pInfo->m_pGameType, Expected.m_pGameType);
		EXPECT_EQ(pInfo->m_GameFlags, Expected.m_GameFlags);
		EXPECT_EQ(pInfo->m_DDRace, Expected.m_DDRace);
	}
	EXPECT_EQ(FindGameMode("1on1"), nullptr);
	EXPECT_EQ(FindGameMode("vanilla.dm"), nullptr);
	EXPECT_EQ(FindGameMode(""), nullptr);
}

TEST(GameModeRegistry, ListsSorted)
{
	const std::vector<const CGameModeInfo *> vpModes = GameModes();
	ASSERT_FALSE(vpModes.empty());
	std::string Names;
	for(size_t i = 0; i < vpModes.size(); i++)
	{
		if(i > 0)
		{
			EXPECT_LT(str_comp(vpModes[i - 1]->m_pName, vpModes[i]->m_pName), 0);
		}
		EXPECT_EQ(FindGameMode(vpModes[i]->m_pName), vpModes[i]);
		Names += (Names.empty() ? "" : ", ") + std::string(vpModes[i]->m_pName);
	}
	EXPECT_EQ(Names, GameModeNames());
}
