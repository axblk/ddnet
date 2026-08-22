#include <game/server/gamecontroller.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

namespace
{
	std::vector<int> Assists(const CMatchRecentAttackers &Attackers, int Tick, int WindowTicks, int KillerParticipantId, int VictimParticipantId)
	{
		int aAssists[CMatchRecentAttackers::MAX_ATTACKERS];
		const int NumAssists = Attackers.CollectAssists(Tick, WindowTicks, KillerParticipantId, VictimParticipantId, aAssists);
		std::vector<int> vAssists(aAssists, aAssists + NumAssists);
		// The order the entries happen to sit in is an implementation detail,
		// every comparison here is about the set of participants.
		std::sort(vAssists.begin(), vAssists.end());
		return vAssists;
	}
}

TEST(MatchRecentAttackers, EmptyGivesNoAssists)
{
	CMatchRecentAttackers Attackers;
	EXPECT_TRUE(Assists(Attackers, 100, 50, 1, 0).empty());
}

TEST(MatchRecentAttackers, KillerAndVictimAreNotCredited)
{
	CMatchRecentAttackers Attackers;
	Attackers.Record(1, 100);
	Attackers.Record(2, 100);
	Attackers.Record(3, 100);
	EXPECT_EQ(Assists(Attackers, 100, 50, 1, 3), std::vector<int>({2}));
}

TEST(MatchRecentAttackers, WindowIsInclusiveAndExpires)
{
	CMatchRecentAttackers Attackers;
	Attackers.Record(1, 100);
	EXPECT_EQ(Assists(Attackers, 150, 50, 7, 0), std::vector<int>({1}));
	EXPECT_TRUE(Assists(Attackers, 151, 50, 7, 0).empty());
}

TEST(MatchRecentAttackers, RepeatedDamageRefreshesInsteadOfDuplicating)
{
	CMatchRecentAttackers Attackers;
	for(int Tick = 100; Tick <= 400; Tick += 100)
		Attackers.Record(1, Tick);
	EXPECT_EQ(Assists(Attackers, 420, 50, 7, 0), std::vector<int>({1}));
}

TEST(MatchRecentAttackers, FreeSlotsAreFilledBeforeAnythingIsEvicted)
{
	CMatchRecentAttackers Attackers;
	for(int i = 0; i < CMatchRecentAttackers::MAX_ATTACKERS; ++i)
		Attackers.Record(i + 1, 100 + i);
	EXPECT_EQ(Assists(Attackers, 110, 50, 7, 0), std::vector<int>({1, 2, 3, 4}));
}

TEST(MatchRecentAttackers, TheLeastRecentAttackerIsEvicted)
{
	CMatchRecentAttackers Attackers;
	for(int i = 0; i < CMatchRecentAttackers::MAX_ATTACKERS; ++i)
		Attackers.Record(i + 1, 100 + i);
	// Refreshing the oldest entry has to move the eviction on to the next one.
	Attackers.Record(1, 200);
	Attackers.Record(9, 210);
	EXPECT_EQ(Assists(Attackers, 220, 200, 7, 0), std::vector<int>({1, 3, 4, 9}));
}

TEST(MatchRecentAttackers, ResetClearsTheWindow)
{
	CMatchRecentAttackers Attackers;
	Attackers.Record(1, 100);
	Attackers.Reset();
	EXPECT_TRUE(Assists(Attackers, 100, 50, 7, 0).empty());
	// Recording again has to work on the cleared state, not on stale entries.
	Attackers.Record(2, 100);
	EXPECT_EQ(Assists(Attackers, 100, 50, 7, 0), std::vector<int>({2}));
}

TEST(MatchRecentAttackers, ParticipantsWithoutAnIdAreIgnored)
{
	CMatchRecentAttackers Attackers;
	Attackers.Record(-1, 100);
	Attackers.Record(5, 100);
	EXPECT_EQ(Assists(Attackers, 100, 50, 7, 0), std::vector<int>({5}));
}
