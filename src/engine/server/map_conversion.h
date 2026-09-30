#ifndef ENGINE_SERVER_MAP_CONVERSION_H
#define ENGINE_SERVER_MAP_CONVERSION_H

#include <base/hash.h>

#include <engine/shared/jobs.h>

#include <game/map/convert/map_convert.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class IMapres;
class IStorage;

/**
 * Reads `sv_map_convert`.
 *
 * @param pSetting Its value: `hybrid`, `remap`, `embed` or `off`.
 * @param Mode Set to the mode maps are converted with, `std::nullopt` for `off`.
 *
 * @return `false` for a value that is none of these.
 */
bool ParseServerMapConvert(const char *pSetting, std::optional<EMapConvertMode> &Mode);

/**
 * Which version of the map the server loaded the clients of one kind get.
 */
enum class EServedMap
{
	/** The map file itself. */
	ORIGINAL,
	/** Its version in `maps7/`, for Teeworlds 0.7 clients. */
	MAPS7,
	/**
	 * What converting it for them gives, which is the map file itself if it
	 * needs no converting (`MapNeedsConversion`).
	 */
	CONVERTED,
	/** None: Teeworlds 0.7 clients cannot play the map. */
	NONE,
};

/**
 * What each kind of client gets of a map.
 */
class CServedMaps
{
public:
	/** What DDNet clients get, which covers every client of the 0.6 protocol. */
	EServedMap m_DDNet;
	/** What Teeworlds 0.7 clients get. */
	EServedMap m_Teeworlds07;
};

/**
 * Decides what each kind of client gets of a map, by its contents: a map
 * Teeworlds 0.7 wrote is converted for DDNet clients, any other one for 0.7
 * clients, unless `maps7/` has a version of it. Whether it really has to be
 * converted is found out next, see `MapDataNeedsConversion`.
 *
 * Every DDNet client gets the same map: a DDNet client does not tell the
 * server whether it draws a map Teeworlds 0.7 wrote with 0.7's tilesets, and
 * DDNet 17.2 to 20.2 do not.
 *
 * @param Teeworlds07Map Whether Teeworlds 0.7 wrote the map, see `IsTeeworlds07Map`.
 * @param HasMaps7 Whether there is a version of it in `maps7/`.
 * @param Convert Whether the server converts maps, see `sv_map_convert`.
 *
 * @return What DDNet clients and what 0.7 clients get.
 */
CServedMaps PlanServedMaps(bool Teeworlds07Map, bool HasMaps7, bool Convert);

/**
 * Whether Teeworlds 0.7 clients take a map by this name: they refuse a map by
 * the name of one of the maps Teeworlds 0.7 comes with unless it is that very
 * file, such as DDNet's `ctf5` or a conversion of it.
 *
 * @param pName Name of the map.
 * @param Crc Its crc.
 * @param Size Its size.
 *
 * @return Whether they take it.
 */
bool Teeworlds07AcceptsMap(const char *pName, unsigned Crc, unsigned Size);

/**
 * The name Teeworlds 0.7 clients are told for a map, see
 * `Teeworlds07AcceptsMap`: its own name, or, for a map that 0.7 clients would
 * refuse under it, the name with `_ddnet` after it.
 *
 * @param pBuffer Set to the name.
 * @param BufferSize Size of the buffer.
 * @param pName Name of the map.
 * @param Crc Crc of the file 0.7 clients get.
 * @param Size Its size.
 *
 * @return Whether it is the map's own name.
 */
bool Teeworlds07MapName(char *pBuffer, size_t BufferSize, const char *pName, unsigned Crc, unsigned Size);

/**
 * Whether a map has to be converted for the clients of the other version,
 * see `MapNeedsConversion`. Quick enough for the thread that loads the map.
 *
 * @param pMapName Name of the map.
 * @param vData The map file.
 * @param Direction The version it would be converted for.
 *
 * @return Whether it has to; `false` for a map that cannot be read either.
 */
bool MapDataNeedsConversion(const char *pMapName, const std::vector<uint8_t> &vData, EMapConvertDirection Direction);

/**
 * A map to convert for the clients of one kind, and where converted maps are
 * kept on disk.
 */
class CMapConversionRequest
{
public:
	/** Name of the map, for the cache and the log. */
	std::string m_MapName;
	/** The map file. */
	std::vector<uint8_t> m_vSource;
	CMapConvertOptions m_Options;
	/** The pictures to embed, shared by every conversion of the server. */
	std::shared_ptr<const IMapres> m_pMapres;
	/** Where converted maps are kept, `nullptr` for nowhere, see `sv_map_convert_cache`. */
	IStorage *m_pCacheStorage = nullptr;
	/** Their folder in the save directory. */
	std::string m_CacheFolder;
	/** The most bytes they may take, 0 for no limit; the oldest go first. */
	int64_t m_CacheMaxSize = 0;
};

/**
 * What converting a map for the clients of one kind gave.
 */
class CMapConversion
{
public:
	EMapConvertDirection m_Direction = EMapConvertDirection::TO07;
	/** The mode asked for; `m_Result.m_Mode` is the one used. */
	EMapConvertMode m_Mode = EMapConvertMode::HYBRID;
	SHA256_DIGEST m_SourceSha256 = {};
	/** Whether the map could be converted; `m_Result.m_Error` says why not. */
	bool m_Ok = false;
	/** Whether the map came from the disk cache. */
	bool m_FromCache = false;
	/** Whether it was written to the disk cache. */
	bool m_Cached = false;
	/** How long it took, in microseconds. */
	int64_t m_DurationUs = 0;
	/**
	 * The converted map, if `m_Result.m_Converted`; otherwise the clients get
	 * the map file itself. Statistics and warnings are empty for a map from
	 * the disk cache.
	 */
	CMapConvertResult m_Result;

	/**
	 * @return Whether converting the map with this sha256 in the same way
	 * again would give the same.
	 */
	bool IsFor(const SHA256_DIGEST &SourceSha256, EMapConvertDirection Direction, EMapConvertMode Mode) const;
};

/**
 * Where the disk cache keeps a converted map: the source sha256, the
 * direction, the mode and `MapConvertVersion()` tell them apart.
 *
 * @param pBuffer Set to the path in the save directory.
 * @param BufferSize Size of the buffer.
 * @param pFolder Folder of the cache.
 * @param pMapName Name of the map.
 * @param SourceSha256 Sha256 of the map file.
 * @param Direction Direction it is converted in.
 * @param Mode Mode it is converted with.
 */
void MapConversionCachePath(char *pBuffer, size_t BufferSize, const char *pFolder, const char *pMapName, const SHA256_DIGEST &SourceSha256, EMapConvertDirection Direction, EMapConvertMode Mode);

/**
 * Converts a map, or reads the map converting it gave before from the disk
 * cache, and keeps a new conversion there. Nothing is kept for a map that
 * needs no converting, for a conversion that failed and for one without the
 * diff tilesets it needed (`EMapConvertMode::HYBRID` that became `REMAP`).
 *
 * @param Request The map and how to convert it.
 * @param Aborted Whether to stop, asked before converting and before writing
 * the cache; may be empty.
 *
 * @return What it gave, `nullptr` if it stopped.
 */
std::shared_ptr<CMapConversion> ConvertServedMap(CMapConversionRequest Request, const std::function<bool()> &Aborted = nullptr);

/**
 * Converts a map on a thread of the job pool, see `ConvertServedMap`. The
 * server keeps playing the map meanwhile; aborting the job makes it stop at
 * the next step.
 */
class CMapConversionJob : public IJob
{
	CMapConversionRequest m_Request;
	std::shared_ptr<const CMapConversion> m_pConversion;
	std::atomic<bool> m_Started = false;
	std::atomic<bool> m_Finished = false;

protected:
	void Run() override;

public:
	explicit CMapConversionJob(CMapConversionRequest Request);

	/**
	 * @return What the conversion gave, once the job is `STATE_DONE`.
	 */
	const std::shared_ptr<const CMapConversion> &Conversion() const { return m_pConversion; }

	/**
	 * Whether the job may still use the pictures and the storage it was
	 * given. After it was aborted, it stops using them soon; one that had not
	 * started yet never does.
	 */
	bool InUse() const { return m_Started && !m_Finished; }
};

#endif
