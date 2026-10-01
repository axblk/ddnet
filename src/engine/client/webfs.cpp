#include "webfs.h"

#if defined(CONF_PLATFORM_EMSCRIPTEN)

#include <base/lock.h>
#include <base/log.h>
#include <base/str.h>
#include <base/thread.h>
#include <base/web_data.h>
#include <base/web_data_index.h>

#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#include <emscripten/threading.h>

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	// clang-format off
EM_JS(double, WebFsNow, (), {
	return Date.now();
});
	// clang-format on

	// `REPLACE` keeps Emscripten's IndexedDB cache out, which could only ever
	// miss, and lets a worker make a synchronous request.
	constexpr uint32_t FETCH_ATTRIBUTES = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_REPLACE;

	// `XMLHttpRequest.DONE`.
	constexpr unsigned short FETCH_STATE_DONE = 4;

	// The files that may still be read as they are opened, which blocks until
	// they are fetched. They are text that is parsed by line where it is
	// opened (languages, settings, automapper rules, JSON), and small; turning
	// each of those readers asynchronous would buy little. Everything binary
	// (maps, images, sounds) is fetched through the asset loader instead: a
	// binary file read here is logged as an error with where it was read, and
	// the headless job in `build.yml` fails on that line.
	constexpr const char *SYNCHRONOUS_EXTENSIONS[] = {".cfg", ".json", ".rules", ".txt"};

	bool MayReadSynchronously(const char *pPath)
	{
		for(const char *pExtension : SYNCHRONOUS_EXTENSIONS)
		{
			if(str_endswith(pPath, pExtension))
				return true;
		}
		return false;
	}

	bool WebFsFetch(const char *pUrl, std::vector<uint8_t> &vData)
	{
		emscripten_fetch_attr_t Attr;
		emscripten_fetch_attr_init(&Attr);
		str_copy(Attr.requestMethod, "GET");
		Attr.attributes = FETCH_ATTRIBUTES;
		if(!emscripten_is_main_runtime_thread())
			Attr.attributes |= EMSCRIPTEN_FETCH_SYNCHRONOUS;
		emscripten_fetch_t *pFetch = emscripten_fetch(&Attr, pUrl);
		if(pFetch == nullptr)
		{
			log_error("webfs", "Could not start a request for '%s'", pUrl);
			return false;
		}
		// The main thread has to give the browser its turn to finish the request.
		while(pFetch->readyState != FETCH_STATE_DONE)
		{
			web_yield(1);
		}
		const bool Success = pFetch->status == 200;
		if(Success)
		{
			vData.assign(pFetch->data, pFetch->data + pFetch->numBytes);
		}
		else
		{
			log_error("webfs", "'%s' answered %d %s", pUrl, (int)pFetch->status,
				pFetch->statusText[0] == '\0' ? "(no status text, see the browser console)" : pFetch->statusText);
		}
		emscripten_fetch_close(pFetch);
		return Success;
	}

	class CWebFs final : public IWebDataMount
	{
		// Read by `webfs_init` before any other thread starts.
		CWebDataIndex m_Index;
		CLock m_Lock;
		// The bytes of everything that was fetched. Nothing is thrown out,
		// because open handles read straight out of here.
		std::unordered_map<std::string, std::vector<uint8_t>> m_Files GUARDED_BY(m_Lock);

		const std::vector<uint8_t> *Bytes(const CWebDataIndex::CEntry &Entry) REQUIRES(!m_Lock);

	public:
		bool Init() REQUIRES(!m_Lock);

		IOHANDLE Open(const char *pPath) override REQUIRES(!m_Lock);
		bool IsFile(const char *pPath) const override { return m_Index.IsFile(pPath); }
		bool IsDirectory(const char *pPath) const override { return m_Index.IsDirectory(pPath); }
		void List(const char *pPath, const std::function<bool(const char *pName, bool IsDirectory)> &Callback) const override;
		const char *Version(const char *pPath) const override;
		bool Digests(const char *pPath, SHA256_DIGEST *pSha256, unsigned *pCrc) const override;
	};

	bool CWebFs::Init()
	{
		// The index is the one file not named by its hash, and a cached copy of
		// it (GitHub Pages says `max-age=600`) would name the files of the
		// deploy before. So its address holds the time.
		char aIndexUrl[512];
		if(!web_data_url("data/index.txt", aIndexUrl, sizeof(aIndexUrl)))
			return false;
		char aTime[32];
		str_format(aTime, sizeof(aTime), "?t=%.0f", WebFsNow());
		str_append(aIndexUrl, aTime);
		std::vector<uint8_t> vIndex;
		if(!WebFsFetch(aIndexUrl, vIndex))
		{
			log_error("webfs", "The index of the data directory could not be fetched");
			return false;
		}
		if(!m_Index.Parse((const char *)vIndex.data(), vIndex.size()))
		{
			log_error("webfs", "The index of the data directory could not be read");
			return false;
		}
		log_info("webfs", "The data directory has %" PRIzu " entries", m_Index.Count());
		return true;
	}

	const std::vector<uint8_t> *CWebFs::Bytes(const CWebDataIndex::CEntry &Entry)
	{
		{
			const CLockScope LockScope(m_Lock);
			const auto Found = m_Files.find(Entry.m_Path);
			if(Found != m_Files.end())
				return &Found->second;
		}

		// Fetched without the lock, so that other threads can read what is
		// here. On the main thread the page stands still for the length of the
		// request; `main-thread-reads.txt` lists the ones that may.
		if(!MayReadSynchronously(Entry.m_Path.c_str()))
		{
			char aCaller[2048];
			emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, aCaller, sizeof(aCaller));
			log_error("webfs", "'%s' is not a text file and was read synchronously, fetch it through the asset loader. Read from:\n%s", Entry.m_Path.c_str(), aCaller);
		}
		// Every one is logged, so that what is still read this way can be
		// listed from a run.
		if(emscripten_is_main_runtime_thread())
			log_warn("webfs", "'%s' was fetched on the main thread", Entry.m_Path.c_str());
		else
			log_info("webfs", "'%s' was fetched synchronously", Entry.m_Path.c_str());
		const std::string Path = "data/" + Entry.m_Path;
		char aUrl[1024];
		std::vector<uint8_t> vData;
		if(!web_data_url(Path.c_str(), aUrl, sizeof(aUrl)) || !WebFsFetch(aUrl, vData))
			return nullptr;
		if((int64_t)vData.size() != Entry.m_Size)
		{
			log_error("webfs", "'%s' is %" PRIzu " bytes, the index says %" PRId64, Entry.m_Path.c_str(), vData.size(), Entry.m_Size);
			return nullptr;
		}

		const CLockScope LockScope(m_Lock);
		// Of two threads that fetched the same file the first one's bytes stay,
		// because a handle may already read from them.
		return &m_Files.emplace(Entry.m_Path, std::move(vData)).first->second;
	}

	IOHANDLE CWebFs::Open(const char *pPath)
	{
		const CWebDataIndex::CEntry *pEntry = m_Index.Find(pPath);
		if(pEntry == nullptr || pEntry->m_IsDirectory)
			return nullptr;
		const std::vector<uint8_t> *pBytes = Bytes(*pEntry);
		if(pBytes == nullptr)
			return nullptr;
		return fmemopen(const_cast<uint8_t *>(pBytes->data()), pBytes->size(), "rb");
	}

	void CWebFs::List(const char *pPath, const std::function<bool(const char *pName, bool IsDirectory)> &Callback) const
	{
		bool Stop = false;
		m_Index.List(pPath, [&](const CWebDataIndex::CEntry &Entry) {
			if(Stop)
				return;
			const size_t Slash = Entry.m_Path.find_last_of('/');
			Stop = Callback(Entry.m_Path.c_str() + (Slash == std::string::npos ? 0 : Slash + 1), Entry.m_IsDirectory);
		});
	}

	const char *CWebFs::Version(const char *pPath) const
	{
		// The bytes behind a name and a hash never change.
		const CWebDataIndex::CEntry *pEntry = m_Index.Find(pPath);
		if(pEntry == nullptr || pEntry->m_IsDirectory)
			return nullptr;
		return pEntry->m_Hash.c_str();
	}

	bool CWebFs::Digests(const char *pPath, SHA256_DIGEST *pSha256, unsigned *pCrc) const
	{
		const CWebDataIndex::CEntry *pEntry = m_Index.Find(pPath);
		if(pEntry == nullptr || !pEntry->m_Sha256.has_value() || !pEntry->m_Crc.has_value())
			return false;
		*pSha256 = pEntry->m_Sha256.value();
		*pCrc = pEntry->m_Crc.value();
		return true;
	}

	CWebFs g_WebFs;
} // namespace

bool webfs_init()
{
	web_data_init();
	if(!g_WebFs.Init())
		return false;
	web_data_mount(&g_WebFs);
	return true;
}

#endif
