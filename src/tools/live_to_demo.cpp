/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <base/fs.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/shared/live_stream.h>
#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

// A live demo stream is a demo in pieces (see `CLiveRecorder`). A server
// puts the pieces together again when a stream is stopped with `keep`; this
// does it for any stream directory, also one that is still written to, and
// can cut where a match ended instead.

namespace
{
	constexpr const char *TOOL_NAME = "live_to_demo";

	void PrintUsage(const char *pProgramName)
	{
		log_info(TOOL_NAME, "Usage: %s [--matches] <stream directory> [<output directory>]", pProgramName);
		log_info(TOOL_NAME, "  --matches  One demo per match that ended, called <stream>_<epoch>_<tick>.demo");
		log_info(TOOL_NAME, "             after the tick it ended on, instead of one demo per map from where");
		log_info(TOOL_NAME, "             the stream began or woke up. A cut is where a segment begins, so a");
		log_info(TOOL_NAME, "             demo opens with the end of the match before.");
		log_info(TOOL_NAME, "  The demos go into the current directory if no other one is given.");
	}
} // namespace

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	CLiveStream::ECut Cut = CLiveStream::ECut::EPOCHS;
	const char *pStreamDir = nullptr;
	const char *pOutputDir = nullptr;
	for(int i = 1; i < argc; ++i)
	{
		if(str_comp(argv[i], "--matches") == 0)
		{
			Cut = CLiveStream::ECut::MATCHES;
		}
		else if(argv[i][0] != '-' && pStreamDir == nullptr)
		{
			pStreamDir = argv[i];
		}
		else if(argv[i][0] != '-' && pOutputDir == nullptr)
		{
			pOutputDir = argv[i];
		}
		else
		{
			PrintUsage(argv[0]);
			return 1;
		}
	}
	if(pStreamDir == nullptr)
	{
		PrintUsage(argv[0]);
		return 1;
	}
	if(pOutputDir == nullptr)
	{
		pOutputDir = ".";
	}

	const std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	if(pStorage == nullptr)
	{
		log_error(TOOL_NAME, "Failed to initialize the storage location");
		return 1;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	str_format(aPath, sizeof(aPath), "%s/index.json", pStreamDir);
	void *pIndex;
	unsigned IndexSize;
	if(!pStorage->ReadFile(aPath, IStorage::TYPE_ABSOLUTE, &pIndex, &IndexSize))
	{
		log_error(TOOL_NAME, "Failed to read '%s'", aPath);
		return 1;
	}
	CLiveStream Stream;
	char aError[256];
	const bool Parsed = Stream.ParseIndex(static_cast<const char *>(pIndex), IndexSize, aError, sizeof(aError));
	free(pIndex);
	if(!Parsed)
	{
		log_error(TOOL_NAME, "'%s' is not the index of a stream: %s", aPath, aError);
		return 1;
	}
	if(Stream.m_Live)
	{
		log_info(TOOL_NAME, "The stream is live, the segment that is still written is left out");
	}

	if(fs_makedir_rec_for(pOutputDir) != 0 || fs_makedir(pOutputDir) != 0)
	{
		log_error(TOOL_NAME, "Failed to create the directory '%s'", pOutputDir);
		return 1;
	}
	bool Ok = true;
	const std::vector<CLiveStream::CDemo> vDemos = Stream.Demos(Cut);
	for(const CLiveStream::CDemo &Demo : vDemos)
	{
		str_format(aPath, sizeof(aPath), "%s/%s.demo", pOutputDir, Demo.m_Name.c_str());
		if(!CLiveStream::WriteDemo(Demo, pStorage.get(), IStorage::TYPE_ABSOLUTE, pStreamDir, aPath))
		{
			log_error(TOOL_NAME, "Failed to write '%s'", aPath);
			Ok = false;
			continue;
		}
		const auto Epoch = std::find_if(Stream.m_vEpochs.begin(), Stream.m_vEpochs.end(), [&](const CLiveStream::CEpoch &Other) { return Other.m_Id == Demo.m_Epoch; });
		log_info(TOOL_NAME, "Wrote '%s': map %s, ticks %d to %d, %d seconds, %d markers", aPath, Epoch->m_Map.c_str(), Demo.m_FirstTick, Demo.m_LastTick, (Demo.m_LastTick - Demo.m_FirstTick) / SERVER_TICK_SPEED, (int)Demo.m_vMarkers.size());
	}
	if(vDemos.empty())
	{
		log_info(TOOL_NAME, Cut == CLiveStream::ECut::MATCHES ? "No match has ended in what the stream has" : "The stream has no complete segment yet");
	}
	return Ok ? 0 : 1;
}
