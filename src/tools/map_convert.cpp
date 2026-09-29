/* (c) DDNet developers. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.  */

#include <base/fs.h>
#include <base/io.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/convert/map_convert.h>
#include <game/map/convert/mapres.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

static const char *const TOOL_NAME = "map_convert";

static void Usage()
{
	log_info(TOOL_NAME, "Usage: map_convert [--to07 | --to06 | --mark-07] [--mode hybrid|remap|embed] [--check] [--repeat <n>] <source map> [<destination map>]");
	log_info(TOOL_NAME, "Converts a map for the clients of the other version: a DDNet or Teeworlds 0.6 map to Teeworlds 0.7 (--to07),");
	log_info(TOOL_NAME, "a Teeworlds 0.7 map to DDNet (--to06). Without either, the map goes to the version it is not made for.");
	log_info(TOOL_NAME, "  --mode hybrid  remap, and draw the tiles without a counterpart from the diff tilesets of the");
	log_info(TOOL_NAME, "                 tiles 0.7 dropped or added, embedded; remap if they are missing (default)");
	log_info(TOOL_NAME, "  --mode remap   move the tiles to where the other version has them, leave out the rest");
	log_info(TOOL_NAME, "  --mode embed   embed the tilesets the map was made with where they differ; no tile changes");
	log_info(TOOL_NAME, "  --mark-07     only mark an official 0.7 map in the old format as a 0.7 one");
	log_info(TOOL_NAME, "  --check       only say whether the map needs converting (exit code 0 if it does, 2 if not)");
	log_info(TOOL_NAME, "  --repeat <n>  check or convert n times and say how long it took, the tilesets read once");
	log_info(TOOL_NAME, "A map that looks the same for both is written unchanged. The tilesets come from mapres/ of the data directories.");
	log_info(TOOL_NAME, "The destination is data/maps7/<name>.map to 0.7 and data/maps/<name>.map to DDNet unless given.");
}

// The first time, and the median and the fastest of the others
static std::string Times(std::vector<double> vTimes)
{
	char aBuf[128];
	if(vTimes.size() == 1)
	{
		str_format(aBuf, sizeof(aBuf), "%.3f ms", vTimes[0]);
		return aBuf;
	}
	const double First = vTimes[0];
	vTimes.erase(vTimes.begin());
	std::sort(vTimes.begin(), vTimes.end());
	str_format(aBuf, sizeof(aBuf), "%.3f ms the first time, then %.3f ms median and %.3f ms at best of %d", First, vTimes[vTimes.size() / 2], vTimes[0], (int)vTimes.size());
	return aBuf;
}

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	std::optional<EMapConvertDirection> Direction;
#if defined(MAP_CONVERT_TO07)
	// map_convert_07, the old name, converts to 0.7
	Direction = EMapConvertDirection::TO07;
#endif
	EMapConvertMode Mode = EMapConvertMode::HYBRID;
	bool CheckOnly = false;
	int Repeat = 1;
	std::vector<const char *> vpPaths;
	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "--to07") == 0)
		{
			Direction = EMapConvertDirection::TO07;
		}
		else if(str_comp(argv[i], "--to06") == 0)
		{
			Direction = EMapConvertDirection::TO06;
		}
		else if(str_comp(argv[i], "--mark-07") == 0)
		{
			Direction = EMapConvertDirection::TO07;
			Mode = EMapConvertMode::MARK;
		}
		else if(str_comp(argv[i], "--mode") == 0 && i + 1 < argc)
		{
			if(!ParseMapConvertMode(argv[++i], Mode) || Mode == EMapConvertMode::MARK)
			{
				log_error(TOOL_NAME, "Unknown mode '%s'", argv[i]);
				Usage();
				return -1;
			}
		}
		else if(str_comp(argv[i], "--repeat") == 0 && i + 1 < argc)
		{
			Repeat = std::max(str_toint(argv[++i]), 1);
		}
		else if(str_comp(argv[i], "--check") == 0)
		{
			CheckOnly = true;
		}
		else if(str_comp(argv[i], "--help") == 0 || str_comp(argv[i], "-h") == 0)
		{
			Usage();
			return 0;
		}
		else if(argv[i][0] == '-' && argv[i][1] == '-')
		{
			log_error(TOOL_NAME, "Unknown option '%s'", argv[i]);
			Usage();
			return -1;
		}
		else
		{
			vpPaths.push_back(argv[i]);
		}
	}
	if(vpPaths.empty() || vpPaths.size() > 2)
	{
		Usage();
		return -1;
	}

	std::unique_ptr<IStorage> pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::BASIC, argc, argv));
	if(!pStorage)
	{
		log_error(TOOL_NAME, "Error creating basic storage");
		return -1;
	}

	const char *pSourcePath = vpPaths[0];
	void *pSourceData;
	unsigned SourceSize;
	if(!pStorage->ReadFile(pSourcePath, IStorage::TYPE_ABSOLUTE, &pSourceData, &SourceSize))
	{
		log_error(TOOL_NAME, "Failed to read '%s'", pSourcePath);
		return -1;
	}
	std::vector<uint8_t> vSource(static_cast<uint8_t *>(pSourceData), static_cast<uint8_t *>(pSourceData) + SourceSize);
	free(pSourceData);

	CDataFileReader Reader;
	if(!Reader.OpenFromMemory(pSourcePath, vSource, pSourcePath))
	{
		log_error(TOOL_NAME, "'%s' is not a map", pSourcePath);
		return -1;
	}
	const bool Is07 = IsTeeworlds07Map(Reader);
	if(!Direction.has_value())
	{
		Direction = Is07 ? EMapConvertDirection::TO06 : EMapConvertDirection::TO07;
	}
	log_info(TOOL_NAME, "'%s' is a %s map, for %s clients now", pSourcePath, Is07 ? "Teeworlds 0.7" : "DDNet", *Direction == EMapConvertDirection::TO07 ? "Teeworlds 0.7" : "DDNet");

	if(CheckOnly)
	{
		bool Needed = false;
		std::vector<double> vTimes;
		for(int i = 0; i < Repeat; i++)
		{
			const int64_t Start = time_get_nanoseconds().count();
			Needed = Mode == EMapConvertMode::MARK ? !Is07 : MapNeedsConversion(Reader, *Direction);
			vTimes.push_back((time_get_nanoseconds().count() - Start) / 1e6);
		}
		log_info(TOOL_NAME, "%s (checked in %s)", Needed ? "needs converting" : "looks the same for both, nothing to convert", Times(vTimes).c_str());
		return Needed ? 0 : 2;
	}

	char aDestPath[IO_MAX_PATH_LENGTH];
	if(vpPaths.size() == 2)
	{
		str_copy(aDestPath, vpPaths[1]);
	}
	else
	{
		const char *pFolder = *Direction == EMapConvertDirection::TO07 ? "data/maps7" : "data/maps";
		char aName[IO_MAX_PATH_LENGTH];
		fs_split_file_extension(fs_filename(pSourcePath), aName, sizeof(aName));
		str_format(aDestPath, sizeof(aDestPath), "%s/%s.map", pFolder, aName);
		if(fs_makedir_rec_for(aDestPath) != 0)
		{
			log_error(TOOL_NAME, "Failed to create the folder '%s'", pFolder);
			return -1;
		}
	}

	CMapresFromStorage Mapres(pStorage.get());
	CMapConvertOptions Options;
	Options.m_Direction = *Direction;
	Options.m_Mode = Mode;
	CMapConvertResult Result;
	bool Success = false;
	std::vector<double> vTimes;
	for(int i = 0; i < Repeat; i++)
	{
		// Only the first run reads the tilesets
		Result = CMapConvertResult();
		const int64_t Start = time_get_nanoseconds().count();
		Success = ConvertMap(Reader, Options, Mapres, Result);
		vTimes.push_back((time_get_nanoseconds().count() - Start) / 1e6);
	}
	const std::string Took = Times(vTimes);
	for(const std::string &Warning : Result.m_vWarnings)
	{
		log_warn(TOOL_NAME, "%s", Warning.c_str());
	}
	if(!Success)
	{
		log_error(TOOL_NAME, "Failed to convert '%s': %s", pSourcePath, Result.m_Error.c_str());
		return -1;
	}

	// A map that needs no converting is written as it is, so both versions get the same file
	const std::vector<uint8_t> &vOutput = Result.m_Converted ? Result.m_vData : vSource;
	IOHANDLE File = pStorage->OpenFile(aDestPath, IOFLAG_WRITE, IStorage::TYPE_ABSOLUTE);
	if(!File)
	{
		log_error(TOOL_NAME, "Failed to open '%s' for writing", aDestPath);
		return -1;
	}
	const bool Written = io_write(File, vOutput.data(), vOutput.size()) == vOutput.size();
	if(io_close(File) != 0 || !Written)
	{
		log_error(TOOL_NAME, "Failed to write '%s'", aDestPath);
		return -1;
	}

	if(!Result.m_Converted)
	{
		log_info(TOOL_NAME, "Nothing to convert, wrote '%s' unchanged (%s)", aDestPath, Took.c_str());
		return 0;
	}
	const CMapConvertStats &Stats = Result.m_Stats;
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(Result.m_Sha256, aSha256, sizeof(aSha256));
	log_info(TOOL_NAME, "Wrote '%s' (%s, %d bytes, sha256 %s) in %s", aDestPath, MapConvertModeName(Result.m_Mode), (int)vOutput.size(), aSha256, Took.c_str());
	log_info(TOOL_NAME, "Tilesets: %d embedded, %d remapped, %d embedded again for quads, %d RGB made RGBA, %d image items marked as 0.7",
		Stats.m_EmbeddedImages, Stats.m_RemappedImages, Stats.m_QuadImages, Stats.m_RgbImages, Stats.m_MarkedImages);
	log_info(TOOL_NAME, "Tile layers: %d rewritten, %d added; %d tiles moved, %d without a counterpart left out; %d envelopes, %d data blocks passed on",
		Stats.m_RewrittenLayers, Stats.m_SplitLayers + Stats.m_DiffLayers, Stats.m_RemappedTiles, Stats.m_LostTiles, Stats.m_Envelopes, Stats.m_PassedData);
	if(Stats.m_DiffTiles > 0 || Stats.m_RestoredImages > 0)
	{
		log_info(TOOL_NAME, "Tiles without a counterpart: %d drawn from %d embedded diff tilesets, in %d layers of their own; %d diff tilesets made tilesets again",
			Stats.m_DiffTiles, Stats.m_DiffImages, Stats.m_DiffLayers, Stats.m_RestoredImages);
	}
	return 0;
}
