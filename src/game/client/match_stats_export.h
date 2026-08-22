#ifndef GAME_CLIENT_MATCH_STATS_EXPORT_H
#define GAME_CLIENT_MATCH_STATS_EXPORT_H

#include "match_journal.h"

#include <engine/shared/csv.h>
#include <engine/shared/jsonwriter.h>

#include <iterator>
#include <string>

inline std::string MatchStatsExportJson(const CStoredMatch &Stored)
{
	CJsonStringWriter Writer;
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
	Writer.WriteAttribute("raw_report");
	Writer.WriteStrValue(Stored.m_RawReport.c_str());
	Writer.EndObject();
	return Writer.GetOutputString();
}

inline void MatchStatsExportCsv(IOHANDLE File, const CStoredMatch &Stored)
{
	static const char *const apHeaders[] = {"origin_id", "match_id", "mode_id", "map_name", "end_time_utc", "duration_ticks", "tick_rate", "source", "completeness", "local_participant_id", "subject_kind", "subject_id", "metric_id", "value", "aggregation"};
	CsvWrite(File, std::size(apHeaders), apHeaders);
	char aMatchId[UUID_MAXSTRSIZE];
	FormatUuid(Stored.m_Report.m_MatchId, aMatchId, sizeof(aMatchId));
	const std::string EndTime = std::to_string(Stored.m_Report.m_EndTimeUtc);
	const std::string Duration = std::to_string(Stored.m_Report.m_DurationTicks);
	const std::string TickRate = std::to_string(Stored.m_Report.m_TickRate);
	const std::string LocalParticipantId = Stored.m_LocalParticipantId.has_value() ? std::to_string(*Stored.m_LocalParticipantId) : "";
	const auto WriteRow = [&](const CMatchMetric *pMetric) {
		const std::string SubjectId = pMetric != nullptr && pMetric->m_SubjectId.has_value() ? std::to_string(*pMetric->m_SubjectId) : "";
		const std::string Value = pMetric != nullptr ? std::to_string(pMetric->m_Value) : "";
		const char *apColumns[] = {Stored.m_OriginId.c_str(), aMatchId, Stored.m_Report.m_ModeId.c_str(), Stored.m_Report.m_MapName.c_str(), EndTime.c_str(), Duration.c_str(), TickRate.c_str(), MatchReportSourceName(Stored.m_Source), MatchCompletenessName(Stored.m_Completeness), LocalParticipantId.c_str(), pMetric != nullptr ? MatchSubjectKindName(pMetric->m_SubjectKind) : "", SubjectId.c_str(), pMetric != nullptr ? pMetric->m_MetricId.c_str() : "", Value.c_str(), pMetric != nullptr ? MatchMetricAggregationName(pMetric->m_Aggregation) : ""};
		CsvWrite(File, std::size(apColumns), apColumns);
	};
	if(Stored.m_Report.m_vMetrics.empty())
		WriteRow(nullptr);
	else
		for(const CMatchMetric &Metric : Stored.m_Report.m_vMetrics)
			WriteRow(&Metric);
}

#endif // GAME_CLIENT_MATCH_STATS_EXPORT_H
