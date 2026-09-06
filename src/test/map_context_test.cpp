#include "test.h"

#include <base/str.h>

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

	EXPECT_EQ(First.GameConfig().Values().m_SvHit, 0);
	EXPECT_EQ(Second.GameConfig().Values().m_SvHit, 1);
	EXPECT_EQ(First.GameConfig().Values().m_SvFreezeDelay, 30);
	EXPECT_EQ(Second.GameConfig().Values().m_SvFreezeDelay, DefaultConfig::SvFreezeDelay);
	EXPECT_EQ(g_Config.m_SvHit, GlobalSvHit);

	float FirstGunSpeed;
	float SecondGunSpeed;
	ASSERT_TRUE(First.TuningList()[4].Get("gun_speed", &FirstGunSpeed));
	ASSERT_TRUE(Second.TuningList()[4].Get("gun_speed", &SecondGunSpeed));
	EXPECT_FLOAT_EQ(FirstGunSpeed, 777.0f);
	EXPECT_FLOAT_EQ(SecondGunSpeed, 1400.0f);
}

TEST(MapContext, EveryGameSettingArrivesFromTheMap)
{
	CConfig Base{};
	CMapContext Context;
	Context.GameConfig().Reset(Base);

	size_t NumChecked = 0;
// Asks for a value other than the default. Not a ternary, a setting with equal
// minimum and maximum would trip -Wduplicated-branches.
#define MACRO_CONFIG_INT(Name, ScriptName, Def, Min, Max, Flags, Desc) \
	if(((Flags) & CFGFLAG_GAME) != 0) \
	{ \
		int Wanted = (Min); \
		if(DefaultConfig::Name == Wanted) \
		{ \
			Wanted = (Max); \
		} \
		char aLine[128]; \
		str_format(aLine, sizeof(aLine), "%s %d", #ScriptName, Wanted); \
		Context.GameConfig().ExecuteLine(aLine); \
		EXPECT_EQ(Context.GameConfig().Values().m_##Name, Wanted) << "map setting " #ScriptName " did not reach the session"; \
		++NumChecked; \
	}
#define MACRO_CONFIG_COL(Name, ScriptName, Def, Flags, Desc)
#define MACRO_CONFIG_STR(Name, ScriptName, Len, Def, Flags, Desc)
#include <engine/shared/config_variables.h>
#undef MACRO_CONFIG_INT
#undef MACRO_CONFIG_COL
#undef MACRO_CONFIG_STR

	EXPECT_GT(NumChecked, 20u);
}
