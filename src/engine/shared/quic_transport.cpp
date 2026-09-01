#include "quic_transport.h"

// The browser has its own implementation in quic_transport_browser.cpp.
#if !defined(CONF_PLATFORM_EMSCRIPTEN)

#if defined(CONF_QUIC)
#include <base/io.h>
#include <base/mem.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/shared/quic.h>

#include <cstdlib>
#include <exception>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	rust::Slice<const uint8_t> Slice(const std::vector<uint8_t> &vData)
	{
		return {vData.data(), vData.size()};
	}

	rust::Slice<const uint8_t> Slice(const rust::Vec<uint8_t> &vData)
	{
		return {vData.data(), vData.size()};
	}

	bool ReadFile(const char *pPath, std::vector<uint8_t> &vData, char *pError, size_t ErrorSize)
	{
		IOHANDLE File = io_open(pPath, IOFLAG_READ);
		void *pData;
		unsigned Size;
		if(!File || !io_read_all(File, &pData, &Size))
		{
			if(File)
				io_close(File);
			str_format(pError, ErrorSize, "could not read '%s'", pPath);
			return false;
		}
		io_close(File);
		vData.assign(static_cast<uint8_t *>(pData), static_cast<uint8_t *>(pData) + Size);
		free(pData);
		return true;
	}

	SHA256_DIGEST LeafCertificateSha256(rust::Slice<const uint8_t> Certificate)
	{
		const rust::Vec<uint8_t> Der = ModernQuic::quic_leaf_certificate_der(Certificate);
		return sha256(Der.data(), Der.size());
	}

	ModernQuic::QuicPin QuicPin(EModernTransportTrust Trust)
	{
		switch(Trust)
		{
		case EModernTransportTrust::TOFU: return ModernQuic::QuicPin::Tofu;
		case EModernTransportTrust::WEBPKI: return ModernQuic::QuicPin::WebPki;
		case EModernTransportTrust::CERTIFICATE_HASH: return ModernQuic::QuicPin::Sha256;
		case EModernTransportTrust::IDENTITY:
		case EModernTransportTrust::INVALID: break;
		}
		return ModernQuic::QuicPin::Identity;
	}
}

class CQuicTransport::CImpl
{
public:
	rust::Box<ModernQuic::QuicEndpoint> m_Endpoint;
	bool m_Client;
	ModernQuic::QuicEvent m_Event = {};
	ModernQuic::UdpDatagram m_UdpDatagram = {};
	std::unordered_map<uint64_t, NETADDR> m_PeerAddresses;
	std::string m_ManagedIdentityPath;
	int64_t m_ManagedCertificateRotateAt = 0;

	CImpl(rust::Box<ModernQuic::QuicEndpoint> Endpoint, bool Client) :
		m_Endpoint(std::move(Endpoint)), m_Client(Client)
	{
	}

	bool TranslateEvent(CQuicEvent &Event, EQuicConnectFailure *pFailure);
};

bool CQuicTransport::CImpl::TranslateEvent(CQuicEvent &Event, EQuicConnectFailure *pFailure)
{
	const CQuicSessionId Session(m_Event.session_id);
	NETADDR PeerAddress = {};
	if(m_Event.kind == ModernQuic::QuicEventKind::Connected || m_Event.kind == ModernQuic::QuicEventKind::PeerMigrated)
	{
		if(net_addr_from_str(&PeerAddress, std::string(m_Event.detail).c_str()) != 0)
			return false;
		m_PeerAddresses[Session.Value()] = PeerAddress;
	}
	else if(const auto It = m_PeerAddresses.find(Session.Value()); It != m_PeerAddresses.end())
		PeerAddress = It->second;

	Event = {EQuicEventType::MESSAGE, {Session, PeerAddress, true, m_Event.payload.data(), static_cast<int>(m_Event.payload.size())}, nullptr};
	Event.m_Sixup = m_Event.sixup;
	Event.m_WebTransport = m_Event.webtransport;
	switch(m_Event.kind)
	{
	case ModernQuic::QuicEventKind::Connected: Event.m_Type = EQuicEventType::CONNECTED; return true;
	case ModernQuic::QuicEventKind::PeerMigrated: Event.m_Type = EQuicEventType::PEER_MIGRATED; return true;
	case ModernQuic::QuicEventKind::MasterChallenge: Event.m_Type = EQuicEventType::MASTER_CHALLENGE; return true;
	case ModernQuic::QuicEventKind::Control: return true;
	case ModernQuic::QuicEventKind::Datagram: Event.m_Message.m_Vital = false; return true;
	case ModernQuic::QuicEventKind::MapHeader: Event.m_Type = EQuicEventType::MAP_HEADER; return true;
	case ModernQuic::QuicEventKind::MapData: Event.m_Type = EQuicEventType::MAP_DATA; return true;
	case ModernQuic::QuicEventKind::MapEnd: Event.m_Type = EQuicEventType::MAP_END; return true;
	case ModernQuic::QuicEventKind::MapFailed:
		Event.m_Type = EQuicEventType::MAP_FAILED;
		Event.m_pReason = m_Event.detail.c_str();
		return true;
	case ModernQuic::QuicEventKind::Disconnected:
	case ModernQuic::QuicEventKind::ConnectFailedNetwork:
	case ModernQuic::QuicEventKind::ConnectFailedIdentity:
	case ModernQuic::QuicEventKind::ConnectFailedProtocol:
		if(m_Event.kind == ModernQuic::QuicEventKind::Disconnected)
			m_PeerAddresses.erase(Session.Value());
		else if(m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedNetwork)
			*pFailure = EQuicConnectFailure::NETWORK;
		else if(m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedIdentity)
			*pFailure = EQuicConnectFailure::IDENTITY;
		else
			*pFailure = EQuicConnectFailure::PROTOCOL;
		Event.m_Type = EQuicEventType::DISCONNECTED;
		Event.m_pReason = m_Event.detail.c_str();
		return true;
	default:
		return false;
	}
}

CQuicTransport::CQuicTransport() = default;
CQuicTransport::~CQuicTransport()
{
	Shutdown();
}

bool CQuicTransport::IsCompiled()
{
	return true;
}

bool CQuicTransport::IsWebTransportClientAvailable()
{
	return false;
}

bool CQuicTransport::StartServer(bool RawQuic, bool WebTransport, const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, const char *pIdentityPath, const unsigned char *pCidKey, int CidKeySize)
{
	Shutdown();
	if((pCertificatePath[0] == '\0') != (pPrivateKeyPath[0] == '\0'))
	{
		str_copy(m_aError, "TLS certificate and private key must either both be set or both be empty");
		return false;
	}
	std::vector<uint8_t> vCertificate;
	std::vector<uint8_t> vNextCertificate;
	std::vector<uint8_t> vPrivateKey;
	if(pCertificatePath[0] != '\0' &&
		(!ReadFile(pCertificatePath, vCertificate, m_aError, sizeof(m_aError)) || !ReadFile(pPrivateKeyPath, vPrivateKey, m_aError, sizeof(m_aError))))
		return false;
	try
	{
		int64_t ManagedCertificateRotateAt = 0;
		if(pCertificatePath[0] == '\0')
		{
			const auto Identity = ModernQuic::quic_managed_identity(pIdentityPath, time_timestamp());
			vCertificate.assign(Identity.certificate_der.begin(), Identity.certificate_der.end());
			vPrivateKey.assign(Identity.private_key_der.begin(), Identity.private_key_der.end());
			vNextCertificate.assign(Identity.next_certificate_der.begin(), Identity.next_certificate_der.end());
			ManagedCertificateRotateAt = Identity.rotate_at;
		}
		if(pNextCertificatePath[0] != '\0' && !ReadFile(pNextCertificatePath, vNextCertificate, m_aError, sizeof(m_aError)))
			return false;
		m_CertificateSha256 = LeafCertificateSha256(Slice(vCertificate));
		if(!vNextCertificate.empty())
			m_NextCertificateSha256 = LeafCertificateSha256(Slice(vNextCertificate));
		auto Endpoint = ModernQuic::quic_server_start(RawQuic, WebTransport, Slice(vCertificate), Slice(vPrivateKey), pIdentityPath, {pCidKey, (size_t)CidKeySize});
		if(RawQuic)
		{
			const rust::Vec<uint8_t> Fingerprint = ModernQuic::quic_server_identity_fingerprint(*Endpoint);
			if(Fingerprint.size() != sizeof(m_IdentityFingerprint.data))
			{
				str_copy(m_aError, "the server identity key is unavailable");
				return false;
			}
			mem_copy(m_IdentityFingerprint.data, Fingerprint.data(), sizeof(m_IdentityFingerprint.data));
			m_HasIdentityFingerprint = true;
		}
		m_pImpl = std::make_unique<CImpl>(std::move(Endpoint), false);
		m_pImpl->m_ManagedCertificateRotateAt = ManagedCertificateRotateAt;
		if(ManagedCertificateRotateAt != 0)
			m_pImpl->m_ManagedIdentityPath = pIdentityPath;
		m_HasCertificateSha256 = true;
		m_HasNextCertificateSha256 = !vNextCertificate.empty() && m_NextCertificateSha256 != m_CertificateSha256;
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
		return false;
	}
}

bool CQuicTransport::UpdateCidKey(const unsigned char *pCidKey, int CidKeySize)
{
	if(!m_pImpl || CidKeySize == 0)
		return false;
	try
	{
		ModernQuic::quic_server_update_cid_key(*m_pImpl->m_Endpoint, {pCidKey, (size_t)CidKeySize});
		return true;
	}
	catch(const std::exception &Exception)
	{
		str_copy(m_aError, Exception.what());
		return false;
	}
}

bool CQuicTransport::MaybeRotateManagedCertificate(bool *pRotated)
{
	*pRotated = false;
	if(!m_pImpl || m_pImpl->m_ManagedIdentityPath.empty() || time_timestamp() < m_pImpl->m_ManagedCertificateRotateAt)
		return true;
	try
	{
		const auto Identity = ModernQuic::quic_managed_identity(m_pImpl->m_ManagedIdentityPath, time_timestamp());
		const SHA256_DIGEST CertificateSha256 = LeafCertificateSha256(Slice(Identity.certificate_der));
		const SHA256_DIGEST NextCertificateSha256 = Identity.next_certificate_der.empty() ? SHA256_DIGEST{} : LeafCertificateSha256(Slice(Identity.next_certificate_der));
		ModernQuic::quic_server_update_certificate(*m_pImpl->m_Endpoint, Slice(Identity.certificate_der), Slice(Identity.private_key_der), {CertificateSha256.data, sizeof(CertificateSha256.data)});
		m_CertificateSha256 = CertificateSha256;
		m_NextCertificateSha256 = NextCertificateSha256;
		m_HasNextCertificateSha256 = !Identity.next_certificate_der.empty() && NextCertificateSha256 != CertificateSha256;
		m_pImpl->m_ManagedCertificateRotateAt = Identity.rotate_at;
		*pRotated = true;
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
		m_pImpl->m_ManagedCertificateRotateAt = time_timestamp() + 60;
		return false;
	}
}

bool CQuicTransport::StartClient(const NETADDR &Address, const char *pServerName, const CModernTransportPin &Pin, bool Sixup)
{
	Shutdown();
	if(Pin.m_Trust == EModernTransportTrust::INVALID)
	{
		str_copy(m_aError, "no way to check the server certificate");
		return false;
	}
	char aAddress[NETADDR_MAXSTRSIZE];
	net_addr_str(&Address, aAddress, sizeof(aAddress), true);
	char aServerName[NETADDR_MAXSTRSIZE];
	if(pServerName[0] == '\0')
	{
		// The address stands in for the name, an IPv6 one without its brackets.
		char aHost[NETADDR_MAXSTRSIZE];
		net_addr_str(&Address, aHost, sizeof(aHost), false);
		const bool Brackets = aHost[0] == '[';
		str_truncate(aServerName, sizeof(aServerName), aHost + Brackets, str_length(aHost) - 2 * Brackets);
		pServerName = aServerName;
	}
	unsigned char aFingerprints[2 * SHA256_DIGEST_LENGTH];
	size_t FingerprintsSize = 0;
	if(Pin.m_Trust == EModernTransportTrust::CERTIFICATE_HASH || Pin.m_Trust == EModernTransportTrust::IDENTITY)
	{
		mem_copy(aFingerprints, Pin.m_Fingerprint.data, SHA256_DIGEST_LENGTH);
		FingerprintsSize = SHA256_DIGEST_LENGTH;
		if(Pin.m_Trust == EModernTransportTrust::CERTIFICATE_HASH && Pin.m_HasNextFingerprint)
		{
			mem_copy(aFingerprints + SHA256_DIGEST_LENGTH, Pin.m_NextFingerprint.data, SHA256_DIGEST_LENGTH);
			FingerprintsSize += SHA256_DIGEST_LENGTH;
		}
	}
	try
	{
		m_pImpl = std::make_unique<CImpl>(ModernQuic::quic_client_start(aAddress, pServerName, QuicPin(Pin.m_Trust), {aFingerprints, FingerprintsSize}, Sixup), true);
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
		return false;
	}
}

bool CQuicTransport::IsRunning() const
{
	return m_pImpl != nullptr;
}

bool CQuicTransport::Send(CQuicSessionId Session, const void *pData, int DataSize, bool Vital)
{
	if(!m_pImpl || !Session.IsValid() || DataSize <= 0)
		return false;
	const rust::Slice<const uint8_t> Payload(static_cast<const uint8_t *>(pData), static_cast<size_t>(DataSize));
	if(Vital ? ModernQuic::quic_send_control(*m_pImpl->m_Endpoint, Session.Value(), Payload) : ModernQuic::quic_send_datagram(*m_pImpl->m_Endpoint, Session.Value(), Payload))
		return true;
	// A client that reconnects has no session to send on until it has resumed,
	// and what it sends in the meantime is lost like on any other bad link.
	return m_pImpl->m_Client && !ModernQuic::quic_session_active(*m_pImpl->m_Endpoint, Session.Value());
}

bool CQuicTransport::SetMap(uint32_t MapId, const char *pName, uint32_t Crc, const SHA256_DIGEST &Sha256, const void *pData, size_t DataSize)
{
	if(!m_pImpl || DataSize == 0)
		return false;
	return ModernQuic::quic_set_map(*m_pImpl->m_Endpoint, MapId,
		{reinterpret_cast<const uint8_t *>(pName), static_cast<size_t>(str_length(pName))}, Crc,
		{Sha256.data, sizeof(Sha256.data)}, {static_cast<const uint8_t *>(pData), DataSize});
}

bool CQuicTransport::SendMap(CQuicSessionId Session, uint32_t MapId)
{
	return m_pImpl && Session.IsValid() && ModernQuic::quic_send_map(*m_pImpl->m_Endpoint, Session.Value(), MapId);
}

bool CQuicTransport::IssueResume(CQuicSessionId Session, uint64_t LogicalSessionId, const unsigned char *pToken, size_t TokenSize)
{
	return m_pImpl && Session.IsValid() && TokenSize > 0 && ModernQuic::quic_issue_resume(*m_pImpl->m_Endpoint, Session.Value(), LogicalSessionId, {pToken, TokenSize});
}

bool CQuicTransport::Reconnect(CQuicSessionId Session)
{
	return m_pImpl && Session.IsValid() && ModernQuic::quic_reconnect(*m_pImpl->m_Endpoint, Session.Value());
}

bool CQuicTransport::Close(CQuicSessionId Session, const char *pReason)
{
	return m_pImpl && ModernQuic::quic_close_session(*m_pImpl->m_Endpoint, Session.Value(), pReason ? pReason : "");
}

bool CQuicTransport::Poll(CQuicEvent &Event)
{
	while(m_pImpl && ModernQuic::quic_poll_event(*m_pImpl->m_Endpoint, m_pImpl->m_Event))
	{
		if(m_pImpl->TranslateEvent(Event, &m_ConnectFailure))
			return true;
	}
	return false;
}

int CQuicTransport::PollUdpSend(NETADDR *pAddress, unsigned char **ppData)
{
	if(!m_pImpl || !ModernQuic::quic_udp_poll_transmit(*m_pImpl->m_Endpoint, m_pImpl->m_UdpDatagram))
		return 0;
	const ModernQuic::UdpDatagram &Datagram = m_pImpl->m_UdpDatagram;
	const size_t IpSize = Datagram.source_is_ipv6 ? 16 : 4;
	if(Datagram.source_ip.size() != IpSize)
		return 0;
	*pAddress = {};
	pAddress->type = Datagram.source_is_ipv6 ? NETTYPE_IPV6 : NETTYPE_IPV4;
	mem_copy(pAddress->ip, Datagram.source_ip.data(), IpSize);
	pAddress->port = Datagram.source_port;
	*ppData = m_pImpl->m_UdpDatagram.payload.data();
	return static_cast<int>(Datagram.payload.size());
}

bool CQuicTransport::FeedUdp(const NETADDR *pAddress, const void *pData, int DataSize)
{
	if(!m_pImpl || DataSize <= 0)
		return false;
	const bool Ipv6 = (pAddress->type & NETTYPE_IPV6) != 0;
	return ModernQuic::quic_udp_feed(*m_pImpl->m_Endpoint, {pAddress->ip, Ipv6 ? size_t{16} : size_t{4}}, pAddress->port, Ipv6, {static_cast<const uint8_t *>(pData), static_cast<size_t>(DataSize)});
}

bool CQuicTransport::SetLegacyPeer(const NETADDR *pAddress, bool Known)
{
	if(!m_pImpl)
		return false;
	const bool Ipv6 = (pAddress->type & NETTYPE_IPV6) != 0;
	return ModernQuic::quic_udp_set_legacy_peer(*m_pImpl->m_Endpoint, {pAddress->ip, Ipv6 ? size_t{16} : size_t{4}}, pAddress->port, Ipv6, Known);
}

int64_t CQuicTransport::NextTimeoutMicroseconds() const
{
	return m_pImpl ? ModernQuic::quic_next_timeout_microseconds(*m_pImpl->m_Endpoint) : -1;
}

void CQuicTransport::LocalAddressChanged()
{
	if(m_pImpl)
		ModernQuic::quic_local_address_changed(*m_pImpl->m_Endpoint);
}

void CQuicTransport::Shutdown()
{
	if(m_pImpl)
		ModernQuic::quic_shutdown(*m_pImpl->m_Endpoint);
	m_pImpl.reset();
	m_ConnectFailure = EQuicConnectFailure::NONE;
	m_HasCertificateSha256 = false;
	m_HasNextCertificateSha256 = false;
	m_HasIdentityFingerprint = false;
}

#else

#include <base/str.h>

class CQuicTransport::CImpl
{
};

CQuicTransport::CQuicTransport() = default;
CQuicTransport::~CQuicTransport() = default;
bool CQuicTransport::IsCompiled() { return false; }
bool CQuicTransport::IsWebTransportClientAvailable() { return false; }

bool CQuicTransport::StartServer(bool RawQuic, bool WebTransport, const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, const char *pIdentityPath, const unsigned char *pCidKey, int CidKeySize)
{
	str_copy(m_aError, "QUIC support is not compiled in");
	return false;
}

bool CQuicTransport::UpdateCidKey(const unsigned char *pCidKey, int CidKeySize)
{
	str_copy(m_aError, "QUIC support is not compiled in");
	return false;
}

bool CQuicTransport::MaybeRotateManagedCertificate(bool *pRotated)
{
	*pRotated = false;
	return true;
}

bool CQuicTransport::StartClient(const NETADDR &Address, const char *pServerName, const CModernTransportPin &Pin, bool Sixup)
{
	str_copy(m_aError, "QUIC support is not compiled in");
	return false;
}

bool CQuicTransport::IsRunning() const { return false; }
bool CQuicTransport::Send(CQuicSessionId Session, const void *pData, int DataSize, bool Vital) { return false; }
bool CQuicTransport::SetMap(uint32_t MapId, const char *pName, uint32_t Crc, const SHA256_DIGEST &Sha256, const void *pData, size_t DataSize) { return false; }
bool CQuicTransport::SendMap(CQuicSessionId Session, uint32_t MapId) { return false; }
bool CQuicTransport::IssueResume(CQuicSessionId Session, uint64_t LogicalSessionId, const unsigned char *pToken, size_t TokenSize) { return false; }
bool CQuicTransport::Reconnect(CQuicSessionId Session) { return false; }
bool CQuicTransport::Close(CQuicSessionId Session, const char *pReason) { return false; }
bool CQuicTransport::Poll(CQuicEvent &Event) { return false; }
int CQuicTransport::PollUdpSend(NETADDR *pAddress, unsigned char **ppData) { return 0; }
bool CQuicTransport::FeedUdp(const NETADDR *pAddress, const void *pData, int DataSize) { return false; }
bool CQuicTransport::SetLegacyPeer(const NETADDR *pAddress, bool Known) { return false; }
int64_t CQuicTransport::NextTimeoutMicroseconds() const { return -1; }
void CQuicTransport::LocalAddressChanged() {}
void CQuicTransport::Shutdown() {}

#endif
#endif
