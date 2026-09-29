#include "mapres.h"

#include <base/log.h>
#include <base/str.h>

#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/storage.h>

#include <game/mapitems.h>

#include <zlib.h>

#include <cstdlib>
#include <vector>

std::shared_ptr<const CMapresImage> CMapresFromStorage::Load(IStorage *pStorage, const char *pPath)
{
	void *pFileData;
	unsigned FileSize;
	if(!pStorage->ReadFile(pPath, IStorage::TYPE_ALL, &pFileData, &FileSize))
	{
		return nullptr;
	}
	CImageInfo Image;
	int PngliteIncompatible;
	CByteBufferReader Reader(static_cast<const uint8_t *>(pFileData), FileSize);
	const bool Loaded = CImageLoader::LoadPng(Reader, pPath, Image, PngliteIncompatible);
	free(pFileData);
	if(!Loaded)
	{
		log_error("mapconv", "could not read the picture '%s'", pPath);
		return nullptr;
	}
	ConvertToRgba(Image); // returns whether it was RGBA already

	auto pImage = std::make_shared<CMapresImage>();
	pImage->m_Width = Image.m_Width;
	pImage->m_Height = Image.m_Height;
	const size_t TileWidth = Image.m_Width / 16;
	const size_t TileHeight = Image.m_Height / 16;
	if(TileWidth > 0 && TileHeight > 0)
	{
		for(int Index = 0; Index < 256; Index++)
		{
			// As the editor decides it for the OPAQUE tile flag
			bool Opaque = true;
			for(size_t y = 0; y < TileHeight && Opaque; y++)
			{
				const uint8_t *pRow = Image.m_pData + (((Index / 16) * TileHeight + y) * Image.m_Width + (Index % 16) * TileWidth) * 4;
				for(size_t x = 0; x < TileWidth; x++)
				{
					if(pRow[x * 4 + 3] < 250)
					{
						Opaque = false;
						break;
					}
				}
			}
			if(Opaque)
			{
				pImage->m_aOpaque[Index / 32] |= 1u << (Index % 32);
			}
		}
	}

	// Compressed the way a map writer compresses it
	const size_t Size = Image.DataSize();
	uLongf CompressedSize = compressBound(Size);
	std::vector<uint8_t> vCompressed(CompressedSize);
	const int Result = compress2(vCompressed.data(), &CompressedSize, Image.m_pData, Size, Z_DEFAULT_COMPRESSION);
	Image.Free();
	if(Result != Z_OK)
	{
		log_error("mapconv", "could not compress the picture '%s'", pPath);
		return nullptr;
	}
	vCompressed.resize(CompressedSize);
	vCompressed.shrink_to_fit();
	auto pBuffer = std::make_shared<const std::vector<uint8_t>>(std::move(vCompressed));
	pImage->m_Data = CDataFileRawData(pBuffer, std::span<const uint8_t>(*pBuffer), Size, true);
	return pImage;
}

std::shared_ptr<const CMapresImage> CMapresFromStorage::Find(const char *pName, bool Teeworlds07) const
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "mapres/%s%s.png", pName, Teeworlds07 && IsMapImageRedrawnFor07(pName) ? "_0.7" : "");
	return FindPath(aPath);
}

std::shared_ptr<const CMapresImage> CMapresFromStorage::FindDiff(const char *pName) const
{
	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "convert/%s.png", pName);
	return FindPath(aPath);
}

std::shared_ptr<const CMapresImage> CMapresFromStorage::FindPath(const char *pPath) const
{
	const CLockScope Lock(m_Mutex);
	const auto Found = m_Cache.find(pPath);
	if(Found != m_Cache.end())
	{
		return Found->second;
	}
	std::shared_ptr<const CMapresImage> pImage = Load(m_pStorage, pPath);
	m_Cache.emplace(pPath, pImage);
	return pImage;
}
