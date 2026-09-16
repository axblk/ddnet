#include <game/map/document/shared_list.h>

#include <gtest/gtest.h>

#include <string>

using namespace map_document;

// Everything a map is made of that is not tiles lives in one of these, so
// what is tested here is the same thing the tile store promises: a copy costs
// a pointer, and a change costs the one list it was made in.

TEST(SharedList, EmptyHoldsNothing)
{
	const CSharedList<int> List;
	EXPECT_EQ(List.Size(), 0u);
	EXPECT_TRUE(List.Empty());
	EXPECT_TRUE(List.All().empty());
	EXPECT_EQ(List.Id(), nullptr);
	EXPECT_EQ(List.Bytes(), 0u);
}

TEST(SharedList, HoldsWhatItWasGiven)
{
	const CSharedList<int> List(std::vector<int>{1, 2, 3});
	ASSERT_EQ(List.Size(), 3u);
	EXPECT_EQ(List[0], 1);
	EXPECT_EQ(List[2], 3);
	EXPECT_EQ(List.All().size(), 3u);
}

TEST(SharedList, ACopyShares)
{
	CSharedList<int> First(std::vector<int>{1, 2, 3});
	const CSharedList<int> Second = First;
	EXPECT_EQ(First.Id(), Second.Id());

	First.Mutable().push_back(4);
	// The one that was written to is a list of its own now; the other one is
	// the list it always was.
	EXPECT_NE(First.Id(), Second.Id());
	EXPECT_EQ(First.Size(), 4u);
	EXPECT_EQ(Second.Size(), 3u);
}

TEST(SharedList, WritingTwiceTakesItApartOnce)
{
	CSharedList<int> First(std::vector<int>{1, 2, 3});
	const CSharedList<int> Second = First;

	First.Mutable().push_back(4);
	const void *pAfterFirstWrite = First.Id();
	First.Mutable().push_back(5);
	EXPECT_EQ(First.Id(), pAfterFirstWrite);
	EXPECT_EQ(Second.Size(), 3u);
}

TEST(SharedList, AListNobodySharesIsWrittenToInPlace)
{
	CSharedList<int> List(std::vector<int>{1, 2, 3});
	const void *pBefore = List.Id();
	List.Mutable()[1] = 7;
	EXPECT_EQ(List.Id(), pBefore);
	EXPECT_EQ(List[1], 7);
}

TEST(SharedList, AnEmptyListIsMadeOnFirstWrite)
{
	CSharedList<std::string> List;
	List.Mutable().emplace_back("cfg_x 1");
	ASSERT_EQ(List.Size(), 1u);
	EXPECT_EQ(List[0], "cfg_x 1");
	EXPECT_NE(List.Id(), nullptr);
	EXPECT_GT(List.Bytes(), 0u);
}

TEST(SharedList, ASharedListIsCountedOnce)
{
	const CSharedList<int> First(std::vector<int>(1000));
	// The copy is the thing being tested; a reference would share nothing to
	// count.
	// NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
	const CSharedList<int> Second = First;

	std::unordered_set<const void *> Seen;
	const uint64_t Both = First.BytesOnce(Seen) + Second.BytesOnce(Seen);
	EXPECT_EQ(Both, First.Bytes());
	EXPECT_GE(Both, 1000u * sizeof(int));
}
