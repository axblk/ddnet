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
 * Reads the fragment of a QUIC, WebTransport or `wss://` address: TOFU without
 * one for QUIC and wss, Web PKI without one for WebTransport, Web PKI with
 * `webpki`, the key of the server with `spki-sha256=` and, for WebTransport,
 * the certificates with `cert-sha256=`.
 *
 * @param pFragment The fragment without its `#`, or an empty string.
 * @param WebTransport Whether it is the fragment of a WebTransport address.
 * @param pPin Set to the pin.
 *
 * @return Whether it is a valid fragment for the transport.
 */
bool ParseModernTransportFragment(const char *pFragment, bool WebTransport, CModernTransportPin *pPin);

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

#endif
