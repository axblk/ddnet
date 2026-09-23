#ifndef ENGINE_SHARED_TRANSPORT_PIN_H
#define ENGINE_SHARED_TRANSPORT_PIN_H

#include <base/hash.h>
#include <base/types.h>

enum class EModernTransportTrust
{
	INVALID,
	TOFU,
	WEBPKI,
	CERTIFICATE_HASH,
	SPKI_HASH,
};

/**
 * What the certificate of a QUIC or WebTransport server is checked against.
 */
class CModernTransportPin
{
public:
	EModernTransportTrust m_Trust;
	// The hash of the DER SubjectPublicKeyInfo of the certificate, which holds
	// the key of the server, for `SPKI_HASH`, the hash of the certificate for
	// `CERTIFICATE_HASH`. A second certificate hash is announced ahead of a
	// rotation.
	SHA256_DIGEST m_Fingerprint;
	SHA256_DIGEST m_NextFingerprint;
	bool m_HasNextFingerprint;
};

/**
 * @return Whether the address has the scheme of a QUIC or WebTransport link.
 */
bool IsModernTransportUrl(const char *pUrl);

/**
 * Reads a `ddnet+quic://`, `tw-0.7+quic://`, `ddnet+wt://` or `tw-0.7+wt://`
 * address and the pin in its fragment.
 *
 * @param pUrl The address.
 * @param pWebTransport Set to whether it is a WebTransport address.
 * @param pPin Set to the pin, TOFU for QUIC and Web PKI for WebTransport without a fragment.
 *
 * @return Whether it is such an address with a valid fragment.
 */
bool ParseModernTransportUrl(const char *pUrl, bool *pWebTransport, CModernTransportPin *pPin);

/**
 * Reads the fragment of a `ddnet-20+wss://` address the way a raw QUIC link
 * has it: TOFU without one, Web PKI with `#webpki`, the key of the server
 * with `#spki-sha256=`.
 *
 * @param pFragment The fragment with its `#`, or an empty string.
 * @param pPin Set to the pin.
 *
 * @return Whether it is a valid fragment for a secure websocket.
 */
bool ParseWebsocketPin(const char *pFragment, CModernTransportPin *pPin);

/**
 * Formats a `ddnet+quic://`, `tw-0.7+quic://`, `ddnet+wt://` or `tw-0.7+wt://`
 * address with its pin in the fragment, the way `ParseModernTransportUrl` reads
 * it back. A pin that is the default of the transport, TOFU for QUIC and Web PKI
 * for WebTransport, is left out, as servers register it.
 *
 * @param pBuffer The buffer to write to.
 * @param BufferSize The size of the buffer.
 * @param WebTransport Whether it is a WebTransport address.
 * @param Address The address, the 0.7 scheme for one with `NETTYPE_TW7`.
 * @param pHostname The name to write instead of the address, empty for none.
 * @param Pin The pin.
 *
 * @return Whether the pin can be written for the transport and the address fits the buffer.
 */
bool FormatModernTransportUrl(char *pBuffer, int BufferSize, bool WebTransport, const NETADDR &Address, const char *pHostname, const CModernTransportPin &Pin);

/**
 * Formats the fragment of a QUIC or WebTransport address, without the `#`.
 * It is empty for the default pin of the transport.
 *
 * @param pBuffer The buffer to write to.
 * @param BufferSize The size of the buffer.
 * @param WebTransport Whether it is the pin of a WebTransport address.
 * @param Pin The pin.
 *
 * @return Whether the pin can be written for the transport.
 */
bool FormatModernTransportFragment(char *pBuffer, int BufferSize, bool WebTransport, const CModernTransportPin &Pin);

/**
 * Reads one certificate hash, or two separated by a comma where the second is
 * the one announced ahead of a rotation, into a `CERTIFICATE_HASH` pin.
 *
 * @return Whether the hashes are valid and different.
 */
bool ParseCertificateHashes(const char *pValue, CModernTransportPin *pPin);

/**
 * Formats the certificate hashes of a pin the way `ParseCertificateHashes` reads them.
 *
 * @param pBuffer The buffer to write to.
 * @param BufferSize The size of the buffer.
 * @param Pin The pin.
 */
void FormatCertificateHashes(char *pBuffer, int BufferSize, const CModernTransportPin &Pin);

/**
 * @return Whether the URL is the `https://host:port/ddnet` a WebTransport server is reached at.
 */
bool ValidateWebTransportUrl(const char *pUrl, int Port);

/**
 * Formats the URL a browser opens a WebTransport session to.
 *
 * @return Whether the host name is valid for it and the URL fits the buffer.
 */
bool FormatWebTransportUrl(char *pBuffer, int BufferSize, const char *pHostname, int Port);

#endif
