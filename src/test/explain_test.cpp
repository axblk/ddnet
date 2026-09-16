#include <game/map/document/explain.h>
#include <game/map/document/structure.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <cstring>

using namespace map_document;

// What a tile is, and what it does. Both questions are answered by the file
// format rather than by the layer that happens to hold the tile, which is
// exactly why they are here and not in the page.

TEST(Explain, ALayerThatDrawsKeepsItsNumberWhereItIsDrawnFrom)
{
	CTileLayer Tiles(ETileLayerKind::TILES, 4, 4);
	CTile Drawn;
	Drawn.m_Index = 7;
	Tiles.m_Tiles.Set(1, 2, Drawn);
	EXPECT_EQ(TileMeaning(Tiles, 1, 2), 7);
	EXPECT_EQ(TileMeaning(Tiles, 0, 0), 0);
	EXPECT_EQ(TileMeaning(Tiles, 4, 0), -1) << "outside is not a tile";
	EXPECT_EQ(TileMeaning(Tiles, -1, 0), -1);
}

TEST(Explain, APhysicsLayerKeepsItBesideTheAirItDraws)
{
	CTileLayer Tele(ETileLayerKind::TELE, 4, 4);
	CTeleTile Gate;
	Gate.m_Type = TILE_TELEIN;
	Gate.m_Number = 3;
	std::get<CTileStore<CTeleTile>>(Tele.m_ExtraTiles).Set(2, 1, Gate);

	// The plane that is drawn stays air - the picture comes from the entities
	// sheet - and the number that says what it is sits in the other one.
	EXPECT_EQ(Tele.m_Tiles.Get(2, 1).m_Index, 0);
	EXPECT_EQ(TileMeaning(Tele, 2, 1), TILE_TELEIN);
}

TEST(Explain, OnlyAPhysicsLayerHasAnythingToExplain)
{
	const char *pTeleIn = ExplainTile(ETileLayerKind::TELE, TILE_TELEIN);
	ASSERT_NE(pTeleIn, nullptr);
	EXPECT_GT(std::strlen(pTeleIn), 0u);

	// The same number in the game layer is a different tile and gets a
	// different sentence - or none.
	const char *pSameInGame = ExplainTile(ETileLayerKind::GAME, TILE_TELEIN);
	EXPECT_STRNE(pTeleIn, pSameInGame == nullptr ? "" : pSameInGame);

	// A layer that is only drawn is a picture, and a picture explains itself.
	EXPECT_EQ(ExplainTile(ETileLayerKind::TILES, TILE_TELEIN), nullptr);

	// The game tiles everybody knows are named.
	EXPECT_STREQ(ExplainTile(ETileLayerKind::GAME, TILE_DEATH), "KILL: Kills the tee.");
	EXPECT_EQ(ExplainTile(ETileLayerKind::GAME, -1), nullptr) << "not a tile";
	EXPECT_EQ(ExplainTile(ETileLayerKind::GAME, 256), nullptr);
}
