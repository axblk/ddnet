#ifndef GAME_CLIENT_COMPONENTS_STATBOARD_H
#define GAME_CLIENT_COMPONENTS_STATBOARD_H

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/match_journal.h>
#include <game/client/match_report_view.h>

#include <cstdint>
#include <optional>
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
	};

	class CColumn
	{
	public:
		EColumnKind m_Kind = EColumnKind::METRIC;
		std::string m_Label;
		std::string_view m_Suffix;
		// Below zero for the column that totals every weapon.
		int m_Weapon = -1;
		bool m_HighlightMax = true;
		float m_Width = 118.0f;
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

	static void ConKeyStats(IConsole::IResult *pResult, void *pUserData);

	bool BuildObservedMatch(const CRenderContext &Context, CStoredMatch &Match) const;
	static std::vector<CRow> BuildRows(const CStoredMatch &Stored, int &Hidden);
	static void MergeMetric(CRow &Row, std::string_view MetricId, int64_t Value, EMatchMetricAggregation Aggregation);
	static std::vector<CColumn> BuildColumns(const CMatchReport &Report, const std::vector<CRow> &vRows, EAspect Aspect);
	static CCell BuildCell(const CMatchReport &Report, const CRow &Row, const CColumn &Column);

	void RenderBoard(const CRenderContext &Context, const CStoredMatch &Stored);
	float RenderTable(const CRenderContext &Context, const CStoredMatch &Stored, const std::vector<CRow> &vRows, const std::vector<CColumn> &vColumns, int Hidden, float X, float Y, float Width);

	void AutoStatScreenshot();
	void AutoStatCSV();

	std::string ReplaceCommata(const char *pStr);
	void FormatStats(char *pDest, size_t DestSize);

public:
	CStatboard();
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnConsoleInit() override;
	void UpdateController();
	void OnRender(const CRenderContext &Context) override;
	void OnRelease() override;
	bool IsActive() const;
	bool IsRenderable() const;
	bool IsRenderable(const CRenderContext &Context) const;
};

#endif // GAME_CLIENT_COMPONENTS_STATBOARD_H
