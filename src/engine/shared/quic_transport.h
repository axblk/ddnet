#ifndef ENGINE_SHARED_QUIC_TRANSPORT_H
#define ENGINE_SHARED_QUIC_TRANSPORT_H

#include "server_identity.h"

#include <base/hash.h>
#include <base/net.h>

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

struct CQuicTransportMetrics
{
	uint64_t m_ConnectAttempts = 0;
	uint64_t m_ConnectFailuresNetwork = 0;
	uint64_t m_ConnectFailuresIdentity = 0;
	uint64_t m_ConnectFailuresProtocol = 0;
	uint64_t m_Fallbacks = 0;
	uint64_t m_LastHandshakeMilliseconds = 0;
	uint64_t m_ReliableSent = 0;
	uint64_t m_DatagramsSent = 0;
	uint64_t m_ReliableReceived = 0;
	uint64_t m_DatagramsReceived = 0;
	uint64_t m_ReliableQueueFull = 0;
	uint64_t m_DatagramsDropped = 0;
	uint64_t m_CommandQueueHighWater = 0;
	uint64_t m_PathChanges = 0;
	uint64_t m_Connections = 0;
	uint64_t m_Disconnections = 0;
	uint64_t m_BytesSent = 0;
	uint64_t m_BytesReceived = 0;
	uint64_t m_MapTransfersReceived = 0;
	uint64_t m_MapBytesReceived = 0;
	uint64_t m_MapTransfersFailed = 0;
	uint64_t m_ResumeSendDrops = 0;
};

enum class EQuicConnectFailure
{
	NONE,
	NETWORK,
	IDENTITY,
	PROTOCOL,
};

class CQuicTransport
{
	class CImpl;
	std::unique_ptr<CImpl> m_pImpl;
	CQuicTransportMetrics m_Metrics;
	SHA256_DIGEST m_CertificateSha256 = {};
	SHA256_DIGEST m_NextCertificateSha256 = {};
	CServerIdentityBinding m_ServerIdentity = {};
	bool m_HasCertificateSha256 = false;
	bool m_HasNextCertificateSha256 = false;
	bool m_HasServerIdentity = false;
	bool m_ClientMode = false;
	int64_t m_ConnectStartTime = 0;
	EQuicConnectFailure m_ConnectFailure = EQuicConnectFailure::NONE;
	char m_aError[256] = {};

public:
	CQuicTransport();
	~CQuicTransport();
	CQuicTransport(const CQuicTransport &) = delete;
	CQuicTransport &operator=(const CQuicTransport &) = delete;

	static bool IsCompiled();
	// Serving WebTransport and dialing it are two different builds. The Rust
	// endpoint answers a WebTransport handshake wherever QUIC is compiled in,
	// but only a browser brings a WebTransport client, so the native client
	// has nothing to dial it with.
	static bool IsWebTransportServerCompiled();
	static bool IsWebTransportClientCompiled();
	static bool IsWebTransportClientAvailable();
	bool StartServer(const char *pLocalAddress, bool RawQuic, bool WebTransport, const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, const char *pIdentityPath);
	bool MaybeRotateManagedCertificate(bool *pRotated);
	bool StartClient(const char *pBindAddress, const char *pServerAddress, const char *pServerName, const char *pCertificatePath, bool Sixup);
	bool StartClientWebPki(const char *pBindAddress, const char *pServerAddress, const char *pServerName, bool Sixup);
	bool StartClientSha256(const char *pBindAddress, const char *pServerAddress, const char *pServerName, SHA256_DIGEST CertificateSha256, const SHA256_DIGEST *pNextCertificateSha256, bool Sixup);
	bool StartClientIdentity(const char *pBindAddress, const char *pServerAddress, const char *pServerName, SHA256_DIGEST IdentityFingerprint, bool Sixup);
	bool StartClientTofu(const char *pBindAddress, const char *pServerAddress, const char *pServerName, bool Sixup);
	bool StartWebTransportClient(const char *pUrl, const NETADDR *pPeerAddress, bool UseCertificateHashes, SHA256_DIGEST CertificateSha256, const SHA256_DIGEST *pNextCertificateSha256, bool Sixup);
	bool IsRunning() const;
	bool Send(CQuicSessionId Session, const void *pData, int DataSize, bool Vital);
	bool SetMap(uint32_t MapId, const char *pName, uint32_t Crc, const unsigned char *pSha256, const void *pData, size_t DataSize);
	bool SendMap(CQuicSessionId Session, uint32_t MapId);
	bool IssueResume(CQuicSessionId Session, uint64_t LogicalSessionId, const unsigned char *pToken, size_t TokenSize);
	bool Reconnect(CQuicSessionId Session);
	bool Close(CQuicSessionId Session, const char *pReason);
	bool Poll(CQuicEvent &Event);
	bool FeedUdp(const NETADDR *pAddress, const void *pData, int DataSize);
	int PollUdpSend(NETADDR *pAddress, unsigned char **ppData);
	bool SetLegacyPeer(const NETADDR *pAddress, bool Known);
	uint64_t RawDropCount() const;
	int64_t NextTimeoutMicroseconds() const;
	void LocalAddressChanged();
	void RecordFallback() { m_Metrics.m_Fallbacks++; }
	void Shutdown();
	const char *ErrorString() const { return m_aError; }
	EQuicConnectFailure ConnectFailure() const { return m_ConnectFailure; }
	const SHA256_DIGEST *CertificateSha256() const { return m_HasCertificateSha256 ? &m_CertificateSha256 : nullptr; }
	const SHA256_DIGEST *NextCertificateSha256() const { return m_HasNextCertificateSha256 ? &m_NextCertificateSha256 : nullptr; }
	const CServerIdentityBinding *ServerIdentity() const { return m_HasServerIdentity ? &m_ServerIdentity : nullptr; }
	const CQuicTransportMetrics &Metrics() const { return m_Metrics; }
};

#endif
