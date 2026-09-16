#include <game/map/document/structure.h>

#include <gtest/gtest.h>

#include <tuple>

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

TEST(Structure, ADrawnLayerIsResizedByItself)
{
	CDocument Document(TwoGroups());
	Document.Begin("Resize");
	CTileLayer Painted = *Document.Map().TileLayer(0, 0);
	CTile Solid = {};
	Solid.m_Index = 4;
	Painted.m_Tiles.Set(7, 7, Solid);
	Painted.m_Tiles.Set(1, 1, Solid);
	Document.Edit().ReplaceLayer(0, 0, Painted);
	Document.Commit();
	const CLayer *pUntouched = Document.Map().Layer(0, 1);

	Document.Begin("Resize");
	ResizeLayer(Document, CLayerAddress{0, 0}, 16, 4);
	Document.Commit();

	EXPECT_EQ(Document.Map().TileLayer(0, 0)->Width(), 16);
	EXPECT_EQ(Document.Map().TileLayer(0, 0)->Height(), 4);
	// What was below the new edge is gone, and the rest is where it was.
	EXPECT_EQ(Document.Map().TileLayer(0, 0)->m_Tiles.Get(7, 7).m_Index, 0);
	EXPECT_EQ(Document.Map().TileLayer(0, 0)->m_Tiles.Get(1, 1).m_Index, 4);
	// The game layer is not a drawn layer's business.
	EXPECT_EQ(Document.Map().TileLayer(1, 0)->Width(), 8);
	// And neither is the layer beside it.
	EXPECT_EQ(Document.Map().Layer(0, 1), pUntouched);
}

TEST(Structure, ResizingOnePhysicsLayerResizesThemAll)
{
	CMapState Map = TwoGroups();
	CGroup Game = *Map.Group(1);
	Game.m_vpLayers.push_back(std::make_shared<const CLayer>(TileLayer("tele", ETileLayerKind::TELE)));
	Map.ReplaceGroup(1, std::move(Game));
	CDocument Document(std::move(Map));
	const CLayer *pDrawn = Document.Map().Layer(0, 0);

	Document.Begin("Resize");
	ResizeLayer(Document, CLayerAddress{1, 1}, 12, 12);
	Document.Commit();

	// The tele layer was asked, but the game layer plays the same size.
	EXPECT_EQ(Document.Map().TileLayer(1, 1)->Width(), 12);
	EXPECT_EQ(Document.Map().TileLayer(1, 1)->Height(), 12);
	EXPECT_EQ(Document.Map().TileLayer(1, 0)->Width(), 12);
	EXPECT_EQ(Document.Map().TileLayer(1, 0)->Height(), 12);
	// A layer that is only drawn keeps its own size, and stays the node it is.
	EXPECT_EQ(Document.Map().TileLayer(0, 0)->Width(), 8);
	EXPECT_EQ(Document.Map().Layer(0, 0), pDrawn);

	// A size that is already there is not a change.
	const size_t Entries = Document.History().NumEntries();
	Document.Begin("Resize");
	ResizeLayer(Document, CLayerAddress{1, 0}, 12, 12);
	Document.Commit();
	EXPECT_EQ(Document.History().NumEntries(), Entries);
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

// Envelopes. The points of one are in time order because the sum that reads
// them counts on it, so where a point goes is its time's business and not the
// caller's - and taking an envelope out has to put right everything that was
// bound to it, or the map comes back with its colours from somewhere else.

namespace
{
	CEnvPoint_runtime Point(int Millis, int Value)
	{
		CEnvPoint_runtime Made = {};
		Made.m_Time = CFixedTime(Millis);
		Made.m_aValues[0] = Value;
		return Made;
	}

	CMapState WithEnvelopes()
	{
		CMapState Map = TwoGroups();
		for(const char *pName : {"first", "second", "third"})
		{
			CEnvelope Envelope;
			Envelope.m_Name = pName;
			Map.AddEnvelope(std::move(Envelope));
		}
		// The game layer is coloured by the middle one, the other two layers
		// by the ones on either side of it.
		CTileLayer Bound = *Map.TileLayer(0, 0);
		Bound.m_ColorEnvelope = 0;
		Map.ReplaceLayer(0, 0, std::move(Bound));
		CTileLayer Middle = *Map.TileLayer(0, 1);
		Middle.m_ColorEnvelope = 1;
		Map.ReplaceLayer(0, 1, std::move(Middle));
		CTileLayer Above = *Map.TileLayer(1, 0);
		Above.m_ColorEnvelope = 2;
		Map.ReplaceLayer(1, 0, std::move(Above));
		return Map;
	}
	CMapState WithImages()
	{
		CMapState Map = TwoGroups();
		for(const char *pName : {"first", "second", "third"})
		{
			CImage Image;
			Image.m_Name = pName;
			Image.m_Width = 4;
			Image.m_Height = 4;
			Map.AddImage(std::move(Image));
		}
		// Each of the three layers drawn with another of the three pictures.
		for(const auto &[Group, Layer, Index] : {std::tuple{0, 0, 0}, std::tuple{0, 1, 1}, std::tuple{1, 0, 2}})
		{
			CTileLayer Bound = *Map.TileLayer(Group, Layer);
			Bound.m_Image = Index;
			Map.ReplaceLayer(Group, Layer, std::move(Bound));
		}
		return Map;
	}
} // namespace

TEST(Structure, APictureIsAddedAtTheEnd)
{
	CDocument Document(WithImages());
	CImage Image;
	Image.m_Name = "fourth";
	Document.Begin("Add");
	EXPECT_EQ(AddImage(Document, std::move(Image)), 3u);
	Document.Commit();
	ASSERT_EQ(Document.Map().NumImages(), 4u);
	EXPECT_EQ(Document.Map().Image(3)->m_Name, "fourth");
}

TEST(Structure, TakingAPictureOutTakesItOffEveryLayerDrawnWithIt)
{
	CDocument Document(WithImages());
	const CLayer *pUntouched = Document.Map().Layer(0, 0);
	Document.Begin("Delete");
	DeleteImage(Document, 1);
	Document.Commit();

	const CMapState &Map = Document.Map();
	ASSERT_EQ(Map.NumImages(), 2u);
	EXPECT_EQ(Map.Image(0)->m_Name, "first");
	EXPECT_EQ(Map.Image(1)->m_Name, "third");
	// Below it: unchanged, and the same node, because nothing about that
	// layer is different.
	EXPECT_EQ(Map.TileLayer(0, 0)->m_Image, 0);
	EXPECT_EQ(Map.Layer(0, 0), pUntouched) << "a layer drawn with another picture is the node it was";
	// Drawn with the one that is gone: drawn with none.
	EXPECT_EQ(Map.TileLayer(0, 1)->m_Image, -1);
	// Above it: one place down, and still the same picture.
	EXPECT_EQ(Map.TileLayer(1, 0)->m_Image, 1);
	EXPECT_EQ(Map.Image(Map.TileLayer(1, 0)->m_Image)->m_Name, "third");
}

TEST(Structure, ReplacingAPictureKeepsTheLayersDrawnWithIt)
{
	CDocument Document(WithImages());
	CImage Other;
	Other.m_Name = "other";
	Other.m_External = false;
	Other.m_Width = 2;
	Other.m_Height = 2;
	Other.m_Data.Mutable().assign(2 * 2 * 4, 0x7f);
	Document.Begin("Replace");
	SetImage(Document, 1, std::move(Other));
	Document.Commit();

	const CMapState &Map = Document.Map();
	ASSERT_EQ(Map.NumImages(), 3u);
	EXPECT_EQ(Map.Image(1)->m_Name, "other");
	EXPECT_EQ(Map.Image(1)->m_Width, 2);
	EXPECT_FALSE(Map.Image(1)->m_External);
	// The layer still points at place one; what is in that place changed.
	EXPECT_EQ(Map.TileLayer(0, 1)->m_Image, 1);

	// And a version before it still has the picture that was there.
	Document.Undo();
	EXPECT_EQ(Document.Map().Image(1)->m_Name, "second");
}

namespace
{
	/** A map with one quad layer, and a picture for it to be drawn with. */
	CDocument QuadMap(int ImageWidth = 0, int ImageHeight = 0)
	{
		CMapState Map;
		CGroup Group;
		CQuadLayer Layer;
		if(ImageWidth > 0)
		{
			Layer.m_Image = 0;
			CImage Image;
			Image.m_Name = "picture";
			Image.m_Width = ImageWidth;
			Image.m_Height = ImageHeight;
			Map.AddImage(std::move(Image));
		}
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Layer)));
		Map.AddGroup(std::move(Group));
		return CDocument(std::move(Map));
	}

	/** A quad whose corners are somewhere rather than in a rectangle. */
	CQuad CrookedQuad()
	{
		CQuad Quad = MakeQuad(0, 0, 64, 64);
		Quad.m_aPoints[0] = CPoint{i2fx(-30), i2fx(-34)};
		Quad.m_aPoints[1] = CPoint{i2fx(40), i2fx(-20)};
		Quad.m_aPoints[2] = CPoint{i2fx(-20), i2fx(30)};
		Quad.m_aPoints[3] = CPoint{i2fx(36), i2fx(26)};
		Quad.m_aPoints[4] = CPoint{i2fx(0), i2fx(0)};
		return Quad;
	}

	const CQuad &QuadIn(const CDocument &Document, size_t Index = 0)
	{
		return std::get<CQuadLayer>(*Document.Map().Layer(0, 0)).m_Quads[Index];
	}
} // namespace

TEST(Structure, ASoundSourceIsAddedChangedAndTakenOut)
{
	CMapState Map;
	CGroup Group;
	Group.m_vpLayers.push_back(std::make_shared<const CLayer>(CSoundLayer()));
	Map.AddGroup(std::move(Group));
	CDocument Document(std::move(Map));
	const CLayerAddress Where{0, 0};

	Document.Begin("Add");
	EXPECT_EQ(AddSoundSource(Document, Where, MakeSoundSource(320, 160)), 0u);
	EXPECT_EQ(AddSoundSource(Document, Where, MakeSoundSource(64, 64, 32)), 1u);
	Document.Commit();

	const auto &&Sources = [&Document]() -> const CSharedList<CSoundSource> & {
		return std::get<CSoundLayer>(*Document.Map().Layer(0, 0)).m_Sources;
	};
	ASSERT_EQ(Sources().Size(), 2u);
	// What a new one is: a circle that loops and is heard all the way to its
	// edge, bound to no envelope.
	EXPECT_EQ(fx2i(Sources()[0].m_Position.x), 320);
	EXPECT_EQ(Sources()[0].m_Shape.m_Type, CSoundShape::SHAPE_CIRCLE);
	EXPECT_EQ(Sources()[0].m_Shape.m_Circle.m_Radius, 96);
	EXPECT_EQ(Sources()[0].m_Loop, 1);
	EXPECT_EQ(Sources()[0].m_PosEnv, -1);
	EXPECT_EQ(Sources()[1].m_Shape.m_Circle.m_Radius, 32);

	Document.Begin("Move");
	CSoundSource Moved = Sources()[0];
	Moved.m_Position = CPoint{i2fx(10), i2fx(20)};
	SetSoundSource(Document, Where, 0, Moved);
	Document.Commit();
	EXPECT_EQ(fx2i(Sources()[0].m_Position.x), 10);
	EXPECT_EQ(fx2i(Sources()[1].m_Position.x), 64) << "the other one is left alone";

	Document.Begin("Delete");
	DeleteSoundSource(Document, Where, 0);
	Document.Commit();
	ASSERT_EQ(Sources().Size(), 1u);
	EXPECT_EQ(fx2i(Sources()[0].m_Position.x), 64);
}

TEST(Structure, SquaringAQuadMakesItTheRectangleItSpans)
{
	CDocument Document = QuadMap();
	Document.Begin("Add");
	AddQuad(Document, CLayerAddress{0, 0}, CrookedQuad());
	Document.Commit();

	Document.Begin("Square");
	EXPECT_TRUE(ShapeQuad(Document, CLayerAddress{0, 0}, 0, EQuadShape::SQUARE));
	Document.Commit();

	const CQuad &Quad = QuadIn(Document);
	EXPECT_EQ(fx2i(Quad.m_aPoints[0].x), -30);
	EXPECT_EQ(fx2i(Quad.m_aPoints[0].y), -34);
	EXPECT_EQ(fx2i(Quad.m_aPoints[1].x), 40);
	EXPECT_EQ(fx2i(Quad.m_aPoints[1].y), -34);
	EXPECT_EQ(fx2i(Quad.m_aPoints[2].x), -30);
	EXPECT_EQ(fx2i(Quad.m_aPoints[2].y), 30);
	EXPECT_EQ(fx2i(Quad.m_aPoints[3].x), 40);
	EXPECT_EQ(fx2i(Quad.m_aPoints[3].y), 30);
	// The pivot is not a corner and is left where it was.
	EXPECT_EQ(fx2i(Quad.m_aPoints[4].x), 0);
}

TEST(Structure, TheProportionsComeFromThePictureOrTheAnswerIsNo)
{
	// Twice as wide as it is tall, so a quad 70 across becomes 35 down.
	CDocument Document = QuadMap(512, 256);
	Document.Begin("Add");
	AddQuad(Document, CLayerAddress{0, 0}, CrookedQuad());
	Document.Commit();

	Document.Begin("Aspect");
	EXPECT_TRUE(ShapeQuad(Document, CLayerAddress{0, 0}, 0, EQuadShape::ASPECT));
	Document.Commit();

	const CQuad &Quad = QuadIn(Document);
	EXPECT_EQ(fx2i(Quad.m_aPoints[1].x) - fx2i(Quad.m_aPoints[0].x), 70);
	EXPECT_EQ(fx2i(Quad.m_aPoints[2].y) - fx2i(Quad.m_aPoints[0].y), 35);

	// A layer drawn with no picture has no proportions to ask for.
	CDocument Plain = QuadMap();
	Plain.Begin("Add");
	AddQuad(Plain, CLayerAddress{0, 0}, CrookedQuad());
	Plain.Commit();
	Plain.Begin("Aspect");
	EXPECT_FALSE(ShapeQuad(Plain, CLayerAddress{0, 0}, 0, EQuadShape::ASPECT));
	Plain.Abort();
}

TEST(Structure, ThePivotGoesInTheMiddleAndTheCornersOntoTheGrid)
{
	CDocument Document = QuadMap();
	Document.Begin("Add");
	AddQuad(Document, CLayerAddress{0, 0}, CrookedQuad());
	Document.Commit();

	Document.Begin("Centre");
	EXPECT_TRUE(ShapeQuad(Document, CLayerAddress{0, 0}, 0, EQuadShape::CENTER_PIVOT));
	Document.Commit();
	// Halfway between -30 and 40, and between -34 and 30.
	EXPECT_EQ(fx2i(QuadIn(Document).m_aPoints[4].x), 5);
	EXPECT_EQ(fx2i(QuadIn(Document).m_aPoints[4].y), -2);

	Document.Begin("Align");
	EXPECT_TRUE(ShapeQuad(Document, CLayerAddress{0, 0}, 0, EQuadShape::ALIGN, 32));
	Document.Commit();
	const CQuad &Quad = QuadIn(Document);
	// Every corner to the nearest crossing, and a corner left of nothing
	// rounds away from zero rather than towards it.
	EXPECT_EQ(fx2i(Quad.m_aPoints[0].x), -32);
	EXPECT_EQ(fx2i(Quad.m_aPoints[0].y), -32);
	EXPECT_EQ(fx2i(Quad.m_aPoints[1].x), 32);
	EXPECT_EQ(fx2i(Quad.m_aPoints[1].y), -32);
	EXPECT_EQ(fx2i(Quad.m_aPoints[2].x), -32);
	EXPECT_EQ(fx2i(Quad.m_aPoints[2].y), 32);
	// The pivot goes along, or it would no longer be where the quad turns.
	EXPECT_EQ(fx2i(Quad.m_aPoints[4].x), 0);
}

TEST(Structure, APointGoesWhereItsTimeBelongs)
{
	CDocument Document(WithEnvelopes());
	Document.Begin("Points");
	EXPECT_EQ(AddEnvelopePoint(Document, 0, Point(1000, 10)), 0u);
	EXPECT_EQ(AddEnvelopePoint(Document, 0, Point(3000, 30)), 1u);
	// In between the two that are there, not at the end.
	EXPECT_EQ(AddEnvelopePoint(Document, 0, Point(2000, 20)), 1u);
	// At the same time as one that is there, and after it.
	EXPECT_EQ(AddEnvelopePoint(Document, 0, Point(2000, 25)), 2u);
	Document.Commit();

	const CEnvelope *pEnvelope = Document.Map().Envelope(0);
	ASSERT_EQ(pEnvelope->m_Points.Size(), 4u);
	EXPECT_EQ(pEnvelope->m_Points[0].m_aValues[0], 10);
	EXPECT_EQ(pEnvelope->m_Points[1].m_aValues[0], 20);
	EXPECT_EQ(pEnvelope->m_Points[2].m_aValues[0], 25);
	EXPECT_EQ(pEnvelope->m_Points[3].m_aValues[0], 30);
}

TEST(Structure, APointDraggedPastItsNeighbourChangesPlaces)
{
	CDocument Document(WithEnvelopes());
	Document.Begin("Points");
	AddEnvelopePoint(Document, 0, Point(1000, 10));
	AddEnvelopePoint(Document, 0, Point(2000, 20));
	AddEnvelopePoint(Document, 0, Point(3000, 30));
	// The first one dragged to the far end.
	EXPECT_EQ(SetEnvelopePoint(Document, 0, 0, Point(4000, 10)), 2u);
	Document.Commit();

	const CEnvelope *pEnvelope = Document.Map().Envelope(0);
	ASSERT_EQ(pEnvelope->m_Points.Size(), 3u);
	EXPECT_EQ(pEnvelope->m_Points[0].m_aValues[0], 20);
	EXPECT_EQ(pEnvelope->m_Points[2].m_aValues[0], 10);

	Document.Begin("Delete");
	DeleteEnvelopePoint(Document, 0, 1);
	Document.Commit();
	ASSERT_EQ(Document.Map().Envelope(0)->m_Points.Size(), 2u);
	EXPECT_EQ(Document.Map().Envelope(0)->m_Points[1].m_aValues[0], 10);
}

TEST(Structure, TakingAnEnvelopeOutTakesEverythingOffIt)
{
	CDocument Document(WithEnvelopes());
	const CLayer *pUntouched = Document.Map().Layer(0, 0);
	Document.Begin("Delete");
	DeleteEnvelope(Document, 1);
	Document.Commit();

	const CMapState &Map = Document.Map();
	ASSERT_EQ(Map.NumEnvelopes(), 2u);
	EXPECT_EQ(Map.Envelope(0)->m_Name, "first");
	EXPECT_EQ(Map.Envelope(1)->m_Name, "third");
	// Bound below it: unchanged, and the same node, because nothing about
	// that layer is different.
	EXPECT_EQ(Map.TileLayer(0, 0)->m_ColorEnvelope, 0);
	EXPECT_EQ(Map.Layer(0, 0), pUntouched) << "a layer that was bound to nothing is the node it was";
	// Bound to the one that is gone: bound to nothing.
	EXPECT_EQ(Map.TileLayer(0, 1)->m_ColorEnvelope, -1);
	// Bound above it: one place down, and still the same envelope.
	EXPECT_EQ(Map.TileLayer(1, 0)->m_ColorEnvelope, 1);
	EXPECT_EQ(Map.Envelope(Map.TileLayer(1, 0)->m_ColorEnvelope)->m_Name, "third");
}
