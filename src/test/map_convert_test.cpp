#include "test.h"

#include <base/mem.h>
#include <base/str.h>

#include <engine/shared/datafile.h>
#include <engine/shared/map.h>
#include <engine/storage.h>

#include <game/gamecore.h>
#include <game/map/convert/map07_tables.h>
#include <game/map/convert/map_convert.h>
#include <game/map/convert/mapres.h>
#include <game/mapitems.h>
#include <game/mapitems_ex.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

static constexpr int ORIENTATION = TILEFLAG_XFLIP | TILEFLAG_YFLIP | TILEFLAG_ROTATE;

static std::vector<uint8_t> ReadMapFile(IStorage *pStorage, const char *pPath, int StorageType)
{
	void *pData;
	unsigned Size;
	if(!pStorage->ReadFile(pPath, StorageType, &pData, &Size))
	{
		return {};
	}
	std::vector<uint8_t> vData(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
	free(pData);
	return vData;
}

static bool Is07(const std::vector<uint8_t> &vMap)
{
	CDataFileReader Reader;
	EXPECT_TRUE(Reader.OpenFromMemory("map", vMap, "memory"));
	return IsTeeworlds07Map(Reader);
}

static bool NeedsConversion(const std::vector<uint8_t> &vMap, EMapConvertDirection Direction)
{
	CDataFileReader Reader;
	EXPECT_TRUE(Reader.OpenFromMemory("map", vMap, "memory"));
	return MapNeedsConversion(Reader, Direction);
}

class CLayerInfo
{
public:
	int m_Group;
	int m_Width;
	int m_Height;
	int m_Version;
	int m_Image;
	std::string m_ImageName;
	bool m_External;
	// What a layer that holds some of another layer's tiles keeps of it
	std::array<int, 4> m_aColor;
	int m_ColorEnv;
	int m_ColorEnvOffset;
	int m_LayerFlags;
	std::vector<CTile> m_vTiles;
};

// The tile layers with a picture, in the order they are drawn
static std::vector<CLayerInfo> TileLayers(const std::vector<uint8_t> &vMap)
{
	CDataFileReader Reader;
	EXPECT_TRUE(Reader.OpenFromMemory("map", vMap, "memory"));
	int ImageStart, ImageNum, LayerStart, LayerNum, GroupStart, GroupNum;
	Reader.GetType(MAPITEMTYPE_IMAGE, &ImageStart, &ImageNum);
	Reader.GetType(MAPITEMTYPE_LAYER, &LayerStart, &LayerNum);
	Reader.GetType(MAPITEMTYPE_GROUP, &GroupStart, &GroupNum);
	std::vector<CLayerInfo> vLayers;
	for(int Group = 0; Group < GroupNum; Group++)
	{
		const CMapItemGroup_v1 *pGroup = static_cast<const CMapItemGroup_v1 *>(Reader.GetItem(GroupStart + Group));
		for(int Layer = pGroup->m_StartLayer; Layer < pGroup->m_StartLayer + pGroup->m_NumLayers; Layer++)
		{
			const CMapItemLayerTilemap_v2 *pTilemap = static_cast<const CMapItemLayerTilemap_v2 *>(Reader.GetItem(LayerStart + Layer));
			if(pTilemap->m_Layer.m_Type != LAYERTYPE_TILES || (pTilemap->m_Flags & TILESLAYERFLAG_GAME) || pTilemap->m_Image < 0 || pTilemap->m_Image >= ImageNum)
			{
				continue;
			}
			CLayerInfo Info;
			Info.m_Group = Group;
			Info.m_Width = pTilemap->m_Width;
			Info.m_Height = pTilemap->m_Height;
			Info.m_Version = pTilemap->m_Version;
			Info.m_Image = pTilemap->m_Image;
			const CMapItemImage_v1 *pImage = static_cast<const CMapItemImage_v1 *>(Reader.GetItem(ImageStart + pTilemap->m_Image));
			Info.m_ImageName = Reader.GetDataString(pImage->m_ImageName);
			Info.m_External = pImage->m_External;
			Info.m_aColor = {pTilemap->m_Color.r, pTilemap->m_Color.g, pTilemap->m_Color.b, pTilemap->m_Color.a};
			Info.m_ColorEnv = pTilemap->m_ColorEnv;
			Info.m_ColorEnvOffset = pTilemap->m_ColorEnvOffset;
			Info.m_LayerFlags = pTilemap->m_Layer.m_Flags;
			const CTile *pTiles = static_cast<const CTile *>(Reader.GetData(pTilemap->m_Data));
			const size_t Stored = Reader.GetDataSize(pTilemap->m_Data) / sizeof(CTile);
			for(size_t i = 0; i < Stored; i++)
			{
				const int Repeat = pTilemap->m_Version >= 4 ? pTiles[i].m_Skip + 1 : 1;
				for(int r = 0; r < Repeat; r++)
				{
					Info.m_vTiles.push_back(CTile{pTiles[i].m_Index, pTiles[i].m_Flags, 0, 0});
				}
			}
			const size_t Count = (size_t)pTilemap->m_Width * pTilemap->m_Height;
			EXPECT_EQ(Info.m_vTiles.size(), Count);
			Info.m_vTiles.resize(Count);
			vLayers.push_back(std::move(Info));
		}
	}
	return vLayers;
}

// A tile of a tile layer with an external picture, and where it is
class CPlacedTile
{
public:
	int m_Group;
	int m_Width;
	int m_Cell;
	std::string m_Image;
	int m_Index;
	int m_Flags;

	bool operator<(const CPlacedTile &Other) const { return std::tie(m_Group, m_Width, m_Cell, m_Image, m_Index, m_Flags) < std::tie(Other.m_Group, Other.m_Width, Other.m_Cell, Other.m_Image, Other.m_Index, Other.m_Flags); }
	bool operator==(const CPlacedTile &Other) const { return std::tie(m_Group, m_Width, m_Cell, m_Image, m_Index, m_Flags) == std::tie(Other.m_Group, Other.m_Width, Other.m_Cell, Other.m_Image, Other.m_Index, Other.m_Flags); }
};

static std::vector<CPlacedTile> PlacedTiles(const std::vector<uint8_t> &vMap, const std::vector<std::string> &vNames)
{
	std::vector<CPlacedTile> vPlaced;
	for(const CLayerInfo &Layer : TileLayers(vMap))
	{
		if(!Layer.m_External || std::find(vNames.begin(), vNames.end(), Layer.m_ImageName) == vNames.end())
		{
			continue;
		}
		for(size_t i = 0; i < Layer.m_vTiles.size(); i++)
		{
			if(Layer.m_vTiles[i].m_Index != 0)
			{
				vPlaced.push_back(CPlacedTile{Layer.m_Group, Layer.m_Width, (int)i, Layer.m_ImageName, Layer.m_vTiles[i].m_Index, Layer.m_vTiles[i].m_Flags & ORIENTATION});
			}
		}
	}
	std::sort(vPlaced.begin(), vPlaced.end());
	return vPlaced;
}

static const Map07Tables::CTileTable *Table(const Map07Tables::CTileTable *pTables, size_t NumTables, Map07Tables::ESet Source, Map07Tables::ESet Home)
{
	for(size_t i = 0; i < NumTables; i++)
	{
		if(pTables[i].m_Source == Source && pTables[i].m_Home == Home)
		{
			return &pTables[i];
		}
	}
	return nullptr;
}

static const Map07Tables::CTileTable *To07(Map07Tables::ESet Set)
{
	return Table(Map07Tables::TO07, std::size(Map07Tables::TO07), Set, Set);
}

static const Map07Tables::CTileTable *To06(Map07Tables::ESet Set, Map07Tables::ESet Home)
{
	return Table(Map07Tables::TO06, std::size(Map07Tables::TO06), Set, Home);
}

// A small map written from scratch
class CTestMap
{
public:
	CDataFileWriter m_Writer;
	int m_NumImages = 0;
	int m_NumLayers = 0;
	int m_NumGroups = 0;
	int m_GroupStart = 0;

	CTestMap()
	{
		CMapItemVersion Version;
		Version.m_Version = 1;
		m_Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(Version), &Version);
	}

	int AddImage(const char *pName, bool External, int Version, int Format, int Width, int Height, const std::vector<uint8_t> &vPixels)
	{
		CMapItemImage_v2 Image;
		Image.m_Version = Version;
		Image.m_Width = Width;
		Image.m_Height = Height;
		Image.m_External = External;
		Image.m_ImageName = m_Writer.AddDataString(pName);
		Image.m_ImageData = vPixels.empty() ? -1 : m_Writer.AddData(vPixels.size(), vPixels.data());
		Image.m_MustBe1 = Format;
		m_Writer.AddItem(MAPITEMTYPE_IMAGE, m_NumImages, Version >= 2 ? sizeof(CMapItemImage_v2) : sizeof(CMapItemImage_v1), &Image);
		return m_NumImages++;
	}

	int AddTileLayer(int Image, int Width, int Height, const std::vector<CTile> &vTiles, int Flags = 0)
	{
		CMapItemLayerTilemap Layer = {};
		Layer.m_Layer.m_Type = LAYERTYPE_TILES;
		Layer.m_Version = 3;
		Layer.m_Width = Width;
		Layer.m_Height = Height;
		Layer.m_Flags = Flags;
		Layer.m_Color = CColor(255, 255, 255, 255);
		Layer.m_ColorEnv = -1;
		Layer.m_Image = Image;
		Layer.m_Data = m_Writer.AddData(vTiles.size() * sizeof(CTile), vTiles.data());
		Layer.m_Tele = Layer.m_Speedup = Layer.m_Front = Layer.m_Switch = Layer.m_Tune = -1;
		StrToInts(Layer.m_aName, std::size(Layer.m_aName), Flags & TILESLAYERFLAG_GAME ? "Game" : "Tiles");
		m_Writer.AddItem(MAPITEMTYPE_LAYER, m_NumLayers, sizeof(Layer), &Layer);
		return m_NumLayers++;
	}

	// A quad showing the part of the picture from (Left, Top) to (Right, Bottom), 1024 being all of it
	int AddQuadsLayer(int Image, int Left = 0, int Top = 0, int Right = 1024, int Bottom = 1024)
	{
		CQuad Quad = {};
		for(auto &Color : Quad.m_aColors)
			Color = CColor(255, 255, 255, 255);
		Quad.m_aTexcoords[0].x = Quad.m_aTexcoords[2].x = Left;
		Quad.m_aTexcoords[1].x = Quad.m_aTexcoords[3].x = Right;
		Quad.m_aTexcoords[0].y = Quad.m_aTexcoords[1].y = Top;
		Quad.m_aTexcoords[2].y = Quad.m_aTexcoords[3].y = Bottom;
		Quad.m_PosEnv = Quad.m_ColorEnv = -1;
		CMapItemLayerQuads Layer = {};
		Layer.m_Layer.m_Type = LAYERTYPE_QUADS;
		Layer.m_Version = 2;
		Layer.m_NumQuads = 1;
		Layer.m_Data = m_Writer.AddData(sizeof(Quad), &Quad);
		Layer.m_Image = Image;
		StrToInts(Layer.m_aName, std::size(Layer.m_aName), "Quads");
		m_Writer.AddItem(MAPITEMTYPE_LAYER, m_NumLayers, sizeof(Layer), &Layer);
		return m_NumLayers++;
	}

	void EndGroup()
	{
		CMapItemGroup Group = {};
		Group.m_Version = 3;
		Group.m_ParallaxX = Group.m_ParallaxY = 100;
		Group.m_StartLayer = m_GroupStart;
		Group.m_NumLayers = m_NumLayers - m_GroupStart;
		StrToInts(Group.m_aName, std::size(Group.m_aName), "Group");
		m_Writer.AddItem(MAPITEMTYPE_GROUP, m_NumGroups++, sizeof(Group), &Group);
		m_GroupStart = m_NumLayers;
	}

	std::vector<uint8_t> Finish()
	{
		return m_Writer.FinishToMemory();
	}
};

static std::vector<CTile> Tiles(std::initializer_list<CTile> Tiles, size_t Count)
{
	std::vector<CTile> vTiles(Tiles);
	vTiles.resize(Count, CTile{0, 0, 0, 0});
	return vTiles;
}

class MapConvert : public ::testing::Test // NOLINT(readability-identifier-naming)
{
protected:
	CTestInfo m_Info;
	std::unique_ptr<IStorage> m_pStorage;
	std::unique_ptr<CMapresFromStorage> m_pMapres;

	void SetUp() override
	{
		// Reads maps/ and mapres/ of the data directory
		m_Info.m_DeleteTestStorageFilesOnSuccess = true;
		m_pStorage = m_Info.CreateTestStorage();
		ASSERT_NE(m_pStorage, nullptr);
		m_pMapres = std::make_unique<CMapresFromStorage>(m_pStorage.get());
	}

	// A map DDNet ships, in its 0.6 form
	std::vector<uint8_t> DDNetMap(const char *pName)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "maps/%s.map", pName);
		std::vector<uint8_t> vData = ReadMapFile(m_pStorage.get(), aPath, IStorage::TYPE_ALL);
		EXPECT_FALSE(vData.empty()) << aPath;
		return vData;
	}

	// An official map of Teeworlds 0.7.5
	std::vector<uint8_t> Teeworlds07Map(const char *pName)
	{
		char aPath[IO_MAX_PATH_LENGTH];
		str_format(aPath, sizeof(aPath), "test/maps07/%s.map", pName);
		std::vector<uint8_t> vData = ReadMapFile(m_pStorage.get(), aPath, IStorage::TYPE_ALL);
		EXPECT_FALSE(vData.empty()) << aPath;
		return vData;
	}

	CMapConvertResult Convert(const std::vector<uint8_t> &vMap, EMapConvertDirection Direction, EMapConvertMode Mode)
	{
		CDataFileReader Reader;
		EXPECT_TRUE(Reader.OpenFromMemory("map", vMap, "memory"));
		CMapConvertOptions Options;
		Options.m_Direction = Direction;
		Options.m_Mode = Mode;
		CMapConvertResult Result;
		EXPECT_TRUE(ConvertMap(Reader, Options, *m_pMapres, Result)) << Result.m_Error;
		if(Result.m_Converted)
		{
			// DDNet itself has to take it
			CMap Map;
			EXPECT_TRUE(Map.LoadFromMemory("converted", Result.m_vData, "memory"));
		}
		return Result;
	}
};

TEST(MapConvertFlags, ComposeLikeTheRenderer)
{
	// Which corner of the texture a corner of the tile shows, as render_layer.cpp computes the texture coordinates
	const auto &&TextureCorner = [](int Flags, int Corner) {
		std::array<int, 4> aTexX = {0, 1, 1, 0};
		std::array<int, 4> aTexY = {0, 0, 1, 1};
		if(Flags & TILEFLAG_XFLIP)
			std::rotate(aTexX.begin(), aTexX.begin() + 2, aTexX.end());
		if(Flags & TILEFLAG_YFLIP)
			std::rotate(aTexY.begin(), aTexY.begin() + 2, aTexY.end());
		if(Flags & TILEFLAG_ROTATE)
		{
			std::rotate(aTexX.begin(), aTexX.begin() + 3, aTexX.end());
			std::rotate(aTexY.begin(), aTexY.begin() + 3, aTexY.end());
		}
		// Corners top left, top right, bottom right, bottom left
		static constexpr int CORNER_OF[2][2] = {{0, 3}, {1, 2}};
		return CORNER_OF[aTexX[Corner]][aTexY[Corner]];
	};
	const int aOrientations[] = {0, 1, 2, 3, 8, 9, 10, 11};
	for(int Flags : aOrientations)
	{
		for(int Fix : aOrientations)
		{
			const int Composed = ComposeTileFlags(Flags, Fix);
			// The tile shows the texture that shows the other version's tile with the fix applied
			for(int Corner = 0; Corner < 4; Corner++)
			{
				EXPECT_EQ(TextureCorner(Composed, Corner), TextureCorner(Fix, TextureCorner(Flags, Corner))) << Flags << " " << Fix;
			}
		}
		EXPECT_EQ(ComposeTileFlags(Flags, 0), Flags);
		EXPECT_EQ(ComposeTileFlags(0, Flags), Flags);
		EXPECT_EQ(ComposeTileFlags(Flags | TILEFLAG_OPAQUE, 0), Flags);
		// Every orientation can be undone
		EXPECT_EQ(std::count_if(std::begin(aOrientations), std::end(aOrientations), [Flags](int Other) { return ComposeTileFlags(Flags, Other) == 0; }), 1);
	}
	EXPECT_EQ(ComposeTileFlags(TILEFLAG_XFLIP, TILEFLAG_XFLIP), 0);
	EXPECT_EQ(ComposeTileFlags(TILEFLAG_ROTATE, TILEFLAG_ROTATE), TILEFLAG_XFLIP | TILEFLAG_YFLIP);
	for(int a : aOrientations)
		for(int b : aOrientations)
			for(int c : aOrientations)
				EXPECT_EQ(ComposeTileFlags(ComposeTileFlags(a, b), c), ComposeTileFlags(a, ComposeTileFlags(b, c)));
}

TEST(MapConvertTables, Samples)
{
	using namespace Map07Tables;
	const CTileTable *pDoodads = To07(SET_GRASS_DOODADS);
	ASSERT_NE(pDoodads, nullptr);
	// The corrections of the owner's mapping
	EXPECT_EQ(pDoodads->m_aKind[39], KIND_EXACT);
	EXPECT_EQ(pDoodads->m_aTarget[39], 40);
	EXPECT_EQ(pDoodads->m_aTarget[41], 27);
	EXPECT_EQ(pDoodads->m_aTarget[42], 28);
	EXPECT_EQ(pDoodads->m_aTarget[43], 31);
	EXPECT_EQ(pDoodads->m_aTarget[76], 49);
	for(int i = 0; i < 4; i++)
		EXPECT_EQ(pDoodads->m_aTarget[204 + i], 220 + i);
	EXPECT_EQ(pDoodads->m_aKind[38], KIND_FALLBACK);
	EXPECT_EQ(pDoodads->m_aKind[254], KIND_DROP);
	EXPECT_EQ(pDoodads->m_aTarget[1], 217);

	const CTileTable *pWinter = To07(SET_WINTER_MAIN);
	ASSERT_NE(pWinter, nullptr);
	EXPECT_EQ(pWinter->m_aKind[182], KIND_DROP);
	EXPECT_EQ(pWinter->m_aTarget[168], 220);

	// Shadows move to generic_shadows
	const CTileTable *pGrass = To07(SET_GRASS_MAIN);
	ASSERT_NE(pGrass, nullptr);
	EXPECT_EQ(pGrass->m_aKind[76], KIND_EXACT);
	EXPECT_EQ(pGrass->m_aTargetSet[76], SET_GENERIC_SHADOWS);
	EXPECT_EQ(pGrass->m_aTarget[76], 17);
	EXPECT_EQ(pGrass->m_aTargetSet[1], SET_GRASS_MAIN);
	EXPECT_EQ(pGrass->m_aKind[69], KIND_DROP);
	const CTileTable *pDesert = To07(SET_DESERT_MAIN);
	ASSERT_NE(pDesert, nullptr);
	EXPECT_EQ(pDesert->m_aTarget[92], 17);
	EXPECT_TRUE(To07(SET_JUNGLE_MAIN)->m_SameGraphics);
	EXPECT_EQ(To07(SET_EASTER), nullptr);

	// And back, onto the tileset the map has
	EXPECT_EQ(To06(SET_GENERIC_SHADOWS, SET_GRASS_MAIN)->m_aTarget[17], 76);
	EXPECT_EQ(To06(SET_GENERIC_SHADOWS, SET_DESERT_MAIN)->m_aTarget[17], 92);
	EXPECT_EQ(To06(SET_GENERIC_SHADOWS, SET_JUNGLE_MAIN)->m_aTarget[17], 76);
	EXPECT_EQ(To06(SET_GENERIC_SHADOWS, SET_GRASS_MAIN)->m_aKind[20], KIND_FALLBACK);
	// A tile 0.7 has twice, flipped
	const CTileTable *pDesert06 = To06(SET_DESERT_MAIN, SET_DESERT_MAIN);
	EXPECT_EQ(pDesert06->m_aKind[8], KIND_EXACT);
	EXPECT_EQ(pDesert06->m_aTarget[8], 17);
	EXPECT_EQ(pDesert06->m_aFlagFix[8], TILEFLAG_XFLIP);
	// The old shadows of 0.7's grass_main are empty and must not come back
	EXPECT_EQ(To06(SET_GRASS_MAIN, SET_GRASS_MAIN)->m_aKind[76], KIND_DROP);
	// New in 0.7: bones and berries
	EXPECT_EQ(To06(SET_GRASS_MAIN, SET_GRASS_MAIN)->m_aKind[14], KIND_FALLBACK);
	EXPECT_EQ(To06(SET_GRASS_DOODADS, SET_GRASS_DOODADS)->m_aKind[94], KIND_FALLBACK);
	for(int i = 1; i < 256; i++)
		EXPECT_NE(To06(SET_EASTER, SET_EASTER)->m_aKind[i], KIND_EXACT);
}

TEST(MapConvertTables, BothDirectionsAgree)
{
	using namespace Map07Tables;
	for(const CTileTable &Forward : TO07)
	{
		for(int i = 1; i < 256; i++)
		{
			if(Forward.m_aKind[i] != KIND_EXACT)
				continue;
			const ESet Set = static_cast<ESet>(Forward.m_aTargetSet[i]);
			const int Target = Forward.m_aTarget[i];
			if(Set == SET_JUNGLE_MAIN)
			{
				continue; // DDNet's jungle_main is 0.7's with the old shadows, nothing to map back
			}
			// Every tile that has a place in 0.7 has one back, in the same tileset
			const CTileTable *pBack = To06(Set, Forward.m_Source);
			ASSERT_NE(pBack, nullptr) << SET_NAMES[Set];
			EXPECT_EQ(pBack->m_aKind[Target], KIND_EXACT) << SET_NAMES[Forward.m_Source] << " " << i;
			EXPECT_EQ(pBack->m_aTargetSet[Target], Forward.m_Source) << SET_NAMES[Forward.m_Source] << " " << i;
			if(pBack->m_aTarget[Target] == i)
			{
				EXPECT_EQ(ComposeTileFlags(pBack->m_aFlagFix[Target], Forward.m_aFlagFix[i]), 0);
			}
			else
			{
				// Several 0.6 tiles went to the same 0.7 one: the way back takes one of them
				EXPECT_EQ(Forward.m_aTarget[pBack->m_aTarget[Target]], Target) << SET_NAMES[Forward.m_Source] << " " << i;
			}
		}
	}
	for(const CTileTable &Back : TO06)
	{
		for(int j = 1; j < 256; j++)
		{
			if(Back.m_aKind[j] == KIND_EXACT)
			{
				EXPECT_EQ(Back.m_aTargetSet[j], Back.m_Home);
				EXPECT_NE(Back.m_aTarget[j], 0);
			}
		}
	}
}

TEST_F(MapConvert, MapresMatchTheTables)
{
	using namespace Map07Tables;
	for(int Set = 0; Set < NUM_SETS; Set++)
	{
		const std::shared_ptr<const CMapresImage> pImage07 = m_pMapres->Find(SET_NAMES[Set], true);
		ASSERT_NE(pImage07, nullptr) << SET_NAMES[Set];
		EXPECT_EQ(pImage07->m_Width, SIZE07[Set][0]);
		EXPECT_EQ(pImage07->m_Height, SIZE07[Set][1]);
		for(int i = 0; i < 8; i++)
			EXPECT_EQ(pImage07->m_aOpaque[i], OPAQUE07[Set][i]) << SET_NAMES[Set];
		const std::shared_ptr<const CMapresImage> pImage06 = m_pMapres->Find(SET_NAMES[Set], false);
		EXPECT_EQ(pImage06 != nullptr, SIZE06[Set][0] != 0) << SET_NAMES[Set];
		if(pImage06 != nullptr)
		{
			for(int i = 0; i < 8; i++)
				EXPECT_EQ(pImage06->m_aOpaque[i], OPAQUE06[Set][i]) << SET_NAMES[Set];
		}
	}
	// The diff tilesets
	for(int Set = 0; Set < NUM_SETS; Set++)
	{
		for(const char *pName : {DIFFS07[Set], DIFFS06[Set]})
		{
			if(pName == nullptr)
				continue;
			const std::shared_ptr<const CMapresImage> pImage = m_pMapres->FindDiff(pName);
			ASSERT_NE(pImage, nullptr) << pName;
			EXPECT_EQ(pImage->m_Width, SIZE07[Set][0]) << pName;
			EXPECT_EQ(pImage->m_Height, SIZE07[Set][1]) << pName;
		}
	}
	EXPECT_EQ(DIFFS06[SET_EASTER], nullptr); // Embedded whole
	EXPECT_NE(DIFFS07[SET_GRASS_DOODADS], nullptr);
	// Read once, then kept
	EXPECT_EQ(m_pMapres->Find("grass_main", true), m_pMapres->Find("grass_main", true));
	EXPECT_EQ(m_pMapres->Find("does_not_exist", false), nullptr);
}

TEST_F(MapConvert, Detection)
{
	for(const char *pName : {"dm1", "dm2", "dm7", "ctf2", "ctf5", "lms1"})
	{
		EXPECT_TRUE(Is07(Teeworlds07Map(pName))) << pName;
	}
	// The official 0.7 maps in the old format, and DDNet's
	for(const char *pName : {"dm6", "ctf3", "ctf4", "ctf6", "dm1", "dm2", "Tutorial"})
	{
		EXPECT_FALSE(Is07(DDNetMap(pName))) << pName;
	}
}

TEST_F(MapConvert, NothingToConvert)
{
	// The official 0.7 maps in the old format and a DDNet map using no picture that changed
	for(const char *pName : {"dm6", "ctf3", "Sunny Side Up"})
	{
		const std::vector<uint8_t> vMap = DDNetMap(pName);
		EXPECT_FALSE(NeedsConversion(vMap, EMapConvertDirection::TO07)) << pName;
		EXPECT_FALSE(NeedsConversion(vMap, EMapConvertDirection::TO06)) << pName;
		for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID, EMapConvertMode::EMBED})
		{
			// The same file then serves both
			const CMapConvertResult Result = Convert(vMap, EMapConvertDirection::TO07, Mode);
			EXPECT_FALSE(Result.m_Converted) << pName;
			EXPECT_TRUE(Result.m_vData.empty());
			EXPECT_TRUE(Result.m_Error.empty());
		}
	}
	// 0.7 maps are 0.7 maps already, DDNet maps DDNet maps
	EXPECT_FALSE(NeedsConversion(Teeworlds07Map("dm1"), EMapConvertDirection::TO07));
	EXPECT_FALSE(Convert(Teeworlds07Map("dm1"), EMapConvertDirection::TO07, EMapConvertMode::REMAP).m_Converted);
	EXPECT_FALSE(NeedsConversion(DDNetMap("dm2"), EMapConvertDirection::TO06));

	// Tiles where 0.7 has them, but of a picture 0.7 clients do not have
	CTestMap Map;
	const int Image = Map.AddImage("ddnet_tiles", true, 1, 1, 1024, 1024, {});
	Map.AddTileLayer(-1, 2, 2, Tiles({{1, 0, 0, 0}}, 4), TILESLAYERFLAG_GAME);
	Map.AddTileLayer(Image, 2, 2, Tiles({{1, 0, 0, 0}}, 4));
	Map.EndGroup();
	const std::vector<uint8_t> vForeign = Map.Finish();
	EXPECT_TRUE(NeedsConversion(vForeign, EMapConvertDirection::TO07));
	const CMapConvertResult Result = Convert(vForeign, EMapConvertDirection::TO07, EMapConvertMode::REMAP);
	ASSERT_TRUE(Result.m_Converted);
	EXPECT_EQ(Result.m_Stats.m_EmbeddedImages, 1);
}

TEST_F(MapConvert, To07SplitsTheShadows)
{
	const std::vector<uint8_t> vSource = DDNetMap("dm2");
	ASSERT_TRUE(NeedsConversion(vSource, EMapConvertDirection::TO07));
	const CMapConvertResult Result = Convert(vSource, EMapConvertDirection::TO07, EMapConvertMode::REMAP);
	ASSERT_TRUE(Result.m_Converted);
	EXPECT_TRUE(Is07(Result.m_vData));
	EXPECT_GT(Result.m_Stats.m_SplitLayers, 0);
	EXPECT_EQ(Result.m_Stats.m_EmbeddedImages, 0);
	EXPECT_GT(Result.m_Stats.m_LostTiles, 0); // grass_doodads' rocks

	CDataFileReader Source, Output;
	ASSERT_TRUE(Source.OpenFromMemory("source", vSource, "memory"));
	ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
	int SourceLayers, OutputLayers, Start;
	Source.GetType(MAPITEMTYPE_LAYER, &Start, &SourceLayers);
	Output.GetType(MAPITEMTYPE_LAYER, &Start, &OutputLayers);
	// Emptied layers stay
	EXPECT_EQ(OutputLayers, SourceLayers + Result.m_Stats.m_SplitLayers);

	// Each shadow layer is drawn right below the layer it came from, in the same group
	const std::vector<CLayerInfo> vSourceLayers = TileLayers(vSource);
	const std::vector<CLayerInfo> vLayers = TileLayers(Result.m_vData);
	int Shadows = 0;
	size_t s = 0;
	for(size_t i = 0; i < vLayers.size(); i++)
	{
		if(vLayers[i].m_ImageName == "generic_shadows")
		{
			ASSERT_LT(i + 1, vLayers.size());
			EXPECT_TRUE(vLayers[i].m_External);
			EXPECT_EQ(vLayers[i + 1].m_ImageName, "grass_main");
			EXPECT_EQ(vLayers[i].m_Group, vLayers[i + 1].m_Group);
			EXPECT_EQ(vLayers[i].m_Width, vLayers[i + 1].m_Width);
			// The version of the layer it came from
			EXPECT_EQ(vLayers[i].m_Version, vLayers[i + 1].m_Version);
			EXPECT_EQ(vLayers[i].m_Version, vSourceLayers[s].m_Version);
			Shadows++;
			continue;
		}
		ASSERT_LT(s, vSourceLayers.size());
		EXPECT_EQ(vLayers[i].m_ImageName, vSourceLayers[s].m_ImageName);
		EXPECT_EQ(vLayers[i].m_Group, vSourceLayers[s].m_Group);
		s++;
	}
	EXPECT_EQ(Shadows, Result.m_Stats.m_SplitLayers);

	// Image items of version 2 with a format, envelopes of version 3
	int ImageStart, ImageNum;
	Output.GetType(MAPITEMTYPE_IMAGE, &ImageStart, &ImageNum);
	for(int i = 0; i < ImageNum; i++)
	{
		ASSERT_EQ(Output.GetItemSize(ImageStart + i), (int)sizeof(CMapItemImage_v2));
		const CMapItemImage_v2 *pImage = static_cast<const CMapItemImage_v2 *>(Output.GetItem(ImageStart + i));
		EXPECT_EQ(pImage->m_Version, 2);
		EXPECT_EQ(pImage->m_MustBe1, 1);
	}
	int EnvStart, EnvNum;
	Output.GetType(MAPITEMTYPE_ENVELOPE, &EnvStart, &EnvNum);
	for(int i = 0; i < EnvNum; i++)
	{
		EXPECT_EQ(static_cast<const CMapItemEnvelope *>(Output.GetItem(EnvStart + i))->m_Version, 3);
	}

	CMapConvertProvenance Provenance;
	ASSERT_TRUE(ReadMapConvertProvenance(Output, Provenance));
	EXPECT_EQ(Provenance.m_Direction, EMapConvertDirection::TO07);
	EXPECT_EQ(Provenance.m_Mode, EMapConvertMode::REMAP);
	EXPECT_EQ(Provenance.m_SourceSha256, Source.Sha256());
	EXPECT_EQ(Provenance.m_SourceCrc, Source.Crc());
	EXPECT_EQ(Provenance.m_SourceSize, Source.Size());
	EXPECT_EQ(Provenance.m_TableVersion, Map07Tables::VERSION);
	EXPECT_EQ(Provenance.m_TableChecksum, Map07Tables::CHECKSUM);
	EXPECT_FALSE(ReadMapConvertProvenance(Source, Provenance));
	EXPECT_EQ(Result.m_Sha256, Output.Sha256());
	EXPECT_EQ(Result.m_Crc, Output.Crc());
}

// The layers of a map drawn from a diff tileset, each with the one right below it
static std::vector<std::pair<CLayerInfo, CLayerInfo>> DiffLayers(const std::vector<uint8_t> &vMap, const char *pDiff)
{
	std::vector<std::pair<CLayerInfo, CLayerInfo>> vDiffLayers;
	const std::vector<CLayerInfo> vLayers = TileLayers(vMap);
	for(size_t i = 1; i < vLayers.size(); i++)
	{
		if(vLayers[i].m_ImageName == pDiff)
		{
			vDiffLayers.emplace_back(vLayers[i], vLayers[i - 1]);
		}
	}
	return vDiffLayers;
}

static std::vector<std::string> ImageNames(const std::vector<uint8_t> &vMap)
{
	CDataFileReader Reader;
	EXPECT_TRUE(Reader.OpenFromMemory("map", vMap, "memory"));
	int Start, Num;
	Reader.GetType(MAPITEMTYPE_IMAGE, &Start, &Num);
	std::vector<std::string> vNames;
	vNames.reserve(Num);
	for(int i = 0; i < Num; i++)
	{
		vNames.emplace_back(Reader.GetDataString(static_cast<const CMapItemImage *>(Reader.GetItem(Start + i))->m_ImageName));
	}
	return vNames;
}

TEST_F(MapConvert, HybridDrawsWhatHasNoCounterpart)
{
	using namespace Map07Tables;
	const char *pDiff = DIFFS07[SET_GRASS_DOODADS];
	ASSERT_NE(pDiff, nullptr);
	// dm2 uses rocks of grass_doodads that 0.7 dropped
	const std::vector<uint8_t> vSource = DDNetMap("dm2");
	const CMapConvertResult Result = Convert(vSource, EMapConvertDirection::TO07, EMapConvertMode::HYBRID);
	ASSERT_TRUE(Result.m_Converted);
	EXPECT_EQ(Result.m_Mode, EMapConvertMode::HYBRID);
	EXPECT_EQ(Result.m_Stats.m_LostTiles, 0);
	EXPECT_EQ(Result.m_Stats.m_EmbeddedImages, 0);
	EXPECT_EQ(Result.m_Stats.m_DiffImages, 1);
	EXPECT_GT(Result.m_Stats.m_DiffLayers, 0);

	// The rocks keep their index and flags in a layer right above the one they came from, which stays external and keeps the rest
	std::vector<std::tuple<int, int, int>> vDrawn;
	for(const auto &[Diff, Below] : DiffLayers(Result.m_vData, pDiff))
	{
		EXPECT_FALSE(Diff.m_External);
		EXPECT_EQ(Below.m_ImageName, "grass_doodads");
		EXPECT_TRUE(Below.m_External);
		EXPECT_EQ(Diff.m_Group, Below.m_Group);
		EXPECT_EQ(Diff.m_Width, Below.m_Width);
		EXPECT_EQ(Diff.m_aColor, Below.m_aColor);
		EXPECT_EQ(Diff.m_ColorEnv, Below.m_ColorEnv);
		EXPECT_EQ(Diff.m_ColorEnvOffset, Below.m_ColorEnvOffset);
		EXPECT_EQ(Diff.m_LayerFlags, Below.m_LayerFlags);
		for(size_t t = 0; t < Diff.m_vTiles.size(); t++)
		{
			if(Diff.m_vTiles[t].m_Index != 0)
			{
				EXPECT_EQ(Below.m_vTiles[t].m_Index, 0); // Never both
				vDrawn.emplace_back(t, Diff.m_vTiles[t].m_Index, Diff.m_vTiles[t].m_Flags);
			}
		}
	}
	EXPECT_EQ((int)vDrawn.size(), Result.m_Stats.m_DiffTiles);
	std::vector<std::tuple<int, int, int>> vRocks;
	for(const CLayerInfo &Layer : TileLayers(vSource))
	{
		for(size_t t = 0; t < Layer.m_vTiles.size() && Layer.m_ImageName == "grass_doodads"; t++)
		{
			const CTile &Tile = Layer.m_vTiles[t];
			if(Tile.m_Index != 0 && To07(SET_GRASS_DOODADS)->m_aKind[Tile.m_Index] == KIND_FALLBACK)
			{
				vRocks.emplace_back(t, Tile.m_Index, Tile.m_Flags);
			}
		}
	}
	std::sort(vRocks.begin(), vRocks.end());
	std::sort(vDrawn.begin(), vDrawn.end());
	EXPECT_FALSE(vRocks.empty());
	EXPECT_EQ(vDrawn, vRocks);

	// Shadows below, rocks above, the rest as it was
	std::vector<std::pair<int, std::string>> vOrder, vSourceOrder;
	const std::vector<CLayerInfo> vLayers = TileLayers(Result.m_vData);
	for(size_t i = 0; i < vLayers.size(); i++)
	{
		if(vLayers[i].m_ImageName == "generic_shadows")
		{
			ASSERT_LT(i + 1, vLayers.size());
			EXPECT_EQ(vLayers[i + 1].m_ImageName, "grass_main");
		}
		else if(vLayers[i].m_ImageName != pDiff)
		{
			vOrder.emplace_back(vLayers[i].m_Group, vLayers[i].m_ImageName);
		}
	}
	for(const CLayerInfo &Layer : TileLayers(vSource))
	{
		vSourceOrder.emplace_back(Layer.m_Group, Layer.m_ImageName);
	}
	EXPECT_EQ(vOrder, vSourceOrder);

	// Only where it is needed: remap leaves the rocks out, a map without them gets no diff tileset
	const CMapConvertResult Remap = Convert(vSource, EMapConvertDirection::TO07, EMapConvertMode::REMAP);
	ASSERT_TRUE(Remap.m_Converted);
	EXPECT_GT(Remap.m_Stats.m_LostTiles, 0);
	EXPECT_EQ(Remap.m_Stats.m_DiffImages, 0);
	const CMapConvertResult WithoutRocks = Convert(DDNetMap("ctf2"), EMapConvertDirection::TO07, EMapConvertMode::HYBRID);
	ASSERT_TRUE(WithoutRocks.m_Converted);
	EXPECT_EQ(WithoutRocks.m_Stats.m_DiffImages, 0);
	EXPECT_EQ(WithoutRocks.m_Stats.m_DiffLayers, 0);
	for(const std::string &Name : ImageNames(WithoutRocks.m_vData))
	{
		EXPECT_NE(Name, pDiff);
	}

	const CMapConvertResult Embed = Convert(vSource, EMapConvertDirection::TO07, EMapConvertMode::EMBED);
	ASSERT_TRUE(Embed.m_Converted);
	EXPECT_EQ(Embed.m_Stats.m_RemappedImages, 0);
	EXPECT_EQ(Embed.m_Stats.m_SplitLayers, 0);
	EXPECT_EQ(Embed.m_Stats.m_DiffImages, 0);
	for(const CLayerInfo &Layer : TileLayers(Embed.m_vData))
	{
		if(Layer.m_ImageName == "grass_doodads" || Layer.m_ImageName == "grass_main")
		{
			EXPECT_FALSE(Layer.m_External);
		}
	}
}

TEST_F(MapConvert, HybridEmbedsOnlyTheDiffsItUses)
{
	using namespace Map07Tables;
	// The official 0.7 maps use tiles that 0.7 added
	for(const char *pName : {"ctf5", "dm1", "lms1"})
	{
		const std::vector<uint8_t> vSource = Teeworlds07Map(pName);
		const CMapConvertResult Result = Convert(vSource, EMapConvertDirection::TO06, EMapConvertMode::HYBRID);
		ASSERT_TRUE(Result.m_Converted) << pName;
		EXPECT_EQ(Result.m_Stats.m_LostTiles, 0) << pName;
		EXPECT_EQ(Result.m_Stats.m_EmbeddedImages, 0) << pName;
		// The sets whose new tiles the map uses
		std::set<std::string> Used;
		for(const CLayerInfo &Layer : TileLayers(vSource))
		{
			for(const Map07Tables::CTileTable &Table : TO06)
			{
				if(Layer.m_External && Layer.m_ImageName == SET_NAMES[Table.m_Source] && DIFFS06[Table.m_Source] != nullptr &&
					std::any_of(Layer.m_vTiles.begin(), Layer.m_vTiles.end(), [&Table](const CTile &Tile) { return Tile.m_Index != 0 && Table.m_aKind[Tile.m_Index] == KIND_FALLBACK; }))
				{
					Used.insert(DIFFS06[Table.m_Source]);
				}
			}
		}
		EXPECT_FALSE(Used.empty()) << pName;
		EXPECT_EQ(Result.m_Stats.m_DiffImages, (int)Used.size()) << pName;
		int Layers = 0;
		const std::vector<std::string> vNames = ImageNames(Result.m_vData);
		for(const char *pDiff : DIFFS06)
		{
			if(pDiff == nullptr)
				continue;
			const bool Embedded = std::find(vNames.begin(), vNames.end(), pDiff) != vNames.end();
			EXPECT_EQ(Embedded, Used.contains(pDiff)) << pName << " " << pDiff;
			for(const auto &[Diff, Below] : DiffLayers(Result.m_vData, pDiff))
			{
				EXPECT_TRUE(Below.m_External);
				EXPECT_EQ(Diff.m_Group, Below.m_Group);
				Layers++;
			}
		}
		EXPECT_EQ(Layers, Result.m_Stats.m_DiffLayers) << pName;
	}
}

// The mapres without the diff tilesets
class CMapresWithoutDiffs : public IMapres
{
	const IMapres &m_Mapres;

public:
	explicit CMapresWithoutDiffs(const IMapres &Mapres) :
		m_Mapres(Mapres) {}
	std::shared_ptr<const CMapresImage> Find(const char *pName, bool Teeworlds07) const override { return m_Mapres.Find(pName, Teeworlds07); }
	std::shared_ptr<const CMapresImage> FindDiff(const char *pName) const override { return nullptr; }
};

TEST_F(MapConvert, HybridWithoutDiffsIsRemap)
{
	const CMapresWithoutDiffs Mapres(*m_pMapres);
	for(const auto &[vSource, Direction] : {std::pair{DDNetMap("dm2"), EMapConvertDirection::TO07}, std::pair{Teeworlds07Map("ctf5"), EMapConvertDirection::TO06}})
	{
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.OpenFromMemory("map", vSource, "memory"));
		CMapConvertOptions Options;
		Options.m_Direction = Direction;
		CMapConvertResult Result;
		ASSERT_TRUE(ConvertMap(Reader, Options, Mapres, Result));
		ASSERT_TRUE(Result.m_Converted);
		EXPECT_EQ(Result.m_Mode, EMapConvertMode::REMAP);
		EXPECT_EQ(Result.m_Stats.m_DiffImages, 0);
		EXPECT_GT(Result.m_Stats.m_LostTiles, 0);
		EXPECT_TRUE(std::any_of(Result.m_vWarnings.begin(), Result.m_vWarnings.end(), [](const std::string &Warning) { return Warning.find("missing, using remap") != std::string::npos; }));
		// The same map as remap gives
		EXPECT_EQ(Result.m_vData, Convert(vSource, Direction, EMapConvertMode::REMAP).m_vData);
	}
}

TEST_F(MapConvert, EmbedChangesNoTile)
{
	for(const auto &[vSource, Direction] : {std::pair{DDNetMap("dm2"), EMapConvertDirection::TO07}, std::pair{Teeworlds07Map("ctf5"), EMapConvertDirection::TO06}, std::pair{Teeworlds07Map("dm1"), EMapConvertDirection::TO06}})
	{
		const CMapConvertResult Result = Convert(vSource, Direction, EMapConvertMode::EMBED);
		ASSERT_TRUE(Result.m_Converted);
		EXPECT_GT(Result.m_Stats.m_EmbeddedImages, 0);
		EXPECT_EQ(Result.m_Stats.m_RemappedImages, 0);
		EXPECT_EQ(Result.m_Stats.m_SplitLayers + Result.m_Stats.m_DiffLayers + Result.m_Stats.m_RewrittenLayers, 0);
		// Every tile layer has the same data, and every image the same name
		CDataFileReader Source, Output;
		ASSERT_TRUE(Source.OpenFromMemory("source", vSource, "memory"));
		ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
		int SourceStart, SourceNum, OutputStart, OutputNum;
		Source.GetType(MAPITEMTYPE_LAYER, &SourceStart, &SourceNum);
		Output.GetType(MAPITEMTYPE_LAYER, &OutputStart, &OutputNum);
		ASSERT_EQ(OutputNum, SourceNum);
		for(int i = 0; i < SourceNum; i++)
		{
			const CMapItemLayer *pSource = static_cast<const CMapItemLayer *>(Source.GetItem(SourceStart + i));
			if(pSource->m_Type != LAYERTYPE_TILES)
				continue;
			const int SourceData = static_cast<const CMapItemLayerTilemap *>(Source.GetItem(SourceStart + i))->m_Data;
			const int OutputData = static_cast<const CMapItemLayerTilemap *>(Output.GetItem(OutputStart + i))->m_Data;
			ASSERT_EQ(Source.GetDataSize(SourceData), Output.GetDataSize(OutputData));
			EXPECT_EQ(mem_comp(Source.GetData(SourceData), Output.GetData(OutputData), Source.GetDataSize(SourceData)), 0) << i;
			EXPECT_EQ(Source.GetItemSize(SourceStart + i), Output.GetItemSize(OutputStart + i));
			EXPECT_EQ(mem_comp(pSource, Output.GetItem(OutputStart + i), Source.GetItemSize(SourceStart + i)), 0) << i;
		}
		Source.GetType(MAPITEMTYPE_IMAGE, &SourceStart, &SourceNum);
		Output.GetType(MAPITEMTYPE_IMAGE, &OutputStart, &OutputNum);
		ASSERT_EQ(OutputNum, SourceNum);
		for(int i = 0; i < SourceNum; i++)
		{
			EXPECT_STREQ(Output.GetDataString(static_cast<const CMapItemImage *>(Output.GetItem(OutputStart + i))->m_ImageName),
				Source.GetDataString(static_cast<const CMapItemImage *>(Source.GetItem(SourceStart + i))->m_ImageName));
		}
	}
}

TEST_F(MapConvert, To06)
{
	const std::vector<uint8_t> vSource = Teeworlds07Map("dm2");
	ASSERT_TRUE(NeedsConversion(vSource, EMapConvertDirection::TO06));
	for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID, EMapConvertMode::EMBED})
	{
		const CMapConvertResult Result = Convert(vSource, EMapConvertDirection::TO06, Mode);
		ASSERT_TRUE(Result.m_Converted);
		EXPECT_FALSE(Is07(Result.m_vData));
		CDataFileReader Output;
		ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
		int ImageStart, ImageNum;
		Output.GetType(MAPITEMTYPE_IMAGE, &ImageStart, &ImageNum);
		for(int i = 0; i < ImageNum; i++)
		{
			EXPECT_EQ(Output.GetItemSize(ImageStart + i), (int)sizeof(CMapItemImage_v1));
			EXPECT_EQ(static_cast<const CMapItemImage *>(Output.GetItem(ImageStart + i))->m_Version, 1);
		}
		// No tile of a picture 0.7 drew again is left on an external image of that name
		for(const CLayerInfo &Layer : TileLayers(Result.m_vData))
		{
			if(Layer.m_External)
			{
				EXPECT_NE(Layer.m_ImageName, "generic_shadows");
				EXPECT_NE(Layer.m_ImageName, "easter");
			}
		}
		CMapConvertProvenance Provenance;
		ASSERT_TRUE(ReadMapConvertProvenance(Output, Provenance));
		EXPECT_EQ(Provenance.m_Direction, EMapConvertDirection::TO06);
		EXPECT_EQ(Provenance.m_Mode, Mode);
	}
}

// The versions of the tile layer items, all of them
static std::set<int> TileLayerVersions(const std::vector<uint8_t> &vMap)
{
	CDataFileReader Reader;
	EXPECT_TRUE(Reader.OpenFromMemory("map", vMap, "memory"));
	int LayerStart, LayerNum;
	Reader.GetType(MAPITEMTYPE_LAYER, &LayerStart, &LayerNum);
	std::set<int> Versions;
	for(int i = 0; i < LayerNum; i++)
	{
		const CMapItemLayerTilemap_v2 *pTilemap = static_cast<const CMapItemLayerTilemap_v2 *>(Reader.GetItem(LayerStart + i));
		if(pTilemap->m_Layer.m_Type == LAYERTYPE_TILES)
		{
			Versions.insert(pTilemap->m_Version);
		}
	}
	return Versions;
}

// All tile layers of a map have one version, the rewritten ones as well as
// those left as they are, or twmap refuses the map
TEST_F(MapConvert, TileLayersShareOneVersion)
{
	for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID, EMapConvertMode::EMBED})
	{
		for(const char *pName : {"dm1", "dm7", "ctf5"})
		{
			const std::vector<uint8_t> vDDNet = DDNetMap(pName);
			const CMapConvertResult To07 = Convert(vDDNet, EMapConvertDirection::TO07, Mode);
			ASSERT_TRUE(To07.m_Converted) << pName;
			EXPECT_EQ(TileLayerVersions(To07.m_vData), TileLayerVersions(vDDNet)) << pName << " " << MapConvertModeName(Mode);
			EXPECT_EQ(TileLayerVersions(To07.m_vData).size(), 1u) << pName;

			const std::vector<uint8_t> vTeeworlds07 = Teeworlds07Map(pName);
			const CMapConvertResult To06 = Convert(vTeeworlds07, EMapConvertDirection::TO06, Mode);
			ASSERT_TRUE(To06.m_Converted) << pName;
			EXPECT_EQ(TileLayerVersions(To06.m_vData), TileLayerVersions(vTeeworlds07)) << pName << " " << MapConvertModeName(Mode);
			EXPECT_EQ(TileLayerVersions(To06.m_vData).size(), 1u) << pName;
		}
	}
}

TEST_F(MapConvert, SameOutputEveryTime)
{
	const std::vector<uint8_t> vDDNet = DDNetMap("dm7");
	const std::vector<uint8_t> vTeeworlds07 = Teeworlds07Map("ctf5");
	for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID, EMapConvertMode::EMBED})
	{
		const CMapConvertResult First = Convert(vDDNet, EMapConvertDirection::TO07, Mode);
		const CMapConvertResult Second = Convert(vDDNet, EMapConvertDirection::TO07, Mode);
		ASSERT_TRUE(First.m_Converted);
		EXPECT_EQ(First.m_vData, Second.m_vData);
		EXPECT_EQ(First.m_Sha256, Second.m_Sha256);
		// And with pictures read anew
		CMapresFromStorage OtherMapres(m_pStorage.get());
		CDataFileReader Reader;
		ASSERT_TRUE(Reader.OpenFromMemory("map", vDDNet, "memory"));
		CMapConvertOptions Options;
		Options.m_Mode = Mode;
		CMapConvertResult Third;
		ASSERT_TRUE(ConvertMap(Reader, Options, OtherMapres, Third));
		EXPECT_EQ(First.m_vData, Third.m_vData);

		const CMapConvertResult Back = Convert(vTeeworlds07, EMapConvertDirection::TO06, Mode);
		ASSERT_TRUE(Back.m_Converted);
		EXPECT_EQ(Back.m_vData, Convert(vTeeworlds07, EMapConvertDirection::TO06, Mode).m_vData);
	}
}

TEST_F(MapConvert, PicturesCanBeFetchedAhead)
{
	const std::vector<std::pair<std::vector<uint8_t>, EMapConvertDirection>> vMaps = {
		{DDNetMap("dm2"), EMapConvertDirection::TO07},
		{DDNetMap("ctf2"), EMapConvertDirection::TO07},
		{Teeworlds07Map("ctf5"), EMapConvertDirection::TO06},
		{Teeworlds07Map("dm2"), EMapConvertDirection::TO06},
		{Teeworlds07Map("dm1"), EMapConvertDirection::TO06},
	};
	size_t NumListed = 0;
	for(const auto &[vSource, Direction] : vMaps)
	{
		for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID, EMapConvertMode::EMBED})
		{
			CMapConvertOptions Options;
			Options.m_Direction = Direction;
			Options.m_Mode = Mode;
			CDataFileReader Reader;
			ASSERT_TRUE(Reader.OpenFromMemory("map", vSource, "memory"));
			const std::vector<std::string> vListed = CMapresFromFiles::Paths(MapConvertPictures(Reader, Options));
			Reader.Close();

			// Only what was listed is there, as if only that was fetched
			std::set<std::string> Asked;
			const CMapresFromFiles Fetched([&](const char *pPath, std::vector<uint8_t> &vData) {
				Asked.emplace(pPath);
				if(std::find(vListed.begin(), vListed.end(), pPath) == vListed.end())
					return false;
				vData = ReadMapFile(m_pStorage.get(), pPath, IStorage::TYPE_ALL);
				return !vData.empty();
			});
			ASSERT_TRUE(Reader.OpenFromMemory("map", vSource, "memory"));
			CMapConvertResult Result;
			ASSERT_TRUE(ConvertMap(Reader, Options, Fetched, Result)) << Result.m_Error;
			const CMapConvertResult Expected = Convert(vSource, Direction, Mode);
			EXPECT_EQ(Result.m_vData, Expected.m_vData) << MapConvertModeName(Mode);
			EXPECT_EQ(Result.m_Mode, Expected.m_Mode) << MapConvertModeName(Mode);
			// And nothing was listed that is not asked for
			EXPECT_EQ(Asked, std::set<std::string>(vListed.begin(), vListed.end())) << MapConvertModeName(Mode);
			NumListed += vListed.size();
		}
	}
	EXPECT_GT(NumListed, 0u);
}

TEST_F(MapConvert, NeverTwiceTheSameWay)
{
	for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID, EMapConvertMode::EMBED})
	{
		const CMapConvertResult To07 = Convert(DDNetMap("dm7"), EMapConvertDirection::TO07, Mode);
		ASSERT_TRUE(To07.m_Converted);
		EXPECT_FALSE(Convert(To07.m_vData, EMapConvertDirection::TO07, Mode).m_Converted);
		const CMapConvertResult To06 = Convert(Teeworlds07Map("dm7"), EMapConvertDirection::TO06, Mode);
		ASSERT_TRUE(To06.m_Converted);
		EXPECT_FALSE(Convert(To06.m_vData, EMapConvertDirection::TO06, Mode).m_Converted);
		EXPECT_FALSE(NeedsConversion(To06.m_vData, EMapConvertDirection::TO06));
	}
}

// The tiles of a map that come back, and those in their place after converting it there and back
static std::pair<std::vector<CPlacedTile>, std::vector<CPlacedTile>> ComparableTiles(std::vector<CPlacedTile> vBefore, std::vector<CPlacedTile> vAfter, const std::function<bool(const CPlacedTile &)> &ComesBack)
{
	std::set<std::tuple<int, int, int, std::string>> Other;
	for(const CPlacedTile &Tile : vBefore)
	{
		if(!ComesBack(Tile))
			Other.emplace(Tile.m_Group, Tile.m_Width, Tile.m_Cell, Tile.m_Image);
	}
	const auto &&Where = [](const CPlacedTile &Tile) { return std::tuple(Tile.m_Group, Tile.m_Width, Tile.m_Cell, Tile.m_Image); };
	vBefore.erase(std::remove_if(vBefore.begin(), vBefore.end(), [&](const CPlacedTile &Tile) { return Other.contains(Where(Tile)); }), vBefore.end());
	vAfter.erase(std::remove_if(vAfter.begin(), vAfter.end(), [&](const CPlacedTile &Tile) { return Other.contains(Where(Tile)); }), vAfter.end());
	return {vBefore, vAfter};
}

// Whether a tile comes back to the index it had when converted there and back: its tile there is its alone
static bool ComesBack(bool To07, const std::string &Image, int Index, EMapConvertMode Mode)
{
	using namespace Map07Tables;
	const CTileTable *pThere = nullptr;
	for(const CTileTable &Table : To07 ? std::span<const CTileTable>(TO07) : std::span<const CTileTable>(TO06))
	{
		if(Image == SET_NAMES[Table.m_Source] && (pThere == nullptr || Table.m_Home == Table.m_Source))
			pThere = &Table;
	}
	if(pThere == nullptr)
		return true;
	if(pThere->m_aKind[Index] == KIND_FALLBACK)
		return Mode == EMapConvertMode::HYBRID; // Kept, in the diff tileset
	if(pThere->m_aKind[Index] != KIND_EXACT)
		return false;
	const int Set = pThere->m_aTargetSet[Index];
	const int Target = pThere->m_aTarget[Index];
	for(const CTileTable &Table : To07 ? std::span<const CTileTable>(TO06) : std::span<const CTileTable>(TO07))
	{
		if(Table.m_Source == Set && (Table.m_Home == pThere->m_Source || Table.m_Home == Table.m_Source))
			return Table.m_aKind[Target] == KIND_EXACT && Table.m_aTarget[Target] == Index && ComposeTileFlags(pThere->m_aFlagFix[Index], Table.m_aFlagFix[Target]) == 0;
	}
	return false;
}

TEST_F(MapConvert, ThereAndBack)
{
	const std::vector<std::string> vSets = {"desert_main", "generic_unhookable", "grass_doodads", "grass_main", "jungle_main", "winter_main", "generic_shadows"};
	// DDNet -> 0.7 -> DDNet: the same tiles, the shadows in a layer of their own
	for(const char *pName : {"dm2", "dm7", "ctf5", "ctf2"})
	{
		const std::vector<uint8_t> vSource = DDNetMap(pName);
		for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID})
		{
			const CMapConvertResult To07 = Convert(vSource, EMapConvertDirection::TO07, Mode);
			ASSERT_TRUE(To07.m_Converted) << pName;
			const CMapConvertResult Back = Convert(To07.m_vData, EMapConvertDirection::TO06, Mode);
			ASSERT_TRUE(Back.m_Converted) << pName;
			// Where the mapping goes both ways, and with hybrid the tiles without a counterpart as well
			const auto [vBefore, vAfter] = ComparableTiles(PlacedTiles(vSource, vSets), PlacedTiles(Back.m_vData, vSets), [Mode](const CPlacedTile &Tile) { return ComesBack(true, Tile.m_Image, Tile.m_Index, Mode); });
			EXPECT_FALSE(vBefore.empty()) << pName;
			EXPECT_EQ(vAfter, vBefore) << pName << " " << MapConvertModeName(Mode);
		}
	}
	// 0.7 -> DDNet -> 0.7
	for(const char *pName : {"dm1", "ctf2"})
	{
		const std::vector<uint8_t> vSource = Teeworlds07Map(pName);
		for(EMapConvertMode Mode : {EMapConvertMode::REMAP, EMapConvertMode::HYBRID})
		{
			const CMapConvertResult To06 = Convert(vSource, EMapConvertDirection::TO06, Mode);
			ASSERT_TRUE(To06.m_Converted) << pName;
			const CMapConvertResult Back = Convert(To06.m_vData, EMapConvertDirection::TO07, Mode);
			ASSERT_TRUE(Back.m_Converted) << pName;
			const auto [vBefore, vAfter] = ComparableTiles(PlacedTiles(vSource, vSets), PlacedTiles(Back.m_vData, vSets), [Mode](const CPlacedTile &Tile) { return ComesBack(false, Tile.m_Image, Tile.m_Index, Mode); });
			EXPECT_FALSE(vBefore.empty()) << pName;
			EXPECT_EQ(vAfter, vBefore) << pName << " " << MapConvertModeName(Mode);
		}
	}
}

TEST_F(MapConvert, RgbBecomesRgba)
{
	CTestMap Map;
	const std::vector<uint8_t> vRgb = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
	const int Image = Map.AddImage("rgb", false, 2, 0, 2, 2, vRgb);
	Map.AddTileLayer(-1, 2, 2, Tiles({}, 4), TILESLAYERFLAG_GAME);
	Map.AddQuadsLayer(Image);
	Map.EndGroup();
	const std::vector<uint8_t> vSource = Map.Finish();
	ASSERT_TRUE(Is07(vSource));
	ASSERT_TRUE(NeedsConversion(vSource, EMapConvertDirection::TO06));

	const CMapConvertResult Result = Convert(vSource, EMapConvertDirection::TO06, EMapConvertMode::REMAP);
	ASSERT_TRUE(Result.m_Converted);
	EXPECT_EQ(Result.m_Stats.m_RgbImages, 1);
	CDataFileReader Output;
	ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
	int ImageStart, ImageNum;
	Output.GetType(MAPITEMTYPE_IMAGE, &ImageStart, &ImageNum);
	ASSERT_EQ(ImageNum, 1);
	ASSERT_EQ(Output.GetItemSize(ImageStart), (int)sizeof(CMapItemImage_v1));
	const CMapItemImage *pImage = static_cast<const CMapItemImage *>(Output.GetItem(ImageStart));
	EXPECT_EQ(pImage->m_Version, 1);
	ASSERT_EQ(Output.GetDataSize(pImage->m_ImageData), 16);
	const uint8_t *pPixels = static_cast<const uint8_t *>(Output.GetData(pImage->m_ImageData));
	const std::vector<uint8_t> vRgba = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
	EXPECT_TRUE(std::equal(vRgba.begin(), vRgba.end(), pPixels));
	// To 0.7 it stays RGB
	EXPECT_FALSE(Convert(vSource, EMapConvertDirection::TO07, EMapConvertMode::REMAP).m_Converted);
}

TEST_F(MapConvert, QuadsShowingWhatChangedNeedConverting)
{
	using namespace Map07Tables;
	// A cell of grass_main that looks the same in both versions, and one that does not
	int Same = -1, Changed = -1;
	for(int Cell = 1; Cell < 256; Cell++)
	{
		const bool Differs = (CHANGED[SET_GRASS_MAIN][Cell / 32] >> (Cell % 32)) & 1;
		if(Differs && Changed < 0)
			Changed = Cell;
		else if(!Differs && Same < 0)
			Same = Cell;
	}
	ASSERT_GE(Same, 0);
	ASSERT_GE(Changed, 0);
	// Tile 1 is where it was
	ASSERT_FALSE((CHANGED[SET_GRASS_MAIN][0] >> 1) & 1);

	const auto QuadMap = [](int Cell, bool Repeated) {
		CTestMap Map;
		const int Image = Map.AddImage("grass_main", true, 1, 1, 1024, 1024, {});
		Map.AddTileLayer(-1, 2, 2, Tiles({}, 4), TILESLAYERFLAG_GAME);
		Map.AddTileLayer(Image, 2, 2, Tiles({CTile{1, 0, 0, 0}}, 4));
		const int Left = Cell % 16 * 64 - (Repeated ? 1024 : 0), Top = Cell / 16 * 64;
		Map.AddQuadsLayer(Image, Left, Top, Left + 64, Top + 64);
		Map.EndGroup();
		return Map.Finish();
	};

	// Only what looks the same: the same file serves both
	const std::vector<uint8_t> vSame = QuadMap(Same, false);
	EXPECT_FALSE(NeedsConversion(vSame, EMapConvertDirection::TO07));
	EXPECT_FALSE(Convert(vSame, EMapConvertDirection::TO07, EMapConvertMode::REMAP).m_Converted);

	// Something that changed: the quads get the picture they were made with
	for(bool Repeated : {false, true})
	{
		const std::vector<uint8_t> vChanged = QuadMap(Repeated ? Same : Changed, Repeated);
		EXPECT_TRUE(NeedsConversion(vChanged, EMapConvertDirection::TO07)) << Repeated;
		const CMapConvertResult Result = Convert(vChanged, EMapConvertDirection::TO07, EMapConvertMode::REMAP);
		ASSERT_TRUE(Result.m_Converted) << Repeated;
		EXPECT_EQ(Result.m_Stats.m_QuadImages, 1);
		EXPECT_EQ(Result.m_Stats.m_SplitLayers, 0);

		CDataFileReader Output;
		ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
		int ImageStart, ImageNum, LayerStart, LayerNum;
		Output.GetType(MAPITEMTYPE_IMAGE, &ImageStart, &ImageNum);
		Output.GetType(MAPITEMTYPE_LAYER, &LayerStart, &LayerNum);
		ASSERT_EQ(ImageNum, 2);
		const CMapItemLayerQuads *pQuads = static_cast<const CMapItemLayerQuads *>(Output.GetItem(LayerStart + 2));
		ASSERT_EQ(pQuads->m_Layer.m_Type, LAYERTYPE_QUADS);
		ASSERT_EQ(pQuads->m_Image, 1);
		EXPECT_FALSE(static_cast<const CMapItemImage *>(Output.GetItem(ImageStart + 1))->m_External);
		// The tiles keep the picture of the other version
		EXPECT_TRUE(static_cast<const CMapItemImage *>(Output.GetItem(ImageStart))->m_External);
		const std::vector<CLayerInfo> vLayers = TileLayers(Result.m_vData);
		ASSERT_EQ(vLayers.size(), 1u);
		EXPECT_EQ(vLayers[0].m_vTiles[0].m_Index, 1);
	}
}

TEST_F(MapConvert, QuadsKeepTheirPicture)
{
	CTestMap Map;
	const int Image = Map.AddImage("grass_main", true, 1, 1, 1024, 1024, {});
	Map.AddTileLayer(-1, 2, 2, Tiles({}, 4), TILESLAYERFLAG_GAME);
	// A shadow, which moves to generic_shadows
	Map.AddTileLayer(Image, 2, 2, Tiles({CTile{1, 0, 0, 0}, CTile{76, TILEFLAG_ROTATE, 0, 0}}, 4));
	Map.AddQuadsLayer(Image);
	Map.EndGroup();
	const CMapConvertResult Result = Convert(Map.Finish(), EMapConvertDirection::TO07, EMapConvertMode::REMAP);
	ASSERT_TRUE(Result.m_Converted);
	EXPECT_EQ(Result.m_Stats.m_QuadImages, 1);
	EXPECT_EQ(Result.m_Stats.m_SplitLayers, 1);

	CDataFileReader Output;
	ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
	int ImageStart, ImageNum, LayerStart, LayerNum;
	Output.GetType(MAPITEMTYPE_IMAGE, &ImageStart, &ImageNum);
	Output.GetType(MAPITEMTYPE_LAYER, &LayerStart, &LayerNum);
	ASSERT_EQ(ImageNum, 3); // grass_main, its copy for the quads, generic_shadows
	ASSERT_EQ(LayerNum, 4);
	const CMapItemLayerQuads *pQuads = static_cast<const CMapItemLayerQuads *>(Output.GetItem(LayerStart + 3));
	ASSERT_EQ(pQuads->m_Layer.m_Type, LAYERTYPE_QUADS);
	ASSERT_NE(pQuads->m_Image, Image);
	const CMapItemImage *pCopy = static_cast<const CMapItemImage *>(Output.GetItem(ImageStart + pQuads->m_Image));
	EXPECT_FALSE(pCopy->m_External);
	EXPECT_STREQ(Output.GetDataString(pCopy->m_ImageName), "grass_main");

	const std::vector<CLayerInfo> vLayers = TileLayers(Result.m_vData);
	ASSERT_EQ(vLayers.size(), 2u);
	EXPECT_EQ(vLayers[0].m_ImageName, "generic_shadows");
	EXPECT_EQ(vLayers[0].m_vTiles[1].m_Index, 17);
	EXPECT_EQ(vLayers[0].m_vTiles[1].m_Flags & ORIENTATION, TILEFLAG_ROTATE);
	EXPECT_EQ(vLayers[1].m_ImageName, "grass_main");
	EXPECT_EQ(vLayers[1].m_vTiles[0].m_Index, 1);
	EXPECT_EQ(vLayers[1].m_vTiles[1].m_Index, 0);
	// Tile 1 of grass_main covers its cell, a shadow does not
	EXPECT_TRUE(vLayers[1].m_vTiles[0].m_Flags & TILEFLAG_OPAQUE);
	EXPECT_FALSE(vLayers[0].m_vTiles[1].m_Flags & TILEFLAG_OPAQUE);
}

TEST_F(MapConvert, EnvelopesKeepTheirCurves)
{
	CTestMap Map;
	const int Image = Map.AddImage("grass_main", true, 1, 1, 1024, 1024, {});
	Map.AddTileLayer(-1, 2, 2, Tiles({}, 4), TILESLAYERFLAG_GAME);
	Map.AddTileLayer(Image, 2, 2, Tiles({CTile{76, 0, 0, 0}}, 4));
	Map.EndGroup();
	CMapItemEnvelope Envelope = {};
	Envelope.m_Version = 2;
	Envelope.m_Channels = 4;
	Envelope.m_NumPoints = 2;
	Envelope.m_Synchronized = 0;
	Map.m_Writer.AddItem(MAPITEMTYPE_ENVELOPE, 0, sizeof(Envelope), &Envelope);
	CEnvPoint aPoints[2] = {};
	aPoints[0].m_Curvetype = CURVETYPE_BEZIER;
	aPoints[1].m_Time = CFixedTime(1000);
	aPoints[1].m_aValues[0] = 1024;
	Map.m_Writer.AddItem(MAPITEMTYPE_ENVPOINTS, 0, sizeof(aPoints), aPoints);
	CEnvPointBezier aBezier[2] = {};
	aBezier[0].m_aOutTangentDeltaX[0] = CFixedTime(300);
	aBezier[0].m_aOutTangentDeltaY[0] = 512;
	Map.m_Writer.AddItem(MAPITEMTYPE_ENVPOINTS_BEZIER, 0, sizeof(aBezier), aBezier);

	const CMapConvertResult Result = Convert(Map.Finish(), EMapConvertDirection::TO07, EMapConvertMode::REMAP);
	ASSERT_TRUE(Result.m_Converted);
	EXPECT_EQ(Result.m_Stats.m_Envelopes, 1);
	CDataFileReader Output;
	ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
	int Start, Num;
	Output.GetType(MAPITEMTYPE_ENVELOPE, &Start, &Num);
	ASSERT_EQ(Num, 1);
	const CMapItemEnvelope *pEnvelope = static_cast<const CMapItemEnvelope *>(Output.GetItem(Start));
	EXPECT_EQ(pEnvelope->m_Version, CMapItemEnvelope::VERSION_TEEWORLDS_BEZIER);
	EXPECT_EQ(pEnvelope->m_Synchronized, 0);
	Output.GetType(MAPITEMTYPE_ENVPOINTS, &Start, &Num);
	ASSERT_EQ(Num, 1);
	ASSERT_EQ(Output.GetItemSize(Start), (int)(2 * sizeof(CEnvPointBezier_upstream)));
	const CEnvPointBezier_upstream *pPoints = static_cast<const CEnvPointBezier_upstream *>(Output.GetItem(Start));
	EXPECT_EQ(pPoints[0].m_Curvetype, CURVETYPE_BEZIER);
	EXPECT_EQ(pPoints[0].m_Bezier.m_aOutTangentDeltaX[0].GetInternal(), 300);
	EXPECT_EQ(pPoints[0].m_Bezier.m_aOutTangentDeltaY[0], 512);
	EXPECT_EQ(pPoints[1].m_Time.GetInternal(), 1000);
	EXPECT_EQ(pPoints[1].m_aValues[0], 1024);
	Output.GetType(MAPITEMTYPE_ENVPOINTS_BEZIER, &Start, &Num);
	EXPECT_EQ(Num, 0);
}

// Some 0.7 maps in circulation say too small a size for an item, which
// Teeworlds 0.7 reads, but older DDNet versions do not
TEST_F(MapConvert, To06WritesWrongItemSizesRight)
{
	std::vector<uint8_t> vMap = Convert(DDNetMap("dm6"), EMapConvertDirection::TO07, EMapConvertMode::MARK).m_vData;
	ASSERT_TRUE(Is07(vMap));
	EXPECT_FALSE(NeedsConversion(vMap, EMapConvertDirection::TO06));

	// The items follow the header, the item types, the item offsets and the
	// data offsets and sizes; the size of an item follows its type and ID
	int32_t aHeader[9];
	ASSERT_GE(vMap.size(), sizeof(aHeader));
	mem_copy(aHeader, vMap.data(), sizeof(aHeader));
	const size_t SizeOffset = sizeof(aHeader) + aHeader[4] * 3 * sizeof(int32_t) + aHeader[5] * sizeof(int32_t) + aHeader[6] * 2 * sizeof(int32_t) + sizeof(int32_t);
	int32_t Size;
	mem_copy(&Size, vMap.data() + SizeOffset, sizeof(Size));
	ASSERT_GT(Size, 0);
	Size -= sizeof(int32_t);
	mem_copy(vMap.data() + SizeOffset, &Size, sizeof(Size));

	EXPECT_TRUE(NeedsConversion(vMap, EMapConvertDirection::TO06));
	const CMapConvertResult Result = Convert(vMap, EMapConvertDirection::TO06, EMapConvertMode::REMAP);
	ASSERT_TRUE(Result.m_Converted);
	CDataFileReader Output;
	ASSERT_TRUE(Output.OpenFromMemory("output", Result.m_vData, "memory"));
	EXPECT_FALSE(Output.ItemSizesWrong());
}

TEST_F(MapConvert, MarkAs07)
{
	for(const char *pName : {"dm6", "ctf3"})
	{
		const std::vector<uint8_t> vSource = DDNetMap(pName);
		CMapConvertResult Result = Convert(vSource, EMapConvertDirection::TO07, EMapConvertMode::MARK);
		ASSERT_TRUE(Result.m_Converted);
		EXPECT_TRUE(Is07(Result.m_vData));
		EXPECT_GT(Result.m_Stats.m_MarkedImages, 0);
		EXPECT_EQ(Result.m_Stats.m_EmbeddedImages, 0);
		EXPECT_EQ(Result.m_Stats.m_RewrittenLayers, 0);
		// Every data block went through untouched
		CDataFileReader Source;
		ASSERT_TRUE(Source.OpenFromMemory("source", vSource, "memory"));
		EXPECT_EQ(Result.m_Stats.m_PassedData, Source.NumData());
		EXPECT_FALSE(Convert(Result.m_vData, EMapConvertDirection::TO07, EMapConvertMode::MARK).m_Converted);
	}
}
