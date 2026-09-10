#ifndef ENGINE_SHARED_EBPF_KEY_H
#define ENGINE_SHARED_EBPF_KEY_H

#include <cstdint>

/**
 * The key the XDP filter service owns, read from the file it writes.
 *
 * The filter recomputes the security tokens and the QUIC connection ID tags
 * from this key, so the networking library has to derive them from it as
 * well, see `ddnet_net_set_filter_key`. Without a readable key the server
 * derives them its own way, which the filter does not arm the port for.
 */
class CEbpfKey
{
public:
	/**
	 * Epoch, then the two key halves little endian, as the networking
	 * library takes it.
	 */
	enum
	{
		MATERIAL_SIZE = 1 + 8 + 8,
	};

	/**
	 * Reads the key file. An empty path disables the key.
	 *
	 * @param pPath Path of the key file.
	 *
	 * @return `true` if a usable key was read, `false` if the file is missing,
	 * unreadable or written in a format this build does not know.
	 */
	bool Load(const char *pPath);

	/**
	 * Re-reads the file if it changed since the last time. A file that
	 * cannot be read keeps the key read before.
	 *
	 * @param pPath Path of the key file.
	 *
	 * @return `true` if a different key is now in use.
	 */
	bool Reload(const char *pPath);

	bool IsLoaded() const { return m_Loaded; }
	const unsigned char *Material() const { return m_aMaterial; }

private:
	bool m_Loaded = false;
	int64_t m_ModifiedTime = 0;
	unsigned char m_aMaterial[MATERIAL_SIZE] = {};
};

#endif
