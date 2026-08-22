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
	int64_t m_Kills = 0;
	int64_t m_Deaths = 0;
	int64_t m_Shots = 0;
	int64_t m_Hits = 0;
	int64_t m_DamageDone = 0;
	int64_t m_DamageTaken = 0;

	bool HasData() const { return m_Kills != 0 || m_Deaths != 0 || m_Shots != 0 || m_Hits != 0 || m_DamageDone != 0 || m_DamageTaken != 0; }
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

	// Keeps the storage of the weapon rows, so that refilling this from a
	// report does not allocate again.
	void Reset()
	{
		m_Total = {};
		m_vWeapons.clear();
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

inline void MatchMetricDisplayName(const std::string &MetricId, int ModeSchemaVersion, char *pBuffer, int BufferSize)
{
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	if(IsKnownMatchMetric(MetricId, ModeSchemaVersion))
	{
		const char *pName = nullptr;
		if(Suffix == "score")
			pName = Localize("Score");
		else if(Suffix == "playtime_ticks")
			pName = Localize("Play time");
		else if(Suffix == "sudden_death")
			pName = Localize("Sudden death");
		else if(Suffix == "kills")
			pName = Localize("Kills");
		else if(Suffix == "deaths")
			pName = Localize("Deaths");
		else if(Suffix == "suicides")
			pName = Localize("Suicides");
		else if(Suffix == "best_spree")
			pName = Localize("Best spree");
		else if(Suffix == "shots")
			pName = Localize("Shots");
		else if(Suffix == "hits")
			pName = Localize("Hits");
		else if(Suffix == "damage_done")
			pName = Localize("Damage done");
		else if(Suffix == "damage_taken")
			pName = Localize("Damage taken");
		else if(Suffix == "flag_grabs")
			pName = Localize("Flag grabs");
		else if(Suffix == "flag_returns")
			pName = Localize("Flag returns");
		else if(Suffix == "flag_captures")
			pName = Localize("Flag captures");
		else if(Suffix == "catches")
			pName = Localize("Catches");
		else if(Suffix == "personal_best_ticks")
			pName = Localize("Personal best");
		else if(Suffix == "map_best_ticks")
			pName = Localize("Map best");
		else if(Suffix == "map_rank")
			pName = Localize("Map rank");
		else if(Suffix == "map_finishes")
			pName = Localize("Stored map finishes");
		else if(Suffix == "session_finishes")
			pName = Localize("Session finishes");
		else if(Suffix == "last_finish_ticks")
			pName = Localize("Last finish");
		else if(Suffix == "current_run_ticks")
			pName = Localize("Current run");
		else if(Suffix == "current_checkpoint")
			pName = Localize("Current checkpoint");
		if(pName != nullptr)
		{
			str_copy(pBuffer, pName, BufferSize);
			return;
		}
	}
	// A metric this build does not know still belongs on the page. Its suffix is
	// the only readable part of the id, so it is shown as a sentence rather than
	// as `somemod@example.org/flag_time`.
	str_truncate(pBuffer, BufferSize, Suffix.data(), static_cast<int>(Suffix.size()));
	for(char *pChar = pBuffer; *pChar != '\0'; ++pChar)
		if(*pChar == '_')
			*pChar = ' ';
	pBuffer[0] = str_uppercase(pBuffer[0]);
}

inline void MatchWeaponDisplayName(int Weapon, char *pBuffer, int BufferSize)
{
	switch(Weapon)
	{
	case WEAPON_HAMMER: str_copy(pBuffer, Localize("Hammer"), BufferSize); return;
	case WEAPON_GUN: str_copy(pBuffer, Localize("Gun"), BufferSize); return;
	case WEAPON_SHOTGUN: str_copy(pBuffer, Localize("Shotgun"), BufferSize); return;
	case WEAPON_GRENADE: str_copy(pBuffer, Localize("Grenade"), BufferSize); return;
	case WEAPON_LASER: str_copy(pBuffer, Localize("Laser"), BufferSize); return;
	case WEAPON_NINJA: str_copy(pBuffer, Localize("Ninja"), BufferSize); return;
	}
	// A mod may report weapons this build has no name or sprite for
	str_format(pBuffer, BufferSize, "%s %d", Localize("Weapon"), Weapon);
}

inline void AddMatchCombatValue(int64_t &Target, int64_t Value)
{
	Value = std::max<int64_t>(0, Value);
	Target = Value > std::numeric_limits<int64_t>::max() - Target ? std::numeric_limits<int64_t>::max() : Target + Value;
}

/**
 * Splits a combat metric id into the weapon it counts for and the counter it
 * names.
 *
 * @param MetricId Metric id from the report.
 * @param ModeSchemaVersion Schema version the report was written with.
 * @param Weapon Set to the weapon index, or to -1 for a counter that is not
 * broken down by weapon.
 * @param StatName Set to the name of the counter, for example "hits".
 *
 * @return false when the id does not name a combat counter.
 */
inline bool MatchCombatMetricParts(const std::string &MetricId, int ModeSchemaVersion, int &Weapon, std::string_view &StatName)
{
	if(!IsKnownMatchMetric(MetricId, ModeSchemaVersion))
		return false;
	const std::string_view Suffix = MatchMetricSuffix(MetricId);
	Weapon = -1;
	StatName = Suffix;
	if(Suffix.starts_with("weapon_"))
	{
		const std::string_view WeaponMetric = Suffix.substr(7);
		const size_t Separator = WeaponMetric.find('_');
		if(Separator == std::string_view::npos || Separator == 0 || Separator > 2)
			return false;
		Weapon = 0;
		for(size_t Digit = 0; Digit < Separator; ++Digit)
		{
			if(WeaponMetric[Digit] < '0' || WeaponMetric[Digit] > '9')
				return false;
			Weapon = Weapon * 10 + (WeaponMetric[Digit] - '0');
		}
		StatName = WeaponMetric.substr(Separator + 1);
	}
	return StatName == "kills" || StatName == "deaths" || StatName == "shots" || StatName == "hits" || StatName == "damage_done" || StatName == "damage_taken";
}

inline void AddMatchCombatCounter(CMatchWeaponStats &Stats, std::string_view StatName, int64_t Value)
{
	if(StatName == "kills")
		AddMatchCombatValue(Stats.m_Kills, Value);
	else if(StatName == "deaths")
		AddMatchCombatValue(Stats.m_Deaths, Value);
	else if(StatName == "shots")
		AddMatchCombatValue(Stats.m_Shots, Value);
	else if(StatName == "hits")
		AddMatchCombatValue(Stats.m_Hits, Value);
	else if(StatName == "damage_done")
		AddMatchCombatValue(Stats.m_DamageDone, Value);
	else if(StatName == "damage_taken")
		AddMatchCombatValue(Stats.m_DamageTaken, Value);
}

inline bool AddMatchCombatMetric(CMatchCombatStats &Stats, const std::string &MetricId, int ModeSchemaVersion, int64_t Value)
{
	int Weapon;
	std::string_view StatName;
	if(!MatchCombatMetricParts(MetricId, ModeSchemaVersion, Weapon, StatName))
		return false;
	CMatchWeaponStats *pStats = Weapon < 0 ? &Stats.m_Total : Stats.Weapon(Weapon);
	if(pStats == nullptr)
		return false;
	AddMatchCombatCounter(*pStats, StatName, Value);
	return true;
}

inline bool IsMatchCombatStatMetric(const std::string &MetricId, int ModeSchemaVersion)
{
	CMatchCombatStats Stats;
	return AddMatchCombatMetric(Stats, MetricId, ModeSchemaVersion, 0);
}

// Fills a caller owned object rather than returning one, because the pages
// that show these are redrawn every frame and the weapon rows would be
// allocated again on each of them.
inline void BuildMatchCombatStats(const CMatchReport &Report, int ParticipantId, CMatchCombatStats &Stats)
{
	Stats.Reset();
	for(const CMatchMetric &Metric : Report.m_vMetrics)
		if(Metric.m_SubjectKind == EMatchSubjectKind::PARTICIPANT && Metric.m_SubjectId == ParticipantId)
			AddMatchCombatMetric(Stats, Metric.m_MetricId, Report.m_ModeSchemaVersion, Metric.m_Value);
}

inline void BuildMatchCombatStats(const CMatchProfile &Profile, CMatchCombatStats &Stats)
{
	Stats.Reset();
	for(const CMatchMetricAggregate &Metric : Profile.m_vMetrics)
		AddMatchCombatMetric(Stats, Metric.m_MetricId, Metric.m_ModeSchemaVersion, Metric.m_Value);
}

class CMatchReportRow
{
public:
	const CMatchParticipant *m_pParticipant = nullptr;
	const CMatchStanding *m_pStanding = nullptr;
	std::optional<int64_t> m_Score;
	std::optional<int64_t> m_Kills;
	std::optional<int64_t> m_Deaths;
	// Summed over the counters that are not broken down by weapon
	CMatchWeaponStats m_Combat;
	// Whether the participant has any combat counter above zero, the per weapon
	// ones included
	bool m_HasCombat = false;

	int Rank() const { return m_pStanding == nullptr ? std::numeric_limits<int>::max() : m_pStanding->m_Rank; }
};

/**
 * Participants of a report in the order a board shows them, together with the
 * values that every row draws.
 *
 * Looking one value up means scanning all metrics of the report, of which a
 * busy server has thousands, so doing that per row and again for every
 * comparison of a sort is what made drawing a report cost more than the rest
 * of the frame. Here a single pass fills every row instead, and the storage
 * lives in the object, so a board that keeps one of these renders without
 * allocating.
 */
class CMatchReportRanking
{
	std::vector<CMatchReportRow> m_vRows;

public:
	void Update(const CMatchReport &Report)
	{
		m_vRows.clear();
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
		{
			CMatchReportRow Row;
			Row.m_pParticipant = &Participant;
			m_vRows.push_back(Row);
		}
		for(const CMatchStanding &Standing : Report.m_vStandings)
		{
			if(Standing.m_SubjectKind != EMatchSubjectKind::PARTICIPANT)
				continue;
			if(CMatchReportRow *pRow = Row(Standing.m_SubjectId))
				pRow->m_pStanding = &Standing;
		}
		for(const CMatchMetric &Metric : Report.m_vMetrics)
		{
			if(Metric.m_SubjectKind != EMatchSubjectKind::PARTICIPANT || !Metric.m_SubjectId.has_value())
				continue;
			CMatchReportRow *pRow = Row(*Metric.m_SubjectId);
			if(pRow == nullptr)
				continue;
			const std::string_view Suffix = MatchMetricSuffix(Metric.m_MetricId);
			if(Suffix == "score")
				pRow->m_Score = Metric.m_Value;
			else if(Suffix == "kills")
				pRow->m_Kills = Metric.m_Value;
			else if(Suffix == "deaths")
				pRow->m_Deaths = Metric.m_Value;
			int Weapon;
			std::string_view StatName;
			if(!MatchCombatMetricParts(Metric.m_MetricId, Report.m_ModeSchemaVersion, Weapon, StatName))
				continue;
			pRow->m_HasCombat = pRow->m_HasCombat || Metric.m_Value > 0;
			if(Weapon < 0)
				AddMatchCombatCounter(pRow->m_Combat, StatName, Metric.m_Value);
		}
		// A plain sort, because the tie breaker below already makes the order
		// total and a stable sort would want a buffer of its own every frame.
		std::sort(m_vRows.begin(), m_vRows.end(), [](const CMatchReportRow &Left, const CMatchReportRow &Right) {
			if(Left.Rank() != Right.Rank())
				return Left.Rank() < Right.Rank();
			if(Left.m_Score.value_or(0) != Right.m_Score.value_or(0))
				return Left.m_Score.value_or(0) > Right.m_Score.value_or(0);
			return Left.m_pParticipant->m_ParticipantId < Right.m_pParticipant->m_ParticipantId;
		});
	}

	const std::vector<CMatchReportRow> &Rows() const { return m_vRows; }

	CMatchReportRow *Row(int ParticipantId)
	{
		// Ids are handed out from zero in the order participants were added, so
		// before the sort a row almost always sits at its own index.
		if(ParticipantId >= 0 && ParticipantId < static_cast<int>(m_vRows.size()) && m_vRows[ParticipantId].m_pParticipant->m_ParticipantId == ParticipantId)
			return &m_vRows[ParticipantId];
		for(CMatchReportRow &Candidate : m_vRows)
			if(Candidate.m_pParticipant->m_ParticipantId == ParticipantId)
				return &Candidate;
		return nullptr;
	}
	const CMatchReportRow *Row(int ParticipantId) const { return const_cast<CMatchReportRanking *>(this)->Row(ParticipantId); }
};

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
