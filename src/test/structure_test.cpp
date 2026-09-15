#include <game/map/document/structure.h>

#include <gtest/gtest.h>

using namespace map_document;

// The shape of a map: which groups there are, which layers are in them and in
// what order. Every test here also asks what a change cost - a layer that was
// only moved has to come along as the node it is, or a reordered panel would
// copy the map every time somebody dragged something.

namespace
{
	CLayer TileLayer(const char *pName, ETileLayerKind Kind = ETileLayerKind::TILES)
	{
		CTileLayer Layer(Kind, 8, 8);
		Layer.m_Name = pName;
		return Layer;
	}

	CMapState TwoGroups()
	{
		CMapState Map;
		CGroup First;
		First.m_Name = "first";
		First.m_vpLayers.push_back(std::make_shared<const CLayer>(TileLayer("a")));
		First.m_vpLayers.push_back(std::make_shared<const CLayer>(TileLayer("b")));
		CGroup Second;
		Second.m_Name = "game";
		Second.m_vpLayers.push_back(std::make_shared<const CLayer>(TileLayer("game", ETileLayerKind::GAME)));
		Map.AddGroup(std::move(First));
		Map.AddGroup(std::move(Second));
		return Map;
	}

	const char *NameOf(const CMapState &Map, size_t Group, size_t Layer)
	{
		return LayerProperties(*Map.Layer(Group, Layer)).m_Name.c_str();
	}
} // namespace

TEST(Structure, AGroupIsAddedAtTheEnd)
{
	CDocument Document(TwoGroups());
	Document.Begin("Add group");
	CGroup Added;
	Added.m_Name = "new";
	const size_t Index = AddGroup(Document, std::move(Added));
	Document.Commit();

	EXPECT_EQ(Index, 2u);
	ASSERT_EQ(Document.Map().NumGroups(), 3u);
	EXPECT_EQ(Document.Map().Group(2)->m_Name, "new");
}

TEST(Structure, ADeletedGroupTakesItsLayersWithIt)
{
	CDocument Document(TwoGroups());
	Document.Begin("Delete group");
	DeleteGroup(Document, 0);
	Document.Commit();

	ASSERT_EQ(Document.Map().NumGroups(), 1u);
	EXPECT_EQ(Document.Map().Group(0)->m_Name, "game");

	ASSERT_TRUE(Document.Undo());
	EXPECT_EQ(Document.Map().NumGroups(), 2u);
	EXPECT_STREQ(NameOf(Document.Map(), 0, 1), "b");
}

TEST(Structure, AMovedGroupGoesWhereItWasDropped)
{
	CDocument Document(TwoGroups());
	Document.Begin("Move group");
	EXPECT_EQ(MoveGroup(Document, 0, 1), 1u);
	Document.Commit();

	EXPECT_EQ(Document.Map().Group(0)->m_Name, "game");
	EXPECT_EQ(Document.Map().Group(1)->m_Name, "first");
}

TEST(Structure, ALayerIsAddedAtTheEndOfItsGroup)
{
	CDocument Document(TwoGroups());
	Document.Begin("Add layer");
	const CLayerAddress Added = AddLayer(Document, 0, TileLayer("c"));
	Document.Commit();

	EXPECT_EQ(Added.m_Group, 0u);
	EXPECT_EQ(Added.m_Layer, 2u);
	EXPECT_STREQ(NameOf(Document.Map(), 0, 2), "c");
}

TEST(Structure, ADeletedLayerLeavesTheRestWhereTheyWere)
{
	CDocument Document(TwoGroups());
	Document.Begin("Delete layer");
	DeleteLayer(Document, CLayerAddress{0, 0});
	Document.Commit();

	ASSERT_EQ(Document.Map().NumLayers(0), 1u);
	EXPECT_STREQ(NameOf(Document.Map(), 0, 0), "b");
	EXPECT_EQ(Document.Map().NumLayers(1), 1u);
}

TEST(Structure, ALayerMovedInsideItsGroupChangesPlaceWithTheOther)
{
	CDocument Document(TwoGroups());
	Document.Begin("Move layer");
	const CLayerAddress Now = MoveLayer(Document, CLayerAddress{0, 0}, CLayerAddress{0, 1});
	Document.Commit();

	EXPECT_EQ(Now, (CLayerAddress{0, 1}));
	EXPECT_STREQ(NameOf(Document.Map(), 0, 0), "b");
	EXPECT_STREQ(NameOf(Document.Map(), 0, 1), "a");
}

TEST(Structure, ALayerMovedIntoAnotherGroupIsTheSameNodeThere)
{
	CDocument Document(TwoGroups());
	const CLayer *pWas = Document.Map().Layer(0, 1);

	Document.Begin("Move layer");
	const CLayerAddress Now = MoveLayer(Document, CLayerAddress{0, 1}, CLayerAddress{1, 0});
	Document.Commit();

	EXPECT_EQ(Now, (CLayerAddress{1, 0}));
	ASSERT_EQ(Document.Map().NumLayers(0), 1u);
	ASSERT_EQ(Document.Map().NumLayers(1), 2u);
	EXPECT_STREQ(NameOf(Document.Map(), 1, 0), "b");
	// Moving a layer does not copy it: what the new version holds there is the
	// very node the old one held.
	EXPECT_EQ(Document.Map().Layer(1, 0), pWas);
}

TEST(Structure, MovingALayerLeavesEveryOtherGroupAlone)
{
	CDocument Document(TwoGroups());
	CMapState Before = Document.Map();

	Document.Begin("Move layer");
	MoveLayer(Document, CLayerAddress{0, 0}, CLayerAddress{0, 1});
	Document.Commit();

	// The group that was reordered is a new node, the one beside it is not.
	EXPECT_NE(Document.Map().Group(0), Before.Group(0));
	EXPECT_EQ(Document.Map().Group(1), Before.Group(1));
}

TEST(Structure, TheGameLayerIsFoundWhereverItIs)
{
	const CMapState Map = TwoGroups();
	const std::optional<CLayerAddress> Game = FindGameLayer(Map);
	ASSERT_TRUE(Game.has_value());
	EXPECT_EQ(*Game, (CLayerAddress{1, 0}));

	CMapState Empty;
	EXPECT_FALSE(FindGameLayer(Empty).has_value());
}

TEST(Structure, WhatCountsAsAPhysicsLayer)
{
	EXPECT_FALSE(IsPhysicsLayer(TileLayer("a")));
	EXPECT_TRUE(IsPhysicsLayer(TileLayer("game", ETileLayerKind::GAME)));
	EXPECT_TRUE(IsPhysicsLayer(TileLayer("tele", ETileLayerKind::TELE)));
	EXPECT_FALSE(IsPhysicsLayer(CLayer(CQuadLayer())));
}

TEST(Structure, AGroupsPropertiesAreChangedLikeEverythingElse)
{
	CDocument Document(TwoGroups());
	Document.Begin("Parallax");
	EditGroup(Document, 0, [](CGroup &Group) {
		Group.m_ParallaxX = 50;
		Group.m_Name = "sky";
	});
	Document.Commit();

	EXPECT_EQ(Document.Map().Group(0)->m_ParallaxX, 50);
	EXPECT_EQ(Document.Map().Group(0)->m_Name, "sky");

	ASSERT_TRUE(Document.Undo());
	EXPECT_EQ(Document.Map().Group(0)->m_ParallaxX, 100);
	EXPECT_EQ(Document.Map().Group(0)->m_Name, "first");
}

TEST(Structure, ALayerIsRenamedWhateverKindItIs)
{
	CDocument Document(TwoGroups());
	Document.Begin("Rename");
	EditLayer(Document, CLayerAddress{0, 0}, [](CLayer &Layer) {
		std::visit([](auto &Kind) { Kind.m_Name = "renamed"; }, Layer);
	});
	Document.Commit();

	EXPECT_STREQ(NameOf(Document.Map(), 0, 0), "renamed");
	EXPECT_EQ(Document.History().NumEntries(), 2u);
}

TEST(Structure, APropertyPutBackWhereItWasIsNoChange)
{
	CDocument Document(TwoGroups());
	Document.Begin("Parallax");
	EditGroup(Document, 0, [](CGroup &Group) { Group.m_ParallaxX = 100; });
	Document.Commit();

	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_FALSE(Document.CanUndo());
}
