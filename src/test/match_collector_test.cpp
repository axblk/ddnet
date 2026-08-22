#include "test.h"

#include <game/client/game_state.h>
#include <game/client/session_context.h>
#include <game/gamecore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace
{
	const char *const MODE_ID = "vanilla.dm@ddnet.org";

	void SetupClient(CGameState::CClientSnapshot &Client, int ClientId, int Team, int Score, const char *pName, const char *pClan)
	{
		Client.m_Active = true;
		Client.m_HasPlayerInfo = true;
		Client.m_PlayerInfo.m_ClientId = ClientId;
		Client.m_PlayerInfo.m_Team = Team;
		Client.m_PlayerInfo.m_Score = Score;
		Client.m_HasClientInfo = true;
		StrToInts(Client.m_ClientInfo.m_aName, std::size(Client.m_ClientInfo.m_aName), pName);
		StrToInts(Client.m_ClientInfo.m_aClan, std::size(Client.m_ClientInfo.m_aClan), pClan);
	}

	std::vector<CGameState::CEntitySnapshot> GameDataEntity(int TeamscoreRed, int TeamscoreBlue)
	{
		CNetObj_GameData Data = {};
		Data.m_TeamscoreRed = TeamscoreRed;
		Data.m_TeamscoreBlue = TeamscoreBlue;
		Data.m_FlagCarrierRed = FLAG_MISSING;
		Data.m_FlagCarrierBlue = FLAG_MISSING;
		const unsigned char *pBytes = reinterpret_cast<const unsigned char *>(&Data);
		CGameState::CEntitySnapshot Entity;
		Entity.m_Id = 0;
		Entity.m_Type = NETOBJTYPE_GAMEDATA;
		Entity.m_vData.assign(pBytes, pBytes + sizeof(Data));
		return {std::move(Entity)};
	}

	CObservedMatchMetadata Metadata(EMatchTermination Termination)
	{
		CObservedMatchMetadata Metadata;
		Metadata.m_OriginId = "127.0.0.1:8303";
		Metadata.m_ModeId = MODE_ID;
		Metadata.m_MapName = "dm1";
		Metadata.m_MapSha256 = sha256("map", 3);
		Metadata.m_EndTimeUtc = 2000000;
		Metadata.m_TickRate = 50;
		Metadata.m_Termination = Termination;
		return Metadata;
	}

	const CMatchParticipant *Participant(const CMatchReport &Report, int ParticipantId)
	{
		const auto It = std::find_if(Report.m_vParticipants.begin(), Report.m_vParticipants.end(), [ParticipantId](const CMatchParticipant &Item) { return Item.m_ParticipantId == ParticipantId; });
		return It == Report.m_vParticipants.end() ? nullptr : &*It;
	}

	std::optional<int64_t> Metric(const CMatchReport &Report, EMatchSubjectKind SubjectKind, int SubjectId, const char *pSuffix)
	{
		const std::string MetricId = std::string(MODE_ID) + "/" + pSuffix;
		const auto It = std::find_if(Report.m_vMetrics.begin(), Report.m_vMetrics.end(), [&](const CMatchMetric &Item) { return Item.m_SubjectKind == SubjectKind && Item.m_SubjectId == SubjectId && Item.m_MetricId == MetricId; });
		return It == Report.m_vMetrics.end() ? std::nullopt : std::optional<int64_t>(It->m_Value);
	}

	std::optional<CMatchStanding> Standing(const CMatchReport &Report, EMatchSubjectKind SubjectKind, int SubjectId)
	{
		const auto It = std::find_if(Report.m_vStandings.begin(), Report.m_vStandings.end(), [&](const CMatchStanding &Item) { return Item.m_SubjectKind == SubjectKind && Item.m_SubjectId == SubjectId; });
		return It == Report.m_vStandings.end() ? std::nullopt : std::optional<CMatchStanding>(*It);
	}
}

TEST(MatchCollector, ObservedMatchCarriesEveryClientThatPlayed)
{
	CGameSessionContext Session(CSessionId(1), "dm1", EGameProtocol::SIX, {CStreamId(1)});
	CGameState &State = *Session.GameStates().FindByStream(CStreamId(1));
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 100;
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	SetupClient(aClients[5], 5, TEAM_RED, 10, "local", "clan");
	aClients[5].m_PlayerInfo.m_Local = 1;
	SetupClient(aClients[7], 7, TEAM_RED, 4, "other", "");
	SetupClient(aClients[9], 9, TEAM_SPECTATORS, 0, "watcher", "");
	aClients[7].m_HasCharacter = true;
	aClients[7].m_Character.m_Weapon = WEAPON_GRENADE;
	State.ApplySnapshotData(150, 4, aClients, &GameInfo);
	Session.Stats().UpdateSnapshot(State, 150);

	CNetMsg_Sv_KillMsg Kill = {};
	Kill.m_Killer = 5;
	Kill.m_Victim = 7;
	Kill.m_Weapon = WEAPON_LASER;
	Session.Stats().HandleMessage(State, false, NETMSGTYPE_SV_KILLMSG, &Kill);

	// The client that lost leaves the server halfway through the match, which
	// takes its identity out of the game state but not out of the report.
	aClients[7] = {};
	State.ApplySnapshotData(175, 3, aClients, &GameInfo);
	Session.Stats().UpdateSnapshot(State, 175);

	GameInfo.m_GameStateFlags = GAMESTATEFLAG_GAMEOVER;
	State.ApplySnapshotData(200, 3, std::move(aClients), &GameInfo);
	ASSERT_TRUE(Session.Stats().UpdateSnapshot(State, 200));
	std::string Error;
	ASSERT_TRUE(Session.Stats().FinalizeObservedMatch(Metadata(EMatchTermination::COMPLETED), State, 200, &Error)) << Error;

	const CStoredMatch &Stored = *Session.Stats().LatestMatch();
	const CMatchReport &Report = Stored.m_Report;
	ASSERT_EQ(Report.m_vParticipants.size(), 2U);
	EXPECT_EQ(Stored.m_LocalParticipantId, 5);
	EXPECT_TRUE(Report.m_vTeams.empty());

	const CMatchParticipant *pLocal = Participant(Report, 5);
	ASSERT_NE(pLocal, nullptr);
	EXPECT_EQ(pLocal->m_DisplayName, "local");
	EXPECT_EQ(pLocal->m_Clan, "clan");
	EXPECT_EQ(pLocal->m_JoinedTick, 50);
	EXPECT_FALSE(pLocal->m_LeftTick.has_value());
	EXPECT_FALSE(pLocal->m_TeamId.has_value());
	EXPECT_FALSE(pLocal->m_Bot);

	const CMatchParticipant *pLeaver = Participant(Report, 7);
	ASSERT_NE(pLeaver, nullptr);
	EXPECT_EQ(pLeaver->m_DisplayName, "other");
	EXPECT_EQ(pLeaver->m_JoinedTick, 50);
	ASSERT_TRUE(pLeaver->m_LeftTick.has_value());
	EXPECT_EQ(*pLeaver->m_LeftTick, 75);

	EXPECT_EQ(Participant(Report, 9), nullptr);

	EXPECT_EQ(Metric(Report, EMatchSubjectKind::PARTICIPANT, 5, "kills"), 1);
	EXPECT_EQ(Metric(Report, EMatchSubjectKind::PARTICIPANT, 5, "score"), 10);
	EXPECT_EQ(Metric(Report, EMatchSubjectKind::PARTICIPANT, 7, "deaths"), 1);
	EXPECT_EQ(Metric(Report, EMatchSubjectKind::PARTICIPANT, 7, "score"), 4);
	const std::string HoldingSuffix = "weapon_" + std::to_string(WEAPON_GRENADE) + "_deaths_holding";
	EXPECT_EQ(Metric(Report, EMatchSubjectKind::PARTICIPANT, 7, HoldingSuffix.c_str()), 1);
	EXPECT_FALSE(Metric(Report, EMatchSubjectKind::PARTICIPANT, 5, HoldingSuffix.c_str()).has_value());

	const std::optional<CMatchStanding> LocalStanding = Standing(Report, EMatchSubjectKind::PARTICIPANT, 5);
	ASSERT_TRUE(LocalStanding.has_value());
	EXPECT_EQ(LocalStanding->m_Rank, 1);
	EXPECT_EQ(LocalStanding->m_Outcome, EMatchOutcome::WIN);
	const std::optional<CMatchStanding> LeaverStanding = Standing(Report, EMatchSubjectKind::PARTICIPANT, 7);
	ASSERT_TRUE(LeaverStanding.has_value());
	EXPECT_EQ(LeaverStanding->m_Rank, 2);
	EXPECT_EQ(LeaverStanding->m_Outcome, EMatchOutcome::LOSS);
}

TEST(MatchCollector, ObservedTeamMatchPlacesEveryClientByTeamScore)
{
	CGameSessionContext Session(CSessionId(1), "ctf1", EGameProtocol::SIX, {CStreamId(1)});
	CGameState &State = *Session.GameStates().FindByStream(CStreamId(1));
	CNetObj_GameInfo GameInfo = {};
	GameInfo.m_RoundStartTick = 0;
	GameInfo.m_GameFlags = GAMEFLAG_TEAMS | GAMEFLAG_FLAGS;
	std::array<CGameState::CClientSnapshot, MAX_CLIENTS> aClients = {};
	SetupClient(aClients[0], 0, TEAM_BLUE, 1, "local", "");
	aClients[0].m_PlayerInfo.m_Local = 1;
	SetupClient(aClients[1], 1, TEAM_RED, 5, "red", "");
	std::vector<CGameState::CEntitySnapshot> vEntities = GameDataEntity(3, 1);
	State.ApplySnapshotData(100, 3, aClients, &GameInfo, &vEntities);
	Session.Stats().UpdateSnapshot(State, 100);

	GameInfo.m_GameStateFlags = GAMESTATEFLAG_GAMEOVER;
	vEntities = GameDataEntity(3, 1);
	State.ApplySnapshotData(200, 3, aClients, &GameInfo, &vEntities);
	ASSERT_TRUE(Session.Stats().UpdateSnapshot(State, 200));
	std::string Error;
	ASSERT_TRUE(Session.Stats().FinalizeObservedMatch(Metadata(EMatchTermination::COMPLETED), State, 200, &Error)) << Error;

	const CMatchReport &Report = Session.Stats().LatestMatch()->m_Report;
	ASSERT_EQ(Report.m_vTeams.size(), 2U);
	ASSERT_EQ(Report.m_vParticipants.size(), 2U);
	EXPECT_EQ(Participant(Report, 0)->m_TeamId, TEAM_BLUE);
	EXPECT_EQ(Participant(Report, 1)->m_TeamId, TEAM_RED);
	EXPECT_EQ(Metric(Report, EMatchSubjectKind::TEAM, TEAM_RED, "score"), 3);
	EXPECT_EQ(Metric(Report, EMatchSubjectKind::TEAM, TEAM_BLUE, "score"), 1);

	// The losing team took the better individual score with it, which must not
	// move it ahead of the team that won the match.
	EXPECT_EQ(Standing(Report, EMatchSubjectKind::TEAM, TEAM_RED)->m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(Standing(Report, EMatchSubjectKind::PARTICIPANT, 1)->m_Rank, 1);
	EXPECT_EQ(Standing(Report, EMatchSubjectKind::PARTICIPANT, 0)->m_Rank, 2);
	EXPECT_EQ(Standing(Report, EMatchSubjectKind::PARTICIPANT, 0)->m_Outcome, EMatchOutcome::LOSS);
}
