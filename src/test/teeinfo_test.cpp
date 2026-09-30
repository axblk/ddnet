#include <base/color.h>

#include <game/server/teeinfo.h>

#include <gtest/gtest.h>

TEST(TeeInfoOverride, ColorsGoOverTheOwnSkin)
{
	const CTeeInfo Own("pinky", false, 0, 0);
	const CTeeInfo Shown = CTeeInfoOverride::Colors(0xA0FF00).Apply(Own);
	EXPECT_STREQ(Shown.m_aSkinName, "pinky");
	EXPECT_TRUE(Shown.m_UseCustomColor);
	EXPECT_EQ(Shown.m_ColorBody, 0xA0FF00);
	// without a colour of their own the feet take that of the body
	EXPECT_EQ(Shown.m_ColorFeet, 0xA0FF00);
}

TEST(TeeInfoOverride, OwnFeetStayUnlessOverridden)
{
	const CTeeInfo Own("default", true, 0x112233, 0x445566);
	EXPECT_EQ(CTeeInfoOverride::Colors(0xA0FF00).Apply(Own).m_ColorFeet, 0x445566);
	EXPECT_EQ(CTeeInfoOverride::Colors(0xA0FF00, 0x00FF80).Apply(Own).m_ColorFeet, 0x00FF80);
}

TEST(TeeInfoOverride, SkinPartsOfSixupGetTheColors)
{
	CTeeInfo Own("default", true, 0x112233, 0x445566);
	Own.ToSixup();
	const CTeeInfo Shown = CTeeInfoOverride::Colors(0xA0FF00, 0x00FF80).Apply(Own);
	const int Body7 = ColorHSLA(0xA0FF00).UnclampLighting(ColorHSLA::DARKEST_LGT).Pack(ColorHSLA::DARKEST_LGT7);
	const int Feet7 = ColorHSLA(0x00FF80).UnclampLighting(ColorHSLA::DARKEST_LGT).Pack(ColorHSLA::DARKEST_LGT7);
	for(int Part : {protocol7::SKINPART_BODY, protocol7::SKINPART_DECORATION, protocol7::SKINPART_HANDS})
	{
		EXPECT_TRUE(Shown.m_aUseCustomColors[Part]);
		EXPECT_EQ(Shown.m_aSkinPartColors[Part], Body7);
	}
	EXPECT_EQ(Shown.m_aSkinPartColors[protocol7::SKINPART_FEET], Feet7);
	for(int Part = 0; Part < protocol7::NUM_SKINPARTS; Part++)
		EXPECT_STREQ(Shown.m_aaSkinPartNames[Part], Own.m_aaSkinPartNames[Part]);
}
