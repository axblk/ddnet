#include <generated/protocol7.h>

#include <game/gamecore.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/server/mode/builtin_game_modes.h>
#include <game/server/mode/game_mode_registry.h>
#include <game/server/modes/vanilla/dm.h>
#include <game/server/modes/vanilla/tdm.h>

#include <gtest/gtest.h>

namespace
{
	std::unique_ptr<IGameController> CreateNothing(CGameServices &, const CGameModeInfo &)
	{
		return nullptr;
	}
}

TEST(GameModeRegistry, RegisterAndFind)
{
	CGameModeRegistry Registry;
	const CGameModeInfo Info = {"test", "Test", "Test", "Test", EGameModeScoreKind::POINTS, 0, GAME_MODE_PROTOCOL_SIX};

	EXPECT_TRUE(Registry.Register(Info, CreateNothing));
	ASSERT_NE(Registry.Find("test"), nullptr);
	EXPECT_STREQ(Registry.Find("test")->m_pDisplayName, "Test");
	EXPECT_EQ(Registry.Find("missing"), nullptr);
	EXPECT_EQ(Registry.Find(nullptr), nullptr);
	EXPECT_FALSE(Registry.Register(Info, CreateNothing));
}

TEST(GameModeRegistry, BuiltInMetadata)
{
	CGameModeRegistry Registry;
	ASSERT_TRUE(RegisterBuiltInGameModes(Registry));

	const CGameModeInfo *pDDNet = Registry.Find("ddnet");
	ASSERT_NE(pDDNet, nullptr);
	EXPECT_STREQ(pDDNet->m_pGameType, "DDraceNetwork");
	EXPECT_STREQ(GameModeScoreKindName(pDDNet->m_ScoreKind), "time");
	EXPECT_EQ(pDDNet->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);
	EXPECT_TRUE(pDDNet->m_UseTuneZones);
	EXPECT_EQ(pDDNet->m_PhysicsRuleset, EPhysicsRuleset::DDNET);

	const CGameModeInfo *pMod = Registry.Find("mod");
	ASSERT_NE(pMod, nullptr);
	EXPECT_STREQ(pMod->m_pTestingGameType, "TestMod");
	EXPECT_TRUE(pMod->m_UseTuneZones);
	EXPECT_EQ(pMod->m_PhysicsRuleset, EPhysicsRuleset::DDNET);

	const CGameModeInfo *pVanillaDM = Registry.Find("vanilla.dm");
	ASSERT_NE(pVanillaDM, nullptr);
	EXPECT_STREQ(pVanillaDM->m_pGameType, "DM");
	EXPECT_EQ(pVanillaDM->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pVanillaDM->m_ActivePlayerLimit, 0);
	EXPECT_FALSE(pVanillaDM->m_UseTuneZones);
	EXPECT_EQ(pVanillaDM->m_PhysicsRuleset, EPhysicsRuleset::VANILLA);

	const CGameModeInfo *pVanilla1on1 = Registry.Find("vanilla.1on1");
	ASSERT_NE(pVanilla1on1, nullptr);
	EXPECT_STREQ(pVanilla1on1->m_pGameType, "1on1");
	EXPECT_EQ(pVanilla1on1->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pVanilla1on1->m_GameFlags, 0);
	EXPECT_EQ(pVanilla1on1->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);
	EXPECT_EQ(pVanilla1on1->m_ActivePlayerLimit, 2);

	const CGameModeInfo *pVanillaTDM = Registry.Find("vanilla.tdm");
	ASSERT_NE(pVanillaTDM, nullptr);
	EXPECT_STREQ(pVanillaTDM->m_pGameType, "TDM");
	EXPECT_EQ(pVanillaTDM->m_GameFlags, protocol7::GAMEFLAG_TEAMS);
	EXPECT_EQ(pVanillaTDM->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pVanillaCTF = Registry.Find("vanilla.ctf");
	ASSERT_NE(pVanillaCTF, nullptr);
	EXPECT_STREQ(pVanillaCTF->m_pGameType, "CTF");
	EXPECT_EQ(pVanillaCTF->m_GameFlags, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS);
	EXPECT_EQ(pVanillaCTF->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pInstagibDM = Registry.Find("insta.idm");
	ASSERT_NE(pInstagibDM, nullptr);
	EXPECT_STREQ(pInstagibDM->m_pGameType, "iDM");
	EXPECT_EQ(pInstagibDM->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pInstagibDM->m_GameFlags, 0);
	EXPECT_EQ(pInstagibDM->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pInstagibTDM = Registry.Find("insta.itdm");
	ASSERT_NE(pInstagibTDM, nullptr);
	EXPECT_STREQ(pInstagibTDM->m_pGameType, "iTDM");
	EXPECT_EQ(pInstagibTDM->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pInstagibTDM->m_GameFlags, protocol7::GAMEFLAG_TEAMS);
	EXPECT_EQ(pInstagibTDM->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pInstagibCTF = Registry.Find("insta.ictf");
	ASSERT_NE(pInstagibCTF, nullptr);
	EXPECT_STREQ(pInstagibCTF->m_pGameType, "iCTF");
	EXPECT_EQ(pInstagibCTF->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pInstagibCTF->m_GameFlags, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS);
	EXPECT_EQ(pInstagibCTF->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pGrenadeDM = Registry.Find("insta.gdm");
	ASSERT_NE(pGrenadeDM, nullptr);
	EXPECT_STREQ(pGrenadeDM->m_pGameType, "gDM");
	EXPECT_EQ(pGrenadeDM->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pGrenadeDM->m_GameFlags, 0);
	EXPECT_EQ(pGrenadeDM->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pGrenadeTDM = Registry.Find("insta.gtdm");
	ASSERT_NE(pGrenadeTDM, nullptr);
	EXPECT_STREQ(pGrenadeTDM->m_pGameType, "gTDM");
	EXPECT_EQ(pGrenadeTDM->m_GameFlags, protocol7::GAMEFLAG_TEAMS);
	EXPECT_EQ(pGrenadeTDM->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pGrenadeCTF = Registry.Find("insta.gctf");
	ASSERT_NE(pGrenadeCTF, nullptr);
	EXPECT_STREQ(pGrenadeCTF->m_pGameType, "gCTF");
	EXPECT_EQ(pGrenadeCTF->m_GameFlags, protocol7::GAMEFLAG_TEAMS | protocol7::GAMEFLAG_FLAGS);
	EXPECT_EQ(pGrenadeCTF->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);

	const CGameModeInfo *pZCatch = Registry.Find("zcatch.laser");
	ASSERT_NE(pZCatch, nullptr);
	EXPECT_STREQ(pZCatch->m_pGameType, "zCatch");
	EXPECT_EQ(pZCatch->m_ScoreKind, EGameModeScoreKind::POINTS);
	EXPECT_EQ(pZCatch->m_GameFlags, 0);
	EXPECT_EQ(pZCatch->m_Protocols, GAME_MODE_PROTOCOL_SIX | GAME_MODE_PROTOCOL_SEVEN);
}

TEST(GameModeRegistry, VanillaDefaultTuning)
{
	CTuningParams Expected = CTuningParams::DEFAULT;
	Expected.Set("laser_bounce_num", 1);
	const CTuningParams Actual = CGameControllerVanillaDM::DefaultTuning();

	for(int i = 0; i < CTuningParams::Num(); i++)
	{
		float ExpectedValue;
		float ActualValue;
		ASSERT_TRUE(Expected.Get(i, &ExpectedValue));
		ASSERT_TRUE(Actual.Get(i, &ActualValue));
		EXPECT_EQ(ActualValue, ExpectedValue) << CTuningParams::Name(i);
	}
}

TEST(VanillaDM, DamageWithoutArmor)
{
	int Health = 10;
	int Armor = 0;
	CGameControllerVanillaDM::ApplyDamage(5, false, Health, Armor);
	EXPECT_EQ(Health, 5);
	EXPECT_EQ(Armor, 0);
}

TEST(VanillaDM, DamageWithArmor)
{
	int Health = 10;
	int Armor = 5;
	CGameControllerVanillaDM::ApplyDamage(5, false, Health, Armor);
	EXPECT_EQ(Health, 9);
	EXPECT_EQ(Armor, 1);
}

TEST(VanillaDM, SelfDamage)
{
	int Health = 10;
	int Armor = 0;
	CGameControllerVanillaDM::ApplyDamage(5, true, Health, Armor);
	EXPECT_EQ(Health, 8);
	EXPECT_EQ(Armor, 0);
}

TEST(VanillaDM, DeathScore)
{
	EXPECT_EQ(CGameControllerVanillaDM::DeathScoreDelta(1, 2, WEAPON_GUN), 1);
	EXPECT_EQ(CGameControllerVanillaDM::DeathScoreDelta(2, 2, WEAPON_SELF), -1);
	EXPECT_EQ(CGameControllerVanillaDM::DeathScoreDelta(1, 2, WEAPON_GAME), 0);
}

TEST(VanillaTDM, DeathScore)
{
	std::array<int, NUM_TEAMS> aTeamScores{};

	CGameControllerVanillaTDM::ApplyTeamDeathScore(aTeamScores, TEAM_RED, TEAM_BLUE, WEAPON_GUN, false);
	EXPECT_EQ(CGameControllerVanillaDM::DeathScoreDelta(1, 2, WEAPON_GUN), 1);
	EXPECT_EQ(aTeamScores[TEAM_BLUE], 1);

	CGameControllerVanillaTDM::ApplyTeamDeathScore(aTeamScores, TEAM_RED, TEAM_RED, WEAPON_GUN, false);
	EXPECT_EQ(CGameControllerVanillaDM::DeathScoreDelta(1, 0, WEAPON_GUN, true), -1);
	EXPECT_EQ(aTeamScores[TEAM_RED], -1);

	CGameControllerVanillaTDM::ApplyTeamDeathScore(aTeamScores, TEAM_BLUE, TEAM_BLUE, WEAPON_SELF, true);
	EXPECT_EQ(CGameControllerVanillaDM::DeathScoreDelta(2, 2, WEAPON_SELF), -1);
	EXPECT_EQ(aTeamScores[TEAM_BLUE], 0);

	CGameControllerVanillaTDM::ApplyTeamDeathScore(aTeamScores, TEAM_RED, TEAM_BLUE, WEAPON_GAME, false);
	EXPECT_EQ(aTeamScores[TEAM_BLUE], 0);
}

TEST(VanillaDM, MatchResult)
{
	using EMatchResult = CGameControllerVanillaDM::EMatchResult;
	EXPECT_EQ(CGameControllerVanillaDM::EvaluateMatch(2, false, false), EMatchResult::RUNNING);
	EXPECT_EQ(CGameControllerVanillaDM::EvaluateMatch(2, true, false), EMatchResult::SUDDEN_DEATH);
	EXPECT_EQ(CGameControllerVanillaDM::EvaluateMatch(1, true, false), EMatchResult::END_ROUND);
	EXPECT_EQ(CGameControllerVanillaDM::EvaluateMatch(1, false, true), EMatchResult::END_ROUND);
}

TEST(VanillaDM, ShotgunSpread)
{
	const vec2 Direction(1.0f, 0.0f);
	const vec2 Left = CGameControllerVanillaDM::ShotgunDirection(Direction, -2, 0.8f);
	const vec2 Center = CGameControllerVanillaDM::ShotgunDirection(Direction, 0, 0.8f);
	const vec2 Right = CGameControllerVanillaDM::ShotgunDirection(Direction, 2, 0.8f);
	EXPECT_NEAR(length(Left), 0.8f, 0.0001f);
	EXPECT_NEAR(length(Center), 1.0f, 0.0001f);
	EXPECT_NEAR(length(Right), 0.8f, 0.0001f);
	EXPECT_NEAR(Left.x, Right.x, 0.0001f);
	EXPECT_NEAR(Left.y, -Right.y, 0.0001f);
}

TEST(GameModeRegistry, DDNetDefaultTuning)
{
	CTuningParams Expected = CTuningParams::DEFAULT;
	Expected.Set("gun_speed", 1400);
	Expected.Set("gun_curvature", 0);
	Expected.Set("shotgun_speed", 500);
	Expected.Set("shotgun_speeddiff", 0);
	Expected.Set("shotgun_curvature", 0);
	const CTuningParams Actual = CGameControllerDDNet::DefaultTuning();

	for(int i = 0; i < CTuningParams::Num(); i++)
	{
		float ExpectedValue;
		float ActualValue;
		ASSERT_TRUE(Expected.Get(i, &ExpectedValue));
		ASSERT_TRUE(Actual.Get(i, &ActualValue));
		EXPECT_EQ(ActualValue, ExpectedValue) << CTuningParams::Name(i);
	}
}
