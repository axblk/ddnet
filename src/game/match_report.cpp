#include "match_report.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace
{
	bool Fail(std::string *pError, const char *pMessage)
	{
		if(pError != nullptr)
			*pError = pMessage;
		return false;
	}

	bool ValidString(const std::string &Value, size_t MaxLength, bool AllowEmpty)
	{
		return (AllowEmpty || !Value.empty()) && Value.size() <= MaxLength && str_utf8_check(Value.c_str());
	}

	bool HasTeam(const CMatchReport &Report, int TeamId)
	{
		return std::ranges::any_of(Report.m_vTeams, [TeamId](const CMatchTeam &Team) { return Team.m_TeamId == TeamId; });
	}

	bool HasParticipant(const CMatchReport &Report, int ParticipantId)
	{
		return std::ranges::any_of(Report.m_vParticipants, [ParticipantId](const CMatchParticipant &Participant) { return Participant.m_ParticipantId == ParticipantId; });
	}

	bool HasSubject(const CMatchReport &Report, EMatchSubjectKind SubjectKind, int SubjectId)
	{
		if(SubjectKind == EMatchSubjectKind::PARTICIPANT)
			return HasParticipant(Report, SubjectId);
		if(SubjectKind == EMatchSubjectKind::TEAM)
			return HasTeam(Report, SubjectId);
		return false;
	}

	bool ParseTermination(const char *pValue, EMatchTermination &Termination)
	{
		if(str_comp(pValue, "completed") == 0)
			Termination = EMatchTermination::COMPLETED;
		else if(str_comp(pValue, "aborted") == 0)
			Termination = EMatchTermination::ABORTED;
		else if(str_comp(pValue, "admin_ended") == 0)
			Termination = EMatchTermination::ADMIN_ENDED;
		else
			return false;
		return true;
	}

	bool ParseSubjectKind(const char *pValue, EMatchSubjectKind &SubjectKind)
	{
		if(str_comp(pValue, "match") == 0)
			SubjectKind = EMatchSubjectKind::MATCH;
		else if(str_comp(pValue, "participant") == 0)
			SubjectKind = EMatchSubjectKind::PARTICIPANT;
		else if(str_comp(pValue, "team") == 0)
			SubjectKind = EMatchSubjectKind::TEAM;
		else
			return false;
		return true;
	}

	bool ParseOutcome(const char *pValue, EMatchOutcome &Outcome)
	{
		if(str_comp(pValue, "win") == 0)
			Outcome = EMatchOutcome::WIN;
		else if(str_comp(pValue, "loss") == 0)
			Outcome = EMatchOutcome::LOSS;
		else if(str_comp(pValue, "draw") == 0)
			Outcome = EMatchOutcome::DRAW;
		else if(str_comp(pValue, "finished") == 0)
			Outcome = EMatchOutcome::FINISHED;
		else if(str_comp(pValue, "dnf") == 0)
			Outcome = EMatchOutcome::DNF;
		else if(str_comp(pValue, "disqualified") == 0)
			Outcome = EMatchOutcome::DISQUALIFIED;
		else
			return false;
		return true;
	}

	bool ParseMetricAggregation(const char *pValue, EMatchMetricAggregation &Aggregation)
	{
		if(str_comp(pValue, "sum") == 0)
			Aggregation = EMatchMetricAggregation::SUM;
		else if(str_comp(pValue, "maximum") == 0)
			Aggregation = EMatchMetricAggregation::MAXIMUM;
		else if(str_comp(pValue, "match_only") == 0)
			Aggregation = EMatchMetricAggregation::MATCH_ONLY;
		else
			return false;
		return true;
	}

	const json_value *RequiredValue(const json_value &Object, const char *pName, json_type Type, std::string *pError)
	{
		const json_value *pValue = json_object_get(&Object, pName);
		if(pValue->type != Type)
		{
			if(pError != nullptr)
				*pError = std::string("invalid or missing field: ") + pName;
			return nullptr;
		}
		return pValue;
	}

	bool ReadString(const json_value &Object, const char *pName, std::string &Value, std::string *pError)
	{
		const json_value *pValue = RequiredValue(Object, pName, json_string, pError);
		if(pValue == nullptr)
			return false;
		Value = pValue->u.string.ptr;
		return true;
	}

	bool ReadInt(const json_value &Object, const char *pName, int &Value, std::string *pError)
	{
		const json_value *pValue = RequiredValue(Object, pName, json_integer, pError);
		if(pValue == nullptr || !in_range<json_int_t>(pValue->u.integer, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()))
			return pValue == nullptr ? false : Fail(pError, "integer field out of range");
		Value = static_cast<int>(pValue->u.integer);
		return true;
	}

	bool ReadInt64(const json_value &Object, const char *pName, int64_t &Value, std::string *pError)
	{
		const json_value *pValue = RequiredValue(Object, pName, json_integer, pError);
		if(pValue == nullptr)
			return false;
		Value = pValue->u.integer;
		return true;
	}

	bool ReadBool(const json_value &Object, const char *pName, bool &Value, std::string *pError)
	{
		const json_value *pValue = RequiredValue(Object, pName, json_boolean, pError);
		if(pValue == nullptr)
			return false;
		Value = pValue->u.boolean != 0;
		return true;
	}

	bool ReadOptionalInt(const json_value &Object, const char *pName, std::optional<int> &Value, std::string *pError)
	{
		const json_value *pValue = json_object_get(&Object, pName);
		if(pValue->type == json_null)
		{
			Value.reset();
			return true;
		}
		if(pValue->type != json_integer || !in_range<json_int_t>(pValue->u.integer, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()))
			return Fail(pError, "invalid optional integer field");
		Value = static_cast<int>(pValue->u.integer);
		return true;
	}

	bool ReadOptionalInt64(const json_value &Object, const char *pName, std::optional<int64_t> &Value, std::string *pError)
	{
		const json_value *pValue = json_object_get(&Object, pName);
		if(pValue->type == json_null)
		{
			Value.reset();
			return true;
		}
		if(pValue->type != json_integer)
			return Fail(pError, "invalid optional int64 field");
		Value = pValue->u.integer;
		return true;
	}

	bool ReadUuid(const json_value &Object, const char *pName, CUuid &Uuid, std::string *pError)
	{
		std::string Value;
		if(!ReadString(Object, pName, Value, pError) || ParseUuid(&Uuid, Value.c_str()) != 0)
			return Fail(pError, "invalid UUID field");
		return true;
	}

	bool ReadOptionalUuid(const json_value &Object, const char *pName, std::optional<CUuid> &Uuid, std::string *pError)
	{
		const json_value *pValue = json_object_get(&Object, pName);
		if(pValue->type == json_null)
		{
			Uuid.reset();
			return true;
		}
		if(pValue->type != json_string)
			return Fail(pError, "invalid optional UUID field");
		CUuid Parsed;
		if(ParseUuid(&Parsed, pValue->u.string.ptr) != 0)
			return Fail(pError, "invalid optional UUID field");
		Uuid = Parsed;
		return true;
	}
}

bool IsValidMatchReportModeId(const std::string &ModeId)
{
	if(!ValidString(ModeId, MatchReportLimits::MAX_MODE_ID_LENGTH, false))
		return false;
	const size_t At = ModeId.find('@');
	if(At == std::string::npos || At == 0 || At + 1 == ModeId.size() || ModeId.find('@', At + 1) != std::string::npos)
		return false;
	return std::ranges::all_of(ModeId, [](const unsigned char Character) { return Character > 0x20 && Character != 0x7f && Character != '/'; });
}

bool IsValidMatchReportMetricId(const std::string &MetricId, const std::string &ModeId)
{
	if(!IsValidMatchReportModeId(ModeId) || MetricId.size() > MatchReportLimits::MAX_METRIC_ID_LENGTH)
		return false;
	const std::string Prefix = ModeId + "/";
	if(MetricId.size() <= Prefix.size() || MetricId.compare(0, Prefix.size(), Prefix) != 0)
		return false;
	for(size_t Index = Prefix.size(); Index < MetricId.size(); ++Index)
	{
		const unsigned char Character = MetricId[Index];
		if(!((Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9') || Character == '_' || Character == '-' || Character == '.'))
			return false;
	}
	return true;
}

const char *MatchTerminationName(EMatchTermination Termination)
{
	switch(Termination)
	{
	case EMatchTermination::COMPLETED: return "completed";
	case EMatchTermination::ABORTED: return "aborted";
	case EMatchTermination::ADMIN_ENDED: return "admin_ended";
	}
	return "invalid";
}

const char *MatchSubjectKindName(EMatchSubjectKind SubjectKind)
{
	switch(SubjectKind)
	{
	case EMatchSubjectKind::MATCH: return "match";
	case EMatchSubjectKind::PARTICIPANT: return "participant";
	case EMatchSubjectKind::TEAM: return "team";
	}
	return "invalid";
}

const char *MatchOutcomeName(EMatchOutcome Outcome)
{
	switch(Outcome)
	{
	case EMatchOutcome::WIN: return "win";
	case EMatchOutcome::LOSS: return "loss";
	case EMatchOutcome::DRAW: return "draw";
	case EMatchOutcome::FINISHED: return "finished";
	case EMatchOutcome::DNF: return "dnf";
	case EMatchOutcome::DISQUALIFIED: return "disqualified";
	}
	return "invalid";
}

const char *MatchMetricAggregationName(EMatchMetricAggregation Aggregation)
{
	switch(Aggregation)
	{
	case EMatchMetricAggregation::INVALID: return "invalid";
	case EMatchMetricAggregation::SUM: return "sum";
	case EMatchMetricAggregation::MAXIMUM: return "maximum";
	case EMatchMetricAggregation::MATCH_ONLY: return "match_only";
	}
	return "invalid";
}

bool MatchReportValidate(const CMatchReport &Report, std::string *pError)
{
	if(Report.m_MatchId == UUID_ZEROED)
		return Fail(pError, "match_id must not be zero");
	if(Report.m_ReportSchemaVersion != 1)
		return Fail(pError, "unsupported report_schema_version");
	if(!IsValidMatchReportModeId(Report.m_ModeId))
		return Fail(pError, "invalid mode_id");
	if(Report.m_ModeSchemaVersion <= 0)
		return Fail(pError, "invalid mode_schema_version");
	if(!ValidString(Report.m_MapName, MatchReportLimits::MAX_MAP_NAME_LENGTH, false))
		return Fail(pError, "invalid map_name");
	if(Report.m_StartTimeUtc < 0 || Report.m_EndTimeUtc < Report.m_StartTimeUtc || Report.m_EndTimeUtc > MatchReportLimits::MAX_TIME_UTC)
		return Fail(pError, "invalid UTC time range");
	if(Report.m_DurationTicks < 0 || Report.m_DurationTicks > MatchReportLimits::MAX_DURATION_TICKS || Report.m_TickRate <= 0 || Report.m_TickRate > 1000 || Report.m_RoundStartTick < 0)
		return Fail(pError, "invalid duration, tick_rate or round_start_tick");
	if(!ValidString(Report.m_UnrankedReason, MatchReportLimits::MAX_REASON_LENGTH, true) || (Report.m_Ranked && !Report.m_UnrankedReason.empty()))
		return Fail(pError, "invalid unranked_reason");
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
		if(Participant.m_TeamId.has_value() && !HasTeam(Report, *Participant.m_TeamId))
			return Fail(pError, "participant references missing team");
		if(!ValidString(Participant.m_DisplayName, MatchReportLimits::MAX_DISPLAY_NAME_LENGTH, false) || !ValidString(Participant.m_Clan, MatchReportLimits::MAX_CLAN_LENGTH, true))
			return Fail(pError, "invalid participant identity");
		if(Participant.m_JoinedTick < 0 || Participant.m_JoinedTick > Report.m_DurationTicks || (Participant.m_LeftTick.has_value() && (*Participant.m_LeftTick < Participant.m_JoinedTick || *Participant.m_LeftTick > Report.m_DurationTicks)))
			return Fail(pError, "invalid participant tick range");
	}

	std::set<std::pair<int, int>> StandingSubjects;
	for(const CMatchStanding &Standing : Report.m_vStandings)
	{
		if(Standing.m_SubjectKind == EMatchSubjectKind::MATCH || Standing.m_Rank <= 0 || !HasSubject(Report, Standing.m_SubjectKind, Standing.m_SubjectId))
			return Fail(pError, "invalid standing subject");
		if(!StandingSubjects.emplace(static_cast<int>(Standing.m_SubjectKind), Standing.m_SubjectId).second)
			return Fail(pError, "duplicate standing subject");
	}

	std::set<std::tuple<int, int, std::string>> MetricKeys;
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		if(!IsValidMatchReportMetricId(Metric.m_MetricId, Report.m_ModeId))
			return Fail(pError, "invalid metric_id");
		if(str_comp(MatchMetricAggregationName(Metric.m_Aggregation), "invalid") == 0)
			return Fail(pError, "invalid metric aggregation");
		if(Metric.m_SubjectKind == EMatchSubjectKind::MATCH)
		{
			if(Metric.m_SubjectId.has_value())
				return Fail(pError, "match metric must not have subject_id");
		}
		else if(!Metric.m_SubjectId.has_value() || !HasSubject(Report, Metric.m_SubjectKind, *Metric.m_SubjectId))
			return Fail(pError, "metric references missing subject");
		const int SubjectId = Metric.m_SubjectId.value_or(-1);
		if(!MetricKeys.emplace(static_cast<int>(Metric.m_SubjectKind), SubjectId, Metric.m_MetricId).second)
			return Fail(pError, "duplicate metric");
	}
	return true;
}

namespace
{
	// The packed format is only ever produced and consumed by this file, so it uses
	// the smallest encoding that does the job: unsigned values as LEB128, signed
	// ones zigzagged first, strings as indices into one table at the head.
	constexpr unsigned char PACKED_FORMAT_VERSION = 1;

	void PackUnsigned(std::string &Out, uint64_t Value)
	{
		while(Value >= 0x80)
		{
			Out.push_back((char)(unsigned char)(Value | 0x80));
			Value >>= 7;
		}
		Out.push_back((char)(unsigned char)Value);
	}

	void PackSigned(std::string &Out, int64_t Value)
	{
		PackUnsigned(Out, ((uint64_t)Value << 1) ^ (uint64_t)(Value >> 63));
	}

	class CPackedReader
	{
		const unsigned char *m_pData;
		size_t m_Size;
		size_t m_Offset = 0;
		bool m_Error = false;

	public:
		CPackedReader(const char *pData, size_t Size) :
			m_pData((const unsigned char *)pData), m_Size(Size)
		{
		}

		bool Error() const { return m_Error; }

		uint64_t Unsigned()
		{
			uint64_t Value = 0;
			for(int Shift = 0; Shift < 64; Shift += 7)
			{
				if(m_Offset >= m_Size)
				{
					m_Error = true;
					return 0;
				}
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

		const unsigned char *Raw(size_t Size)
		{
			if(m_Size - m_Offset < Size)
			{
				m_Error = true;
				return nullptr;
			}
			const unsigned char *pResult = m_pData + m_Offset;
			m_Offset += Size;
			return pResult;
		}

		bool AtEnd() const { return m_Offset == m_Size; }
	};

	class CStringTable
	{
		std::vector<std::string> m_vStrings;
		std::unordered_map<std::string, uint64_t> m_Indices;

	public:
		void Add(const std::string &Value)
		{
			if(m_Indices.find(Value) != m_Indices.end())
				return;
			m_Indices[Value] = m_vStrings.size();
			m_vStrings.push_back(Value);
		}

		// Every string is collected before the table is written, so looking one up
		// afterwards can never grow it.
		uint64_t Index(const std::string &Value) const
		{
			const auto Found = m_Indices.find(Value);
			dbg_assert(Found != m_Indices.end(), "match report string was not collected");
			return Found->second;
		}

		void Write(std::string &Out) const
		{
			PackUnsigned(Out, m_vStrings.size());
			for(const std::string &Value : m_vStrings)
			{
				PackUnsigned(Out, Value.size());
				Out.append(Value);
			}
		}
	};

	void CollectStrings(const CMatchReport &Report, CStringTable &Table)
	{
		Table.Add(Report.m_ModeId);
		Table.Add(Report.m_MapName);
		Table.Add(Report.m_UnrankedReason);
		for(const CMatchTeam &Team : Report.m_vTeams)
			Table.Add(Team.m_DisplayName);
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
		{
			Table.Add(Participant.m_DisplayName);
			Table.Add(Participant.m_Clan);
		}
		for(const CMatchMetric &Metric : Report.m_vMetrics)
			Table.Add(Metric.m_MetricId);
	}
} // namespace

bool MatchReportToPacked(const CMatchReport &Report, std::string &Packed, std::string *pError)
{
	if(!MatchReportValidate(Report, pError))
		return false;

	CStringTable Table;
	CollectStrings(Report, Table);

	std::string Out;
	Out.push_back((char)PACKED_FORMAT_VERSION);
	Table.Write(Out);

	Out.append((const char *)&Report.m_MatchId, sizeof(Report.m_MatchId));
	PackUnsigned(Out, Report.m_GameUuid.has_value() ? 1 : 0);
	if(Report.m_GameUuid.has_value())
		Out.append((const char *)&*Report.m_GameUuid, sizeof(CUuid));
	PackSigned(Out, Report.m_ReportSchemaVersion);
	PackUnsigned(Out, Table.Index(Report.m_ModeId));
	PackSigned(Out, Report.m_ModeSchemaVersion);
	PackUnsigned(Out, Table.Index(Report.m_MapName));
	Out.append((const char *)&Report.m_MapSha256, sizeof(Report.m_MapSha256));
	PackSigned(Out, Report.m_StartTimeUtc);
	PackSigned(Out, Report.m_EndTimeUtc);
	PackSigned(Out, Report.m_DurationTicks);
	PackSigned(Out, Report.m_TickRate);
	PackSigned(Out, Report.m_RoundStartTick);
	PackUnsigned(Out, (uint64_t)Report.m_Termination);
	PackUnsigned(Out, Report.m_Ranked ? 1 : 0);
	PackUnsigned(Out, Table.Index(Report.m_UnrankedReason));

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
		PackUnsigned(Out, Participant.m_TeamId.has_value() ? 1 : 0);
		if(Participant.m_TeamId.has_value())
			PackSigned(Out, *Participant.m_TeamId);
		PackUnsigned(Out, Table.Index(Participant.m_DisplayName));
		PackUnsigned(Out, Table.Index(Participant.m_Clan));
		PackSigned(Out, Participant.m_JoinedTick);
		PackUnsigned(Out, Participant.m_LeftTick.has_value() ? 1 : 0);
		if(Participant.m_LeftTick.has_value())
			PackSigned(Out, *Participant.m_LeftTick);
		PackUnsigned(Out, Participant.m_Bot ? 1 : 0);
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
		PackUnsigned(Out, Metric.m_SubjectId.has_value() ? 1 : 0);
		if(Metric.m_SubjectId.has_value())
			PackSigned(Out, *Metric.m_SubjectId);
		PackUnsigned(Out, Table.Index(Metric.m_MetricId));
		PackSigned(Out, Metric.m_Value);
		PackSigned(Out, (int64_t)Metric.m_Aggregation);
	}

	if(Out.size() > MatchReportLimits::MAX_PAYLOAD_SIZE)
		return Fail(pError, "packed report exceeds payload limit");
	Packed = std::move(Out);
	return true;
}

bool MatchReportFromPacked(const char *pData, size_t Size, CMatchReport &Report, std::string *pError)
{
	Report = {};
	CPackedReader Reader(pData, Size);
	if(pData == nullptr || Size == 0 || Size > MatchReportLimits::MAX_PAYLOAD_SIZE || (unsigned char)pData[0] != PACKED_FORMAT_VERSION)
		return Fail(pError, "unknown packed match report format");
	Reader.Raw(1);

	std::vector<std::string> vStrings;
	// Every string costs at least its length byte, so the size of the input is
	// the bound that keeps a bogus count from reserving gigabytes.
	const uint64_t NumStrings = Reader.Unsigned();
	if(Reader.Error() || NumStrings > Size)
		return Fail(pError, "malformed packed match report string table");
	// A payload can claim far more strings than it carries, so the count only
	// steers the first allocation and the loop below is what has to run out.
	vStrings.reserve(std::min<uint64_t>(NumStrings, 4096));
	for(uint64_t i = 0; i < NumStrings; ++i)
	{
		const uint64_t Length = Reader.Unsigned();
		const unsigned char *pBytes = Reader.Error() ? nullptr : Reader.Raw(Length);
		if(pBytes == nullptr)
			return Fail(pError, "truncated packed match report string");
		vStrings.emplace_back((const char *)pBytes, Length);
	}
	const auto String = [&](uint64_t Index, std::string &Out) {
		if(Index >= vStrings.size())
			return false;
		Out = vStrings[Index];
		return true;
	};
	// The enums are numbers on the wire and their names are what the rest of the
	// code validates, so an unknown number has to be rejected here.
	const auto Enum = [&](uint64_t Count) {
		const uint64_t Value = Reader.Unsigned();
		if(Value >= Count)
			return Count;
		return Value;
	};

	const unsigned char *pMatchId = Reader.Raw(sizeof(Report.m_MatchId));
	if(pMatchId == nullptr)
		return Fail(pError, "truncated packed match report");
	mem_copy(&Report.m_MatchId, pMatchId, sizeof(Report.m_MatchId));
	if(Reader.Bool())
	{
		const unsigned char *pGameUuid = Reader.Raw(sizeof(CUuid));
		if(pGameUuid == nullptr)
			return Fail(pError, "truncated packed match report");
		CUuid GameUuid;
		mem_copy(&GameUuid, pGameUuid, sizeof(GameUuid));
		Report.m_GameUuid = GameUuid;
	}
	Report.m_ReportSchemaVersion = Reader.Int();
	if(!String(Reader.Unsigned(), Report.m_ModeId))
		return Fail(pError, "packed match report references an unknown string");
	Report.m_ModeSchemaVersion = Reader.Int();
	if(!String(Reader.Unsigned(), Report.m_MapName))
		return Fail(pError, "packed match report references an unknown string");
	const unsigned char *pMapSha256 = Reader.Raw(sizeof(Report.m_MapSha256));
	if(pMapSha256 == nullptr)
		return Fail(pError, "truncated packed match report");
	mem_copy(&Report.m_MapSha256, pMapSha256, sizeof(Report.m_MapSha256));
	Report.m_StartTimeUtc = Reader.Signed();
	Report.m_EndTimeUtc = Reader.Signed();
	Report.m_DurationTicks = Reader.Signed();
	Report.m_TickRate = Reader.Int();
	Report.m_RoundStartTick = Reader.Int();
	const uint64_t Termination = Enum(3);
	if(Termination >= 3)
		return Fail(pError, "packed match report has an unknown termination");
	Report.m_Termination = (EMatchTermination)Termination;
	Report.m_Ranked = Reader.Bool();
	if(!String(Reader.Unsigned(), Report.m_UnrankedReason))
		return Fail(pError, "packed match report references an unknown string");

	const uint64_t NumTeams = Reader.Unsigned();
	if(Reader.Error() || NumTeams > (uint64_t)MatchReportLimits::MAX_TEAMS)
		return Fail(pError, "packed match report has too many teams");
	Report.m_vTeams.resize(NumTeams);
	for(CMatchTeam &Team : Report.m_vTeams)
	{
		Team.m_TeamId = Reader.Int();
		if(!String(Reader.Unsigned(), Team.m_DisplayName))
			return Fail(pError, "packed match report references an unknown string");
	}

	const uint64_t NumParticipants = Reader.Unsigned();
	if(Reader.Error() || NumParticipants > (uint64_t)MatchReportLimits::MAX_PARTICIPANTS)
		return Fail(pError, "packed match report has too many participants");
	Report.m_vParticipants.resize(NumParticipants);
	for(CMatchParticipant &Participant : Report.m_vParticipants)
	{
		Participant.m_ParticipantId = Reader.Int();
		if(Reader.Bool())
			Participant.m_TeamId = Reader.Int();
		if(!String(Reader.Unsigned(), Participant.m_DisplayName) || !String(Reader.Unsigned(), Participant.m_Clan))
			return Fail(pError, "packed match report references an unknown string");
		Participant.m_JoinedTick = Reader.Signed();
		if(Reader.Bool())
			Participant.m_LeftTick = Reader.Signed();
		Participant.m_Bot = Reader.Bool();
	}

	const uint64_t NumStandings = Reader.Unsigned();
	if(Reader.Error() || NumStandings > (uint64_t)MatchReportLimits::MAX_STANDINGS)
		return Fail(pError, "packed match report has too many standings");
	Report.m_vStandings.resize(NumStandings);
	for(CMatchStanding &Standing : Report.m_vStandings)
	{
		const uint64_t SubjectKind = Enum(3);
		const int SubjectId = Reader.Int();
		const int Rank = Reader.Int();
		const uint64_t Outcome = Enum(6);
		if(SubjectKind >= 3 || Outcome >= 6)
			return Fail(pError, "packed match report has an unknown standing kind");
		Standing.m_SubjectKind = (EMatchSubjectKind)SubjectKind;
		Standing.m_SubjectId = SubjectId;
		Standing.m_Rank = Rank;
		Standing.m_Outcome = (EMatchOutcome)Outcome;
	}

	const uint64_t NumMetrics = Reader.Unsigned();
	if(Reader.Error() || NumMetrics > (uint64_t)MatchReportLimits::MAX_METRICS)
		return Fail(pError, "packed match report has too many metrics");
	Report.m_vMetrics.resize(NumMetrics);
	for(CMatchMetric &Metric : Report.m_vMetrics)
	{
		const uint64_t SubjectKind = Enum(3);
		if(SubjectKind >= 3)
			return Fail(pError, "packed match report has an unknown metric kind");
		Metric.m_SubjectKind = (EMatchSubjectKind)SubjectKind;
		if(Reader.Bool())
			Metric.m_SubjectId = Reader.Int();
		if(!String(Reader.Unsigned(), Metric.m_MetricId))
			return Fail(pError, "packed match report references an unknown string");
		Metric.m_Value = Reader.Signed();
		// Aggregation is the one enum that starts below zero, so it is read as a
		// number and named before it becomes one.
		const int64_t Aggregation = Reader.Signed();
		if(Aggregation < (int64_t)EMatchMetricAggregation::SUM || Aggregation > (int64_t)EMatchMetricAggregation::MATCH_ONLY)
			return Fail(pError, "packed match report has an unknown metric aggregation");
		Metric.m_Aggregation = (EMatchMetricAggregation)Aggregation;
	}

	if(Reader.Error() || !Reader.AtEnd())
		return Fail(pError, "malformed packed match report");
	// The enums above are read as numbers, so the report is only trustworthy
	// after the same validation the JSON path runs.
	return MatchReportValidate(Report, pError);
}

bool MatchReportToJson(const CMatchReport &Report, std::string &Json, std::string *pError)
{
	if(!MatchReportValidate(Report, pError))
		return false;

	char aMatchId[UUID_MAXSTRSIZE];
	char aGameUuid[UUID_MAXSTRSIZE];
	char aMapSha256[SHA256_MAXSTRSIZE];
	FormatUuid(Report.m_MatchId, aMatchId, sizeof(aMatchId));
	if(Report.m_GameUuid.has_value())
		FormatUuid(*Report.m_GameUuid, aGameUuid, sizeof(aGameUuid));
	sha256_str(Report.m_MapSha256, aMapSha256, sizeof(aMapSha256));

	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("match_id");
	Writer.WriteStrValue(aMatchId);
	Writer.WriteAttribute("game_uuid");
	if(Report.m_GameUuid.has_value())
		Writer.WriteStrValue(aGameUuid);
	else
		Writer.WriteNullValue();
	Writer.WriteAttribute("report_schema_version");
	Writer.WriteIntValue(Report.m_ReportSchemaVersion);
	Writer.WriteAttribute("mode_id");
	Writer.WriteStrValue(Report.m_ModeId.c_str());
	Writer.WriteAttribute("mode_schema_version");
	Writer.WriteIntValue(Report.m_ModeSchemaVersion);
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
	Writer.WriteAttribute("ranked");
	Writer.WriteBoolValue(Report.m_Ranked);
	Writer.WriteAttribute("unranked_reason");
	if(Report.m_UnrankedReason.empty())
		Writer.WriteNullValue();
	else
		Writer.WriteStrValue(Report.m_UnrankedReason.c_str());

	Writer.WriteAttribute("teams");
	Writer.BeginArray();
	for(const CMatchTeam &Team : Report.m_vTeams)
	{
		Writer.BeginObject();
		Writer.WriteAttribute("team_id");
		Writer.WriteIntValue(Team.m_TeamId);
		Writer.WriteAttribute("display_name");
		if(Team.m_DisplayName.empty())
			Writer.WriteNullValue();
		else
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
		Writer.WriteAttribute("bot");
		Writer.WriteBoolValue(Participant.m_Bot);
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

	Json = Writer.GetOutputString();
	if(Json.size() > MatchReportLimits::MAX_PAYLOAD_SIZE)
	{
		Json.clear();
		return Fail(pError, "serialized report exceeds payload limit");
	}
	return true;
}

bool MatchReportFromJson(const char *pJson, size_t JsonSize, CMatchReport &Report, std::string *pError)
{
	if(pJson == nullptr || JsonSize == 0 || JsonSize > MatchReportLimits::MAX_PAYLOAD_SIZE)
		return Fail(pError, "invalid report payload size");
	json_settings Settings = {};
	Settings.max_memory = 4 * 1024 * 1024;
	char aJsonError[json_error_max];
	std::unique_ptr<json_value, decltype(&json_value_free)> pRoot(JsonParseEx(&Settings, pJson, JsonSize, aJsonError), json_value_free);
	if(pRoot == nullptr || pRoot->type != json_object)
		return Fail(pError, pRoot == nullptr ? aJsonError : "report root is not an object");

	CMatchReport Parsed;
	std::string Termination;
	std::string MapSha256;
	if(!ReadUuid(*pRoot, "match_id", Parsed.m_MatchId, pError) ||
		!ReadOptionalUuid(*pRoot, "game_uuid", Parsed.m_GameUuid, pError) ||
		!ReadInt(*pRoot, "report_schema_version", Parsed.m_ReportSchemaVersion, pError) ||
		!ReadString(*pRoot, "mode_id", Parsed.m_ModeId, pError) ||
		!ReadInt(*pRoot, "mode_schema_version", Parsed.m_ModeSchemaVersion, pError) ||
		!ReadString(*pRoot, "map_name", Parsed.m_MapName, pError) ||
		!ReadString(*pRoot, "map_sha256", MapSha256, pError) || sha256_from_str(&Parsed.m_MapSha256, MapSha256.c_str()) != 0 ||
		!ReadInt64(*pRoot, "start_time_utc", Parsed.m_StartTimeUtc, pError) ||
		!ReadInt64(*pRoot, "end_time_utc", Parsed.m_EndTimeUtc, pError) ||
		!ReadInt64(*pRoot, "duration_ticks", Parsed.m_DurationTicks, pError) ||
		!ReadInt(*pRoot, "tick_rate", Parsed.m_TickRate, pError) ||
		!ReadString(*pRoot, "termination", Termination, pError) || !ParseTermination(Termination.c_str(), Parsed.m_Termination) ||
		!ReadBool(*pRoot, "ranked", Parsed.m_Ranked, pError))
	{
		if(pError != nullptr && !pError->empty())
			return false;
		return Fail(pError, "invalid core report field");
	}
	const json_value *pRoundStartTick = json_object_get(pRoot.get(), "round_start_tick");
	if(pRoundStartTick->type != json_none && (pRoundStartTick->type != json_integer || !in_range<json_int_t>(pRoundStartTick->u.integer, 0, std::numeric_limits<int>::max())))
		return Fail(pError, "invalid round_start_tick");
	if(pRoundStartTick->type == json_integer)
		Parsed.m_RoundStartTick = static_cast<int>(pRoundStartTick->u.integer);

	const json_value *pUnrankedReason = json_object_get(pRoot.get(), "unranked_reason");
	if(pUnrankedReason->type == json_string)
		Parsed.m_UnrankedReason = pUnrankedReason->u.string.ptr;
	else if(pUnrankedReason->type != json_null)
		return Fail(pError, "invalid unranked_reason");

	const json_value *pTeams = RequiredValue(*pRoot, "teams", json_array, pError);
	const json_value *pParticipants = RequiredValue(*pRoot, "participants", json_array, pError);
	const json_value *pStandings = RequiredValue(*pRoot, "standings", json_array, pError);
	const json_value *pMetrics = RequiredValue(*pRoot, "metrics", json_array, pError);
	if(pTeams == nullptr || pParticipants == nullptr || pStandings == nullptr || pMetrics == nullptr ||
		pTeams->u.array.length > MatchReportLimits::MAX_TEAMS || pParticipants->u.array.length > MatchReportLimits::MAX_PARTICIPANTS || pStandings->u.array.length > MatchReportLimits::MAX_STANDINGS || pMetrics->u.array.length > MatchReportLimits::MAX_METRICS)
		return Fail(pError, "invalid report array");

	for(unsigned Index = 0; Index < pTeams->u.array.length; ++Index)
	{
		const json_value &Item = *pTeams->u.array.values[Index];
		if(Item.type != json_object)
			return Fail(pError, "team is not an object");
		CMatchTeam Team;
		if(!ReadInt(Item, "team_id", Team.m_TeamId, pError))
			return false;
		const json_value *pDisplayName = json_object_get(&Item, "display_name");
		if(pDisplayName->type == json_string)
			Team.m_DisplayName = pDisplayName->u.string.ptr;
		else if(pDisplayName->type != json_null)
			return Fail(pError, "invalid team display_name");
		Parsed.m_vTeams.push_back(std::move(Team));
	}

	for(unsigned Index = 0; Index < pParticipants->u.array.length; ++Index)
	{
		const json_value &Item = *pParticipants->u.array.values[Index];
		if(Item.type != json_object)
			return Fail(pError, "participant is not an object");
		CMatchParticipant Participant;
		if(!ReadInt(Item, "participant_id", Participant.m_ParticipantId, pError) ||
			!ReadOptionalInt(Item, "team_id", Participant.m_TeamId, pError) ||
			!ReadString(Item, "display_name", Participant.m_DisplayName, pError) ||
			!ReadString(Item, "clan", Participant.m_Clan, pError) ||
			!ReadInt64(Item, "joined_tick", Participant.m_JoinedTick, pError) ||
			!ReadOptionalInt64(Item, "left_tick", Participant.m_LeftTick, pError) ||
			!ReadBool(Item, "bot", Participant.m_Bot, pError))
			return false;
		Parsed.m_vParticipants.push_back(std::move(Participant));
	}

	for(unsigned Index = 0; Index < pStandings->u.array.length; ++Index)
	{
		const json_value &Item = *pStandings->u.array.values[Index];
		if(Item.type != json_object)
			return Fail(pError, "standing is not an object");
		CMatchStanding Standing;
		std::string SubjectKind;
		std::string Outcome;
		if(!ReadString(Item, "subject_kind", SubjectKind, pError) || !ParseSubjectKind(SubjectKind.c_str(), Standing.m_SubjectKind) || Standing.m_SubjectKind == EMatchSubjectKind::MATCH ||
			!ReadInt(Item, "subject_id", Standing.m_SubjectId, pError) ||
			!ReadInt(Item, "rank", Standing.m_Rank, pError) ||
			!ReadString(Item, "outcome", Outcome, pError) || !ParseOutcome(Outcome.c_str(), Standing.m_Outcome))
			return Fail(pError, "invalid standing");
		Parsed.m_vStandings.push_back(Standing);
	}

	for(unsigned Index = 0; Index < pMetrics->u.array.length; ++Index)
	{
		const json_value &Item = *pMetrics->u.array.values[Index];
		if(Item.type != json_object)
			return Fail(pError, "metric is not an object");
		CMatchMetric Metric;
		std::string SubjectKind;
		std::string Aggregation;
		if(!ReadString(Item, "subject_kind", SubjectKind, pError) || !ParseSubjectKind(SubjectKind.c_str(), Metric.m_SubjectKind) ||
			!ReadOptionalInt(Item, "subject_id", Metric.m_SubjectId, pError) ||
			!ReadString(Item, "metric_id", Metric.m_MetricId, pError) ||
			!ReadInt64(Item, "value", Metric.m_Value, pError))
			return Fail(pError, "invalid metric");
		const json_value *pAggregation = json_object_get(&Item, "aggregation");
		if(pAggregation->type == json_string)
		{
			Aggregation = pAggregation->u.string.ptr;
			if(!ParseMetricAggregation(Aggregation.c_str(), Metric.m_Aggregation))
				return Fail(pError, "invalid metric aggregation");
		}
		else if(pAggregation->type != json_none)
			return Fail(pError, "invalid metric aggregation");
		Parsed.m_vMetrics.push_back(std::move(Metric));
	}

	if(!MatchReportValidate(Parsed, pError))
		return false;
	Report = std::move(Parsed);
	return true;
}

CMatchReportBuilder::CMatchReportBuilder(CMatchReport Report) :
	m_Report(std::move(Report))
{
}

CMatchMetric *CMatchReportBuilder::FindMetric(EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pMetricId)
{
	// The scan is bounded by MAX_METRICS and only runs while a report is being
	// built, so an index would cost more than it saves.
	for(CMatchMetric &Metric : m_Report.m_vMetrics)
	{
		if(Metric.m_SubjectKind == SubjectKind && Metric.m_SubjectId == SubjectId && Metric.m_MetricId == pMetricId)
			return &Metric;
	}
	return nullptr;
}

bool CMatchReportBuilder::MutationFailed()
{
	m_MutationFailed = true;
	return false;
}

bool CMatchReportBuilder::AddTeam(CMatchTeam Team)
{
	if(m_Finalized)
		return false;
	if(m_Report.m_vTeams.size() >= MatchReportLimits::MAX_TEAMS || HasTeam(m_Report, Team.m_TeamId))
		return MutationFailed();
	m_Report.m_vTeams.push_back(std::move(Team));
	return true;
}

bool CMatchReportBuilder::AddParticipant(CMatchParticipant Participant)
{
	if(m_Finalized)
		return false;
	if(m_Report.m_vParticipants.size() >= MatchReportLimits::MAX_PARTICIPANTS || HasParticipant(m_Report, Participant.m_ParticipantId))
		return MutationFailed();
	m_Report.m_vParticipants.push_back(std::move(Participant));
	return true;
}

bool CMatchReportBuilder::AddStanding(CMatchStanding Standing)
{
	if(m_Finalized)
		return false;
	if(m_Report.m_vStandings.size() >= MatchReportLimits::MAX_STANDINGS)
		return MutationFailed();
	for(const CMatchStanding &Existing : m_Report.m_vStandings)
	{
		if(Existing.m_SubjectKind == Standing.m_SubjectKind && Existing.m_SubjectId == Standing.m_SubjectId)
			return MutationFailed();
	}
	m_Report.m_vStandings.push_back(Standing);
	return true;
}

bool CMatchReportBuilder::AddMetric(CMatchMetric Metric)
{
	if(m_Finalized)
		return false;
	if(m_Report.m_vMetrics.size() >= MatchReportLimits::MAX_METRICS || FindMetric(Metric.m_SubjectKind, Metric.m_SubjectId, Metric.m_MetricId.c_str()) != nullptr)
		return MutationFailed();
	m_Report.m_vMetrics.push_back(std::move(Metric));
	return true;
}

bool CMatchReportBuilder::AddMetricValue(EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pMetricId, int64_t Value, EMatchMetricAggregation Aggregation)
{
	if(m_Finalized)
		return false;
	if(pMetricId == nullptr)
		return MutationFailed();
	CMatchMetric *pMetric = FindMetric(SubjectKind, SubjectId, pMetricId);
	if(pMetric == nullptr)
		return AddMetric({SubjectKind, SubjectId, pMetricId, Value, Aggregation});
	if(pMetric->m_Aggregation != Aggregation)
		return MutationFailed();
	if((Value > 0 && pMetric->m_Value > std::numeric_limits<int64_t>::max() - Value) || (Value < 0 && pMetric->m_Value < std::numeric_limits<int64_t>::min() - Value))
		return MutationFailed();
	pMetric->m_Value += Value;
	return true;
}

bool CMatchReportBuilder::SetMetricValue(EMatchSubjectKind SubjectKind, std::optional<int> SubjectId, const char *pMetricId, int64_t Value, EMatchMetricAggregation Aggregation)
{
	if(m_Finalized)
		return false;
	if(pMetricId == nullptr)
		return MutationFailed();
	CMatchMetric *pMetric = FindMetric(SubjectKind, SubjectId, pMetricId);
	if(pMetric == nullptr)
		return AddMetric({SubjectKind, SubjectId, pMetricId, Value, Aggregation});
	if(pMetric->m_Aggregation != Aggregation)
		return MutationFailed();
	pMetric->m_Value = Value;
	return true;
}

bool CMatchReportBuilder::Finalize(std::string *pError)
{
	if(m_Finalized)
		return Fail(pError, "report already finalized");
	if(m_MutationFailed)
		return Fail(pError, "report builder mutation failed");
	if(!MatchReportValidate(m_Report, pError))
		return false;
	m_Finalized = true;
	return true;
}
