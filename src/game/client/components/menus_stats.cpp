#include "menus.h"

#include <base/hash.h>
#include <base/io.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/storage.h>

#include <generated/client_data.h>

#include <game/client/gameclient.h>
#include <game/client/match_report_view.h>
#include <game/client/match_stats_export.h>
#include <game/client/ui_listbox.h>
#include <game/client/ui_scrollregion.h>
#include <game/localization.h>

#include <algorithm>
#include <ctime>
#include <functional>
#include <limits>
#include <vector>

namespace
{
	// One accent per outcome, so that a result is recognised before it is read.
	const ColorRGBA COLOR_WIN = ColorRGBA(0.35f, 0.82f, 0.45f, 1.0f);
	const ColorRGBA COLOR_LOSS = ColorRGBA(0.93f, 0.36f, 0.36f, 1.0f);
	const ColorRGBA COLOR_NEUTRAL = ColorRGBA(0.72f, 0.74f, 0.78f, 1.0f);
	const ColorRGBA COLOR_ACCENT = ColorRGBA(0.30f, 0.62f, 1.0f, 1.0f);
	const ColorRGBA COLOR_PANEL = ColorRGBA(1.0f, 1.0f, 1.0f, 0.06f);
	const ColorRGBA COLOR_TABLE_HEADER = ColorRGBA(1.0f, 1.0f, 1.0f, 0.14f);
	// Width of the leftmost column, shared so that the tables line up
	const float STATS_LABEL_WIDTH = 190.0f;

	ColorRGBA OutcomeColor(const std::optional<EMatchOutcome> &Outcome)
	{
		if(!Outcome.has_value())
			return COLOR_NEUTRAL;
		switch(*Outcome)
		{
		case EMatchOutcome::WIN:
		case EMatchOutcome::FINISHED:
			return COLOR_WIN;
		case EMatchOutcome::LOSS:
		case EMatchOutcome::DNF:
		case EMatchOutcome::DISQUALIFIED:
			return COLOR_LOSS;
		case EMatchOutcome::DRAW:
			return COLOR_NEUTRAL;
		}
		return COLOR_NEUTRAL;
	}

	const char *OutcomeName(const std::optional<EMatchOutcome> &Outcome)
	{
		return Outcome.has_value() ? MatchOutcomeDisplayName(*Outcome) : "-";
	}

	// The mode ids carry a vendor suffix that says nothing in a list of matches.
	std::string ShortModeName(const std::string &ModeId)
	{
		const size_t At = ModeId.find('@');
		return At == std::string::npos ? ModeId : ModeId.substr(0, At);
	}

	void FormatRatio(int64_t Numerator, int64_t Denominator, char *pBuffer, int BufferSize)
	{
		if(Denominator <= 0)
			str_format(pBuffer, BufferSize, "%" PRId64 ".00", Numerator);
		else
			str_format(pBuffer, BufferSize, "%.2f", static_cast<double>(Numerator) / static_cast<double>(Denominator));
	}
}

void CMenus::OpenDemos()
{
	if(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK)
		m_GamePage = PAGE_DEMOS;
	else
		SetMenuPage(PAGE_DEMOS);
	SetActive(true);
}

void CMenus::OpenStats()
{
	m_StatsTab = EStatsTab::MATCHES;
	m_StatsShowMatch = false;
	m_StatsInitialized = false;
	if(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK)
		m_GamePage = PAGE_STATS;
	else
		SetMenuPage(PAGE_STATS);
	SetActive(true);
}

const char *CMenus::StatsModeFilter() const
{
	return m_StatsModeIndex > 0 && m_StatsModeIndex <= static_cast<int>(m_vStatsModes.size()) ? m_vStatsModes[m_StatsModeIndex - 1].c_str() : "";
}

bool CMenus::StatsModeDropDown(CUIRect Rect)
{
	std::vector<std::string> vNames;
	vNames.reserve(m_vStatsModes.size() + 1);
	vNames.emplace_back(Localize("All gametypes"));
	for(const std::string &ModeId : m_vStatsModes)
		vNames.push_back(ShortModeName(ModeId));
	std::vector<const char *> vpNames;
	vpNames.reserve(vNames.size());
	for(const std::string &Name : vNames)
		vpNames.push_back(Name.c_str());
	static CUi::SDropDownState s_DropDownState;
	static CScrollRegion s_DropDownScrollRegion;
	s_DropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_DropDownScrollRegion;
	const int Picked = Ui()->DoDropDown(&Rect, std::clamp(m_StatsModeIndex, 0, static_cast<int>(vpNames.size()) - 1), vpNames.data(), vpNames.size(), s_DropDownState);
	if(Picked == m_StatsModeIndex)
		return false;
	m_StatsModeIndex = Picked;
	m_StatsSelectedIndex = -1;
	RefreshStats();
	return true;
}

void CMenus::RefreshStats()
{
	m_StatsError.clear();
	m_vStatsHistory.clear();
	m_StatsSelectedMatch.reset();
	for(CMatchProfile &Profile : m_aStatsProfiles)
		Profile = {};
	m_StatsInfo = {};
	CMatchJournal &Journal = GameClient()->MatchJournal();
	if(!Journal.IsOpen())
	{
		m_StatsError = "Match journal is unavailable";
		m_StatsInitialized = true;
		return;
	}

	CMatchHistoryFilter HistoryFilter;
	if(!Journal.ListMatches(HistoryFilter, m_vStatsHistory, &m_StatsError))
	{
		m_StatsInitialized = true;
		return;
	}
	// The gametype filter offers what the journal holds, so it is collected
	// from the unfiltered list before anything is thrown away.
	m_vStatsModes.clear();
	for(const CMatchHistoryEntry &Entry : m_vStatsHistory)
		if(std::find(m_vStatsModes.begin(), m_vStatsModes.end(), Entry.m_ModeId) == m_vStatsModes.end())
			m_vStatsModes.push_back(Entry.m_ModeId);
	std::sort(m_vStatsModes.begin(), m_vStatsModes.end());
	if(m_StatsModeIndex > static_cast<int>(m_vStatsModes.size()))
		m_StatsModeIndex = 0;
	const char *pMode = StatsModeFilter();

	const char *pSearch = m_StatsHistorySearchInput.GetString();
	m_vStatsHistory.erase(std::remove_if(m_vStatsHistory.begin(), m_vStatsHistory.end(), [&](const CMatchHistoryEntry &Entry) {
		const bool ModeMatches = pMode[0] == '\0' || Entry.m_ModeId == pMode;
		const bool SearchMatches = pSearch[0] == '\0' || str_find_nocase(Entry.m_ModeId.c_str(), pSearch) != nullptr || str_find_nocase(Entry.m_MapName.c_str(), pSearch) != nullptr || str_find_nocase(Entry.m_OriginId.c_str(), pSearch) != nullptr;
		const bool QualityMatches = m_StatsQualityFilter == EStatsQualityFilter::ALL ||
					    (m_StatsQualityFilter == EStatsQualityFilter::COMPLETE && Entry.m_Completeness == EMatchCompleteness::COMPLETE) ||
					    (m_StatsQualityFilter == EStatsQualityFilter::SERVER && Entry.m_Source != EMatchReportSource::CLIENT_OBSERVED);
		return !ModeMatches || !SearchMatches || !QualityMatches;
	}),
		m_vStatsHistory.end());
	if(m_StatsSelectedIndex >= static_cast<int>(m_vStatsHistory.size()))
		m_StatsSelectedIndex = -1;
	if(m_StatsSelectedIndex < 0 && !m_vStatsHistory.empty())
		m_StatsSelectedIndex = 0;
	LoadSelectedStatsMatch();

	// The periods are queried together, because the profile shows them next to
	// each other rather than one at a time.
	static const int s_aPeriodDays[(int)EStatsPeriod::COUNT] = {1, 7, 30, 0};
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
	{
		CMatchProfileFilter ProfileFilter;
		if(s_aPeriodDays[Period] > 0)
			ProfileFilter.m_SinceUtc = time_timestamp() - static_cast<int64_t>(s_aPeriodDays[Period]) * 24 * 60 * 60;
		ProfileFilter.m_ModeId = pMode;
		if(!Journal.QueryProfile(ProfileFilter, m_aStatsProfiles[Period], &m_StatsError))
			m_StatsError = m_StatsError.empty() ? "Unable to query match journal" : m_StatsError;
	}
	if(!Journal.Info(m_StatsInfo, &m_StatsError))
		m_StatsError = m_StatsError.empty() ? "Unable to query match journal" : m_StatsError;
	m_StatsInitialized = true;
}

void CMenus::LoadSelectedStatsMatch()
{
	m_StatsSelectedMatch.reset();
	if(m_StatsSelectedIndex < 0 || m_StatsSelectedIndex >= static_cast<int>(m_vStatsHistory.size()))
		return;
	CStoredMatch Match;
	const CMatchHistoryEntry &Entry = m_vStatsHistory[m_StatsSelectedIndex];
	if(GameClient()->MatchJournal().LoadMatch(Entry.m_OriginId.c_str(), Entry.m_MatchId, Match, &m_StatsError))
		m_StatsSelectedMatch = std::move(Match);
}

void CMenus::ExportSelectedStats(bool Csv)
{
	if(!m_StatsSelectedMatch.has_value())
		return;
	ExportMatchStats(*m_StatsSelectedMatch, Csv);
}

void CMenus::ExportMatchStats(const CStoredMatch &Stored, bool Csv)
{
	Storage()->CreateFolder("match_stats", IStorage::TYPE_SAVE);
	char aMatchId[UUID_MAXSTRSIZE];
	FormatUuid(Stored.m_Report.m_MatchId, aMatchId, sizeof(aMatchId));
	char aOriginHash[SHA256_MAXSTRSIZE];
	sha256_str(sha256(Stored.m_OriginId.data(), Stored.m_OriginId.size()), aOriginHash, sizeof(aOriginHash));
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "match_stats/%s-%s.%s", aMatchId, aOriginHash, Csv ? "csv" : "json");
	IOHANDLE File = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(File == nullptr)
	{
		PopupMessage(Localize("Export failed"), Localize("Unable to create the export file."), Localize("Ok"));
		return;
	}

	if(!Csv)
	{
		const std::string Json = MatchStatsExportJson(Stored);
		if(io_write(File, Json.data(), static_cast<unsigned>(Json.size())) != Json.size())
		{
			io_close(File);
			PopupMessage(Localize("Export failed"), Localize("Unable to write the export file."), Localize("Ok"));
			return;
		}
	}
	else
		MatchStatsExportCsv(File, Stored);
	if(io_close(File) != 0)
	{
		PopupMessage(Localize("Export failed"), Localize("Unable to write the export file."), Localize("Ok"));
		return;
	}
	Storage()->SyncPersistentStorage();
	PopupMessage(Localize("Export complete"), aFilename, Localize("Ok"));
}

void CMenus::PopupConfirmDeleteStatsMatch()
{
	if(!m_StatsSelectedMatch.has_value())
		return;
	const CStoredMatch &Stored = *m_StatsSelectedMatch;
	if(!GameClient()->MatchJournal().DeleteMatch(Stored.m_OriginId.c_str(), Stored.m_Report.m_MatchId, &m_StatsError))
	{
		PopupMessage(Localize("Error"), m_StatsError.c_str(), Localize("Ok"));
		return;
	}
	m_StatsSelectedIndex = -1;
	m_StatsShowMatch = false;
	RefreshStats();
}

void CMenus::PopupConfirmDeleteStatsPeriod()
{
	if(!GameClient()->MatchJournal().DeleteAll(&m_StatsError))
	{
		PopupMessage(Localize("Error"), m_StatsError.c_str(), Localize("Ok"));
		return;
	}
	m_StatsSelectedIndex = -1;
	m_StatsShowMatch = false;
	RefreshStats();
}

void CMenus::StatsHeading(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pText)
{
	CUIRect Line, Underline;
	pContent->HSplitTop(8.0f, nullptr, pContent);
	pContent->HSplitTop(22.0f, &Line, pContent);
	if(!pScrollRegion->AddRect(Line))
		return;
	Line.HSplitBottom(1.0f, &Line, &Underline);
	Ui()->DoLabel(&Line, pText, 14.0f, TEXTALIGN_ML);
	Underline.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.15f), IGraphics::CORNER_NONE, 0.0f);
}

void CMenus::StatsTiles(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *const *ppLabels, const char *const *ppValues, int Count)
{
	CUIRect Row;
	pContent->HSplitTop(6.0f, nullptr, pContent);
	pContent->HSplitTop(52.0f, &Row, pContent);
	if(!pScrollRegion->AddRect(Row))
		return;
	for(int Tile = 0; Tile < Count; ++Tile)
	{
		CUIRect Box, Value, Label;
		Row.VSplitLeft(Row.w / (Count - Tile), &Box, &Row);
		Box.VMargin(3.0f, &Box);
		Box.Draw(COLOR_PANEL, IGraphics::CORNER_ALL, 4.0f);
		Box.Margin(5.0f, &Box);
		// The number is what the eye should land on, the label only names it.
		Box.HSplitTop(26.0f, &Value, &Label);
		Ui()->DoLabel(&Value, ppValues[Tile], 21.0f, TEXTALIGN_MC, {.m_MaxWidth = Value.w, .m_EllipsisAtEnd = true});
		SLabelProperties LabelProperties;
		LabelProperties.m_MaxWidth = Label.w;
		LabelProperties.m_EllipsisAtEnd = true;
		LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.55f));
		Ui()->DoLabel(&Label, ppLabels[Tile], 9.5f, TEXTALIGN_MC, LabelProperties);
	}
}

void CMenus::StatsMetricLine(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pLabel, const char *pValue)
{
	CUIRect Line, Label, Value;
	pContent->HSplitTop(19.0f, &Line, pContent);
	if(!pScrollRegion->AddRect(Line))
		return;
	Line.VSplitLeft(220.0f, &Label, &Value);
	SLabelProperties LabelProperties;
	LabelProperties.m_MaxWidth = Label.w;
	LabelProperties.m_EllipsisAtEnd = true;
	LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
	Ui()->DoLabel(&Label, pLabel, 11.5f, TEXTALIGN_ML, LabelProperties);
	Ui()->DoLabel(&Value, pValue, 11.5f, TEXTALIGN_ML, {.m_MaxWidth = Value.w, .m_EllipsisAtEnd = true});
}

void CMenus::StatsWeaponMatrix(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pHeading, const CMatchCombatStats &Stats)
{
	if(!Stats.HasData())
		return;
	// Weapons are columns and the numbers are rows, which is the only layout
	// that stays readable when a mod reports more weapons than the game knows.
	std::vector<const CMatchWeaponStats *> vpColumns;
	vpColumns.push_back(&Stats.m_Total);
	for(const CMatchWeaponStats &WeaponStats : Stats.m_vWeapons)
		if(WeaponStats.HasData())
			vpColumns.push_back(&WeaponStats);

	StatsHeading(pScrollRegion, pContent, pHeading);

	CUIRect Header;
	pContent->HSplitTop(24.0f, &Header, pContent);
	const float ColumnWidth = (Header.w - STATS_LABEL_WIDTH) / vpColumns.size();
	if(pScrollRegion->AddRect(Header))
	{
		Header.Draw(COLOR_TABLE_HEADER, IGraphics::CORNER_T, 4.0f);
		CUIRect Cell;
		Header.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Header);
		Cell.VMargin(6.0f, &Cell);
		SLabelProperties Properties;
		Properties.m_MaxWidth = Cell.w;
		Properties.m_EllipsisAtEnd = true;
		Properties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
		Ui()->DoLabel(&Cell, Localize("Weapon"), 10.0f, TEXTALIGN_ML, Properties);
		for(size_t Column = 0; Column < vpColumns.size(); ++Column)
		{
			Header.VSplitLeft(ColumnWidth, &Cell, &Header);
			const int Weapon = Column == 0 ? -1 : vpColumns[Column]->m_Weapon;
			// The icon says which weapon it is at a glance; a weapon without a
			// sprite falls back to its name so the column is not anonymous.
			if(Weapon >= 0 && Weapon < NUM_WEAPONS && GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon].IsValid())
			{
				const CDataWeaponspec &WeaponSpec = g_pData->m_Weapons.m_aId[Weapon];
				float ScaleX;
				float ScaleY;
				Graphics()->GetSpriteScale(WeaponSpec.m_pSpriteBody, ScaleX, ScaleY);
				const float Width = WeaponSpec.m_VisualSize * ScaleX;
				const float Height = WeaponSpec.m_VisualSize * ScaleY;
				const float Scale = std::min((Cell.w - 6.0f) / Width, (Cell.h - 6.0f) / Height);
				Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon]);
				Graphics()->QuadsBegin();
				Graphics()->DrawSprite(Cell.x + Cell.w / 2.0f, Cell.y + Cell.h / 2.0f, Width * Scale, Height * Scale);
				Graphics()->QuadsEnd();
			}
			else
			{
				const std::string Name = Column == 0 ? Localize("Total") : MatchWeaponDisplayName(Weapon);
				Properties.m_MaxWidth = Cell.w - 4.0f;
				Ui()->DoLabel(&Cell, Name.c_str(), 10.0f, TEXTALIGN_MC, Properties);
			}
		}
	}

	const auto MatrixRow = [&](const char *pLabel, const std::function<int64_t(const CMatchWeaponStats &)> &Value, const std::function<void(const CMatchWeaponStats &, char *, int)> &Format, bool AccuracyBar) {
		// Nothing reported it, so there is nothing to say about it. A table of
		// dashes reads as missing data even when the data was never possible.
		if(std::none_of(vpColumns.begin(), vpColumns.end(), [&](const CMatchWeaponStats *pWeaponStats) { return Value(*pWeaponStats) != 0; }))
			return;
		CUIRect Row;
		pContent->HSplitTop(20.0f, &Row, pContent);
		if(!pScrollRegion->AddRect(Row))
			return;
		Row.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.14f), IGraphics::CORNER_NONE, 0.0f);
		CUIRect Cell;
		Row.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Row);
		Cell.VMargin(6.0f, &Cell);
		SLabelProperties LabelProperties;
		LabelProperties.m_MaxWidth = Cell.w;
		LabelProperties.m_EllipsisAtEnd = true;
		LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.65f));
		Ui()->DoLabel(&Cell, pLabel, 11.5f, TEXTALIGN_ML, LabelProperties);
		for(const CMatchWeaponStats *pWeaponStats : vpColumns)
		{
			Row.VSplitLeft(ColumnWidth, &Cell, &Row);
			if(AccuracyBar && pWeaponStats->m_Shots > 0)
			{
				CUIRect Bar = Cell;
				Bar.HMargin(4.0f, &Bar);
				Bar.VMargin(4.0f, &Bar);
				Bar.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_ALL, 3.0f);
				Bar.w *= std::clamp(static_cast<float>(pWeaponStats->m_Hits) / static_cast<float>(pWeaponStats->m_Shots), 0.0f, 1.0f);
				Bar.Draw(ColorRGBA(COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, 0.45f), IGraphics::CORNER_ALL, 3.0f);
			}
			char aValue[32];
			Format(*pWeaponStats, aValue, sizeof(aValue));
			Cell.VMargin(6.0f, &Cell);
			Ui()->DoLabel(&Cell, aValue, 11.5f, TEXTALIGN_MC, {.m_MaxWidth = Cell.w, .m_EllipsisAtEnd = true});
		}
	};

	MatrixRow(
		Localize("Kills"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Kills; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_Kills); }, false);
	MatrixRow(
		Localize("Deaths"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Deaths; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_Deaths); }, false);
	MatrixRow(
		Localize("Shots"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Shots; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_Shots); }, false);
	MatrixRow(
		Localize("Hits"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Hits; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_Hits); }, false);
	MatrixRow(
		// Shots without hits cannot say how many of them landed, so the row
		// waits for whoever reports the hits.
		Localize("Accuracy"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Hits; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { FormatMatchAccuracy(WeaponStats.m_Hits, WeaponStats.m_Shots, pBuffer, Size); }, true);
	MatrixRow(
		Localize("Damage done"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_DamageDone; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_DamageDone); }, false);
	MatrixRow(
		Localize("Damage taken"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_DamageTaken; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_DamageTaken); }, false);
}

void CMenus::RenderStatsMatchList(CUIRect View)
{
	CUIRect Filters, List;
	View.HSplitTop(26.0f, &Filters, &View);
	View.HSplitTop(4.0f, nullptr, &List);
	CUIRect Search, Quality, Mode;
	Filters.VSplitRight(150.0f, &Search, &Quality);
	Search.VSplitRight(6.0f, &Search, nullptr);
	Search.VSplitRight(140.0f, &Search, &Mode);
	Search.VSplitRight(6.0f, &Search, nullptr);
	StatsModeDropDown(Mode);
	if(Ui()->DoEditBox_Search(&m_StatsHistorySearchInput, &Search, 12.0f, !Ui()->IsPopupOpen() && !GameClient()->m_GameConsole.IsActive()))
	{
		m_StatsSelectedIndex = -1;
		RefreshStats();
	}
	static const char *s_apQualityNames[3];
	s_apQualityNames[0] = Localize("All reports");
	s_apQualityNames[1] = Localize("Complete only");
	s_apQualityNames[2] = Localize("Server reports");
	static CUi::SDropDownState s_QualityDropDownState;
	static CScrollRegion s_QualityDropDownScrollRegion;
	s_QualityDropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_QualityDropDownScrollRegion;
	const int NewQuality = Ui()->DoDropDown(&Quality, static_cast<int>(m_StatsQualityFilter), s_apQualityNames, 3, s_QualityDropDownState);
	if(NewQuality != static_cast<int>(m_StatsQualityFilter))
	{
		m_StatsQualityFilter = static_cast<EStatsQualityFilter>(NewQuality);
		m_StatsSelectedIndex = -1;
		RefreshStats();
	}

	List.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.15f), IGraphics::CORNER_ALL, 5.0f);
	static CListBox s_ListBox;
	s_ListBox.DoStart(34.0f, m_vStatsHistory.size(), 1, 3, m_StatsSelectedIndex, &List, false, IGraphics::CORNER_ALL, true);
	for(int Index = 0; Index < static_cast<int>(m_vStatsHistory.size()); ++Index)
	{
		const CMatchHistoryEntry &Entry = m_vStatsHistory[Index];
		const CListboxItem Item = s_ListBox.DoNextItem(&Entry, Index == m_StatsSelectedIndex);
		if(!Item.m_Visible)
			continue;

		// An accent stripe carries the result, so the list can be scanned for
		// wins and losses without reading a single word.
		CUIRect Row = Item.m_Rect;
		CUIRect Accent;
		Row.VSplitLeft(3.0f, &Accent, &Row);
		Accent.HMargin(3.0f, &Accent);
		Accent.Draw(OutcomeColor(Entry.m_LocalOutcome), IGraphics::CORNER_ALL, 1.5f);
		Row.VSplitLeft(7.0f, nullptr, &Row);

		CUIRect Result, Score;
		Row.VSplitRight(90.0f, &Row, &Result);
		Row.VSplitRight(56.0f, &Row, &Score);
		CUIRect Title, Detail;
		Row.HMargin(3.0f, &Row);
		Row.HSplitTop(16.0f, &Title, &Detail);

		char aTitle[192];
		str_format(aTitle, sizeof(aTitle), "%s  ·  %s", Entry.m_MapName.c_str(), ShortModeName(Entry.m_ModeId).c_str());
		Ui()->DoLabel(&Title, aTitle, 12.5f, TEXTALIGN_ML, {.m_MaxWidth = Title.w, .m_EllipsisAtEnd = true});

		char aDate[64];
		FormatMatchTimestamp(Entry.m_EndTimeUtc, aDate, sizeof(aDate));
		char aDuration[64];
		FormatMatchDuration(Entry.m_DurationTicks, Entry.m_TickRate, aDuration, sizeof(aDuration));
		char aDetail[320];
		str_format(aDetail, sizeof(aDetail), "%s  ·  %s  ·  %s", aDate, aDuration, Entry.m_OriginId.c_str());
		SLabelProperties DetailProperties;
		DetailProperties.m_MaxWidth = Detail.w;
		DetailProperties.m_EllipsisAtEnd = true;
		DetailProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
		Ui()->DoLabel(&Detail, aDetail, 9.5f, TEXTALIGN_ML, DetailProperties);

		Ui()->DoLabel(&Score, Entry.m_LocalScore.has_value() ? std::to_string(*Entry.m_LocalScore).c_str() : "-", 16.0f, TEXTALIGN_MC);
		SLabelProperties ResultProperties;
		ResultProperties.m_MaxWidth = Result.w;
		ResultProperties.m_EllipsisAtEnd = true;
		ResultProperties.SetColor(OutcomeColor(Entry.m_LocalOutcome));
		Ui()->DoLabel(&Result, OutcomeName(Entry.m_LocalOutcome), 12.0f, TEXTALIGN_MC, ResultProperties);
	}
	const int NewSelectedIndex = s_ListBox.DoEnd();
	if(NewSelectedIndex != m_StatsSelectedIndex)
	{
		m_StatsSelectedIndex = NewSelectedIndex;
		LoadSelectedStatsMatch();
	}
	if(s_ListBox.WasItemActivated() && m_StatsSelectedMatch.has_value())
		m_StatsShowMatch = true;
	if(m_vStatsHistory.empty())
	{
		CUIRect EmptyHint;
		List.HMargin(List.h / 2.0f - 10.0f, &EmptyHint);
		const bool Filtered = m_StatsHistorySearchInput.GetString()[0] != '\0' || m_StatsQualityFilter != EStatsQualityFilter::ALL;
		Ui()->DoLabel(&EmptyHint, Filtered ? Localize("No match matches the current filter") : Localize("No matches have been recorded yet"), 14.0f, TEXTALIGN_MC);
	}
}

void CMenus::RenderStatsMatchSummary(CUIRect View)
{
	static CScrollRegion s_ScrollRegion;
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 40.0f;
	s_ScrollRegion.Begin(&View, &ScrollParams);
	if(!m_StatsSelectedMatch.has_value())
	{
		CUIRect Hint;
		View.HSplitTop(40.0f, &Hint, &View);
		if(s_ScrollRegion.AddRect(Hint))
			Ui()->DoLabel(&Hint, Localize("Select a match in the list to see its report"), 14.0f, TEXTALIGN_MC);
		s_ScrollRegion.End();
		return;
	}

	const CStoredMatch &Stored = *m_StatsSelectedMatch;
	const CMatchReport &Report = Stored.m_Report;
	const CMatchParticipant *pLocal = nullptr;
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
		if(Stored.m_LocalParticipantId.has_value() && Participant.m_ParticipantId == *Stored.m_LocalParticipantId)
			pLocal = &Participant;
	std::optional<EMatchOutcome> LocalOutcome;
	if(pLocal != nullptr)
		if(const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, pLocal->m_ParticipantId))
			LocalOutcome = pStanding->m_Outcome;

	// The banner answers "how did it go" in one glance, everything below it
	// answers "why".
	CUIRect Banner, Outcome, Subtitle;
	View.HSplitTop(58.0f, &Banner, &View);
	if(s_ScrollRegion.AddRect(Banner))
	{
		Banner.Draw(COLOR_PANEL, IGraphics::CORNER_ALL, 5.0f);
		Banner.Margin(6.0f, &Banner);
		Banner.HSplitTop(28.0f, &Outcome, &Subtitle);
		SLabelProperties OutcomeProperties;
		OutcomeProperties.SetColor(OutcomeColor(LocalOutcome));
		Ui()->DoLabel(&Outcome, LocalOutcome.has_value() ? MatchOutcomeDisplayName(*LocalOutcome) : Localize("Match report"), 24.0f, TEXTALIGN_MC, OutcomeProperties);
		char aDate[64];
		FormatMatchTimestamp(Report.m_EndTimeUtc, aDate, sizeof(aDate));
		char aDuration[64];
		FormatMatchDuration(Report.m_DurationTicks, Report.m_TickRate, aDuration, sizeof(aDuration));
		char aSubtitle[320];
		str_format(aSubtitle, sizeof(aSubtitle), "%s  ·  %s  ·  %s  ·  %s", Report.m_MapName.c_str(), ShortModeName(Report.m_ModeId).c_str(), aDuration, aDate);
		SLabelProperties SubtitleProperties;
		SubtitleProperties.m_MaxWidth = Subtitle.w;
		SubtitleProperties.m_EllipsisAtEnd = true;
		SubtitleProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
		Ui()->DoLabel(&Subtitle, aSubtitle, 11.0f, TEXTALIGN_MC, SubtitleProperties);
	}

	if(pLocal != nullptr)
	{
		const CMatchCombatStats Combat = BuildMatchCombatStats(Report, pLocal->m_ParticipantId);
		const int64_t Kills = ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, pLocal->m_ParticipantId, "kills").value_or(0);
		const int64_t Deaths = ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, pLocal->m_ParticipantId, "deaths").value_or(0);
		char aScore[32] = "-";
		if(const std::optional<int64_t> Score = ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, pLocal->m_ParticipantId, "score"))
			str_format(aScore, sizeof(aScore), "%" PRId64, *Score);
		char aRank[32] = "-";
		if(const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, pLocal->m_ParticipantId))
			str_format(aRank, sizeof(aRank), "#%d", pStanding->m_Rank);
		char aKd[32];
		FormatRatio(Kills, Deaths, aKd, sizeof(aKd));
		char aAccuracy[32];
		FormatMatchAccuracy(Combat.m_Total.m_Hits, Combat.m_Total.m_Shots, aAccuracy, sizeof(aAccuracy));
		char aKills[32];
		str_format(aKills, sizeof(aKills), "%" PRId64 " / %" PRId64, Kills, Deaths);
		const char *apLabels[5] = {Localize("Rank"), Localize("Score"), Localize("Kills / deaths"), Localize("K/D"), Localize("Accuracy")};
		const char *apValues[5] = {aRank, aScore, aKills, aKd, aAccuracy};
		StatsTiles(&s_ScrollRegion, &View, apLabels, apValues, 5);
	}

	if(pLocal != nullptr)
	{
		StatsWeaponMatrix(&s_ScrollRegion, &View, Localize("Weapons"), BuildMatchCombatStats(Report, pLocal->m_ParticipantId));
	}
	else
	{
		// Watching a report of a match nobody here played gives every
		// participant a table, so each one says whose it is.
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
		{
			char aHeading[MatchReportLimits::MAX_DISPLAY_NAME_LENGTH + 32];
			str_format(aHeading, sizeof(aHeading), "%s — %s", Localize("Weapons"), Participant.m_DisplayName.c_str());
			StatsWeaponMatrix(&s_ScrollRegion, &View, aHeading, BuildMatchCombatStats(Report, Participant.m_ParticipantId));
		}
	}

	StatsHeading(&s_ScrollRegion, &View, Localize("Participants"));
	const auto RenderParticipantRow = [&](const char *pRank, const char *pName, const char *pClan, const char *pScore, const char *pOutcome, bool Header, bool Local) {
		CUIRect Row;
		View.HSplitTop(20.0f, &Row, &View);
		if(!s_ScrollRegion.AddRect(Row))
			return;
		if(Header)
			Row.Draw(COLOR_TABLE_HEADER, IGraphics::CORNER_T, 4.0f);
		else if(Local)
			Row.Draw(ColorRGBA(COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, 0.18f), IGraphics::CORNER_NONE, 0.0f);
		static const float s_aWidths[4] = {0.07f, 0.36f, 0.22f, 0.13f};
		const char *apValues[5] = {pRank, pName, pClan, pScore, pOutcome};
		const float TotalWidth = Row.w;
		for(int Column = 0; Column < 5; ++Column)
		{
			CUIRect Cell;
			if(Column < 4)
				Row.VSplitLeft(TotalWidth * s_aWidths[Column], &Cell, &Row);
			else
				Cell = Row;
			Cell.VMargin(4.0f, &Cell);
			SLabelProperties CellProperties;
			CellProperties.m_MaxWidth = Cell.w;
			CellProperties.m_EllipsisAtEnd = true;
			if(Header)
				CellProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
			Ui()->DoLabel(&Cell, apValues[Column], Header ? 10.0f : 12.0f, Column == 0 || Column >= 3 ? TEXTALIGN_MC : TEXTALIGN_ML, CellProperties);
		}
	};
	// A row that names a team and carries the team's own rank, score and
	// result, so a team mode reads as its teams and not as one long list with
	// the team numbers repeated somewhere below it.
	const auto RenderTeamRow = [&](const CMatchTeam *pTeam) {
		CUIRect Row;
		View.HSplitTop(20.0f, &Row, &View);
		if(!s_ScrollRegion.AddRect(Row))
			return;
		Row.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.10f), IGraphics::CORNER_NONE, 0.0f);
		char aRight[96] = "";
		if(pTeam != nullptr)
		{
			std::optional<int64_t> Score = ReportMetric(Report, EMatchSubjectKind::TEAM, pTeam->m_TeamId, "score");
			if(!Score.has_value())
			{
				// A mode that only scores its players still has a team score,
				// it just never wrote it down.
				int64_t Sum = 0;
				bool Any = false;
				for(const CMatchParticipant &Participant : Report.m_vParticipants)
				{
					if(Participant.m_TeamId != pTeam->m_TeamId)
						continue;
					if(const std::optional<int64_t> Value = ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId, "score"))
					{
						Sum += *Value;
						Any = true;
					}
				}
				if(Any)
					Score = Sum;
			}
			const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::TEAM, pTeam->m_TeamId);
			if(Score.has_value() && pStanding != nullptr)
				str_format(aRight, sizeof(aRight), "%" PRId64 "  ·  #%d %s", *Score, pStanding->m_Rank, MatchOutcomeDisplayName(pStanding->m_Outcome));
			else if(Score.has_value())
				str_format(aRight, sizeof(aRight), "%" PRId64, *Score);
			else if(pStanding != nullptr)
				str_format(aRight, sizeof(aRight), "#%d %s", pStanding->m_Rank, MatchOutcomeDisplayName(pStanding->m_Outcome));
		}
		CUIRect Name, Right;
		Row.VSplitRight(Row.w * 0.35f, &Name, &Right);
		Name.VMargin(4.0f, &Name);
		Right.VMargin(4.0f, &Right);
		SLabelProperties NameProperties;
		NameProperties.m_MaxWidth = Name.w;
		NameProperties.m_EllipsisAtEnd = true;
		char aName[MatchReportLimits::MAX_DISPLAY_NAME_LENGTH + 16];
		if(pTeam == nullptr)
			str_copy(aName, Localize("Without a team"));
		else if(pTeam->m_DisplayName.empty())
			str_format(aName, sizeof(aName), "%s %d", Localize("Team"), pTeam->m_TeamId);
		else
			str_copy(aName, pTeam->m_DisplayName.c_str());
		Ui()->DoLabel(&Name, aName, 12.0f, TEXTALIGN_ML, NameProperties);
		SLabelProperties RightProperties;
		RightProperties.m_MaxWidth = Right.w;
		RightProperties.m_EllipsisAtEnd = true;
		Ui()->DoLabel(&Right, aRight, 12.0f, TEXTALIGN_MR, RightProperties);
	};

	RenderParticipantRow(Localize("#"), Localize("Name"), Localize("Clan"), Localize("Score"), Localize("Result"), true, false);
	// A table with a rank column is expected to be ordered by it, the report
	// itself keeps the order in which participants joined.
	std::vector<const CMatchParticipant *> vpRanked;
	vpRanked.reserve(Report.m_vParticipants.size());
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
		vpRanked.push_back(&Participant);
	const auto RankOf = [&](EMatchSubjectKind Kind, int SubjectId) {
		const CMatchStanding *pStanding = ReportStanding(Report, Kind, SubjectId);
		// Whatever has no standing is listed last.
		return pStanding == nullptr ? std::numeric_limits<int>::max() : pStanding->m_Rank;
	};
	std::stable_sort(vpRanked.begin(), vpRanked.end(), [&](const CMatchParticipant *pLeft, const CMatchParticipant *pRight) {
		return RankOf(EMatchSubjectKind::PARTICIPANT, pLeft->m_ParticipantId) < RankOf(EMatchSubjectKind::PARTICIPANT, pRight->m_ParticipantId);
	});
	const auto RenderParticipants = [&](const std::optional<int> &TeamId) {
		for(const CMatchParticipant *pParticipant : vpRanked)
		{
			if(pParticipant->m_TeamId != TeamId)
				continue;
			const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, pParticipant->m_ParticipantId);
			char aRank[16] = "-";
			const char *pOutcome = "-";
			if(pStanding != nullptr)
			{
				str_format(aRank, sizeof(aRank), "%d", pStanding->m_Rank);
				pOutcome = MatchOutcomeDisplayName(pStanding->m_Outcome);
			}
			char aScore[32] = "-";
			if(const std::optional<int64_t> Score = ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, pParticipant->m_ParticipantId, "score"))
				str_format(aScore, sizeof(aScore), "%" PRId64, *Score);
			std::string Name = pParticipant->m_DisplayName;
			if(pParticipant->m_LeftTick.has_value())
				Name += " (" + std::string(Localize("left")) + ")";
			const bool Local = Stored.m_LocalParticipantId.has_value() && *Stored.m_LocalParticipantId == pParticipant->m_ParticipantId;
			RenderParticipantRow(aRank, Name.c_str(), pParticipant->m_Clan.c_str(), aScore, pOutcome, false, Local);
		}
	};
	if(Report.m_vTeams.empty())
	{
		RenderParticipants(std::nullopt);
	}
	else
	{
		std::vector<const CMatchTeam *> vpTeams;
		vpTeams.reserve(Report.m_vTeams.size());
		for(const CMatchTeam &Team : Report.m_vTeams)
			vpTeams.push_back(&Team);
		std::stable_sort(vpTeams.begin(), vpTeams.end(), [&](const CMatchTeam *pLeft, const CMatchTeam *pRight) {
			return RankOf(EMatchSubjectKind::TEAM, pLeft->m_TeamId) < RankOf(EMatchSubjectKind::TEAM, pRight->m_TeamId);
		});
		for(const CMatchTeam *pTeam : vpTeams)
		{
			RenderTeamRow(pTeam);
			RenderParticipants(pTeam->m_TeamId);
		}
		const bool AnyWithoutTeam = std::any_of(vpRanked.begin(), vpRanked.end(), [](const CMatchParticipant *pParticipant) { return !pParticipant->m_TeamId.has_value(); });
		if(AnyWithoutTeam)
		{
			RenderTeamRow(nullptr);
			RenderParticipants(std::nullopt);
		}
	}

	for(const CMatchStanding &Standing : Report.m_vStandings)
	{
		// Team and participant standings are shown where they belong, on the
		// rows above. What is left is the match itself, which a mode uses to
		// say how the match as a whole ended.
		if(Standing.m_SubjectKind != EMatchSubjectKind::MATCH)
			continue;
		char aValue[64];
		str_format(aValue, sizeof(aValue), "%d — %s", Standing.m_Rank, MatchOutcomeDisplayName(Standing.m_Outcome));
		StatsMetricLine(&s_ScrollRegion, &View, Localize("Match result"), aValue);
	}

	// The banner, the tiles and the weapon table have already said most of
	// this. What is left is what the mode reported on top of them, and only
	// where it reported something: a line reading zero is a line nobody needs.
	const auto IsAlreadyShown = [&](const std::string &MetricId) {
		const std::string_view Suffix = MatchMetricSuffix(MetricId);
		return Suffix == "score" || Suffix == "kills" || Suffix == "deaths" || Suffix == "playtime_ticks";
	};
	const auto WorthALine = [&](const CMatchMetric &Metric, EMatchMetricCategory Category) {
		return Metric.m_Value != 0 &&
		       !IsMatchCombatStatMetric(Metric.m_MetricId, Report.m_ModeSchemaVersion) &&
		       !IsAlreadyShown(Metric.m_MetricId) &&
		       MatchMetricCategory(Metric.m_MetricId, Report.m_ModeSchemaVersion) == Category;
	};
	for(const EMatchMetricCategory Category : {EMatchMetricCategory::OVERVIEW, EMatchMetricCategory::COMBAT, EMatchMetricCategory::OBJECTIVES, EMatchMetricCategory::OTHER})
	{
		if(std::none_of(Report.m_vMetrics.begin(), Report.m_vMetrics.end(), [&](const CMatchMetric &Metric) { return WorthALine(Metric, Category); }))
			continue;
		StatsHeading(&s_ScrollRegion, &View, MatchMetricCategoryDisplayName(Category));
		for(const CMatchMetric &Metric : Report.m_vMetrics)
			if(WorthALine(Metric, Category))
			{
				char aValue[64];
				FormatMatchMetricValue(Metric, Report.m_ModeSchemaVersion, Report.m_TickRate, aValue, sizeof(aValue));
				StatsMetricLine(&s_ScrollRegion, &View, MatchMetricDisplayName(Metric.m_MetricId, Report.m_ModeSchemaVersion).c_str(), aValue);
			}
	}

	// The bookkeeping fields belong to the report, not to the match, so they
	// end up as one dim line at the bottom instead of four rows at the top.
	CUIRect Footer;
	View.HSplitTop(10.0f, nullptr, &View);
	View.HSplitTop(16.0f, &Footer, &View);
	if(s_ScrollRegion.AddRect(Footer))
	{
		char aFooter[512];
		str_format(aFooter, sizeof(aFooter), "%s  ·  %s  ·  %s  ·  %s", Stored.m_OriginId.c_str(), MatchReportSourceDisplayName(Stored.m_Source), MatchCompletenessDisplayName(Stored.m_Completeness), MatchTerminationDisplayName(Report.m_Termination));
		SLabelProperties FooterProperties;
		FooterProperties.m_MaxWidth = Footer.w;
		FooterProperties.m_EllipsisAtEnd = true;
		FooterProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.45f));
		Ui()->DoLabel(&Footer, aFooter, 10.0f, TEXTALIGN_ML, FooterProperties);
	}
	s_ScrollRegion.End();
}

const char *CMenus::StatsPeriodName(EStatsPeriod Period)
{
	switch(Period)
	{
	case EStatsPeriod::DAY: return Localize("Day");
	case EStatsPeriod::WEEK: return Localize("Week");
	case EStatsPeriod::MONTH: return Localize("Month");
	case EStatsPeriod::ALL_TIME: return Localize("All time");
	default: dbg_assert(false, "invalid statistics period"); return "";
	}
}

void CMenus::StatsPeriodHeader(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pLabel)
{
	CUIRect Row;
	pContent->HSplitTop(18.0f, &Row, pContent);
	if(!pScrollRegion->AddRect(Row))
		return;
	Row.Draw(COLOR_TABLE_HEADER, IGraphics::CORNER_T, 4.0f);
	SLabelProperties Properties;
	Properties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
	CUIRect Cell;
	Row.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Row);
	Cell.VMargin(6.0f, &Cell);
	Properties.m_MaxWidth = Cell.w;
	Properties.m_EllipsisAtEnd = true;
	Ui()->DoLabel(&Cell, pLabel, 10.0f, TEXTALIGN_ML, Properties);
	const float ColumnWidth = Row.w / (int)EStatsPeriod::COUNT;
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
	{
		Row.VSplitLeft(ColumnWidth, &Cell, &Row);
		Cell.VMargin(6.0f, &Cell);
		Properties.m_MaxWidth = Cell.w;
		Ui()->DoLabel(&Cell, StatsPeriodName((EStatsPeriod)Period), 10.0f, TEXTALIGN_MR, Properties);
	}
}

void CMenus::StatsPeriodRow(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pLabel, const char *const *ppValues)
{
	CUIRect Row;
	pContent->HSplitTop(18.0f, &Row, pContent);
	if(!pScrollRegion->AddRect(Row))
		return;
	CUIRect Cell;
	Row.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Row);
	Cell.VMargin(6.0f, &Cell);
	SLabelProperties LabelProperties;
	LabelProperties.m_MaxWidth = Cell.w;
	LabelProperties.m_EllipsisAtEnd = true;
	LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.65f));
	Ui()->DoLabel(&Cell, pLabel, 11.5f, TEXTALIGN_ML, LabelProperties);
	const float ColumnWidth = Row.w / (int)EStatsPeriod::COUNT;
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
	{
		Row.VSplitLeft(ColumnWidth, &Cell, &Row);
		Cell.VMargin(6.0f, &Cell);
		// The rightmost column is the complete record, the shorter periods sit
		// next to it for comparison and are dimmed so it stays the anchor.
		SLabelProperties ValueProperties;
		ValueProperties.m_MaxWidth = Cell.w;
		ValueProperties.m_EllipsisAtEnd = true;
		if(Period != (int)EStatsPeriod::ALL_TIME)
			ValueProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.7f));
		Ui()->DoLabel(&Cell, ppValues[Period], 11.5f, TEXTALIGN_MR, ValueProperties);
	}
}

void CMenus::RenderStatsProfile(CUIRect View)
{
	CUIRect FilterLine;
	View.HSplitTop(24.0f, &FilterLine, &View);
	View.HSplitTop(4.0f, nullptr, &View);
	CUIRect ModeDropDown, ScaleButtons;
	FilterLine.VSplitLeft(180.0f, &ModeDropDown, &ScaleButtons);
	StatsModeDropDown(ModeDropDown);
	// The same numbers read completely differently depending on how many matches
	// went into them, so they can be divided by matches or by time played.
	static CButtonContainer s_TotalButton;
	static CButtonContainer s_PerMatchButton;
	static CButtonContainer s_PerMinuteButton;
	const auto ScaleButton = [&](CButtonContainer &Button, const char *pText, EStatsScale Scale, int Corners) {
		CUIRect Rect;
		ScaleButtons.VSplitRight(90.0f, &ScaleButtons, &Rect);
		if(DoButton_Menu(&Button, pText, m_StatsScale == Scale, &Rect, BUTTONFLAG_LEFT, nullptr, Corners))
			m_StatsScale = Scale;
	};
	ScaleButton(s_PerMinuteButton, Localize("Per minute"), EStatsScale::PER_MINUTE, IGraphics::CORNER_R);
	ScaleButton(s_PerMatchButton, Localize("Per match"), EStatsScale::PER_MATCH, IGraphics::CORNER_NONE);
	ScaleButton(s_TotalButton, Localize("Total"), EStatsScale::TOTAL, IGraphics::CORNER_L);

	static CScrollRegion s_ScrollRegion;
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 40.0f;
	s_ScrollRegion.Begin(&View, &ScrollParams);

	const CMatchProfile &AllTime = m_aStatsProfiles[(int)EStatsPeriod::ALL_TIME];

	// Scaling divides by what the period actually contains, so a value only
	// means something if that period has matches at all.
	const auto FormatScaled = [this](int64_t Value, int Period, char *pBuffer, int BufferSize) {
		const CMatchProfile &Profile = m_aStatsProfiles[Period];
		double By = 1.0;
		if(m_StatsScale == EStatsScale::PER_MATCH)
			By = Profile.m_Matches;
		else if(m_StatsScale == EStatsScale::PER_MINUTE)
			By = Profile.m_PlaytimeSeconds / 60.0;
		if(By <= 0.0)
			str_copy(pBuffer, "-", BufferSize);
		else if(m_StatsScale == EStatsScale::TOTAL)
			str_format(pBuffer, BufferSize, "%" PRId64, Value);
		else
			str_format(pBuffer, BufferSize, "%.2f", Value / By);
	};

	char aaValues[(int)EStatsPeriod::COUNT][64];
	const char *apValues[(int)EStatsPeriod::COUNT];
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		apValues[Period] = aaValues[Period];
	const auto PeriodRow = [&](const char *pLabel) { StatsPeriodRow(&s_ScrollRegion, &View, pLabel, apValues); };

	StatsHeading(&s_ScrollRegion, &View, Localize("Record"));
	StatsPeriodHeader(&s_ScrollRegion, &View, Localize("Matches"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		str_format(aaValues[Period], sizeof(aaValues[Period]), "%d", m_aStatsProfiles[Period].m_Matches);
	PeriodRow(Localize("Played"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		str_format(aaValues[Period], sizeof(aaValues[Period]), "%d / %d / %d", m_aStatsProfiles[Period].m_Wins, m_aStatsProfiles[Period].m_Draws, m_aStatsProfiles[Period].m_Losses);
	PeriodRow(Localize("Win / draw / loss"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
	{
		const CMatchProfile &Profile = m_aStatsProfiles[Period];
		if(Profile.m_Matches > 0)
			str_format(aaValues[Period], sizeof(aaValues[Period]), "%.0f%%", 100.0f * Profile.m_Wins / Profile.m_Matches);
		else
			str_copy(aaValues[Period], "-");
	}
	PeriodRow(Localize("Win rate"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		FormatMatchSeconds(m_aStatsProfiles[Period].m_PlaytimeSeconds, aaValues[Period], sizeof(aaValues[Period]));
	PeriodRow(Localize("Playtime"));

	if(AllTime.m_Matches > 0)
	{
		CUIRect Bar;
		View.HSplitTop(6.0f, nullptr, &View);
		View.HSplitTop(10.0f, &Bar, &View);
		if(s_ScrollRegion.AddRect(Bar))
		{
			Bar.VMargin(3.0f, &Bar);
			Bar.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_ALL, 3.0f);
			const float TotalWidth = Bar.w;
			const int aCounts[3] = {AllTime.m_Wins, AllTime.m_Draws, AllTime.m_Losses};
			const ColorRGBA aColors[3] = {COLOR_WIN, COLOR_NEUTRAL, COLOR_LOSS};
			for(int Part = 0; Part < 3; ++Part)
			{
				CUIRect Segment;
				Bar.VSplitLeft(TotalWidth * static_cast<float>(aCounts[Part]) / static_cast<float>(AllTime.m_Matches), &Segment, &Bar);
				Segment.Draw(ColorRGBA(aColors[Part].r, aColors[Part].g, aColors[Part].b, 0.6f), IGraphics::CORNER_ALL, 3.0f);
			}
		}
	}

	// The weapon numbers are a table of weapons against numbers already, so a
	// third dimension of periods does not fit into it. It shows one period,
	// picked here and named in the heading.
	CUIRect PeriodButtons;
	View.HSplitTop(8.0f, nullptr, &View);
	View.HSplitTop(20.0f, &PeriodButtons, &View);
	if(s_ScrollRegion.AddRect(PeriodButtons))
	{
		static CButtonContainer s_aWeaponPeriodButtons[(int)EStatsPeriod::COUNT];
		const float ButtonWidth = std::min(90.0f, PeriodButtons.w / (int)EStatsPeriod::COUNT);
		for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		{
			CUIRect Button;
			PeriodButtons.VSplitLeft(ButtonWidth, &Button, &PeriodButtons);
			int Corners = IGraphics::CORNER_NONE;
			if(Period == 0)
				Corners = IGraphics::CORNER_L;
			else if(Period == (int)EStatsPeriod::COUNT - 1)
				Corners = IGraphics::CORNER_R;
			if(DoButton_Menu(&s_aWeaponPeriodButtons[Period], StatsPeriodName((EStatsPeriod)Period), m_StatsWeaponPeriod == (EStatsPeriod)Period, &Button, BUTTONFLAG_LEFT, nullptr, Corners))
				m_StatsWeaponPeriod = (EStatsPeriod)Period;
		}
	}
	char aWeaponHeading[64];
	str_format(aWeaponHeading, sizeof(aWeaponHeading), "%s — %s", Localize("Weapons"), StatsPeriodName(m_StatsWeaponPeriod));
	StatsWeaponMatrix(&s_ScrollRegion, &View, aWeaponHeading, BuildMatchCombatStats(m_aStatsProfiles[(int)m_StatsWeaponPeriod]));

	// Everything the reports carried that is not a weapon number. Which metrics
	// exist is decided by the reports, not by a list in here, so a mod's own
	// metrics appear next to the ones this build knows.
	if(StatsModeFilter()[0] == '\0')
	{
		// Every mode keeps its own metrics, and summing a race time onto a flag
		// capture would be nonsense, so without a gametype there is nothing
		// honest to put here.
		CUIRect Hint;
		View.HSplitTop(14.0f, nullptr, &View);
		View.HSplitTop(20.0f, &Hint, &View);
		if(s_ScrollRegion.AddRect(Hint))
		{
			SLabelProperties HintProperties;
			HintProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
			Ui()->DoLabel(&Hint, Localize("Pick a gametype to see what it counts"), 11.0f, TEXTALIGN_MC, HintProperties);
		}
		s_ScrollRegion.End();
		return;
	}
	for(const EMatchMetricCategory Category : {EMatchMetricCategory::OVERVIEW, EMatchMetricCategory::COMBAT, EMatchMetricCategory::OBJECTIVES, EMatchMetricCategory::OTHER})
	{
		bool WroteHeading = false;
		for(const CMatchMetricAggregate &Metric : AllTime.m_vMetrics)
		{
			if(Metric.m_Value == 0 || IsMatchCombatStatMetric(Metric.m_MetricId, Metric.m_ModeSchemaVersion) || MatchMetricCategory(Metric.m_MetricId, Metric.m_ModeSchemaVersion) != Category)
				continue;
			const std::string Label = MatchMetricDisplayName(Metric.m_MetricId, Metric.m_ModeSchemaVersion);
			for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
			{
				const CMatchProfile &Profile = m_aStatsProfiles[Period];
				const auto It = std::find_if(Profile.m_vMetrics.begin(), Profile.m_vMetrics.end(), [&](const CMatchMetricAggregate &Other) {
					return Other.m_MetricId == Metric.m_MetricId && Other.m_ModeId == Metric.m_ModeId && Other.m_ModeSchemaVersion == Metric.m_ModeSchemaVersion;
				});
				if(It == Profile.m_vMetrics.end())
					str_copy(aaValues[Period], "-");
				else if(Metric.m_Aggregation == EMatchMetricAggregation::SUM)
					FormatScaled(It->m_Value, Period, aaValues[Period], sizeof(aaValues[Period]));
				else
					// A best or a rank cannot be divided by anything and stay true
					str_format(aaValues[Period], sizeof(aaValues[Period]), "%" PRId64, It->m_Value);
			}
			if(!WroteHeading)
			{
				StatsHeading(&s_ScrollRegion, &View, MatchMetricCategoryDisplayName(Category));
				StatsPeriodHeader(&s_ScrollRegion, &View, Localize("Metric"));
				WroteHeading = true;
			}
			PeriodRow(Label.c_str());
		}
	}
	s_ScrollRegion.End();
}

void CMenus::RenderStats(CUIRect MainView)
{
	GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_DEMOS);
	if(!m_StatsInitialized)
		RefreshStats();

	MainView.Draw(ms_ColorTabbarActive, IGraphics::CORNER_B, 10.0f);
	MainView.Margin(10.0f, &MainView);
	CUIRect Tabs, Content, Buttons, Status;
	MainView.HSplitTop(26.0f, &Tabs, &Content);
	Content.HSplitBottom(51.0f, &Content, &Buttons);
	Buttons.HSplitBottom(24.0f, &Buttons, &Status);
	Content.Margin(5.0f, &Content);

	CUIRect Tab;
	if(m_StatsShowMatch)
	{
		// The report replaces the list, so the only navigation it needs is back.
		Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
		static CButtonContainer s_BackButton;
		if(DoButton_Menu(&s_BackButton, Localize("Back"), 0, &Tab) || Ui()->ConsumeHotkey(CUi::HOTKEY_ESCAPE))
			m_StatsShowMatch = false;
		if(m_StatsSelectedMatch.has_value())
			Ui()->DoLabel(&Tabs, m_StatsSelectedMatch->m_Report.m_MapName.c_str(), 14.0f, TEXTALIGN_MR);
		RenderStatsMatchSummary(Content);
	}
	else
	{
		static CButtonContainer s_MatchesTab;
		static CButtonContainer s_ProfileTab;
		Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
		if(DoButton_Menu(&s_MatchesTab, Localize("Matches"), m_StatsTab == EStatsTab::MATCHES, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_L))
			m_StatsTab = EStatsTab::MATCHES;
		Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
		if(DoButton_Menu(&s_ProfileTab, Localize("Profile"), m_StatsTab == EStatsTab::PROFILE, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_R))
			m_StatsTab = EStatsTab::PROFILE;
		SLabelProperties HintProperties;
		HintProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
		Ui()->DoLabel(&Tabs, Localize("Stored only on this device"), 10.0f, TEXTALIGN_MR, HintProperties);
		if(m_StatsTab == EStatsTab::MATCHES)
			RenderStatsMatchList(Content);
		else
			RenderStatsProfile(Content);
	}

	Buttons.HMargin(2.0f, &Buttons);
	CUIRect Button;
	static CButtonContainer s_RefreshButton;
	Buttons.VSplitLeft(85.0f, &Button, &Buttons);
	if(DoButton_Menu(&s_RefreshButton, Localize("Refresh"), 0, &Button))
		RefreshStats();
	Buttons.VSplitLeft(5.0f, nullptr, &Buttons);
	if(!m_StatsShowMatch && m_StatsTab == EStatsTab::MATCHES && m_StatsSelectedMatch.has_value())
	{
		static CButtonContainer s_OpenButton;
		Buttons.VSplitLeft(95.0f, &Button, &Buttons);
		if(DoButton_Menu(&s_OpenButton, Localize("Open report"), 0, &Button))
			m_StatsShowMatch = true;
		Buttons.VSplitLeft(5.0f, nullptr, &Buttons);
	}
	if(m_StatsSelectedMatch.has_value() && m_StatsTab == EStatsTab::MATCHES)
	{
		static CButtonContainer s_JsonButton;
		static CButtonContainer s_CsvButton;
		static CButtonContainer s_DeleteButton;
		Buttons.VSplitLeft(95.0f, &Button, &Buttons);
		if(DoButton_Menu(&s_JsonButton, Localize("Export JSON"), 0, &Button))
			ExportSelectedStats(false);
		Buttons.VSplitLeft(5.0f, nullptr, &Buttons);
		Buttons.VSplitLeft(95.0f, &Button, &Buttons);
		if(DoButton_Menu(&s_CsvButton, Localize("Export CSV"), 0, &Button))
			ExportSelectedStats(true);
		Buttons.VSplitLeft(5.0f, nullptr, &Buttons);
		Buttons.VSplitLeft(95.0f, &Button, &Buttons);
		if(DoButton_Menu(&s_DeleteButton, Localize("Delete match"), 0, &Button))
			PopupConfirm(Localize("Delete match"), Localize("Are you sure that you want to delete the selected match?"), Localize("Yes"), Localize("No"), &CMenus::PopupConfirmDeleteStatsMatch);
	}
	if(m_StatsTab == EStatsTab::PROFILE && !m_StatsShowMatch && m_StatsInfo.m_NumMatches > 0)
	{
		static CButtonContainer s_DeleteAllButton;
		Buttons.VSplitRight(105.0f, &Buttons, &Button);
		if(DoButton_Menu(&s_DeleteAllButton, Localize("Delete all"), 0, &Button))
			PopupConfirm(Localize("Delete statistics"), Localize("Delete all locally stored match statistics?"), Localize("Yes"), Localize("No"), &CMenus::PopupConfirmDeleteStatsPeriod);
	}

	char aStatus[256];
	if(!m_StatsError.empty())
		str_copy(aStatus, m_StatsError.c_str());
	else
	{
		char aOldest[64] = "-";
		if(m_StatsInfo.m_OldestMatchUtc.has_value())
			FormatMatchTimestamp(*m_StatsInfo.m_OldestMatchUtc, aOldest, sizeof(aOldest));
		str_format(aStatus, sizeof(aStatus), Localize("%d matches, %.1f MiB, oldest: %s"), m_StatsInfo.m_NumMatches, m_StatsInfo.m_DatabaseSize / (1024.0 * 1024.0), aOldest);
	}
	SLabelProperties StatusProperties;
	StatusProperties.m_MaxWidth = Status.w;
	StatusProperties.m_EllipsisAtEnd = true;
	StatusProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
	Ui()->DoLabel(&Status, aStatus, 10.0f, TEXTALIGN_ML, StatusProperties);
}
