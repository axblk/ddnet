#include "server_identity.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <game/localization.h>

#include <algorithm>

bool NormalizeQuicTrustHost(const char *pHost, char *pBuffer, int BufferSize)
{
	if(!pHost || pHost[0] == '\0' || str_length(pHost) >= BufferSize || !str_utf8_check(pHost))
		return false;
	NETADDR Address;
	// A normalized IPv6 host is stored without its brackets, but net_addr_from_str
	// only reads IPv6 in brackets, so normalizing an already normalized address
	// would fail to parse it and then reject the colons as a hostname. Bracket a
	// bare IPv6 so that normalization is idempotent.
	char aBracketed[128];
	const char *pParse = pHost;
	if(pHost[0] != '[' && str_find(pHost, ":"))
	{
		str_format(aBracketed, sizeof(aBracketed), "[%s]", pHost);
		pParse = aBracketed;
	}
	if(net_addr_from_str(&Address, pParse) == 0)
	{
		if(Address.port != 0)
			return false;
		net_addr_str(&Address, pBuffer, BufferSize, false);
		if(Address.type == NETTYPE_IPV6)
		{
			const int Length = str_length(pBuffer);
			mem_move(pBuffer, pBuffer + 1, Length - 2);
			pBuffer[Length - 2] = '\0';
		}
		return true;
	}
	str_utf8_tolower(pHost, pBuffer, BufferSize);
	int Length = str_length(pBuffer);
	if(Length > 0 && pBuffer[Length - 1] == '.')
		pBuffer[--Length] = '\0';
	if(Length == 0)
		return false;
	for(const unsigned char *p = reinterpret_cast<const unsigned char *>(pBuffer); *p; ++p)
	{
		if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_'))
			return false;
	}
	return true;
}

bool SetModernTransportStart(CModernTransportStart *pStart, const NETADDR &Addr, const char *pHost, const char *pFragment, bool Listed)
{
	*pStart = CModernTransportStart();
	if((Addr.type & (NETTYPE_QUIC | NETTYPE_WEBSOCKET_TLS)) == 0)
		return false;
	pStart->m_Address = Addr;
	pStart->m_WebTransport = (Addr.type & NETTYPE_WEBTRANSPORT) != 0;

	char aHost[128];
	if(pHost != nullptr && pHost[0] != '\0')
	{
		str_copy(aHost, pHost);
		str_copy(pStart->m_aServerName, pHost);
	}
	else
	{
		NETADDR Ip = Addr;
		Ip.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
		net_addr_str(&Ip, aHost, sizeof(aHost), false);
	}
	if(!NormalizeQuicTrustHost(aHost, pStart->m_aHost, sizeof(pStart->m_aHost)))
		return false;
	if(!ParseModernTransportFragment(pFragment ? pFragment : "", pStart->m_WebTransport, &pStart->m_Pin))
		return false;
	if(pFragment == nullptr || pFragment[0] == '\0')
		pStart->m_PinSource = EServerIdentitySource::ADDRESS;
	else
		pStart->m_PinSource = Listed ? EServerIdentitySource::LIST : EServerIdentitySource::LINK;
	return true;
}

const char *ModernTransportName(const CModernTransportStart &Start)
{
	if(Start.m_WebTransport)
		return "WebTransport";
	return Start.m_Address.type & NETTYPE_WEBSOCKET_TLS ? "wss" : "QUIC";
}

const CQuicKnownHosts::CHost *CQuicKnownHosts::Find(const char *pHost, int Port) const
{
	for(const CHost &Host : m_vHosts)
	{
		if(Host.m_Port == Port && str_comp(Host.m_aHost, pHost) == 0)
			return &Host;
	}
	return nullptr;
}

bool CQuicKnownHosts::Add(const char *pHost, int Port, const SHA256_DIGEST &SpkiSha256)
{
	char aNormalizedHost[128];
	if(!in_range(Port, 1, 65535) || !NormalizeQuicTrustHost(pHost, aNormalizedHost, sizeof(aNormalizedHost)))
		return false;
	if(const CHost *pKnownHost = Find(aNormalizedHost, Port))
		return pKnownHost->m_SpkiSha256 == SpkiSha256;
	if(m_vHosts.size() >= MAX_HOSTS)
		return false;
	CHost &Host = m_vHosts.emplace_back();
	str_copy(Host.m_aHost, aNormalizedHost);
	Host.m_Port = Port;
	Host.m_SpkiSha256 = SpkiSha256;
	return true;
}

bool CQuicKnownHosts::Update(const char *pHost, int Port, const SHA256_DIGEST &SpkiSha256)
{
	char aNormalizedHost[128];
	if(!NormalizeQuicTrustHost(pHost, aNormalizedHost, sizeof(aNormalizedHost)))
		return false;
	for(CHost &Host : m_vHosts)
	{
		if(Host.m_Port == Port && str_comp(Host.m_aHost, aNormalizedHost) == 0 && Host.m_SpkiSha256 != SpkiSha256)
		{
			Host.m_SpkiSha256 = SpkiSha256;
			return true;
		}
	}
	return false;
}

bool CQuicKnownHosts::Forget(const char *pHost, int Port)
{
	char aNormalizedHost[128];
	if(!NormalizeQuicTrustHost(pHost, aNormalizedHost, sizeof(aNormalizedHost)))
		return false;
	const auto NewEnd = std::remove_if(m_vHosts.begin(), m_vHosts.end(), [&](const CHost &Host) {
		return str_comp(Host.m_aHost, aNormalizedHost) == 0 && (Port == 0 || Host.m_Port == Port);
	});
	const bool Found = NewEnd != m_vHosts.end();
	m_vHosts.erase(NewEnd, m_vHosts.end());
	return Found;
}

void CQuicIdentityCheck::Reset()
{
	*this = CQuicIdentityCheck();
}

void CQuicIdentityCheck::Prepare(CModernTransportStart *pStart, const CQuicKnownHosts &KnownHosts)
{
	Reset();
	// WebTransport shows a certificate a browser takes, no key, and the browser
	// keeps nothing.
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	const bool Browser = true;
#else
	const bool Browser = false;
#endif
	if(Browser || pStart->m_WebTransport)
		return;
	str_copy(m_aHost, pStart->m_aHost);
	m_Port = pStart->m_Address.port;
	const CQuicKnownHosts::CHost *pKnownHost = KnownHosts.Find(pStart->m_aHost, pStart->m_Address.port);
	if(pStart->m_Pin.m_Trust == EModernTransportTrust::SPKI_HASH)
	{
		// The pin of a list or a link is what counts. A key remembered for the
		// host is brought up to date with it once it is proven, a new host is
		// not remembered, or every listed server would end up in the settings.
		m_Expected = pStart->m_Pin.m_Fingerprint;
		m_Required = true;
		m_Pinned = true;
		m_Update = pKnownHost != nullptr;
	}
	else if(pStart->m_Pin.m_Trust == EModernTransportTrust::TOFU)
	{
		// Trusted on first use, and remembered from then on.
		m_Required = true;
		if(pKnownHost)
		{
			m_Expected = pKnownHost->m_SpkiSha256;
			m_Known = true;
			pStart->m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
			pStart->m_Pin.m_Fingerprint = pKnownHost->m_SpkiSha256;
			pStart->m_PinSource = EServerIdentitySource::REMEMBERED;
		}
		else
			m_Remember = true;
	}
}

CQuicIdentityCheck::EResult CQuicIdentityCheck::Check(const void *pData, int DataSize, CQuicKnownHosts *pKnownHosts)
{
	if(!m_Required)
		return EResult::OK;
	if(DataSize != SHA256_DIGEST_LENGTH)
		return EResult::MISSING;
	SHA256_DIGEST SpkiSha256;
	mem_copy(SpkiSha256.data, pData, sizeof(SpkiSha256.data));
	if((m_Known || m_Pinned) && SpkiSha256 != m_Expected)
		return EResult::CHANGED;
	if(m_Update)
	{
		m_Update = false;
		return pKnownHosts->Update(m_aHost, m_Port, SpkiSha256) ? EResult::STORED : EResult::OK;
	}
	if(!m_Remember)
		return EResult::OK;
	if(!pKnownHosts->Add(m_aHost, m_Port, SpkiSha256))
		return EResult::NOT_STORED;
	m_Expected = SpkiSha256;
	m_Known = true;
	m_Remember = false;
	return EResult::STORED;
}

const char *const SERVER_IDENTITY_DISCONNECT_REASON = "server identity could not be verified";

CServerIdentityFailure CServerIdentityFailure::Expected(const CModernTransportStart &Start, const char *pTransport)
{
	CServerIdentityFailure Failure;
	str_copy(Failure.m_aHost, Start.m_aHost);
	Failure.m_Port = Start.m_Address.port;
	Failure.m_pTransport = pTransport;
	Failure.m_Pin = Start.m_Pin;
	Failure.m_Source = Start.m_PinSource;
	return Failure;
}

const char *ServerIdentityWarningTitle()
{
	return Localize("Server identity could not be verified");
}

// The start of a hash, enough to tell two apart by eye.
static void ShortSha256(const SHA256_DIGEST &Sha256, char *pBuffer, int BufferSize)
{
	char aSha256[SHA256_MAXSTRSIZE];
	sha256_str(Sha256, aSha256, sizeof(aSha256));
	str_format(pBuffer, BufferSize, "%.16s…", aSha256);
}

static const char *ServerIdentitySourceText(EServerIdentitySource Source)
{
	switch(Source)
	{
	case EServerIdentitySource::ADDRESS: return Localize("The address says how to check it.");
	case EServerIdentitySource::LIST: return Localize("The server list says how to check it.");
	case EServerIdentitySource::LINK: return Localize("The link says how to check it.");
	case EServerIdentitySource::REMEMBERED: return Localize("Its key was remembered from an earlier connection.");
	}
	dbg_assert_failed("invalid server identity source %d", static_cast<int>(Source));
}

void FormatServerIdentityWarning(char *pBuffer, int BufferSize, const CServerIdentityFailure &Failure)
{
	char aServer[160];
	if(str_find(Failure.m_aHost, ":"))
		str_format(aServer, sizeof(aServer), "[%s]:%d", Failure.m_aHost, Failure.m_Port);
	else
		str_format(aServer, sizeof(aServer), "%s:%d", Failure.m_aHost, Failure.m_Port);
	str_format(pBuffer, BufferSize, Localize("Could not verify the identity of %s over %s."), aServer, Failure.m_pTransport);

	str_append(pBuffer, " ", BufferSize);
	str_append(pBuffer, ServerIdentitySourceText(Failure.m_Source), BufferSize);

	char aExpected[32];
	char aPresented[32];
	ShortSha256(Failure.m_Pin.m_Fingerprint, aExpected, sizeof(aExpected));
	ShortSha256(Failure.m_Presented, aPresented, sizeof(aPresented));
	char aCheck[256] = "";
	switch(Failure.m_Pin.m_Trust)
	{
	case EModernTransportTrust::SPKI_HASH:
		if(Failure.m_HasPresented)
			str_format(aCheck, sizeof(aCheck), Localize("It should hold the key %s, it showed %s."), aExpected, aPresented);
		else
			str_format(aCheck, sizeof(aCheck), Localize("It should hold the key %s."), aExpected);
		break;
	case EModernTransportTrust::CERTIFICATE_HASH:
		if(Failure.m_HasPresented)
			str_format(aCheck, sizeof(aCheck), Localize("It should show the certificate %s, it showed %s."), aExpected, aPresented);
		else
			str_format(aCheck, sizeof(aCheck), Localize("It should show the certificate %s."), aExpected);
		break;
	case EModernTransportTrust::WEBPKI:
		str_copy(aCheck, Localize("Its certificate should be valid for its name (Web PKI)."));
		break;
	case EModernTransportTrust::TOFU:
	case EModernTransportTrust::INVALID:
		break;
	}
	if(aCheck[0] != '\0')
	{
		str_append(pBuffer, " ", BufferSize);
		str_append(pBuffer, aCheck, BufferSize);
	}

	if(Failure.m_Source == EServerIdentitySource::REMEMBERED)
	{
		char aForget[256];
		str_format(aForget, sizeof(aForget), Localize("Once you have made sure that the server changed its key, forget the old one with 'quic_forget_host %s %d'."), Failure.m_aHost, Failure.m_Port);
		str_append(pBuffer, " ", BufferSize);
		str_append(pBuffer, aForget, BufferSize);
	}
	str_append(pBuffer, " ", BufferSize);
	str_append(pBuffer, Localize("Refresh the server list, or pick another transport next to the address."), BufferSize);
}
