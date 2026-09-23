#ifndef ENGINE_SHARED_TRANSPORT_PIN_H
#define ENGINE_SHARED_TRANSPORT_PIN_H

#include <base/hash.h>

enum class EModernTransportTrust
{
	INVALID,
	TOFU,
	WEBPKI,
	CERTIFICATE_HASH,
	IDENTITY,
};

/**
 * What the certificate of a QUIC or WebTransport server is checked against.
 */
class CModernTransportPin
{
public:
	EModernTransportTrust m_Trust;
	// The fingerprint of the identity key for `IDENTITY`, the hash of the
	// certificate for `CERTIFICATE_HASH`. A second certificate hash is announced
	// ahead of a rotation.
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
 * Reads one certificate hash, or two separated by a comma where the second is
 * the one announced ahead of a rotation, into a `CERTIFICATE_HASH` pin.
 *
 * @return Whether the hashes are valid and different.
 */
bool ParseCertificateHashes(const char *pValue, CModernTransportPin *pPin);

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
