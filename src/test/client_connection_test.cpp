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

	EXPECT_TRUE(Router.Set(Second, Third, EStreamInputPolicy::DIRECT));
	EXPECT_EQ(Router.NumRoutes(), 3U);
	EXPECT_EQ(Router.Find(Second)->m_Source, Third);
	EXPECT_TRUE(Router.Remove(Primary));
	EXPECT_EQ(Router.Find(Primary), nullptr);
	EXPECT_FALSE(Router.Set({}, Primary, EStreamInputPolicy::DIRECT));
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
