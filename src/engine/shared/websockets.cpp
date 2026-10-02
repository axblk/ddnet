#if defined(CONF_WEBSOCKETS)

#include "websockets.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/config.h>
#include <engine/shared/network.h>
#include <engine/shared/protocol.h>
#include <engine/shared/ringbuffer.h>

#if defined(CONF_FAMILY_UNIX)
#include <arpa/inet.h>
#elif defined(CONF_FAMILY_WINDOWS)
#include <ws2tcpip.h>
#endif
#include <libwebsockets.h>

#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>

// Secure websockets need libwebsockets with TLS through OpenSSL, whose
// certificate and verification hooks this uses.
#if defined(LWS_WITH_TLS) && defined(LWS_OPENSSL_SUPPORT) && !defined(LWS_WITH_MBEDTLS)
#define WEBSOCKETS_TLS
#endif

// NOLINTBEGIN(readability-identifier-naming)
struct websocket_chunk
{
	size_t size;
	size_t read;
	NETADDR addr;
	unsigned char data[0];
};

// Client opens two connections for whatever reason
typedef CStaticRingBuffer<websocket_chunk, (MAX_CLIENTS * 2) * NET_CONN_BUFFERSIZE,
	CRingBufferBase::FLAG_RECYCLE>
	TRecvBuffer;
typedef CStaticRingBuffer<websocket_chunk, NET_CONN_BUFFERSIZE,
	CRingBufferBase::FLAG_RECYCLE>
	TSendBuffer;

struct per_session_data
{
	lws *wsi;
	NETADDR addr;
	TSendBuffer send_buffer;
};

struct context_data
{
	char bindaddr_str[NETADDR_MAXSTRSIZE];
	lws_context_creation_info creation_info;
	lws_context *context;
	std::map<NETADDR, per_session_data *> port_map;
	// Accepted connections from adoption until destruction. Unlike port_map, this
	// also covers connections still in the TLS/HTTP handshake phase, whose sockets
	// must be watched for the handshake to progress between select() timeouts.
	std::set<lws *> adopted_wsis;
	TRecvBuffer recv_buffer;
	int64_t accept_window_start;
	int accept_window_count;
};

// Accepting a connection completes its TLS handshake on the game loop's thread,
// before any DDNet-level limit can apply, so this is what keeps an
// unauthenticated peer from stalling the server's ticks by connecting in a loop.
// Far above what legitimate joins need, far below what a flood achieves.
static constexpr int WEBSOCKET_MAX_ACCEPTS_PER_SECOND = 50;
// Leave room in the caller's fd_set for the UDP sockets and other descriptors.
static constexpr int WEBSOCKET_MAX_CONNECTIONS = FD_SETSIZE > 16 ? FD_SETSIZE - 16 : 1;
#if defined(LWS_WITH_PEER_LIMITS)
// Bounds concurrent connections per peer. Only available when lws was built with
// peer limits, so the accept rate limit above cannot rely on it.
static constexpr unsigned short WEBSOCKET_MAX_CONNECTIONS_PER_IP = 20;
#endif

// Client has main, dummy and contact connections with IPv4 and IPv6
static context_data contexts[3 * 2];

// How a client checks the certificate of a secure websocket server, and what
// the server showed.
struct client_tls_target
{
	char host[256];
	// TOFU takes any key and leaves remembering it to the caller.
	CModernTransportPin pin;
	// What the last handshake showed. A connection to the same address is
	// used again without another handshake, so this stays when the pin
	// changes, and the key is checked against the new one.
	SHA256_DIGEST presented;
	bool has_presented;
	// The name Web PKI accepted the certificate of the last handshake for,
	// empty if it was not checked that way.
	char webpki_host[256];
	// Whether a handshake failed because the certificate was refused: not the
	// pinned key, or not valid for Web PKI.
	bool failed;
};

// By the address of the server without `NETTYPE_WEBSOCKET_TLS`. Entries stay,
// a connection points to its own for as long as it runs.
static std::map<NETADDR, client_tls_target> client_tls_targets;

// Connections are known by the address of the peer, which is the same for
// `ws` and `wss`.
static NETADDR websocket_session_addr(const NETADDR &addr)
{
	NETADDR session_addr = addr;
	session_addr.type &= ~NETTYPE_WEBSOCKET_TLS;
	return session_addr;
}

#if defined(WEBSOCKETS_TLS)
// What a DDNet client offers next to HTTP/1.1, as raw QUIC does, to be shown
// the identity key of the server rather than its Web PKI certificate.
static const unsigned char DDNET_ALPN[] = {'d', 'd', 'n', 'e', 't', '/', '1'};

// A server certificate as OpenSSL takes it.
struct tls_certificate
{
	X509 *leaf = nullptr;
	STACK_OF(X509) *chain = nullptr;
	EVP_PKEY *private_key = nullptr;

	tls_certificate() = default;
	tls_certificate(const tls_certificate &) = delete;
	tls_certificate &operator=(const tls_certificate &) = delete;
	~tls_certificate()
	{
		X509_free(leaf);
		sk_X509_pop_free(chain, X509_free);
		EVP_PKEY_free(private_key);
	}
};

// Each server handshake takes the certificates that are current when it
// starts: the identity one for DDNet clients where raw QUIC has one, the TLS
// certificate for everyone else.
static std::shared_ptr<const tls_certificate> server_certificate;
static std::shared_ptr<const tls_certificate> server_identity;
// Where a handshake keeps whether the client offered `DDNET_ALPN`.
static int ddnet_alpn_index = -1;

// The identity key of raw QUIC is stored as PKCS #8 version 2, with the public
// key after the private one, and the OpenSSL of Ubuntu 24.04 (3.0) reads only
// version 1. An Ed25519 key of version 2 starts like one of version 1, so the
// private key is read off where both have it.
static EVP_PKEY *ed25519_private_key_v2(const unsigned char *private_key, size_t private_key_size)
{
	// SEQUENCE { INTEGER 1, SEQUENCE { OID 1.3.101.112 }, OCTET STRING { OCTET STRING (32) }, [1] public key }
	static const unsigned char prefix[] = {0x02, 0x01, 0x01, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20};
	constexpr size_t seed_size = 32;
	if(private_key_size < 2 + sizeof(prefix) + seed_size || private_key_size - 2 > 0x7f || private_key[0] != 0x30 || private_key[1] != private_key_size - 2 || mem_comp(private_key + 2, prefix, sizeof(prefix)) != 0)
	{
		return nullptr;
	}
	return EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key + 2 + sizeof(prefix), seed_size);
}

static std::shared_ptr<const tls_certificate> parse_certificate(const unsigned char *chain, size_t chain_size, const unsigned char *private_key, size_t private_key_size, char *error, size_t error_size)
{
	auto certificate = std::make_shared<tls_certificate>();
	certificate->chain = sk_X509_new_null();
	const unsigned char *next = chain;
	const unsigned char *end = chain + chain_size;
	while(certificate->chain != nullptr && next < end)
	{
		X509 *x509 = d2i_X509(nullptr, &next, (long)(end - next));
		if(x509 == nullptr)
		{
			break;
		}
		if(certificate->leaf == nullptr)
		{
			certificate->leaf = x509;
		}
		else if(sk_X509_push(certificate->chain, x509) <= 0)
		{
			X509_free(x509);
			break;
		}
	}
	const unsigned char *key = private_key;
	if(next == end && certificate->leaf != nullptr)
	{
		certificate->private_key = d2i_AutoPrivateKey(nullptr, &key, (long)private_key_size);
		if(certificate->private_key == nullptr)
		{
			certificate->private_key = ed25519_private_key_v2(private_key, private_key_size);
		}
	}
	const bool valid = certificate->private_key != nullptr && X509_check_private_key(certificate->leaf, certificate->private_key) == 1;
	ERR_clear_error();
	if(!valid)
	{
		str_copy(error, "OpenSSL cannot serve the TLS certificate and key", error_size);
		return nullptr;
	}
	return certificate;
}

static int websocket_client_hello_callback(SSL *ssl, int *alert, void *arg)
{
	const unsigned char *alpn;
	size_t alpn_size;
	bool ddnet = false;
	// ProtocolNameList: a two byte length, then each name after its one byte length.
	if(SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, &alpn, &alpn_size) == 1 && alpn_size >= 2)
	{
		for(size_t offset = 2; offset < alpn_size && !ddnet; offset += 1 + alpn[offset])
		{
			ddnet = alpn[offset] == sizeof(DDNET_ALPN) && offset + 1 + sizeof(DDNET_ALPN) <= alpn_size && mem_comp(&alpn[offset + 1], DDNET_ALPN, sizeof(DDNET_ALPN)) == 0;
		}
	}
	SSL_set_ex_data(ssl, ddnet_alpn_index, ddnet ? ssl : nullptr);
	return SSL_CLIENT_HELLO_SUCCESS;
}

static int websocket_server_certificate_callback(SSL *ssl, void *arg)
{
	const bool ddnet = SSL_get_ex_data(ssl, ddnet_alpn_index) != nullptr;
	const std::shared_ptr<const tls_certificate> certificate = ddnet && server_identity != nullptr ? server_identity : server_certificate;
	if(certificate == nullptr)
	{
		return 0;
	}
	// Whatever key type the certificate before had, only this one is offered.
	SSL_certs_clear(ssl);
	return SSL_use_cert_and_key(ssl, certificate->leaf, certificate->private_key, certificate->chain, 1) == 1 ? 1 : 0;
}

static bool certificate_spki_sha256(X509 *certificate, SHA256_DIGEST *spki_sha256)
{
	unsigned char *spki = nullptr;
	const int spki_size = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(certificate), &spki);
	if(spki_size <= 0)
	{
		return false;
	}
	*spki_sha256 = sha256(spki, spki_size);
	OPENSSL_free(spki);
	return true;
}

// Remembers the key of the end-entity certificate, and where the key is what
// the server is trusted by, pinned or on first use, checks only that: the
// chain, the names and the dates do not count, the handshake proves that the
// server holds the key. Web PKI is left to OpenSSL.
static int websocket_check_server_key(client_tls_target &target, X509_STORE_CTX *x509_ctx, bool preverify_ok)
{
	const bool by_key = target.pin.m_Trust == EModernTransportTrust::SPKI_HASH || target.pin.m_Trust == EModernTransportTrust::TOFU;
	const bool end_entity = X509_STORE_CTX_get_error_depth(x509_ctx) == 0;
	if(end_entity)
	{
		X509 *certificate = X509_STORE_CTX_get_current_cert(x509_ctx);
		target.has_presented = certificate != nullptr && certificate_spki_sha256(certificate, &target.presented);
		target.webpki_host[0] = '\0';
	}
	if(!by_key)
	{
		// What OpenSSL refuses, the connection fails with. The end-entity
		// certificate comes last, with the result for the whole chain.
		if(!preverify_ok && X509_STORE_CTX_get_error(x509_ctx) != X509_V_OK)
		{
			target.failed = true;
		}
		else if(end_entity)
		{
			str_copy(target.webpki_host, target.host);
		}
		return 0;
	}
	if(!end_entity)
	{
		X509_STORE_CTX_set_error(x509_ctx, X509_V_OK);
		return 0;
	}
	if(!target.has_presented)
	{
		log_error("websockets", "Cannot read the key of the server certificate of '%s'", target.host);
		target.failed = true;
		X509_STORE_CTX_set_error(x509_ctx, X509_V_ERR_APPLICATION_VERIFICATION);
		return 1;
	}
	if(target.pin.m_Trust == EModernTransportTrust::SPKI_HASH && target.presented != target.pin.m_Fingerprint)
	{
		char presented_str[SHA256_MAXSTRSIZE];
		sha256_str(target.presented, presented_str, sizeof(presented_str));
		log_error("websockets", "server key does not match the pin (presented %s)", presented_str);
		target.failed = true;
		X509_STORE_CTX_set_error(x509_ctx, X509_V_ERR_APPLICATION_VERIFICATION);
		return 1;
	}
	X509_STORE_CTX_set_error(x509_ctx, X509_V_OK);
	return 0;
}
#endif

static lws_context *websocket_context(int socket)
{
	dbg_assert(socket >= 0 && socket < (int)std::size(contexts), "socket index invalid: %d", socket);
	lws_context *context = contexts[socket].context;
	dbg_assert(context != nullptr, "socket context not initialized: %d", socket);
	return context;
}

static void receive_chunk(context_data *ctx_data, per_session_data *pss, const void *in, size_t len)
{
	websocket_chunk *chunk = ctx_data->recv_buffer.Allocate(len + sizeof(websocket_chunk));
	dbg_assert(chunk != nullptr, "failed to allocate websocket receive buffer chunk of size %" PRIzu, len);
	chunk->size = len;
	chunk->read = 0;
	chunk->addr = pss->addr;
	// A secure connection to a server is known by its address with the scheme
	// it was opened with.
	if(lws_get_opaque_user_data(pss->wsi) != nullptr)
	{
		chunk->addr.type |= NETTYPE_WEBSOCKET_TLS;
	}
	mem_copy(&chunk->data[0], in, len);
}

static void sockaddr_to_netaddr_websocket(const sockaddr *src, socklen_t src_len, NETADDR *dst)
{
	*dst = NETADDR_ZEROED;
	if(src->sa_family == AF_INET && src_len >= (socklen_t)sizeof(sockaddr_in))
	{
		const sockaddr_in *src_in = (const sockaddr_in *)src;
		dst->type = NETTYPE_WEBSOCKET_IPV4;
		dst->port = htons(src_in->sin_port);
		static_assert(sizeof(dst->ip) >= sizeof(src_in->sin_addr.s_addr));
		mem_copy(dst->ip, &src_in->sin_addr.s_addr, sizeof(src_in->sin_addr.s_addr));
	}
	else if(src->sa_family == AF_INET6 && src_len >= (socklen_t)sizeof(sockaddr_in6))
	{
		const sockaddr_in6 *src_in6 = (const sockaddr_in6 *)src;
		dst->type = NETTYPE_WEBSOCKET_IPV6;
		dst->port = htons(src_in6->sin6_port);
		static_assert(sizeof(dst->ip) >= sizeof(src_in6->sin6_addr.s6_addr));
		mem_copy(dst->ip, &src_in6->sin6_addr.s6_addr, sizeof(src_in6->sin6_addr.s6_addr));
	}
	else
	{
		log_warn("websockets", "Cannot convert sockaddr of family %d", src->sa_family);
	}
}

static int websocket_protocol_callback(lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len)
{
	switch(reason)
	{
#if defined(WEBSOCKETS_TLS)
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS:
		// The certificate is handed to each handshake rather than loaded once,
		// so that a new one reaches the handshakes from then on.
		if(ddnet_alpn_index < 0)
		{
			ddnet_alpn_index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
		}
		SSL_CTX_set_client_hello_cb(static_cast<SSL_CTX *>(user), websocket_client_hello_callback, nullptr);
		SSL_CTX_set_cert_cb(static_cast<SSL_CTX *>(user), websocket_server_certificate_callback, nullptr);
		return 0;

	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
#if !defined(LWS_SSL_CLIENT_USE_OS_CA_CERTS)
		// Web PKI is checked against the certificate authorities of the system.
		SSL_CTX_set_default_verify_paths(static_cast<SSL_CTX *>(user));
#endif
		return 0;

	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
	{
		auto *target = static_cast<client_tls_target *>(lws_get_opaque_user_data(wsi));
		return target != nullptr ? websocket_check_server_key(*target, static_cast<X509_STORE_CTX *>(user), len != 0) : 0;
	}
#endif

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		log_error("websockets", "Connection failed: %s", in != nullptr ? static_cast<const char *>(in) : "unknown error");
		return 0;

	default:
		break;
	}

	per_session_data *pss = (per_session_data *)user;
	lws_context *context = lws_get_context(wsi);
	context_data *ctx_data = static_cast<context_data *>(lws_context_user(context));
	if(ctx_data == nullptr)
	{
		return 0;
	}
	switch(reason)
	{
	case LWS_CALLBACK_FILTER_NETWORK_CONNECTION:
	{
		// Issued right after accept(), before the TLS handshake is started, and
		// returning non-zero closes the connection there. Rate limit it, because
		// everything past this point runs on the game loop's thread.
		const int64_t now = time_get();
		if(now - ctx_data->accept_window_start > time_freq())
		{
			ctx_data->accept_window_start = now;
			ctx_data->accept_window_count = 0;
		}
		ctx_data->accept_window_count++;
		const auto *pArgs = static_cast<const lws_filter_network_conn_args *>(user);
		if(
#if !defined(CONF_FAMILY_WINDOWS)
			(pArgs != nullptr && pArgs->accept_fd >= FD_SETSIZE) ||
#endif
			ctx_data->accept_window_count > WEBSOCKET_MAX_ACCEPTS_PER_SECOND ||
			ctx_data->adopted_wsis.size() >= WEBSOCKET_MAX_CONNECTIONS)
		{
			return 1;
		}
		return 0;
	}

	case LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION:
	{
		if(g_Config.m_SvWebsocketOrigin[0] == '\0')
			return 0;
		char aOrigin[256];
		if(lws_hdr_copy(wsi, aOrigin, sizeof(aOrigin), WSI_TOKEN_ORIGIN) < 0 || str_comp(aOrigin, g_Config.m_SvWebsocketOrigin) != 0)
		{
			log_debug("websockets", "Rejected websocket connection with disallowed Origin");
			return 1;
		}
		return 0;
	}

	case LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED:
		// Fires once the accepted connection has its socket attached. From here on
		// every death of the wsi goes through LWS_CALLBACK_WSI_DESTROY, which is
		// not guaranteed for wsis that failed adoption before this point.
		if(lws_get_socket_fd(wsi) < 0 ||
#if !defined(CONF_FAMILY_WINDOWS)
			lws_get_socket_fd(wsi) >= FD_SETSIZE ||
#endif
			ctx_data->adopted_wsis.size() >= WEBSOCKET_MAX_CONNECTIONS)
		{
			return 1;
		}
		ctx_data->adopted_wsis.insert(wsi);
		return 0;

	case LWS_CALLBACK_ESTABLISHED:
		// Bit 0 of len marks a websocket running over an HTTP/2 stream (RFC 8441).
		// All streams of an HTTP/2 connection share its peer address, which is the
		// only identity the network layer knows a connection by, so they cannot be
		// told apart and are refused by returning non-zero, which closes them.
		if((len & 1) != 0)
		{
			return 1;
		}
		[[fallthrough]];
	case LWS_CALLBACK_WSI_CREATE:
	{
		if(pss == nullptr)
		{
			return 0;
		}
		sockaddr_storage peersockaddr;
		socklen_t peersockaddr_size = sizeof(peersockaddr);
		if(getpeername(lws_get_socket_fd(wsi), (sockaddr *)&peersockaddr, &peersockaddr_size) != 0)
		{
			log_warn("websockets", "Failed to determine peer address: %s", net_error_message().c_str());
			return 0;
		}
		NETADDR addr;
		sockaddr_to_netaddr_websocket((sockaddr *)&peersockaddr, peersockaddr_size, &addr);
		if(addr.type == NETTYPE_INVALID)
		{
			return 0;
		}

		pss->wsi = wsi;
		pss->addr = addr;
		pss->send_buffer.Init();
		ctx_data->port_map[addr] = pss;

		char addr_str[NETADDR_MAXSTRSIZE];
		net_addr_str(&addr, addr_str, sizeof(addr_str), true);
		log_trace("websockets", "Connection established with '%s'", addr_str);
		return 0;
	}

	case LWS_CALLBACK_CLIENT_CLOSED:
		[[fallthrough]];
	case LWS_CALLBACK_CLOSED:
	{
		// A rejected HTTP/WebSocket handshake can be closed before ESTABLISHED
		// initialized the per-session address and buffers.
		if(pss == nullptr || pss->wsi != wsi)
			return 0;
		char addr_str[NETADDR_MAXSTRSIZE];
		net_addr_str(&pss->addr, addr_str, sizeof(addr_str), true);
		log_trace("websockets", "Connection closed with '%s'", addr_str);

		static const unsigned char CLOSE_PACKET[] = {0x10, 0x0e, 0x00, 0x04};
		receive_chunk(ctx_data, pss, &CLOSE_PACKET, sizeof(CLOSE_PACKET));
		return 0;
	}

	case LWS_CALLBACK_WSI_DESTROY:
	{
		ctx_data->adopted_wsis.erase(wsi);
		if(pss == nullptr)
		{
			return 0;
		}
		pss->wsi = nullptr;
		ctx_data->port_map.erase(pss->addr);
		return 0;
	}

	case LWS_CALLBACK_CLIENT_WRITEABLE:
		[[fallthrough]];
	case LWS_CALLBACK_SERVER_WRITEABLE:
	{
		websocket_chunk *chunk = pss->send_buffer.First();
		if(chunk == nullptr)
		{
			return 0;
		}

		int chunk_len = chunk->size - chunk->read;
		int n = lws_write(wsi, &chunk->data[LWS_SEND_BUFFER_PRE_PADDING + chunk->read], chunk->size - chunk->read, LWS_WRITE_BINARY);
		if(n < 0)
		{
			return 1;
		}

		if(n < chunk_len)
		{
			chunk->read += n;
			lws_callback_on_writable(wsi);
			return 0;
		}

		pss->send_buffer.PopFirst();
		lws_callback_on_writable(wsi);
		return 0;
	}

	case LWS_CALLBACK_CLIENT_RECEIVE:
		[[fallthrough]];
	case LWS_CALLBACK_RECEIVE:
		receive_chunk(ctx_data, pss, in, len);
		return 0;

	default:
		return 0;
	}
}

static const lws_protocols protocols[] = {
	{"binary", websocket_protocol_callback, sizeof(per_session_data)},
	{"base64", websocket_protocol_callback, sizeof(per_session_data)},
	{nullptr, nullptr, 0}};

static LEVEL websocket_level_to_loglevel(int level)
{
	switch(level)
	{
	case LLL_ERR:
		return LEVEL_ERROR;
	case LLL_WARN:
		return LEVEL_WARN;
	case LLL_NOTICE:
	case LLL_INFO:
		return LEVEL_DEBUG;
	default:
		dbg_assert_failed("invalid log level: %d", level);
	}
}

static void websocket_log_callback(int level, const char *line)
{
	if((level == LLL_NOTICE || level == LLL_INFO) && !g_Config.m_DbgWebsockets)
	{
		return;
	}

	// Truncate duplicate timestamp from beginning and newline from end
	char line_truncated[4096]; // Longest log line length
	const char *line_time_end = str_find(line, "] ");
	dbg_assert(line_time_end != nullptr, "unexpected log format");
	str_copy(line_truncated, line_time_end + 2);
	const int length = str_length(line_truncated);
	if(line_truncated[length - 1] == '\n')
	{
		line_truncated[length - 1] = '\0';
	}
	if(line_truncated[length - 2] == '\r')
	{
		line_truncated[length - 2] = '\0';
	}
	log_log(websocket_level_to_loglevel(level), "websockets", "%s", line_truncated);
}

bool websocket_tls_available()
{
#if defined(WEBSOCKETS_TLS)
	return true;
#else
	return false;
#endif
}

bool websocket_set_server_certificate(const unsigned char *chain, size_t chain_size, const unsigned char *private_key, size_t private_key_size, char *error, size_t error_size)
{
#if defined(WEBSOCKETS_TLS)
	std::shared_ptr<const tls_certificate> certificate = parse_certificate(chain, chain_size, private_key, private_key_size, error, error_size);
	if(certificate == nullptr)
	{
		return false;
	}
	server_certificate = std::move(certificate);
	return true;
#else
	str_copy(error, "libwebsockets was built without TLS through OpenSSL", error_size);
	return false;
#endif
}

bool websocket_set_server_identity(const unsigned char *chain, size_t chain_size, const unsigned char *private_key, size_t private_key_size, char *error, size_t error_size)
{
#if defined(WEBSOCKETS_TLS)
	if(chain_size == 0)
	{
		server_identity = nullptr;
		return true;
	}
	std::shared_ptr<const tls_certificate> identity = parse_certificate(chain, chain_size, private_key, private_key_size, error, error_size);
	if(identity == nullptr)
	{
		return false;
	}
	server_identity = std::move(identity);
	return true;
#else
	str_copy(error, "libwebsockets was built without TLS through OpenSSL", error_size);
	return false;
#endif
}

bool websocket_server_tls()
{
#if defined(WEBSOCKETS_TLS)
	return server_certificate != nullptr;
#else
	return false;
#endif
}

void websocket_set_client_tls(const NETADDR *addr, const char *host, const CModernTransportPin &pin)
{
	client_tls_target &target = client_tls_targets[websocket_session_addr(*addr)];
	str_copy(target.host, host);
	target.pin = pin;
	target.failed = false;
}

bool websocket_client_tls_result(const NETADDR *addr, SHA256_DIGEST *spki_sha256, bool *failed)
{
	const auto it = client_tls_targets.find(websocket_session_addr(*addr));
	if(it == client_tls_targets.end())
	{
		return false;
	}
	const client_tls_target &target = it->second;
	// A connection that is used again was checked another way before, and
	// cannot stand in for Web PKI.
	const bool webpki_missing = target.pin.m_Trust == EModernTransportTrust::WEBPKI && target.has_presented && str_comp(target.webpki_host, target.host) != 0;
	*failed = target.failed || webpki_missing;
	*spki_sha256 = target.presented;
	return target.has_presented;
}

static int websocket_create(const NETADDR *bindaddr)
{
	static bool logging_set = false;
	if(!logging_set)
	{
		lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO, websocket_log_callback);
		logging_set = true;
	}

	// find free context
	int first_free = -1;
	for(int i = 0; i < (int)std::size(contexts); i++)
	{
		if(contexts[i].context == nullptr)
		{
			first_free = i;
			break;
		}
	}
	if(first_free == -1)
	{
		log_error("websockets", "Failed to create websocket: no free contexts available");
		return -1;
	}

	context_data *ctx_data = &contexts[first_free];
	ctx_data->port_map.clear();
	ctx_data->adopted_wsis.clear();
	ctx_data->accept_window_start = time_get();
	ctx_data->accept_window_count = 0;
	mem_zero(&ctx_data->creation_info, sizeof(ctx_data->creation_info));
	ctx_data->creation_info.options = LWS_SERVER_OPTION_FAIL_UPON_UNABLE_TO_BIND;
	if(bindaddr->type == NETTYPE_WEBSOCKET_IPV6)
	{
		// Set IPv6-only mode and socket option for IPv6 Websockets.
		ctx_data->creation_info.options |= LWS_SERVER_OPTION_IPV6_V6ONLY_VALUE | LWS_SERVER_OPTION_IPV6_V6ONLY_MODIFY;
	}
	net_addr_str(bindaddr, ctx_data->bindaddr_str, sizeof(ctx_data->bindaddr_str), false);
	if(ctx_data->bindaddr_str[0] == '[' && ctx_data->bindaddr_str[str_length(ctx_data->bindaddr_str) - 1] == ']')
	{
		// Bindaddr must not be enclosed in brackets for IPv6 Websockets.
		ctx_data->bindaddr_str[str_length(ctx_data->bindaddr_str) - 1] = '\0';
		mem_move(&ctx_data->bindaddr_str[0], &ctx_data->bindaddr_str[1], str_length(ctx_data->bindaddr_str) + 1);
	}
	ctx_data->creation_info.iface = ctx_data->bindaddr_str;
	ctx_data->creation_info.port = bindaddr->port;
	ctx_data->creation_info.protocols = protocols;
#if defined(LWS_WITH_TLS)
	// Only offer HTTP/1.1. Browsers that negotiate HTTP/2 run websockets over it as
	// streams (RFC 8441), which all share the peer address of their connection and
	// therefore cannot be told apart. ALPN is a TLS extension, and the field only
	// exists when lws was built with TLS support.
	ctx_data->creation_info.alpn = "http/1.1";
#endif
#if defined(LWS_WITH_PEER_LIMITS)
	ctx_data->creation_info.ip_limit_wsi = WEBSOCKET_MAX_CONNECTIONS_PER_IP;
#endif
#if defined(WEBSOCKETS_TLS)
	// Any websocket can open secure connections, and serves them where a
	// server certificate is set.
	ctx_data->creation_info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	if(server_certificate != nullptr)
	{
		ctx_data->creation_info.options |= LWS_SERVER_OPTION_CREATE_VHOST_SSL_CTX;
	}
#endif
	ctx_data->creation_info.gid = -1;
	ctx_data->creation_info.uid = -1;
	ctx_data->creation_info.user = ctx_data;

	ctx_data->context = lws_create_context(&ctx_data->creation_info);
	if(ctx_data->context == nullptr)
	{
		return -1;
	}
	ctx_data->recv_buffer.Init();
	return first_free;
}

static void websocket_destroy(int socket)
{
	lws_context *context = websocket_context(socket);
	lws_context_destroy(context);
	contexts[socket].context = nullptr;
}

static int websocket_recv(int socket, unsigned char *data, size_t maxsize, NETADDR *addr)
{
	lws_context *context = websocket_context(socket);
	const int service_result = lws_service(context, -1);
	if(service_result < 0)
	{
		return service_result;
	}

	context_data *ctx_data = static_cast<context_data *>(lws_context_user(context));
	websocket_chunk *chunk = ctx_data->recv_buffer.First();
	if(chunk == nullptr)
	{
		return 0;
	}

	if(maxsize >= chunk->size - chunk->read)
	{
		const int len = chunk->size - chunk->read;
		mem_copy(data, &chunk->data[chunk->read], len);
		*addr = chunk->addr;
		ctx_data->recv_buffer.PopFirst();
		return len;
	}
	else
	{
		mem_copy(data, &chunk->data[chunk->read], maxsize);
		*addr = chunk->addr;
		chunk->read += maxsize;
		return maxsize;
	}
}

static int websocket_send(int socket, const unsigned char *data, size_t size, const NETADDR *addr)
{
	lws_context *context = websocket_context(socket);
	context_data *ctx_data = static_cast<context_data *>(lws_context_user(context));
	const NETADDR session_addr = websocket_session_addr(*addr);
	per_session_data *pss = ctx_data->port_map[session_addr];
	if(pss == nullptr)
	{
		char addr_str[NETADDR_MAXSTRSIZE];
		net_addr_str(addr, addr_str, sizeof(addr_str), false);
		lws_client_connect_info ccinfo = {};
		ccinfo.context = context;
		ccinfo.address = addr_str;
		ccinfo.port = addr->port;
		ccinfo.protocol = protocols[0].name;
		if((addr->type & NETTYPE_WEBSOCKET_TLS) != 0)
		{
#if defined(WEBSOCKETS_TLS)
			client_tls_target &target = client_tls_targets[session_addr];
			// A new connection, which shows its certificate anew.
			target.has_presented = false;
			target.webpki_host[0] = '\0';
			if(target.host[0] == '\0')
			{
				// Without a name the certificate is checked for the address,
				// an IPv6 one without its brackets.
				const bool brackets = addr_str[0] == '[';
				str_truncate(target.host, sizeof(target.host), addr_str + brackets, str_length(addr_str) - 2 * brackets);
			}
			if(target.pin.m_Trust == EModernTransportTrust::INVALID)
			{
				target.pin.m_Trust = EModernTransportTrust::WEBPKI;
			}
			const bool by_key = target.pin.m_Trust == EModernTransportTrust::SPKI_HASH || target.pin.m_Trust == EModernTransportTrust::TOFU;
			ccinfo.ssl_connection = LCCSCF_USE_SSL | (by_key ? LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK : 0);
			ccinfo.host = target.host;
			ccinfo.alpn = "ddnet/1,http/1.1";
			ccinfo.opaque_user_data = &target;
#else
			log_error("websockets", "Cannot open wss: libwebsockets was built without TLS through OpenSSL");
			return -1;
#endif
		}
		lws *wsi = lws_client_connect_via_info(&ccinfo);
		if(wsi == nullptr)
		{
			return -1;
		}
		lws_service(context, -1);
		pss = ctx_data->port_map[session_addr];
		if(pss == nullptr)
		{
			return -1;
		}
	}

	const size_t chunk_size = size + sizeof(websocket_chunk) + LWS_SEND_BUFFER_PRE_PADDING + LWS_SEND_BUFFER_POST_PADDING;
	websocket_chunk *chunk = pss->send_buffer.Allocate(chunk_size);
	dbg_assert(chunk != nullptr, "failed to allocate websocket send buffer chunk of size %" PRIzu, size);
	mem_zero(chunk, chunk_size);
	chunk->size = size;
	chunk->read = 0;
	chunk->addr = pss->addr;
	mem_copy(&chunk->data[LWS_SEND_BUFFER_PRE_PADDING], data, size);
	lws_callback_on_writable(pss->wsi);
	lws_service(context, -1);
	return size;
}

static int websocket_fd_set(int socket, void *set_untyped)
{
	fd_set *set = static_cast<fd_set *>(set_untyped);
	lws_context *context = websocket_context(socket);
	lws_service(context, -1);

	context_data *ctx_data = static_cast<context_data *>(lws_context_user(context));
	int max = 0;
	for(lws *wsi : ctx_data->adopted_wsis)
	{
		const int fd = lws_get_socket_fd(wsi);
		if(fd < 0
#if !defined(CONF_FAMILY_WINDOWS)
			|| fd >= FD_SETSIZE
#endif
		)
		{
			continue;
		}
		max = std::max(fd, max);
		FD_SET(fd, set);
	}
	for(const auto &[_, pss] : ctx_data->port_map)
	{
		if(pss == nullptr)
		{
			continue;
		}
		const int fd = lws_get_socket_fd(pss->wsi);
		if(fd < 0
#if !defined(CONF_FAMILY_WINDOWS)
			|| fd >= FD_SETSIZE
#endif
		)
		{
			continue;
		}
		max = std::max(fd, max);
		FD_SET(fd, set);
	}
	return max;
}

static int websocket_fd_get(int socket, void *set_untyped)
{
	fd_set *set = static_cast<fd_set *>(set_untyped);
	lws_context *context = websocket_context(socket);
	lws_service(context, -1);

	context_data *ctx_data = static_cast<context_data *>(lws_context_user(context));
	if(ctx_data->recv_buffer.First() != nullptr)
	{
		// Packets already consumed by lws_service are no longer readable on the
		// socket, so select cannot see them and they would never be processed.
		return 1;
	}
	for(lws *wsi : ctx_data->adopted_wsis)
	{
		const int fd = lws_get_socket_fd(wsi);
		if(fd >= 0
#if !defined(CONF_FAMILY_WINDOWS)
			&& fd < FD_SETSIZE
#endif
			&& FD_ISSET(fd, set))
		{
			return 1;
		}
	}
	for(const auto &[_, pss] : ctx_data->port_map)
	{
		if(pss == nullptr)
		{
			continue;
		}
		const int fd = lws_get_socket_fd(pss->wsi);
		if(fd >= 0
#if !defined(CONF_FAMILY_WINDOWS)
			&& fd < FD_SETSIZE
#endif
			&& FD_ISSET(fd, set))
		{
			return 1;
		}
	}
	return 0;
}
// NOLINTEND(readability-identifier-naming)

// The transport installs itself, so that linking it is all it takes to be able
// to open a websocket and nothing has to remember to ask for it.
static const NETWEBSOCKET WEBSOCKET_TRANSPORT = {
	websocket_create,
	websocket_destroy,
	websocket_recv,
	websocket_send,
	websocket_fd_set,
	websocket_fd_get,
};

static const struct CWebsocketTransport
{
	CWebsocketTransport() { net_websocket_transport(&WEBSOCKET_TRANSPORT); }
} WEBSOCKET_TRANSPORT_INSTALLER;

#endif
