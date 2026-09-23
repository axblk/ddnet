#ifndef ENGINE_SHARED_SERVER_PIN_H
#define ENGINE_SHARED_SERVER_PIN_H

#include <base/types.h>

/**
 * What a client pins a server by, as the fragments of the server's QUIC,
 * WebTransport and WebSocket addresses carry it: `spki-sha256=<hex>`, the
 * SHA-256 of the DER SubjectPublicKeyInfo of the server's identity key, on
 * the QUIC and `wss://` addresses, and on the WebTransport address
 * `cert-sha256=<hex>[,<hex>]` for the certificates a browser takes by
 * their hash or `webpki` for one signed for the host name. Plain WebSockets
 * have no certificate to pin.
 */
class CServerPin
{
public:
	/**
	 * The `spki-sha256` of the identity as 64 hex digits, empty if unknown.
	 */
	char m_aIdentity[65];
	/**
	 * The fragment of the WebTransport address, empty if unknown.
	 */
	char m_aWebTransport[160];

	void Reset();

	/**
	 * Takes what the fragment of one of the server's addresses pins, where
	 * nothing of its kind was known yet.
	 *
	 * @param Addr The address, its scheme's flags tell the transport.
	 * @param pFragment The fragment, without the `#`.
	 */
	void AddFragment(const NETADDR &Addr, const char *pFragment);

	/**
	 * Takes what another pin knows where nothing of its kind was known yet.
	 *
	 * @param Other The other pin.
	 */
	void Merge(const CServerPin &Other);

	/**
	 * Writes the fragment one of the server's addresses connects with.
	 *
	 * @param Addr The address, its scheme's flags tell the transport.
	 * @param pBuffer The buffer, left empty for a legacy address or where
	 * nothing is known.
	 * @param BufferSize The size of the buffer.
	 */
	void Fragment(const NETADDR &Addr, char *pBuffer, int BufferSize) const;

	/**
	 * Writes one of the server's addresses with its scheme and the fragment
	 * it connects with, the way a master lists it and the server list and a
	 * connect read it back.
	 *
	 * @param Addr The address.
	 * @param pHost The host name to write in place of the IP address, with
	 * the address's port, or `nullptr`.
	 * @param pBuffer The buffer, of at least `URL_MAXSTRSIZE` bytes, and
	 * `NAMED_URL_MAXSTRSIZE` with a host name.
	 * @param BufferSize The size of the buffer.
	 */
	void AddressUrl(const NETADDR &Addr, const char *pHost, char *pBuffer, int BufferSize) const;

	/**
	 * Whether a browser checks the certificate of the address against the
	 * server's host name, so that it has to connect by the name.
	 *
	 * @param Addr The address.
	 */
	bool SignedForName(const NETADDR &Addr) const;

	static constexpr int URL_MAXSTRSIZE = NETADDR_URL_MAXSTRSIZE + 1 + sizeof(m_aWebTransport);
	static constexpr int NAMED_URL_MAXSTRSIZE = URL_MAXSTRSIZE + 128;
};

#endif
