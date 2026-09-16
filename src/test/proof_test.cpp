#include <engine/graphics.h>
#include <engine/shared/json.h>

#include <game/map/document/proof.h>
#include <game/map/document/report.h>
#include <game/map/document/structure.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace map_document;

// What a player would see. The point of proof mode is that it is not a guess,
// so what is checked here is that the rectangle it gives is the one the game's
// own arithmetic gives - held against `CalcViewSize` rather than against a
// number somebody typed once.

namespace
{
	using CJson = std::unique_ptr<json_value, decltype(&json_value_free)>;

	CJson Parse(const std::string &Json)
	{
		return CJson(JsonParse(Json.c_str(), Json.size()), json_value_free);
	}

	CMapState WithAGameLayer()
	{
		CMapState Map;
		CGroup Group;
		Group.m_Name = "Game";
		CTileLayer Game(ETileLayerKind::GAME, 20, 10);
		Game.m_Name = "Game";
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Game)));
		Map.AddGroup(std::move(Group));
		return Map;
	}
} // namespace

TEST(Proof, AScreenIsTheOneTheGameWouldGive)
{
	const vec2 Center(1000.0f, 500.0f);
	const CProofRect Rect = ProofScreen(Center, 16.0f / 9.0f, 1.0f);

	float Width, Height;
	CalcViewSize(16.0f / 9.0f, 1.0f, PROOF_MAX_ASPECT / 100.0f, &Width, &Height);
	EXPECT_FLOAT_EQ(Rect.Width(), Width);
	EXPECT_FLOAT_EQ(Rect.Height(), Height);

	// And it stands around the place rather than beside it.
	EXPECT_FLOAT_EQ((Rect.m_TopLeft.x + Rect.m_BottomRight.x) / 2.0f, Center.x);
	EXPECT_FLOAT_EQ((Rect.m_TopLeft.y + Rect.m_BottomRight.y) / 2.0f, Center.y);
}

TEST(Proof, AWiderScreenIsAShorterOneAndAMenuStandsFurtherBack)
{
	const vec2 Center(0.0f, 0.0f);
	const CProofRect Square = ProofScreen(Center, 1.0f, 1.0f);
	const CProofRect Wide = ProofScreen(Center, 16.0f / 9.0f, 1.0f);
	// The view keeps its area, so what a wide screen wins sideways it loses in
	// height - that is the whole reason proof mode exists.
	EXPECT_GT(Wide.Width(), Square.Width());
	EXPECT_LT(Wide.Height(), Square.Height());

	const CProofRect Menu = ProofScreen(Center, 16.0f / 9.0f, 0.7f);
	EXPECT_FLOAT_EQ(Menu.Width(), Wide.Width() * 0.7f);
	EXPECT_FLOAT_EQ(Menu.Height(), Wide.Height() * 0.7f);
}

TEST(Proof, TheMenuPositionsAreTheTimeCheckpointsTheMapNames)
{
	CMapState Map = WithAGameLayer();
	EXPECT_TRUE(MenuPositions(Map).empty()) << "a map that names none has none";

	CTileLayer Game = std::get<CTileLayer>(*Map.Layer(0, 0));
	CTile First;
	First.m_Index = TILE_TIME_CHECKPOINT_FIRST;
	Game.m_Tiles.Set(3, 4, First);
	CTile Third;
	Third.m_Index = TILE_TIME_CHECKPOINT_FIRST + 2;
	Game.m_Tiles.Set(7, 1, Third);
	Map.ReplaceLayer(0, 0, CLayer(std::move(Game)));

	const std::vector<CMenuPosition> vPositions = MenuPositions(Map);
	ASSERT_EQ(vPositions.size(), 2u);
	// Read in the order they are found, which is top to bottom.
	EXPECT_EQ(vPositions[0].m_Index, 2);
	EXPECT_FLOAT_EQ(vPositions[0].m_Position.x, 7 * 32.0f + 16.0f) << "the middle of the tile, not its corner";
	EXPECT_FLOAT_EQ(vPositions[0].m_Position.y, 1 * 32.0f + 16.0f);
	EXPECT_EQ(vPositions[1].m_Index, 0);
	EXPECT_FLOAT_EQ(vPositions[1].m_Position.x, 3 * 32.0f + 16.0f);
}

TEST(Proof, TheReportHoldsAnOutlineTwoNamedShapesAndOnlyInAMenuThePositions)
{
	CMapState Map = WithAGameLayer();
	CTileLayer Game = std::get<CTileLayer>(*Map.Layer(0, 0));
	CTile Checkpoint;
	Checkpoint.m_Index = TILE_TIME_CHECKPOINT_FIRST + 1;
	Game.m_Tiles.Set(2, 2, Checkpoint);
	Map.ReplaceLayer(0, 0, CLayer(std::move(Game)));

	const CJson pGame = Parse(ProofJson(Map, vec2(640.0f, 320.0f), false));
	ASSERT_NE(pGame, nullptr);
	EXPECT_FALSE(json_boolean_get(json_object_get(pGame.get(), "menu")));
	EXPECT_EQ(json_int_get(json_array_get(json_object_get(pGame.get(), "center"), 0)), 640);
	const json_value *pSteps = json_object_get(pGame.get(), "steps");
	ASSERT_EQ(pSteps->type, json_array);
	EXPECT_EQ(pSteps->u.array.length, 21u) << "twenty steps have twenty-one edges";
	EXPECT_EQ(json_array_get(pSteps, 0)->u.array.length, 4u) << "left, top, right, bottom";

	const json_value *pNamed = json_object_get(pGame.get(), "named");
	ASSERT_EQ(pNamed->u.array.length, 2u);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pNamed, 0), "name")), "4:3");
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pNamed, 1), "name")), "16:10");
	EXPECT_EQ(json_object_get(pGame.get(), "positions")->u.array.length, 0u)
		<< "a game has no menu backgrounds to stand at";

	// The same map in menu mode: further back, and the places come with it.
	const CJson pMenu = Parse(ProofJson(Map, vec2(640.0f, 320.0f), true));
	ASSERT_NE(pMenu, nullptr);
	EXPECT_TRUE(json_boolean_get(json_object_get(pMenu.get(), "menu")));
	const json_value *pPositions = json_object_get(pMenu.get(), "positions");
	ASSERT_EQ(pPositions->u.array.length, 1u);
	EXPECT_EQ(json_int_get(json_object_get(json_array_get(pPositions, 0), "index")), 1);

	const json_value *pMenuFirst = json_array_get(json_object_get(pMenu.get(), "steps"), 0);
	const json_value *pGameFirst = json_array_get(pSteps, 0);
	const int MenuWide = json_int_get(json_array_get(pMenuFirst, 2)) - json_int_get(json_array_get(pMenuFirst, 0));
	const int GameWide = json_int_get(json_array_get(pGameFirst, 2)) - json_int_get(json_array_get(pGameFirst, 0));
	EXPECT_LT(MenuWide, GameWide);
}
