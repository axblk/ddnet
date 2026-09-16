#include <game/map/document/settings.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace map_document;

// The lines a server runs when it loads a map. Nothing checks them today until
// a server refuses to start, which is the wrong moment to find out that a
// variable is spelled wrong.

TEST(Settings, TheListIsTheServersOwnAndTheCommandsBeside)
{
	const std::vector<CMapSetting> &vSettings = KnownSettings();
	EXPECT_GE(vSettings.size(), 20u);

	const CMapSetting *pDeepfly = FindSetting("sv_deepfly");
	ASSERT_NE(pDeepfly, nullptr) << "a variable with CFGFLAG_GAME is one a map may set";
	EXPECT_TRUE(pDeepfly->m_IsVariable);
	EXPECT_EQ(pDeepfly->m_Min, 0);
	EXPECT_EQ(pDeepfly->m_Max, 1);
	EXPECT_FALSE(pDeepfly->m_Help.empty());
	EXPECT_EQ(FindSetting("SV_DEEPFLY"), pDeepfly) << "a console does not care about case";

	const CMapSetting *pZone = FindSetting("tune_zone");
	ASSERT_NE(pZone, nullptr);
	EXPECT_FALSE(pZone->m_IsVariable);
	ASSERT_EQ(pZone->m_Args.size(), 3u);
	EXPECT_EQ(pZone->m_Args[0].m_Name, "zone");
	EXPECT_EQ(pZone->m_Args[0].m_Type, 'i');
	EXPECT_EQ(pZone->m_Args[2].m_Type, 'f');

	// A variable a map may not set is not here, however real it is.
	EXPECT_EQ(FindSetting("sv_name"), nullptr);
	EXPECT_EQ(FindSetting("sv_nonsense"), nullptr);
}

TEST(Settings, ALineIsSplitTheWayAConsoleWouldSplitIt)
{
	int Comment = 0;
	EXPECT_EQ(SplitSetting("tune ground_jump_impulse 12.5", &Comment),
		(std::vector<std::string>{"tune", "ground_jump_impulse", "12.5"}));
	EXPECT_EQ(Comment, -1);

	// Quotes hold a word together and a backslash escapes.
	EXPECT_EQ(SplitSetting(R"(tune_zone_enter 1 "you are \"in\" now")"),
		(std::vector<std::string>{"tune_zone_enter", "1", "you are \"in\" now"}));
	// Runs of spaces are one space.
	EXPECT_EQ(SplitSetting("  sv_deepfly   0  "), (std::vector<std::string>{"sv_deepfly", "0"}));

	// An unquoted hash starts a comment; a quoted one is a hash.
	EXPECT_EQ(SplitSetting("sv_deepfly 0 # off for this map", &Comment),
		(std::vector<std::string>{"sv_deepfly", "0"}));
	EXPECT_EQ(Comment, 13);
	EXPECT_EQ(SplitSetting(R"(mapbug "grenade#doubleexplosion")"),
		(std::vector<std::string>{"mapbug", "grenade#doubleexplosion"}));
}

TEST(Settings, WhatIsWrongWithALineIsSaidBeforeAServerSeesIt)
{
	EXPECT_EQ(CheckSetting("sv_deepfly 0"), "");
	EXPECT_EQ(CheckSetting("tune_zone 1 ground_jump_impulse 12.5"), "");
	EXPECT_EQ(CheckSetting("# just a note"), "") << "a line that is only a comment says nothing";
	EXPECT_EQ(CheckSetting("sv_deepfly 0 # and a note"), "");

	EXPECT_EQ(CheckSetting("sv_deepfy 0"), "a server has no 'sv_deepfy'");
	EXPECT_EQ(CheckSetting("sv_deepfly"), "'sv_deepfly' wants value");
	EXPECT_EQ(CheckSetting("sv_deepfly yes"), "value is a whole number, not 'yes'");
	EXPECT_EQ(CheckSetting("sv_deepfly 2"), "value is between 0 and 1");
	EXPECT_EQ(CheckSetting("sv_deepfly 0 1"), "'sv_deepfly' takes 1 argument, not 2");
	EXPECT_EQ(CheckSetting("tune_zone 1 ground_jump_impulse twelve"), "value is a number, not 'twelve'");
	EXPECT_EQ(CheckSetting("tune_zone one ground_jump_impulse 12"), "zone is a whole number, not 'one'");

	// The rest of a line is one argument however many words it holds.
	EXPECT_EQ(CheckSetting("tune_zone_enter 1 welcome to the zone"), "");
}

TEST(Settings, SayingTheSameThingTwiceIsFoundButTwoZonesAreTwoThings)
{
	const std::vector<std::string> vLines = {
		"sv_deepfly 0",
		"tune_zone 1 ground_jump_impulse 12",
		"tune_zone 2 ground_jump_impulse 13",
	};
	EXPECT_EQ(CollidingSetting(vLines, "sv_deepfly 1"), 0) << "a variable is set once";
	EXPECT_EQ(CollidingSetting(vLines, "SV_DEEPFLY 1"), 0);
	EXPECT_EQ(CollidingSetting(vLines, "tune_zone 1 ground_jump_impulse 99"), 1);
	EXPECT_EQ(CollidingSetting(vLines, "tune_zone 3 ground_jump_impulse 12"), -1)
		<< "another zone is another thing";
	EXPECT_EQ(CollidingSetting(vLines, "sv_teleport_hold_hook 1"), -1);
	EXPECT_EQ(CollidingSetting(vLines, "sv_nonsense 1"), -1)
		<< "nothing is the same as a line nobody understands";
}

TEST(Settings, WhatCouldBeMeantIsOfferedByWhatWasTypedSoFar)
{
	const std::vector<std::string> vTune = CompleteSetting("tune");
	EXPECT_EQ(vTune, (std::vector<std::string>{"tune", "tune_zone", "tune_zone_enter", "tune_zone_leave"}));
	EXPECT_TRUE(CompleteSetting("SV_DEEP") == std::vector<std::string>{"sv_deepfly"});
	EXPECT_TRUE(CompleteSetting("zzz").empty());
	EXPECT_EQ(CompleteSetting("").size(), KnownSettings().size()) << "nothing typed is everything offered";
}
