#ifndef GAME_MAP_CONVERT_MAPRES_H
#define GAME_MAP_CONVERT_MAPRES_H

#include <base/lock.h>

#include <engine/shared/datafile.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

class IStorage;

/**
 * A picture of the mapres, ready to be embedded into a map.
 */
class CMapresImage
{
public:
	int m_Width = 0;
	int m_Height = 0;
	/**
	 * The RGBA pixels, compressed as a map stores them. The bytes are shared,
	 * so every map that embeds the picture uses the same ones.
	 */
	CDataFileRawData m_Data;
	/**
	 * Whether the tile with that index covers its whole cell, a bit per index.
	 */
	uint32_t m_aOpaque[8] = {};

	bool IsOpaque(int Index) const { return (m_aOpaque[Index / 32] >> (Index % 32)) & 1; }
};

/**
 * The pictures a map converter embeds instead of the external images of a map.
 */
class IMapres
{
public:
	virtual ~IMapres() = default;

	/**
	 * @param pName The name the map gives the external image.
	 * @param Teeworlds07 Whether the map was made for Teeworlds 0.7, which
	 * means the 0.7 picture where 0.7 drew it again.
	 *
	 * @return The picture a client shows for that image, or `nullptr` if
	 * there is none.
	 */
	virtual std::shared_ptr<const CMapresImage> Find(const char *pName, bool Teeworlds07) const = 0;

	/**
	 * @param pName The name of a diff tileset, see `Map07Tables::DIFFS07` and
	 * `Map07Tables::DIFFS06`.
	 *
	 * @return The picture, or `nullptr` if it is missing.
	 */
	virtual std::shared_ptr<const CMapresImage> FindDiff(const char *pName) const = 0;
};

/**
 * The pictures in `mapres/` of the data directory: `<name>.png`, and
 * `<name>_0.7.png` for the tilesets Teeworlds 0.7 drew again; the diff
 * tilesets are in `convert/`. Each is read
 * and compressed once, when it is first asked for, and then kept; one
 * instance can serve every conversion of a process, from several threads.
 */
class CMapresFromStorage : public IMapres
{
	IStorage *m_pStorage;
	mutable CLock m_Mutex;
	// Pictures that are missing are kept as well, as `nullptr`
	mutable std::map<std::string, std::shared_ptr<const CMapresImage>, std::less<>> m_Cache GUARDED_BY(m_Mutex);

	std::shared_ptr<const CMapresImage> FindPath(const char *pPath) const REQUIRES(!m_Mutex);

public:
	explicit CMapresFromStorage(IStorage *pStorage) :
		m_pStorage(pStorage) {}

	std::shared_ptr<const CMapresImage> Find(const char *pName, bool Teeworlds07) const override REQUIRES(!m_Mutex);
	std::shared_ptr<const CMapresImage> FindDiff(const char *pName) const override REQUIRES(!m_Mutex);

	/**
	 * Reads a picture from a file.
	 *
	 * @param pStorage The storage to read from.
	 * @param pPath The path of the PNG file.
	 *
	 * @return The picture, or `nullptr` if it is missing or cannot be read.
	 */
	static std::shared_ptr<const CMapresImage> Load(IStorage *pStorage, const char *pPath);
};

#endif
