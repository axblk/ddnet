#ifndef ENGINE_SHARED_WEBSOCKETS_H
#define ENGINE_SHARED_WEBSOCKETS_H

#include <base/hash.h>
#include <base/types.h>

#include <engine/shared/transport_pin.h>

#include <cstddef>

// The transport itself is reached through the table `base` is handed, see
// `NETWEBSOCKET` in `base/net.h`. What TLS takes, the certificate of a server
// and how a client checks the one of a server, is set here.
// NOLINTBEGIN(readability-identifier-naming)

/**
 * @return Whether this build can open and serve secure websockets, which takes
 * libwebsockets with TLS through OpenSSL.
 */
bool websocket_tls_available();

/**
 * Sets the certificate secure websockets are served with. Websockets opened
 * from then on serve `wss` instead of `ws`, and those that serve already
 * hand it to the handshakes from now on; connections that run keep theirs.
 * On failure the certificate stays as it was.
 *
 * @param chain The DER certificates of the chain, the end-entity one first, one after the other.
 * @param chain_size The size of the chain.
 * @param private_key The private key in DER: PKCS#8, SEC1 or PKCS#1.
 * @param private_key_size The size of the private key.
 * @param error Receives why the certificate cannot be used.
 * @param error_size The size of the error buffer.
 *
 * @return Whether the certificate is used.
 */
bool websocket_set_server_certificate(const unsigned char *chain, size_t chain_size, const unsigned char *private_key, size_t private_key_size, char *error, size_t error_size);

/**
 * Sets the certificate of the identity key raw QUIC is served with, which
 * secure websockets show DDNet clients instead of the TLS certificate, so that
 * they check the same key on both. Browsers keep getting the TLS certificate.
 *
 * @param chain The DER certificate, nothing to show the TLS certificate to everyone.
 * @param chain_size The size of the certificate.
 * @param private_key The private key in DER.
 * @param private_key_size The size of the private key.
 * @param error Receives why the certificate cannot be used.
 * @param error_size The size of the error buffer.
 *
 * @return Whether the certificate is used.
 */
bool websocket_set_server_identity(const unsigned char *chain, size_t chain_size, const unsigned char *private_key, size_t private_key_size, char *error, size_t error_size);

/**
 * @return Whether websockets are served as `wss`.
 */
bool websocket_server_tls();

/**
 * Says how the certificate of a secure websocket server is checked by the
 * connections opened to it from then on.
 *
 * @param addr The address of the server, `NETTYPE_WEBSOCKET_TLS` or not.
 * @param host The name the certificate is checked for and sent as SNI.
 * @param pin Web PKI for the name, the key of the server, or any key for TOFU,
 * which `websocket_client_tls_result` then tells.
 */
void websocket_set_client_tls(const NETADDR *addr, const char *host, const CModernTransportPin &pin);

/**
 * What the server showed in the last handshake of a secure websocket.
 *
 * @param addr The address of the server.
 * @param spki_sha256 Set to the SHA-256 of the DER SubjectPublicKeyInfo of its certificate.
 * @param failed Set to whether a handshake failed because the certificate was
 * refused, as not the pinned key or not valid for Web PKI.
 *
 * @return Whether the server showed a certificate.
 */
bool websocket_client_tls_result(const NETADDR *addr, SHA256_DIGEST *spki_sha256, bool *failed);

// NOLINTEND(readability-identifier-naming)

#endif // ENGINE_SHARED_WEBSOCKETS_H
