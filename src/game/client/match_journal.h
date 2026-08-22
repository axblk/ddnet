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
	SERVER_SNAPSHOT,
	NUM,
};

enum class EMatchCompleteness
{
	COMPLETE,
	PARTIAL_SINCE_JOIN,
	ABORTED,
	NUM,
};

const char *MatchReportSourceName(EMatchReportSource Source);
const char *MatchCompletenessName(EMatchCompleteness Completeness);

/**
 * Decides whether a report of a round takes the place of the one the journal
 * holds under the same server and match id.
 *
 * The client keeps the live snapshot it had when the connection broke, and
 * after a reconnect the server sends the finished report of the same round.
 * The one that knows less gives way: first by who said it, then by how much of
 * the round it covers.
 */
bool MatchSupersedes(EMatchReportSource NewSource, EMatchCompleteness NewCompleteness, EMatchReportSource StoredSource, EMatchCompleteness StoredCompleteness);

class CStoredMatch
{
public:
	// the address of the server
	std::string m_OriginId;
	EMatchReportSource m_Source = EMatchReportSource::CLIENT_OBSERVED;
	EMatchCompleteness m_Completeness = EMatchCompleteness::ABORTED;
	std::optional<int> m_LocalParticipantId;
	CMatchReport m_Report;
};

class CMatchHistoryEntry
{
public:
	std::string m_OriginId;
	CUuid m_MatchId = UUID_ZEROED;
	EMatchReportSource m_Source = EMatchReportSource::CLIENT_OBSERVED;
	EMatchCompleteness m_Completeness = EMatchCompleteness::ABORTED;
	std::string m_ModeId;
	std::string m_MapName;
	int64_t m_EndTimeUtc = 0;
	int64_t m_DurationTicks = 0;
	int m_TickRate = 0;
	std::optional<EMatchOutcome> m_LocalOutcome;
	std::optional<int64_t> m_LocalScore;
};

class CMatchMetricAggregate
{
public:
	std::string m_ModeId;
	std::string m_MetricId;
	int64_t m_Value = 0;
	EMatchMetricAggregation m_Aggregation = EMatchMetricAggregation::SUM;
};

class CMatchProfileFilter
{
public:
	int64_t m_SinceUtc = 0;
	// empty for every mode
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

// The matches the local player played, stored on this device.
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

public:
	bool Open(IStorage *pStorage, std::string *pError);
	bool IsOpen() const { return m_pSqlite != nullptr; }

	/**
	 * @param Match The match, it must have a local participant.
	 * @param pReplacedObserved What the client observed of the same round, dropped in favour of the server's report.
	 * @param pError Where what went wrong is put.
	 */
	EInsertResult Insert(const CStoredMatch &Match, const CStoredMatch *pReplacedObserved, std::string *pError);
	// the newest matches first
	bool ListMatches(std::vector<CMatchHistoryEntry> &vEntries, std::string *pError) const;
	bool LoadMatch(const char *pOriginId, CUuid MatchId, CStoredMatch &Match, std::string *pError) const;
	bool QueryProfile(const CMatchProfileFilter &Filter, CMatchProfile &Profile, std::string *pError) const;
	bool DeleteMatch(const char *pOriginId, CUuid MatchId, std::string *pError);
	bool DeleteAll(std::string *pError);
	bool Info(CMatchJournalInfo &Info, std::string *pError) const;
};

#endif // GAME_CLIENT_MATCH_JOURNAL_H
