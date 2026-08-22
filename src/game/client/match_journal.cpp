#include "match_journal.h"

#include <base/str.h>

#include <engine/storage.h>

#include <sqlite3.h>

#include <algorithm>
#include <type_traits>

const char *MatchReportSourceName(EMatchReportSource Source)
{
	static const char *const s_apNames[] = {"client_observed", "server_report", "server_snapshot"};
	static_assert(std::size(s_apNames) == (size_t)EMatchReportSource::NUM);
	return s_apNames[(int)Source];
}

const char *MatchCompletenessName(EMatchCompleteness Completeness)
{
	static const char *const s_apNames[] = {"complete", "partial_since_join", "aborted"};
	static_assert(std::size(s_apNames) == (size_t)EMatchCompleteness::NUM);
	return s_apNames[(int)Completeness];
}

bool MatchSupersedes(EMatchReportSource NewSource, EMatchCompleteness NewCompleteness, EMatchReportSource StoredSource, EMatchCompleteness StoredCompleteness)
{
	static const int s_aSourceRanks[] = {0, 2, 1};
	static const int s_aCompletenessRanks[] = {2, 1, 0};
	static_assert(std::size(s_aSourceRanks) == (size_t)EMatchReportSource::NUM && std::size(s_aCompletenessRanks) == (size_t)EMatchCompleteness::NUM);
	if(NewSource != StoredSource)
		return s_aSourceRanks[(int)NewSource] > s_aSourceRanks[(int)StoredSource];
	return s_aCompletenessRanks[(int)NewCompleteness] > s_aCompletenessRanks[(int)StoredCompleteness];
}

namespace
{
	class CBlob
	{
	public:
		const std::string &m_Data;
	};

	bool BindValue(sqlite3_stmt *pStmt, int Index, int64_t Value) { return sqlite3_bind_int64(pStmt, Index, Value) == SQLITE_OK; }
	bool BindValue(sqlite3_stmt *pStmt, int Index, const std::string &Value) { return sqlite3_bind_text(pStmt, Index, Value.c_str(), (int)Value.size(), SQLITE_TRANSIENT) == SQLITE_OK; }
	bool BindValue(sqlite3_stmt *pStmt, int Index, const CBlob &Value) { return sqlite3_bind_blob(pStmt, Index, Value.m_Data.data(), (int)Value.m_Data.size(), SQLITE_TRANSIENT) == SQLITE_OK; }
	template<typename T>
	bool BindValue(sqlite3_stmt *pStmt, int Index, const std::optional<T> &Value)
	{
		return Value.has_value() ? BindValue(pStmt, Index, (int64_t)*Value) : sqlite3_bind_null(pStmt, Index) == SQLITE_OK;
	}
	template<typename TEnum>
		requires std::is_enum_v<TEnum>
	bool BindValue(sqlite3_stmt *pStmt, int Index, TEnum Value)
	{
		return BindValue(pStmt, Index, (int64_t)Value);
	}

	bool SqlError(sqlite3 *pSqlite, std::string *pError, const char *pContext)
	{
		if(pError != nullptr)
			*pError = std::string(pContext) + ": " + sqlite3_errmsg(pSqlite);
		return false;
	}

	// nullptr on error, with the error in pError
	template<typename... TValues>
	CSqliteStmt Statement(sqlite3 *pSqlite, std::string *pError, const char *pSql, const TValues &...Values)
	{
		CSqliteStmt pStmt = SqlitePrepare(pSqlite, pSql);
		int Index = 1;
		if(!pStmt || !(BindValue(pStmt.get(), Index++, Values) && ...))
		{
			SqlError(pSqlite, pError, "prepare statement");
			return nullptr;
		}
		return pStmt;
	}

	template<typename... TValues>
	bool Run(sqlite3 *pSqlite, std::string *pError, const char *pSql, const TValues &...Values)
	{
		CSqliteStmt pStmt = Statement(pSqlite, pError, pSql, Values...);
		return pStmt && (sqlite3_step(pStmt.get()) == SQLITE_DONE || SqlError(pSqlite, pError, "run statement"));
	}

	std::string UuidString(CUuid Uuid)
	{
		char aUuid[UUID_MAXSTRSIZE];
		FormatUuid(Uuid, aUuid, sizeof(aUuid));
		return aUuid;
	}

	const char *ColumnText(sqlite3_stmt *pStmt, int Column)
	{
		const unsigned char *pText = sqlite3_column_text(pStmt, Column);
		return pText ? (const char *)pText : "";
	}

	template<typename TEnum>
	bool ColumnEnum(sqlite3_stmt *pStmt, int Column, TEnum &Value)
	{
		const int Raw = sqlite3_column_int(pStmt, Column);
		Value = (TEnum)Raw;
		return Raw >= 0 && Raw < (int)TEnum::NUM;
	}
}

bool CMatchJournal::Execute(const char *pStatement, std::string *pError)
{
	return m_pSqlite && (sqlite3_exec(m_pSqlite.get(), pStatement, nullptr, nullptr, nullptr) == SQLITE_OK || SqlError(m_pSqlite.get(), pError, pStatement));
}

bool CMatchJournal::Rollback(std::string *pError)
{
	std::string RollbackError;
	return Execute("ROLLBACK", pError && pError->empty() ? pError : &RollbackError);
}

bool CMatchJournal::Open(IStorage *pStorage, std::string *pError)
{
	// earlier versions were never released
	static constexpr int SCHEMA_VERSION = 3;
	m_pStorage = pStorage;
	m_pSqlite = SqliteOpen(pStorage, "match-journal.sqlite3");
	if(!m_pSqlite)
	{
		if(pError)
			*pError = "unable to open the match journal";
		return false;
	}
	// The report is kept as it came, packed; the other columns and the metrics of the local
	// player exist so that the history and the profile can be queried.
	static const char *s_pSchema = R"sql(
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 1000;
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS journal_meta (
	id INTEGER PRIMARY KEY CHECK(id = 1),
	schema_version INTEGER NOT NULL
);
INSERT OR IGNORE INTO journal_meta(id, schema_version) VALUES(1, 3);
CREATE TABLE IF NOT EXISTS origins (
	origin_id INTEGER PRIMARY KEY,
	endpoint TEXT NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS matches (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	source INTEGER NOT NULL,
	completeness INTEGER NOT NULL,
	local_participant_id INTEGER NOT NULL,
	local_outcome INTEGER,
	mode_id TEXT NOT NULL,
	map_name TEXT NOT NULL,
	end_time_utc INTEGER NOT NULL,
	duration_ticks INTEGER NOT NULL,
	tick_rate INTEGER NOT NULL,
	report BLOB NOT NULL,
	PRIMARY KEY(origin_id, match_id),
	FOREIGN KEY(origin_id) REFERENCES origins(origin_id)
);
CREATE TABLE IF NOT EXISTS local_metrics (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	metric_id TEXT NOT NULL,
	value INTEGER NOT NULL,
	aggregation INTEGER NOT NULL,
	PRIMARY KEY(origin_id, match_id, metric_id),
	FOREIGN KEY(origin_id, match_id) REFERENCES matches(origin_id, match_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS matches_end_time ON matches(end_time_utc);
COMMIT;
)sql";
	std::string Error;
	if(!Execute(s_pSchema, &Error))
	{
		Rollback(nullptr);
		m_pSqlite.reset();
		if(pError)
			*pError = Error;
		return false;
	}
	CSqliteStmt pVersion = Statement(m_pSqlite.get(), pError, "SELECT schema_version FROM journal_meta WHERE id = 1");
	if(!pVersion || sqlite3_step(pVersion.get()) != SQLITE_ROW || sqlite3_column_int(pVersion.get(), 0) != SCHEMA_VERSION)
	{
		// sqlite3_close() leaves the database open while a statement is left
		pVersion.reset();
		m_pSqlite.reset();
		if(pError)
			*pError = "unsupported match journal schema version";
		return false;
	}
	return true;
}

CMatchJournal::EInsertResult CMatchJournal::Insert(const CStoredMatch &Match, const CStoredMatch *pReplacedObserved, std::string *pError)
{
	const CMatchReport &Report = Match.m_Report;
	if(!m_pSqlite || Match.m_OriginId.empty() || Match.m_OriginId.size() > 256 || !str_utf8_check(Match.m_OriginId.c_str()) ||
		!Match.m_LocalParticipantId || !Report.Participant(*Match.m_LocalParticipantId))
	{
		if(pError)
			*pError = "invalid match for the journal";
		return EInsertResult::ERROR;
	}
	std::string Packed;
	if(!MatchReportToPacked(Report, Packed, pError))
		return EInsertResult::ERROR;
	if(!Execute("BEGIN IMMEDIATE", pError))
		return EInsertResult::ERROR;
	const auto Fail = [&]() {
		Rollback(pError);
		return EInsertResult::ERROR;
	};

	sqlite3 *pSqlite = m_pSqlite.get();
	const std::string MatchId = UuidString(Report.m_MatchId);
	if(!Run(pSqlite, pError, "INSERT OR IGNORE INTO origins(endpoint) VALUES(?)", Match.m_OriginId))
		return Fail();
	CSqliteStmt pOrigin = Statement(pSqlite, pError, "SELECT origin_id FROM origins WHERE endpoint = ?", Match.m_OriginId);
	if(!pOrigin || sqlite3_step(pOrigin.get()) != SQLITE_ROW)
		return Fail();
	const int64_t OriginId = sqlite3_column_int64(pOrigin.get(), 0);

	// the same round can arrive again, the version that knows more takes the place of the other
	CSqliteStmt pExisting = Statement(pSqlite, pError, "SELECT source, completeness FROM matches WHERE origin_id = ? AND match_id = ?", OriginId, MatchId);
	if(!pExisting)
		return Fail();
	EMatchReportSource StoredSource;
	EMatchCompleteness StoredCompleteness;
	if(sqlite3_step(pExisting.get()) == SQLITE_ROW && (!ColumnEnum(pExisting.get(), 0, StoredSource) || !ColumnEnum(pExisting.get(), 1, StoredCompleteness) ||
								  MatchSupersedes(Match.m_Source, Match.m_Completeness, StoredSource, StoredCompleteness)))
	{
		if(!Run(pSqlite, pError, "DELETE FROM matches WHERE origin_id = ? AND match_id = ?", OriginId, MatchId))
			return Fail();
	}

	const CMatchStanding *pLocalStanding = Report.Standing(EMatchSubjectKind::PARTICIPANT, *Match.m_LocalParticipantId);
	const std::optional<int64_t> LocalOutcome = pLocalStanding ? std::optional<int64_t>((int64_t)pLocalStanding->m_Outcome) : std::nullopt;
	if(!Run(pSqlite, pError, "INSERT OR IGNORE INTO matches VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
		   OriginId, MatchId, Match.m_Source, Match.m_Completeness, (int64_t)*Match.m_LocalParticipantId, LocalOutcome,
		   Report.m_ModeId, Report.m_MapName, Report.m_EndTimeUtc, Report.m_DurationTicks, (int64_t)Report.m_TickRate, CBlob{Packed}))
		return Fail();
	const bool Inserted = sqlite3_changes(pSqlite) != 0;
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		if(!Inserted || Metric.m_SubjectKind != EMatchSubjectKind::PARTICIPANT || Metric.m_SubjectId != Match.m_LocalParticipantId)
			continue;
		if(!Run(pSqlite, pError, "INSERT INTO local_metrics VALUES(?, ?, ?, ?, ?)", OriginId, MatchId, Metric.m_MetricId, Metric.m_Value, Metric.m_Aggregation))
			return Fail();
	}

	bool Changed = Inserted;
	if(pReplacedObserved && pReplacedObserved->m_Source == EMatchReportSource::CLIENT_OBSERVED)
	{
		if(!Run(pSqlite, pError, "DELETE FROM matches WHERE match_id = ? AND source = ? AND origin_id = (SELECT origin_id FROM origins WHERE endpoint = ?)",
			   UuidString(pReplacedObserved->m_Report.m_MatchId), EMatchReportSource::CLIENT_OBSERVED, pReplacedObserved->m_OriginId))
			return Fail();
		Changed = Changed || sqlite3_changes(pSqlite) != 0;
		if(!Execute("DELETE FROM origins WHERE NOT EXISTS (SELECT 1 FROM matches WHERE matches.origin_id = origins.origin_id)", pError))
			return Fail();
	}
	if(!Execute("COMMIT", pError))
		return Fail();
	if(Changed)
		m_pStorage->SyncPersistentStorage();
	return Inserted ? EInsertResult::INSERTED : EInsertResult::DUPLICATE;
}

bool CMatchJournal::ListMatches(std::vector<CMatchHistoryEntry> &vEntries, std::string *pError) const
{
	vEntries.clear();
	if(!m_pSqlite)
		return false;
	CSqliteStmt pStmt = Statement(m_pSqlite.get(), pError, R"sql(
SELECT o.endpoint, m.match_id, m.source, m.completeness, m.mode_id, m.map_name, m.end_time_utc, m.duration_ticks, m.tick_rate, m.local_outcome, x.value
FROM matches m
JOIN origins o ON o.origin_id = m.origin_id
LEFT JOIN local_metrics x ON x.origin_id = m.origin_id AND x.match_id = m.match_id AND x.metric_id = 'score'
ORDER BY m.end_time_utc DESC
LIMIT 10000
)sql");
	if(!pStmt)
		return false;
	int Result;
	while((Result = sqlite3_step(pStmt.get())) == SQLITE_ROW)
	{
		CMatchHistoryEntry &Entry = vEntries.emplace_back();
		Entry.m_OriginId = ColumnText(pStmt.get(), 0);
		EMatchOutcome Outcome;
		if(ParseUuid(&Entry.m_MatchId, ColumnText(pStmt.get(), 1)) != 0 || !ColumnEnum(pStmt.get(), 2, Entry.m_Source) || !ColumnEnum(pStmt.get(), 3, Entry.m_Completeness))
			return SqlError(m_pSqlite.get(), pError, "corrupt match row");
		Entry.m_ModeId = ColumnText(pStmt.get(), 4);
		Entry.m_MapName = ColumnText(pStmt.get(), 5);
		Entry.m_EndTimeUtc = sqlite3_column_int64(pStmt.get(), 6);
		Entry.m_DurationTicks = sqlite3_column_int64(pStmt.get(), 7);
		Entry.m_TickRate = sqlite3_column_int(pStmt.get(), 8);
		if(sqlite3_column_type(pStmt.get(), 9) != SQLITE_NULL && ColumnEnum(pStmt.get(), 9, Outcome))
			Entry.m_LocalOutcome = Outcome;
		if(sqlite3_column_type(pStmt.get(), 10) != SQLITE_NULL)
			Entry.m_LocalScore = sqlite3_column_int64(pStmt.get(), 10);
	}
	return Result == SQLITE_DONE || SqlError(m_pSqlite.get(), pError, "list matches");
}

bool CMatchJournal::LoadMatch(const char *pOriginId, CUuid MatchId, CStoredMatch &Match, std::string *pError) const
{
	if(!m_pSqlite)
		return false;
	CSqliteStmt pStmt = Statement(m_pSqlite.get(), pError, R"sql(
SELECT m.source, m.completeness, m.local_participant_id, m.report
FROM matches m JOIN origins o ON o.origin_id = m.origin_id
WHERE o.endpoint = ? AND m.match_id = ?
)sql",
		std::string(pOriginId), UuidString(MatchId));
	if(!pStmt)
		return false;
	if(sqlite3_step(pStmt.get()) != SQLITE_ROW)
	{
		if(pError)
			*pError = "match not found";
		return false;
	}
	CStoredMatch Loaded;
	Loaded.m_OriginId = pOriginId;
	Loaded.m_LocalParticipantId = sqlite3_column_int(pStmt.get(), 2);
	if(!ColumnEnum(pStmt.get(), 0, Loaded.m_Source) || !ColumnEnum(pStmt.get(), 1, Loaded.m_Completeness) ||
		!MatchReportFromPacked(sqlite3_column_blob(pStmt.get(), 3), sqlite3_column_bytes(pStmt.get(), 3), Loaded.m_Report, pError))
		return false;
	Match = std::move(Loaded);
	return true;
}

bool CMatchJournal::QueryProfile(const CMatchProfileFilter &Filter, CMatchProfile &Profile, std::string *pError) const
{
	Profile = {};
	if(!m_pSqlite)
		return false;
	CSqliteStmt pSummary = Statement(m_pSqlite.get(), pError, R"sql(
SELECT COUNT(*),
 COALESCE(SUM(m.local_outcome = ?), 0),
 COALESCE(SUM(m.local_outcome = ?), 0),
 COALESCE(SUM(m.local_outcome = ?), 0),
 COALESCE(SUM(COALESCE(p.value, m.duration_ticks) / m.tick_rate), 0)
FROM matches m
LEFT JOIN local_metrics p ON p.origin_id = m.origin_id AND p.match_id = m.match_id AND p.metric_id = 'playtime_ticks'
WHERE m.completeness = ? AND m.end_time_utc >= ? AND (? = '' OR m.mode_id = ?)
)sql",
		EMatchOutcome::WIN, EMatchOutcome::LOSS, EMatchOutcome::DRAW, EMatchCompleteness::COMPLETE, Filter.m_SinceUtc, Filter.m_ModeId, Filter.m_ModeId);
	if(!pSummary || sqlite3_step(pSummary.get()) != SQLITE_ROW)
		return SqlError(m_pSqlite.get(), pError, "query profile summary");
	Profile.m_Matches = sqlite3_column_int(pSummary.get(), 0);
	Profile.m_Wins = sqlite3_column_int(pSummary.get(), 1);
	Profile.m_Losses = sqlite3_column_int(pSummary.get(), 2);
	Profile.m_Draws = sqlite3_column_int(pSummary.get(), 3);
	Profile.m_PlaytimeSeconds = sqlite3_column_int64(pSummary.get(), 4);

	CSqliteStmt pMetrics = Statement(m_pSqlite.get(), pError, R"sql(
SELECT m.mode_id, x.metric_id, x.aggregation, CASE WHEN x.aggregation = ? THEN MAX(x.value) ELSE SUM(x.value) END
FROM matches m
JOIN local_metrics x ON x.origin_id = m.origin_id AND x.match_id = m.match_id
WHERE m.completeness = ? AND m.end_time_utc >= ? AND (? = '' OR m.mode_id = ?) AND x.aggregation != ?
GROUP BY m.mode_id, x.metric_id, x.aggregation
ORDER BY m.mode_id, x.metric_id
)sql",
		EMatchMetricAggregation::MAXIMUM, EMatchCompleteness::COMPLETE, Filter.m_SinceUtc, Filter.m_ModeId, Filter.m_ModeId, EMatchMetricAggregation::MATCH_ONLY);
	if(!pMetrics)
		return false;
	int Result;
	while((Result = sqlite3_step(pMetrics.get())) == SQLITE_ROW)
	{
		CMatchMetricAggregate &Metric = Profile.m_vMetrics.emplace_back();
		Metric.m_ModeId = ColumnText(pMetrics.get(), 0);
		Metric.m_MetricId = ColumnText(pMetrics.get(), 1);
		if(!ColumnEnum(pMetrics.get(), 2, Metric.m_Aggregation))
			return SqlError(m_pSqlite.get(), pError, "corrupt metric row");
		Metric.m_Value = sqlite3_column_int64(pMetrics.get(), 3);
	}
	return Result == SQLITE_DONE || SqlError(m_pSqlite.get(), pError, "query profile metrics");
}

bool CMatchJournal::DeleteMatch(const char *pOriginId, CUuid MatchId, std::string *pError)
{
	if(!Execute("BEGIN IMMEDIATE", pError))
		return false;
	if(!Run(m_pSqlite.get(), pError, "DELETE FROM matches WHERE match_id = ? AND origin_id = (SELECT origin_id FROM origins WHERE endpoint = ?)", UuidString(MatchId), std::string(pOriginId)) ||
		!Execute("DELETE FROM origins WHERE NOT EXISTS (SELECT 1 FROM matches WHERE matches.origin_id = origins.origin_id)", pError) ||
		!Execute("COMMIT", pError))
	{
		Rollback(pError);
		return false;
	}
	m_pStorage->SyncPersistentStorage();
	return true;
}

bool CMatchJournal::DeleteAll(std::string *pError)
{
	if(!Execute("BEGIN IMMEDIATE", pError))
		return false;
	if(!Execute("DELETE FROM matches", pError) || !Execute("DELETE FROM origins", pError) || !Execute("COMMIT", pError))
	{
		Rollback(pError);
		return false;
	}
	m_pStorage->SyncPersistentStorage();
	return true;
}

bool CMatchJournal::Info(CMatchJournalInfo &Info, std::string *pError) const
{
	Info = {};
	if(!m_pSqlite)
		return false;
	CSqliteStmt pPageCount = Statement(m_pSqlite.get(), pError, "PRAGMA page_count");
	CSqliteStmt pPageSize = Statement(m_pSqlite.get(), pError, "PRAGMA page_size");
	CSqliteStmt pMatches = Statement(m_pSqlite.get(), pError, "SELECT COUNT(*), MIN(end_time_utc) FROM matches");
	if(!pPageCount || !pPageSize || !pMatches || sqlite3_step(pPageCount.get()) != SQLITE_ROW || sqlite3_step(pPageSize.get()) != SQLITE_ROW || sqlite3_step(pMatches.get()) != SQLITE_ROW)
		return SqlError(m_pSqlite.get(), pError, "query journal info");
	Info.m_DatabaseSize = sqlite3_column_int64(pPageCount.get(), 0) * sqlite3_column_int64(pPageSize.get(), 0);
	Info.m_NumMatches = sqlite3_column_int(pMatches.get(), 0);
	if(sqlite3_column_type(pMatches.get(), 1) != SQLITE_NULL)
		Info.m_OldestMatchUtc = sqlite3_column_int64(pMatches.get(), 1);
	return true;
}
