#include <base/fs.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/client/window_sdl.h>
#include <engine/config.h>
#include <engine/console.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/input.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/map/standalone/map_view.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

static constexpr const char *TOOL_NAME = "map_viewer";

namespace
{
	constexpr int DEFAULT_WIDTH = 1280;
	constexpr int DEFAULT_HEIGHT = 720;
	// What one notch of the wheel does, and how fast the keys move the view: a
	// screen width every second and a half, whatever the zoom.
	constexpr float ZOOM_STEP = 1.1f;
	constexpr float PAN_SCREENS_PER_SECOND = 0.66f;
	// How far the view can be taken either way, so that neither a wheel that
	// keeps turning nor a page that asks for anything can put the map where it
	// is no longer to be found.
	constexpr float MIN_ZOOM = 0.01f;
	constexpr float MAX_ZOOM = 1000.0f;

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// Where a picture the page asked for is written before it is handed to the
	// browser, which needs a file it can read back.
	constexpr const char *EXPORT_DIRECTORY = "screenshots";

	enum class EExportState
	{
		IDLE,
		PENDING,
		SUCCEEDED,
		FAILED,
	};

	// What the page around the canvas asks of the view, and what came of it.
	// The asking and the doing are apart on purpose: a request arrives while
	// the loop is between two frames, and a page that drew from there would
	// cut into a frame that is already half drawn.
	struct SPageRequests
	{
		bool m_Fit = false;
		bool m_ExportView = false;
		bool m_ExportFullMap = false;
		EExportState m_ExportState = EExportState::IDLE;
	};

	CStandaloneMapView *g_pView = nullptr;
	SPageRequests *g_pRequests = nullptr;
	CStandaloneMapView::SRenderParams *g_pRenderParams = nullptr;
#endif

	void PrintUsage(const char *pProgramName)
	{
		log_info(TOOL_NAME, "Usage: %s [-w <width>] [-h <height>] [-o <output>] [<input.map>]", pProgramName);
		log_info(TOOL_NAME, "  -w <width>   Window width (default: %d)", DEFAULT_WIDTH);
		log_info(TOOL_NAME, "  -h <height>  Window height (default: %d)", DEFAULT_HEIGHT);
		log_info(TOOL_NAME, "  -o <output>  Where F2 writes the picture (default: output.png)");
		log_info(TOOL_NAME, "Drag to move, the wheel zooms, the arrow keys move, Home fits the whole");
		log_info(TOOL_NAME, "map on the screen, F2 saves what is on it and Escape closes the window.");
		log_info(TOOL_NAME, "A map dropped on the window replaces the one that is shown.");
	}
} // namespace

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// What the controls beside the canvas call. A page shows no key bindings, so
// everything the window does with a key has a button there as well.
extern "C" {

EMSCRIPTEN_KEEPALIVE void MapViewerFit()
{
	if(g_pRequests != nullptr)
		g_pRequests->m_Fit = true;
}

EMSCRIPTEN_KEEPALIVE void MapViewerExportView()
{
	if(g_pRequests != nullptr && g_pRequests->m_ExportState != EExportState::PENDING)
	{
		g_pRequests->m_ExportView = true;
		g_pRequests->m_ExportState = EExportState::PENDING;
	}
}

EMSCRIPTEN_KEEPALIVE void MapViewerExportFullMap()
{
	if(g_pRequests != nullptr && g_pRequests->m_ExportState != EExportState::PENDING)
	{
		g_pRequests->m_ExportFullMap = true;
		g_pRequests->m_ExportState = EExportState::PENDING;
	}
}

// Where the view looks and how close, in the world units of the map - 32 of
// them to a tile. Unlike a picture, which needs a frame to be drawn before it
// can be read back, this is only the state the next frame is drawn from, so it
// is set where it is asked for: the page gets to call in between two frames,
// which is exactly when that state is nobody else's.
EMSCRIPTEN_KEEPALIVE void MapViewerSetCenter(float X, float Y)
{
	if(g_pRenderParams != nullptr)
		g_pRenderParams->m_Center = vec2(X, Y);
}

EMSCRIPTEN_KEEPALIVE void MapViewerSetZoom(float Zoom)
{
	if(g_pRenderParams != nullptr)
		g_pRenderParams->m_Zoom = std::clamp(Zoom, MIN_ZOOM, MAX_ZOOM);
}

EMSCRIPTEN_KEEPALIVE float MapViewerCenterX()
{
	return g_pRenderParams == nullptr ? 0.0f : g_pRenderParams->m_Center.x;
}

EMSCRIPTEN_KEEPALIVE float MapViewerCenterY()
{
	return g_pRenderParams == nullptr ? 0.0f : g_pRenderParams->m_Center.y;
}

EMSCRIPTEN_KEEPALIVE float MapViewerZoom()
{
	return g_pRenderParams == nullptr ? 1.0f : g_pRenderParams->m_Zoom;
}

// How wide the piece of the world on the screen is. That is what a zoom
// actually means to whoever is looking, and it says the same thing in every
// window, which the zoom itself does not: a link that names it shows the same
// piece of the map on a phone as on a screen.
EMSCRIPTEN_KEEPALIVE float MapViewerVisibleWidth()
{
	if(g_pView == nullptr || g_pRenderParams == nullptr)
		return 0.0f;
	IGraphics *pGraphics = g_pView->Graphics();
	float ViewWidth, ViewHeight;
	pGraphics->CalcScreenParams(pGraphics->ScreenAspect(), 1.0f, &ViewWidth, &ViewHeight);
	return ViewWidth * g_pRenderParams->m_Zoom;
}

// How big the map is, which is what tells a page whether where it is looking
// is anywhere near it. Without a game layer there is no such size, and the
// view answers with the size of the screen instead.
EMSCRIPTEN_KEEPALIVE float MapViewerMapWidth()
{
	return g_pView == nullptr ? 0.0f : g_pView->MapWorldSize().x;
}

EMSCRIPTEN_KEEPALIVE float MapViewerMapHeight()
{
	return g_pView == nullptr ? 0.0f : g_pView->MapWorldSize().y;
}

EMSCRIPTEN_KEEPALIVE int MapViewerMapLoaded()
{
	return g_pView != nullptr && g_pView->MapLoaded() ? 1 : 0;
}

// 0 while nothing was ever asked for, 1 while a picture is being made, 2 when
// the last one was handed over and 3 when it failed.
EMSCRIPTEN_KEEPALIVE int MapViewerExportState()
{
	return g_pRequests == nullptr ? 0 : (int)g_pRequests->m_ExportState;
}
}
#endif

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	int Width = DEFAULT_WIDTH;
	int Height = DEFAULT_HEIGHT;
	std::string OutputFile = "output.png";
	std::string InputMap;
	bool InvalidUsage = false;

	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
		{
			Width = std::max(1, atoi(argv[++i]));
		}
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
		{
			Height = std::max(1, atoi(argv[++i]));
		}
		else if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
		{
			OutputFile = argv[++i];
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

	if(InvalidUsage)
	{
		PrintUsage(argv[0]);
		return 1;
	}

	CStandaloneMapView View(TOOL_NAME);
	if(!View.Init(argc, argv))
		return 1;

	// The input reads the settings and talks to the console, so both of them
	// are in the kernel before the window opens - registering the settings is
	// what puts their defaults in place.
	IConsole *pConsole = CreateConsole(CFGFLAG_CLIENT).release();
	View.Kernel()->RegisterInterface(pConsole);
	IConfigManager *pConfigManager = CreateConfigManager();
	View.Kernel()->RegisterInterface(pConfigManager);
	pConsole->Init();
	pConfigManager->Init();

	// The same way out the client has where there is no display: it draws into
	// a surface on no screen. Nobody can look around in one, so all that is
	// left of the program there is the one picture it can write.
	const bool Surfaceless = std::getenv("GFX_SURFACELESS") != nullptr;
	if(!View.OpenWindow(Width, Height, Surfaceless ? CreateOffscreenGraphicsWindow() : CreateSdlGraphicsWindow(), !Surfaceless))
		return 1;

	IEngineInput *pInput = nullptr;
	if(!Surfaceless)
	{
		pInput = CreateEngineInput();
		View.Kernel()->RegisterInterface(pInput);
		View.Kernel()->RegisterInterface(static_cast<IInput *>(pInput), false);
		pInput->Init();
		// There is a pointer on the screen here and it is what moves the map,
		// so it stays where the window system put it.
		pInput->MouseModeAbsolute();
	}

	IGraphics *pGraphics = View.Graphics();
	pGraphics->AddWindowResizeListener([&] { View.OnResize(pGraphics->ScreenWidth(), pGraphics->ScreenHeight()); });

	// A path that names a file where it stands is opened as it stands; the
	// rest is looked for in the data directories, like any other map.
	std::string MapName;
	const auto &&LoadMapPath = [&](const char *pPath) {
		if(!View.LoadMap(pPath, fs_is_file(pPath) ? IStorage::TYPE_ABSOLUTE : IStorage::TYPE_ALL))
			return false;
		char aName[IO_MAX_PATH_LENGTH];
		fs_split_file_extension(fs_filename(pPath), aName, sizeof(aName));
		MapName = aName;
		return true;
	};
	if(!InputMap.empty() && !LoadMapPath(InputMap.c_str()))
		return 1;

	CStandaloneMapView::SRenderParams RenderParams;
	const auto &&FitView = [&]() {
		RenderParams.m_Center = View.MapWorldSize() / 2.0f;
		RenderParams.m_Zoom = View.FitZoom();
	};
	FitView();

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	SPageRequests Requests;
	g_pView = &View;
	g_pRequests = &Requests;
	g_pRenderParams = &RenderParams;
	// A picture the page asked for goes to where the user's own files live and
	// is handed to the browser from there, which is how everything else this
	// build writes leaves it.
	const auto &&ExportForPage = [&](bool FullMap) {
		char aFilename[IO_MAX_PATH_LENGTH];
		str_format(aFilename, sizeof(aFilename), "%s/%s%s.png", EXPORT_DIRECTORY,
			MapName.empty() ? "map" : MapName.c_str(), FullMap ? "-full" : "");
		View.Storage()->CreateFolder(EXPORT_DIRECTORY, IStorage::TYPE_SAVE);
		char aPath[IO_MAX_PATH_LENGTH];
		View.Storage()->GetCompletePath(IStorage::TYPE_SAVE, aFilename, aPath, sizeof(aPath));
		const bool Success = FullMap ? View.SaveFullImage(aPath, RenderParams.m_TimeOffsetMillis) : View.SaveImage(aPath);
		if(Success)
			View.Storage()->SendFileToUser(aFilename, IStorage::TYPE_SAVE);
		Requests.m_ExportState = Success ? EExportState::SUCCEEDED : EExportState::FAILED;
	};
#endif

	const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
	std::chrono::nanoseconds LastFrameTime = StartTime;
	std::chrono::nanoseconds NextFrameTime{};
	vec2 LastMousePos = vec2(0.0f, 0.0f);
	bool Dragging = false;
	while(true)
	{
		if(pInput != nullptr && pInput->Update())
			break;

		const std::chrono::nanoseconds Now = time_get_nanoseconds();
		const float FrameTime = std::chrono::duration_cast<std::chrono::duration<float>>(Now - LastFrameTime).count();
		LastFrameTime = Now;
		// The envelopes of a map move, so the view runs the same clock the
		// client runs and shows the map as it would look in it.
		RenderParams.m_TimeOffsetMillis = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();

		if(pInput != nullptr)
		{
			char aDroppedFile[IO_MAX_PATH_LENGTH];
			if(pInput->GetDropFile(aDroppedFile, sizeof(aDroppedFile)) && LoadMapPath(aDroppedFile))
			{
				FitView();
			}

			if(pInput->KeyIsPressed(KEY_ESCAPE))
				break;
		}

		// How far the map moves under one pixel of the pointer, which is also
		// what the keys move by, so both stay the same speed at any zoom.
		float ViewWidth, ViewHeight;
		pGraphics->CalcScreenParams(pGraphics->ScreenAspect(), 1.0f, &ViewWidth, &ViewHeight);
		const float WorldPerPixel = pGraphics->ScreenWidth() == 0 ? 0.0f : ViewWidth * RenderParams.m_Zoom / pGraphics->ScreenWidth();

		bool SaveNow = false;
		if(pInput != nullptr)
		{
			vec2 Move(0.0f, 0.0f);
			if(pInput->KeyIsPressed(KEY_LEFT) || pInput->KeyIsPressed(KEY_A))
				Move.x -= 1.0f;
			if(pInput->KeyIsPressed(KEY_RIGHT) || pInput->KeyIsPressed(KEY_D))
				Move.x += 1.0f;
			if(pInput->KeyIsPressed(KEY_UP) || pInput->KeyIsPressed(KEY_W))
				Move.y -= 1.0f;
			if(pInput->KeyIsPressed(KEY_DOWN) || pInput->KeyIsPressed(KEY_S))
				Move.y += 1.0f;
			if(Move.x != 0.0f || Move.y != 0.0f)
				RenderParams.m_Center += normalize(Move) * (ViewWidth * RenderParams.m_Zoom * PAN_SCREENS_PER_SECOND * FrameTime);

			const vec2 MousePos = pInput->NativeMousePos();
			if(pInput->NativeMousePressed(1))
			{
				if(Dragging)
					RenderParams.m_Center -= (MousePos - LastMousePos) * WorldPerPixel;
				Dragging = true;
			}
			else
			{
				Dragging = false;
			}
			LastMousePos = MousePos;

			if(pInput->KeyPress(KEY_MOUSE_WHEEL_UP))
				RenderParams.m_Zoom /= ZOOM_STEP;
			if(pInput->KeyPress(KEY_MOUSE_WHEEL_DOWN))
				RenderParams.m_Zoom *= ZOOM_STEP;
			RenderParams.m_Zoom = std::clamp(RenderParams.m_Zoom, MIN_ZOOM, MAX_ZOOM);
			if(pInput->KeyPress(KEY_HOME))
				FitView();
			SaveNow = pInput->KeyPress(KEY_F2);
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			if(Requests.m_Fit)
			{
				Requests.m_Fit = false;
				FitView();
			}
#endif
			// What was pressed in this frame has been read, and until this is
			// said it stays pressed: a wheel is only ever a press and a release
			// in the same breath, so one notch of it would otherwise go on
			// zooming for as long as the window is open.
			pInput->Clear();
		}
		else
		{
			// Nothing can move the view here, so the first frame is the only
			// one there is a point in drawing.
			RenderParams.m_TimeOffsetMillis = 0;
			SaveNow = true;
		}

		View.Render(RenderParams);

#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// The page asks between two frames and is answered here, after one was
		// drawn: reading a frame back needs a frame to read.
		if(Requests.m_ExportView || Requests.m_ExportFullMap)
		{
			const bool FullMap = Requests.m_ExportFullMap;
			Requests.m_ExportView = false;
			Requests.m_ExportFullMap = false;
			if(FullMap)
			{
				// The frame that was just drawn goes on the screen first. What
				// follows draws the map in pieces into a target of its own, so
				// the window goes on showing the view all the while, and this
				// is the frame it shows.
				pGraphics->Swap();
			}
			ExportForPage(FullMap);
			continue;
		}
#endif

		// Reading the frame back puts it on the screen as well, so what is
		// written is the frame that was shown.
		if(SaveNow)
		{
			if(View.SaveImage(OutputFile.c_str()))
			{
				constexpr LOG_COLOR SuccessLogColor = LOG_COLOR{0, 255, 128};
				log_info_color(SuccessLogColor, TOOL_NAME, "Saved screenshot to '%s'", OutputFile.c_str());
			}
			if(pInput == nullptr)
				break;
		}
		else
		{
			pGraphics->Swap();
		}

		// Nothing here is worth drawing faster than a screen shows, and with
		// `gfx_vsync` off nothing else holds the loop back. In a browser this
		// is also the only moment the page has to paint and to answer, since a
		// thread that runs there without letting go stops both.
		thread_sleep_until_next_frame(NextFrameTime, g_Config.m_ClRefreshRate);
	}

	if(pInput != nullptr)
		pInput->Shutdown();
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pRenderParams = nullptr;
	g_pRequests = nullptr;
	g_pView = nullptr;
#endif
	View.Shutdown();
	return 0;
}
