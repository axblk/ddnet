#include "test.h"

#include <engine/client/local_seats.h>
#include <engine/client/session.h>

#include <gtest/gtest.h>

class CTestSessionSource : public CSessionSource
{
	ESessionSourceType m_Type;

public:
	explicit CTestSessionSource(ESessionSourceType Type) :
		m_Type(Type)
	{
	}

	ESessionSourceType Type() const override { return m_Type; }
};

TEST(SessionManager, CreateAndFocus)
{
	CSessionManager Manager;
	const CSessionId NetworkId = Manager.Create(std::make_unique<CTestSessionSource>(ESessionSourceType::NETWORK));
	const CSessionId DemoId = Manager.Create(std::make_unique<CTestSessionSource>(ESessionSourceType::DEMO));

	ASSERT_TRUE(NetworkId.IsValid());
	ASSERT_TRUE(DemoId.IsValid());
	EXPECT_NE(NetworkId, DemoId);
	EXPECT_EQ(Manager.FocusedId(), NetworkId);
	EXPECT_EQ(Manager.Find(DemoId)->Type(), ESessionSourceType::DEMO);
	EXPECT_EQ(Manager.Find(CSessionId()), nullptr);

	EXPECT_TRUE(Manager.SetFocused(DemoId));
	EXPECT_EQ(Manager.Focused(), Manager.Find(DemoId));
	EXPECT_FALSE(Manager.SetFocused(CSessionId(1000)));
	EXPECT_EQ(Manager.FocusedId(), DemoId);
}

TEST(SessionManager, StopKeepsReasonUntilTaken)
{
	CTestSessionSource Source(ESessionSourceType::NETWORK);
	Source.RequestStop("ignored");
	EXPECT_EQ(Source.State(), ESessionState::OFFLINE);

	ASSERT_TRUE(Source.SetState(ESessionState::CONNECTING));
	Source.RequestStop("kicked");
	EXPECT_EQ(Source.State(), ESessionState::STOPPING);
	EXPECT_EQ(Source.TakeStopReason(), "kicked");
	EXPECT_EQ(Source.TakeStopReason(), "");

	Source.Fail("broken");
	EXPECT_EQ(Source.State(), ESessionState::ERROR);
	EXPECT_STREQ(Source.ErrorString(), "broken");
	EXPECT_TRUE(Source.SetState(ESessionState::OFFLINE));
	EXPECT_STREQ(Source.ErrorString(), "");
}

TEST(LocalSeats, TheDummyPlaysInASessionOfItsOwn)
{
	const CSessionId NetworkId(1);
	const CSessionId DemoId(2);
	const CSessionId DummyId(3);
	const CLocalSeats Seats(NetworkId, DummyId);

	EXPECT_EQ(Seats.SessionOf(CLocalSeats::SEAT_PLAYER), NetworkId);
	EXPECT_EQ(Seats.SessionOf(CLocalSeats::SEAT_DUMMY), DummyId);
	EXPECT_EQ(Seats.SeatOf(NetworkId), CLocalSeats::SEAT_PLAYER);
	EXPECT_EQ(Seats.SeatOf(DummyId), CLocalSeats::SEAT_DUMMY);
	EXPECT_EQ(Seats.SeatOf(DemoId), CLocalSeats::NO_SEAT);
	EXPECT_EQ(Seats.SeatOf(CSessionId()), CLocalSeats::NO_SEAT);

	// A program without a connection has no seats at all.
	const CLocalSeats NoSeats;
	EXPECT_EQ(NoSeats.SeatOf(CSessionId()), CLocalSeats::NO_SEAT);
	EXPECT_EQ(NoSeats.SeatOf(DemoId), CLocalSeats::NO_SEAT);
	EXPECT_EQ(NoSeats.InputSessionId(DemoId, CLocalSeats::SEAT_DUMMY), DemoId);
}

TEST(LocalSeats, InputGoesToTheDummySwitchOnlyWhileTheServerIsInFocus)
{
	const CSessionId NetworkId(1);
	const CSessionId DemoId(2);
	const CSessionId DummyId(3);
	const CLocalSeats Seats(NetworkId, DummyId);

	// The session that gets the input is also the only one predicted.
	EXPECT_EQ(Seats.InputSessionId(NetworkId, CLocalSeats::SEAT_PLAYER), NetworkId);
	EXPECT_EQ(Seats.InputSessionId(NetworkId, CLocalSeats::SEAT_DUMMY), DummyId);
	EXPECT_EQ(Seats.InputSessionId(DemoId, CLocalSeats::SEAT_PLAYER), DemoId);
	EXPECT_EQ(Seats.InputSessionId(DemoId, CLocalSeats::SEAT_DUMMY), DemoId);
	EXPECT_EQ(Seats.InputSessionId(CSessionId(), CLocalSeats::SEAT_DUMMY), CSessionId());
}
