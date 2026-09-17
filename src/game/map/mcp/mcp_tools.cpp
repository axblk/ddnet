#include "mcp_tools.h"

#include <base/fs.h>
#include <base/hash.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/gfx/image_loader.h>
#include <engine/gfx/image_manipulation.h>
#include <engine/image.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/art.h>
#include <game/map/document/automap.h>
#include <game/map/document/command.h>
#include <game/map/document/edit.h>
#include <game/map/document/explain.h>
#include <game/map/document/map_file.h>
#include <game/map/document/proof.h>
#include <game/map/document/report.h>
#include <game/map/document/settings.h>
#include <game/map/document/structure.h>
#include <game/map/document/tiles.h>
#include <game/map/document/view.h>
#include <game/map/mcp/mcp_core.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <variant>
#include <vector>

namespace map_mcp
{
	using namespace map_document;

	bool ReadPngSize(const unsigned char *pData, size_t Size, int *pWidth, int *pHeight)
	{
		if(Size < 24 || std::memcmp(pData, "\x89PNG\r\n\x1a\n", 8) != 0)
			return false;
		*pWidth = (pData[16] << 24) | (pData[17] << 16) | (pData[18] << 8) | pData[19];
		*pHeight = (pData[20] << 24) | (pData[21] << 16) | (pData[22] << 8) | pData[23];
		return true;
	}

	namespace
	{
		constexpr int MAX_RENDER_SIDE = 2048;
		constexpr int AUTO_RENDER_SIDE = 512;
		constexpr int MAX_NEW_SIDE = 10000;

		const char *KindName(ETileLayerKind Kind)
		{
			switch(Kind)
			{
			case ETileLayerKind::TILES: return "tiles";
			case ETileLayerKind::GAME: return "game";
			case ETileLayerKind::FRONT: return "front";
			case ETileLayerKind::TELE: return "tele";
			case ETileLayerKind::SPEEDUP: return "speedup";
			case ETileLayerKind::SWITCH: return "switch";
			case ETileLayerKind::TUNE: return "tune";
			}
			return "tiles";
		}

		bool ReadKindName(const char *pName, ETileLayerKind *pOut)
		{
			static const ETileLayerKind s_aKinds[] = {ETileLayerKind::GAME, ETileLayerKind::FRONT, ETileLayerKind::TELE, ETileLayerKind::SPEEDUP, ETileLayerKind::SWITCH, ETileLayerKind::TUNE};
			const auto *pFound = std::find_if(std::begin(s_aKinds), std::end(s_aKinds), [pName](ETileLayerKind Kind) { return str_comp(pName, KindName(Kind)) == 0; });
			if(pFound == std::end(s_aKinds))
				return false;
			*pOut = *pFound;
			return true;
		}

		/** A whole number argument, or the default; a complaint if it is something else. */
		int IntArg(const CJson &Args, const char *pName, int Default, std::string *pError)
		{
			const CJson &Value = Args.Get(pName);
			if(Value.IsNull())
				return Default;
			if(!Value.IsNumber() || Value.AsDouble() != (double)(int)Value.AsDouble())
			{
				if(pError->empty())
					*pError = std::string("'") + pName + "' has to be a whole number";
				return Default;
			}
			return (int)Value.AsDouble();
		}

		/** The JSON the document's `Apply` answered, as a tree. */
		CJson ApplyCommand(CDocument &Document, const CJson &Command)
		{
			const std::string Answer = Apply(Document, Command.Serialize().c_str());
			CJson Parsed;
			std::string Error;
			if(!CJson::Parse(Answer.c_str(), &Parsed, &Error))
			{
				Parsed = CJson::Object();
				Parsed.Set("ok", CJson::Bool(false));
				Parsed.Set("error", CJson::Str("the document answered with something that is not JSON"));
			}
			return Parsed;
		}

		CJson RectJson(const CTileRect &Rect)
		{
			CJson Json = CJson::Object();
			Json.Set("x", CJson::Int(Rect.m_X));
			Json.Set("y", CJson::Int(Rect.m_Y));
			Json.Set("w", CJson::Int(Rect.m_Width));
			Json.Set("h", CJson::Int(Rect.m_Height));
			return Json;
		}

		/** What a map is made of, short enough to come back with every open. */
		CJson Summary(const CMapState &Map)
		{
			CJson Json = CJson::Object();
			const std::optional<CLayerAddress> Game = FindGameLayer(Map);
			if(Game.has_value())
			{
				const CTileLayer *pGame = Map.TileLayer(Game->m_Group, Game->m_Layer);
				CJson GameJson = CJson::Object();
				GameJson.Set("group", CJson::Int((int64_t)Game->m_Group));
				GameJson.Set("layer", CJson::Int((int64_t)Game->m_Layer));
				GameJson.Set("width", CJson::Int(pGame->Width()));
				GameJson.Set("height", CJson::Int(pGame->Height()));
				Json.Set("gameLayer", GameJson);
			}
			else
			{
				Json.Set("gameLayer", CJson::Null());
			}
			CJson Groups = CJson::Array();
			for(size_t GroupIndex = 0; GroupIndex < Map.NumGroups(); ++GroupIndex)
			{
				const CGroup &Group = *Map.Group(GroupIndex);
				CJson GroupJson = CJson::Object();
				GroupJson.Set("group", CJson::Int((int64_t)GroupIndex));
				GroupJson.Set("name", CJson::Str(Group.m_Name));
				if(Group.m_ParallaxX != 100 || Group.m_ParallaxY != 100)
				{
					GroupJson.Set("parallaxX", CJson::Int(Group.m_ParallaxX));
					GroupJson.Set("parallaxY", CJson::Int(Group.m_ParallaxY));
				}
				if(Group.m_OffsetX != 0 || Group.m_OffsetY != 0)
				{
					GroupJson.Set("offsetX", CJson::Int(Group.m_OffsetX));
					GroupJson.Set("offsetY", CJson::Int(Group.m_OffsetY));
				}
				CJson Layers = CJson::Array();
				for(size_t LayerIndex = 0; LayerIndex < Group.m_vpLayers.size(); ++LayerIndex)
				{
					const CLayer &Layer = *Group.m_vpLayers[LayerIndex];
					CJson LayerJson = CJson::Object();
					LayerJson.Set("layer", CJson::Int((int64_t)LayerIndex));
					LayerJson.Set("name", CJson::Str(LayerProperties(Layer).m_Name));
					if(const CTileLayer *pTiles = std::get_if<CTileLayer>(&Layer); pTiles != nullptr)
					{
						LayerJson.Set("type", CJson::Str("tiles"));
						LayerJson.Set("kind", CJson::Str(KindName(pTiles->m_Kind)));
						LayerJson.Set("width", CJson::Int(pTiles->Width()));
						LayerJson.Set("height", CJson::Int(pTiles->Height()));
						if(pTiles->m_Image >= 0)
							LayerJson.Set("image", CJson::Int(pTiles->m_Image));
					}
					else if(const CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Layer); pQuads != nullptr)
					{
						LayerJson.Set("type", CJson::Str("quads"));
						LayerJson.Set("quads", CJson::Int((int64_t)pQuads->m_Quads.Size()));
						if(pQuads->m_Image >= 0)
							LayerJson.Set("image", CJson::Int(pQuads->m_Image));
					}
					else if(const CSoundLayer *pSounds = std::get_if<CSoundLayer>(&Layer); pSounds != nullptr)
					{
						LayerJson.Set("type", CJson::Str("sounds"));
						LayerJson.Set("sources", CJson::Int((int64_t)pSounds->m_Sources.Size()));
					}
					if(LayerProperties(Layer).m_Detail)
						LayerJson.Set("detail", CJson::Bool(true));
					Layers.Push(LayerJson);
				}
				GroupJson.Set("layers", Layers);
				Groups.Push(GroupJson);
			}
			Json.Set("groups", Groups);
			CJson Images = CJson::Array();
			for(size_t Index = 0; Index < Map.NumImages(); ++Index)
			{
				const CImage &Image = *Map.Image(Index);
				CJson ImageJson = CJson::Object();
				ImageJson.Set("image", CJson::Int((int64_t)Index));
				ImageJson.Set("name", CJson::Str(Image.m_Name));
				ImageJson.Set("external", CJson::Bool(Image.m_External));
				ImageJson.Set("width", CJson::Int(Image.m_Width));
				ImageJson.Set("height", CJson::Int(Image.m_Height));
				Images.Push(ImageJson);
			}
			Json.Set("images", Images);
			Json.Set("envelopes", CJson::Int((int64_t)Map.NumEnvelopes()));
			Json.Set("sounds", CJson::Int((int64_t)Map.NumSounds()));
			Json.Set("settings", CJson::Int((int64_t)Map.m_Info.m_Settings.Size()));
			return Json;
		}

		const CTileLayer *TileLayerAt(const CMapState &Map, const CJson &Args, CLayerAddress *pAddress, std::string *pError)
		{
			const int Group = IntArg(Args, "group", -1, pError);
			const int Layer = IntArg(Args, "layer", -1, pError);
			if(!pError->empty())
				return nullptr;
			if(Group < 0 || (size_t)Group >= Map.NumGroups())
			{
				*pError = "'group' is " + std::to_string(Group) + ", which is not there";
				return nullptr;
			}
			if(Layer < 0 || (size_t)Layer >= Map.NumLayers(Group))
			{
				*pError = "'layer' is " + std::to_string(Layer) + ", which is not in group " + std::to_string(Group);
				return nullptr;
			}
			const CTileLayer *pTiles = std::get_if<CTileLayer>(Map.Layer(Group, Layer));
			if(pTiles == nullptr)
			{
				*pError = "layer " + std::to_string(Layer) + " of group " + std::to_string(Group) + " holds no tiles";
				return nullptr;
			}
			pAddress->m_Group = Group;
			pAddress->m_Layer = Layer;
			return pTiles;
		}
	} // namespace

	/**
	 * The tools themselves. A friend of the core, so that it can reach the
	 * open maps and the renderer without the core growing an accessor for
	 * every one of them.
	 */
	class CTools
	{
	public:
		using COpenMap = CMapMcp::COpenMap;

		static COpenMap *Map(CMapMcp &Mcp, const CJson &Args, std::string *pError) { return Mcp.MapArg(Args, pError); }

		/** What every change comes back with: where the history stands now. */
		static void Stamp(CJson &Json, const COpenMap &Map)
		{
			Json.Set("map", CJson::Str(Map.m_Handle));
			Json.Set("historyIndex", CJson::Int((int64_t)Map.m_Document.History().CurrentIndex()));
			Json.Set("historyEntries", CJson::Int((int64_t)Map.m_Document.History().NumEntries()));
			Json.Set("historyBytes", CJson::Int((int64_t)Map.m_Document.History().Bytes()));
			Json.Set("dirty", CJson::Bool(Map.m_Dirty));
		}

		static CToolResult Done(COpenMap &Map, CJson Structured, const std::string &Text)
		{
			CToolResult Result;
			Result.m_Structured = std::move(Structured);
			Stamp(Result.m_Structured, Map);
			Result.m_Text = Text;
			return Result;
		}

		/**
		 * Draws a picture of a rectangle of tiles, marked, for a change
		 * that asked to be shown. Nothing happens without a renderer.
		 */
		static void RenderAfter(CMapMcp &Mcp, COpenMap &Map, size_t Group, const CTileRect &Rect, CToolResult &Result)
		{
			if(Mcp.m_pRenderer == nullptr || Rect.Empty())
				return;
			CRenderRequest Request;
			// A margin of tiles around what changed, so that it is seen in
			// its surroundings, and a picture no larger than a small one.
			const int Margin = 4;
			const float Wide = (float)(Rect.m_Width + 2 * Margin) * 32.0f;
			const float High = (float)(Rect.m_Height + 2 * Margin) * 32.0f;
			const float Aspect = Wide / High;
			Request.m_Width = Aspect >= 1.0f ? AUTO_RENDER_SIDE : std::max(64, (int)(AUTO_RENDER_SIDE * Aspect));
			Request.m_Height = Aspect >= 1.0f ? std::max(64, (int)(AUTO_RENDER_SIDE / Aspect)) : AUTO_RENDER_SIDE;
			CView Camera;
			Camera.SetSurface(Request.m_Width, Request.m_Height);
			Camera.Fit(vec2(Wide, High));
			Camera.SetCenter(vec2((float)(Rect.m_X + Rect.m_Width / 2.0f) * 32.0f, (float)(Rect.m_Y + Rect.m_Height / 2.0f) * 32.0f));
			Request.m_Center = Camera.Center();
			Request.m_Zoom = Camera.Zoom();
			Request.m_EntityOverlay = 100;
			Request.m_MarkGroup = Group;
			Request.m_Mark = Rect;
			std::vector<uint8_t> vPng;
			int Width = 0;
			int Height = 0;
			std::string Error;
			if(Mcp.m_pRenderer->Render(Map.m_Document.Map(), Request, &vPng, &Width, &Height, &Error))
				Result.m_vImages.push_back(std::move(vPng));
			else
				Result.m_Structured.Set("renderError", CJson::Str(Error));
		}

		/**
		 * A change that is one command of the document: the arguments go
		 * through as they are, `map`, `dryRun`, `render` and `label` taken
		 * out or passed along. `dryRun` runs the command and throws the
		 * version away; `render` draws the rectangle afterwards.
		 */
		static CToolResult Passthrough(CMapMcp &Mcp, const CJson &Args, const char *pOp, const char *pLabel, bool Mutating)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			CJson Command = CJson::Object();
			Command.Set("op", CJson::Str(pOp));
			for(size_t i = 0; i < Args.Size(); ++i)
			{
				const std::string &Key = Args.KeyAt(i);
				if(Key == "map" || Key == "dryRun" || Key == "render" || Key == "op")
					continue;
				Command.Set(Key.c_str(), Args.Get(Key.c_str()));
			}
			if(Mutating && !Args.Has("label"))
				Command.Set("label", CJson::Str(pLabel));
			const bool DryRun = Mutating && Args.Get("dryRun").AsBool();
			if(Mutating)
				pMap->m_Document.Begin(Args.Get("label").AsCStr(pLabel));
			CJson Answer = ApplyCommand(pMap->m_Document, Command);
			if(!Answer.Get("ok").AsBool())
			{
				if(Mutating)
					pMap->m_Document.Abort();
				return CToolResult::Error(Answer.Get("error").AsCStr("the command failed"));
			}
			if(Mutating)
			{
				if(DryRun)
					pMap->m_Document.Abort();
				else
				{
					pMap->m_Document.Commit();
					pMap->m_Dirty = true;
				}
			}
			CJson Structured = CJson::Object();
			for(size_t i = 0; i < Answer.Size(); ++i)
			{
				if(Answer.KeyAt(i) != "ok")
					Structured.Set(Answer.KeyAt(i).c_str(), Answer.Get(Answer.KeyAt(i).c_str()));
			}
			if(DryRun)
				Structured.Set("dryRun", CJson::Bool(true));
			CToolResult Result = Done(*pMap, std::move(Structured), "");
			if(Mutating && !DryRun && Args.Get("render").AsBool())
			{
				CLayerAddress Address;
				std::string LayerError;
				const CTileLayer *pLayer = TileLayerAt(pMap->m_Document.Map(), Args, &Address, &LayerError);
				if(pLayer != nullptr)
				{
					CTileRect Rect;
					Rect.m_X = IntArg(Args, "x", 0, &LayerError);
					Rect.m_Y = IntArg(Args, "y", 0, &LayerError);
					Rect.m_Width = IntArg(Args, "w", pLayer->Width() - Rect.m_X, &LayerError);
					Rect.m_Height = IntArg(Args, "h", pLayer->Height() - Rect.m_Y, &LayerError);
					RenderAfter(Mcp, *pMap, Address.m_Group, ClipTileRect(*pLayer, Rect), Result);
				}
			}
			return Result;
		}

		// ---- 5.1 session and files ----

		static CToolResult MapsList(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			const char *pSubdir = Args.Get("subdir").AsCStr(".");
			const std::string Dir = pSubdir[0] == '\0' || str_comp(pSubdir, ".") == 0 ? Mcp.m_Root : Mcp.ResolvePath(pSubdir, false, &Error);
			if(Dir.empty())
				return CToolResult::Error(Error);
			if(!fs_is_dir(Dir.c_str()))
				return CToolResult::Error(std::string("'") + pSubdir + "' is not a directory");
			class CEntries
			{
			public:
				std::vector<std::pair<std::string, int64_t>> m_vFiles;
				std::vector<std::string> m_vDirs;
			} Entries;
			fs_listdir_fileinfo(
				Dir.c_str(), [](const CFsFileInfo *pInfo, int IsDir, int, void *pUser) {
					CEntries *pEntries = static_cast<CEntries *>(pUser);
					if(pInfo->m_pName[0] == '.')
						return 0;
					if(IsDir)
						pEntries->m_vDirs.emplace_back(pInfo->m_pName);
					else if(str_endswith(pInfo->m_pName, ".map"))
						pEntries->m_vFiles.emplace_back(pInfo->m_pName, (int64_t)pInfo->m_TimeModified);
					return 0;
				},
				0, &Entries);
			std::sort(Entries.m_vFiles.begin(), Entries.m_vFiles.end());
			std::sort(Entries.m_vDirs.begin(), Entries.m_vDirs.end());
			const std::string Prefix = Dir == Mcp.m_Root ? "" : Dir.substr(Mcp.m_Root.size() + 1) + "/";
			CJson Files = CJson::Array();
			for(const auto &[Name, Modified] : Entries.m_vFiles)
			{
				CJson File = CJson::Object();
				File.Set("path", CJson::Str(Prefix + Name));
				std::string FullName = Dir;
				FullName += "/";
				FullName += Name;
				IOHANDLE Handle = io_open(FullName.c_str(), IOFLAG_READ);
				if(Handle != nullptr)
				{
					File.Set("bytes", CJson::Int(io_length(Handle)));
					io_close(Handle);
				}
				File.Set("modified", CJson::Int(Modified));
				Files.Push(File);
			}
			CJson Dirs = CJson::Array();
			for(const std::string &Name : Entries.m_vDirs)
				Dirs.Push(CJson::Str(Prefix + Name));
			CJson Structured = CJson::Object();
			Structured.Set("maps", Files);
			Structured.Set("directories", Dirs);
			CToolResult Result;
			Result.m_Structured = Structured;
			Result.m_Text = std::to_string(Entries.m_vFiles.size()) + " map files under '" + (Prefix.empty() ? std::string(".") : Prefix) + "'";
			return Result;
		}

		static CToolResult OpenResult(COpenMap &Map, const char *pVerb)
		{
			CJson Structured = CJson::Object();
			Structured.Set("name", CJson::Str(Map.m_Name));
			Structured.Set("path", CJson::Str(Map.m_Path));
			CJson Warnings = CJson::Array();
			for(const std::string &Warning : Map.m_vWarnings)
				Warnings.Push(CJson::Str(Warning));
			Structured.Set("warnings", Warnings);
			Structured.Set("summary", Summary(Map.m_Document.Map()));
			return Done(Map, std::move(Structured), std::string(pVerb) + " '" + Map.m_Name + "' as " + Map.m_Handle + "; the handle is valid until map.close or until this server stops");
		}

		static CToolResult MapOpen(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			const CJson &Path = Args.Get("path");
			if(!Path.IsString())
				return CToolResult::Error("'path' is needed, relative to the root directory");
			// The same file twice is the same map, the way an editor opens
			// it once.
			std::string ResolveError;
			const std::string Full = Mcp.ResolvePath(Path.AsCStr(), false, &ResolveError);
			if(!Full.empty())
			{
				const std::string Relative = Full.substr(Mcp.m_Root.size() + 1);
				for(auto &[Handle, pMap] : Mcp.m_Maps)
				{
					if(pMap->m_Path == Relative)
					{
						pMap->m_LastUsedNanos = time_get_nanoseconds().count();
						return OpenResult(*pMap, "Already open:");
					}
				}
			}
			COpenMap *pMap = Mcp.Open(Path.AsCStr(), &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			return OpenResult(*pMap, "Opened");
		}

		static CToolResult MapNew(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			const int Width = IntArg(Args, "width", 100, &Error);
			const int Height = IntArg(Args, "height", 50, &Error);
			if(!Error.empty())
				return CToolResult::Error(Error);
			if(Width < 1 || Height < 1 || Width > MAX_NEW_SIDE || Height > MAX_NEW_SIDE)
				return CToolResult::Error("a map is 1 to 10000 tiles a side");
			if(Mcp.m_Maps.size() >= Mcp.m_Options.m_MaxMaps)
				return CToolResult::Error("there are already " + std::to_string(Mcp.m_Maps.size()) + " maps open; close one with map.close first");
			CMapState State;
			CGroup Group;
			Group.m_Name = "Game";
			CTileLayer Game(ETileLayerKind::GAME, Width, Height);
			Game.m_Name = "Game";
			Group.m_vpLayers.push_back(std::make_shared<const CLayer>(std::move(Game)));
			State.AddGroup(std::move(Group));
			COpenMap *pMap = Mcp.Add(std::move(State), Args.Get("name").AsCStr("unnamed"), "", {});
			return OpenResult(*pMap, "Made");
		}

		static CToolResult MapSave(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			std::string Path = Args.Get("path").AsCStr("");
			if(Path.empty())
				Path = pMap->m_Path;
			if(Path.empty())
				return CToolResult::Error("this map has no file yet; say where to save it with 'path'");
			if(!str_endswith(Path.c_str(), ".map"))
				return CToolResult::Error("a map is saved as a .map file");
			const std::string Full = Mcp.ResolvePath(Path.c_str(), true, &Error);
			if(Full.empty())
				return CToolResult::Error(Error);
			const std::string Relative = Full.substr(Mcp.m_Root.size() + 1);
			const bool Exists = fs_is_file(Full.c_str());
			const bool Own = Relative == pMap->m_Path;
			if(Exists && !Own && !Args.Get("overwrite").AsBool())
				return CToolResult::Error("'" + Relative + "' exists already; pass overwrite: true to replace it, or another path");
			if(pMap->m_Document.IsEditing())
				return CToolResult::Error("the map is in the middle of a change");
			// Written beside the file and moved into place, so that a save
			// that fails halfway leaves the old file as it was.
			const std::string Temp = Full + ".part";
			CDataFileWriter Writer;
			if(!Writer.Open(Mcp.m_pStorage.get(), Temp.c_str(), IStorage::TYPE_ABSOLUTE))
				return CToolResult::Error("'" + Relative + "' could not be written");
			WriteMapState(Writer, pMap->m_Document.Map());
			Writer.Finish();
			if(fs_rename(Temp.c_str(), Full.c_str()) != 0)
			{
				(void)fs_remove(Temp.c_str());
				return CToolResult::Error("'" + Relative + "' could not be put in place");
			}
			void *pData = nullptr;
			unsigned Size = 0;
			IOHANDLE File = io_open(Full.c_str(), IOFLAG_READ);
			if(File == nullptr || !io_read_all(File, &pData, &Size))
			{
				if(File != nullptr)
					io_close(File);
				return CToolResult::Error("'" + Relative + "' was written but could not be read back");
			}
			io_close(File);
			char aHash[SHA256_MAXSTRSIZE];
			sha256_str(sha256(pData, Size), aHash, sizeof(aHash));
			free(pData);
			pMap->m_Path = Relative;
			pMap->m_Dirty = false;
			CJson Structured = CJson::Object();
			Structured.Set("path", CJson::Str(Relative));
			Structured.Set("bytes", CJson::Int(Size));
			Structured.Set("sha256", CJson::Str(aHash));
			return Done(*pMap, std::move(Structured), "Saved '" + Relative + "'");
		}

		static CToolResult MapClose(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			if(pMap->m_Dirty && !Args.Get("discard").AsBool())
				return CToolResult::Error("'" + pMap->m_Name + "' has changes that were not saved; map.save it, or pass discard: true");
			const std::string Handle = pMap->m_Handle;
			const std::string Name = pMap->m_Name;
			Mcp.m_Maps.erase(Handle);
			CToolResult Result;
			Result.m_Structured.Set("closed", CJson::Str(Handle));
			Result.m_Text = "Closed '" + Name + "' (" + Handle + ")";
			return Result;
		}

		static CToolResult MapList(CMapMcp &Mcp, const CJson &)
		{
			CJson Maps = CJson::Array();
			for(const auto &[Handle, pMap] : Mcp.m_Maps)
			{
				CJson Json = CJson::Object();
				Json.Set("name", CJson::Str(pMap->m_Name));
				Json.Set("path", CJson::Str(pMap->m_Path));
				Stamp(Json, *pMap);
				Maps.Push(Json);
			}
			CToolResult Result;
			Result.m_Structured.Set("maps", Maps);
			CJson Limits = CJson::Object();
			Limits.Set("maxMaps", CJson::Int((int64_t)Mcp.m_Options.m_MaxMaps));
			Limits.Set("historyBytesPerMap", CJson::Int((int64_t)Mcp.m_Options.m_HistoryBytes));
			Limits.Set("historyEntriesPerMap", CJson::Int((int64_t)Mcp.m_Options.m_HistoryEntries));
			Limits.Set("idleSeconds", CJson::Int(Mcp.m_Options.m_IdleSeconds));
			Limits.Set("renderer", CJson::Bool(Mcp.m_pRenderer != nullptr));
			Result.m_Structured.Set("limits", Limits);
			Result.m_Text = std::to_string(Mcp.m_Maps.size()) + " maps open";
			return Result;
		}

		static CToolResult MapAppend(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const std::string Full = Mcp.ResolvePath(Args.Get("path").AsCStr(), false, &Error);
			if(Full.empty())
				return CToolResult::Error(Error);
			CDataFileReader File;
			if(!File.Open(Mcp.m_pStorage.get(), Full.c_str(), IStorage::TYPE_ABSOLUTE))
				return CToolResult::Error("'" + std::string(Args.Get("path").AsCStr()) + "' could not be opened as a map file");
			CMapState Other;
			std::vector<std::string> vWarnings;
			const bool Ok = ReadMapState(File, &Other, &vWarnings);
			File.Close();
			if(!Ok)
				return CToolResult::Error("'" + std::string(Args.Get("path").AsCStr()) + "' could not be read as a map");
			pMap->m_Document.Begin(Args.Get("label").AsCStr("Append map"));
			const CAppendReport Report = AppendMap(pMap->m_Document, Other);
			pMap->m_Document.Commit();
			pMap->m_Dirty = true;
			CJson Structured = CJson::Object();
			Structured.Set("groups", CJson::Int((int64_t)Report.m_Groups));
			Structured.Set("images", CJson::Int((int64_t)Report.m_Images));
			Structured.Set("sharedImages", CJson::Int((int64_t)Report.m_SharedImages));
			Structured.Set("renamedImages", CJson::Int((int64_t)Report.m_RenamedImages));
			Structured.Set("sounds", CJson::Int((int64_t)Report.m_Sounds));
			Structured.Set("envelopes", CJson::Int((int64_t)Report.m_Envelopes));
			Structured.Set("settings", CJson::Int((int64_t)Report.m_Settings));
			CJson Warnings = CJson::Array();
			for(const std::string &Warning : vWarnings)
				Warnings.Push(CJson::Str(Warning));
			Structured.Set("warnings", Warnings);
			return Done(*pMap, std::move(Structured), "Appended " + std::to_string(Report.m_Groups) + " groups and " + std::to_string(Report.m_Images) + " images");
		}

		// ---- 5.2 reading ----

		static CToolResult MapStructure(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const bool Full = str_comp(Args.Get("detail").AsCStr("summary"), "full") == 0;
			CToolResult Result;
			Result.m_Structured = Full ? CJson::Raw(StructureJson(pMap->m_Document.Map())) : Summary(pMap->m_Document.Map());
			return Result;
		}

		static CToolResult RawOf(CMapMcp &Mcp, const CJson &Args, const std::function<std::string(const COpenMap &, std::string *)> &Make)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const std::string Json = Make(*pMap, &Error);
			if(!Error.empty())
				return CToolResult::Error(Error);
			CToolResult Result;
			Result.m_Structured = CJson::Raw(Json);
			return Result;
		}

		static CToolResult MapEnvelope(CMapMcp &Mcp, const CJson &Args)
		{
			return RawOf(Mcp, Args, [&](const COpenMap &Map, std::string *pError) {
				const int Index = IntArg(Args, "envelope", -1, pError);
				if(Index < 0 || (size_t)Index >= Map.m_Document.Map().NumEnvelopes())
				{
					*pError = "'envelope' is " + std::to_string(Index) + ", which is not there";
					return std::string();
				}
				return EnvelopeJson(Map.m_Document.Map(), Index);
			});
		}

		static CToolResult LayerListOf(CMapMcp &Mcp, const CJson &Args, bool Quads)
		{
			return RawOf(Mcp, Args, [&](const COpenMap &Map, std::string *pError) {
				const CMapState &State = Map.m_Document.Map();
				const int Group = IntArg(Args, "group", -1, pError);
				const int Layer = IntArg(Args, "layer", -1, pError);
				if(!pError->empty())
					return std::string();
				if(Group < 0 || (size_t)Group >= State.NumGroups() || Layer < 0 || (size_t)Layer >= State.NumLayers(Group))
				{
					*pError = "there is no such layer";
					return std::string();
				}
				const CLayer &Found = *State.Layer(Group, Layer);
				if(Quads ? !std::holds_alternative<CQuadLayer>(Found) : !std::holds_alternative<CSoundLayer>(Found))
				{
					*pError = Quads ? "that layer holds no quads" : "that layer holds no sound sources";
					return std::string();
				}
				return Quads ? QuadsJson(State, Group, Layer) : SoundSourcesJson(State, Group, Layer);
			});
		}

		static CToolResult MapHistory(CMapMcp &Mcp, const CJson &Args)
		{
			return RawOf(Mcp, Args, [](const COpenMap &Map, std::string *) { return HistoryJson(Map.m_Document); });
		}

		static CToolResult TileExplain(CMapMcp &, const CJson &Args)
		{
			std::string Error;
			ETileLayerKind Kind;
			if(!ReadKindName(Args.Get("kind").AsCStr(""), &Kind))
				return CToolResult::Error("'kind' is game, front, tele, speedup, switch or tune");
			const int Index = IntArg(Args, "index", -1, &Error);
			if(Index < 0 || Index > 255)
				return CToolResult::Error("'index' is 0 to 255");
			const char *pText = ExplainTile(Kind, Index);
			CToolResult Result;
			Result.m_Structured.Set("kind", CJson::Str(KindName(Kind)));
			Result.m_Structured.Set("index", CJson::Int(Index));
			Result.m_Structured.Set("text", pText == nullptr ? CJson::Null() : CJson::Str(pText));
			Result.m_Structured.Set("used", CJson::Bool(TileIsUsed(Kind, Index)));
			Result.m_Text = pText == nullptr ? "Index " + std::to_string(Index) + " means nothing in a " + KindName(Kind) + " layer" : pText;
			return Result;
		}

		static CToolResult TileAt(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const int X = IntArg(Args, "x", -1, &Error);
			const int Y = IntArg(Args, "y", -1, &Error);
			if(!Error.empty())
				return CToolResult::Error(Error);
			if(X < 0 || Y < 0)
				return CToolResult::Error("'x' and 'y' are needed, in tiles");
			const CMapState &State = pMap->m_Document.Map();
			CJson Layers = CJson::Array();
			std::string Text;
			for(size_t Group = 0; Group < State.NumGroups(); ++Group)
			{
				for(size_t Layer = 0; Layer < State.NumLayers(Group); ++Layer)
				{
					const CTileLayer *pTiles = std::get_if<CTileLayer>(State.Layer(Group, Layer));
					if(pTiles == nullptr)
						continue;
					const CTileValue Value = GetTileValue(*pTiles, X, Y);
					if(Value.IsAir() && pTiles->m_Kind == ETileLayerKind::TILES)
						continue;
					CJson Json = CJson::Object();
					Json.Set("group", CJson::Int((int64_t)Group));
					Json.Set("layer", CJson::Int((int64_t)Layer));
					Json.Set("name", CJson::Str(pTiles->m_Name));
					Json.Set("kind", CJson::Str(KindName(pTiles->m_Kind)));
					Json.Set("tile", CJson::Str(TileToken(Value)));
					Json.Set("index", CJson::Int(Value.Index()));
					if(pTiles->m_Kind != ETileLayerKind::TILES)
					{
						const char *pExplain = ExplainTile(pTiles->m_Kind, Value.Index());
						Json.Set("meaning", pExplain == nullptr ? CJson::Null() : CJson::Str(pExplain));
						if(!Text.empty())
							Text += "; ";
						Text += std::string(KindName(pTiles->m_Kind)) + ": " + (pExplain == nullptr ? (Value.IsAir() ? std::string("air") : "index " + std::to_string(Value.Index())) : std::string(pExplain).substr(0, std::string(pExplain).find(':')));
					}
					Layers.Push(Json);
				}
			}
			CToolResult Result;
			Result.m_Structured.Set("x", CJson::Int(X));
			Result.m_Structured.Set("y", CJson::Int(Y));
			Result.m_Structured.Set("layers", Layers);
			Result.m_Text = Text;
			return Result;
		}

		static CToolResult MapProof(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const int X = IntArg(Args, "x", -1, &Error);
			const int Y = IntArg(Args, "y", -1, &Error);
			if(!Error.empty())
				return CToolResult::Error(Error);
			if(X < 0 || Y < 0)
				return CToolResult::Error("'x' and 'y' are needed, in tiles");
			const bool Menu = Args.Get("menu").AsBool();
			const vec2 Center((X + 0.5f) * 32.0f, (Y + 0.5f) * 32.0f);
			CToolResult Result;
			Result.m_Structured = CJson::Raw(ProofJson(pMap->m_Document.Map(), Center, Menu));
			if(Args.Get("render").AsBool())
			{
				if(Mcp.m_pRenderer == nullptr)
					return CToolResult::Error("this server was started without a renderer");
				const CProofRect Rect = ProofScreen(Center, 16.0f / 9.0f, Menu ? 0.7f : 1.0f);
				CRenderRequest Request;
				Request.m_Width = 1024;
				Request.m_Height = 576;
				CView Camera;
				Camera.SetSurface(Request.m_Width, Request.m_Height);
				Camera.Fit(vec2(Rect.Width(), Rect.Height()));
				Camera.SetCenter(Center);
				Request.m_Center = Camera.Center();
				Request.m_Zoom = Camera.Zoom();
				std::vector<uint8_t> vPng;
				int Width = 0;
				int Height = 0;
				if(!Mcp.m_pRenderer->Render(pMap->m_Document.Map(), Request, &vPng, &Width, &Height, &Error))
					return CToolResult::Error(Error);
				Result.m_vImages.push_back(std::move(vPng));
			}
			return Result;
		}

		// ---- 5.3 changing ----

		static CToolResult MapApply(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const CJson &Ops = Args.Get("ops");
			if(!Ops.IsArray() || Ops.Size() == 0)
				return CToolResult::Error("'ops' is a list of commands, each with an 'op'");
			if(Ops.Size() > 1000)
				return CToolResult::Error("at most a thousand commands at once");
			const bool DryRun = Args.Get("dryRun").AsBool();
			pMap->m_Document.Begin(Args.Get("label").AsCStr("Apply"));
			CJson Results = CJson::Array();
			for(size_t i = 0; i < Ops.Size(); ++i)
			{
				const CJson &Op = Ops.At(i);
				if(!Op.IsObject() || !Op.Get("op").IsString())
				{
					pMap->m_Document.Abort();
					return CToolResult::Error("command " + std::to_string(i) + " has no 'op'");
				}
				const std::string OpName = Op.Get("op").AsString();
				if(str_startswith(OpName.c_str(), "history."))
				{
					pMap->m_Document.Abort();
					return CToolResult::Error("history.undo, history.redo and history.jump are tools of their own and cannot be part of a change");
				}
				CJson Answer = ApplyCommand(pMap->m_Document, Op);
				if(!Answer.Get("ok").AsBool())
				{
					pMap->m_Document.Abort();
					return CToolResult::Error("command " + std::to_string(i) + " (" + OpName + ") failed: " + Answer.Get("error").AsCStr("") + "; nothing was changed");
				}
				Results.Push(Answer);
			}
			if(DryRun)
				pMap->m_Document.Abort();
			else
			{
				pMap->m_Document.Commit();
				pMap->m_Dirty = true;
			}
			CJson Structured = CJson::Object();
			Structured.Set("results", Results);
			if(DryRun)
				Structured.Set("dryRun", CJson::Bool(true));
			return Done(*pMap, std::move(Structured), (DryRun ? "Tried " : "Applied ") + std::to_string(Ops.Size()) + " commands");
		}

		static CToolResult TilesBrush(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const CMapState &State = pMap->m_Document.Map();
			const CJson &From = Args.Get("from");
			const CJson &To = Args.Get("to");
			if(!From.IsObject() || !To.IsObject())
				return CToolResult::Error("'from' {group, layer, x, y, w, h} and 'to' {group, layer, x, y} are needed");
			CLayerAddress FromAddress;
			CLayerAddress ToAddress;
			const CTileLayer *pFrom = TileLayerAt(State, From, &FromAddress, &Error);
			if(pFrom == nullptr)
				return CToolResult::Error("from: " + Error);
			const CTileLayer *pTo = TileLayerAt(State, To, &ToAddress, &Error);
			if(pTo == nullptr)
				return CToolResult::Error("to: " + Error);
			CTileRect Source;
			Source.m_X = IntArg(From, "x", 0, &Error);
			Source.m_Y = IntArg(From, "y", 0, &Error);
			Source.m_Width = IntArg(From, "w", 1, &Error);
			Source.m_Height = IntArg(From, "h", 1, &Error);
			const int X = IntArg(To, "x", 0, &Error);
			const int Y = IntArg(To, "y", 0, &Error);
			const int Rotate = IntArg(Args, "rotate", 0, &Error);
			const CJson &Repeat = Args.Get("repeat");
			const int RepeatWidth = Repeat.IsObject() ? IntArg(Repeat, "w", 0, &Error) : 0;
			const int RepeatHeight = Repeat.IsObject() ? IntArg(Repeat, "h", 0, &Error) : 0;
			if(!Error.empty())
				return CToolResult::Error(Error);
			if(Source.Empty() || (int64_t)Source.m_Width * Source.m_Height > 1000 * 1000)
				return CToolResult::Error("'from' is a rectangle of up to a million tiles");
			if(Rotate % 90 != 0)
				return CToolResult::Error("'rotate' is 0, 90, 180 or 270");
			if(!CanStamp(pTo->m_Kind, pFrom->m_Kind))
				return CToolResult::Error(std::string("a ") + KindName(pFrom->m_Kind) + " brush cannot be stamped into a " + KindName(pTo->m_Kind) + " layer");
			CBrush Brush = GrabTiles(*pFrom, Source.m_X, Source.m_Y, Source.m_Width, Source.m_Height);
			if(Args.Get("flipX").AsBool())
				FlipBrushX(Brush);
			if(Args.Get("flipY").AsBool())
				FlipBrushY(Brush);
			for(int Turns = ((Rotate % 360) + 360) % 360 / 90; Turns > 0; --Turns)
				RotateBrush(Brush);
			const CTileLayer Before = *pTo;
			pMap->m_Document.Begin(Args.Get("label").AsCStr("Brush"));
			CTileRect Written;
			Written.m_X = X;
			Written.m_Y = Y;
			Written.m_Width = RepeatWidth > 0 ? RepeatWidth : Brush.Width();
			Written.m_Height = RepeatHeight > 0 ? RepeatHeight : Brush.Height();
			if(RepeatWidth > 0 || RepeatHeight > 0)
			{
				EditTileLayer(pMap->m_Document, ToAddress.m_Group, ToAddress.m_Layer, [&](CTileLayer &Layer) {
					FillTiles(Layer, X, Y, Written.m_Width, Written.m_Height, Brush);
				});
			}
			else
			{
				PaintTiles(pMap->m_Document, ToAddress.m_Group, ToAddress.m_Layer, X, Y, Brush);
			}
			if(Args.Get("dryRun").AsBool())
			{
				pMap->m_Document.Abort();
			}
			else
			{
				pMap->m_Document.Commit();
				pMap->m_Dirty = true;
			}
			CJson Structured = CJson::Object();
			Structured.Set("brush", RectJson(CTileRect{0, 0, Brush.Width(), Brush.Height()}));
			Structured.Set("written", RectJson(ClipTileRect(*pTo, Written)));
			Structured.Set("chunks", CJson::Int((int64_t)ChangedChunks(Before, *pMap->m_Document.Map().TileLayer(ToAddress.m_Group, ToAddress.m_Layer))));
			CToolResult Result = Done(*pMap, std::move(Structured), "Stamped " + std::to_string(Brush.Width()) + " by " + std::to_string(Brush.Height()) + " tiles");
			if(Args.Get("render").AsBool() && !Args.Get("dryRun").AsBool())
				RenderAfter(Mcp, *pMap, ToAddress.m_Group, ClipTileRect(*pTo, Written), Result);
			return Result;
		}

		static bool LoadRules(CMapMcp &Mcp, const char *pName, CAutomapRules *pRules, std::vector<int> *pvNotUnderstood, std::string *pError)
		{
			if(pName == nullptr || pName[0] == '\0' || str_find(pName, "/") != nullptr || str_find(pName, "..") != nullptr)
			{
				*pError = "'rules' names an image, which names its rules file";
				return false;
			}
			char *pText = Mcp.m_pStorage->ReadFileStr((std::string("editor/automap/") + pName + ".rules").c_str(), IStorage::TYPE_ALL);
			if(pText == nullptr)
			{
				*pError = std::string("there is no rules file for '") + pName + "'; automap.rules lists the ones there are";
				return false;
			}
			*pRules = ParseAutomapRules(pText, pvNotUnderstood);
			free(pText);
			return true;
		}

		static CToolResult AutomapRun(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const CMapState &State = pMap->m_Document.Map();
			CLayerAddress Address;
			const CTileLayer *pLayer = TileLayerAt(State, Args, &Address, &Error);
			if(pLayer == nullptr)
				return CToolResult::Error(Error);
			if(pLayer->m_Kind != ETileLayerKind::TILES)
				return CToolResult::Error("only a design layer is automapped, not the " + std::string(KindName(pLayer->m_Kind)) + " layer");
			CAutomapRules Rules;
			std::vector<int> vNotUnderstood;
			std::string RulesName = Args.Get("rules").AsCStr("");
			if(RulesName.empty() && pLayer->m_Image >= 0 && (size_t)pLayer->m_Image < State.NumImages())
				RulesName = State.Image(pLayer->m_Image)->m_Name;
			if(!LoadRules(Mcp, RulesName.c_str(), &Rules, &vNotUnderstood, &Error))
				return CToolResult::Error(Error);
			const CJson &ConfigArg = Args.Get("config");
			int Config = -1;
			if(ConfigArg.IsNumber())
				Config = (int)ConfigArg.AsInt();
			else if(ConfigArg.IsString())
			{
				for(size_t i = 0; i < Rules.NumConfigs(); ++i)
				{
					if(str_comp(Rules.ConfigName(i), ConfigArg.AsCStr()) == 0)
						Config = (int)i;
				}
				if(Config < 0)
					return CToolResult::Error("'" + RulesName + "' has no configuration called '" + ConfigArg.AsString() + "'");
			}
			else
				Config = 0;
			if(Config < 0 || (size_t)Config >= Rules.NumConfigs())
				return CToolResult::Error("'" + RulesName + "' has " + std::to_string(Rules.NumConfigs()) + " configurations");
			const int Seed = IntArg(Args, "seed", 1, &Error);
			const int Reference = IntArg(Args, "reference", -1, &Error);
			CTileRect Rect;
			Rect.m_X = IntArg(Args, "x", 0, &Error);
			Rect.m_Y = IntArg(Args, "y", 0, &Error);
			Rect.m_Width = IntArg(Args, "w", -1, &Error);
			Rect.m_Height = IntArg(Args, "h", -1, &Error);
			if(!Error.empty())
				return CToolResult::Error(Error);
			if(Reference < -1 || Reference > 9)
				return CToolResult::Error("'reference' is -1 for no filter, 0 for the game layer, or 1 to 9");
			const std::optional<CLayerAddress> Game = FindGameLayer(State);
			const CTileLayer *pGame = Game.has_value() ? State.TileLayer(Game->m_Group, Game->m_Layer) : nullptr;
			const CTileLayer Before = *pLayer;
			pMap->m_Document.Begin(Args.Get("label").AsCStr("Automap"));
			EditTileLayer(pMap->m_Document, Address.m_Group, Address.m_Layer, [&](CTileLayer &Layer) {
				Automap(Layer, pGame, Rules, (size_t)Config, Seed, Reference, Rect.m_X, Rect.m_Y, Rect.m_Width, Rect.m_Height);
			});
			const size_t Chunks = ChangedChunks(Before, *pMap->m_Document.Map().TileLayer(Address.m_Group, Address.m_Layer));
			if(Args.Get("dryRun").AsBool())
			{
				pMap->m_Document.Abort();
			}
			else
			{
				pMap->m_Document.Commit();
				pMap->m_Dirty = true;
			}
			CJson Structured = CJson::Object();
			Structured.Set("rules", CJson::Str(RulesName));
			Structured.Set("config", CJson::Str(Rules.ConfigName(Config)));
			Structured.Set("seed", CJson::Int(Seed));
			Structured.Set("chunks", CJson::Int((int64_t)Chunks));
			if(!vNotUnderstood.empty())
			{
				CJson Lines = CJson::Array();
				for(const int Line : vNotUnderstood)
					Lines.Push(CJson::Int(Line));
				Structured.Set("linesNotUnderstood", Lines);
			}
			CToolResult Result = Done(*pMap, std::move(Structured), "Automapped with '" + RulesName + "' (" + Rules.ConfigName(Config) + "), " + std::to_string(Chunks) + " blocks changed");
			if(Args.Get("render").AsBool() && !Args.Get("dryRun").AsBool())
			{
				CTileRect Shown = Rect;
				if(Shown.m_Width < 0)
					Shown.m_Width = pLayer->Width() - Shown.m_X;
				if(Shown.m_Height < 0)
					Shown.m_Height = pLayer->Height() - Shown.m_Y;
				RenderAfter(Mcp, *pMap, Address.m_Group, ClipTileRect(*pLayer, Shown), Result);
			}
			return Result;
		}

		/** A PNG under the root, as RGBA pixels. */
		static bool LoadImage(CMapMcp &Mcp, const char *pPath, CImageInfo *pImage, std::string *pError)
		{
			const std::string Full = Mcp.ResolvePath(pPath, false, pError);
			if(Full.empty())
				return false;
			if(!str_endswith(Full.c_str(), ".png"))
			{
				*pError = std::string("'") + pPath + "' is not a .png file";
				return false;
			}
			IOHANDLE File = io_open(Full.c_str(), IOFLAG_READ);
			if(File == nullptr)
			{
				*pError = std::string("'") + pPath + "' could not be opened";
				return false;
			}
			if(io_length(File) > (int64_t)64 * 1024 * 1024)
			{
				io_close(File);
				*pError = std::string("'") + pPath + "' is larger than 64 MiB";
				return false;
			}
			int PngliteIncompatible = 0;
			const bool Loaded = CImageLoader::LoadPng(File, Full.c_str(), *pImage, PngliteIncompatible, false);
			// LoadPng closes the file.
			if(!Loaded)
			{
				*pError = std::string("'") + pPath + "' could not be read as a PNG";
				return false;
			}
			if((int64_t)pImage->m_Width * pImage->m_Height > (int64_t)16 * 1024 * 1024)
			{
				pImage->Free();
				*pError = std::string("'") + pPath + "' has more than 16 million pixels";
				return false;
			}
			if(!ConvertToRgba(*pImage))
			{
				pImage->Free();
				*pError = std::string("'") + pPath + "' could not be turned into RGBA";
				return false;
			}
			return true;
		}

		static CToolResult Art(CMapMcp &Mcp, const CJson &Args, bool Quads)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			CImageInfo Image;
			if(!LoadImage(Mcp, Args.Get("path").AsCStr(""), &Image, &Error))
				return CToolResult::Error(Error);
			char aName[IO_MAX_PATH_LENGTH];
			fs_split_file_extension(fs_filename(Args.Get("path").AsCStr("")), aName, sizeof(aName));
			const char *pName = Args.Get("name").AsCStr(aName);
			const size_t Colors = CountArtColors((int)Image.m_Width, (int)Image.m_Height, Image.m_pData);
			if(!Quads && Colors > ART_PALETTE_SIZE)
			{
				Image.Free();
				return CToolResult::Error("the picture has " + std::to_string(Colors) + " colours, and tile art takes at most " + std::to_string(ART_PALETTE_SIZE) + "; use art.quads");
			}
			pMap->m_Document.Begin(Args.Get("label").AsCStr(Quads ? "Quad art" : "Tile art"));
			size_t Group;
			if(Quads)
			{
				CQuadArtOptions Options;
				Options.m_PixelStep = IntArg(Args, "pixelStep", 1, &Error);
				Options.m_QuadSize = IntArg(Args, "quadSize", 64, &Error);
				Options.m_Centralize = Args.Get("centralize").AsBool();
				Options.m_Merge = Args.Get("merge").AsBool(true);
				if(!Error.empty() || Options.m_PixelStep < 1 || Options.m_QuadSize < 1)
				{
					pMap->m_Document.Abort();
					Image.Free();
					return CToolResult::Error(Error.empty() ? "'pixelStep' and 'quadSize' are at least 1" : Error);
				}
				Group = AddQuadArt(pMap->m_Document, pName, (int)Image.m_Width, (int)Image.m_Height, Image.m_pData, Options);
			}
			else
			{
				Group = AddTileArt(pMap->m_Document, pName, (int)Image.m_Width, (int)Image.m_Height, Image.m_pData);
			}
			pMap->m_Document.Commit();
			pMap->m_Dirty = true;
			CJson Structured = CJson::Object();
			Structured.Set("group", CJson::Int((int64_t)Group));
			Structured.Set("width", CJson::Int((int64_t)Image.m_Width));
			Structured.Set("height", CJson::Int((int64_t)Image.m_Height));
			Structured.Set("colors", CJson::Int((int64_t)Colors));
			Image.Free();
			return Done(*pMap, std::move(Structured), std::string("Added the picture as ") + (Quads ? "quads" : "tiles") + " in group " + std::to_string(Group));
		}

		static CToolResult ImageAdd(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			map_document::CImage Image;
			if(Args.Get("path").IsString())
			{
				CImageInfo Pixels;
				if(!LoadImage(Mcp, Args.Get("path").AsCStr(), &Pixels, &Error))
					return CToolResult::Error(Error);
				char aName[IO_MAX_PATH_LENGTH];
				fs_split_file_extension(fs_filename(Args.Get("path").AsCStr()), aName, sizeof(aName));
				Image.m_Name = Args.Get("name").AsCStr(aName);
				Image.m_External = false;
				Image.m_Width = (int)Pixels.m_Width;
				Image.m_Height = (int)Pixels.m_Height;
				Image.m_Data.Mutable().assign(Pixels.m_pData, Pixels.m_pData + Pixels.DataSize());
				Pixels.Free();
			}
			else if(Args.Get("name").IsString())
			{
				const std::string Name = Args.Get("name").AsString();
				if(Name.empty() || Name.find('/') != std::string::npos || Name.find("..") != std::string::npos)
					return CToolResult::Error("'name' is the name of an image in the game's mapres");
				void *pData = nullptr;
				unsigned Size = 0;
				if(!Mcp.m_pStorage->ReadFile(("mapres/" + Name + ".png").c_str(), IStorage::TYPE_ALL, &pData, &Size))
					return CToolResult::Error("there is no mapres image called '" + Name + "'; read ddnet://mapres for the ones there are");
				int Width = 0;
				int Height = 0;
				const bool Png = ReadPngSize(static_cast<const unsigned char *>(pData), Size, &Width, &Height);
				free(pData);
				if(!Png)
					return CToolResult::Error("'" + Name + "' is not a PNG");
				Image.m_Name = Name;
				Image.m_External = true;
				Image.m_Width = Width;
				Image.m_Height = Height;
			}
			else
				return CToolResult::Error("'name' of a mapres image, or 'path' of a PNG under the root, is needed");
			for(size_t i = 0; i < pMap->m_Document.Map().NumImages(); ++i)
			{
				if(pMap->m_Document.Map().Image(i)->m_Name == Image.m_Name)
					return CToolResult::Error("the map already has an image called '" + Image.m_Name + "' (image " + std::to_string(i) + ")");
			}
			pMap->m_Document.Begin(Args.Get("label").AsCStr("Add image"));
			const size_t Index = AddImage(pMap->m_Document, std::move(Image));
			pMap->m_Document.Commit();
			pMap->m_Dirty = true;
			CJson Structured = CJson::Object();
			Structured.Set("image", CJson::Int((int64_t)Index));
			return Done(*pMap, std::move(Structured), "Added image " + std::to_string(Index) + "; set it on a layer with map.apply {op: layer.setProp, prop: image, value: " + std::to_string(Index) + "}");
		}

		static CToolResult History(CMapMcp &Mcp, const CJson &Args, const char *pOp)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			CJson Command = CJson::Object();
			Command.Set("op", CJson::Str(pOp));
			if(Args.Has("index"))
				Command.Set("index", Args.Get("index"));
			const size_t Before = pMap->m_Document.History().CurrentIndex();
			CJson Answer = ApplyCommand(pMap->m_Document, Command);
			if(!Answer.Get("ok").AsBool())
				return CToolResult::Error(Answer.Get("error").AsCStr("the history could not be stepped"));
			const size_t After = pMap->m_Document.History().CurrentIndex();
			if(After != Before)
				pMap->m_Dirty = true;
			CJson Structured = CJson::Object();
			Structured.Set("label", CJson::Str(pMap->m_Document.History().Entry(After).m_Label));
			Structured.Set("moved", CJson::Bool(After != Before));
			return Done(*pMap, std::move(Structured), After == Before ? "Nothing to step to" : "Now at '" + pMap->m_Document.History().Entry(After).m_Label + "' (" + std::to_string(After) + ")");
		}

		// ---- 5.4 checking and rendering ----

		static CToolResult MapCheck(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			const CMapState &State = pMap->m_Document.Map();
			CJson Findings = CJson::Array();
			auto Finding = [&](const char *pSeverity, const char *pCheck, const std::string &Message, const CJson &Where = CJson::Null()) {
				CJson Json = CJson::Object();
				Json.Set("severity", CJson::Str(pSeverity));
				Json.Set("check", CJson::Str(pCheck));
				Json.Set("message", CJson::Str(Message));
				if(!Where.IsNull())
					Json.Set("where", Where);
				Findings.Push(Json);
			};
			for(const std::string &Warning : pMap->m_vWarnings)
				Finding("warning", "file", Warning);

			const std::optional<CLayerAddress> Game = FindGameLayer(State);
			if(!Game.has_value())
			{
				Finding("error", "physics", "the map has no game layer");
			}
			else
			{
				const CTileLayer *pGame = State.TileLayer(Game->m_Group, Game->m_Layer);
				CJson GameWhere = CJson::Object();
				GameWhere.Set("group", CJson::Int((int64_t)Game->m_Group));
				GameWhere.Set("layer", CJson::Int((int64_t)Game->m_Layer));
				const CTileStats GameStats = CountTiles(*pGame, CTileRect{0, 0, pGame->Width(), pGame->Height()});
				auto Count = [&](int Index) {
					for(const CTileCount &Entry : GameStats.m_vCounts)
						if(Entry.m_Index == Index)
							return Entry.m_Count;
					return (size_t)0;
				};
				if(Count(ENTITY_OFFSET + ENTITY_SPAWN) + Count(ENTITY_OFFSET + ENTITY_SPAWN_RED) + Count(ENTITY_OFFSET + ENTITY_SPAWN_BLUE) == 0)
					Finding("error", "physics", "the game layer has no spawn (index 192)", GameWhere);
				if(Count(TILE_START) == 0)
					Finding("warning", "physics", "the game layer has no start line (index 33); the race timer never starts", GameWhere);
				if(Count(TILE_FINISH) == 0)
					Finding("warning", "physics", "the game layer has no finish line (index 34)", GameWhere);
				if(GameStats.m_Tiles == 0)
					Finding("warning", "physics", "the game layer is empty", GameWhere);

				for(size_t Group = 0; Group < State.NumGroups(); ++Group)
				{
					for(size_t Layer = 0; Layer < State.NumLayers(Group); ++Layer)
					{
						const CTileLayer *pTiles = std::get_if<CTileLayer>(State.Layer(Group, Layer));
						if(pTiles == nullptr || pTiles->m_Kind == ETileLayerKind::TILES)
							continue;
						CJson Where = CJson::Object();
						Where.Set("group", CJson::Int((int64_t)Group));
						Where.Set("layer", CJson::Int((int64_t)Layer));
						if(pTiles->Width() != pGame->Width() || pTiles->Height() != pGame->Height())
							Finding("error", "physics", std::string("the ") + KindName(pTiles->m_Kind) + " layer is " + std::to_string(pTiles->Width()) + " by " + std::to_string(pTiles->Height()) + " tiles, the game layer " + std::to_string(pGame->Width()) + " by " + std::to_string(pGame->Height()), Where);
						if(Group != Game->m_Group)
							Finding("warning", "physics", std::string("the ") + KindName(pTiles->m_Kind) + " layer is not in the game layer's group", Where);
						const CTileStats Stats = CountTiles(*pTiles, CTileRect{0, 0, pTiles->Width(), pTiles->Height()});
						for(const CTileCount &Entry : Stats.m_vCounts)
						{
							const char *pExplain = ExplainTile(pTiles->m_Kind, Entry.m_Index);
							CJson TileWhere = Where;
							TileWhere.Set("index", CJson::Int(Entry.m_Index));
							TileWhere.Set("count", CJson::Int((int64_t)Entry.m_Count));
							if(!TileIsUsed(pTiles->m_Kind, Entry.m_Index))
								Finding("warning", "tiles", "index " + std::to_string(Entry.m_Index) + " does nothing in a " + KindName(pTiles->m_Kind) + " layer (" + std::to_string(Entry.m_Count) + " tiles); tiles.find shows where", TileWhere);
							else if(pExplain != nullptr && str_find_nocase(pExplain, "deprecated") != nullptr)
								Finding("warning", "tiles", "index " + std::to_string(Entry.m_Index) + " is deprecated: " + pExplain, TileWhere);
						}
						if(pTiles->m_Kind == ETileLayerKind::TELE)
						{
							std::set<int> Ins;
							std::set<int> Outs;
							std::set<int> CheckIns;
							std::set<int> CheckOuts;
							for(int y = 0; y < pTiles->Height(); ++y)
							{
								for(int x = 0; x < pTiles->Width(); ++x)
								{
									const CTileValue Value = GetTileValue(*pTiles, x, y);
									const int Type = Value.Index();
									const int Number = Value.m_aFields[1];
									if(Type == TILE_TELEIN || Type == TILE_TELEINEVIL || Type == TILE_TELEINWEAPON || Type == TILE_TELEINHOOK)
										Ins.insert(Number);
									else if(Type == TILE_TELEOUT)
										Outs.insert(Number);
									else if(Type == TILE_TELECHECKIN || Type == TILE_TELECHECKINEVIL)
										CheckIns.insert(Number);
									else if(Type == TILE_TELECHECKOUT)
										CheckOuts.insert(Number);
								}
							}
							for(const int Number : Ins)
							{
								if(!Outs.contains(Number))
								{
									CJson TeleWhere = Where;
									TeleWhere.Set("number", CJson::Int(Number));
									Finding("error", "tele", "teleporter " + std::to_string(Number) + " has an entrance but no exit (index 27 with that number)", TeleWhere);
								}
							}
							for(const int Number : Outs)
							{
								if(!Ins.contains(Number))
								{
									CJson TeleWhere = Where;
									TeleWhere.Set("number", CJson::Int(Number));
									Finding("warning", "tele", "teleporter exit " + std::to_string(Number) + " has no entrance", TeleWhere);
								}
							}
							for(const int Number : CheckIns)
							{
								if(!CheckOuts.contains(Number))
								{
									CJson TeleWhere = Where;
									TeleWhere.Set("number", CJson::Int(Number));
									Finding("error", "tele", "checkpoint teleporter " + std::to_string(Number) + " has an entrance but no exit (index 30)", TeleWhere);
								}
							}
						}
						if(pTiles->m_Kind == ETileLayerKind::SWITCH)
						{
							std::set<int> Togglers;
							std::set<int> Switched;
							for(int y = 0; y < pTiles->Height(); ++y)
							{
								for(int x = 0; x < pTiles->Width(); ++x)
								{
									const CTileValue Value = GetTileValue(*pTiles, x, y);
									if(Value.IsAir())
										continue;
									const int Type = Value.Index();
									if(Type == TILE_SWITCHOPEN || Type == TILE_SWITCHCLOSE || Type == TILE_SWITCHTIMEDOPEN || Type == TILE_SWITCHTIMEDCLOSE)
										Togglers.insert(Value.m_aFields[1]);
									else
										Switched.insert(Value.m_aFields[1]);
								}
							}
							for(const int Number : Togglers)
							{
								if(Number != 0 && !Switched.contains(Number))
								{
									CJson SwitchWhere = Where;
									SwitchWhere.Set("number", CJson::Int(Number));
									Finding("warning", "switch", "switch " + std::to_string(Number) + " toggles nothing: no tile of the switch layer carries that number", SwitchWhere);
								}
							}
						}
					}
				}
			}

			const std::vector<size_t> vUnusedEnvelopes = UnusedEnvelopes(State);
			for(const size_t Envelope : vUnusedEnvelopes)
			{
				CJson Where = CJson::Object();
				Where.Set("envelope", CJson::Int((int64_t)Envelope));
				Finding("info", "envelopes", "envelope " + std::to_string(Envelope) + " ('" + State.Envelope(Envelope)->m_Name + "') is bound to nothing; map.apply {op: envelope.deleteUnused} removes such envelopes", Where);
			}
			std::vector<bool> vImageUsed(State.NumImages(), false);
			for(size_t Group = 0; Group < State.NumGroups(); ++Group)
			{
				for(size_t Layer = 0; Layer < State.NumLayers(Group); ++Layer)
				{
					const CLayer &Found = *State.Layer(Group, Layer);
					int Image = -1;
					if(const CTileLayer *pTiles = std::get_if<CTileLayer>(&Found); pTiles != nullptr)
						Image = pTiles->m_Image;
					else if(const CQuadLayer *pQuads = std::get_if<CQuadLayer>(&Found); pQuads != nullptr)
						Image = pQuads->m_Image;
					if(Image >= 0 && (size_t)Image < vImageUsed.size())
						vImageUsed[Image] = true;
				}
			}
			for(size_t Image = 0; Image < State.NumImages(); ++Image)
			{
				const map_document::CImage &Found = *State.Image(Image);
				CJson Where = CJson::Object();
				Where.Set("image", CJson::Int((int64_t)Image));
				if(!vImageUsed[Image])
					Finding("info", "images", "image " + std::to_string(Image) + " ('" + Found.m_Name + "') is drawn by no layer", Where);
				if(Found.m_External && !Mcp.m_pStorage->FileExists(("mapres/" + Found.m_Name + ".png").c_str(), IStorage::TYPE_ALL))
					Finding("error", "images", "image " + std::to_string(Image) + " ('" + Found.m_Name + "') is external, but the game has no mapres of that name; embed it or pick one from ddnet://mapres", Where);
			}
			CJson SettingProblems;
			std::string ParseError;
			if(CJson::Parse(SettingProblemsJson(State).c_str(), &SettingProblems, &ParseError) && SettingProblems.IsArray())
			{
				for(size_t i = 0; i < SettingProblems.Size(); ++i)
				{
					const CJson &Problem = SettingProblems.At(i);
					const std::string Message = Problem.IsObject() ? Problem.Get("problem").AsCStr(Problem.Get("error").AsCStr("")) : Problem.AsCStr("");
					if(!Message.empty())
					{
						CJson Where = CJson::Object();
						Where.Set("line", Problem.IsObject() ? Problem.Get("line") : CJson::Int((int64_t)i));
						Finding("warning", "settings", Message, Where);
					}
				}
			}
			size_t Errors = 0;
			size_t Warnings = 0;
			for(size_t i = 0; i < Findings.Size(); ++i)
			{
				const std::string Severity = Findings.At(i).Get("severity").AsCStr();
				Errors += Severity == "error";
				Warnings += Severity == "warning";
			}
			CToolResult Result;
			Result.m_Structured.Set("map", CJson::Str(pMap->m_Handle));
			Result.m_Structured.Set("errors", CJson::Int((int64_t)Errors));
			Result.m_Structured.Set("warnings", CJson::Int((int64_t)Warnings));
			Result.m_Structured.Set("findings", Findings);
			Result.m_Text = std::to_string(Errors) + " errors, " + std::to_string(Warnings) + " warnings, " + std::to_string(Findings.Size() - Errors - Warnings) + " notes";
			return Result;
		}

		static CToolResult SettingsCheck(CMapMcp &, const CJson &Args)
		{
			const CJson &Line = Args.Get("line");
			if(!Line.IsString())
				return CToolResult::Error("'line' is needed: one line of a map's settings");
			const std::string Problem = CheckSetting(Line.AsCStr());
			CToolResult Result;
			Result.m_Structured.Set("line", Line);
			Result.m_Structured.Set("ok", CJson::Bool(Problem.empty()));
			Result.m_Structured.Set("problem", Problem.empty() ? CJson::Null() : CJson::Str(Problem));
			Result.m_Text = Problem.empty() ? "The line is fine" : Problem;
			return Result;
		}

		static CToolResult AutomapRules(CMapMcp &Mcp, const CJson &Args)
		{
			std::vector<std::string> vNames;
			Mcp.m_pStorage->ListDirectory(
				IStorage::TYPE_ALL, "editor/automap", [](const char *pName, int IsDir, int, void *pUser) {
					if(!IsDir && str_endswith(pName, ".rules"))
						static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName, str_length(pName) - str_length(".rules"));
					return 0;
				},
				&vNames);
			std::sort(vNames.begin(), vNames.end());
			const char *pImage = Args.Get("image").AsCStr(nullptr);
			CJson List = CJson::Array();
			for(const std::string &Name : vNames)
			{
				if(pImage != nullptr && Name != pImage)
					continue;
				CAutomapRules Rules;
				std::vector<int> vNotUnderstood;
				std::string Error;
				CJson Json = CJson::Object();
				Json.Set("image", CJson::Str(Name));
				if(!LoadRules(Mcp, Name.c_str(), &Rules, &vNotUnderstood, &Error))
				{
					Json.Set("error", CJson::Str(Error));
					List.Push(Json);
					continue;
				}
				CJson Configs = CJson::Array();
				for(size_t i = 0; i < Rules.NumConfigs(); ++i)
					Configs.Push(CJson::Str(Rules.ConfigName(i)));
				Json.Set("configs", Configs);
				if(!vNotUnderstood.empty())
				{
					CJson Lines = CJson::Array();
					for(const int Line : vNotUnderstood)
						Lines.Push(CJson::Int(Line));
					Json.Set("linesNotUnderstood", Lines);
				}
				List.Push(Json);
			}
			if(pImage != nullptr && List.Size() == 0)
				return CToolResult::Error(std::string("there is no rules file for '") + pImage + "'");
			CToolResult Result;
			Result.m_Structured.Set("rules", List);
			Result.m_Text = std::to_string(List.Size()) + " rules files";
			return Result;
		}

		static CToolResult MapRender(CMapMcp &Mcp, const CJson &Args)
		{
			std::string Error;
			COpenMap *pMap = Map(Mcp, Args, &Error);
			if(pMap == nullptr)
				return CToolResult::Error(Error);
			if(Mcp.m_pRenderer == nullptr)
				return CToolResult::Error("this server was started without a renderer");
			const CMapState &State = pMap->m_Document.Map();
			CRenderRequest Request;
			Request.m_Width = IntArg(Args, "width", 1024, &Error);
			Request.m_Height = IntArg(Args, "height", 512, &Error);
			Request.m_EntityOverlay = IntArg(Args, "overlay", Args.Has("entities") ? 100 : 0, &Error);
			Request.m_Grid = IntArg(Args, "grid", 0, &Error);
			Request.m_TimeOffsetMillis = IntArg(Args, "time", 0, &Error);
			if(!Error.empty())
				return CToolResult::Error(Error);
			if(Request.m_Width < 16 || Request.m_Height < 16 || Request.m_Width > MAX_RENDER_SIDE || Request.m_Height > MAX_RENDER_SIDE)
				return CToolResult::Error("a picture is 16 to " + std::to_string(MAX_RENDER_SIDE) + " pixels a side");
			if(Request.m_EntityOverlay < 0 || Request.m_EntityOverlay > 100)
				return CToolResult::Error("'overlay' is 0 to 100");
			if(Args.Get("entities").IsString())
				Request.m_Entities = Args.Get("entities").AsString();
			if(!Mcp.m_pStorage->FileExists(("editor/entities_clear/" + Request.m_Entities + ".png").c_str(), IStorage::TYPE_ALL))
				return CToolResult::Error("there is no entities sheet called '" + Request.m_Entities + "'; ddnet, ddrace, f-ddrace, fng, race, vanilla and blockworlds are the ones there are");
			const std::optional<CLayerAddress> Game = FindGameLayer(State);
			vec2 WorldSize(Request.m_Width, Request.m_Height);
			if(Game.has_value())
			{
				const CTileLayer *pGame = State.TileLayer(Game->m_Group, Game->m_Layer);
				WorldSize = vec2(pGame->Width() * 32.0f, pGame->Height() * 32.0f);
				Request.m_GridGroup = Game->m_Group;
			}
			CView Camera;
			Camera.SetSurface(Request.m_Width, Request.m_Height);
			Camera.Fit(WorldSize);
			const CJson &Region = Args.Get("region");
			if(Region.IsObject())
			{
				CTileRect Rect;
				Rect.m_X = IntArg(Region, "x", 0, &Error);
				Rect.m_Y = IntArg(Region, "y", 0, &Error);
				Rect.m_Width = IntArg(Region, "w", 0, &Error);
				Rect.m_Height = IntArg(Region, "h", 0, &Error);
				if(!Error.empty() || Rect.Empty())
					return CToolResult::Error(Error.empty() ? "'region' is {x, y, w, h} in tiles" : Error);
				Camera.Fit(vec2(Rect.m_Width * 32.0f, Rect.m_Height * 32.0f));
				Camera.SetCenter(vec2((Rect.m_X + Rect.m_Width / 2.0f) * 32.0f, (Rect.m_Y + Rect.m_Height / 2.0f) * 32.0f));
			}
			else if(Args.Get("center").IsObject())
			{
				const CJson &Center = Args.Get("center");
				Camera.SetCenter(vec2((float)Center.Get("x").AsDouble() * 32.0f, (float)Center.Get("y").AsDouble() * 32.0f));
				if(Args.Has("zoom"))
					Camera.SetZoom((float)Args.Get("zoom").AsDouble(1.0));
			}
			Request.m_Center = Camera.Center();
			Request.m_Zoom = Camera.Zoom();
			const CJson &Hide = Args.Get("hide");
			for(size_t i = 0; i < Hide.Size(); ++i)
			{
				const char *pPair = Hide.At(i).AsCStr("");
				const char *pColon = str_find(pPair, ":");
				if(pColon == nullptr)
					return CToolResult::Error("'hide' is a list of \"group:layer\"");
				Request.m_vHidden.emplace_back((size_t)std::max(0, str_toint(pPair)), (size_t)std::max(0, str_toint(pColon + 1)));
			}
			const CJson &Mark = Args.Get("mark");
			if(Mark.IsObject())
			{
				Request.m_MarkGroup = (size_t)std::max(0, IntArg(Mark, "group", Game.has_value() ? (int)Game->m_Group : 0, &Error));
				Request.m_Mark.m_X = IntArg(Mark, "x", 0, &Error);
				Request.m_Mark.m_Y = IntArg(Mark, "y", 0, &Error);
				Request.m_Mark.m_Width = IntArg(Mark, "w", 0, &Error);
				Request.m_Mark.m_Height = IntArg(Mark, "h", 0, &Error);
			}
			const CJson &Quad = Args.Get("quad");
			if(Quad.IsObject())
			{
				Request.m_ShowQuad = true;
				Request.m_QuadGroup = (size_t)std::max(0, IntArg(Quad, "group", 0, &Error));
				Request.m_QuadLayer = (size_t)std::max(0, IntArg(Quad, "layer", 0, &Error));
				Request.m_Quad = (size_t)std::max(0, IntArg(Quad, "quad", 0, &Error));
			}
			if(!Error.empty())
				return CToolResult::Error(Error);
			std::vector<uint8_t> vPng;
			int Width = 0;
			int Height = 0;
			const int64_t Start = time_get_nanoseconds().count();
			if(!Mcp.m_pRenderer->Render(State, Request, &vPng, &Width, &Height, &Error))
				return CToolResult::Error(Error);
			const int64_t Millis = (time_get_nanoseconds().count() - Start) / 1000000;
			const vec2 Visible = Camera.VisibleSize();
			CTileRect View;
			View.m_X = (int)std::floor((Camera.Center().x - Visible.x / 2.0f) / 32.0f);
			View.m_Y = (int)std::floor((Camera.Center().y - Visible.y / 2.0f) / 32.0f);
			View.m_Width = (int)std::ceil(Visible.x / 32.0f);
			View.m_Height = (int)std::ceil(Visible.y / 32.0f);
			CToolResult Result;
			Result.m_Structured.Set("map", CJson::Str(pMap->m_Handle));
			Result.m_Structured.Set("width", CJson::Int(Width));
			Result.m_Structured.Set("height", CJson::Int(Height));
			Result.m_Structured.Set("view", RectJson(View));
			Result.m_Structured.Set("pixelsPerTile", CJson::Double((double)Width / std::max(1.0f, Visible.x / 32.0f)));
			Result.m_Structured.Set("bytes", CJson::Int((int64_t)vPng.size()));
			Result.m_Structured.Set("renderMillis", CJson::Int(Millis));
			Result.m_vImages.push_back(std::move(vPng));
			Result.m_Text = "Tiles " + std::to_string(View.m_X) + "," + std::to_string(View.m_Y) + " to " + std::to_string(View.m_X + View.m_Width) + "," + std::to_string(View.m_Y + View.m_Height) + " at " + std::to_string(Width) + "x" + std::to_string(Height);
			return Result;
		}
	};

	namespace
	{
		constexpr const char *MAP_PROP = "\"map\":{\"type\":\"string\",\"description\":\"Handle from map.open or map.new (m1), or a map path relative to the root\"}";
		constexpr const char *LAYER_PROPS = "\"group\":{\"type\":\"integer\"},\"layer\":{\"type\":\"integer\"}";
		constexpr const char *RECT_PROPS = "\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"w\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"}";
		constexpr const char *CHANGE_PROPS = "\"label\":{\"type\":\"string\",\"description\":\"Name of the history entry\"},\"dryRun\":{\"type\":\"boolean\",\"description\":\"Run and report, but keep nothing\"},\"render\":{\"type\":\"boolean\",\"description\":\"Attach a picture of the changed rectangle\"}";

		std::string Schema(const std::string &Properties, const std::string &Required)
		{
			return "{\"type\":\"object\",\"properties\":{" + Properties + "},\"required\":[" + Required + "]}";
		}

		/** Schemas live as long as the program: the table points into them. */
		const char *Keep(const std::string &Text)
		{
			static std::vector<std::unique_ptr<std::string>> s_vKept;
			s_vKept.push_back(std::make_unique<std::string>(Text));
			return s_vKept.back()->c_str();
		}

		CTool Tool(const char *pName, const char *pTitle, const char *pDescription, const std::string &Schema, bool ReadOnly, bool Destructive, bool Idempotent, std::function<CToolResult(CMapMcp &, const CJson &)> Handler)
		{
			return CTool{pName, pTitle, pDescription, Keep(Schema), ReadOnly, Destructive, Idempotent, std::move(Handler)};
		}
	} // namespace

	std::vector<CTool> MakeTools()
	{
		const std::string Map = MAP_PROP;
		const std::string Layer = std::string(MAP_PROP) + "," + LAYER_PROPS;
		const std::string Rect = Layer + "," + RECT_PROPS;
		std::vector<CTool> vTools;

		// 5.1 session and files
		vTools.push_back(Tool("maps.list", "List map files", "Lists the .map files in the root directory or one of its subdirectories, with size and modification time.",
			Schema("\"subdir\":{\"type\":\"string\",\"description\":\"Subdirectory of the root, or omitted for the root itself\"}", ""), true, false, true, CTools::MapsList));
		vTools.push_back(Tool("map.open", "Open a map", "Opens a map file from the root directory and answers with a handle (m1, m2, ...) and a summary of its groups, layers and images. The handle is valid until map.close or until this server stops; a file that is already open answers with its existing handle.",
			Schema("\"path\":{\"type\":\"string\",\"description\":\"Path of the .map file relative to the root\"}", "\"path\""), true, false, true, CTools::MapOpen));
		vTools.push_back(Tool("map.new", "Make a new map", "Makes an empty map with a game group and a game layer of the given size, open under a new handle. It has no file until map.save is called with a path.",
			Schema("\"width\":{\"type\":\"integer\",\"default\":100},\"height\":{\"type\":\"integer\",\"default\":50},\"name\":{\"type\":\"string\"}", ""), false, false, false, CTools::MapNew));
		vTools.push_back(Tool("map.save", "Save a map", "Writes the map to its file, or to another path under the root. An existing file other than the map's own is only replaced with overwrite: true. Written atomically; answers with the size and SHA-256 of the file.",
			Schema(Map + ",\"path\":{\"type\":\"string\",\"description\":\"Where to save, relative to the root; the map's own file when omitted\"},\"overwrite\":{\"type\":\"boolean\"}", "\"map\""), false, true, true, CTools::MapSave));
		vTools.push_back(Tool("map.close", "Close a map", "Closes an open map and frees its memory. A map with unsaved changes is only closed with discard: true.",
			Schema(Map + ",\"discard\":{\"type\":\"boolean\"}", "\"map\""), false, true, true, CTools::MapClose));
		vTools.push_back(Tool("map.list", "List open maps", "Lists the open maps with their handles, files, unsaved state and history size, and the server's limits.",
			Schema("", ""), true, false, true, CTools::MapList));
		vTools.push_back(Tool("map.append", "Append a map", "Puts the groups, images, sounds, envelopes and settings of another map file into this map (not its game layer), and reports what came over and what was shared or renamed.",
			Schema(Map + ",\"path\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}", "\"map\",\"path\""), false, false, false, CTools::MapAppend));

		// 5.2 reading
		vTools.push_back(Tool("map.structure", "Map structure", "The groups, layers (with kind and size), images, envelopes and settings of a map. detail: summary (default) is short; full is the document's whole structure JSON with every property.",
			Schema(Map + ",\"detail\":{\"type\":\"string\",\"enum\":[\"summary\",\"full\"]}", "\"map\""), true, false, true, CTools::MapStructure));
		vTools.push_back(Tool("map.envelope", "Envelope points", "The points of one envelope: times in milliseconds, values in 22.10 fixed point.",
			Schema(Map + ",\"envelope\":{\"type\":\"integer\"}", "\"map\",\"envelope\""), true, false, true, CTools::MapEnvelope));
		vTools.push_back(Tool("map.quads", "Quads of a layer", "The quads of one quad layer, with corners, colours, texture coordinates and envelopes.",
			Schema(Layer, "\"map\",\"group\",\"layer\""), true, false, true, [](CMapMcp &Mcp, const CJson &Args) { return CTools::LayerListOf(Mcp, Args, true); }));
		vTools.push_back(Tool("map.sources", "Sound sources of a layer", "The sound sources of one sound layer.",
			Schema(Layer, "\"map\",\"group\",\"layer\""), true, false, true, [](CMapMcp &Mcp, const CJson &Args) { return CTools::LayerListOf(Mcp, Args, false); }));
		vTools.push_back(Tool("map.history", "Undo history", "The undo history of a map: every entry with its label and time, which one is current, and the bytes held.",
			Schema(Map, "\"map\""), true, false, true, CTools::MapHistory));
		vTools.push_back(Tool("tiles.read", "Read tiles", "A rectangle of a tile layer as text. Encodings: rle (default; one line per row, idx or idx/flags, a run written once as idxxN), rows (one token per tile), sparse (x,y:idx for non-air tiles), glyph (one character per tile with a legend; reading only). Physics layers answer with type/number tokens. At most 128 by 128 tiles per call, 256 by 256 with large: true; use tiles.stats and tiles.find for a whole layer. Read ddnet://docs/encodings for the details.",
			Schema(Rect + ",\"encoding\":{\"type\":\"string\",\"enum\":[\"rle\",\"rows\",\"sparse\",\"glyph\"]},\"large\":{\"type\":\"boolean\"}", "\"map\",\"group\",\"layer\",\"x\",\"y\",\"w\",\"h\""), true, false, true,
			[](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "tiles.read", "", false); }));
		vTools.push_back(Tool("tiles.stats", "Count tiles", "For one tile layer, or every tile layer of the map: how many tiles are not air, the bounding box, the used blocks, and a histogram of indices with their meaning for physics layers (used: false marks an index the game ignores there).",
			Schema(Rect, "\"map\""), true, false, true, [](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "tiles.stats", "", false); }));
		vTools.push_back(Tool("tiles.find", "Find tiles", "Where tiles of the given indices are, as runs along rows (x, y, len, index). Names one layer with group and layer, or every layer of a kind (default game). At most limit runs (default 200); total says how many there are.",
			Schema(Rect + ",\"kind\":{\"type\":\"string\",\"enum\":[\"game\",\"front\",\"tele\",\"speedup\",\"switch\",\"tune\",\"tiles\"]},\"index\":{\"type\":\"integer\"},\"indices\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"}},\"limit\":{\"type\":\"integer\"}", "\"map\""), true, false, true,
			[](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "tiles.find", "", false); }));
		vTools.push_back(Tool("tile.explain", "Explain a tile", "What a tile index does in a physics layer of the given kind, in a sentence, and whether the game reads it there at all.",
			Schema("\"kind\":{\"type\":\"string\",\"enum\":[\"game\",\"front\",\"tele\",\"speedup\",\"switch\",\"tune\"]},\"index\":{\"type\":\"integer\"}", "\"kind\",\"index\""), true, false, true, CTools::TileExplain));
		vTools.push_back(Tool("tile.at", "What is at a tile", "What every physics layer says at one place, with numbers and meaning, and which design layers draw something there.",
			Schema(Map + ",\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}", "\"map\",\"x\",\"y\""), true, false, true, CTools::TileAt));
		vTools.push_back(Tool("map.proof", "Player's view", "What a player standing at a tile sees: the visible rectangle at the game's zoom (or the menu's with menu: true), and with render: true a picture of it.",
			Schema(Map + ",\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"menu\":{\"type\":\"boolean\"},\"render\":{\"type\":\"boolean\"}", "\"map\",\"x\",\"y\""), true, false, true, CTools::MapProof));

		// 5.3 changing
		vTools.push_back(Tool("map.apply", "Apply document commands", "Runs a list of the document's commands as one change (one undo step): group.add/delete/move/setProp, layer.add/delete/move/setProp/resize/type/constructGameTiles, envelope.add/delete/deleteUnused/setProp, envelope.point.add/delete/set, quad.add/delete/setPoint/setColor/setTexcoord/setProp/shape/carve, source.add/delete/setPoint/setProp, image.add/delete/setProp, sound.add/delete/setProp, info.setProp, info.settings.add/set/delete, and the tiles.* commands. If one fails nothing is changed. dryRun runs them and keeps nothing.",
			Schema(Map + ",\"ops\":{\"type\":\"array\",\"items\":{\"type\":\"object\"},\"description\":\"Commands, each an object with an op and its arguments\"}," + CHANGE_PROPS, "\"map\",\"ops\""), false, true, false, CTools::MapApply));
		vTools.push_back(Tool("tiles.write", "Write tiles", "Writes a rectangle of tiles from text at x, y of a tile layer. The text is in the encoding of tiles.read (rle default; rows; sparse); w and h may be omitted and are taken from the text. mode: replace (default) writes air too, overlay leaves the layer where the text has air. Tiles a physics layer does not read are dropped and counted unless allowUnused is true. One undo step.",
			Schema(Rect + ",\"tiles\":{\"type\":\"string\"},\"encoding\":{\"type\":\"string\",\"enum\":[\"rle\",\"rows\",\"sparse\"]},\"mode\":{\"type\":\"string\",\"enum\":[\"replace\",\"overlay\"]},\"allowUnused\":{\"type\":\"boolean\"}," + CHANGE_PROPS, "\"map\",\"group\",\"layer\",\"x\",\"y\",\"tiles\""), false, true, false,
			[](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "tiles.write", "Write tiles", true); }));
		vTools.push_back(Tool("tiles.fill", "Fill a rectangle", "Writes one tile over a rectangle, or with border: n over its rim only. For physics layers index is the tile type and number, delay, force, maxSpeed and angle go beside it; flags turn a drawn tile (1 mirror, 2 flip, 8 rotate).",
			Schema(Rect + ",\"index\":{\"type\":\"integer\"},\"flags\":{\"type\":\"integer\"},\"number\":{\"type\":\"integer\"},\"delay\":{\"type\":\"integer\"},\"force\":{\"type\":\"integer\"},\"maxSpeed\":{\"type\":\"integer\"},\"angle\":{\"type\":\"integer\"},\"border\":{\"type\":\"integer\"},\"allowUnused\":{\"type\":\"boolean\"}," + CHANGE_PROPS, "\"map\",\"group\",\"layer\",\"x\",\"y\",\"w\",\"h\",\"index\""), false, true, false,
			[](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "tiles.fill", "Fill tiles", true); }));
		vTools.push_back(Tool("tiles.replace", "Replace an index", "Turns every tile of one index into another, keeping flags and numbers, on one layer (group and layer) or on every layer of a kind (default game), optionally within a rectangle. Answers with the count per layer.",
			Schema(Rect + ",\"kind\":{\"type\":\"string\"},\"from\":{\"type\":\"integer\"},\"to\":{\"type\":\"integer\"},\"allowUnused\":{\"type\":\"boolean\"}," + CHANGE_PROPS, "\"map\",\"from\",\"to\""), false, true, false,
			[](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "tiles.replace", "Replace tiles", true); }));
		vTools.push_back(Tool("tiles.brush", "Copy tiles", "Takes a rectangle of one layer as a brush, optionally mirrored (flipX, flipY) or turned (rotate 90/180/270), and stamps it at a place on a layer of a compatible kind - once, or repeated over repeat {w, h} tiles.",
			Schema(Map + ",\"from\":{\"type\":\"object\",\"properties\":{" + LAYER_PROPS + "," + RECT_PROPS + "},\"required\":[\"group\",\"layer\",\"x\",\"y\",\"w\",\"h\"]},\"to\":{\"type\":\"object\",\"properties\":{" + LAYER_PROPS + ",\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"}},\"required\":[\"group\",\"layer\",\"x\",\"y\"]},\"flipX\":{\"type\":\"boolean\"},\"flipY\":{\"type\":\"boolean\"},\"rotate\":{\"type\":\"integer\"},\"repeat\":{\"type\":\"object\",\"properties\":{\"w\":{\"type\":\"integer\"},\"h\":{\"type\":\"integer\"}}}," + CHANGE_PROPS, "\"map\",\"from\",\"to\""), false, true, false, CTools::TilesBrush));
		vTools.push_back(Tool("automap.run", "Run the automapper", "Draws a design layer from the rules of its image (or of rules) with a configuration (name or index), over the whole layer or a rectangle. seed picks the random choices (same seed, same result); reference filters the first run by a physics tile (-1 none, 0 game layer as it is, 1-9 hookable, death, unhookable, freeze, unfreeze, deep freeze, deep unfreeze, live freeze, live unfreeze).",
			Schema(Rect + ",\"rules\":{\"type\":\"string\"},\"config\":{\"type\":[\"string\",\"integer\"]},\"seed\":{\"type\":\"integer\"},\"reference\":{\"type\":\"integer\"}," + CHANGE_PROPS, "\"map\",\"group\",\"layer\""), false, true, false, CTools::AutomapRun));
		vTools.push_back(Tool("art.tiles", "Picture as tiles", "Adds a PNG from the root as a new group of tile layers, one tile per pixel (at most 256 colours).",
			Schema(Map + ",\"path\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}", "\"map\",\"path\""), false, false, false, [](CMapMcp &Mcp, const CJson &Args) { return CTools::Art(Mcp, Args, false); }));
		vTools.push_back(Tool("art.quads", "Picture as quads", "Adds a PNG from the root as a new group of quads, pixelStep pixels per quad of quadSize world units, neighbouring quads of one colour merged unless merge is false.",
			Schema(Map + ",\"path\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"pixelStep\":{\"type\":\"integer\"},\"quadSize\":{\"type\":\"integer\"},\"centralize\":{\"type\":\"boolean\"},\"merge\":{\"type\":\"boolean\"},\"label\":{\"type\":\"string\"}", "\"map\",\"path\""), false, false, false, [](CMapMcp &Mcp, const CJson &Args) { return CTools::Art(Mcp, Args, true); }));
		vTools.push_back(Tool("text.type", "Type text as tiles", "Writes text onto a design layer with the font tileset (letters and digits), starting at x, y.",
			Schema(Layer + ",\"x\":{\"type\":\"integer\"},\"y\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"}," + CHANGE_PROPS, "\"map\",\"group\",\"layer\",\"x\",\"y\",\"text\""), false, true, false,
			[](CMapMcp &Mcp, const CJson &Args) { return CTools::Passthrough(Mcp, Args, "layer.type", "Type", true); }));
		vTools.push_back(Tool("image.add", "Add an image", "Adds an image to the map: by name from the game's mapres (external, read ddnet://mapres), or embedded from a PNG under the root with path. Answers with the image index to set on a layer.",
			Schema(Map + ",\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"},\"label\":{\"type\":\"string\"}", "\"map\""), false, false, false, CTools::ImageAdd));
		vTools.push_back(Tool("history.undo", "Undo", "Steps the map back one change.", Schema(Map, "\"map\""), false, false, false, [](CMapMcp &Mcp, const CJson &Args) { return CTools::History(Mcp, Args, "history.undo"); }));
		vTools.push_back(Tool("history.redo", "Redo", "Steps the map forward one change that was undone.", Schema(Map, "\"map\""), false, false, false, [](CMapMcp &Mcp, const CJson &Args) { return CTools::History(Mcp, Args, "history.redo"); }));
		vTools.push_back(Tool("history.jump", "Jump in the history", "Goes straight to a history entry by index (see map.history).", Schema(Map + ",\"index\":{\"type\":\"integer\"}", "\"map\",\"index\""), false, false, true, [](CMapMcp &Mcp, const CJson &Args) { return CTools::History(Mcp, Args, "history.jump"); }));

		// 5.4 checking and rendering
		vTools.push_back(Tool("map.check", "Check a map", "Reports what is wrong with a map: file warnings, a missing game layer, spawn, start or finish, physics layers of the wrong size, indices the game ignores or deprecates, teleporters without exits, switches that toggle nothing, unused envelopes and images, external images the game does not have, and bad settings lines. Findings carry a where to follow up with tiles.find or map.render.",
			Schema(Map, "\"map\""), true, false, true, CTools::MapCheck));
		vTools.push_back(Tool("settings.check", "Check a settings line", "Checks one line of map settings for a known command and arguments.",
			Schema("\"line\":{\"type\":\"string\"}", "\"line\""), true, false, true, CTools::SettingsCheck));
		vTools.push_back(Tool("automap.rules", "List automapper rules", "The automapper rules files that come with the game (named after their image) with their configurations, or the one for image.",
			Schema("\"image\":{\"type\":\"string\"}", ""), true, false, true, CTools::AutomapRules));
		vTools.push_back(Tool("map.render", "Render a picture", "Draws the map as a PNG: the whole map, or region {x, y, w, h} in tiles, or center {x, y} in tiles with zoom. width and height up to 2048. entities names a sheet (ddnet, ddrace, fng, race, vanilla, blockworlds, f-ddrace) and draws the physics layers over the design (overlay 0-100, 100 with entities). hide lists \"group:layer\" not to draw, mark {group, x, y, w, h} outlines a rectangle of tiles, grid draws lines every n tiles, quad {group, layer, quad} shows a quad's corners.",
			Schema(Map + ",\"region\":{\"type\":\"object\",\"properties\":{" + RECT_PROPS + "}},\"center\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}}},\"zoom\":{\"type\":\"number\"},\"width\":{\"type\":\"integer\"},\"height\":{\"type\":\"integer\"},\"entities\":{\"type\":\"string\"},\"overlay\":{\"type\":\"integer\"},\"hide\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"mark\":{\"type\":\"object\",\"properties\":{\"group\":{\"type\":\"integer\"}," + RECT_PROPS + "}},\"grid\":{\"type\":\"integer\"},\"quad\":{\"type\":\"object\",\"properties\":{" + LAYER_PROPS + ",\"quad\":{\"type\":\"integer\"}}},\"time\":{\"type\":\"integer\"}", "\"map\""), true, false, true, CTools::MapRender));
		return vTools;
	}
} // namespace map_mcp
