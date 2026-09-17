#include "test.h"

#include <base/fs.h>
#include <base/io.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/storage.h>

#include <game/map/mcp/mcp_core.h>
#include <game/map/mcp/mcp_json.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if !defined(CONF_FAMILY_WINDOWS)
#include <unistd.h>
#endif

using namespace map_mcp;

// The tools without a transport: what a model gets back from each of them,
// that handles live across calls and die cleanly, and that nothing leaves
// the root directory.

namespace
{
	class CRoot
	{
	public:
		std::string m_Path;

		CRoot()
		{
			char aName[64];
			str_format(aName, sizeof(aName), "mcp-test-root-%d", (int)time_get() % 100000);
			m_Path = aName;
			(void)fs_makedir(m_Path.c_str());
			Put("Tutorial.map", "data/maps/Tutorial.map");
		}

		~CRoot()
		{
			std::vector<std::string> vFiles;
			fs_listdir(
				m_Path.c_str(), [](const char *pName, int IsDir, int, void *pUser) {
					if(!IsDir)
						static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName);
					return 0;
				},
				0, &vFiles);
			for(const std::string &File : vFiles)
				(void)fs_remove((m_Path + "/" + File).c_str());
			for(const char *pDir : {"out"})
			{
				const std::string Sub = m_Path + "/" + pDir;
				std::vector<std::string> vSub;
				fs_listdir(
					Sub.c_str(), [](const char *pName, int IsDir, int, void *pUser) {
						if(!IsDir)
							static_cast<std::vector<std::string> *>(pUser)->emplace_back(pName);
						return 0;
					},
					0, &vSub);
				for(const std::string &File : vSub)
					(void)fs_remove((Sub + "/").append(File).c_str());
				(void)fs_removedir(Sub.c_str());
			}
			(void)fs_removedir(m_Path.c_str());
		}

		/** Copies a file of the game's data into the root. */
		void Put(const char *pName, const char *pFrom) const
		{
			std::unique_ptr<IStorage> pStorage = CreateLocalStorage();
			void *pData = nullptr;
			unsigned Size = 0;
			ASSERT_TRUE(pStorage->ReadFile(pFrom, IStorage::TYPE_ALL, &pData, &Size)) << pFrom;
			IOHANDLE File = io_open((m_Path + "/" + pName).c_str(), IOFLAG_WRITE);
			ASSERT_NE(File, nullptr);
			io_write(File, pData, Size);
			io_close(File);
			free(pData);
		}
	};

	class CServer
	{
	public:
		CRoot m_Root;
		std::string m_Argv0;
		const char *m_apArgs[1] = {nullptr};
		std::unique_ptr<CMapMcp> m_pMcp;

		explicit CServer(size_t MaxMaps = 4, int64_t IdleSeconds = 0)
		{
			// The program's own path lets the storage find the game's data
			// directory, where the mapres and the automapper rules live.
			m_Argv0 = ::testing::internal::GetArgvs().front();
			m_apArgs[0] = m_Argv0.c_str();
			COptions Options;
			Options.m_Root = m_Root.m_Path;
			Options.m_NumArgs = 1;
			Options.m_ppArguments = m_apArgs;
			Options.m_SequentialHandles = true;
			Options.m_MaxMaps = MaxMaps;
			Options.m_IdleSeconds = IdleSeconds;
			m_pMcp = std::make_unique<CMapMcp>(Options, nullptr);
		}

		CJson Args(const char *pJson)
		{
			CJson Json;
			std::string Error;
			EXPECT_TRUE(CJson::Parse(pJson, &Json, &Error)) << pJson << ": " << Error;
			return Json;
		}

		CToolResult Call(const char *pTool, const char *pArgs)
		{
			bool Unknown = false;
			CToolResult Result = m_pMcp->CallTool(pTool, Args(pArgs), &Unknown);
			EXPECT_FALSE(Unknown) << pTool;
			return Result;
		}

		CToolResult Ok(const char *pTool, const char *pArgs)
		{
			CToolResult Result = Call(pTool, pArgs);
			EXPECT_FALSE(Result.m_IsError) << pTool << " " << pArgs << ": " << Result.m_Text;
			return Result;
		}

		std::string Fails(const char *pTool, const char *pArgs)
		{
			CToolResult Result = Call(pTool, pArgs);
			EXPECT_TRUE(Result.m_IsError) << pTool << " " << pArgs << " succeeded: " << Result.m_Structured.Serialize();
			return Result.m_Text;
		}
	};

	const char *const s_apExpectedTools[] = {
		"maps.list", "map.open", "map.new", "map.save", "map.close", "map.list", "map.append",
		"map.structure", "map.envelope", "map.quads", "map.sources", "map.history", "tiles.read", "tiles.stats", "tiles.find", "tile.explain", "tile.at", "map.proof",
		"map.apply", "tiles.write", "tiles.fill", "tiles.replace", "tiles.brush", "automap.run", "art.tiles", "art.quads", "text.type", "image.add", "history.undo", "history.redo", "history.jump",
		"map.check", "settings.check", "automap.rules", "map.render"};
} // namespace

TEST(Mcp, TheToolsAreListedInAFixedOrderWithSchemas)
{
	CServer Server;
	const CJson Tools = Server.m_pMcp->ToolsJson();
	ASSERT_EQ(Tools.Size(), std::size(s_apExpectedTools));
	for(size_t i = 0; i < Tools.Size(); ++i)
	{
		const CJson &Tool = Tools.At(i);
		EXPECT_EQ(Tool.Get("name").AsString(), s_apExpectedTools[i]);
		EXPECT_FALSE(Tool.Get("title").AsString().empty());
		EXPECT_FALSE(Tool.Get("description").AsString().empty());
		// The schema is text in the table; it has to be JSON, and an object.
		CJson Schema;
		std::string Error;
		ASSERT_TRUE(CJson::Parse(Tool.Get("inputSchema").Serialize().c_str(), &Schema, &Error)) << Tool.Get("name").AsString() << ": " << Error;
		EXPECT_EQ(Schema.Get("type").AsString(), "object") << Tool.Get("name").AsString();
		EXPECT_TRUE(Schema.Get("properties").IsObject()) << Tool.Get("name").AsString();
		EXPECT_TRUE(Tool.Get("annotations").Get("readOnlyHint").IsBool());
		for(const char *pName : {"tiles.read", "tiles.write", "map.render", "map.open", "map.save"})
		{
			if(Tool.Get("name").AsString() == pName)
				EXPECT_GT(Schema.Get("required").Size(), 0u) << pName;
		}
	}
	EXPECT_NE(Server.m_pMcp->Instructions().find("map.open"), std::string::npos);
}

TEST(Mcp, AResultIsTextAndStructuredContent)
{
	CServer Server;
	const CToolResult Result = Server.Ok("tile.explain", "{\"kind\":\"game\",\"index\":1}");
	const CJson Json = CMapMcp::ResultJson(Result);
	ASSERT_EQ(Json.Get("content").Size(), 1u);
	EXPECT_EQ(Json.Get("content").At(0).Get("type").AsString(), "text");
	EXPECT_NE(Json.Get("content").At(0).Get("text").AsString().find("HOOKABLE"), std::string::npos);
	EXPECT_EQ(Json.Get("structuredContent").Get("index").AsInt(), 1);
	EXPECT_TRUE(Json.Get("structuredContent").Get("used").AsBool());
	EXPECT_FALSE(Json.Has("isError"));

	const CJson Error = CMapMcp::ResultJson(Server.Call("tile.explain", "{\"kind\":\"game\",\"index\":999}"));
	EXPECT_TRUE(Error.Get("isError").AsBool());
	EXPECT_FALSE(Error.Has("structuredContent"));

	bool Unknown = false;
	Server.m_pMcp->CallTool("no.such.tool", CJson::Object(), &Unknown);
	EXPECT_TRUE(Unknown) << "a tool that does not exist is a protocol error, not a tool error";
}

TEST(Mcp, OpenDescribeChangeUndoSaveAndCheck)
{
	CServer Server;
	CToolResult Opened = Server.Ok("map.open", "{\"path\":\"Tutorial.map\"}");
	EXPECT_EQ(Opened.m_Structured.Get("map").AsString(), "m1");
	EXPECT_EQ(Opened.m_Structured.Get("name").AsString(), "Tutorial");
	const CJson &Game = Opened.m_Structured.Get("summary").Get("gameLayer");
	ASSERT_TRUE(Game.IsObject());
	const std::string Layer = "\"group\":" + std::to_string(Game.Get("group").AsInt()) + ",\"layer\":" + std::to_string(Game.Get("layer").AsInt());
	EXPECT_EQ(Game.Get("width").AsInt(), 2210);
	EXPECT_EQ(Opened.m_Structured.Get("historyIndex").AsInt(), 0);
	EXPECT_FALSE(Opened.m_Structured.Get("dirty").AsBool());

	const CToolResult Structure = Server.Ok("map.structure", "{\"map\":\"m1\",\"detail\":\"full\"}");
	EXPECT_EQ(Structure.m_Structured.Type(), CJson::RAW);
	EXPECT_NE(Structure.m_Structured.Serialize().find("\"groups\""), std::string::npos);

	// A start area: a floor, walls and a freeze rim, then a spawn.
	CToolResult Fill = Server.Ok("tiles.fill", ("{\"map\":\"m1\"," + Layer + ",\"x\":2,\"y\":2,\"w\":20,\"h\":10,\"index\":9,\"border\":1,\"label\":\"Freeze rim\"}").c_str());
	EXPECT_EQ(Fill.m_Structured.Get("written").AsInt(), 56);
	EXPECT_EQ(Fill.m_Structured.Get("historyIndex").AsInt(), 1);
	EXPECT_TRUE(Fill.m_Structured.Get("dirty").AsBool());
	CToolResult Wrote = Server.Ok("tiles.write", ("{\"map\":\"m1\"," + Layer + ",\"x\":10,\"y\":6,\"tiles\":\"192 0 33\"}").c_str());
	EXPECT_EQ(Wrote.m_Structured.Get("written").AsInt(), 3);
	CToolResult Read = Server.Ok("tiles.read", ("{\"map\":\"m1\"," + Layer + ",\"x\":2,\"y\":2,\"w\":20,\"h\":10,\"encoding\":\"glyph\"}").c_str());
	const std::string Glyphs = Read.m_Structured.Get("tiles").AsString();
	EXPECT_EQ(Glyphs.substr(0, 21), "ffffffffffffffffffff\n");
	EXPECT_NE(Glyphs.find("p.S"), std::string::npos) << Glyphs;
	EXPECT_NE(Read.m_Structured.Get("legend").AsString().find("f=9"), std::string::npos);
	CToolResult At = Server.Ok("tile.at", "{\"map\":\"m1\",\"x\":10,\"y\":6}");
	EXPECT_NE(At.m_Text.find("game: SPAWN"), std::string::npos) << At.m_Text;

	CToolResult History = Server.Ok("map.history", "{\"map\":\"m1\"}");
	EXPECT_NE(History.m_Structured.Serialize().find("Freeze rim"), std::string::npos);
	CToolResult Undo = Server.Ok("history.undo", "{\"map\":\"m1\"}");
	EXPECT_TRUE(Undo.m_Structured.Get("moved").AsBool());
	EXPECT_EQ(Undo.m_Structured.Get("historyIndex").AsInt(), 1);
	EXPECT_EQ(Undo.m_Structured.Get("label").AsString(), "Freeze rim");
	Server.Ok("history.undo", "{\"map\":\"m1\"}");
	EXPECT_FALSE(Server.Ok("history.undo", "{\"map\":\"m1\"}").m_Structured.Get("moved").AsBool());
	Server.Ok("history.redo", "{\"map\":\"m1\"}");
	EXPECT_EQ(Server.Ok("history.jump", "{\"map\":\"m1\",\"index\":2}").m_Structured.Get("historyIndex").AsInt(), 2);

	// Saved as a copy, and the copy reads back with the change in it.
	(void)fs_makedir((Server.m_Root.m_Path + "/out").c_str());
	CToolResult Saved = Server.Ok("map.save", "{\"map\":\"m1\",\"path\":\"out/copy.map\"}");
	EXPECT_EQ(Saved.m_Structured.Get("path").AsString(), "out/copy.map");
	EXPECT_GT(Saved.m_Structured.Get("bytes").AsInt(), 100000);
	EXPECT_EQ(Saved.m_Structured.Get("sha256").AsString().size(), 64u);
	EXPECT_FALSE(Saved.m_Structured.Get("dirty").AsBool());
	Server.Fails("map.save", "{\"map\":\"m1\",\"path\":\"Tutorial.map\"}");
	Server.Ok("map.save", "{\"map\":\"m1\",\"path\":\"Tutorial.map\",\"overwrite\":true}");
	CToolResult Copy = Server.Ok("map.open", "{\"path\":\"out/copy.map\"}");
	EXPECT_EQ(Copy.m_Structured.Get("map").AsString(), "m2");
	CToolResult CopyRead = Server.Ok("tiles.read", ("{\"map\":\"m2\"," + Layer + ",\"x\":10,\"y\":6,\"w\":3,\"h\":1}").c_str());
	EXPECT_EQ(CopyRead.m_Structured.Get("tiles").AsString(), "192 0 33\n");

	CToolResult Check = Server.Ok("map.check", "{\"map\":\"m2\"}");
	EXPECT_TRUE(Check.m_Structured.Get("findings").IsArray());
	EXPECT_EQ(Check.m_Structured.Get("errors").AsInt(), 0) << Check.m_Structured.Serialize();

	CToolResult List = Server.Ok("map.list", "{}");
	EXPECT_EQ(List.m_Structured.Get("maps").Size(), 2u);
	Server.Ok("map.close", "{\"map\":\"m2\"}");
	EXPECT_EQ(Server.m_pMcp->NumOpen(), 1u);
}

TEST(Mcp, AnUnknownHandleSaysToOpenTheMapAgain)
{
	CServer Server;
	const std::string Message = Server.Fails("tiles.stats", "{\"map\":\"m9\"}");
	EXPECT_NE(Message.find("open the map again"), std::string::npos) << Message;
	Server.Fails("tiles.stats", "{}");
	Server.Ok("map.open", "{\"path\":\"Tutorial.map\"}");
	Server.Ok("map.close", "{\"map\":\"m1\"}");
	EXPECT_NE(Server.Fails("tiles.stats", "{\"map\":\"m1\"}").find("m1"), std::string::npos) << "closed is gone";
}

TEST(Mcp, APathWorksAsAHandleAndOpensOnce)
{
	CServer Server;
	CToolResult Stats = Server.Ok("tiles.stats", "{\"map\":\"Tutorial.map\"}");
	EXPECT_EQ(Stats.m_Structured.Get("map").AsString(), "m1");
	EXPECT_EQ(Server.m_pMcp->NumOpen(), 1u);
	EXPECT_EQ(Server.Ok("map.open", "{\"path\":\"./Tutorial.map\"}").m_Structured.Get("map").AsString(), "m1") << "the same file is the same map";
	Server.Ok("tiles.stats", "{\"map\":\"Tutorial.map\"}");
	EXPECT_EQ(Server.m_pMcp->NumOpen(), 1u);
	Server.Fails("tiles.stats", "{\"map\":\"nothing.map\"}");
}

TEST(Mcp, NothingLeavesTheRoot)
{
	CServer Server;
	EXPECT_NE(Server.Fails("map.open", "{\"path\":\"../Tutorial.map\"}").find("leaves the root"), std::string::npos);
	Server.Fails("map.open", "{\"path\":\"/etc/passwd\"}");
	Server.Fails("map.open", "{\"path\":\"a/../../b.map\"}");
	Server.Fails("map.open", "{\"path\":\"Tutorial.map/../../x.map\"}");
	Server.Fails("maps.list", "{\"subdir\":\"..\"}");
	Server.Ok("map.open", "{\"path\":\"Tutorial.map\"}");
	Server.Fails("map.save", "{\"map\":\"m1\",\"path\":\"../escaped.map\"}");
	Server.Fails("map.save", "{\"map\":\"m1\",\"path\":\"nowhere/escaped.map\"}");
	Server.Fails("map.append", "{\"map\":\"m1\",\"path\":\"../Tutorial.map\"}");
	Server.Fails("art.tiles", "{\"map\":\"m1\",\"path\":\"../x.png\"}");
#if !defined(CONF_FAMILY_WINDOWS)
	// A link out of the root is not followed, either way.
	const std::string Link = Server.m_Root.m_Path + "/link";
	ASSERT_EQ(symlink("/", Link.c_str()), 0);
	Server.Fails("maps.list", "{\"subdir\":\"link\"}");
	Server.Fails("map.open", "{\"path\":\"link/etc/hostname\"}");
	Server.Fails("map.save", "{\"map\":\"m1\",\"path\":\"link/tmp/escaped.map\"}");
	const std::string FileLink = Server.m_Root.m_Path + "/linked.map";
	ASSERT_EQ(symlink("/dev/null", FileLink.c_str()), 0);
	Server.Fails("map.save", "{\"map\":\"m1\",\"path\":\"linked.map\",\"overwrite\":true}");
	(void)fs_remove(FileLink.c_str());
	(void)fs_remove(Link.c_str());
#endif
	CToolResult List = Server.Ok("maps.list", "{}");
	ASSERT_EQ(List.m_Structured.Get("maps").Size(), 1u);
	EXPECT_EQ(List.m_Structured.Get("maps").At(0).Get("path").AsString(), "Tutorial.map");
}

TEST(Mcp, LimitsAreCleanErrors)
{
	CServer Server(1);
	Server.Ok("map.open", "{\"path\":\"Tutorial.map\"}");
	EXPECT_NE(Server.Fails("map.new", "{\"width\":10,\"height\":10}").find("already"), std::string::npos);
	Server.Fails("map.new", "{\"width\":20000,\"height\":10}");
	EXPECT_NE(Server.Fails("tiles.read", "{\"map\":\"m1\",\"group\":14,\"layer\":2,\"x\":0,\"y\":0,\"w\":2000,\"h\":100}").find("128"), std::string::npos);
	Server.Fails("tiles.fill", "{\"map\":\"m1\",\"group\":14,\"layer\":2,\"x\":0,\"y\":0,\"w\":2000,\"h\":2000,\"index\":1}");
	EXPECT_NE(Server.Fails("map.render", "{\"map\":\"m1\"}").find("renderer"), std::string::npos);
	Server.Fails("map.render", "{\"map\":\"m1\",\"width\":4000,\"height\":4000}");
	Server.Fails("map.proof", "{\"map\":\"m1\",\"x\":1,\"y\":1,\"render\":true}");
	EXPECT_FALSE(Server.Call("map.proof", "{\"map\":\"m1\",\"x\":1,\"y\":1}").m_IsError);
	Server.Fails("tiles.write", "{\"map\":\"m1\",\"group\":14,\"layer\":2,\"x\":0,\"y\":0,\"tiles\":\"1x2000000\"}");
	Server.Fails("map.close", "{}");
	Server.Fails("map.apply", "{\"map\":\"m1\",\"ops\":[]}");
	Server.Fails("map.apply", "{\"map\":\"m1\",\"ops\":[{\"op\":\"history.undo\"}]}");
}

TEST(Mcp, DryRunsAndBatchesLeaveNothingHalfDone)
{
	CServer Server;
	Server.Ok("map.new", "{\"width\":50,\"height\":20,\"name\":\"fresh\"}");
	CToolResult Dry = Server.Ok("tiles.fill", "{\"map\":\"m1\",\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"w\":5,\"h\":5,\"index\":1,\"dryRun\":true}");
	EXPECT_EQ(Dry.m_Structured.Get("written").AsInt(), 25);
	EXPECT_TRUE(Dry.m_Structured.Get("dryRun").AsBool());
	EXPECT_EQ(Dry.m_Structured.Get("historyIndex").AsInt(), 0);
	EXPECT_EQ(Server.Ok("tiles.stats", "{\"map\":\"m1\",\"group\":0,\"layer\":0}").m_Structured.Get("layers").At(0).Get("tiles").AsInt(), 0);

	CToolResult Batch = Server.Ok("map.apply", "{\"map\":\"m1\",\"ops\":[{\"op\":\"group.add\",\"name\":\"Design\"},{\"op\":\"layer.add\",\"group\":1,\"type\":\"tiles\",\"name\":\"Walls\"},{\"op\":\"tiles.fill\",\"group\":1,\"layer\":0,\"x\":0,\"y\":0,\"w\":50,\"h\":1,\"index\":7}],\"label\":\"Design layer\"}");
	EXPECT_EQ(Batch.m_Structured.Get("results").Size(), 3u);
	EXPECT_EQ(Batch.m_Structured.Get("historyIndex").AsInt(), 1) << "three commands, one undo step";
	EXPECT_EQ(Server.Ok("map.structure", "{\"map\":\"m1\"}").m_Structured.Get("groups").Size(), 2u);

	const std::string Failed = Server.Fails("map.apply", "{\"map\":\"m1\",\"ops\":[{\"op\":\"group.add\",\"name\":\"Half\"},{\"op\":\"layer.delete\",\"group\":9,\"layer\":0}]}");
	EXPECT_NE(Failed.find("command 1"), std::string::npos) << Failed;
	EXPECT_EQ(Server.Ok("map.structure", "{\"map\":\"m1\"}").m_Structured.Get("groups").Size(), 2u) << "the group of the failed batch is not there";
	EXPECT_EQ(Server.Ok("map.history", "{\"map\":\"m1\"}").m_Structured.Serialize().find("Half"), std::string::npos);

	Server.Fails("map.save", "{\"map\":\"m1\"}");
	EXPECT_NE(Server.Fails("map.close", "{\"map\":\"m1\"}").find("discard"), std::string::npos);
	Server.Ok("map.close", "{\"map\":\"m1\",\"discard\":true}");
}

TEST(Mcp, BrushesAutomapperAndImages)
{
	CServer Server;
	Server.Ok("map.new", "{\"width\":64,\"height\":64}");
	Server.Ok("tiles.write", "{\"map\":\"m1\",\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"tiles\":\"1 3\\n9 0\"}");
	CToolResult Stamped = Server.Ok("tiles.brush", "{\"map\":\"m1\",\"from\":{\"group\":0,\"layer\":0,\"x\":0,\"y\":0,\"w\":2,\"h\":2},\"to\":{\"group\":0,\"layer\":0,\"x\":10,\"y\":10},\"rotate\":90,\"repeat\":{\"w\":4,\"h\":2}}");
	EXPECT_EQ(Stamped.m_Structured.Get("written").Get("w").AsInt(), 4);
	const std::string Tiles = Server.Ok("tiles.read", "{\"map\":\"m1\",\"group\":0,\"layer\":0,\"x\":10,\"y\":10,\"w\":4,\"h\":2}").m_Structured.Get("tiles").AsString();
	EXPECT_EQ(Tiles, "9 1 9 1\n0 3 0 3\n") << "turned a quarter clockwise and repeated twice";

	CToolResult Rules = Server.Ok("automap.rules", "{\"image\":\"grass_main\"}");
	EXPECT_GT(Rules.m_Structured.Get("rules").At(0).Get("configs").Size(), 0u);
	CToolResult Image = Server.Ok("image.add", "{\"map\":\"m1\",\"name\":\"grass_main\"}");
	EXPECT_EQ(Image.m_Structured.Get("image").AsInt(), 0);
	Server.Fails("image.add", "{\"map\":\"m1\",\"name\":\"grass_main\"}");
	Server.Fails("image.add", "{\"map\":\"m1\",\"name\":\"no_such_picture\"}");
	Server.Ok("map.apply", "{\"map\":\"m1\",\"ops\":[{\"op\":\"group.add\",\"name\":\"Grass\"},{\"op\":\"layer.add\",\"group\":1,\"type\":\"tiles\",\"name\":\"Grass\"},{\"op\":\"layer.setProp\",\"group\":1,\"layer\":0,\"prop\":\"image\",\"value\":0},{\"op\":\"tiles.fill\",\"group\":1,\"layer\":0,\"x\":10,\"y\":10,\"w\":20,\"h\":10,\"index\":1}]}");
	CToolResult Automapped = Server.Ok("automap.run", "{\"map\":\"m1\",\"group\":1,\"layer\":0,\"config\":0,\"seed\":7}");
	EXPECT_EQ(Automapped.m_Structured.Get("rules").AsString(), "grass_main");
	EXPECT_GT(Automapped.m_Structured.Get("chunks").AsInt(), 0);
	Server.Fails("automap.run", "{\"map\":\"m1\",\"group\":0,\"layer\":0,\"rules\":\"grass_main\"}");
	Server.Fails("automap.run", "{\"map\":\"m1\",\"group\":1,\"layer\":0,\"config\":\"no such config\"}");

	CToolResult Typed = Server.Ok("text.type", "{\"map\":\"m1\",\"group\":1,\"layer\":0,\"x\":0,\"y\":0,\"text\":\"HI\"}");
	EXPECT_EQ(Typed.m_Structured.Get("tiles").AsInt(), 2);
	EXPECT_FALSE(Server.Call("settings.check", "{\"line\":\"sv_test_cmds 1\"}").m_Structured.Get("ok").AsBool());
	EXPECT_TRUE(Server.Call("settings.check", "{\"line\":\"sv_hit 1\"}").m_Structured.Get("ok").AsBool());
}

TEST(Mcp, ResourcesAndPrompts)
{
	CServer Server;
	const CJson Resources = Server.m_pMcp->ResourcesJson();
	bool FoundGame = false;
	for(size_t i = 0; i < Resources.Size(); ++i)
		FoundGame |= Resources.At(i).Get("uri").AsString() == "ddnet://tiles/game";
	EXPECT_TRUE(FoundGame);
	EXPECT_GE(Server.m_pMcp->ResourceTemplatesJson().Size(), 4u);

	std::string Error;
	CJson Contents = Server.m_pMcp->ReadResource("ddnet://tiles/tele", &Error);
	ASSERT_TRUE(Contents.IsArray()) << Error;
	EXPECT_NE(Contents.At(0).Get("text").AsString().find("26: "), std::string::npos);
	EXPECT_LT(Contents.At(0).Get("text").AsString().size(), 6u * 1024u) << "a tile reference stays under 6 KiB";
	Contents = Server.m_pMcp->ReadResource("ddnet://mapres", &Error);
	ASSERT_TRUE(Contents.IsArray()) << Error;
	EXPECT_NE(Contents.At(0).Get("text").AsString().find("grass_main"), std::string::npos);
	Contents = Server.m_pMcp->ReadResource("ddnet://automap/rules/grass_main", &Error);
	ASSERT_TRUE(Contents.IsArray()) << Error;
	EXPECT_TRUE(Server.m_pMcp->ReadResource("ddnet://automap/rules/../../secret", &Error).IsNull());
	EXPECT_TRUE(Server.m_pMcp->ReadResource("ddnet://map/m1/structure", &Error).IsNull());
	EXPECT_NE(Error.find("m1"), std::string::npos);
	Server.Ok("map.open", "{\"path\":\"Tutorial.map\"}");
	Contents = Server.m_pMcp->ReadResource("ddnet://map/m1/structure", &Error);
	ASSERT_TRUE(Contents.IsArray()) << Error;
	Contents = Server.m_pMcp->ReadResource("ddnet://map/m1/tiles/14/2?x=0&y=0&w=4&h=2", &Error);
	ASSERT_TRUE(Contents.IsArray()) << Error;
	EXPECT_NE(Contents.At(0).Get("text").AsString().find("\"tiles\""), std::string::npos);
	EXPECT_TRUE(Server.m_pMcp->ReadResource("ddnet://nothing", &Error).IsNull());
	EXPECT_EQ(Server.m_pMcp->ResourcesJson().Size(), Resources.Size() + 2) << "an open map adds its structure and history";

	const CJson Prompts = Server.m_pMcp->PromptsJson();
	EXPECT_EQ(Prompts.Size(), 4u);
	const CJson Prompt = Server.m_pMcp->GetPrompt("build-start-area", Server.Args("{\"map\":\"m1\",\"width\":\"30\"}"), &Error);
	ASSERT_TRUE(Prompt.IsObject()) << Error;
	const std::string Text = Prompt.Get("messages").At(0).Get("content").Get("text").AsString();
	EXPECT_NE(Text.find("map m1, about 30 by 20 tiles"), std::string::npos) << Text;
	EXPECT_TRUE(Server.m_pMcp->GetPrompt("no-such-prompt", CJson::Object(), &Error).IsNull());
}

TEST(Mcp, IdleMapsAreClosed)
{
	CServer Server(4, 1);
	Server.Ok("map.open", "{\"path\":\"Tutorial.map\"}");
	Server.m_pMcp->CloseIdle();
	EXPECT_EQ(Server.m_pMcp->NumOpen(), 1u);
	std::this_thread::sleep_for(std::chrono::milliseconds(1100));
	Server.m_pMcp->CloseIdle();
	EXPECT_EQ(Server.m_pMcp->NumOpen(), 0u);
	EXPECT_NE(Server.Fails("tiles.stats", "{\"map\":\"m1\"}").find("open the map again"), std::string::npos);
}
