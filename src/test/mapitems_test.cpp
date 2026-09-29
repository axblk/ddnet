#include <game/mapitems.h>

#include <gtest/gtest-printers.h>
#include <gtest/gtest.h>

namespace testing::internal
{
	template<>
	class UniversalPrinter<CFixedTime>
	{
	public:
		static void Print(const CFixedTime &FixedTime, std::ostream *pOutputStream)
		{
			*pOutputStream << "CFixedTime with internal value " << FixedTime.GetInternal();
		}
	};

}

TEST(Mapitems, FixedTimeRoundtrip)
{
	for(CFixedTime Fixed = CFixedTime(0); Fixed < CFixedTime(1000000); Fixed += CFixedTime(1))
	{
		ASSERT_EQ(Fixed, CFixedTime::FromSeconds(Fixed.AsSeconds()));
	}
}

TEST(Mapitems, ImageFormat)
{
	CMapItemImage_v2 Image = {};
	Image.m_Version = 1;
	Image.m_MustBe1 = 0; // not read for the first version
	EXPECT_EQ(MapImageFormat(&Image), CImageInfo::FORMAT_RGBA);
	Image.m_Version = 2;
	Image.m_MustBe1 = 1;
	EXPECT_EQ(MapImageFormat(&Image), CImageInfo::FORMAT_RGBA);
	Image.m_MustBe1 = 0;
	EXPECT_EQ(MapImageFormat(&Image), CImageInfo::FORMAT_RGB);
	Image.m_MustBe1 = 2;
	EXPECT_EQ(MapImageFormat(&Image), CImageInfo::FORMAT_UNDEFINED);
	Image.m_MustBe1 = -1;
	EXPECT_EQ(MapImageFormat(&Image), CImageInfo::FORMAT_UNDEFINED);
}
