#ifndef GAME_CLIENT_MATCH_REPORT_VIEW_H
#define GAME_CLIENT_MATCH_REPORT_VIEW_H

#include "match_journal.h"

#include <base/str.h>
#include <base/time.h>

#include <engine/shared/localization.h>

#include <game/gamecore.h>
#include <game/localization.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <limits>
#include <optional>
#include <string>
#include <vector>

// a mod may report more weapons than the game knows, this only bounds a malformed report
inline constexpr int MAX_MATCH_WEAPONS = 32;

class CMatchWeaponStats
{
public:
	int m_Weapon = -1;
	std::array<int64_t, NUM_MATCH_COMBAT_STATS> m_aValues{};

	int64_t Value(EMatchCombatStat Stat) const { return m_aValues[Stat]; }
	bool HasData() const
	{
		return std::any_of(m_aValues.begin(), m_aValues.end(), [](int64_t Value) { return Value != 0; });
	}
	void Add(int Stat, int64_t Value)
	{
		// profile sums can come close to the limit, a sum over them must not overflow
		m_aValues[Stat] = std::min(m_aValues[Stat], std::numeric_limits<int64_t>::max() - std::max<int64_t>(0, Value)) + std::max<int64_t>(0, Value);
	}
};

class CMatchCombatStats
{
public:
	CMatchWeaponStats m_Total;
	// indexed by weapon, as long as the highest weapon reported
	std::vector<CMatchWeaponStats> m_vWeapons;

	bool HasData() const
	{
		return m_Total.HasData() || std::any_of(m_vWeapons.begin(), m_vWeapons.end(), [](const CMatchWeaponStats &Stats) { return Stats.HasData(); });
	}

	void Reset()
	{
		m_Total = {};
		m_vWeapons.clear();
	}

	// false if the metric is no combat counter
	bool Add(const std::string &MetricId, int64_t Value)
	{
		int Weapon;
		const CMatchMetricInfo *pInfo = FindMatchMetric(MetricId, &Weapon);
		if(!pInfo || pInfo->m_CombatStat < 0 || Weapon >= MAX_MATCH_WEAPONS)
			return false;
		if(Weapon < 0)
		{
			m_Total.Add(pInfo->m_CombatStat, Value);
			return true;
		}
		while((int)m_vWeapons.size() <= Weapon)
			m_vWeapons.emplace_back().m_Weapon = (int)m_vWeapons.size() - 1;
		m_vWeapons[Weapon].Add(pInfo->m_CombatStat, Value);
		return true;
	}
};

inline void BuildMatchCombatStats(const CMatchReport &Report, int ParticipantId, CMatchCombatStats &Stats)
{
	Stats.Reset();
	for(const CMatchMetric &Metric : Report.m_vMetrics)
		if(Metric.m_SubjectKind == EMatchSubjectKind::PARTICIPANT && Metric.m_SubjectId == ParticipantId)
			Stats.Add(Metric.m_MetricId, Metric.m_Value);
}

inline void BuildMatchCombatStats(const CMatchProfile &Profile, CMatchCombatStats &Stats)
{
	Stats.Reset();
	for(const CMatchMetricAggregate &Metric : Profile.m_vMetrics)
		Stats.Add(Metric.m_MetricId, Metric.m_Value);
}

class CMatchReportRow
{
public:
	const CMatchParticipant *m_pParticipant = nullptr;
	const CMatchStanding *m_pStanding = nullptr;
	std::optional<int64_t> m_Score;
	// the counters that are not broken down by weapon
	CMatchWeaponStats m_Combat;

	int Rank() const { return m_pStanding == nullptr ? std::numeric_limits<int>::max() : m_pStanding->m_Rank; }
};

// The participants of a report in the order of their standings, with what a board shows of them.
class CMatchReportRanking
{
	std::vector<CMatchReportRow> m_vRows;

public:
	void Update(const CMatchReport &Report)
	{
		// participant ids are handed out in order, so this is a direct index almost always
		const auto FindRow = [this](int ParticipantId) -> CMatchReportRow * {
			if(ParticipantId >= 0 && ParticipantId < (int)m_vRows.size() && m_vRows[ParticipantId].m_pParticipant->m_ParticipantId == ParticipantId)
				return &m_vRows[ParticipantId];
			const auto It = std::find_if(m_vRows.begin(), m_vRows.end(), [ParticipantId](const CMatchReportRow &Row) { return Row.m_pParticipant->m_ParticipantId == ParticipantId; });
			return It == m_vRows.end() ? nullptr : &*It;
		};
		m_vRows.clear();
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
			m_vRows.push_back({&Participant});
		for(const CMatchStanding &Standing : Report.m_vStandings)
			if(CMatchReportRow *pRow = Standing.m_SubjectKind == EMatchSubjectKind::PARTICIPANT ? FindRow(Standing.m_SubjectId) : nullptr)
				pRow->m_pStanding = &Standing;
		for(const CMatchMetric &Metric : Report.m_vMetrics)
		{
			CMatchReportRow *pRow = Metric.m_SubjectKind == EMatchSubjectKind::PARTICIPANT ? FindRow(*Metric.m_SubjectId) : nullptr;
			if(!pRow)
				continue;
			int Weapon;
			const CMatchMetricInfo *pInfo = FindMatchMetric(Metric.m_MetricId, &Weapon);
			if(Metric.m_MetricId == "score")
				pRow->m_Score = Metric.m_Value;
			else if(pInfo && pInfo->m_CombatStat >= 0 && Weapon < 0)
				pRow->m_Combat.Add(pInfo->m_CombatStat, Metric.m_Value);
		}
		std::sort(m_vRows.begin(), m_vRows.end(), [](const CMatchReportRow &Left, const CMatchReportRow &Right) {
			if(Left.Rank() != Right.Rank())
				return Left.Rank() < Right.Rank();
			if(Left.m_Score.value_or(0) != Right.m_Score.value_or(0))
				return Left.m_Score.value_or(0) > Right.m_Score.value_or(0);
			return Left.m_pParticipant->m_ParticipantId < Right.m_pParticipant->m_ParticipantId;
		});
	}

	const std::vector<CMatchReportRow> &Rows() const { return m_vRows; }
	const CMatchReportRow *Row(int ParticipantId) const
	{
		const auto It = std::find_if(m_vRows.begin(), m_vRows.end(), [ParticipantId](const CMatchReportRow &Row) { return Row.m_pParticipant->m_ParticipantId == ParticipantId; });
		return It == m_vRows.end() ? nullptr : &*It;
	}
};

inline EMatchMetricCategory MatchMetricCategory(const std::string &MetricId)
{
	const CMatchMetricInfo *pInfo = FindMatchMetric(MetricId);
	return pInfo ? pInfo->m_Category : EMatchMetricCategory::OTHER;
}

inline bool IsMatchCombatStatMetric(const std::string &MetricId)
{
	const CMatchMetricInfo *pInfo = FindMatchMetric(MetricId);
	return pInfo && pInfo->m_CombatStat >= 0;
}

inline const char *MatchMetricCategoryDisplayName(EMatchMetricCategory Category)
{
	switch(Category)
	{
	case EMatchMetricCategory::OVERVIEW: return Localize("Overview");
	case EMatchMetricCategory::COMBAT: return Localize("Combat");
	case EMatchMetricCategory::OBJECTIVES: return Localize("Objectives");
	case EMatchMetricCategory::OTHER: return Localize("Other metrics");
	}
	return "";
}

inline void MatchMetricDisplayName(const std::string &MetricId, char *pBuffer, int BufferSize)
{
	if(const CMatchMetricInfo *pInfo = FindMatchMetric(MetricId); pInfo && MetricId == pInfo->m_pId)
	{
		str_copy(pBuffer, Localize(pInfo->m_pName), BufferSize);
		return;
	}
	// a metric of a mod, its id is all there is to name it
	str_copy(pBuffer, MetricId.c_str(), BufferSize);
	for(char *pChar = pBuffer; *pChar != '\0'; ++pChar)
		if(*pChar == '_')
			*pChar = ' ';
	pBuffer[0] = str_uppercase(pBuffer[0]);
}

inline void MatchWeaponDisplayName(int Weapon, char *pBuffer, int BufferSize)
{
	static const char *const s_apNames[] = {Localizable("Hammer"), Localizable("Gun"), Localizable("Shotgun"), Localizable("Grenade"), Localizable("Laser"), Localizable("Ninja")};
	static_assert(std::size(s_apNames) == NUM_WEAPONS);
	if(Weapon >= 0 && Weapon < NUM_WEAPONS)
		str_copy(pBuffer, Localize(s_apNames[Weapon]), BufferSize);
	else
		str_format(pBuffer, BufferSize, "%s %d", Localize("Weapon"), Weapon);
}

/**
 * Hits over shots as a percentage, capped at a hundred.
 *
 * A shot is one use of the weapon and a hit is one damage event, so a shotgun
 * blast or a grenade that catches two players produces more hits than shots.
 */
inline void FormatMatchAccuracy(int64_t Hits, int64_t Shots, char *pBuffer, int BufferSize)
{
	if(Shots <= 0)
		str_copy(pBuffer, "-", BufferSize);
	else
		str_format(pBuffer, BufferSize, "%.1f%%", std::min(100.0, 100.0 * (double)Hits / (double)Shots));
}

inline void FormatMatchDuration(int64_t DurationTicks, int TickRate, char *pBuffer, int BufferSize)
{
	const int64_t Centiseconds = TickRate > 0 && DurationTicks > 0 ? DurationTicks / TickRate * 100 + DurationTicks % TickRate * 100 / TickRate : 0;
	str_time(Centiseconds, ETimeFormat::HOURS, pBuffer, BufferSize);
}

inline void FormatMatchMetricValue(const CMatchMetric &Metric, int TickRate, char *pBuffer, int BufferSize)
{
	const CMatchMetricInfo *pInfo = FindMatchMetric(Metric.m_MetricId);
	const EMatchMetricFormat Format = pInfo ? pInfo->m_Format : EMatchMetricFormat::NUMBER;
	if(Format == EMatchMetricFormat::TICKS)
		FormatMatchDuration(Metric.m_Value, TickRate, pBuffer, BufferSize);
	else if(Format == EMatchMetricFormat::RANK && Metric.m_Value > 0)
		str_format(pBuffer, BufferSize, "#%" PRId64, Metric.m_Value);
	else
		str_format(pBuffer, BufferSize, "%" PRId64, Metric.m_Value);
}

inline void FormatMatchSeconds(int64_t Seconds, char *pBuffer, int BufferSize)
{
	str_time(std::clamp<int64_t>(Seconds, 0, std::numeric_limits<int64_t>::max() / 100) * 100, ETimeFormat::HOURS, pBuffer, BufferSize);
}

inline void FormatMatchTimestamp(int64_t Timestamp, char *pBuffer, int BufferSize)
{
	str_timestamp_ex((time_t)std::clamp<int64_t>(Timestamp, 0, MatchReportLimits::MAX_TIME_UTC), pBuffer, BufferSize, TimestampFormat::SPACE);
}

inline const char *MatchReportSourceDisplayName(EMatchReportSource Source)
{
	static const char *const s_apNames[] = {Localizable("Client observation"), Localizable("Server report"), Localizable("Server snapshot")};
	static_assert(std::size(s_apNames) == (size_t)EMatchReportSource::NUM);
	return Localize(s_apNames[(int)Source]);
}

inline const char *MatchCompletenessDisplayName(EMatchCompleteness Completeness)
{
	static const char *const s_apNames[] = {Localizable("Complete"), Localizable("Partial since join"), Localizable("Aborted")};
	static_assert(std::size(s_apNames) == (size_t)EMatchCompleteness::NUM);
	return Localize(s_apNames[(int)Completeness]);
}

inline const char *MatchOutcomeDisplayName(EMatchOutcome Outcome)
{
	static const char *const s_apNames[] = {Localizable("Win"), Localizable("Loss"), Localizable("Draw"), Localizable("Finished"), Localizable("Did not finish"), Localizable("Disqualified")};
	static_assert(std::size(s_apNames) == (size_t)EMatchOutcome::NUM);
	return (size_t)Outcome < std::size(s_apNames) ? Localize(s_apNames[(int)Outcome]) : "";
}

inline const char *MatchTerminationDisplayName(EMatchTermination Termination)
{
	static const char *const s_apNames[] = {Localizable("Completed"), Localizable("Aborted"), Localizable("Ended by administrator")};
	static_assert(std::size(s_apNames) == (size_t)EMatchTermination::NUM);
	return Localize(s_apNames[(int)Termination]);
}

#endif // GAME_CLIENT_MATCH_REPORT_VIEW_H
