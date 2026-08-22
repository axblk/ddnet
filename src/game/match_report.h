#ifndef GAME_MATCH_REPORT_H
#define GAME_MATCH_REPORT_H

#include <base/hash.h>

#include <engine/shared/network.h>
#include <engine/shared/uuid_manager.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class CJsonWriter;

namespace MatchReportLimits
{
	inline constexpr int MAX_TEAMS = 64;
	inline constexpr int MAX_PARTICIPANTS = 128;
	inline constexpr int MAX_STANDINGS = MAX_PARTICIPANTS + MAX_TEAMS;
	// score, playtime, the combat totals and six counters for each weapon, with room for what a mode adds
	inline constexpr int MAX_METRICS_PER_PARTICIPANT = 64;
	inline constexpr int MAX_METRICS = MAX_PARTICIPANTS * MAX_METRICS_PER_PARTICIPANT + MAX_TEAMS * 16 + 256;
	inline constexpr int MAX_PAYLOAD_SIZE = MAX_METRICS * 32;
	// profiles sum values across all stored matches, this keeps the sum far from overflowing
	inline constexpr int64_t MAX_METRIC_VALUE = 1000000000000LL;
	inline constexpr int MAX_ID_LENGTH = 64;
	inline constexpr int MAX_MAP_NAME_LENGTH = 128;
	inline constexpr int MAX_DISPLAY_NAME_LENGTH = 64;
	inline constexpr int MAX_CLAN_LENGTH = 64;
	// 3000-01-01, within the supported range of Windows timestamp conversion
	inline constexpr int64_t MAX_TIME_UTC = 32503680000LL;
	inline constexpr int64_t MAX_DURATION_TICKS = 366LL * 24 * 60 * 60 * 1000;
	inline constexpr int MAX_CHUNK_SIZE = NET_MAX_CHUNK_SIZE - 128;
}

enum class EMatchTermination
{
	COMPLETED,
	ABORTED,
	ADMIN_ENDED,
	NUM,
};

enum class EMatchSubjectKind
{
	MATCH,
	PARTICIPANT,
	TEAM,
	NUM,
};

// How the profile combines a metric across matches
enum class EMatchMetricAggregation
{
	SUM,
	MAXIMUM,
	MATCH_ONLY,
	NUM,
};

enum class EMatchOutcome
{
	WIN,
	LOSS,
	DRAW,
	FINISHED,
	DNF,
	DISQUALIFIED,
	NUM,
};

enum class EMatchMetricCategory
{
	OVERVIEW,
	COMBAT,
	OBJECTIVES,
	OTHER,
};

enum class EMatchMetricFormat
{
	NUMBER,
	TICKS,
	RANK,
};

// The combat counters, reported in total and as `weapon_<weapon>_<counter>`
enum EMatchCombatStat
{
	MATCH_COMBAT_KILLS,
	MATCH_COMBAT_DEATHS,
	MATCH_COMBAT_SHOTS,
	MATCH_COMBAT_HITS,
	MATCH_COMBAT_DAMAGE_DONE,
	MATCH_COMBAT_DAMAGE_TAKEN,
	NUM_MATCH_COMBAT_STATS,
};

class CMatchMetricInfo
{
public:
	const char *m_pId;
	EMatchMetricAggregation m_Aggregation;
	EMatchMetricCategory m_Category;
	EMatchMetricFormat m_Format;
	// not localized yet, pass it through Localize
	const char *m_pName;
	// one of EMatchCombatStat, or -1
	int m_CombatStat;
};

/**
 * Looks up a metric this build knows.
 *
 * @param Id The metric id, for example `kills` or `weapon_2_hits`.
 * @param pWeapon Set to the weapon of a per-weapon counter, -1 otherwise.
 *
 * @return nullptr for a metric this build does not know, a mod's own for example.
 */
const CMatchMetricInfo *FindMatchMetric(std::string_view Id, int *pWeapon = nullptr);
const CMatchMetricInfo &MatchCombatStatInfo(int CombatStat);

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
	// the name the server's game mode is registered under
	std::string m_ModeId;
	std::string m_MapName;
	SHA256_DIGEST m_MapSha256 = {};
	int64_t m_StartTimeUtc = 0;
	int64_t m_EndTimeUtc = 0;
	int64_t m_DurationTicks = 0;
	int m_TickRate = 0;
	int m_RoundStartTick = 0;
	EMatchTermination m_Termination = EMatchTermination::COMPLETED;
	std::vector<CMatchTeam> m_vTeams;
	std::vector<CMatchParticipant> m_vParticipants;
	std::vector<CMatchStanding> m_vStandings;
	std::vector<CMatchMetric> m_vMetrics;

	const CMatchParticipant *Participant(int ParticipantId) const;
	const CMatchStanding *Standing(EMatchSubjectKind SubjectKind, int SubjectId) const;
	std::optional<int64_t> Metric(EMatchSubjectKind SubjectKind, int SubjectId, const char *pMetricId) const;
};

const char *MatchTerminationName(EMatchTermination Termination);
const char *MatchSubjectKindName(EMatchSubjectKind SubjectKind);
const char *MatchOutcomeName(EMatchOutcome Outcome);
const char *MatchMetricAggregationName(EMatchMetricAggregation Aggregation);

bool IsValidMatchReportId(const std::string &Id);
bool MatchReportValidate(const CMatchReport &Report, std::string *pError);
void MatchReportWriteJson(CJsonWriter &Writer, const CMatchReport &Report);

/**
 * Packs a report for the network and the local journal.
 *
 * Every participant carries the same metric ids, so every distinct string is
 * written once and referenced by index, and the numbers are variable length.
 * The result carries its own string table, so it can be stored and read back
 * without knowing the mode.
 */
bool MatchReportToPacked(const CMatchReport &Report, std::string &Packed, std::string *pError);
bool MatchReportFromPacked(const void *pData, size_t Size, CMatchReport &Report, std::string *pError);

#endif // GAME_MATCH_REPORT_H
