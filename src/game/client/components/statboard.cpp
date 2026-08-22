#include <base/io.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/client_data.h>

#include <game/client/animstate.h>
#include <game/client/components/statboard.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

#include <algorithm>
#include <cinttypes>
#include <iterator>
#include <limits>
#include <vector>

namespace
{

	// `MatchMetricSuffix` needs an owning string, which a board that keeps metric
	// ids as views into the report it draws does not have.
	std::string_view MetricIdSuffix(std::string_view MetricId)
	{
		const size_t Slash = MetricId.rfind('/');
		return Slash == std::string_view::npos ? MetricId : MetricId.substr(Slash + 1);
	}

	ColorRGBA TeamColor(size_t TeamIndex)
	{
		if(TeamIndex == 0)
			return ColorRGBA(0.975f, 0.17f, 0.17f, 1.0f);
		return TeamIndex == 1 ? ColorRGBA(0.17f, 0.46f, 0.975f, 1.0f) : ColorRGBA(0.8f, 0.8f, 0.8f, 1.0f);
	}

	void AddCombatStats(CMatchCombatStats &Target, const CMatchCombatStats &Source)
	{
		const auto Add = [](CMatchWeaponStats &Left, const CMatchWeaponStats &Right) {
			AddMatchCombatValue(Left.m_Kills, Right.m_Kills);
			AddMatchCombatValue(Left.m_Deaths, Right.m_Deaths);
			AddMatchCombatValue(Left.m_Shots, Right.m_Shots);
			AddMatchCombatValue(Left.m_Hits, Right.m_Hits);
			AddMatchCombatValue(Left.m_DamageDone, Right.m_DamageDone);
			AddMatchCombatValue(Left.m_DamageTaken, Right.m_DamageTaken);
		};
		Add(Target.m_Total, Source.m_Total);
		for(const CMatchWeaponStats &Weapon : Source.m_vWeapons)
			if(CMatchWeaponStats *pTarget = Target.Weapon(Weapon.m_Weapon))
				Add(*pTarget, Weapon);
	}

	void RightText(ITextRender *pTextRender, float Right, float Y, float FontSize, const char *pText)
	{
		pTextRender->Text(Right - pTextRender->TextWidth(FontSize, pText, -1, -1.0f), Y, FontSize, pText, -1.0f);
	}

	// A full server has to fit above the game, so the rows get tighter instead
	// of running off the bottom of the screen.
	float RowHeight(size_t Rows)
	{
		return Rows > 20 ? 22.0f : 30.0f;
	}

} // namespace

const CStatboard::CRowMetric *CStatboard::CRow::Metric(std::string_view Suffix) const
{
	for(const CRowMetric &Metric : m_vMetrics)
		if(MetricIdSuffix(Metric.m_MetricId) == Suffix)
			return &Metric;
	return nullptr;
}

CStatboard::CStatboard()
{
	m_Active = false;
	m_ScreenshotTaken = false;
	m_ScreenshotTime = -1;
	m_Aspect = 0;
	m_CloseTime = 0;
}

void CStatboard::OnReset()
{
	m_Active = false;
	m_ScreenshotTaken = false;
	m_ScreenshotTime = -1;
	m_Aspect = 0;
	m_CloseTime = 0;
}

void CStatboard::OnRelease()
{
	m_Active = false;
}

void CStatboard::ConKeyStats(IConsole::IResult *pResult, void *pUserData)
{
	CStatboard *pSelf = static_cast<CStatboard *>(pUserData);
	const bool Active = pResult->GetInteger(0) != 0;
	// The statboard key is the only way to change the aspect: pressing it again
	// right after letting go means "the next one", pressing it after a pause
	// means a fresh look and starts over.
	if(Active && !pSelf->m_Active)
		pSelf->m_Aspect = time_get() - pSelf->m_CloseTime < time_freq() ? pSelf->m_Aspect + 1 : 0;
	else if(!Active && pSelf->m_Active)
		pSelf->m_CloseTime = time_get();
	pSelf->m_Active = Active;
}

void CStatboard::OnConsoleInit()
{
	Console()->Register("+statboard", "", CFGFLAG_CLIENT, ConKeyStats, this, "Show stats, press again to cycle through the aspects");
}

bool CStatboard::IsActive() const
{
	return m_Active;
}

bool CStatboard::IsRenderable() const
{
	return IsActive();
}

bool CStatboard::IsRenderable(const CRenderContext &Context) const
{
	return IsActive() && Context.m_Time.m_IsGameActive;
}

void CStatboard::UpdateController()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if((g_Config.m_ClAutoStatboardScreenshot || g_Config.m_ClAutoCSV) && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(m_ScreenshotTime < 0 && GameClient()->Snap().m_pGameInfoObj && GameClient()->Snap().m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER)
			m_ScreenshotTime = time_get() + time_freq() * 3;
		if(m_ScreenshotTime > -1 && m_ScreenshotTime < time_get())
			m_Active = true;
		if(!m_ScreenshotTaken && m_ScreenshotTime > -1 && m_ScreenshotTime + time_freq() / 5 < time_get())
		{
			if(g_Config.m_ClAutoStatboardScreenshot)
				AutoStatScreenshot();
			if(g_Config.m_ClAutoCSV)
				AutoStatCSV();
			m_ScreenshotTaken = true;
		}
	}
}

void CStatboard::OnRender(const CRenderContext &Context)
{
	if(!IsRenderable(Context))
		return;
	// Either or, not both: while a server reports the running match, its
	// numbers are the authoritative version of everything the client estimates
	// on its own, and they are the only ones a mode can extend.
	if(const CStoredMatch *pLive = GameClient()->LiveStats(Context.m_Session.Id()))
	{
		RenderBoard(Context, *pLive);
		return;
	}
	CStoredMatch Observed;
	if(BuildObservedMatch(Context, Observed))
		RenderBoard(Context, Observed);
}

bool CStatboard::BuildObservedMatch(const CRenderContext &Context, CStoredMatch &Match) const
{
	const CGameState &State = Context.m_State;
	const CSessionStatsState &Stats = Context.m_Session.Stats();
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> *pClientsByScore = Presentation.ClientsByScore(State.Id());
	if(pClientsByScore == nullptr)
		return false;

	// What this client counted, phrased as a report, so that the board only
	// ever reads one thing. Participant ids are client ids here, which is what
	// lets a row carry the tee of the player it belongs to.
	CMatchReport &Report = Match.m_Report;
	const CServerInfo &ServerInfo = Client()->ServerInfo(Context.m_Session.Id());
	Report.m_ModeId = ServerInfo.m_aGameType;
	Report.m_MapName = ServerInfo.m_aMap;
	Report.m_TickRate = Context.m_Time.m_GameTickSpeed;
	Report.m_DurationTicks = std::max(0, Context.m_Time.m_GameTick - (State.HasGameInfo() ? State.GameInfo().m_RoundStartTick : 0));

	const bool TeamPlay = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	if(TeamPlay)
	{
		Report.m_vTeams.push_back({TEAM_RED, Localize("Red team")});
		Report.m_vTeams.push_back({TEAM_BLUE, Localize("Blue team")});
		if(const CNetObj_GameData *pGameData = State.GameData())
		{
			Report.m_vMetrics.push_back({EMatchSubjectKind::TEAM, TEAM_RED, "observed/score", pGameData->m_TeamscoreRed, EMatchMetricAggregation::MATCH_ONLY});
			Report.m_vMetrics.push_back({EMatchSubjectKind::TEAM, TEAM_BLUE, "observed/score", pGameData->m_TeamscoreBlue, EMatchMetricAggregation::MATCH_ONLY});
		}
	}
	const int Spectated = Context.m_View.IsSpectating() ? Context.m_View.SpectatorId() : State.LocalClientId();
	if(Spectated >= 0 && Spectated < MAX_CLIENTS)
		Match.m_LocalParticipantId = Spectated;

	const auto AddMetric = [&](int ClientId, const std::string &Suffix, int64_t Value, EMatchMetricAggregation Aggregation) {
		if(Value != 0)
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, "observed/" + Suffix, Value, Aggregation});
	};
	int Rank = 0;
	for(const int ClientId : *pClientsByScore)
	{
		if(ClientId < 0)
			break;
		const CSessionClientStats &ClientStats = Stats.Client(ClientId);
		if(!ClientStats.HasJoined())
			continue;
		const CClientPresentation *pClient = Presentation.Client(State.Id(), ClientId);
		const bool Playing = pClient != nullptr && ClientStats.IsActive() && (pClient->m_Team == TEAM_RED || (TeamPlay && pClient->m_Team == TEAM_BLUE));

		CMatchParticipant Participant;
		Participant.m_ParticipantId = ClientId;
		if(pClient != nullptr)
		{
			Participant.m_DisplayName = pClient->m_aName;
			Participant.m_Clan = pClient->m_aClan;
		}
		if(Playing)
		{
			if(TeamPlay)
				Participant.m_TeamId = pClient->m_Team;
			Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, ++Rank, EMatchOutcome::FINISHED});
			if(State.Client(ClientId).m_HasPlayerInfo)
				AddMetric(ClientId, "score", State.Client(ClientId).m_PlayerInfo.m_Score, EMatchMetricAggregation::MATCH_ONLY);
		}
		else
		{
			Participant.m_LeftTick = Context.m_Time.m_GameTick;
		}
		Report.m_vParticipants.push_back(std::move(Participant));

		// Kills and deaths anchor the table and are reported even at zero; the
		// rest only earns a column once it happened.
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, "observed/kills", ClientStats.m_Frags, EMatchMetricAggregation::SUM});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, "observed/deaths", ClientStats.m_Deaths, EMatchMetricAggregation::SUM});
		AddMetric(ClientId, "best_spree", ClientStats.m_BestSpree, EMatchMetricAggregation::MAXIMUM);
		AddMetric(ClientId, "flag_grabs", ClientStats.m_FlagGrabs, EMatchMetricAggregation::SUM);
		AddMetric(ClientId, "flag_captures", ClientStats.m_FlagCaptures, EMatchMetricAggregation::SUM);
		for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
		{
			const std::string Prefix = "weapon_" + std::to_string(Weapon) + "_";
			AddMetric(ClientId, Prefix + "kills", ClientStats.m_aFragsWith[Weapon], EMatchMetricAggregation::SUM);
			AddMetric(ClientId, Prefix + "deaths", ClientStats.m_aDeathsFrom[Weapon], EMatchMetricAggregation::SUM);
		}
	}
	return Rank > 0 && Rank <= MAX_ROWS;
}

std::vector<CStatboard::CRow> CStatboard::BuildRows(const CStoredMatch &Stored, int &Hidden)
{
	const CMatchReport &Report = Stored.m_Report;
	std::vector<CRow> vRows;
	// Parallel to the rows, so that the single pass over the metrics below can
	// find the row a metric belongs to without searching the report again. The
	// folded row belongs to nobody and takes an id no subject can have.
	std::vector<int> vIds;
	int Folded = -1;
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
	{
		const bool Local = Stored.m_LocalParticipantId.has_value() && *Stored.m_LocalParticipantId == Participant.m_ParticipantId;
		// Everybody who is gone shares one row at the end of the table. A list
		// of absentees is not worth the space, and they belong to no team any
		// more, so the summary band stays about who is playing now.
		if(Participant.m_LeftTick.has_value() && !Local)
		{
			if(Folded < 0)
			{
				Folded = vRows.size();
				vRows.emplace_back().m_Members = 0;
				vIds.push_back(std::numeric_limits<int>::min());
			}
			vRows[Folded].m_Members++;
			continue;
		}

		CRow Row;
		Row.m_pParticipant = &Participant;
		Row.m_Local = Local;
		for(size_t Team = 0; Team < Report.m_vTeams.size(); ++Team)
			if(Participant.m_TeamId.has_value() && Report.m_vTeams[Team].m_TeamId == *Participant.m_TeamId)
				Row.m_TeamIndex = Team;
		if(const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId))
			Row.m_Rank = pStanding->m_Rank;
		vRows.push_back(std::move(Row));
		vIds.push_back(Participant.m_ParticipantId);
	}

	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		if(Metric.m_SubjectKind != EMatchSubjectKind::PARTICIPANT || !Metric.m_SubjectId.has_value())
			continue;
		// A metric of somebody who left survives only in the folded row.
		int Index = Folded;
		for(size_t Row = 0; Row < vIds.size(); ++Row)
			if(vIds[Row] == *Metric.m_SubjectId)
				Index = Row;
		if(Index < 0)
			continue;
		MergeMetric(vRows[Index], Metric.m_MetricId, Metric.m_Value, Metric.m_Aggregation);
		AddMatchCombatMetric(vRows[Index].m_Combat, Metric.m_MetricId, Report.m_ModeSchemaVersion, Metric.m_Value);
	}

	std::stable_sort(vRows.begin(), vRows.end(), [](const CRow &Left, const CRow &Right) {
		if((Left.m_pParticipant == nullptr) != (Right.m_pParticipant == nullptr))
			return Right.m_pParticipant == nullptr;
		if(Left.m_TeamIndex != Right.m_TeamIndex)
			return Left.m_TeamIndex < Right.m_TeamIndex;
		const int LeftRank = Left.m_Rank > 0 ? Left.m_Rank : std::numeric_limits<int>::max();
		const int RightRank = Right.m_Rank > 0 ? Right.m_Rank : std::numeric_limits<int>::max();
		if(LeftRank != RightRank)
			return LeftRank < RightRank;
		const CRowMetric *pLeft = Left.Metric("score");
		const CRowMetric *pRight = Right.Metric("score");
		return (pLeft == nullptr ? 0 : pLeft->m_Value) > (pRight == nullptr ? 0 : pRight->m_Value);
	});
	Hidden = std::max<int>(0, vRows.size() - MAX_ROWS);
	if(Hidden > 0)
		vRows.resize(MAX_ROWS);
	return vRows;
}

void CStatboard::MergeMetric(CRow &Row, std::string_view MetricId, int64_t Value, EMatchMetricAggregation Aggregation)
{
	const std::string_view Suffix = MetricIdSuffix(MetricId);
	for(size_t Index = 0; Index < Row.m_vMetrics.size(); ++Index)
	{
		CRowMetric &Existing = Row.m_vMetrics[Index];
		if(MetricIdSuffix(Existing.m_MetricId) != Suffix)
			continue;
		if(Existing.m_Aggregation == EMatchMetricAggregation::SUM)
			AddMatchCombatValue(Existing.m_Value, Value);
		else if(Existing.m_Aggregation == EMatchMetricAggregation::MAXIMUM)
			Existing.m_Value = std::max(Existing.m_Value, Value);
		else
			// A rank or a personal best describes one subject only. Folding
			// several of them together would invent a number nobody reported,
			// so the merged row simply has no value for it.
			Row.m_vMetrics.erase(Row.m_vMetrics.begin() + Index);
		return;
	}
	Row.m_vMetrics.push_back({MetricId, Value, Aggregation});
}

std::vector<CStatboard::CColumn> CStatboard::BuildColumns(const CMatchReport &Report, const std::vector<CRow> &vRows, EAspect Aspect)
{
	// Widest a table may get, so that a mode reporting a hundred metrics still
	// leaves room for the game it is drawn over.
	static constexpr size_t MAX_COLUMNS = 10;
	std::vector<CColumn> vColumns;

	if(Aspect != EAspect::SCORE)
	{
		const EColumnKind Kind = Aspect == EAspect::WEAPON_KILLS ? EColumnKind::WEAPON_KILLS : Aspect == EAspect::WEAPON_DEATHS ? EColumnKind::WEAPON_DEATHS :
																	  EColumnKind::ACCURACY;
		// A weapon only gets a column once somebody used it, and the column
		// that totals them all is always last.
		for(int Weapon = 0; Weapon < MAX_MATCH_WEAPONS && vColumns.size() + 1 < MAX_COLUMNS; ++Weapon)
		{
			if(std::none_of(vRows.begin(), vRows.end(), [&](const CRow &Row) {
				   const auto Found = std::find_if(Row.m_Combat.m_vWeapons.begin(), Row.m_Combat.m_vWeapons.end(), [&](const CMatchWeaponStats &Stats) { return Stats.m_Weapon == Weapon; });
				   return Found != Row.m_Combat.m_vWeapons.end() && (Kind == EColumnKind::WEAPON_KILLS ? Found->m_Kills : Kind == EColumnKind::WEAPON_DEATHS ? Found->m_Deaths :
																					       Found->m_Shots) > 0;
			   }))
				continue;
			// A mod may use a weapon this build has no sprite for, which then
			// gets its name written out instead of an icon.
			char aWeaponName[64] = "";
			if(Weapon >= NUM_WEAPONS)
				MatchWeaponDisplayName(Weapon, aWeaponName, sizeof(aWeaponName));
			vColumns.push_back({Kind, aWeaponName, {}, Weapon, Kind != EColumnKind::WEAPON_DEATHS, 96.0f});
		}
		vColumns.push_back({Kind, Localize("Total"), {}, -1, Kind != EColumnKind::WEAPON_DEATHS, 118.0f});
		return vColumns;
	}

	// The score aspect, in the order these columns are laid out. `m_Always`
	// marks the two that anchor the table and are shown even at zero; every
	// other column has to be worth its space before it appears.
	static const struct
	{
		const char *m_pSuffix;
		bool m_Always;
		bool m_HighlightMax;
	} s_aScoreColumns[] = {
		{"score", true, true},
		{"kills", true, true},
		{"deaths", true, false},
		{"assists", false, true},
		{"damage_done", false, true},
		{"damage_taken", false, false},
		{"flag_captures", false, true},
		{"flag_grabs", false, true},
		{"best_spree", false, true},
	};
	const auto AddColumn = [&](std::string_view Suffix, bool HighlightMax) {
		CColumn Column;
		Column.m_Suffix = Suffix;
		Column.m_HighlightMax = HighlightMax;
		for(const CRow &Row : vRows)
		{
			if(const CRowMetric *pMetric = Row.Metric(Suffix))
			{
				char aLabel[64];
				MatchMetricDisplayName(std::string(pMetric->m_MetricId), Report.m_ModeSchemaVersion, aLabel, sizeof(aLabel));
				Column.m_Label = aLabel;
				break;
			}
		}
		vColumns.push_back(std::move(Column));
	};

	for(const auto &Candidate : s_aScoreColumns)
	{
		bool Present = false;
		for(const CRow &Row : vRows)
		{
			const CRowMetric *pMetric = Row.Metric(Candidate.m_pSuffix);
			Present = Present || (pMetric != nullptr && (Candidate.m_Always || pMetric->m_Value != 0));
		}
		if(Present && vColumns.size() < MAX_COLUMNS)
			AddColumn(Candidate.m_pSuffix, Candidate.m_HighlightMax);
	}
	if(vColumns.size() < MAX_COLUMNS && std::any_of(vRows.begin(), vRows.end(), [](const CRow &Row) { return Row.m_Combat.m_Total.m_Shots > 0; }))
	{
		CColumn Column;
		Column.m_Kind = EColumnKind::ACCURACY;
		Column.m_Label = Localize("Accuracy");
		vColumns.push_back(std::move(Column));
	}

	// Whatever else the mode reported goes after the known columns. This is
	// what puts a metric of a mod on the board without a change here: it only
	// has to be reported.
	for(const CRow &Row : vRows)
	{
		for(const CRowMetric &Metric : Row.m_vMetrics)
		{
			if(vColumns.size() >= MAX_COLUMNS)
				return vColumns;
			const std::string_view Suffix = MetricIdSuffix(Metric.m_MetricId);
			// Weapons have aspects of their own, shots and hits are what the
			// accuracy column is made of, and a play time per row is noise.
			if(Metric.m_Value == 0 || Suffix.starts_with("weapon_") || Suffix == "shots" || Suffix == "hits" || Suffix == "playtime_ticks" || Suffix == "suicides")
				continue;
			if(std::any_of(std::begin(s_aScoreColumns), std::end(s_aScoreColumns), [&](const auto &Column) { return Suffix == Column.m_pSuffix; }) ||
				std::any_of(vColumns.begin(), vColumns.end(), [&](const CColumn &Column) { return Column.m_Suffix == Suffix; }))
				continue;
			AddColumn(Suffix, false);
		}
	}
	return vColumns;
}

CStatboard::CCell CStatboard::BuildCell(const CMatchReport &Report, const CRow &Row, const CColumn &Column)
{
	CCell Cell;
	if(Column.m_Kind == EColumnKind::METRIC)
	{
		const CRowMetric *pRowMetric = Row.Metric(Column.m_Suffix);
		if(pRowMetric == nullptr)
			return Cell;
		CMatchMetric Metric;
		Metric.m_MetricId = std::string(pRowMetric->m_MetricId);
		Metric.m_Value = pRowMetric->m_Value;
		FormatMatchMetricValue(Metric, Report.m_ModeSchemaVersion, Report.m_TickRate, Cell.m_aText, sizeof(Cell.m_aText));
		if(Column.m_HighlightMax)
			Cell.m_Comparable = static_cast<double>(pRowMetric->m_Value);
		return Cell;
	}

	const CMatchWeaponStats *pStats = &Row.m_Combat.m_Total;
	if(Column.m_Weapon >= 0)
	{
		const auto Found = std::find_if(Row.m_Combat.m_vWeapons.begin(), Row.m_Combat.m_vWeapons.end(), [&](const CMatchWeaponStats &Weapon) { return Weapon.m_Weapon == Column.m_Weapon; });
		if(Found == Row.m_Combat.m_vWeapons.end())
			return Cell;
		pStats = &*Found;
	}
	if(Column.m_Kind == EColumnKind::ACCURACY)
	{
		FormatMatchAccuracy(pStats->m_Hits, pStats->m_Shots, Cell.m_aText, sizeof(Cell.m_aText));
		if(pStats->m_Shots > 0)
			Cell.m_Comparable = 100.0 * static_cast<double>(pStats->m_Hits) / static_cast<double>(pStats->m_Shots);
		return Cell;
	}
	const int64_t Value = Column.m_Kind == EColumnKind::WEAPON_KILLS ? pStats->m_Kills : pStats->m_Deaths;
	str_format(Cell.m_aText, sizeof(Cell.m_aText), "%" PRId64, Value);
	if(Column.m_HighlightMax)
		Cell.m_Comparable = static_cast<double>(Value);
	return Cell;
}

void CStatboard::RenderBoard(const CRenderContext &Context, const CStoredMatch &Stored)
{
	const CMatchReport &Report = Stored.m_Report;
	const float ScreenWidth = 400.0f * 3.0f * Context.AspectRatio(Graphics()->ScreenAspect());
	const float ScreenHeight = 400.0f * 3.0f;
	Graphics()->MapScreenToSize(ScreenWidth, ScreenHeight);

	// A mode that reports a single subject, a race run for instance, has no
	// standing to tabulate: one row of columns would say less about what it
	// measured than a plain list of it.
	const bool Panel = Report.m_vTeams.empty() && Report.m_vParticipants.size() <= 1;
	std::vector<const CMatchMetric *> vpEntries;
	std::vector<CRow> vRows;
	std::vector<CColumn> vColumns;
	std::vector<EAspect> vAspects;
	EAspect Aspect = EAspect::SCORE;
	int Hidden = 0;
	float Width = 700.0f;
	float Height = 54.0f;
	if(Panel)
	{
		const std::optional<int> Subject = Report.m_vParticipants.empty() ? std::nullopt : std::optional<int>(Report.m_vParticipants.front().m_ParticipantId);
		for(const CMatchMetric &Metric : Report.m_vMetrics)
			if(Metric.m_SubjectKind == EMatchSubjectKind::MATCH || (Metric.m_SubjectKind == EMatchSubjectKind::PARTICIPANT && Metric.m_SubjectId == Subject))
				vpEntries.push_back(&Metric);
		// Within a category the order the mode declared its metrics in is kept,
		// which is the order it considers them worth reading in.
		std::stable_sort(vpEntries.begin(), vpEntries.end(), [&](const CMatchMetric *pLeft, const CMatchMetric *pRight) {
			return MatchMetricCategory(pLeft->m_MetricId, Report.m_ModeSchemaVersion) < MatchMetricCategory(pRight->m_MetricId, Report.m_ModeSchemaVersion);
		});
		// An empty list is still worth drawing: the title and the name say that
		// the mode is reporting and simply has nothing to tell yet.
		Height += 34.0f + vpEntries.size() * 30.0f;
	}
	else
	{
		vRows = BuildRows(Stored, Hidden);
		if(vRows.empty())
			return;
		vAspects.push_back(EAspect::SCORE);
		// A mode that reports no weapon numbers does not offer the aspects that
		// would only show empty columns.
		bool aUsed[3] = {};
		for(const CRow &Row : vRows)
			for(const CMatchWeaponStats &Weapon : Row.m_Combat.m_vWeapons)
			{
				aUsed[0] = aUsed[0] || Weapon.m_Kills > 0;
				aUsed[1] = aUsed[1] || Weapon.m_Deaths > 0;
				aUsed[2] = aUsed[2] || Weapon.m_Shots > 0;
			}
		if(aUsed[0])
			vAspects.push_back(EAspect::WEAPON_KILLS);
		if(aUsed[1])
			vAspects.push_back(EAspect::WEAPON_DEATHS);
		if(aUsed[2])
			vAspects.push_back(EAspect::WEAPON_ACCURACY);
		Aspect = vAspects[m_Aspect % static_cast<int>(vAspects.size())];
		vColumns = BuildColumns(Report, vRows, Aspect);

		Width = 366.0f;
		for(const CColumn &Column : vColumns)
			Width += Column.m_Width;
		// Per team a band row and the bar that introduces its table, plus the
		// column headers, the rows and the line that counts what did not fit.
		Height += Report.m_vTeams.size() * 96.0f + (vAspects.size() > 1 ? 34.0f : 0.0f) + 28.0f + vRows.size() * RowHeight(vRows.size()) + (Hidden > 0 ? 24.0f : 0.0f);
	}
	Width = std::max(Width, 760.0f) + 20.0f;

	const float X = ScreenWidth / 2.0f - Width / 2.0f;
	const float Y = std::max(60.0f, (ScreenHeight - Height) / 2.0f - 120.0f);
	GameClient()->m_Menus.RenderBackdropRegion({X, Y, Width, Height});
	Graphics()->DrawRect(X, Y, Width, Height, ColorRGBA(0.0f, 0.0f, 0.0f, 0.6f), IGraphics::CORNER_ALL, 17.0f);

	const float ContentX = X + 10.0f;
	const float ContentWidth = Width - 20.0f;
	float ContentY = Y + 10.0f;

	char aDuration[64];
	FormatMatchDuration(Report.m_DurationTicks, Report.m_TickRate, aDuration, sizeof(aDuration));
	char aTitle[512];
	str_format(aTitle, sizeof(aTitle), "%s  ·  %s  ·  %s", Report.m_MapName.c_str(), Report.m_ModeId.c_str(), aDuration);
	TextRender()->Text(ContentX, ContentY, 20.0f, aTitle, ContentWidth - 200.0f);
	// Says where these numbers come from, in the words the statistics pages use
	// for a stored match.
	const char *pSource = MatchReportSourceDisplayName(Stored.m_Source);
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
	RightText(TextRender(), ContentX + ContentWidth, ContentY + 3.0f, 18.0f, pSource);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	ContentY += 34.0f;

	if(Panel)
	{
		if(!Report.m_vParticipants.empty())
		{
			TextRender()->TextColor(ColorRGBA(0.30f, 0.72f, 1.0f, 1.0f));
			TextRender()->Text(ContentX, ContentY, 24.0f, Report.m_vParticipants.front().m_DisplayName.c_str(), ContentWidth - 160.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}
		ContentY += 34.0f;
		// Whatever the mode measured, named and formatted by the same helpers
		// the statistics pages use, so that a metric this build has never heard
		// of still reads as a sentence and a duration still reads as a time.
		for(const CMatchMetric *pMetric : vpEntries)
		{
			char aValue[64];
			FormatMatchMetricValue(*pMetric, Report.m_ModeSchemaVersion, Report.m_TickRate, aValue, sizeof(aValue));
			TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
			char aMetricName[64];
			MatchMetricDisplayName(pMetric->m_MetricId, Report.m_ModeSchemaVersion, aMetricName, sizeof(aMetricName));
			TextRender()->Text(ContentX, ContentY + 4.0f, 20.0f, aMetricName, ContentWidth - 220.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
			RightText(TextRender(), ContentX + ContentWidth, ContentY + 3.0f, 22.0f, aValue);
			ContentY += 30.0f;
		}
		return;
	}

	// The team that is ahead, which is the one thing a summary band has to say
	// at a glance. A tie leaves nobody highlighted.
	int Leader = -1;
	for(size_t Team = 0; Team < Report.m_vTeams.size(); ++Team)
	{
		const std::optional<int64_t> Score = ReportMetric(Report, EMatchSubjectKind::TEAM, Report.m_vTeams[Team].m_TeamId, "score");
		const std::optional<int64_t> Best = Leader < 0 ? std::nullopt : ReportMetric(Report, EMatchSubjectKind::TEAM, Report.m_vTeams[Leader].m_TeamId, "score");
		if(!Score.has_value())
			continue;
		if(!Best.has_value() || *Score > *Best)
			Leader = Team;
		else if(*Score == *Best)
			Leader = -1;
	}

	static const char *const s_apTotals[] = {"kills", "deaths", "damage_done", "damage_taken"};
	for(size_t Team = 0; Team < Report.m_vTeams.size(); ++Team)
	{
		Graphics()->DrawRect(ContentX, ContentY, ContentWidth, 60.0f, TeamColor(Team).WithAlpha(static_cast<int>(Team) == Leader ? 0.45f : 0.18f), IGraphics::CORNER_ALL, 6.0f);
		TextRender()->Text(ContentX + 12.0f, ContentY + 18.0f, 24.0f, Report.m_vTeams[Team].m_DisplayName.c_str(), 240.0f);
		if(const std::optional<int64_t> Score = ReportMetric(Report, EMatchSubjectKind::TEAM, Report.m_vTeams[Team].m_TeamId, "score"); Score.has_value())
		{
			char aScore[32];
			str_format(aScore, sizeof(aScore), "%" PRId64, *Score);
			RightText(TextRender(), ContentX + 330.0f, ContentY + 15.0f, 30.0f, aScore);
		}

		// Whatever the players currently on the team reported, combined the way
		// the mode said it may be combined, so that a rank or a best time is
		// never added up.
		CRow Totals;
		for(const CRow &Row : vRows)
		{
			if(Row.m_TeamIndex != static_cast<int>(Team))
				continue;
			for(const CRowMetric &Metric : Row.m_vMetrics)
				MergeMetric(Totals, Metric.m_MetricId, Metric.m_Value, Metric.m_Aggregation);
			AddCombatStats(Totals.m_Combat, Row.m_Combat);
		}

		float Right = ContentX + ContentWidth - 12.0f;
		const auto Summary = [&](const char *pLabel, const char *pValue) {
			Right -= std::max(TextRender()->TextWidth(15.0f, pLabel, -1, -1.0f), TextRender()->TextWidth(22.0f, pValue, -1, -1.0f)) + 30.0f;
			TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.55f));
			TextRender()->Text(Right, ContentY + 10.0f, 15.0f, pLabel, -1.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
			TextRender()->Text(Right, ContentY + 28.0f, 22.0f, pValue, -1.0f);
		};
		if(Totals.m_Combat.m_Total.m_Shots > 0)
		{
			char aAccuracy[32];
			FormatMatchAccuracy(Totals.m_Combat.m_Total.m_Hits, Totals.m_Combat.m_Total.m_Shots, aAccuracy, sizeof(aAccuracy));
			Summary(Localize("Accuracy"), aAccuracy);
		}
		for(size_t Index = std::size(s_apTotals); Index-- > 0;)
		{
			const CRowMetric *pMetric = Totals.Metric(s_apTotals[Index]);
			if(pMetric == nullptr)
				continue;
			char aValue[32];
			str_format(aValue, sizeof(aValue), "%" PRId64, pMetric->m_Value);
			char aMetricName[64];
			MatchMetricDisplayName(std::string(pMetric->m_MetricId), Report.m_ModeSchemaVersion, aMetricName, sizeof(aMetricName));
			Summary(aMetricName, aValue);
		}
		ContentY += 70.0f;
	}

	if(vAspects.size() > 1)
	{
		float TabX = ContentX;
		for(const EAspect Candidate : vAspects)
		{
			const char *pLabel = Candidate == EAspect::SCORE ? Localize("Score") : Candidate == EAspect::WEAPON_KILLS ? Localize("Kills") :
										       Candidate == EAspect::WEAPON_DEATHS        ? Localize("Deaths") :
																    Localize("Accuracy");
			const float LabelWidth = TextRender()->TextWidth(19.0f, pLabel, -1, -1.0f);
			if(Candidate == Aspect)
				Graphics()->DrawRect(TabX - 6.0f, ContentY - 2.0f, LabelWidth + 12.0f, 28.0f, ColorRGBA(0.30f, 0.62f, 1.0f, 0.35f), IGraphics::CORNER_ALL, 4.0f);
			TextRender()->TextColor(Candidate == Aspect ? TextRender()->DefaultTextColor() : ColorRGBA(1.0f, 1.0f, 1.0f, 0.45f));
			TextRender()->Text(TabX, ContentY + 2.0f, 19.0f, pLabel, -1.0f);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
			TabX += LabelWidth + 30.0f;
		}
		ContentY += 34.0f;
	}

	RenderTable(Context, Stored, vRows, vColumns, Hidden, ContentX, ContentY, ContentWidth);
}

float CStatboard::RenderTable(const CRenderContext &Context, const CStoredMatch &Stored, const std::vector<CRow> &vRows, const std::vector<CColumn> &vColumns, int Hidden, float X, float Y, float Width)
{
	const CMatchReport &Report = Stored.m_Report;
	// Participant ids only mean client ids on the board this client counted
	// itself, and only there can a row show the tee of its player.
	const CSessionPresentation *pPresentation = Stored.m_Source == EMatchReportSource::CLIENT_OBSERVED ? &GameClient()->SessionPresentation(Context.m_Session.Id()) : nullptr;
	const float NameX = X + 46.0f;
	const float FirstColumnX = NameX + 320.0f;
	const float RowH = RowHeight(vRows.size());
	const float FontSize = RowH > 26.0f ? 20.0f : 17.0f;
	const float TextOffset = (RowH - FontSize) / 2.0f;

	// Column headers once for the whole body: repeating nine words in the
	// middle of the table is what a second team header would cost.
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.55f));
	TextRender()->Text(NameX, Y + 4.0f, 17.0f, Localize("Name"), 300.0f);
	float ColumnX = FirstColumnX;
	for(const CColumn &Column : vColumns)
	{
		if(Column.m_Label.empty() && Column.m_Weapon >= 0 && Column.m_Weapon < NUM_WEAPONS)
		{
			const int Weapon = Column.m_Weapon;
			float ScaleX, ScaleY;
			Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[Weapon].m_pSpriteBody, ScaleX, ScaleY);
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon]);
			Graphics()->QuadsBegin();
			const float Size = g_pData->m_Weapons.m_aId[Weapon].m_VisualSize * (Weapon == WEAPON_HAMMER ? 0.6f : 0.75f);
			Graphics()->DrawSprite(ColumnX + Column.m_Width / 2.0f, Y + 12.0f, Size * ScaleX, Size * ScaleY);
			Graphics()->QuadsEnd();
		}
		else
			RightText(TextRender(), ColumnX + Column.m_Width - 8.0f, Y + 4.0f, 17.0f, Column.m_Label.c_str());
		ColumnX += Column.m_Width;
	}
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	Y += 28.0f;

	// The best value of every column, so that a row does not have to know
	// about the others to emphasise it.
	std::vector<std::optional<double>> vBest(vColumns.size());
	for(const CRow &Row : vRows)
	{
		if(Row.m_pParticipant == nullptr)
			continue;
		for(size_t Index = 0; Index < vColumns.size(); ++Index)
		{
			const CCell Cell = BuildCell(Report, Row, vColumns[Index]);
			if(Cell.m_Comparable.has_value() && *Cell.m_Comparable > 0.0 && (!vBest[Index].has_value() || *Cell.m_Comparable > *vBest[Index]))
				vBest[Index] = Cell.m_Comparable;
		}
	}

	int TeamIndex = -1;
	for(const CRow &Row : vRows)
	{
		// One table per team, introduced by a bar in the team's colour.
		if(Row.m_pParticipant != nullptr && Row.m_TeamIndex != TeamIndex && Row.m_TeamIndex >= 0 && Row.m_TeamIndex < static_cast<int>(Report.m_vTeams.size()))
		{
			TeamIndex = Row.m_TeamIndex;
			Graphics()->DrawRect(X, Y + 2.0f, Width, 22.0f, TeamColor(TeamIndex).WithAlpha(0.35f), IGraphics::CORNER_ALL, 4.0f);
			TextRender()->Text(X + 8.0f, Y + 4.0f, 18.0f, Report.m_vTeams[TeamIndex].m_DisplayName.c_str(), 300.0f);
			Y += 26.0f;
		}

		if(Row.m_pParticipant == nullptr)
		{
			char aLabel[128];
			str_format(aLabel, sizeof(aLabel), Localize("%d players who left"), Row.m_Members);
			TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
			TextRender()->Text(NameX, Y + TextOffset, FontSize, aLabel, 300.0f);
		}
		else
		{
			if(Row.m_Rank > 0)
			{
				char aRank[16];
				str_format(aRank, sizeof(aRank), "%d", Row.m_Rank);
				TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
				TextRender()->Text(X + 4.0f, Y + TextOffset, FontSize, aRank, 40.0f);
			}
			float TextX = NameX;
			const CClientPresentation *pClient = pPresentation == nullptr ? nullptr : pPresentation->Client(Context.m_State.Id(), Row.m_pParticipant->m_ParticipantId);
			if(pClient != nullptr)
			{
				CTeeRenderInfo TeeInfo = pClient->m_BaseRenderInfo;
				TeeInfo.m_Size *= RowH / 66.0f;
				vec2 OffsetToMid;
				const CAnimState *pIdleState = CAnimState::GetIdle();
				CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
				RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(TextX + TeeInfo.m_Size / 2.0f, Y + RowH / 2.0f + OffsetToMid.y));
				TextX += RowH + 4.0f;
			}
			// The local player is marked by the colour of his name, not by a
			// bar behind it, which would fight with the team colours.
			TextRender()->TextColor(Row.m_Local ? ColorRGBA(0.30f, 0.72f, 1.0f, 1.0f) : TextRender()->DefaultTextColor());
			TextRender()->Text(TextX, Y + TextOffset, FontSize, Row.m_pParticipant->m_DisplayName.c_str(), NameX + 236.0f - TextX);
			if(!Row.m_pParticipant->m_Clan.empty())
			{
				TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.4f));
				TextRender()->Text(NameX + 240.0f, Y + TextOffset + 2.0f, FontSize - 4.0f, Row.m_pParticipant->m_Clan.c_str(), 76.0f);
			}
		}

		ColumnX = FirstColumnX;
		for(size_t Index = 0; Index < vColumns.size(); ++Index)
		{
			const CCell Cell = BuildCell(Report, Row, vColumns[Index]);
			const bool Best = Row.m_pParticipant != nullptr && Cell.m_Comparable.has_value() && vBest[Index].has_value() && *Cell.m_Comparable >= *vBest[Index];
			TextRender()->TextColor(Row.m_pParticipant == nullptr ? ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f) : Best ? ColorRGBA(1.0f, 0.86f, 0.4f, 1.0f) :
															   ColorRGBA(1.0f, 1.0f, 1.0f, 0.85f));
			RightText(TextRender(), ColumnX + vColumns[Index].m_Width - 8.0f, Y + TextOffset, FontSize, Cell.m_aText);
			ColumnX += vColumns[Index].m_Width;
		}
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		Y += RowH;
	}

	if(Hidden > 0)
	{
		char aHidden[64];
		str_format(aHidden, sizeof(aHidden), Localize("%d more not shown"), Hidden);
		TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.45f));
		TextRender()->Text(NameX, Y + 2.0f, 17.0f, aHidden, 300.0f);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		Y += 24.0f;
	}
	return Y;
}

void CStatboard::AutoStatScreenshot()
{
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
		Client()->AutoStatScreenshot_Start();
}

void CStatboard::AutoStatCSV()
{
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		char aDate[20], aFilename[IO_MAX_PATH_LENGTH];
		str_timestamp(aDate, sizeof(aDate));
		str_format(aFilename, sizeof(aFilename), "screenshots/auto/stats_%s.csv", aDate);
		IOHANDLE File = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);

		if(File)
		{
			char aStats[1024 * (VANILLA_MAX_CLIENTS + 1)];
			FormatStats(aStats, sizeof(aStats));
			io_write(File, aStats, str_length(aStats));
			io_close(File);
		}

		Client()->AutoCSV_Start();
	}
}

std::string CStatboard::ReplaceCommata(const char *pStr)
{
	if(!str_find(pStr, ","))
		return pStr;

	char aOutbuf[256] = "";
	for(int i = 0, Skip = 0; i < 64; i++)
	{
		if(pStr[i] == ',')
		{
			aOutbuf[i + Skip++] = '%';
			aOutbuf[i + Skip++] = '2';
			aOutbuf[i + Skip] = 'C';
		}
		else
			aOutbuf[i + Skip] = pStr[i];
	}
	return aOutbuf;
}

void CStatboard::FormatStats(char *pDest, size_t DestSize)
{
	const CSessionId SessionId = GameClient()->SessionContext().Id();
	const CSessionStatsState &Stats = GameClient()->SessionContext().Stats();
	// server stats
	const CServerInfo &CurrentServerInfo = Client()->ServerInfo(SessionId);

	char aServerStats[1024];
	str_format(aServerStats, sizeof(aServerStats), "Servername,Game-type,Map\n%s,%s,%s", ReplaceCommata(CurrentServerInfo.m_aName).c_str(), ReplaceCommata(CurrentServerInfo.m_aGameType).c_str(), ReplaceCommata(CurrentServerInfo.m_aMap).c_str());

	// player stats

	// sort players
	const CNetObj_PlayerInfo *apPlayers[MAX_CLIENTS] = {nullptr};
	int NumPlayers = 0;

	// sort red or dm players by score
	for(const auto *pInfo : GameClient()->Snap().m_apInfoByScore)
	{
		if(!pInfo || !Stats.Client(pInfo->m_ClientId).IsActive() || GameClient()->m_aClients[pInfo->m_ClientId].m_Team != TEAM_RED)
			continue;
		apPlayers[NumPlayers] = pInfo;
		NumPlayers++;
	}

	// sort blue players by score after
	if(GameClient()->IsTeamPlay())
	{
		for(const auto *pInfo : GameClient()->Snap().m_apInfoByScore)
		{
			if(!pInfo || !Stats.Client(pInfo->m_ClientId).IsActive() || GameClient()->m_aClients[pInfo->m_ClientId].m_Team != TEAM_BLUE)
				continue;
			apPlayers[NumPlayers] = pInfo;
			NumPlayers++;
		}
	}

	char aPlayerStats[1024 * VANILLA_MAX_CLIENTS] = "Local-player,Team,Name,Clan,Score,Frags,Deaths,Suicides,F/D-ratio,Net,FPM,Spree,Best,Hammer-F/D,Gun-F/D,Shotgun-F/D,Grenade-F/D,Laser-F/D,Ninja-F/D,GameWithFlags,Flag-grabs,Flag-captures\n";
	for(int i = 0; i < NumPlayers; i++)
	{
		const CNetObj_PlayerInfo *pInfo = apPlayers[i];
		const CSessionClientStats *pStats = &Stats.Client(pInfo->m_ClientId);

		// Pre-formatting

		// Weapons frags/deaths
		char aWeaponFD[64 * NUM_WEAPONS];
		for(int j = 0; j < NUM_WEAPONS; j++)
		{
			if(j == 0)
				str_format(aWeaponFD, sizeof(aWeaponFD), "%d/%d", pStats->m_aFragsWith[j], pStats->m_aDeathsFrom[j]);
			else
				str_format(aWeaponFD, sizeof(aWeaponFD), "%s,%d/%d", aWeaponFD, pStats->m_aFragsWith[j], pStats->m_aDeathsFrom[j]);
		}

		// Frag/Death ratio
		float KillRatio = 0.0f;
		if(pStats->m_Deaths != 0)
			KillRatio = (float)(pStats->m_Frags) / pStats->m_Deaths;

		// Local player
		bool LocalPlayer = (GameClient()->Snap().m_LocalClientId == pInfo->m_ClientId || (GameClient()->Snap().m_SpecInfo.m_Active && pInfo->m_ClientId == GameClient()->Snap().m_SpecInfo.m_SpectatorId));

		// Game with flags
		bool GameWithFlags = (GameClient()->Snap().m_pGameInfoObj && GameClient()->Snap().m_pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS);

		char aBuf[1024];
		str_format(aBuf, sizeof(aBuf), "%d,%d,%s,%s,%d,%d,%d,%d,%.2f,%i,%.1f,%d,%d,%s,%d,%d,%d\n",
			LocalPlayer ? 1 : 0, // Local player
			GameClient()->m_aClients[pInfo->m_ClientId].m_Team, // Team
			ReplaceCommata(GameClient()->m_aClients[pInfo->m_ClientId].m_aName).c_str(), // Name
			ReplaceCommata(GameClient()->m_aClients[pInfo->m_ClientId].m_aClan).c_str(), // Clan
			std::clamp(pInfo->m_Score, -999, 999), // Score
			pStats->m_Frags, // Frags
			pStats->m_Deaths, // Deaths
			pStats->m_Suicides, // Suicides
			KillRatio, // Kill ratio
			pStats->m_Frags - pStats->m_Deaths, // Net
			pStats->GetFPM(Client()->GameTick(SessionId, GameClient()->ActiveConnection()), Client()->GameTickSpeed()), // FPM
			pStats->m_CurrentSpree, // CurSpree
			pStats->m_BestSpree, // BestSpree
			aWeaponFD, // WeaponFD
			GameWithFlags ? 1 : 0, // GameWithFlags
			pStats->m_FlagGrabs, // Flag grabs
			pStats->m_FlagCaptures); // Flag captures

		str_append(aPlayerStats, aBuf);
	}

	str_format(pDest, DestSize, "%s\n\n%s", aServerStats, aPlayerStats);
}
