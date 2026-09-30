#ifndef GAME_MAP_CONVERT_MAP_CONVERT_H
#define GAME_MAP_CONVERT_MAP_CONVERT_H

#include <base/hash.h>

#include <engine/shared/uuid_manager.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CDataFileReader;
class IMapres;

/**
 * Which version a map is converted for.
 */
enum class EMapConvertDirection
{
	/** A DDNet or Teeworlds 0.6 map for Teeworlds 0.7 clients. */
	TO07,
	/** A Teeworlds 0.7 map for DDNet clients, readable by DDNet 17.2 and newer. */
	TO06,
};

/**
 * What happens to the external pictures whose tiles are elsewhere in the
 * other version.
 */
enum class EMapConvertMode
{
	/** Move the tiles to where the other version has them; tiles without a counterpart are left out. */
	REMAP,
	/**
	 * Like `REMAP`, and the tiles without a counterpart keep their index and
	 * are drawn from the diff tileset of theirs (`Map07Tables::DIFFS07`,
	 * `Map07Tables::DIFFS06`), embedded, in a tile layer right above theirs.
	 * Diff tilesets of the other direction become their tileset again.
	 * Without the diff tilesets it is `REMAP`.
	 */
	HYBRID,
	/** Embed the pictures the map was made with where the other version's differ; no tile changes. */
	EMBED,
	/** Only mark a 0.6 map as a 0.7 one (image items of version 2); for the official 0.7 maps in the old format. */
	MARK,
};

const char *MapConvertDirectionName(EMapConvertDirection Direction);
const char *MapConvertModeName(EMapConvertMode Mode);
bool ParseMapConvertMode(const char *pName, EMapConvertMode &Mode);
/**
 * @return A number that changes whenever the converter or its tables do, so
 * that maps converted before can be told from what a conversion makes now.
 */
uint32_t MapConvertVersion();

class CMapConvertOptions
{
public:
	EMapConvertDirection m_Direction = EMapConvertDirection::TO07;
	EMapConvertMode m_Mode = EMapConvertMode::HYBRID;
	/** Largest output in bytes, 0 for four times the source plus 32 MiB. */
	size_t m_MaxOutputSize = 0;
};

class CMapConvertStats
{
public:
	/** External pictures that were embedded. */
	int m_EmbeddedImages = 0;
	/** External pictures whose tiles were moved. */
	int m_RemappedImages = 0;
	/** Pictures embedded again for the quads of a remapped picture. */
	int m_QuadImages = 0;
	/** Embedded RGB pictures that became RGBA. */
	int m_RgbImages = 0;
	/** Image items of version 1 that became version 2 (`TO07`, `MARK`). */
	int m_MarkedImages = 0;
	/** Tile layers whose tiles changed. */
	int m_RewrittenLayers = 0;
	/** Tile layers added for tiles that moved to another picture. */
	int m_SplitLayers = 0;
	/** Tiles moved or changed. */
	int m_RemappedTiles = 0;
	/** Tiles without a counterpart that `REMAP` left out. */
	int m_LostTiles = 0;
	/** Tiles without a counterpart that `HYBRID` drew from a diff tileset. */
	int m_DiffTiles = 0;
	/** Tile layers added for them. */
	int m_DiffLayers = 0;
	/** Diff tilesets that were embedded. */
	int m_DiffImages = 0;
	/** Diff tilesets of a conversion the other way that became their tileset again. */
	int m_RestoredImages = 0;
	/** Data blocks copied without being uncompressed. */
	int m_PassedData = 0;
	/** Envelopes changed to the 0.7 form. */
	int m_Envelopes = 0;
};

class CMapConvertResult
{
public:
	/**
	 * Whether there is a converted map. `false` for a map that is in the form
	 * asked for already, which is then used as it is: `m_vData` is empty.
	 */
	bool m_Converted = false;
	/** The mode the map was converted with: `REMAP` where `HYBRID` lacked its diff tilesets. */
	EMapConvertMode m_Mode = EMapConvertMode::HYBRID;
	std::vector<uint8_t> m_vData;
	SHA256_DIGEST m_Sha256 = {};
	unsigned m_Crc = 0;
	CMapConvertStats m_Stats;
	std::vector<std::string> m_vWarnings;
	/** Why the conversion failed, empty if it did not. */
	std::string m_Error;
};

/**
 * Where a converted map came from. Every converted map carries it in an item
 * of its own, which clients that do not know it skip.
 */
class CMapConvertProvenance
{
public:
	static constexpr int CURRENT_VERSION = 1;

	int m_Version = CURRENT_VERSION;
	EMapConvertDirection m_Direction = EMapConvertDirection::TO07;
	EMapConvertMode m_Mode = EMapConvertMode::REMAP;
	SHA256_DIGEST m_SourceSha256 = {};
	unsigned m_SourceCrc = 0;
	int m_SourceSize = 0;
	/** `Map07Tables::VERSION` and `Map07Tables::CHECKSUM` of the tables used. */
	int m_TableVersion = 0;
	unsigned m_TableChecksum = 0;
	int m_ConverterVersion = 0;
};

/**
 * @return The UUID of the item that says where a converted map came from.
 */
CUuid MapConvertProvenanceUuid();

/**
 * @param Reader An open map.
 * @param Provenance Set to what the map says, if it was converted.
 *
 * @return Whether the map was converted.
 */
bool ReadMapConvertProvenance(CDataFileReader &Reader, CMapConvertProvenance &Provenance);

/**
 * Whether a map has to be converted to look the same for the other version's
 * clients. Only then does `ConvertMap` convert it; otherwise both kinds of
 * clients get the same file.
 *
 * It has to when a tile layer uses a tile of an external picture that is
 * somewhere else in the other version, when a quad shows a part of such a
 * picture that looks different there, and
 * - to 0.7: when it uses an external picture that 0.7 clients do not have;
 * - to DDNet: when it embeds an RGB picture or uses `easter` or
 *   `generic_shadows`, which DDNet has only as 0.7 pictures, or when an item
 *   says another size than it takes (`CDataFileReader::ItemSizesWrong`).
 * Nothing else: 0.7 clients read image items of version 1 and skip what they
 * do not know, DDNet reads tileskip, 0.7 envelopes and image items of
 * version 2. Only the tile and quad layers of the pictures that changed are
 * unpacked.
 *
 * @param Source The map, open.
 * @param Direction The version it is for.
 */
bool MapNeedsConversion(CDataFileReader &Source, EMapConvertDirection Direction);

/**
 * Converts a map between Teeworlds 0.7 and DDNet in memory.
 *
 * Only the external pictures whose tiles are elsewhere in the other version
 * and the tile layers that use them change; every other item and data block
 * is handed on as it is, the data without being uncompressed. The output
 * depends only on the source, the options and the pictures, so converting the
 * same map twice gives the same bytes.
 *
 * A map that is in the form asked for already, or that looks the same in
 * both versions (`MapNeedsConversion`), stays as it is, see
 * `CMapConvertResult::m_Converted`: the same file serves both. A map is never
 * converted twice in the same direction. `EMapConvertMode::MARK` marks every
 * map that is not a 0.7 one yet.
 *
 * @param Source The map, open.
 * @param Options Direction and mode.
 * @param Mapres The pictures to embed.
 * @param Result The converted map, what was done, warnings and errors.
 *
 * @return `false` if the map could not be converted, `Result.m_Error` says why.
 */
bool ConvertMap(CDataFileReader &Source, const CMapConvertOptions &Options, const IMapres &Mapres, CMapConvertResult &Result);

/**
 * The pictures `ConvertMap` asks its `IMapres` for, so that they can be
 * fetched ahead.
 */
class CMapConvertPictures
{
public:
	/** The names `IMapres::Find` is asked for. */
	std::vector<std::string> m_vImages;
	/** What `IMapres::Find` is told about Teeworlds 0.7. */
	bool m_Teeworlds07 = false;
	/** The names `IMapres::FindDiff` may be asked for. */
	std::vector<std::string> m_vDiffs;
};

/**
 * Tells which pictures converting a map asks for, without converting it.
 *
 * @param Source The map, open.
 * @param Options Direction and mode, as for `ConvertMap`.
 *
 * @return The pictures; none if the map stays as it is.
 */
CMapConvertPictures MapConvertPictures(CDataFileReader &Source, const CMapConvertOptions &Options);

/**
 * The flags of a tile that is drawn like `Fix` applied to the tile first and
 * then `Flags`: flipping horizontally (XFLIP), vertically (YFLIP) and turning
 * right (ROTATE) are the eight symmetries of a square. OPAQUE is left out.
 *
 * @param Flags The flags the tile has.
 * @param Fix The flags that make the other version's tile look like this one.
 *
 * @return XFLIP, YFLIP and ROTATE of the composed flags.
 */
int ComposeTileFlags(int Flags, int Fix);

#endif
