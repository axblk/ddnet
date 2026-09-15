#include <game/map/document/history.h>

#include <gtest/gtest.h>

// Undoing is meant to be pointing at an older version rather than putting a
// map back together, so what is tested here is that going back really is a
// pointer swap, that a new version drops what could have been redone, and
// that the limit throws away the oldest versions and nothing else.

namespace
{
	constexpr int CHUNK = CTileStore<CTile>::CHUNK_SIZE;

	CTile Tile(int Index)
	{
		CTile Result = {};
		Result.m_Index = (unsigned char)Index;
		return Result;
	}

	// One group, one layer, eight blocks across and one down - room for eight
	// strokes that have nothing to do with each other.
	CMapState OneLayer()
	{
		CMapState State;
		CGroup Group;
		Group.m_Name = "group";
		CTileLayer Layer(ETileLayerKind::TILES, 8 * CHUNK, CHUNK);
		Layer.m_Name = "layer";
		Group.m_vpLayers.push_back(std::make_shared<const CTileLayer>(std::move(Layer)));
		State.AddGroup(std::move(Group));
		return State;
	}

	// The map with one more tile painted into the block at `Chunk`.
	CMapState Draw(const CMapState &Current, int Chunk, int Index)
	{
		CMapState Next = Current;
		CTileLayer Changed = *Next.Layer(0, 0);
		Changed.m_Tiles.Set(Chunk * CHUNK, 0, Tile(Index));
		Next.ReplaceLayer(0, 0, std::move(Changed));
		return Next;
	}
}

TEST(History, OpensWithOneEntryAndNoWayOut)
{
	CHistory History(OneLayer(), "Opened");
	EXPECT_EQ(History.NumEntries(), 1u);
	EXPECT_EQ(History.CurrentIndex(), 0u);
	EXPECT_FALSE(History.CanUndo());
	EXPECT_FALSE(History.CanRedo());
	EXPECT_FALSE(History.Undo());
	EXPECT_FALSE(History.Redo());
	EXPECT_EQ(History.Entry(0).m_Label, "Opened");
}

TEST(History, UndoAndRedoWalkTheLine)
{
	CHistory History(OneLayer());
	History.Push(Draw(History.Current(), 0, 5), "Draw");
	History.Push(Draw(History.Current(), 1, 6), "Draw");

	EXPECT_EQ(History.NumEntries(), 3u);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(CHUNK, 0).m_Index, 6);

	ASSERT_TRUE(History.Undo());
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 5);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(CHUNK, 0).m_Index, 0);

	ASSERT_TRUE(History.Undo());
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 0);
	EXPECT_FALSE(History.CanUndo());

	ASSERT_TRUE(History.Redo());
	ASSERT_TRUE(History.Redo());
	EXPECT_FALSE(History.CanRedo());
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(CHUNK, 0).m_Index, 6);
}

TEST(History, UndoIsAPointerSwap)
{
	CHistory History(OneLayer());
	const CGroup *pOpened = History.Current().Group(0);
	const void *pUntouched = History.Current().Layer(0, 0)->m_Tiles.ChunkId(7, 0);

	History.Push(Draw(History.Current(), 0, 5), "Draw");
	// The version that was undone to is the node that was there before, not a
	// copy of it - that is the whole of what undoing costs.
	ASSERT_TRUE(History.Undo());
	EXPECT_EQ(History.Current().Group(0), pOpened);
	// And the version that came after shares every block the stroke missed.
	ASSERT_TRUE(History.Redo());
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.ChunkId(7, 0), pUntouched);
}

TEST(History, PushingAfterUndoDropsTheWayForward)
{
	CHistory History(OneLayer());
	History.Push(Draw(History.Current(), 0, 5), "Draw");
	History.Push(Draw(History.Current(), 1, 6), "Draw");
	ASSERT_TRUE(History.Undo());

	History.Push(Draw(History.Current(), 2, 7), "Draw");
	EXPECT_EQ(History.NumEntries(), 3u);
	EXPECT_EQ(History.CurrentIndex(), 2u);
	EXPECT_FALSE(History.CanRedo());
	// The stroke that was undone is gone for good; the one before it stands.
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(CHUNK, 0).m_Index, 0);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 5);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(2 * CHUNK, 0).m_Index, 7);
}

TEST(History, JumpingGoesStraightThereAndKeepsTheRest)
{
	CHistory History(OneLayer());
	for(int i = 0; i < 4; ++i)
	{
		History.Push(Draw(History.Current(), i, 5 + i), "Draw");
	}

	History.JumpTo(1);
	EXPECT_EQ(History.CurrentIndex(), 1u);
	EXPECT_EQ(History.NumEntries(), 5u);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 5);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(CHUNK, 0).m_Index, 0);

	History.JumpTo(4);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(3 * CHUNK, 0).m_Index, 8);
}

TEST(History, ASharedBlockIsCountedOnceOverTheWholeHistory)
{
	CHistory History(OneLayer());
	// A stroke of eight blocks, then seven versions that touch one of them
	// each: the blocks that stand still are held by every version, and they
	// are still eight blocks worth of memory, not fifty-six.
	CMapState Painted = History.Current();
	for(int i = 0; i < 8; ++i)
	{
		CTileLayer Changed = *Painted.Layer(0, 0);
		Changed.m_Tiles.Set(i * CHUNK, 0, Tile(1));
		Painted.ReplaceLayer(0, 0, std::move(Changed));
	}
	History.Push(std::move(Painted), "Draw");

	const uint64_t AfterOne = History.Bytes();
	for(int i = 1; i < 8; ++i)
	{
		History.Push(Draw(History.Current(), i, 2), "Draw");
	}
	// Seven more versions, each having taken one block apart: seven blocks
	// more than the eight, and nothing else worth speaking of.
	const uint64_t Block = sizeof(CTile) * CTileStore<CTile>::TILES_PER_CHUNK;
	EXPECT_GE(History.Bytes(), AfterOne + 6 * Block);
	EXPECT_LT(History.Bytes(), AfterOne + 9 * Block);
}

TEST(History, TheByteLimitDropsTheOldestVersions)
{
	CHistory History(OneLayer());
	const uint64_t Block = sizeof(CTile) * CTileStore<CTile>::TILES_PER_CHUNK;
	History.SetLimits(4 * Block, 1000);

	// Eight strokes over the same block, so that every version but the
	// current one is a whole block of memory that only the way back holds on
	// to - which is the case the limit exists for.
	for(int i = 0; i < 8; ++i)
	{
		History.Push(Draw(History.Current(), 0, 5 + i), "Draw");
	}

	EXPECT_LE(History.Bytes(), 4 * Block);
	EXPECT_GE(History.NumEntries(), 2u);
	EXPECT_LT(History.NumEntries(), 9u);
	EXPECT_EQ(History.CurrentIndex(), History.NumEntries() - 1);
	// The map itself lost nothing - the last stroke stands, only the way back
	// to the early ones is gone.
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 12);
	EXPECT_FALSE(History.CanRedo());
}

TEST(History, TheEntryLimitDropsTheOldestVersions)
{
	CHistory History(OneLayer());
	History.SetLimits((uint64_t)1 << 40, 3);

	for(int i = 0; i < 8; ++i)
	{
		History.Push(Draw(History.Current(), i, 5), "Draw");
	}

	EXPECT_EQ(History.NumEntries(), 3u);
	EXPECT_EQ(History.CurrentIndex(), 2u);
	ASSERT_TRUE(History.Undo());
	ASSERT_TRUE(History.Undo());
	EXPECT_FALSE(History.CanUndo());
	// Two steps back from the eighth stroke is the sixth, and the five before
	// it are painted into it for good.
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(5 * CHUNK, 0).m_Index, 5);
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(6 * CHUNK, 0).m_Index, 0);
}

TEST(History, TheVersionTheMapIsInSurvivesAnyLimit)
{
	CHistory History(OneLayer());
	History.SetLimits(0, 1);
	History.Push(Draw(History.Current(), 0, 5), "Draw");

	EXPECT_EQ(History.NumEntries(), 1u);
	EXPECT_FALSE(History.CanUndo());
	EXPECT_FALSE(History.CanRedo());
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 5);
}

TEST(History, ALimitGivesUpTheWayForwardBeforeTheWayBack)
{
	CHistory History(OneLayer());
	for(int i = 0; i < 4; ++i)
	{
		History.Push(Draw(History.Current(), i, 5), "Draw");
	}
	History.JumpTo(0);

	History.SetLimits((uint64_t)1 << 40, 2);
	// Standing on the oldest version, there is nothing older to give up, so
	// what goes is the far end of the way forward.
	EXPECT_EQ(History.NumEntries(), 2u);
	EXPECT_EQ(History.CurrentIndex(), 0u);
	ASSERT_TRUE(History.Redo());
	EXPECT_EQ(History.Current().Layer(0, 0)->m_Tiles.Get(0, 0).m_Index, 5);
	EXPECT_FALSE(History.CanRedo());
}
