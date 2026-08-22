#include <game/client/match_report_view.h>
#include <game/match_report.h>

#include <gtest/gtest.h>

#include <array>
#include <limits>

namespace
{
	CMatchReport SampleReport()
	{
		CMatchReport Report;
		Report.m_MatchId = CalculateUuid("sample-match@ddnet.org");
		Report.m_GameUuid = CalculateUuid("sample-game@ddnet.org");
		Report.m_ModeId = "vanilla.ctf@ddnet.org";
		Report.m_ModeSchemaVersion = 1;
		Report.m_MapName = "ctf1";
		Report.m_MapSha256 = sha256("map", 3);
		Report.m_StartTimeUtc = 1720000000;
		Report.m_EndTimeUtc = 1720000120;
		Report.m_DurationTicks = 6000;
		Report.m_TickRate = 50;
		Report.m_Termination = EMatchTermination::COMPLETED;
		Report.m_Ranked = false;
		Report.m_UnrankedReason = "public_server";
		Report.m_vTeams = {{0, "Red"}, {1, "Blue"}};
		Report.m_vParticipants = {
			{0, 0, "Alice", "A", 0, std::nullopt, false},
			{1, 1, "Bob", "B", 100, 5900, false}};
		Report.m_vStandings = {
			{EMatchSubjectKind::TEAM, 0, 1, EMatchOutcome::WIN},
			{EMatchSubjectKind::TEAM, 1, 2, EMatchOutcome::LOSS}};
		Report.m_vMetrics = {
			{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.ctf@ddnet.org/kills", std::numeric_limits<int64_t>::max()},
			{EMatchSubjectKind::TEAM, 0, "vanilla.ctf@ddnet.org/score", 3}};
		return Report;
	}
}

TEST(MatchReport, CanonicalJsonRoundTrip)
{
	const CMatchReport Original = SampleReport();
	std::string Json;
	std::string Error;
	ASSERT_TRUE(MatchReportToJson(Original, Json, &Error)) << Error;
	EXPECT_LT(Json.find("\"match_id\""), Json.find("\"game_uuid\""));
	EXPECT_LT(Json.find("\"teams\""), Json.find("\"participants\""));
	EXPECT_NE(Json.find("9223372036854775807"), std::string::npos);

	CMatchReport Parsed;
	ASSERT_TRUE(MatchReportFromJson(Json.data(), Json.size(), Parsed, &Error)) << Error;
	std::string Reserialized;
	ASSERT_TRUE(MatchReportToJson(Parsed, Reserialized, &Error)) << Error;
	EXPECT_EQ(Reserialized, Json);
}

TEST(MatchReport, PackedRoundTrip)
{
	const CMatchReport Original = SampleReport();
	std::string Error;
	std::string Json;
	ASSERT_TRUE(MatchReportToJson(Original, Json, &Error)) << Error;
	std::string Packed;
	ASSERT_TRUE(MatchReportToPacked(Original, Packed, &Error)) << Error;
	EXPECT_LT(Packed.size(), Json.size());

	// The report has no comparison operator, so the canonical JSON of what came
	// back out is what says the packed round trip lost nothing.
	CMatchReport Parsed;
	ASSERT_TRUE(MatchReportFromPacked(Packed.data(), Packed.size(), Parsed, &Error)) << Error;
	std::string Reserialized;
	ASSERT_TRUE(MatchReportToJson(Parsed, Reserialized, &Error)) << Error;
	EXPECT_EQ(Reserialized, Json);
}

TEST(MatchReport, RejectsMalformedPackedPayloads)
{
	std::string Error;
	std::string Packed;
	ASSERT_TRUE(MatchReportToPacked(SampleReport(), Packed, &Error)) << Error;

	CMatchReport Parsed;
	EXPECT_FALSE(MatchReportFromPacked(Packed.data(), 0, Parsed, &Error));
	// Every field is read back, so no prefix of the payload may parse.
	for(size_t Size = 1; Size < Packed.size(); ++Size)
		EXPECT_FALSE(MatchReportFromPacked(Packed.data(), Size, Parsed, &Error)) << "truncated to " << Size;
	std::string Trailing = Packed;
	Trailing.push_back(0);
	EXPECT_FALSE(MatchReportFromPacked(Trailing.data(), Trailing.size(), Parsed, &Error));
	std::string WrongVersion = Packed;
	WrongVersion[0] = 2;
	EXPECT_FALSE(MatchReportFromPacked(WrongVersion.data(), WrongVersion.size(), Parsed, &Error));
}

TEST(MatchReport, SurvivesEverySingleByteChangeInAPackedPayload)
{
	std::string Error;
	std::string Packed;
	ASSERT_TRUE(MatchReportToPacked(SampleReport(), Packed, &Error)) << Error;

	// The payload arrives from the network, so every byte of it is an input a
	// server can choose. What comes out has to be a report that passes
	// validation or nothing at all, and never a read past the payload, which is
	// what the sanitizer build turns into a failure here.
	for(size_t Index = 0; Index < Packed.size(); ++Index)
	{
		for(const unsigned char Value : {0x00, 0x01, 0x7F, 0x80, 0xFF})
		{
			std::string Mutated = Packed;
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

TEST(MatchReport, CanonicalJsonGoldenBytesAndDigest)
{
	CMatchReport Report;
	ASSERT_EQ(ParseUuid(&Report.m_MatchId, "11111111-2222-3333-4444-555555555555"), 0);
	Report.m_ModeId = "mødé@ddnet.org";
	Report.m_MapName = "Map \"雪\"\n";
	Report.m_MapSha256 = sha256("", 0);
	Report.m_EndTimeUtc = MatchReportLimits::MAX_TIME_UTC;
	Report.m_DurationTicks = MatchReportLimits::MAX_DURATION_TICKS;
	Report.m_TickRate = 50;
	Report.m_vParticipants = {{0, std::nullopt, "Tee \"雪\"\n", "C\t", 0, std::nullopt, false}};
	Report.m_vStandings = {{EMatchSubjectKind::PARTICIPANT, 0, 1, EMatchOutcome::FINISHED}};
	Report.m_vMetrics = {{EMatchSubjectKind::PARTICIPANT, 0, "mødé@ddnet.org/score", std::numeric_limits<int64_t>::min()}};

	const std::string Expected = R"json({
	"match_id": "11111111-2222-3333-4444-555555555555",
	"game_uuid": null,
	"report_schema_version": 1,
	"mode_id": "mødé@ddnet.org",
	"mode_schema_version": 1,
	"map_name": "Map \"雪\"\n",
	"map_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
	"start_time_utc": 0,
	"end_time_utc": 32503680000,
	"duration_ticks": 31622400000,
	"tick_rate": 50,
	"round_start_tick": 0,
	"termination": "completed",
	"ranked": false,
	"unranked_reason": null,
	"teams": [
	],
	"participants": [
		{
			"participant_id": 0,
			"team_id": null,
			"display_name": "Tee \"雪\"\n",
			"clan": "C\t",
			"joined_tick": 0,
			"left_tick": null,
			"bot": false
		}
	],
	"standings": [
		{
			"subject_kind": "participant",
			"subject_id": 0,
			"rank": 1,
			"outcome": "finished"
		}
	],
	"metrics": [
		{
			"subject_kind": "participant",
			"subject_id": 0,
			"metric_id": "mødé@ddnet.org/score",
			"value": -9223372036854775808,
			"aggregation": "sum"
		}
	]
}
)json";
	SHA256_DIGEST ExpectedDigest;
	ASSERT_EQ(sha256_from_str(&ExpectedDigest, "042bfe9c4fe68b30a20e59cf4a3183df28e7ab942dc80d34565f0d6508e06c26"), 0);

	std::string Json;
	std::string Error;
	ASSERT_TRUE(MatchReportToJson(Report, Json, &Error)) << Error;
	EXPECT_EQ(Json, Expected);
	EXPECT_EQ(sha256(Json.data(), Json.size()), ExpectedDigest);
}

TEST(MatchReport, ValidatesNamespacedIds)
{
	const std::array<std::string, 3> aValidModeIds = {"vanilla.ctf@ddnet.org", "mødé@owner.example", "mode+variant@owner"};
	for(const std::string &ModeId : aValidModeIds)
		EXPECT_TRUE(IsValidMatchReportModeId(ModeId)) << ModeId;
	const std::array<std::string, 8> aInvalidModeIds = {"", "@owner", "mode@", "mode@@owner", "mode@owner/extra", "mode @owner", "mode@\towner", std::string("mode@") + "\xc3\x28"};
	for(const std::string &ModeId : aInvalidModeIds)
		EXPECT_FALSE(IsValidMatchReportModeId(ModeId)) << ModeId;

	const std::string ModeId = "mødé@owner.example";
	const std::array<std::string, 3> aValidMetricIds = {ModeId + "/score", ModeId + "/flag_captures", ModeId + "/Weapon-2.kills"};
	for(const std::string &MetricId : aValidMetricIds)
		EXPECT_TRUE(IsValidMatchReportMetricId(MetricId, ModeId)) << MetricId;
	const std::array<std::string, 6> aInvalidMetricIds = {ModeId, ModeId + "/", ModeId + "/two/parts", ModeId + "/white space", ModeId + "/cätches", "other@owner.example/score"};
	for(const std::string &MetricId : aInvalidMetricIds)
		EXPECT_FALSE(IsValidMatchReportMetricId(MetricId, ModeId)) << MetricId;
	EXPECT_FALSE(IsValidMatchReportMetricId(ModeId + "/" + std::string(MatchReportLimits::MAX_METRIC_ID_LENGTH, 'x'), ModeId));
}

TEST(MatchReport, RejectsInvalidReferencesAndDuplicates)
{
	CMatchReport Report = SampleReport();
	Report.m_vParticipants[0].m_TeamId = 5;
	std::string Error;
	EXPECT_FALSE(MatchReportValidate(Report, &Error));

	Report = SampleReport();
	Report.m_vMetrics.push_back(Report.m_vMetrics.front());
	EXPECT_FALSE(MatchReportValidate(Report, &Error));

	Report = SampleReport();
	Report.m_ModeId = "not-namespaced";
	EXPECT_FALSE(MatchReportValidate(Report, &Error));

	Report = SampleReport();
	Report.m_vMetrics.front().m_MetricId = "vanilla.dm@ddnet.org/kills";
	EXPECT_FALSE(MatchReportValidate(Report, &Error));

	Report = SampleReport();
	Report.m_EndTimeUtc = MatchReportLimits::MAX_TIME_UTC + 1;
	EXPECT_FALSE(MatchReportValidate(Report, &Error));
	Report = SampleReport();
	Report.m_DurationTicks = MatchReportLimits::MAX_DURATION_TICKS + 1;
	EXPECT_FALSE(MatchReportValidate(Report, &Error));
	Report = SampleReport();
	Report.m_RoundStartTick = -1;
	EXPECT_FALSE(MatchReportValidate(Report, &Error));
	Report = SampleReport();
	Report.m_vMetrics.front().m_Aggregation = EMatchMetricAggregation::INVALID;
	EXPECT_FALSE(MatchReportValidate(Report, &Error));
}

TEST(MatchReport, RejectsOversizedAndMalformedPayloads)
{
	CMatchReport Report;
	std::string Error;
	const std::string Oversized(MatchReportLimits::MAX_PAYLOAD_SIZE + 1, 'x');
	EXPECT_FALSE(MatchReportFromJson(Oversized.data(), Oversized.size(), Report, &Error));
	EXPECT_FALSE(MatchReportFromJson("{}", 2, Report, &Error));
}

TEST(MatchReportBuilder, AccumulatesAndFinalizesOnce)
{
	CMatchReport Report = SampleReport();
	Report.m_vMetrics.clear();
	CMatchReportBuilder Builder(std::move(Report));
	EXPECT_TRUE(Builder.AddMetricValue(EMatchSubjectKind::PARTICIPANT, 0, "vanilla.ctf@ddnet.org/kills", 2, EMatchMetricAggregation::SUM));
	EXPECT_TRUE(Builder.AddMetricValue(EMatchSubjectKind::PARTICIPANT, 0, "vanilla.ctf@ddnet.org/kills", 3, EMatchMetricAggregation::SUM));
	ASSERT_EQ(Builder.Report().m_vMetrics.size(), 1U);
	EXPECT_EQ(Builder.Report().m_vMetrics[0].m_Value, 5);

	std::string Error;
	EXPECT_TRUE(Builder.Finalize(&Error)) << Error;
	EXPECT_FALSE(Builder.Finalize(&Error));
	EXPECT_FALSE(Builder.SetMetricValue(EMatchSubjectKind::PARTICIPANT, 0, "vanilla.ctf@ddnet.org/kills", 10, EMatchMetricAggregation::SUM));
}

TEST(MatchReportBuilder, RejectsMetricOverflow)
{
	CMatchReport Report = SampleReport();
	Report.m_vMetrics.clear();
	CMatchReportBuilder Builder(std::move(Report));
	ASSERT_TRUE(Builder.AddMetricValue(EMatchSubjectKind::PARTICIPANT, 0, "vanilla.ctf@ddnet.org/kills", std::numeric_limits<int64_t>::max(), EMatchMetricAggregation::SUM));
	EXPECT_FALSE(Builder.AddMetricValue(EMatchSubjectKind::PARTICIPANT, 0, "vanilla.ctf@ddnet.org/kills", 1, EMatchMetricAggregation::SUM));
	std::string Error;
	EXPECT_FALSE(Builder.Finalize(&Error));
}

TEST(MatchReportBuilder, RejectsSerializedPayloadOverflow)
{
	CMatchReport Report = SampleReport();
	Report.m_ModeId = "large@ddnet.org";
	Report.m_vTeams.clear();
	Report.m_vParticipants.clear();
	Report.m_vStandings.clear();
	Report.m_vMetrics.clear();
	CMatchReportBuilder Builder(std::move(Report));
	// Enough long metric ids to exceed the payload limit whatever it is set to
	for(int i = 0; i < MatchReportLimits::MAX_METRICS; ++i)
	{
		const std::string MetricId = "large@ddnet.org/metric_" + std::to_string(i) + "_" + std::string(70, 'x');
		ASSERT_TRUE(Builder.AddMetric({EMatchSubjectKind::MATCH, std::nullopt, MetricId, i}));
	}
	std::string Error;
	std::string Payload;
	ASSERT_TRUE(Builder.Finalize(&Error)) << Error;
	EXPECT_FALSE(MatchReportToJson(Builder.Report(), Payload, &Error));
	// The same report still packs, which is the point of the packed form. A
	// client that receives it only fails once it writes its JSON journal.
	EXPECT_TRUE(MatchReportToPacked(Builder.Report(), Payload, &Error)) << Error;
}

TEST(MatchReportView, CategorizesKnownAndUnknownMetrics)
{
	EXPECT_EQ(MatchMetricCategory("vanilla.dm@ddnet.org/kills", 1), EMatchMetricCategory::COMBAT);
	EXPECT_EQ(MatchMetricCategory("vanilla.dm@ddnet.org/damage_done", 1), EMatchMetricCategory::COMBAT);
	EXPECT_EQ(MatchMetricCategory("vanilla.dm@ddnet.org/weapon_2_kills", 1), EMatchMetricCategory::WEAPONS);
	EXPECT_EQ(MatchMetricCategory("vanilla.ctf@ddnet.org/flag_captures", 1), EMatchMetricCategory::OBJECTIVES);
	EXPECT_EQ(MatchMetricCategory("zcatch.laser@ddnet.org/catches", 1), EMatchMetricCategory::OBJECTIVES);
	EXPECT_EQ(MatchMetricCategory("ddnet.race@ddnet.org/personal_best_ticks", 1), EMatchMetricCategory::OVERVIEW);
	// A mode this build has never heard of uses the same metric vocabulary, so
	// its metrics are categorised and named like everyone else's.
	EXPECT_EQ(MatchMetricCategory("custom@server.example/kills", 1), EMatchMetricCategory::COMBAT);
	EXPECT_EQ(MatchMetricDisplayName("custom@server.example/kills", 1), "Kills");
	EXPECT_EQ(MatchMetricCategory("vanilla.dm@ddnet.org/kills", 2), EMatchMetricCategory::OTHER);
	// An unknown metric still gets a readable label instead of its raw id
	EXPECT_EQ(MatchMetricDisplayName("custom@server.example/wall_runs", 1), "Wall runs");
	EXPECT_EQ(MatchMetricSuffix("custom@server.example/frozen_teammates"), "frozen_teammates");
	char aTime[64];
	FormatMatchDuration(MatchReportLimits::MAX_DURATION_TICKS, 1, aTime, sizeof(aTime));
	EXPECT_NE(aTime[0], '\0');
	FormatMatchSeconds(std::numeric_limits<int64_t>::max(), aTime, sizeof(aTime));
	EXPECT_NE(aTime[0], '\0');
	CMatchMetric Rank = {EMatchSubjectKind::PARTICIPANT, 0, "ddnet.race@ddnet.org/map_rank", 7};
	FormatMatchMetricValue(Rank, 1, 50, aTime, sizeof(aTime));
	EXPECT_STREQ(aTime, "#7");
	// A tick metric formats as a duration whatever mode reported it
	CMatchMetric ModRun = {EMatchSubjectKind::PARTICIPANT, 0, "custom@server.example/current_run_ticks", 500};
	FormatMatchMetricValue(ModRun, 1, 50, aTime, sizeof(aTime));
	EXPECT_STREQ(aTime, "00:10");
	// A suffix nobody knows stays a plain number
	CMatchMetric Unknown = {EMatchSubjectKind::PARTICIPANT, 0, "custom@server.example/wall_runs", 500};
	FormatMatchMetricValue(Unknown, 1, 50, aTime, sizeof(aTime));
	EXPECT_STREQ(aTime, "500");
}

TEST(MatchReportView, BuildsWeaponCombatStatsAndAccuracy)
{
	CMatchReport Report = SampleReport();
	Report.m_ModeId = "vanilla.dm@ddnet.org";
	Report.m_ModeSchemaVersion = 1;
	Report.m_vMetrics = {
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/shots", 4},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/hits", 3},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/damage_done", 30},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/damage_taken", 12},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/weapon_1_shots", 4},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/weapon_1_hits", 3},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/weapon_1_damage_done", 30},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/weapon_1_damage_taken", 12},
		{EMatchSubjectKind::PARTICIPANT, 1, "vanilla.dm@ddnet.org/shots", 100},
		{EMatchSubjectKind::PARTICIPANT, 0, "vanilla.dm@ddnet.org/weapon_bad_shots", 100},
	};

	const CMatchCombatStats Stats = BuildMatchCombatStats(Report, 0);
	EXPECT_EQ(Stats.m_Total.m_Shots, 4);
	EXPECT_EQ(Stats.m_Total.m_Hits, 3);
	EXPECT_EQ(Stats.m_Total.m_DamageDone, 30);
	EXPECT_EQ(Stats.m_Total.m_DamageTaken, 12);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_GUN].m_Shots, 4);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_GUN].m_Hits, 3);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_GUN].m_DamageDone, 30);
	EXPECT_EQ(Stats.m_vWeapons[WEAPON_GUN].m_DamageTaken, 12);
	EXPECT_FALSE(Stats.m_vWeapons[WEAPON_HAMMER].HasData());

	char aAccuracy[32];
	FormatMatchAccuracy(Stats.m_Total.m_Hits, Stats.m_Total.m_Shots, aAccuracy, sizeof(aAccuracy));
	EXPECT_STREQ(aAccuracy, "75.0%");
	FormatMatchAccuracy(5, 2, aAccuracy, sizeof(aAccuracy));
	EXPECT_STREQ(aAccuracy, "250.0%");
	FormatMatchAccuracy(0, 0, aAccuracy, sizeof(aAccuracy));
	EXPECT_STREQ(aAccuracy, "-");

	CMatchProfile Profile;
	Profile.m_vMetrics = {
		{"vanilla.dm@ddnet.org", 1, "vanilla.dm@ddnet.org/weapon_4_shots", 8},
		{"vanilla.dm@ddnet.org", 1, "vanilla.dm@ddnet.org/weapon_4_hits", 6},
	};
	const CMatchCombatStats ProfileStats = BuildMatchCombatStats(Profile);
	EXPECT_EQ(ProfileStats.m_vWeapons[WEAPON_LASER].m_Shots, 8);
	EXPECT_EQ(ProfileStats.m_vWeapons[WEAPON_LASER].m_Hits, 6);
}

TEST(MatchReportView, ModWeaponsBeyondTheKnownOnes)
{
	CMatchReport Report;
	Report.m_ModeSchemaVersion = 1;
	Report.m_vMetrics = {
		{EMatchSubjectKind::PARTICIPANT, 0, "custom@server.example/weapon_11_shots", 20},
		{EMatchSubjectKind::PARTICIPANT, 0, "custom@server.example/weapon_11_hits", 9},
		{EMatchSubjectKind::PARTICIPANT, 0, "custom@server.example/weapon_99_shots", 5},
	};
	const CMatchCombatStats Stats = BuildMatchCombatStats(Report, 0);
	ASSERT_GT(Stats.m_vWeapons.size(), 11u);
	EXPECT_EQ(Stats.m_vWeapons[11].m_Shots, 20);
	EXPECT_EQ(Stats.m_vWeapons[11].m_Hits, 9);
	// Beyond the limit the metric is ignored instead of allocating a row per index
	EXPECT_LE(Stats.m_vWeapons.size(), static_cast<size_t>(MAX_MATCH_WEAPONS));
	EXPECT_EQ(MatchWeaponDisplayName(11), "Weapon 11");
}

TEST(MatchReportLimitsFit, AFullServerOfParticipants)
{
	// A report that a real server can produce has to survive validation; the
	// limits used to be picked independently and a full server exceeded them.
	EXPECT_GE(MatchReportLimits::MAX_METRICS, MatchReportLimits::MAX_PARTICIPANTS * MatchReportLimits::MAX_METRICS_PER_PARTICIPANT);
	EXPECT_GE(MatchReportLimits::MAX_STANDINGS, MatchReportLimits::MAX_PARTICIPANTS);

	CMatchReport Report = SampleReport();
	Report.m_vParticipants.clear();
	Report.m_vStandings.clear();
	Report.m_vMetrics.clear();
	for(int Participant = 0; Participant < 64; ++Participant)
	{
		Report.m_vParticipants.push_back({Participant, std::nullopt, "Player", "", 0, std::nullopt, false});
		Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, Participant, Participant + 1, EMatchOutcome::LOSS});
		for(int Metric = 0; Metric < 30; ++Metric)
		{
			char aMetricId[64];
			str_format(aMetricId, sizeof(aMetricId), "vanilla.ctf@ddnet.org/weapon_%d_shots", Metric);
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, Participant, aMetricId, 1234});
		}
	}
	std::string Error;
	EXPECT_TRUE(MatchReportValidate(Report, &Error)) << Error;
	std::string Json;
	ASSERT_TRUE(MatchReportToJson(Report, Json, &Error)) << Error;
	EXPECT_LE(static_cast<int>(Json.size()), MatchReportLimits::MAX_PAYLOAD_SIZE);

	// This is the report that live statistics resend every few seconds, so the
	// packed form has to be an order of magnitude smaller than the JSON.
	std::string Packed;
	ASSERT_TRUE(MatchReportToPacked(Report, Packed, &Error)) << Error;
	EXPECT_LT(Packed.size() * 10, Json.size());
}
