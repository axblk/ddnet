#ifndef GAME_CLIENT_MATCH_REPORT_VIEW_H
#define GAME_CLIENT_MATCH_REPORT_VIEW_H

#include "match_journal.h"

#include <base/str.h>
#include <base/time.h>

#include <game/gamecore.h>
#include <game/localization.h>

#include <algorithm>
#include <array>
#include <cinttypes>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class EMatchMetricCategory
{
	OVERVIEW,
	COMBAT,
	WEAPONS,
	OBJECTIVES,
	OTHER,
};

class CMatchWeaponStats
{
public:
	int m_Weapon = -1;
	int64_t m_Shots = 0;
	int64_t m_Hits = 0;
	int64_t m_DamageDone = 0;
	int64_t m_DamageTaken = 0;

	bool HasData() const { return m_Shots != 0 || m_Hits != 0 || m_DamageDone != 0 || m_DamageTaken != 0; }
};

/**
 * Highest weapon index a report may use.
 *
 * The game knows `NUM_WEAPONS` of them, but a mod is free to report more. The
 * limit only exists so that a malformed report cannot ask for an unbounded
 * number of rows.
 */
inline constexpr int MAX_MATCH_WEAPONS = 32;

class CMatchCombatStats
{
public:
	CMatchWeaponStats m_Total;
	// Indexed by weapon, grown to whatever the report actually used
	std::vector<CMatchWeaponStats> m_vWeapons;

	/**
	 * @param Weapon Weapon index, valid below @link MAX_MATCH_WEAPONS @endlink.
	 *
	 * @return The entry for that weapon, created if it did not exist.
	 */
	CMatchWeaponStats *Weapon(int Weapon)
	{
		if(Weapon < 0 || Weapon >= MAX_MATCH_WEAPONS)
			return nullptr;
		while(static_cast<int>(m_vWeapons.size()) <= Weapon)
		{
			CMatchWeaponStats Stats;
			Stats.m_Weapon = static_cast<int>(m_vWeapons.size());
			m_vWeapons.push_back(Stats);
		}
		return &m_vWeapons[Weapon];
	}

	bool HasData() const
	{
		return m_Total.HasData() || std::any_of(m_vWeapons.begin(), m_vWeapons.end(), [](const CMatchWeaponStats &Stats) { return Stats.HasData(); });
	}
};

inline std::string_view MatchMetricSuffix(const std::string &MetricId)
{
	const size_t Slash = MetricId.rfind('/');
	return Slash == std::string::npos ? std::string_view(MetricId) : std::string_view(MetricId).substr(Slash + 1);
}

/**
 * Looks up the standing of one subject in a report.
 *
 * @param Report Report to search.
 * @param SubjectKind Whether the subject is a participant or a team.
 * @param SubjectId Id of the subject within its kind.
 *
 * @return The standing, or nullptr when the subject has none.
 */
inline const CMatchStanding *ReportStanding(const CMatchReport &Report, EMatchSubjectKind SubjectKind, int SubjectId)
{
	for(const CMatchStanding &Standing : Report.m_vStandings)
	{
		if(Standing.m_SubjectKind == SubjectKind && Standing.m_SubjectId == SubjectId)
			return &Standing;
	}
	return nullptr;
}

/**
 * Looks up one metric of one subject in a report.
 *
 * The mode part of the metric id is ignored, so the same call works whatever
 * mode produced the report.
 *
 * @param Report Report to search.
 * @param SubjectKind Whether the subject is a participant or a team.
 * @param SubjectId Id of the subject within its kind.
 * @param pSuffix Metric id without its mode prefix, for example "score".
 *
 * @return The value, or nothing when the subject does not have that metric.
 */
inline std::optional<int64_t> ReportMetric(const CMatchReport &Report, EMatchSubjectKind SubjectKind, int SubjectId, const char *pSuffix)
{
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		if(Metric.m_SubjectKind == SubjectKind && Metric.m_SubjectId == SubjectId && MatchMetricSuffix(Metric.m_MetricId) == pSuffix)
			return Metric.m_Value;
	}
	return std::nullopt;
}

inline bool IsKnownMatchMetric(const std::string &MetricId, int ModeSchemaVersion)
{
	// Every metric id is `<mode id>/<suffix>`. The suffix vocabulary below is a
	// convention that any mode can use, including one this build has never
	// heard of; only the meaning of a suffix is tied to the schema version.
	if(ModeSchemaVersion != 1 || MetricId.rfind('/') == std::string::npos)
		return false;
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	if(Suffix == "score" || Suffix == "playtime_ticks" || Suffix == "sudden_death" || Suffix == "kills" || Suffix == "deaths" || Suffix == "suicides" || Suffix == "best_spree" || Suffix == "shots" || Suffix == "hits" || Suffix == "damage_done" || Suffix == "damage_taken" || Suffix == "catches" || Suffix == "personal_best_ticks" || Suffix == "map_best_ticks" || Suffix == "map_rank" || Suffix == "map_finishes" || Suffix == "session_finishes" || Suffix == "last_finish_ticks" || Suffix == "current_run_ticks" || Suffix == "current_checkpoint" || Suffix == "flag_grabs" || Suffix == "flag_returns" || Suffix == "flag_captures" || Suffix.starts_with("weapon_"))
		return true;
	return false;
}

inline EMatchMetricCategory MatchMetricCategory(const std::string &MetricId, int ModeSchemaVersion)
{
	if(!IsKnownMatchMetric(MetricId, ModeSchemaVersion))
		return EMatchMetricCategory::OTHER;
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	if(Suffix == "score" || Suffix == "playtime_ticks" || Suffix == "sudden_death" || Suffix == "personal_best_ticks" || Suffix == "map_best_ticks" || Suffix == "map_rank" || Suffix == "map_finishes" || Suffix == "session_finishes" || Suffix == "last_finish_ticks" || Suffix == "current_run_ticks" || Suffix == "current_checkpoint")
		return EMatchMetricCategory::OVERVIEW;
	if(Suffix.starts_with("weapon_"))
		return EMatchMetricCategory::WEAPONS;
	if(Suffix == "flag_grabs" || Suffix == "flag_returns" || Suffix == "flag_captures" || Suffix == "catches")
		return EMatchMetricCategory::OBJECTIVES;
	if(Suffix == "kills" || Suffix == "deaths" || Suffix == "suicides" || Suffix == "best_spree" || Suffix == "shots" || Suffix == "hits" || Suffix == "damage_done" || Suffix == "damage_taken")
		return EMatchMetricCategory::COMBAT;
	return EMatchMetricCategory::OTHER;
}

inline const char *MatchMetricCategoryDisplayName(EMatchMetricCategory Category)
{
	switch(Category)
	{
	case EMatchMetricCategory::OVERVIEW: return Localize("Overview");
	case EMatchMetricCategory::COMBAT: return Localize("Combat");
	case EMatchMetricCategory::WEAPONS: return Localize("Weapons");
	case EMatchMetricCategory::OBJECTIVES: return Localize("Objectives");
	case EMatchMetricCategory::OTHER: return Localize("Other metrics");
	}
	return "";
}

inline std::string MatchMetricDisplayName(const std::string &MetricId, int ModeSchemaVersion)
{
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	if(IsKnownMatchMetric(MetricId, ModeSchemaVersion))
	{
		if(Suffix == "score")
			return Localize("Score");
		if(Suffix == "playtime_ticks")
			return Localize("Play time");
		if(Suffix == "sudden_death")
			return Localize("Sudden death");
		if(Suffix == "kills")
			return Localize("Kills");
		if(Suffix == "deaths")
			return Localize("Deaths");
		if(Suffix == "suicides")
			return Localize("Suicides");
		if(Suffix == "best_spree")
			return Localize("Best spree");
		if(Suffix == "shots")
			return Localize("Shots");
		if(Suffix == "hits")
			return Localize("Hits");
		if(Suffix == "damage_done")
			return Localize("Damage done");
		if(Suffix == "damage_taken")
			return Localize("Damage taken");
		if(Suffix == "flag_grabs")
			return Localize("Flag grabs");
		if(Suffix == "flag_returns")
			return Localize("Flag returns");
		if(Suffix == "flag_captures")
			return Localize("Flag captures");
		if(Suffix == "catches")
			return Localize("Catches");
		if(Suffix == "personal_best_ticks")
			return Localize("Personal best");
		if(Suffix == "map_best_ticks")
			return Localize("Map best");
		if(Suffix == "map_rank")
			return Localize("Map rank");
		if(Suffix == "map_finishes")
			return Localize("Stored map finishes");
		if(Suffix == "session_finishes")
			return Localize("Session finishes");
		if(Suffix == "last_finish_ticks")
			return Localize("Last finish");
		if(Suffix == "current_run_ticks")
			return Localize("Current run");
		if(Suffix == "current_checkpoint")
			return Localize("Current checkpoint");
	}
	// A metric this build does not know still belongs on the page. Its suffix is
	// the only readable part of the id, so it is shown as a sentence rather than
	// as `somemod@example.org/flag_time`.
	std::string Label(Suffix);
	std::replace(Label.begin(), Label.end(), '_', ' ');
	if(!Label.empty())
		Label[0] = str_uppercase(Label[0]);
	return Label;
}

inline std::string MatchWeaponDisplayName(int Weapon)
{
	switch(Weapon)
	{
	case WEAPON_HAMMER: return Localize("Hammer");
	case WEAPON_GUN: return Localize("Gun");
	case WEAPON_SHOTGUN: return Localize("Shotgun");
	case WEAPON_GRENADE: return Localize("Grenade");
	case WEAPON_LASER: return Localize("Laser");
	case WEAPON_NINJA: return Localize("Ninja");
	}
	// A mod may report weapons this build has no name or sprite for
	char aBuf[32];
	str_format(aBuf, sizeof(aBuf), "%s %d", Localize("Weapon"), Weapon);
	return aBuf;
}

inline void AddMatchCombatValue(int64_t &Target, int64_t Value)
{
	Value = std::max<int64_t>(0, Value);
	Target = Value > std::numeric_limits<int64_t>::max() - Target ? std::numeric_limits<int64_t>::max() : Target + Value;
}

inline bool AddMatchCombatMetric(CMatchCombatStats &Stats, const std::string &MetricId, int ModeSchemaVersion, int64_t Value)
{
	if(!IsKnownMatchMetric(MetricId, ModeSchemaVersion))
		return false;
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	CMatchWeaponStats *pStats = &Stats.m_Total;
	std::string_view StatName = Suffix;
	if(Suffix.starts_with("weapon_"))
	{
		const std::string_view WeaponMetric = Suffix.substr(7);
		const size_t Separator = WeaponMetric.find('_');
		if(Separator == std::string_view::npos || Separator == 0 || Separator > 2)
			return false;
		int Weapon = 0;
		for(size_t Digit = 0; Digit < Separator; ++Digit)
		{
			if(WeaponMetric[Digit] < '0' || WeaponMetric[Digit] > '9')
				return false;
			Weapon = Weapon * 10 + (WeaponMetric[Digit] - '0');
		}
		pStats = Stats.Weapon(Weapon);
		if(pStats == nullptr)
			return false;
		StatName = WeaponMetric.substr(Separator + 1);
	}

	if(StatName == "shots")
		AddMatchCombatValue(pStats->m_Shots, Value);
	else if(StatName == "hits")
		AddMatchCombatValue(pStats->m_Hits, Value);
	else if(StatName == "damage_done")
		AddMatchCombatValue(pStats->m_DamageDone, Value);
	else if(StatName == "damage_taken")
		AddMatchCombatValue(pStats->m_DamageTaken, Value);
	else
		return false;
	return true;
}

inline bool IsMatchCombatStatMetric(const std::string &MetricId, int ModeSchemaVersion)
{
	CMatchCombatStats Stats;
	return AddMatchCombatMetric(Stats, MetricId, ModeSchemaVersion, 0);
}

inline CMatchCombatStats BuildMatchCombatStats(const CMatchReport &Report, int ParticipantId)
{
	CMatchCombatStats Stats;
	for(const CMatchMetric &Metric : Report.m_vMetrics)
		if(Metric.m_SubjectKind == EMatchSubjectKind::PARTICIPANT && Metric.m_SubjectId == ParticipantId)
			AddMatchCombatMetric(Stats, Metric.m_MetricId, Report.m_ModeSchemaVersion, Metric.m_Value);
	return Stats;
}

inline CMatchCombatStats BuildMatchCombatStats(const CMatchProfile &Profile)
{
	CMatchCombatStats Stats;
	for(const CMatchMetricAggregate &Metric : Profile.m_vMetrics)
		AddMatchCombatMetric(Stats, Metric.m_MetricId, Metric.m_ModeSchemaVersion, Metric.m_Value);
	return Stats;
}

inline void FormatMatchAccuracy(int64_t Hits, int64_t Shots, char *pBuffer, int BufferSize)
{
	if(Shots <= 0)
		str_copy(pBuffer, "-", BufferSize);
	else
		str_format(pBuffer, BufferSize, "%.1f%%", 100.0 * static_cast<double>(Hits) / static_cast<double>(Shots));
}

inline void FormatMatchDuration(int64_t DurationTicks, int TickRate, char *pBuffer, int BufferSize)
{
	const int64_t Centiseconds = TickRate > 0 && DurationTicks > 0 ? DurationTicks / TickRate * 100 + DurationTicks % TickRate * 100 / TickRate : 0;
	str_time(Centiseconds, ETimeFormat::HOURS, pBuffer, BufferSize);
}

inline void FormatMatchMetricValue(const CMatchMetric &Metric, int ModeSchemaVersion, int TickRate, char *pBuffer, int BufferSize)
{
	const std::string_view Suffix = MatchMetricSuffix(Metric.m_MetricId);
	if(!IsKnownMatchMetric(Metric.m_MetricId, ModeSchemaVersion))
		str_format(pBuffer, BufferSize, "%" PRId64, Metric.m_Value);
	else if(Suffix == "playtime_ticks" || Suffix == "personal_best_ticks" || Suffix == "map_best_ticks" || Suffix == "last_finish_ticks" || Suffix == "current_run_ticks")
		FormatMatchDuration(Metric.m_Value, TickRate, pBuffer, BufferSize);
	else if(Suffix == "map_rank" && Metric.m_Value > 0)
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
	str_timestamp_ex(static_cast<time_t>(std::clamp<int64_t>(Timestamp, 0, MatchReportLimits::MAX_TIME_UTC)), pBuffer, BufferSize, TimestampFormat::SPACE);
}

inline const char *MatchReportSourceDisplayName(EMatchReportSource Source)
{
	switch(Source)
	{
	case EMatchReportSource::CLIENT_OBSERVED: return Localize("Client observation");
	case EMatchReportSource::SERVER_REPORT: return Localize("Server report");
	case EMatchReportSource::SERVER_DATABASE: return Localize("Server database");
	case EMatchReportSource::SERVER_SNAPSHOT: return Localize("Server snapshot");
	}
	return "";
}

inline const char *MatchCompletenessDisplayName(EMatchCompleteness Completeness)
{
	switch(Completeness)
	{
	case EMatchCompleteness::COMPLETE: return Localize("Complete");
	case EMatchCompleteness::PARTIAL_SINCE_JOIN: return Localize("Partial since join");
	case EMatchCompleteness::ABORTED: return Localize("Aborted");
	}
	return "";
}

inline const char *MatchOutcomeDisplayName(EMatchOutcome Outcome)
{
	switch(Outcome)
	{
	case EMatchOutcome::WIN: return Localize("Win");
	case EMatchOutcome::LOSS: return Localize("Loss");
	case EMatchOutcome::DRAW: return Localize("Draw");
	case EMatchOutcome::FINISHED: return Localize("Finished");
	case EMatchOutcome::DNF: return Localize("Did not finish");
	case EMatchOutcome::DISQUALIFIED: return Localize("Disqualified");
	}
	return "";
}

inline const char *MatchTerminationDisplayName(EMatchTermination Termination)
{
	switch(Termination)
	{
	case EMatchTermination::COMPLETED: return Localize("Completed");
	case EMatchTermination::ABORTED: return Localize("Aborted");
	case EMatchTermination::ADMIN_ENDED: return Localize("Ended by administrator");
	}
	return "";
}

#endif // GAME_CLIENT_MATCH_REPORT_VIEW_H
