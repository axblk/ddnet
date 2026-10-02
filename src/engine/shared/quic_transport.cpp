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

	SHA256_DIGEST Sha256FromVec(const rust::Vec<uint8_t> &vSha256)
	{
		SHA256_DIGEST Digest = {};
		if(vSha256.size() == sizeof(Digest.data))
			mem_copy(Digest.data, vSha256.data(), sizeof(Digest.data));
		return Digest;
	}

	// The one place certificates are read, for files and the managed certificate alike.
	bool LoadCertificate(rust::Slice<const uint8_t> Certificate, rust::Slice<const uint8_t> PrivateKey, rust::Slice<const uint8_t> NextCertificate, CTlsServerCertificate *pResult, char *pError, int ErrorSize)
	{
		const ModernQuic::QuicServerCertificate Loaded = ModernQuic::quic_load_server_certificate(Certificate, PrivateKey, NextCertificate);
		if(!Loaded.error.empty())
		{
			str_copy(pError, std::string(Loaded.error).c_str(), ErrorSize);
			return false;
		}
		pResult->m_vChain.assign(Loaded.chain_der.begin(), Loaded.chain_der.end());
		pResult->m_vPrivateKey.assign(Loaded.private_key_der.begin(), Loaded.private_key_der.end());
		pResult->m_Sha256 = Sha256FromVec(Loaded.sha256);
		pResult->m_NextSha256.reset();
		if(!Loaded.next_sha256.empty())
			pResult->m_NextSha256 = Sha256FromVec(Loaded.next_sha256);
		pResult->m_SpkiSha256 = Sha256FromVec(Loaded.spki_sha256);
		pResult->m_NotAfter = Loaded.not_after;
		return true;
	}

	ModernQuic::QuicPin QuicPin(EModernTransportTrust Trust)
	{
		switch(Trust)
		{
		case EModernTransportTrust::TOFU: return ModernQuic::QuicPin::Tofu;
		case EModernTransportTrust::WEBPKI: return ModernQuic::QuicPin::WebPki;
		case EModernTransportTrust::CERTIFICATE_HASH: return ModernQuic::QuicPin::Sha256;
		case EModernTransportTrust::SPKI_HASH:
		case EModernTransportTrust::INVALID: break;
		}
		return ModernQuic::QuicPin::Spki;
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
	// Where the managed certificate is kept, empty for a certificate read from a file.
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
	case ModernQuic::QuicEventKind::ConnectFailedPin:
	case ModernQuic::QuicEventKind::ConnectFailedProtocol:
		if(m_Event.kind == ModernQuic::QuicEventKind::Disconnected)
			m_PeerAddresses.erase(Session.Value());
		else if(m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedNetwork)
			*pFailure = EQuicConnectFailure::NETWORK;
		else if(m_Event.kind == ModernQuic::QuicEventKind::ConnectFailedPin)
			*pFailure = EQuicConnectFailure::PIN;
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

bool CTlsServerCertificate::Load(const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, char *pError, int ErrorSize)
{
	if(pCertificatePath[0] == '\0' || pPrivateKeyPath[0] == '\0')
	{
		str_copy(pError, "TLS certificate and private key must both be set", ErrorSize);
		return false;
	}
	std::vector<uint8_t> vCertificate;
	std::vector<uint8_t> vNextCertificate;
	std::vector<uint8_t> vPrivateKey;
	if(!ReadFile(pCertificatePath, vCertificate, pError, ErrorSize) ||
		!ReadFile(pPrivateKeyPath, vPrivateKey, pError, ErrorSize) ||
		(pNextCertificatePath[0] != '\0' && !ReadFile(pNextCertificatePath, vNextCertificate, pError, ErrorSize)))
		return false;
	CTlsServerCertificate Loaded;
	if(!LoadCertificate(Slice(vCertificate), Slice(vPrivateKey), Slice(vNextCertificate), &Loaded, pError, ErrorSize))
		return false;
	*this = std::move(Loaded);
	return true;
}

void CQuicTransport::SetServerCertificateHashes(const CTlsServerCertificate &Certificate)
{
	m_CertificateSha256 = Certificate.m_Sha256;
	m_HasCertificateSha256 = true;
	m_NextCertificateSha256 = Certificate.m_NextSha256.value_or(SHA256_DIGEST{});
	m_HasNextCertificateSha256 = Certificate.m_NextSha256.has_value() && *Certificate.m_NextSha256 != Certificate.m_Sha256;
}

bool CQuicTransport::StartServer(bool RawQuic, bool WebTransport, const CTlsServerCertificate *pCertificate, const char *pIdentityPath)
{
	Shutdown();
	try
	{
		CTlsServerCertificate Managed;
		int64_t ManagedCertificateRotateAt = 0;
		if(!pCertificate)
		{
			const auto Identity = ModernQuic::quic_managed_identity(pIdentityPath, time_timestamp());
			if(!LoadCertificate(Slice(Identity.certificate_der), Slice(Identity.private_key_der), Slice(Identity.next_certificate_der), &Managed, m_aError, sizeof(m_aError)))
				return false;
			ManagedCertificateRotateAt = Identity.rotate_at;
			pCertificate = &Managed;
		}
		auto Endpoint = ModernQuic::quic_server_start(RawQuic, WebTransport, Slice(pCertificate->m_vChain), Slice(pCertificate->m_vPrivateKey), pIdentityPath);
		m_pImpl = std::make_unique<CImpl>(std::move(Endpoint), false);
		m_pImpl->m_ManagedCertificateRotateAt = ManagedCertificateRotateAt;
		if(ManagedCertificateRotateAt != 0)
			m_pImpl->m_ManagedIdentityPath = pIdentityPath;
		SetServerCertificateHashes(*pCertificate);
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
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
		CTlsServerCertificate Managed;
		if(!LoadCertificate(Slice(Identity.certificate_der), Slice(Identity.private_key_der), Slice(Identity.next_certificate_der), &Managed, m_aError, sizeof(m_aError)))
		{
			m_pImpl->m_ManagedCertificateRotateAt = time_timestamp() + 60;
			return false;
		}
		ModernQuic::quic_server_update_certificate(*m_pImpl->m_Endpoint, Slice(Managed.m_vChain), Slice(Managed.m_vPrivateKey));
		SetServerCertificateHashes(Managed);
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

bool CQuicTransport::ReloadServerCertificate(const CTlsServerCertificate &Certificate)
{
	if(!m_pImpl || m_pImpl->m_Client)
	{
		str_copy(m_aError, "no QUIC or WebTransport server is running");
		return false;
	}
	if(!m_pImpl->m_ManagedIdentityPath.empty())
	{
		str_copy(m_aError, "the server was started with a managed certificate, which it rotates itself");
		return false;
	}
	try
	{
		// Checks the key and whether it belongs to the certificate, and only
		// then replaces the certificate.
		ModernQuic::quic_server_update_certificate(*m_pImpl->m_Endpoint, Slice(Certificate.m_vChain), Slice(Certificate.m_vPrivateKey));
		SetServerCertificateHashes(Certificate);
		return true;
	}
	catch(const std::exception &Error)
	{
		str_copy(m_aError, Error.what());
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
	if(Pin.m_Trust == EModernTransportTrust::CERTIFICATE_HASH || Pin.m_Trust == EModernTransportTrust::SPKI_HASH)
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

std::optional<SHA256_DIGEST> CQuicTransport::RawQuicSpkiSha256() const
{
	if(!m_pImpl || m_pImpl->m_Client)
		return std::nullopt;
	const rust::Vec<uint8_t> Sha256 = ModernQuic::quic_server_spki_sha256(*m_pImpl->m_Endpoint);
	SHA256_DIGEST Digest;
	if(Sha256.size() != sizeof(Digest.data))
		return std::nullopt;
	mem_copy(Digest.data, Sha256.data(), sizeof(Digest.data));
	return Digest;
}

bool CQuicTransport::RawQuicIdentity(CTlsServerCertificate *pIdentity) const
{
	if(!m_pImpl || m_pImpl->m_Client)
		return false;
	const ModernQuic::QuicIdentity Identity = ModernQuic::quic_server_identity(*m_pImpl->m_Endpoint);
	char aError[256];
	return !Identity.certificate_der.empty() && LoadCertificate(Slice(Identity.certificate_der), Slice(Identity.private_key_der), {}, pIdentity, aError, sizeof(aError));
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

bool CTlsServerCertificate::Load(const char *pCertificatePath, const char *pNextCertificatePath, const char *pPrivateKeyPath, char *pError, int ErrorSize)
{
	// The certificate is read by the QUIC code, in Rust.
	str_copy(pError, "this build cannot read TLS certificates, QUIC support is not compiled in", ErrorSize);
	return false;
}

bool CQuicTransport::StartServer(bool RawQuic, bool WebTransport, const CTlsServerCertificate *pCertificate, const char *pIdentityPath)
{
	str_copy(m_aError, "QUIC support is not compiled in");
	return false;
}

bool CQuicTransport::MaybeRotateManagedCertificate(bool *pRotated)
{
	*pRotated = false;
	return true;
}

bool CQuicTransport::ReloadServerCertificate(const CTlsServerCertificate &Certificate)
{
	str_copy(m_aError, "QUIC support is not compiled in");
	return false;
}

bool CQuicTransport::StartClient(const NETADDR &Address, const char *pServerName, const CModernTransportPin &Pin, bool Sixup)
{
	str_copy(m_aError, "QUIC support is not compiled in");
	return false;
}

bool CQuicTransport::IsRunning() const { return false; }
std::optional<SHA256_DIGEST> CQuicTransport::RawQuicSpkiSha256() const { return std::nullopt; }
bool CQuicTransport::RawQuicIdentity(CTlsServerCertificate *pIdentity) const { return false; }
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
