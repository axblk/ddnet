#include "test.h"

#include <engine/client/session.h>
#include <engine/client/session_sources.h>

#include <gtest/gtest.h>

class CTestSessionSource : public IGameSessionSource
{
	ESessionSourceType m_Type;
	ESessionState m_State = ESessionState::OFFLINE;
	std::string m_Error;
	int m_Updates = 0;
	int m_Tick = 0;

public:
	explicit CTestSessionSource(ESessionSourceType Type) :
		m_Type(Type)
	{
	}

	ESessionSourceType Type() const override { return m_Type; }
	std::vector<CStreamId> StreamIds() const override { return {CStreamId(1)}; }
	CStreamId PrimaryStreamId() const override { return CStreamId(1); }
	CStreamId ActiveStreamId() const override { return CStreamId(1); }
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return m_Error.c_str(); }
	bool SetState(ESessionState State) override
	{
		m_State = State;
		return true;
	}
	void Fail(const char *pError) override
	{
		m_Error = pError;
		m_State = ESessionState::ERROR;
	}
	void Update() override
	{
		m_Updates++;
		if(m_State == ESessionState::READY)
			m_Tick++;
		if(m_State == ESessionState::STOPPING)
			m_State = ESessionState::OFFLINE;
	}
	void RequestStop(const char *) override { m_State = ESessionState::STOPPING; }
	int Updates() const { return m_Updates; }
	int Tick() const { return m_Tick; }
	void SetTick(int Tick) { m_Tick = Tick; }
};

TEST(SessionManager, SourcesProgressAndFailIndependently)
{
	CSessionManager Manager;
	auto pNetwork = std::make_unique<CTestSessionSource>(ESessionSourceType::NETWORK);
	auto pDemo = std::make_unique<CTestSessionSource>(ESessionSourceType::DEMO);
	CTestSessionSource *pNetworkRaw = pNetwork.get();
	CTestSessionSource *pDemoRaw = pDemo.get();
	const CSessionId NetworkId = Manager.Create(std::move(pNetwork));
	const CSessionId DemoId = Manager.Create(std::move(pDemo));

	ASSERT_TRUE(NetworkId.IsValid());
	ASSERT_TRUE(DemoId.IsValid());
	EXPECT_NE(NetworkId, DemoId);
	EXPECT_EQ(Manager.FocusedId(), NetworkId);

	pNetworkRaw->SetState(ESessionState::CONNECTING);
	pDemoRaw->SetState(ESessionState::LOADING_MAP);
	Manager.Update();
	pNetworkRaw->SetState(ESessionState::READY);
	pDemoRaw->Fail("broken demo");

	EXPECT_EQ(Manager.Find(NetworkId)->State(), ESessionState::READY);
	EXPECT_EQ(Manager.Find(DemoId)->State(), ESessionState::ERROR);
	EXPECT_STREQ(Manager.Find(DemoId)->Source().ErrorString(), "broken demo");
	EXPECT_EQ(pNetworkRaw->Updates(), 1);
	EXPECT_EQ(pDemoRaw->Updates(), 1);

	EXPECT_TRUE(Manager.Close(NetworkId));
	EXPECT_EQ(Manager.Find(NetworkId)->State(), ESessionState::STOPPING);
	Manager.Update();
	EXPECT_EQ(Manager.Find(NetworkId)->State(), ESessionState::OFFLINE);
	EXPECT_TRUE(Manager.Destroy(NetworkId));
	EXPECT_EQ(Manager.Find(NetworkId), nullptr);
	EXPECT_EQ(Manager.Find(DemoId)->State(), ESessionState::ERROR);
	EXPECT_EQ(Manager.FocusedId(), DemoId);
}

TEST(SessionManager, NetworksProgressWhileDemoLoadsSeeksAndStops)
{
	CSessionManager Manager;
	auto pFirstNetwork = std::make_unique<CTestSessionSource>(ESessionSourceType::NETWORK);
	auto pSecondNetwork = std::make_unique<CTestSessionSource>(ESessionSourceType::NETWORK);
	auto pDemo = std::make_unique<CTestSessionSource>(ESessionSourceType::DEMO);
	CTestSessionSource *pFirstRaw = pFirstNetwork.get();
	CTestSessionSource *pSecondRaw = pSecondNetwork.get();
	CTestSessionSource *pDemoRaw = pDemo.get();
	const CSessionId FirstId = Manager.Create(std::move(pFirstNetwork));
	const CSessionId SecondId = Manager.Create(std::move(pSecondNetwork));
	const CSessionId DemoId = Manager.Create(std::move(pDemo));
	EXPECT_EQ(Manager.SessionIds(), (std::vector<CSessionId>{FirstId, SecondId, DemoId}));
	pFirstRaw->SetTick(100);
	pSecondRaw->SetTick(500);
	pFirstRaw->SetState(ESessionState::READY);
	pSecondRaw->SetState(ESessionState::READY);
	pDemoRaw->SetState(ESessionState::LOADING_MAP);

	Manager.Update();
	EXPECT_EQ(pFirstRaw->Tick(), 101);
	EXPECT_EQ(pSecondRaw->Tick(), 501);
	EXPECT_EQ(pDemoRaw->Tick(), 0);
	EXPECT_TRUE(Manager.SetFocused(DemoId));
	pDemoRaw->SetState(ESessionState::READY);
	pDemoRaw->SetTick(200);
	Manager.Update();
	EXPECT_EQ(pFirstRaw->Tick(), 102);
	EXPECT_EQ(pSecondRaw->Tick(), 502);
	EXPECT_EQ(pDemoRaw->Tick(), 201);

	EXPECT_TRUE(Manager.Close(DemoId));
	Manager.Update();
	EXPECT_EQ(Manager.Find(DemoId)->State(), ESessionState::OFFLINE);
	EXPECT_EQ(Manager.Find(FirstId)->State(), ESessionState::READY);
	EXPECT_EQ(Manager.Find(SecondId)->State(), ESessionState::READY);
	EXPECT_EQ(pFirstRaw->Tick(), 103);
	EXPECT_EQ(pSecondRaw->Tick(), 503);
}

TEST(SessionManager, ConcreteSourceLifecycleRunsThroughManager)
{
	CSessionManager Manager;
	auto pSource = std::make_unique<CNetworkSessionSource>();
	CNetworkSessionSource *pSourceRaw = pSource.get();
	CSessionId Id;
	int Updates = 0;
	bool CloseDuringUpdate = false;
	std::string StopReason;
	pSource->SetLifecycleCallbacks(
		[&]() {
			Updates++;
			if(CloseDuringUpdate)
				Manager.Close(Id, "inside");
		},
		[&StopReason](const char *pReason) { StopReason = pReason ? pReason : ""; });
	Id = Manager.Create(std::move(pSource));
	ASSERT_TRUE(pSourceRaw->SetState(ESessionState::LOADING_MAP));
	ASSERT_TRUE(pSourceRaw->SetState(ESessionState::READY));

	Manager.Update();
	EXPECT_EQ(Updates, 1);
	ASSERT_TRUE(Manager.Close(Id, "done"));
	EXPECT_EQ(pSourceRaw->State(), ESessionState::STOPPING);
	Manager.Update();
	EXPECT_EQ(pSourceRaw->State(), ESessionState::OFFLINE);
	EXPECT_EQ(Updates, 1);
	EXPECT_EQ(StopReason, "done");

	ASSERT_TRUE(pSourceRaw->SetState(ESessionState::LOADING_MAP));
	ASSERT_TRUE(pSourceRaw->SetState(ESessionState::READY));
	CloseDuringUpdate = true;
	Manager.Update();
	EXPECT_EQ(pSourceRaw->State(), ESessionState::OFFLINE);
	EXPECT_EQ(Updates, 2);
	EXPECT_EQ(StopReason, "inside");
}
