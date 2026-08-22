#include "test.h"

#include <game/client/map_context.h>

#include <gtest/gtest.h>

TEST(MapContext, SettingsAreIsolated)
{
	CConfig Base{};
	Base.m_SvHit = 1;
	Base.m_SvFreezeDelay = DefaultConfig::SvFreezeDelay;
	const int GlobalSvHit = g_Config.m_SvHit;

	CMapContext First;
	CMapContext Second;
	First.GameConfig().Reset(Base);
	Second.GameConfig().Reset(Base);

	First.GameConfig().ExecuteLine("sv_hit 0");
	First.GameConfig().ExecuteLine("sv_freeze_delay 99");
	First.GameConfig().ExecuteLine("tune_zone 4 gun_speed 777");

	EXPECT_EQ(First.GameConfig().m_SvHit, 0);
	EXPECT_EQ(Second.GameConfig().m_SvHit, 1);
	EXPECT_EQ(First.GameConfig().m_SvFreezeDelay, 30);
	EXPECT_EQ(Second.GameConfig().m_SvFreezeDelay, DefaultConfig::SvFreezeDelay);
	EXPECT_EQ(g_Config.m_SvHit, GlobalSvHit);

	float FirstGunSpeed;
	float SecondGunSpeed;
	ASSERT_TRUE(First.TuningList()[4].Get("gun_speed", &FirstGunSpeed));
	ASSERT_TRUE(Second.TuningList()[4].Get("gun_speed", &SecondGunSpeed));
	EXPECT_FLOAT_EQ(FirstGunSpeed, 777.0f);
	EXPECT_FLOAT_EQ(SecondGunSpeed, 1400.0f);
}
