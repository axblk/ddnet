#ifndef BASE_WEB_DATA_H
#define BASE_WEB_DATA_H

#include <base/detect.h>
#include <base/hash.h>
#include <base/types.h>

#include <cstddef>
#include <functional>

#if defined(CONF_PLATFORM_EMSCRIPTEN)

/**
 * The data directory of a page. A browser build does not carry it: every
 * file of it is fetched, from beside the page or from where
 * `Module.ddnetDataBase` says.
 *
 * A program that knows what is in the directory mounts it (the client does,
 * see `webfs_init`), and then the file system functions open, list and look
 * up the directory's files through the mount. A program without a mount
 * asks for the files it wants by their address, see `web_data_url`, and
 * learns from the answer whether one is there. For it the file system
 * functions refuse the directory and say so in the log, because a program
 * that reaches for the directory that way does something it cannot do.
 */
class IWebDataMount
{
public:
	virtual ~IWebDataMount() = default;

	/**
	 * Opens a file for reading.
	 *
	 * @param pPath Path below the data directory, with `/` separators.
	 *
	 * @return The handle, or `nullptr` when there is no such file.
	 */
	virtual IOHANDLE Open(const char *pPath) = 0;
	virtual bool IsFile(const char *pPath) const = 0;
	/**
	 * @param pPath Path below the data directory, `""` for the directory
	 * itself.
	 */
	virtual bool IsDirectory(const char *pPath) const = 0;
	/**
	 * Calls `Callback` with the name of everything directly in a directory
	 * and whether that is a directory, until it returns `true`.
	 */
	virtual void List(const char *pPath, const std::function<bool(const char *pName, bool IsDirectory)> &Callback) const = 0;
	/**
	 * What the address of a file names besides its path, so that the
	 * address changes with the file and can be cached for good.
	 *
	 * @return The version, or `nullptr` when there is no such file.
	 */
	virtual const char *Version(const char *pPath) const = 0;
	/**
	 * The SHA-256 and the CRC a map is named by, known without fetching it.
	 *
	 * @return `false` for a file whose digests are not known.
	 */
	virtual bool Digests(const char *pPath, SHA256_DIGEST *pSha256, unsigned *pCrc) const = 0;
};

/**
 * Reads where the data directory is. Runs once, on the thread the program
 * started on, before any address is asked for.
 */
void web_data_init();

/**
 * Hands the file system functions a mount of the data directory. Has to
 * happen before other threads start, and the mount has to outlive them.
 */
void web_data_mount(IWebDataMount *pMount);

/**
 * @return The mount of the data directory, `nullptr` when the program has
 * none.
 */
IWebDataMount *web_data_mounted();

/**
 * The part of a path below the data directory. The storage writes `data/`,
 * `./data/` and `/data/`.
 *
 * @return The rest of the path, `""` for the directory itself, or `nullptr`
 * for a path elsewhere.
 */
const char *web_data_relative_path(const char *pPath);

/**
 * The address of a file of the data directory, to fetch it without waiting.
 * With a mount it names the version of the file, see
 * `IWebDataMount::Version`, and there is none for a file the mount does not
 * know. Without one it is the plain path: whether there is such a file, the
 * answer to the request tells.
 *
 * @param pPath Path of the file, as `web_data_relative_path` accepts it.
 * @param pBuffer Receives the address.
 * @param BufferSize Size of `pBuffer`.
 *
 * @return `false` when there is no address, and then `pBuffer` is empty.
 */
bool web_data_url(const char *pPath, char *pBuffer, size_t BufferSize);

/**
 * Logs that a file system function was used on the data directory by a
 * program without a mount of it, which is an error: the file has to be
 * fetched. A debug build stops there with a failed assertion.
 *
 * @param pFunction The function that was called.
 * @param pPath The path it was called with.
 */
void web_data_refuse(const char *pFunction, const char *pPath);

#endif

#endif
