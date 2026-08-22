#include <game/client/match_detail_view.h>

#include <gtest/gtest.h>

#include <string>

namespace
{
	const char *const MODE_ID = "vanilla.ctf@ddnet.org";
	const char *const RACE_MODE_ID = "ddrace.race@ddnet.org";

	CStoredMatch MakeMatch(const char *pModeId, bool Teams)
	{
		CStoredMatch Stored;
		Stored.m_Source = EMatchReportSource::SERVER_REPORT;
		Stored.m_Completeness = EMatchCompleteness::COMPLETE;
		Stored.m_LocalParticipantId = 0;
		Stored.m_Report.m_ModeId = pModeId;
		Stored.m_Report.m_ModeSchemaVersion = 1;
		Stored.m_Report.m_MapName = "map";
		Stored.m_Report.m_TickRate = 50;
		Stored.m_Report.m_DurationTicks = 6000;
		if(Teams)
			Stored.m_Report.m_vTeams = {{0, "Red"}, {1, "Blue"}};
		return Stored;
	}

	void AddParticipant(CStoredMatch &Stored, int ParticipantId, std::optional<int> TeamId, const char *pName, std::optional<int64_t> LeftTick)
	{
		CMatchParticipant Participant;
		Participant.m_ParticipantId = ParticipantId;
		Participant.m_TeamId = TeamId;
		Participant.m_DisplayName = pName;
		Participant.m_LeftTick = LeftTick;
		Stored.m_Report.m_vParticipants.push_back(std::move(Participant));
	}

	void AddMetric(CStoredMatch &Stored, EMatchSubjectKind SubjectKind, int SubjectId, const char *pSuffix, int64_t Value)
	{
		Stored.m_Report.m_vMetrics.push_back({SubjectKind, SubjectId, Stored.m_Report.m_ModeId + "/" + pSuffix, Value, EMatchMetricAggregation::SUM});
	}

	const CMatchDetailColumn *FindColumn(const CMatchDetailView &View, const char *pLabel)
	{
		for(const CMatchDetailColumn &Column : View.m_vColumns)
			if(Column.m_Label == pLabel)
				return &Column;
		return nullptr;
	}

	/**
	 * A report of the kind the client puts together by watching the game: it
	 * knows who killed whom with what, and nothing about shots or damage.
	 */
	CStoredMatch ObservedMatch()
	{
		CStoredMatch Stored = MakeMatch(MODE_ID, false);
		AddParticipant(Stored, 0, std::nullopt, "Alice", std::nullopt);
		AddParticipant(Stored, 1, std::nullopt, "Bob", std::nullopt);
		for(int Participant = 0; Participant < 2; ++Participant)
		{
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "score", 4 - Participant);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "kills", 4 - Participant);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "deaths", 2 + Participant);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_kills", 3 - Participant);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_3_kills", 1);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_deaths", 2 + Participant);
		}
		return Stored;
	}

	/**
	 * A report of the kind a server sends: everything the mode measured.
	 */
	CStoredMatch ServerMatch()
	{
		CStoredMatch Stored = ObservedMatch();
		for(int Participant = 0; Participant < 2; ++Participant)
		{
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_shots", 100 + Participant * 100);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_hits", 40);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_damage_done", 80);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_damage_taken", 60);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "weapon_1_deaths_holding", 1);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "assists", 2 - Participant);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "flag_captures", 1);
			AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, Participant, "health_picked_up", 10);
		}
		return Stored;
	}

	CStoredMatch RaceMatch()
	{
		CStoredMatch Stored = MakeMatch(RACE_MODE_ID, false);
		AddParticipant(Stored, 0, std::nullopt, "Alice", std::nullopt);
		AddParticipant(Stored, 1, std::nullopt, "Bob", std::nullopt);
		AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "personal_best_ticks", 9000);
		AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "map_rank", 12);
		AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 1, "personal_best_ticks", 12000);
		AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 1, "map_rank", 400);
		AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 1, "current_run_ticks", 3000);
		return Stored;
	}
}

TEST(MatchDetailView, ObservedReportOffersOnlyWhatItMeasured)
{
	const CStoredMatch Stored = ObservedMatch();
	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::SCORE));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::FRAGS));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::DEATHS));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::ACCURACY));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::SHOTS));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::DAMAGE));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::OBJECTIVES));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::RUN));
}

TEST(MatchDetailView, ServerReportOffersEveryCombatTab)
{
	const CStoredMatch Stored = ServerMatch();
	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::SCORE));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::FRAGS));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::DEATHS));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::ACCURACY));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::SHOTS));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::DAMAGE));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::OBJECTIVES));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::RUN));
}

TEST(MatchDetailView, RaceReportReplacesTheWeaponTabs)
{
	const CStoredMatch Stored = RaceMatch();
	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::SCORE));
	EXPECT_TRUE(View.HasTab(EStatsMatchTab::RUN));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::FRAGS));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::DEATHS));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::ACCURACY));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::SHOTS));
	EXPECT_FALSE(View.HasTab(EStatsMatchTab::DAMAGE));
	EXPECT_TRUE(View.m_vWeapons.empty());

	UpdateMatchDetailBest(View, EStatsMatchTab::RUN);
	ASSERT_EQ(View.m_vColumns.size(), 3);
	const CMatchDetailColumn *pBest = FindColumn(View, "Personal best");
	ASSERT_NE(pBest, nullptr);
	EXPECT_EQ(pBest->m_Format, EMatchDetailFormat::DURATION);
	// The faster run wins the column, not the larger number
	EXPECT_TRUE(pBest->m_LowerIsBetter);
	ASSERT_TRUE(pBest->m_Best.has_value());
	EXPECT_EQ(*pBest->m_Best, 9000);
	const CMatchDetailColumn *pRank = FindColumn(View, "Map rank");
	ASSERT_NE(pRank, nullptr);
	EXPECT_EQ(pRank->m_Format, EMatchDetailFormat::RANK);
	ASSERT_TRUE(pRank->m_Best.has_value());
	EXPECT_EQ(*pRank->m_Best, 12);
	// A participant that has no run of their own must not read as zero
	const CMatchDetailColumn *pRun = FindColumn(View, "Current run");
	ASSERT_NE(pRun, nullptr);
	EXPECT_FALSE(MatchDetailCell(View.m_vRows[0], *pRun).has_value());
	EXPECT_EQ(MatchDetailCell(View.m_vRows[1], *pRun).value_or(0), 3000);
}

TEST(MatchDetailView, WeaponColumnsCoverOnlyUsedWeapons)
{
	const CStoredMatch Stored = ServerMatch();
	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	ASSERT_EQ(View.m_vWeapons.size(), 2);
	EXPECT_EQ(View.m_vWeapons[0], WEAPON_GUN);
	EXPECT_EQ(View.m_vWeapons[1], WEAPON_GRENADE);

	UpdateMatchDetailBest(View, EStatsMatchTab::FRAGS);
	ASSERT_EQ(View.m_vColumns.size(), 3);
	EXPECT_EQ(View.m_vColumns[0].m_Weapon, WEAPON_GUN);
	EXPECT_EQ(View.m_vColumns[1].m_Weapon, WEAPON_GRENADE);
	EXPECT_EQ(View.m_vColumns[2].m_Weapon, -1);
	EXPECT_EQ(MatchDetailCell(View.m_vRows[0], View.m_vColumns[0]).value_or(0), 3);
	EXPECT_EQ(MatchDetailCell(View.m_vRows[0], View.m_vColumns[2]).value_or(0), 4);
}

TEST(MatchDetailView, BestCellPerColumn)
{
	const CStoredMatch Stored = ServerMatch();
	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	UpdateMatchDetailBest(View, EStatsMatchTab::FRAGS);
	ASSERT_EQ(View.m_vColumns.size(), 3);
	ASSERT_TRUE(View.m_vColumns[0].m_Best.has_value());
	EXPECT_EQ(*View.m_vColumns[0].m_Best, 3);
	EXPECT_EQ(*View.m_vColumns[2].m_Best, 4);

	// The second participant fired twice as much for the same number of hits,
	// so the better accuracy is the first one and not the larger hit count.
	UpdateMatchDetailBest(View, EStatsMatchTab::ACCURACY);
	ASSERT_FALSE(View.m_vColumns.empty());
	ASSERT_TRUE(View.m_vColumns[0].m_Best.has_value());
	EXPECT_EQ(*View.m_vColumns[0].m_Best, 400);
	char aBuffer[32];
	FormatMatchDetailCell(View.m_vRows[0], View.m_vColumns[0], View.m_TickRate, aBuffer, sizeof(aBuffer));
	EXPECT_STREQ(aBuffer, "40.0%");
	FormatMatchDetailCell(View.m_vRows[1], View.m_vColumns[0], View.m_TickRate, aBuffer, sizeof(aBuffer));
	EXPECT_STREQ(aBuffer, "20.0%");
}

TEST(MatchDetailView, UnknownMetricStillGetsAColumn)
{
	CStoredMatch Stored = ObservedMatch();
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "wall_runs", 7);
	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	UpdateMatchDetailBest(View, EStatsMatchTab::SCORE);
	const CMatchDetailColumn *pColumn = FindColumn(View, "Wall runs");
	ASSERT_NE(pColumn, nullptr);
	EXPECT_EQ(MatchDetailCell(View.m_vRows[0], *pColumn).value_or(0), 7);
	// Nobody reported it for the second participant, which is not the same as
	// having done it zero times.
	EXPECT_FALSE(MatchDetailCell(View.m_vRows[1], *pColumn).has_value());
}

TEST(MatchDetailView, TeamsAggregateTheirParticipants)
{
	CStoredMatch Stored = MakeMatch(MODE_ID, true);
	AddParticipant(Stored, 0, 0, "Alice", std::nullopt);
	AddParticipant(Stored, 1, 0, "Bob", std::nullopt);
	AddParticipant(Stored, 2, 1, "Carol", std::nullopt);
	Stored.m_Report.m_vStandings = {
		{EMatchSubjectKind::TEAM, 0, 1, EMatchOutcome::WIN},
		{EMatchSubjectKind::TEAM, 1, 2, EMatchOutcome::LOSS}};
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "score", 2);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "kills", 5);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 0, "flag_captures", 2);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 1, "score", 4);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 1, "kills", 3);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 1, "flag_captures", 1);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 2, "score", 9);
	AddMetric(Stored, EMatchSubjectKind::PARTICIPANT, 2, "kills", 9);
	AddMetric(Stored, EMatchSubjectKind::TEAM, 0, "score", 3);

	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	EXPECT_TRUE(View.m_HasTeams);
	ASSERT_EQ(View.m_vBlocks.size(), 2);
	EXPECT_EQ(View.m_vBlocks[0].m_DisplayName, "Red");
	ASSERT_TRUE(View.m_vBlocks[0].m_Outcome.has_value());
	EXPECT_EQ(*View.m_vBlocks[0].m_Outcome, EMatchOutcome::WIN);
	EXPECT_EQ(View.m_vBlocks[0].m_vPlaying.size(), 2);
	EXPECT_EQ(View.m_vBlocks[1].m_vPlaying.size(), 1);
	EXPECT_EQ(View.m_vBlocks[0].m_Summary.m_Combat.m_Total.m_Kills, 8);
	// The team captured three flags, its two players touched them three times
	EXPECT_EQ(View.m_vBlocks[0].m_Summary.m_FlagCaptures, 3);
	// What the mode said about the team beats the sum of six over its players
	EXPECT_EQ(View.m_vBlocks[0].m_Summary.m_Score, 3);
	EXPECT_EQ(View.m_vBlocks[1].m_Summary.m_Score, 9);
	EXPECT_TRUE(View.m_vBlocks[0].m_Summary.m_Local);
	EXPECT_FALSE(View.m_vBlocks[1].m_Summary.m_Local);
}

TEST(MatchDetailView, ParticipantsThatLeftAreKeptApart)
{
	CStoredMatch Stored = MakeMatch(MODE_ID, false);
	AddParticipant(Stored, 0, std::nullopt, "Alice", std::nullopt);
	AddParticipant(Stored, 1, std::nullopt, "Bob", 4000);
	AddParticipant(Stored, 2, std::nullopt, "Carol", std::nullopt);
	Stored.m_Report.m_vStandings = {
		{EMatchSubjectKind::PARTICIPANT, 2, 1, EMatchOutcome::WIN},
		{EMatchSubjectKind::PARTICIPANT, 0, 2, EMatchOutcome::LOSS},
		{EMatchSubjectKind::PARTICIPANT, 1, 3, EMatchOutcome::LOSS}};

	CMatchDetailView View;
	BuildMatchDetailView(Stored, View);
	EXPECT_FALSE(View.m_HasTeams);
	ASSERT_EQ(View.m_vBlocks.size(), 1);
	ASSERT_EQ(View.m_vBlocks[0].m_vPlaying.size(), 2);
	ASSERT_EQ(View.m_vBlocks[0].m_vLeft.size(), 1);
	// Best rank first, and the one that left is not mixed into the standings
	EXPECT_EQ(View.m_vRows[View.m_vBlocks[0].m_vPlaying[0]].m_pParticipant->m_DisplayName, "Carol");
	EXPECT_EQ(View.m_vRows[View.m_vBlocks[0].m_vPlaying[1]].m_pParticipant->m_DisplayName, "Alice");
	EXPECT_EQ(View.m_vRows[View.m_vBlocks[0].m_vLeft[0]].m_pParticipant->m_DisplayName, "Bob");
	EXPECT_TRUE(View.m_vRows[1].m_Left);
	EXPECT_TRUE(View.m_vRows[0].m_Local);
	EXPECT_FALSE(View.m_vRows[1].m_Local);
	EXPECT_EQ(View.m_vRows[2].m_Rank, 1);
	ASSERT_TRUE(View.m_vRows[2].m_Outcome.has_value());
	EXPECT_EQ(*View.m_vRows[2].m_Outcome, EMatchOutcome::WIN);
}
