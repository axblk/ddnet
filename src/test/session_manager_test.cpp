#include "test.h"

#include <engine/client/session.h>

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
	ESessionState State() const override { return m_State; }
	const char *ErrorString() const override { return m_Error.c_str(); }
	void SetState(ESessionState State) override { m_State = State; }
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
	void RequestStop() override { m_State = ESessionState::STOPPING; }
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
