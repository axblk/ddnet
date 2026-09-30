#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/str.h>

#include <engine/server/map_conversion.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/convert/map_convert.h>
#include <game/map/convert/mapres.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

TEST(MapConversionPlan, Setting)
{
	std::optional<EMapConvertMode> Mode;
	EXPECT_TRUE(ParseServerMapConvert("hybrid", Mode));
	EXPECT_EQ(Mode, EMapConvertMode::HYBRID);
	EXPECT_TRUE(ParseServerMapConvert("remap", Mode));
	EXPECT_EQ(Mode, EMapConvertMode::REMAP);
	EXPECT_TRUE(ParseServerMapConvert("embed", Mode));
	EXPECT_EQ(Mode, EMapConvertMode::EMBED);
	EXPECT_TRUE(ParseServerMapConvert("off", Mode));
	EXPECT_FALSE(Mode.has_value());

	// Marking is for the map tool, and auto is gone
	Mode = EMapConvertMode::REMAP;
	EXPECT_FALSE(ParseServerMapConvert("mark", Mode));
	EXPECT_FALSE(ParseServerMapConvert("auto", Mode));
	EXPECT_FALSE(ParseServerMapConvert("", Mode));
	EXPECT_FALSE(ParseServerMapConvert("HYBRID", Mode));
	EXPECT_EQ(Mode, EMapConvertMode::REMAP);
}

TEST(MapConversionPlan, WhoGetsWhat)
{
	// A DDNet or 0.6 map: DDNet clients get it as it is, 0.7 clients its
	// version in maps7/, a conversion, or nothing
	CServedMaps Served = PlanServedMaps(false, true, true);
	EXPECT_EQ(Served.m_DDNet, EServedMap::ORIGINAL);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::MAPS7);
	Served = PlanServedMaps(false, true, false);
	EXPECT_EQ(Served.m_DDNet, EServedMap::ORIGINAL);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::MAPS7);
	Served = PlanServedMaps(false, false, true);
	EXPECT_EQ(Served.m_DDNet, EServedMap::ORIGINAL);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::CONVERTED);
	Served = PlanServedMaps(false, false, false);
	EXPECT_EQ(Served.m_DDNet, EServedMap::ORIGINAL);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::NONE);

	// A map Teeworlds 0.7 wrote: 0.7 clients get it as it is, unless maps7/
	// has a version of it; DDNet clients a conversion, or the map as it is
	Served = PlanServedMaps(true, false, true);
	EXPECT_EQ(Served.m_DDNet, EServedMap::CONVERTED);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::ORIGINAL);
	Served = PlanServedMaps(true, true, true);
	EXPECT_EQ(Served.m_DDNet, EServedMap::CONVERTED);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::MAPS7);
	Served = PlanServedMaps(true, false, false);
	EXPECT_EQ(Served.m_DDNet, EServedMap::ORIGINAL);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::ORIGINAL);
	Served = PlanServedMaps(true, true, false);
	EXPECT_EQ(Served.m_DDNet, EServedMap::ORIGINAL);
	EXPECT_EQ(Served.m_Teeworlds07, EServedMap::MAPS7);
}

TEST(MapConversionPlan, NamesTeeworlds07Takes)
{
	// The maps Teeworlds 0.7 comes with are only taken as they are
	EXPECT_TRUE(Teeworlds07AcceptsMap("ctf5", 0x20b97905, 10292));
	EXPECT_TRUE(Teeworlds07AcceptsMap("ctf5", 0x3a1694a7, 10250));
	EXPECT_FALSE(Teeworlds07AcceptsMap("ctf5", 0x20b97905, 10293));
	EXPECT_FALSE(Teeworlds07AcceptsMap("dm7", 0x12345678, 9685));
	EXPECT_TRUE(Teeworlds07AcceptsMap("ctf5_07", 0x12345678, 1));
	EXPECT_TRUE(Teeworlds07AcceptsMap("Tutorial", 0x12345678, 1));

	char aName[128];
	EXPECT_TRUE(Teeworlds07MapName(aName, sizeof(aName), "dm1", 0x64548818, 6793));
	EXPECT_STREQ(aName, "dm1");
	EXPECT_FALSE(Teeworlds07MapName(aName, sizeof(aName), "dm7", 0x12345678, 1));
	EXPECT_STREQ(aName, "dm7_ddnet");
	EXPECT_TRUE(Teeworlds07MapName(aName, sizeof(aName), "whitehell", 0x12345678, 1));
	EXPECT_STREQ(aName, "whitehell");
}

TEST(MapConversionPlan, CacheKey)
{
	SHA256_DIGEST Sha256 = {};
	Sha256.data[0] = 0xab;
	char aPath[IO_MAX_PATH_LENGTH];
	MapConversionCachePath(aPath, sizeof(aPath), "mapcache", "subfolder/ctf5", Sha256, EMapConvertDirection::TO07, EMapConvertMode::HYBRID);
	char aExpected[IO_MAX_PATH_LENGTH];
	str_format(aExpected, sizeof(aExpected), "mapcache/to07/ctf5_ab%s_hybrid_%08x.map", std::string(62, '0').c_str(), MapConvertVersion());
	EXPECT_STREQ(aPath, aExpected);

	// Everything that changes the converted map changes the path
	char aOther[IO_MAX_PATH_LENGTH];
	MapConversionCachePath(aOther, sizeof(aOther), "mapcache", "subfolder/ctf5", Sha256, EMapConvertDirection::TO06, EMapConvertMode::HYBRID);
	EXPECT_STRNE(aPath, aOther);
	MapConversionCachePath(aOther, sizeof(aOther), "mapcache", "subfolder/ctf5", Sha256, EMapConvertDirection::TO07, EMapConvertMode::REMAP);
	EXPECT_STRNE(aPath, aOther);
	Sha256.data[31] = 1;
	MapConversionCachePath(aOther, sizeof(aOther), "mapcache", "subfolder/ctf5", Sha256, EMapConvertDirection::TO07, EMapConvertMode::HYBRID);
	EXPECT_STRNE(aPath, aOther);
}

class MapConversion : public ::testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CTestInfo m_Info;
	std::unique_ptr<IStorage> m_pStorage;
	std::shared_ptr<CMapresFromStorage> m_pMapres;

	void SetUp() override
	{
		m_Info.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_Info.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
		m_pMapres = std::make_shared<CMapresFromStorage>(m_pStorage.get());
	}

	CMapConversionRequest Request(const char *pPath, EMapConvertDirection Direction, bool Cache)
	{
		CMapConversionRequest Request;
		void *pData;
		unsigned Size;
		EXPECT_TRUE(m_pStorage->ReadFile(pPath, IStorage::TYPE_ALL, &pData, &Size)) << pPath;
		if(pData != nullptr)
		{
			Request.m_vSource.assign(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
			free(pData);
		}
		// The name as the server knows the map by, without the extension
		Request.m_MapName = fs_filename(pPath);
		Request.m_MapName.resize(Request.m_MapName.size() - str_length(".map"));
		Request.m_Options.m_Direction = Direction;
		Request.m_Options.m_Mode = EMapConvertMode::HYBRID;
		Request.m_pMapres = m_pMapres;
		if(Cache)
		{
			Request.m_pCacheStorage = m_pStorage.get();
			Request.m_CacheFolder = "mapcache";
		}
		return Request;
	}

	bool CacheHas(const CMapConversion &Conversion, const char *pMapName)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		MapConversionCachePath(aPath, sizeof(aPath), "mapcache", pMapName, Conversion.m_SourceSha256, Conversion.m_Direction, Conversion.m_Mode);
		return m_pStorage->FileExists(aPath, IStorage::TYPE_SAVE);
	}
};

TEST_F(MapConversion, ConvertsWhatNeedsIt)
{
	// ctf5 has no version in maps7/ and uses tiles 0.7 moved
	std::shared_ptr<CMapConversion> pConversion = ConvertServedMap(Request("maps/ctf5.map", EMapConvertDirection::TO07, false));
	ASSERT_NE(pConversion, nullptr);
	EXPECT_TRUE(pConversion->m_Ok) << pConversion->m_Result.m_Error;
	EXPECT_TRUE(pConversion->m_Result.m_Converted);
	EXPECT_FALSE(pConversion->m_FromCache);
	EXPECT_FALSE(pConversion->m_Cached);
	EXPECT_EQ(pConversion->m_Result.m_Mode, EMapConvertMode::HYBRID);
	const std::vector<uint8_t> &vData = pConversion->m_Result.m_vData;
	EXPECT_EQ(pConversion->m_Result.m_Sha256, sha256(vData.data(), vData.size()));
	CDataFileReader Reader;
	ASSERT_TRUE(Reader.OpenFromMemory("ctf5", vData, "memory"));
	EXPECT_TRUE(IsTeeworlds07Map(Reader));
	CMapConvertProvenance Provenance;
	ASSERT_TRUE(ReadMapConvertProvenance(Reader, Provenance));
	EXPECT_EQ(Provenance.m_SourceSha256, pConversion->m_SourceSha256);
	EXPECT_EQ(Reader.Crc(), pConversion->m_Result.m_Crc);

	// dm6 looks the same in both versions, both kinds of clients get the file
	pConversion = ConvertServedMap(Request("maps/dm6.map", EMapConvertDirection::TO07, true));
	ASSERT_NE(pConversion, nullptr);
	EXPECT_TRUE(pConversion->m_Ok);
	EXPECT_FALSE(pConversion->m_Result.m_Converted);
	EXPECT_TRUE(pConversion->m_Result.m_vData.empty());
	EXPECT_FALSE(pConversion->m_Cached);
	EXPECT_FALSE(CacheHas(*pConversion, "dm6"));

	// A map Teeworlds 0.7 wrote, for DDNet clients
	pConversion = ConvertServedMap(Request("test/maps07/ctf5.map", EMapConvertDirection::TO06, false));
	ASSERT_NE(pConversion, nullptr);
	EXPECT_TRUE(pConversion->m_Ok);
	EXPECT_TRUE(pConversion->m_Result.m_Converted);
	CDataFileReader Reader06;
	ASSERT_TRUE(Reader06.OpenFromMemory("ctf5", pConversion->m_Result.m_vData, "memory"));
	EXPECT_FALSE(IsTeeworlds07Map(Reader06));
}

TEST_F(MapConversion, BrokenMap)
{
	CMapConversionRequest Broken;
	Broken.m_MapName = "broken";
	Broken.m_vSource = {1, 2, 3, 4};
	Broken.m_pMapres = m_pMapres;
	std::shared_ptr<CMapConversion> pConversion = ConvertServedMap(std::move(Broken));
	ASSERT_NE(pConversion, nullptr);
	EXPECT_FALSE(pConversion->m_Ok);
	EXPECT_FALSE(pConversion->m_Result.m_Converted);
	EXPECT_FALSE(pConversion->m_Result.m_Error.empty());
}

TEST_F(MapConversion, DiskCache)
{
	std::shared_ptr<CMapConversion> pFirst = ConvertServedMap(Request("maps/ctf5.map", EMapConvertDirection::TO07, true));
	ASSERT_NE(pFirst, nullptr);
	ASSERT_TRUE(pFirst->m_Result.m_Converted);
	EXPECT_TRUE(pFirst->m_Cached);
	EXPECT_TRUE(CacheHas(*pFirst, "ctf5"));

	std::shared_ptr<CMapConversion> pSecond = ConvertServedMap(Request("maps/ctf5.map", EMapConvertDirection::TO07, true));
	ASSERT_NE(pSecond, nullptr);
	EXPECT_TRUE(pSecond->m_Ok);
	EXPECT_TRUE(pSecond->m_FromCache);
	EXPECT_FALSE(pSecond->m_Cached);
	EXPECT_EQ(pSecond->m_Result.m_vData, pFirst->m_Result.m_vData);
	EXPECT_EQ(pSecond->m_Result.m_Sha256, pFirst->m_Result.m_Sha256);
	EXPECT_EQ(pSecond->m_Result.m_Crc, pFirst->m_Result.m_Crc);
	EXPECT_EQ(pSecond->m_Result.m_Mode, EMapConvertMode::HYBRID);

	// Another mode is another conversion
	CMapConversionRequest Remap = Request("maps/ctf5.map", EMapConvertDirection::TO07, true);
	Remap.m_Options.m_Mode = EMapConvertMode::REMAP;
	std::shared_ptr<CMapConversion> pRemap = ConvertServedMap(std::move(Remap));
	ASSERT_NE(pRemap, nullptr);
	EXPECT_FALSE(pRemap->m_FromCache);
	EXPECT_TRUE(pRemap->m_Cached);
	EXPECT_NE(pRemap->m_Result.m_vData, pFirst->m_Result.m_vData);

	// A file in the cache that is not what its name says is converted again
	char aPath[IO_MAX_PATH_LENGTH];
	MapConversionCachePath(aPath, sizeof(aPath), "mapcache", "ctf5", pFirst->m_SourceSha256, EMapConvertDirection::TO07, EMapConvertMode::HYBRID);
	IOHANDLE File = m_pStorage->OpenFile(aPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	ASSERT_TRUE(File);
	io_write(File, "not a map", 9);
	io_close(File);
	std::shared_ptr<CMapConversion> pThird = ConvertServedMap(Request("maps/ctf5.map", EMapConvertDirection::TO07, true));
	ASSERT_NE(pThird, nullptr);
	EXPECT_FALSE(pThird->m_FromCache);
	EXPECT_TRUE(pThird->m_Cached);
	EXPECT_EQ(pThird->m_Result.m_vData, pFirst->m_Result.m_vData);
}

TEST_F(MapConversion, DiskCacheKeepsToItsSize)
{
	std::shared_ptr<CMapConversion> pCtf5 = ConvertServedMap(Request("maps/ctf5.map", EMapConvertDirection::TO07, true));
	ASSERT_NE(pCtf5, nullptr);
	ASSERT_TRUE(pCtf5->m_Cached);

	// Room for one map only: the older one goes
	CMapConversionRequest Dm7 = Request("maps/dm7.map", EMapConvertDirection::TO07, true);
	Dm7.m_CacheMaxSize = pCtf5->m_Result.m_vData.size() + 1;
	std::shared_ptr<CMapConversion> pDm7 = ConvertServedMap(std::move(Dm7));
	ASSERT_NE(pDm7, nullptr);
	ASSERT_TRUE(pDm7->m_Cached);
	EXPECT_TRUE(CacheHas(*pDm7, "dm7"));
	EXPECT_FALSE(CacheHas(*pCtf5, "ctf5"));
}

TEST_F(MapConversion, Aborted)
{
	std::shared_ptr<CMapConversion> pConversion = ConvertServedMap(Request("maps/ctf5.map", EMapConvertDirection::TO07, true), []() { return true; });
	EXPECT_EQ(pConversion, nullptr);
	SHA256_DIGEST Sha256;
	void *pData;
	unsigned Size;
	ASSERT_TRUE(m_pStorage->ReadFile("maps/ctf5.map", IStorage::TYPE_ALL, &pData, &Size));
	Sha256 = sha256(pData, Size);
	free(pData);
	char aPath[IO_MAX_PATH_LENGTH];
	MapConversionCachePath(aPath, sizeof(aPath), "mapcache", "ctf5", Sha256, EMapConvertDirection::TO07, EMapConvertMode::HYBRID);
	EXPECT_FALSE(m_pStorage->FileExists(aPath, IStorage::TYPE_SAVE));
}
