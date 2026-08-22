#include <engine/shared/jsonwriter.h>

#include <game/client/match_report_view.h>
#include <game/match_report.h>

#include <gtest/gtest.h>

#include <string>

namespace
{
	CMatchReport SampleReport()
	{
		CMatchReport Report;
		Report.m_MatchId = CalculateUuid("sample-match");
		Report.m_GameUuid = CalculateUuid("sample-game");
		Report.m_ModeId = "ctf";
		Report.m_MapName = "ctf1";
		Report.m_MapSha256 = sha256("map", 3);
		Report.m_StartTimeUtc = 1720000000;
		Report.m_EndTimeUtc = 1720000120;
		Report.m_DurationTicks = 6000;
		Report.m_TickRate = 50;
		Report.m_RoundStartTick = 1234;
		Report.m_Termination = EMatchTermination::COMPLETED;
		Report.m_vTeams = {{0, "Red"}, {1, "Blue"}};
		Report.m_vParticipants = {
			{0, 0, "Alice", "A", 0, std::nullopt},
			{1, 1, "Bob", "B", 100, 5900}};
		Report.m_vStandings = {
			{EMatchSubjectKind::TEAM, 0, 1, EMatchOutcome::WIN},
			{EMatchSubjectKind::TEAM, 1, 2, EMatchOutcome::LOSS},
			{EMatchSubjectKind::PARTICIPANT, 0, 1, EMatchOutcome::WIN},
			{EMatchSubjectKind::PARTICIPANT, 1, 2, EMatchOutcome::LOSS}};
		Report.m_vMetrics = {
			{EMatchSubjectKind::PARTICIPANT, 0, "kills", MatchReportLimits::MAX_METRIC_VALUE},
			{EMatchSubjectKind::PARTICIPANT, 1, "kills", -MatchReportLimits::MAX_METRIC_VALUE},
			{EMatchSubjectKind::PARTICIPANT, 0, "weapon_1_shots", 7},
			{EMatchSubjectKind::PARTICIPANT, 0, "best_spree", 3, EMatchMetricAggregation::MAXIMUM},
			{EMatchSubjectKind::TEAM, 0, "score", 3},
			{EMatchSubjectKind::MATCH, std::nullopt, "sudden_death", 1, EMatchMetricAggregation::MATCH_ONLY}};
		return Report;
	}

	// the report has no comparison operator, its JSON says whether anything was lost
	std::string Json(const CMatchReport &Report)
	{
		CJsonStringWriter Writer;
		MatchReportWriteJson(Writer, Report);
		return Writer.GetOutputString();
	}

	std::string Packed(const CMatchReport &Report)
	{
		std::string Result;
		std::string Error;
		EXPECT_TRUE(MatchReportToPacked(Report, Result, &Error)) << Error;
		return Result;
	}
}

TEST(MatchReport, PackedRoundTrip)
{
	const CMatchReport Original = SampleReport();
	std::string Error;
	ASSERT_TRUE(MatchReportValidate(Original, &Error)) << Error;
	const std::string Payload = Packed(Original);
	CMatchReport Parsed;
	ASSERT_TRUE(MatchReportFromPacked(Payload.data(), Payload.size(), Parsed, &Error)) << Error;
	EXPECT_EQ(Json(Parsed), Json(Original));
	EXPECT_NE(Json(Original).find("\"sudden_death\""), std::string::npos);

	// a report without the optional parts
	CMatchReport Minimal = Original;
	Minimal.m_GameUuid.reset();
	Minimal.m_vTeams.clear();
	Minimal.m_vStandings.clear();
	Minimal.m_vMetrics.clear();
	for(CMatchParticipant &Participant : Minimal.m_vParticipants)
		Participant.m_TeamId.reset();
	const std::string MinimalPayload = Packed(Minimal);
	ASSERT_TRUE(MatchReportFromPacked(MinimalPayload.data(), MinimalPayload.size(), Parsed, &Error)) << Error;
	EXPECT_EQ(Json(Parsed), Json(Minimal));
}

TEST(MatchReport, PacksRepeatedMetricIdsOnce)
{
	CMatchReport Report = SampleReport();
	Report.m_vParticipants.clear();
	Report.m_vStandings.clear();
	Report.m_vMetrics.clear();
	for(int Participant = 0; Participant < 64; ++Participant)
	{
		Report.m_vParticipants.push_back({Participant, std::nullopt, "Player", "", 0, std::nullopt});
		for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
		{
			char aMetricId[32];
			str_format(aMetricId, sizeof(aMetricId), "weapon_%d_shots", Weapon);
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, Participant, aMetricId, 1234});
		}
	}
	std::string Error;
	ASSERT_TRUE(MatchReportValidate(Report, &Error)) << Error;
	// live statistics resend this every few seconds, it has to stay far below its JSON
	EXPECT_LT(Packed(Report).size() * 10, Json(Report).size());
}

TEST(MatchReport, RejectsMalformedPackedPayloads)
{
	const std::string Payload = Packed(SampleReport());
	CMatchReport Parsed;
	std::string Error;
	EXPECT_FALSE(MatchReportFromPacked(Payload.data(), 0, Parsed, &Error));
	// every field is read back, so no prefix of the payload may parse
	for(size_t Size = 1; Size < Payload.size(); ++Size)
		EXPECT_FALSE(MatchReportFromPacked(Payload.data(), Size, Parsed, &Error)) << "truncated to " << Size;
	std::string Trailing = Payload;
	Trailing.push_back(0);
	EXPECT_FALSE(MatchReportFromPacked(Trailing.data(), Trailing.size(), Parsed, &Error));
	std::string WrongVersion = Payload;
	WrongVersion[0] = 2;
	EXPECT_FALSE(MatchReportFromPacked(WrongVersion.data(), WrongVersion.size(), Parsed, &Error));
	const std::string Oversized(MatchReportLimits::MAX_PAYLOAD_SIZE + 1, '\x01');
	EXPECT_FALSE(MatchReportFromPacked(Oversized.data(), Oversized.size(), Parsed, &Error));
}

TEST(MatchReport, SurvivesEverySingleByteChangeInAPackedPayload)
{
	const std::string Payload = Packed(SampleReport());
	// the payload comes from the network: what comes out is a valid report or nothing, never a read past the end
	std::string Error;
	for(size_t Index = 0; Index < Payload.size(); ++Index)
	{
		for(const unsigned char Value : {0x00, 0x01, 0x7F, 0x80, 0xFF})
		{
			std::string Mutated = Payload;
			if((unsigned char)Mutated[Index] == Value)
				continue;
			Mutated[Index] = (char)Value;
			CMatchReport Parsed;
			if(MatchReportFromPacked(Mutated.data(), Mutated.size(), Parsed, &Error))
			{
				EXPECT_TRUE(MatchReportValidate(Parsed, &Error)) << "byte " << Index << " set to " << (int)Value << ": " << Error;
			}
		}
	}
}

TEST(MatchReport, ValidatesIds)
{
	for(const char *pId : {"ctf", "vanilla.ctf", "weapon_1_shots", "mod-mode.v2"})
		EXPECT_TRUE(IsValidMatchReportId(pId)) << pId;
	for(const std::string &Id : {std::string(), std::string("white space"), std::string("two/parts"), std::string("cätches"), std::string("mode@owner"), std::string(MatchReportLimits::MAX_ID_LENGTH + 1, 'x')})
		EXPECT_FALSE(IsValidMatchReportId(Id)) << Id;
	EXPECT_TRUE(IsValidMatchReportId(std::string(MatchReportLimits::MAX_ID_LENGTH, 'x')));
}

TEST(MatchReport, RejectsInvalidReferencesAndDuplicates)
{
	std::string Error;
	const auto ExpectInvalid = [&](auto &&Change) {
		CMatchReport Report = SampleReport();
		Change(Report);
		EXPECT_FALSE(MatchReportValidate(Report, &Error));
	};
	ExpectInvalid([](CMatchReport &Report) { Report.m_MatchId = UUID_ZEROED; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_ModeId = "not valid"; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vParticipants[0].m_TeamId = 5; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vParticipants[1].m_ParticipantId = 0; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vParticipants[1].m_LeftTick = Report.m_DurationTicks + 1; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vStandings.push_back(Report.m_vStandings.front()); });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vStandings[0].m_Rank = 0; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vStandings[2].m_SubjectId = 9; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vMetrics.push_back(Report.m_vMetrics.front()); });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vMetrics[0].m_MetricId = "not valid"; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vMetrics[0].m_Value = MatchReportLimits::MAX_METRIC_VALUE + 1; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vMetrics[0].m_SubjectId.reset(); });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vMetrics.back().m_SubjectId = 0; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_vMetrics[0].m_Aggregation = EMatchMetricAggregation::NUM; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_EndTimeUtc = MatchReportLimits::MAX_TIME_UTC + 1; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_DurationTicks = MatchReportLimits::MAX_DURATION_TICKS + 1; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_RoundStartTick = -1; });
	ExpectInvalid([](CMatchReport &Report) { Report.m_Termination = EMatchTermination::NUM; });

	CMatchReport Invalid = SampleReport();
	Invalid.m_vMetrics.push_back(Invalid.m_vMetrics.front());
	std::string Payload;
	EXPECT_FALSE(MatchReportToPacked(Invalid, Payload, &Error));
}

TEST(MatchReport, FindsKnownMetrics)
{
	int Weapon;
	const CMatchMetricInfo *pKills = FindMatchMetric("kills", &Weapon);
	ASSERT_NE(pKills, nullptr);
	EXPECT_EQ(Weapon, -1);
	EXPECT_EQ(pKills->m_CombatStat, MATCH_COMBAT_KILLS);
	EXPECT_EQ(pKills->m_Category, EMatchMetricCategory::COMBAT);
	const CMatchMetricInfo *pWeaponHits = FindMatchMetric("weapon_11_hits", &Weapon);
	ASSERT_NE(pWeaponHits, nullptr);
	EXPECT_EQ(Weapon, 11);
	EXPECT_EQ(pWeaponHits->m_CombatStat, MATCH_COMBAT_HITS);
	for(int Stat = 0; Stat < NUM_MATCH_COMBAT_STATS; Stat++)
		EXPECT_EQ(FindMatchMetric(MatchCombatStatInfo(Stat).m_pId)->m_CombatStat, Stat);
	EXPECT_EQ(FindMatchMetric("best_spree")->m_Aggregation, EMatchMetricAggregation::MAXIMUM);
	EXPECT_EQ(FindMatchMetric("playtime_ticks")->m_Format, EMatchMetricFormat::TICKS);
	EXPECT_EQ(FindMatchMetric("flag_captures")->m_Category, EMatchMetricCategory::OBJECTIVES);
	EXPECT_EQ(FindMatchMetric("catches")->m_Category, EMatchMetricCategory::OBJECTIVES);
	EXPECT_EQ(FindMatchMetric("map_rank")->m_Format, EMatchMetricFormat::RANK);
	EXPECT_EQ(FindMatchMetric("wall_runs"), nullptr);
	EXPECT_EQ(FindMatchMetric("weapon_x_hits"), nullptr);
	EXPECT_EQ(FindMatchMetric("weapon_1_catches"), nullptr);
	EXPECT_EQ(FindMatchMetric("weapon_1_"), nullptr);
}

TEST(MatchReportView, NamesAndFormatsMetrics)
{
	EXPECT_EQ(MatchMetricCategory("damage_done"), EMatchMetricCategory::COMBAT);
	EXPECT_EQ(MatchMetricCategory("flag_captures"), EMatchMetricCategory::OBJECTIVES);
	EXPECT_EQ(MatchMetricCategory("wall_runs"), EMatchMetricCategory::OTHER);
	EXPECT_TRUE(IsMatchCombatStatMetric("weapon_2_kills"));
	EXPECT_FALSE(IsMatchCombatStatMetric("suicides"));
	char aName[64];
	MatchMetricDisplayName("kills", aName, sizeof(aName));
	EXPECT_STREQ(aName, "Kills");
	// a metric of a mod still reads as words
	MatchMetricDisplayName("wall_runs", aName, sizeof(aName));
	EXPECT_STREQ(aName, "Wall runs");
	MatchWeaponDisplayName(11, aName, sizeof(aName));
	EXPECT_STREQ(aName, "Weapon 11");

	char aValue[64];
	FormatMatchMetricValue({EMatchSubjectKind::PARTICIPANT, 0, "map_rank", 7}, 50, aValue, sizeof(aValue));
	EXPECT_STREQ(aValue, "#7");
	FormatMatchMetricValue({EMatchSubjectKind::PARTICIPANT, 0, "current_run_ticks", 500}, 50, aValue, sizeof(aValue));
	EXPECT_STREQ(aValue, "00:10");
	FormatMatchMetricValue({EMatchSubjectKind::PARTICIPANT, 0, "wall_runs", 500}, 50, aValue, sizeof(aValue));
	EXPECT_STREQ(aValue, "500");
	FormatMatchDuration(MatchReportLimits::MAX_DURATION_TICKS, 1, aValue, sizeof(aValue));
	EXPECT_NE(aValue[0], '\0');
	FormatMatchSeconds(std::numeric_limits<int64_t>::max(), aValue, sizeof(aValue));
	EXPECT_NE(aValue[0], '\0');
	FormatMatchAccuracy(3, 4, aValue, sizeof(aValue));
	EXPECT_STREQ(aValue, "75.0%");
	// a shotgun blast can hit more often than it was shot
	FormatMatchAccuracy(5, 2, aValue, sizeof(aValue));
	EXPECT_STREQ(aValue, "100.0%");
	FormatMatchAccuracy(0, 0, aValue, sizeof(aValue));
	EXPECT_STREQ(aValue, "-");
}

TEST(MatchReportView, BuildsCombatStatsAndRanking)
{
	CMatchReport Report = SampleReport();
	Report.m_vMetrics = {
		{EMatchSubjectKind::PARTICIPANT, 1, "score", 9},
		{EMatchSubjectKind::PARTICIPANT, 1, "shots", 4},
		{EMatchSubjectKind::PARTICIPANT, 1, "hits", 3},
		{EMatchSubjectKind::PARTICIPANT, 1, "weapon_1_shots", 4},
		{EMatchSubjectKind::PARTICIPANT, 1, "weapon_1_damage_taken", 12},
		{EMatchSubjectKind::PARTICIPANT, 1, "weapon_11_hits", 9},
		{EMatchSubjectKind::PARTICIPANT, 1, "weapon_99_shots", 5},
		{EMatchSubjectKind::PARTICIPANT, 0, "shots", 100},
	};
	CMatchCombatStats Stats;
	BuildMatchCombatStats(Report, 1, Stats);
	EXPECT_EQ(Stats.m_Total.Value(MATCH_COMBAT_SHOTS), 4);
	EXPECT_EQ(Stats.m_Total.Value(MATCH_COMBAT_HITS), 3);
	ASSERT_EQ(Stats.m_vWeapons.size(), 12u);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_GUN].Value(MATCH_COMBAT_SHOTS), 4);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_GUN].Value(MATCH_COMBAT_DAMAGE_TAKEN), 12);
	EXPECT_EQ(Stats.m_vWeapons[11].Value(MATCH_COMBAT_HITS), 9);
	EXPECT_FALSE(Stats.m_vWeapons[WEAPON_HAMMER].HasData());

	CMatchProfile Profile;
	Profile.m_vMetrics = {{"dm", "weapon_4_shots", 8}, {"dm", "weapon_4_hits", 6}, {"dm", "suicides", 2}};
	BuildMatchCombatStats(Profile, Stats);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_LASER].Value(MATCH_COMBAT_SHOTS), 8);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_LASER].Value(MATCH_COMBAT_HITS), 6);
	EXPECT_FALSE(Stats.m_Total.HasData());

	// the standings order the board, the report keeps the order of joining
	CMatchReportRanking Ranking;
	Report.m_vStandings[2].m_Rank = 2;
	Report.m_vStandings[3].m_Rank = 1;
	Ranking.Update(Report);
	ASSERT_EQ(Ranking.Rows().size(), 2u);
	EXPECT_EQ(Ranking.Rows()[0].m_pParticipant->m_ParticipantId, 1);
	EXPECT_EQ(Ranking.Rows()[0].m_Score, 9);
	EXPECT_EQ(Ranking.Rows()[0].m_Combat.Value(MATCH_COMBAT_SHOTS), 4);
	EXPECT_FALSE(Ranking.Rows()[1].m_Score.has_value());
	ASSERT_NE(Ranking.Row(0), nullptr);
	EXPECT_EQ(Ranking.Row(0)->Rank(), 2);
}
