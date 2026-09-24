#include "test.h"

#include <base/str.h>

#include <engine/storage.h>

#include <game/client/map_context.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>

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

TEST(MapContext, SharedMapIsFreedWhenTheLastSessionLeaves)
{
	const std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	ASSERT_NE(pStorage, nullptr);
	CMapContext First;
	ASSERT_TRUE(First.Map()->Load("ctf1", pStorage.get(), "data/maps/ctf1.map", IStorage::TYPE_ALL));
	First.Data()->InitLayers();
	First.Collision()->Init(First.Layers());

	CMapContext Second;
	Second.Share(First);
	Second.Data()->InitLayers();
	Second.Collision()->Init(Second.Layers());
	EXPECT_EQ(First.Data(), Second.Data());
	EXPECT_EQ(First.Layers(), Second.Layers());
	// Collision is written per session: doors, switches, predicted lasers.
	EXPECT_NE(First.Collision(), Second.Collision());
	const std::weak_ptr<CMapData> pShared = First.Data();

	// The session that loaded it leaves, the other one still plays it.
	First.Unload();
	EXPECT_FALSE(First.Map()->IsLoaded());
	EXPECT_NE(First.Data(), Second.Data());
	ASSERT_TRUE(Second.Map()->IsLoaded());
	ASSERT_NE(Second.Layers()->GameLayer(), nullptr);
	EXPECT_EQ(Second.Collision()->GetWidth(), Second.Layers()->GameLayer()->m_Width);
	EXPECT_EQ(Second.Data().use_count(), 1);
	EXPECT_FALSE(pShared.expired());

	// With the last one it is gone.
	Second.Unload();
	EXPECT_TRUE(pShared.expired());
	EXPECT_FALSE(Second.Map()->IsLoaded());
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
