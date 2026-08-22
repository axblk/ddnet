#include "test.h"

#include <base/io.h>

#include <engine/shared/json.h>
#include <engine/storage.h>

#include <game/client/match_journal.h>
#include <game/client/match_report_assembler.h>
#include <game/client/match_stats_export.h>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace
{
	CStoredMatch StoredMatch(const char *pId, int64_t EndTimeUtc, EMatchCompleteness Completeness, EMatchOutcome Outcome, int64_t Kills, std::optional<int64_t> PlaytimeTicks = std::nullopt, std::optional<int64_t> Score = std::nullopt)
	{
		CStoredMatch Stored;
		Stored.m_OriginId = "127.0.0.1:8303";
		Stored.m_Source = EMatchReportSource::CLIENT_OBSERVED;
		Stored.m_Completeness = Completeness;
		Stored.m_LocalParticipantId = 0;
		CMatchReport &Report = Stored.m_Report;
		Report.m_MatchId = CalculateUuid(pId);
		Report.m_ModeId = "vanilla.dm@ddnet.org";
		Report.m_MapName = "dm1";
		Report.m_MapSha256 = sha256("map", 3);
		Report.m_StartTimeUtc = EndTimeUtc - 60;
		Report.m_EndTimeUtc = EndTimeUtc;
		Report.m_DurationTicks = 3000;
		Report.m_TickRate = 50;
		Report.m_UnrankedReason = "client_observed";
		Report.m_vParticipants = {{0, std::nullopt, "Local", "", 0, std::nullopt, false}};
		Report.m_vStandings = {{EMatchSubjectKind::PARTICIPANT, 0, Outcome == EMatchOutcome::WIN ? 1 : 2, Outcome}};
		Report.m_vMetrics = {{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/kills", Kills}};
		if(PlaytimeTicks.has_value())
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/playtime_ticks", *PlaytimeTicks});
		if(Score.has_value())
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/score", *Score});
		std::string Error;
		EXPECT_TRUE(MatchReportToJson(Report, Stored.m_RawReport, &Error)) << Error;
		return Stored;
	}

	void AddMetric(CStoredMatch &Stored, EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pSuffix, int64_t Value, EMatchMetricAggregation Aggregation)
	{
		Stored.m_Report.m_vMetrics.push_back({SubjectKind, SubjectId, Stored.m_Report.m_ModeId + "/" + pSuffix, Value, Aggregation});
		std::string Error;
		ASSERT_TRUE(MatchReportToJson(Stored.m_Report, Stored.m_RawReport, &Error)) << Error;
	}
}

class MatchJournal : public testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CTestInfo m_TestInfo;
	std::unique_ptr<IStorage> m_pStorage;
	CMatchJournal m_Journal;

	void SetUp() override
	{
		m_pStorage = m_TestInfo.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
		std::string Error;
		ASSERT_TRUE(m_Journal.Open(m_pStorage.get(), &Error)) << Error;
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
}

TEST(MatchJournalOpen, MigratesAggregationSchema)
{
	CTestInfo TestInfo;
	const std::unique_ptr<IStorage> pStorage = TestInfo.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	{
		const CSqlite pSqlite = SqliteOpen(pStorage.get(), "match-journal.sqlite3");
		ASSERT_NE(pSqlite, nullptr);
		ASSERT_EQ(sqlite3_exec(pSqlite.get(), "CREATE TABLE journal_meta(id INTEGER PRIMARY KEY, schema_version INTEGER NOT NULL); INSERT INTO journal_meta VALUES(1, 1); CREATE TABLE metrics(origin_id INTEGER NOT NULL, match_id TEXT NOT NULL, subject_kind INTEGER NOT NULL, subject_id INTEGER, metric_id TEXT NOT NULL, value INTEGER NOT NULL)", nullptr, nullptr, nullptr), SQLITE_OK);
	}
	CMatchJournal Journal;
	std::string Error;
	ASSERT_TRUE(Journal.Open(pStorage.get(), &Error)) << Error;
	const CSqlite pSqlite = SqliteOpen(pStorage.get(), "match-journal.sqlite3");
	ASSERT_NE(pSqlite, nullptr);
	const CSqliteStmt pStatement = SqlitePrepare(pSqlite.get(), "SELECT aggregation FROM metrics");
	EXPECT_NE(pStatement, nullptr);
}

TEST_F(MatchJournal, InsertListLoadAndDuplicate)
{
	const CStoredMatch Complete = StoredMatch("complete@ddnet.org", 2000000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 5);
	const CStoredMatch Partial = StoredMatch("partial@ddnet.org", 2000100, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::LOSS, 2);
	std::string Error;
	EXPECT_EQ(m_Journal.Insert(Complete, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_EQ(m_Journal.Insert(Complete, &Error), CMatchJournal::EInsertResult::DUPLICATE) << Error;
	EXPECT_EQ(m_Journal.Insert(Partial, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	CMatchHistoryFilter Filter;
	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches(Filter, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 2U);
	EXPECT_EQ(vEntries[0].m_MatchId, Partial.m_Report.m_MatchId);
	EXPECT_EQ(vEntries[0].m_Completeness, EMatchCompleteness::PARTIAL_SINCE_JOIN);

	Filter.m_IncludeIncomplete = false;
	ASSERT_TRUE(m_Journal.ListMatches(Filter, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 1U);
	EXPECT_EQ(vEntries[0].m_MatchId, Complete.m_Report.m_MatchId);

	CStoredMatch Loaded;
	ASSERT_TRUE(m_Journal.LoadMatch(Complete.m_OriginId.c_str(), Complete.m_Report.m_MatchId, Loaded, &Error)) << Error;
	EXPECT_EQ(Loaded.m_RawReport, Complete.m_RawReport);
	EXPECT_EQ(Loaded.m_LocalParticipantId, 0);
}

TEST_F(MatchJournal, ProfileUsesCompleteMatchesAndUtcBoundary)
{
	// 2024-03-31 01:00:00 UTC is the European daylight-saving transition.
	constexpr int64_t BoundaryUtc = 1711846800;
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(StoredMatch("old@ddnet.org", BoundaryUtc - 1, EMatchCompleteness::COMPLETE, EMatchOutcome::LOSS, 10), &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	ASSERT_EQ(m_Journal.Insert(StoredMatch("boundary@ddnet.org", BoundaryUtc, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 5), &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	ASSERT_EQ(m_Journal.Insert(StoredMatch("partial-profile@ddnet.org", BoundaryUtc + 100, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::WIN, 100), &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	CMatchProfileFilter Filter;
	Filter.m_SinceUtc = BoundaryUtc;
	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile(Filter, Profile, &Error)) << Error;
	EXPECT_EQ(Profile.m_Matches, 1);
	EXPECT_EQ(Profile.m_Wins, 1);
	EXPECT_EQ(Profile.m_Losses, 0);
	EXPECT_EQ(Profile.m_PlaytimeSeconds, 60);
	ASSERT_EQ(Profile.m_vMetrics.size(), 1U);
	EXPECT_EQ(Profile.m_vMetrics[0].m_Value, 5);
}

TEST_F(MatchJournal, ProfileUsesParticipantPlaytimeWithLegacyFallback)
{
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(StoredMatch("playtime@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1, 500), &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	ASSERT_EQ(m_Journal.Insert(StoredMatch("legacy-playtime@ddnet.org", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::LOSS, 1), &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &Error)) << Error;
	EXPECT_EQ(Profile.m_Matches, 2);
	EXPECT_EQ(Profile.m_PlaytimeSeconds, 70);
}

TEST_F(MatchJournal, ProfileUsesRegisteredAggregationSemantics)
{
	CStoredMatch First = StoredMatch("profile-aggregation-first@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 3);
	AddMetric(First, EMatchSubjectKind::PARTICIPANT, 0, "best_spree", 4, EMatchMetricAggregation::MAXIMUM);
	AddMetric(First, EMatchSubjectKind::PARTICIPANT, 0, "sudden_death", 1, EMatchMetricAggregation::MATCH_ONLY);
	AddMetric(First, EMatchSubjectKind::PARTICIPANT, 0, "custom_peak", 9, EMatchMetricAggregation::MAXIMUM);
	CStoredMatch Second = StoredMatch("profile-aggregation-second@ddnet.org", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::LOSS, 5);
	AddMetric(Second, EMatchSubjectKind::PARTICIPANT, 0, "best_spree", 2, EMatchMetricAggregation::MAXIMUM);
	AddMetric(Second, EMatchSubjectKind::PARTICIPANT, 0, "sudden_death", 0, EMatchMetricAggregation::MATCH_ONLY);
	AddMetric(Second, EMatchSubjectKind::PARTICIPANT, 0, "custom_peak", 3, EMatchMetricAggregation::MAXIMUM);
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(First, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	ASSERT_EQ(m_Journal.Insert(Second, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &Error)) << Error;
	const auto FindMetric = [&](const char *pSuffix) {
		return std::find_if(Profile.m_vMetrics.begin(), Profile.m_vMetrics.end(), [&](const CMatchMetricAggregate &Metric) { return Metric.m_MetricId == First.m_Report.m_ModeId + "/" + pSuffix; });
	};
	const auto Kills = FindMetric("kills");
	ASSERT_NE(Kills, Profile.m_vMetrics.end());
	EXPECT_EQ(Kills->m_Value, 8);
	const auto BestSpree = FindMetric("best_spree");
	ASSERT_NE(BestSpree, Profile.m_vMetrics.end());
	EXPECT_EQ(BestSpree->m_Value, 4);
	EXPECT_EQ(BestSpree->m_Aggregation, EMatchMetricAggregation::MAXIMUM);
	const auto CustomPeak = FindMetric("custom_peak");
	ASSERT_NE(CustomPeak, Profile.m_vMetrics.end());
	EXPECT_EQ(CustomPeak->m_Value, 9);
	EXPECT_EQ(FindMetric("sudden_death"), Profile.m_vMetrics.end());
}

TEST_F(MatchJournal, HistoryIncludesLocalModeScore)
{
	std::string Error;
	CStoredMatch Stored = StoredMatch("history-score@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 7, std::nullopt, 42);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "deaths", 3, EMatchMetricAggregation::SUM);
	ASSERT_EQ(m_Journal.Insert(Stored, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches({}, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 1U);
	EXPECT_EQ(vEntries[0].m_LocalScore, 42);
	// The list shows a kill/death column without loading a single full report
	EXPECT_EQ(vEntries[0].m_LocalKills, 7);
	EXPECT_EQ(vEntries[0].m_LocalDeaths, 3);
}

TEST_F(MatchJournal, ExportsPreserveEnvelopeAndMetricSubjects)
{
	CStoredMatch Stored = StoredMatch("export-envelope@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Stored.m_OriginId = "stats.example:8303";
	Stored.m_Source = EMatchReportSource::SERVER_REPORT;
	Stored.m_Report.m_vTeams.push_back({5, "Blue"});
	AddMetric(Stored, EMatchSubjectKind::TEAM, 5, "team_total", 7, EMatchMetricAggregation::SUM);

	const std::string Json = MatchStatsExportJson(Stored);
	const std::unique_ptr<json_value, decltype(&json_value_free)> pJson(JsonParse(Json.c_str(), Json.size()), json_value_free);
	ASSERT_NE(pJson, nullptr);
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "origin_id")), Stored.m_OriginId.c_str());
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "source")), "server_report");
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "completeness")), "complete");
	EXPECT_EQ(json_int_get(json_object_get(pJson.get(), "local_participant_id")), 0);
	EXPECT_STREQ(json_string_get(json_object_get(pJson.get(), "raw_report")), Stored.m_RawReport.c_str());

	IOHANDLE File = m_pStorage->OpenFile("match-export.csv", IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_NE(File, nullptr);
	MatchStatsExportCsv(File, Stored);
	ASSERT_EQ(io_close(File), 0);
	char *pCsv = m_pStorage->ReadFileStr("match-export.csv", IStorage::TYPE_SAVE);
	ASSERT_NE(pCsv, nullptr);
	const std::string Csv = pCsv;
	free(pCsv);
	EXPECT_NE(Csv.find("origin_id,match_id,mode_id,map_name,end_time_utc,duration_ticks,tick_rate,source,completeness,local_participant_id,subject_kind,subject_id,metric_id,value"), std::string::npos);
	EXPECT_NE(Csv.find("stats.example:8303"), std::string::npos);
	EXPECT_NE(Csv.find("server_report,complete,0,participant,0,vanilla.dm@ddnet.org/kills,1"), std::string::npos);
	EXPECT_NE(Csv.find("server_report,complete,0,team,5,vanilla.dm@ddnet.org/team_total,7"), std::string::npos);
}

TEST_F(MatchJournal, DeleteAndInfo)
{
	const CStoredMatch First = StoredMatch("delete-first@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::DRAW, 1);
	const CStoredMatch Second = StoredMatch("delete-second@ddnet.org", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(First, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	ASSERT_EQ(m_Journal.Insert(Second, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	CMatchJournalInfo Info;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	EXPECT_EQ(Info.m_NumMatches, 2);
	EXPECT_EQ(Info.m_OldestMatchUtc, 2000);
	EXPECT_GT(Info.m_DatabaseSize, 0);

	EXPECT_TRUE(m_Journal.DeleteMatch(First.m_OriginId.c_str(), First.m_Report.m_MatchId, &Error)) << Error;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	EXPECT_EQ(Info.m_NumMatches, 1);
	EXPECT_EQ(Info.m_OldestMatchUtc, 3000);

	ASSERT_EQ(m_Journal.Insert(First, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_TRUE(m_Journal.DeleteMatch(Second.m_OriginId.c_str(), Second.m_Report.m_MatchId, &Error)) << Error;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	EXPECT_EQ(Info.m_NumMatches, 1);
	EXPECT_EQ(Info.m_OldestMatchUtc, 2000);

	EXPECT_TRUE(m_Journal.DeleteAll(&Error)) << Error;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	EXPECT_EQ(Info.m_NumMatches, 0);
	EXPECT_FALSE(Info.m_OldestMatchUtc.has_value());
}

TEST_F(MatchJournal, DeletesPruneOrphanOrigins)
{
	CStoredMatch First = StoredMatch("origin-first@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	First.m_OriginId = "first.example:8303";
	CStoredMatch Second = StoredMatch("origin-second@ddnet.org", 3000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Second.m_OriginId = "second.example:8303";
	CStoredMatch Third = StoredMatch("origin-third@ddnet.org", 4000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Third.m_OriginId = "third.example:8303";
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(First, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	ASSERT_EQ(m_Journal.Insert(Second, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_EQ(OriginCount(), 2);

	EXPECT_TRUE(m_Journal.DeleteMatch(First.m_OriginId.c_str(), First.m_Report.m_MatchId, &Error)) << Error;
	EXPECT_EQ(OriginCount(), 1);
	ASSERT_EQ(m_Journal.Insert(Third, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_EQ(OriginCount(), 2);

	EXPECT_TRUE(m_Journal.DeleteMatch(Third.m_OriginId.c_str(), Third.m_Report.m_MatchId, &Error)) << Error;
	EXPECT_EQ(OriginCount(), 1);
	EXPECT_TRUE(m_Journal.DeleteAll(&Error)) << Error;
	EXPECT_EQ(OriginCount(), 0);
}

TEST_F(MatchJournal, ReplacesObservedAtomicallyIncludingDuplicateAuthority)
{
	CStoredMatch Observed = StoredMatch("replace-observed@ddnet.org", 2000, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::WIN, 1);
	Observed.m_OriginId = "observed.example:8303";
	CStoredMatch Authority = StoredMatch("replace-authority@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	Authority.m_OriginId = "authority.example:8303";
	Authority.m_Source = EMatchReportSource::SERVER_REPORT;
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(Observed, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_EQ(m_Journal.InsertReplacingObserved(Authority, Observed.m_OriginId.c_str(), Observed.m_Report.m_MatchId, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;

	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches({}, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 1U);
	EXPECT_EQ(vEntries[0].m_MatchId, Authority.m_Report.m_MatchId);
	EXPECT_EQ(vEntries[0].m_Source, EMatchReportSource::SERVER_REPORT);
	EXPECT_EQ(OriginCount(), 1);

	CStoredMatch SecondObserved = StoredMatch("replace-second-observed@ddnet.org", 2001, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::LOSS, 1);
	SecondObserved.m_OriginId = "second-observed.example:8303";
	ASSERT_EQ(m_Journal.Insert(SecondObserved, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_EQ(m_Journal.InsertReplacingObserved(Authority, SecondObserved.m_OriginId.c_str(), SecondObserved.m_Report.m_MatchId, &Error), CMatchJournal::EInsertResult::DUPLICATE) << Error;
	ASSERT_TRUE(m_Journal.ListMatches({}, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 1U);
	EXPECT_EQ(vEntries[0].m_MatchId, Authority.m_Report.m_MatchId);
	EXPECT_EQ(OriginCount(), 1);
}

TEST_F(MatchJournal, StoresDisconnectSnapshotWithoutDowngradingFinalReport)
{
	CStoredMatch Observed = StoredMatch("disconnect-observed@ddnet.org", 2000, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::DNF, 1);
	Observed.m_OriginId = "server.example:8303";
	CStoredMatch Snapshot = StoredMatch("disconnect-snapshot@ddnet.org", 2000, EMatchCompleteness::ABORTED, EMatchOutcome::DNF, 2);
	Snapshot.m_OriginId = "server.example:8303";
	Snapshot.m_Source = EMatchReportSource::SERVER_SNAPSHOT;
	Snapshot.m_Report.m_Termination = EMatchTermination::ABORTED;
	std::string Error;
	ASSERT_TRUE(MatchReportToJson(Snapshot.m_Report, Snapshot.m_RawReport, &Error)) << Error;
	std::string Packed;
	ASSERT_TRUE(MatchReportToPacked(Snapshot.m_Report, Packed, &Error)) << Error;
	ASSERT_EQ(m_Journal.Insert(Observed, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	CLiveStatsAssembler LiveStats;
	const int NumChunks = (Packed.size() + MatchReportTransportLimits::MAX_CHUNK_SIZE - 1) / MatchReportTransportLimits::MAX_CHUNK_SIZE;
	ASSERT_TRUE(LiveStats.Start(Snapshot.m_Report.m_MatchId, 0, Snapshot.m_Report.m_ReportSchemaVersion, 0, true, Packed.size(), NumChunks, &Error)) << Error;
	for(int Chunk = 0; Chunk < NumChunks; ++Chunk)
	{
		const size_t Offset = Chunk * MatchReportTransportLimits::MAX_CHUNK_SIZE;
		const size_t Size = std::min(Packed.size() - Offset, static_cast<size_t>(MatchReportTransportLimits::MAX_CHUNK_SIZE));
		ASSERT_TRUE(LiveStats.AddChunk(Snapshot.m_Report.m_MatchId, 0, Chunk, Packed.data() + Offset, Size, &Error)) << Error;
	}
	ASSERT_TRUE(LiveStats.Finish(Snapshot.m_Report.m_MatchId, 0, sha256(Packed.data(), Packed.size()), Snapshot.m_OriginId.c_str(), &Error)) << Error;
	ASSERT_TRUE(PersistLiveStatsSnapshotOnDisconnect(m_Journal, ESessionSourceType::NETWORK, true, true, false, Observed, LiveStats, &Error)) << Error;

	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches({}, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 1u);
	EXPECT_EQ(vEntries[0].m_Source, EMatchReportSource::SERVER_SNAPSHOT);
	EXPECT_EQ(vEntries[0].m_Completeness, EMatchCompleteness::ABORTED);

	CStoredMatch Final = Snapshot;
	Final.m_Source = EMatchReportSource::SERVER_REPORT;
	Final.m_Completeness = EMatchCompleteness::COMPLETE;
	Final.m_Report.m_Termination = EMatchTermination::COMPLETED;
	ASSERT_TRUE(MatchReportToJson(Final.m_Report, Final.m_RawReport, &Error)) << Error;
	ASSERT_TRUE(m_Journal.DeleteMatch(Snapshot.m_OriginId.c_str(), Snapshot.m_Report.m_MatchId, &Error)) << Error;
	ASSERT_EQ(m_Journal.Insert(Final, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	EXPECT_TRUE(PersistLiveStatsSnapshotOnDisconnect(m_Journal, ESessionSourceType::NETWORK, true, true, true, Observed, LiveStats, &Error)) << Error;
	CStoredMatch Loaded;
	ASSERT_TRUE(m_Journal.LoadMatch(Final.m_OriginId.c_str(), Final.m_Report.m_MatchId, Loaded, &Error)) << Error;
	EXPECT_EQ(Loaded.m_Source, EMatchReportSource::SERVER_REPORT);
	EXPECT_EQ(Loaded.m_Completeness, EMatchCompleteness::COMPLETE);
}

TEST_F(MatchJournal, RollsBackFailedObservedReplacement)
{
	CStoredMatch Observed = StoredMatch("rollback-observed@ddnet.org", 2000, EMatchCompleteness::PARTIAL_SINCE_JOIN, EMatchOutcome::WIN, 1);
	Observed.m_OriginId = "observed.example:8303";
	CStoredMatch Authority = StoredMatch("rollback-authority@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 2);
	Authority.m_OriginId = "authority.example:8303";
	Authority.m_Source = EMatchReportSource::SERVER_REPORT;
	std::string Error;
	ASSERT_EQ(m_Journal.Insert(Observed, &Error), CMatchJournal::EInsertResult::INSERTED) << Error;
	const CSqlite pSqlite = SqliteOpen(m_pStorage.get(), "match-journal.sqlite3");
	ASSERT_NE(pSqlite, nullptr);
	ASSERT_EQ(sqlite3_exec(pSqlite.get(), "CREATE TRIGGER fail_replace BEFORE DELETE ON matches BEGIN SELECT RAISE(ABORT, 'test failure'); END", nullptr, nullptr, nullptr), SQLITE_OK);

	EXPECT_EQ(m_Journal.InsertReplacingObserved(Authority, Observed.m_OriginId.c_str(), Observed.m_Report.m_MatchId, &Error), CMatchJournal::EInsertResult::ERROR);
	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches({}, vEntries, &Error)) << Error;
	ASSERT_EQ(vEntries.size(), 1U);
	EXPECT_EQ(vEntries[0].m_MatchId, Observed.m_Report.m_MatchId);
	EXPECT_EQ(vEntries[0].m_Source, EMatchReportSource::CLIENT_OBSERVED);
	EXPECT_EQ(OriginCount(), 1);
}

TEST_F(MatchJournal, RejectsInvalidMatchWithoutPartialInsert)
{
	CStoredMatch Invalid = StoredMatch("invalid-local@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Invalid.m_LocalParticipantId = 5;
	std::string Error;
	EXPECT_EQ(m_Journal.Insert(Invalid, &Error), CMatchJournal::EInsertResult::ERROR);
	CMatchJournalInfo Info;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	EXPECT_EQ(Info.m_NumMatches, 0);
}

TEST_F(MatchJournal, RollsBackFailedChildInsert)
{
	const CSqlite pSqlite = SqliteOpen(m_pStorage.get(), "match-journal.sqlite3");
	ASSERT_NE(pSqlite, nullptr);
	char *pErrorMessage = nullptr;
	const int Result = sqlite3_exec(pSqlite.get(), "CREATE TRIGGER fail_participant BEFORE INSERT ON participants BEGIN SELECT RAISE(ABORT, 'test failure'); END", nullptr, nullptr, &pErrorMessage);
	const std::string TriggerError = pErrorMessage == nullptr ? "" : pErrorMessage;
	sqlite3_free(pErrorMessage);
	ASSERT_EQ(Result, SQLITE_OK) << TriggerError;

	std::string Error;
	EXPECT_EQ(m_Journal.Insert(StoredMatch("rollback@ddnet.org", 2000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1), &Error), CMatchJournal::EInsertResult::ERROR);
	CMatchJournalInfo Info;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	EXPECT_EQ(Info.m_NumMatches, 0);
	EXPECT_EQ(OriginCount(), 0);
}

TEST_F(MatchJournal, TenThousandMatchesStayWithinUiQueryBudget)
{
	std::string Error;
	for(int i = 0; i < 10000; ++i)
	{
		const std::string MatchId = "scale-" + std::to_string(i) + "@ddnet.org";
		if(m_Journal.Insert(StoredMatch(MatchId.c_str(), 2000000 + i, EMatchCompleteness::COMPLETE, i % 2 == 0 ? EMatchOutcome::WIN : EMatchOutcome::LOSS, i), &Error) != CMatchJournal::EInsertResult::INSERTED)
		{
			FAIL() << "failed to insert synthetic match " << i << ": " << Error;
		}
	}

	const auto QueryStart = std::chrono::steady_clock::now();
	CMatchHistoryFilter HistoryFilter;
	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches(HistoryFilter, vEntries, &Error)) << Error;
	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile({}, Profile, &Error)) << Error;
	CMatchJournalInfo Info;
	ASSERT_TRUE(m_Journal.Info(Info, &Error)) << Error;
	const auto QueryDuration = std::chrono::steady_clock::now() - QueryStart;

	EXPECT_EQ(vEntries.size(), 10000U);
	EXPECT_EQ(Profile.m_Matches, 10000);
	EXPECT_EQ(Info.m_NumMatches, 10000);
	EXPECT_GT(Info.m_DatabaseSize, 0);
	EXPECT_LT(Info.m_DatabaseSize, 256 * 1024 * 1024);
	EXPECT_LT(QueryDuration, std::chrono::seconds(5));
	RecordProperty("query_latency_ms", std::chrono::duration_cast<std::chrono::milliseconds>(QueryDuration).count());
	RecordProperty("database_size_bytes", Info.m_DatabaseSize);
}

TEST_F(MatchJournal, GeneratedSampleMatchesAreStoredAndQueryable)
{
	std::string Error;
	ASSERT_TRUE(GenerateSampleMatches(m_Journal, 24, "Local", &Error)) << Error;

	std::vector<CMatchHistoryEntry> vEntries;
	ASSERT_TRUE(m_Journal.ListMatches(CMatchHistoryFilter(), vEntries, &Error)) << Error;
	EXPECT_EQ(vEntries.size(), 24);
	int RaceMatches = 0;
	for(const CMatchHistoryEntry &Entry : vEntries)
	{
		EXPECT_TRUE(Entry.m_LocalOutcome.has_value());
		// A race is measured in run times, it has neither a score nor frags
		const bool Race = Entry.m_ModeId.starts_with("ddrace.");
		RaceMatches += Race ? 1 : 0;
		EXPECT_EQ(Entry.m_LocalScore.has_value(), !Race);
		EXPECT_EQ(Entry.m_LocalKills.has_value(), !Race);
		EXPECT_EQ(Entry.m_LocalDeaths.has_value(), !Race);
		CStoredMatch Loaded;
		ASSERT_TRUE(m_Journal.LoadMatch(Entry.m_OriginId.c_str(), Entry.m_MatchId, Loaded, &Error)) << Error;
		EXPECT_EQ(Loaded.m_LocalParticipantId, 0);
		EXPECT_EQ(Loaded.m_Report.m_vParticipants.front().m_DisplayName, "Local");
	}
	// The samples have to cover the race shape as well, otherwise the run page
	// could never be looked at without playing a map first.
	EXPECT_GT(RaceMatches, 0);

	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile(CMatchProfileFilter(), Profile, &Error)) << Error;
	EXPECT_GT(Profile.m_Matches, 0);
	EXPECT_FALSE(Profile.m_vMetrics.empty());
}

TEST_F(MatchJournal, RefusesMetricValuesThatWouldOverflowTheProfileSum)
{
	CStoredMatch Stored = StoredMatch("overflow", 1720000000, EMatchCompleteness::COMPLETE, EMatchOutcome::WIN, 1);
	Stored.m_Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/damage_done", std::numeric_limits<int64_t>::max()});
	std::string Error;
	ASSERT_TRUE(MatchReportToJson(Stored.m_Report, Stored.m_RawReport, &Error)) << Error;
	EXPECT_EQ(m_Journal.Insert(Stored, &Error), CMatchJournal::EInsertResult::ERROR);

	CMatchProfile Profile;
	ASSERT_TRUE(m_Journal.QueryProfile(CMatchProfileFilter(), Profile, &Error)) << Error;
	EXPECT_EQ(Profile.m_Matches, 0);
}
