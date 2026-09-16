#include <engine/shared/json.h>

#include <game/map/document/document.h>
#include <game/map/document/report.h>
#include <game/map/document/structure.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

using namespace map_document;

// What the interface is told about the map, and what it is deliberately not
// told: the shape of the map rather than its contents. Every test here parses
// the text back, because a report nobody can read is not a report.

namespace
{
	using CJson = std::unique_ptr<json_value, decltype(&json_value_free)>;

	CJson Parse(const std::string &Json)
	{
		return CJson(JsonParse(Json.c_str(), Json.size()), json_value_free);
	}

	CMapState SmallMap()
	{
		CMapState Map;

		CGroup Background;
		Background.m_Name = "background";
		Background.m_ParallaxX = 50;
		Background.m_ParallaxY = 40;
		Background.m_OffsetX = -16;
		Background.m_OffsetY = 32;
		Background.m_UseClipping = true;
		Background.m_ClipX = 1;
		Background.m_ClipY = 2;
		Background.m_ClipW = 3;
		Background.m_ClipH = 4;
		CTileLayer Sky(ETileLayerKind::TILES, 8, 4);
		Sky.m_Name = "sky";
		Sky.m_Detail = true;
		Sky.m_Image = 1;
		Sky.m_Color = CColor(10, 20, 30, 40);
		Sky.m_ColorEnvelope = 2;
		Sky.m_ColorEnvelopeOffset = 500;
		Background.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Sky)));
		CQuadLayer Clouds;
		Clouds.m_Name = "clouds";
		Clouds.m_Image = 0;
		Clouds.m_Quads = CSharedList<CQuad>(std::vector<CQuad>(3));
		Background.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Clouds)));
		Map.AddGroup(std::move(Background));

		CGroup Game;
		Game.m_Name = "game";
		CTileLayer Physics(ETileLayerKind::GAME, 16, 9);
		Physics.m_Name = "Game";
		Game.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Physics)));
		Map.AddGroup(std::move(Game));

		CEnvelope Envelope;
		Envelope.m_Name = "colour";
		Envelope.m_Channels = 4;
		Envelope.m_Synchronized = true;
		Envelope.m_Points = CSharedList<CEnvPoint_runtime>(std::vector<CEnvPoint_runtime>(2));
		Map.AddEnvelope(std::move(Envelope));

		CImage Image;
		Image.m_Name = "grass_main";
		Image.m_External = true;
		Image.m_Width = 1024;
		Image.m_Height = 512;
		Map.AddImage(std::move(Image));

		CSound Sound;
		Sound.m_Name = "wind";
		Sound.m_External = false;
		Sound.m_Data = CSharedList<uint8_t>(std::vector<uint8_t>(7));
		Map.AddSound(std::move(Sound));

		Map.m_Info.m_Author = "somebody";
		Map.m_Info.m_Settings = CSharedList<std::string>({"sv_test 1", "sv_test 2"});
		return Map;
	}
} // namespace

TEST(Report, TheGroupsAndTheirLayersComeOutInOrder)
{
	const CJson pJson = Parse(StructureJson(SmallMap()));
	ASSERT_NE(pJson, nullptr);

	const json_value *pGroups = json_object_get(pJson.get(), "groups");
	ASSERT_EQ(json_array_length(pGroups), 2);
	const json_value *pBackground = json_array_get(pGroups, 0);
	EXPECT_STREQ(json_string_get(json_object_get(pBackground, "name")), "background");
	const json_value *pLayers = json_object_get(pBackground, "layers");
	ASSERT_EQ(json_array_length(pLayers), 2);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pLayers, 0), "name")), "sky");
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pLayers, 1), "name")), "clouds");
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pGroups, 1), "name")), "game");
}

TEST(Report, AGroupCarriesWhatItDoesToWhatIsInIt)
{
	const CJson pJson = Parse(StructureJson(SmallMap()));
	ASSERT_NE(pJson, nullptr);
	const json_value *pGroup = json_array_get(json_object_get(pJson.get(), "groups"), 0);

	const json_value *pParallax = json_object_get(pGroup, "parallax");
	ASSERT_EQ(json_array_length(pParallax), 2);
	EXPECT_EQ(json_int_get(json_array_get(pParallax, 0)), 50);
	EXPECT_EQ(json_int_get(json_array_get(pParallax, 1)), 40);
	const json_value *pOffset = json_object_get(pGroup, "offset");
	ASSERT_EQ(json_array_length(pOffset), 2);
	EXPECT_EQ(json_int_get(json_array_get(pOffset, 0)), -16);
	EXPECT_EQ(json_int_get(json_array_get(pOffset, 1)), 32);
	EXPECT_TRUE(json_boolean_get(json_object_get(pGroup, "useClipping")));
	const json_value *pClip = json_object_get(pGroup, "clip");
	ASSERT_EQ(json_array_length(pClip), 4);
	EXPECT_EQ(json_int_get(json_array_get(pClip, 3)), 4);
}

TEST(Report, ATileLayerSaysWhichKindItIsAndHowItIsDrawn)
{
	const CJson pJson = Parse(StructureJson(SmallMap()));
	ASSERT_NE(pJson, nullptr);
	const json_value *pGroups = json_object_get(pJson.get(), "groups");
	const json_value *pSky = json_array_get(json_object_get(json_array_get(pGroups, 0), "layers"), 0);

	EXPECT_STREQ(json_string_get(json_object_get(pSky, "type")), "tiles");
	EXPECT_STREQ(json_string_get(json_object_get(pSky, "kind")), "tiles");
	EXPECT_TRUE(json_boolean_get(json_object_get(pSky, "detail")));
	EXPECT_EQ(json_int_get(json_object_get(pSky, "image")), 1);
	const json_value *pSize = json_object_get(pSky, "size");
	ASSERT_EQ(json_array_length(pSize), 2);
	EXPECT_EQ(json_int_get(json_array_get(pSize, 0)), 8);
	EXPECT_EQ(json_int_get(json_array_get(pSize, 1)), 4);
	const json_value *pColor = json_object_get(pSky, "color");
	ASSERT_EQ(json_array_length(pColor), 4);
	EXPECT_EQ(json_int_get(json_array_get(pColor, 0)), 10);
	EXPECT_EQ(json_int_get(json_array_get(pColor, 3)), 40);
	EXPECT_EQ(json_int_get(json_object_get(pSky, "colorEnvelope")), 2);
	EXPECT_EQ(json_int_get(json_object_get(pSky, "colorEnvelopeOffset")), 500);

	const json_value *pGame = json_array_get(json_object_get(json_array_get(pGroups, 1), "layers"), 0);
	EXPECT_STREQ(json_string_get(json_object_get(pGame, "kind")), "game");
}

TEST(Report, AQuadLayerSaysHowManyQuadsItHasAndNotWhichOnes)
{
	const CJson pJson = Parse(StructureJson(SmallMap()));
	ASSERT_NE(pJson, nullptr);
	const json_value *pClouds = json_array_get(json_object_get(json_array_get(json_object_get(pJson.get(), "groups"), 0), "layers"), 1);

	EXPECT_STREQ(json_string_get(json_object_get(pClouds, "type")), "quads");
	EXPECT_EQ(json_int_get(json_object_get(pClouds, "quads")), 3);
	EXPECT_EQ(json_int_get(json_object_get(pClouds, "image")), 0);
	// The quads themselves are not in here, whatever they are called.
	EXPECT_EQ(json_object_get(pClouds, "points")->type, json_none);
}

TEST(Report, WhatTheGroupsPointAtIsListedWithThem)
{
	const CJson pJson = Parse(StructureJson(SmallMap()));
	ASSERT_NE(pJson, nullptr);

	const json_value *pEnvelope = json_array_get(json_object_get(pJson.get(), "envelopes"), 0);
	EXPECT_STREQ(json_string_get(json_object_get(pEnvelope, "name")), "colour");
	EXPECT_EQ(json_int_get(json_object_get(pEnvelope, "channels")), 4);
	EXPECT_TRUE(json_boolean_get(json_object_get(pEnvelope, "synchronized")));
	EXPECT_EQ(json_int_get(json_object_get(pEnvelope, "points")), 2);

	const json_value *pImage = json_array_get(json_object_get(pJson.get(), "images"), 0);
	EXPECT_STREQ(json_string_get(json_object_get(pImage, "name")), "grass_main");
	EXPECT_TRUE(json_boolean_get(json_object_get(pImage, "external")));
	EXPECT_EQ(json_int_get(json_array_get(json_object_get(pImage, "size"), 1)), 512);

	const json_value *pSound = json_array_get(json_object_get(pJson.get(), "sounds"), 0);
	EXPECT_STREQ(json_string_get(json_object_get(pSound, "name")), "wind");
	EXPECT_FALSE(json_boolean_get(json_object_get(pSound, "external")));
	EXPECT_EQ(json_int_get(json_object_get(pSound, "bytes")), 7);

	const json_value *pInfo = json_object_get(pJson.get(), "info");
	EXPECT_STREQ(json_string_get(json_object_get(pInfo, "author")), "somebody");
	ASSERT_EQ(json_array_length(json_object_get(pInfo, "settings")), 2);
	EXPECT_STREQ(json_string_get(json_array_get(json_object_get(pInfo, "settings"), 1)), "sv_test 2");
}

TEST(Report, AnEmptyMapIsStillAnObject)
{
	const CJson pJson = Parse(StructureJson(CMapState()));
	ASSERT_NE(pJson, nullptr);
	EXPECT_EQ(json_array_length(json_object_get(pJson.get(), "groups")), 0);
	EXPECT_EQ(json_array_length(json_object_get(pJson.get(), "images")), 0);
}

TEST(Report, AWordWithAQuoteInItComesBackWhole)
{
	CMapState Map;
	Map.m_Info.m_Author = "a \"name\" with \\ and \n in it";
	const CJson pJson = Parse(StructureJson(Map));
	ASSERT_NE(pJson, nullptr);
	EXPECT_STREQ(json_string_get(json_object_get(json_object_get(pJson.get(), "info"), "author")), Map.m_Info.m_Author.c_str());
}

TEST(Report, TheHistoryIsWhatWasDoneAndWhereTheMapStandsInIt)
{
	CDocument Document(SmallMap());
	Document.Begin("Delete group");
	DeleteGroup(Document, 0);
	Document.Commit();
	ASSERT_TRUE(Document.Undo());

	const CJson pJson = Parse(HistoryJson(Document));
	ASSERT_NE(pJson, nullptr);
	EXPECT_EQ(json_int_get(json_object_get(pJson.get(), "current")), 0);
	EXPECT_FALSE(json_boolean_get(json_object_get(pJson.get(), "canUndo")));
	EXPECT_TRUE(json_boolean_get(json_object_get(pJson.get(), "canRedo")));
	EXPECT_FALSE(json_boolean_get(json_object_get(pJson.get(), "editing")));
	EXPECT_GT(json_int64_get(json_object_get(pJson.get(), "bytes")), 0);
	EXPECT_GT(json_int64_get(json_object_get(pJson.get(), "maxBytes")), 0);

	const json_value *pEntries = json_object_get(pJson.get(), "entries");
	ASSERT_EQ(json_array_length(pEntries), 2);
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pEntries, 0), "label")), "Opened");
	EXPECT_STREQ(json_string_get(json_object_get(json_array_get(pEntries, 1), "label")), "Delete group");
	EXPECT_GT(json_int64_get(json_object_get(json_array_get(pEntries, 1), "timeNanos")), 0);
}

TEST(Report, AHalfMadeChangeIsReportedAsOne)
{
	CDocument Document(SmallMap());
	Document.Begin("Delete group");
	DeleteGroup(Document, 0);

	const CJson pJson = Parse(HistoryJson(Document));
	ASSERT_NE(pJson, nullptr);
	EXPECT_TRUE(json_boolean_get(json_object_get(pJson.get(), "editing")));
	// The entry is not there yet, but what is drawn is already the map
	// without the group - so the structure and the history say different
	// things on purpose, and the panel has to be told which.
	EXPECT_EQ(json_array_length(json_object_get(pJson.get(), "entries")), 1);
	EXPECT_EQ(json_array_length(json_object_get(Parse(StructureJson(Document.Map())).get(), "groups")), 1);
	Document.Abort();
}

// The points of one envelope. They are whole numbers in the file and whole
// numbers here, so what goes out is what comes back - an editor that saves
// the map again writes the same bytes it read.
TEST(Report, AnEnvelopeSaysWhatItsPointsAre)
{
	CMapState Map;
	CEnvelope Envelope;
	Envelope.m_Name = "colour";
	Envelope.m_Channels = 4;
	std::vector<CEnvPoint_runtime> vPoints(2);
	vPoints[0].m_Time = CFixedTime(0);
	vPoints[0].m_Curvetype = CURVETYPE_LINEAR;
	vPoints[0].m_aValues[0] = 1024;
	vPoints[1].m_Time = CFixedTime(1500);
	vPoints[1].m_Curvetype = CURVETYPE_BEZIER;
	vPoints[1].m_aValues[0] = 512;
	vPoints[1].m_Bezier.m_aOutTangentDeltaX[0] = CFixedTime(200);
	vPoints[1].m_Bezier.m_aOutTangentDeltaY[0] = -64;
	Envelope.m_Points = CSharedList<CEnvPoint_runtime>(std::move(vPoints));
	Map.AddEnvelope(std::move(Envelope));

	const std::string Json = EnvelopeJson(Map, 0);
	const CJson pRead(JsonParse(Json.c_str(), Json.size()), json_value_free);
	ASSERT_NE(pRead, nullptr) << Json;
	EXPECT_STREQ(json_string_get(json_object_get(pRead.get(), "name")), "colour");
	EXPECT_EQ(json_int_get(json_object_get(pRead.get(), "channels")), 4);
	const json_value *pPoints = json_object_get(pRead.get(), "points");
	ASSERT_EQ(json_array_length(pPoints), 2u);

	const json_value *pSecond = json_array_get(pPoints, 1);
	EXPECT_EQ(json_int_get(json_object_get(pSecond, "time")), 1500);
	EXPECT_EQ(json_int_get(json_object_get(pSecond, "curve")), CURVETYPE_BEZIER);
	EXPECT_EQ(json_int_get(json_array_get(json_object_get(pSecond, "values"), 0)), 512);
	// The tangents go out as pairs, one pair per channel.
	const json_value *pOut = json_object_get(pSecond, "out");
	ASSERT_EQ(json_array_length(pOut), 8u);
	EXPECT_EQ(json_int_get(json_array_get(pOut, 0)), 200);
	EXPECT_EQ(json_int_get(json_array_get(pOut, 1)), -64);

	// Only four channels' worth of values, whatever the point holds room for.
	EXPECT_EQ(json_array_length(json_object_get(json_array_get(pPoints, 0), "values")), 4u);
	EXPECT_STREQ(EnvelopeJson(Map, 1).c_str(), "null");
}
