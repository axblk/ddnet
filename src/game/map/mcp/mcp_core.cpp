#include "mcp_core.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/map/document/command.h>
#include <game/map/document/explain.h>
#include <game/map/document/map_file.h>
#include <game/map/document/report.h>
#include <game/map/document/structure.h>
#include <game/map/mcp/mcp_tools.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <vector>

#if !defined(CONF_FAMILY_WINDOWS)
#include <sys/stat.h>
#endif

namespace map_mcp
{
	CToolResult CToolResult::Error(const std::string &Message)
	{
		CToolResult Result;
		Result.m_IsError = true;
		Result.m_Text = Message;
		Result.m_Structured = CJson::Null();
		return Result;
	}

	namespace
	{
		/** The absolute, symlink-free path of something that exists, or empty. */
		std::string RealPath(const char *pPath)
		{
#if defined(CONF_FAMILY_WINDOWS)
			char aBuffer[IO_MAX_PATH_LENGTH];
			if(_fullpath(aBuffer, pPath, sizeof(aBuffer)) == nullptr)
				return "";
			// The paths are compared with '/' between their parts, and
			// Windows answers with '\\'.
			std::string Resolved = aBuffer;
			std::replace(Resolved.begin(), Resolved.end(), '\\', '/');
			return Resolved;
#else
			char *pResolved = realpath(pPath, nullptr);
			if(pResolved == nullptr)
				return "";
			std::string Resolved = pResolved;
			free(pResolved);
			return Resolved;
#endif
		}

		bool IsSymlink(const char *pPath)
		{
#if defined(CONF_FAMILY_WINDOWS)
			(void)pPath;
			return false;
#else
			struct stat Stat;
			return lstat(pPath, &Stat) == 0 && S_ISLNK(Stat.st_mode);
#endif
		}

		/** Whether `Path` is `Root` or lies below it. */
		bool Under(const std::string &Path, const std::string &Root)
		{
			if(Path == Root)
				return true;
			// A root that ends in a separator is a drive or `/` itself.
			const bool Separated = !Root.empty() && Root.back() == '/';
			return Path.size() > Root.size() && Path.compare(0, Root.size(), Root) == 0 && (Separated || Path[Root.size()] == '/');
		}

		const char *KindName(map_document::ETileLayerKind Kind)
		{
			switch(Kind)
			{
			case map_document::ETileLayerKind::TILES: return "tiles";
			case map_document::ETileLayerKind::GAME: return "game";
			case map_document::ETileLayerKind::FRONT: return "front";
			case map_document::ETileLayerKind::TELE: return "tele";
			case map_document::ETileLayerKind::SPEEDUP: return "speedup";
			case map_document::ETileLayerKind::SWITCH: return "switch";
			case map_document::ETileLayerKind::TUNE: return "tune";
			}
			return "tiles";
		}

		bool ReadKindName(const char *pName, map_document::ETileLayerKind *pOut)
		{
			static const map_document::ETileLayerKind s_aKinds[] = {
				map_document::ETileLayerKind::GAME, map_document::ETileLayerKind::FRONT, map_document::ETileLayerKind::TELE,
				map_document::ETileLayerKind::SPEEDUP, map_document::ETileLayerKind::SWITCH, map_document::ETileLayerKind::TUNE};
			const auto *pFound = std::find_if(std::begin(s_aKinds), std::end(s_aKinds), [pName](map_document::ETileLayerKind Kind) { return str_comp(pName, KindName(Kind)) == 0; });
			if(pFound == std::end(s_aKinds))
				return false;
			*pOut = *pFound;
			return true;
		}
	} // namespace

	CMapMcp::CMapMcp(const COptions &Options, std::unique_ptr<IRenderer> pRenderer) :
		m_Options(Options), m_pRenderer(std::move(pRenderer))
	{
		if(Options.m_NumArgs > 0)
			m_pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::BASIC, Options.m_NumArgs, Options.m_ppArguments));
		else
			m_pStorage = CreateLocalStorage();
		if(m_pStorage == nullptr)
		{
			m_Error = "the game's data directory could not be found";
			return;
		}
		if(Options.m_Root.empty())
		{
			m_Error = "a root directory is required";
			return;
		}
		m_Root = RealPath(Options.m_Root.c_str());
		if(m_Root.empty() || !fs_is_dir(m_Root.c_str()))
		{
			m_Error = "the root directory '" + Options.m_Root + "' is not a directory";
			return;
		}
		m_vTools = MakeTools();
	}

	CMapMcp::~CMapMcp() = default;

	std::string CMapMcp::Instructions() const
	{
		return "Tools for reading and changing DDNet map files. "
		       "Open a map with map.open (path relative to the root directory) or make one with map.new; both answer with a handle such as m1, and every other tool names the map by that handle. "
		       "A handle is valid until map.close or until this server stops; a call against an unknown handle fails and says to open the map again. Passing the map's path as the handle opens it on the way. "
		       "Changes stay in memory until map.save. Every change is one undo step: history.undo takes it back. "
		       "Tiles are read and written as text (see tiles.read); read no more than 128 by 128 tiles at once and ask tiles.stats or tiles.find for questions about a whole layer. "
		       "Coordinates are in tiles from the top left corner; a tile is 32 world units. map.render draws a picture; map.check reports what is wrong with a map.";
	}

	CJson CMapMcp::ToolsJson() const
	{
		CJson Tools = CJson::Array();
		for(const CTool &Tool : m_vTools)
		{
			CJson Json = CJson::Object();
			Json.Set("name", CJson::Str(Tool.m_pName));
			Json.Set("title", CJson::Str(Tool.m_pTitle));
			Json.Set("description", CJson::Str(Tool.m_pDescription));
			Json.Set("inputSchema", CJson::Raw(Tool.m_pInputSchema));
			CJson Annotations = CJson::Object();
			Annotations.Set("readOnlyHint", CJson::Bool(Tool.m_ReadOnly));
			Annotations.Set("destructiveHint", CJson::Bool(Tool.m_Destructive));
			Annotations.Set("idempotentHint", CJson::Bool(Tool.m_Idempotent));
			Annotations.Set("openWorldHint", CJson::Bool(false));
			Json.Set("annotations", Annotations);
			Tools.Push(Json);
		}
		return Tools;
	}

	CToolResult CMapMcp::CallTool(const char *pName, const CJson &Arguments, bool *pUnknownTool)
	{
		*pUnknownTool = false;
		for(const CTool &Tool : m_vTools)
		{
			if(str_comp(Tool.m_pName, pName) != 0)
				continue;
			if(!Arguments.IsObject() && !Arguments.IsNull())
				return CToolResult::Error("the arguments have to be an object");
			return Tool.m_Handler(*this, Arguments);
		}
		*pUnknownTool = true;
		return CToolResult::Error(std::string("there is no tool called '") + pName + "'");
	}

	CJson CMapMcp::ResultJson(const CToolResult &Result)
	{
		CJson Json = CJson::Object();
		CJson Content = CJson::Array();
		std::string Text = Result.m_Text;
		if(!Result.m_Structured.IsNull())
		{
			if(!Text.empty())
				Text += '\n';
			Text += Result.m_Structured.Serialize();
		}
		CJson TextBlock = CJson::Object();
		TextBlock.Set("type", CJson::Str("text"));
		TextBlock.Set("text", CJson::Str(Text));
		Content.Push(TextBlock);
		for(const std::vector<uint8_t> &Png : Result.m_vImages)
		{
			std::vector<char> vBase64(Png.size() / 3 * 4 + 8);
			str_base64(vBase64.data(), (int)vBase64.size(), Png.data(), (int)Png.size());
			CJson Image = CJson::Object();
			Image.Set("type", CJson::Str("image"));
			Image.Set("data", CJson::Str(vBase64.data()));
			Image.Set("mimeType", CJson::Str("image/png"));
			Content.Push(Image);
		}
		Json.Set("content", Content);
		if(!Result.m_Structured.IsNull())
			Json.Set("structuredContent", Result.m_Structured);
		if(Result.m_IsError)
			Json.Set("isError", CJson::Bool(true));
		return Json;
	}

	std::string CMapMcp::NewHandle()
	{
		if(m_Options.m_SequentialHandles)
			return "m" + std::to_string(m_NextHandle++);
		std::random_device Device;
		std::string Handle = "m";
		for(int i = 0; i < 4; ++i)
		{
			const unsigned Bits = Device();
			for(int Nibble = 0; Nibble < 4; ++Nibble)
				Handle += "0123456789abcdef"[(Bits >> (Nibble * 4)) & 15];
		}
		return Handle;
	}

	std::string CMapMcp::ResolvePath(const char *pPath, bool ForWriting, std::string *pError)
	{
		if(pPath == nullptr || pPath[0] == '\0')
		{
			*pError = "a path is needed";
			return "";
		}
		if(pPath[0] == '/' || pPath[0] == '\\' || str_find(pPath, "\\") != nullptr || (str_length(pPath) > 1 && pPath[1] == ':'))
		{
			*pError = std::string("'") + pPath + "' is not a path relative to the root directory";
			return "";
		}
		std::string Joined;
		const char *pRead = pPath;
		while(*pRead != '\0')
		{
			const char *pEnd = pRead;
			while(*pEnd != '\0' && *pEnd != '/')
				++pEnd;
			const std::string Part(pRead, pEnd);
			if(Part == "..")
			{
				*pError = std::string("'") + pPath + "' leaves the root directory";
				return "";
			}
			if(!Part.empty() && Part != ".")
			{
				if(!Joined.empty())
					Joined += '/';
				Joined += Part;
			}
			pRead = *pEnd == '\0' ? pEnd : pEnd + 1;
		}
		if(Joined.empty())
		{
			*pError = "a path is needed";
			return "";
		}
		std::string Full = m_Root + "/" + Joined;
		if(ForWriting)
		{
			std::string Parent = Full;
			const size_t Slash = Parent.rfind('/');
			Parent = Parent.substr(0, Slash);
			const std::string RealParent = RealPath(Parent.c_str());
			if(RealParent.empty() || !fs_is_dir(RealParent.c_str()))
			{
				*pError = "the directory of '" + Joined + "' does not exist under the root";
				return "";
			}
			if(!Under(RealParent, m_Root))
			{
				*pError = "'" + Joined + "' leaves the root directory";
				return "";
			}
			if(IsSymlink(Full.c_str()))
			{
				*pError = "'" + Joined + "' is a link, and links are not written through";
				return "";
			}
			return Full;
		}
		std::string Real = RealPath(Full.c_str());
		if(Real.empty())
		{
			*pError = "there is no '" + Joined + "' under the root";
			return "";
		}
		if(!Under(Real, m_Root))
		{
			*pError = "'" + Joined + "' leaves the root directory";
			return "";
		}
		return Real;
	}

	CMapMcp::COpenMap *CMapMcp::Add(map_document::CMapState Opened, const char *pName, const char *pPath, std::vector<std::string> vWarnings)
	{
		auto pMap = std::make_unique<COpenMap>(std::move(Opened));
		pMap->m_Handle = NewHandle();
		pMap->m_Name = pName == nullptr ? "" : pName;
		pMap->m_Path = pPath == nullptr ? "" : pPath;
		pMap->m_vWarnings = std::move(vWarnings);
		pMap->m_Document.SetHistoryLimits(m_Options.m_HistoryBytes, m_Options.m_HistoryEntries);
		// Nothing merges: every call is one thing a model did, and one
		// thing to undo.
		pMap->m_Document.SetMergeWindow(0);
		pMap->m_LastUsedNanos = time_get_nanoseconds().count();
		COpenMap *pAdded = pMap.get();
		m_Maps[pAdded->m_Handle] = std::move(pMap);
		return pAdded;
	}

	CMapMcp::COpenMap *CMapMcp::Open(const char *pPath, std::string *pError)
	{
		if(m_Maps.size() >= m_Options.m_MaxMaps)
		{
			*pError = "there are already " + std::to_string(m_Maps.size()) + " maps open; close one with map.close first";
			return nullptr;
		}
		const std::string Full = ResolvePath(pPath, false, pError);
		if(Full.empty())
			return nullptr;
		if(!str_endswith(Full.c_str(), ".map"))
		{
			*pError = std::string("'") + pPath + "' is not a .map file";
			return nullptr;
		}
		CDataFileReader File;
		if(!File.Open(m_pStorage.get(), Full.c_str(), IStorage::TYPE_ABSOLUTE))
		{
			*pError = std::string("'") + pPath + "' could not be opened as a map file";
			return nullptr;
		}
		map_document::CMapState Read;
		std::vector<std::string> vWarnings;
		const bool Ok = map_document::ReadMapState(File, &Read, &vWarnings);
		File.Close();
		if(!Ok)
		{
			*pError = std::string("'") + pPath + "' could not be read as a map";
			return nullptr;
		}
		char aName[IO_MAX_PATH_LENGTH];
		fs_split_file_extension(fs_filename(Full.c_str()), aName, sizeof(aName));
		// The path the caller gave, made relative and clean, is what the map
		// is saved back to.
		const std::string Relative = Full.substr(m_Root.size() + 1);
		return Add(std::move(Read), aName, Relative.c_str(), std::move(vWarnings));
	}

	CMapMcp::COpenMap *CMapMcp::MapArg(const CJson &Arguments, std::string *pError)
	{
		const CJson &Map = Arguments.Get("map");
		if(!Map.IsString() || Map.AsString().empty())
		{
			*pError = "'map' is needed: the handle map.open answered with, or a path";
			return nullptr;
		}
		const std::string &Handle = Map.AsString();
		const auto Found = m_Maps.find(Handle);
		if(Found != m_Maps.end())
		{
			Found->second->m_LastUsedNanos = time_get_nanoseconds().count();
			return Found->second.get();
		}
		const bool LooksLikePath = str_endswith(Handle.c_str(), ".map") || Handle.find('/') != std::string::npos;
		if(!LooksLikePath)
		{
			*pError = "there is no open map with the handle '" + Handle + "'; handles live only as long as this server runs and the map is not closed - open the map again with map.open, or pass its path as 'map'";
			return nullptr;
		}
		// A path names the map that was opened from it, if it still is.
		std::string ResolveError;
		const std::string Full = ResolvePath(Handle.c_str(), false, &ResolveError);
		if(!Full.empty())
		{
			const std::string Relative = Full.substr(m_Root.size() + 1);
			for(auto &[Key, pMap] : m_Maps)
			{
				if(pMap->m_Path == Relative)
				{
					pMap->m_LastUsedNanos = time_get_nanoseconds().count();
					return pMap.get();
				}
			}
		}
		return Open(Handle.c_str(), pError);
	}

	void CMapMcp::CloseIdle()
	{
		if(m_Options.m_IdleSeconds <= 0)
			return;
		const int64_t Now = time_get_nanoseconds().count();
		for(auto It = m_Maps.begin(); It != m_Maps.end();)
		{
			if(Now - It->second->m_LastUsedNanos > m_Options.m_IdleSeconds * (int64_t)1000000000)
			{
				log_info("mcp", "closing '%s' (%s), idle for more than %d seconds", It->second->m_Name.c_str(), It->first.c_str(), (int)m_Options.m_IdleSeconds);
				It = m_Maps.erase(It);
			}
			else
			{
				++It;
			}
		}
	}

	// ---- resources ----

	namespace
	{
		constexpr const char *const s_apTileKinds[] = {"game", "front", "tele", "speedup", "switch", "tune"};

		CJson Resource(const char *pUri, const char *pName, const char *pDescription, const char *pMimeType)
		{
			CJson Json = CJson::Object();
			Json.Set("uri", CJson::Str(pUri));
			Json.Set("name", CJson::Str(pName));
			Json.Set("description", CJson::Str(pDescription));
			Json.Set("mimeType", CJson::Str(pMimeType));
			return Json;
		}

		CJson Template(const char *pUri, const char *pName, const char *pDescription, const char *pMimeType)
		{
			CJson Json = CJson::Object();
			Json.Set("uriTemplate", CJson::Str(pUri));
			Json.Set("name", CJson::Str(pName));
			Json.Set("description", CJson::Str(pDescription));
			Json.Set("mimeType", CJson::Str(pMimeType));
			return Json;
		}

		CJson Contents(const char *pUri, const char *pMimeType, const std::string &Text)
		{
			CJson Item = CJson::Object();
			Item.Set("uri", CJson::Str(pUri));
			Item.Set("mimeType", CJson::Str(pMimeType));
			Item.Set("text", CJson::Str(Text));
			CJson List = CJson::Array();
			List.Push(Item);
			return List;
		}

		int ListNames(const char *pName, int IsDir, int, void *pUser)
		{
			if(!IsDir)
				static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName);
			return 0;
		}

		/** The query part of a URI, `a=1&b=2`, as an object of strings. */
		CJson Query(const std::string &Uri)
		{
			CJson Json = CJson::Object();
			const size_t Mark = Uri.find('?');
			if(Mark == std::string::npos)
				return Json;
			std::string Rest = Uri.substr(Mark + 1);
			while(!Rest.empty())
			{
				const size_t Amp = Rest.find('&');
				const std::string Pair = Rest.substr(0, Amp);
				Rest = Amp == std::string::npos ? "" : Rest.substr(Amp + 1);
				const size_t Equal = Pair.find('=');
				if(Equal != std::string::npos)
					Json.Set(Pair.substr(0, Equal).c_str(), CJson::Str(Pair.substr(Equal + 1)));
			}
			return Json;
		}

		std::string TileReference(map_document::ETileLayerKind Kind)
		{
			std::string Text = std::string("Tile indices of a ") + KindName(Kind) + " layer (index: what it does)\n";
			for(int Index = 0; Index < 256; ++Index)
			{
				const char *pText = map_document::ExplainTile(Kind, Index);
				if(pText == nullptr)
					continue;
				Text += std::to_string(Index);
				Text += ": ";
				Text += pText;
				Text += '\n';
			}
			return Text;
		}
	} // namespace

	const char *EncodingsDoc()
	{
		return "Tile text encodings (tiles.read, tiles.write)\n"
		       "\n"
		       "A tile is a token: its index, then after a slash whatever else it carries, trailing zeroes left off. Air is 0.\n"
		       "- tiles, game, front layers: index/flags (flags: 1 mirror across, 2 mirror down, 8 rotate; a tile drawn plain is just its index)\n"
		       "- tele, tune layers: type/number\n"
		       "- switch layers: type/number/delay/flags\n"
		       "- speedup layers: type/force/maxSpeed/angle\n"
		       "\n"
		       "rle: one line per row of the rectangle, tokens separated by spaces, a run of equal tiles written once with xN (1x20 is twenty walls). Cheapest to read; the one to write with.\n"
		       "rows: one line per row, one token per tile.\n"
		       "sparse: x,y:token for every tile that is not air, coordinates counted from the rectangle's corner.\n"
		       "glyph: one character per tile with a legend (. air, # wall, n unhookable, f freeze, u unfreeze, S start, E finish, p spawn); reading only.\n"
		       "\n"
		       "Limits: tiles.read hands back at most 128 by 128 tiles (256 by 256 with large: true); a write takes at most a million. "
		       "A physics layer only takes tiles that mean something there; the rest are dropped and counted unless allowUnused is set.";
	}

	CJson CMapMcp::ResourcesJson() const
	{
		CJson List = CJson::Array();
		for(const char *pKind : s_apTileKinds)
		{
			const std::string Uri = std::string("ddnet://tiles/") + pKind;
			const std::string Name = std::string("Tile reference: ") + pKind;
			List.Push(Resource(Uri.c_str(), Name.c_str(), "Every tile index that means something in this kind of physics layer, with what it does", "text/plain"));
		}
		List.Push(Resource("ddnet://mapres", "Map resources", "The images that come with the game for tile and quad layers, with their size and whether automapper rules exist for them", "application/json"));
		List.Push(Resource("ddnet://settings/help", "Map settings help", "Every setting a map may ask of a server, with its arguments", "application/json"));
		List.Push(Resource("ddnet://docs/encodings", "Tile encodings", "How rectangles of tiles are written as text, and the limits", "text/plain"));
		for(const auto &[Handle, pMap] : m_Maps)
		{
			const std::string Structure = "ddnet://map/" + Handle + "/structure";
			const std::string History = "ddnet://map/" + Handle + "/history";
			List.Push(Resource(Structure.c_str(), ("Structure of " + pMap->m_Name).c_str(), "Groups, layers, images, envelopes and settings of the open map", "application/json"));
			List.Push(Resource(History.c_str(), ("History of " + pMap->m_Name).c_str(), "The undo history of the open map", "application/json"));
		}
		return List;
	}

	CJson CMapMcp::ResourceTemplatesJson() const
	{
		CJson List = CJson::Array();
		List.Push(Template("ddnet://map/{map}/structure", "Map structure", "Groups, layers, images, envelopes and settings of an open map, by handle", "application/json"));
		List.Push(Template("ddnet://map/{map}/history", "Map history", "The undo history of an open map, by handle", "application/json"));
		List.Push(Template("ddnet://map/{map}/tiles/{group}/{layer}{?x,y,w,h,encoding}", "Tiles of a layer", "A rectangle of tiles as text; at most 128 by 128", "application/json"));
		List.Push(Template("ddnet://tiles/{kind}", "Tile reference", "Tile indices of a physics layer kind: game, front, tele, speedup, switch or tune", "text/plain"));
		List.Push(Template("ddnet://automap/rules/{name}", "Automapper rules", "The rules file for an image, as text", "text/plain"));
		return List;
	}

	CJson CMapMcp::ReadResource(const char *pUri, std::string *pError)
	{
		const std::string Uri = pUri;
		const std::string Path = Uri.substr(0, Uri.find('?'));
		if(str_startswith(Path.c_str(), "ddnet://tiles/"))
		{
			map_document::ETileLayerKind Kind;
			if(!ReadKindName(Path.c_str() + str_length("ddnet://tiles/"), &Kind))
			{
				*pError = "there is no tile reference for that; game, front, tele, speedup, switch and tune are the kinds there are";
				return CJson::Null();
			}
			return Contents(pUri, "text/plain", TileReference(Kind));
		}
		if(Path == "ddnet://docs/encodings")
			return Contents(pUri, "text/plain", EncodingsDoc());
		if(Path == "ddnet://settings/help")
			return Contents(pUri, "application/json", map_document::SettingsHelpJson());
		if(Path == "ddnet://mapres")
		{
			std::vector<std::string> vNames;
			m_pStorage->ListDirectory(IStorage::TYPE_ALL, "mapres", ListNames, &vNames);
			std::sort(vNames.begin(), vNames.end());
			CJson List = CJson::Array();
			for(const std::string &File : vNames)
			{
				if(!str_endswith(File.c_str(), ".png"))
					continue;
				const std::string Name = File.substr(0, File.size() - 4);
				CJson Item = CJson::Object();
				Item.Set("name", CJson::Str(Name));
				void *pData = nullptr;
				unsigned Size = 0;
				if(m_pStorage->ReadFile(("mapres/" + File).c_str(), IStorage::TYPE_ALL, &pData, &Size))
				{
					int Width = 0;
					int Height = 0;
					if(ReadPngSize(static_cast<const unsigned char *>(pData), Size, &Width, &Height))
					{
						Item.Set("width", CJson::Int(Width));
						Item.Set("height", CJson::Int(Height));
					}
					free(pData);
				}
				Item.Set("rules", CJson::Bool(m_pStorage->FileExists(("editor/automap/" + Name + ".rules").c_str(), IStorage::TYPE_ALL)));
				List.Push(Item);
			}
			CJson Json = CJson::Object();
			Json.Set("images", List);
			return Contents(pUri, "application/json", Json.Serialize());
		}
		if(str_startswith(Path.c_str(), "ddnet://automap/rules/"))
		{
			const std::string Name = Path.substr(str_length("ddnet://automap/rules/"));
			if(Name.empty() || Name.find('/') != std::string::npos || Name.find("..") != std::string::npos)
			{
				*pError = "a rules file is named by its image";
				return CJson::Null();
			}
			char *pText = m_pStorage->ReadFileStr(("editor/automap/" + Name + ".rules").c_str(), IStorage::TYPE_ALL);
			if(pText == nullptr)
			{
				*pError = "there is no rules file called '" + Name + "'";
				return CJson::Null();
			}
			const std::string Text = pText;
			free(pText);
			return Contents(pUri, "text/plain", Text);
		}
		if(str_startswith(Path.c_str(), "ddnet://map/"))
		{
			const std::string Rest = Path.substr(str_length("ddnet://map/"));
			const size_t Slash = Rest.find('/');
			const std::string Handle = Rest.substr(0, Slash);
			const std::string What = Slash == std::string::npos ? "" : Rest.substr(Slash + 1);
			const auto Found = m_Maps.find(Handle);
			if(Found == m_Maps.end())
			{
				*pError = "there is no open map with the handle '" + Handle + "'; open it with map.open first";
				return CJson::Null();
			}
			COpenMap &Map = *Found->second;
			Map.m_LastUsedNanos = time_get_nanoseconds().count();
			if(What == "structure")
				return Contents(pUri, "application/json", map_document::StructureJson(Map.m_Document.Map()));
			if(What == "history")
				return Contents(pUri, "application/json", map_document::HistoryJson(Map.m_Document));
			if(str_startswith(What.c_str(), "tiles/"))
			{
				const CJson Args = Query(Uri);
				const std::string Where = What.substr(str_length("tiles/"));
				const size_t Colon = Where.find('/');
				CJson Command = CJson::Object();
				Command.Set("op", CJson::Str("tiles.read"));
				Command.Set("group", CJson::Int(str_toint(Where.substr(0, Colon).c_str())));
				Command.Set("layer", CJson::Int(Colon == std::string::npos ? 0 : str_toint(Where.substr(Colon + 1).c_str())));
				for(const char *pKey : {"x", "y", "w", "h"})
				{
					if(Args.Has(pKey))
						Command.Set(pKey, CJson::Int(str_toint(Args.Get(pKey).AsCStr())));
				}
				if(Args.Has("encoding"))
					Command.Set("encoding", Args.Get("encoding"));
				const std::string Answer = map_document::Apply(Map.m_Document, Command.Serialize().c_str());
				CJson Parsed;
				std::string ParseError;
				if(!CJson::Parse(Answer.c_str(), &Parsed, &ParseError) || !Parsed.Get("ok").AsBool())
				{
					*pError = Parsed.Get("error").AsCStr("the tiles could not be read");
					return CJson::Null();
				}
				return Contents(pUri, "application/json", Answer);
			}
			*pError = "a map has a structure, a history and tiles/{group}/{layer}; nothing else";
			return CJson::Null();
		}
		*pError = "there is no resource at '" + Uri + "'";
		return CJson::Null();
	}

	// ---- prompts ----

	namespace
	{
		class CPromptDef
		{
		public:
			const char *m_pName;
			const char *m_pTitle;
			const char *m_pDescription;
			std::vector<std::pair<const char *, const char *>> m_vArguments;
			const char *m_pText;
		};

		const std::vector<CPromptDef> &Prompts()
		{
			static const std::vector<CPromptDef> s_vPrompts = {
				{"build-start-area", "Build a start area",
					"Lay out a spawn with a freeze border and a start line on the map",
					{{"map", "Handle or path of the map"}, {"width", "How many tiles wide the area is (default 40)"}, {"height", "How many tiles tall (default 20)"}},
					"Build a start area on map {map}, about {width} by {height} tiles, near the top left of the game layer. "
					"Do it in this order: map.structure to find the game layer; tiles.stats on it to find free room; "
					"tiles.fill for the floor (index 1) and walls (index 3, unhookable) and a one-tile freeze rim (tiles.fill with border: 1 and index 9); "
					"tiles.write for the spawn (index 192) and a column of start tiles (index 33) at the exit; "
					"map.render with entities to look at it, and map.check before map.save. "
					"Read tiles with tiles.read in pieces of at most 128 by 128; undo any step you do not like with history.undo."},
				{"check-map", "Check a map",
					"Report what is wrong with a map, with places to look at",
					{{"map", "Handle or path of the map"}},
					"Check map {map}: call map.check, then for every finding use tiles.find to list where the tiles are and map.render with mark to show one of them. "
					"Report the findings in order of how much they would break play (no spawn, no start or finish, teleporters without a target, unknown tiles), and say which you can fix with tiles.replace or tiles.write."},
				{"decorate-layer", "Decorate a layer with the automapper",
					"Draw a design layer from an image's rules over the shape of the game layer",
					{{"map", "Handle or path of the map"}, {"group", "Group of the design layer (default: add one)"}, {"layer", "Layer to decorate"}, {"image", "Name of the image in the map resources, which names its rules file"}},
					"Decorate layer {layer} of group {group} on map {map} with the image {image}. "
					"Read ddnet://automap/rules/{image} or call automap.rules to see the configurations; if the layer does not exist yet, add it with map.apply (layer.add) and set its image with image.add and layer.setProp; "
					"then automap.run with a configuration, map.render to look at it, and history.undo with another seed or configuration if it looks wrong."},
				{"replace-deprecated", "Replace deprecated tiles",
					"Find tiles that mean nothing any more and replace them",
					{{"map", "Handle or path of the map"}},
					"On map {map}, call tiles.stats for every physics layer and look for indices marked used: false or whose name says Deprecated. "
					"For each, call tiles.find to show where they are, ask what they should become, and use tiles.replace on that layer. Finish with map.check."},
			};
			return s_vPrompts;
		}

		std::string FillIn(const char *pText, const CJson &Arguments, const CPromptDef &Prompt)
		{
			std::string Text = pText;
			for(const auto &[pName, pDescription] : Prompt.m_vArguments)
			{
				const std::string Key = std::string("{") + pName + "}";
				std::string Value = Arguments.Get(pName).IsString() ? Arguments.Get(pName).AsString() : Arguments.Get(pName).IsNumber() ? std::to_string(Arguments.Get(pName).AsInt()) :
																			  "";
				if(Value.empty())
				{
					// The default is in the description, in brackets.
					const char *pDefault = str_find(pDescription, "(default");
					if(pDefault != nullptr)
					{
						const char *pStart = pDefault + str_length("(default");
						while(*pStart == ' ' || *pStart == ':')
							++pStart;
						const char *pEnd = str_find(pStart, ")");
						Value = pEnd == nullptr ? pStart : std::string(pStart, pEnd);
					}
					else
					{
						Value = std::string("<") + pName + ">";
					}
				}
				size_t At = 0;
				while((At = Text.find(Key, At)) != std::string::npos)
				{
					Text.replace(At, Key.size(), Value);
					At += Value.size();
				}
			}
			return Text;
		}
	} // namespace

	CJson CMapMcp::PromptsJson() const
	{
		CJson List = CJson::Array();
		for(const CPromptDef &Prompt : Prompts())
		{
			CJson Json = CJson::Object();
			Json.Set("name", CJson::Str(Prompt.m_pName));
			Json.Set("title", CJson::Str(Prompt.m_pTitle));
			Json.Set("description", CJson::Str(Prompt.m_pDescription));
			CJson Arguments = CJson::Array();
			for(const auto &[pName, pDescription] : Prompt.m_vArguments)
			{
				CJson Argument = CJson::Object();
				Argument.Set("name", CJson::Str(pName));
				Argument.Set("description", CJson::Str(pDescription));
				Argument.Set("required", CJson::Bool(str_comp(pName, "map") == 0));
				Arguments.Push(Argument);
			}
			Json.Set("arguments", Arguments);
			List.Push(Json);
		}
		return List;
	}

	CJson CMapMcp::GetPrompt(const char *pName, const CJson &Arguments, std::string *pError)
	{
		for(const CPromptDef &Prompt : Prompts())
		{
			if(str_comp(Prompt.m_pName, pName) != 0)
				continue;
			CJson Json = CJson::Object();
			Json.Set("description", CJson::Str(Prompt.m_pDescription));
			CJson Content = CJson::Object();
			Content.Set("type", CJson::Str("text"));
			Content.Set("text", CJson::Str(FillIn(Prompt.m_pText, Arguments, Prompt)));
			CJson Message = CJson::Object();
			Message.Set("role", CJson::Str("user"));
			Message.Set("content", Content);
			CJson Messages = CJson::Array();
			Messages.Push(Message);
			Json.Set("messages", Messages);
			return Json;
		}
		*pError = std::string("there is no prompt called '") + pName + "'";
		return CJson::Null();
	}
} // namespace map_mcp
