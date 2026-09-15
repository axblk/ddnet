#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/gfx/image_loader.h>
#include <engine/graphics_window.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/map_file.h>
#include <game/map/document/structure.h>
#include <game/map/document/view.h>
#include <game/map/document_images.h>
#include <game/map/document_render.h>
#include <game/map/standalone/map_view.h>

#include <memory>
#include <string>
#include <vector>

// Draws a map the way the editor will: not from the file, but from a version
// of the document that was read out of it. What it is for is the comparison -
// the same map through this and through `map_render` has to come out as the
// same picture, or the document says something else than the file it came from.

static constexpr const char *TOOL_NAME = "map_document_render";

static void PrintUsage(const char *pProgramName)
{
	log_error(TOOL_NAME, "Usage: %s [-o <output>] [-w <width>] [-h <height>] [-z <zoom>] [-x <x-position>] [-y <y-position>] [-t <time>] [-e <overlay>] <input.map>", pProgramName);
	log_error(TOOL_NAME, "  -o <output>      Output PNG file (default: output.png)");
	log_error(TOOL_NAME, "  -w <width>       Output image width (default: 1920)");
	log_error(TOOL_NAME, "  -h <height>      Output image height (default: 1080)");
	log_error(TOOL_NAME, "  -z <zoom>        Map zoom (default: auto, expects: 1.0f)");
	log_error(TOOL_NAME, "  -x <x-position>  X-Position (default: auto, expects ingame X coordinate i.e. 20.32)");
	log_error(TOOL_NAME, "  -y <y-position>  Y-Position (default: auto, expects ingame Y coordinate i.e. 20.32)");
	log_error(TOOL_NAME, "  -t <time>        Time offset for envelopes in ms (default: 0 ms, minimum: 0)");
	log_error(TOOL_NAME, "  -e <overlay>     How strongly the physics layers are drawn over the design, 0 to 100");
}

static bool ReadDocument(IStorage *pStorage, const char *pPath, map_document::CMapState *pMap)
{
	CDataFileReader File;
	if(!File.Open(pStorage, pPath, IStorage::TYPE_ABSOLUTE))
	{
		log_error(TOOL_NAME, "Failed to open map '%s'", pPath);
		return false;
	}
	std::vector<std::string> vWarnings;
	const bool Read = map_document::ReadMapState(File, pMap, &vWarnings);
	for(const std::string &Warning : vWarnings)
		log_warn(TOOL_NAME, "%s", Warning.c_str());
	File.Close();
	if(!Read)
		log_error(TOOL_NAME, "Failed to read map '%s' as a document", pPath);
	return Read;
}

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	std::string OutputFile = "output.png";
	int OutputWidth = 1920;
	int OutputHeight = 1080;
	std::string InputMap;
	bool AutoPosition = true;
	vec2 Position(0, 0);
	bool AutoZoom = true;
	float Zoom = 1.0f;
	int TimeOffsetMillis = 0;
	int EntityOverlay = 0;
	bool InvalidUsage = false;

	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
		{
			OutputFile = argv[++i];
		}
		else if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
		{
			OutputWidth = str_toint(argv[++i]);
		}
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
		{
			OutputHeight = str_toint(argv[++i]);
		}
		else if(str_comp(argv[i], "-z") == 0 && i + 1 < argc)
		{
			Zoom = str_tofloat(argv[++i]);
			AutoZoom = false;
		}
		else if(str_comp(argv[i], "-x") == 0 && i + 1 < argc)
		{
			Position.x = str_tofloat(argv[++i]);
			AutoPosition = false;
		}
		else if(str_comp(argv[i], "-y") == 0 && i + 1 < argc)
		{
			Position.y = str_tofloat(argv[++i]);
			AutoPosition = false;
		}
		else if(str_comp(argv[i], "-t") == 0 && i + 1 < argc)
		{
			TimeOffsetMillis = std::max(0, str_toint(argv[++i]));
		}
		else if(str_comp(argv[i], "-e") == 0 && i + 1 < argc)
		{
			EntityOverlay = std::clamp(str_toint(argv[++i]), 0, 100);
		}
		else if(argv[i][0] != '-' && InputMap.empty())
		{
			InputMap = argv[i];
		}
		else
		{
			InvalidUsage = true;
			break;
		}
	}

	if(InputMap.empty() || InvalidUsage)
	{
		PrintUsage(argv[0]);
		return 1;
	}

	// Nothing is watching this, so it draws into a surface that is on no
	// screen and reads the one frame it drew back off it.
	CStandaloneMapView View(TOOL_NAME);
	if(!View.Init(argc, argv) || !View.OpenWindow(OutputWidth, OutputHeight, CreateOffscreenGraphicsWindow(), false))
		return 1;

	map_document::CMapState Read;
	if(!ReadDocument(View.Storage(), InputMap.c_str(), &Read))
		return 1;
	const auto pMap = std::make_shared<const map_document::CMapState>(std::move(Read));

	// The pictures go back to the graphics card before it is shut down, so
	// they are let go of by hand rather than at the end of main.
	auto pImages = std::make_unique<CDocumentImages>(View.Graphics(), View.Storage(), View.AssetLoader(), nullptr, TOOL_NAME);
	CDocumentImages &Images = *pImages;
	Images.Use(*pMap);
	// A tool draws one picture and stops, so it may wait for the pictures; an
	// editor draws every frame and does not - that is what `Loading` is for.
	while(Images.Loading())
		Images.Update();

	CDocumentRenderer Renderer;
	Renderer.OnInit(View.Graphics(), &Images);
	Renderer.Use(pMap);

	vec2 WorldSize(View.Width(), View.Height());
	const std::optional<map_document::CLayerAddress> Game = map_document::FindGameLayer(*pMap);
	if(Game.has_value())
	{
		const map_document::CTileLayer *pGame = pMap->TileLayer(Game->m_Group, Game->m_Layer);
		WorldSize = vec2(pGame->Width() * 32.0f, pGame->Height() * 32.0f);
	}

	// The camera is the editor's, so that what this draws is what the editor
	// would draw - and so that the sums the page does to turn a click into a
	// tile are held against a picture rather than only against themselves.
	map_document::CView Camera;
	Camera.SetSurface(View.Width(), View.Height());
	Camera.Fit(WorldSize);
	if(!AutoPosition)
		Camera.SetCenter(Position * 32.0f);
	if(!AutoZoom)
		Camera.SetZoom(Zoom);

	CDocumentRenderer::CParams Params;
	Params.m_Center = Camera.Center();
	Params.m_Zoom = Camera.Zoom();
	Params.m_ViewSize = Camera.ViewSize();
	Params.m_TimeOffsetMillis = TimeOffsetMillis;
	Params.m_EntityOverlayVal = EntityOverlay;

	View.Graphics()->MapScreen(CScreenRect(0, 0, View.Width(), View.Height()));
	View.Graphics()->Clear(0, 0, 0);
	Renderer.Render(Params);

	CImageInfo Image;
	int ReturnCode = 1;
	if(View.ReadFrame(Image))
	{
		IOHANDLE File = io_open(OutputFile.c_str(), IOFLAG_WRITE);
		if(File == nullptr)
		{
			log_error(TOOL_NAME, "Failed to open '%s' for writing", OutputFile.c_str());
		}
		else if(!CImageLoader::SavePng(File, OutputFile.c_str(), Image))
		{
			log_error(TOOL_NAME, "Failed to save screenshot to '%s'", OutputFile.c_str());
		}
		else
		{
			log_info(TOOL_NAME, "Saved screenshot to '%s'", OutputFile.c_str());
			ReturnCode = 0;
		}
	}
	Image.Free();

	Renderer.Clear();
	pImages = nullptr;
	View.Shutdown();
	return ReturnCode;
}
