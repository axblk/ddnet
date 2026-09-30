#ifndef GAME_MAP_CONVERT_MAPRES_H
#define GAME_MAP_CONVERT_MAPRES_H

#include <base/lock.h>

#include <engine/shared/datafile.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class CMapConvertPictures;
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
 * tilesets are in `convert/`. The files come from a function, so that they
 * can be fetched ahead (see `MapConvertPictures`) or read from a storage
 * (`CMapresFromStorage`). Each is read and compressed once, when it is first
 * asked for, and then kept; one instance can serve every conversion of a
 * process, from several threads.
 */
class CMapresFromFiles : public IMapres
{
public:
	/**
	 * Gives the bytes of a file of the data directory.
	 *
	 * @param pPath The path in the data directory, such as `mapres/jungle_main.png`.
	 * @param vData Gets the bytes.
	 *
	 * @return `false` if the file is missing.
	 */
	using FReadFile = std::function<bool(const char *pPath, std::vector<uint8_t> &vData)>;

private:
	FReadFile m_ReadFile;
	mutable CLock m_Mutex;
	// Pictures that are missing are kept as well, as `nullptr`
	mutable std::map<std::string, std::shared_ptr<const CMapresImage>, std::less<>> m_Cache GUARDED_BY(m_Mutex);

	std::shared_ptr<const CMapresImage> FindPath(const char *pPath) const REQUIRES(!m_Mutex);

public:
	/**
	 * @param ReadFile Gives the files.
	 */
	explicit CMapresFromFiles(FReadFile ReadFile) :
		m_ReadFile(std::move(ReadFile)) {}

	std::shared_ptr<const CMapresImage> Find(const char *pName, bool Teeworlds07) const override REQUIRES(!m_Mutex);
	std::shared_ptr<const CMapresImage> FindDiff(const char *pName) const override REQUIRES(!m_Mutex);

	/**
	 * The files that `Find` and `FindDiff` read for these pictures.
	 *
	 * @param Pictures What a conversion asks for, see `MapConvertPictures`.
	 *
	 * @return The paths in the data directory.
	 */
	static std::vector<std::string> Paths(const CMapConvertPictures &Pictures);

	/**
	 * Reads a picture from the bytes of a PNG file.
	 *
	 * @param vData The bytes.
	 * @param pPath Where they are from, for what is told.
	 *
	 * @return The picture, or `nullptr` if it cannot be read.
	 */
	static std::shared_ptr<const CMapresImage> Load(const std::vector<uint8_t> &vData, const char *pPath);
};

/**
 * The pictures of the data directory, read from a storage.
 */
class CMapresFromStorage : public CMapresFromFiles
{
public:
	/**
	 * @param pStorage The storage to read from.
	 */
	explicit CMapresFromStorage(IStorage *pStorage);
};

#endif
