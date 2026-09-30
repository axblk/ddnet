#include "mapres.h"

#include "map_convert.h"

#include <base/log.h>
#include <base/str.h>

#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/storage.h>

#include <game/mapitems.h>

#include <zlib.h>

#include <cstdlib>
#include <vector>

std::shared_ptr<const CMapresImage> CMapresFromFiles::Load(const std::vector<uint8_t> &vData, const char *pPath)
{
	CImageInfo Image;
	int PngliteIncompatible;
	CByteBufferReader Reader(vData.data(), vData.size());
	const bool Loaded = CImageLoader::LoadPng(Reader, pPath, Image, PngliteIncompatible);
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

static void ImagePath(const char *pName, bool Teeworlds07, char *pPath, size_t PathSize)
{
	str_format(pPath, PathSize, "mapres/%s%s.png", pName, Teeworlds07 && IsMapImageRedrawnFor07(pName) ? "_0.7" : "");
}

static void DiffPath(const char *pName, char *pPath, size_t PathSize)
{
	str_format(pPath, PathSize, "convert/%s.png", pName);
}

std::shared_ptr<const CMapresImage> CMapresFromFiles::Find(const char *pName, bool Teeworlds07) const
{
	char aPath[IO_MAX_PATH_LENGTH];
	ImagePath(pName, Teeworlds07, aPath, sizeof(aPath));
	return FindPath(aPath);
}

std::shared_ptr<const CMapresImage> CMapresFromFiles::FindDiff(const char *pName) const
{
	char aPath[IO_MAX_PATH_LENGTH];
	DiffPath(pName, aPath, sizeof(aPath));
	return FindPath(aPath);
}

std::vector<std::string> CMapresFromFiles::Paths(const CMapConvertPictures &Pictures)
{
	std::vector<std::string> vPaths;
	char aPath[IO_MAX_PATH_LENGTH];
	for(const std::string &Name : Pictures.m_vImages)
	{
		ImagePath(Name.c_str(), Pictures.m_Teeworlds07, aPath, sizeof(aPath));
		vPaths.emplace_back(aPath);
	}
	for(const std::string &Name : Pictures.m_vDiffs)
	{
		DiffPath(Name.c_str(), aPath, sizeof(aPath));
		vPaths.emplace_back(aPath);
	}
	return vPaths;
}

std::shared_ptr<const CMapresImage> CMapresFromFiles::FindPath(const char *pPath) const
{
	const CLockScope Lock(m_Mutex);
	const auto Found = m_Cache.find(pPath);
	if(Found != m_Cache.end())
	{
		return Found->second;
	}
	std::vector<uint8_t> vData;
	std::shared_ptr<const CMapresImage> pImage = m_ReadFile(pPath, vData) ? Load(vData, pPath) : nullptr;
	m_Cache.emplace(pPath, pImage);
	return pImage;
}

CMapresFromStorage::CMapresFromStorage(IStorage *pStorage) :
	CMapresFromFiles([pStorage](const char *pPath, std::vector<uint8_t> &vData) {
		void *pData;
		unsigned Size;
		if(!pStorage->ReadFile(pPath, IStorage::TYPE_ALL, &pData, &Size))
		{
			return false;
		}
		vData.assign(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
		free(pData);
		return true;
	})
{
}
