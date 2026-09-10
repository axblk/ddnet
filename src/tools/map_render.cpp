#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/graphics_window.h>
#include <engine/storage.h>

#include <game/map/standalone/map_view.h>

#include <string>

static constexpr const char *TOOL_NAME = "map_render";

static void PrintUsage(const char *pProgramName)
{
	log_error(TOOL_NAME, "Usage: %s [-o <output>] [-w <width>] [-h <height>] [-z <zoom>] [-x <x-position>] [-y <y-position>] [-t <time>] <input.map>", pProgramName);
	log_error(TOOL_NAME, "  -o <output>      Output PNG file (default: output.png)");
	log_error(TOOL_NAME, "  -w <width>       Output image width (default: 1920)");
	log_error(TOOL_NAME, "  -h <height>      Output image height (default: 1080)");
	log_error(TOOL_NAME, "  -z <zoom>        Map zoom (default: auto, expects: 1.0f)");
	log_error(TOOL_NAME, "  -x <x-position>  X-Position (default: auto, expects ingame X coordinate i.e. 20.32)");
	log_error(TOOL_NAME, "  -y <y-position>  Y-Position (default: auto, expects ingame Y coordinate i.e. 20.32)");
	log_error(TOOL_NAME, "  -t <time>        Time offset for envelopes in ms (default: 0 ms, minimum: 0)");
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

	bool InvalidUsage = false;

	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
		{
			OutputFile = argv[++i];
		}
		else if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
		{
			OutputWidth = std::max(1, atoi(argv[++i]));
		}
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
		{
			OutputHeight = std::max(1, atoi(argv[++i]));
		}
		else if(str_comp(argv[i], "-z") == 0 && i + 1 < argc)
		{
			AutoZoom = false;
			Zoom = std::max(0.001f, (float)atof(argv[++i]));
		}
		else if(str_comp(argv[i], "-x") == 0 && i + 1 < argc)
		{
			AutoPosition = false;
			Position.x = std::max(0.0f, (float)atof(argv[++i]));
		}
		else if(str_comp(argv[i], "-y") == 0 && i + 1 < argc)
		{
			AutoPosition = false;
			Position.y = std::max(0.0f, (float)atof(argv[++i]));
		}
		else if(str_comp(argv[i], "-t") == 0 && i + 1 < argc)
		{
			TimeOffsetMillis = std::max(0, atoi(argv[++i]));
		}
		else if(argv[i][0] != '-')
		{
			if(!InputMap.empty())
			{
				InvalidUsage = true;
				break;
			}
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

	constexpr LOG_COLOR WarningLogColor = LOG_COLOR{255, 255, 0};
	constexpr LOG_COLOR SuccessLogColor = LOG_COLOR{0, 255, 128};

	if(!OutputFile.ends_with(".png") && !OutputFile.ends_with(".PNG"))
	{
		log_warn_color(WarningLogColor, TOOL_NAME, "Output name '%s' does not end with '.png' suffix", OutputFile.c_str());
	}

	// Nothing is watching this, so it draws into a surface that is on no
	// screen and reads the one frame it drew back off it.
	CStandaloneMapView View(TOOL_NAME);
	if(!View.Init(argc, argv) || !View.OpenWindow(OutputWidth, OutputHeight, CreateOffscreenGraphicsWindow(), false))
		return 1;

	// Load map from absolute path
	if(!View.LoadMap(InputMap.c_str(), IStorage::TYPE_ABSOLUTE))
		return 1;

	// Calculate center and zoom to fit the map
	const vec2 MapWorldSize = View.MapWorldSize();
	if(AutoZoom)
		Zoom = View.FitZoom();

	CStandaloneMapView::SRenderParams RenderParams;
	RenderParams.m_Center = AutoPosition ? MapWorldSize / 2.0f : Position * 32.0f;
	RenderParams.m_Zoom = Zoom;
	RenderParams.m_TimeOffsetMillis = TimeOffsetMillis;
	View.Render(RenderParams);

	int ReturnCode = 1;
	if(View.SaveImage(OutputFile.c_str()))
	{
		log_info_color(SuccessLogColor, TOOL_NAME, "Saved screenshot to '%s'", OutputFile.c_str());
		ReturnCode = 0;
	}

	View.Shutdown();
	return ReturnCode;
}
