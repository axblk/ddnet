#ifndef GAME_CLIENT_COMPONENTS_STATBOARD_H
#define GAME_CLIENT_COMPONENTS_STATBOARD_H

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/match_journal.h>
#include <game/client/match_report_view.h>
#include <game/client/ui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class CStatboard : public CComponent
{
	/**
	 * Which numbers the table body shows.
	 *
	 * Only one of them is on screen at a time, because a line of every counter
	 * at once is what made the old board unreadable. The statboard key cycles
	 * through the aspects the running match has data for.
	 */
	enum class EAspect
	{
		SCORE,
		WEAPON_KILLS,
		WEAPON_DEATHS,
		WEAPON_ACCURACY,
	};

	enum class EColumnKind
	{
		METRIC,
		WEAPON_KILLS,
		WEAPON_DEATHS,
		ACCURACY,
	};

	// One metric of one row. The id is a view into the report being drawn,
	// which outlives the frame, so no cell has to own a copy of it.
	class CRowMetric
	{
	public:
		std::string_view m_MetricId;
		int64_t m_Value = 0;
		EMatchMetricAggregation m_Aggregation = EMatchMetricAggregation::SUM;
	};

	class CRow
	{
	public:
		// Null for the one row that folds away everybody who left.
		const CMatchParticipant *m_pParticipant = nullptr;
		int m_TeamIndex = -1;
		int m_Rank = 0;
		bool m_Local = false;
		int m_Members = 1;
		std::vector<CRowMetric> m_vMetrics;
		CMatchCombatStats m_Combat;

		const CRowMetric *Metric(std::string_view Suffix) const;
		// Empties the row without giving up what its two vectors already
		// allocated, so that refilling it costs nothing.
		void Reset();
	};

	class CColumn
	{
	public:
		EColumnKind m_Kind = EColumnKind::METRIC;
		char m_aLabel[64] = "";
		std::string_view m_Suffix;
		// Below zero for the column that totals every weapon.
		int m_Weapon = -1;
		bool m_HighlightMax = true;
		float m_Width = 118.0f;
		// Best value any row has here, so that a row does not have to know
		// about the others to emphasise it.
		std::optional<double> m_Best;
	};

	class CCell
	{
	public:
		char m_aText[32] = "-";
		// Set only when the value may be compared against the rest of the
		// column, so that a rank or a duration is never called a high score.
		std::optional<double> m_Comparable;
	};

	bool m_Active;
	bool m_ScreenshotTaken;
	int64_t m_ScreenshotTime;
	int m_Aspect;
	int64_t m_CloseTime;

	// Bound on the player rows, so that a full server cannot push the board
	// off the screen.
	static constexpr int MAX_ROWS = 32;
	// Widest a table may get, so that a mode reporting a hundred metrics still
	// leaves room for the game it is drawn over.
	static constexpr size_t MAX_COLUMNS = 10;

	/**
	 * Everything the board reads while it draws.
	 *
	 * The board is redrawn as often as the game behind it, so none of this is
	 * built on the stack: the vectors are refilled without being emptied and
	 * the counts say how much of them is in use, which is what keeps a frame
	 * from allocating a row, a column and a string for every player again.
	 */
	CStoredMatch m_Observed;
	std::vector<CRow> m_vRows;
	size_t m_NumRows = 0;
	std::vector<CColumn> m_vColumns;
	size_t m_NumColumns = 0;
	std::vector<const CMatchMetric *> m_vpEntries;
	std::vector<int> m_vRowIds;
	CRow m_Totals;
	// The helpers that name and format a metric want the metric of a report,
	// and a row only carries a view of its id. This is where such a view is
	// copied to, because assigning into a string that is kept costs nothing
	// while building one for every cell costs an allocation each.
	CMatchMetric m_ScratchMetric;
	int m_Hidden = 0;

	std::span<const CRow> Rows() const { return {m_vRows.data(), m_NumRows}; }
	std::span<const CColumn> Columns() const { return {m_vColumns.data(), m_NumColumns}; }

	/**
	 * The drawn geometry of one string of the table.
	 *
	 * Drawing a string builds a text container and throws it away again, which
	 * is two allocations, and a right aligned one measures the string first,
	 * which is two more. The table draws a few hundred of them per frame while
	 * almost none of them change, so each keeps the geometry it was last given
	 * and only rebuilds it when its text does change.
	 */
	class CRowText
	{
	public:
		CCachedText m_Rank;
		CCachedText m_Name;
		CCachedText m_Clan;
		std::array<CCachedText, MAX_COLUMNS> m_aCells;
	};
	std::array<CCachedText, MAX_COLUMNS> m_aColumnTexts;
	std::array<CRowText, MAX_ROWS> m_aRowTexts;
	CCachedText m_NameHeader;
	CCachedText m_Title;
	CCachedText m_Source;
	CCachedText m_Subject;
	// The panel and the table are alternatives, so a board is only ever one of
	// them and the name and the value of a panel line reuse the strings of a
	// table row.
	// One per aspect the board can offer, see EAspect
	std::array<CCachedText, 4> m_aAspectTexts;
	void ResetTexts();

	static void ConKeyStats(IConsole::IResult *pResult, void *pUserData);

	bool BuildObservedMatch(const CRenderContext &Context, CStoredMatch &Match) const;
	void BuildRows(const CStoredMatch &Stored);
	static void MergeMetric(CRow &Row, std::string_view MetricId, int64_t Value, EMatchMetricAggregation Aggregation);
	void BuildColumns(const CMatchReport &Report, EAspect Aspect);
	CCell BuildCell(const CMatchReport &Report, const CRow &Row, const CColumn &Column);

	void RenderBoard(const CRenderContext &Context, const CStoredMatch &Stored);
	float RenderTable(const CRenderContext &Context, const CStoredMatch &Stored, float X, float Y, float Width);

	void AutoStatScreenshot();
	void AutoStatCSV();

	std::string ReplaceCommata(const char *pStr);
	void FormatStats(char *pDest, size_t DestSize);

public:
	CStatboard();
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnShutdown() override;
	void OnWindowResize() override;
	void OnConsoleInit() override;
	void UpdateController();
	void OnRender(const CRenderContext &Context) override;
	void OnRelease() override;
	bool IsActive() const;
	bool IsRenderable() const;
	bool IsRenderable(const CRenderContext &Context) const;
};

#endif // GAME_CLIENT_COMPONENTS_STATBOARD_H
