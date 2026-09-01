#ifndef ENGINE_SHARED_EBPF_KEY_H
#define ENGINE_SHARED_EBPF_KEY_H

#include <base/types.h>

/**
 * The key the XDP filter service owns, read from the file it writes.
 *
 * The filter decides whether a packet belongs to an existing connection by
 * recomputing the security token or the connection ID tag from this key. Both sides
 * therefore have to derive them the same way, and the service is the authority: it
 * generates the key, rotates it and writes it out, while a server only ever reads.
 *
 * Without a readable key the server keeps its previous derivation, which the filter
 * cannot recognise. That is deliberate: a port whose server does not use the key is
 * never armed, so it goes unprotected rather than being cut off.
 */
class CEbpfKey
{
public:
	/**
	 * Epoch, then the two key halves little endian, in the layout the QUIC
	 * connection ID generator expects.
	 */
	enum
	{
		MATERIAL_SIZE = 1 + 8 + 8,
	};

	/**
	 * Reads the key file. An empty path disables the derivation.
	 *
	 * @return `true` if a usable key was read, `false` if the file is missing,
	 * unreadable or written in a format this build does not know.
	 */
	bool Load(const char *pPath);

	/**
	 * Re-reads the file if it changed since the last time.
	 *
	 * @return `true` if a different key is now in use.
	 */
	bool Reload(const char *pPath);

	bool IsLoaded() const { return m_Loaded; }

	/**
	 * Derives the token for an address. The port is part of the derivation, so two
	 * clients behind the same address do not share a token.
	 */
	unsigned int Token(const NETADDR &Addr) const;

	const unsigned char *Material() const { return m_aMaterial; }

	/**
	 * Counts up whenever a different key comes into use. Whoever hands the
	 * material to something that keeps its own copy - the QUIC connection ID
	 * generator does - can tell from this that its copy is stale, without
	 * comparing key material it should not have to hold on to.
	 */
	unsigned long long Generation() const { return m_Generation; }

private:
	bool m_Loaded = false;
	int64_t m_ModifiedTime = 0;
	unsigned long long m_K0 = 0;
	unsigned long long m_K1 = 0;
	unsigned char m_aMaterial[MATERIAL_SIZE] = {};
	unsigned long long m_Generation = 0;
};

#endif
