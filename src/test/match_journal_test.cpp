#include "test.h"

#include <base/io.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/storage.h>

#include <game/client/match_journal.h>
#include <game/client/match_stats_export.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <cstdlib>

namespace
{
	CStoredMatch StoredMatch(const char *pId, int64_t EndTimeUtc, EMatchCompleteness Completeness, EMatchOutcome Outcome, int64_t Kills)
	{
		CStoredMatch Stored;
		Stored.m_OriginId = "127.0.0.1:8303";
		Stored.m_Source = EMatchReportSource::CLIENT_OBSERVED;
		Stored.m_Completeness = Completeness;
		Stored.m_LocalParticipantId = 0;
		CMatchReport &Report = Stored.m_Report;
		Report.m_MatchId = CalculateUuid(pId);
		Report.m_ModeId = "dm";
		Report.m_MapName = "dm1";
		Report.m_MapSha256 = sha256("map", 3);
		Report.m_StartTimeUtc = EndTimeUtc - 60;
		Report.m_EndTimeUtc = EndTimeUtc;
		Report.m_DurationTicks = 3000;
		Report.m_TickRate = 50;
		Report.m_vParticipants = {{0, std::nullopt, "Local", "", 0, std::nullopt}, {1, std::nullopt, "Other", "", 0, std::nullopt}};
		Report.m_vStandings = {{EMatchSubjectKind::PARTICIPANT, 0, Outcome == EMatchOutcome::WIN ? 1 : 2, Outcome}};
		Report.m_vMetrics = {
			{EMatchSubjectKind::PARTICIPANT, 0, "kills", Kills},
			// only what the local player did counts for the profile
			{EMatchSubjectKind::PARTICIPANT, 1, "kills", 1000}};
		return Stored;
	}

	std::string Json(const CMatchReport &Report)
	{
		CJsonStringWriter Writer;
		MatchReportWriteJson(Writer, Report);
		return Writer.GetOutputString();
	}
}

class MatchJournal : public testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CTestInfo m_TestInfo;
	std::unique_ptr<IStorage> m_pStorage;
	CMatchJournal m_Journal;
	std::string m_Error;

	void SetUp() override
	{
		m_TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_TestInfo.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
		ASSERT_TRUE(m_Journal.Open(m_pStorage.get(), &m_Error)) << m_Error;
	}

	CMatchJournal::EInsertResult Insert(const CStoredMatch &Match, const CStoredMatch *pReplacedObserved = nullptr)
	{
		return m_Journal.Insert(Match, pReplacedObserved, &m_Error);
	}

	std::vector<CMatchHistoryEntry> List()
	{
		std::vector<CMatchHistoryEntry> vEntries;
		EXPECT_TRUE(m_Journal.ListMatches(vEntries, &m_Error)) << m_Error;
		return vEntries;
	}

	int OriginCount() const
	{
		const CSqlite pSqlite = SqliteOpen(m_pStorage.get(), "match-journal.sqlite3");
		if(pSqlite == nullptr)
			return -1;
		const CSqliteStmt pStatement = SqlitePrepare(pSqlite.get(), "SELECT COUNT(*) FROM origins");
		if(pStatement == nullptr || sqlite3_step(pStatement.get()) != SQLITE_ROW)
			return -1;
		return sqlite3_column_int(pStatement.get(), 0);
	}
};

TEST(MatchJournalOpen, ClosesUnsupportedSchema)
{
	CTestInfo TestInfo;
	TestInfo.m_DeleteTestStorageFilesOnSuccess = true;
	const std::unique_ptr<IStorage> pStorage = TestInfo.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	{
		const CSqlite pSqlite = SqliteOpen(pStorage.get(), "match-journal.sqlite3");
		ASSERT_NE(pSqlite, nullptr);
		ASSERT_EQ(sqlite3_exec(pSqlite.get(), "CREATE TABLE journal_meta(id INTEGER PRIMARY KEY, schema_version INTEGER NOT NULL); INSERT INTO journal_meta VALUES(1, 99)", nullptr, nullptr, nullptr), SQLITE_OK);
	}
	CMatchJournal Journal;
	std::string Error;
	EXPECT_FALSE(Journal.Open(pStorage.get(), &Error));
	EXPECT_FALSE(Journal.IsOpen());
	std::vector<CMatchHistoryEntry> vEntries;
	EXPECT_FALSE(Journal.ListMatches(vEntries, &Error));
}

TEST_F(MatchJournal, InsertListLoadAndDuplicate)
{
	CStoredMatch Complete = StoredMatch("complete", 2000000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 5);
	Complete.m_Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "score", 42});
	const CStoredMatch Partial = StoredMatch("partial", 2000100, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::LOSS, 2);
	EXPECT_EQ(Insert(Complete), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	EXPECT_EQ(Insert(Complete), CMatchJournal::EInsertResult::DUPLICATE) << m_Error;
	EXPECT_EQ(Insert(Partial), CMatchJournal::EInsertResult::INSERTED) << m_Error;

	const std::vector<CMatchHistoryEntry> vEntries = List();
	ASSERT_EQ(vEntries.size(), 2u);
	EXPECT_EQ(vEntries[0].m_MatchId, Partial.m_Report.m_MatchId);
	EXPECT_EQ(vEntries[0].m_Completeness, EMatchCompleteness::PARTIAL_SINCE_JOIN);
	EXPECT_EQ(vEntries[0].m_LocalOutcome, EMatchOutcome::LOSS);
	EXPECT_FALSE(vEntries[0].m_LocalScore.has_value());
	EXPECT_EQ(vEntries[1].m_MatchId, Complete.m_Report.m_MatchId);
	EXPECT_EQ(vEntries[1].m_ModeId, "dm");
	EXPECT_EQ(vEntries[1].m_MapName, "dm1");
	EXPECT_EQ(vEntries[1].m_EndTimeUtc, 2000000);
	EXPECT_EQ(vEntries[1].m_DurationTicks, 3000);
	EXPECT_EQ(vEntries[1].m_TickRate, 50);
	EXPECT_EQ(vEntries[1].m_LocalScore, 42);

	CStoredMatch Loaded;
	ASSERT_TRUE(m_Journal.LoadMatch(Complete.m_OriginId.c_str(), Complete.m_Report.m_MatchId, Loaded, &m_Error)) << m_Error;
	EXPECT_EQ(Json(Loaded.m_Report), Json(Complete.m_Report));
	EXPECT_EQ(Loaded.m_OriginId, Complete.m_OriginId);
	EXPECT_EQ(Loaded.m_Source, Complete.m_Source);
	EXPECT_EQ(Loaded.m_Completeness, Complete.m_Completeness);
	EXPECT_EQ(Loaded.m_LocalParticipantId, 0);
	EXPECT_FALSE(m_Journal.LoadMatch("elsewhere:8303", Complete.m_Report.m_MatchId, Loaded, &m_Error));
}

TEST_F(MatchJournal, RejectsMatchesWithoutLocalParticipant)
{
	CStoredMatch Invalid = StoredMatch("invalid-local", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Invalid.m_LocalParticipantId = 5;
	EXPECT_EQ(Insert(Invalid), CMatchJournal::EInsertResult::ERROR);
	Invalid.m_LocalParticipantId.reset();
	EXPECT_EQ(Insert(Invalid), CMatchJournal::EInsertResult::ERROR);
	CStoredMatch InvalidReport = StoredMatch("invalid-report", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	InvalidReport.m_Report.m_TickRate = 0;
	EXPECT_EQ(Insert(InvalidReport), CMatchJournal::EInsertResult::ERROR);
	EXPECT_TRUE(List().empty());
	EXPECT_EQ(OriginCount(), 0);
}

TEST_F(MatchJournal, ProfileCountsCompleteMatchesSinceTheBoundary)
{
	// 2024-03-31 01:00:00 UTC is the European daylight-saving transition
	constexpr int64_t BoundaryUtc = 1711846800;
	ASSERT_EQ(Insert(StoredMatch("old", BoundaryUtc - 1, EMatchCompleteness::COMPLETE, EMatchOutcome::LOSS, 10)), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	ASSERT_EQ(Insert(StoredMatch("boundary", BoundaryUtc, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 5)), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	ASSERT_EQ(Insert(StoredMatch("partial", BoundaryUtc + 100, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::WIN, 100)), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	CStoredMatch OtherMode = StoredMatch("other-mode", BoundaryUtc + 200, EMatchCompleteness::COMPLETE, EMatchOutcome::DRAW, 7);
	OtherMode.m_Report.m_ModeId = "ctf";
	ASSERT_EQ(Insert(OtherMode), CMatchJournal::EInsertResult::INSERTED) << m_Error;

	CMatchProfileFilter Filter;
	Filter.m_SinceUtc = BoundaryUtc;
	Filter.m_ModeId = "dm";
	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile(Filter, Profile, &m_Error)) << m_Error;
	EXPECT_EQ(Profile.m_Matches, 1);
	EXPECT_EQ(Profile.m_Wins, 1);
	EXPECT_EQ(Profile.m_Losses, 0);
	EXPECT_EQ(Profile.m_PlaytimeSeconds, 60);
	ASSERT_EQ(Profile.m_vMetrics.size(), 1u);
	EXPECT_EQ(Profile.m_vMetrics[0].m_ModeId, "dm");
	EXPECT_EQ(Profile.m_vMetrics[0].m_MetricId, "kills");
	EXPECT_EQ(Profile.m_vMetrics[0].m_Value, 5);

	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &m_Error)) << m_Error;
	EXPECT_EQ(Profile.m_Matches, 3);
	EXPECT_EQ(Profile.m_Draws, 1);
	EXPECT_EQ(Profile.m_vMetrics.size(), 2u);
}

TEST_F(MatchJournal, ProfileAggregatesByMetric)
{
	CStoredMatch First = StoredMatch("first", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 3);
	First.m_Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "best_spree", 4, EMatchMetricAggregation::MAXIMUM});
	First.m_Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "map_rank", 1, EMatchMetricAggregation::MATCH_ONLY});
	First.m_Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "playtime_ticks", 500});
	CStoredMatch Second = StoredMatch("second", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::LOSS, 5);
	Second.m_Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "best_spree", 2, EMatchMetricAggregation::MAXIMUM});
	ASSERT_EQ(Insert(First), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	ASSERT_EQ(Insert(Second), CMatchJournal::EInsertResult::INSERTED) << m_Error;

	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &m_Error)) << m_Error;
	// the played time where it was counted, the duration of the match where not
	EXPECT_EQ(Profile.m_PlaytimeSeconds, 10 + 60);
	const auto Find = [&](const char *pMetricId) {
		return std::find_if(Profile.m_vMetrics.begin(), Profile.m_vMetrics.end(), [&](const CMatchMetricAggregate &Metric) { return Metric.m_MetricId == pMetricId; });
	};
	ASSERT_NE(Find("kills"), Profile.m_vMetrics.end());
	EXPECT_EQ(Find("kills")->m_Value, 8);
	ASSERT_NE(Find("best_spree"), Profile.m_vMetrics.end());
	EXPECT_EQ(Find("best_spree")->m_Value, 4);
	EXPECT_EQ(Find("best_spree")->m_Aggregation, EMatchMetricAggregation::MAXIMUM);
	EXPECT_EQ(Find("map_rank"), Profile.m_vMetrics.end());
}

TEST_F(MatchJournal, TheReportThatKnowsMoreTakesThePlace)
{
	CStoredMatch Snapshot = StoredMatch("round", 2000, EMatchCompleteness::ABORTED, EMatchOutcome::DNF, 1);
	Snapshot.m_Source = EMatchReportSource::SERVER_SNAPSHOT;
	Snapshot.m_Report.m_Termination = EMatchTermination::ABORTED;
	CStoredMatch Final = StoredMatch("round", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	Final.m_Source = EMatchReportSource::SERVER_REPORT;

	ASSERT_EQ(Insert(Snapshot), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	ASSERT_EQ(Insert(Final), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	std::vector<CMatchHistoryEntry> vEntries = List();
	ASSERT_EQ(vEntries.size(), 1u);
	EXPECT_EQ(vEntries[0].m_Source, EMatchReportSource::SERVER_REPORT);
	EXPECT_EQ(vEntries[0].m_LocalOutcome, EMatchOutcome::WIN);
	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &m_Error)) << m_Error;
	ASSERT_EQ(Profile.m_vMetrics.size(), 1u);
	EXPECT_EQ(Profile.m_vMetrics[0].m_Value, 2);

	// a snapshot arriving after the final report leaves it alone
	EXPECT_EQ(Insert(Snapshot), CMatchJournal::EInsertResult::DUPLICATE) << m_Error;
	vEntries = List();
	ASSERT_EQ(vEntries.size(), 1u);
	EXPECT_EQ(vEntries[0].m_Source, EMatchReportSource::SERVER_REPORT);

	EXPECT_TRUE(MatchSupersedes(EMatchReportSource::SERVER_REPORT, EMatchCompleteness::ABORTED, EMatchReportSource::SERVER_SNAPSHOT, EMatchCompleteness::COMPLETE));
	EXPECT_TRUE(MatchSupersedes(EMatchReportSource::SERVER_SNAPSHOT, EMatchCompleteness::ABORTED, EMatchReportSource::CLIENT_OBSERVED, EMatchCompleteness::COMPLETE));
	EXPECT_TRUE(MatchSupersedes(EMatchReportSource::CLIENT_OBSERVED, EMatchCompleteness::COMPLETE, EMatchReportSource::CLIENT_OBSERVED, EMatchCompleteness::PARTIAL_SINCE_JOIN));
	EXPECT_FALSE(MatchSupersedes(EMatchReportSource::SERVER_REPORT, EMatchCompleteness::COMPLETE, EMatchReportSource::SERVER_REPORT, EMatchCompleteness::COMPLETE));
}

TEST_F(MatchJournal, ServerReportReplacesTheObservedRound)
{
	CStoredMatch Observed = StoredMatch("observed", 2000, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::WIN, 1);
	Observed.m_OriginId = "observed.example:8303";
	CStoredMatch Server = StoredMatch("server", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	Server.m_OriginId = "server.example:8303";
	Server.m_Source = EMatchReportSource::SERVER_REPORT;
	ASSERT_EQ(Insert(Observed), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	EXPECT_EQ(Insert(Server, &Observed), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	std::vector<CMatchHistoryEntry> vEntries = List();
	ASSERT_EQ(vEntries.size(), 1u);
	EXPECT_EQ(vEntries[0].m_MatchId, Server.m_Report.m_MatchId);
	EXPECT_EQ(OriginCount(), 1);

	// the server's report is there already, the observation still goes
	CStoredMatch SecondObserved = StoredMatch("second-observed", 2001, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::LOSS, 1);
	ASSERT_EQ(Insert(SecondObserved), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	EXPECT_EQ(Insert(Server, &SecondObserved), CMatchJournal::EInsertResult::DUPLICATE) << m_Error;
	vEntries = List();
	ASSERT_EQ(vEntries.size(), 1u);
	EXPECT_EQ(vEntries[0].m_MatchId, Server.m_Report.m_MatchId);

	// only what the client observed is dropped for it
	EXPECT_EQ(Insert(StoredMatch("third", 2002, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1), &Server), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	EXPECT_EQ(List().size(), 2u);
}

TEST_F(MatchJournal, RollsBackAFailedReplacement)
{
	const CStoredMatch Observed = StoredMatch("rollback-observed", 2000, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::WIN, 1);
	CStoredMatch Server = StoredMatch("rollback-server", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	Server.m_Source = EMatchReportSource::SERVER_REPORT;
	ASSERT_EQ(Insert(Observed), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	{
		const CSqlite pSqlite = SqliteOpen(m_pStorage.get(), "match-journal.sqlite3");
		ASSERT_NE(pSqlite, nullptr);
		ASSERT_EQ(sqlite3_exec(pSqlite.get(), "CREATE TRIGGER fail_replace BEFORE DELETE ON matches BEGIN SELECT RAISE(ABORT, 'test failure'); END", nullptr, nullptr, nullptr), SQLITE_OK);
	}
	EXPECT_EQ(Insert(Server, &Observed), CMatchJournal::EInsertResult::ERROR);
	const std::vector<CMatchHistoryEntry> vEntries = List();
	ASSERT_EQ(vEntries.size(), 1u);
	EXPECT_EQ(vEntries[0].m_MatchId, Observed.m_Report.m_MatchId);

	// and one that fails halfway through its metrics
	{
		const CSqlite pSqlite = SqliteOpen(m_pStorage.get(), "match-journal.sqlite3");
		ASSERT_NE(pSqlite, nullptr);
		ASSERT_EQ(sqlite3_exec(pSqlite.get(), "CREATE TRIGGER fail_metric BEFORE INSERT ON local_metrics BEGIN SELECT RAISE(ABORT, 'test failure'); END", nullptr, nullptr, nullptr), SQLITE_OK);
	}
	EXPECT_EQ(Insert(StoredMatch("rollback-metric", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1)), CMatchJournal::EInsertResult::ERROR);
	EXPECT_EQ(List().size(), 1u);
}

TEST_F(MatchJournal, DeleteInfoAndOrigins)
{
	CStoredMatch First = StoredMatch("first", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::DRAW, 1);
	First.m_OriginId = "first.example:8303";
	CStoredMatch Second = StoredMatch("second", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	Second.m_OriginId = "second.example:8303";
	ASSERT_EQ(Insert(First), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	ASSERT_EQ(Insert(Second), CMatchJournal::EInsertResult::INSERTED) << m_Error;
	EXPECT_EQ(OriginCount(), 2);

	CMatchJournalInfo Info;
	ASSERT_TRUE(m_Journal.Info(Info, &m_Error)) << m_Error;
	EXPECT_EQ(Info.m_NumMatches, 2);
	EXPECT_EQ(Info.m_OldestMatchUtc, 2000);
	EXPECT_GT(Info.m_DatabaseSize, 0);

	EXPECT_TRUE(m_Journal.DeleteMatch(First.m_OriginId.c_str(), First.m_Report.m_MatchId, &m_Error)) << m_Error;
	ASSERT_TRUE(m_Journal.Info(Info, &m_Error)) << m_Error;
	EXPECT_EQ(Info.m_NumMatches, 1);
	EXPECT_EQ(Info.m_OldestMatchUtc, 3000);
	EXPECT_EQ(OriginCount(), 1);
	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &m_Error)) << m_Error;
	EXPECT_EQ(Profile.m_Matches, 1);
	ASSERT_EQ(Profile.m_vMetrics.size(), 1u);
	EXPECT_EQ(Profile.m_vMetrics[0].m_Value, 2);

	EXPECT_TRUE(m_Journal.DeleteAll(&m_Error)) << m_Error;
	ASSERT_TRUE(m_Journal.Info(Info, &m_Error)) << m_Error;
	EXPECT_EQ(Info.m_NumMatches, 0);
	EXPECT_FALSE(Info.m_OldestMatchUtc.has_value());
	EXPECT_EQ(OriginCount(), 0);
}

TEST_F(MatchJournal, ExportsTheStoredMatch)
{
	CStoredMatch Stored = StoredMatch("export", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Stored.m_OriginId = "stats.example:8303";
	Stored.m_Source = EMatchReportSource::SERVER_REPORT;
	Stored.m_Report.m_vTeams.push_back({5, "Blue, the \"good\" team"});
	Stored.m_Report.m_vMetrics.push_back({EMatchSubjectKind::TEAM, 5, "score", 7});

	CJsonStringWriter Writer;
	MatchStatsExportJson(Writer, Stored);
	const std::string Exported = Writer.GetOutputString();
	const std::unique_ptr<json_value, decltype(&json_value_free)> pJson(JsonParse(Exported.c_str(), Exported.size()), json_value_free);
	ASSERT_NE(pJson, nullptr);
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "origin_id")), Stored.m_OriginId.c_str());
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "source")), "server_report");
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "completeness")), "complete");
	EXPECT_EQ(json_int_get(json_object_get(pJson.get(), "local_participant_id")), 0);
	// the report is an object, not a string holding one
	const json_value *pReport = json_object_get(pJson.get(), "report");
	ASSERT_NE(pReport, &json_value_none);
	EXPECT_EQ(pReport->type, json_object);
	EXPECT_STREQ(json_string_get(json_object_get(pReport, "mode_id")), "dm");

	IOHANDLE File = m_pStorage->OpenFile("match-export.csv", IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_NE(File, nullptr);
	MatchStatsExportCsv(File, Stored);
	ASSERT_EQ(io_close(File), 0);
	char *pCsv = m_pStorage->ReadFileStr("match-export.csv", IStorage::TYPE_SAVE);
	ASSERT_NE(pCsv, nullptr);
	std::string Csv = pCsv;
	free(pCsv);
#if defined(CONF_FAMILY_WINDOWS)
	// CsvWrite ends its lines the way the platform does
	Csv.erase(std::remove(Csv.begin(), Csv.end(), '\r'), Csv.end());
#endif
	EXPECT_EQ(Csv.rfind("origin_id,match_id,mode_id,map_name,end_time_utc,duration_ticks,tick_rate,source,completeness,local_participant_id,subject_kind,subject_id,metric_id,value,aggregation\n", 0), 0u);
	EXPECT_NE(Csv.find("stats.example:8303,"), std::string::npos);
	EXPECT_NE(Csv.find(",dm,dm1,2000,3000,50,server_report,complete,0,participant,0,kills,1,sum\n"), std::string::npos);
	EXPECT_NE(Csv.find(",team,5,score,7,sum\n"), std::string::npos);
	EXPECT_EQ(std::count(Csv.begin(), Csv.end(), '\n'), 4);
}
