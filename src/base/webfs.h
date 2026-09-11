#ifndef BASE_WEBFS_H
#define BASE_WEBFS_H

#include <base/detect.h>
#include <base/types.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

/**
 * The index of the data directory that the browser build is given before it
 * starts.
 *
 * A browser has no directory it can walk, so the build writes down what is in
 * `data` and the client reads that instead: what exists, how big it is and the
 * hash that names its URL. Only the reading half lives here, so that it is the
 * same everywhere and can be tested where there is no browser.
 */
class CWebDataIndex
{
public:
	class CEntry
	{
	public:
		std::string m_Path;
		bool m_IsDirectory = false;
		/**
		 * Size of the file in bytes. Zero for a directory.
		 */
		int64_t m_Size = 0;
		/**
		 * First 16 hex digits of the file's SHA-256, as they were written. The
		 * URL of the file is named after it, so bytes that were fetched once
		 * never have to be fetched again. Empty for a directory.
		 */
		std::string m_Hash;
	};

	/**
	 * Reads an index that `scripts/generate_web_data.py` wrote.
	 *
	 * Replaces whatever was read before, and keeps nothing of a file it could
	 * not read to the end.
	 *
	 * @param pData Contents of the index file.
	 * @param Size Number of bytes in `pData`.
	 *
	 * @return `true` on success.
	 */
	[[nodiscard]] bool Parse(const char *pData, size_t Size);

	/**
	 * Looks a path up in the index.
	 *
	 * @param pPath Path relative to the data directory, with `/` separators.
	 *
	 * @return The entry, or `nullptr` when there is none.
	 */
	const CEntry *Find(const char *pPath) const;
	bool IsFile(const char *pPath) const;
	bool IsDirectory(const char *pPath) const;

	/**
	 * Calls `pfnCallback` for everything directly in a directory, in the order
	 * the index has it, which is sorted by path.
	 *
	 * @param pPath Directory relative to the data directory, `""` for the data
	 * directory itself.
	 * @param pfnCallback Called once for every entry.
	 */
	void List(const char *pPath, const std::function<void(const CEntry &)> &pfnCallback) const;

	size_t Count() const { return m_Entries.size(); }

private:
	std::map<std::string, CEntry> m_Entries;
};

#if defined(CONF_PLATFORM_EMSCRIPTEN)

/**
 * Brings the data directory up: reads the index the build wrote and remembers
 * where it is, so that every file below `data` can be fetched from there
 * afterwards.
 *
 * Where it is, is `Module.ddnetDataBase` resolved against the page, or the page
 * itself where that is unset. A page that embeds this program and keeps `data`
 * somewhere else says so there.
 *
 * Has to run before anything looks for the data directory, so before the
 * storage is initialised.
 *
 * @return `true` when the index is there. Where it is not, nothing below
 * `data` can be opened and the caller has a broken installation on its hands,
 * which is what it is told.
 */
bool webfs_init();

/**
 * Whether a path names something in the data directory, which is the only
 * thing that comes over the network. Everything the user writes is somewhere
 * else and is left to the file system.
 */
bool webfs_owns(const char *pPath);

/**
 * Opens a file of the data directory for reading, fetching it the first time
 * and reading it out of memory every time after that.
 *
 * @return A handle to read from, or `nullptr`, which is what a caller already
 * expects from a file that is not there.
 */
IOHANDLE webfs_open(const char *pPath);

bool webfs_is_file(const char *pPath);
bool webfs_is_dir(const char *pPath);
/**
 * The time of a file of the data directory. Everything in it was written when
 * the page was built, and nothing in it changes afterwards, so this is the one
 * time the whole directory has.
 */
bool webfs_file_time(const char *pPath, int64_t *pCreated, int64_t *pModified);
void webfs_listdir(const char *pPath, FS_LISTDIR_CALLBACK pfnCallback, int Type, void *pUser);
void webfs_listdir_fileinfo(const char *pPath, FS_LISTDIR_CALLBACK_FILEINFO pfnCallback, int Type, void *pUser);

#endif

#endif
