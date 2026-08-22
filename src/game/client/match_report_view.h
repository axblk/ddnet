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
#include <string>
#include <string_view>

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

class CMatchCombatStats
{
public:
	CMatchWeaponStats m_Total;
	std::array<CMatchWeaponStats, NUM_WEAPONS> m_aWeapons;

	CMatchCombatStats()
	{
		for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
			m_aWeapons[Weapon].m_Weapon = Weapon;
	}

	bool HasData() const
	{
		return m_Total.HasData() || std::any_of(m_aWeapons.begin(), m_aWeapons.end(), [](const CMatchWeaponStats &Stats) { return Stats.HasData(); });
	}
};

inline std::string_view MatchMetricSuffix(const std::string &MetricId)
{
	const size_t Slash = MetricId.rfind('/');
	return Slash == std::string::npos ? std::string_view(MetricId) : std::string_view(MetricId).substr(Slash + 1);
}

inline bool IsKnownMatchMetric(const std::string &MetricId, int ModeSchemaVersion)
{
	if(ModeSchemaVersion != 1)
		return false;
	const size_t Slash = MetricId.rfind('/');
	if(Slash == std::string::npos)
		return false;
	const std::string_view ModeId(MetricId.data(), Slash);
	static constexpr auto s_aKnownModes = std::to_array<std::string_view>({
		"vanilla.dm@ddnet.org",
		"vanilla.1on1@ddnet.org",
		"vanilla.tdm@ddnet.org",
		"vanilla.ctf@ddnet.org",
		"insta.idm@ddnet.org",
		"insta.itdm@ddnet.org",
		"insta.ictf@ddnet.org",
		"insta.gdm@ddnet.org",
		"insta.gtdm@ddnet.org",
		"insta.gctf@ddnet.org",
		"zcatch.laser@ddnet.org",
		"ddnet.race@ddnet.org",
		"ddrace.mod@ddnet.org",
	});
	if(std::find(s_aKnownModes.begin(), s_aKnownModes.end(), ModeId) == s_aKnownModes.end())
		return false;
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	if(Suffix == "score" || Suffix == "playtime_ticks" || Suffix == "sudden_death" || Suffix == "kills" || Suffix == "deaths" || Suffix == "suicides" || Suffix == "best_spree" || Suffix == "shots" || Suffix == "hits" || Suffix == "damage_done" || Suffix == "damage_taken" || Suffix == "catches" || Suffix == "personal_best_ticks" || Suffix == "map_best_ticks" || Suffix == "map_rank" || Suffix == "map_finishes" || Suffix == "session_finishes" || Suffix == "last_finish_ticks" || Suffix == "current_run_ticks" || Suffix == "current_checkpoint" || Suffix.starts_with("weapon_"))
		return true;
	return ModeId.find("ctf@ddnet.org") != std::string_view::npos && (Suffix == "flag_grabs" || Suffix == "flag_returns" || Suffix == "flag_captures");
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

inline const char *MatchMetricDisplayName(const std::string &MetricId, int ModeSchemaVersion)
{
	if(!IsKnownMatchMetric(MetricId, ModeSchemaVersion))
		return MetricId.c_str();
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
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
	return MetricId.c_str();
}

inline const char *MatchWeaponDisplayName(int Weapon)
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
	return Localize("Unknown");
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
		if(Separator == std::string_view::npos)
			return false;
		if(Separator != 1 || WeaponMetric[0] < '0' || WeaponMetric[0] > '9')
			return false;
		const int Weapon = WeaponMetric[0] - '0';
		if(Weapon >= NUM_WEAPONS)
			return false;
		pStats = &Stats.m_aWeapons[Weapon];
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
