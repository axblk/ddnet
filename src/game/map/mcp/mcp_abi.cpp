#include "mcp_abi.h"

#include <base/io.h>
#include <base/log.h>
#include <base/logger.h>
#include <base/str.h>

#include <game/map/mcp/mcp_core.h>
#include <game/map/mcp/mcp_json.h>
#include <game/map/mcp/mcp_render.h>

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace map_mcp;

namespace
{
	char *Give(const std::string &Text)
	{
		char *pText = static_cast<char *>(malloc(Text.size() + 1));
		std::memcpy(pText, Text.c_str(), Text.size() + 1);
		return pText;
	}

	std::string ErrorJson(int Code, const std::string &Message)
	{
		CJson Error = CJson::Object();
		Error.Set("code", CJson::Int(Code));
		Error.Set("message", CJson::Str(Message));
		CJson Json = CJson::Object();
		Json.Set("error", Error);
		return Json.Serialize();
	}

	std::string ResultJson(CJson Result)
	{
		CJson Json = CJson::Object();
		Json.Set("result", std::move(Result));
		return Json.Serialize();
	}
} // namespace

/**
 * The worker thread and its queue: whoever calls waits for the answer, and
 * the answers come out in the order the calls came in.
 */
struct ddnet_map_mcp
{
	std::thread m_Thread;
	std::mutex m_Mutex;
	std::condition_variable m_Wake;
	std::deque<std::packaged_task<std::string()>> m_Queue;
	bool m_Stop = false;
	// Set by the worker once the server is made, or failed to be.
	bool m_Ready = false;
	std::string m_Error;
	std::unique_ptr<CMapMcp> m_pMcp;
	std::vector<std::string> m_vArgs;
	std::vector<const char *> m_vpArgs;

	std::string Run(std::function<std::string()> Job)
	{
		std::packaged_task<std::string()> Task(std::move(Job));
		std::future<std::string> Answer = Task.get_future();
		{
			const std::lock_guard<std::mutex> Lock(m_Mutex);
			m_Queue.push_back(std::move(Task));
		}
		m_Wake.notify_all();
		return Answer.get();
	}

	void Loop(const COptions &Options, bool Render)
	{
		std::unique_ptr<IRenderer> pRenderer;
		std::string RenderError;
		if(Render)
		{
			pRenderer = CreateHeadlessRenderer(Options.m_NumArgs, Options.m_ppArguments, &RenderError);
			if(pRenderer == nullptr)
				log_warn("mcp", "no renderer: %s", RenderError.c_str());
		}
		auto pMcp = std::make_unique<CMapMcp>(Options, std::move(pRenderer));
		{
			const std::lock_guard<std::mutex> Lock(m_Mutex);
			if(pMcp->Ok())
				m_pMcp = std::move(pMcp);
			else
				m_Error = pMcp->Error();
			m_Ready = true;
		}
		m_Wake.notify_all();
		while(true)
		{
			std::packaged_task<std::string()> Task;
			{
				std::unique_lock<std::mutex> Lock(m_Mutex);
				m_Wake.wait(Lock, [this] { return m_Stop || !m_Queue.empty(); });
				if(m_Stop && m_Queue.empty())
					break;
				Task = std::move(m_Queue.front());
				m_Queue.pop_front();
			}
			if(m_pMcp != nullptr)
				m_pMcp->CloseIdle();
			Task();
		}
		// The maps and the renderer die on the thread that made them.
		m_pMcp = nullptr;
	}
};

extern "C" {

ddnet_map_mcp *ddnet_map_mcp_create(const char *pOptionsJson, char **ppErrorOut)
{
	if(ppErrorOut != nullptr)
		*ppErrorOut = nullptr;
	CJson Options;
	std::string ParseError;
	if(pOptionsJson == nullptr || !CJson::Parse(pOptionsJson, &Options, &ParseError) || !Options.IsObject())
	{
		if(ppErrorOut != nullptr)
			*ppErrorOut = Give("the options are not a JSON object: " + ParseError);
		return nullptr;
	}
	// Whatever this library says goes to stderr: stdout may be the protocol.
	log_set_global_logger(log_logger_file(io_stderr()).release());
	auto *pServer = new ddnet_map_mcp();
	COptions Parsed;
	Parsed.m_Root = Options.Get("root").AsCStr("");
	if(Options.Has("historyMb"))
		Parsed.m_HistoryBytes = (uint64_t)std::max<int64_t>(1, Options.Get("historyMb").AsInt(256)) * 1024 * 1024;
	if(Options.Has("historyEntries"))
		Parsed.m_HistoryEntries = (size_t)std::max<int64_t>(1, Options.Get("historyEntries").AsInt(1000));
	if(Options.Has("maxMaps"))
		Parsed.m_MaxMaps = (size_t)std::max<int64_t>(1, Options.Get("maxMaps").AsInt(4));
	Parsed.m_SequentialHandles = Options.Get("sequentialHandles").AsBool(false);
	Parsed.m_IdleSeconds = Options.Get("idleSeconds").AsInt(0);
	const CJson &Args = Options.Get("args");
	for(size_t i = 0; i < Args.Size(); ++i)
		pServer->m_vArgs.emplace_back(Args.At(i).AsCStr(""));
	for(const std::string &Arg : pServer->m_vArgs)
		pServer->m_vpArgs.push_back(Arg.c_str());
	Parsed.m_NumArgs = (int)pServer->m_vpArgs.size();
	Parsed.m_ppArguments = pServer->m_vpArgs.data();
	const bool Render = Options.Get("render").AsBool(true);

	pServer->m_Thread = std::thread([pServer, Parsed, Render] { pServer->Loop(Parsed, Render); });
	{
		std::unique_lock<std::mutex> Lock(pServer->m_Mutex);
		pServer->m_Wake.wait(Lock, [pServer] { return pServer->m_Ready; });
	}
	if(pServer->m_pMcp == nullptr)
	{
		if(ppErrorOut != nullptr)
			*ppErrorOut = Give(pServer->m_Error);
		ddnet_map_mcp_destroy(pServer);
		return nullptr;
	}
	return pServer;
}

void ddnet_map_mcp_destroy(ddnet_map_mcp *pServer)
{
	if(pServer == nullptr)
		return;
	{
		const std::lock_guard<std::mutex> Lock(pServer->m_Mutex);
		pServer->m_Stop = true;
	}
	pServer->m_Wake.notify_all();
	if(pServer->m_Thread.joinable())
		pServer->m_Thread.join();
	delete pServer;
}

char *ddnet_map_mcp_info(ddnet_map_mcp *pServer)
{
	return Give(pServer->Run([pServer] {
		CJson Json = CJson::Object();
		Json.Set("name", CJson::Str(CMapMcp::SERVER_NAME));
		Json.Set("version", CJson::Str(CMapMcp::SERVER_VERSION));
		Json.Set("instructions", CJson::Str(pServer->m_pMcp->Instructions()));
		return Json.Serialize();
	}));
}

char *ddnet_map_mcp_tools(ddnet_map_mcp *pServer)
{
	return Give(pServer->Run([pServer] { return pServer->m_pMcp->ToolsJson().Serialize(); }));
}

char *ddnet_map_mcp_call(ddnet_map_mcp *pServer, const char *pName, const char *pArgumentsJson, const char *pMetaJson)
{
	(void)pMetaJson;
	const std::string Name = pName == nullptr ? "" : pName;
	const std::string Arguments = pArgumentsJson == nullptr ? "" : pArgumentsJson;
	return Give(pServer->Run([pServer, Name, Arguments] {
		CJson Args = CJson::Object();
		std::string ParseError;
		if(!Arguments.empty() && !CJson::Parse(Arguments.c_str(), &Args, &ParseError))
			return ErrorJson(-32602, "the arguments are not JSON: " + ParseError);
		if(Args.IsNull())
			Args = CJson::Object();
		bool UnknownTool = false;
		const CToolResult Result = pServer->m_pMcp->CallTool(Name.c_str(), Args, &UnknownTool);
		if(UnknownTool)
			return ErrorJson(-32602, "Unknown tool: " + Name);
		return ResultJson(CMapMcp::ResultJson(Result));
	}));
}

char *ddnet_map_mcp_resources(ddnet_map_mcp *pServer)
{
	return Give(pServer->Run([pServer] { return pServer->m_pMcp->ResourcesJson().Serialize(); }));
}

char *ddnet_map_mcp_resource_templates(ddnet_map_mcp *pServer)
{
	return Give(pServer->Run([pServer] { return pServer->m_pMcp->ResourceTemplatesJson().Serialize(); }));
}

char *ddnet_map_mcp_read_resource(ddnet_map_mcp *pServer, const char *pUri)
{
	const std::string Uri = pUri == nullptr ? "" : pUri;
	return Give(pServer->Run([pServer, Uri] {
		std::string Error;
		const CJson Contents = pServer->m_pMcp->ReadResource(Uri.c_str(), &Error);
		if(Contents.IsNull())
			return ErrorJson(-32602, Error);
		CJson Result = CJson::Object();
		Result.Set("contents", Contents);
		return ResultJson(std::move(Result));
	}));
}

char *ddnet_map_mcp_prompts(ddnet_map_mcp *pServer)
{
	return Give(pServer->Run([pServer] { return pServer->m_pMcp->PromptsJson().Serialize(); }));
}

char *ddnet_map_mcp_get_prompt(ddnet_map_mcp *pServer, const char *pName, const char *pArgumentsJson)
{
	const std::string Name = pName == nullptr ? "" : pName;
	const std::string Arguments = pArgumentsJson == nullptr ? "" : pArgumentsJson;
	return Give(pServer->Run([pServer, Name, Arguments] {
		CJson Args = CJson::Object();
		std::string ParseError;
		if(!Arguments.empty() && !CJson::Parse(Arguments.c_str(), &Args, &ParseError))
			return ErrorJson(-32602, "the arguments are not JSON: " + ParseError);
		std::string Error;
		const CJson Prompt = pServer->m_pMcp->GetPrompt(Name.c_str(), Args, &Error);
		if(Prompt.IsNull())
			return ErrorJson(-32602, Error);
		return ResultJson(Prompt);
	}));
}

void ddnet_map_mcp_close_idle(ddnet_map_mcp *pServer)
{
	pServer->Run([pServer] {
		pServer->m_pMcp->CloseIdle();
		return std::string();
	});
}

void ddnet_map_mcp_free(char *pText)
{
	free(pText);
}
}
