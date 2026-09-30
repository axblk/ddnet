#include "map_conversion.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/convert/mapres.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>

bool ParseServerMapConvert(const char *pSetting, std::optional<EMapConvertMode> &Mode)
{
	if(str_comp(pSetting, "off") == 0)
	{
		Mode = std::nullopt;
		return true;
	}
	EMapConvertMode Parsed;
	if(!ParseMapConvertMode(pSetting, Parsed) || Parsed == EMapConvertMode::MARK)
	{
		return false;
	}
	Mode = Parsed;
	return true;
}

CServedMaps PlanServedMaps(bool Teeworlds07Map, bool HasMaps7, bool Convert)
{
	CServedMaps Served;
	if(Teeworlds07Map)
	{
		Served.m_DDNet = Convert ? EServedMap::CONVERTED : EServedMap::ORIGINAL;
		Served.m_Teeworlds07 = HasMaps7 ? EServedMap::MAPS7 : EServedMap::ORIGINAL;
	}
	else
	{
		Served.m_DDNet = EServedMap::ORIGINAL;
		Served.m_Teeworlds07 = HasMaps7 ? EServedMap::MAPS7 : Convert ? EServedMap::CONVERTED :
										EServedMap::NONE;
	}
	return Served;
}

bool Teeworlds07AcceptsMap(const char *pName, unsigned Crc, unsigned Size)
{
	// The maps Teeworlds 0.7 checks, from its `mapversions.h`
	class CStandardMap
	{
	public:
		const char *m_pName;
		unsigned m_Crc;
		unsigned m_Size;
	};
	static constexpr CStandardMap STANDARD_MAPS[] = {
		{"ctf1", 0x22bf58cc, 5731},
		{"ctf2", 0x4a5ee326, 24440},
		{"ctf3", 0xa3739d41, 5903},
		{"ctf4", 0xbe7c4db9, 12030},
		{"ctf5", 0x3a1694a7, 10250},
		{"ctf5", 0x20b97905, 10292},
		{"ctf6", 0x28c84351, 26927},
		{"ctf7", 0xec97bf07, 6454},
		{"ctf8", 0x68e543d2, 16091},
		{"dm1", 0x64548818, 6793},
		{"dm2", 0x054b1259, 9601},
		{"dm3", 0x77523f3e, 75957},
		{"dm6", 0x474da235, 7829},
		{"dm7", 0xa0092906, 10910},
		{"dm7", 0xce1484ea, 9685},
		{"dm8", 0x18396d44, 70301},
		{"dm9", 0x51649e61, 8587},
		{"lms1", 0xb2714ade, 11571},
	};
	bool Standard = false;
	for(const CStandardMap &Map : STANDARD_MAPS)
	{
		if(str_comp(Map.m_pName, pName) != 0)
			continue;
		if(Map.m_Crc == Crc && Map.m_Size == Size)
			return true;
		Standard = true;
	}
	return !Standard;
}

bool Teeworlds07MapName(char *pBuffer, size_t BufferSize, const char *pName, unsigned Crc, unsigned Size)
{
	if(Teeworlds07AcceptsMap(pName, Crc, Size))
	{
		str_copy(pBuffer, pName, BufferSize);
		return true;
	}
	str_format(pBuffer, BufferSize, "%s_ddnet", pName);
	return false;
}

bool MapDataNeedsConversion(const char *pMapName, const std::vector<uint8_t> &vData, EMapConvertDirection Direction)
{
	CDataFileReader Reader;
	return Reader.OpenFromMemory(pMapName, vData, pMapName) && MapNeedsConversion(Reader, Direction);
}

bool CMapConversion::IsFor(const SHA256_DIGEST &SourceSha256, EMapConvertDirection Direction, EMapConvertMode Mode) const
{
	return m_SourceSha256 == SourceSha256 && m_Direction == Direction && m_Mode == Mode;
}

void MapConversionCachePath(char *pBuffer, size_t BufferSize, const char *pFolder, const char *pMapName, const SHA256_DIGEST &SourceSha256, EMapConvertDirection Direction, EMapConvertMode Mode)
{
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(SourceSha256, aSha256, sizeof(aSha256));
	str_format(pBuffer, BufferSize, "%s/%s/%s_%s_%s_%08x.map", pFolder, MapConvertDirectionName(Direction), fs_filename(pMapName), aSha256, MapConvertModeName(Mode), MapConvertVersion());
}

// A converted map from the cache, if it is there and is what its name says
static bool ReadCachedMap(const CMapConversionRequest &Request, const char *pPath, const SHA256_DIGEST &SourceSha256, CMapConversion &Conversion)
{
	void *pData;
	unsigned Size;
	if(!Request.m_pCacheStorage->ReadFile(pPath, IStorage::TYPE_SAVE, &pData, &Size))
	{
		return false;
	}
	std::vector<uint8_t> vData(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
	free(pData);
	CDataFileReader Reader;
	CMapConvertProvenance Provenance;
	if(!Reader.OpenFromMemory(fs_filename(pPath), vData, pPath) ||
		!ReadMapConvertProvenance(Reader, Provenance) ||
		Provenance.m_SourceSha256 != SourceSha256 ||
		Provenance.m_Direction != Request.m_Options.m_Direction)
	{
		log_warn("mapconv", "ignoring %s, which is not the map it should be", pPath);
		return false;
	}
	Conversion.m_Result.m_Converted = true;
	Conversion.m_Result.m_Mode = Provenance.m_Mode;
	Conversion.m_Result.m_Sha256 = Reader.Sha256();
	Conversion.m_Result.m_Crc = Reader.Crc();
	Reader.Close();
	Conversion.m_Result.m_vData = std::move(vData);
	return true;
}

class CCachedFile
{
public:
	std::string m_Path;
	time_t m_Modified;
	int64_t m_Size;
};

static void ListCachedMaps(IStorage *pStorage, const char *pFolder, std::vector<CCachedFile> &vFiles)
{
	class CListing
	{
	public:
		IStorage *m_pStorage;
		const char *m_pFolder;
		std::vector<CCachedFile> *m_pvFiles;
	};
	CListing Listing = {pStorage, pFolder, &vFiles};
	pStorage->ListDirectoryInfo(
		IStorage::TYPE_SAVE, pFolder, [](const CFsFileInfo *pInfo, int IsDir, int StorageType, void *pUser) {
			const CListing *pListing = static_cast<const CListing *>(pUser);
			if(IsDir || !str_endswith(pInfo->m_pName, ".map"))
			{
				return 0;
			}
			char aPath[IO_MAX_PATH_LENGTH];
			str_format(aPath, sizeof(aPath), "%s/%s", pListing->m_pFolder, pInfo->m_pName);
			IOHANDLE File = pListing->m_pStorage->OpenFile(aPath, IOFLAG_READ, IStorage::TYPE_SAVE);
			if(File)
			{
				pListing->m_pvFiles->push_back(CCachedFile{aPath, pInfo->m_TimeModified, io_length(File)});
				io_close(File);
			}
			return 0;
		},
		&Listing);
}

// Makes room for a new map, the oldest go first
static void TrimCache(const CMapConversionRequest &Request, const char *pKeep)
{
	if(Request.m_CacheMaxSize <= 0)
	{
		return;
	}
	std::vector<CCachedFile> vFiles;
	for(EMapConvertDirection Direction : {EMapConvertDirection::TO07, EMapConvertDirection::TO06})
	{
		char aFolder[IO_MAX_PATH_LENGTH];
		str_format(aFolder, sizeof(aFolder), "%s/%s", Request.m_CacheFolder.c_str(), MapConvertDirectionName(Direction));
		ListCachedMaps(Request.m_pCacheStorage, aFolder, vFiles);
	}
	int64_t Size = 0;
	for(const CCachedFile &File : vFiles)
	{
		Size += File.m_Size;
	}
	std::sort(vFiles.begin(), vFiles.end(), [](const CCachedFile &A, const CCachedFile &B) { return A.m_Modified < B.m_Modified; });
	for(const CCachedFile &File : vFiles)
	{
		if(Size <= Request.m_CacheMaxSize)
		{
			break;
		}
		if(File.m_Path == pKeep)
		{
			continue;
		}
		if(Request.m_pCacheStorage->RemoveFile(File.m_Path.c_str(), IStorage::TYPE_SAVE))
		{
			Size -= File.m_Size;
			log_info("mapconv", "removed %s from the cache", File.m_Path.c_str());
		}
	}
}

static bool WriteCachedMap(const CMapConversionRequest &Request, const char *pPath, const std::vector<uint8_t> &vData)
{
	IStorage *pStorage = Request.m_pCacheStorage;
	char aFolder[IO_MAX_PATH_LENGTH];
	str_format(aFolder, sizeof(aFolder), "%s/%s", Request.m_CacheFolder.c_str(), MapConvertDirectionName(Request.m_Options.m_Direction));
	if(!pStorage->CreateFolder(Request.m_CacheFolder.c_str(), IStorage::TYPE_SAVE) || !pStorage->CreateFolder(aFolder, IStorage::TYPE_SAVE))
	{
		log_error("mapconv", "could not create the cache folder %s", aFolder);
		return false;
	}
	// Written under another name first, so that no one reads half a map
	char aTempPath[IO_MAX_PATH_LENGTH];
	str_format(aTempPath, sizeof(aTempPath), "%s.tmp", pPath);
	IOHANDLE File = pStorage->OpenFile(aTempPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
	{
		log_error("mapconv", "could not write %s", aTempPath);
		return false;
	}
	const bool Written = io_write(File, vData.data(), vData.size()) == vData.size();
	const bool Closed = io_close(File) == 0;
	if(!Written || !Closed || !pStorage->RenameFile(aTempPath, pPath, IStorage::TYPE_SAVE))
	{
		log_error("mapconv", "could not write %s", pPath);
		pStorage->RemoveFile(aTempPath, IStorage::TYPE_SAVE);
		return false;
	}
	TrimCache(Request, pPath);
	return true;
}

std::shared_ptr<CMapConversion> ConvertServedMap(CMapConversionRequest Request, const std::function<bool()> &Aborted)
{
	// On a thread of its own, where `time_get` is not updated
	const std::chrono::nanoseconds Start = time_get_nanoseconds();
	auto pConversion = std::make_shared<CMapConversion>();
	pConversion->m_Direction = Request.m_Options.m_Direction;
	pConversion->m_Mode = Request.m_Options.m_Mode;
	const auto &&Finish = [&]() {
		pConversion->m_DurationUs = std::chrono::duration_cast<std::chrono::microseconds>(time_get_nanoseconds() - Start).count();
		return pConversion;
	};

	CDataFileReader Reader;
	if(!Reader.OpenFromMemory(Request.m_MapName.c_str(), std::move(Request.m_vSource), Request.m_MapName.c_str()))
	{
		pConversion->m_Result.m_Error = "the map cannot be read";
		return Finish();
	}
	pConversion->m_SourceSha256 = Reader.Sha256();

	char aCachePath[IO_MAX_PATH_LENGTH] = "";
	if(Request.m_pCacheStorage != nullptr)
	{
		MapConversionCachePath(aCachePath, sizeof(aCachePath), Request.m_CacheFolder.c_str(), Request.m_MapName.c_str(), pConversion->m_SourceSha256, Request.m_Options.m_Direction, Request.m_Options.m_Mode);
		if(ReadCachedMap(Request, aCachePath, pConversion->m_SourceSha256, *pConversion))
		{
			pConversion->m_Ok = true;
			pConversion->m_FromCache = true;
			return Finish();
		}
	}

	if(Aborted && Aborted())
	{
		return nullptr;
	}
	pConversion->m_Ok = ConvertMap(Reader, Request.m_Options, *Request.m_pMapres, pConversion->m_Result);
	Reader.Close();
	if(!pConversion->m_Ok)
	{
		pConversion->m_Result.m_Converted = false;
		pConversion->m_Result.m_vData.clear();
		return Finish();
	}

	if(pConversion->m_Result.m_Converted && pConversion->m_Result.m_Mode == Request.m_Options.m_Mode && aCachePath[0] != '\0')
	{
		if(Aborted && Aborted())
		{
			return nullptr;
		}
		pConversion->m_Cached = WriteCachedMap(Request, aCachePath, pConversion->m_Result.m_vData);
	}
	return Finish();
}

CMapConversionJob::CMapConversionJob(CMapConversionRequest Request) :
	m_Request(std::move(Request))
{
	Abortable(true);
}

void CMapConversionJob::Run()
{
	// Whoever aborts the job before this sees that it started, or the job
	// sees that it was aborted
	m_Started = true;
	const auto &&Aborted = [this]() { return State() == IJob::STATE_ABORTED; };
	if(!Aborted())
	{
		m_pConversion = ConvertServedMap(std::move(m_Request), Aborted);
	}
	m_Request.m_pMapres = nullptr;
	m_Request.m_pCacheStorage = nullptr;
	m_Finished = true;
}
