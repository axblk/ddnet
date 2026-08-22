#include "match_journal.h"

#include "match_report_assembler.h"

#include <base/str.h>
#include <base/time.h>

#include <engine/storage.h>

#include <generated/protocol.h>

#include <sqlite3.h>

#include <algorithm>
#include <utility>

namespace
{
	bool SetSqlError(sqlite3 *pSqlite, std::string *pError, const char *pContext)
	{
		if(pError != nullptr)
			*pError = std::string(pContext) + ": " + sqlite3_errmsg(pSqlite);
		return false;
	}

	bool SqlSuccess(int Result, sqlite3 *pSqlite, std::string *pError, const char *pContext)
	{
		if(Result == SQLITE_OK || Result == SQLITE_DONE || Result == SQLITE_ROW)
			return true;
		SqliteHandleError(Result, pSqlite, pContext);
		return SetSqlError(pSqlite, pError, pContext);
	}

	bool BindText(sqlite3 *pSqlite, sqlite3_stmt *pStatement, int Index, const std::string &Value, std::string *pError)
	{
		return SqlSuccess(sqlite3_bind_text(pStatement, Index, Value.c_str(), static_cast<int>(Value.size()), SQLITE_TRANSIENT), pSqlite, pError, "bind text");
	}

	bool BindOptionalInt(sqlite3 *pSqlite, sqlite3_stmt *pStatement, int Index, std::optional<int> Value, std::string *pError)
	{
		const int Result = Value.has_value() ? sqlite3_bind_int(pStatement, Index, *Value) : sqlite3_bind_null(pStatement, Index);
		return SqlSuccess(Result, pSqlite, pError, "bind optional integer");
	}

	bool StepDone(sqlite3 *pSqlite, sqlite3_stmt *pStatement, std::string *pError)
	{
		return SqlSuccess(sqlite3_step(pStatement), pSqlite, pError, "step statement") && sqlite3_reset(pStatement) == SQLITE_OK;
	}

	std::string UuidString(CUuid Uuid)
	{
		char aUuid[UUID_MAXSTRSIZE];
		FormatUuid(Uuid, aUuid, sizeof(aUuid));
		return aUuid;
	}

	bool ValidSource(int Value)
	{
		return Value >= static_cast<int>(EMatchReportSource::CLIENT_OBSERVED) && Value <= static_cast<int>(EMatchReportSource::SERVER_SNAPSHOT);
	}

	bool ValidCompleteness(int Value)
	{
		return Value >= static_cast<int>(EMatchCompleteness::COMPLETE) && Value <= static_cast<int>(EMatchCompleteness::ABORTED);
	}

	bool Prepare(sqlite3 *pSqlite, const char *pSql, CSqliteStmt &pStatement, std::string *pError)
	{
		pStatement = SqlitePrepare(pSqlite, pSql);
		return pStatement != nullptr || SetSqlError(pSqlite, pError, "prepare statement");
	}
}

const char *MatchReportSourceName(EMatchReportSource Source)
{
	switch(Source)
	{
	case EMatchReportSource::CLIENT_OBSERVED: return "client_observed";
	case EMatchReportSource::SERVER_REPORT: return "server_report";
	case EMatchReportSource::SERVER_DATABASE: return "server_database";
	case EMatchReportSource::SERVER_SNAPSHOT: return "server_snapshot";
	}
	return "invalid";
}

const char *MatchCompletenessName(EMatchCompleteness Completeness)
{
	switch(Completeness)
	{
	case EMatchCompleteness::COMPLETE: return "complete";
	case EMatchCompleteness::PARTIAL_SINCE_JOIN: return "partial_since_join";
	case EMatchCompleteness::ABORTED: return "aborted";
	}
	return "invalid";
}

bool CMatchJournal::Execute(const char *pStatement, std::string *pError)
{
	if(m_pSqlite == nullptr)
		return false;
	const int Result = sqlite3_exec(m_pSqlite.get(), pStatement, nullptr, nullptr, nullptr);
	return SqlSuccess(Result, m_pSqlite.get(), pError, pStatement);
}

bool CMatchJournal::Rollback(std::string *pError)
{
	const std::string OriginalError = pError != nullptr ? *pError : std::string();
	std::string RollbackError;
	const bool Success = Execute("ROLLBACK", &RollbackError);
	if(pError != nullptr)
		*pError = OriginalError.empty() ? RollbackError : OriginalError;
	return Success;
}

bool CMatchJournal::Open(IStorage *pStorage, std::string *pError)
{
	m_pStorage = nullptr;
	m_pSqlite = SqliteOpen(pStorage, "match-journal.sqlite3");
	if(m_pSqlite == nullptr)
		return false;
	m_pStorage = pStorage;
	static const char *s_pSchema = R"sql(
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 1000;
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS journal_meta (
	id INTEGER PRIMARY KEY CHECK(id = 1),
	schema_version INTEGER NOT NULL
);
INSERT OR IGNORE INTO journal_meta(id, schema_version) VALUES(1, 2);
CREATE TABLE IF NOT EXISTS origins (
	origin_id INTEGER PRIMARY KEY,
	endpoint TEXT NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS matches (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	source INTEGER NOT NULL,
	completeness INTEGER NOT NULL,
	local_participant_id INTEGER,
	report_schema_version INTEGER NOT NULL,
	mode_id TEXT NOT NULL,
	mode_schema_version INTEGER NOT NULL,
	map_name TEXT NOT NULL,
	map_sha256 BLOB NOT NULL,
	start_time_utc INTEGER NOT NULL,
	end_time_utc INTEGER NOT NULL,
	duration_ticks INTEGER NOT NULL,
	tick_rate INTEGER NOT NULL,
	termination INTEGER NOT NULL,
	ranked INTEGER NOT NULL,
	unranked_reason TEXT NOT NULL,
	raw_report TEXT NOT NULL,
	PRIMARY KEY(origin_id, match_id),
	FOREIGN KEY(origin_id) REFERENCES origins(origin_id)
);
CREATE TABLE IF NOT EXISTS teams (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	team_id INTEGER NOT NULL,
	display_name TEXT NOT NULL,
	PRIMARY KEY(origin_id, match_id, team_id),
	FOREIGN KEY(origin_id, match_id) REFERENCES matches(origin_id, match_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS participants (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	participant_id INTEGER NOT NULL,
	team_id INTEGER,
	display_name TEXT NOT NULL,
	clan TEXT NOT NULL,
	joined_tick INTEGER NOT NULL,
	left_tick INTEGER,
	bot INTEGER NOT NULL,
	PRIMARY KEY(origin_id, match_id, participant_id),
	FOREIGN KEY(origin_id, match_id) REFERENCES matches(origin_id, match_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS standings (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	subject_kind INTEGER NOT NULL,
	subject_id INTEGER NOT NULL,
	rank INTEGER NOT NULL,
	outcome INTEGER NOT NULL,
	PRIMARY KEY(origin_id, match_id, subject_kind, subject_id),
	FOREIGN KEY(origin_id, match_id) REFERENCES matches(origin_id, match_id) ON DELETE CASCADE
);
CREATE TABLE IF NOT EXISTS metrics (
	origin_id INTEGER NOT NULL,
	match_id TEXT NOT NULL,
	subject_kind INTEGER NOT NULL,
	subject_id INTEGER,
	metric_id TEXT NOT NULL,
	value INTEGER NOT NULL,
	aggregation INTEGER NOT NULL DEFAULT 0,
	FOREIGN KEY(origin_id, match_id) REFERENCES matches(origin_id, match_id) ON DELETE CASCADE
);
CREATE UNIQUE INDEX IF NOT EXISTS metrics_unique_subject ON metrics(origin_id, match_id, subject_kind, IFNULL(subject_id, -1), metric_id);
CREATE INDEX IF NOT EXISTS matches_end_time ON matches(end_time_utc);
CREATE INDEX IF NOT EXISTS matches_mode ON matches(mode_id, mode_schema_version);
CREATE INDEX IF NOT EXISTS matches_map ON matches(map_name);
CREATE INDEX IF NOT EXISTS matches_local_participant ON matches(local_participant_id);
CREATE INDEX IF NOT EXISTS metrics_metric_id ON metrics(metric_id);
COMMIT;
)sql";
	if(!Execute(s_pSchema, pError))
	{
		Rollback(nullptr);
		m_pStorage = nullptr;
		m_pSqlite.reset();
		return false;
	}

	CSqliteStmt pVersion;
	if(!Prepare(m_pSqlite.get(), "SELECT schema_version FROM journal_meta WHERE id = 1", pVersion, pError))
	{
		m_pStorage = nullptr;
		m_pSqlite.reset();
		return false;
	}
	const int VersionResult = sqlite3_step(pVersion.get());
	const int Version = VersionResult == SQLITE_ROW ? sqlite3_column_int(pVersion.get(), 0) : -1;
	pVersion.reset();
	if(Version == 1 && !Execute("BEGIN IMMEDIATE; ALTER TABLE metrics ADD COLUMN aggregation INTEGER NOT NULL DEFAULT 0; UPDATE metrics SET aggregation = 1 WHERE metric_id LIKE '%/best_spree'; UPDATE metrics SET aggregation = 2 WHERE metric_id LIKE '%/sudden_death'; UPDATE journal_meta SET schema_version = 2 WHERE id = 1; COMMIT;", pError))
	{
		Rollback(nullptr);
		m_pStorage = nullptr;
		m_pSqlite.reset();
		return false;
	}
	if(Version != 1 && Version != 2)
	{
		m_pStorage = nullptr;
		m_pSqlite.reset();
		if(pError != nullptr)
			*pError = "unsupported match journal schema version";
		return false;
	}
	return true;
}

CMatchJournal::EInsertResult CMatchJournal::Insert(const CStoredMatch &Match, std::string *pError)
{
	return InsertImpl(Match, nullptr, UUID_ZEROED, pError);
}

CMatchJournal::EInsertResult CMatchJournal::InsertReplacingObserved(const CStoredMatch &Match, const char *pObservedOriginId, CUuid ObservedMatchId, std::string *pError)
{
	if(Match.m_Source == EMatchReportSource::CLIENT_OBSERVED || pObservedOriginId == nullptr || ObservedMatchId == UUID_ZEROED)
		return EInsertResult::ERROR;
	return InsertImpl(Match, pObservedOriginId, ObservedMatchId, pError);
}

CMatchJournal::EInsertResult CMatchJournal::InsertImpl(const CStoredMatch &Match, const char *pObservedOriginId, CUuid ObservedMatchId, std::string *pError)
{
	const bool ReplacingObserved = pObservedOriginId != nullptr;
	if(m_pSqlite == nullptr || Match.m_OriginId.empty() || Match.m_OriginId.size() > 256 || !str_utf8_check(Match.m_OriginId.c_str()) || Match.m_RawReport.empty() ||
		!ValidSource(static_cast<int>(Match.m_Source)) || !ValidCompleteness(static_cast<int>(Match.m_Completeness)) ||
		(ReplacingObserved && (pObservedOriginId[0] == '\0' || str_length(pObservedOriginId) > 256 || !str_utf8_check(pObservedOriginId))))
		return EInsertResult::ERROR;
	CMatchReport Parsed;
	if(!MatchReportFromJson(Match.m_RawReport.data(), Match.m_RawReport.size(), Parsed, pError))
		return EInsertResult::ERROR;
	// The profile page sums these across every stored match. A single absurd
	// value, from a broken or hostile server, would make that sum overflow and
	// leave the page unusable until the journal is cleared, so such a report is
	// refused rather than stored.
	for(const CMatchMetric &Metric : Parsed.m_vMetrics)
	{
		if(Metric.m_Value < -MatchReportLimits::MAX_METRIC_VALUE || Metric.m_Value > MatchReportLimits::MAX_METRIC_VALUE)
		{
			if(pError != nullptr)
				*pError = "metric value out of range";
			return EInsertResult::ERROR;
		}
	}
	if(Match.m_LocalParticipantId.has_value())
	{
		bool Found = false;
		for(const CMatchParticipant &Participant : Parsed.m_vParticipants)
			Found = Found || Participant.m_ParticipantId == *Match.m_LocalParticipantId;
		if(!Found)
		{
			if(pError != nullptr)
				*pError = "local participant does not exist";
			return EInsertResult::ERROR;
		}
	}
	const std::string MatchId = UuidString(Parsed.m_MatchId);
	const std::string ObservedMatchIdString = ReplacingObserved ? UuidString(ObservedMatchId) : std::string();
	if(ReplacingObserved && Match.m_OriginId == pObservedOriginId && MatchId == ObservedMatchIdString)
		return EInsertResult::ERROR;
	if(!Execute("BEGIN IMMEDIATE", pError))
		return EInsertResult::ERROR;

	const auto FailInsert = [&]() {
		Rollback(pError);
		return EInsertResult::ERROR;
	};
	sqlite3 *pSqlite = m_pSqlite.get();
	CSqliteStmt pInsertOrigin;
	if(!Prepare(pSqlite, "INSERT OR IGNORE INTO origins(endpoint) VALUES(?)", pInsertOrigin, pError) ||
		!BindText(pSqlite, pInsertOrigin.get(), 1, Match.m_OriginId, pError) || !StepDone(pSqlite, pInsertOrigin.get(), pError))
		return FailInsert();

	CSqliteStmt pSelectOrigin;
	if(!Prepare(pSqlite, "SELECT origin_id FROM origins WHERE endpoint = ?", pSelectOrigin, pError) || !BindText(pSqlite, pSelectOrigin.get(), 1, Match.m_OriginId, pError))
		return FailInsert();
	if(sqlite3_step(pSelectOrigin.get()) != SQLITE_ROW)
	{
		SetSqlError(pSqlite, pError, "select origin");
		return FailInsert();
	}
	const int64_t OriginId = sqlite3_column_int64(pSelectOrigin.get(), 0);

	CSqliteStmt pInsertMatch;
	if(!Prepare(pSqlite, R"sql(INSERT OR IGNORE INTO matches(
origin_id, match_id, source, completeness, local_participant_id, report_schema_version,
mode_id, mode_schema_version, map_name, map_sha256, start_time_utc, end_time_utc,
duration_ticks, tick_rate, termination, ranked, unranked_reason, raw_report)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))sql",
		   pInsertMatch, pError))
		return FailInsert();
	const bool Bound = SqlSuccess(sqlite3_bind_int64(pInsertMatch.get(), 1, OriginId), pSqlite, pError, "bind origin") &&
			   BindText(pSqlite, pInsertMatch.get(), 2, MatchId, pError) &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 3, static_cast<int>(Match.m_Source)), pSqlite, pError, "bind source") &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 4, static_cast<int>(Match.m_Completeness)), pSqlite, pError, "bind completeness") &&
			   BindOptionalInt(pSqlite, pInsertMatch.get(), 5, Match.m_LocalParticipantId, pError) &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 6, Parsed.m_ReportSchemaVersion), pSqlite, pError, "bind report version") &&
			   BindText(pSqlite, pInsertMatch.get(), 7, Parsed.m_ModeId, pError) &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 8, Parsed.m_ModeSchemaVersion), pSqlite, pError, "bind mode version") &&
			   BindText(pSqlite, pInsertMatch.get(), 9, Parsed.m_MapName, pError) &&
			   SqlSuccess(sqlite3_bind_blob(pInsertMatch.get(), 10, Parsed.m_MapSha256.data, sizeof(Parsed.m_MapSha256.data), SQLITE_TRANSIENT), pSqlite, pError, "bind map hash") &&
			   SqlSuccess(sqlite3_bind_int64(pInsertMatch.get(), 11, Parsed.m_StartTimeUtc), pSqlite, pError, "bind start time") &&
			   SqlSuccess(sqlite3_bind_int64(pInsertMatch.get(), 12, Parsed.m_EndTimeUtc), pSqlite, pError, "bind end time") &&
			   SqlSuccess(sqlite3_bind_int64(pInsertMatch.get(), 13, Parsed.m_DurationTicks), pSqlite, pError, "bind duration") &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 14, Parsed.m_TickRate), pSqlite, pError, "bind tick rate") &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 15, static_cast<int>(Parsed.m_Termination)), pSqlite, pError, "bind termination") &&
			   SqlSuccess(sqlite3_bind_int(pInsertMatch.get(), 16, Parsed.m_Ranked ? 1 : 0), pSqlite, pError, "bind ranked") &&
			   BindText(pSqlite, pInsertMatch.get(), 17, Parsed.m_UnrankedReason, pError) &&
			   BindText(pSqlite, pInsertMatch.get(), 18, Match.m_RawReport, pError);
	if(!Bound || !SqlSuccess(sqlite3_step(pInsertMatch.get()), pSqlite, pError, "insert match"))
		return FailInsert();
	const bool Inserted = sqlite3_changes(pSqlite) != 0;

	if(Inserted)
	{
		CSqliteStmt pInsertTeam;
		CSqliteStmt pInsertParticipant;
		CSqliteStmt pInsertStanding;
		CSqliteStmt pInsertMetric;
		if(!Prepare(pSqlite, "INSERT INTO teams VALUES(?, ?, ?, ?)", pInsertTeam, pError) ||
			!Prepare(pSqlite, "INSERT INTO participants VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)", pInsertParticipant, pError) ||
			!Prepare(pSqlite, "INSERT INTO standings VALUES(?, ?, ?, ?, ?, ?)", pInsertStanding, pError) ||
			!Prepare(pSqlite, "INSERT INTO metrics VALUES(?, ?, ?, ?, ?, ?, ?)", pInsertMetric, pError))
			return FailInsert();

		for(const CMatchTeam &Team : Parsed.m_vTeams)
		{
			if(!SqlSuccess(sqlite3_bind_int64(pInsertTeam.get(), 1, OriginId), pSqlite, pError, "bind team origin") || !BindText(pSqlite, pInsertTeam.get(), 2, MatchId, pError) || !SqlSuccess(sqlite3_bind_int(pInsertTeam.get(), 3, Team.m_TeamId), pSqlite, pError, "bind team id") || !BindText(pSqlite, pInsertTeam.get(), 4, Team.m_DisplayName, pError) || !StepDone(pSqlite, pInsertTeam.get(), pError))
				return FailInsert();
		}
		for(const CMatchParticipant &Participant : Parsed.m_vParticipants)
		{
			if(!SqlSuccess(sqlite3_bind_int64(pInsertParticipant.get(), 1, OriginId), pSqlite, pError, "bind participant origin") || !BindText(pSqlite, pInsertParticipant.get(), 2, MatchId, pError) || !SqlSuccess(sqlite3_bind_int(pInsertParticipant.get(), 3, Participant.m_ParticipantId), pSqlite, pError, "bind participant id") || !BindOptionalInt(pSqlite, pInsertParticipant.get(), 4, Participant.m_TeamId, pError) || !BindText(pSqlite, pInsertParticipant.get(), 5, Participant.m_DisplayName, pError) || !BindText(pSqlite, pInsertParticipant.get(), 6, Participant.m_Clan, pError) || !SqlSuccess(sqlite3_bind_int64(pInsertParticipant.get(), 7, Participant.m_JoinedTick), pSqlite, pError, "bind joined tick") || !SqlSuccess(Participant.m_LeftTick.has_value() ? sqlite3_bind_int64(pInsertParticipant.get(), 8, *Participant.m_LeftTick) : sqlite3_bind_null(pInsertParticipant.get(), 8), pSqlite, pError, "bind left tick") || !SqlSuccess(sqlite3_bind_int(pInsertParticipant.get(), 9, Participant.m_Bot ? 1 : 0), pSqlite, pError, "bind bot") || !StepDone(pSqlite, pInsertParticipant.get(), pError))
				return FailInsert();
		}
		for(const CMatchStanding &Standing : Parsed.m_vStandings)
		{
			if(!SqlSuccess(sqlite3_bind_int64(pInsertStanding.get(), 1, OriginId), pSqlite, pError, "bind standing origin") || !BindText(pSqlite, pInsertStanding.get(), 2, MatchId, pError) || !SqlSuccess(sqlite3_bind_int(pInsertStanding.get(), 3, static_cast<int>(Standing.m_SubjectKind)), pSqlite, pError, "bind standing kind") || !SqlSuccess(sqlite3_bind_int(pInsertStanding.get(), 4, Standing.m_SubjectId), pSqlite, pError, "bind standing subject") || !SqlSuccess(sqlite3_bind_int(pInsertStanding.get(), 5, Standing.m_Rank), pSqlite, pError, "bind rank") || !SqlSuccess(sqlite3_bind_int(pInsertStanding.get(), 6, static_cast<int>(Standing.m_Outcome)), pSqlite, pError, "bind outcome") || !StepDone(pSqlite, pInsertStanding.get(), pError))
				return FailInsert();
		}
		for(const CMatchMetric &Metric : Parsed.m_vMetrics)
		{
			if(!SqlSuccess(sqlite3_bind_int64(pInsertMetric.get(), 1, OriginId), pSqlite, pError, "bind metric origin") || !BindText(pSqlite, pInsertMetric.get(), 2, MatchId, pError) || !SqlSuccess(sqlite3_bind_int(pInsertMetric.get(), 3, static_cast<int>(Metric.m_SubjectKind)), pSqlite, pError, "bind metric kind") || !BindOptionalInt(pSqlite, pInsertMetric.get(), 4, Metric.m_SubjectId, pError) || !BindText(pSqlite, pInsertMetric.get(), 5, Metric.m_MetricId, pError) || !SqlSuccess(sqlite3_bind_int64(pInsertMetric.get(), 6, Metric.m_Value), pSqlite, pError, "bind metric value") || !SqlSuccess(sqlite3_bind_int(pInsertMetric.get(), 7, static_cast<int>(Metric.m_Aggregation)), pSqlite, pError, "bind metric aggregation") || !StepDone(pSqlite, pInsertMetric.get(), pError))
				return FailInsert();
		}
	}

	bool Changed = Inserted;
	if(ReplacingObserved)
	{
		CSqliteStmt pDeleteObserved;
		if(!Prepare(pSqlite, "DELETE FROM matches WHERE match_id = ? AND origin_id = (SELECT origin_id FROM origins WHERE endpoint = ?) AND source = ?", pDeleteObserved, pError) ||
			!BindText(pSqlite, pDeleteObserved.get(), 1, ObservedMatchIdString, pError) ||
			!BindText(pSqlite, pDeleteObserved.get(), 2, pObservedOriginId, pError) ||
			!SqlSuccess(sqlite3_bind_int(pDeleteObserved.get(), 3, static_cast<int>(EMatchReportSource::CLIENT_OBSERVED)), pSqlite, pError, "bind observed source") ||
			!SqlSuccess(sqlite3_step(pDeleteObserved.get()), pSqlite, pError, "delete observed match"))
			return FailInsert();
		Changed = Changed || sqlite3_changes(pSqlite) != 0;
		if(!Execute("DELETE FROM origins WHERE NOT EXISTS (SELECT 1 FROM matches WHERE matches.origin_id = origins.origin_id)", pError))
			return FailInsert();
		Changed = Changed || sqlite3_changes(pSqlite) != 0;
	}
	if(!Execute("COMMIT", pError))
		return FailInsert();
	if(Changed)
		m_pStorage->SyncPersistentStorage();
	return Inserted ? EInsertResult::INSERTED : EInsertResult::DUPLICATE;
}

bool CMatchJournal::ListMatches(const CMatchHistoryFilter &Filter, std::vector<CMatchHistoryEntry> &vEntries, std::string *pError) const
{
	vEntries.clear();
	if(m_pSqlite == nullptr)
		return false;
	static const char *s_pSql = R"sql(
SELECT o.endpoint, m.match_id, m.source, m.completeness, m.mode_id, m.mode_schema_version,
       m.map_name, m.end_time_utc, m.duration_ticks, m.tick_rate, s.outcome, x.value,
       k.value, d.value
FROM matches m
JOIN origins o ON o.origin_id = m.origin_id
LEFT JOIN standings s ON s.origin_id = m.origin_id AND s.match_id = m.match_id
 AND s.subject_kind = ? AND s.subject_id = m.local_participant_id
LEFT JOIN metrics x ON x.origin_id = m.origin_id AND x.match_id = m.match_id
 AND x.subject_kind = ? AND x.subject_id = m.local_participant_id
 AND x.metric_id = m.mode_id || '/score'
LEFT JOIN metrics k ON k.origin_id = m.origin_id AND k.match_id = m.match_id
 AND k.subject_kind = ? AND k.subject_id = m.local_participant_id
 AND k.metric_id = m.mode_id || '/kills'
LEFT JOIN metrics d ON d.origin_id = m.origin_id AND d.match_id = m.match_id
 AND d.subject_kind = ? AND d.subject_id = m.local_participant_id
 AND d.metric_id = m.mode_id || '/deaths'
WHERE (? = '' OR m.mode_id = ?) AND (? = '' OR m.map_name = ?) AND (? = '' OR o.endpoint = ?)
 AND (? OR m.completeness = ?)
ORDER BY m.end_time_utc DESC
LIMIT ?
)sql";
	CSqliteStmt pStatement;
	if(!Prepare(m_pSqlite.get(), s_pSql, pStatement, pError))
		return false;
	const bool Bound = SqlSuccess(sqlite3_bind_int(pStatement.get(), 1, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind participant kind") &&
			   SqlSuccess(sqlite3_bind_int(pStatement.get(), 2, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind metric participant kind") &&
			   SqlSuccess(sqlite3_bind_int(pStatement.get(), 3, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind kills participant kind") &&
			   SqlSuccess(sqlite3_bind_int(pStatement.get(), 4, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind deaths participant kind") &&
			   BindText(m_pSqlite.get(), pStatement.get(), 5, Filter.m_ModeId, pError) && BindText(m_pSqlite.get(), pStatement.get(), 6, Filter.m_ModeId, pError) &&
			   BindText(m_pSqlite.get(), pStatement.get(), 7, Filter.m_MapName, pError) && BindText(m_pSqlite.get(), pStatement.get(), 8, Filter.m_MapName, pError) &&
			   BindText(m_pSqlite.get(), pStatement.get(), 9, Filter.m_OriginId, pError) && BindText(m_pSqlite.get(), pStatement.get(), 10, Filter.m_OriginId, pError) &&
			   SqlSuccess(sqlite3_bind_int(pStatement.get(), 11, Filter.m_IncludeIncomplete ? 1 : 0), m_pSqlite.get(), pError, "bind incomplete filter") &&
			   SqlSuccess(sqlite3_bind_int(pStatement.get(), 12, static_cast<int>(EMatchCompleteness::COMPLETE)), m_pSqlite.get(), pError, "bind completeness") &&
			   SqlSuccess(sqlite3_bind_int(pStatement.get(), 13, std::clamp(Filter.m_Limit, 1, 10000)), m_pSqlite.get(), pError, "bind limit");
	if(!Bound)
		return false;

	int Result;
	while((Result = sqlite3_step(pStatement.get())) == SQLITE_ROW)
	{
		CMatchHistoryEntry Entry;
		Entry.m_OriginId = reinterpret_cast<const char *>(sqlite3_column_text(pStatement.get(), 0));
		if(ParseUuid(&Entry.m_MatchId, reinterpret_cast<const char *>(sqlite3_column_text(pStatement.get(), 1))) != 0)
			return false;
		const int Source = sqlite3_column_int(pStatement.get(), 2);
		const int Completeness = sqlite3_column_int(pStatement.get(), 3);
		if(!ValidSource(Source) || !ValidCompleteness(Completeness))
			return false;
		Entry.m_Source = static_cast<EMatchReportSource>(Source);
		Entry.m_Completeness = static_cast<EMatchCompleteness>(Completeness);
		Entry.m_ModeId = reinterpret_cast<const char *>(sqlite3_column_text(pStatement.get(), 4));
		Entry.m_ModeSchemaVersion = sqlite3_column_int(pStatement.get(), 5);
		Entry.m_MapName = reinterpret_cast<const char *>(sqlite3_column_text(pStatement.get(), 6));
		Entry.m_EndTimeUtc = sqlite3_column_int64(pStatement.get(), 7);
		Entry.m_DurationTicks = sqlite3_column_int64(pStatement.get(), 8);
		Entry.m_TickRate = sqlite3_column_int(pStatement.get(), 9);
		if(sqlite3_column_type(pStatement.get(), 10) != SQLITE_NULL)
			Entry.m_LocalOutcome = static_cast<EMatchOutcome>(sqlite3_column_int(pStatement.get(), 10));
		if(sqlite3_column_type(pStatement.get(), 11) != SQLITE_NULL)
			Entry.m_LocalScore = sqlite3_column_int64(pStatement.get(), 11);
		if(sqlite3_column_type(pStatement.get(), 12) != SQLITE_NULL)
			Entry.m_LocalKills = sqlite3_column_int64(pStatement.get(), 12);
		if(sqlite3_column_type(pStatement.get(), 13) != SQLITE_NULL)
			Entry.m_LocalDeaths = sqlite3_column_int64(pStatement.get(), 13);
		vEntries.push_back(std::move(Entry));
	}
	return Result == SQLITE_DONE || SetSqlError(m_pSqlite.get(), pError, "list matches");
}

bool CMatchJournal::LoadMatch(const char *pOriginId, CUuid MatchId, CStoredMatch &Match, std::string *pError) const
{
	if(m_pSqlite == nullptr || pOriginId == nullptr)
		return false;
	CSqliteStmt pStatement;
	if(!Prepare(m_pSqlite.get(), R"sql(
SELECT m.source, m.completeness, m.local_participant_id, m.raw_report
FROM matches m JOIN origins o ON o.origin_id = m.origin_id
WHERE o.endpoint = ? AND m.match_id = ?
)sql",
		   pStatement, pError))
		return false;
	const std::string OriginId = pOriginId;
	const std::string MatchIdString = UuidString(MatchId);
	if(!BindText(m_pSqlite.get(), pStatement.get(), 1, OriginId, pError) || !BindText(m_pSqlite.get(), pStatement.get(), 2, MatchIdString, pError))
		return false;
	if(sqlite3_step(pStatement.get()) != SQLITE_ROW)
	{
		if(pError != nullptr)
			*pError = "match not found";
		return false;
	}
	const int Source = sqlite3_column_int(pStatement.get(), 0);
	const int Completeness = sqlite3_column_int(pStatement.get(), 1);
	if(!ValidSource(Source) || !ValidCompleteness(Completeness))
		return false;
	CStoredMatch Loaded;
	Loaded.m_OriginId = OriginId;
	Loaded.m_Source = static_cast<EMatchReportSource>(Source);
	Loaded.m_Completeness = static_cast<EMatchCompleteness>(Completeness);
	if(sqlite3_column_type(pStatement.get(), 2) != SQLITE_NULL)
		Loaded.m_LocalParticipantId = sqlite3_column_int(pStatement.get(), 2);
	Loaded.m_RawReport = reinterpret_cast<const char *>(sqlite3_column_text(pStatement.get(), 3));
	if(!MatchReportFromJson(Loaded.m_RawReport.data(), Loaded.m_RawReport.size(), Loaded.m_Report, pError))
		return false;
	Match = std::move(Loaded);
	return true;
}

bool CMatchJournal::QueryProfile(const CMatchProfileFilter &Filter, CMatchProfile &Profile, std::string *pError) const
{
	Profile = {};
	if(m_pSqlite == nullptr)
		return false;
	static const char *s_pSummarySql = R"sql(
SELECT COUNT(*),
 COALESCE(SUM(CASE WHEN s.outcome = ? THEN 1 ELSE 0 END), 0),
 COALESCE(SUM(CASE WHEN s.outcome = ? THEN 1 ELSE 0 END), 0),
 COALESCE(SUM(CASE WHEN s.outcome = ? THEN 1 ELSE 0 END), 0),
 COALESCE(SUM(COALESCE(p.value, m.duration_ticks) / m.tick_rate), 0)
FROM matches m
LEFT JOIN standings s ON s.origin_id = m.origin_id AND s.match_id = m.match_id
 AND s.subject_kind = ? AND s.subject_id = m.local_participant_id
LEFT JOIN metrics p ON p.origin_id = m.origin_id AND p.match_id = m.match_id
 AND p.subject_kind = ? AND p.subject_id = m.local_participant_id
 AND p.metric_id = m.mode_id || '/playtime_ticks'
WHERE m.completeness = ? AND m.local_participant_id IS NOT NULL
 AND m.end_time_utc >= ? AND (? = '' OR m.mode_id = ?)
)sql";
	CSqliteStmt pSummary;
	if(!Prepare(m_pSqlite.get(), s_pSummarySql, pSummary, pError))
		return false;
	const bool SummaryBound = SqlSuccess(sqlite3_bind_int(pSummary.get(), 1, static_cast<int>(EMatchOutcome::WIN)), m_pSqlite.get(), pError, "bind win") &&
				  SqlSuccess(sqlite3_bind_int(pSummary.get(), 2, static_cast<int>(EMatchOutcome::LOSS)), m_pSqlite.get(), pError, "bind loss") &&
				  SqlSuccess(sqlite3_bind_int(pSummary.get(), 3, static_cast<int>(EMatchOutcome::DRAW)), m_pSqlite.get(), pError, "bind draw") &&
				  SqlSuccess(sqlite3_bind_int(pSummary.get(), 4, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind subject kind") &&
				  SqlSuccess(sqlite3_bind_int(pSummary.get(), 5, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind playtime subject kind") &&
				  SqlSuccess(sqlite3_bind_int(pSummary.get(), 6, static_cast<int>(EMatchCompleteness::COMPLETE)), m_pSqlite.get(), pError, "bind complete") &&
				  SqlSuccess(sqlite3_bind_int64(pSummary.get(), 7, Filter.m_SinceUtc), m_pSqlite.get(), pError, "bind since") &&
				  BindText(m_pSqlite.get(), pSummary.get(), 8, Filter.m_ModeId, pError) && BindText(m_pSqlite.get(), pSummary.get(), 9, Filter.m_ModeId, pError);
	if(!SummaryBound || sqlite3_step(pSummary.get()) != SQLITE_ROW)
		return SetSqlError(m_pSqlite.get(), pError, "query profile summary");
	Profile.m_Matches = sqlite3_column_int(pSummary.get(), 0);
	Profile.m_Wins = sqlite3_column_int(pSummary.get(), 1);
	Profile.m_Losses = sqlite3_column_int(pSummary.get(), 2);
	Profile.m_Draws = sqlite3_column_int(pSummary.get(), 3);
	Profile.m_PlaytimeSeconds = sqlite3_column_int64(pSummary.get(), 4);

	static const char *s_pMetricSql = R"sql(
SELECT m.mode_id, m.mode_schema_version, x.metric_id, x.aggregation,
 CASE WHEN x.aggregation = ? THEN MAX(x.value) ELSE SUM(x.value) END
FROM matches m
JOIN metrics x ON x.origin_id = m.origin_id AND x.match_id = m.match_id
 AND x.subject_kind = ? AND x.subject_id = m.local_participant_id
WHERE m.completeness = ? AND m.local_participant_id IS NOT NULL
 AND m.end_time_utc >= ? AND (? = '' OR m.mode_id = ?)
 AND x.aggregation != ?
GROUP BY m.mode_id, m.mode_schema_version, x.metric_id, x.aggregation
ORDER BY m.mode_id, m.mode_schema_version, x.metric_id
)sql";
	CSqliteStmt pMetrics;
	if(!Prepare(m_pSqlite.get(), s_pMetricSql, pMetrics, pError))
		return false;
	const bool MetricsBound = SqlSuccess(sqlite3_bind_int(pMetrics.get(), 1, static_cast<int>(EMatchMetricAggregation::MAXIMUM)), m_pSqlite.get(), pError, "bind maximum aggregation") &&
				  SqlSuccess(sqlite3_bind_int(pMetrics.get(), 2, static_cast<int>(EMatchSubjectKind::PARTICIPANT)), m_pSqlite.get(), pError, "bind metric subject") &&
				  SqlSuccess(sqlite3_bind_int(pMetrics.get(), 3, static_cast<int>(EMatchCompleteness::COMPLETE)), m_pSqlite.get(), pError, "bind metric complete") &&
				  SqlSuccess(sqlite3_bind_int64(pMetrics.get(), 4, Filter.m_SinceUtc), m_pSqlite.get(), pError, "bind metric since") &&
				  BindText(m_pSqlite.get(), pMetrics.get(), 5, Filter.m_ModeId, pError) && BindText(m_pSqlite.get(), pMetrics.get(), 6, Filter.m_ModeId, pError) &&
				  SqlSuccess(sqlite3_bind_int(pMetrics.get(), 7, static_cast<int>(EMatchMetricAggregation::MATCH_ONLY)), m_pSqlite.get(), pError, "bind match-only aggregation");
	if(!MetricsBound)
		return false;
	int Result;
	while((Result = sqlite3_step(pMetrics.get())) == SQLITE_ROW)
	{
		CMatchMetricAggregate Metric;
		Metric.m_ModeId = reinterpret_cast<const char *>(sqlite3_column_text(pMetrics.get(), 0));
		Metric.m_ModeSchemaVersion = sqlite3_column_int(pMetrics.get(), 1);
		Metric.m_MetricId = reinterpret_cast<const char *>(sqlite3_column_text(pMetrics.get(), 2));
		Metric.m_Aggregation = static_cast<EMatchMetricAggregation>(sqlite3_column_int(pMetrics.get(), 3));
		Metric.m_Value = sqlite3_column_int64(pMetrics.get(), 4);
		Profile.m_vMetrics.push_back(std::move(Metric));
	}
	return Result == SQLITE_DONE || SetSqlError(m_pSqlite.get(), pError, "query profile metrics");
}

bool CMatchJournal::DeleteMatch(const char *pOriginId, CUuid MatchId, std::string *pError)
{
	if(m_pSqlite == nullptr || pOriginId == nullptr || !Execute("BEGIN IMMEDIATE", pError))
		return false;
	const auto FailDelete = [&]() {
		Rollback(pError);
		return false;
	};
	CSqliteStmt pStatement;
	if(!Prepare(m_pSqlite.get(), R"sql(
DELETE FROM matches WHERE match_id = ? AND origin_id = (SELECT origin_id FROM origins WHERE endpoint = ?)
)sql",
		   pStatement, pError))
		return FailDelete();
	const std::string MatchIdString = UuidString(MatchId);
	const std::string OriginId = pOriginId;
	if(!BindText(m_pSqlite.get(), pStatement.get(), 1, MatchIdString, pError) || !BindText(m_pSqlite.get(), pStatement.get(), 2, OriginId, pError) || !SqlSuccess(sqlite3_step(pStatement.get()), m_pSqlite.get(), pError, "delete match"))
		return FailDelete();
	if(sqlite3_changes(m_pSqlite.get()) == 0)
	{
		Rollback(nullptr);
		return false;
	}
	if(!Execute("DELETE FROM origins WHERE NOT EXISTS (SELECT 1 FROM matches WHERE matches.origin_id = origins.origin_id)", pError) || !Execute("COMMIT", pError))
		return FailDelete();
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
	if(m_pSqlite == nullptr)
		return false;
	CSqliteStmt pPageCount;
	CSqliteStmt pPageSize;
	CSqliteStmt pMatches;
	if(!Prepare(m_pSqlite.get(), "PRAGMA page_count", pPageCount, pError) || !Prepare(m_pSqlite.get(), "PRAGMA page_size", pPageSize, pError) || !Prepare(m_pSqlite.get(), "SELECT COUNT(*), MIN(end_time_utc) FROM matches", pMatches, pError))
		return false;
	if(sqlite3_step(pPageCount.get()) != SQLITE_ROW || sqlite3_step(pPageSize.get()) != SQLITE_ROW || sqlite3_step(pMatches.get()) != SQLITE_ROW)
		return SetSqlError(m_pSqlite.get(), pError, "query journal info");
	Info.m_DatabaseSize = sqlite3_column_int64(pPageCount.get(), 0) * sqlite3_column_int64(pPageSize.get(), 0);
	Info.m_NumMatches = sqlite3_column_int(pMatches.get(), 0);
	if(sqlite3_column_type(pMatches.get(), 1) != SQLITE_NULL)
		Info.m_OldestMatchUtc = sqlite3_column_int64(pMatches.get(), 1);
	return true;
}

bool PersistLiveStatsSnapshotOnDisconnect(CMatchJournal &Journal, ESessionSourceType SourceType, bool SavingEnabled, bool IsCurrentMatch, bool HasFinalServerReport, const std::optional<CStoredMatch> &ObservedMatch, const CLiveStatsAssembler &LiveStats, std::string *pError)
{
	const std::optional<CStoredMatch> &Live = LiveStats.Latest();
	if(!Live.has_value() || !LiveStats.LatestPersistOnDisconnect() || Live->m_Source != EMatchReportSource::SERVER_SNAPSHOT || !IsCurrentMatch || HasFinalServerReport)
		return true;
	if(!ShouldPersistMatchReport(SourceType, SavingEnabled, Live->m_LocalParticipantId.has_value()) || !Journal.IsOpen())
		return true;

	const bool ReplaceObserved = ShouldReplaceObservedMatch(ObservedMatch, *Live);
	const CMatchJournal::EInsertResult Result = ReplaceObserved ? Journal.InsertReplacingObserved(*Live, ObservedMatch->m_OriginId.c_str(), ObservedMatch->m_Report.m_MatchId, pError) : Journal.Insert(*Live, pError);
	return Result != CMatchJournal::EInsertResult::ERROR;
}

namespace
{
	// Deterministic so that two runs produce the same journal and a reported
	// oddity can be looked at again.
	class CSampleRandom
	{
		uint64_t m_State = 0x2545f4914f6cdd1dULL;

	public:
		int Next(int Min, int Max)
		{
			m_State = m_State * 6364136223846793005ULL + 1442695040888963407ULL;
			return Min + static_cast<int>((m_State >> 33) % static_cast<uint64_t>(Max - Min + 1));
		}
	};

	class CSampleMode
	{
	public:
		const char *m_pModeId;
		const char *m_pMapName;
		bool m_Teams;
		// A race is measured in run times instead of in frags, which is the one
		// sample shape that has no weapon statistics at all
		bool m_Race;
	};
}

bool GenerateSampleMatches(CMatchJournal &Journal, int Count, const char *pLocalName, std::string *pError)
{
	if(!Journal.IsOpen())
	{
		if(pError != nullptr)
			*pError = "match journal is unavailable";
		return false;
	}

	static const CSampleMode s_aModes[] = {
		{"vanilla.ctf@ddnet.org", "ctf1", true, false},
		{"vanilla.dm@ddnet.org", "dm1", false, false},
		{"vanilla.tdm@ddnet.org", "dm2", true, false},
		{"insta.ictf@ddnet.org", "ctf5", true, false},
		{"insta.idm@ddnet.org", "dm6", false, false},
		{"vanilla.1on1@ddnet.org", "dm1", false, false},
		{"ddrace.race@ddnet.org", "Kobra4", false, true},
	};
	static const char *const s_apNames[] = {"nameless tee", "Chillerdragon", "deen", "Learath2", "Ryozuki", "heinrich5991", "Jupeyy", "fokkonaut", "Zwelf", "trml", "Patiga", "murpi"};
	static const char *const s_apClans[] = {"", "DDNet", "Chillerbot", "", "iMebi", "", "GER", ""};
	static const char *const s_apOrigins[] = {"ger10.ddnet.org:8303", "nl2.ddnet.org:8305", "chi2.ddnet.org:8303"};

	CSampleRandom Random;
	const int64_t Now = time_timestamp();
	for(int Match = 0; Match < Count; ++Match)
	{
		const CSampleMode &Mode = s_aModes[Random.Next(0, std::size(s_aModes) - 1)];
		const int NumParticipants = Mode.m_Teams ? Random.Next(6, 12) : Random.Next(2, 8);
		const int TickRate = 50;
		const int64_t DurationTicks = static_cast<int64_t>(Random.Next(240, 900)) * TickRate;
		const int64_t EndTimeUtc = Now - static_cast<int64_t>(Match) * Random.Next(2, 30) * 3600;

		CMatchReport Report;
		Report.m_MatchId = RandomUuid();
		Report.m_ModeId = Mode.m_pModeId;
		Report.m_MapName = Mode.m_pMapName;
		Report.m_StartTimeUtc = EndTimeUtc - DurationTicks / TickRate;
		Report.m_EndTimeUtc = EndTimeUtc;
		Report.m_DurationTicks = DurationTicks;
		Report.m_TickRate = TickRate;
		Report.m_Ranked = true;
		CMatchReportBuilder Builder(std::move(Report));

		const auto MetricId = [&Mode](const char *pSuffix) { return std::string(Mode.m_pModeId) + "/" + pSuffix; };
		if(Mode.m_Teams)
		{
			Builder.AddTeam({0, "Red"});
			Builder.AddTeam({1, "Blue"});
		}

		// The local player is participant 0 and keeps a plausible spread of
		// results so that the profile page shows wins as well as losses.
		std::vector<int> vScores(NumParticipants);
		for(int Participant = 0; Participant < NumParticipants; ++Participant)
		{
			CMatchParticipant Entry;
			Entry.m_ParticipantId = Participant;
			Entry.m_DisplayName = Participant == 0 ? pLocalName : s_apNames[(Participant + Match) % std::size(s_apNames)];
			Entry.m_Clan = s_apClans[(Participant + Match) % std::size(s_apClans)];
			if(Mode.m_Teams)
				Entry.m_TeamId = Participant % 2;
			if(Participant != 0 && Random.Next(0, 5) == 0)
				Entry.m_LeftTick = Random.Next(1, static_cast<int>(DurationTicks));
			Builder.AddParticipant(Entry);
			vScores[Participant] = Random.Next(0, 40);
		}

		std::vector<int> vRanking(NumParticipants);
		for(int Participant = 0; Participant < NumParticipants; ++Participant)
			vRanking[Participant] = Participant;
		std::stable_sort(vRanking.begin(), vRanking.end(), [&vScores](int Left, int Right) { return vScores[Left] > vScores[Right]; });

		int aTeamScores[2] = {0, 0};
		for(int Rank = 0; Rank < NumParticipants; ++Rank)
		{
			const int Participant = vRanking[Rank];
			const int Score = vScores[Participant];
			if(Mode.m_Teams)
				aTeamScores[Participant % 2] += Score;
			const auto AddParticipantMetric = [&](const char *pSuffix, int64_t Value, EMatchMetricAggregation Aggregation = EMatchMetricAggregation::SUM) {
				Builder.AddMetric({EMatchSubjectKind::PARTICIPANT, Participant, MetricId(pSuffix), Value, Aggregation});
			};
			if(Mode.m_Race)
			{
				// A higher sample score means a faster run, so the ranking above
				// stays the ranking of the race.
				const int64_t RunTicks = static_cast<int64_t>(700 - Score * 12) * TickRate;
				const bool Finished = Score > 5;
				Builder.AddStanding({EMatchSubjectKind::PARTICIPANT, Participant, Rank + 1, Finished ? EMatchOutcome::FINISHED : EMatchOutcome::DNF});
				AddParticipantMetric("playtime_ticks", DurationTicks);
				AddParticipantMetric("map_rank", Random.Next(1, 4000), EMatchMetricAggregation::MATCH_ONLY);
				AddParticipantMetric("map_finishes", Random.Next(1, 400), EMatchMetricAggregation::MATCH_ONLY);
				AddParticipantMetric("session_finishes", Random.Next(0, 4));
				AddParticipantMetric("map_best_ticks", static_cast<int64_t>(Random.Next(180, 260)) * TickRate, EMatchMetricAggregation::MATCH_ONLY);
				if(Finished)
				{
					AddParticipantMetric("personal_best_ticks", RunTicks, EMatchMetricAggregation::MATCH_ONLY);
					AddParticipantMetric("last_finish_ticks", RunTicks, EMatchMetricAggregation::MATCH_ONLY);
				}
				else
				{
					AddParticipantMetric("current_run_ticks", RunTicks / 2, EMatchMetricAggregation::MATCH_ONLY);
					AddParticipantMetric("current_checkpoint", Random.Next(0, 24), EMatchMetricAggregation::MATCH_ONLY);
				}
				continue;
			}
			const EMatchOutcome Outcome = Mode.m_Teams ? EMatchOutcome::FINISHED : Rank == 0 ? EMatchOutcome::WIN :
													   EMatchOutcome::LOSS;
			Builder.AddStanding({EMatchSubjectKind::PARTICIPANT, Participant, Rank + 1, Outcome});
			const int Deaths = Random.Next(0, 30);
			AddParticipantMetric("score", Score);
			AddParticipantMetric("kills", Score);
			AddParticipantMetric("deaths", Deaths);
			AddParticipantMetric("assists", Random.Next(0, 8));
			AddParticipantMetric("suicides", Random.Next(0, 4));
			AddParticipantMetric("best_spree", Random.Next(0, 12), EMatchMetricAggregation::MAXIMUM);
			AddParticipantMetric("health_picked_up", Random.Next(0, 40));
			AddParticipantMetric("armor_picked_up", Random.Next(0, 30));
			// The weapon breakdown has to add up to the totals above, otherwise
			// the frag and death pages would contradict the score page.
			int RemainingKills = Score;
			int RemainingDeaths = Deaths;
			int RemainingHeld = Deaths;
			for(int Weapon = WEAPON_HAMMER; Weapon <= WEAPON_LASER; ++Weapon)
			{
				const bool Last = Weapon == WEAPON_LASER;
				const int Kills = Last ? RemainingKills : Random.Next(0, RemainingKills);
				const int WeaponDeaths = Last ? RemainingDeaths : Random.Next(0, RemainingDeaths);
				const int Held = Last ? RemainingHeld : Random.Next(0, RemainingHeld);
				RemainingKills -= Kills;
				RemainingDeaths -= WeaponDeaths;
				RemainingHeld -= Held;
				char aSuffix[32];
				if(Kills != 0)
				{
					str_format(aSuffix, sizeof(aSuffix), "weapon_%d_kills", Weapon);
					AddParticipantMetric(aSuffix, Kills);
				}
				if(WeaponDeaths != 0)
				{
					str_format(aSuffix, sizeof(aSuffix), "weapon_%d_deaths", Weapon);
					AddParticipantMetric(aSuffix, WeaponDeaths);
				}
				if(Held != 0)
				{
					str_format(aSuffix, sizeof(aSuffix), "weapon_%d_deaths_holding", Weapon);
					AddParticipantMetric(aSuffix, Held);
				}
				const int Shots = Random.Next(0, 160);
				if(Shots == 0)
					continue;
				const int Hits = Shots * Random.Next(10, 70) / 100;
				str_format(aSuffix, sizeof(aSuffix), "weapon_%d_shots", Weapon);
				AddParticipantMetric(aSuffix, Shots);
				str_format(aSuffix, sizeof(aSuffix), "weapon_%d_hits", Weapon);
				AddParticipantMetric(aSuffix, Hits);
				str_format(aSuffix, sizeof(aSuffix), "weapon_%d_damage_done", Weapon);
				AddParticipantMetric(aSuffix, Hits * Random.Next(1, 6));
				str_format(aSuffix, sizeof(aSuffix), "weapon_%d_damage_taken", Weapon);
				AddParticipantMetric(aSuffix, Random.Next(0, 90));
			}
			if(str_find(Mode.m_pModeId, "ctf") != nullptr)
			{
				AddParticipantMetric("flag_grabs", Random.Next(0, 9));
				AddParticipantMetric("flag_returns", Random.Next(0, 6));
				AddParticipantMetric("flag_captures", Random.Next(0, 4));
			}
		}

		if(Mode.m_Teams)
		{
			for(int Team = 0; Team < 2; ++Team)
			{
				const bool Won = aTeamScores[Team] > aTeamScores[1 - Team];
				const bool Drawn = aTeamScores[Team] == aTeamScores[1 - Team];
				Builder.AddStanding({EMatchSubjectKind::TEAM, Team, Won || Drawn ? 1 : 2, Drawn ? EMatchOutcome::DRAW : Won ? EMatchOutcome::WIN :
																	      EMatchOutcome::LOSS});
				Builder.AddMetric({EMatchSubjectKind::TEAM, Team, MetricId("score"), aTeamScores[Team], EMatchMetricAggregation::SUM});
			}
		}
		Builder.AddMetric({EMatchSubjectKind::MATCH, std::nullopt, MetricId("playtime_ticks"), DurationTicks, EMatchMetricAggregation::SUM});

		CStoredMatch Stored;
		if(!Builder.Finalize(pError) || !MatchReportToJson(Builder.Report(), Stored.m_RawReport, pError))
			return false;
		Stored.m_Report = Builder.Report();
		Stored.m_OriginId = s_apOrigins[Match % std::size(s_apOrigins)];
		Stored.m_Source = Match % 3 == 0 ? EMatchReportSource::CLIENT_OBSERVED : EMatchReportSource::SERVER_REPORT;
		Stored.m_Completeness = Match % 7 == 0 ? EMatchCompleteness::PARTIAL_SINCE_JOIN : EMatchCompleteness::COMPLETE;
		Stored.m_LocalParticipantId = 0;
		if(Journal.Insert(Stored, pError) == CMatchJournal::EInsertResult::ERROR)
			return false;
	}
	return true;
}
