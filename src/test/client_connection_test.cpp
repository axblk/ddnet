#include "test.h"

#include <engine/client/connection.h>
#include <engine/client/session_sources.h>

#include <game/client/input_policy.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>

TEST(ClientConnection, UpdateTiming)
{
	// Connections are too large for the stack of a Windows test runner
	const std::unique_ptr<CConnection> pConnection = std::make_unique<CConnection>();
	CConnection &Connection = *pConnection;
	Connection.m_PrevGameTick = 100;
	Connection.m_CurGameTick = 101;

	EXPECT_EQ(Connection.UpdateTiming(2010, 2070, 50, 1000), 104);
	EXPECT_FLOAT_EQ(Connection.m_GameIntraTick, 0.5f);
	EXPECT_FLOAT_EQ(Connection.m_GameTickTime, 0.01f);
	EXPECT_FLOAT_EQ(Connection.m_GameIntraTickSincePrev, 0.5f);
	EXPECT_FLOAT_EQ(Connection.m_PredIntraTick, 0.5f);
}

TEST(ClientConnection, ServerRequestedConnectSurvivesTheSessionStop)
{
	const std::unique_ptr<CNetworkSessionSource> pSource = std::make_unique<CNetworkSessionSource>();
	CNetworkSessionSource &Source = *pSource;
	std::string Address;
	std::string Password;
	EXPECT_FALSE(Source.ConsumePendingConnect(Address, Password));

	Source.m_Password = "secret";
	Source.ScheduleServerConnect("127.0.0.1:8304", "secret");
	Source.ResetAfterDisconnect("Timeout", 3, 5, 200, 10);
	ASSERT_TRUE(Source.ConsumePendingConnect(Address, Password));
	EXPECT_EQ(Address, "127.0.0.1:8304");
	EXPECT_EQ(Password, "secret");
	EXPECT_EQ(Source.m_ReconnectTime, 0);
	EXPECT_FALSE(Source.ConsumePendingConnect(Address, Password));

	Source.ScheduleServerConnect("127.0.0.1:8305", "other");
	Source.CancelReconnect();
	EXPECT_FALSE(Source.ConsumePendingConnect(Address, Password));
}

TEST(ClientConnection, SessionSourceRejectsInvalidTransitions)
{
	const std::unique_ptr<CNetworkSessionSource> pSource = std::make_unique<CNetworkSessionSource>();
	CNetworkSessionSource &Source = *pSource;
	EXPECT_FALSE(Source.SetState(ESessionState::READY));
	EXPECT_EQ(Source.State(), ESessionState::OFFLINE);
	EXPECT_TRUE(Source.SetState(ESessionState::CONNECTING));
	EXPECT_FALSE(Source.SetState(ESessionState::READY));
	EXPECT_EQ(Source.State(), ESessionState::CONNECTING);
	EXPECT_TRUE(Source.SetState(ESessionState::LOADING_MAP));
	EXPECT_TRUE(Source.SetState(ESessionState::READY));
	EXPECT_TRUE(Source.SetState(ESessionState::STOPPING));
	EXPECT_FALSE(Source.SetState(ESessionState::CONNECTING));
	EXPECT_TRUE(Source.SetState(ESessionState::OFFLINE));
}

TEST(ClientConnection, HammerInputCadenceAndRelease)
{
	CInputRoute Route;
	Route.m_Policy = EInputPolicy::HAMMER;
	for(int Tick = 0; Tick < 50; Tick++)
		EXPECT_EQ(Route.AdvanceHammer(), Tick % 25 == 0);

	Route.m_HammerInput.m_Fire = 5;
	CNetObj_PlayerInput TargetInput = {};
	TargetInput.m_Fire = 9;
	Route.FinishHammering(TargetInput);
	EXPECT_EQ(TargetInput.m_Fire, 6);
	EXPECT_EQ(Route.m_HammerCounter, 0U);
	Route.FinishHammering(TargetInput);
	EXPECT_EQ(TargetInput.m_Fire, 6);
}
