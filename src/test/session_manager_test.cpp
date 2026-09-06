#include "test.h"

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
