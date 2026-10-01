#ifndef ENGINE_CLIENT_WEBFS_H
#define ENGINE_CLIENT_WEBFS_H

#include <base/detect.h>

#if defined(CONF_PLATFORM_EMSCRIPTEN)

/**
 * Fetches the index of the page's data directory and mounts the directory
 * with it (see `IWebDataMount`): listing and looking up read the index, and
 * opening a file fetches it, which blocks, so it is only for the few small
 * files that are read as text. Everything else goes through the asset
 * loader. Only the client mounts the directory; the web tools ask for their
 * files by address. Has to run before the storage is initialised and before
 * other threads start.
 *
 * @return `true` when the index was read.
 */
bool webfs_init();

#endif

#endif
