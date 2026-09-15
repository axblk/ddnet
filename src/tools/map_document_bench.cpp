#include <base/log.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/document.h>
#include <game/map/document/map_file.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// What the map document costs, on real maps rather than on made-up ones.
//
// The numbers that matter are the ones that have to stay small while the map
// gets big: what one stroke costs in memory and in time, and what adding up
// the memory of a whole history costs - because the limit of the history is
// held against that number after every change.

using namespace map_document;

static const char *TOOL_NAME = "map_document_bench";

static double MillisSince(std::chrono::steady_clock::time_point Start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
}

// The first tile layer of the map that is big enough to draw on.
static bool FindLayer(const CMapState &State, size_t *pGroup, size_t *pLayer)
{
	for(size_t g = 0; g < State.NumGroups(); ++g)
	{
		for(size_t l = 0; l < State.NumLayers(g); ++l)
		{
			const CLayer *pCandidate = State.Layer(g, l);
			if(!std::holds_alternative<CTileLayer>(*pCandidate))
				continue;
			const CTileLayer &Tiles = std::get<CTileLayer>(*pCandidate);
			if(Tiles.Width() < 8 || Tiles.Height() < 8)
				continue;
			*pGroup = g;
			*pLayer = l;
			return true;
		}
	}
	return false;
}

static void Measure(IStorage *pStorage, const char *pPath)
{
	CDataFileReader File;
	if(!File.Open(pStorage, pPath, IStorage::TYPE_ALL))
	{
		log_error(TOOL_NAME, "Failed to open '%s'", pPath);
		return;
	}

	auto Start = std::chrono::steady_clock::now();
	CMapState State;
	std::vector<std::string> vWarnings;
	if(!ReadMapState(File, &State, &vWarnings))
	{
		log_error(TOOL_NAME, "Failed to read '%s'", pPath);
		return;
	}
	const double ReadMs = MillisSince(Start);
	for(const std::string &Warning : vWarnings)
	{
		log_warn(TOOL_NAME, "%s", Warning.c_str());
	}

	size_t Group, Layer;
	if(!FindLayer(State, &Group, &Layer))
	{
		log_warn(TOOL_NAME, "'%s' has no tile layer to draw on", pPath);
		return;
	}
	const CTileLayer *pTiles = State.TileLayer(Group, Layer);
	log_info(TOOL_NAME, "%s", pPath);
	log_info(TOOL_NAME, "  read in %.1f ms, %.2f MiB in the version, %d blocks in the layer drawn on (%dx%d)",
		ReadMs, State.Bytes() / 1048576.0, pTiles->m_Tiles.UsedChunks(), pTiles->Width(), pTiles->Height());

	// A stroke: one transaction that paints a line of tiles across the layer,
	// the way a brush dragged over it would.
	CDocument Document(std::move(State));
	Document.SetHistoryLimits((uint64_t)1 << 60, 100000);
	const uint64_t BeforeStrokes = Document.History().Bytes();
	const int Strokes = 200;
	Start = std::chrono::steady_clock::now();
	for(int i = 0; i < Strokes; ++i)
	{
		Document.Begin("Draw");
		CTileLayer Changed = *Document.Edit().TileLayer(Group, Layer);
		CTile Tile = {};
		Tile.m_Index = (unsigned char)(1 + i % 200);
		for(int x = 0; x < Changed.Width(); x += 8)
		{
			Changed.m_Tiles.Set(x, i % Changed.Height(), Tile);
		}
		Document.Edit().ReplaceLayer(Group, Layer, std::move(Changed));
		Document.Commit();
	}
	const double StrokeMs = MillisSince(Start) / Strokes;
	const uint64_t AfterStrokes = Document.History().Bytes();
	log_info(TOOL_NAME, "  %d strokes: %.3f ms each, %.1f KiB each, history now %.2f MiB in %d versions",
		Strokes, StrokeMs, (AfterStrokes - BeforeStrokes) / 1024.0 / Strokes,
		AfterStrokes / 1048576.0, (int)Document.History().NumEntries());

	// Adding up the memory of the history, which is what the limit is held
	// against after every change.
	Start = std::chrono::steady_clock::now();
	uint64_t Sum = 0;
	for(int i = 0; i < 5; ++i)
	{
		Sum += Document.History().Bytes();
	}
	log_info(TOOL_NAME, "  adding up %d versions: %.3f ms (%.2f MiB)",
		(int)Document.History().NumEntries(), MillisSince(Start) / 5, Sum / 5.0 / 1048576.0);

	// Stepping back through the whole history, which is meant to be a pointer
	// swap and nothing else.
	Start = std::chrono::steady_clock::now();
	int Steps = 0;
	while(Document.Undo())
	{
		++Steps;
	}
	while(Document.Redo())
	{
	}
	log_info(TOOL_NAME, "  %d steps back and forward: %.3f ms", 2 * Steps, MillisSince(Start));

	// Writing it out again.
	Start = std::chrono::steady_clock::now();
	CDataFileWriter Writer;
	char aOut[IO_MAX_PATH_LENGTH];
	str_format(aOut, sizeof(aOut), "%s.bench.map", pPath);
	if(Writer.Open(pStorage, aOut, IStorage::TYPE_ABSOLUTE))
	{
		WriteMapState(Writer, Document.Map());
		Writer.Finish();
		log_info(TOOL_NAME, "  written in %.1f ms", MillisSince(Start));
		pStorage->RemoveFile(aOut, IStorage::TYPE_ABSOLUTE);
	}
	File.Close();
}

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	if(argc < 2)
	{
		log_error(TOOL_NAME, "Usage: %s <map>...", argv[0]);
		return -1;
	}

	std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
	if(pStorage == nullptr)
	{
		log_error(TOOL_NAME, "Failed to open the storage");
		return -1;
	}
	for(int i = 1; i < argc; ++i)
	{
		Measure(pStorage.get(), argv[i]);
	}
	return 0;
}
