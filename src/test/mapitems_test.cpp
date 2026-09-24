#include "test.h"

#include <base/mem.h>

#include <engine/map.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/mapitems.h>

#include <gtest/gtest-printers.h>
#include <gtest/gtest.h>

#include <memory>

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

TEST(Mapitems, ImagesRedrawnFor07)
{
	for(const char *pName : {"desert_main", "easter", "generic_shadows", "generic_unhookable", "grass_doodads", "grass_main", "winter_main"})
		EXPECT_TRUE(IsMapImageRedrawnFor07(pName)) << pName;
	for(const char *pName : {"", "desert_doodads", "grass_main_0.7", "jungle_main", "Grass_main"})
		EXPECT_FALSE(IsMapImageRedrawnFor07(pName)) << pName;
}

// Writes the map pFrom again as pTo, with every image item in the second
// version, the one Teeworlds 0.7 writes, and the format field set to Format.
static void WriteWithImageFormat(IStorage *pStorage, const char *pFrom, const char *pTo, int Format)
{
	CDataFileReader Reader;
	ASSERT_TRUE(Reader.Open(pStorage, pFrom, IStorage::TYPE_ALL));
	CDataFileWriter Writer;
	ASSERT_TRUE(Writer.Open(pStorage, pTo));
	for(int Index = 0; Index < Reader.NumItems(); Index++)
	{
		int Type, Id;
		CUuid Uuid;
		const void *pItem = Reader.GetItem(Index, &Type, &Id, &Uuid);
		if(Type == ITEMTYPE_EX)
			continue;
		if(Type == MAPITEMTYPE_IMAGE)
		{
			CMapItemImage_v2 Image = {};
			mem_copy(&Image, pItem, sizeof(CMapItemImage_v1));
			Image.m_Version = 2;
			Image.m_MustBe1 = Format;
			Writer.AddItem(Type, Id, sizeof(Image), &Image, &Uuid);
		}
		else
		{
			Writer.AddItem(Type, Id, Reader.GetItemSize(Index), pItem, &Uuid);
		}
	}
	for(int Index = 0; Index < Reader.NumData(); Index++)
		Writer.AddData(Reader.GetDataSize(Index), Reader.GetData(Index));
	Writer.Finish();
}

TEST(Mapitems, Teeworlds07Map)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	std::unique_ptr<IMap> pMap = CreateMap();

	// DDNet writes the first version of the image item
	ASSERT_TRUE(pMap->Load(pStorage.get(), "maps/dm1.map", IStorage::TYPE_ALL));
	EXPECT_FALSE(IsTeeworlds07Map(pMap.get()));

	char aFilename[IO_MAX_PATH_LENGTH];
	Info.Filename(aFilename, sizeof(aFilename), ".map");
	for(int Format : {0, 1, 2})
	{
		WriteWithImageFormat(pStorage.get(), "maps/dm1.map", aFilename, Format);
		ASSERT_TRUE(pMap->Load(pStorage.get(), aFilename, IStorage::TYPE_SAVE)) << Format;
		// 0 is RGB and 1 RGBA, no map writer uses another format
		EXPECT_EQ(IsTeeworlds07Map(pMap.get()), Format != 2) << Format;
	}
	pMap->Unload();
}
