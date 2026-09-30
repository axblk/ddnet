// The rules of the building blocks the PvP modes share, and of the modes, without a game world.
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
