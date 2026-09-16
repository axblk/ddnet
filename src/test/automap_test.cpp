#include "test.h"

#include <base/str.h>

#include <engine/shared/linereader.h>
#include <engine/storage.h>

#include <game/map/document/automap.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

using namespace map_document;

// The automapper: reading a `.rules` file and putting the tiles it asks for
// into a layer. What it does is what the editor in the client does - a map
// automapped in the browser has to come out as it would have come out
// natively - so the tests here are about the grammar and about the tiles.

namespace
{
	std::string RulesText(const char *pName)
	{
		std::unique_ptr<IStorage> pStorage(CreateLocalStorage());
		if(pStorage == nullptr)
			return std::string();
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "data/editor/automap/%s.rules", pName);
		void *pData = nullptr;
		unsigned Size = 0;
		if(!pStorage->ReadFile(aPath, IStorage::TYPE_ALL, &pData, &Size))
			return std::string();
		std::string Text((const char *)pData, Size);
		free(pData);
		return Text;
	}

	CTileLayer Layer(int Width, int Height)
	{
		return CTileLayer(ETileLayerKind::TILES, Width, Height);
	}

	void Put(CTileLayer &Into, int x, int y, int Index, int Flags = 0)
	{
		CTile Tile = {};
		Tile.m_Index = (unsigned char)Index;
		Tile.m_Flags = (unsigned char)Flags;
		Into.m_Tiles.Set(x, y, Tile);
	}

	int IndexAt(const CTileLayer &Of, int x, int y) { return Of.m_Tiles.Get(x, y).m_Index; }
} // namespace

TEST(Automap, EveryRulesFileTheGameShipsWithIsRead)
{
	static const char *s_apNames[] = {"basic_freeze", "ddmax_freeze", "ddnet_grass", "ddnet_tiles",
		"ddnet_walls", "desert_main", "fadeout", "generic_clear", "generic_unhookable",
		"generic_unhookable_0.7", "grass_main", "jungle_main", "jungle_midground", "round_tiles",
		"water", "winter_main"};
	for(const char *pName : s_apNames)
	{
		const std::string Text = RulesText(pName);
		ASSERT_FALSE(Text.empty()) << pName;
		const CAutomapRules Rules = ParseAutomapRules(Text.c_str());
		EXPECT_GT(Rules.NumConfigs(), 0u) << pName;
		for(size_t Config = 0; Config < Rules.NumConfigs(); ++Config)
		{
			EXPECT_NE(Rules.ConfigName(Config)[0], '\0') << pName << " config " << Config;
			EXPECT_GT(Rules.m_vConfigs[Config].m_vRuns.size(), 0u) << pName;
		}
	}
}

TEST(Automap, AnOrListWithFlagsIsReadWordForWord)
{
	// `round_tiles` has the one line in all of them that uses "OR" together
	// with flags, which is where a parser that counts words wrong shows it.
	const std::string Text = RulesText("round_tiles");
	ASSERT_FALSE(Text.empty());
	const CAutomapRules Rules = ParseAutomapRules(Text.c_str());
	ASSERT_GT(Rules.NumConfigs(), 0u);
	EXPECT_STREQ(Rules.ConfigName(0), "DDNet");

	const CAutomapRules::CIndexRule &First = Rules.m_vConfigs[0].m_vRuns[0].m_vIndexRules[0];
	EXPECT_EQ(First.m_Id, 0);
	ASSERT_GE(First.m_vRules.size(), 1u);
	const CAutomapRules::CPosRule &Rule = First.m_vRules[0];
	EXPECT_EQ(Rule.m_X, 0);
	EXPECT_EQ(Rule.m_Y, 0);
	EXPECT_EQ(Rule.m_Value, CAutomapRules::CPosRule::INDEX);
	ASSERT_EQ(Rule.m_vIndexList.size(), 4u);
	EXPECT_EQ(Rule.m_vIndexList[0].m_Id, 33);
	EXPECT_EQ(Rule.m_vIndexList[0].m_Flag, 0);
	EXPECT_FALSE(Rule.m_vIndexList[0].m_TestFlag) << "an index with no flags after it tests none";
	EXPECT_EQ(Rule.m_vIndexList[1].m_Flag, TILEFLAG_XFLIP);
	EXPECT_TRUE(Rule.m_vIndexList[1].m_TestFlag);
	EXPECT_EQ(Rule.m_vIndexList[2].m_Flag, TILEFLAG_YFLIP);
	EXPECT_EQ(Rule.m_vIndexList[3].m_Flag, TILEFLAG_XFLIP | TILEFLAG_YFLIP);

	// The rule stands on a tile that has to be a 33, so air can be skipped
	// outright - and it is not a rule about air, so full tiles cannot be.
	EXPECT_TRUE(First.m_SkipEmpty);
	EXPECT_FALSE(First.m_SkipFull);
}

TEST(Automap, ARuleThatSaysNothingAboutItsOwnTileIsAboutATileThatIsThere)
{
	// Neither rule says anything about the tile it stands on, so both get
	// "not air" written in - which is what makes an automapper leave air
	// alone instead of filling the map with tile 1.
	const CAutomapRules Rules = ParseAutomapRules(
		"[Test]\n"
		"Index 1\n"
		"Pos 0 -1 EMPTY\n"
		"\n"
		"Index 2\n"
		"NoDefaultRule\n"
		"Pos 0 -1 EMPTY\n");
	ASSERT_EQ(Rules.NumConfigs(), 1u);
	ASSERT_EQ(Rules.m_vConfigs[0].m_vRuns.size(), 1u);
	const auto &vIndexRules = Rules.m_vConfigs[0].m_vRuns[0].m_vIndexRules;
	ASSERT_EQ(vIndexRules.size(), 2u);
	EXPECT_EQ(vIndexRules[0].m_vRules.size(), 2u) << "the one that was written and the one written in";
	EXPECT_TRUE(vIndexRules[0].m_SkipEmpty);
	EXPECT_EQ(vIndexRules[1].m_vRules.size(), 1u) << "and none written in for the one that asked";
	EXPECT_FALSE(vIndexRules[1].m_SkipEmpty);

	// How far the rules of this configuration reach, which is the margin an
	// automapped rectangle needs around it.
	EXPECT_EQ(Rules.m_vConfigs[0].m_StartY, -1);
	EXPECT_EQ(Rules.m_vConfigs[0].m_EndY, 0);
}

TEST(Automap, TheTilesThatComeOutAreTheOnesTheRulesAskFor)
{
	// A wall of tile 1 with air above it. The rule puts a 5 on top of a tile
	// whose neighbour above is air, and a 6 everywhere else that is not air.
	const CAutomapRules Rules = ParseAutomapRules(
		"[Test]\n"
		"Index 6\n"
		"\n"
		"Index 5\n"
		"Pos 0 -1 EMPTY\n");
	ASSERT_EQ(Rules.NumConfigs(), 1u);

	CTileLayer Tiles = Layer(4, 4);
	for(int y = 2; y < 4; ++y)
		for(int x = 0; x < 4; ++x)
			Put(Tiles, x, y, 1);

	Automap(Tiles, nullptr, Rules, 0, 1, -1, 0, 0, -1, -1);

	for(int x = 0; x < 4; ++x)
	{
		EXPECT_EQ(IndexAt(Tiles, x, 0), 0) << "air stays air at " << x;
		EXPECT_EQ(IndexAt(Tiles, x, 1), 0) << "air stays air at " << x;
		EXPECT_EQ(IndexAt(Tiles, x, 2), 5) << "the top of the wall at " << x;
		EXPECT_EQ(IndexAt(Tiles, x, 3), 6) << "under it at " << x;
	}
}

TEST(Automap, ARectangleComesOutAsTheWholeLayerWould)
{
	// The point of running a rectangle at all: it has to give the same tiles
	// as running everything, or a brush stroke that automaps itself would
	// leave a seam where it stopped.
	const std::string Text = RulesText("ddnet_walls");
	ASSERT_FALSE(Text.empty());
	const CAutomapRules Rules = ParseAutomapRules(Text.c_str());
	ASSERT_GT(Rules.NumConfigs(), 0u);

	CTileLayer Drawn = Layer(24, 16);
	// A lump of wall with a hole in it, so that there are edges of every kind.
	for(int y = 4; y < 12; ++y)
		for(int x = 4; x < 20; ++x)
			if(x < 9 || x > 12 || y < 6 || y > 9)
				Put(Drawn, x, y, 1);

	CTileLayer Whole = Drawn;
	Automap(Whole, nullptr, Rules, 0, 7, -1, 0, 0, -1, -1);

	CTileLayer Piece = Drawn;
	Automap(Piece, nullptr, Rules, 0, 7, -1, 6, 5, 10, 6);

	for(int y = 5; y < 11; ++y)
		for(int x = 6; x < 16; ++x)
			EXPECT_EQ(IndexAt(Piece, x, y), IndexAt(Whole, x, y)) << "at " << x << ", " << y;

	// And the same seed twice is the same map: the rules that only fire
	// sometimes fire off a hash of the place, not off a die.
	CTileLayer Again = Drawn;
	Automap(Again, nullptr, Rules, 0, 7, -1, 0, 0, -1, -1);
	EXPECT_EQ(Again, Whole);
}

TEST(Automap, ARunIsFilteredByThePhysicsTileItWasToldAbout)
{
	// The first run reads the game layer instead of the layer being drawn in,
	// and only the one physics tile it was named - which is how one rules
	// file draws freeze and hookable from the same game layer.
	const CAutomapRules Rules = ParseAutomapRules(
		"[Test]\n"
		"Index 9\n");
	ASSERT_EQ(Rules.NumConfigs(), 1u);

	CTileLayer Game = CTileLayer(ETileLayerKind::GAME, 4, 1);
	Put(Game, 0, 0, TILE_SOLID);
	Put(Game, 1, 0, TILE_FREEZE);
	Put(Game, 2, 0, TILE_SOLID);

	// Reference 1 is the hookable tile: the freeze in the middle is air to it.
	CTileLayer Drawn = Layer(4, 1);
	Automap(Drawn, &Game, Rules, 0, 1, 1, 0, 0, -1, -1);
	EXPECT_EQ(IndexAt(Drawn, 0, 0), 9);
	EXPECT_EQ(IndexAt(Drawn, 1, 0), 0);
	EXPECT_EQ(IndexAt(Drawn, 2, 0), 9);
	EXPECT_EQ(IndexAt(Drawn, 3, 0), 0);

	// Reference 4 is freeze, and then it is the other way round.
	CTileLayer Freeze = Layer(4, 1);
	Automap(Freeze, &Game, Rules, 0, 1, 4, 0, 0, -1, -1);
	EXPECT_EQ(IndexAt(Freeze, 0, 0), 0);
	EXPECT_EQ(IndexAt(Freeze, 1, 0), 9);
}
