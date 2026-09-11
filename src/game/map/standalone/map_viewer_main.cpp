#include <base/fs.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/client/viewer_controls.h>
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
	// A button is pressed once where a wheel is turned several notches, so it
	// takes a bigger step.
	constexpr float BUTTON_ZOOM_STEP = 1.4f;
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
#endif

	enum class EExportState
	{
		IDLE,
		PENDING,
		SUCCEEDED,
		FAILED,
	};

	// What is asked of the view from outside the loop - by the page around the
	// canvas, or by the bar of controls the viewer draws itself - and what came
	// of it. The asking and the doing are apart on purpose: a picture is drawn
	// in pieces and read back, and doing that from the middle of a frame would
	// cut into one that is already half drawn.
	struct SRequests
	{
		bool m_Fit = false;
		bool m_ExportView = false;
		bool m_ExportFullMap = false;
		EExportState m_ExportState = EExportState::IDLE;
	};

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	CStandaloneMapView *g_pView = nullptr;
	SRequests *g_pRequests = nullptr;
	CStandaloneMapView::SRenderParams *g_pRenderParams = nullptr;
	bool *g_pShowControls = nullptr;
#endif

	void PrintUsage(const char *pProgramName)
	{
		log_info(TOOL_NAME, "Usage: %s [-w <width>] [-h <height>] [-o <output>] [--no-controls] [<input.map>]", pProgramName);
		log_info(TOOL_NAME, "  -w <width>   Window width (default: %d)", DEFAULT_WIDTH);
		log_info(TOOL_NAME, "  -h <height>  Window height (default: %d)", DEFAULT_HEIGHT);
		log_info(TOOL_NAME, "  -o <output>  Where F2 writes the picture (default: output.png)");
		log_info(TOOL_NAME, "  --no-controls  Leave off the bar of controls, for whoever brings their own");
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

// Whether the viewer draws its own bar of controls over the map. A page with
// a bar of its own beside the canvas says so and gets a bare picture.
EMSCRIPTEN_KEEPALIVE void MapViewerSetControls(int Show)
{
	if(g_pShowControls != nullptr)
		*g_pShowControls = Show != 0;
}

EMSCRIPTEN_KEEPALIVE int MapViewerControls()
{
	return g_pShowControls != nullptr && *g_pShowControls ? 1 : 0;
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
	return g_pView->ViewSize().x * g_pRenderParams->m_Zoom;
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
	bool HideControls = false;

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
		else if(str_comp(argv[i], "--no-controls") == 0)
		{
			HideControls = true;
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
	// The viewer's own bar of controls. There are no letters on it: this
	// program needs nothing out of `data/`, and a font is the one thing that
	// would change that, so everything it offers it offers as a shape.
	CViewerControls Controls;
	Controls.Init(pGraphics, nullptr);
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

	SRequests Requests;
	bool ShowControls = !HideControls;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pView = &View;
	g_pRequests = &Requests;
	g_pRenderParams = &RenderParams;
	g_pShowControls = &ShowControls;
#endif
	// Where a picture goes: in a browser to where the user's own files live
	// and from there to the downloads, which is how everything else this build
	// writes leaves it; everywhere else the file that was named on the command
	// line, with the whole map beside it under its own name.
	const auto &&SaveImage = [&](bool FullMap) {
		bool Success;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		char aFilename[IO_MAX_PATH_LENGTH];
		str_format(aFilename, sizeof(aFilename), "%s/%s%s.png", EXPORT_DIRECTORY,
			MapName.empty() ? "map" : MapName.c_str(), FullMap ? "-full" : "");
		View.Storage()->CreateFolder(EXPORT_DIRECTORY, IStorage::TYPE_SAVE);
		char aPath[IO_MAX_PATH_LENGTH];
		View.Storage()->GetCompletePath(IStorage::TYPE_SAVE, aFilename, aPath, sizeof(aPath));
		Success = FullMap ? View.SaveFullImage(aPath, RenderParams.m_TimeOffsetMillis) : View.SaveImage(RenderParams, aPath);
		if(Success)
			View.Storage()->SendFileToUser(aFilename, IStorage::TYPE_SAVE);
#else
		std::string Path = OutputFile;
		if(FullMap)
		{
			const size_t Dot = Path.find_last_of('.');
			Path.insert(Dot == std::string::npos ? Path.size() : Dot, "-full");
		}
		Success = FullMap ? View.SaveFullImage(Path.c_str(), RenderParams.m_TimeOffsetMillis) : View.SaveImage(RenderParams, Path.c_str());
		if(Success)
		{
			constexpr LOG_COLOR SuccessLogColor = LOG_COLOR{0, 255, 128};
			log_info_color(SuccessLogColor, TOOL_NAME, "Saved screenshot to '%s'", Path.c_str());
		}
#endif
		Requests.m_ExportState = Success ? EExportState::SUCCEEDED : EExportState::FAILED;
	};

	// What is on the bar, and what pressing it does. Written here rather than
	// in the loop because it is the same every frame and reads as one thing:
	// what a map viewer offers.
	const auto &&RenderControls = [&]() {
		enum
		{
			ITEM_ZOOM_OUT,
			ITEM_ZOOM_IN,
			ITEM_FIT,
			ITEM_SPACER,
			ITEM_SAVE_VIEW,
			ITEM_SAVE_MAP,
			NUM_ITEMS,
		};
		CViewerControls::SItem aItems[NUM_ITEMS];
		aItems[ITEM_ZOOM_OUT].m_Icon = CViewerControls::EIcon::MINUS;
		aItems[ITEM_ZOOM_IN].m_Icon = CViewerControls::EIcon::PLUS;
		aItems[ITEM_FIT].m_Icon = CViewerControls::EIcon::FIT;
		// What the view does is on one side, what leaves the program on the
		// other, so that nobody saves a picture while reaching for the zoom.
		aItems[ITEM_SPACER].m_Type = CViewerControls::EItem::SPACER;
		aItems[ITEM_SAVE_VIEW].m_Icon = CViewerControls::EIcon::SAVE;
		aItems[ITEM_SAVE_MAP].m_Icon = CViewerControls::EIcon::SAVE_ALL;
		const bool Busy = Requests.m_ExportView || Requests.m_ExportFullMap;
		aItems[ITEM_SAVE_VIEW].m_Disabled = Busy;
		aItems[ITEM_SAVE_MAP].m_Disabled = Busy;

		CViewerControls::SInput ControlsInput;
		ControlsInput.m_MousePos = pInput->NativeMousePos();
		ControlsInput.m_MousePressed = pInput->NativeMousePressed(1);
		ControlsInput.m_MouseClicked = pInput->KeyPress(KEY_MOUSE_1);
		switch(Controls.Render(aItems, NUM_ITEMS, ControlsInput, nullptr))
		{
		case ITEM_ZOOM_OUT:
			RenderParams.m_Zoom = std::clamp(RenderParams.m_Zoom * BUTTON_ZOOM_STEP, MIN_ZOOM, MAX_ZOOM);
			break;
		case ITEM_ZOOM_IN:
			RenderParams.m_Zoom = std::clamp(RenderParams.m_Zoom / BUTTON_ZOOM_STEP, MIN_ZOOM, MAX_ZOOM);
			break;
		case ITEM_FIT:
			FitView();
			break;
		case ITEM_SAVE_VIEW:
			Requests.m_ExportView = true;
			Requests.m_ExportState = EExportState::PENDING;
			break;
		case ITEM_SAVE_MAP:
			Requests.m_ExportFullMap = true;
			Requests.m_ExportState = EExportState::PENDING;
			break;
		default:
			break;
		}
	};

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
		const float ViewWidth = View.ViewSize().x;
		const float WorldPerPixel = pGraphics->ScreenWidth() == 0 ? 0.0f : ViewWidth * RenderParams.m_Zoom / pGraphics->ScreenWidth();

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

			// In the pixels that are drawn, not the ones the window is
			// measured in: on a screen with more of the former the map would
			// otherwise move at half the speed of the pointer.
			const vec2 MousePos = pInput->NativeMousePos() * pGraphics->ScreenHiDPIScale();
			// A press that landed on the bar belongs to the bar. Without this
			// every button would drag the map out from under the pointer.
			if(pInput->NativeMousePressed(1) && !Controls.Hovered())
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
			if(pInput->KeyPress(KEY_F2))
				Requests.m_ExportView = true;
			if(Requests.m_Fit)
			{
				Requests.m_Fit = false;
				FitView();
			}
			// What was pressed in this frame has been read, and until this is
			// said it stays pressed: a wheel is only ever a press and a release
			// in the same breath, so one notch of it would otherwise go on
			// zooming for as long as the window is open.
			pInput->Clear();
		}
		else
		{
			// Nothing can move the view here, so the first frame is the only
			// one there is a point in drawing, and writing it is all this
			// program is here for.
			RenderParams.m_TimeOffsetMillis = 0;
			Requests.m_ExportView = true;
		}

		View.Render(RenderParams);
		if(pInput != nullptr && ShowControls && View.MapLoaded())
		{
			RenderControls();
		}
		pGraphics->Swap();

		// Asked for while the loop was between two frames, and done here where
		// no frame is half drawn: a picture is drawn for itself, in pieces for
		// the whole map, and none of that may cut into the frame that is being
		// shown.
		if(Requests.m_ExportView || Requests.m_ExportFullMap)
		{
			const bool FullMap = Requests.m_ExportFullMap;
			Requests.m_ExportView = false;
			Requests.m_ExportFullMap = false;
			SaveImage(FullMap);
			if(pInput == nullptr)
				break;
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
