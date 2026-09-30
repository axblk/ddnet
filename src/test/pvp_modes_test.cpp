// The rules of the building blocks the PvP modes share, and of the modes, without a game world.
#include <base/str.h>

#include <engine/shared/config.h>

#include <game/server/modes/catch16/groups.h>
#include <game/server/modes/fng/spikes.h>
#include <game/server/modes/insta/grenade_ammo.h>
#include <game/server/modes/insta/instagib.h>
#include <game/server/modes/pvp/anticamper.h>
#include <game/server/modes/pvp/killing_spree.h>
#include <game/server/modes/zcatch/catches.h>
#include <game/server/modes/zcatch/rules.h>

#include <gtest/gtest.h>

TEST(KillingSpree, EveryStepIsTold)
{
	CKillingSpree Spree;
	for(int Kill = 1; Kill < 5; Kill++)
		EXPECT_FALSE(Spree.Add(5));
	EXPECT_TRUE(Spree.Add(5));
	EXPECT_EQ(Spree.Kills(), 5);
	for(int Kill = 6; Kill < 10; Kill++)
		EXPECT_FALSE(Spree.Add(5));
	EXPECT_TRUE(Spree.Add(5));
	EXPECT_EQ(Spree.End(), 10);
	EXPECT_EQ(Spree.Kills(), 0);

	// off
	for(int Kill = 1; Kill <= 10; Kill++)
		EXPECT_FALSE(Spree.Add(0));
}

TEST(KillingSpree, Texts)
{
	char aBuf[128];
	CKillingSpree::FormatStep(aBuf, sizeof(aBuf), "a", 10, 10);
	EXPECT_STREQ(aBuf, "'a' is on a killing spree with 10 kills!");
	CKillingSpree::FormatStep(aBuf, sizeof(aBuf), "a", 20, 10);
	EXPECT_STREQ(aBuf, "'a' is on a rampage with 20 kills!");
	CKillingSpree::FormatStep(aBuf, sizeof(aBuf), "a", 50, 10);
	EXPECT_STREQ(aBuf, "'a' is godlike with 50 kills!");
	CKillingSpree::FormatStep(aBuf, sizeof(aBuf), "a", 90, 10);
	EXPECT_STREQ(aBuf, "'a' is godlike with 90 kills!");
	CKillingSpree::FormatEnd(aBuf, sizeof(aBuf), "a", 12, "b");
	EXPECT_STREQ(aBuf, "'a' 12-kills killing spree was ended by 'b'");

	EXPECT_FALSE(CKillingSpree::IsEndWorthTelling(9, 10));
	EXPECT_TRUE(CKillingSpree::IsEndWorthTelling(10, 10));
	EXPECT_FALSE(CKillingSpree::IsEndWorthTelling(10, 0));
}

TEST(MultiKill, KillsWithinFiveSecondsCount)
{
	constexpr int TickSpeed = 50;
	CMultiKill Multi;
	EXPECT_EQ(Multi.Add(100, TickSpeed), 1);
	EXPECT_EQ(Multi.Add(100 + 5 * TickSpeed, TickSpeed), 2);
	EXPECT_EQ(Multi.Add(100 + 6 * TickSpeed, TickSpeed), 3);
	EXPECT_EQ(Multi.Add(100 + 11 * TickSpeed + 1, TickSpeed), 1);
	Multi.Reset();
	EXPECT_EQ(Multi.Add(100 + 12 * TickSpeed, TickSpeed), 1);

	char aBuf[64];
	CMultiKill::Format(aBuf, sizeof(aBuf), "a", 3);
	EXPECT_STREQ(aBuf, "'a' multi x3!");
}

TEST(Anticamper, WarnsThenPunishesWhoStays)
{
	constexpr int TickSpeed = 50;
	CAnticamper Anticamper;
	int Tick = 0;
	EXPECT_EQ(Anticamper.Tick(vec2(0, 0), Tick, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	Tick = 5 * TickSpeed - 1;
	EXPECT_EQ(Anticamper.Tick(vec2(199, 0), Tick, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	Tick++;
	EXPECT_EQ(Anticamper.Tick(vec2(0, 199), Tick, TickSpeed, 10, 200), CAnticamper::EAction::WARN);
	Tick++;
	EXPECT_EQ(Anticamper.Tick(vec2(0, 0), Tick, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	Tick = 10 * TickSpeed;
	EXPECT_EQ(Anticamper.Tick(vec2(0, 0), Tick, TickSpeed, 10, 200), CAnticamper::EAction::PUNISH);
	// and the clock starts anew
	EXPECT_EQ(Anticamper.Tick(vec2(0, 0), Tick + 1, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
}

TEST(Anticamper, MovingAwayStartsTheClockAnew)
{
	constexpr int TickSpeed = 50;
	CAnticamper Anticamper;
	EXPECT_EQ(Anticamper.Tick(vec2(0, 0), 0, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	EXPECT_EQ(Anticamper.Tick(vec2(200, 0), 9 * TickSpeed, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	// the new spot is where the player was when the clock started again
	EXPECT_EQ(Anticamper.Tick(vec2(200, 0), 10 * TickSpeed, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	EXPECT_EQ(Anticamper.Tick(vec2(200, 0), 15 * TickSpeed - 1, TickSpeed, 10, 200), CAnticamper::EAction::NONE);
	EXPECT_EQ(Anticamper.Tick(vec2(200, 0), 15 * TickSpeed, TickSpeed, 10, 200), CAnticamper::EAction::WARN);
	EXPECT_EQ(Anticamper.Tick(vec2(200, 0), 20 * TickSpeed, TickSpeed, 10, 200), CAnticamper::EAction::PUNISH);
}

static CGrenadeAmmo::CSettings GctfGrenades()
{
	// as on the official gCTF servers
	CGrenadeAmmo::CSettings Settings;
	Settings.m_Regen = true;
	Settings.m_RegenTicks = 50;
	Settings.m_Max = 4;
	Settings.m_SelfPushRefund = true;
	Settings.m_RefillOnHit = 2;
	return Settings;
}

TEST(GrenadeAmmo, EndlessWithoutRegeneration)
{
	const CGrenadeAmmo::CSettings Settings;
	EXPECT_EQ(CGrenadeAmmo::Spawn(Settings), -1);
	EXPECT_EQ(CGrenadeAmmo::AfterHit(-1, Settings), -1);
	EXPECT_EQ(CGrenadeAmmo::AfterSelfPush(-1, Settings), -1);
	CGrenadeAmmo::CRegen Regen;
	EXPECT_EQ(Regen.Tick(-1, Settings), -1);
}

TEST(GrenadeAmmo, HitsAndGrenadeJumpsBringGrenadesBack)
{
	CGrenadeAmmo::CSettings Settings = GctfGrenades();
	EXPECT_EQ(CGrenadeAmmo::Spawn(Settings), 4);
	EXPECT_EQ(CGrenadeAmmo::AfterHit(1, Settings), 4);
	EXPECT_EQ(CGrenadeAmmo::AfterSelfPush(1, Settings), 2);
	EXPECT_EQ(CGrenadeAmmo::AfterSelfPush(4, Settings), 4);
	Settings.m_RefillOnHit = 1;
	EXPECT_EQ(CGrenadeAmmo::AfterHit(1, Settings), 2);
	EXPECT_EQ(CGrenadeAmmo::AfterHit(4, Settings), 4);
	Settings.m_RefillOnHit = 0;
	EXPECT_EQ(CGrenadeAmmo::AfterHit(1, Settings), 1);
	Settings.m_SelfPushRefund = false;
	EXPECT_EQ(CGrenadeAmmo::AfterSelfPush(1, Settings), 1);
}

TEST(GrenadeAmmo, OneComesBackAfterTheReloadAndTheRegenerationTime)
{
	const CGrenadeAmmo::CSettings Settings = GctfGrenades();
	CGrenadeAmmo::CRegen Regen;
	constexpr int ReloadTicks = 25;
	// the tick of the shot counts as the first
	Regen.OnFire(ReloadTicks);
	int Ammo = 2;
	for(int Tick = 0; Tick < ReloadTicks + 1 + Settings.m_RegenTicks; Tick++)
		Ammo = Regen.Tick(Ammo, Settings);
	EXPECT_EQ(Ammo, 2);
	Ammo = Regen.Tick(Ammo, Settings);
	EXPECT_EQ(Ammo, 3);
	// the next one a tick later than the time, as in ddnet-insta
	for(int Tick = 0; Tick < Settings.m_RegenTicks; Tick++)
		Ammo = Regen.Tick(Ammo, Settings);
	EXPECT_EQ(Ammo, 3);
	Ammo = Regen.Tick(Ammo, Settings);
	EXPECT_EQ(Ammo, 4);
	// never more than the most
	for(int Tick = 0; Tick < 3 * Settings.m_RegenTicks; Tick++)
		Ammo = Regen.Tick(Ammo, Settings);
	EXPECT_EQ(Ammo, 4);
}

TEST(Instagib, SpawnWeapon)
{
	EXPECT_EQ(InstagibSpawnWeapon("grenade"), WEAPON_GRENADE);
	EXPECT_EQ(InstagibSpawnWeapon("laser"), WEAPON_LASER);
	EXPECT_EQ(InstagibSpawnWeapon("Rifle"), WEAPON_LASER);
	EXPECT_EQ(InstagibSpawnWeapon("hammer"), WEAPON_GRENADE);
}

TEST(ZCatch, WinPoints)
{
	const int aExpected[] = {0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 5, 7, 9, 11, 12, 14, 16, 16, 16};
	for(int Kills = 0; Kills < (int)std::size(aExpected); Kills++)
		EXPECT_EQ(ZCatch::WinPoints(Kills), aExpected[Kills]) << Kills;
}

TEST(ZCatch, KillsToWin)
{
	EXPECT_EQ(ZCatch::KillsToWin(5), 4);
	EXPECT_EQ(ZCatch::KillsToWin(16), 4);
	EXPECT_EQ(ZCatch::KillsToWin(3), 2);
	EXPECT_EQ(ZCatch::KillsToWin(2), 1);
}

TEST(ZCatch, Colors)
{
	EXPECT_EQ(ZCatch::ParseColors("teetime"), ZCatch::EColors::TEETIME);
	EXPECT_EQ(ZCatch::ParseColors("Savander"), ZCatch::EColors::SAVANDER);
	EXPECT_EQ(ZCatch::ParseColors("rainbow"), ZCatch::EColors::TEETIME);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::TEETIME, 0), 0xA0FF00);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::TEETIME, 3), 0x82FF00);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::TEETIME, 20), 0x00FF00);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::SAVANDER, 0), 0xFFBB00);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::SAVANDER, 1), 0x00FF00);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::SAVANDER, 15), 0xEEFF00);
	EXPECT_EQ(ZCatch::BodyColor(ZCatch::EColors::SAVANDER, 16), 0xFFBB00);
}

TEST(Catches, ADeathFreesOnlyWhomTheCatcherHeld)
{
	CCatches Catches;
	Catches.Catch(2, 1, true);
	Catches.Catch(1, 0, true);
	EXPECT_EQ(Catches.CatcherId(2), 1);
	EXPECT_EQ(Catches.KillsThatCount(0), 1);
	EXPECT_EQ(Catches.ReleaseAll(1), std::vector<int>{2});
	EXPECT_EQ(Catches.KillsThatCount(1), 0);
	EXPECT_FALSE(Catches.IsCaught(2));
	EXPECT_TRUE(Catches.IsCaught(1));
}

TEST(Catches, TheKillKeyLetsTheLastCaughtGoFirst)
{
	CCatches Catches;
	Catches.Catch(1, 0, true);
	Catches.Catch(2, 0, true);
	// joined and caught by the leader, which is no kill
	Catches.Catch(3, 0, false);
	EXPECT_EQ(Catches.KillsThatCount(0), 2);
	EXPECT_EQ(Catches.ReleaseLast(0), std::vector<int>{3});
	EXPECT_EQ(Catches.KillsThatCount(0), 1);
	// the last kill that counts lets everybody go
	EXPECT_EQ(Catches.ReleaseLast(0), (std::vector<int>{2, 1}));
	EXPECT_EQ(Catches.KillsThatCount(0), 0);
	EXPECT_TRUE(Catches.VictimIds(0).empty());
	EXPECT_TRUE(Catches.ReleaseLast(0).empty());
}

TEST(Catches, TheLeaderHasTheMostKillsThatCount)
{
	CCatches Catches;
	EXPECT_EQ(Catches.LeaderId(), CCatches::NONE);
	Catches.Catch(5, 3, true);
	Catches.Catch(6, 2, true);
	EXPECT_EQ(Catches.LeaderId(), 2);
	Catches.Catch(7, 3, true);
	EXPECT_EQ(Catches.LeaderId(), 3);
	// the same one twice counts once
	Catches.Catch(6, 2, true);
	EXPECT_EQ(Catches.KillsThatCount(2), 1);
}

TEST(Catches, WhoLeavesIsLetGoAndLetsGo)
{
	CCatches Catches;
	Catches.Catch(1, 0, true);
	Catches.Catch(2, 1, true);
	EXPECT_EQ(Catches.Leave(1), std::vector<int>{2});
	EXPECT_TRUE(Catches.VictimIds(0).empty());
	EXPECT_FALSE(Catches.IsCaught(2));
	Catches.Clear();
	EXPECT_EQ(Catches.LeaderId(), CCatches::NONE);
}

TEST(Catch16Groups, WhoIsHitJoinsTheGroup)
{
	CCatch16Groups Groups;
	EXPECT_EQ(Groups.Group(3), 3);
	EXPECT_TRUE(Groups.Join(3, 1));
	EXPECT_FALSE(Groups.Join(3, 1));
	EXPECT_TRUE(Groups.SameGroup(1, 3));
	// the founder can be caught by another group, the group stays
	EXPECT_TRUE(Groups.Join(1, 2));
	EXPECT_EQ(Groups.Group(3), 1);
	Groups.Leave(3);
	EXPECT_EQ(Groups.Group(3), 3);
}

TEST(Catch16Groups, AGroupEndsWithItsFounder)
{
	CCatch16Groups Groups;
	Groups.Join(3, 1);
	Groups.Join(4, 1);
	Groups.Join(1, 2);
	EXPECT_EQ(Groups.Dissolve(1), (std::vector<int>{3, 4}));
	EXPECT_EQ(Groups.Group(3), 3);
	EXPECT_EQ(Groups.Group(1), 1);
}

TEST(Catch16Groups, RoundIsOverWithOneGroup)
{
	CCatch16Groups Groups;
	std::bitset<MAX_CLIENTS> Playing;
	Playing.set(0);
	EXPECT_EQ(Groups.OnlyGroup(Playing), -1);
	Playing.set(1);
	Playing.set(2);
	EXPECT_EQ(Groups.OnlyGroup(Playing), -1);
	Groups.Join(1, 0);
	EXPECT_EQ(Groups.OnlyGroup(Playing), -1);
	Groups.Join(2, 0);
	EXPECT_EQ(Groups.OnlyGroup(Playing), 0);
	// who does not play does not count
	Groups.Join(0, 5);
	Playing.reset(0);
	EXPECT_EQ(Groups.OnlyGroup(Playing), 0);
}

TEST(Catch16Groups, NewPlayersGoIntoTheBiggestGroups)
{
	CCatch16Groups Groups;
	std::bitset<MAX_CLIENTS> Playing;
	for(int ClientId = 0; ClientId < 6; ClientId++)
		Playing.set(ClientId);
	EXPECT_TRUE(Groups.BiggestGroups(Playing).empty());
	Groups.Join(1, 0);
	Groups.Join(3, 2);
	EXPECT_EQ(Groups.BiggestGroups(Playing), (std::vector<int>{0, 2}));
	Groups.Join(4, 2);
	EXPECT_EQ(Groups.BiggestGroups(Playing), std::vector<int>{2});
}

TEST(Catch16Groups, ColorsAfterCatch64)
{
	// cherry, the first of 16 hues
	EXPECT_EQ(Catch16::GroupColors(0).m_Body, 0x00FF00);
	EXPECT_EQ(Catch16::GroupColors(0).m_Feet, 0x00FF00);
	// cyan
	EXPECT_EQ(Catch16::GroupColors(1).m_Body, 0x80FF00);
	// then with white feet, a black body and a white body
	EXPECT_EQ(Catch16::GroupColors(17).m_Feet, 0x00FFFF);
	EXPECT_EQ(Catch16::GroupColors(33).m_Body, 0x000000);
	EXPECT_EQ(Catch16::GroupColors(49).m_Body, 0x00FFFF);
	EXPECT_EQ(Catch16::GroupColors(49).m_Feet, 0x80FF00);
	char aName[32];
	Catch16::GroupColorName(1, aName, sizeof(aName));
	EXPECT_STREQ(aName, "cyan");
	Catch16::GroupColorName(33, aName, sizeof(aName));
	EXPECT_STREQ(aName, "black-cyan");
	for(int Founder = 0; Founder < 16; Founder++)
		for(int Other = Founder + 1; Other < 16; Other++)
			EXPECT_NE(Catch16::GroupColors(Founder).m_Body, Catch16::GroupColors(Other).m_Body);
}

// the settings as the server starts with them, for the rules that read them
class FngRules : public ::testing::Test // NOLINT(readability-identifier-naming)
{
	CConfig m_Backup = g_Config;

public:
	FngRules()
	{
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) g_Config.m_##Name = Def;
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc) g_Config.m_##Name = Def;
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc) str_copy(g_Config.m_##Name, Def);
#include <engine/shared/config_variables.h>
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR
	}
	~FngRules() override { g_Config = m_Backup; }
};

TEST(Fng, SpikesOfTheTiles)
{
	EXPECT_EQ(Fng::SpikeOfTile(7), Fng::ESpike::GOLD);
	EXPECT_EQ(Fng::SpikeOfTile(8), Fng::ESpike::NORMAL);
	EXPECT_EQ(Fng::SpikeOfTile(9), Fng::ESpike::RED);
	EXPECT_EQ(Fng::SpikeOfTile(10), Fng::ESpike::BLUE);
	EXPECT_EQ(Fng::SpikeOfTile(14), Fng::ESpike::GREEN);
	EXPECT_EQ(Fng::SpikeOfTile(15), Fng::ESpike::PURPLE);
	EXPECT_EQ(Fng::SpikeOfTile(1), Fng::ESpike::NONE);
}

TEST(Fng, TheNearestSpikeUnderTheTee)
{
	// a gold spike right of the tile the tee is in, a normal one below
	const auto Tiles = [](int x, int y) {
		if(x == 3 && y == 2)
			return Fng::ESpike::GOLD;
		if(x == 2 && y == 3)
			return Fng::ESpike::NORMAL;
		return Fng::ESpike::NONE;
	};
	// the corners reach a third of the radius of 28 around the centre
	EXPECT_EQ(Fng::TouchedSpike(vec2(80.0f, 80.0f), 28.0f, 10, 10, Tiles), Fng::ESpike::NONE);
	EXPECT_EQ(Fng::TouchedSpike(vec2(90.0f, 80.0f), 28.0f, 10, 10, Tiles), Fng::ESpike::GOLD);
	EXPECT_EQ(Fng::TouchedSpike(vec2(80.0f, 90.0f), 28.0f, 10, 10, Tiles), Fng::ESpike::NORMAL);
	EXPECT_EQ(Fng::TouchedSpike(vec2(92.0f, 90.0f), 28.0f, 10, 10, Tiles), Fng::ESpike::GOLD);
	EXPECT_EQ(Fng::TouchedSpike(vec2(90.0f, 92.0f), 28.0f, 10, 10, Tiles), Fng::ESpike::NORMAL);
}

TEST_F(FngRules, WhatTheSpikesAreWorth)
{
	const Fng::CSpikePoints Normal = Fng::SpikePoints(Fng::ESpike::NORMAL, TEAM_RED, true);
	EXPECT_EQ(Normal.m_Player, 3);
	EXPECT_EQ(Normal.m_Team, 5);
	EXPECT_FALSE(Normal.m_Wrong);
	EXPECT_EQ(Fng::SpikePoints(Fng::ESpike::GOLD, TEAM_RED, true).m_Player, 6);
	EXPECT_EQ(Fng::SpikePoints(Fng::ESpike::GREEN, TEAM_RED, true).m_Team, 15);
	EXPECT_EQ(Fng::SpikePoints(Fng::ESpike::PURPLE, TEAM_RED, true).m_Player, 10);
	const Fng::CSpikePoints Own = Fng::SpikePoints(Fng::ESpike::RED, TEAM_RED, true);
	EXPECT_EQ(Own.m_Player, 5);
	EXPECT_EQ(Own.m_Team, 10);
	const Fng::CSpikePoints Wrong = Fng::SpikePoints(Fng::ESpike::BLUE, TEAM_RED, true);
	EXPECT_TRUE(Wrong.m_Wrong);
	EXPECT_EQ(Wrong.m_Player, -5);
	EXPECT_EQ(Wrong.m_Team, 0);
	// without teams every team spike is right
	EXPECT_FALSE(Fng::SpikePoints(Fng::ESpike::BLUE, TEAM_GAME, false).m_Wrong);
}

TEST_F(FngRules, TheHammerTheDDNetClientPredicts)
{
	EXPECT_FALSE(Fng::IsPredictedHammer());
	g_Config.m_SvFngHammer = 1;
	EXPECT_TRUE(Fng::IsPredictedHammer());
	const vec2 Push = vec2(0.0f, -1.0f) + normalize(vec2(1.0f, 0.0f) + vec2(0.0f, -1.1f)) * 10.0f;
	const vec2 Force = Fng::HammerForce(vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), false);
	EXPECT_FLOAT_EQ(Force.x, Push.x * 3.2f);
	EXPECT_FLOAT_EQ(Force.y, Push.y * 1.2f);
	const vec2 Melt = Fng::HammerForce(vec2(0.0f, 0.0f), vec2(10.0f, 0.0f), true);
	EXPECT_FLOAT_EQ(Melt.x, Push.x * 0.5f);
	g_Config.m_SvHammerScaleX = 300;
	EXPECT_FALSE(Fng::IsPredictedHammer());
}

TEST(Fng, MeltingTakesThreeSeconds)
{
	bool Thawed;
	EXPECT_EQ(Fng::MeltFreeze(500, 50, &Thawed), 350);
	EXPECT_FALSE(Thawed);
	EXPECT_EQ(Fng::MeltFreeze(100, 50, &Thawed), 2);
	EXPECT_TRUE(Thawed);
}
