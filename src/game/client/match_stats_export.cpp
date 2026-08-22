#include "match_stats_export.h"

#include <engine/shared/csv.h>
#include <engine/shared/jsonwriter.h>

#include <iterator>
#include <string>

void MatchStatsExportJson(CJsonWriter &Writer, const CStoredMatch &Stored)
{
	Writer.BeginObject();
	Writer.WriteAttribute("origin_id");
	Writer.WriteStrValue(Stored.m_OriginId.c_str());
	Writer.WriteAttribute("source");
	Writer.WriteStrValue(MatchReportSourceName(Stored.m_Source));
	Writer.WriteAttribute("completeness");
	Writer.WriteStrValue(MatchCompletenessName(Stored.m_Completeness));
	Writer.WriteAttribute("local_participant_id");
	if(Stored.m_LocalParticipantId.has_value())
		Writer.WriteIntValue(*Stored.m_LocalParticipantId);
	else
		Writer.WriteNullValue();
	Writer.WriteAttribute("report");
	MatchReportWriteJson(Writer, Stored.m_Report);
	Writer.EndObject();
}

void MatchStatsExportCsv(IOHANDLE File, const CStoredMatch &Stored)
{
	static const char *const s_apHeaders[] = {"origin_id", "match_id", "mode_id", "map_name", "end_time_utc", "duration_ticks", "tick_rate", "source", "completeness", "local_participant_id", "subject_kind", "subject_id", "metric_id", "value", "aggregation"};
	CsvWrite(File, std::size(s_apHeaders), s_apHeaders);
	const CMatchReport &Report = Stored.m_Report;
	char aMatchId[UUID_MAXSTRSIZE];
	FormatUuid(Report.m_MatchId, aMatchId, sizeof(aMatchId));
	const std::string EndTime = std::to_string(Report.m_EndTimeUtc);
	const std::string Duration = std::to_string(Report.m_DurationTicks);
	const std::string TickRate = std::to_string(Report.m_TickRate);
	const std::string LocalParticipantId = Stored.m_LocalParticipantId.has_value() ? std::to_string(*Stored.m_LocalParticipantId) : "";
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		const std::string SubjectId = Metric.m_SubjectId.has_value() ? std::to_string(*Metric.m_SubjectId) : "";
		const std::string Value = std::to_string(Metric.m_Value);
		const char *apColumns[] = {Stored.m_OriginId.c_str(), aMatchId, Report.m_ModeId.c_str(), Report.m_MapName.c_str(), EndTime.c_str(), Duration.c_str(), TickRate.c_str(), MatchReportSourceName(Stored.m_Source), MatchCompletenessName(Stored.m_Completeness), LocalParticipantId.c_str(), MatchSubjectKindName(Metric.m_SubjectKind), SubjectId.c_str(), Metric.m_MetricId.c_str(), Value.c_str(), MatchMetricAggregationName(Metric.m_Aggregation)};
		CsvWrite(File, std::size(apColumns), apColumns);
	}
}
