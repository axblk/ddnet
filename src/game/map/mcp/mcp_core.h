#ifndef GAME_MAP_MCP_MCP_CORE_H
#define GAME_MAP_MCP_MCP_CORE_H

#include <base/vmath.h>

#include <game/map/document/document.h>
#include <game/map/document/tiles.h>
#include <game/map/mcp/mcp_json.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class IStorage;

namespace map_mcp
{
	/** What one picture of a map is to show. */
	class CRenderRequest
	{
	public:
		int m_Width = 1024;
		int m_Height = 512;
		/** Where the picture looks, in world units, and how far out. */
		vec2 m_Center = vec2(0.0f, 0.0f);
		float m_Zoom = 1.0f;
		/** How strongly the physics layers are drawn over the design, 0 to 100. */
		int m_EntityOverlay = 0;
		/** Which entities sheet draws them: `ddnet`, `ddrace`, `fng`, ... */
		std::string m_Entities = "ddnet";
		std::vector<std::pair<size_t, size_t>> m_vHidden;
		int m_Grid = 0;
		size_t m_GridGroup = 0;
		/** A rectangle of tiles to mark, in the tiles of that group. */
		size_t m_MarkGroup = 0;
		map_document::CTileRect m_Mark;
		bool m_ShowQuad = false;
		size_t m_QuadGroup = 0;
		size_t m_QuadLayer = 0;
		size_t m_Quad = 0;
		int m_TimeOffsetMillis = 0;
	};

	/**
	 * Whatever draws a map for `map.render`.
	 *
	 * The core knows nothing of graphics, so that it can be linked and
	 * tested without any; a program that can draw hands one of these in.
	 */
	class IRenderer
	{
	public:
		virtual ~IRenderer() = default;

		/**
		 * Draws one picture.
		 *
		 * @param Map The version to draw.
		 * @param Request What to show.
		 * @param pvPng Where the PNG goes.
		 * @param pWidth How wide it came out.
		 * @param pHeight How tall it came out.
		 * @param pError Why it could not be drawn, if it could not.
		 *
		 * @return Whether there is a picture.
		 */
		virtual bool Render(const map_document::CMapState &Map, const CRenderRequest &Request, std::vector<uint8_t> *pvPng, int *pWidth, int *pHeight, std::string *pError) = 0;

		/** How long the last picture took, for whoever measures. */
		virtual int64_t LastRenderNanos() const = 0;
	};

	/** What the server is started with. */
	class COptions
	{
	public:
		/** The only directory maps are read from and written to. */
		std::string m_Root;
		/** How much history every open map may hold. */
		uint64_t m_HistoryBytes = (uint64_t)256 * 1024 * 1024;
		size_t m_HistoryEntries = 1000;
		/** How many maps may be open at once. */
		size_t m_MaxMaps = 4;
		/**
		 * Whether handles are counted up (`m1`, `m2`) rather than drawn at
		 * random. Counting is for transcripts that have to come out the same
		 * twice; a server anybody can reach wants the random ones.
		 */
		bool m_SequentialHandles = false;
		/** After how long without a call an open map is closed, or 0 for never. */
		int64_t m_IdleSeconds = 0;
		/** For finding the game's data directory the way every program does. */
		int m_NumArgs = 0;
		const char **m_ppArguments = nullptr;
	};

	/** What a tool call came to. */
	class CToolResult
	{
	public:
		/** The result for the model, also written as text. */
		CJson m_Structured = CJson::Object();
		/** A line or two for a client that shows text, or empty for the JSON. */
		std::string m_Text;
		/** Pictures, as PNG. */
		std::vector<std::vector<uint8_t>> m_vImages;
		bool m_IsError = false;

		static CToolResult Error(const std::string &Message);
	};

	class CMapMcp;

	/** One tool, as it is listed and as it is called. */
	class CTool
	{
	public:
		const char *m_pName;
		const char *m_pTitle;
		const char *m_pDescription;
		/** JSON Schema of the arguments, as text. */
		const char *m_pInputSchema;
		bool m_ReadOnly;
		bool m_Destructive;
		bool m_Idempotent;
		std::function<CToolResult(CMapMcp &, const CJson &)> m_Handler;
	};

	/**
	 * The server without a transport: what the tools do, what the
	 * resources hold, what the prompts say.
	 *
	 * Handles: `map.open` and `map.new` answer with a handle such as `m1`,
	 * and everything else names the map by it. A handle lives as long as
	 * this object does and the map is not closed - across requests, whatever
	 * transport carried them - and a call against one that is gone answers
	 * with an error that says to open the map again. A `map` argument may
	 * also be a path: the map is then opened on the way, and a second call
	 * with the same path finds it open.
	 *
	 * One thread at a time: the document core is single-threaded, and so is
	 * this.
	 */
	class CMapMcp
	{
	public:
		static constexpr const char *SERVER_NAME = "ddnet-map-mcp";
		static constexpr const char *SERVER_VERSION = "0.1.0";

		CMapMcp(const COptions &Options, std::unique_ptr<IRenderer> pRenderer);
		~CMapMcp();

		CMapMcp(const CMapMcp &) = delete;
		CMapMcp &operator=(const CMapMcp &) = delete;

		/** Whether the root directory could be used; the reason if not. */
		bool Ok() const { return m_Error.empty(); }
		const std::string &Error() const { return m_Error; }

		/** What the model is told about the server before any tool. */
		std::string Instructions() const;

		/** The tools, as `tools/list` lists them. */
		CJson ToolsJson() const;

		/**
		 * One tool call.
		 *
		 * @param pName Which tool.
		 * @param Arguments Its arguments.
		 * @param pUnknownTool Set when there is no tool of that name, which
		 * is a protocol error rather than a tool error.
		 *
		 * @return The result, or an error result.
		 */
		CToolResult CallTool(const char *pName, const CJson &Arguments, bool *pUnknownTool);

		/** The result as the protocol wants it: `content`, `structuredContent`, `isError`. */
		static CJson ResultJson(const CToolResult &Result);

		CJson ResourcesJson() const;
		CJson ResourceTemplatesJson() const;

		/**
		 * One resource.
		 *
		 * @param pUri Which.
		 * @param pError Why it could not be read, if it could not.
		 *
		 * @return The `contents` list, or null with the error set.
		 */
		CJson ReadResource(const char *pUri, std::string *pError);

		CJson PromptsJson() const;

		/**
		 * One prompt, filled in.
		 *
		 * @param pName Which.
		 * @param Arguments Its arguments.
		 * @param pError Why there is none, if there is none.
		 *
		 * @return The `prompts/get` result, or null with the error set.
		 */
		CJson GetPrompt(const char *pName, const CJson &Arguments, std::string *pError);

		/** Closes every map nobody has touched for `m_IdleSeconds`. */
		void CloseIdle();

		/** How many maps are open. */
		size_t NumOpen() const { return m_Maps.size(); }

		const COptions &Options() const { return m_Options; }
		IStorage *Storage() { return m_pStorage.get(); }

	private:
		class COpenMap
		{
		public:
			std::string m_Handle;
			/** Where it came from, relative to the root, or empty for a new map. */
			std::string m_Path;
			std::string m_Name;
			map_document::CDocument m_Document;
			std::vector<std::string> m_vWarnings;
			bool m_Dirty = false;
			int64_t m_LastUsedNanos = 0;

			explicit COpenMap(map_document::CMapState Opened) :
				m_Document(std::move(Opened)) {}
		};

		friend class CTools;

		/**
		 * The map a call is about, by handle or by path.
		 *
		 * @param Arguments The call's arguments, whose `map` says which.
		 * @param pError Why there is none, if there is none.
		 *
		 * @return The map, or null.
		 */
		COpenMap *MapArg(const CJson &Arguments, std::string *pError);

		/**
		 * Turns a path from a call into one under the root, or refuses it.
		 *
		 * @param pPath The path as the call gave it.
		 * @param ForWriting Whether the file is to be written: then its
		 * directory has to exist under the root, and the file itself may not.
		 * @param pError Why it was refused, if it was.
		 *
		 * @return The full path, or empty.
		 */
		std::string ResolvePath(const char *pPath, bool ForWriting, std::string *pError);

		COpenMap *Open(const char *pPath, std::string *pError);
		COpenMap *Add(map_document::CMapState Opened, const char *pName, const char *pPath, std::vector<std::string> vWarnings);
		std::string NewHandle();

		COptions m_Options;
		std::string m_Root;
		std::string m_Error;
		std::unique_ptr<IStorage> m_pStorage;
		std::unique_ptr<IRenderer> m_pRenderer;
		std::vector<CTool> m_vTools;
		std::map<std::string, std::unique_ptr<COpenMap>> m_Maps;
		int m_NextHandle = 1;
	};

	/** The tool table, kept apart so that the core stays readable. */
	std::vector<CTool> MakeTools();
} // namespace map_mcp

#endif // GAME_MAP_MCP_MCP_CORE_H
