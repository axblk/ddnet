#include "test.h"

#include <base/fs.h>
#include <base/str.h>

#include <engine/map.h>
#include <engine/storage.h>

#include <game/layers.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace
{
	std::vector<std::string> Maps7(IStorage *pStorage)
	{
		std::vector<std::string> vMaps;
		pStorage->ListDirectory(
			IStorage::TYPE_ALL, "maps7", [](const char *pName, int IsDir, int, void *pUser) {
				if(!IsDir && str_endswith(pName, ".map"))
					static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName, str_length(pName) - str_length(".map"));
				return 0;
			},
			&vMaps);
		return vMaps;
	}

	std::unique_ptr<IMap> LoadMap(IStorage *pStorage, const char *pDirectory, const char *pName)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "%s/%s.map", pDirectory, pName);
		std::unique_ptr<IMap> pMap = CreateMap();
		EXPECT_TRUE(pMap->Load(pStorage, aPath, IStorage::TYPE_ALL)) << aPath;
		return pMap;
	}
}

// A 0.7 client plays the version of a map in maps7/, everybody else the map
// in maps/. Both must be the same game: the same tiles, and the same
// rotation of every tile that is not air.
TEST(Maps7, GameLayerIsTheMapsOwn)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	const std::vector<std::string> vMaps = Maps7(pStorage.get());
	EXPECT_GE(vMaps.size(), 16u);
	for(const std::string &Name : vMaps)
	{
		std::unique_ptr<IMap> pMap6 = LoadMap(pStorage.get(), "maps", Name.c_str());
		std::unique_ptr<IMap> pMap7 = LoadMap(pStorage.get(), "maps7", Name.c_str());
		if(!pMap6->IsLoaded() || !pMap7->IsLoaded())
			continue;
		CLayers Layers6, Layers7;
		Layers6.Init(pMap6.get(), true, false);
		Layers7.Init(pMap7.get(), true, false);
		const CMapItemLayerTilemap *pGame6 = Layers6.GameLayer();
		const CMapItemLayerTilemap *pGame7 = Layers7.GameLayer();
		ASSERT_EQ(pGame6->m_Width, pGame7->m_Width) << Name;
		ASSERT_EQ(pGame6->m_Height, pGame7->m_Height) << Name;
		const CTile *pTiles6 = static_cast<const CTile *>(pMap6->GetData(pGame6->m_Data));
		const CTile *pTiles7 = static_cast<const CTile *>(pMap7->GetData(pGame7->m_Data));
		int NumDifferent = 0;
		for(int i = 0; i < pGame6->m_Width * pGame6->m_Height; i++)
		{
			const bool Same = pTiles6[i].m_Index == pTiles7[i].m_Index && (pTiles6[i].m_Index == TILE_AIR || pTiles6[i].m_Flags == pTiles7[i].m_Flags);
			if(!Same && NumDifferent++ == 0)
				ADD_FAILURE() << Name << ": first different tile at " << i % pGame6->m_Width << "," << i / pGame6->m_Width;
		}
		EXPECT_EQ(NumDifferent, 0) << Name;
	}
}

// Teeworlds 0.7 writes image items of the second version. It shipped some
// of its maps in the format of 0.6, and those do not count as 0.7 maps.
TEST(Maps7, Teeworlds07Maps)
{
	CTestInfo Info;
	std::unique_ptr<IStorage> pStorage = Info.CreateTestStorage();
	ASSERT_NE(pStorage, nullptr);
	for(const char *pName : {"ctf1", "ctf2", "ctf7", "dm1", "dm2", "dm8", "dm9"})
		EXPECT_TRUE(IsTeeworlds07Map(LoadMap(pStorage.get(), "maps7", pName).get())) << pName;
	for(const char *pName : {"ctf3", "ctf4", "ctf6", "dm6", "Gold Mine", "LearnToPlay", "Sunny Side Up", "Tsunami", "Tutorial"})
		EXPECT_FALSE(IsTeeworlds07Map(LoadMap(pStorage.get(), "maps7", pName).get())) << pName;
	for(const std::string &Name : Maps7(pStorage.get()))
		EXPECT_FALSE(IsTeeworlds07Map(LoadMap(pStorage.get(), "maps", Name.c_str()).get())) << Name;
}
