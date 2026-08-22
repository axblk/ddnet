#ifndef GAME_CLIENT_MATCH_DETAIL_VIEW_H
#define GAME_CLIENT_MATCH_DETAIL_VIEW_H

#include "match_journal.h"
#include "match_report_view.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

/**
 * The pages the match report can be looked at through.
 *
 * A report only carries what its mode measured, so not every page can be
 * filled: see @link CMatchDetailView::HasTab @endlink.
 */
enum class EStatsMatchTab
{
	SCORE,
	FRAGS,
	DEATHS,
	ACCURACY,
	DAMAGE,
	SHOTS,
	OBJECTIVES,
	RUN,
	COUNT,
};

const char *StatsMatchTabDisplayName(EStatsMatchTab Tab);

/**
 * Where the number of one cell comes from.
 *
 * Everything that is not measured per weapon is a plain metric of the report
 * and is addressed by its position in @link CMatchDetailView::m_vMetricIds
 * @endlink, so a mode can report something this build has never heard of and
 * still get a column.
 */
enum class EMatchDetailValue
{
	METRIC,
	KILLS,
	DEATHS,
	DEATHS_HOLDING,
	SHOTS,
	HITS,
	DAMAGE_DONE,
	DAMAGE_TAKEN,
	ACCURACY,
};

enum class EMatchDetailFormat
{
	COUNT,
	DURATION,
	RANK,
	ACCURACY,
};

class CMatchDetailColumn
{
public:
	std::string m_Label;
	EMatchDetailValue m_Value = EMatchDetailValue::METRIC;
	EMatchDetailFormat m_Format = EMatchDetailFormat::COUNT;
	// Position in CMatchDetailView::m_vMetricIds for EMatchDetailValue::METRIC
	int m_Metric = -1;
	// Weapon the column reports on, -1 for the total over all weapons
	int m_Weapon = -1;
	// A rank and a run time are better the smaller they are
	bool m_LowerIsBetter = false;
	// Value of the best cell of this column, nothing when no row has one
	std::optional<int64_t> m_Best;
};

/**
 * One participant of the match, or the summary of a team.
 *
 * The named members are the metrics the pages need often enough that looking
 * them up by name for every drawn frame would be wasteful; everything else,
 * including what an unknown mode reports, stays in @link m_vMetrics @endlink.
 */
class CMatchDetailRow
{
public:
	const CMatchParticipant *m_pParticipant = nullptr;
	CMatchCombatStats m_Combat;
	// Indexed by CMatchDetailView::m_vMetricIds, empty where the report does
	// not have that metric for this participant
	std::vector<std::optional<int64_t>> m_vMetrics;
	int64_t m_Score = 0;
	int64_t m_Suicides = 0;
	int64_t m_Assists = 0;
	int64_t m_PlaytimeTicks = 0;
	int64_t m_FlagGrabs = 0;
	int64_t m_FlagReturns = 0;
	int64_t m_FlagCaptures = 0;
	int64_t m_Catches = 0;
	int64_t m_HealthPickedUp = 0;
	int64_t m_ArmorPickedUp = 0;
	int m_Rank = 0;
	std::optional<EMatchOutcome> m_Outcome;
	bool m_Local = false;
	bool m_Left = false;
};

/**
 * One team of the match, or every participant at once when the mode has no
 * teams.
 *
 * Participants that left are kept apart from the ones that were still there at
 * the end, because a table that mixes them makes the standings unreadable.
 */
class CMatchDetailBlock
{
public:
	std::optional<int> m_TeamId;
	std::string m_DisplayName;
	std::optional<EMatchOutcome> m_Outcome;
	CMatchDetailRow m_Summary;
	// Positions in CMatchDetailView::m_vRows, best rank first
	std::vector<int> m_vPlaying;
	std::vector<int> m_vLeft;
};

/**
 * Everything the match report page draws, prepared once when the report is
 * opened instead of while it is on screen.
 */
class CMatchDetailView
{
public:
	std::vector<CMatchDetailRow> m_vRows;
	std::vector<CMatchDetailBlock> m_vBlocks;
	// Metric ids that are not measured per weapon, ascending
	std::vector<std::string> m_vMetricIds;
	// Weapons at least one participant used, ascending
	std::vector<int> m_vWeapons;
	// Columns of m_Tab, rebuilt by UpdateMatchDetailBest
	std::vector<CMatchDetailColumn> m_vColumns;
	EStatsMatchTab m_Tab = EStatsMatchTab::SCORE;
	uint32_t m_TabMask = 0;
	int m_TickRate = 0;
	int m_ModeSchemaVersion = 0;
	bool m_HasTeams = false;

	bool HasTab(EStatsMatchTab Tab) const { return (m_TabMask & (1u << static_cast<int>(Tab))) != 0; }
};

/**
 * Turns a stored report into what the page draws.
 *
 * The rows point into the report, so the view is only usable for as long as the
 * stored match it was built from stays alive and unchanged.
 *
 * @param Stored Report to read, together with who of it is the local player.
 * @param View Overwritten with the result, its tab is reset to the first one
 * the report can fill.
 */
void BuildMatchDetailView(const CStoredMatch &Stored, CMatchDetailView &View);

/**
 * Lays out the columns of one tab and finds the best cell of each.
 *
 * Only the selected tab has columns, so this runs when the selection changes
 * rather than while the table is drawn.
 *
 * @param View View to update, must have been built.
 * @param Tab Tab to lay out. A tab the report cannot fill lays out no columns.
 */
void UpdateMatchDetailBest(CMatchDetailView &View, EStatsMatchTab Tab);

/**
 * @return The number of one cell, or nothing when the row has no such value.
 */
std::optional<int64_t> MatchDetailCell(const CMatchDetailRow &Row, const CMatchDetailColumn &Column);

void FormatMatchDetailCell(const CMatchDetailRow &Row, const CMatchDetailColumn &Column, int TickRate, char *pBuffer, int BufferSize);

#endif // GAME_CLIENT_MATCH_DETAIL_VIEW_H
