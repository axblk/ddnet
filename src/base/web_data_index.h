#ifndef BASE_WEB_DATA_INDEX_H
#define BASE_WEB_DATA_INDEX_H

#include <base/hash.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

/**
 * The index of the data directory in the browser build: what exists, how big
 * it is and the hash in its URL, and for a map the SHA-256 and the CRC a
 * server or a demo names it by. Not platform specific, so it can be tested.
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
		/**
		 * The whole SHA-256 of a map, which is what a server or a demo asks
		 * for. Only maps have it.
		 */
		std::optional<SHA256_DIGEST> m_Sha256;
		/**
		 * The CRC-32 of a map, which is all an old server asks for. Only
		 * maps have it.
		 */
		std::optional<unsigned> m_Crc;
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

#endif
