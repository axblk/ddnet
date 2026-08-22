#ifndef GAME_CLIENT_MATCH_JOURNAL_H
#define GAME_CLIENT_MATCH_JOURNAL_H

#include <engine/sqlite.h>

#include <game/match_report.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class IStorage;

enum class EMatchReportSource
{
	CLIENT_OBSERVED,
	SERVER_REPORT,
	SERVER_DATABASE,
	SERVER_SNAPSHOT,
};

enum class EMatchCompleteness
{
	COMPLETE,
	PARTIAL_SINCE_JOIN,
	ABORTED,
};

const char *MatchReportSourceName(EMatchReportSource Source);
const char *MatchCompletenessName(EMatchCompleteness Completeness);

class CStoredMatch
{
public:
	std::string m_OriginId;
	EMatchReportSource m_Source = EMatchReportSource::CLIENT_OBSERVED;
	EMatchCompleteness m_Completeness = EMatchCompleteness::ABORTED;
	std::optional<int> m_LocalParticipantId;
	CMatchReport m_Report;
	std::string m_RawReport;
};

class CMatchHistoryFilter
{
public:
	std::string m_ModeId;
	std::string m_MapName;
	std::string m_OriginId;
	bool m_IncludeIncomplete = true;
	int m_Limit = 10000;
};

class CMatchHistoryEntry
{
public:
	std::string m_OriginId;
	CUuid m_MatchId = UUID_ZEROED;
	EMatchReportSource m_Source = EMatchReportSource::CLIENT_OBSERVED;
	EMatchCompleteness m_Completeness = EMatchCompleteness::ABORTED;
	std::string m_ModeId;
	int m_ModeSchemaVersion = 0;
	std::string m_MapName;
	int64_t m_EndTimeUtc = 0;
	int64_t m_DurationTicks = 0;
	int m_TickRate = 0;
	std::optional<EMatchOutcome> m_LocalOutcome;
	std::optional<int64_t> m_LocalScore;
	// Kept in the list so that a kill/death column costs one query instead of
	// loading and parsing every report of the list
	std::optional<int64_t> m_LocalKills;
	std::optional<int64_t> m_LocalDeaths;
};

class CMatchMetricAggregate
{
public:
	std::string m_ModeId;
	int m_ModeSchemaVersion = 0;
	std::string m_MetricId;
	int64_t m_Value = 0;
	EMatchMetricAggregation m_Aggregation = EMatchMetricAggregation::SUM;
};

class CMatchProfileFilter
{
public:
	int64_t m_SinceUtc = 0;
	std::string m_ModeId;
};

class CMatchProfile
{
public:
	int m_Matches = 0;
	int m_Wins = 0;
	int m_Losses = 0;
	int m_Draws = 0;
	int64_t m_PlaytimeSeconds = 0;
	std::vector<CMatchMetricAggregate> m_vMetrics;
};

class CMatchJournalInfo
{
public:
	int64_t m_DatabaseSize = 0;
	std::optional<int64_t> m_OldestMatchUtc;
	int m_NumMatches = 0;
};

class CMatchJournal
{
public:
	enum class EInsertResult
	{
		INSERTED,
		DUPLICATE,
		ERROR,
	};

private:
	IStorage *m_pStorage = nullptr;
	CSqlite m_pSqlite;

	bool Execute(const char *pStatement, std::string *pError);
	bool Rollback(std::string *pError);
	EInsertResult InsertImpl(const CStoredMatch &Match, const char *pObservedOriginId, CUuid ObservedMatchId, std::string *pError);

public:
	bool Open(IStorage *pStorage, std::string *pError);
	bool IsOpen() const { return m_pSqlite != nullptr; }

	EInsertResult Insert(const CStoredMatch &Match, std::string *pError);
	EInsertResult InsertReplacingObserved(const CStoredMatch &Match, const char *pObservedOriginId, CUuid ObservedMatchId, std::string *pError);
	bool ListMatches(const CMatchHistoryFilter &Filter, std::vector<CMatchHistoryEntry> &vEntries, std::string *pError) const;
	bool LoadMatch(const char *pOriginId, CUuid MatchId, CStoredMatch &Match, std::string *pError) const;
	bool QueryProfile(const CMatchProfileFilter &Filter, CMatchProfile &Profile, std::string *pError) const;
	bool DeleteMatch(const char *pOriginId, CUuid MatchId, std::string *pError);
	bool DeleteAll(std::string *pError);
	bool Info(CMatchJournalInfo &Info, std::string *pError) const;
};

/**
 * Fills the journal with matches that look like played ones, so that the
 * statistics pages can be looked at without playing a season first.
 *
 * @param Journal Journal that takes the matches, must be open.
 * @param Count Number of matches to add, spread over the last weeks.
 * @param pLocalName Name of the local participant in the generated matches.
 * @param pError Set to the reason when the function returns false.
 *
 * @return true when all matches were stored.
 */
bool GenerateSampleMatches(CMatchJournal &Journal, int Count, const char *pLocalName, std::string *pError);

#endif // GAME_CLIENT_MATCH_JOURNAL_H
