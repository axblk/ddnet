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

namespace
{
	const char *OutcomeName(const std::optional<EMatchOutcome> &Outcome)
	{
		return Outcome.has_value() ? MatchOutcomeDisplayName(*Outcome) : "-";
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
	m_StatsTab = Client()->State() == IClient::STATE_ONLINE ? EStatsTab::LIVE : EStatsTab::HISTORY;
	m_StatsInitialized = false;
	if(Client()->State() == IClient::STATE_ONLINE || Client()->State() == IClient::STATE_DEMOPLAYBACK)
		m_GamePage = PAGE_STATS;
	else
		SetMenuPage(PAGE_STATS);
	SetActive(true);
	if(m_StatsTab == EStatsTab::LIVE)
		GameClient()->RequestLiveStatsNow();
}

void CMenus::RefreshStats()
{
	m_StatsError.clear();
	m_vStatsHistory.clear();
	m_StatsSelectedMatch.reset();
	m_StatsProfile = {};
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
	const char *pSearch = m_StatsHistorySearchInput.GetString();
	m_vStatsHistory.erase(std::remove_if(m_vStatsHistory.begin(), m_vStatsHistory.end(), [&](const CMatchHistoryEntry &Entry) {
		const bool SearchMatches = pSearch[0] == '\0' || str_find_nocase(Entry.m_ModeId.c_str(), pSearch) != nullptr || str_find_nocase(Entry.m_MapName.c_str(), pSearch) != nullptr || str_find_nocase(Entry.m_OriginId.c_str(), pSearch) != nullptr;
		const bool QualityMatches = m_StatsQualityFilter == EStatsQualityFilter::ALL ||
					    (m_StatsQualityFilter == EStatsQualityFilter::COMPLETE && Entry.m_Completeness == EMatchCompleteness::COMPLETE) ||
					    (m_StatsQualityFilter == EStatsQualityFilter::SERVER && Entry.m_Source != EMatchReportSource::CLIENT_OBSERVED);
		return !SearchMatches || !QualityMatches;
	}),
		m_vStatsHistory.end());
	if(m_StatsSelectedIndex >= static_cast<int>(m_vStatsHistory.size()))
		m_StatsSelectedIndex = -1;
	if(m_StatsSelectedIndex < 0 && !m_vStatsHistory.empty())
		m_StatsSelectedIndex = 0;
	LoadSelectedStatsMatch();

	CMatchProfileFilter ProfileFilter;
	if(m_StatsPeriodDays > 0)
		ProfileFilter.m_SinceUtc = time_timestamp() - static_cast<int64_t>(m_StatsPeriodDays) * 24 * 60 * 60;
	ProfileFilter.m_ModeId = m_StatsProfileModeInput.GetString();
	if(!Journal.QueryProfile(ProfileFilter, m_StatsProfile, &m_StatsError) || !Journal.Info(m_StatsInfo, &m_StatsError))
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
	RefreshStats();
}

void CMenus::PopupConfirmDeleteStatsPeriod()
{
	const int64_t SinceUtc = m_StatsPeriodDays > 0 ? time_timestamp() - static_cast<int64_t>(m_StatsPeriodDays) * 24 * 60 * 60 : 0;
	const bool Success = m_StatsPeriodDays > 0 ? GameClient()->MatchJournal().DeleteMatchesSince(SinceUtc, &m_StatsError) : GameClient()->MatchJournal().DeleteAll(&m_StatsError);
	if(!Success)
	{
		PopupMessage(Localize("Error"), m_StatsError.c_str(), Localize("Ok"));
		return;
	}
	m_StatsSelectedIndex = -1;
	RefreshStats();
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

	static CButtonContainer s_LiveTab;
	static CButtonContainer s_HistoryTab;
	static CButtonContainer s_ProfileTab;
	static CButtonContainer s_DetailsTab;
	CUIRect Tab;
	Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
	if(DoButton_Menu(&s_LiveTab, Localize("Live"), m_StatsTab == EStatsTab::LIVE, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_L))
	{
		m_StatsTab = EStatsTab::LIVE;
		GameClient()->RequestLiveStatsNow();
	}
	Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
	if(DoButton_Menu(&s_HistoryTab, Localize("History"), m_StatsTab == EStatsTab::HISTORY, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_NONE))
		m_StatsTab = EStatsTab::HISTORY;
	Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
	if(DoButton_Menu(&s_ProfileTab, Localize("Profile"), m_StatsTab == EStatsTab::PROFILE, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_NONE))
		m_StatsTab = EStatsTab::PROFILE;
	Tabs.VSplitLeft(110.0f, &Tab, &Tabs);
	if(DoButton_Menu(&s_DetailsTab, Localize("Details"), m_StatsTab == EStatsTab::DETAILS, &Tab, BUTTONFLAG_LEFT, nullptr, IGraphics::CORNER_R))
		m_StatsTab = EStatsTab::DETAILS;
	Ui()->DoLabel(&Tabs, m_StatsTab == EStatsTab::LIVE ? Localize("Live from server") : Localize("Stored only on this device"), 11.0f, TEXTALIGN_MR);
	Content.Margin(5.0f, &Content);

	if(m_StatsTab == EStatsTab::HISTORY)
	{
		CUIRect Filters, Header, List;
		Content.HSplitTop(28.0f, &Filters, &Content);
		CUIRect FilterLabel, Search, Quality;
		Filters.VSplitLeft(125.0f, &FilterLabel, &Filters);
		Ui()->DoLabel(&FilterLabel, Localize("Mode, map or server"), 11.0f, TEXTALIGN_ML);
		Filters.VSplitRight(130.0f, &Filters, &Quality);
		Filters.VSplitRight(5.0f, &Search, nullptr);
		if(Ui()->DoEditBox_Search(&m_StatsHistorySearchInput, &Search, 12.0f, !Ui()->IsPopupOpen() && !GameClient()->m_GameConsole.IsActive()))
		{
			m_StatsSelectedIndex = -1;
			RefreshStats();
		}
		static CButtonContainer s_QualityButton;
		const char *pQuality = m_StatsQualityFilter == EStatsQualityFilter::ALL ? Localize("All reports") : m_StatsQualityFilter == EStatsQualityFilter::COMPLETE ? Localize("Complete only") :
																					    Localize("Server reports");
		if(DoButton_Menu(&s_QualityButton, pQuality, 0, &Quality))
		{
			m_StatsQualityFilter = m_StatsQualityFilter == EStatsQualityFilter::ALL ? EStatsQualityFilter::COMPLETE : m_StatsQualityFilter == EStatsQualityFilter::COMPLETE ? EStatsQualityFilter::SERVER :
																							  EStatsQualityFilter::ALL;
			m_StatsSelectedIndex = -1;
			RefreshStats();
		}
		Content.HSplitTop(ms_ListheaderHeight, &Header, &List);
		const bool CompactHistory = List.w < 1000.0f;
		Header.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_T, 5.0f);
		Ui()->DoLabel(&Header, CompactHistory ? Localize("Date  |  Map  |  Mode  |  Result  —  Server  |  Source") : Localize("Date  |  Duration  |  Server  |  Map  |  Mode  |  Score  |  Result  |  Source / completeness"), 12.0f, TEXTALIGN_ML);
		List.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.15f), IGraphics::CORNER_B, 5.0f);
		static CListBox s_ListBox;
		s_ListBox.DoStart(CompactHistory ? 34.0f : ms_ListheaderHeight, m_vStatsHistory.size(), 1, 3, m_StatsSelectedIndex, &List, false, IGraphics::CORNER_ALL, true);
		for(int Index = 0; Index < static_cast<int>(m_vStatsHistory.size()); ++Index)
		{
			const CMatchHistoryEntry &Entry = m_vStatsHistory[Index];
			const CListboxItem Item = s_ListBox.DoNextItem(&Entry, Index == m_StatsSelectedIndex);
			if(!Item.m_Visible)
				continue;
			char aDate[64];
			FormatMatchTimestamp(Entry.m_EndTimeUtc, aDate, sizeof(aDate));
			char aDuration[64];
			FormatMatchDuration(Entry.m_DurationTicks, Entry.m_TickRate, aDuration, sizeof(aDuration));
			const std::string Score = Entry.m_LocalScore.has_value() ? std::to_string(*Entry.m_LocalScore) : "-";
			SLabelProperties Properties;
			Properties.m_MaxWidth = Item.m_Rect.w;
			Properties.m_EllipsisAtEnd = true;
			if(CompactHistory)
			{
				CUIRect Primary, Secondary;
				Item.m_Rect.HSplitTop(17.0f, &Primary, &Secondary);
				char aPrimary[512];
				char aSecondary[512];
				str_format(aPrimary, sizeof(aPrimary), "%s  |  %s  |  %s  |  %s  |  %s  |  %s", aDate, aDuration, Entry.m_MapName.c_str(), Entry.m_ModeId.c_str(), Score.c_str(), OutcomeName(Entry.m_LocalOutcome));
				str_format(aSecondary, sizeof(aSecondary), "%s  |  %s / %s", Entry.m_OriginId.c_str(), MatchReportSourceDisplayName(Entry.m_Source), MatchCompletenessDisplayName(Entry.m_Completeness));
				Ui()->DoLabel(&Primary, aPrimary, 11.0f, TEXTALIGN_ML, Properties);
				Ui()->DoLabel(&Secondary, aSecondary, 10.0f, TEXTALIGN_ML, Properties);
			}
			else
			{
				char aLine[768];
				str_format(aLine, sizeof(aLine), "%s  |  %s  |  %s  |  %s  |  %s  |  %s  |  %s  |  %s / %s", aDate, aDuration, Entry.m_OriginId.c_str(), Entry.m_MapName.c_str(), Entry.m_ModeId.c_str(), Score.c_str(), OutcomeName(Entry.m_LocalOutcome), MatchReportSourceDisplayName(Entry.m_Source), MatchCompletenessDisplayName(Entry.m_Completeness));
				Ui()->DoLabel(&Item.m_Rect, aLine, 12.0f, TEXTALIGN_ML, Properties);
			}
		}
		const int NewSelectedIndex = s_ListBox.DoEnd();
		if(NewSelectedIndex != m_StatsSelectedIndex)
		{
			m_StatsSelectedIndex = NewSelectedIndex;
			LoadSelectedStatsMatch();
		}
		if(s_ListBox.WasItemActivated() && m_StatsSelectedMatch.has_value())
			m_StatsTab = EStatsTab::DETAILS;
	}
	else
	{
		static CScrollRegion s_ScrollRegion;
		CScrollRegionParams ScrollParams;
		ScrollParams.m_ScrollUnit = 40.0f;
		s_ScrollRegion.Begin(&Content, &ScrollParams);
		const auto RenderLine = [&](const char *pLabel, const std::string &Value) {
			CUIRect Line, Label, Data;
			Content.HSplitTop(21.0f, &Line, &Content);
			if(!s_ScrollRegion.AddRect(Line))
				return;
			Line.VSplitLeft(190.0f, &Label, &Data);
			Ui()->DoLabel(&Label, pLabel, 12.0f, TEXTALIGN_ML);
			Ui()->DoLabel(&Data, Value.c_str(), 12.0f, TEXTALIGN_ML);
		};
		const auto RenderHeading = [&](const char *pText) {
			CUIRect Line;
			Content.HSplitTop(26.0f, &Line, &Content);
			if(s_ScrollRegion.AddRect(Line))
				Ui()->DoLabel(&Line, pText, 16.0f, TEXTALIGN_ML);
		};
		const auto RenderWeaponIcon = [&](int Weapon, CUIRect Rect) {
			if(Weapon < 0 || Weapon >= NUM_WEAPONS || !GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon].IsValid())
				return;
			const CDataWeaponspec &WeaponSpec = g_pData->m_Weapons.m_aId[Weapon];
			float ScaleX;
			float ScaleY;
			Graphics()->GetSpriteScale(WeaponSpec.m_pSpriteBody, ScaleX, ScaleY);
			const float Width = WeaponSpec.m_VisualSize * ScaleX;
			const float Height = WeaponSpec.m_VisualSize * ScaleY;
			const float Scale = std::min((Rect.w - 4.0f) / Width, (Rect.h - 4.0f) / Height);
			Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[Weapon]);
			Graphics()->QuadsBegin();
			Graphics()->DrawSprite(Rect.x + Rect.w / 2.0f, Rect.y + Rect.h / 2.0f, Width * Scale, Height * Scale);
			Graphics()->QuadsEnd();
		};
		const auto RenderWeaponTable = [&](const char *pHeading, const CMatchCombatStats &Stats) {
			if(!Stats.HasData())
				return;
			RenderHeading(pHeading);
			const auto SplitColumns = [](CUIRect Row) {
				std::array<CUIRect, 6> aColumns;
				const float WeaponWidth = std::clamp(Row.w * 0.28f, 135.0f, 220.0f);
				Row.VSplitLeft(WeaponWidth, aColumns.data(), &Row);
				for(int Column = 1; Column < 5; ++Column)
					Row.VSplitLeft(Row.w / (6 - Column), &aColumns[Column], &Row);
				aColumns[5] = Row;
				return aColumns;
			};
			CUIRect Header;
			Content.HSplitTop(24.0f, &Header, &Content);
			if(s_ScrollRegion.AddRect(Header))
			{
				Header.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.18f), IGraphics::CORNER_T, 4.0f);
				const auto aColumns = SplitColumns(Header);
				const char *apLabels[] = {Localize("Weapon"), Localize("Shots"), Localize("Hits"), Localize("Accuracy"), Localize("Damage done"), Localize("Damage taken")};
				for(int Column = 0; Column < 6; ++Column)
					Ui()->DoLabel(&aColumns[Column], apLabels[Column], 11.0f, Column == 0 ? TEXTALIGN_ML : TEXTALIGN_MC);
			}
			const auto RenderWeaponRow = [&](const CMatchWeaponStats &WeaponStats, bool Total) {
				CUIRect Row;
				Content.HSplitTop(32.0f, &Row, &Content);
				if(!s_ScrollRegion.AddRect(Row))
					return;
				Row.Draw(Total ? ColorRGBA(0.15f, 0.45f, 0.7f, 0.24f) : ColorRGBA(0.0f, 0.0f, 0.0f, 0.14f), IGraphics::CORNER_NONE, 0.0f);
				auto aColumns = SplitColumns(Row);
				CUIRect Icon;
				aColumns[0].VSplitLeft(36.0f, &Icon, aColumns.data());
				if(!Total)
					RenderWeaponIcon(WeaponStats.m_Weapon, Icon);
				Ui()->DoLabel(aColumns.data(), Total ? Localize("Total") : MatchWeaponDisplayName(WeaponStats.m_Weapon), 12.0f, TEXTALIGN_ML);
				Ui()->DoLabel(&aColumns[1], std::to_string(WeaponStats.m_Shots).c_str(), 12.0f, TEXTALIGN_MC);
				Ui()->DoLabel(&aColumns[2], std::to_string(WeaponStats.m_Hits).c_str(), 12.0f, TEXTALIGN_MC);
				char aAccuracy[32];
				FormatMatchAccuracy(WeaponStats.m_Hits, WeaponStats.m_Shots, aAccuracy, sizeof(aAccuracy));
				if(WeaponStats.m_Shots > 0)
				{
					CUIRect AccuracyBar = aColumns[3];
					AccuracyBar.HMargin(7.0f, &AccuracyBar);
					AccuracyBar.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_ALL, 3.0f);
					AccuracyBar.w *= std::clamp(static_cast<float>(WeaponStats.m_Hits) / static_cast<float>(WeaponStats.m_Shots), 0.0f, 1.0f);
					AccuracyBar.Draw(ColorRGBA(0.2f, 0.7f, 1.0f, 0.42f), IGraphics::CORNER_ALL, 3.0f);
				}
				Ui()->DoLabel(&aColumns[3], aAccuracy, 12.0f, TEXTALIGN_MC);
				Ui()->DoLabel(&aColumns[4], std::to_string(WeaponStats.m_DamageDone).c_str(), 12.0f, TEXTALIGN_MC);
				Ui()->DoLabel(&aColumns[5], std::to_string(WeaponStats.m_DamageTaken).c_str(), 12.0f, TEXTALIGN_MC);
			};
			RenderWeaponRow(Stats.m_Total, true);
			for(const CMatchWeaponStats &WeaponStats : Stats.m_aWeapons)
				if(WeaponStats.HasData())
					RenderWeaponRow(WeaponStats, false);
		};

		const CStoredMatch *pDisplayedMatch = m_StatsTab == EStatsTab::LIVE ? GameClient()->LiveStats(Client()->FocusedSessionId()) : m_StatsSelectedMatch.has_value() ? &*m_StatsSelectedMatch :
																						 nullptr;
		if(m_StatsTab == EStatsTab::PROFILE)
		{
			CUIRect PeriodLine;
			Content.HSplitTop(26.0f, &PeriodLine, &Content);
			s_ScrollRegion.AddRect(PeriodLine);
			static CButtonContainer s_SevenDays;
			static CButtonContainer s_ThirtyDays;
			static CButtonContainer s_AllTime;
			const auto PeriodButton = [&](CButtonContainer &Button, const char *pText, int Days) {
				CUIRect Rect;
				PeriodLine.VSplitLeft(100.0f, &Rect, &PeriodLine);
				if(DoButton_Menu(&Button, pText, m_StatsPeriodDays == Days, &Rect))
				{
					m_StatsPeriodDays = Days;
					RefreshStats();
				}
			};
			PeriodButton(s_SevenDays, Localize("7 days"), 7);
			PeriodButton(s_ThirtyDays, Localize("30 days"), 30);
			PeriodButton(s_AllTime, Localize("All time"), 0);
			CUIRect ModeLabel, ModeInput;
			PeriodLine.VSplitLeft(42.0f, &ModeLabel, &ModeInput);
			Ui()->DoLabel(&ModeLabel, Localize("Mode"), 11.0f, TEXTALIGN_ML);
			if(Ui()->DoEditBox(&m_StatsProfileModeInput, &ModeInput, 12.0f))
				RefreshStats();
			RenderHeading(Localize("Summary"));
			RenderLine(Localize("Matches"), std::to_string(m_StatsProfile.m_Matches));
			RenderLine(Localize("Wins"), std::to_string(m_StatsProfile.m_Wins));
			RenderLine(Localize("Losses"), std::to_string(m_StatsProfile.m_Losses));
			RenderLine(Localize("Draws"), std::to_string(m_StatsProfile.m_Draws));
			char aWinRate[32];
			str_format(aWinRate, sizeof(aWinRate), "%.1f%%", m_StatsProfile.m_Matches > 0 ? 100.0 * m_StatsProfile.m_Wins / m_StatsProfile.m_Matches : 0.0);
			RenderLine(Localize("Win rate"), aWinRate);
			char aPlaytime[64];
			FormatMatchSeconds(m_StatsProfile.m_PlaytimeSeconds, aPlaytime, sizeof(aPlaytime));
			RenderLine(Localize("Playtime"), aPlaytime);
			RenderWeaponTable(Localize("Weapon performance"), BuildMatchCombatStats(m_StatsProfile));
			const bool HasOtherMetrics = std::any_of(m_StatsProfile.m_vMetrics.begin(), m_StatsProfile.m_vMetrics.end(), [](const CMatchMetricAggregate &Metric) { return !IsMatchCombatStatMetric(Metric.m_MetricId, Metric.m_ModeSchemaVersion); });
			if(HasOtherMetrics)
				RenderHeading(Localize("Other metrics"));
			for(const CMatchMetricAggregate &Metric : m_StatsProfile.m_vMetrics)
			{
				if(IsMatchCombatStatMetric(Metric.m_MetricId, Metric.m_ModeSchemaVersion))
					continue;
				std::string Label = MatchMetricDisplayName(Metric.m_MetricId, Metric.m_ModeSchemaVersion);
				if(m_StatsProfileModeInput.GetString()[0] == '\0')
					Label += " (" + Metric.m_ModeId + " v" + std::to_string(Metric.m_ModeSchemaVersion) + ")";
				RenderLine(Label.c_str(), std::to_string(Metric.m_Value));
			}
		}
		else if(pDisplayedMatch != nullptr)
		{
			const CStoredMatch &Stored = *pDisplayedMatch;
			const CMatchReport &Report = Stored.m_Report;
			RenderHeading(Localize("Overview"));
			RenderLine(Localize("Map"), Report.m_MapName);
			RenderLine(Localize("Mode"), Report.m_ModeId);
			RenderLine(Localize("Source"), MatchReportSourceDisplayName(Stored.m_Source));
			if(m_StatsTab != EStatsTab::LIVE)
			{
				RenderLine(Localize("Termination"), MatchTerminationDisplayName(Report.m_Termination));
				RenderLine(Localize("Completeness"), MatchCompletenessDisplayName(Stored.m_Completeness));
			}
			RenderLine(Localize("Server endpoint"), Stored.m_OriginId);
			char aDuration[64];
			FormatMatchDuration(Report.m_DurationTicks, Report.m_TickRate, aDuration, sizeof(aDuration));
			RenderLine(Localize("Duration"), aDuration);
			RenderHeading(Localize("Participants"));
			for(const CMatchParticipant &Participant : Report.m_vParticipants)
			{
				std::string Name = Participant.m_DisplayName;
				if(!Participant.m_Clan.empty())
					Name += " (" + Participant.m_Clan + ")";
				RenderLine((Localize("Player") + std::string(" ") + std::to_string(Participant.m_ParticipantId)).c_str(), Name);
			}
			if(!Report.m_vStandings.empty())
			{
				RenderHeading(Localize("Standings"));
				for(const CMatchStanding &Standing : Report.m_vStandings)
					RenderLine((std::string(MatchSubjectKindName(Standing.m_SubjectKind)) + " " + std::to_string(Standing.m_SubjectId)).c_str(), std::to_string(Standing.m_Rank) + " / " + MatchOutcomeDisplayName(Standing.m_Outcome));
			}
			if(Stored.m_LocalParticipantId.has_value())
			{
				const auto It = std::find_if(Report.m_vParticipants.begin(), Report.m_vParticipants.end(), [&](const CMatchParticipant &Participant) { return Participant.m_ParticipantId == *Stored.m_LocalParticipantId; });
				if(It != Report.m_vParticipants.end())
				{
					const std::string Heading = std::string(Localize("Weapon performance")) + " — " + It->m_DisplayName;
					RenderWeaponTable(Heading.c_str(), BuildMatchCombatStats(Report, It->m_ParticipantId));
				}
			}
			else
			{
				for(const CMatchParticipant &Participant : Report.m_vParticipants)
				{
					const CMatchCombatStats Stats = BuildMatchCombatStats(Report, Participant.m_ParticipantId);
					if(Stats.HasData())
					{
						const std::string Heading = std::string(Localize("Weapon performance")) + " — " + Participant.m_DisplayName;
						RenderWeaponTable(Heading.c_str(), Stats);
					}
				}
			}
			for(const EMatchMetricCategory Category : {EMatchMetricCategory::OVERVIEW, EMatchMetricCategory::COMBAT, EMatchMetricCategory::WEAPONS, EMatchMetricCategory::OBJECTIVES, EMatchMetricCategory::OTHER})
			{
				const bool HasCategory = std::any_of(Report.m_vMetrics.begin(), Report.m_vMetrics.end(), [Category, &Report](const CMatchMetric &Metric) { return !IsMatchCombatStatMetric(Metric.m_MetricId, Report.m_ModeSchemaVersion) && MatchMetricCategory(Metric.m_MetricId, Report.m_ModeSchemaVersion) == Category; });
				if(!HasCategory)
					continue;
				RenderHeading(MatchMetricCategoryDisplayName(Category));
				for(const CMatchMetric &Metric : Report.m_vMetrics)
					if(!IsMatchCombatStatMetric(Metric.m_MetricId, Report.m_ModeSchemaVersion) && MatchMetricCategory(Metric.m_MetricId, Report.m_ModeSchemaVersion) == Category)
					{
						char aValue[64];
						FormatMatchMetricValue(Metric, Report.m_ModeSchemaVersion, Report.m_TickRate, aValue, sizeof(aValue));
						RenderLine(MatchMetricDisplayName(Metric.m_MetricId, Report.m_ModeSchemaVersion), aValue);
					}
			}
		}
		else
		{
			RenderHeading(Localize("No match selected"));
		}
		s_ScrollRegion.End();
	}

	Buttons.HMargin(2.0f, &Buttons);
	CUIRect Button;
	static CButtonContainer s_RefreshButton;
	Buttons.VSplitLeft(85.0f, &Button, &Buttons);
	if(DoButton_Menu(&s_RefreshButton, Localize("Refresh"), 0, &Button))
	{
		if(m_StatsTab == EStatsTab::LIVE)
			GameClient()->RequestLiveStatsNow();
		else
			RefreshStats();
	}
	Buttons.VSplitLeft(5.0f, nullptr, &Buttons);
	if(m_StatsTab != EStatsTab::LIVE && m_StatsSelectedMatch.has_value())
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
	if(m_StatsTab == EStatsTab::PROFILE && m_StatsInfo.m_NumMatches > 0)
	{
		static CButtonContainer s_DeletePeriodButton;
		Buttons.VSplitRight(105.0f, &Buttons, &Button);
		if(DoButton_Menu(&s_DeletePeriodButton, m_StatsPeriodDays > 0 ? Localize("Delete period") : Localize("Delete all"), 0, &Button))
			PopupConfirm(Localize("Delete statistics"), m_StatsPeriodDays > 0 ? Localize("Delete all matches in the selected period?") : Localize("Delete all locally stored match statistics?"), Localize("Yes"), Localize("No"), &CMenus::PopupConfirmDeleteStatsPeriod);
	}

	char aStatus[256];
	if(m_StatsTab == EStatsTab::LIVE)
	{
		const CStoredMatch *pLive = GameClient()->LiveStats(Client()->FocusedSessionId());
		str_copy(aStatus, pLive != nullptr ? Localize("Updated automatically every 10 seconds") : Localize("Waiting for live statistics from the server"));
	}
	else if(!m_StatsError.empty())
		str_copy(aStatus, m_StatsError.c_str());
	else
	{
		char aOldest[64] = "-";
		if(m_StatsInfo.m_OldestMatchUtc.has_value())
			FormatMatchTimestamp(*m_StatsInfo.m_OldestMatchUtc, aOldest, sizeof(aOldest));
		str_format(aStatus, sizeof(aStatus), Localize("%d matches, %.1f MiB, oldest: %s"), m_StatsInfo.m_NumMatches, m_StatsInfo.m_DatabaseSize / (1024.0 * 1024.0), aOldest);
	}
	Ui()->DoLabel(&Status, aStatus, 11.0f, TEXTALIGN_ML);
}
