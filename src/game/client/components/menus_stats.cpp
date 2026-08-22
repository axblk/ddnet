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
#include <limits>
#include <string>

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
	// How many weapons the weapon picker offers as buttons. A mode that reports
	// more of them keeps all of them in the matrix, which has no such bound.
	const int MAX_STATS_WEAPON_BUTTONS = 12;

	// The metrics the overall block already shows by name. They would otherwise
	// appear a second time in the list of what else a gametype counts.
	bool IsStatsOverallMetric(const std::string &MetricId)
	{
		static const char *const s_apSuffixes[] = {"/score", "/playtime_ticks", "/kills", "/assists", "/deaths", "/damage_done", "/damage_taken", "/shots", "/hits"};
		return std::any_of(std::begin(s_apSuffixes), std::end(s_apSuffixes), [&](const char *pSuffix) { return str_endswith(MetricId.c_str(), pSuffix) != nullptr; });
	}

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
	void ShortModeName(const std::string &ModeId, char *pBuffer, int BufferSize)
	{
		const size_t At = ModeId.find('@');
		str_truncate(pBuffer, BufferSize, ModeId.c_str(), static_cast<int>(At == std::string::npos ? ModeId.size() : At));
	}

	// The metric ids carry the mode they belong to, so a total over every mode
	// is a sum over everything that ends in the same suffix. Only what may be
	// summed is summed: a best or a rank is not a total of anything.
	int64_t SumMetricSuffix(const CMatchProfile &Profile, const char *pSuffix)
	{
		int64_t Total = 0;
		for(const CMatchMetricAggregate &Metric : Profile.m_vMetrics)
			if(Metric.m_Aggregation == EMatchMetricAggregation::SUM && str_endswith(Metric.m_MetricId.c_str(), pSuffix) != nullptr)
				Total += Metric.m_Value;
		return Total;
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
	m_StatsTab = EStatsTab::OVERVIEW;
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
	// The names of the gametypes are collected when the journal is read, only
	// the entry for all of them at once follows the current language.
	m_vpStatsModeNames[0] = Localize("All gametypes");
	static CUi::SDropDownState s_DropDownState;
	static CScrollRegion s_DropDownScrollRegion;
	s_DropDownState.m_SelectionPopupContext.m_pScrollRegion = &s_DropDownScrollRegion;
	const int Picked = Ui()->DoDropDown(&Rect, std::clamp(m_StatsModeIndex, 0, static_cast<int>(m_vpStatsModeNames.size()) - 1), m_vpStatsModeNames.data(), m_vpStatsModeNames.size(), s_DropDownState);
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
	m_vStatsModeSummaries.clear();
	m_StatsLastMatch.reset();
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
	// The gametype filter offers what the journal holds, and the overview says
	// what was played most and what was played last. All three read the whole
	// history, so they are collected before anything is thrown away.
	m_vStatsModes.clear();
	m_vStatsModeSummaries.clear();
	m_StatsLastMatch = m_vStatsHistory.empty() ? std::optional<CMatchHistoryEntry>() : m_vStatsHistory.front();
	for(const CMatchHistoryEntry &Entry : m_vStatsHistory)
	{
		if(std::find(m_vStatsModes.begin(), m_vStatsModes.end(), Entry.m_ModeId) == m_vStatsModes.end())
			m_vStatsModes.push_back(Entry.m_ModeId);
		auto Summary = std::find_if(m_vStatsModeSummaries.begin(), m_vStatsModeSummaries.end(), [&](const CStatsModeSummary &Other) { return Other.m_ModeId == Entry.m_ModeId; });
		if(Summary == m_vStatsModeSummaries.end())
		{
			Summary = m_vStatsModeSummaries.emplace(m_vStatsModeSummaries.end());
			Summary->m_ModeId = Entry.m_ModeId;
		}
		++Summary->m_Matches;
		if(Entry.m_LocalOutcome.has_value() && (*Entry.m_LocalOutcome == EMatchOutcome::WIN || *Entry.m_LocalOutcome == EMatchOutcome::FINISHED))
			++Summary->m_Wins;
		// How long the match ran, not how long the local player was in it: the
		// per-participant time needs the report, and this only ranks gametypes.
		if(Entry.m_TickRate > 0)
			Summary->m_PlaytimeSeconds += Entry.m_DurationTicks / Entry.m_TickRate;
	}
	std::sort(m_vStatsModes.begin(), m_vStatsModes.end());
	std::sort(m_vStatsModeSummaries.begin(), m_vStatsModeSummaries.end(), [](const CStatsModeSummary &Left, const CStatsModeSummary &Right) { return Left.m_Matches > Right.m_Matches; });
	// The drop down wants its labels as one array of pointers, and neither the
	// gametypes nor their names change again until the next refresh.
	m_vStatsModeNames.clear();
	m_vpStatsModeNames.assign(1, nullptr);
	char aModeName[MatchReportLimits::MAX_MODE_ID_LENGTH];
	for(const std::string &ModeId : m_vStatsModes)
	{
		ShortModeName(ModeId, aModeName, sizeof(aModeName));
		m_vStatsModeNames.emplace_back(aModeName);
	}
	for(const std::string &Name : m_vStatsModeNames)
		m_vpStatsModeNames.push_back(Name.c_str());
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
	static const int s_aPeriodDays[(int)EStatsPeriod::COUNT] = {7, 30, 0};
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
	{
		m_StatsSelectedMatch = std::move(Match);
		BuildMatchDetailView(*m_StatsSelectedMatch, m_StatsMatchView);
	}
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

void CMenus::StatsLabel(const CUIRect *pRect, const char *pText, float Size, int Align, const SLabelProperties &LabelProps)
{
	// Labels are drawn from a pool of cached text containers, so that a page
	// that is not changing does not lay its text out again on every frame. The
	// pool only grows to the number of labels visible at the same time, and a
	// label that only moved has its geometry shifted instead of rebuilt.
	SkipStatsLabels(1);
	Ui()->DoLabelStreamed(*m_vpStatsLabelUiElements[m_StatsLabelUiElementIndex - 1]->Rect(0), pRect, pText, Size, Align, LabelProps);
}

void CMenus::SkipStatsLabels(size_t Count)
{
	// A label that is scrolled out of view still consumes its slot. Without
	// that the whole pool shifts by one for every row that leaves the view, and
	// every cached text container behind the shift is laid out again on every
	// frame of the scroll.
	m_StatsLabelUiElementIndex += Count;
	while(m_vpStatsLabelUiElements.size() < m_StatsLabelUiElementIndex)
		m_vpStatsLabelUiElements.push_back(Ui()->GetNewUIElement(1));
}

void CMenus::SetStatsLabelSlot(size_t Slot)
{
	dbg_assert(Slot <= m_vpStatsLabelUiElements.size(), "statistics label slot was not reserved");
	m_StatsLabelUiElementIndex = Slot;
}

void CMenus::StatsHeading(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pText)
{
	CUIRect Line, Underline;
	pContent->HSplitTop(8.0f, nullptr, pContent);
	pContent->HSplitTop(22.0f, &Line, pContent);
	if(!pScrollRegion->AddRect(Line))
	{
		SkipStatsLabels(1);
		return;
	}
	Line.HSplitBottom(1.0f, &Line, &Underline);
	StatsLabel(&Line, pText, 14.0f, TEXTALIGN_ML, {});
	Underline.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.15f), IGraphics::CORNER_NONE, 0.0f);
}

void CMenus::StatsTiles(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *const *ppLabels, const char *const *ppValues, int Count)
{
	CUIRect Row;
	pContent->HSplitTop(6.0f, nullptr, pContent);
	pContent->HSplitTop(52.0f, &Row, pContent);
	if(!pScrollRegion->AddRect(Row))
	{
		SkipStatsLabels(2 * (size_t)Count);
		return;
	}
	for(int Tile = 0; Tile < Count; ++Tile)
	{
		CUIRect Box, Value, Label;
		Row.VSplitLeft(Row.w / (Count - Tile), &Box, &Row);
		Box.VMargin(3.0f, &Box);
		Box.Draw(COLOR_PANEL, IGraphics::CORNER_ALL, 4.0f);
		Box.Margin(5.0f, &Box);
		// The number is what the eye should land on, the label only names it.
		Box.HSplitTop(26.0f, &Value, &Label);
		StatsLabel(&Value, ppValues[Tile], 21.0f, TEXTALIGN_MC, {.m_MaxWidth = Value.w, .m_EllipsisAtEnd = true});
		SLabelProperties LabelProperties;
		LabelProperties.m_MaxWidth = Label.w;
		LabelProperties.m_EllipsisAtEnd = true;
		LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.55f));
		StatsLabel(&Label, ppLabels[Tile], 9.5f, TEXTALIGN_MC, LabelProperties);
	}
}

void CMenus::StatsMetricLine(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pLabel, const char *pValue)
{
	CUIRect Line, Label, Value;
	pContent->HSplitTop(19.0f, &Line, pContent);
	if(!pScrollRegion->AddRect(Line))
	{
		SkipStatsLabels(2);
		return;
	}
	Line.VSplitLeft(220.0f, &Label, &Value);
	SLabelProperties LabelProperties;
	LabelProperties.m_MaxWidth = Label.w;
	LabelProperties.m_EllipsisAtEnd = true;
	LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
	StatsLabel(&Label, pLabel, 11.5f, TEXTALIGN_ML, LabelProperties);
	StatsLabel(&Value, pValue, 11.5f, TEXTALIGN_ML, {.m_MaxWidth = Value.w, .m_EllipsisAtEnd = true});
}

int CMenus::StatsToggle(CButtonContainer *pButtons, CUIRect Rect, const char *const *ppNames, int Count, int Current)
{
	// One segmented control for every place that picks one of a few things, so
	// that a period toggle and a scale toggle cannot drift apart in look.
	const float ButtonWidth = std::min(90.0f, Rect.w / Count);
	Rect.VSplitRight(ButtonWidth * Count, nullptr, &Rect);
	for(int Index = 0; Index < Count; ++Index)
	{
		CUIRect Button;
		Rect.VSplitLeft(ButtonWidth, &Button, &Rect);
		int Corners = IGraphics::CORNER_NONE;
		if(Index == 0)
			Corners = IGraphics::CORNER_L;
		else if(Index == Count - 1)
			Corners = IGraphics::CORNER_R;
		if(DoButton_Menu(&pButtons[Index], ppNames[Index], Current == Index, &Button, BUTTONFLAG_LEFT, nullptr, Corners))
			Current = Index;
	}
	return Current;
}

void CMenus::StatsWeaponIcon(const CUIRect *pRect, int Weapon)
{
	// The icon says which weapon it is at a glance; a weapon without a sprite
	// falls back to its name so the column is not anonymous. The slot is used
	// either way, so that the labels after it do not move when a weapon skin
	// appears or disappears.
	if(Weapon >= 0 && Weapon < NUM_WEAPONS && GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon].IsValid())
	{
		const CDataWeaponspec &WeaponSpec = g_pData->m_Weapons.m_aId[Weapon];
		float ScaleX;
		float ScaleY;
		Graphics()->GetSpriteScale(WeaponSpec.m_pSpriteBody, ScaleX, ScaleY);
		const float Width = WeaponSpec.m_VisualSize * ScaleX;
		const float Height = WeaponSpec.m_VisualSize * ScaleY;
		const float Scale = std::min((pRect->w - 6.0f) / Width, (pRect->h - 6.0f) / Height);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon]);
		Graphics()->QuadsBegin();
		Graphics()->DrawSprite(pRect->x + pRect->w / 2.0f, pRect->y + pRect->h / 2.0f, Width * Scale, Height * Scale);
		Graphics()->QuadsEnd();
		SkipStatsLabels(1);
		return;
	}
	char aName[64];
	if(Weapon < 0)
		str_copy(aName, Localize("Total"));
	else
		MatchWeaponDisplayName(Weapon, aName, sizeof(aName));
	SLabelProperties Properties;
	Properties.m_MaxWidth = pRect->w - 4.0f;
	Properties.m_EllipsisAtEnd = true;
	Properties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
	StatsLabel(pRect, aName, 10.0f, TEXTALIGN_MC, Properties);
}

void CMenus::StatsOutcomeBar(CUIRect Rect, int Wins, int Draws, int Losses)
{
	const int Total = Wins + Draws + Losses;
	if(Total <= 0)
		return;
	Rect.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_ALL, 3.0f);
	const float TotalWidth = Rect.w;
	const int aCounts[3] = {Wins, Draws, Losses};
	const ColorRGBA aColors[3] = {COLOR_WIN, COLOR_NEUTRAL, COLOR_LOSS};
	for(int Part = 0; Part < 3; ++Part)
	{
		CUIRect Segment;
		Rect.VSplitLeft(TotalWidth * static_cast<float>(aCounts[Part]) / static_cast<float>(Total), &Segment, &Rect);
		Segment.Draw(ColorRGBA(aColors[Part].r, aColors[Part].g, aColors[Part].b, 0.6f), IGraphics::CORNER_ALL, 3.0f);
	}
}

void CMenus::StatsHighlightCard(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pBadge, const char *pTitle, const char *pSubtitle, const char *const *ppLabels, const char *const *ppValues, int Count, const ColorRGBA *pLastValueColor)
{
	CUIRect Card;
	pContent->HSplitTop(8.0f, nullptr, pContent);
	pContent->HSplitTop(56.0f, &Card, pContent);
	if(!pScrollRegion->AddRect(Card))
	{
		SkipStatsLabels(3 + 2 * (size_t)Count);
		return;
	}
	Card.Draw(COLOR_PANEL, IGraphics::CORNER_ALL, 5.0f);
	Card.Margin(7.0f, &Card);
	CUIRect Badge, Text, Pairs;
	Card.VSplitLeft(58.0f, &Badge, &Text);
	Text.VSplitLeft(8.0f, nullptr, &Text);
	// The pairs need a fixed share so that a long map name cannot push the
	// numbers off the card.
	Text.VSplitRight(std::min(Text.w * 0.6f, 74.0f * Count), &Text, &Pairs);

	Badge.HMargin((Badge.h - 26.0f) / 2.0f, &Badge);
	Badge.Draw(ColorRGBA(COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, 0.22f), IGraphics::CORNER_ALL, 4.0f);
	StatsLabel(&Badge, pBadge, 12.0f, TEXTALIGN_MC, {.m_MaxWidth = Badge.w - 4.0f, .m_EllipsisAtEnd = true});

	CUIRect Title, Subtitle;
	Text.HSplitMid(&Title, &Subtitle, 0.0f);
	StatsLabel(&Title, pTitle, 15.0f, TEXTALIGN_BL, {.m_MaxWidth = Title.w, .m_EllipsisAtEnd = true});
	SLabelProperties SubtitleProperties;
	SubtitleProperties.m_MaxWidth = Subtitle.w;
	SubtitleProperties.m_EllipsisAtEnd = true;
	SubtitleProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
	StatsLabel(&Subtitle, pSubtitle, 10.0f, TEXTALIGN_TL, SubtitleProperties);

	for(int Pair = 0; Pair < Count; ++Pair)
	{
		CUIRect Cell, Value, Label;
		Pairs.VSplitLeft(Pairs.w / (Count - Pair), &Cell, &Pairs);
		Cell.HSplitMid(&Value, &Label, 0.0f);
		SLabelProperties ValueProperties;
		ValueProperties.m_MaxWidth = Value.w;
		ValueProperties.m_EllipsisAtEnd = true;
		if(pLastValueColor != nullptr && Pair == Count - 1)
			ValueProperties.SetColor(*pLastValueColor);
		StatsLabel(&Value, ppValues[Pair], 14.0f, TEXTALIGN_BC, ValueProperties);
		SLabelProperties LabelProperties;
		LabelProperties.m_MaxWidth = Label.w;
		LabelProperties.m_EllipsisAtEnd = true;
		LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
		StatsLabel(&Label, ppLabels[Pair], 9.0f, TEXTALIGN_TC, LabelProperties);
	}
}

void CMenus::StatsWeaponMatrix(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pHeading, const CMatchCombatStats &Stats)
{
	if(!Stats.HasData())
		return;
	// Weapons are columns and the numbers are rows, which is the only layout
	// that stays readable when a mod reports more weapons than the game knows.
	const CMatchWeaponStats *apColumns[MAX_MATCH_WEAPONS + 1];
	int NumColumns = 0;
	apColumns[NumColumns++] = &Stats.m_Total;
	for(const CMatchWeaponStats &WeaponStats : Stats.m_vWeapons)
		if(WeaponStats.HasData() && NumColumns < MAX_MATCH_WEAPONS + 1)
			apColumns[NumColumns++] = &WeaponStats;

	StatsHeading(pScrollRegion, pContent, pHeading);

	CUIRect Header;
	pContent->HSplitTop(24.0f, &Header, pContent);
	const float ColumnWidth = (Header.w - STATS_LABEL_WIDTH) / NumColumns;
	if(!pScrollRegion->AddRect(Header))
	{
		SkipStatsLabels(1 + NumColumns);
	}
	else
	{
		Header.Draw(COLOR_TABLE_HEADER, IGraphics::CORNER_T, 4.0f);
		CUIRect Cell;
		Header.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Header);
		Cell.VMargin(6.0f, &Cell);
		SLabelProperties Properties;
		Properties.m_MaxWidth = Cell.w;
		Properties.m_EllipsisAtEnd = true;
		Properties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
		StatsLabel(&Cell, Localize("Weapon"), 10.0f, TEXTALIGN_ML, Properties);
		for(int Column = 0; Column < NumColumns; ++Column)
		{
			Header.VSplitLeft(ColumnWidth, &Cell, &Header);
			StatsWeaponIcon(&Cell, Column == 0 ? -1 : apColumns[Column]->m_Weapon);
		}
	}

	const auto MatrixRow = [&](const char *pLabel, const auto &Value, const auto &Format, bool AccuracyBar) {
		// Nothing reported it, so there is nothing to say about it. A table of
		// dashes reads as missing data even when the data was never possible.
		if(std::none_of(apColumns, apColumns + NumColumns, [&](const CMatchWeaponStats *pWeaponStats) { return Value(*pWeaponStats) != 0; }))
			return;
		CUIRect Row;
		pContent->HSplitTop(20.0f, &Row, pContent);
		if(!pScrollRegion->AddRect(Row))
		{
			SkipStatsLabels(1 + NumColumns);
			return;
		}
		Row.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.14f), IGraphics::CORNER_NONE, 0.0f);
		CUIRect Cell;
		Row.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Row);
		Cell.VMargin(6.0f, &Cell);
		SLabelProperties LabelProperties;
		LabelProperties.m_MaxWidth = Cell.w;
		LabelProperties.m_EllipsisAtEnd = true;
		LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.65f));
		StatsLabel(&Cell, pLabel, 11.5f, TEXTALIGN_ML, LabelProperties);
		for(int Column = 0; Column < NumColumns; ++Column)
		{
			const CMatchWeaponStats *pWeaponStats = apColumns[Column];
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
			StatsLabel(&Cell, aValue, 11.5f, TEXTALIGN_MC, {.m_MaxWidth = Cell.w, .m_EllipsisAtEnd = true});
		}
	};

	MatrixRow(
		Localize("Kills"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Kills; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_Kills); }, false);
	MatrixRow(
		Localize("Deaths"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Deaths; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_Deaths); }, false);
	MatrixRow(
		// A weapon that traded evenly reads zero here, which is a result and
		// not missing data, so the row is offered whenever either side is set.
		Localize("+/-"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Kills + WeaponStats.m_Deaths; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%+" PRId64, WeaponStats.m_Kills - WeaponStats.m_Deaths); }, false);
	MatrixRow(
		Localize("Damage done"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_DamageDone; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_DamageDone); }, false);
	MatrixRow(
		Localize("Damage taken"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_DamageTaken; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { str_format(pBuffer, Size, "%" PRId64, WeaponStats.m_DamageTaken); }, false);
	MatrixRow(
		// What the weapon was reached for, which is the question the two rows
		// of shots and hits only answered together.
		Localize("Usage"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Shots; },
		[&Stats](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { FormatMatchAccuracy(WeaponStats.m_Shots, Stats.m_Total.m_Shots, pBuffer, Size); }, false);
	MatrixRow(
		// Shots without hits cannot say how many of them landed, so the row
		// waits for whoever reports the hits.
		Localize("Accuracy"), [](const CMatchWeaponStats &WeaponStats) { return WeaponStats.m_Hits; },
		[](const CMatchWeaponStats &WeaponStats, char *pBuffer, int Size) { FormatMatchAccuracy(WeaponStats.m_Hits, WeaponStats.m_Shots, pBuffer, Size); }, true);
}

void CMenus::RenderStatsOverview(CUIRect View)
{
	static CScrollRegion s_ScrollRegion;
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 40.0f;
	s_ScrollRegion.Begin(&View, &ScrollParams);

	const CMatchProfile &AllTime = m_aStatsProfiles[(int)EStatsPeriod::ALL_TIME];

	CUIRect Header;
	View.HSplitTop(58.0f, &Header, &View);
	if(!s_ScrollRegion.AddRect(Header))
	{
		SkipStatsLabels(3);
	}
	else
	{
		Header.Draw(COLOR_PANEL, IGraphics::CORNER_ALL, 5.0f);
		Header.Margin(8.0f, &Header);
		CUIRect Skin, Text, Name, Subtitle, Bar;
		Header.VSplitLeft(34.0f, &Skin, &Text);
		Text.VSplitLeft(6.0f, nullptr, &Text);
		Text.HSplitTop(20.0f, &Name, &Text);
		Text.HSplitTop(14.0f, &Subtitle, &Text);
		Text.HSplitTop(8.0f, &Bar, nullptr);

		const CTeeRenderInfo TeeInfo = GetTeeRenderInfo(vec2(Skin.w, Skin.h), g_Config.m_ClPlayerSkin, g_Config.m_ClPlayerUseCustomColor, g_Config.m_ClPlayerColorBody, g_Config.m_ClPlayerColorFeet);
		const CAnimState *pIdleState = CAnimState::GetIdle();
		vec2 OffsetToMid;
		CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
		RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), vec2(Skin.x + TeeInfo.m_Size / 2.0f, Skin.y + Skin.h / 2.0f + OffsetToMid.y));

		CUIRect Flag;
		Name.VSplitRight(24.0f, &Name, &Flag);
		StatsLabel(&Name, g_Config.m_PlayerName, 16.0f, TEXTALIGN_ML, {.m_MaxWidth = Name.w, .m_EllipsisAtEnd = true});
		GameClient()->m_CountryFlags.Render(g_Config.m_PlayerCountry, ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f), Flag.x, Flag.y + Flag.h / 2.0f - 0.375f * Flag.h, 1.5f * 0.5f * Flag.h, 0.75f * 0.5f * Flag.h);

		char aSubtitle[128];
		if(m_StatsInfo.m_OldestMatchUtc.has_value())
		{
			char aOldest[64];
			FormatMatchTimestamp(*m_StatsInfo.m_OldestMatchUtc, aOldest, sizeof(aOldest));
			str_format(aSubtitle, sizeof(aSubtitle), Localize("%d matches on this device, since %s"), m_StatsInfo.m_NumMatches, aOldest);
		}
		else
		{
			str_copy(aSubtitle, Localize("No matches on this device yet"));
		}
		SLabelProperties SubtitleProperties;
		SubtitleProperties.m_MaxWidth = Subtitle.w;
		SubtitleProperties.m_EllipsisAtEnd = true;
		SubtitleProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
		StatsLabel(&Subtitle, aSubtitle, 10.0f, TEXTALIGN_ML, SubtitleProperties);
		StatsOutcomeBar(Bar, AllTime.m_Wins, AllTime.m_Draws, AllTime.m_Losses);
	}

	// What a look at the page should answer without reading anything else.
	const int64_t Kills = SumMetricSuffix(AllTime, "/kills");
	const int64_t Deaths = SumMetricSuffix(AllTime, "/deaths");
	char aaTiles[4][64];
	str_format(aaTiles[0], sizeof(aaTiles[0]), "%d", AllTime.m_Matches);
	if(AllTime.m_Matches > 0)
		str_format(aaTiles[1], sizeof(aaTiles[1]), "%.0f%%", 100.0f * AllTime.m_Wins / AllTime.m_Matches);
	else
		str_copy(aaTiles[1], "-");
	FormatMatchSeconds(AllTime.m_PlaytimeSeconds, aaTiles[2], sizeof(aaTiles[2]));
	if(Kills == 0 && Deaths == 0)
		str_copy(aaTiles[3], "-");
	else
		FormatRatio(Kills, Deaths, aaTiles[3], sizeof(aaTiles[3]));
	const char *apTileLabels[4] = {Localize("Matches"), Localize("Win rate"), Localize("Playtime"), Localize("K/D")};
	const char *apTileValues[4] = {aaTiles[0], aaTiles[1], aaTiles[2], aaTiles[3]};
	StatsTiles(&s_ScrollRegion, &View, apTileLabels, apTileValues, 4);

	if(!m_vStatsModeSummaries.empty())
	{
		const CStatsModeSummary &Summary = m_vStatsModeSummaries.front();
		char aaValues[3][32];
		str_format(aaValues[0], sizeof(aaValues[0]), "%d", Summary.m_Matches);
		if(Summary.m_Matches > 0)
			str_format(aaValues[1], sizeof(aaValues[1]), "%.0f%%", 100.0f * Summary.m_Wins / Summary.m_Matches);
		else
			str_copy(aaValues[1], "-");
		FormatMatchSeconds(Summary.m_PlaytimeSeconds, aaValues[2], sizeof(aaValues[2]));
		const char *apLabels[3] = {Localize("Matches"), Localize("Win rate"), Localize("Playtime")};
		const char *apValues[3] = {aaValues[0], aaValues[1], aaValues[2]};
		char aBadge[MatchReportLimits::MAX_MODE_ID_LENGTH];
		ShortModeName(Summary.m_ModeId, aBadge, sizeof(aBadge));
		StatsHighlightCard(&s_ScrollRegion, &View, aBadge, Localize("Most played gametype"), Summary.m_ModeId.c_str(), apLabels, apValues, 3, nullptr);
	}

	if(m_StatsLastMatch.has_value())
	{
		const CMatchHistoryEntry &Entry = *m_StatsLastMatch;
		char aaValues[3][32];
		if(Entry.m_LocalScore.has_value())
			str_format(aaValues[0], sizeof(aaValues[0]), "%" PRId64, *Entry.m_LocalScore);
		else
			str_copy(aaValues[0], "-");
		FormatMatchDuration(Entry.m_DurationTicks, Entry.m_TickRate, aaValues[1], sizeof(aaValues[1]));
		str_copy(aaValues[2], OutcomeName(Entry.m_LocalOutcome));
		const char *apLabels[3] = {Localize("Score"), Localize("Duration"), Localize("Result")};
		const char *apValues[3] = {aaValues[0], aaValues[1], aaValues[2]};
		char aWhen[64];
		FormatMatchTimestamp(Entry.m_EndTimeUtc, aWhen, sizeof(aWhen));
		char aSubtitle[192];
		char aBadge[MatchReportLimits::MAX_MODE_ID_LENGTH];
		ShortModeName(Entry.m_ModeId, aBadge, sizeof(aBadge));
		str_format(aSubtitle, sizeof(aSubtitle), "%s  ·  %s  ·  %s", aBadge, aWhen, Entry.m_OriginId.c_str());
		const ColorRGBA ResultColor = OutcomeColor(Entry.m_LocalOutcome);
		StatsHighlightCard(&s_ScrollRegion, &View, aBadge, Entry.m_MapName.c_str(), aSubtitle, apLabels, apValues, 3, &ResultColor);
	}

	s_ScrollRegion.End();
}

void CMenus::RenderStatsMatchList(CUIRect View)
{
	static constexpr float ROW_HEIGHT = 34.0f;
	static constexpr size_t LABELS_PER_ROW = 4;
	CUIRect Filters, List;
	View.HSplitTop(26.0f, &Filters, &View);
	View.HSplitTop(4.0f, nullptr, &List);
	CUIRect Search, Quality, Mode;
	Filters.VSplitRight(150.0f, &Search, &Quality);
	Search.VSplitRight(6.0f, &Search, nullptr);
	Search.VSplitRight(140.0f, &Search, &Mode);
	Search.VSplitRight(6.0f, &Search, nullptr);
	StatsModeDropDown(Mode);
	if(Ui()->DoEditBox_SearchCached(&m_StatsHistorySearchInput, &Search, 12.0f, !Ui()->IsPopupOpen() && !GameClient()->m_GameConsole.IsActive(), m_aStatsSearchUiElements.data(), &m_aStatsSearchUiElements[1]))
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
	// A row owns four labels and takes its slots from its own index, so a row
	// keeps its cached text while scrolling without the pool having to hold a
	// slot for every match ever recorded. The window is one row larger than
	// what fits, because the first and the last row are usually cut off.
	const size_t SlotRows = (size_t)(List.h / ROW_HEIGHT) + 2;
	const size_t FirstSlot = m_StatsLabelUiElementIndex;
	SkipStatsLabels(SlotRows * LABELS_PER_ROW);
	static CListBox s_ListBox;
	s_ListBox.DoStart(ROW_HEIGHT, m_vStatsHistory.size(), 1, 3, m_StatsSelectedIndex, &List, false, IGraphics::CORNER_ALL, true);
	for(int Index = 0; Index < static_cast<int>(m_vStatsHistory.size()); ++Index)
	{
		const CMatchHistoryEntry &Entry = m_vStatsHistory[Index];
		const CListboxItem Item = s_ListBox.DoNextItem(&Entry, Index == m_StatsSelectedIndex);
		if(!Item.m_Visible)
			continue;
		SetStatsLabelSlot(FirstSlot + ((size_t)Index % SlotRows) * LABELS_PER_ROW);

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

		char aMode[MatchReportLimits::MAX_MODE_ID_LENGTH];
		ShortModeName(Entry.m_ModeId, aMode, sizeof(aMode));
		char aTitle[192];
		str_format(aTitle, sizeof(aTitle), "%s  ·  %s", Entry.m_MapName.c_str(), aMode);
		StatsLabel(&Title, aTitle, 12.5f, TEXTALIGN_ML, {.m_MaxWidth = Title.w, .m_EllipsisAtEnd = true});

		char aDate[64];
		FormatMatchTimestamp(Entry.m_EndTimeUtc, aDate, sizeof(aDate));
		char aDuration[64];
		FormatMatchDuration(Entry.m_DurationTicks, Entry.m_TickRate, aDuration, sizeof(aDuration));
		char aKd[64] = "";
		if(Entry.m_LocalKills.has_value() || Entry.m_LocalDeaths.has_value())
			str_format(aKd, sizeof(aKd), "  ·  %" PRId64 " / %" PRId64, Entry.m_LocalKills.value_or(0), Entry.m_LocalDeaths.value_or(0));
		// A report the client wrote itself or one that missed part of the match
		// says so here, so that a number nobody vouches for is not read as one
		// that the server confirmed.
		char aTrust[64] = "";
		if(Entry.m_Source == EMatchReportSource::CLIENT_OBSERVED)
			str_format(aTrust, sizeof(aTrust), "  ·  %s", Localize("observed"));
		else if(Entry.m_Completeness != EMatchCompleteness::COMPLETE)
			str_format(aTrust, sizeof(aTrust), "  ·  %s", Localize("partial"));
		char aDetail[320];
		str_format(aDetail, sizeof(aDetail), "%s  ·  %s%s%s", aDate, aDuration, aKd, aTrust);
		SLabelProperties DetailProperties;
		DetailProperties.m_MaxWidth = Detail.w;
		DetailProperties.m_EllipsisAtEnd = true;
		DetailProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f));
		StatsLabel(&Detail, aDetail, 9.5f, TEXTALIGN_ML, DetailProperties);

		char aScore[32] = "-";
		if(Entry.m_LocalScore.has_value())
			str_format(aScore, sizeof(aScore), "%" PRId64, *Entry.m_LocalScore);
		StatsLabel(&Score, aScore, 16.0f, TEXTALIGN_MC, {});
		SLabelProperties ResultProperties;
		ResultProperties.m_MaxWidth = Result.w;
		ResultProperties.m_EllipsisAtEnd = true;
		ResultProperties.SetColor(OutcomeColor(Entry.m_LocalOutcome));
		StatsLabel(&Result, OutcomeName(Entry.m_LocalOutcome), 12.0f, TEXTALIGN_MC, ResultProperties);
	}
	SetStatsLabelSlot(FirstSlot + SlotRows * LABELS_PER_ROW);
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
		StatsLabel(&EmptyHint, Filtered ? Localize("No match matches the current filter") : Localize("No matches have been recorded yet"), 14.0f, TEXTALIGN_MC, {});
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
			StatsLabel(&Hint, Localize("Select a match in the list to see its report"), 14.0f, TEXTALIGN_MC, {});
		s_ScrollRegion.End();
		return;
	}

	const CStoredMatch &Stored = *m_StatsSelectedMatch;
	const CMatchReport &Report = Stored.m_Report;
	CMatchDetailView &Detail = m_StatsMatchView;
	const CMatchDetailRow *pLocalRow = nullptr;
	for(const CMatchDetailRow &Row : Detail.m_vRows)
		if(Row.m_Local)
			pLocalRow = &Row;

	// The banner answers "how did it go" in one glance, everything below it
	// answers "why".
	CUIRect Banner, Outcome, Subtitle;
	View.HSplitTop(58.0f, &Banner, &View);
	if(s_ScrollRegion.AddRect(Banner))
	{
		Banner.Draw(COLOR_PANEL, IGraphics::CORNER_ALL, 5.0f);
		Banner.Margin(6.0f, &Banner);
		Banner.HSplitTop(28.0f, &Outcome, &Subtitle);
		const std::optional<EMatchOutcome> LocalOutcome = pLocalRow == nullptr ? std::nullopt : pLocalRow->m_Outcome;
		SLabelProperties OutcomeProperties;
		OutcomeProperties.SetColor(OutcomeColor(LocalOutcome));
		StatsLabel(&Outcome, LocalOutcome.has_value() ? MatchOutcomeDisplayName(*LocalOutcome) : Localize("Match report"), 24.0f, TEXTALIGN_MC, OutcomeProperties);
		char aDate[64];
		FormatMatchTimestamp(Report.m_EndTimeUtc, aDate, sizeof(aDate));
		char aDuration[64];
		FormatMatchDuration(Report.m_DurationTicks, Report.m_TickRate, aDuration, sizeof(aDuration));
		char aMode[MatchReportLimits::MAX_MODE_ID_LENGTH];
		ShortModeName(Report.m_ModeId, aMode, sizeof(aMode));
		char aSubtitle[320];
		str_format(aSubtitle, sizeof(aSubtitle), "%s  ·  %s  ·  %s  ·  %s", Report.m_MapName.c_str(), aMode, aDuration, aDate);
		SLabelProperties SubtitleProperties;
		SubtitleProperties.m_MaxWidth = Subtitle.w;
		SubtitleProperties.m_EllipsisAtEnd = true;
		SubtitleProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
		StatsLabel(&Subtitle, aSubtitle, 11.0f, TEXTALIGN_MC, SubtitleProperties);
	}
	else
	{
		SkipStatsLabels(2);
	}

	if(pLocalRow != nullptr)
	{
		char aRank[32] = "-";
		if(pLocalRow->m_Rank > 0)
			str_format(aRank, sizeof(aRank), "#%d", pLocalRow->m_Rank);
		char aScore[32];
		str_format(aScore, sizeof(aScore), "%" PRId64, pLocalRow->m_Score);
		char aKills[32];
		str_format(aKills, sizeof(aKills), "%" PRId64 " / %" PRId64, pLocalRow->m_Combat.m_Total.m_Kills, pLocalRow->m_Combat.m_Total.m_Deaths);
		char aKd[32];
		FormatRatio(pLocalRow->m_Combat.m_Total.m_Kills, pLocalRow->m_Combat.m_Total.m_Deaths, aKd, sizeof(aKd));
		char aAccuracy[32];
		FormatMatchAccuracy(pLocalRow->m_Combat.m_Total.m_Hits, pLocalRow->m_Combat.m_Total.m_Shots, aAccuracy, sizeof(aAccuracy));
		const char *apLabels[5] = {Localize("Rank"), Localize("Score"), Localize("Kills / deaths"), Localize("K/D"), Localize("Accuracy")};
		const char *apValues[5] = {aRank, aScore, aKills, aKd, aAccuracy};
		StatsTiles(&s_ScrollRegion, &View, apLabels, apValues, 5);
	}

	// One table seen through a page instead of one block per kind of number:
	// the pages a report cannot fill are not offered, so a race mode shows its
	// run and a deathmatch its weapons without either knowing about the other.
	EStatsMatchTab aTabs[(int)EStatsMatchTab::COUNT];
	const char *apTabNames[(int)EStatsMatchTab::COUNT];
	int NumTabs = 0;
	int CurrentTab = 0;
	for(int Index = 0; Index < (int)EStatsMatchTab::COUNT; ++Index)
	{
		const EStatsMatchTab Tab = (EStatsMatchTab)Index;
		if(!Detail.HasTab(Tab))
			continue;
		if(Tab == Detail.m_Tab)
			CurrentTab = NumTabs;
		aTabs[NumTabs] = Tab;
		apTabNames[NumTabs] = StatsMatchTabDisplayName(Tab);
		++NumTabs;
	}
	if(NumTabs > 0)
	{
		CUIRect Bar;
		View.HSplitTop(10.0f, nullptr, &View);
		View.HSplitTop(22.0f, &Bar, &View);
		if(s_ScrollRegion.AddRect(Bar))
		{
			static CButtonContainer s_aTabButtons[(int)EStatsMatchTab::COUNT];
			const int NewTab = StatsToggle(s_aTabButtons, Bar, apTabNames, NumTabs, CurrentTab);
			if(NewTab != CurrentTab)
				UpdateMatchDetailBest(Detail, aTabs[NewTab]);
		}
	}

	const int NumColumns = (int)Detail.m_vColumns.size();
	// The name keeps a fixed share of the width, so that a mode with a dozen
	// weapons narrows its columns instead of pushing the names out of the row.
	const float NameWidth = std::clamp(View.w * 0.32f, 90.0f, 190.0f);
	const auto CellRect = [&](CUIRect *pCells, int Column) {
		CUIRect Cell;
		pCells->VSplitLeft(pCells->w / (NumColumns - Column), &Cell, pCells);
		return Cell;
	};

	CUIRect Header;
	View.HSplitTop(6.0f, nullptr, &View);
	View.HSplitTop(20.0f, &Header, &View);
	if(s_ScrollRegion.AddRect(Header))
	{
		Header.Draw(COLOR_TABLE_HEADER, IGraphics::CORNER_T, 4.0f);
		CUIRect Name, Cells;
		Header.VSplitLeft(NameWidth, &Name, &Cells);
		Name.VMargin(7.0f, &Name);
		SLabelProperties HeaderProperties;
		HeaderProperties.m_MaxWidth = Name.w;
		HeaderProperties.m_EllipsisAtEnd = true;
		HeaderProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
		StatsLabel(&Name, Localize("Player"), 10.0f, TEXTALIGN_ML, HeaderProperties);
		for(int Column = 0; Column < NumColumns; ++Column)
		{
			CUIRect Cell = CellRect(&Cells, Column);
			if(Detail.m_vColumns[Column].m_Weapon >= 0)
			{
				StatsWeaponIcon(&Cell, Detail.m_vColumns[Column].m_Weapon);
				continue;
			}
			SLabelProperties CellProperties;
			CellProperties.m_MaxWidth = Cell.w - 2.0f;
			CellProperties.m_EllipsisAtEnd = true;
			CellProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
			StatsLabel(&Cell, Detail.m_vColumns[Column].m_Label.c_str(), 10.0f, TEXTALIGN_MC, CellProperties);
		}
	}
	else
	{
		SkipStatsLabels(1 + (size_t)NumColumns);
	}

	const auto RenderDetailRow = [&](const CMatchDetailRow &Row, const std::optional<EMatchOutcome> &RowOutcome, const char *pName, bool Summary) {
		CUIRect Line;
		View.HSplitTop(20.0f, &Line, &View);
		if(!s_ScrollRegion.AddRect(Line))
		{
			SkipStatsLabels(1 + (size_t)NumColumns);
			return;
		}
		if(Summary)
			Line.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.10f), IGraphics::CORNER_NONE, 0.0f);
		else if(Row.m_Local)
			Line.Draw(ColorRGBA(COLOR_ACCENT.r, COLOR_ACCENT.g, COLOR_ACCENT.b, 0.18f), IGraphics::CORNER_NONE, 0.0f);
		// The result of a row is a stripe rather than a column, which keeps it
		// readable next to a table whose columns belong to the mode.
		CUIRect Accent, Name, Cells;
		Line.VSplitLeft(3.0f, &Accent, &Line);
		if(RowOutcome.has_value())
		{
			Accent.HMargin(3.0f, &Accent);
			Accent.Draw(OutcomeColor(RowOutcome), IGraphics::CORNER_ALL, 1.5f);
		}
		Line.VSplitLeft(NameWidth - 3.0f, &Name, &Cells);
		Name.VMargin(4.0f, &Name);
		const float Alpha = Row.m_Left ? 0.5f : 1.0f;
		SLabelProperties NameProperties;
		NameProperties.m_MaxWidth = Name.w;
		NameProperties.m_EllipsisAtEnd = true;
		NameProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, Alpha));
		StatsLabel(&Name, pName, Summary ? 11.0f : 12.0f, TEXTALIGN_ML, NameProperties);
		for(int Column = 0; Column < NumColumns; ++Column)
		{
			CUIRect Cell = CellRect(&Cells, Column);
			char aValue[64];
			FormatMatchDetailCell(Row, Detail.m_vColumns[Column], Detail.m_TickRate, aValue, sizeof(aValue));
			SLabelProperties CellProperties;
			CellProperties.m_MaxWidth = Cell.w - 2.0f;
			CellProperties.m_EllipsisAtEnd = true;
			const std::optional<int64_t> Value = MatchDetailCell(Row, Detail.m_vColumns[Column]);
			// The best cell of a column is worth pointing at; a summary is not
			// in the running for it.
			if(!Summary && Value.has_value() && Value == Detail.m_vColumns[Column].m_Best)
				CellProperties.SetColor(COLOR_ACCENT);
			else
				CellProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, Alpha));
			StatsLabel(&Cell, aValue, Summary ? 11.0f : 12.0f, TEXTALIGN_MC, CellProperties);
		}
	};

	for(const CMatchDetailBlock &Block : Detail.m_vBlocks)
	{
		if(Detail.m_HasTeams)
		{
			char aName[MatchReportLimits::MAX_DISPLAY_NAME_LENGTH + 64];
			if(Block.m_Outcome.has_value())
				str_format(aName, sizeof(aName), "%s  ·  %s", Block.m_DisplayName.c_str(), MatchOutcomeDisplayName(*Block.m_Outcome));
			else
				str_copy(aName, Block.m_DisplayName.c_str());
			RenderDetailRow(Block.m_Summary, Block.m_Outcome, aName, true);
		}
		for(const std::vector<int> *pvIndices : {&Block.m_vPlaying, &Block.m_vLeft})
		{
			for(const int Index : *pvIndices)
			{
				const CMatchDetailRow &Row = Detail.m_vRows[Index];
				char aName[MatchReportLimits::MAX_DISPLAY_NAME_LENGTH + 32];
				if(Row.m_Left)
					str_format(aName, sizeof(aName), "%d. %s (%s)", Row.m_Rank, Row.m_pParticipant->m_DisplayName.c_str(), Localize("left"));
				else
					str_format(aName, sizeof(aName), "%d. %s", Row.m_Rank, Row.m_pParticipant->m_DisplayName.c_str());
				RenderDetailRow(Row, Detail.m_HasTeams ? std::nullopt : Row.m_Outcome, aName, false);
			}
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
		StatsLabel(&Footer, aFooter, 10.0f, TEXTALIGN_ML, FooterProperties);
	}
	else
	{
		SkipStatsLabels(1);
	}
	s_ScrollRegion.End();
}

const char *CMenus::StatsPeriodName(EStatsPeriod Period)
{
	switch(Period)
	{
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
	{
		SkipStatsLabels(1 + (size_t)EStatsPeriod::COUNT);
		return;
	}
	Row.Draw(COLOR_TABLE_HEADER, IGraphics::CORNER_T, 4.0f);
	SLabelProperties Properties;
	Properties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
	CUIRect Cell;
	Row.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Row);
	Cell.VMargin(6.0f, &Cell);
	Properties.m_MaxWidth = Cell.w;
	Properties.m_EllipsisAtEnd = true;
	StatsLabel(&Cell, pLabel, 10.0f, TEXTALIGN_ML, Properties);
	const float ColumnWidth = Row.w / (int)EStatsPeriod::COUNT;
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
	{
		Row.VSplitLeft(ColumnWidth, &Cell, &Row);
		Cell.VMargin(6.0f, &Cell);
		Properties.m_MaxWidth = Cell.w;
		StatsLabel(&Cell, StatsPeriodName((EStatsPeriod)Period), 10.0f, TEXTALIGN_MR, Properties);
	}
}

void CMenus::StatsPeriodRow(CScrollRegion *pScrollRegion, CUIRect *pContent, const char *pLabel, const char *const *ppValues)
{
	CUIRect Row;
	pContent->HSplitTop(18.0f, &Row, pContent);
	if(!pScrollRegion->AddRect(Row))
	{
		SkipStatsLabels(1 + (size_t)EStatsPeriod::COUNT);
		return;
	}
	CUIRect Cell;
	Row.VSplitLeft(STATS_LABEL_WIDTH, &Cell, &Row);
	Cell.VMargin(6.0f, &Cell);
	SLabelProperties LabelProperties;
	LabelProperties.m_MaxWidth = Cell.w;
	LabelProperties.m_EllipsisAtEnd = true;
	LabelProperties.SetColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.65f));
	StatsLabel(&Cell, pLabel, 11.5f, TEXTALIGN_ML, LabelProperties);
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
		StatsLabel(&Cell, ppValues[Period], 11.5f, TEXTALIGN_MR, ValueProperties);
	}
}

void CMenus::RenderStatsAggregate(CUIRect View)
{
	CUIRect FilterLine;
	View.HSplitTop(24.0f, &FilterLine, &View);
	View.HSplitTop(4.0f, nullptr, &View);
	CUIRect ModeDropDown, ScaleButtons;
	FilterLine.VSplitLeft(180.0f, &ModeDropDown, &ScaleButtons);
	StatsModeDropDown(ModeDropDown);

	// The same numbers read completely differently depending on how many matches
	// went into them, so they can be divided by matches or by time played.
	static CButtonContainer s_aScaleButtons[3];
	const char *apScaleNames[3] = {Localize("Total"), Localize("Per match"), Localize("Per minute")};
	m_StatsScale = (EStatsScale)StatsToggle(s_aScaleButtons, ScaleButtons, apScaleNames, 3, (int)m_StatsScale);

	static CScrollRegion s_ScrollRegion;
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 40.0f;
	s_ScrollRegion.Begin(&View, &ScrollParams);

	const CMatchProfile &AllTime = m_aStatsProfiles[(int)EStatsPeriod::ALL_TIME];

	const char *apPeriodNames[(int)EStatsPeriod::COUNT];
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		apPeriodNames[Period] = StatsPeriodName((EStatsPeriod)Period);

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
	// A row of a metric that is summed over the period, which is the only kind
	// the scale toggle may touch.
	const auto SummedRow = [&](const char *pLabel, const char *pSuffix) {
		if(SumMetricSuffix(AllTime, pSuffix) == 0)
			return;
		for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
			FormatScaled(SumMetricSuffix(m_aStatsProfiles[Period], pSuffix), Period, aaValues[Period], sizeof(aaValues[Period]));
		PeriodRow(pLabel);
	};

	StatsHeading(&s_ScrollRegion, &View, Localize("Overall"));
	StatsPeriodHeader(&s_ScrollRegion, &View, Localize("Record"));
	// Counting how many matches there were and then dividing by how many matches
	// there were says nothing, so this group is never scaled.
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		FormatMatchSeconds(m_aStatsProfiles[Period].m_PlaytimeSeconds, aaValues[Period], sizeof(aaValues[Period]));
	PeriodRow(Localize("Playtime"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		str_format(aaValues[Period], sizeof(aaValues[Period]), "%d", m_aStatsProfiles[Period].m_Matches);
	PeriodRow(Localize("Matches played"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
	{
		const CMatchProfile &Profile = m_aStatsProfiles[Period];
		if(Profile.m_Matches > 0)
			str_format(aaValues[Period], sizeof(aaValues[Period]), "%d  (%.0f%%)", Profile.m_Wins, 100.0f * Profile.m_Wins / Profile.m_Matches);
		else
			str_copy(aaValues[Period], "-");
	}
	PeriodRow(Localize("Matches won"));
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		str_format(aaValues[Period], sizeof(aaValues[Period]), "%d", m_aStatsProfiles[Period].m_Losses);
	PeriodRow(Localize("Matches lost"));

	if(AllTime.m_Matches > 0)
	{
		CUIRect Bar;
		View.HSplitTop(6.0f, nullptr, &View);
		View.HSplitTop(10.0f, &Bar, &View);
		if(s_ScrollRegion.AddRect(Bar))
		{
			Bar.VMargin(3.0f, &Bar);
			StatsOutcomeBar(Bar, AllTime.m_Wins, AllTime.m_Draws, AllTime.m_Losses);
		}
	}

	// The frag numbers only belong to a gametype that counts frags. A race
	// gametype reports none of them and gets nothing to look at rather than a
	// table of dashes.
	const bool HasCombat = SumMetricSuffix(AllTime, "/kills") != 0 || SumMetricSuffix(AllTime, "/deaths") != 0 || SumMetricSuffix(AllTime, "/shots") != 0;
	if(HasCombat)
	{
		StatsHeading(&s_ScrollRegion, &View, Localize("Combat"));
		StatsPeriodHeader(&s_ScrollRegion, &View, Localize("Metric"));
		SummedRow(Localize("Kills"), "/kills");
		SummedRow(Localize("Assists"), "/assists");
		SummedRow(Localize("Deaths"), "/deaths");
		if(SumMetricSuffix(AllTime, "/kills") != 0 || SumMetricSuffix(AllTime, "/deaths") != 0)
		{
			for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
				str_format(aaValues[Period], sizeof(aaValues[Period]), "%+" PRId64, SumMetricSuffix(m_aStatsProfiles[Period], "/kills") - SumMetricSuffix(m_aStatsProfiles[Period], "/deaths"));
			PeriodRow(Localize("+/-"));
		}
		SummedRow(Localize("Damage done"), "/damage_done");
		SummedRow(Localize("Damage taken"), "/damage_taken");
		if(SumMetricSuffix(AllTime, "/shots") != 0)
		{
			// A quotient of two sums, so it is already an average and the scale
			// toggle has nothing left to do to it.
			for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
				FormatMatchAccuracy(SumMetricSuffix(m_aStatsProfiles[Period], "/hits"), SumMetricSuffix(m_aStatsProfiles[Period], "/shots"), aaValues[Period], sizeof(aaValues[Period]));
			PeriodRow(Localize("Accuracy"));
		}
	}

	// The weapon numbers are a table of weapons against numbers already, so a
	// third dimension of periods does not fit into it. It shows one period,
	// picked here and named in the heading.
	// Kept between frames, because this page is redrawn like any other and the
	// weapon rows would otherwise be allocated again on each of them.
	static CMatchCombatStats s_WeaponStats;
	BuildMatchCombatStats(m_aStatsProfiles[(int)m_StatsWeaponPeriod], s_WeaponStats);
	const CMatchCombatStats &WeaponStats = s_WeaponStats;
	if(WeaponStats.HasData())
	{
		CUIRect PeriodButtons;
		View.HSplitTop(8.0f, nullptr, &View);
		View.HSplitTop(20.0f, &PeriodButtons, &View);
		if(s_ScrollRegion.AddRect(PeriodButtons))
		{
			static CButtonContainer s_aWeaponPeriodButtons[(int)EStatsPeriod::COUNT];
			m_StatsWeaponPeriod = (EStatsPeriod)StatsToggle(s_aWeaponPeriodButtons, PeriodButtons, apPeriodNames, (int)EStatsPeriod::COUNT, (int)m_StatsWeaponPeriod);
		}
		char aWeaponHeading[64];
		str_format(aWeaponHeading, sizeof(aWeaponHeading), "%s — %s", Localize("Weapons"), StatsPeriodName(m_StatsWeaponPeriod));
		StatsWeaponMatrix(&s_ScrollRegion, &View, aWeaponHeading, WeaponStats);
	}

	// One weapon over all periods, which is the question the matrix cannot
	// answer because it spends its columns on the weapons.
	static CMatchCombatStats s_aPeriodStats[(int)EStatsPeriod::COUNT];
	for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		BuildMatchCombatStats(m_aStatsProfiles[Period], s_aPeriodStats[Period]);
	const CMatchCombatStats *aPeriodStats = s_aPeriodStats;
	int aWeapons[MAX_MATCH_WEAPONS];
	int NumWeapons = 0;
	for(const CMatchWeaponStats &Weapon : aPeriodStats[(int)EStatsPeriod::ALL_TIME].m_vWeapons)
		if(Weapon.HasData() && NumWeapons < MAX_MATCH_WEAPONS)
			aWeapons[NumWeapons++] = Weapon.m_Weapon;
	if(NumWeapons > 0)
	{
		StatsHeading(&s_ScrollRegion, &View, Localize("Weapon over time"));
		m_StatsWeaponIndex = std::clamp(m_StatsWeaponIndex, 0, NumWeapons - 1);
		CUIRect Selector;
		View.HSplitTop(26.0f, &Selector, &View);
		if(!s_ScrollRegion.AddRect(Selector))
		{
			SkipStatsLabels(NumWeapons);
		}
		else
		{
			// A mod may report more weapons than fit into a row of buttons; the
			// ones past the row stay reachable through the matrix above.
			static CButtonContainer s_aWeaponButtons[MAX_STATS_WEAPON_BUTTONS];
			const int Shown = std::min(NumWeapons, MAX_STATS_WEAPON_BUTTONS);
			SkipStatsLabels(NumWeapons - Shown);
			const float ButtonWidth = std::min(44.0f, Selector.w / Shown);
			for(int Index = 0; Index < Shown; ++Index)
			{
				CUIRect Button;
				Selector.VSplitLeft(ButtonWidth, &Button, &Selector);
				Button.Margin(2.0f, &Button);
				Button.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, (int)Index == m_StatsWeaponIndex ? 0.22f : 0.06f), IGraphics::CORNER_ALL, 3.0f);
				StatsWeaponIcon(&Button, aWeapons[Index]);
				if(Ui()->DoButtonLogic(&s_aWeaponButtons[Index], 0, &Button, BUTTONFLAG_LEFT))
					m_StatsWeaponIndex = (int)Index;
			}
		}

		const int Weapon = aWeapons[m_StatsWeaponIndex];
		const auto WeaponOf = [&](int Period) -> const CMatchWeaponStats * {
			for(const CMatchWeaponStats &Stats : aPeriodStats[Period].m_vWeapons)
				if(Stats.m_Weapon == Weapon)
					return &Stats;
			return nullptr;
		};
		char aHeading[64];
		MatchWeaponDisplayName(Weapon, aHeading, sizeof(aHeading));
		StatsPeriodHeader(&s_ScrollRegion, &View, aHeading);
		const auto WeaponRow = [&](const char *pLabel, int64_t CMatchWeaponStats::*pMember) {
			bool Any = false;
			for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
			{
				const CMatchWeaponStats *pStats = WeaponOf(Period);
				if(pStats == nullptr)
					str_copy(aaValues[Period], "-");
				else
				{
					Any = Any || pStats->*pMember != 0;
					FormatScaled(pStats->*pMember, Period, aaValues[Period], sizeof(aaValues[Period]));
				}
			}
			if(Any)
				PeriodRow(pLabel);
		};
		WeaponRow(Localize("Kills"), &CMatchWeaponStats::m_Kills);
		WeaponRow(Localize("Deaths"), &CMatchWeaponStats::m_Deaths);
		WeaponRow(Localize("Damage done"), &CMatchWeaponStats::m_DamageDone);
		WeaponRow(Localize("Damage taken"), &CMatchWeaponStats::m_DamageTaken);
		bool AnyShots = false;
		for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
		{
			const CMatchWeaponStats *pStats = WeaponOf(Period);
			AnyShots = AnyShots || (pStats != nullptr && pStats->m_Shots != 0);
		}
		if(AnyShots)
		{
			for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
			{
				const CMatchWeaponStats *pStats = WeaponOf(Period);
				FormatMatchAccuracy(pStats == nullptr ? 0 : pStats->m_Shots, aPeriodStats[Period].m_Total.m_Shots, aaValues[Period], sizeof(aaValues[Period]));
			}
			PeriodRow(Localize("Usage"));
			for(int Period = 0; Period < (int)EStatsPeriod::COUNT; ++Period)
			{
				const CMatchWeaponStats *pStats = WeaponOf(Period);
				FormatMatchAccuracy(pStats == nullptr ? 0 : pStats->m_Hits, pStats == nullptr ? 0 : pStats->m_Shots, aaValues[Period], sizeof(aaValues[Period]));
			}
			PeriodRow(Localize("Accuracy"));
		}
	}

	// Everything the reports carried that is neither a weapon number nor one of
	// the frag numbers above. Which metrics exist is decided by the reports and
	// not by a list in here, so a mod's own metrics appear next to the ones this
	// build knows.
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
			Ui()->DoLabel(&Hint, Localize("Pick a gametype to see what else it counts"), 11.0f, TEXTALIGN_MC, HintProperties);
		}
		s_ScrollRegion.End();
		return;
	}
	bool WroteHeading = false;
	for(const CMatchMetricAggregate &Metric : AllTime.m_vMetrics)
	{
		if(Metric.m_Value == 0 || IsMatchCombatStatMetric(Metric.m_MetricId, Metric.m_ModeSchemaVersion) || IsStatsOverallMetric(Metric.m_MetricId))
			continue;
		char aLabel[64];
		MatchMetricDisplayName(Metric.m_MetricId, Metric.m_ModeSchemaVersion, aLabel, sizeof(aLabel));
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
			StatsHeading(&s_ScrollRegion, &View, Localize("What this gametype counts"));
			StatsPeriodHeader(&s_ScrollRegion, &View, Localize("Metric"));
			WroteHeading = true;
		}
		PeriodRow(aLabel);
	}
	s_ScrollRegion.End();
}

void CMenus::RenderStats(CUIRect MainView)
{
	GameClient()->m_MenuBackground.ChangePosition(CMenuBackground::POS_DEMOS);
	if(!m_StatsInitialized)
		RefreshStats();
	// The page hands out its cached label slots in draw order, so the counter
	// starts over here and every label of this frame keeps the slot it had in
	// the last one.
	m_StatsLabelUiElementIndex = 0;

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
			StatsLabel(&Tabs, m_StatsSelectedMatch->m_Report.m_MapName.c_str(), 14.0f, TEXTALIGN_MR, {});
		RenderStatsMatchSummary(Content);
	}
	else
	{
		static CButtonContainer s_OverviewTab;
		static CButtonContainer s_MatchesTab;
		static CButtonContainer s_AggregateTab;
		Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
		if(DoButton_Menu(&s_OverviewTab, Localize("Overview"), m_StatsTab == EStatsTab::OVERVIEW, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_L))
			m_StatsTab = EStatsTab::OVERVIEW;
		Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
		if(DoButton_Menu(&s_MatchesTab, Localize("Matches"), m_StatsTab == EStatsTab::MATCHES, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_NONE))
			m_StatsTab = EStatsTab::MATCHES;
		Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
		if(DoButton_Menu(&s_AggregateTab, Localize("Statistics"), m_StatsTab == EStatsTab::AGGREGATE, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_R))
			m_StatsTab = EStatsTab::AGGREGATE;
		if(m_StatsTab == EStatsTab::OVERVIEW)
			RenderStatsOverview(Content);
		else if(m_StatsTab == EStatsTab::MATCHES)
			RenderStatsMatchList(Content);
		else
			RenderStatsAggregate(Content);
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
	if(m_StatsTab == EStatsTab::AGGREGATE && !m_StatsShowMatch && m_StatsInfo.m_NumMatches > 0)
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
	StatsLabel(&Status, aStatus, 10.0f, TEXTALIGN_ML, StatusProperties);
}
