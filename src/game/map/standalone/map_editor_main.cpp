#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/math.h>
#include <base/os.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/time.h>

#include <engine/client/window_sdl.h>
#include <engine/gfx/image_loader.h>
#include <engine/graphics.h>
#include <engine/graphics_window.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/map/standalone/map_editor.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#if defined(CONF_PLATFORM_EMSCRIPTEN)
#include <emscripten/emscripten.h>
#endif

static constexpr const char *TOOL_NAME = "map_editor";

namespace
{
	constexpr int DEFAULT_WIDTH = 1280;
	constexpr int DEFAULT_HEIGHT = 720;

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	constexpr const char *SAVE_DIRECTORY = "maps";

	CMapEditor *g_pEditor = nullptr;
	bool g_Quit = false;
	// What a question was answered with, kept alive until the next one is
	// asked: a page reads it out of the heap after the call has returned.
	std::string g_Answer;
	// What the page is still to be told, and is told between two frames.
	//
	// Not on the spot, because telling it means calling into the page, and
	// the page will ask questions back - which is exactly what the buffer
	// above cannot survive: the answer to the call that is still returning
	// would be overwritten by the answer to the question the listener asked.
	// Between two frames nothing is half done and nobody is waiting for an
	// answer.
	std::vector<std::pair<std::string, std::string>> g_vSaid;

	const char *Answer(std::string &&Text)
	{
		g_Answer = std::move(Text);
		return g_Answer.c_str();
	}
#endif

	void PrintUsage(const char *pProgramName)
	{
		log_info(TOOL_NAME, "Usage: %s [-w <width>] [-h <height>] [-o <output.png>] [<input.map>]", pProgramName);
		log_info(TOOL_NAME, "  -w <width>   Surface width (default: %d)", DEFAULT_WIDTH);
		log_info(TOOL_NAME, "  -h <height>  Surface height (default: %d)", DEFAULT_HEIGHT);
		log_info(TOOL_NAME, "  -o <output>  Draw the map once, write it there and stop");
		log_info(TOOL_NAME, "There is nothing to press here: the editor is driven from outside,");
		log_info(TOOL_NAME, "which on a page is the page and on the command line is -o.");
	}
} // namespace

#if defined(CONF_PLATFORM_EMSCRIPTEN)
// What the page is told, as one hook with a name and a JSON text - see the
// loader, which turns it into an event on the instance.
// clang-format off
EM_JS(void, BrowserEditorEvent, (const char *pType, const char *pJson), {
	if(typeof Module.ddnetEditorEvent === 'function')
		Module.ddnetEditorEvent(UTF8ToString(pType), UTF8ToString(pJson));
});
// clang-format on

namespace
{
	/**
	 * Says that a map changed, which is the one thing the page may not find
	 * out by asking: everything else it asks for when it needs it.
	 */
	void Say(const char *pType, const std::string &Json)
	{
		g_vSaid.emplace_back(pType, Json);
	}

	void SayChanged(int Id)
	{
		if(g_pEditor == nullptr)
			return;
		Say("document", "{\"map\":" + std::to_string(Id) + ",\"history\":" + g_pEditor->HistoryJson(Id) + "}");
	}

	/** Tells the page everything that has happened since the last frame. */
	void SayEverything()
	{
		// Taken away first: a listener that changes something adds to the
		// list while it is being walked, and that belongs to the next frame.
		std::vector<std::pair<std::string, std::string>> vSaid;
		vSaid.swap(g_vSaid);
		for(const auto &[Type, Json] : vSaid)
			BrowserEditorEvent(Type.c_str(), Json.c_str());
	}
} // namespace

// The page's side of the editor. Two sorts of call: the ones that ask - where
// the view looks, what the map is made of - and the ones that change
// something. Neither of them waits for anything, which is what makes them
// safe to call at any moment: the program gives the browser its thread back
// between two frames and while a frame is being presented, and a call that
// arrived then is answered on the spot rather than put in a queue. The two
// that do wait - reading a file and writing one - are the exceptions, and
// they say when they are done with an event.
extern "C" {

// How the loader hands a map over, the same way it hands a demo to the demo
// player: it writes the bytes where the program can read them and names the
// path. Reading it is one of the two calls here that waits, so the page hears
// what came of it as an event rather than as an answer.
EMSCRIPTEN_KEEPALIVE void EmscriptenCallbackDropFile(const char *pPath)
{
	if(g_pEditor == nullptr)
		return;
	const int Id = g_pEditor->Open(pPath, IStorage::TYPE_ABSOLUTE);
	if(Id < 0)
	{
		Say("error", std::string("{\"what\":\"open\",\"path\":\"") + pPath + "\"}");
		return;
	}
	Say("loaded", "{\"map\":" + std::to_string(Id) + "}");
}

// The page's two ways of closing the tab. There is nothing here that has to
// be finished first - a map that was not saved is the page's problem, and it
// is the page that can ask about it.
EMSCRIPTEN_KEEPALIVE void EmscriptenCallbackQuit()
{
	g_Quit = true;
}

EMSCRIPTEN_KEEPALIVE void EmscriptenCallbackQuitForce()
{
	emscripten_force_exit(-1);
}

EMSCRIPTEN_KEEPALIVE int MapEditorCreate(int Width, int Height, const char *pName)
{
	if(g_pEditor == nullptr)
		return -1;
	const int Id = g_pEditor->Create(Width, Height, pName);
	Say("loaded", "{\"map\":" + std::to_string(Id) + "}");
	return Id;
}

EMSCRIPTEN_KEEPALIVE int MapEditorClose(int Id)
{
	if(g_pEditor == nullptr || !g_pEditor->Close(Id))
		return 0;
	Say("closed", "{\"map\":" + std::to_string(Id) + "}");
	return 1;
}

EMSCRIPTEN_KEEPALIVE int MapEditorCount()
{
	return g_pEditor == nullptr ? 0 : (int)g_pEditor->Count();
}

EMSCRIPTEN_KEEPALIVE int MapEditorIdAt(int Index)
{
	return g_pEditor == nullptr || Index < 0 ? -1 : g_pEditor->IdAt((size_t)Index);
}

EMSCRIPTEN_KEEPALIVE int MapEditorActive()
{
	return g_pEditor == nullptr ? -1 : g_pEditor->Active();
}

EMSCRIPTEN_KEEPALIVE int MapEditorSetActive(int Id)
{
	return g_pEditor != nullptr && g_pEditor->SetActive(Id) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorName(int Id)
{
	return g_pEditor == nullptr ? "" : g_pEditor->Name(Id);
}

// Writing the map out is one of the two calls that wait: the file is written,
// closed and handed to the browser, and the page hears about it afterwards.
EMSCRIPTEN_KEEPALIVE int MapEditorSave(int Id)
{
	if(g_pEditor == nullptr || g_pEditor->Document(Id) == nullptr)
		return 0;
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "%s/%s.map", SAVE_DIRECTORY, g_pEditor->Name(Id));
	g_pEditor->Storage()->CreateFolder(SAVE_DIRECTORY, IStorage::TYPE_SAVE);
	if(!g_pEditor->Save(Id, aFilename, IStorage::TYPE_SAVE))
	{
		Say("error", "{\"what\":\"save\",\"map\":" + std::to_string(Id) + "}");
		return 0;
	}
	// Into the browser's own storage, so that a map survives the tab, and
	// then out to wherever the user keeps their files.
	g_pEditor->Storage()->SyncPersistentStorage();
	g_pEditor->Storage()->SendFileToUser(aFilename, IStorage::TYPE_SAVE);
	Say("saved", "{\"map\":" + std::to_string(Id) + "}");
	return 1;
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorApply(int Id, const char *pJson)
{
	if(g_pEditor == nullptr)
		return R"({"ok":false,"error":"The editor is not running"})";
	const char *pAnswer = Answer(g_pEditor->Apply(Id, pJson));
	SayChanged(Id);
	return pAnswer;
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorStructure(int Id)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->StructureJson(Id));
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorHistory(int Id)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->HistoryJson(Id));
}

// A change that is made over many calls - a slider being dragged, a brush
// being drawn with - is one entry in the history and many previews. The page
// opens it, changes what it likes, and closes it when the pointer is let go.
EMSCRIPTEN_KEEPALIVE void MapEditorBegin(int Id, const char *pLabel)
{
	if(g_pEditor != nullptr && g_pEditor->Document(Id) != nullptr)
		g_pEditor->Document(Id)->Begin(pLabel);
}

EMSCRIPTEN_KEEPALIVE void MapEditorCommit(int Id)
{
	if(g_pEditor == nullptr || g_pEditor->Document(Id) == nullptr || !g_pEditor->Document(Id)->IsEditing())
		return;
	g_pEditor->Document(Id)->Commit();
	g_pEditor->Touch();
	SayChanged(Id);
}

EMSCRIPTEN_KEEPALIVE void MapEditorAbort(int Id)
{
	if(g_pEditor == nullptr || g_pEditor->Document(Id) == nullptr || !g_pEditor->Document(Id)->IsEditing())
		return;
	g_pEditor->Document(Id)->Abort();
	g_pEditor->Touch();
	SayChanged(Id);
}

// How big to draw. The page measures the box it gave the editor and says so;
// nothing else here ever asks the window how large it is.
EMSCRIPTEN_KEEPALIVE void MapEditorSetSize(int Width, int Height)
{
	if(g_pEditor == nullptr || g_pEditor->Surface().Window() == nullptr)
		return;
	g_pEditor->Surface().Window()->Resize(std::max(Width, 1), std::max(Height, 1), g_Config.m_GfxScreenRefreshRate);
}

EMSCRIPTEN_KEEPALIVE void MapEditorSetCenter(int Id, float X, float Y)
{
	if(g_pEditor != nullptr && g_pEditor->View(Id) != nullptr)
		g_pEditor->View(Id)->SetCenter(vec2(X, Y));
}

EMSCRIPTEN_KEEPALIVE void MapEditorSetZoom(int Id, float Zoom)
{
	if(g_pEditor != nullptr && g_pEditor->View(Id) != nullptr)
		g_pEditor->View(Id)->SetZoom(Zoom);
}

EMSCRIPTEN_KEEPALIVE void MapEditorMoveByPixels(int Id, float X, float Y)
{
	if(g_pEditor != nullptr && g_pEditor->View(Id) != nullptr)
		g_pEditor->View(Id)->MoveByPixels(vec2(X, Y));
}

// The wheel, which keeps the point under the pointer where it is - anything
// else and the map crawls away from whatever somebody is looking at.
EMSCRIPTEN_KEEPALIVE void MapEditorZoomAt(int Id, float X, float Y, float Factor)
{
	if(g_pEditor != nullptr && g_pEditor->View(Id) != nullptr)
		g_pEditor->View(Id)->ZoomAt(vec2(X, Y), Factor);
}

EMSCRIPTEN_KEEPALIVE void MapEditorFit(int Id)
{
	if(g_pEditor != nullptr && g_pEditor->View(Id) != nullptr)
		g_pEditor->View(Id)->Fit(g_pEditor->WorldSize(Id));
}

EMSCRIPTEN_KEEPALIVE float MapEditorCenterX(int Id)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 0.0f : g_pEditor->View(Id)->Center().x;
}

EMSCRIPTEN_KEEPALIVE float MapEditorCenterY(int Id)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 0.0f : g_pEditor->View(Id)->Center().y;
}

EMSCRIPTEN_KEEPALIVE float MapEditorZoom(int Id)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 1.0f : g_pEditor->View(Id)->Zoom();
}

// Where a click landed, in the world and on the grid. The page does no such
// sums itself: they depend on the surface, the zoom and the shape of the
// view, and two answers that disagree by a pixel are a brush that paints
// beside the pointer.
EMSCRIPTEN_KEEPALIVE float MapEditorWorldX(int Id, float X, float Y)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 0.0f : g_pEditor->View(Id)->ScreenToWorld(vec2(X, Y)).x;
}

EMSCRIPTEN_KEEPALIVE float MapEditorWorldY(int Id, float X, float Y)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 0.0f : g_pEditor->View(Id)->ScreenToWorld(vec2(X, Y)).y;
}

EMSCRIPTEN_KEEPALIVE int MapEditorTileX(int Id, float X, float Y)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 0 : g_pEditor->View(Id)->ScreenToTile(vec2(X, Y)).x;
}

EMSCRIPTEN_KEEPALIVE int MapEditorTileY(int Id, float X, float Y)
{
	return g_pEditor == nullptr || g_pEditor->View(Id) == nullptr ? 0 : g_pEditor->View(Id)->ScreenToTile(vec2(X, Y)).y;
}

EMSCRIPTEN_KEEPALIVE float MapEditorWorldWidth(int Id)
{
	return g_pEditor == nullptr ? 0.0f : g_pEditor->WorldSize(Id).x;
}

EMSCRIPTEN_KEEPALIVE float MapEditorWorldHeight(int Id)
{
	return g_pEditor == nullptr ? 0.0f : g_pEditor->WorldSize(Id).y;
}

EMSCRIPTEN_KEEPALIVE void MapEditorSetHighDetail(int Id, int On)
{
	if(g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr)
	{
		g_pEditor->Display(Id)->m_HighDetail = On != 0;
		g_pEditor->Touch();
	}
}

EMSCRIPTEN_KEEPALIVE int MapEditorHighDetail(int Id)
{
	return g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr && g_pEditor->Display(Id)->m_HighDetail ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void MapEditorSetEntities(int Id, int Value)
{
	if(g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr)
	{
		g_pEditor->Display(Id)->m_EntityOverlayVal = std::clamp(Value, 0, 100);
		g_pEditor->Touch();
	}
}

EMSCRIPTEN_KEEPALIVE int MapEditorEntities(int Id)
{
	return g_pEditor == nullptr || g_pEditor->Display(Id) == nullptr ? 0 : g_pEditor->Display(Id)->m_EntityOverlayVal;
}

// Whether the envelopes run. An editor stands still by default, because
// somebody placing a tile beside a moving quad is fighting the map.
EMSCRIPTEN_KEEPALIVE void MapEditorSetAnimate(int Id, int On)
{
	if(g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr)
	{
		g_pEditor->Display(Id)->m_Animate = On != 0;
		g_pEditor->Touch();
	}
}

EMSCRIPTEN_KEEPALIVE int MapEditorAnimate(int Id)
{
	return g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr && g_pEditor->Display(Id)->m_Animate ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int MapEditorLoading()
{
	return g_pEditor != nullptr && g_pEditor->Loading() ? 1 : 0;
}
}
#endif

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	int Width = DEFAULT_WIDTH;
	int Height = DEFAULT_HEIGHT;
	std::string OutputFile;
	std::string InputMap;
	bool InvalidUsage = false;

	for(int i = 1; i < argc; i++)
	{
		if(str_comp(argv[i], "-w") == 0 && i + 1 < argc)
		{
			Width = std::max(1, str_toint(argv[++i]));
		}
		else if(str_comp(argv[i], "-h") == 0 && i + 1 < argc)
		{
			Height = std::max(1, str_toint(argv[++i]));
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

	// One picture and out is what this program is on the command line: there
	// is nothing to press, because the editor is driven from outside and
	// outside is a page. It draws into a surface on no screen then, which is
	// also how the picture is held against the one the map renderer draws.
	const bool OnePicture = !OutputFile.empty();
	const bool Surfaceless = OnePicture || std::getenv("GFX_SURFACELESS") != nullptr;

	CMapEditor Editor(TOOL_NAME);
	if(!Editor.Init(argc, argv))
		return 1;

	if(!Editor.OpenWindow(Width, Height, Surfaceless ? CreateOffscreenGraphicsWindow() : CreateSdlGraphicsWindow(), !Surfaceless))
		return 1;

	IGraphics *pGraphics = Editor.Graphics();
	pGraphics->AddWindowResizeListener([&] { Editor.OnResize(pGraphics->ScreenWidth(), pGraphics->ScreenHeight()); });

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pEditor = &Editor;
#endif

	if(!InputMap.empty())
	{
		const int Id = Editor.Open(InputMap.c_str(), fs_is_file(InputMap.c_str()) ? IStorage::TYPE_ABSOLUTE : IStorage::TYPE_ALL);
		if(Id < 0)
			return 1;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// The page is already listening by now: starting the graphics gives
		// the browser its thread back, and that is the moment the page was
		// answered with a running program.
		Say("loaded", "{\"map\":" + std::to_string(Id) + "}");
#endif
	}

	int ReturnCode = 0;
	const std::chrono::nanoseconds StartTime = time_get_nanoseconds();
	std::chrono::nanoseconds NextFrameTime{};
	while(true)
	{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		if(g_Quit)
			break;
		SayEverything();
#endif
		Editor.Update();

		const int Active = Editor.Active();
		CMapEditor::CDisplay *pDisplay = Editor.Display(Active);
		if(pDisplay != nullptr && pDisplay->m_Animate)
		{
			const std::chrono::nanoseconds Now = time_get_nanoseconds();
			pDisplay->m_TimeOffsetMillis = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Now - StartTime).count();
		}

		if(OnePicture)
		{
			// The pictures of the map are fetched beside each other, and a
			// picture taken before they are here is a picture of the map
			// without them.
			if(Editor.Loading())
			{
				thread_sleep_until_next_frame(NextFrameTime, g_Config.m_ClRefreshRate);
				continue;
			}
			Editor.Render();
			CImageInfo Image;
			ReturnCode = 1;
			if(Editor.Surface().ReadFrame(Image))
			{
				IOHANDLE File = io_open(OutputFile.c_str(), IOFLAG_WRITE);
				if(File == nullptr)
				{
					log_error(TOOL_NAME, "Failed to open '%s' for writing", OutputFile.c_str());
				}
				else if(!CImageLoader::SavePng(File, OutputFile.c_str(), Image))
				{
					log_error(TOOL_NAME, "Failed to save the picture to '%s'", OutputFile.c_str());
				}
				else
				{
					log_info(TOOL_NAME, "Saved the picture to '%s'", OutputFile.c_str());
					ReturnCode = 0;
				}
			}
			Image.Free();
			break;
		}

		// Nothing is drawn while nothing has changed. An editor that draws
		// sixty frames a second at a map nobody is touching empties a
		// telephone for nothing, and there is no clock running in it either
		// unless somebody asked for the envelopes to move.
		if(Editor.NeedsRedraw())
		{
			Editor.Render();
			pGraphics->Swap();
		}

		// In a browser this is the only moment the page has to paint and to
		// answer, since a thread that runs there without letting go stops
		// both.
		thread_sleep_until_next_frame(NextFrameTime, g_Config.m_ClRefreshRate);
	}

#if defined(CONF_PLATFORM_EMSCRIPTEN)
	g_pEditor = nullptr;
#endif
	Editor.Shutdown();
	return ReturnCode;
}
