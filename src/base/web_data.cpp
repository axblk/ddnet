#include "web_data.h"

#if defined(CONF_PLATFORM_EMSCRIPTEN)

#include <base/dbg.h>
#include <base/log.h>
#include <base/str.h>

#include <emscripten/emscripten.h>
#include <emscripten/threading.h>

#include <cstdlib>
#include <string>

namespace
{
	// Where the data directory is: where the page says, beside it otherwise.
	// Read on the page's thread, where the page's module and address are, so
	// that a program in a worker builds the same addresses.
	// clang-format off
EM_JS(char *, WebDataPageBase, (), {
	const base = Module["ddnetDataBase"];
	return stringToNewUTF8(new URL(base === undefined ? "." : base, location.href).href);
});
	// clang-format on

	// Written by `web_data_init` and `web_data_mount` before any other
	// thread starts, only read after that.
	bool g_Initialized = false;
	std::string g_Base;
	IWebDataMount *g_pMount = nullptr;
} // namespace

void web_data_init()
{
	if(g_Initialized)
		return;
	char *pBase = emscripten_is_main_runtime_thread() ?
			      WebDataPageBase() :
			      reinterpret_cast<char *>(emscripten_sync_run_in_main_runtime_thread(EM_FUNC_SIG_I, WebDataPageBase));
	g_Base = pBase == nullptr ? "" : pBase;
	free(pBase);
	g_Initialized = true;
}

void web_data_mount(IWebDataMount *pMount)
{
	dbg_assert(g_pMount == nullptr, "The data directory is mounted already");
	g_pMount = pMount;
}

IWebDataMount *web_data_mounted()
{
	return g_pMount;
}

const char *web_data_relative_path(const char *pPath)
{
	if(pPath == nullptr)
		return nullptr;
	if(pPath[0] == '.' && pPath[1] == '/')
		pPath += 2;
	else if(pPath[0] == '/')
		pPath += 1;
	if(str_comp_num(pPath, "data", 4) != 0)
		return nullptr;
	if(pPath[4] == '\0')
		return pPath + 4;
	if(pPath[4] != '/')
		return nullptr;
	return pPath + 5;
}

bool web_data_url(const char *pPath, char *pBuffer, size_t BufferSize)
{
	dbg_assert(g_Initialized, "web_data_init was not called");
	if(pBuffer == nullptr || BufferSize == 0)
		return false;
	pBuffer[0] = '\0';
	const char *pRelativePath = web_data_relative_path(pPath);
	if(pRelativePath == nullptr || pRelativePath[0] == '\0')
		return false;
	if(g_pMount == nullptr)
	{
		str_format(pBuffer, BufferSize, "%sdata/%s", g_Base.c_str(), pRelativePath);
		return true;
	}
	const char *pVersion = g_pMount->Version(pRelativePath);
	if(pVersion == nullptr)
		return false;
	str_format(pBuffer, BufferSize, "%sdata/%s?v=%s", g_Base.c_str(), pRelativePath, pVersion);
	return true;
}

void web_data_refuse(const char *pFunction, const char *pPath)
{
	log_error("web_data", "%s('%s'): this program has no file system for the data directory, the file has to be fetched", pFunction, pPath);
#if defined(CONF_DEBUG)
	// A debug build stops right there, so that a test cannot miss it.
	dbg_assert_failed("%s('%s') reached for the data directory, which this program has no file system for", pFunction, pPath);
#endif
}

#endif
