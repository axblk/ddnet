#include <game/server/mode/ready_check.h>

#include <gtest/gtest.h>

static constexpr int TICK_SPEED = 50;

TEST(ReadyCheck, EverybodyIsReadyWithoutAWait)
{
	CReadyCheck Check;
	EXPECT_FALSE(Check.IsWaiting());
	EXPECT_EQ(Check.Wait(), CReadyCheck::EWait::NONE);
	EXPECT_TRUE(Check.IsReady(0));
	EXPECT_TRUE(Check.IsReady(MAX_CLIENTS - 1));
	CClientMask Participants;
	Participants.set(0).set(5);
	EXPECT_TRUE(Check.NotReady(Participants).none());
}

TEST(ReadyCheck, AWaitBeginsWithNobodyReady)
{
	CReadyCheck Check;
	Check.SetReady(3, true);
	Check.Begin(CReadyCheck::EWait::START, 100);
	EXPECT_TRUE(Check.IsWaiting());
	EXPECT_EQ(Check.Wait(), CReadyCheck::EWait::START);
	EXPECT_EQ(Check.WaitStartTick(), 100);
	EXPECT_FALSE(Check.IsReady(3));

	CClientMask Participants;
	Participants.set(1).set(3);
	EXPECT_EQ(Check.NotReady(Participants), Participants);
	Check.SetReady(1, true);
	EXPECT_TRUE(Check.IsReady(1));
	EXPECT_EQ(Check.NotReady(Participants), CClientMask().set(3));
	// who does not take part does not count
	Check.SetReady(3, true);
	EXPECT_TRUE(Check.NotReady(Participants).none());
	EXPECT_TRUE(Check.NotReady(CClientMask().set(7)).any());

	// the next wait starts over
	Check.Begin(CReadyCheck::EWait::RESUME, 200);
	EXPECT_EQ(Check.Wait(), CReadyCheck::EWait::RESUME);
	EXPECT_EQ(Check.NotReady(Participants), Participants);

	Check.End();
	EXPECT_FALSE(Check.IsWaiting());
	EXPECT_TRUE(Check.IsReady(3));
	// and nobody is left ready from the last one
	Check.Begin(CReadyCheck::EWait::START, 300);
	EXPECT_FALSE(Check.IsReady(1));
}

TEST(ReadyCheck, SetAllReady)
{
	CReadyCheck Check;
	Check.Begin(CReadyCheck::EWait::RESUME, 0);
	Check.SetAllReady();
	EXPECT_TRUE(Check.NotReady(CClientMask().set()).none());
}

TEST(ReadyCheck, APlayerChangesTheirMindOnceASecond)
{
	CReadyCheck Check;
	EXPECT_TRUE(Check.TakeChange(2, 1000, TICK_SPEED));
	EXPECT_FALSE(Check.TakeChange(2, 1000 + TICK_SPEED - 1, TICK_SPEED));
	// the others are not held back
	EXPECT_TRUE(Check.TakeChange(4, 1001, TICK_SPEED));
	EXPECT_TRUE(Check.TakeChange(2, 1000 + TICK_SPEED, TICK_SPEED));
	EXPECT_FALSE(Check.TakeChange(2, 1000 + TICK_SPEED + 1, TICK_SPEED));
}

TEST(ReadyCheck, APlayerWhoLeavesIsForgotten)
{
	CReadyCheck Check;
	Check.Begin(CReadyCheck::EWait::START, 0);
	Check.SetReady(9, true);
	EXPECT_TRUE(Check.TakeChange(9, 500, TICK_SPEED));
	Check.Forget(9);
	EXPECT_FALSE(Check.IsReady(9));
	// whoever gets the slot next may change right away
	EXPECT_TRUE(Check.TakeChange(9, 501, TICK_SPEED));
}
