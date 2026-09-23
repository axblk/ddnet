#ifndef ENGINE_SHARED_QUIC_TRANSPORT_H
#define ENGINE_SHARED_QUIC_TRANSPORT_H

#include <base/hash.h>
#include <base/net.h>

#include <engine/shared/transport_pin.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class CQuicSessionId
{
	uint64_t m_Value = 0;

public:
	constexpr CQuicSessionId() = default;
	explicit constexpr CQuicSessionId(uint64_t Value) :
		m_Value(Value)
	{
	}

	constexpr bool IsValid() const { return m_Value != 0; }
	constexpr uint64_t Value() const { return m_Value; }
	constexpr bool operator==(const CQuicSessionId &Other) const = default;
};

struct CQuicMessage
{
	CQuicSessionId m_Session;
	NETADDR m_PeerAddress;
	bool m_Vital;
	const void *m_pData;
	int m_DataSize;
};

enum class EQuicEventType
{
	CONNECTED,
	MASTER_CHALLENGE,
	MESSAGE,
	MAP_HEADER,
	MAP_DATA,
	MAP_END,
	MAP_FAILED,
	PEER_MIGRATED,
	DISCONNECTED,
};

struct CQuicEvent
{
	EQuicEventType m_Type;
	CQuicMessage m_Message;
	const char *m_pReason;
	bool m_Sixup = false;
	bool m_WebTransport = false;
};

enum class EQuicConnectFailure
{
	NONE,
	NETWORK,
	// The certificate of the server did not match the pin.
	PIN,
	PROTOCOL,
};

/**
 * The TLS certificate of a server as `sv_tls_cert`, `sv_tls_cert_next` and
 * `sv_tls_key` name it, read and checked once for QUIC, WebTransport and
 * secure websockets alike.
 */
class CTlsServerCertificate
{
public:
	// The DER certificates of the chain, the end-entity one first, one after the other.
	std::vector<unsigned char> m_vChain;
	// The private key in DER: PKCS#8, SEC1 or PKCS#1.
	std::vector<unsigned char> m_vPrivateKey;
	SHA256_DIGEST m_Sha256 = {};
	// The certificate announced ahead of a rotation.
	std::optional<SHA256_DIGEST> m_NextSha256;
	// The SHA-256 of the DER SubjectPublicKeyInfo of the end-entity certificate,
	// which a key pin checks.
	SHA256_DIGEST m_SpkiSha256 = {};
	// When the end-entity certificate expires, in seconds since 1970.
	int64_t m_NotAfter = 0;

	/**
	 * Reads a certificate, PEM or DER, its key and the certificate announced
	 * next, and checks that the key belongs to the certificate. Only builds
	 * with QUIC can read them.
	 *
	 * @param pCertificatePath The certificate or chain.
	 * @param pNextCertificatePath The certificate announced ahead of a rotation, may be empty.
	 * @param pPrivateKeyPath The key of the certificate.
	 * @param pError Receives why they cannot be used.
	 * @param ErrorSize The size of the error buffer.
	 *
	 * @return Whether they can be used. On failure this is left as it was.
	 */
	bool Load(const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, char *pError, int ErrorSize);
};

/**
 * The QUIC and WebTransport endpoint of a client or server.
 *
 * Natively this is raw QUIC, and on a server WebTransport as well, driven by
 * the UDP socket of the legacy transport. In the browser it is a WebTransport
 * client.
 */
class CQuicTransport
{
	class CImpl;
	std::unique_ptr<CImpl> m_pImpl;
	SHA256_DIGEST m_CertificateSha256 = {};
	SHA256_DIGEST m_NextCertificateSha256 = {};
	bool m_HasCertificateSha256 = false;
	bool m_HasNextCertificateSha256 = false;
	EQuicConnectFailure m_ConnectFailure = EQuicConnectFailure::NONE;
	char m_aError[256] = {};

	void SetServerCertificateHashes(const CTlsServerCertificate &Certificate);

public:
	CQuicTransport();
	~CQuicTransport();
	CQuicTransport(const CQuicTransport &) = delete;
	CQuicTransport &operator=(const CQuicTransport &) = delete;

	/**
	 * Whether this build has native QUIC, and serves WebTransport with it.
	 */
	static bool IsCompiled();
	/**
	 * Whether this client can dial WebTransport, which only a browser that
	 * supports it can.
	 */
	static bool IsWebTransportClientAvailable();

	/**
	 * Starts a server.
	 *
	 * @param RawQuic Serve raw QUIC.
	 * @param WebTransport Serve WebTransport.
	 * @param pCertificate The TLS certificate, a managed one if null.
	 * @param pIdentityPath Where the identity key and the managed certificate
	 * are kept. Raw QUIC is served with a certificate of the identity key, or
	 * with the TLS certificate if this is empty.
	 */
	bool StartServer(bool RawQuic, bool WebTransport, const CTlsServerCertificate *pCertificate, const char *pIdentityPath);
	bool MaybeRotateManagedCertificate(bool *pRotated);
	/**
	 * Replaces the TLS certificate of a running server for the handshakes from
	 * now on. Connections that run keep theirs, and on failure the server keeps
	 * the certificate it has. The certificate of the identity key stays.
	 *
	 * Only a server started with a certificate of its own can do this; a
	 * managed one is rotated by the server itself.
	 *
	 * @param Certificate The new certificate.
	 *
	 * @return Whether the certificate was replaced, `ErrorString` says why not.
	 */
	bool ReloadServerCertificate(const CTlsServerCertificate &Certificate);
	/**
	 * Connects to a server, over raw QUIC natively and over WebTransport in the
	 * browser.
	 *
	 * @param Address The address of the server.
	 * @param pServerName The name the TLS certificate is checked for, the address if empty.
	 * @param Pin What the certificate is checked against.
	 * @param Sixup Whether to speak the 0.7 game protocol.
	 */
	bool StartClient(const NETADDR &Address, const char *pServerName, const CModernTransportPin &Pin, bool Sixup);
	bool IsRunning() const;
	bool Send(CQuicSessionId Session, const void *pData, int DataSize, bool Vital);
	bool SetMap(uint32_t MapId, const char *pName, uint32_t Crc, const SHA256_DIGEST &Sha256, const void *pData, size_t DataSize);
	bool SendMap(CQuicSessionId Session, uint32_t MapId);
	bool IssueResume(CQuicSessionId Session, uint64_t LogicalSessionId, const unsigned char *pToken, size_t TokenSize);
	bool Reconnect(CQuicSessionId Session);
	bool Close(CQuicSessionId Session, const char *pReason);
	bool Poll(CQuicEvent &Event);
	bool FeedUdp(const NETADDR *pAddress, const void *pData, int DataSize);
	int PollUdpSend(NETADDR *pAddress, unsigned char **ppData);
	bool SetLegacyPeer(const NETADDR *pAddress, bool Known);
	int64_t NextTimeoutMicroseconds() const;
	void LocalAddressChanged();
	void Shutdown();
	const char *ErrorString() const { return m_aError; }
	EQuicConnectFailure ConnectFailure() const { return m_ConnectFailure; }
	const SHA256_DIGEST *CertificateSha256() const { return m_HasCertificateSha256 ? &m_CertificateSha256 : nullptr; }
	const SHA256_DIGEST *NextCertificateSha256() const { return m_HasNextCertificateSha256 ? &m_NextCertificateSha256 : nullptr; }
	/**
	 * The SHA-256 of the DER SubjectPublicKeyInfo of the certificate a server
	 * serves raw QUIC with, which is what a raw QUIC link pins.
	 */
	std::optional<SHA256_DIGEST> RawQuicSpkiSha256() const;
	/**
	 * The certificate of the identity key a server serves raw QUIC with, for
	 * secure websockets to show DDNet clients the same key.
	 *
	 * @param pIdentity Set to the certificate and the key.
	 *
	 * @return Whether raw QUIC has one, it has the TLS certificate where not.
	 */
	bool RawQuicIdentity(CTlsServerCertificate *pIdentity) const;
};

#endif
