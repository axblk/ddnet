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
 * The index of the data directory in the browser build: what exists, how big
 * it is and the hash in its URL. Not platform specific, so it can be tested.
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
		 * First 16 hex digits of the file's SHA-256. Empty for a directory.
		 */
		std::string m_Hash;
	};

	/**
	 * Reads an index that `scripts/generate_web_data.py` wrote. Keeps nothing
	 * of one it could not read to the end.
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
	 * Calls `pfnCallback` for everything directly in a directory, sorted by
	 * path.
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
 * Fetches the index of the data directory, which lies at
 * `Module.ddnetDataBase` or beside the page. Has to run before the storage is
 * initialised and before other threads start.
 *
 * @return `true` when the index was read.
 */
bool webfs_init();

/**
 * Whether a path is below the data directory, which is fetched; everything
 * else is left to the file system.
 */
bool webfs_owns(const char *pPath);

/**
 * Opens a file of the data directory for reading. It is fetched the first
 * time and kept in memory.
 */
IOHANDLE webfs_open(const char *pPath);

/**
 * The URL of a file below the data directory, for fetching it without
 * waiting. It holds the file's hash, so it can be cached for good.
 *
 * @param pPath Path of the file, as `webfs_owns` accepts it.
 * @param pBuffer Receives the address.
 * @param BufferSize Size of `pBuffer`.
 *
 * @return `false` when the index has no such file, and then `pBuffer` is empty.
 */
bool webfs_url(const char *pPath, char *pBuffer, size_t BufferSize);

bool webfs_is_file(const char *pPath);
bool webfs_is_dir(const char *pPath);
/**
 * Always 0: the data directory does not change after the build.
 */
bool webfs_file_time(const char *pPath, int64_t *pCreated, int64_t *pModified);
void webfs_listdir(const char *pPath, FS_LISTDIR_CALLBACK pfnCallback, int Type, void *pUser);
void webfs_listdir_fileinfo(const char *pPath, FS_LISTDIR_CALLBACK_FILEINFO pfnCallback, int Type, void *pUser);

#endif

#endif
