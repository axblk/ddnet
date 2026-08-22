#ifndef GAME_MATCH_REPORT_H
#define GAME_MATCH_REPORT_H

#include <base/hash.h>

#include <engine/shared/network.h>
#include <engine/shared/uuid_manager.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace MatchReportLimits
{
	inline constexpr int MAX_PAYLOAD_SIZE = 256 * 1024;
	inline constexpr int MAX_TEAMS = 64;
	inline constexpr int MAX_PARTICIPANTS = 128;
	inline constexpr int MAX_STANDINGS = MAX_PARTICIPANTS + MAX_TEAMS;
	inline constexpr int MAX_METRICS = 4096;
	inline constexpr int MAX_MODE_ID_LENGTH = 96;
	inline constexpr int MAX_METRIC_ID_LENGTH = 128;
	inline constexpr int MAX_MAP_NAME_LENGTH = 128;
	inline constexpr int MAX_DISPLAY_NAME_LENGTH = 64;
	inline constexpr int MAX_CLAN_LENGTH = 64;
	inline constexpr int MAX_REASON_LENGTH = 64;
	// 3000-01-01, within the supported range of Windows timestamp conversion.
	inline constexpr int64_t MAX_TIME_UTC = 32503680000LL;
	inline constexpr int64_t MAX_DURATION_TICKS = 366LL * 24 * 60 * 60 * 1000;
}

namespace MatchReportTransportLimits
{
	inline constexpr int MAX_CHUNK_SIZE = NET_MAX_CHUNK_SIZE - 128;
	inline constexpr int MAX_CHUNKS = (MatchReportLimits::MAX_PAYLOAD_SIZE + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;
}

enum class EMatchTermination
{
	COMPLETED,
	ABORTED,
	ADMIN_ENDED,
};

enum class EMatchSubjectKind
{
	MATCH,
	PARTICIPANT,
	TEAM,
};

enum class EMatchMetricAggregation
{
	INVALID = -1,
	SUM = 0,
	MAXIMUM = 1,
	MATCH_ONLY = 2,
};

enum class EMatchOutcome
{
	WIN,
	LOSS,
	DRAW,
	FINISHED,
	DNF,
	DISQUALIFIED,
};

class CMatchTeam
{
public:
	int m_TeamId = 0;
	std::string m_DisplayName;
};

class CMatchParticipant
{
public:
	int m_ParticipantId = 0;
	std::optional<int> m_TeamId;
	std::string m_DisplayName;
	std::string m_Clan;
	int64_t m_JoinedTick = 0;
	std::optional<int64_t> m_LeftTick;
	bool m_Bot = false;
};

class CMatchStanding
{
public:
	EMatchSubjectKind m_SubjectKind = EMatchSubjectKind::PARTICIPANT;
	int m_SubjectId = 0;
	int m_Rank = 0;
	EMatchOutcome m_Outcome = EMatchOutcome::FINISHED;
};

class CMatchMetric
{
public:
	EMatchSubjectKind m_SubjectKind = EMatchSubjectKind::MATCH;
	std::optional<int> m_SubjectId;
	std::string m_MetricId;
	int64_t m_Value = 0;
	EMatchMetricAggregation m_Aggregation = EMatchMetricAggregation::SUM;
};

class CMatchReport
{
public:
	CUuid m_MatchId = UUID_ZEROED;
	std::optional<CUuid> m_GameUuid;
	int m_ReportSchemaVersion = 1;
	std::string m_ModeId;
	int m_ModeSchemaVersion = 1;
	std::string m_MapName;
	SHA256_DIGEST m_MapSha256 = {};
	int64_t m_StartTimeUtc = 0;
	int64_t m_EndTimeUtc = 0;
	int64_t m_DurationTicks = 0;
	int m_TickRate = 0;
	int m_RoundStartTick = 0;
	EMatchTermination m_Termination = EMatchTermination::COMPLETED;
	bool m_Ranked = false;
	std::string m_UnrankedReason;
	std::vector<CMatchTeam> m_vTeams;
	std::vector<CMatchParticipant> m_vParticipants;
	std::vector<CMatchStanding> m_vStandings;
	std::vector<CMatchMetric> m_vMetrics;
};

const char *MatchTerminationName(EMatchTermination Termination);
const char *MatchSubjectKindName(EMatchSubjectKind SubjectKind);
const char *MatchOutcomeName(EMatchOutcome Outcome);
const char *MatchMetricAggregationName(EMatchMetricAggregation Aggregation);

bool IsValidMatchReportModeId(const std::string &ModeId);
bool IsValidMatchReportMetricId(const std::string &MetricId, const std::string &ModeId);
bool MatchReportValidate(const CMatchReport &Report, std::string *pError);
bool MatchReportToJson(const CMatchReport &Report, std::string &Json, std::string *pError);
bool MatchReportFromJson(const char *pJson, size_t JsonSize, CMatchReport &Report, std::string *pError);

class CMatchReportBuilder
{
	CMatchReport m_Report;
	bool m_Finalized = false;
	bool m_MutationFailed = false;

	CMatchMetric *FindMetric(EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pMetricId);
	bool MutationFailed();

public:
	explicit CMatchReportBuilder(CMatchReport Report);

	bool AddTeam(CMatchTeam Team);
	bool AddParticipant(CMatchParticipant Participant);
	bool AddStanding(CMatchStanding Standing);
	bool AddMetric(CMatchMetric Metric);
	bool AddMetricValue(EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pMetricId, int64_t Value, EMatchMetricAggregation Aggregation);
	bool SetMetricValue(EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pMetricId, int64_t Value, EMatchMetricAggregation Aggregation);
	bool Finalize(std::string *pError);
	bool Finalize(std::string *pError, std::string *pJson);

	bool IsFinalized() const { return m_Finalized; }
	CMatchReport &Report() { return m_Report; }
	const CMatchReport &Report() const { return m_Report; }
};

#endif // GAME_MATCH_REPORT_H
