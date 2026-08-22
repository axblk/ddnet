#include "match_report.h"

#include <base/dbg.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/shared/jsonwriter.h>
#include <engine/shared/localization.h>

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>
#include <unordered_map>

// The combat counters come first, in the order of EMatchCombatStat.
static const CMatchMetricInfo gs_aMatchMetrics[] = {
	{"kills", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Kills"), MATCH_COMBAT_KILLS},
	{"deaths", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Deaths"), MATCH_COMBAT_DEATHS},
	{"shots", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Shots"), MATCH_COMBAT_SHOTS},
	{"hits", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Hits"), MATCH_COMBAT_HITS},
	{"damage_done", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Damage done"), MATCH_COMBAT_DAMAGE_DONE},
	{"damage_taken", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Damage taken"), MATCH_COMBAT_DAMAGE_TAKEN},
	{"suicides", EMatchMetricAggregation::SUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Suicides"), -1},
	{"best_spree", EMatchMetricAggregation::MAXIMUM, EMatchMetricCategory::COMBAT, EMatchMetricFormat::NUMBER, Localizable("Best spree"), -1},
	{"score", EMatchMetricAggregation::SUM, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::NUMBER, Localizable("Score"), -1},
	{"playtime_ticks", EMatchMetricAggregation::SUM, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::TICKS, Localizable("Play time"), -1},
	{"sudden_death", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::NUMBER, Localizable("Sudden death"), -1},
	{"flag_grabs", EMatchMetricAggregation::SUM, EMatchMetricCategory::OBJECTIVES, EMatchMetricFormat::NUMBER, Localizable("Flag grabs"), -1},
	{"flag_returns", EMatchMetricAggregation::SUM, EMatchMetricCategory::OBJECTIVES, EMatchMetricFormat::NUMBER, Localizable("Flag returns"), -1},
	{"flag_captures", EMatchMetricAggregation::SUM, EMatchMetricCategory::OBJECTIVES, EMatchMetricFormat::NUMBER, Localizable("Flag captures"), -1},
	{"catches", EMatchMetricAggregation::SUM, EMatchMetricCategory::OBJECTIVES, EMatchMetricFormat::NUMBER, Localizable("Catches"), -1},
	{"personal_best_ticks", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::TICKS, Localizable("Personal best"), -1},
	{"map_best_ticks", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::TICKS, Localizable("Map best"), -1},
	{"map_rank", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::RANK, Localizable("Map rank"), -1},
	{"map_finishes", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::NUMBER, Localizable("Stored map finishes"), -1},
	{"session_finishes", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::NUMBER, Localizable("Session finishes"), -1},
	{"last_finish_ticks", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::TICKS, Localizable("Last finish"), -1},
	{"current_run_ticks", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::TICKS, Localizable("Current run"), -1},
	{"current_checkpoint", EMatchMetricAggregation::MATCH_ONLY, EMatchMetricCategory::OVERVIEW, EMatchMetricFormat::NUMBER, Localizable("Current checkpoint"), -1},
};

const CMatchMetricInfo &MatchCombatStatInfo(int CombatStat)
{
	dbg_assert(CombatStat >= 0 && CombatStat < NUM_MATCH_COMBAT_STATS, "invalid combat stat %d", CombatStat);
	return gs_aMatchMetrics[CombatStat];
}

const CMatchMetricInfo *FindMatchMetric(std::string_view Id, int *pWeapon)
{
	int Weapon = -1;
	if(Id.starts_with("weapon_"))
	{
		Id.remove_prefix(7);
		const size_t Separator = Id.find('_');
		if(Separator == std::string_view::npos || Separator == 0 || Separator > 2 || !std::all_of(Id.begin(), Id.begin() + Separator, [](char c) { return c >= '0' && c <= '9'; }))
			return nullptr;
		Weapon = 0;
		for(size_t i = 0; i < Separator; i++)
			Weapon = Weapon * 10 + (Id[i] - '0');
		Id.remove_prefix(Separator + 1);
	}
	for(const CMatchMetricInfo &Info : gs_aMatchMetrics)
	{
		if(Id != Info.m_pId || (Weapon >= 0 && Info.m_CombatStat < 0))
			continue;
		if(pWeapon)
			*pWeapon = Weapon;
		return &Info;
	}
	return nullptr;
}

const CMatchParticipant *CMatchReport::Participant(int ParticipantId) const
{
	const auto It = std::find_if(m_vParticipants.begin(), m_vParticipants.end(), [ParticipantId](const CMatchParticipant &Participant) { return Participant.m_ParticipantId == ParticipantId; });
	return It == m_vParticipants.end() ? nullptr : &*It;
}

const CMatchStanding *CMatchReport::Standing(EMatchSubjectKind SubjectKind, int SubjectId) const
{
	const auto It = std::find_if(m_vStandings.begin(), m_vStandings.end(), [&](const CMatchStanding &Standing) { return Standing.m_SubjectKind == SubjectKind && Standing.m_SubjectId == SubjectId; });
	return It == m_vStandings.end() ? nullptr : &*It;
}

std::optional<int64_t> CMatchReport::Metric(EMatchSubjectKind SubjectKind, int SubjectId, const char *pMetricId) const
{
	for(const CMatchMetric &Metric : m_vMetrics)
	{
		if(Metric.m_SubjectKind == SubjectKind && Metric.m_SubjectId == SubjectId && Metric.m_MetricId == pMetricId)
			return Metric.m_Value;
	}
	return std::nullopt;
}

const char *MatchTerminationName(EMatchTermination Termination)
{
	static const char *const s_apNames[] = {"completed", "aborted", "admin_ended"};
	static_assert(std::size(s_apNames) == (size_t)EMatchTermination::NUM);
	return s_apNames[(int)Termination];
}

const char *MatchSubjectKindName(EMatchSubjectKind SubjectKind)
{
	static const char *const s_apNames[] = {"match", "participant", "team"};
	static_assert(std::size(s_apNames) == (size_t)EMatchSubjectKind::NUM);
	return s_apNames[(int)SubjectKind];
}

const char *MatchOutcomeName(EMatchOutcome Outcome)
{
	static const char *const s_apNames[] = {"win", "loss", "draw", "finished", "dnf", "disqualified"};
	static_assert(std::size(s_apNames) == (size_t)EMatchOutcome::NUM);
	return s_apNames[(int)Outcome];
}

const char *MatchMetricAggregationName(EMatchMetricAggregation Aggregation)
{
	static const char *const s_apNames[] = {"sum", "maximum", "match_only"};
	static_assert(std::size(s_apNames) == (size_t)EMatchMetricAggregation::NUM);
	return s_apNames[(int)Aggregation];
}

static bool Fail(std::string *pError, const char *pMessage)
{
	if(pError != nullptr)
		*pError = pMessage;
	return false;
}

static bool ValidString(const std::string &Value, size_t MaxLength, bool AllowEmpty)
{
	return (AllowEmpty || !Value.empty()) && Value.size() <= MaxLength && str_utf8_check(Value.c_str());
}

bool IsValidMatchReportId(const std::string &Id)
{
	return !Id.empty() && Id.size() <= MatchReportLimits::MAX_ID_LENGTH && std::all_of(Id.begin(), Id.end(), [](char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
	});
}

bool MatchReportValidate(const CMatchReport &Report, std::string *pError)
{
	if(Report.m_MatchId == UUID_ZEROED)
		return Fail(pError, "match_id must not be zero");
	if(!IsValidMatchReportId(Report.m_ModeId))
		return Fail(pError, "invalid mode_id");
	if(!ValidString(Report.m_MapName, MatchReportLimits::MAX_MAP_NAME_LENGTH, false))
		return Fail(pError, "invalid map_name");
	if(Report.m_StartTimeUtc < 0 || Report.m_EndTimeUtc < Report.m_StartTimeUtc || Report.m_EndTimeUtc > MatchReportLimits::MAX_TIME_UTC)
		return Fail(pError, "invalid UTC time range");
	if(Report.m_DurationTicks < 0 || Report.m_DurationTicks > MatchReportLimits::MAX_DURATION_TICKS || Report.m_TickRate <= 0 || Report.m_TickRate > 1000 || Report.m_RoundStartTick < 0)
		return Fail(pError, "invalid duration, tick_rate or round_start_tick");
	if(Report.m_Termination < EMatchTermination::COMPLETED || Report.m_Termination >= EMatchTermination::NUM)
		return Fail(pError, "invalid termination");
	if(Report.m_vTeams.size() > MatchReportLimits::MAX_TEAMS || Report.m_vParticipants.size() > MatchReportLimits::MAX_PARTICIPANTS || Report.m_vStandings.size() > MatchReportLimits::MAX_STANDINGS || Report.m_vMetrics.size() > MatchReportLimits::MAX_METRICS)
		return Fail(pError, "report item limit exceeded");

	std::set<int> TeamIds;
	for(const CMatchTeam &Team : Report.m_vTeams)
	{
		if(Team.m_TeamId < 0 || !TeamIds.insert(Team.m_TeamId).second || !ValidString(Team.m_DisplayName, MatchReportLimits::MAX_DISPLAY_NAME_LENGTH, true))
			return Fail(pError, "invalid or duplicate team");
	}

	std::set<int> ParticipantIds;
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
	{
		if(Participant.m_ParticipantId < 0 || !ParticipantIds.insert(Participant.m_ParticipantId).second)
			return Fail(pError, "invalid or duplicate participant");
		if(Participant.m_TeamId.has_value() && !TeamIds.contains(*Participant.m_TeamId))
			return Fail(pError, "participant references missing team");
		if(!ValidString(Participant.m_DisplayName, MatchReportLimits::MAX_DISPLAY_NAME_LENGTH, false) || !ValidString(Participant.m_Clan, MatchReportLimits::MAX_CLAN_LENGTH, true))
			return Fail(pError, "invalid participant identity");
		if(Participant.m_JoinedTick < 0 || Participant.m_JoinedTick > Report.m_DurationTicks || (Participant.m_LeftTick.has_value() && (*Participant.m_LeftTick < Participant.m_JoinedTick || *Participant.m_LeftTick > Report.m_DurationTicks)))
			return Fail(pError, "invalid participant tick range");
	}

	const auto HasSubject = [&](EMatchSubjectKind SubjectKind, int SubjectId) {
		return (SubjectKind == EMatchSubjectKind::PARTICIPANT && ParticipantIds.contains(SubjectId)) || (SubjectKind == EMatchSubjectKind::TEAM && TeamIds.contains(SubjectId));
	};
	std::set<std::pair<EMatchSubjectKind, int>> StandingSubjects;
	for(const CMatchStanding &Standing : Report.m_vStandings)
	{
		if(Standing.m_Rank <= 0 || Standing.m_Outcome < EMatchOutcome::WIN || Standing.m_Outcome >= EMatchOutcome::NUM || !HasSubject(Standing.m_SubjectKind, Standing.m_SubjectId))
			return Fail(pError, "invalid standing");
		if(!StandingSubjects.emplace(Standing.m_SubjectKind, Standing.m_SubjectId).second)
			return Fail(pError, "duplicate standing subject");
	}

	std::set<std::tuple<EMatchSubjectKind, int, std::string>> MetricKeys;
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		if(!IsValidMatchReportId(Metric.m_MetricId))
			return Fail(pError, "invalid metric_id");
		if(Metric.m_Aggregation < EMatchMetricAggregation::SUM || Metric.m_Aggregation >= EMatchMetricAggregation::NUM)
			return Fail(pError, "invalid metric aggregation");
		if(Metric.m_Value < -MatchReportLimits::MAX_METRIC_VALUE || Metric.m_Value > MatchReportLimits::MAX_METRIC_VALUE)
			return Fail(pError, "metric value out of range");
		if(Metric.m_SubjectKind == EMatchSubjectKind::MATCH ? Metric.m_SubjectId.has_value() : !Metric.m_SubjectId.has_value() || !HasSubject(Metric.m_SubjectKind, *Metric.m_SubjectId))
			return Fail(pError, "invalid metric subject");
		if(!MetricKeys.emplace(Metric.m_SubjectKind, Metric.m_SubjectId.value_or(-1), Metric.m_MetricId).second)
			return Fail(pError, "duplicate metric");
	}
	return true;
}

void MatchReportWriteJson(CJsonWriter &Writer, const CMatchReport &Report)
{
	char aMatchId[UUID_MAXSTRSIZE];
	FormatUuid(Report.m_MatchId, aMatchId, sizeof(aMatchId));
	char aMapSha256[SHA256_MAXSTRSIZE];
	sha256_str(Report.m_MapSha256, aMapSha256, sizeof(aMapSha256));

	Writer.BeginObject();
	Writer.WriteAttribute("match_id");
	Writer.WriteStrValue(aMatchId);
	Writer.WriteAttribute("game_uuid");
	if(Report.m_GameUuid.has_value())
	{
		char aGameUuid[UUID_MAXSTRSIZE];
		FormatUuid(*Report.m_GameUuid, aGameUuid, sizeof(aGameUuid));
		Writer.WriteStrValue(aGameUuid);
	}
	else
		Writer.WriteNullValue();
	Writer.WriteAttribute("mode_id");
	Writer.WriteStrValue(Report.m_ModeId.c_str());
	Writer.WriteAttribute("map_name");
	Writer.WriteStrValue(Report.m_MapName.c_str());
	Writer.WriteAttribute("map_sha256");
	Writer.WriteStrValue(aMapSha256);
	Writer.WriteAttribute("start_time_utc");
	Writer.WriteInt64Value(Report.m_StartTimeUtc);
	Writer.WriteAttribute("end_time_utc");
	Writer.WriteInt64Value(Report.m_EndTimeUtc);
	Writer.WriteAttribute("duration_ticks");
	Writer.WriteInt64Value(Report.m_DurationTicks);
	Writer.WriteAttribute("tick_rate");
	Writer.WriteIntValue(Report.m_TickRate);
	Writer.WriteAttribute("round_start_tick");
	Writer.WriteIntValue(Report.m_RoundStartTick);
	Writer.WriteAttribute("termination");
	Writer.WriteStrValue(MatchTerminationName(Report.m_Termination));

	Writer.WriteAttribute("teams");
	Writer.BeginArray();
	for(const CMatchTeam &Team : Report.m_vTeams)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("team_id");
		Writer.WriteIntValue(Team.m_TeamId);
		Writer.WriteAttribute("display_name");
		Writer.WriteStrValue(Team.m_DisplayName.c_str());
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("participants");
	Writer.BeginArray();
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("participant_id");
		Writer.WriteIntValue(Participant.m_ParticipantId);
		Writer.WriteAttribute("team_id");
		if(Participant.m_TeamId.has_value())
			Writer.WriteIntValue(*Participant.m_TeamId);
		else
			Writer.WriteNullValue();
		Writer.WriteAttribute("display_name");
		Writer.WriteStrValue(Participant.m_DisplayName.c_str());
		Writer.WriteAttribute("clan");
		Writer.WriteStrValue(Participant.m_Clan.c_str());
		Writer.WriteAttribute("joined_tick");
		Writer.WriteInt64Value(Participant.m_JoinedTick);
		Writer.WriteAttribute("left_tick");
		if(Participant.m_LeftTick.has_value())
			Writer.WriteInt64Value(*Participant.m_LeftTick);
		else
			Writer.WriteNullValue();
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("standings");
	Writer.BeginArray();
	for(const CMatchStanding &Standing : Report.m_vStandings)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("subject_kind");
		Writer.WriteStrValue(MatchSubjectKindName(Standing.m_SubjectKind));
		Writer.WriteAttribute("subject_id");
		Writer.WriteIntValue(Standing.m_SubjectId);
		Writer.WriteAttribute("rank");
		Writer.WriteIntValue(Standing.m_Rank);
		Writer.WriteAttribute("outcome");
		Writer.WriteStrValue(MatchOutcomeName(Standing.m_Outcome));
		Writer.EndObject();
	}
	Writer.EndArray();

	Writer.WriteAttribute("metrics");
	Writer.BeginArray();
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("subject_kind");
		Writer.WriteStrValue(MatchSubjectKindName(Metric.m_SubjectKind));
		Writer.WriteAttribute("subject_id");
		if(Metric.m_SubjectId.has_value())
			Writer.WriteIntValue(*Metric.m_SubjectId);
		else
			Writer.WriteNullValue();
		Writer.WriteAttribute("metric_id");
		Writer.WriteStrValue(Metric.m_MetricId.c_str());
		Writer.WriteAttribute("value");
		Writer.WriteInt64Value(Metric.m_Value);
		Writer.WriteAttribute("aggregation");
		Writer.WriteStrValue(MatchMetricAggregationName(Metric.m_Aggregation));
		Writer.EndObject();
	}
	Writer.EndArray();
	Writer.EndObject();
}

// Unsigned values are LEB128, signed ones zigzagged first, strings are indices
// into one table at the head.
static constexpr unsigned char PACKED_FORMAT_VERSION = 1;

static void PackUnsigned(std::string &Out, uint64_t Value)
{
	while(Value >= 0x80)
	{
		Out.push_back((char)(unsigned char)(Value | 0x80));
		Value >>= 7;
	}
	Out.push_back((char)(unsigned char)Value);
}

static void PackSigned(std::string &Out, int64_t Value)
{
	PackUnsigned(Out, ((uint64_t)Value << 1) ^ (uint64_t)(Value >> 63));
}

namespace
{
	class CPackedReader
	{
		const unsigned char *m_pData;
		size_t m_Size;
		size_t m_Offset = 0;
		bool m_Error = false;
		std::vector<std::string> m_vStrings;

	public:
		CPackedReader(const void *pData, size_t Size) :
			m_pData((const unsigned char *)pData), m_Size(Size)
		{
		}

		bool Error() const { return m_Error; }
		bool AtEnd() const { return m_Offset == m_Size; }

		uint64_t Unsigned()
		{
			uint64_t Value = 0;
			for(int Shift = 0; Shift < 64; Shift += 7)
			{
				if(m_Offset >= m_Size)
					break;
				const unsigned char Byte = m_pData[m_Offset++];
				Value |= (uint64_t)(Byte & 0x7F) << Shift;
				if((Byte & 0x80) == 0)
					return Value;
			}
			m_Error = true;
			return 0;
		}

		int64_t Signed()
		{
			const uint64_t Value = Unsigned();
			return (int64_t)(Value >> 1) ^ -(int64_t)(Value & 1);
		}

		int Int()
		{
			const int64_t Value = Signed();
			if(Value < std::numeric_limits<int>::min() || Value > std::numeric_limits<int>::max())
				m_Error = true;
			return (int)Value;
		}

		bool Bool() { return Unsigned() != 0; }

		template<typename TEnum>
		TEnum Enum()
		{
			const uint64_t Value = Unsigned();
			if(Value >= (uint64_t)TEnum::NUM)
				m_Error = true;
			return m_Error ? TEnum() : (TEnum)Value;
		}

		void Raw(void *pOut, size_t Size)
		{
			if(m_Size - m_Offset < Size)
			{
				m_Error = true;
				return;
			}
			mem_copy(pOut, m_pData + m_Offset, Size);
			m_Offset += Size;
		}

		void ReadStringTable()
		{
			const uint64_t NumStrings = Unsigned();
			// every string costs at least its length byte
			if(NumStrings > m_Size)
			{
				m_Error = true;
				return;
			}
			for(uint64_t i = 0; i < NumStrings && !m_Error; ++i)
			{
				const uint64_t Length = Unsigned();
				if(m_Error || m_Size - m_Offset < Length)
				{
					m_Error = true;
					return;
				}
				m_vStrings.emplace_back((const char *)m_pData + m_Offset, Length);
				m_Offset += Length;
			}
		}

		std::string String()
		{
			const uint64_t Index = Unsigned();
			if(Index >= m_vStrings.size())
			{
				m_Error = true;
				return {};
			}
			return m_vStrings[Index];
		}
	};

	class CStringTable
	{
		std::vector<const std::string *> m_vpStrings;
		std::unordered_map<std::string, uint64_t> m_Indices;

	public:
		void Add(const std::string &Value)
		{
			if(m_Indices.emplace(Value, m_vpStrings.size()).second)
				m_vpStrings.push_back(&Value);
		}

		uint64_t Index(const std::string &Value) const
		{
			const auto Found = m_Indices.find(Value);
			dbg_assert(Found != m_Indices.end(), "match report string was not collected");
			return Found->second;
		}

		void Write(std::string &Out) const
		{
			PackUnsigned(Out, m_vpStrings.size());
			for(const std::string *pValue : m_vpStrings)
			{
				PackUnsigned(Out, pValue->size());
				Out.append(*pValue);
			}
		}
	};
}

bool MatchReportToPacked(const CMatchReport &Report, std::string &Packed, std::string *pError)
{
	if(!MatchReportValidate(Report, pError))
		return false;

	CStringTable Table;
	Table.Add(Report.m_ModeId);
	Table.Add(Report.m_MapName);
	for(const CMatchTeam &Team : Report.m_vTeams)
		Table.Add(Team.m_DisplayName);
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
	{
		Table.Add(Participant.m_DisplayName);
		Table.Add(Participant.m_Clan);
	}
	for(const CMatchMetric &Metric : Report.m_vMetrics)
		Table.Add(Metric.m_MetricId);

	std::string Out;
	Out.push_back((char)PACKED_FORMAT_VERSION);
	Table.Write(Out);
	Out.append((const char *)&Report.m_MatchId, sizeof(Report.m_MatchId));
	PackUnsigned(Out, Report.m_GameUuid.has_value());
	if(Report.m_GameUuid.has_value())
		Out.append((const char *)&*Report.m_GameUuid, sizeof(CUuid));
	PackUnsigned(Out, Table.Index(Report.m_ModeId));
	PackUnsigned(Out, Table.Index(Report.m_MapName));
	Out.append((const char *)&Report.m_MapSha256, sizeof(Report.m_MapSha256));
	PackSigned(Out, Report.m_StartTimeUtc);
	PackSigned(Out, Report.m_EndTimeUtc);
	PackSigned(Out, Report.m_DurationTicks);
	PackSigned(Out, Report.m_TickRate);
	PackSigned(Out, Report.m_RoundStartTick);
	PackUnsigned(Out, (uint64_t)Report.m_Termination);

	PackUnsigned(Out, Report.m_vTeams.size());
	for(const CMatchTeam &Team : Report.m_vTeams)
	{
		PackSigned(Out, Team.m_TeamId);
		PackUnsigned(Out, Table.Index(Team.m_DisplayName));
	}

	PackUnsigned(Out, Report.m_vParticipants.size());
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
	{
		PackSigned(Out, Participant.m_ParticipantId);
		PackUnsigned(Out, Participant.m_TeamId.has_value());
		if(Participant.m_TeamId.has_value())
			PackSigned(Out, *Participant.m_TeamId);
		PackUnsigned(Out, Table.Index(Participant.m_DisplayName));
		PackUnsigned(Out, Table.Index(Participant.m_Clan));
		PackSigned(Out, Participant.m_JoinedTick);
		PackUnsigned(Out, Participant.m_LeftTick.has_value());
		if(Participant.m_LeftTick.has_value())
			PackSigned(Out, *Participant.m_LeftTick);
	}

	PackUnsigned(Out, Report.m_vStandings.size());
	for(const CMatchStanding &Standing : Report.m_vStandings)
	{
		PackUnsigned(Out, (uint64_t)Standing.m_SubjectKind);
		PackSigned(Out, Standing.m_SubjectId);
		PackSigned(Out, Standing.m_Rank);
		PackUnsigned(Out, (uint64_t)Standing.m_Outcome);
	}

	PackUnsigned(Out, Report.m_vMetrics.size());
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		PackUnsigned(Out, (uint64_t)Metric.m_SubjectKind);
		PackUnsigned(Out, Metric.m_SubjectId.has_value());
		if(Metric.m_SubjectId.has_value())
			PackSigned(Out, *Metric.m_SubjectId);
		PackUnsigned(Out, Table.Index(Metric.m_MetricId));
		PackSigned(Out, Metric.m_Value);
		PackUnsigned(Out, (uint64_t)Metric.m_Aggregation);
	}

	if(Out.size() > MatchReportLimits::MAX_PAYLOAD_SIZE)
		return Fail(pError, "packed report exceeds payload limit");
	Packed = std::move(Out);
	return true;
}

bool MatchReportFromPacked(const void *pData, size_t Size, CMatchReport &Report, std::string *pError)
{
	if(pData == nullptr || Size == 0 || Size > MatchReportLimits::MAX_PAYLOAD_SIZE || *(const unsigned char *)pData != PACKED_FORMAT_VERSION)
		return Fail(pError, "unknown packed match report format");
	CPackedReader Reader((const unsigned char *)pData + 1, Size - 1);
	Reader.ReadStringTable();

	CMatchReport Parsed;
	Reader.Raw(&Parsed.m_MatchId, sizeof(Parsed.m_MatchId));
	if(Reader.Bool())
	{
		CUuid GameUuid;
		Reader.Raw(&GameUuid, sizeof(GameUuid));
		Parsed.m_GameUuid = GameUuid;
	}
	Parsed.m_ModeId = Reader.String();
	Parsed.m_MapName = Reader.String();
	Reader.Raw(&Parsed.m_MapSha256, sizeof(Parsed.m_MapSha256));
	Parsed.m_StartTimeUtc = Reader.Signed();
	Parsed.m_EndTimeUtc = Reader.Signed();
	Parsed.m_DurationTicks = Reader.Signed();
	Parsed.m_TickRate = Reader.Int();
	Parsed.m_RoundStartTick = Reader.Int();
	Parsed.m_Termination = Reader.Enum<EMatchTermination>();

	// The counts are checked against the limits before anything is allocated for them.
	const uint64_t NumTeams = Reader.Unsigned();
	if(Reader.Error() || NumTeams > (uint64_t)MatchReportLimits::MAX_TEAMS)
		return Fail(pError, "malformed packed match report teams");
	Parsed.m_vTeams.resize(NumTeams);
	for(CMatchTeam &Team : Parsed.m_vTeams)
	{
		Team.m_TeamId = Reader.Int();
		Team.m_DisplayName = Reader.String();
	}

	const uint64_t NumParticipants = Reader.Unsigned();
	if(Reader.Error() || NumParticipants > (uint64_t)MatchReportLimits::MAX_PARTICIPANTS)
		return Fail(pError, "malformed packed match report participants");
	Parsed.m_vParticipants.resize(NumParticipants);
	for(CMatchParticipant &Participant : Parsed.m_vParticipants)
	{
		Participant.m_ParticipantId = Reader.Int();
		if(Reader.Bool())
			Participant.m_TeamId = Reader.Int();
		Participant.m_DisplayName = Reader.String();
		Participant.m_Clan = Reader.String();
		Participant.m_JoinedTick = Reader.Signed();
		if(Reader.Bool())
			Participant.m_LeftTick = Reader.Signed();
	}

	const uint64_t NumStandings = Reader.Unsigned();
	if(Reader.Error() || NumStandings > (uint64_t)MatchReportLimits::MAX_STANDINGS)
		return Fail(pError, "malformed packed match report standings");
	Parsed.m_vStandings.resize(NumStandings);
	for(CMatchStanding &Standing : Parsed.m_vStandings)
	{
		Standing.m_SubjectKind = Reader.Enum<EMatchSubjectKind>();
		Standing.m_SubjectId = Reader.Int();
		Standing.m_Rank = Reader.Int();
		Standing.m_Outcome = Reader.Enum<EMatchOutcome>();
	}

	const uint64_t NumMetrics = Reader.Unsigned();
	if(Reader.Error() || NumMetrics > (uint64_t)MatchReportLimits::MAX_METRICS)
		return Fail(pError, "malformed packed match report metrics");
	Parsed.m_vMetrics.resize(NumMetrics);
	for(CMatchMetric &Metric : Parsed.m_vMetrics)
	{
		Metric.m_SubjectKind = Reader.Enum<EMatchSubjectKind>();
		if(Reader.Bool())
			Metric.m_SubjectId = Reader.Int();
		Metric.m_MetricId = Reader.String();
		Metric.m_Value = Reader.Signed();
		Metric.m_Aggregation = Reader.Enum<EMatchMetricAggregation>();
	}

	if(Reader.Error() || !Reader.AtEnd())
		return Fail(pError, "malformed packed match report");
	if(!MatchReportValidate(Parsed, pError))
		return false;
	Report = std::move(Parsed);
	return true;
}
