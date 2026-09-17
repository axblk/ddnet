#include "test.h"

#include <base/str.h>

#include <engine/shared/datafile.h>
#include <engine/shared/json.h>
#include <engine/storage.h>

#include <game/map/document/command.h>
#include <game/map/document/document.h>
#include <game/map/document/map_file.h>
#include <game/map/document/structure.h>
#include <game/map/document/tiles.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace map_document;

// A rectangle of tiles becomes text and comes back the same, in every
// encoding, for every kind of layer, and the commands built on that do what
// they say on a map that came from a file.

namespace
{
	CTileValue Value(int a, int b = 0, int c = 0, int d = 0)
	{
		CTileValue Result;
		Result.m_aFields[0] = a;
		Result.m_aFields[1] = b;
		Result.m_aFields[2] = c;
		Result.m_aFields[3] = d;
		return Result;
	}

	/** A layer of every kind with a few tiles of every shape in it. */
	CTileLayer Filled(ETileLayerKind Kind)
	{
		CTileLayer Layer(Kind, 70, 66);
		Layer.m_Name = "test";
		SetTileValue(Layer, 0, 0, Value(1));
		SetTileValue(Layer, 1, 0, Value(1));
		SetTileValue(Layer, 2, 0, Value(1));
		SetTileValue(Layer, 3, 0, Value(3, 8));
		SetTileValue(Layer, 0, 1, Value(26, 5));
		SetTileValue(Layer, 69, 65, Value(9, 2, 3, 4));
		SetTileValue(Layer, 65, 2, Value(24, 7, 1, 1));
		SetTileValue(Layer, 10, 10, Value(28, 100, 50, 359));
		return Layer;
	}

	bool SameTiles(const CTileLayer &One, const CTileLayer &Other, const CTileRect &Rect)
	{
		for(int y = Rect.m_Y; y < Rect.Bottom(); ++y)
			for(int x = Rect.m_X; x < Rect.Right(); ++x)
				if(GetTileValue(One, x, y) != GetTileValue(Other, x, y))
					return false;
		return true;
	}

	const ETileLayerKind s_aKinds[] = {ETileLayerKind::TILES, ETileLayerKind::GAME, ETileLayerKind::FRONT, ETileLayerKind::TELE, ETileLayerKind::SPEEDUP, ETileLayerKind::SWITCH, ETileLayerKind::TUNE};
	const ETileEncoding s_aWritable[] = {ETileEncoding::RLE, ETileEncoding::ROWS, ETileEncoding::SPARSE};

	std::unique_ptr<json_value, decltype(&json_value_free)> Parse(const std::string &Json)
	{
		return {JsonParse(Json.c_str(), Json.size()), json_value_free};
	}

	bool Ok(const std::string &Json)
	{
		const auto pParsed = Parse(Json);
		return pParsed != nullptr && json_boolean_get(json_object_get(pParsed.get(), "ok")) != 0;
	}

	int IntOf(const std::string &Json, const char *pName)
	{
		const auto pParsed = Parse(Json);
		return json_int_get(json_object_get(pParsed.get(), pName));
	}

	std::string StrOf(const std::string &Json, const char *pName)
	{
		const auto pParsed = Parse(Json);
		const json_value *pValue = json_object_get(pParsed.get(), pName);
		return pValue->type == json_string ? json_string_get(pValue) : "";
	}

	CMapState TwoLayers()
	{
		CMapState State;
		CGroup Group;
		Group.m_Name = "Game";
		CTileLayer Game(ETileLayerKind::GAME, 200, 100);
		Game.m_Name = "Game";
		SetTileValue(Game, 5, 5, Value(TILE_SOLID));
		SetTileValue(Game, 6, 5, Value(TILE_SOLID));
		SetTileValue(Game, 7, 5, Value(TILE_FREEZE));
		SetTileValue(Game, 90, 40, Value(TILE_SOLID));
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Game)));
		CTileLayer Design(ETileLayerKind::TILES, 200, 100);
		Design.m_Name = "Design";
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Design)));
		State.AddGroup(std::move(Group));
		return State;
	}
} // namespace

TEST(Tiles, ATokenSaysWhatATileIsAndNoMore)
{
	EXPECT_EQ(TileToken(Value(0)), "0");
	EXPECT_EQ(TileToken(Value(1)), "1");
	EXPECT_EQ(TileToken(Value(3, 8)), "3/8");
	EXPECT_EQ(TileToken(Value(26, 5)), "26/5");
	EXPECT_EQ(TileToken(Value(24, 7, 0, 1)), "24/7/0/1");
	EXPECT_EQ(TileToken(Value(28, 100, 50, 359)), "28/100/50/359");
}

TEST(Tiles, ATokenReadsBackWhatItSaid)
{
	std::string Error;
	CTileValue Read;
	EXPECT_TRUE(ParseTileToken("3/8", ETileLayerKind::GAME, &Read, &Error));
	EXPECT_EQ(Read, Value(3, 8));
	EXPECT_TRUE(ParseTileToken("28/100/50/359", ETileLayerKind::SPEEDUP, &Read, &Error));
	EXPECT_EQ(Read, Value(28, 100, 50, 359));
	EXPECT_FALSE(ParseTileToken("x", ETileLayerKind::GAME, &Read, &Error));
	EXPECT_FALSE(ParseTileToken("1/2/3", ETileLayerKind::GAME, &Read, &Error)) << "a drawn tile carries two numbers";
	EXPECT_FALSE(ParseTileToken("256", ETileLayerKind::GAME, &Read, &Error));
	EXPECT_FALSE(ParseTileToken("", ETileLayerKind::GAME, &Read, &Error));
}

TEST(Tiles, EveryWritableEncodingComesBackTheSameForEveryKind)
{
	for(const ETileLayerKind Kind : s_aKinds)
	{
		const CTileLayer Layer = Filled(Kind);
		const CTileRect Whole{0, 0, Layer.Width(), Layer.Height()};
		for(const ETileEncoding Encoding : s_aWritable)
		{
			const std::string Text = EncodeTiles(Layer, Whole, Encoding);
			CBrush Brush;
			std::string Error;
			ASSERT_TRUE(DecodeTiles(Text.c_str(), Encoding, Kind, Layer.Width(), Layer.Height(), &Brush, &Error)) << Error;
			EXPECT_TRUE(SameTiles(Layer, Brush, Whole)) << TileEncodingName(Encoding) << " for kind " << (int)Kind;
			// And without saying how large it is, which is what a model
			// that writes a few rows does.
			CBrush Sized;
			ASSERT_TRUE(DecodeTiles(Text.c_str(), Encoding, Kind, 0, 0, &Sized, &Error)) << Error;
			EXPECT_EQ(Sized.Width(), Layer.Width());
			EXPECT_EQ(Sized.Height(), Layer.Height());
		}
	}
}

TEST(Tiles, RleWritesRunsAndReadsThem)
{
	CTileLayer Layer(ETileLayerKind::GAME, 8, 2);
	for(int x = 0; x < 8; ++x)
		SetTileValue(Layer, x, 0, Value(1));
	SetTileValue(Layer, 7, 0, Value(3));
	EXPECT_EQ(EncodeTiles(Layer, CTileRect{0, 0, 8, 2}, ETileEncoding::RLE), "1x7 3\n0x8\n");
	EXPECT_EQ(EncodeTiles(Layer, CTileRect{0, 0, 8, 2}, ETileEncoding::ROWS), "1 1 1 1 1 1 1 3\n0 0 0 0 0 0 0 0\n");
	EXPECT_EQ(EncodeTiles(Layer, CTileRect{6, 0, 2, 1}, ETileEncoding::SPARSE), "0,0:1 1,0:3");
	CBrush Brush;
	std::string Error;
	ASSERT_TRUE(DecodeTiles("1x3\n\n9x2", ETileEncoding::RLE, ETileLayerKind::GAME, 0, 0, &Brush, &Error)) << Error;
	EXPECT_EQ(Brush.Width(), 3);
	EXPECT_EQ(Brush.Height(), 3);
	EXPECT_EQ(GetTileValue(Brush, 2, 0), Value(1));
	EXPECT_EQ(GetTileValue(Brush, 0, 1), Value(0)) << "an empty line is a row of air";
	EXPECT_EQ(GetTileValue(Brush, 1, 2), Value(9));
	EXPECT_FALSE(DecodeTiles("1 2 3", ETileEncoding::RLE, ETileLayerKind::GAME, 2, 1, &Brush, &Error)) << "more tiles than the rectangle holds";
	EXPECT_FALSE(DecodeTiles("1x", ETileEncoding::RLE, ETileLayerKind::GAME, 0, 0, &Brush, &Error));
	EXPECT_FALSE(DecodeTiles("#", ETileEncoding::GLYPH, ETileLayerKind::GAME, 0, 0, &Brush, &Error)) << "glyphs are for reading";
}

TEST(Tiles, GlyphsShowTheShapeAndCarryALegend)
{
	CTileLayer Layer(ETileLayerKind::GAME, 4, 2);
	SetTileValue(Layer, 0, 0, Value(TILE_SOLID));
	SetTileValue(Layer, 1, 0, Value(TILE_FREEZE));
	SetTileValue(Layer, 2, 0, Value(TILE_START));
	SetTileValue(Layer, 3, 0, Value(ENTITY_OFFSET + ENTITY_SPAWN));
	SetTileValue(Layer, 0, 1, Value(200));
	std::string Legend;
	const std::string Text = EncodeTiles(Layer, CTileRect{0, 0, 4, 2}, ETileEncoding::GLYPH, &Legend);
	EXPECT_EQ(Text, "#fSp\nA...\n");
	EXPECT_NE(Legend.find("#=1 HOOKABLE"), std::string::npos) << Legend;
	EXPECT_NE(Legend.find("A=200"), std::string::npos) << Legend;
}

TEST(Tiles, FillReplaceFindAndCountAgree)
{
	CTileLayer Layer(ETileLayerKind::GAME, 20, 10);
	EXPECT_EQ(FillTileRect(Layer, CTileRect{2, 2, 6, 4}, Value(TILE_SOLID)), 24);
	EXPECT_EQ(FillTileRect(Layer, CTileRect{2, 2, 6, 4}, Value(TILE_FREEZE), 1), 16) << "the rim of six by four is sixteen tiles";
	EXPECT_EQ(GetTileValue(Layer, 3, 3), Value(TILE_SOLID)) << "the inside stays";
	EXPECT_EQ(GetTileValue(Layer, 2, 2), Value(TILE_FREEZE));
	EXPECT_EQ(FillTileRect(Layer, CTileRect{18, 8, 10, 10}, Value(TILE_NOHOOK)), 4) << "clipped to the layer";
	const CTileStats Stats = CountTiles(Layer, CTileRect{0, 0, 20, 10});
	EXPECT_EQ(Stats.m_Tiles, 28u);
	EXPECT_EQ(Stats.m_Bounds.m_X, 2);
	EXPECT_EQ(Stats.m_Bounds.m_Y, 2);
	EXPECT_EQ(Stats.m_Bounds.Right(), 20);
	EXPECT_EQ(Stats.m_Bounds.Bottom(), 10);
	ASSERT_EQ(Stats.m_vCounts.size(), 3u);
	EXPECT_EQ(Stats.m_vCounts[0].m_Index, TILE_SOLID);
	EXPECT_EQ(Stats.m_vCounts[0].m_Count, 8u);
	EXPECT_EQ(Stats.m_vCounts[1].m_Index, TILE_NOHOOK);
	EXPECT_EQ(Stats.m_vCounts[2].m_Index, TILE_FREEZE);
	EXPECT_EQ(Stats.m_vCounts[2].m_Count, 16u);
	size_t Total = 0;
	const std::vector<CTileRun> vRuns = FindTileRuns(Layer, CTileRect{0, 0, 20, 10}, {TILE_FREEZE}, 2, &Total);
	EXPECT_EQ(Total, 6u) << "the top row, two tiles on each of two rows, the bottom row";
	ASSERT_EQ(vRuns.size(), 2u);
	EXPECT_EQ(vRuns[0].m_X, 2);
	EXPECT_EQ(vRuns[0].m_Y, 2);
	EXPECT_EQ(vRuns[0].m_Length, 6);
	EXPECT_EQ(ReplaceTileIndex(Layer, CTileRect{0, 0, 20, 10}, TILE_FREEZE, TILE_DFREEZE), 16);
	EXPECT_EQ(GetTileValue(Layer, 2, 2), Value(TILE_DFREEZE));
	EXPECT_EQ(ReplaceTileIndex(Layer, CTileRect{0, 0, 20, 10}, TILE_SOLID, 0), 8);
	EXPECT_EQ(CountTiles(Layer, CTileRect{0, 0, 20, 10}).m_Tiles, 20u);
}

TEST(Tiles, WritingOverlaysOrReplaces)
{
	CTileLayer Layer(ETileLayerKind::GAME, 4, 1);
	for(int x = 0; x < 4; ++x)
		SetTileValue(Layer, x, 0, Value(TILE_SOLID));
	CBrush Brush(ETileLayerKind::GAME, 2, 1);
	SetTileValue(Brush, 1, 0, Value(TILE_FREEZE));
	EXPECT_EQ(WriteTiles(Layer, 1, 0, Brush, true), 1);
	EXPECT_EQ(GetTileValue(Layer, 1, 0), Value(TILE_SOLID)) << "air in the brush leaves the layer alone";
	EXPECT_EQ(GetTileValue(Layer, 2, 0), Value(TILE_FREEZE));
	EXPECT_EQ(WriteTiles(Layer, 1, 0, Brush, false), 2);
	EXPECT_EQ(GetTileValue(Layer, 1, 0), Value(0)) << "replacing writes the air";
	EXPECT_EQ(WriteTiles(Layer, 3, 0, Brush, false), 1) << "what hangs over the edge is left off";
}

TEST(Tiles, ChangedChunksCountsBlocks)
{
	CTileLayer Layer(ETileLayerKind::TELE, 130, 70);
	const CTileLayer Before = Layer;
	SetTileValue(Layer, 0, 0, Value(26, 1));
	EXPECT_EQ(ChangedChunks(Before, Layer), 1u);
	SetTileValue(Layer, 129, 69, Value(27, 1));
	EXPECT_EQ(ChangedChunks(Before, Layer), 2u);
	EXPECT_EQ(ChangedChunks(Layer, Layer), 0u);
}

TEST(Tiles, TheCommandsRunThroughApply)
{
	CDocument Document(TwoLayers());

	std::string Answer = Apply(Document, "{\"op\":\"tiles.read\",\"group\":0,\"layer\":0,\"x\":4,\"y\":5,\"w\":5,\"h\":1}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	EXPECT_EQ(StrOf(Answer, "tiles"), "0 1x2 9 0\n");
	EXPECT_EQ(StrOf(Answer, "kind"), "game");

	Answer = Apply(Document, "{\"op\":\"tiles.read\",\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"w\":200,\"h\":200}");
	EXPECT_FALSE(Ok(Answer)) << "more than 128 by 128";
	Answer = Apply(Document, "{\"op\":\"tiles.read\",\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"w\":100,\"h\":50,\"large\":true}");
	EXPECT_TRUE(Ok(Answer)) << Answer;
	Answer = Apply(Document, "{\"op\":\"tiles.read\",\"group\":0,\"layer\":0,\"x\":500,\"y\":0,\"w\":10,\"h\":10}");
	EXPECT_FALSE(Ok(Answer)) << "off the layer";
	Answer = Apply(Document, "{\"op\":\"tiles.read\",\"group\":0,\"layer\":5,\"x\":0,\"y\":0,\"w\":10,\"h\":10}");
	EXPECT_FALSE(Ok(Answer)) << "no such layer";

	Answer = Apply(Document, "{\"op\":\"tiles.write\",\"group\":0,\"layer\":0,\"x\":10,\"y\":10,\"tiles\":\"1x3\\n3 0 3\\n230\"}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	EXPECT_EQ(IntOf(Answer, "written"), 9);
	EXPECT_EQ(IntOf(Answer, "dropped"), 1) << "230 means nothing in a game layer";
	EXPECT_EQ(IntOf(Answer, "chunks"), 1);
	EXPECT_EQ(Document.History().NumEntries(), 2u);
	EXPECT_EQ(GetTileValue(*Document.Map().TileLayer(0, 0), 12, 10), Value(1));
	EXPECT_EQ(GetTileValue(*Document.Map().TileLayer(0, 0), 10, 12), Value(0));

	Answer = Apply(Document, "{\"op\":\"tiles.write\",\"group\":0,\"layer\":0,\"x\":10,\"y\":10,\"tiles\":\"0 9\",\"mode\":\"overlay\"}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	EXPECT_EQ(GetTileValue(*Document.Map().TileLayer(0, 0), 10, 10), Value(1)) << "overlay keeps what is under air";
	EXPECT_EQ(GetTileValue(*Document.Map().TileLayer(0, 0), 11, 10), Value(9));

	Answer = Apply(Document, "{\"op\":\"tiles.fill\",\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"index\":9,\"border\":1}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	EXPECT_EQ(IntOf(Answer, "written"), 36);
	Answer = Apply(Document, "{\"op\":\"tiles.fill\",\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"index\":230}");
	EXPECT_FALSE(Ok(Answer)) << "an index the game ignores is refused";
	Answer = Apply(Document, "{\"op\":\"tiles.fill\",\"group\":0,\"layer\":1,\"x\":0,\"y\":0,\"w\":10,\"h\":10,\"index\":230}");
	EXPECT_TRUE(Ok(Answer)) << "a design layer takes any index";

	Answer = Apply(Document, "{\"op\":\"tiles.stats\"}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	{
		const auto pParsed = Parse(Answer);
		const json_value *pLayers = json_object_get(pParsed.get(), "layers");
		ASSERT_EQ(json_array_length(pLayers), 2);
		const json_value *pGame = json_array_get(pLayers, 0);
		EXPECT_STREQ(json_string_get(json_object_get(pGame, "kind")), "game");
		EXPECT_GT(json_int_get(json_object_get(pGame, "tiles")), 40);
		const json_value *pHistogram = json_object_get(pGame, "histogram");
		EXPECT_GE(json_array_length(pHistogram), 2);
		EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pHistogram, 0), "name")), "HOOKABLE: It's possible to hook and collide with it.");
	}

	Answer = Apply(Document, "{\"op\":\"tiles.find\",\"index\":9,\"limit\":3}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	{
		const auto pParsed = Parse(Answer);
		EXPECT_EQ(json_array_length(json_object_get(pParsed.get(), "runs")), 3);
		EXPECT_GT(json_int_get(json_object_get(pParsed.get(), "total")), 3);
		EXPECT_TRUE(json_boolean_get(json_object_get(pParsed.get(), "truncated")));
	}

	Answer = Apply(Document, "{\"op\":\"tiles.replace\",\"from\":9,\"to\":12}");
	ASSERT_TRUE(Ok(Answer)) << Answer;
	EXPECT_EQ(IntOf(Answer, "replaced"), 38);
	Answer = Apply(Document, "{\"op\":\"tiles.find\",\"index\":9}");
	EXPECT_EQ(IntOf(Answer, "total"), 0);

	const size_t Entries = Document.History().NumEntries();
	Answer = Apply(Document, "{\"op\":\"tiles.nothing\"}");
	EXPECT_FALSE(Ok(Answer));
	EXPECT_EQ(Document.History().NumEntries(), Entries) << "a refused command leaves no entry";
}

TEST(Tiles, EveryShippedMapRoundTripsAndStaysSmall)
{
	std::vector<std::string> vNames;
	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	pStorage->ListDirectory(IStorage::TYPE_ALL, "data/maps", [](const char *pName, int IsDir, int, void *pUser) {
		if(!IsDir && str_endswith(pName, ".map"))
			static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName);
		return 0; }, &vNames);
	ASSERT_FALSE(vNames.empty());
	size_t LargestRle = 0;
	for(const std::string &Name : vNames)
	{
		CDataFileReader File;
		ASSERT_TRUE(File.Open(pStorage.get(), ("data/maps/" + Name).c_str(), IStorage::TYPE_ALL)) << Name;
		CMapState State;
		std::vector<std::string> vWarnings;
		ASSERT_TRUE(ReadMapState(File, &State, &vWarnings)) << Name;
		File.Close();
		const std::optional<CLayerAddress> Game = FindGameLayer(State);
		ASSERT_TRUE(Game.has_value()) << Name;
		const CTileLayer &Layer = *State.TileLayer(Game->m_Group, Game->m_Layer);
		constexpr int BLOCK = 64;
		for(int y = 0; y < Layer.Height(); y += BLOCK)
		{
			for(int x = 0; x < Layer.Width(); x += BLOCK)
			{
				const CTileRect Rect = ClipTileRect(Layer, CTileRect{x, y, BLOCK, BLOCK});
				const std::string Text = EncodeTiles(Layer, Rect, ETileEncoding::RLE);
				LargestRle = std::max(LargestRle, Text.size());
				CBrush Brush;
				std::string Error;
				ASSERT_TRUE(DecodeTiles(Text.c_str(), ETileEncoding::RLE, Layer.m_Kind, Rect.m_Width, Rect.m_Height, &Brush, &Error)) << Name << ": " << Error;
				CTileLayer Written(Layer.m_Kind, Layer.Width(), Layer.Height());
				WriteTiles(Written, Rect.m_X, Rect.m_Y, Brush, false);
				ASSERT_TRUE(SameTiles(Layer, Written, Rect)) << Name << " at " << x << "," << y;
			}
		}
	}
	EXPECT_LE(LargestRle, 4096u) << "a block of 64 by 64 game tiles as rle stays under 4 KiB";
}
