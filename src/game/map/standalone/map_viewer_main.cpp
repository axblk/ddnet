#include <base/fs.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>
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
#include <utility>

static constexpr const char *TOOL_NAME = "map_viewer";

namespace
{
	constexpr int DEFAULT_WIDTH = 1280;
	constexpr int DEFAULT_HEIGHT = 720;
	// What one notch of the wheel does, and how fast the keys move the view: a
	// screen width every second and a half, whatever the zoom.
	constexpr float ZOOM_STEP = 1.1f;
	constexpr float PAN_SCREENS_PER_SECOND = 0.66f;

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

	class CMapViewer
	{
		CStandaloneMapView &m_View;
		IEngineInput *m_pInput;
		std::string m_OutputFile;
		CStandaloneMapView::SRenderParams m_RenderParams;
		vec2 m_LastMousePos = vec2(0.0f, 0.0f);
		bool m_Dragging = false;

		// A path that names a file where it stands is opened as it stands, the
		// rest is looked for in the data directories like any other map.
		bool LoadMapPath(const char *pPath)
		{
			if(!m_View.LoadMap(pPath, fs_is_file(pPath) ? IStorage::TYPE_ABSOLUTE : IStorage::TYPE_ALL))
				return false;
			FitView();
			return true;
		}

		void FitView()
		{
			m_RenderParams.m_Center = m_View.MapWorldSize() / 2.0f;
			m_RenderParams.m_Zoom = m_View.FitZoom();
		}

		/**
		 * Moves the view with the keys and the pointer.
		 *
		 * @return `false` when the window was closed.
		 */
		bool HandleInput(float FrameTime, bool &SaveNow)
		{
			if(m_pInput->Update() || m_pInput->KeyIsPressed(KEY_ESCAPE))
				return false;
			char aDroppedFile[IO_MAX_PATH_LENGTH];
			if(m_pInput->GetDropFile(aDroppedFile, sizeof(aDroppedFile)))
				LoadMapPath(aDroppedFile);

			// How far the map moves under one pixel of the pointer, which is
			// also what the keys move by, so both keep their speed at any zoom.
			IGraphics *pGraphics = m_View.Graphics();
			float ViewWidth, ViewHeight;
			pGraphics->CalcScreenParams(pGraphics->ScreenAspect(), 1.0f, &ViewWidth, &ViewHeight);
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

			const vec2 MousePos = m_pInput->NativeMousePos();
			if(m_pInput->NativeMousePressed(1))
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
			m_RenderParams.m_Zoom = std::clamp(m_RenderParams.m_Zoom, 0.01f, 1000.0f);
			if(m_pInput->KeyPress(KEY_HOME))
				FitView();
			SaveNow = m_pInput->KeyPress(KEY_F2);
			return true;
		}

	public:
		CMapViewer(CStandaloneMapView &View, IEngineInput *pInput, std::string OutputFile) :
			m_View(View), m_pInput(pInput), m_OutputFile(std::move(OutputFile)) {}

		int Run(const std::string &InputMap)
		{
			if(!InputMap.empty() && !LoadMapPath(InputMap.c_str()))
				return 1;
			FitView();

			const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
			std::chrono::nanoseconds LastFrameTime = StartTime;
			while(true)
			{
				const std::chrono::nanoseconds Now = time_get_nanoseconds();
				const float FrameTime = std::chrono::duration_cast<std::chrono::duration<float>>(Now - LastFrameTime).count();
				LastFrameTime = Now;
				bool SaveNow = false;
				if(!HandleInput(FrameTime, SaveNow))
					break;
				// The envelopes of a map move, so the view runs the clock the
				// client runs and shows the map as it would look in it.
				m_RenderParams.m_TimeOffsetMillis = std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
				m_View.Render(m_RenderParams);

				// Reading the frame back puts it on the screen as well, so what
				// is written is the frame that was shown.
				if(!SaveNow)
					m_View.Graphics()->Swap();
				else if(m_View.SaveImage(m_OutputFile.c_str()))
					log_info_color(LOG_COLOR{0, 255, 128}, TOOL_NAME, "Saved screenshot to '%s'", m_OutputFile.c_str());
			}
			return 0;
		}
	};
} // namespace

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	int Width = DEFAULT_WIDTH;
	int Height = DEFAULT_HEIGHT;
	std::string OutputFile = "output.png";
	std::string InputMap;
	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
			Width = std::max(1, atoi(argv[++i]));
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
			Height = std::max(1, atoi(argv[++i]));
		else if(str_comp(argv[i], "-o") == 0 && i + 1 < argc)
			OutputFile = argv[++i];
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

	if(!View.OpenWindow(Width, Height, CreateSdlGraphicsWindow(), true))
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

	const int ExitCode = CMapViewer(View, pInput, OutputFile).Run(InputMap);
	pInput->Shutdown();
	View.Shutdown();
	return ExitCode;
}
