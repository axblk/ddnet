#include "test.h"

#include <base/str.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/map_file.h>
#include <game/map/document/structure.h>
#include <game/mapitems.h>
#include <game/mapitems_ex.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <vector>

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

namespace
{
	// Reads a whole file, for comparing what was written against what was there.
	std::vector<uint8_t> FileBytes(IStorage *pStorage, const char *pPath, int Type)
	{
		void *pData;
		unsigned Size;
		if(!pStorage->ReadFile(pPath, Type, &pData, &Size))
			return std::vector<uint8_t>();
		std::vector<uint8_t> vBytes(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
		free(pData);
		return vBytes;
	}

	// Writes a state out and hands back the bytes that landed on disk.
	std::vector<uint8_t> WriteAndRead(IStorage *pStorage, const char *pPath, const CMapState &State)
	{
		CDataFileWriter Writer;
		if(!Writer.Open(pStorage, pPath, IStorage::TYPE_ABSOLUTE))
			return std::vector<uint8_t>();
		WriteMapState(Writer, State);
		Writer.Finish();
		return FileBytes(pStorage, pPath, IStorage::TYPE_ABSOLUTE);
	}
} // namespace

namespace
{
	// Whether two stretches of bytes are the same. Nothing of nothing is the
	// same as nothing - and asking `memcmp` that, with the two null pointers
	// an empty list hands out, is undefined behaviour. An external image and
	// a layer without quads both ask exactly that.
	bool SameBytes(const void *pOne, const void *pOther, size_t Size)
	{
		return Size == 0 || std::memcmp(pOne, pOther, Size) == 0;
	}

	// Everything two versions of a map have to agree on for one to be the other
	// written down and read back.
	void ExpectSameMap(const CMapState &One, const CMapState &Other, const std::string &Name)
	{
		ASSERT_EQ(One.NumGroups(), Other.NumGroups()) << Name;
		ASSERT_EQ(One.NumImages(), Other.NumImages()) << Name;
		ASSERT_EQ(One.NumSounds(), Other.NumSounds()) << Name;
		ASSERT_EQ(One.NumEnvelopes(), Other.NumEnvelopes()) << Name;
		EXPECT_EQ(One.m_Info.m_Author, Other.m_Info.m_Author) << Name;
		EXPECT_EQ(One.m_Info.m_MapVersion, Other.m_Info.m_MapVersion) << Name;
		EXPECT_EQ(One.m_Info.m_Credits, Other.m_Info.m_Credits) << Name;
		EXPECT_EQ(One.m_Info.m_License, Other.m_Info.m_License) << Name;
		ASSERT_EQ(One.m_Info.m_Settings.Size(), Other.m_Info.m_Settings.Size()) << Name;
		for(size_t i = 0; i < One.m_Info.m_Settings.Size(); ++i)
		{
			EXPECT_EQ(One.m_Info.m_Settings[i], Other.m_Info.m_Settings[i]) << Name;
		}

		for(size_t i = 0; i < One.NumImages(); ++i)
		{
			const CImage *pOne = One.Image(i);
			const CImage *pOther = Other.Image(i);
			EXPECT_EQ(pOne->m_Name, pOther->m_Name) << Name;
			EXPECT_EQ(pOne->m_External, pOther->m_External) << Name;
			EXPECT_EQ(pOne->m_Width, pOther->m_Width) << Name;
			EXPECT_EQ(pOne->m_Height, pOther->m_Height) << Name;
			ASSERT_EQ(pOne->m_Data.Size(), pOther->m_Data.Size()) << Name;
			EXPECT_TRUE(SameBytes(pOne->m_Data.All().data(), pOther->m_Data.All().data(), pOne->m_Data.Size())) << Name;
		}
		for(size_t i = 0; i < One.NumSounds(); ++i)
		{
			ASSERT_EQ(One.Sound(i)->m_Data.Size(), Other.Sound(i)->m_Data.Size()) << Name;
			EXPECT_EQ(One.Sound(i)->m_Name, Other.Sound(i)->m_Name) << Name;
			EXPECT_TRUE(SameBytes(One.Sound(i)->m_Data.All().data(), Other.Sound(i)->m_Data.All().data(), One.Sound(i)->m_Data.Size())) << Name;
		}
		for(size_t i = 0; i < One.NumEnvelopes(); ++i)
		{
			const CEnvelope *pOne = One.Envelope(i);
			const CEnvelope *pOther = Other.Envelope(i);
			EXPECT_EQ(pOne->m_Name, pOther->m_Name) << Name;
			EXPECT_EQ(pOne->m_Channels, pOther->m_Channels) << Name;
			EXPECT_EQ(pOne->m_Synchronized, pOther->m_Synchronized) << Name;
			ASSERT_EQ(pOne->m_Points.Size(), pOther->m_Points.Size()) << Name;
			for(size_t p = 0; p < pOne->m_Points.Size(); ++p)
			{
				// Field by field rather than byte by byte: a point is a
				// `CEnvPoint_runtime`, which inherits, and what a compiler
				// puts between the base and what was added is its own
				// business.
				const CEnvPoint &OnePoint = pOne->m_Points[p];
				const CEnvPoint &OtherPoint = pOther->m_Points[p];
				EXPECT_EQ(OnePoint.m_Time, OtherPoint.m_Time) << Name;
				EXPECT_EQ(OnePoint.m_Curvetype, OtherPoint.m_Curvetype) << Name;
				for(int Channel = 0; Channel < CEnvPoint::MAX_CHANNELS; ++Channel)
				{
					EXPECT_EQ(OnePoint.m_aValues[Channel], OtherPoint.m_aValues[Channel]) << Name;
				}
			}
		}

		for(size_t g = 0; g < One.NumGroups(); ++g)
		{
			const CGroup *pOne = One.Group(g);
			const CGroup *pOther = Other.Group(g);
			EXPECT_EQ(pOne->m_Name, pOther->m_Name) << Name;
			EXPECT_EQ(pOne->m_OffsetX, pOther->m_OffsetX) << Name;
			EXPECT_EQ(pOne->m_ParallaxY, pOther->m_ParallaxY) << Name;
			EXPECT_EQ(pOne->m_UseClipping, pOther->m_UseClipping) << Name;
			EXPECT_EQ(pOne->m_ClipW, pOther->m_ClipW) << Name;
			ASSERT_EQ(One.NumLayers(g), Other.NumLayers(g)) << Name;
			for(size_t l = 0; l < One.NumLayers(g); ++l)
			{
				const CLayer *pLayerOne = One.Layer(g, l);
				const CLayer *pLayerOther = Other.Layer(g, l);
				ASSERT_EQ(pLayerOne->index(), pLayerOther->index()) << Name;
				EXPECT_EQ(LayerProperties(*pLayerOne).m_Name, LayerProperties(*pLayerOther).m_Name) << Name;
				EXPECT_EQ(LayerProperties(*pLayerOne).m_Detail, LayerProperties(*pLayerOther).m_Detail) << Name;
				if(std::holds_alternative<CTileLayer>(*pLayerOne))
				{
					const CTileLayer &TilesOne = std::get<CTileLayer>(*pLayerOne);
					const CTileLayer &TilesOther = std::get<CTileLayer>(*pLayerOther);
					EXPECT_EQ((int)TilesOne.m_Kind, (int)TilesOther.m_Kind) << Name;
					EXPECT_EQ(TilesOne.m_Image, TilesOther.m_Image) << Name;
					EXPECT_EQ(TilesOne.m_ColorEnvelope, TilesOther.m_ColorEnvelope) << Name;
					EXPECT_EQ(TilesOne.m_AutomapperConfig, TilesOther.m_AutomapperConfig) << Name;
					EXPECT_EQ(TilesOne.m_AutomapperSeed, TilesOther.m_AutomapperSeed) << Name;
					EXPECT_TRUE(TilesOne.m_Tiles == TilesOther.m_Tiles) << Name;
					EXPECT_TRUE(TilesOne.m_ExtraTiles == TilesOther.m_ExtraTiles) << Name;
				}
				else if(std::holds_alternative<CQuadLayer>(*pLayerOne))
				{
					const CQuadLayer &QuadsOne = std::get<CQuadLayer>(*pLayerOne);
					const CQuadLayer &QuadsOther = std::get<CQuadLayer>(*pLayerOther);
					EXPECT_EQ(QuadsOne.m_Image, QuadsOther.m_Image) << Name;
					ASSERT_EQ(QuadsOne.m_Quads.Size(), QuadsOther.m_Quads.Size()) << Name;
					EXPECT_TRUE(SameBytes(QuadsOne.m_Quads.All().data(), QuadsOther.m_Quads.All().data(), QuadsOne.m_Quads.Size() * sizeof(CQuad))) << Name;
				}
				else
				{
					const CSoundLayer &SoundsOne = std::get<CSoundLayer>(*pLayerOne);
					const CSoundLayer &SoundsOther = std::get<CSoundLayer>(*pLayerOther);
					EXPECT_EQ(SoundsOne.m_Sound, SoundsOther.m_Sound) << Name;
					ASSERT_EQ(SoundsOne.m_Sources.Size(), SoundsOther.m_Sources.Size()) << Name;
					EXPECT_TRUE(SameBytes(SoundsOne.m_Sources.All().data(), SoundsOther.m_Sources.All().data(), SoundsOne.m_Sources.Size() * sizeof(CSoundSource))) << Name;
				}
			}
		}
	}
} // namespace

TEST(MapFile, WritingAMapThatWasReadGivesTheSameMapBack)
{
	CTestInfo Info;
	const std::vector<std::string> vNames = MapNames();
	ASSERT_GT(vNames.size(), 10u);

	std::vector<std::string> vSameAsTheOriginal;
	for(const std::string &Name : vNames)
	{
		CMapFile Map;
		ASSERT_TRUE(Map.Read(Name.c_str())) << Name;

		char aFirst[IO_MAX_PATH_LENGTH];
		char aSecond[IO_MAX_PATH_LENGTH];
		str_format(aFirst, sizeof(aFirst), "%s-1.map", Info.m_aFilenamePrefix);
		str_format(aSecond, sizeof(aSecond), "%s-2.map", Info.m_aFilenamePrefix);

		const std::vector<uint8_t> vFirst = WriteAndRead(Map.m_pStorage.get(), aFirst, Map.m_State);
		ASSERT_FALSE(vFirst.empty()) << Name;

		// The map that was written is read again, and writing that one has to
		// give the same bytes: whatever the reader kept, the writer wrote, and
		// whatever it threw away was already gone the first time round.
		CDataFileReader Again;
		ASSERT_TRUE(Again.Open(Map.m_pStorage.get(), aFirst, IStorage::TYPE_ABSOLUTE)) << Name;
		CMapState State;
		std::vector<std::string> vWarnings;
		ASSERT_TRUE(ReadMapState(Again, &State, &vWarnings)) << Name;
		EXPECT_TRUE(vWarnings.empty()) << Name << ": " << (vWarnings.empty() ? "" : vWarnings[0]);

		const std::vector<uint8_t> vSecond = WriteAndRead(Map.m_pStorage.get(), aSecond, State);
		EXPECT_EQ(vFirst, vSecond) << Name;
		ExpectSameMap(Map.m_State, State, Name);

		char aOriginal[IO_MAX_PATH_LENGTH];
		str_format(aOriginal, sizeof(aOriginal), "data/maps/%s.map", Name.c_str());
		if(vFirst == FileBytes(Map.m_pStorage.get(), aOriginal, IStorage::TYPE_ALL))
			vSameAsTheOriginal.push_back(Name);

		Map.m_pStorage->RemoveFile(aFirst, IStorage::TYPE_ABSOLUTE);
		Map.m_pStorage->RemoveFile(aSecond, IStorage::TYPE_ABSOLUTE);
	}
	// Which ones came out byte for byte as they went in is worth knowing, but
	// it is not what is being asked of the writer here: a map that was
	// written by an older editor is written the way this one writes, not the
	// way it was. The ones that do match are the ones that were already
	// written this way - see the test below.
	std::cerr << "[          ] " << vSameAsTheOriginal.size() << " of " << vNames.size()
		  << " maps came out byte for byte as the file that was read:";
	for(const std::string &Same : vSameAsTheOriginal)
	{
		std::cerr << " " << Same;
	}
	std::cerr << "\n";
}

TEST(MapFile, NothingInTheseFilesGoesUnread)
{
	// A map item this does not know about is a map item that would be lost
	// on the way out, and nothing here would notice - so the list of what
	// there is to read is checked against the files themselves.
	const int aKnown[] = {
		MAPITEMTYPE_VERSION, MAPITEMTYPE_INFO, MAPITEMTYPE_IMAGE, MAPITEMTYPE_ENVELOPE,
		MAPITEMTYPE_GROUP, MAPITEMTYPE_LAYER, MAPITEMTYPE_ENVPOINTS, MAPITEMTYPE_SOUND,
		MAPITEMTYPE_AUTOMAPPER_CONFIG, MAPITEMTYPE_ENVPOINTS_BEZIER,
		// The list the file keeps of its own item types that are named by a
		// uuid rather than by a number. It is the file's bookkeeping, not the
		// map's, and the writer makes a new one.
		ITEMTYPE_EX};

	for(const std::string &Name : MapNames())
	{
		CMapFile Map;
		ASSERT_TRUE(Map.Read(Name.c_str())) << Name;
		for(int i = 0; i < Map.m_File.NumItems(); ++i)
		{
			int Type;
			Map.m_File.GetItem(i, &Type, nullptr, nullptr);
			EXPECT_NE(std::find(std::begin(aKnown), std::end(aKnown), Type), std::end(aKnown))
				<< Name << " holds an item of type " << Type << " that is not read";
		}
	}
}

TEST(MapFile, AMapWrittenTheWayThisWritesComesOutAsItWentIn)
{
	// `coverage` is written in the format of today, which is what this writes,
	// so there is nothing left to explain a difference away with: the bytes
	// have to match. It is the guard against a field being read and then
	// written as something else - the map has envelopes, quads, sound and
	// tile layers, an embedded and an external image, and server settings.
	CMapFile Map;
	ASSERT_TRUE(Map.Read("coverage"));
	EXPECT_TRUE(Map.m_vWarnings.empty());

	CTestInfo Info;
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "%s.map", Info.m_aFilenamePrefix);
	const std::vector<uint8_t> vOurs = WriteAndRead(Map.m_pStorage.get(), aPath, Map.m_State);
	ASSERT_FALSE(vOurs.empty());
	EXPECT_EQ(vOurs, FileBytes(Map.m_pStorage.get(), "data/maps/coverage.map", IStorage::TYPE_ALL));
	Map.m_pStorage->RemoveFile(aPath, IStorage::TYPE_ABSOLUTE);
}

TEST(MapFile, APictureAddedInTheEditorSurvivesBeingWritten)
{
	// A picture that the editor put in has pixels that were never in a file,
	// which is the one case the maps that ship with the game cannot cover:
	// they were all written by an editor that had already written them once.
	CMapFile Map;
	ASSERT_TRUE(Map.Read("coverage"));
	CDocument Document(Map.m_State);
	CImage Image;
	Image.m_Name = "added";
	Image.m_External = false;
	Image.m_Width = 4;
	Image.m_Height = 2;
	std::vector<uint8_t> &vPixels = Image.m_Data.Mutable();
	vPixels.resize(4 * 2 * 4);
	for(size_t Byte = 0; Byte < vPixels.size(); ++Byte)
		vPixels[Byte] = (uint8_t)Byte;
	Document.Begin("Add image");
	const size_t Index = AddImage(Document, std::move(Image));
	Document.Commit();

	CTestInfo Info;
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "%s.map", Info.m_aFilenamePrefix);
	ASSERT_FALSE(WriteAndRead(Map.m_pStorage.get(), aPath, Document.Map()).empty());

	CDataFileReader Again;
	ASSERT_TRUE(Again.Open(Map.m_pStorage.get(), aPath, IStorage::TYPE_ABSOLUTE));
	CMapState Read;
	std::vector<std::string> vWarnings;
	ASSERT_TRUE(ReadMapState(Again, &Read, &vWarnings));
	EXPECT_TRUE(vWarnings.empty()) << (vWarnings.empty() ? "" : vWarnings[0]);

	ASSERT_EQ(Read.NumImages(), Index + 1);
	const CImage *pBack = Read.Image(Index);
	EXPECT_EQ(pBack->m_Name, "added");
	EXPECT_FALSE(pBack->m_External);
	EXPECT_EQ(pBack->m_Width, 4);
	EXPECT_EQ(pBack->m_Height, 2);
	ASSERT_EQ(pBack->m_Data.Size(), 4u * 2u * 4u);
	for(size_t Byte = 0; Byte < pBack->m_Data.Size(); ++Byte)
		ASSERT_EQ(pBack->m_Data[Byte], (uint8_t)Byte) << "byte " << Byte;

	Map.m_pStorage->RemoveFile(aPath, IStorage::TYPE_ABSOLUTE);
}

TEST(MapFile, AGroupKeepsTheClipItWasSavedWith)
{
	// dm6 has two clipped groups, and their items are of version 2: the clip
	// arrived with that version and the name only with the next, so a version
	// 2 item is shorter than the struct it is read as. Asking for the whole
	// size before reading the clip dropped it from every map of that age -
	// found by drawing dm6 twice, once from the file and once from the
	// document, and seeing one tile come out where the file clips it away.
	CMapFile Map;
	ASSERT_TRUE(Map.Read("dm6"));
	ASSERT_GE(Map.m_State.NumGroups(), 6u);

	const CGroup *pFirst = Map.m_State.Group(4);
	EXPECT_TRUE(pFirst->m_UseClipping);
	EXPECT_EQ(pFirst->m_ClipX, 1307);
	EXPECT_EQ(pFirst->m_ClipY, 0);
	EXPECT_EQ(pFirst->m_ClipW, 775);
	EXPECT_EQ(pFirst->m_ClipH, 485);

	const CGroup *pSecond = Map.m_State.Group(5);
	EXPECT_TRUE(pSecond->m_UseClipping);
	EXPECT_EQ(pSecond->m_ClipX, 2);
	EXPECT_EQ(pSecond->m_ClipY, 1225);
	EXPECT_EQ(pSecond->m_ClipW, 2884);
	EXPECT_EQ(pSecond->m_ClipH, 1821);

	// And a group that says it does not clip keeps whatever rectangle it
	// carries - the file holds one either way.
	EXPECT_FALSE(Map.m_State.Group(6)->m_UseClipping);
}
