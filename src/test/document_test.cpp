#include <game/map/document/document.h>

#include <gtest/gtest.h>

using namespace map_document;

// A transaction is the only way to change a map, so what is tested here is
// that it behaves like one: what is half done is what is drawn, what is given
// up leaves nothing behind, and what is done ends up in the history once,
// however many tools were inside it.

namespace
{
	constexpr int CHUNK = CTileStore<CTile>::CHUNK_SIZE;

	CTile Tile(int Index)
	{
		CTile Result = {};
		Result.m_Index = (unsigned char)Index;
		return Result;
	}

	CMapState OneLayer()
	{
		CMapState State;
		CGroup Group;
		CTileLayer Layer(ETileLayerKind::TILES, 4 * CHUNK, CHUNK);
		Layer.m_Name = "layer";
		Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Layer)));
		State.AddGroup(std::move(Group));
		return State;
	}

	// One tile painted into the version being made.
	void Draw(CDocument *pDocument, int x, int Index)
	{
		CTileLayer Changed = *pDocument->Edit().TileLayer(0, 0);
		Changed.m_Tiles.Set(x, 0, Tile(Index));
		pDocument->Edit().ReplaceLayer(0, 0, std::move(Changed));
	}

	int TileAt(const CDocument &Document, int x)
	{
		return Document.Map().TileLayer(0, 0)->m_Tiles.Get(x, 0).m_Index;
	}
}

TEST(Document, AMapThatWasJustOpenedHasOneVersionAndNoChange)
{
	const CDocument Document(OneLayer());
	EXPECT_FALSE(Document.IsEditing());
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_FALSE(Document.CanUndo());
	EXPECT_FALSE(Document.CanRedo());
}

TEST(Document, WhatIsHalfDoneIsWhatIsDrawn)
{
	CDocument Document(OneLayer());
	Document.Begin("Draw");
	Draw(&Document, 0, 5);

	// The map is the half-made version while the change is open, and the
	// history knows nothing of it yet.
	EXPECT_TRUE(Document.IsEditing());
	EXPECT_EQ(TileAt(Document, 0), 5);
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_EQ(Document.History().Current().TileLayer(0, 0)->m_Tiles.Get(0, 0).m_Index, 0);

	Document.Commit();
	EXPECT_FALSE(Document.IsEditing());
	EXPECT_EQ(Document.History().NumEntries(), 2u);
	EXPECT_EQ(TileAt(Document, 0), 5);
	EXPECT_EQ(Document.History().Entry(1).m_Label, "Draw");
}

TEST(Document, AChangeThatIsGivenUpLeavesNothingBehind)
{
	CDocument Document(OneLayer());
	Document.Begin("Draw");
	Draw(&Document, 0, 5);
	Draw(&Document, CHUNK, 6);
	Document.Abort();

	EXPECT_FALSE(Document.IsEditing());
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_EQ(TileAt(Document, 0), 0);
	EXPECT_EQ(TileAt(Document, CHUNK), 0);
	EXPECT_FALSE(Document.CanUndo());
}

TEST(Document, ManyChangesInOneTransactionAreOneEntry)
{
	CDocument Document(OneLayer());
	Document.Begin("Draw");
	for(int i = 0; i < 4; ++i)
	{
		Draw(&Document, i * CHUNK, 5 + i);
	}
	Document.Commit();

	EXPECT_EQ(Document.History().NumEntries(), 2u);
	// And one step back takes all four of them, because they were one stroke.
	ASSERT_TRUE(Document.Undo());
	for(int i = 0; i < 4; ++i)
	{
		EXPECT_EQ(TileAt(Document, i * CHUNK), 0);
	}
	ASSERT_TRUE(Document.Redo());
	EXPECT_EQ(TileAt(Document, 3 * CHUNK), 8);
}

TEST(Document, ATransactionInsideAnotherIsStillOneEntry)
{
	CDocument Document(OneLayer());
	Document.Begin("Draw");
	Draw(&Document, 0, 5);
	{
		// What an automapper run inside a stroke looks like from here.
		Document.Begin("Automap");
		Draw(&Document, CHUNK, 6);
		Document.Commit();
	}
	EXPECT_TRUE(Document.IsEditing());
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	Document.Commit();

	EXPECT_EQ(Document.History().NumEntries(), 2u);
	// The outermost one named it, not the one inside.
	EXPECT_EQ(Document.History().Entry(1).m_Label, "Draw");
	EXPECT_EQ(TileAt(Document, 0), 5);
	EXPECT_EQ(TileAt(Document, CHUNK), 6);
}

TEST(Document, GivingUpInsideGivesUpTheWholeChange)
{
	CDocument Document(OneLayer());
	Document.Begin("Draw");
	Draw(&Document, 0, 5);
	Document.Begin("Automap");
	Draw(&Document, CHUNK, 6);
	Document.Abort();

	EXPECT_FALSE(Document.IsEditing());
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_EQ(TileAt(Document, 0), 0);
}

TEST(Document, AChangeThatChangedNothingIsNoVersion)
{
	CDocument Document(OneLayer());
	Document.Begin("Look around");
	Document.Commit();
	EXPECT_EQ(Document.History().NumEntries(), 1u);

	// Painting the tile that is already there changes no block, so there is
	// nothing to write down - a slider dragged and put back is the same case.
	Document.Begin("Draw");
	Draw(&Document, 0, 0);
	Document.Commit();
	EXPECT_EQ(Document.History().NumEntries(), 1u);
	EXPECT_FALSE(Document.CanUndo());

	Document.Begin("Draw");
	Draw(&Document, 0, 5);
	Document.Commit();
	EXPECT_EQ(Document.History().NumEntries(), 2u);
}

TEST(Document, ChangingAPropertyIsAVersionLikeAnyOther)
{
	CDocument Document(OneLayer());
	Document.Begin("Rename layer");
	CTileLayer Renamed = *Document.Edit().TileLayer(0, 0);
	Renamed.m_Name = "sky";
	Document.Edit().ReplaceLayer(0, 0, std::move(Renamed));
	Document.Commit();

	EXPECT_EQ(LayerProperties(*Document.Map().Layer(0, 0)).m_Name, "sky");
	ASSERT_TRUE(Document.Undo());
	EXPECT_EQ(LayerProperties(*Document.Map().Layer(0, 0)).m_Name, "layer");
}

TEST(Document, TheHistoryLimitReachesThroughTheDocument)
{
	CDocument Document(OneLayer());
	Document.SetHistoryLimits((uint64_t)1 << 40, 3);
	for(int i = 0; i < 8; ++i)
	{
		Document.Begin("Draw");
		Draw(&Document, 0, 5 + i);
		Document.Commit();
	}
	EXPECT_EQ(Document.History().NumEntries(), 3u);
	EXPECT_EQ(TileAt(Document, 0), 12);
}
