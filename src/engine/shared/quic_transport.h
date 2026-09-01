#ifndef ENGINE_SHARED_QUIC_TRANSPORT_H
#define ENGINE_SHARED_QUIC_TRANSPORT_H

#include <base/hash.h>
#include <base/net.h>

#include <engine/shared/transport_pin.h>

#include <cstdint>
#include <memory>

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
	IDENTITY,
	PROTOCOL,
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
	SHA256_DIGEST m_IdentityFingerprint = {};
	bool m_HasCertificateSha256 = false;
	bool m_HasNextCertificateSha256 = false;
	bool m_HasIdentityFingerprint = false;
	EQuicConnectFailure m_ConnectFailure = EQuicConnectFailure::NONE;
	char m_aError[256] = {};

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
	 * @param pCertificatePath The TLS certificate, a managed one if empty.
	 * @param pNextCertificatePath The certificate announced ahead of a rotation, may be empty.
	 * @param pPrivateKeyPath The key of the TLS certificate, empty for a managed one.
	 * @param pIdentityPath Where the identity key and the managed certificate are kept.
	 * @param pCidKey The material the XDP filter service publishes. With it, connection
	 * IDs carry a tag the filter can check, which is what lets it tell an established
	 * connection from a spoof.
	 * @param CidKeySize The size of `pCidKey`, 0 when no filter is in play.
	 */
	bool StartServer(bool RawQuic, bool WebTransport, const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, const char *pIdentityPath, const unsigned char *pCidKey, int CidKeySize);
	/**
	 * Adopts a rotated connection ID key. Connections running under the previous one
	 * keep their ids.
	 *
	 * @param pCidKey The material the XDP filter service publishes.
	 * @param CidKeySize The size of `pCidKey`.
	 */
	bool UpdateCidKey(const unsigned char *pCidKey, int CidKeySize);
	bool MaybeRotateManagedCertificate(bool *pRotated);
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
	 * The fingerprint of the identity key of a server serving raw QUIC.
	 */
	const SHA256_DIGEST *IdentityFingerprint() const { return m_HasIdentityFingerprint ? &m_IdentityFingerprint : nullptr; }
};

#endif
