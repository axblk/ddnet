#include <game/gamecore.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/server/mode/builtin_game_modes.h>
#include <game/server/mode/game_mode_registry.h>
#include <game/server/modes/vanilla/dm.h>

#include <gtest/gtest.h>

namespace
{
	std::unique_ptr<IGameController> CreateNothing(CGameContext *pGameServer, const CGameModeInfo &Info)
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

	const CGameModeInfo *pMod = Registry.Find("mod");
	ASSERT_NE(pMod, nullptr);
	EXPECT_STREQ(pMod->m_pTestingGameType, "TestMod");

	const CGameModeInfo *pVanillaDM = Registry.Find("vanilla.dm");
	ASSERT_NE(pVanillaDM, nullptr);
	EXPECT_STREQ(pVanillaDM->m_pGameType, "DM");
	EXPECT_EQ(pVanillaDM->m_ScoreKind, EGameModeScoreKind::POINTS);
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
	std::array<int, MAX_CLIENTS> aScores{};
	CGameControllerVanillaDM::ApplyDeathScore(aScores, 1, 2, WEAPON_GUN);
	EXPECT_EQ(aScores[1], 0);
	EXPECT_EQ(aScores[2], 1);

	CGameControllerVanillaDM::ApplyDeathScore(aScores, 2, 2, WEAPON_SELF);
	EXPECT_EQ(aScores[2], 0);
	CGameControllerVanillaDM::ApplyDeathScore(aScores, 1, 2, WEAPON_GAME);
	EXPECT_EQ(aScores[2], 0);
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
