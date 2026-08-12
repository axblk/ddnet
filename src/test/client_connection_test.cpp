#include "test.h"

#include <engine/client/connection.h>
#include <engine/client/session_sources.h>

#include <game/client/input_policy.h>
#include <game/client/local_player_profile.h>

#include <gtest/gtest.h>

TEST(ClientConnection, ResetGameplayIsLocal)
{
	CConnection ResetConnection;
	CConnection UntouchedConnection;

	ResetConnection.m_aInputs[0].m_Tick = 123;
	ResetConnection.m_CurrentInput = 17;
	ResetConnection.m_SnapshotParts = 3;
	ResetConnection.m_ReceivedSnapshots = 4;
	ResetConnection.m_SnapshotIncomingDataSize = 5;
	ResetConnection.m_AckGameTick = 6;
	ResetConnection.m_CurrentRecvTick = 7;
	ResetConnection.m_PrevGameTick = 8;
	ResetConnection.m_CurGameTick = 9;
	ResetConnection.m_PredTick = 10;

	UntouchedConnection.m_aInputs[0].m_Tick = 321;
	UntouchedConnection.m_ReceivedSnapshots = 11;
	UntouchedConnection.m_CurGameTick = 12;

	ResetConnection.ResetGameplay();

	EXPECT_EQ(ResetConnection.m_aInputs[0].m_Tick, -1);
	EXPECT_EQ(ResetConnection.m_CurrentInput, 0);
	EXPECT_EQ(ResetConnection.m_SnapshotParts, 0U);
	EXPECT_EQ(ResetConnection.m_ReceivedSnapshots, 0);
	EXPECT_EQ(ResetConnection.m_SnapshotIncomingDataSize, 0);
	EXPECT_EQ(ResetConnection.m_AckGameTick, -1);
	EXPECT_EQ(ResetConnection.m_CurrentRecvTick, 0);
	EXPECT_EQ(ResetConnection.m_PrevGameTick, 0);
	EXPECT_EQ(ResetConnection.m_CurGameTick, 0);
	EXPECT_EQ(ResetConnection.m_PredTick, 0);

	EXPECT_EQ(UntouchedConnection.m_aInputs[0].m_Tick, 321);
	EXPECT_EQ(UntouchedConnection.m_ReceivedSnapshots, 11);
	EXPECT_EQ(UntouchedConnection.m_CurGameTick, 12);
}

TEST(ClientConnection, TimingUpdatesAreLocal)
{
	CConnection First;
	CConnection Second;
	First.m_PrevGameTick = 100;
	First.m_CurGameTick = 101;
	Second.m_PrevGameTick = 200;
	Second.m_CurGameTick = 201;

	EXPECT_EQ(First.UpdateTiming(2010, 2070, 50, 1000), 104);
	EXPECT_FLOAT_EQ(First.m_GameIntraTick, 0.5f);
	EXPECT_FLOAT_EQ(First.m_GameTickTime, 0.01f);
	EXPECT_FLOAT_EQ(First.m_GameIntraTickSincePrev, 0.5f);
	EXPECT_FLOAT_EQ(First.m_PredIntraTick, 0.5f);

	EXPECT_EQ(Second.UpdateTiming(4015, 4075, 50, 1000), 204);
	EXPECT_FLOAT_EQ(Second.m_GameIntraTick, 0.75f);
	EXPECT_FLOAT_EQ(Second.m_PredIntraTick, 0.75f);
	EXPECT_FLOAT_EQ(First.m_GameIntraTick, 0.5f);
	EXPECT_FLOAT_EQ(First.m_PredIntraTick, 0.5f);
}

TEST(ClientConnection, DynamicStreamsKeepStableIdsAndStorage)
{
	CNetworkSessionSource Source;
	ASSERT_EQ(Source.NumStreams(), 2U);
	const CStreamId PrimaryId = Source.StreamIdAt(0);
	const CStreamId RemovedId = Source.StreamIdAt(1);
	const CStreamId ThirdId = Source.CreateStream();
	ASSERT_TRUE(ThirdId.IsValid());
	ASSERT_NE(Source.Connection(PrimaryId), nullptr);
	ASSERT_NE(Source.Connection(ThirdId), nullptr);

	Source.Connection(PrimaryId)->m_CurGameTick = 100;
	Source.Connection(ThirdId)->m_CurGameTick = 300;
	CConnection *pThirdConnection = Source.Connection(ThirdId);
	EXPECT_TRUE(Source.DestroyStream(RemovedId));
	EXPECT_EQ(Source.NumStreams(), 2U);
	EXPECT_EQ(Source.Connection(ThirdId), pThirdConnection);
	EXPECT_EQ(Source.Connection(PrimaryId)->m_CurGameTick, 100);
	EXPECT_EQ(Source.Connection(ThirdId)->m_CurGameTick, 300);
	EXPECT_EQ(Source.Connection(RemovedId), nullptr);

	const CStreamId FourthId = Source.CreateStream();
	EXPECT_GT(FourthId.Value(), ThirdId.Value());
	EXPECT_EQ(Source.NumStreams(), 3U);
}

TEST(ClientConnection, DemoStateDoesNotAliasNetworkMain)
{
	CNetworkSessionSource Network;
	CDemoSessionSource Demo(false, [] {});
	CConnection &NetworkMain = Network.ConnectionAt(IClient::CONN_MAIN);
	NetworkMain.m_CurGameTick = 123;
	CSnapshotStorage::CHolder NetworkSnapshot;
	NetworkMain.m_apSnapshots[IClient::SNAP_CURRENT] = &NetworkSnapshot;
	Network.SetSixup(false);
	Demo.SetSixup(true);
	str_copy(Network.ServerInfo().m_aMap, "network");
	str_copy(Demo.ServerInfo().m_aMap, "demo");

	Demo.PrepareSnapshots();
	ASSERT_NE(Demo.Connection().m_apSnapshots[IClient::SNAP_CURRENT], nullptr);
	EXPECT_NE(Demo.Connection().m_apSnapshots[IClient::SNAP_CURRENT], NetworkMain.m_apSnapshots[IClient::SNAP_CURRENT]);
	EXPECT_NE(&Demo.SnapshotDelta(false), &Network.SnapshotDelta(false));
	EXPECT_NE(&Demo.SnapshotDelta(true), &Network.SnapshotDelta(true));
	Demo.Connection().m_CurGameTick = 456;
	Demo.Connection().ResetSnapshots();
	Demo.ResetMetadata();

	EXPECT_EQ(NetworkMain.m_CurGameTick, 123);
	EXPECT_EQ(NetworkMain.m_apSnapshots[IClient::SNAP_CURRENT], &NetworkSnapshot);
	EXPECT_FALSE(Network.IsSixup());
	EXPECT_STREQ(Network.ServerInfo().m_aMap, "network");
	EXPECT_FALSE(Demo.IsSixup());
	EXPECT_EQ(Demo.ServerInfo().m_aMap[0], '\0');
}

TEST(ClientConnection, StreamValueStorageGrowsWithoutChangingExistingValues)
{
	CStreamStorage<int> Values(2);
	Values[0] = 100;
	Values[1] = 200;
	Values[4] = 500;

	EXPECT_EQ(Values.size(), 5U);
	EXPECT_EQ(Values[0], 100);
	EXPECT_EQ(Values[1], 200);
	EXPECT_EQ(Values[2], 0);
	EXPECT_EQ(Values[4], 500);
	Values.Fill(-1);
	EXPECT_EQ(Values[0], -1);
	EXPECT_EQ(Values[4], -1);
}

TEST(ClientConnection, InputRoutesUseExplicitSourceAndTargetStreams)
{
	CStreamInputRouter Router;
	const CStreamId Primary(1);
	const CStreamId Second(2);
	const CStreamId Third(3);
	EXPECT_TRUE(Router.Set(Primary, Primary, EStreamInputPolicy::DIRECT));
	EXPECT_TRUE(Router.Set(Second, Primary, EStreamInputPolicy::COPY_MOVES));
	EXPECT_TRUE(Router.Set(Third, Primary, EStreamInputPolicy::HAMMER));
	ASSERT_NE(Router.Find(Second), nullptr);
	EXPECT_EQ(Router.Find(Second)->m_Source, Primary);
	EXPECT_EQ(Router.Find(Second)->m_Policy, EStreamInputPolicy::COPY_MOVES);
	EXPECT_EQ(Router.Find(Third)->m_Policy, EStreamInputPolicy::HAMMER);
	Router.Find(Second)->m_HammerInput.m_Fire = 3;
	Router.Find(Third)->m_HammerInput.m_Fire = 7;
	Router.Find(Third)->m_HammerCounter = 24;

	EXPECT_TRUE(Router.Set(Second, Third, EStreamInputPolicy::COPY_MOVES));
	EXPECT_EQ(Router.NumRoutes(), 3U);
	EXPECT_EQ(Router.Find(Second)->m_Source, Third);
	EXPECT_EQ(Router.Find(Second)->m_HammerInput.m_Fire, 3);
	EXPECT_EQ(Router.Find(Third)->m_HammerInput.m_Fire, 7);
	EXPECT_EQ(Router.Find(Third)->m_HammerCounter, 24U);
	EXPECT_FALSE(Router.Set(Second, Third, EStreamInputPolicy::DIRECT));
	EXPECT_TRUE(Router.Remove(Primary));
	EXPECT_EQ(Router.Find(Primary), nullptr);
	EXPECT_NE(Router.Find(Second), nullptr);
	EXPECT_EQ(Router.Find(Third), nullptr);
	EXPECT_TRUE(Router.Remove(Third));
	EXPECT_EQ(Router.Find(Second), nullptr);
	EXPECT_FALSE(Router.Set({}, Primary, EStreamInputPolicy::DIRECT));
	EXPECT_TRUE(Router.Set(Primary, Primary, EStreamInputPolicy::DIRECT));
	Router.Reset();
	EXPECT_EQ(Router.NumRoutes(), 0U);
}

TEST(ClientConnection, HammerInputCadenceAndReleaseAreRouteLocal)
{
	CStreamInputRouter Router;
	const CStreamId Primary(1);
	const CStreamId Second(2);
	const CStreamId Third(3);
	ASSERT_TRUE(Router.Set(Second, Primary, EStreamInputPolicy::HAMMER));
	ASSERT_TRUE(Router.Set(Third, Primary, EStreamInputPolicy::HAMMER));
	CStreamInputRoute *pSecond = Router.Find(Second);
	CStreamInputRoute *pThird = Router.Find(Third);
	ASSERT_NE(pSecond, nullptr);
	ASSERT_NE(pThird, nullptr);

	for(int Tick = 0; Tick < 50; Tick++)
		EXPECT_EQ(pSecond->AdvanceHammer(), Tick % 25 == 0);
	EXPECT_EQ(pThird->m_HammerCounter, 0U);

	pSecond->m_HammerInput.m_Fire = 5;
	CNetObj_PlayerInput TargetInput = {};
	TargetInput.m_Fire = 9;
	pSecond->FinishHammering(TargetInput);
	EXPECT_EQ(TargetInput.m_Fire, 6);
	EXPECT_EQ(pSecond->m_HammerCounter, 0U);
	pSecond->FinishHammering(TargetInput);
	EXPECT_EQ(TargetInput.m_Fire, 6);
}

TEST(ClientConnection, LocalProfilesBindToThreeStreams)
{
	CLocalPlayerProfileBindings Profiles;
	CLocalPlayerProfile Primary;
	Primary.m_Name = "primary";
	CLocalPlayerProfile Second;
	Second.m_Name = "second";
	CLocalPlayerProfile Third;
	Third.m_Name = "third";
	EXPECT_TRUE(Profiles.Set(CStreamId(1), Primary));
	EXPECT_TRUE(Profiles.Set(CStreamId(2), Second));
	EXPECT_TRUE(Profiles.Set(CStreamId(3), Third));
	EXPECT_EQ(Profiles.NumProfiles(), 3U);
	EXPECT_EQ(Profiles.Find(CStreamId(1))->m_Name, "primary");
	EXPECT_EQ(Profiles.Find(CStreamId(3))->m_Name, "third");

	Second.m_Name = "replacement";
	EXPECT_TRUE(Profiles.Set(CStreamId(2), Second));
	EXPECT_EQ(Profiles.NumProfiles(), 3U);
	EXPECT_EQ(Profiles.Find(CStreamId(2))->m_Name, "replacement");
	EXPECT_TRUE(Profiles.Remove(CStreamId(1)));
	EXPECT_EQ(Profiles.Find(CStreamId(1)), nullptr);
	EXPECT_EQ(Profiles.Find(CStreamId(3))->m_Name, "third");
}
