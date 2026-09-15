#include <base/str.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/map_file.h>
#include <game/mapitems.h>

#include <gtest/gtest.h>

#include <memory>

using namespace map_document;

// The maps that come with the game are the only maps there are to read here,
// so they are what the reader is held against: every one of them has to come
// out with the groups, layers, images and envelopes the file says it has, and
// with its tiles where the file has them.

namespace
{
	class CMapFile
	{
	public:
		std::unique_ptr<IStorage> m_pStorage = CreateLocalStorage();
		CDataFileReader m_File;
		CMapState m_State;
		std::vector<std::string> m_vWarnings;

		bool Read(const char *pName)
		{
			char aPath[IO_MAX_PATH_LENGTH];
			str_format(aPath, sizeof(aPath), "data/maps/%s.map", pName);
			if(!m_File.Open(m_pStorage.get(), aPath, IStorage::TYPE_ALL))
				return false;
			return ReadMapState(m_File, &m_State, &m_vWarnings);
		}
	};

	std::vector<std::string> MapNames()
	{
		std::vector<std::string> vNames;
		std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
		pStorage->ListDirectory(IStorage::TYPE_ALL, "data/maps", [](const char *pName, int IsDir, int, void *pUser) {
			if(!IsDir && str_endswith(pName, ".map"))
			{
				std::string Name(pName);
				static_cast<std::vector<std::string> *>(pUser)->push_back(Name.substr(0, Name.size() - 4));
			}
			return 0; }, &vNames);
		return vNames;
	}
}

TEST(MapFile, ReadsEveryMapTheGameShipsWith)
{
	const std::vector<std::string> vNames = MapNames();
	ASSERT_GT(vNames.size(), 10u) << "no maps found to read";

	for(const std::string &Name : vNames)
	{
		CMapFile Map;
		ASSERT_TRUE(Map.Read(Name.c_str())) << Name;
		EXPECT_TRUE(Map.m_vWarnings.empty()) << Name << ": " << (Map.m_vWarnings.empty() ? "" : Map.m_vWarnings[0]);

		// What the file says it holds is what came out of it.
		int Start, Num;
		Map.m_File.GetType(MAPITEMTYPE_GROUP, &Start, &Num);
		EXPECT_EQ(Map.m_State.NumGroups(), (size_t)Num) << Name;
		Map.m_File.GetType(MAPITEMTYPE_IMAGE, &Start, &Num);
		EXPECT_EQ(Map.m_State.NumImages(), (size_t)Num) << Name;
		Map.m_File.GetType(MAPITEMTYPE_SOUND, &Start, &Num);
		EXPECT_EQ(Map.m_State.NumSounds(), (size_t)Num) << Name;
		Map.m_File.GetType(MAPITEMTYPE_ENVELOPE, &Start, &Num);
		EXPECT_EQ(Map.m_State.NumEnvelopes(), (size_t)Num) << Name;

		size_t Layers = 0;
		for(size_t g = 0; g < Map.m_State.NumGroups(); ++g)
		{
			Layers += Map.m_State.NumLayers(g);
		}
		Map.m_File.GetType(MAPITEMTYPE_LAYER, &Start, &Num);
		EXPECT_EQ(Layers, (size_t)Num) << Name;
	}
}

TEST(MapFile, ReadsTheTilesWhereTheFileHasThem)
{
	CMapFile Map;
	ASSERT_TRUE(Map.Read("ctf1"));

	// Every tile layer of the map, tile by tile against the data item it was
	// read from - the store has to answer what the file holds, air included.
	int LayersStart, LayersNum;
	Map.m_File.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);
	int Checked = 0;
	size_t Layer = 0;
	for(size_t g = 0; g < Map.m_State.NumGroups(); ++g)
	{
		for(size_t l = 0; l < Map.m_State.NumLayers(g); ++l, ++Layer)
		{
			const CMapItemLayer *pItem = static_cast<CMapItemLayer *>(Map.m_File.GetItem(LayersStart + (int)Layer));
			if(pItem->m_Type != LAYERTYPE_TILES)
				continue;
			const CMapItemLayerTilemap *pTilemap = reinterpret_cast<const CMapItemLayerTilemap *>(pItem);
			const CTileLayer *pLayer = Map.m_State.TileLayer(g, l);
			ASSERT_EQ(pLayer->Width(), pTilemap->m_Width);
			ASSERT_EQ(pLayer->Height(), pTilemap->m_Height);

			const CTile *pTiles = static_cast<CTile *>(Map.m_File.GetData(pTilemap->m_Data));
			ASSERT_NE(pTiles, nullptr);
			for(int y = 0; y < pTilemap->m_Height; ++y)
			{
				for(int x = 0; x < pTilemap->m_Width; ++x)
				{
					const CTile &Expected = pTiles[y * pTilemap->m_Width + x];
					const CTile Got = pLayer->m_Tiles.Get(x, y);
					ASSERT_EQ(Got.m_Index, Expected.m_Index) << "at " << x << "," << y << " of layer " << Layer;
					ASSERT_EQ(Got.m_Flags, Expected.m_Flags) << "at " << x << "," << y << " of layer " << Layer;
					ASSERT_EQ(Got.m_Skip, Expected.m_Skip) << "at " << x << "," << y << " of layer " << Layer;
				}
			}
			++Checked;
		}
	}
	EXPECT_GT(Checked, 0);
}

TEST(MapFile, ReadsWhatALayerPointsAt)
{
	CMapFile Map;
	ASSERT_TRUE(Map.Read("Tutorial"));

	EXPECT_FALSE(Map.m_State.m_Info.m_Author.empty());
	// A map that names images has them, and an embedded one has its pixels.
	ASSERT_GT(Map.m_State.NumImages(), 0u);
	bool AnyEmbedded = false;
	for(size_t i = 0; i < Map.m_State.NumImages(); ++i)
	{
		const CImage *pImage = Map.m_State.Image(i);
		EXPECT_FALSE(pImage->m_Name.empty());
		if(pImage->m_External)
		{
			EXPECT_TRUE(pImage->m_Data.Empty());
			continue;
		}
		AnyEmbedded = true;
		EXPECT_EQ(pImage->m_Data.Size(), (size_t)pImage->m_Width * pImage->m_Height * 4);
	}
	EXPECT_TRUE(AnyEmbedded);

	// The physics layers are there, and their second plane with them.
	bool FoundGame = false;
	bool FoundSecondPlane = false;
	for(size_t g = 0; g < Map.m_State.NumGroups(); ++g)
	{
		for(size_t l = 0; l < Map.m_State.NumLayers(g); ++l)
		{
			const CLayer *pLayer = Map.m_State.Layer(g, l);
			if(!std::holds_alternative<CTileLayer>(*pLayer))
				continue;
			const CTileLayer &Tiles = std::get<CTileLayer>(*pLayer);
			if(Tiles.m_Kind == ETileLayerKind::GAME)
			{
				FoundGame = true;
				EXPECT_GT(Tiles.m_Tiles.UsedChunks(), 0);
			}
			if(std::holds_alternative<CTileStore<CSwitchTile>>(Tiles.m_ExtraTiles))
			{
				FoundSecondPlane = true;
				// The plane every layer has is air in the file for these, so
				// what the layer holds is the second one.
				EXPECT_EQ(Tiles.m_Tiles.UsedChunks(), 0);
				EXPECT_GT(std::get<CTileStore<CSwitchTile>>(Tiles.m_ExtraTiles).UsedChunks(), 0);
			}
		}
	}
	EXPECT_TRUE(FoundGame);
	EXPECT_TRUE(FoundSecondPlane);
}
