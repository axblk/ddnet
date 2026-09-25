#include <base/fs.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/client/viewer_gestures.h>
#if defined(CONF_WEB_PLATFORM)
#include <engine/client/web/window_web.h>
#else
#include <engine/client/viewer_controls.h>
#include <engine/client/viewer_fullscreen.h>
#include <engine/client/window_sdl.h>
#endif
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
#include <utility>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

static constexpr const char *TOOL_NAME = "map_viewer";

using namespace std::chrono_literals;

namespace
{
	constexpr int DEFAULT_WIDTH = 1280;
	constexpr int DEFAULT_HEIGHT = 720;
	// What one notch of the wheel does, and how fast the keys move the view: a
	// screen width every second and a half, whatever the zoom.
	constexpr float ZOOM_STEP = 1.1f;
	// A button takes a bigger step than a notch of the wheel.
	constexpr float BUTTON_ZOOM_STEP = 1.4f;
	constexpr float PAN_SCREENS_PER_SECOND = 0.66f;
	// Keeps the map findable, whatever the wheel or a page asks for.
	constexpr float MIN_ZOOM = 0.01f;
	constexpr float MAX_ZOOM = 1000.0f;
	// How long one frame spends on a picture of the whole map.
	constexpr std::chrono::nanoseconds FULL_IMAGE_BUDGET = 40ms;

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// Where a picture is written before it goes to the browser.
	constexpr const char *EXPORT_DIRECTORY = "screenshots";
#endif

	enum class EExportState
	{
		IDLE,
		PENDING,
		SUCCEEDED,
		FAILED,
	};

	// What the page or the bar asked for, done between frames: a picture is
	// drawn for itself and must not cut into a frame that is half drawn.
	struct SRequests
	{
		bool m_Fit = false;
		bool m_ExportView = false;
		bool m_ExportFullMap = false;
		EExportState m_ExportState = EExportState::IDLE;
	};

	void PrintUsage(const char *pProgramName)
	{
		log_info(TOOL_NAME, "Usage: %s [-w <width>] [-h <height>] [-o <output>] [--no-controls] [<input.map>]", pProgramName);
		log_info(TOOL_NAME, "  -w <width>     Window width (default: %d)", DEFAULT_WIDTH);
		log_info(TOOL_NAME, "  -h <height>    Window height (default: %d)", DEFAULT_HEIGHT);
		log_info(TOOL_NAME, "  -o <output>    Where F2 writes the picture (default: output.png)");
		log_info(TOOL_NAME, "  --no-controls  Leave off the controls, for whoever brings their own");
		log_info(TOOL_NAME, "Drag to move, the wheel zooms, the arrow keys move, Home fills the screen");
		log_info(TOOL_NAME, "with the map and F2 saves what is on it.");
		log_info(TOOL_NAME, "A map dropped on the window replaces the one that is shown.");
	}

	class CMapViewer
	{
		CStandaloneMapView &m_View;
		IEngineInput *m_pInput;
		std::string m_OutputFile;
#if !defined(CONF_WEB_PLATFORM)
		// In a browser the page draws the controls, next to the canvas.
		CViewerControls m_Controls;
#endif
		CViewerGestures m_Gestures;
		vec2 m_LastMousePos = vec2(0.0f, 0.0f);
		bool m_Dragging = false;
		// The name of the map, for the pictures of it.
		std::string m_MapName;
		// Where the picture of the whole map that is being drawn goes.
		std::string m_FullMapFilename;
		int m_LoadCount = 0;

		// A path that names a file where it stands is opened as it stands, the
		// rest is looked for in the data directories like any other map.
		bool LoadMapPath(const char *pPath)
		{
			if(!m_View.LoadMap(pPath, fs_is_file(pPath) ? IStorage::TYPE_ABSOLUTE : IStorage::TYPE_ALL))
				return false;
			char aName[IO_MAX_PATH_LENGTH];
			fs_split_file_extension(fs_filename(pPath), aName, sizeof(aName));
			m_MapName = aName;
			++m_LoadCount;
			FitView();
			return true;
		}

		// The window is filled rather than fitted.
		void FitView()
		{
			m_RenderParams.m_Center = m_View.MapWorldSize() / 2.0f;
			m_RenderParams.m_Zoom = m_View.FillZoom();
		}

		/**
		 * Moves the view with the keys, the pointer and the fingers.
		 *
		 * @return `false` when the window was closed.
		 */
		bool HandleInput(float FrameTime)
		{
			if(m_pInput->Update())
				return false;
			char aDroppedFile[IO_MAX_PATH_LENGTH];
			if(m_pInput->GetDropFile(aDroppedFile, sizeof(aDroppedFile)))
				LoadMapPath(aDroppedFile);

			// How far the map moves under one pixel of the pointer, which is
			// also what the keys move by, so both keep their speed at any zoom.
			IGraphics *pGraphics = m_View.Graphics();
			const float ViewWidth = m_View.ViewSize().x;
			const float WorldPerPixel = pGraphics->ScreenWidth() == 0 ? 0.0f : ViewWidth * m_RenderParams.m_Zoom / pGraphics->ScreenWidth();

			vec2 Move(0.0f, 0.0f);
			if(m_pInput->KeyIsPressed(KEY_LEFT) || m_pInput->KeyIsPressed(KEY_A))
				Move.x -= 1.0f;
			if(m_pInput->KeyIsPressed(KEY_RIGHT) || m_pInput->KeyIsPressed(KEY_D))
				Move.x += 1.0f;
			if(m_pInput->KeyIsPressed(KEY_UP) || m_pInput->KeyIsPressed(KEY_W))
				Move.y -= 1.0f;
			if(m_pInput->KeyIsPressed(KEY_DOWN) || m_pInput->KeyIsPressed(KEY_S))
				Move.y += 1.0f;
			if(Move.x != 0.0f || Move.y != 0.0f)
				m_RenderParams.m_Center += normalize(Move) * (ViewWidth * m_RenderParams.m_Zoom * PAN_SCREENS_PER_SECOND * FrameTime);

			const CViewerGestures::SResult Gesture = m_Gestures.Update(m_pInput->TouchFingerStates(), vec2(pGraphics->ScreenWidth(), pGraphics->ScreenHeight()));
			if(Gesture.m_Active)
			{
				m_RenderParams.m_Zoom = std::clamp(m_RenderParams.m_Zoom * Gesture.m_Zoom, MIN_ZOOM, MAX_ZOOM);
				m_RenderParams.m_Center -= Gesture.m_Move * WorldPerPixel;
#if !defined(CONF_WEB_PLATFORM)
				m_Controls.Show();
#endif
			}

			// In drawn pixels, so that the map keeps up with the pointer on high
			// density screens. Presses on the controls and pinches are not drags.
			const vec2 MousePos = m_pInput->NativeMousePos() * pGraphics->ScreenHiDPIScale();
#if defined(CONF_WEB_PLATFORM)
			const bool OverControls = false;
#else
			const bool OverControls = m_Controls.Hovered();
#endif
			if(m_pInput->NativeMousePressed(1) && !OverControls && !Gesture.m_Active)
			{
				if(m_Dragging)
					m_RenderParams.m_Center -= (MousePos - m_LastMousePos) * WorldPerPixel;
				m_Dragging = true;
			}
			else
			{
				m_Dragging = false;
			}
			m_LastMousePos = MousePos;

			if(m_pInput->KeyPress(KEY_MOUSE_WHEEL_UP))
				m_RenderParams.m_Zoom /= ZOOM_STEP;
			if(m_pInput->KeyPress(KEY_MOUSE_WHEEL_DOWN))
				m_RenderParams.m_Zoom *= ZOOM_STEP;
			m_RenderParams.m_Zoom = std::clamp(m_RenderParams.m_Zoom, MIN_ZOOM, MAX_ZOOM);
			if(m_pInput->KeyPress(KEY_HOME) || m_Requests.m_Fit)
				FitView();
			m_Requests.m_Fit = false;
			if(m_pInput->KeyPress(KEY_F2))
				m_Requests.m_ExportView = true;
			return true;
		}

#if !defined(CONF_WEB_PLATFORM)
		// A map is not played, so there is no bar: what the view does sits in a
		// corner, and the rest in a menu behind it.
		void RenderControls()
		{
			enum
			{
				ITEM_FULLSCREEN,
				ITEM_ZOOM_OUT,
				ITEM_ZOOM_IN,
				ITEM_FIT,
				ITEM_MENU,
				ITEM_DETAIL,
				ITEM_ENTITIES,
				ITEM_SAVE_VIEW,
				ITEM_SAVE_MAP,
				NUM_ITEMS,
			};
			CViewerControls::SItem aItems[NUM_ITEMS];
			aItems[ITEM_FULLSCREEN].m_Icon = CViewerControls::EIcon::FULLSCREEN;
			aItems[ITEM_FULLSCREEN].m_Active = ViewerFullscreen::Active(m_View.Window());
			aItems[ITEM_FULLSCREEN].m_Hidden = !ViewerFullscreen::Supported(m_View.Window());
			aItems[ITEM_ZOOM_OUT].m_Icon = CViewerControls::EIcon::MINUS;
			aItems[ITEM_ZOOM_IN].m_Icon = CViewerControls::EIcon::PLUS;
			aItems[ITEM_FIT].m_Icon = CViewerControls::EIcon::FIT;
			aItems[ITEM_MENU].m_Icon = CViewerControls::EIcon::MENU;
			aItems[ITEM_MENU].m_OpensMenu = true;
			aItems[ITEM_DETAIL].m_Icon = CViewerControls::EIcon::DETAIL;
			aItems[ITEM_DETAIL].m_Active = m_RenderParams.m_HighDetail;
			aItems[ITEM_DETAIL].m_InMenu = true;
			aItems[ITEM_ENTITIES].m_Icon = CViewerControls::EIcon::ENTITIES;
			aItems[ITEM_ENTITIES].m_Active = m_RenderParams.m_EntityOverlayVal > 0;
			aItems[ITEM_ENTITIES].m_InMenu = true;
			aItems[ITEM_SAVE_VIEW].m_Icon = CViewerControls::EIcon::SAVE;
			aItems[ITEM_SAVE_VIEW].m_InMenu = true;
			aItems[ITEM_SAVE_MAP].m_Icon = CViewerControls::EIcon::SAVE_ALL;
			aItems[ITEM_SAVE_MAP].m_InMenu = true;
			const bool Busy = m_Requests.m_ExportView || m_Requests.m_ExportFullMap || m_View.FullImageRunning();
			aItems[ITEM_SAVE_VIEW].m_Disabled = Busy;
			aItems[ITEM_SAVE_MAP].m_Disabled = Busy;

			CViewerControls::SInput ControlsInput;
			ControlsInput.m_MousePos = m_pInput->NativeMousePos();
			ControlsInput.m_MousePressed = m_pInput->NativeMousePressed(1);
			ControlsInput.m_MouseClicked = m_pInput->KeyPress(KEY_MOUSE_1);
			switch(m_Controls.Render(aItems, NUM_ITEMS, ControlsInput, nullptr))
			{
			case ITEM_FULLSCREEN:
				ViewerFullscreen::Toggle(m_View.Window());
				break;
			case ITEM_ZOOM_OUT:
				m_RenderParams.m_Zoom = std::clamp(m_RenderParams.m_Zoom * BUTTON_ZOOM_STEP, MIN_ZOOM, MAX_ZOOM);
				break;
			case ITEM_ZOOM_IN:
				m_RenderParams.m_Zoom = std::clamp(m_RenderParams.m_Zoom / BUTTON_ZOOM_STEP, MIN_ZOOM, MAX_ZOOM);
				break;
			case ITEM_FIT:
				FitView();
				break;
			case ITEM_DETAIL:
				m_RenderParams.m_HighDetail = !m_RenderParams.m_HighDetail;
				break;
			case ITEM_ENTITIES:
				// All of it or none.
				m_RenderParams.m_EntityOverlayVal = m_RenderParams.m_EntityOverlayVal > 0 ? 0 : 100;
				break;
			case ITEM_SAVE_VIEW:
				m_Requests.m_ExportView = true;
				m_Requests.m_ExportState = EExportState::PENDING;
				break;
			case ITEM_SAVE_MAP:
				m_Requests.m_ExportFullMap = true;
				m_Requests.m_ExportState = EExportState::PENDING;
				break;
			default:
				break;
			}
		}
#endif

		// Pictures go to the downloads in a browser and to the file named on
		// the command line elsewhere, the whole map beside it under its own
		// name.
		void FinishFullMap(bool Success)
		{
			if(Success)
			{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
				m_View.Storage()->SendFileToUser(m_FullMapFilename.c_str(), IStorage::TYPE_SAVE);
#else
				log_info_color(LOG_COLOR{0, 255, 128}, TOOL_NAME, "Saved screenshot to '%s'", m_FullMapFilename.c_str());
#endif
			}
			m_Requests.m_ExportState = Success ? EExportState::SUCCEEDED : EExportState::FAILED;
		}

		void SaveImage(bool FullMap)
		{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
			char aFilename[IO_MAX_PATH_LENGTH];
			str_format(aFilename, sizeof(aFilename), "%s/%s%s.png", EXPORT_DIRECTORY,
				m_MapName.empty() ? "map" : m_MapName.c_str(), FullMap ? "-full" : "");
			m_View.Storage()->CreateFolder(EXPORT_DIRECTORY, IStorage::TYPE_SAVE);
			char aPath[IO_MAX_PATH_LENGTH];
			m_View.Storage()->GetCompletePath(IStorage::TYPE_SAVE, aFilename, aPath, sizeof(aPath));
			if(FullMap)
			{
				// Drawn over the frames that follow, so that the page keeps
				// painting.
				m_FullMapFilename = aFilename;
				if(!m_View.BeginFullImage(aPath, m_RenderParams.m_TimeOffsetMillis, CStandaloneMapView::VIEWER_FULL_IMAGE_PIXELS))
					m_Requests.m_ExportState = EExportState::FAILED;
				return;
			}
			const bool Success = m_View.SaveImage(m_RenderParams, aPath);
			if(Success)
				m_View.Storage()->SendFileToUser(aFilename, IStorage::TYPE_SAVE);
#else
			std::string Path = m_OutputFile;
			if(FullMap)
			{
				const size_t Dot = Path.find_last_of('.');
				Path.insert(Dot == std::string::npos ? Path.size() : Dot, "-full");
				m_FullMapFilename = Path;
				if(!m_View.BeginFullImage(Path.c_str(), m_RenderParams.m_TimeOffsetMillis, CStandaloneMapView::VIEWER_FULL_IMAGE_PIXELS))
					m_Requests.m_ExportState = EExportState::FAILED;
				return;
			}
			const bool Success = m_View.SaveImage(m_RenderParams, Path.c_str());
			if(Success)
				log_info_color(LOG_COLOR{0, 255, 128}, TOOL_NAME, "Saved screenshot to '%s'", Path.c_str());
#endif
			m_Requests.m_ExportState = Success ? EExportState::SUCCEEDED : EExportState::FAILED;
		}

	public:
		// What the page may set between frames, see the functions below.
		CStandaloneMapView::SRenderParams m_RenderParams;
		SRequests m_Requests;
		bool m_ShowControls;

		CMapViewer(CStandaloneMapView &View, IEngineInput *pInput, std::string OutputFile, bool ShowControls) :
			m_View(View), m_pInput(pInput), m_OutputFile(std::move(OutputFile)), m_ShowControls(ShowControls)
		{
			// Only shapes, no letters: a font would be the one thing needed
			// from `data/`.
#if !defined(CONF_WEB_PLATFORM)
			m_Controls.Init(m_View.Graphics(), nullptr);
			m_Controls.SetPlacement(CViewerControls::EPlacement::CORNER);
#endif
		}

		CStandaloneMapView &View() { return m_View; }
		/**
		 * How many maps were opened so far, which tells a page that asked
		 * for another when it is there.
		 */
		int LoadCount() const { return m_View.MapLoaded() ? m_LoadCount : 0; }

		int Run(const std::string &InputMap)
		{
			if(!InputMap.empty() && !LoadMapPath(InputMap.c_str()))
				return 1;
			FitView();

			const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
			std::chrono::nanoseconds LastFrameTime = StartTime;
			std::chrono::nanoseconds NextFrameTime{};
			while(true)
			{
				const std::chrono::nanoseconds Now = time_get_nanoseconds();
				const float FrameTime = std::chrono::duration_cast<std::chrono::duration<float>>(Now - LastFrameTime).count();
				LastFrameTime = Now;
				if(!HandleInput(FrameTime))
					break;
				// The envelopes of a map move, so the view runs the clock the
				// client runs and shows the map as it would look in it.
				m_RenderParams.m_TimeOffsetMillis = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
				m_View.Render(m_RenderParams);
#if !defined(CONF_WEB_PLATFORM)
				if(m_ShowControls && m_View.MapLoaded())
					RenderControls();
#endif
				// Until cleared, a wheel notch stays pressed and keeps zooming.
				m_pInput->Clear();
				m_View.Graphics()->Swap();

				// Asked for between frames and done here, where no frame is
				// half drawn.
				if(m_Requests.m_ExportView || m_Requests.m_ExportFullMap)
				{
					const bool FullMap = m_Requests.m_ExportFullMap;
					m_Requests.m_ExportView = false;
					m_Requests.m_ExportFullMap = false;
					SaveImage(FullMap);
				}
				// A picture of the whole map is drawn a little at a time
				// between frames.
				else if(m_View.FullImageRunning() && !m_View.StepFullImage(FULL_IMAGE_BUDGET))
					FinishFullMap(!m_View.FullImageFailed());

				// No point drawing faster than the screen shows, and in a
				// browser this is when the page gets to paint and answer.
				thread_sleep_until_next_frame(NextFrameTime, g_Config.m_ClRefreshRate);
			}
			return 0;
		}
	};

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// The one viewer of the page, for the controls beside the canvas.
	CMapViewer *gs_pMapViewer = nullptr;
#endif
} // namespace

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// What the controls beside the canvas call. Safe to call before a map is
// shown.
extern "C" {

EMSCRIPTEN_KEEPALIVE void MapViewerFit()
{
	if(gs_pMapViewer != nullptr)
		gs_pMapViewer->m_Requests.m_Fit = true;
}

EMSCRIPTEN_KEEPALIVE void MapViewerExportView()
{
	if(gs_pMapViewer != nullptr && gs_pMapViewer->m_Requests.m_ExportState != EExportState::PENDING)
	{
		gs_pMapViewer->m_Requests.m_ExportView = true;
		gs_pMapViewer->m_Requests.m_ExportState = EExportState::PENDING;
	}
}

EMSCRIPTEN_KEEPALIVE void MapViewerExportFullMap()
{
	if(gs_pMapViewer != nullptr && gs_pMapViewer->m_Requests.m_ExportState != EExportState::PENDING)
	{
		gs_pMapViewer->m_Requests.m_ExportFullMap = true;
		gs_pMapViewer->m_Requests.m_ExportState = EExportState::PENDING;
	}
}

// For a viewer in a box of the page's own; one that fills the window follows it.
EMSCRIPTEN_KEEPALIVE void MapViewerSetSize(int Width, int Height)
{
	if(gs_pMapViewer == nullptr || gs_pMapViewer->View().Window() == nullptr)
		return;
	gs_pMapViewer->View().Window()->Resize(std::max(Width, 1), std::max(Height, 1), g_Config.m_GfxScreenRefreshRate);
}

// Whether the viewer draws its own controls over the map.
EMSCRIPTEN_KEEPALIVE void MapViewerSetControls(int Show)
{
	if(gs_pMapViewer != nullptr)
		gs_pMapViewer->m_ShowControls = Show != 0;
}

EMSCRIPTEN_KEEPALIVE int MapViewerControls()
{
	return gs_pMapViewer != nullptr && gs_pMapViewer->m_ShowControls ? 1 : 0;
}

// Where the view looks and how close, in world units - 32 to a tile. Only the
// state the next frame is drawn from, so it is set right away.
EMSCRIPTEN_KEEPALIVE void MapViewerSetCenter(float X, float Y)
{
	if(gs_pMapViewer != nullptr)
		gs_pMapViewer->m_RenderParams.m_Center = vec2(X, Y);
}

EMSCRIPTEN_KEEPALIVE void MapViewerSetZoom(float Zoom)
{
	if(gs_pMapViewer != nullptr)
		gs_pMapViewer->m_RenderParams.m_Zoom = std::clamp(Zoom, MIN_ZOOM, MAX_ZOOM);
}

// The detail layers and the entity overlay.
EMSCRIPTEN_KEEPALIVE void MapViewerSetHighDetail(int On)
{
	if(gs_pMapViewer != nullptr)
		gs_pMapViewer->m_RenderParams.m_HighDetail = On != 0;
}

EMSCRIPTEN_KEEPALIVE int MapViewerHighDetail()
{
	return gs_pMapViewer != nullptr && gs_pMapViewer->m_RenderParams.m_HighDetail ? 1 : 0;
}

// All of it or none.
EMSCRIPTEN_KEEPALIVE void MapViewerSetEntities(int On)
{
	if(gs_pMapViewer != nullptr)
		gs_pMapViewer->m_RenderParams.m_EntityOverlayVal = On != 0 ? 100 : 0;
}

EMSCRIPTEN_KEEPALIVE int MapViewerEntities()
{
	return gs_pMapViewer != nullptr && gs_pMapViewer->m_RenderParams.m_EntityOverlayVal > 0 ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE float MapViewerCenterX()
{
	return gs_pMapViewer == nullptr ? 0.0f : gs_pMapViewer->m_RenderParams.m_Center.x;
}

EMSCRIPTEN_KEEPALIVE float MapViewerCenterY()
{
	return gs_pMapViewer == nullptr ? 0.0f : gs_pMapViewer->m_RenderParams.m_Center.y;
}

EMSCRIPTEN_KEEPALIVE float MapViewerZoom()
{
	return gs_pMapViewer == nullptr ? 1.0f : gs_pMapViewer->m_RenderParams.m_Zoom;
}

// How wide the piece of the world on the screen is, which unlike the zoom
// means the same in every window.
EMSCRIPTEN_KEEPALIVE float MapViewerVisibleWidth()
{
	return gs_pMapViewer == nullptr ? 0.0f : gs_pMapViewer->View().ViewSize().x * gs_pMapViewer->m_RenderParams.m_Zoom;
}

// The size of the map, or of the screen where it has no game layer.
EMSCRIPTEN_KEEPALIVE float MapViewerMapWidth()
{
	return gs_pMapViewer == nullptr ? 0.0f : gs_pMapViewer->View().MapWorldSize().x;
}

EMSCRIPTEN_KEEPALIVE float MapViewerMapHeight()
{
	return gs_pMapViewer == nullptr ? 0.0f : gs_pMapViewer->View().MapWorldSize().y;
}

// Tells a page that asked for another map when it is there.
EMSCRIPTEN_KEEPALIVE int MapViewerLoadCount()
{
	return gs_pMapViewer == nullptr ? 0 : gs_pMapViewer->LoadCount();
}

// 0 while nothing was ever asked for, 1 while a picture is being made, 2 when
// the last one was handed over and 3 when it failed.
EMSCRIPTEN_KEEPALIVE int MapViewerExportState()
{
	return gs_pMapViewer == nullptr ? 0 : (int)gs_pMapViewer->m_Requests.m_ExportState;
}

// How far a picture of the whole map has got, from 0 to 1.
EMSCRIPTEN_KEEPALIVE float MapViewerExportProgress()
{
	if(gs_pMapViewer == nullptr || !gs_pMapViewer->View().FullImageRunning())
		return 0.0f;
	return gs_pMapViewer->View().FullImageProgress();
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
	bool ShowControls = true;
	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
			Width = std::max(1, atoi(argv[++i]));
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
			Height = std::max(1, atoi(argv[++i]));
		else if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
			OutputFile = argv[++i];
		else if(str_comp(argv[i], "--no-controls") == 0)
			ShowControls = false;
		else if(argv[i][0] != '-' && InputMap.empty())
			InputMap = argv[i];
		else
		{
			PrintUsage(argv[0]);
			return 1;
		}
	}

	CStandaloneMapView View(TOOL_NAME);
	if(!View.Init(argc, argv))
		return 1;

	// The input reads the settings and talks to the console, so both are in
	// the kernel before the window opens; registering the settings puts their
	// defaults in place.
	IConsole *pConsole = CreateConsole(CFGFLAG_CLIENT).release();
	View.Kernel()->RegisterInterface(pConsole);
	IConfigManager *pConfigManager = CreateConfigManager();
	View.Kernel()->RegisterInterface(pConfigManager);
	pConsole->Init();
	pConfigManager->Init();

#if defined(CONF_WEB_PLATFORM)
	IEngineGraphicsWindow *pWindow = CreateWebGraphicsWindow();
#else
	IEngineGraphicsWindow *pWindow = CreateSdlGraphicsWindow();
#endif
	if(!View.OpenWindow(Width, Height, pWindow, true))
		return 1;

	IEngineInput *pInput = CreateEngineInput();
	View.Kernel()->RegisterInterface(pInput);
	View.Kernel()->RegisterInterface(static_cast<IInput *>(pInput), false);
	pInput->Init();
	// The pointer is what moves the map, so it stays where the window system
	// put it.
	pInput->MouseModeAbsolute();

	IGraphics *pGraphics = View.Graphics();
	pGraphics->AddWindowResizeListener([&] { View.OnResize(pGraphics->ScreenWidth(), pGraphics->ScreenHeight()); });

	CMapViewer Viewer(View, pInput, OutputFile, ShowControls);
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	gs_pMapViewer = &Viewer;
#endif
	const int ExitCode = Viewer.Run(InputMap);
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	gs_pMapViewer = nullptr;
#endif
	pInput->Shutdown();
	View.Shutdown();
	return ExitCode;
}
