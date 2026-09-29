#include "map_convert.h"

#include "map07_tables.h"
#include "mapres.h"

#include <base/bytes.h>
#include <base/dbg.h>
#include <base/hash.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/str.h>

#include <engine/shared/datafile.h>

#include <game/mapitems.h>
#include <game/mapitems_ex.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <iterator>
#include <limits>
#include <map>
#include <memory>

static constexpr int CONVERTER_VERSION = 1;
static constexpr int TILE_ORIENTATION = TILEFLAG_XFLIP | TILEFLAG_YFLIP | TILEFLAG_ROTATE;
static constexpr int PHYSICS_LAYER_FLAGS = TILESLAYERFLAG_GAME | TILESLAYERFLAG_TELE | TILESLAYERFLAG_SPEEDUP | TILESLAYERFLAG_FRONT | TILESLAYERFLAG_SWITCH | TILESLAYERFLAG_TUNE;
static constexpr int IMAGE_FORMAT_RGBA = 1;

const char *MapConvertDirectionName(EMapConvertDirection Direction)
{
	return Direction == EMapConvertDirection::TO07 ? "to07" : "to06";
}

static constexpr const char *MODE_NAMES[] = {"remap", "hybrid", "embed", "mark"};

const char *MapConvertModeName(EMapConvertMode Mode)
{
	return MODE_NAMES[static_cast<int>(Mode)];
}

bool ParseMapConvertMode(const char *pName, EMapConvertMode &Mode)
{
	for(size_t i = 0; i < std::size(MODE_NAMES); i++)
	{
		if(str_comp(pName, MODE_NAMES[i]) == 0)
		{
			Mode = static_cast<EMapConvertMode>(i);
			return true;
		}
	}
	return false;
}

uint32_t MapConvertVersion()
{
	const int32_t aParts[] = {CMapConvertProvenance::CURRENT_VERSION, Map07Tables::VERSION, static_cast<int32_t>(Map07Tables::CHECKSUM), CONVERTER_VERSION};
	return crc32(0, reinterpret_cast<const Bytef *>(aParts), sizeof(aParts));
}

// The corners of a tile, top left, top right, bottom left, bottom right, as
// a tile with these flags shows them: flipped horizontally, then vertically,
// then turned right, the way the tile renderer draws it.
static std::array<int, 4> OrientCorners(std::array<int, 4> Corners, int Flags)
{
	if(Flags & TILEFLAG_XFLIP)
		Corners = {Corners[1], Corners[0], Corners[3], Corners[2]};
	if(Flags & TILEFLAG_YFLIP)
		Corners = {Corners[2], Corners[3], Corners[0], Corners[1]};
	if(Flags & TILEFLAG_ROTATE)
		Corners = {Corners[2], Corners[0], Corners[3], Corners[1]};
	return Corners;
}

int ComposeTileFlags(int Flags, int Fix)
{
	static constexpr std::array<int, 4> CORNERS = {0, 1, 2, 3};
	const std::array<int, 4> Wanted = OrientCorners(OrientCorners(CORNERS, Fix), Flags);
	for(int Composed = 0; Composed <= TILE_ORIENTATION; Composed++)
	{
		if((Composed & ~TILE_ORIENTATION) == 0 && OrientCorners(CORNERS, Composed) == Wanted)
		{
			return Composed;
		}
	}
	dbg_assert_failed("The tile flags do not form a group: %d %d", Flags, Fix);
}

// What a converted map says about where it came from, as it is stored
class CMapItemConverted
{
public:
	int m_Version;
	int m_Direction; // 7 or 6
	int m_Mode;
	int m_aSourceSha256[8];
	int m_SourceCrc;
	int m_SourceSize;
	int m_TableVersion;
	int m_TableChecksum;
	int m_ConverterVersion;
};

CUuid MapConvertProvenanceUuid()
{
	static const CUuid s_Uuid = CalculateUuid("converted@ddnet.tw");
	return s_Uuid;
}

static int FindProvenanceItem(CDataFileReader &Reader)
{
	const CUuid Uuid = MapConvertProvenanceUuid();
	for(int Index = 0; Index < Reader.NumItems(); Index++)
	{
		CUuid ItemUuid;
		Reader.GetItem(Index, nullptr, nullptr, &ItemUuid);
		if(ItemUuid == Uuid && Reader.GetItemSize(Index) >= (int)sizeof(CMapItemConverted))
		{
			return Index;
		}
	}
	return -1;
}

bool ReadMapConvertProvenance(CDataFileReader &Reader, CMapConvertProvenance &Provenance)
{
	const int Index = FindProvenanceItem(Reader);
	if(Index < 0)
	{
		return false;
	}
	const CMapItemConverted *pItem = static_cast<const CMapItemConverted *>(Reader.GetItem(Index));
	Provenance.m_Version = pItem->m_Version;
	Provenance.m_Direction = pItem->m_Direction == 6 ? EMapConvertDirection::TO06 : EMapConvertDirection::TO07;
	Provenance.m_Mode = in_range(pItem->m_Mode, 0, (int)std::size(MODE_NAMES) - 1) ? static_cast<EMapConvertMode>(pItem->m_Mode) : EMapConvertMode::REMAP;
	for(int i = 0; i < 8; i++)
	{
		uint_to_bytes_be(&Provenance.m_SourceSha256.data[i * 4], pItem->m_aSourceSha256[i]);
	}
	Provenance.m_SourceCrc = pItem->m_SourceCrc;
	Provenance.m_SourceSize = pItem->m_SourceSize;
	Provenance.m_TableVersion = pItem->m_TableVersion;
	Provenance.m_TableChecksum = pItem->m_TableChecksum;
	Provenance.m_ConverterVersion = pItem->m_ConverterVersion;
	return true;
}

static bool IsOpaque(const uint32_t (&aBits)[8], int Index)
{
	return (aBits[Index / 32] >> (Index % 32)) & 1;
}

// The table for a picture of the source; 0.7's generic_shadows has one for every 0.6 tileset with the shadows
static const Map07Tables::CTileTable *FindTable(bool To07, const char *pName, Map07Tables::ESet ShadowHome)
{
	const auto &&Find = [&](const auto &aTables) -> const Map07Tables::CTileTable * {
		const Map07Tables::CTileTable *pFound = nullptr;
		for(const Map07Tables::CTileTable &Table : aTables)
		{
			if(str_comp(Map07Tables::SET_NAMES[Table.m_Source], pName) != 0)
				continue;
			if(pFound == nullptr || Table.m_Home == ShadowHome)
				pFound = &Table;
		}
		return pFound;
	};
	return To07 ? Find(Map07Tables::TO07) : Find(Map07Tables::TO06);
}

static bool IsTeeworlds07Mapres(const char *pName)
{
	return std::any_of(std::begin(Map07Tables::MAPRES07), std::end(Map07Tables::MAPRES07), [pName](const char *pMapres) {
		return str_comp(pName, pMapres) == 0;
	});
}

static std::vector<int32_t> CopyItem(const void *pData, int Size)
{
	std::vector<int32_t> vItem(Size / sizeof(int32_t));
	if(!vItem.empty())
	{
		mem_copy(vItem.data(), pData, vItem.size() * sizeof(int32_t));
	}
	return vItem;
}

// The members up to MinimumSize must be there, the rest of T may be missing in older versions of the item
template<typename T>
static T *ItemAs(std::vector<int32_t> &vItem, size_t MinimumSize = sizeof(T))
{
	dbg_assert(vItem.size() * sizeof(int32_t) >= MinimumSize, "Item too small");
	return static_cast<T *>(static_cast<void *>(vItem.data()));
}

static constexpr size_t QUADS_LAYER_WITH_IMAGE = offsetof(CMapItemLayerQuads, m_Image) + sizeof(int);

static CDataFileRawData UncompressedData(std::vector<uint8_t> vData)
{
	const size_t Size = vData.size();
	return CDataFileRawData(std::move(vData), Size, false);
}

static CDataFileRawData StringData(const char *pStr)
{
	return UncompressedData(std::vector<uint8_t>(pStr, pStr + str_length(pStr) + 1));
}

// Tile layers of version 4 store a run of equal tiles as one tile with the length of the run in m_Skip
static bool UnpackTileskip(std::vector<CTile> &vTiles, const CTile *pPacked, size_t NumPacked)
{
	size_t Tile = 0;
	for(size_t i = 0; i < NumPacked; i++)
	{
		if(pPacked[i].m_MustBe0 != 0 || Tile + pPacked[i].m_Skip + 1 > vTiles.size())
		{
			return false;
		}
		for(int Repeat = 0; Repeat <= pPacked[i].m_Skip; Repeat++)
		{
			vTiles[Tile++] = CTile{pPacked[i].m_Index, pPacked[i].m_Flags, 0, 0};
		}
	}
	return Tile == vTiles.size();
}

static std::vector<uint8_t> PackTiles(const std::vector<CTile> &vTiles, bool Tileskip)
{
	std::vector<uint8_t> vPacked;
	vPacked.reserve(vTiles.size() * sizeof(CTile));
	for(size_t i = 0; i < vTiles.size();)
	{
		size_t Run = 1;
		if(Tileskip)
		{
			while(i + Run < vTiles.size() && Run < 256 && vTiles[i + Run].m_Index == vTiles[i].m_Index && vTiles[i + Run].m_Flags == vTiles[i].m_Flags)
			{
				Run++;
			}
		}
		vPacked.push_back(vTiles[i].m_Index);
		vPacked.push_back(vTiles[i].m_Flags);
		vPacked.push_back(Run - 1);
		vPacked.push_back(0);
		i += Run;
	}
	return vPacked;
}

class CMapConverter
{
	enum class EAction
	{
		KEEP,
		EMBED,
		REMAP,
	};

	class CImage
	{
	public:
		std::vector<int32_t> m_vItem;
		int m_Id = 0;
		bool m_External = false;
		char m_aName[128] = "";
		CImageInfo::EImageFormat m_Format = CImageInfo::FORMAT_RGBA;
		const Map07Tables::CTileTable *m_pTable = nullptr;
		std::bitset<256> m_Used;
		bool m_UsedByTiles = false;
		bool m_UsedByQuads = false;
		bool m_UsesChanged = false;
		bool m_UsesFallback = false;
		// The diff tileset the tiles without a counterpart are drawn from (`HYBRID`), -1 for none
		int m_DiffImage = -1;
		// For an embedded diff tileset of the other direction: the tileset it becomes again, -1 for none
		int m_RestoreSet = -1;
		// Whether quads show a part of the picture that looks different in the other version
		bool m_QuadsUseChanged = false;
		EAction m_Action = EAction::KEEP;
		std::shared_ptr<const CMapresImage> m_pPicture;
		// The image the quads of a remapped picture use instead, -1 for none
		int m_QuadImage = -1;
	};

	class CLayer
	{
	public:
		int m_Type = 0;
		int m_Id = 0;
		std::vector<int32_t> m_vItem;
		// Tiles of a tile layer whose picture may change, unpacked
		std::vector<CTile> m_vTiles;
		bool m_Changed = false;
		// Tile layers for the tiles that go to another picture, drawn right below this one
		std::vector<std::vector<int32_t>> m_vvSplitItems;
		// Tile layers for the tiles without a counterpart (`HYBRID`), drawn right above this one
		std::vector<std::vector<int32_t>> m_vvAboveItems;
	};

	class COutputItem
	{
	public:
		int m_Type;
		int m_Id;
		CUuid m_Uuid;
		const void *m_pData;
		int m_Size;
		std::vector<int32_t> m_vOwned;
	};

	CDataFileReader &m_Reader;
	const CMapConvertOptions &m_Options;
	const IMapres &m_Mapres;
	CMapConvertResult &m_Result;
	const bool m_To07;

	std::vector<CDataFileRawData> m_vData;
	std::vector<CImage> m_vImages;
	std::vector<CImage> m_vNewImages;
	std::vector<CLayer> m_vLayers;
	int m_LayerStart = 0;
	int m_ImageStart = 0;
	std::map<int, int> m_SplitImages; // set -> image index
	EMapConvertMode m_Mode;

	[[gnu::format(printf, 2, 3)]] void Warn(const char *pFormat, ...);
	bool Fail(const char *pMessage);

	void ReadData();
	void ReadImages();
	void ReadLayers();
	bool DecideImages();
	bool RemapLayer(CLayer &Layer, CImage &Image);
	int SplitImage(Map07Tables::ESet Set);
	int DiffImage(Map07Tables::ESet Set, std::shared_ptr<const CMapresImage> pPicture);
	void UpdateImageItem(CImage &Image, int Index);
	void ConvertEnvelopes(std::vector<COutputItem> &vItems);
	bool Write();

public:
	/**
	 * Reads what decides whether the map needs converting.
	 *
	 * @return `false` if the map is in the target form already.
	 */
	bool Prepare();
	bool Needed() const;
	bool QuadsShowChangedCells(const CMapItemLayerQuads &Quads, Map07Tables::ESet Set) const;
	CMapConverter(CDataFileReader &Reader, const CMapConvertOptions &Options, const IMapres &Mapres, CMapConvertResult &Result) :
		m_Reader(Reader), m_Options(Options), m_Mapres(Mapres), m_Result(Result), m_To07(Options.m_Direction == EMapConvertDirection::TO07 || Options.m_Mode == EMapConvertMode::MARK), m_Mode(Options.m_Mode) {}

	bool Run();
};

void CMapConverter::Warn(const char *pFormat, ...)
{
	char aBuf[512];
	va_list Args;
	va_start(Args, pFormat);
	str_format_v(aBuf, sizeof(aBuf), pFormat, Args);
	va_end(Args);
	m_Result.m_vWarnings.emplace_back(aBuf);
}

bool CMapConverter::Fail(const char *pMessage)
{
	m_Result.m_Error = pMessage;
	m_Result.m_vData.clear();
	m_Result.m_Converted = false;
	return false;
}

void CMapConverter::ReadData()
{
	// Before anything else of the reader is used: data it has loaded already would be handed out uncompressed
	m_vData.resize(m_Reader.NumData());
	for(int Index = 0; Index < m_Reader.NumData(); Index++)
	{
		if(!m_Reader.GetRawData(Index, m_vData[Index]))
		{
			// Some old maps have data of size 0, which no one can read; the index must stay taken
			Warn("data %d cannot be read, it is left empty", Index);
			m_vData[Index] = UncompressedData(std::vector<uint8_t>(1, 0));
		}
	}
}

void CMapConverter::ReadImages()
{
	int Num;
	m_Reader.GetType(MAPITEMTYPE_IMAGE, &m_ImageStart, &Num);
	m_vImages.resize(Num);
	for(int i = 0; i < Num; i++)
	{
		CImage &Image = m_vImages[i];
		const int Size = m_Reader.GetItemSize(m_ImageStart + i);
		Image.m_vItem = CopyItem(m_Reader.GetItem(m_ImageStart + i, nullptr, &Image.m_Id), Size);
		if(Size < (int)sizeof(CMapItemImage_v1))
		{
			Warn("image %d is truncated", i);
			continue;
		}
		const CMapItemImage_v1 *pItem = ItemAs<CMapItemImage_v1>(Image.m_vItem);
		Image.m_External = pItem->m_External != 0;
		if(pItem->m_ImageName >= 0 && pItem->m_ImageName < (int)m_vData.size())
		{
			const std::unique_ptr<uint8_t[]> pName = m_vData[pItem->m_ImageName].Uncompress();
			const size_t NameSize = m_vData[pItem->m_ImageName].UncompressedSize();
			if(pName != nullptr && NameSize > 0 && pName[NameSize - 1] == '\0')
			{
				str_copy(Image.m_aName, reinterpret_cast<const char *>(pName.get()));
			}
		}
		if(Size >= (int)sizeof(CMapItemImage_v2))
		{
			Image.m_Format = MapImageFormat(ItemAs<CMapItemImage_v2>(Image.m_vItem));
		}
		// A diff tileset a conversion the other way embedded: the tiles are where they are in the tileset
		const auto &apBackDiffs = m_To07 ? Map07Tables::DIFFS06 : Map07Tables::DIFFS07;
		for(int Set = 0; Set < Map07Tables::NUM_SETS && !Image.m_External; Set++)
		{
			if(apBackDiffs[Set] != nullptr && str_comp(Image.m_aName, apBackDiffs[Set]) == 0)
			{
				Image.m_RestoreSet = Set;
			}
		}
	}

	// A generic_shadows picture of 0.7 goes to the shadows of a 0.6 tileset the map has already
	Map07Tables::ESet ShadowHome = Map07Tables::SET_JUNGLE_MAIN;
	for(Map07Tables::ESet Home : {Map07Tables::SET_GRASS_MAIN, Map07Tables::SET_DESERT_MAIN, Map07Tables::SET_JUNGLE_MAIN})
	{
		if(std::any_of(m_vImages.begin(), m_vImages.end(), [Home](const CImage &Image) { return Image.m_External && str_comp(Image.m_aName, Map07Tables::SET_NAMES[Home]) == 0; }))
		{
			ShadowHome = Home;
			break;
		}
	}
	for(CImage &Image : m_vImages)
	{
		if(Image.m_External && m_Mode != EMapConvertMode::MARK)
		{
			Image.m_pTable = FindTable(m_To07, Image.m_aName, ShadowHome);
		}
	}
}

bool CMapConverter::QuadsShowChangedCells(const CMapItemLayerQuads &Quads, Map07Tables::ESet Set) const
{
	if(!m_To07 && Map07Tables::SIZE06[Set][0] == 0)
	{
		return true; // DDNet has the picture only as a 0.7 one, under another name
	}
	if(Quads.m_Data < 0 || Quads.m_Data >= (int)m_vData.size())
	{
		return true;
	}
	const CDataFileRawData &Raw = m_vData[Quads.m_Data];
	const std::unique_ptr<uint8_t[]> pRaw = Raw.Uncompress();
	const size_t NumQuads = std::min<size_t>(Quads.m_NumQuads, Raw.UncompressedSize() / sizeof(CQuad));
	if(pRaw == nullptr || NumQuads < (size_t)Quads.m_NumQuads)
	{
		return true;
	}
	constexpr int FULL = 1024; // Texture coordinates are fixed point, 1024 is the whole picture
	for(size_t q = 0; q < NumQuads; q++)
	{
		CQuad Quad;
		mem_copy(&Quad, pRaw.get() + q * sizeof(CQuad), sizeof(CQuad));
		int aMin[2] = {Quad.m_aTexcoords[0].x, Quad.m_aTexcoords[0].y};
		int aMax[2] = {aMin[0], aMin[1]};
		for(const CPoint &Point : Quad.m_aTexcoords)
		{
			aMin[0] = std::min(aMin[0], Point.x);
			aMin[1] = std::min(aMin[1], Point.y);
			aMax[0] = std::max(aMax[0], Point.x);
			aMax[1] = std::max(aMax[1], Point.y);
		}
		// Outside the picture it repeats, so the quad may show any of it
		int aFirst[2], aLast[2];
		for(int Axis = 0; Axis < 2; Axis++)
		{
			if(aMin[Axis] < 0 || aMax[Axis] > FULL)
			{
				aFirst[Axis] = 0;
				aLast[Axis] = 15;
				continue;
			}
			aFirst[Axis] = std::min(aMin[Axis] * 16 / FULL, 15);
			aLast[Axis] = std::clamp((aMax[Axis] * 16 + FULL - 1) / FULL - 1, aFirst[Axis], 15);
		}
		for(int y = aFirst[1]; y <= aLast[1]; y++)
		{
			for(int x = aFirst[0]; x <= aLast[0]; x++)
			{
				const int Cell = y * 16 + x;
				if((Map07Tables::CHANGED[Set][Cell / 32] >> (Cell % 32)) & 1)
				{
					return true;
				}
			}
		}
	}
	return false;
}

void CMapConverter::ReadLayers()
{
	int Num;
	m_Reader.GetType(MAPITEMTYPE_LAYER, &m_LayerStart, &Num);
	m_vLayers.resize(Num);
	for(int i = 0; i < Num; i++)
	{
		CLayer &Layer = m_vLayers[i];
		const int Size = m_Reader.GetItemSize(m_LayerStart + i);
		Layer.m_vItem = CopyItem(m_Reader.GetItem(m_LayerStart + i, nullptr, &Layer.m_Id), Size);
		if(Size < (int)sizeof(CMapItemLayer))
		{
			continue;
		}
		Layer.m_Type = ItemAs<CMapItemLayer>(Layer.m_vItem)->m_Type;
		if(Layer.m_Type == LAYERTYPE_QUADS && Size >= (int)QUADS_LAYER_WITH_IMAGE)
		{
			const CMapItemLayerQuads *pQuads = ItemAs<CMapItemLayerQuads>(Layer.m_vItem, QUADS_LAYER_WITH_IMAGE);
			if(pQuads->m_NumQuads > 0 && pQuads->m_Image >= 0 && pQuads->m_Image < (int)m_vImages.size())
			{
				CImage &Image = m_vImages[pQuads->m_Image];
				Image.m_UsedByQuads = true;
				if(Image.m_pTable != nullptr && !Image.m_QuadsUseChanged)
				{
					Image.m_QuadsUseChanged = QuadsShowChangedCells(*pQuads, Image.m_pTable->m_Source);
				}
			}
			continue;
		}
		if(Layer.m_Type != LAYERTYPE_TILES || Size < (int)sizeof(CMapItemLayerTilemap_v2))
		{
			continue;
		}
		const CMapItemLayerTilemap_v2 *pTilemap = ItemAs<CMapItemLayerTilemap_v2>(Layer.m_vItem);
		if((pTilemap->m_Flags & PHYSICS_LAYER_FLAGS) != 0 || pTilemap->m_Image < 0 || pTilemap->m_Image >= (int)m_vImages.size())
		{
			continue;
		}
		CImage &Image = m_vImages[pTilemap->m_Image];
		Image.m_UsedByTiles = true;
		if(Image.m_pTable == nullptr)
		{
			continue;
		}
		if(pTilemap->m_Width <= 0 || pTilemap->m_Height <= 0 || pTilemap->m_Data < 0 || pTilemap->m_Data >= (int)m_vData.size() ||
			(size_t)pTilemap->m_Width * pTilemap->m_Height > std::numeric_limits<int>::max() / sizeof(CTile))
		{
			Warn("tile layer %d is broken and stays as it is", i);
			continue;
		}
		const size_t Count = (size_t)pTilemap->m_Width * pTilemap->m_Height;
		const CDataFileRawData &Raw = m_vData[pTilemap->m_Data];
		const std::unique_ptr<uint8_t[]> pRaw = Raw.Uncompress();
		bool Unpacked = pRaw != nullptr;
		if(Unpacked)
		{
			Layer.m_vTiles.resize(Count);
			if(pTilemap->m_Version >= 4)
			{
				Unpacked = UnpackTileskip(Layer.m_vTiles, reinterpret_cast<const CTile *>(pRaw.get()), Raw.UncompressedSize() / sizeof(CTile));
			}
			else if(Raw.UncompressedSize() >= Count * sizeof(CTile))
			{
				mem_copy(Layer.m_vTiles.data(), pRaw.get(), Count * sizeof(CTile));
			}
			else
			{
				Unpacked = false;
			}
		}
		if(!Unpacked)
		{
			Warn("tiles of layer %d cannot be read, they stay as they are", i);
			Layer.m_vTiles.clear();
			continue;
		}
		const Map07Tables::CTileTable &Table = *Image.m_pTable;
		for(const CTile &Tile : Layer.m_vTiles)
		{
			if(Tile.m_Index == 0)
			{
				continue;
			}
			Image.m_Used.set(Tile.m_Index);
		}
		for(int Index = 1; Index < 256; Index++)
		{
			if(!Image.m_Used.test(Index))
			{
				continue;
			}
			const uint8_t Kind = Table.m_aKind[Index];
			Image.m_UsesFallback |= Kind == Map07Tables::KIND_FALLBACK;
			Image.m_UsesChanged |= Kind != Map07Tables::KIND_EXACT || Table.m_aTarget[Index] != Index || Table.m_aTargetSet[Index] != Table.m_Home || Table.m_aFlagFix[Index] != 0;
		}
	}
}

bool CMapConverter::DecideImages()
{
	for(size_t i = 0; i < m_vImages.size() && m_To07 && m_Mode != EMapConvertMode::MARK; i++)
	{
		const CImage &Image = m_vImages[i];
		if(!Image.m_UsedByTiles || Image.m_External || Image.m_vItem.size() * sizeof(int32_t) < sizeof(CMapItemImage_v1))
		{
			continue;
		}
		const CMapItemImage_v1 *pItem = static_cast<const CMapItemImage_v1 *>(static_cast<const void *>(Image.m_vItem.data()));
		if(pItem->m_Width % 16 != 0 || pItem->m_Height % 16 != 0 || pItem->m_Width <= 0 || pItem->m_Height <= 0)
		{
			char aError[256];
			str_format(aError, sizeof(aError), "tiles use the image '%s' of %dx%d, which Teeworlds 0.7 cannot draw tiles of: width and height must be multiples of 16", Image.m_aName, pItem->m_Width, pItem->m_Height);
			return Fail(aError);
		}
	}

	for(CImage &Image : m_vImages)
	{
		if(!Image.m_External || m_Mode == EMapConvertMode::MARK || (!Image.m_UsedByTiles && !Image.m_UsedByQuads))
		{
			continue;
		}
		const bool Foreign = m_To07 && !IsTeeworlds07Mapres(Image.m_aName);
		if(Image.m_pTable == nullptr && !Foreign)
		{
			continue;
		}
		if(Foreign)
		{
			// A 0.7 client does not have the picture at all
			Image.m_Action = EAction::EMBED;
		}
		else
		{
			const Map07Tables::CTileTable &Table = *Image.m_pTable;
			const bool Remappable = std::any_of(std::begin(Table.m_aKind), std::end(Table.m_aKind), [](uint8_t Kind) { return Kind == Map07Tables::KIND_EXACT; });
			if(!Image.m_UsesChanged && !Image.m_QuadsUseChanged && (m_To07 || Map07Tables::SIZE06[Table.m_Source][0] != 0))
			{
				// The tiles and the parts of the picture the map uses are where they were, and the other version has the picture
				continue;
			}
			// A picture without any counterpart (easter) is embedded in every mode
			Image.m_Action = m_Mode == EMapConvertMode::EMBED || !Remappable ? EAction::EMBED : EAction::REMAP;
			// Quads show any part of the picture: with nothing to remap, embed it for them
			if(Image.m_Action == EAction::REMAP && !Image.m_UsedByTiles)
			{
				Image.m_Action = EAction::EMBED;
			}
		}

		if(Image.m_Action == EAction::EMBED || (Image.m_Action == EAction::REMAP && Image.m_QuadsUseChanged))
		{
			Image.m_pPicture = m_Mapres.Find(Image.m_aName, !m_To07);
			if(Image.m_pPicture == nullptr)
			{
				// Never remapped instead: `EMBED` changes no tile
				Warn("the picture '%s' is missing, the image stays external", Image.m_aName);
				Image.m_Action = Image.m_Action == EAction::EMBED ? EAction::KEEP : Image.m_Action;
			}
			else if(m_To07 && Image.m_UsedByTiles && (Image.m_pPicture->m_Width % 16 != 0 || Image.m_pPicture->m_Height % 16 != 0))
			{
				char aError[256];
				str_format(aError, sizeof(aError), "the picture '%s' of %dx%d is used for tiles, which Teeworlds 0.7 cannot draw: width and height must be multiples of 16", Image.m_aName, Image.m_pPicture->m_Width, Image.m_pPicture->m_Height);
				return Fail(aError);
			}
		}
		if(Image.m_Action == EAction::REMAP && Image.m_QuadsUseChanged && Image.m_pPicture != nullptr)
		{
			// Quads use the picture by its coordinates, which moving tiles does not change: they get their own copy of it
			CImage Copy = Image;
			Copy.m_Action = EAction::EMBED;
			Copy.m_pTable = nullptr;
			Copy.m_Id = -1;
			Image.m_QuadImage = m_vImages.size() + m_vNewImages.size();
			m_vNewImages.push_back(std::move(Copy));
			m_Result.m_Stats.m_QuadImages++;
		}
		if(Image.m_Action == EAction::EMBED)
		{
			m_Result.m_Stats.m_EmbeddedImages++;
		}
		else if(Image.m_Action == EAction::REMAP)
		{
			m_Result.m_Stats.m_RemappedImages++;
		}
	}

	if(m_Mode != EMapConvertMode::HYBRID)
	{
		return true;
	}
	// The diff tilesets of the tiles without a counterpart the map uses, all of them or none
	const auto &apDiffs = m_To07 ? Map07Tables::DIFFS07 : Map07Tables::DIFFS06;
	std::map<int, std::shared_ptr<const CMapresImage>> Diffs;
	for(const CImage &Image : m_vImages)
	{
		if(Image.m_Action != EAction::REMAP || !Image.m_UsesFallback || apDiffs[Image.m_pTable->m_Source] == nullptr || Diffs.contains(Image.m_pTable->m_Source))
		{
			continue;
		}
		const char *pName = apDiffs[Image.m_pTable->m_Source];
		std::shared_ptr<const CMapresImage> pDiff = m_Mapres.FindDiff(pName);
		if(pDiff == nullptr)
		{
			log_warn("mapconv", "diff tileset %s missing, using remap", pName);
			Warn("diff tileset %s missing, using remap", pName);
			m_Mode = EMapConvertMode::REMAP;
			return true;
		}
		Diffs[Image.m_pTable->m_Source] = std::move(pDiff);
	}
	for(CImage &Image : m_vImages)
	{
		const auto Found = Image.m_Action == EAction::REMAP ? Diffs.find(Image.m_pTable->m_Source) : Diffs.end();
		if(Found != Diffs.end())
		{
			Image.m_DiffImage = DiffImage(static_cast<Map07Tables::ESet>(Found->first), Found->second);
		}
	}
	return true;
}

int CMapConverter::SplitImage(Map07Tables::ESet Set)
{
	const auto Found = m_SplitImages.find(Set);
	if(Found != m_SplitImages.end())
	{
		return Found->second;
	}
	const char *pName = Map07Tables::SET_NAMES[Set];
	// An external image of that picture that stays as it is will do
	for(size_t i = 0; i < m_vImages.size(); i++)
	{
		const CImage &Image = m_vImages[i];
		if(Image.m_External && Image.m_Action == EAction::KEEP && Image.m_pTable == nullptr && str_comp(Image.m_aName, pName) == 0)
		{
			m_SplitImages[Set] = i;
			return i;
		}
	}
	CImage Image;
	Image.m_External = true;
	str_copy(Image.m_aName, pName);
	Image.m_Id = -1;
	CMapItemImage_v2 Item = {};
	Item.m_Version = 1;
	Item.m_Width = m_To07 ? Map07Tables::SIZE07[Set][0] : Map07Tables::SIZE06[Set][0];
	Item.m_Height = m_To07 ? Map07Tables::SIZE07[Set][1] : Map07Tables::SIZE06[Set][1];
	Item.m_External = 1;
	Item.m_ImageName = m_vData.size();
	Item.m_ImageData = -1;
	Item.m_MustBe1 = IMAGE_FORMAT_RGBA;
	m_vData.push_back(StringData(pName));
	Image.m_vItem = CopyItem(&Item, sizeof(CMapItemImage_v1));
	const int Index = m_vImages.size() + m_vNewImages.size();
	m_vNewImages.push_back(std::move(Image));
	m_SplitImages[Set] = Index;
	return Index;
}

int CMapConverter::DiffImage(Map07Tables::ESet Set, std::shared_ptr<const CMapresImage> pPicture)
{
	for(size_t i = 0; i < m_vNewImages.size(); i++)
	{
		if(m_vNewImages[i].m_pPicture == pPicture && m_vNewImages[i].m_pTable == nullptr)
		{
			return m_vImages.size() + i;
		}
	}
	const char *pName = m_To07 ? Map07Tables::DIFFS07[Set] : Map07Tables::DIFFS06[Set];
	CImage Image;
	Image.m_pPicture = std::move(pPicture);
	Image.m_Action = EAction::EMBED;
	Image.m_Id = -1;
	str_copy(Image.m_aName, pName);
	CMapItemImage_v2 Item = {};
	Item.m_Version = 1;
	Item.m_ImageName = m_vData.size();
	Item.m_ImageData = -1;
	Item.m_MustBe1 = IMAGE_FORMAT_RGBA;
	m_vData.push_back(StringData(pName));
	Image.m_vItem = CopyItem(&Item, sizeof(CMapItemImage_v1));
	const int Index = m_vImages.size() + m_vNewImages.size();
	m_vNewImages.push_back(std::move(Image));
	m_Result.m_Stats.m_DiffImages++;
	return Index;
}

bool CMapConverter::RemapLayer(CLayer &Layer, CImage &Image)
{
	const Map07Tables::CTileTable &Table = *Image.m_pTable;
	CMapItemLayerTilemap_v2 *pTilemap = ItemAs<CMapItemLayerTilemap_v2>(Layer.m_vItem);
	const bool FullAlpha = pTilemap->m_Color.a == 255;
	const auto &aaOpaque = m_To07 ? Map07Tables::OPAQUE07 : Map07Tables::OPAQUE06;

	std::vector<CTile> vHome(Layer.m_vTiles.size(), CTile{0, 0, 0, 0});
	std::map<int, std::vector<CTile>> Split;
	std::vector<CTile> vDiff; // The tiles without a counterpart, as they are, for the diff tileset
	std::bitset<256> Lost;
	int LostTiles = 0;
	for(size_t i = 0; i < Layer.m_vTiles.size(); i++)
	{
		const CTile &Tile = Layer.m_vTiles[i];
		if(Tile.m_Index == 0)
		{
			continue;
		}
		const uint8_t Kind = Table.m_aKind[Tile.m_Index];
		if(Kind == Map07Tables::KIND_FALLBACK)
		{
			if(Image.m_DiffImage < 0)
			{
				Lost.set(Tile.m_Index);
				LostTiles++;
				continue;
			}
			// Where it is in the diff tileset, as it was
			if(vDiff.empty())
			{
				vDiff.resize(Layer.m_vTiles.size(), CTile{0, 0, 0, 0});
			}
			vDiff[i] = CTile{Tile.m_Index, Tile.m_Flags, 0, 0};
			m_Result.m_Stats.m_DiffTiles++;
			continue;
		}
		if(Kind != Map07Tables::KIND_EXACT)
		{
			continue;
		}
		const int Set = Table.m_aTargetSet[Tile.m_Index];
		const int Target = Table.m_aTarget[Tile.m_Index];
		int Flags = ComposeTileFlags(Tile.m_Flags & TILE_ORIENTATION, Table.m_aFlagFix[Tile.m_Index]);
		if(FullAlpha && IsOpaque(aaOpaque[Set], Target))
		{
			Flags |= TILEFLAG_OPAQUE;
		}
		std::vector<CTile> &vTarget = Set == Table.m_Home ? vHome : Split[Set];
		if(vTarget.empty())
		{
			vTarget.resize(Layer.m_vTiles.size(), CTile{0, 0, 0, 0});
		}
		vTarget[i].m_Index = Target;
		vTarget[i].m_Flags = Flags;
	}
	if(LostTiles > 0)
	{
		char aIndices[256] = "";
		for(int Index = 1; Index < 256; Index++)
		{
			if(Lost.test(Index))
			{
				char aIndex[8];
				str_format(aIndex, sizeof(aIndex), "%s%d", aIndices[0] ? ", " : "", Index);
				str_append(aIndices, aIndex);
			}
		}
		Warn("%d tiles of '%s' have no counterpart and are left out (%s)", LostTiles, Image.m_aName, aIndices);
		m_Result.m_Stats.m_LostTiles += LostTiles;
	}

	int Changed = 0;
	for(size_t i = 0; i < vHome.size(); i++)
	{
		Changed += vHome[i].m_Index != Layer.m_vTiles[i].m_Index || vHome[i].m_Flags != Layer.m_vTiles[i].m_Flags;
	}
	if(Changed == 0 && Split.empty() && vDiff.empty())
	{
		return true;
	}
	m_Result.m_Stats.m_RemappedTiles += Changed;

	const int Size = Layer.m_vItem.size() * sizeof(int32_t);
	// The layers keep the version of the source, and with it tileskip or not:
	// the layers left as they are have it too, and all tile layers of a map
	// share one version (twmap refuses a map where they do not).
	const int Version = pTilemap->m_Version;
	const bool Tileskip = Version >= 4;

	// A copy of the layer, with its color, envelope and flags, for other tiles and another picture
	const auto &&CopyLayer = [&](const std::vector<CTile> &vTiles, int ImageIndex) {
		std::vector<int32_t> vItem = Layer.m_vItem;
		CMapItemLayerTilemap_v2 *pCopy = ItemAs<CMapItemLayerTilemap_v2>(vItem);
		pCopy->m_Image = ImageIndex;
		pCopy->m_Data = m_vData.size();
		if(Size >= (int)sizeof(CMapItemLayerTilemap))
		{
			CMapItemLayerTilemap *pFull = ItemAs<CMapItemLayerTilemap>(vItem);
			pFull->m_Tele = pFull->m_Speedup = pFull->m_Front = pFull->m_Switch = pFull->m_Tune = -1;
		}
		m_vData.push_back(UncompressedData(PackTiles(vTiles, Tileskip)));
		return vItem;
	};
	for(auto &[Set, vTiles] : Split)
	{
		Layer.m_vvSplitItems.push_back(CopyLayer(vTiles, SplitImage(static_cast<Map07Tables::ESet>(Set))));
		m_Result.m_Stats.m_SplitLayers++;
		for(const CTile &Tile : vTiles)
		{
			m_Result.m_Stats.m_RemappedTiles += Tile.m_Index != 0;
		}
	}
	if(!vDiff.empty())
	{
		Layer.m_vvAboveItems.push_back(CopyLayer(vDiff, Image.m_DiffImage));
		m_Result.m_Stats.m_DiffLayers++;
	}

	m_vData[pTilemap->m_Data] = UncompressedData(PackTiles(vHome, Tileskip));
	Layer.m_Changed = true;
	m_Result.m_Stats.m_RewrittenLayers++;
	return true;
}

void CMapConverter::UpdateImageItem(CImage &Image, int Index)
{
	if(Image.m_vItem.size() * sizeof(int32_t) < sizeof(CMapItemImage_v1))
	{
		return;
	}
	CMapItemImage_v2 Item = {};
	mem_copy(&Item, Image.m_vItem.data(), std::min(Image.m_vItem.size() * sizeof(int32_t), sizeof(Item)));
	if(Item.m_Version < 2 || Image.m_vItem.size() * sizeof(int32_t) < sizeof(CMapItemImage_v2))
	{
		Item.m_MustBe1 = IMAGE_FORMAT_RGBA;
	}

	if(Image.m_Action == EAction::EMBED && Image.m_pPicture != nullptr)
	{
		Item.m_External = 0;
		Item.m_Width = Image.m_pPicture->m_Width;
		Item.m_Height = Image.m_pPicture->m_Height;
		Item.m_ImageData = m_vData.size();
		Item.m_MustBe1 = IMAGE_FORMAT_RGBA;
		m_vData.push_back(Image.m_pPicture->m_Data);
	}
	else if(Image.m_RestoreSet >= 0 && (m_Mode == EMapConvertMode::REMAP || m_Mode == EMapConvertMode::HYBRID))
	{
		// A diff tileset has its tiles where the tileset has them: the tileset again, and its pixels are not needed any more
		const int Set = Image.m_RestoreSet;
		if(Item.m_ImageData >= 0 && Item.m_ImageData < (int)m_vData.size())
		{
			m_vData[Item.m_ImageData] = UncompressedData(std::vector<uint8_t>(1, 0));
		}
		Item.m_External = 1;
		Item.m_ImageData = -1;
		Item.m_Width = m_To07 ? Map07Tables::SIZE07[Set][0] : Map07Tables::SIZE06[Set][0];
		Item.m_Height = m_To07 ? Map07Tables::SIZE07[Set][1] : Map07Tables::SIZE06[Set][1];
		Item.m_MustBe1 = IMAGE_FORMAT_RGBA;
		Item.m_ImageName = m_vData.size();
		m_vData.push_back(StringData(Map07Tables::SET_NAMES[Set]));
		Image.m_Format = CImageInfo::FORMAT_RGBA;
		m_Result.m_Stats.m_RestoredImages++;
	}
	else if(Image.m_Action == EAction::REMAP && Image.m_pTable->m_Home != Image.m_pTable->m_Source)
	{
		// The tiles are all in another picture now (0.7's generic_shadows to a 0.6 tileset with the shadows)
		Item.m_ImageName = m_vData.size();
		m_vData.push_back(StringData(Map07Tables::SET_NAMES[Image.m_pTable->m_Home]));
	}

	if(m_To07 || m_Mode == EMapConvertMode::MARK)
	{
		if(Item.m_Version < 2)
		{
			m_Result.m_Stats.m_MarkedImages++;
		}
		Item.m_Version = 2;
		Image.m_vItem = CopyItem(&Item, sizeof(CMapItemImage_v2));
		return;
	}

	// DDNet 18.3 and newer do not read RGB pictures
	if(!Item.m_External && Image.m_Format == CImageInfo::FORMAT_RGB && Item.m_ImageData >= 0 && Item.m_ImageData < (int)m_vData.size())
	{
		const CDataFileRawData &Raw = m_vData[Item.m_ImageData];
		const size_t Pixels = (size_t)Item.m_Width * Item.m_Height;
		const std::unique_ptr<uint8_t[]> pRgb = Raw.Uncompress();
		if(pRgb != nullptr && Item.m_Width > 0 && Item.m_Height > 0 && Raw.UncompressedSize() >= Pixels * 3)
		{
			std::vector<uint8_t> vRgba(Pixels * 4);
			for(size_t p = 0; p < Pixels; p++)
			{
				vRgba[p * 4] = pRgb[p * 3];
				vRgba[p * 4 + 1] = pRgb[p * 3 + 1];
				vRgba[p * 4 + 2] = pRgb[p * 3 + 2];
				vRgba[p * 4 + 3] = 255;
			}
			m_vData[Item.m_ImageData] = UncompressedData(std::move(vRgba));
			m_Result.m_Stats.m_RgbImages++;
		}
		else
		{
			Warn("the RGB picture of image %d cannot be read", Index);
		}
	}
	else if(!Item.m_External && Image.m_Format == CImageInfo::FORMAT_UNDEFINED)
	{
		Warn("image %d has a format no one writes", Index);
	}
	Item.m_Version = 1;
	Image.m_vItem = CopyItem(&Item, sizeof(CMapItemImage_v1));
}

void CMapConverter::ConvertEnvelopes(std::vector<COutputItem> &vItems)
{
	int EnvStart, EnvNum;
	m_Reader.GetType(MAPITEMTYPE_ENVELOPE, &EnvStart, &EnvNum);
	if(EnvNum == 0)
	{
		return;
	}
	for(int Index = EnvStart; Index < EnvStart + EnvNum; Index++)
	{
		if(m_Reader.GetItemSize(Index) >= (int)sizeof(CMapItemEnvelope_v1) && static_cast<const CMapItemEnvelope_v1 *>(m_Reader.GetItem(Index))->m_Version >= CMapItemEnvelope::VERSION_TEEWORLDS_BEZIER)
		{
			return; // The points have the bezier tangents already
		}
	}

	int PointsStart, PointsNum;
	m_Reader.GetType(MAPITEMTYPE_ENVPOINTS, &PointsStart, &PointsNum);
	int BezierStart, BezierNum;
	m_Reader.GetType(MAPITEMTYPE_ENVPOINTS_BEZIER, &BezierStart, &BezierNum);
	const int NumPoints = PointsNum > 0 ? m_Reader.GetItemSize(PointsStart) / (int)sizeof(CEnvPoint) : 0;
	const CEnvPoint *pPoints = PointsNum > 0 ? static_cast<const CEnvPoint *>(m_Reader.GetItem(PointsStart)) : nullptr;
	const CEnvPointBezier *pBezier = nullptr;
	if(BezierNum > 0 && m_Reader.GetItemSize(BezierStart) / (int)sizeof(CEnvPointBezier) == NumPoints)
	{
		pBezier = static_cast<const CEnvPointBezier *>(m_Reader.GetItem(BezierStart));
	}

	for(COutputItem &Item : vItems)
	{
		if(Item.m_Type == MAPITEMTYPE_ENVELOPE && Item.m_Size >= (int)sizeof(CMapItemEnvelope_v1))
		{
			std::vector<int32_t> vEnvelope = CopyItem(Item.m_pData, Item.m_Size);
			if(vEnvelope.size() * sizeof(int32_t) < sizeof(CMapItemEnvelope_v2))
			{
				// Version 1 envelopes are synchronized
				vEnvelope.resize(sizeof(CMapItemEnvelope_v2) / sizeof(int32_t));
				ItemAs<CMapItemEnvelope_v2>(vEnvelope)->m_Synchronized = 1;
			}
			ItemAs<CMapItemEnvelope_v2>(vEnvelope)->m_Version = CMapItemEnvelope::VERSION_TEEWORLDS_BEZIER;
			Item.m_vOwned = std::move(vEnvelope);
			Item.m_pData = Item.m_vOwned.data();
			Item.m_Size = Item.m_vOwned.size() * sizeof(int32_t);
			m_Result.m_Stats.m_Envelopes++;
		}
		else if(Item.m_Type == MAPITEMTYPE_ENVPOINTS && Item.m_pData == pPoints && pPoints != nullptr)
		{
			std::vector<CEnvPointBezier_upstream> vUpstream(NumPoints);
			for(int i = 0; i < NumPoints; i++)
			{
				static_cast<CEnvPoint &>(vUpstream[i]) = pPoints[i];
				// Without tangents a bezier curve is linear, as DDNet draws it
				vUpstream[i].m_Bezier = pBezier != nullptr ? pBezier[i] : CEnvPointBezier{};
			}
			Item.m_vOwned = CopyItem(vUpstream.data(), vUpstream.size() * sizeof(CEnvPointBezier_upstream));
			Item.m_pData = Item.m_vOwned.data();
			Item.m_Size = Item.m_vOwned.size() * sizeof(int32_t);
		}
	}
	// The tangents are in the points now
	vItems.erase(std::remove_if(vItems.begin(), vItems.end(), [](const COutputItem &Item) { return Item.m_Type == MAPITEMTYPE_ENVPOINTS_BEZIER; }), vItems.end());
}

bool CMapConverter::Write()
{
	// Layers: split layers come right before the layer they came from, those of the tiles without a counterpart right after it
	std::vector<int> vInsertedBefore(m_vLayers.size() + 1, 0); // layers added before layer i, summed up
	for(size_t i = 0; i < m_vLayers.size(); i++)
	{
		vInsertedBefore[i + 1] = vInsertedBefore[i] + m_vLayers[i].m_vvSplitItems.size() + m_vLayers[i].m_vvAboveItems.size();
	}
	const auto &&NewLayerIndex = [&](int Old) {
		// The first of the layers that stand where the old one stood
		return Old + vInsertedBefore[std::clamp(Old, 0, (int)m_vLayers.size())];
	};

	int ProvenanceIndex = FindProvenanceItem(m_Reader);
	std::vector<COutputItem> vItems;
	vItems.reserve(m_Reader.NumItems() + m_vNewImages.size() + vInsertedBefore.back() + 1);
	int NextLayerId = 0;
	int MaxImageId = -1;
	for(const CImage &Image : m_vImages)
	{
		MaxImageId = std::max(MaxImageId, Image.m_Id);
	}
	for(int Index = 0; Index < m_Reader.NumItems(); Index++)
	{
		int Type, Id;
		CUuid Uuid;
		const void *pData = m_Reader.GetItem(Index, &Type, &Id, &Uuid);
		const int Size = m_Reader.GetItemSize(Index);
		if(Type == ITEMTYPE_EX || Index == ProvenanceIndex)
		{
			continue; // The writer adds the UUIDs again; the provenance is written anew
		}
		if(Type == UUID_UNKNOWN)
		{
			Type = -1;
		}
		COutputItem Item{Type, Id, Uuid, pData, Size, {}};
		if(Type == MAPITEMTYPE_IMAGE && Index >= m_ImageStart && Index < m_ImageStart + (int)m_vImages.size())
		{
			CImage &Image = m_vImages[Index - m_ImageStart];
			Item.m_vOwned = Image.m_vItem;
			vItems.push_back(std::move(Item));
			// New images come after the last one of the source
			if(Index == m_ImageStart + (int)m_vImages.size() - 1)
			{
				for(CImage &New : m_vNewImages)
				{
					vItems.push_back(COutputItem{MAPITEMTYPE_IMAGE, ++MaxImageId, CUuid(), nullptr, 0, New.m_vItem});
				}
			}
			continue;
		}
		if(Type == MAPITEMTYPE_LAYER && Index >= m_LayerStart && Index < m_LayerStart + (int)m_vLayers.size())
		{
			CLayer &Layer = m_vLayers[Index - m_LayerStart];
			for(std::vector<int32_t> &vSplit : Layer.m_vvSplitItems)
			{
				vItems.push_back(COutputItem{MAPITEMTYPE_LAYER, NextLayerId++, CUuid(), nullptr, 0, std::move(vSplit)});
			}
			Item.m_Id = NextLayerId++;
			Item.m_vOwned = std::move(Layer.m_vItem);
			vItems.push_back(std::move(Item));
			for(std::vector<int32_t> &vAbove : Layer.m_vvAboveItems)
			{
				vItems.push_back(COutputItem{MAPITEMTYPE_LAYER, NextLayerId++, CUuid(), nullptr, 0, std::move(vAbove)});
			}
			continue;
		}
		if(Type == MAPITEMTYPE_GROUP && Size >= (int)sizeof(CMapItemGroup_v1) && vInsertedBefore.back() > 0)
		{
			Item.m_vOwned = CopyItem(pData, Size);
			CMapItemGroup_v1 *pGroup = ItemAs<CMapItemGroup_v1>(Item.m_vOwned);
			const int Start = pGroup->m_StartLayer;
			const int End = Start + std::max(pGroup->m_NumLayers, 0);
			pGroup->m_StartLayer = NewLayerIndex(Start);
			pGroup->m_NumLayers = NewLayerIndex(End) - pGroup->m_StartLayer;
			vItems.push_back(std::move(Item));
			continue;
		}
		if(Type == MAPITEMTYPE_AUTOMAPPER_CONFIG && Size >= (int)sizeof(CMapItemAutomapperConfig) && vInsertedBefore.back() > 0)
		{
			// The automapper names its layer by its place in the group
			Item.m_vOwned = CopyItem(pData, Size);
			CMapItemAutomapperConfig *pConfig = ItemAs<CMapItemAutomapperConfig>(Item.m_vOwned);
			int GroupStart, GroupNum;
			m_Reader.GetType(MAPITEMTYPE_GROUP, &GroupStart, &GroupNum);
			if(pConfig->m_GroupId >= 0 && pConfig->m_GroupId < GroupNum && m_Reader.GetItemSize(GroupStart + pConfig->m_GroupId) >= (int)sizeof(CMapItemGroup_v1))
			{
				const CMapItemGroup_v1 *pGroup = static_cast<const CMapItemGroup_v1 *>(m_Reader.GetItem(GroupStart + pConfig->m_GroupId));
				const int Old = pGroup->m_StartLayer + pConfig->m_LayerId;
				if(Old >= 0 && Old < (int)m_vLayers.size())
				{
					pConfig->m_LayerId = NewLayerIndex(Old) + m_vLayers[Old].m_vvSplitItems.size() - NewLayerIndex(pGroup->m_StartLayer);
				}
			}
			vItems.push_back(std::move(Item));
			continue;
		}
		vItems.push_back(std::move(Item));
	}
	if(m_vImages.empty())
	{
		for(CImage &New : m_vNewImages)
		{
			vItems.push_back(COutputItem{MAPITEMTYPE_IMAGE, ++MaxImageId, CUuid(), nullptr, 0, New.m_vItem});
		}
	}
	if(m_To07 && m_Mode != EMapConvertMode::MARK)
	{
		ConvertEnvelopes(vItems);
	}

	// Where the map came from
	CMapItemConverted Provenance;
	Provenance.m_Version = CMapConvertProvenance::CURRENT_VERSION;
	Provenance.m_Direction = m_To07 ? 7 : 6;
	Provenance.m_Mode = static_cast<int>(m_Mode);
	const SHA256_DIGEST SourceSha256 = m_Reader.Sha256();
	for(int i = 0; i < 8; i++)
	{
		Provenance.m_aSourceSha256[i] = bytes_be_to_uint(&SourceSha256.data[i * 4]);
	}
	Provenance.m_SourceCrc = m_Reader.Crc();
	Provenance.m_SourceSize = m_Reader.Size();
	Provenance.m_TableVersion = Map07Tables::VERSION;
	Provenance.m_TableChecksum = Map07Tables::CHECKSUM;
	Provenance.m_ConverterVersion = CONVERTER_VERSION;
	vItems.push_back(COutputItem{-1, 0, MapConvertProvenanceUuid(), nullptr, 0, CopyItem(&Provenance, sizeof(Provenance))});

	if(m_vImages.size() + m_vNewImages.size() > MAX_MAPIMAGES)
	{
		char aError[128];
		str_format(aError, sizeof(aError), "the map would have %d images, more than the %d a client loads", (int)(m_vImages.size() + m_vNewImages.size()), (int)MAX_MAPIMAGES);
		return Fail(aError);
	}

	CDataFileWriter Writer;
	for(const COutputItem &Item : vItems)
	{
		const void *pData = Item.m_vOwned.empty() && Item.m_pData != nullptr ? Item.m_pData : Item.m_vOwned.data();
		const int Size = Item.m_vOwned.empty() && Item.m_pData != nullptr ? Item.m_Size : (int)(Item.m_vOwned.size() * sizeof(int32_t));
		Writer.AddItem(Item.m_Type, Item.m_Id, Size, Size > 0 ? pData : nullptr, &Item.m_Uuid);
	}
	for(CDataFileRawData &Data : m_vData)
	{
		m_Result.m_Stats.m_PassedData += Data.Compressed();
		Writer.AddRawData(std::move(Data));
	}
	m_vData.clear();
	m_Result.m_vData = Writer.FinishToMemory();

	const size_t Limit = m_Options.m_MaxOutputSize != 0 ? m_Options.m_MaxOutputSize : (size_t)m_Reader.Size() * 4 + 32 * 1024 * 1024;
	if(m_Result.m_vData.size() > Limit)
	{
		char aError[128];
		str_format(aError, sizeof(aError), "the converted map would have %" PRIzu " bytes, more than the limit of %" PRIzu, m_Result.m_vData.size(), Limit);
		return Fail(aError);
	}
	m_Result.m_Sha256 = sha256(m_Result.m_vData.data(), m_Result.m_vData.size());
	m_Result.m_Crc = crc32(0, m_Result.m_vData.data(), m_Result.m_vData.size());
	m_Result.m_Converted = true;
	m_Result.m_Mode = m_Mode;
	return true;
}

bool CMapConverter::Prepare()
{
	// Never twice in the same direction: a map in the form asked for stays as it is
	CMapConvertProvenance Provenance;
	const bool Converted = ReadMapConvertProvenance(m_Reader, Provenance);
	const bool Is07 = IsTeeworlds07Map(m_Reader);
	if(m_To07 ? (Is07 || (Converted && Provenance.m_Direction == EMapConvertDirection::TO07)) : !Is07)
	{
		return false;
	}
	ReadData();
	ReadImages();
	ReadLayers();
	return true;
}

bool CMapConverter::Needed() const
{
	if(m_Mode == EMapConvertMode::MARK)
	{
		return true;
	}
	for(const CImage &Image : m_vImages)
	{
		if(!Image.m_External)
		{
			// DDNet 18.3 and newer do not read RGB pictures
			if(!m_To07 && Image.m_Format == CImageInfo::FORMAT_RGB)
			{
				return true;
			}
			continue;
		}
		const bool Used = Image.m_UsedByTiles || Image.m_UsedByQuads;
		if(m_To07 && Used && !IsTeeworlds07Mapres(Image.m_aName))
		{
			return true; // A 0.7 client does not have the picture
		}
		if(!m_To07 && Used && (str_comp(Image.m_aName, "easter") == 0 || str_comp(Image.m_aName, "generic_shadows") == 0))
		{
			return true; // DDNet has these pictures only as 0.7 ones
		}
		if(Image.m_pTable != nullptr && Image.m_UsesChanged)
		{
			return true; // A tile the map uses is somewhere else in the other version
		}
		if(Image.m_pTable != nullptr && Image.m_QuadsUseChanged)
		{
			return true; // Quads show a part of the picture that looks different in the other version
		}
	}
	return false;
}

bool CMapConverter::Run()
{
	if(!Prepare() || !Needed())
	{
		return true;
	}
	if(!DecideImages())
	{
		return false;
	}
	for(CLayer &Layer : m_vLayers)
	{
		if(Layer.m_vTiles.empty())
		{
			continue;
		}
		CImage &Image = m_vImages[ItemAs<CMapItemLayerTilemap_v2>(Layer.m_vItem)->m_Image];
		if(Image.m_Action == EAction::REMAP && !RemapLayer(Layer, Image))
		{
			return false;
		}
		Layer.m_vTiles.clear();
		Layer.m_vTiles.shrink_to_fit();
	}
	for(CLayer &Layer : m_vLayers)
	{
		if(Layer.m_Type == LAYERTYPE_QUADS && Layer.m_vItem.size() * sizeof(int32_t) >= QUADS_LAYER_WITH_IMAGE)
		{
			CMapItemLayerQuads *pQuads = ItemAs<CMapItemLayerQuads>(Layer.m_vItem, QUADS_LAYER_WITH_IMAGE);
			if(pQuads->m_Image >= 0 && pQuads->m_Image < (int)m_vImages.size() && m_vImages[pQuads->m_Image].m_QuadImage >= 0)
			{
				pQuads->m_Image = m_vImages[pQuads->m_Image].m_QuadImage;
			}
		}
	}
	for(size_t i = 0; i < m_vImages.size(); i++)
	{
		UpdateImageItem(m_vImages[i], i);
	}
	for(size_t i = 0; i < m_vNewImages.size(); i++)
	{
		UpdateImageItem(m_vNewImages[i], m_vImages.size() + i);
	}
	return Write();
}

// Stands in for the pictures where none are needed
class CNoMapres : public IMapres
{
public:
	std::shared_ptr<const CMapresImage> Find(const char *pName, bool Teeworlds07) const override { return nullptr; }
	std::shared_ptr<const CMapresImage> FindDiff(const char *pName) const override { return nullptr; }
};

bool MapNeedsConversion(CDataFileReader &Source, EMapConvertDirection Direction)
{
	CMapConvertOptions Options;
	Options.m_Direction = Direction;
	CMapConvertResult Result;
	const CNoMapres NoMapres;
	CMapConverter Converter(Source, Options, NoMapres, Result);
	return Converter.Prepare() && Converter.Needed();
}

bool ConvertMap(CDataFileReader &Source, const CMapConvertOptions &Options, const IMapres &Mapres, CMapConvertResult &Result)
{
	Result = CMapConvertResult();
	CMapConverter Converter(Source, Options, Mapres, Result);
	return Converter.Run();
}
