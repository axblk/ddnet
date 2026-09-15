#include "webfs.h"

#include <algorithm>
#include <string_view>

namespace
{
	// Kept in step with `scripts/generate_web_data.py`.
	constexpr std::string_view FORMAT_HEADER = "ddnet-web-data 1";
	constexpr size_t HASH_LENGTH = 16;

	bool PathIsSane(std::string_view Path)
	{
		// The index names files that are fetched and opened, so a path that walks
		// out of the data directory or that no file can have is refused here
		// rather than everywhere it is used.
		if(Path.empty() || Path.front() == '/' || Path.back() == '/')
			return false;
		if(Path.find('\\') != std::string_view::npos || Path.find("//") != std::string_view::npos)
			return false;
		for(const char Character : Path)
		{
			if((unsigned char)Character < 0x20)
				return false;
		}
		size_t Start = 0;
		while(Start <= Path.size())
		{
			const size_t End = std::min(Path.find('/', Start), Path.size());
			const std::string_view Part = Path.substr(Start, End - Start);
			if(Part == "." || Part == "..")
				return false;
			Start = End + 1;
		}
		return true;
	}

	bool ParseSize(std::string_view Text, int64_t *pSize)
	{
		if(Text.empty() || Text.size() > 19)
			return false;
		int64_t Size = 0;
		for(const char Character : Text)
		{
			if(Character < '0' || Character > '9')
				return false;
			Size = Size * 10 + (Character - '0');
		}
		*pSize = Size;
		return true;
	}

	bool ParseHash(std::string_view Text)
	{
		if(Text.size() != HASH_LENGTH)
			return false;
		return std::all_of(Text.begin(), Text.end(), [](const char Character) {
			return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
		});
	}
} // namespace

bool CWebDataIndex::Parse(const char *pData, size_t Size)
{
	m_Entries.clear();

	std::string_view Rest(pData, Size);
	const auto &&NextLine = [&Rest]() -> std::string_view {
		const size_t End = std::min(Rest.find('\n'), Rest.size());
		const std::string_view Line = Rest.substr(0, End);
		Rest = Rest.substr(std::min(End + 1, Rest.size()));
		// Written with Unix line endings, but a checkout may have turned them.
		return Line.empty() || Line.back() != '\r' ? Line : Line.substr(0, Line.size() - 1);
	};

	if(NextLine() != FORMAT_HEADER)
	{
		m_Entries.clear();
		return false;
	}

	while(!Rest.empty())
	{
		const std::string_view Line = NextLine();
		if(Line.empty())
			continue;

		CEntry Entry;
		std::string_view Path;
		if(Line.substr(0, 2) == "d ")
		{
			Entry.m_IsDirectory = true;
			Path = Line.substr(2);
		}
		else if(Line.substr(0, 2) == "f ")
		{
			// A file name may hold spaces, so the two fields behind it are
			// taken off the end and whatever is left is the path.
			const size_t HashStart = Line.rfind(' ');
			if(HashStart == std::string_view::npos)
			{
				m_Entries.clear();
				return false;
			}
			const size_t SizeStart = Line.rfind(' ', HashStart - 1);
			if(SizeStart == std::string_view::npos || SizeStart < 2)
			{
				m_Entries.clear();
				return false;
			}
			if(!ParseSize(Line.substr(SizeStart + 1, HashStart - SizeStart - 1), &Entry.m_Size) ||
				!ParseHash(Line.substr(HashStart + 1)))
			{
				m_Entries.clear();
				return false;
			}
			Entry.m_Hash = Line.substr(HashStart + 1);
			Path = Line.substr(2, SizeStart - 2);
		}
		else
		{
			m_Entries.clear();
			return false;
		}

		if(!PathIsSane(Path))
		{
			m_Entries.clear();
			return false;
		}
		Entry.m_Path = Path;
		if(!m_Entries.emplace(Entry.m_Path, std::move(Entry)).second)
		{
			m_Entries.clear();
			return false;
		}
	}

	return true;
}

const CWebDataIndex::CEntry *CWebDataIndex::Find(const char *pPath) const
{
	const auto It = m_Entries.find(pPath);
	return It == m_Entries.end() ? nullptr : &It->second;
}

bool CWebDataIndex::IsFile(const char *pPath) const
{
	const CEntry *pEntry = Find(pPath);
	return pEntry != nullptr && !pEntry->m_IsDirectory;
}

bool CWebDataIndex::IsDirectory(const char *pPath) const
{
	if(pPath[0] == '\0')
		return true;
	const CEntry *pEntry = Find(pPath);
	return pEntry != nullptr && pEntry->m_IsDirectory;
}

void CWebDataIndex::List(const char *pPath, const std::function<void(const CEntry &)> &pfnCallback) const
{
	std::string Prefix = pPath;
	if(!Prefix.empty())
		Prefix += '/';
	// Everything in a directory sorts directly behind it, because the
	// directory with its separator is a prefix of every path below it.
	for(auto It = m_Entries.lower_bound(Prefix); It != m_Entries.end(); ++It)
	{
		const std::string &Path = It->first;
		if(Path.compare(0, Prefix.size(), Prefix) != 0)
			break;
		if(Path.find('/', Prefix.size()) != std::string::npos)
			continue;
		pfnCallback(It->second);
	}
}

#if defined(CONF_PLATFORM_EMSCRIPTEN)

#include <base/lock.h>
#include <base/log.h>
#include <base/str.h>

#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/threading.h>

#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>

namespace
{
	// Where the page is, so that a worker builds the same URL as the page does
	// rather than resolving one against the script it was started from.
	// clang-format off
EM_JS(char *, WebFsPageBase, (), {
	return stringToNewUTF8(new URL(".", location.href).href);
});

EM_JS(double, WebFsNow, (), {
	return Date.now();
});
	// clang-format on

	CLock g_WebFsLock;
	std::string g_WebFsBase GUARDED_BY(g_WebFsLock);
	CWebDataIndex g_WebFsIndex;
	bool g_WebFsReady = false;
	// The bytes of everything that was fetched. Nothing is ever thrown out: a
	// handle that is still open reads straight out of here, and the whole data
	// directory is smaller than what the page used to be given before it started.
	std::unordered_map<std::string, std::vector<uint8_t>> g_WebFsFiles GUARDED_BY(g_WebFsLock);

	// Emscripten's own cache in IndexedDB is not wanted anywhere here: nothing
	// writes to it, so looking in it means an asynchronous detour before every
	// single read that can only ever miss. `REPLACE` is what turns it off, and
	// on a worker it is also what keeps a synchronous request from being
	// refused outright.
	constexpr uint32_t FETCH_ATTRIBUTES = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE;

	// `emscripten_fetch_t::readyState` is the one of the `XMLHttpRequest` behind
	// it, and `DONE` is the only value worth naming here.
	constexpr unsigned short FETCH_STATE_DONE = 4;

	bool WebFsFetch(const std::string &Url, std::vector<uint8_t> &vData)
	{
		emscripten_fetch_attr_t Attr;
		emscripten_fetch_attr_init(&Attr);
		str_copy(Attr.requestMethod, "GET");
		Attr.attributes = FETCH_ATTRIBUTES;
		if(!emscripten_is_main_runtime_thread())
		{
			// A worker may wait, and every file that is read while something is
			// going on is read on one.
			Attr.attributes |= EMSCRIPTEN_FETCH_SYNCHRONOUS;
		}
		emscripten_fetch_t *pFetch = emscripten_fetch(&Attr, Url.c_str());
		if(pFetch == nullptr)
		{
			log_error("webfs", "Could not start a request for '%s'", Url.c_str());
			return false;
		}
		// The main thread cannot wait for a request without the browser getting
		// its turn back, so it unwinds instead. A synchronous request is already
		// finished by the time it returns, so this does nothing for one.
		while(pFetch->readyState != FETCH_STATE_DONE)
		{
			emscripten_sleep(1);
		}
		const bool Success = pFetch->status == 200;
		if(Success)
		{
			vData.assign(pFetch->data, pFetch->data + pFetch->numBytes);
		}
		else
		{
			log_error("webfs", "'%s' answered %d %s", Url.c_str(), (int)pFetch->status,
				pFetch->statusText[0] == '\0' ? "(no status text, see the browser console)" : pFetch->statusText);
		}
		emscripten_fetch_close(pFetch);
		return Success;
	}

	// The part of the path below the data directory, or `nullptr` for a path that
	// is somewhere else. The client writes the same place three ways depending on
	// where the path came from, so all three are understood.
	const char *WebFsRelativePath(const char *pPath)
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

	const std::vector<uint8_t> *WebFsBytes(const char *pRelativePath)
	{
		const CWebDataIndex::CEntry *pEntry = g_WebFsIndex.Find(pRelativePath);
		if(pEntry == nullptr || pEntry->m_IsDirectory)
			return nullptr;

		{
			const CLockScope LockScope(g_WebFsLock);
			const auto Found = g_WebFsFiles.find(pEntry->m_Path);
			if(Found != g_WebFsFiles.end())
				return &Found->second;
		}

		// Fetched without the lock: a worker that waits for a file must not keep
		// every other worker from reading the ones that are already here.
		std::string Url;
		{
			const CLockScope LockScope(g_WebFsLock);
			Url = g_WebFsBase;
		}
		// The bytes behind a name and a hash never change, so a browser may keep
		// them for as long as it likes.
		Url += "data/" + pEntry->m_Path + "?v=" + pEntry->m_Hash;
		std::vector<uint8_t> vData;
		if(!WebFsFetch(Url, vData))
			return nullptr;
		if((int64_t)vData.size() != pEntry->m_Size)
		{
			log_error("webfs", "'%s' is %" PRIzu " bytes, the index says %" PRId64, pEntry->m_Path.c_str(), vData.size(), pEntry->m_Size);
			return nullptr;
		}

		const CLockScope LockScope(g_WebFsLock);
		// Two threads that asked for the same file at the same time both fetched
		// it; whichever got here first is the one that is kept, so that the bytes
		// an open handle reads from stay where they are.
		return &g_WebFsFiles.emplace(pEntry->m_Path, std::move(vData)).first->second;
	}
} // namespace

bool webfs_init()
{
	char *pBase = WebFsPageBase();
	{
		const CLockScope LockScope(g_WebFsLock);
		g_WebFsBase = pBase == nullptr ? "" : pBase;
	}
	free(pBase);

	std::vector<uint8_t> vIndex;
	std::string Url;
	{
		const CLockScope LockScope(g_WebFsLock);
		Url = g_WebFsBase;
	}
	// Every other file is named after the hash of its contents, so a cached copy
	// of one is by definition the right one. The index is the exception: it is
	// what those hashes are read from, and GitHub Pages answers `max-age=600`,
	// so a page opened within ten minutes of a deploy would otherwise look for
	// files by the hashes of the deploy before it. The moment the page was
	// opened is therefore part of its URL, which no cache can have seen.
	char aIndexUrl[512];
	str_format(aIndexUrl, sizeof(aIndexUrl), "%sdata/index.txt?t=%.0f", Url.c_str(), WebFsNow());
	if(!WebFsFetch(aIndexUrl, vIndex))
	{
		log_error("webfs", "The index of the data directory could not be fetched");
		return false;
	}
	if(!g_WebFsIndex.Parse((const char *)vIndex.data(), vIndex.size()))
	{
		log_error("webfs", "The index of the data directory could not be read");
		return false;
	}
	g_WebFsReady = true;
	log_info("webfs", "The data directory has %" PRIzu " entries", g_WebFsIndex.Count());
	return true;
}

bool webfs_owns(const char *pPath)
{
	return g_WebFsReady && WebFsRelativePath(pPath) != nullptr;
}

IOHANDLE webfs_open(const char *pPath)
{
	const char *pRelativePath = WebFsRelativePath(pPath);
	if(pRelativePath == nullptr)
		return nullptr;
	const std::vector<uint8_t> *pBytes = WebFsBytes(pRelativePath);
	if(pBytes == nullptr)
		return nullptr;
	// Reading, so this keeps the pointer instead of copying, which is why
	// nothing is ever thrown out of the cache.
	return fmemopen(const_cast<uint8_t *>(pBytes->data()), pBytes->size(), "rb");
}

bool webfs_is_file(const char *pPath)
{
	const char *pRelativePath = WebFsRelativePath(pPath);
	return pRelativePath != nullptr && g_WebFsIndex.IsFile(pRelativePath);
}

bool webfs_is_dir(const char *pPath)
{
	const char *pRelativePath = WebFsRelativePath(pPath);
	if(pRelativePath == nullptr)
		return false;
	// The data directory itself is not in its own index, but it is the one
	// directory that is certainly there: it is what the index describes.
	return pRelativePath[0] == '\0' || g_WebFsIndex.IsDirectory(pRelativePath);
}

bool webfs_file_time(const char *pPath, int64_t *pCreated, int64_t *pModified)
{
	const char *pRelativePath = WebFsRelativePath(pPath);
	if(pRelativePath == nullptr || (pRelativePath[0] != '\0' && g_WebFsIndex.Find(pRelativePath) == nullptr))
		return false;
	// Everything here was written when the page was built.
	*pCreated = 0;
	*pModified = 0;
	return true;
}

void webfs_listdir(const char *pPath, FS_LISTDIR_CALLBACK pfnCallback, int Type, void *pUser)
{
	const char *pRelativePath = WebFsRelativePath(pPath);
	if(pRelativePath == nullptr)
		return;
	bool Stop = false;
	g_WebFsIndex.List(pRelativePath, [&](const CWebDataIndex::CEntry &Entry) {
		if(Stop)
			return;
		const size_t Slash = Entry.m_Path.find_last_of('/');
		const char *pName = Entry.m_Path.c_str() + (Slash == std::string::npos ? 0 : Slash + 1);
		Stop = pfnCallback(pName, Entry.m_IsDirectory ? 1 : 0, Type, pUser) != 0;
	});
}

void webfs_listdir_fileinfo(const char *pPath, FS_LISTDIR_CALLBACK_FILEINFO pfnCallback, int Type, void *pUser)
{
	const char *pRelativePath = WebFsRelativePath(pPath);
	if(pRelativePath == nullptr)
		return;
	bool Stop = false;
	g_WebFsIndex.List(pRelativePath, [&](const CWebDataIndex::CEntry &Entry) {
		if(Stop)
			return;
		const size_t Slash = Entry.m_Path.find_last_of('/');
		CFsFileInfo Info;
		Info.m_pName = Entry.m_Path.c_str() + (Slash == std::string::npos ? 0 : Slash + 1);
		Info.m_TimeCreated = 0;
		Info.m_TimeModified = 0;
		Stop = pfnCallback(&Info, Entry.m_IsDirectory ? 1 : 0, Type, pUser) != 0;
	});
}

#endif
