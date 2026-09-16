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

#include <game/map/document/edit.h>
#include <game/map/standalone/map_editor.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
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
	constexpr const char *PICTURE_DIRECTORY = "screenshots";
	// A picture that was asked for and has not been begun yet, and how the
	// last one went: 0 never asked, 1 under way, 2 handed over, 3 failed.
	int g_PictureMap = -1;
	int g_PictureState = 0;
	std::string g_PictureFile;
	// How long a frame may spend on the picture before the page gets the
	// thread back - the same as the viewer.
	constexpr std::chrono::nanoseconds PICTURE_BUDGET = std::chrono::milliseconds(40);
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
		log_info(TOOL_NAME, "  -g <tiles>   Draw a grid every so many tiles (default: none)");
		log_info(TOOL_NAME, "  -x <g>:<l>   Leave that layer out, as an editor hiding it would");
		log_info(TOOL_NAME, "  -m <g>:<x>:<y>:<w>:<h>  Mark that rectangle of tiles");
		log_info(TOOL_NAME, "  -q <g>:<l>:<n>  Put handles on the corners of that quad");
		log_info(TOOL_NAME, "  -e <sheet>      Show what the tiles do, drawn with that entities picture (ddnet, race, fng, ...)");
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

	/**
	 * Tells the page that tiles went down as air because they do nothing
	 * there - once per frame at most, however many stamps a stroke was.
	 */
	void SayDropped()
	{
		if(g_pEditor == nullptr || g_pEditor->LastDropped() == 0)
			return;
		for(const auto &[Type, Json] : g_vSaid)
		{
			if(Type == "unused")
				return;
		}
		Say("unused", "{\"tiles\":" + std::to_string(g_pEditor->LastDropped()) + "}");
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

EMSCRIPTEN_KEEPALIVE int MapEditorRename(int Id, const char *pName)
{
	if(g_pEditor == nullptr || !g_pEditor->Rename(Id, pName))
		return 0;
	SayChanged(Id);
	return 1;
}

// Writing the map out is one of the two calls that wait: the file is written,
// closed and handed to the browser, and the page hears about it afterwards.
// Writing the map out. `Handout` says whether it also goes to wherever the
// user keeps their files: an autosave writes into the browser's own storage
// and stops there, because a browser that dropped a file into the downloads
// every minute would be a browser nobody leaves open.
EMSCRIPTEN_KEEPALIVE int MapEditorSave(int Id, int Handout)
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
	if(Handout != 0)
		g_pEditor->Storage()->SendFileToUser(aFilename, IStorage::TYPE_SAVE);
	Say("saved", "{\"map\":" + std::to_string(Id) + ",\"handout\":" + (Handout != 0 ? "true" : "false") + "}");
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

// Where a pixel of the surface is in the coordinates of one group - which for
// a group with parallax is somewhere else than the plain view says. A quad's
// points are in its group's coordinates, so this is what a pointer over one
// has to ask.
EMSCRIPTEN_KEEPALIVE float MapEditorGroupWorldX(int Id, int Group, float X, float Y)
{
	return g_pEditor == nullptr || Group < 0 ? 0.0f : g_pEditor->WorldInGroup(Id, (size_t)Group, vec2(X, Y)).x;
}

EMSCRIPTEN_KEEPALIVE float MapEditorGroupWorldY(int Id, int Group, float X, float Y)
{
	return g_pEditor == nullptr || Group < 0 ? 0.0f : g_pEditor->WorldInGroup(Id, (size_t)Group, vec2(X, Y)).y;
}

// And the other way round: where a place in a group's coordinates is on the
// surface. What an overlay drawn in the page has to ask, so that a shape over
// the map stays over the place it belongs to when the view moves.
EMSCRIPTEN_KEEPALIVE float MapEditorGroupPixelX(int Id, int Group, float X, float Y)
{
	return g_pEditor == nullptr || Group < 0 ? 0.0f : g_pEditor->PixelInGroup(Id, (size_t)Group, vec2(X, Y)).x;
}

EMSCRIPTEN_KEEPALIVE float MapEditorGroupPixelY(int Id, int Group, float X, float Y)
{
	return g_pEditor == nullptr || Group < 0 ? 0.0f : g_pEditor->PixelInGroup(Id, (size_t)Group, vec2(X, Y)).y;
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorSoundSources(int Id, int Group, int Layer)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->SoundSourcesJson(Id, Group, Layer));
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorExplain(int Id, int Group, int Layer, int Index)
{
	if(g_pEditor == nullptr)
		return "";
	const char *pExplanation = g_pEditor->Explain(Id, Group, Layer, Index);
	// An empty answer rather than nothing, because the page reads a string
	// and "there is nothing to say" is a thing to say.
	return pExplanation == nullptr ? "" : pExplanation;
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorSettingsHelp()
{
	return g_pEditor == nullptr ? "[]" : Answer(g_pEditor->SettingsHelpJson());
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorSettingProblems(int Id)
{
	return g_pEditor == nullptr ? "[]" : Answer(g_pEditor->SettingProblemsJson(Id));
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorSettingNames(const char *pPrefix)
{
	return g_pEditor == nullptr ? "[]" : Answer(g_pEditor->SettingNamesJson(pPrefix));
}

/**
 * Asks for a picture of the map in front, the whole of it.
 *
 * Begun between two frames and drawn over the ones after, like everything
 * that takes longer than a frame; `MapEditorPictureState` says how it went.
 */
EMSCRIPTEN_KEEPALIVE int MapEditorPicture(int Id)
{
	if(g_pEditor == nullptr || g_pEditor->Document(Id) == nullptr || g_pEditor->PictureRunning())
		return 0;
	g_PictureMap = Id;
	g_PictureState = 1;
	return 1;
}

/** 0 never asked, 1 being drawn, 2 handed over, 3 failed. */
EMSCRIPTEN_KEEPALIVE int MapEditorPictureState()
{
	return g_PictureState;
}

/** How far the picture has got, from 0 to 1. */
EMSCRIPTEN_KEEPALIVE float MapEditorPictureProgress()
{
	return g_pEditor == nullptr ? 0.0f : g_pEditor->PictureProgress();
}

/** The maps that are in the browser's own storage, with their sizes. */
EMSCRIPTEN_KEEPALIVE const char *MapEditorSaved()
{
	return g_pEditor == nullptr ? "[]" : Answer(g_pEditor->SavedJson(SAVE_DIRECTORY, IStorage::TYPE_SAVE));
}

/** Opens one of them by the name the list gave. */
EMSCRIPTEN_KEEPALIVE int MapEditorOpenSaved(const char *pName)
{
	if(g_pEditor == nullptr || pName == nullptr || pName[0] == '\0')
		return -1;
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "%s/%s.map", SAVE_DIRECTORY, pName);
	const int Id = g_pEditor->Open(aFilename, IStorage::TYPE_SAVE);
	if(Id < 0)
	{
		Say("error", "{\"what\":\"open\"}");
		return -1;
	}
	Say("loaded", "{\"map\":" + std::to_string(Id) + "}");
	return Id;
}

/**
 * Writes the map out under another name without becoming that map.
 *
 * What "save a copy" means: the copy is made and the map one is working on
 * is the one one was working on, with the name it had and the place in the
 * history it had. Saving *as* is a rename and then a save, and that is a
 * different thing.
 */
EMSCRIPTEN_KEEPALIVE int MapEditorSaveCopy(int Id, const char *pName, int Handout)
{
	if(g_pEditor == nullptr || g_pEditor->Document(Id) == nullptr || pName == nullptr || pName[0] == '\0')
		return 0;
	char aFilename[IO_MAX_PATH_LENGTH];
	str_format(aFilename, sizeof(aFilename), "%s/%s.map", SAVE_DIRECTORY, pName);
	g_pEditor->Storage()->CreateFolder(SAVE_DIRECTORY, IStorage::TYPE_SAVE);
	if(!g_pEditor->Save(Id, aFilename, IStorage::TYPE_SAVE))
	{
		Say("error", "{\"what\":\"save\",\"map\":" + std::to_string(Id) + "}");
		return 0;
	}
	g_pEditor->Storage()->SyncPersistentStorage();
	if(Handout != 0)
		g_pEditor->Storage()->SendFileToUser(aFilename, IStorage::TYPE_SAVE);
	// Not `saved`: the map is as changed as it was, and a dot that went away
	// because a copy was written would be a lie.
	Say("copied", "{\"map\":" + std::to_string(Id) + "}");
	return 1;
}

EMSCRIPTEN_KEEPALIVE int MapEditorTileArt(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels)
{
	return g_pEditor == nullptr ? -1 : g_pEditor->AddTileArt(Id, pName, Width, Height, pPixels);
}

EMSCRIPTEN_KEEPALIVE int MapEditorArtColors(int Width, int Height, const uint8_t *pPixels)
{
	return g_pEditor == nullptr ? 0 : g_pEditor->CountArtColors(Width, Height, pPixels);
}

EMSCRIPTEN_KEEPALIVE int MapEditorQuadArt(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels,
	int PixelStep, int QuadSize, int Centralize, int Merge)
{
	return g_pEditor == nullptr ?
		       -1 :
		       g_pEditor->AddQuadArt(Id, pName, Width, Height, pPixels, PixelStep, QuadSize, Centralize != 0, Merge != 0);
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorAppend(int Id, const char *pPath, int StorageType)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->Append(Id, pPath, StorageType));
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorCheckSetting(const char *pLine)
{
	return g_pEditor == nullptr ? "" : Answer(g_pEditor->CheckSetting(pLine));
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorProof(int Id, int Menu)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->ProofJson(Id, Menu != 0));
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorQuads(int Id, int Group, int Layer)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->QuadsJson(Id, Group, Layer));
}

// Which quad has handles on its corners. A width of nothing - `Show` of zero -
// takes them away again. Nothing about this reaches the map.
EMSCRIPTEN_KEEPALIVE void MapEditorShowQuad(int Id, int Group, int Layer, int Quad, int Show)
{
	if(g_pEditor == nullptr || g_pEditor->Display(Id) == nullptr)
		return;
	CDocumentRenderer::CParams::CShownQuad &Shown = g_pEditor->Display(Id)->m_ShownQuad;
	Shown.m_Group = (size_t)std::max(0, Group);
	Shown.m_Layer = (size_t)std::max(0, Layer);
	Shown.m_Quad = (size_t)std::max(0, Quad);
	Shown.m_Shown = Show != 0 && Group >= 0 && Layer >= 0 && Quad >= 0;
	g_pEditor->Touch();
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorEnvelope(int Id, int Index)
{
	return g_pEditor == nullptr ? "null" : Answer(g_pEditor->EnvelopeJson(Id, Index));
}

// The lowest number a physics layer is not using yet, so that a page need not
// walk the layer itself to find one.
EMSCRIPTEN_KEEPALIVE int MapEditorNextFreeNumber(int Id, int Group, int Layer, int Checkpoint)
{
	return g_pEditor == nullptr ? -1 : g_pEditor->NextFreeNumber(Id, Group, Layer, Checkpoint != 0);
}

// What tile stands in one place of a layer. One at a time, because a map of
// four million tiles is not a thing to hand out after every stroke.
EMSCRIPTEN_KEEPALIVE int MapEditorTileIndex(int Id, int Group, int Layer, int X, int Y)
{
	return g_pEditor == nullptr ? -1 : g_pEditor->TileIndex(Id, Group, Layer, X, Y);
}

// A `.rules` file, as text, kept under the name the map calls the picture -
// which is how a layer finds its rules. The file itself is not read here:
// natively it comes off the disk and in the browser the page fetches it.
EMSCRIPTEN_KEEPALIVE int MapEditorLoadRules(const char *pName, const char *pText)
{
	return g_pEditor == nullptr ? 0 : (int)g_pEditor->LoadRules(pName, pText);
}

// Which lines of a rules file were passed over, as a JSON array of line
// numbers counting from one. Empty for a file that was understood whole.
EMSCRIPTEN_KEEPALIVE const char *MapEditorRuleProblems(const char *pName)
{
	return g_pEditor == nullptr ? "[]" : Answer(g_pEditor->RuleProblems(pName));
}

EMSCRIPTEN_KEEPALIVE int MapEditorNumRuleConfigs(const char *pName)
{
	return g_pEditor == nullptr ? 0 : (int)g_pEditor->NumRuleConfigs(pName);
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorRuleConfigName(const char *pName, int Config)
{
	return g_pEditor == nullptr || Config < 0 ? "" : Answer(std::string(g_pEditor->RuleConfigName(pName, (size_t)Config)));
}

// Runs one configuration over a layer, or over a piece of one. A change of
// its own, so one press of the button is one entry in the history.
EMSCRIPTEN_KEEPALIVE int MapEditorAutomap(int Id, int Group, int Layer, const char *pRules, int Config,
	int Seed, int Reference, int X, int Y, int Width, int Height)
{
	return g_pEditor != nullptr && g_pEditor->Automap(Id, Group, Layer, pRules, Config, Seed, Reference, X, Y, Width, Height) ? 1 : 0;
}

// A picture and its pixels, which come over as bytes rather than as JSON: a
// thousand by a thousand is four megabytes, and the page has them already -
// it decoded the PNG itself, because browsers do that.
EMSCRIPTEN_KEEPALIVE int MapEditorAddImage(int Id, const char *pName, int Width, int Height, const uint8_t *pPixels)
{
	return g_pEditor == nullptr ? -1 : g_pEditor->AddImage(Id, pName, Width, Height, pPixels);
}

// Other pixels in the place of a picture's, which keeps every layer that is
// drawn with it - that is what replacing a picture is for.
EMSCRIPTEN_KEEPALIVE int MapEditorSetImagePixels(int Id, int Index, int Width, int Height, const uint8_t *pPixels)
{
	return g_pEditor != nullptr && g_pEditor->SetImagePixels(Id, Index, Width, Height, pPixels) ? 1 : 0;
}

// Whether the tiles in hand are tele checkpoints, which have a free-number
// count of their own. No Id: the brush belongs to the editor, not to a map.
EMSCRIPTEN_KEEPALIVE int MapEditorBrushCheckpoint()
{
	return g_pEditor != nullptr && g_pEditor->BrushIsCheckpoint() ? 1 : 0;
}

// Moves the view to where a number is used, and says how many such places
// there are. Which of them is the page's to count: it is the page that knows
// somebody pressed the button twice.
EMSCRIPTEN_KEEPALIVE int MapEditorGotoNumber(int Id, int Group, int Layer, int Number, int Which)
{
	return g_pEditor == nullptr ? 0 : (int)g_pEditor->GotoNumber(Id, Group, Layer, Number, (size_t)std::max(0, Which));
}

// The pixels of a picture that is packed into the map file, so that a page can
// show a tileset it cannot fetch. They lie in the version, already unpacked,
// and this hands out where - no copy is made, because the one the page makes
// when it reads them is the one copy that is needed.
//
// The address is good until the map changes. A page that keeps it instead of
// the pixels is holding a page of somebody else's memory.
EMSCRIPTEN_KEEPALIVE const uint8_t *MapEditorImagePixels(int Id, int Index)
{
	const map_document::CImage *pImage = g_pEditor == nullptr ? nullptr : g_pEditor->Image(Id, Index);
	return pImage == nullptr || pImage->m_Data.Empty() ? nullptr : &pImage->m_Data[0];
}

// The bytes of a sound, and how many. The same rule as the pixels above: no
// copy is made, and the address is good until the map changes.
EMSCRIPTEN_KEEPALIVE const uint8_t *MapEditorSoundData(int Id, int Index)
{
	const map_document::CSound *pSound = g_pEditor == nullptr ? nullptr : g_pEditor->Sound(Id, Index);
	return pSound == nullptr || pSound->m_Data.Empty() ? nullptr : &pSound->m_Data[0];
}

EMSCRIPTEN_KEEPALIVE int MapEditorSoundSize(int Id, int Index)
{
	const map_document::CSound *pSound = g_pEditor == nullptr ? nullptr : g_pEditor->Sound(Id, Index);
	return pSound == nullptr ? 0 : (int)pSound->m_Data.Size();
}

// An Opus file read into the map. Bytes rather than a command, because
// hundreds of kilobytes of JSON are a text nobody should write or read.
EMSCRIPTEN_KEEPALIVE int MapEditorAddSound(int Id, const char *pName, int Size, const uint8_t *pData)
{
	return g_pEditor == nullptr ? -1 : g_pEditor->AddSound(Id, pName, Size, pData);
}

EMSCRIPTEN_KEEPALIVE int MapEditorSetSoundData(int Id, int Index, int Size, const uint8_t *pData)
{
	return g_pEditor != nullptr && g_pEditor->SetSoundData(Id, Index, Size, pData) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int MapEditorImageWidth(int Id, int Index)
{
	const map_document::CImage *pImage = g_pEditor == nullptr ? nullptr : g_pEditor->Image(Id, Index);
	return pImage == nullptr ? 0 : pImage->m_Width;
}

EMSCRIPTEN_KEEPALIVE int MapEditorImageHeight(int Id, int Index)
{
	const map_document::CImage *pImage = g_pEditor == nullptr ? nullptr : g_pEditor->Image(Id, Index);
	return pImage == nullptr ? 0 : pImage->m_Height;
}

// A change that is made over many calls - a slider being dragged, a brush
// being drawn with - is one entry in the history and many previews. The page
// opens it, changes what it likes, and closes it when the pointer is let go.
// The merge key names what is being changed rather than what is being done:
// two changes of the same thing, close enough together, become one history
// entry. An empty one means this change stands alone, which is the usual case.
EMSCRIPTEN_KEEPALIVE void MapEditorBegin(int Id, const char *pLabel, const char *pMerge)
{
	if(g_pEditor != nullptr && g_pEditor->Document(Id) != nullptr)
		g_pEditor->Document(Id)->Begin(pLabel, pMerge);
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

// Hiding a layer is a thing about looking: it changes no version, writes no
// history entry, and is gone when the page is closed.
EMSCRIPTEN_KEEPALIVE void MapEditorSetLayerVisible(int Id, int Group, int Layer, int On)
{
	if(g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr && Group >= 0 && Layer >= 0)
	{
		g_pEditor->Display(Id)->SetVisible((size_t)Group, (size_t)Layer, On != 0);
		g_pEditor->Touch();
	}
}

EMSCRIPTEN_KEEPALIVE int MapEditorLayerVisible(int Id, int Group, int Layer)
{
	if(g_pEditor == nullptr || g_pEditor->Display(Id) == nullptr || Group < 0 || Layer < 0)
		return 1;
	return g_pEditor->Display(Id)->Visible((size_t)Group, (size_t)Layer) ? 1 : 0;
}

// How many tiles apart the lines of the grid are, 0 for no grid.
EMSCRIPTEN_KEEPALIVE void MapEditorSetGrid(int Id, int Spacing)
{
	if(g_pEditor != nullptr && g_pEditor->Display(Id) != nullptr)
	{
		g_pEditor->Display(Id)->m_Grid = std::max(0, Spacing);
		g_pEditor->Touch();
	}
}

EMSCRIPTEN_KEEPALIVE int MapEditorGrid(int Id)
{
	return g_pEditor == nullptr || g_pEditor->Display(Id) == nullptr ? 0 : g_pEditor->Display(Id)->m_Grid;
}

// What a gesture about an area is doing while it is being done: the rectangle
// is drawn in the tiles of the group it is in, so it sits on them at every
// zoom. A width or height of zero takes the mark away again.
EMSCRIPTEN_KEEPALIVE void MapEditorMark(int Id, int Group, int X, int Y, int Width, int Height)
{
	if(g_pEditor == nullptr || g_pEditor->Display(Id) == nullptr)
		return;
	CDocumentRenderer::CParams::CMarked &Marked = g_pEditor->Display(Id)->m_Marked;
	Marked.m_Group = (size_t)std::max(0, Group);
	Marked.m_X = X;
	Marked.m_Y = Y;
	Marked.m_Width = std::max(0, Width);
	Marked.m_Height = std::max(0, Height);
	g_pEditor->Touch();
}

// The brush: what is in hand and what putting it down does. A stroke is the
// page opening a change when the button goes down, calling `Paint` on every
// move and closing it when the button comes up - one entry in the history,
// however many tiles it touched.
EMSCRIPTEN_KEEPALIVE int MapEditorPickTiles(int Id, int Group, int Layer, int X, int Y, int Width, int Height)
{
	return g_pEditor != nullptr && Group >= 0 && Layer >= 0 &&
			       g_pEditor->PickTiles(Id, (size_t)Group, (size_t)Layer, X, Y, Width, Height) ?
		       1 :
		       0;
}

EMSCRIPTEN_KEEPALIVE int MapEditorGrab(int Id, int Group, int Layer, int X, int Y, int Width, int Height)
{
	return g_pEditor != nullptr && Group >= 0 && Layer >= 0 &&
			       g_pEditor->Grab(Id, (size_t)Group, (size_t)Layer, X, Y, Width, Height) ?
		       1 :
		       0;
}

EMSCRIPTEN_KEEPALIVE int MapEditorPaint(int Id, int Group, int Layer, int X, int Y)
{
	if(g_pEditor == nullptr || Group < 0 || Layer < 0 || !g_pEditor->Paint(Id, (size_t)Group, (size_t)Layer, X, Y))
		return 0;
	SayChanged(Id);
	SayDropped();
	return 1;
}

/** Which entities sheet the physics layers are drawn out of, by name. */
EMSCRIPTEN_KEEPALIVE int MapEditorSetEntitiesImage(const char *pName)
{
	return g_pEditor != nullptr && g_pEditor->SetEntitiesImage(pName) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char *MapEditorEntitiesImage()
{
	return g_pEditor == nullptr ? "" : g_pEditor->EntitiesImage();
}

/** Whether tiles that do nothing in a physics layer may be put there. */
EMSCRIPTEN_KEEPALIVE void MapEditorSetAllowUnused(int Allow)
{
	if(g_pEditor != nullptr)
		g_pEditor->SetAllowUnused(Allow != 0);
}

EMSCRIPTEN_KEEPALIVE int MapEditorAllowUnused()
{
	return g_pEditor != nullptr && g_pEditor->AllowUnused() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int MapEditorFill(int Id, int Group, int Layer, int X, int Y, int Width, int Height)
{
	if(g_pEditor == nullptr || Group < 0 || Layer < 0 || !g_pEditor->Fill(Id, (size_t)Group, (size_t)Layer, X, Y, Width, Height))
		return 0;
	SayChanged(Id);
	SayDropped();
	return 1;
}

EMSCRIPTEN_KEEPALIVE int MapEditorErase(int Id, int Group, int Layer, int X, int Y, int Width, int Height)
{
	if(g_pEditor == nullptr || Group < 0 || Layer < 0 || !g_pEditor->Erase(Id, (size_t)Group, (size_t)Layer, X, Y, Width, Height))
		return 0;
	SayChanged(Id);
	return 1;
}

EMSCRIPTEN_KEEPALIVE void MapEditorFlipBrushX()
{
	if(g_pEditor != nullptr)
		g_pEditor->FlipBrushX();
}

EMSCRIPTEN_KEEPALIVE void MapEditorFlipBrushY()
{
	if(g_pEditor != nullptr)
		g_pEditor->FlipBrushY();
}

EMSCRIPTEN_KEEPALIVE void MapEditorRotateBrush()
{
	if(g_pEditor != nullptr)
		g_pEditor->RotateBrush();
}

EMSCRIPTEN_KEEPALIVE void MapEditorClearBrush()
{
	if(g_pEditor != nullptr)
		g_pEditor->ClearBrush();
}

EMSCRIPTEN_KEEPALIVE int MapEditorStoreBrush(int Slot)
{
	return g_pEditor != nullptr && Slot >= 0 && g_pEditor->StoreBrush((size_t)Slot) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int MapEditorUseBrush(int Slot)
{
	return g_pEditor != nullptr && Slot >= 0 && g_pEditor->UseBrush((size_t)Slot) ? 1 : 0;
}

// What goes beside a physics tile: which tele, which switch and how long it
// waits, how hard and which way a speedup pushes. One set for the brush rather
// than one per tile - a number is chosen and then tiles are put down with it.
// Read back after a grab, which is how a piece of a map carries its numbers.
EMSCRIPTEN_KEEPALIVE void MapEditorSetNumbers(int Number, int Delay, int Force, int MaxSpeed, int Angle)
{
	if(g_pEditor == nullptr)
		return;
	map_document::CBrushNumbers Numbers;
	Numbers.m_Number = Number;
	Numbers.m_Delay = Delay;
	Numbers.m_Force = Force;
	Numbers.m_MaxSpeed = MaxSpeed;
	Numbers.m_Angle = Angle;
	g_pEditor->SetNumbers(Numbers);
}

EMSCRIPTEN_KEEPALIVE int MapEditorNumber()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Numbers().m_Number;
}

EMSCRIPTEN_KEEPALIVE int MapEditorDelay()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Numbers().m_Delay;
}

EMSCRIPTEN_KEEPALIVE int MapEditorForce()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Numbers().m_Force;
}

EMSCRIPTEN_KEEPALIVE int MapEditorMaxSpeed()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Numbers().m_MaxSpeed;
}

EMSCRIPTEN_KEEPALIVE int MapEditorAngle()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Numbers().m_Angle;
}

EMSCRIPTEN_KEEPALIVE int MapEditorBrushWidth()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Brush().Width();
}

EMSCRIPTEN_KEEPALIVE int MapEditorBrushHeight()
{
	return g_pEditor == nullptr ? 0 : g_pEditor->Brush().Height();
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
	int Grid = 0;
	std::vector<std::pair<size_t, size_t>> vHide;
	std::string Marked;
	std::string ShownQuad;
	std::string Entities;
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
		else if(str_comp(argv[i], "-g") == 0 && i + 1 < argc)
		{
			Grid = std::max(0, str_toint(argv[++i]));
		}
		else if(str_comp(argv[i], "-m") == 0 && i + 1 < argc)
		{
			Marked = argv[++i];
		}
		else if(str_comp(argv[i], "-q") == 0 && i + 1 < argc)
		{
			ShownQuad = argv[++i];
		}
		else if(str_comp(argv[i], "-e") == 0 && i + 1 < argc)
		{
			Entities = argv[++i];
		}
		else if(str_comp(argv[i], "-x") == 0 && i + 1 < argc)
		{
			const char *pWhich = argv[++i];
			const char *pColon = str_find(pWhich, ":");
			if(pColon == nullptr)
			{
				InvalidUsage = true;
				break;
			}
			vHide.emplace_back((size_t)std::max(0, str_toint(pWhich)), (size_t)std::max(0, str_toint(pColon + 1)));
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
		if(Grid > 0)
			Editor.Display(Id)->m_Grid = Grid;
		if(!Entities.empty())
		{
			if(!Editor.SetEntitiesImage(Entities.c_str()))
			{
				log_error(TOOL_NAME, "There is no entities picture called '%s'", Entities.c_str());
				return 1;
			}
			Editor.Display(Id)->m_EntityOverlayVal = 100;
		}
		for(const auto &[Group, Layer] : vHide)
			Editor.Display(Id)->SetVisible(Group, Layer, false);
		if(!ShownQuad.empty())
		{
			int aNumbers[3] = {0, 0, 0};
			const char *pRead = ShownQuad.c_str();
			for(int &Number : aNumbers)
			{
				Number = str_toint(pRead);
				const char *pColon = str_find(pRead, ":");
				pRead = pColon == nullptr ? "" : pColon + 1;
			}
			CDocumentRenderer::CParams::CShownQuad &Shown = Editor.Display(Id)->m_ShownQuad;
			Shown.m_Group = (size_t)std::max(0, aNumbers[0]);
			Shown.m_Layer = (size_t)std::max(0, aNumbers[1]);
			Shown.m_Quad = (size_t)std::max(0, aNumbers[2]);
			Shown.m_Shown = true;
		}
		if(!Marked.empty())
		{
			int aNumbers[5] = {0, 0, 0, 0, 0};
			const char *pRead = Marked.c_str();
			for(int &Number : aNumbers)
			{
				Number = str_toint(pRead);
				const char *pColon = str_find(pRead, ":");
				pRead = pColon == nullptr ? "" : pColon + 1;
			}
			CDocumentRenderer::CParams::CMarked &Where = Editor.Display(Id)->m_Marked;
			Where.m_Group = (size_t)std::max(0, aNumbers[0]);
			Where.m_X = aNumbers[1];
			Where.m_Y = aNumbers[2];
			Where.m_Width = std::max(0, aNumbers[3]);
			Where.m_Height = std::max(0, aNumbers[4]);
		}
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

#if defined(CONF_PLATFORM_EMSCRIPTEN)
		if(g_PictureMap >= 0)
		{
			// Begun here rather than in the call: the call comes from the page
			// in the middle of whatever, and this is between two frames.
			const int Map = std::exchange(g_PictureMap, -1);
			char aName[IO_MAX_PATH_LENGTH];
			str_format(aName, sizeof(aName), "%s/%s.png", PICTURE_DIRECTORY, Editor.Name(Map));
			Editor.Storage()->CreateFolder(PICTURE_DIRECTORY, IStorage::TYPE_SAVE);
			char aPath[IO_MAX_PATH_LENGTH];
			Editor.Storage()->GetCompletePath(IStorage::TYPE_SAVE, aName, aPath, sizeof(aPath));
			g_PictureFile = aName;
			if(!Editor.BeginPicture(Map, aPath, CStandaloneMapView::VIEWER_FULL_IMAGE_PIXELS))
			{
				g_PictureState = 3;
				Say("error", "{\"what\":\"picture\"}");
			}
		}
		else if(Editor.PictureRunning() && !Editor.StepPicture(PICTURE_BUDGET))
		{
			const bool Made = !Editor.PictureFailed();
			if(Made)
				Editor.Storage()->SendFileToUser(g_PictureFile.c_str(), IStorage::TYPE_SAVE);
			g_PictureState = Made ? 2 : 3;
			Say(Made ? "picture" : "error", Made ? "{}" : "{\"what\":\"picture\"}");
		}
#endif

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
