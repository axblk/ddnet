#include "connect_target.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <engine/shared/config.h>
#include <engine/shared/quic_transport.h>

#include <algorithm>
#include <iterator>

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

// A websocket address carries no IPv4 or IPv6 bit, so reducing it to the
// address family leaves a type of zero, which cannot be formatted or connected
// to. QUIC and WebTransport never run over such an address.
static bool ToModernTransportAddress(const NETADDR &Address, NETADDR *pResult)
{
	if((Address.type & (NETTYPE_IPV4 | NETTYPE_IPV6)) == 0)
		return false;
	*pResult = Address;
	pResult->type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	return true;
}

static bool FindModernAddress(const NETADDR *pAddresses, int NumAddresses, const NETADDR &Reference, bool Sixup, NETADDR *pResult)
{
	const NETADDR *pFallback = nullptr;
	NETADDR ReferenceAddress = Reference;
	ReferenceAddress.type = ConnectAddressFamily(Reference) == EConnectAddressFamily::IPV6 ? NETTYPE_IPV6 : NETTYPE_IPV4;
	for(int i = 0; i < NumAddresses; i++)
	{
		if(((pAddresses[i].type & NETTYPE_TW7) != 0) != Sixup)
			continue;
		NETADDR Address = pAddresses[i];
		Address.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
		// Without a match, another address in the same family. The family is
		// the one that was picked, and the address field shows the legacy
		// address it has where the modern transport has none in it.
		if(Address.type != ReferenceAddress.type)
			continue;
		if(!pFallback)
			pFallback = &pAddresses[i];
		if(net_addr_comp_noport(&Address, &ReferenceAddress) == 0)
		{
			*pResult = pAddresses[i];
			return true;
		}
	}
	if(!pFallback)
		return false;
	*pResult = *pFallback;
	return true;
}

// A connect link carries its certificate hashes as `#cert-sha256=A,B`, so the
// commas that separate addresses are only the ones before the fragment.
static const char *NextConnectAddress(const char *pStr, char *pBuffer, int BufferSize)
{
	while(*pStr == ',')
		pStr++;
	if(*pStr == '\0')
		return nullptr;
	const char *pEnd = pStr;
	while(*pEnd != '\0' && *pEnd != ',' && *pEnd != '#')
		pEnd++;
	if(*pEnd == '#')
		pEnd = pStr + str_length(pStr);
	str_truncate(pBuffer, BufferSize, pStr, pEnd - pStr);
	return pEnd;
}

// The name a certificate is checked for, none for an address.
static const char *CertificateName(const char *pHost)
{
	NETADDR Address;
	return str_find(pHost, ":") || net_addr_from_str(&Address, pHost) == 0 ? "" : pHost;
}
bool CConnectTarget::Parse(const char *pAddress, int NetTypes, EConnectAddressFamily Family)
{
	// IPv6 is the preference and not a demand, so it is left to the resolver,
	// which already takes IPv6 where a hostname has it and IPv4 where it does
	// not. IPv4 is the one that rules a family out.
	*this = CConnectTarget();
	int LookupNetType = NetTypes;
	if(Family == EConnectAddressFamily::IPV4)
		LookupNetType &= ~(NETTYPE_IPV6 | NETTYPE_WEBSOCKET_IPV6);
	char aBuffer[256];
	const char *pNextAddr = pAddress;
	for(int Token = 0; (pNextAddr = NextConnectAddress(pNextAddr, aBuffer, sizeof(aBuffer))); Token++)
	{
		const bool Link = IsModernTransportUrl(aBuffer);
		// A link names the one server to connect to.
		if(m_Link || (Link && (Token != 0 || !ParseModernTransportUrl(aBuffer, &m_LinkWebTransport, &m_LinkPin))))
		{
			log_error("client", "invalid QUIC or WebTransport link, or more than one connect address with one");
			return false;
		}
		m_Link = Link;

		NETADDR NextAddr;
		char aHost[128];
		NETADDR ParsedAddr;
		const int UrlResult = net_addr_from_url(&ParsedAddr, aBuffer, aHost, sizeof(aHost));
		if(UrlResult > 0)
		{
			if(net_addr_from_str(&ParsedAddr, aBuffer) == 0)
				net_addr_str(&ParsedAddr, aHost, sizeof(aHost), false);
			else
			{
				str_copy(aHost, aBuffer);
				if(char *pPort = const_cast<char *>(str_rchr(aHost, ':')))
					*pPort = '\0';
			}
		}
		else if(UrlResult == 0)
			net_addr_str(&ParsedAddr, aHost, sizeof(aHost), false);
		else if(char *pPort = const_cast<char *>(str_rchr(aHost, ':')))
			*pPort = '\0';
		int AddrNetType = LookupNetType;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// WebTransport is the browser's own and reaches IPv6, which the
		// websockets the network runs on here cannot: Emscripten's sockets are
		// IPv4 only. A host name stays with the resolver as it is.
		if(Link && UrlResult == 0)
			AddrNetType = Family == EConnectAddressFamily::IPV4 ? NETTYPE_IPV4 : NETTYPE_IPV4 | NETTYPE_IPV6;
#endif
		if(net_addr_from_url_lookup(&NextAddr, aBuffer, AddrNetType) != 0)
		{
			log_error("client", "could not find address of %s", aBuffer);
			continue;
		}
#if defined(CONF_PLATFORM_EMSCRIPTEN)
		// The browser opens all its websockets with one scheme, which the
		// client sets for the whole connect. An address without a scheme is
		// one of them (see `LegacyAddresses`), with the scheme of the page.
		if(UrlResult > 0 || (NextAddr.type & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) != 0)
		{
			const int NextWebsocketSecure = UrlResult > 0 ? net_websocket_secure_default() : (NextAddr.type & NETTYPE_WEBSOCKET_TLS) != 0;
			if(m_WebsocketSecure >= 0 && m_WebsocketSecure != NextWebsocketSecure)
			{
				log_error("client", "cannot mix ws and wss connect addresses");
				return false;
			}
			m_WebsocketSecure = NextWebsocketSecure;
		}
#else
		if((NextAddr.type & NETTYPE_WEBSOCKET_TLS) != 0)
		{
			log_error("client", "secure websockets (ddnet-20+wss://) are not supported by this client");
			continue;
		}
		if((NextAddr.type & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) != 0 &&
			(NetTypes & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) == 0)
		{
			log_error("client", "websockets (ddnet-20+ws://) are not supported by this client");
			continue;
		}
#endif
		if(m_NumAddrs == (int)std::size(m_aAddrs))
		{
			log_warn("client", "too many connect addresses, ignoring %s", aBuffer);
			continue;
		}
		if(NextAddr.port == 0)
		{
			NextAddr.port = 8303;
		}
		if(!NormalizeQuicTrustHost(aHost, m_aaHosts[m_NumAddrs], sizeof(m_aaHosts[m_NumAddrs])))
		{
			log_error("client", "invalid connect host '%s'", aHost);
			continue;
		}
		if((NextAddr.type & NETTYPE_TW7) == 0)
			m_OnlySixup = false;

		char aNextAddr[NETADDR_URL_MAXSTRSIZE];
		net_addr_url_str(&NextAddr, aNextAddr, sizeof(aNextAddr), true);
		log_debug("client", "resolved connect address '%s' to %s", aBuffer, aNextAddr);
		m_aSchemeless[m_NumAddrs] = UrlResult > 0;
		m_aAddrs[m_NumAddrs++] = NextAddr;
	}

	return true;
}

int CConnectTarget::LegacyAddresses(int NetTypes, NETADDR *pAddrs) const
{
	int NumAddrs = 0;
	for(int i = 0; i < m_NumAddrs; i++)
	{
		NETADDR Addr = m_aAddrs[i];
		const int Udp = Addr.type & (NETTYPE_IPV4 | NETTYPE_IPV6);
		const int Websocket = ((Udp & NETTYPE_IPV4) != 0 ? NETTYPE_WEBSOCKET_IPV4 : 0) | ((Udp & NETTYPE_IPV6) != 0 ? NETTYPE_WEBSOCKET_IPV6 : 0);
		if(m_aSchemeless[i] && (Udp & NetTypes) == 0 && (Websocket & NetTypes) != 0)
			Addr.type = (Addr.type & ~Udp) | Websocket | (m_WebsocketSecure > 0 ? NETTYPE_WEBSOCKET_TLS : 0);
		if((Addr.type & NetTypes) != 0)
			pAddrs[NumAddrs++] = Addr;
	}
	return NumAddrs;
}

EConnectTransport ChooseConnectTransport(const CConnectTarget &Target, const CConnectTransportOptions &Options, const FFindListedServer &FindListedServer, CModernTransportStart *pStart)
{
	dbg_assert(Target.m_NumAddrs > 0, "connect target without addresses");
	// A browser has WebTransport, everything else QUIC.
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	const bool WebTransport = true;
#else
	const bool WebTransport = false;
#endif
	*pStart = CModernTransportStart();
	pStart->m_Address = Target.m_aAddrs[0];
	pStart->m_Pin = Target.m_LinkPin;
	pStart->m_Sixup = Target.m_OnlySixup;
	pStart->m_WebTransport = WebTransport;
	str_copy(pStart->m_aHost, Target.m_aaHosts[0]);
	const char *pServerName = Target.m_aaHosts[0];
	if(Target.m_Link && Target.m_LinkWebTransport != WebTransport)
	{
		log_error("client", WebTransport ? "QUIC links are only supported by the native client" : "WebTransport links are only supported by the web client");
		return EConnectTransport::FAILED;
	}
	if(!Target.m_Link)
	{
		// Nothing picked next to the address field means the best there is,
		// which is QUIC where the client has it, and WebTransport where the
		// browser has it.
		const bool Picked = Options.m_Protocol >= 0;
		const EConnectProtocol Best = WebTransport ? EConnectProtocol::WEBTRANSPORT : EConnectProtocol::QUIC;
		const bool HasBest = WebTransport ? CQuicTransport::IsWebTransportClientAvailable() : CQuicTransport::IsCompiled();
		const EConnectProtocol Protocol = Picked ? (EConnectProtocol)std::clamp(Options.m_Protocol, 0, (int)EConnectProtocol::COUNT - 1) :
							   (Options.m_Quic && HasBest ? Best : EConnectProtocol::LEGACY);
		if(Protocol != (WebTransport ? EConnectProtocol::WEBTRANSPORT : EConnectProtocol::QUIC))
			return EConnectTransport::LEGACY;

		// A server that has it says so in the browser, with the port and how to
		// check its certificate. A known server that does not gets the legacy
		// transport. One the browser has never seen says nothing either way, so
		// there a transport picked by hand is the only thing to go on.
		const CModernTransportInfo *pInfo = nullptr;
		bool Known = false;
		for(int i = 0; FindListedServer && i < Target.m_NumAddrs && !pInfo; i++)
		{
			NETADDR EntryAddress = Target.m_aAddrs[i];
			const CServerInfo *pServer = FindListedServer(EntryAddress);
			EntryAddress.type &= ~NETTYPE_TW7;
			if(!pServer)
				pServer = FindListedServer(EntryAddress);
			if(!pServer)
				continue;
			Known = true;
			const CModernTransportInfo &Info = WebTransport ? pServer->m_WebTransport : pServer->m_Quic;
			if(FindModernAddress(Info.m_aAddresses, Info.m_NumAddresses, Target.m_aAddrs[i], Target.m_OnlySixup, &pStart->m_Address))
				pInfo = &Info;
		}
		if(!pInfo && (Known || !Picked))
			return EConnectTransport::LEGACY;
		if(pInfo)
		{
			pStart->m_Pin = pInfo->m_Pin;
			if(pInfo->m_aHostname[0] != '\0')
				pServerName = pInfo->m_aHostname;
		}
		else
			pStart->m_Pin.m_Trust = WebTransport ? EModernTransportTrust::WEBPKI : EModernTransportTrust::TOFU;
	}
	if(!ToModernTransportAddress(pStart->m_Address, &pStart->m_Address))
	{
		log_error("client", "%s cannot be used with this address", WebTransport ? "WebTransport" : "QUIC");
		return EConnectTransport::FAILED;
	}

	if(!WebTransport)
	{
		if(Options.m_pCertificateSha256[0] != '\0')
		{
			pStart->m_Pin = {};
			pStart->m_Pin.m_Trust = EModernTransportTrust::CERTIFICATE_HASH;
			if(sha256_from_str(&pStart->m_Pin.m_Fingerprint, Options.m_pCertificateSha256) != 0)
			{
				log_error("client", "cl_quic_cert must be a SHA-256 hash");
				return EConnectTransport::FAILED;
			}
		}
		if(Options.m_pServerName[0] != '\0')
			pServerName = Options.m_pServerName;
	}
	str_copy(pStart->m_aServerName, CertificateName(pServerName));
	return EConnectTransport::MODERN;
}

CConnectPlatform CConnectPlatform::ThisClient()
{
	CConnectPlatform Platform;
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	Platform.m_Browser = true;
	Platform.m_Modern = g_Config.m_ClQuic && CQuicTransport::IsWebTransportClientAvailable();
	Platform.m_SecureWebsocketsOnly = net_websocket_secure_default();
#else
	Platform.m_Modern = g_Config.m_ClQuic && CQuicTransport::IsCompiled();
#endif
	return Platform;
}

EConnectAddressFamily ConnectAddressFamily(const NETADDR &Address)
{
	return (Address.type & (NETTYPE_IPV6 | NETTYPE_WEBSOCKET_IPV6)) != 0 ? EConnectAddressFamily::IPV6 : EConnectAddressFamily::IPV4;
}

CServerEndpoints::CServerEndpoints(const CServerInfo &Info, const CConnectPlatform &Platform)
{
	// The legacy addresses of a server that has none are those of its modern
	// transport, which it is only known by.
	for(int i = 0; !Info.m_ModernOnly && i < Info.m_NumAddresses; i++)
	{
		const NETADDR &Address = Info.m_aAddresses[i];
		const bool Websocket = (Address.type & (NETTYPE_WEBSOCKET_IPV4 | NETTYPE_WEBSOCKET_IPV6)) != 0;
		const bool Reachable = Platform.m_Browser ? Websocket && (!Platform.m_SecureWebsocketsOnly || (Address.type & NETTYPE_WEBSOCKET_TLS) != 0) :
							    !Websocket && (Address.type & (NETTYPE_IPV4 | NETTYPE_IPV6)) != 0;
		if(Reachable)
			m_aEndpoints[m_NumEndpoints++] = {Websocket ? EConnectProtocol::WEBSOCKET : EConnectProtocol::LEGACY, Address};
	}
	const CModernTransportInfo &Modern = Platform.m_Browser ? Info.m_WebTransport : Info.m_Quic;
	if(Platform.m_Modern && Modern.m_NumAddresses > 0)
	{
		m_pModern = &Modern;
		for(int i = 0; i < Modern.m_NumAddresses; i++)
			m_aEndpoints[m_NumEndpoints++] = {Platform.ModernProtocol(), Modern.m_aAddresses[i]};
	}

	// A server that speaks DDNet is played on DDNet, and 0.7 is what is left
	// where it does not.
	bool Ddnet = false;
	for(int i = 0; i < m_NumEndpoints; i++)
		Ddnet = Ddnet || (m_aEndpoints[i].m_Address.type & NETTYPE_TW7) == 0;
	if(Ddnet)
	{
		auto *const NewEnd = std::remove_if(m_aEndpoints, m_aEndpoints + m_NumEndpoints, [](const CServerEndpoint &Endpoint) {
			return (Endpoint.m_Address.type & NETTYPE_TW7) != 0;
		});
		m_NumEndpoints = NewEnd - m_aEndpoints;
	}
	if(!Has(Platform.ModernProtocol()))
		m_pModern = nullptr;
}

const CServerEndpoint *CServerEndpoints::Find(EConnectProtocol Protocol, EConnectAddressFamily Family) const
{
	for(int i = 0; i < m_NumEndpoints; i++)
	{
		if(m_aEndpoints[i].m_Protocol == Protocol && ConnectAddressFamily(m_aEndpoints[i].m_Address) == Family)
			return &m_aEndpoints[i];
	}
	return nullptr;
}

bool CServerEndpoints::Has(EConnectProtocol Protocol) const
{
	return Find(Protocol, EConnectAddressFamily::IPV6) || Find(Protocol, EConnectAddressFamily::IPV4);
}

bool CServerEndpoints::Has(EConnectAddressFamily Family) const
{
	for(int i = 0; i < m_NumEndpoints; i++)
	{
		if(ConnectAddressFamily(m_aEndpoints[i].m_Address) == Family)
			return true;
	}
	return false;
}

// The address field may hold several addresses, the first one says what it is.
static bool FirstConnectAddressHost(const char *pAddress, NETADDR *pResult, char *pHost, int HostSize)
{
	char aFirst[512] = "";
	NextConnectAddress(pAddress, aFirst, sizeof(aFirst));
	pHost[0] = '\0';
	if(net_addr_from_url(pResult, aFirst, pHost, HostSize) == 0)
		return true;
	if(pHost[0] != '\0')
		return false;
	// Not a URL, an address or a host name with a port.
	str_copy(pHost, aFirst, HostSize);
	return net_addr_from_str(pResult, aFirst) == 0;
}

// Best first: QUIC, or WebTransport in a browser, then what the legacy transport
// runs over here.
static const EConnectProtocol PROTOCOL_PREFERENCE[] = {EConnectProtocol::QUIC, EConnectProtocol::WEBTRANSPORT, EConnectProtocol::WEBSOCKET, EConnectProtocol::LEGACY};
static const EConnectAddressFamily FAMILY_PREFERENCE[] = {EConnectAddressFamily::IPV6, EConnectAddressFamily::IPV4};

EConnectProtocol ConnectProtocolOf(const char *pAddress, const CConnectPlatform &Platform)
{
	if(str_startswith(pAddress, "ddnet+wt://") || str_startswith(pAddress, "tw-0.7+wt://"))
		return EConnectProtocol::WEBTRANSPORT;
	if(IsModernTransportUrl(pAddress))
		return EConnectProtocol::QUIC;
	if(str_startswith(pAddress, "ws://") || str_startswith(pAddress, "wss://") || str_startswith(pAddress, "ddnet-20+ws://") || str_startswith(pAddress, "ddnet-20+wss://"))
		return EConnectProtocol::WEBSOCKET;
	// The browser has no UDP and opens a websocket to an address without a scheme.
	return Platform.m_Browser ? EConnectProtocol::WEBSOCKET : EConnectProtocol::LEGACY;
}

bool FirstConnectAddress(const char *pAddress, NETADDR *pResult)
{
	char aHost[128];
	return FirstConnectAddressHost(pAddress, pResult, aHost, sizeof(aHost));
}

CConnectChoices ConnectChoicesFor(const CServerInfo *pServer, const char *pAddress, const CConnectPlatform &Platform)
{
	CConnectChoices Choices;
	if(pServer != nullptr)
	{
		// What the server has, whatever form the address field is in: a link
		// to it is one of its endpoints and the others stay on offer.
		const CServerEndpoints Endpoints(*pServer, Platform);
		for(EConnectProtocol Protocol : PROTOCOL_PREFERENCE)
		{
			if(Endpoints.Has(Protocol))
				Choices.m_aProtocols[Choices.m_NumProtocols++] = Protocol;
		}
		for(EConnectAddressFamily Family : FAMILY_PREFERENCE)
		{
			if(Endpoints.Has(Family))
				Choices.m_aFamilies[Choices.m_NumFamilies++] = Family;
		}
		if(Choices.m_NumProtocols > 0)
			return Choices;
		Choices = CConnectChoices();
	}

	// An address no listed server has says itself what it is, so there is
	// nothing to choose. A host name is left to the resolver, which prefers IPv6
	// and falls back.
	Choices.m_aProtocols[Choices.m_NumProtocols++] = ConnectProtocolOf(pAddress, Platform);
	NETADDR Address;
	Choices.m_aFamilies[Choices.m_NumFamilies++] = FirstConnectAddress(pAddress, &Address) ? ConnectAddressFamily(Address) : EConnectAddressFamily::IPV6;
	return Choices;
}

bool FormatConnectAddress(char *pBuffer, int BufferSize, const CServerInfo &Server, const CConnectPlatform &Platform, int Protocol, EConnectAddressFamily Family, bool FamilyFirst)
{
	const CServerEndpoints Endpoints(Server, Platform);
	if(Endpoints.m_NumEndpoints == 0)
		return false;

	// The transport and the family that were picked where the server has them,
	// the best it has where it does not.
	const bool ProtocolPicked = in_range(Protocol, 0, (int)EConnectProtocol::COUNT - 1) && Endpoints.Has((EConnectProtocol)Protocol);
	const CServerEndpoint *pChosen = nullptr;
	if(FamilyFirst && Endpoints.Has(Family))
	{
		if(ProtocolPicked)
			pChosen = Endpoints.Find((EConnectProtocol)Protocol, Family);
		for(int i = 0; !pChosen && i < (int)std::size(PROTOCOL_PREFERENCE); i++)
			pChosen = Endpoints.Find(PROTOCOL_PREFERENCE[i], Family);
	}
	else
	{
		EConnectProtocol Chosen = ProtocolPicked ? (EConnectProtocol)Protocol : EConnectProtocol::COUNT;
		for(int i = 0; Chosen == EConnectProtocol::COUNT && i < (int)std::size(PROTOCOL_PREFERENCE); i++)
		{
			if(Endpoints.Has(PROTOCOL_PREFERENCE[i]))
				Chosen = PROTOCOL_PREFERENCE[i];
		}
		pChosen = Endpoints.Find(Chosen, Family);
		for(int i = 0; !pChosen && i < (int)std::size(FAMILY_PREFERENCE); i++)
			pChosen = Endpoints.Find(Chosen, FAMILY_PREFERENCE[i]);
	}
	dbg_assert(pChosen != nullptr, "server endpoints without a transport");

	if(pChosen->m_Protocol == Platform.ModernProtocol() && Endpoints.m_pModern != nullptr)
		return FormatModernTransportUrl(pBuffer, BufferSize, Platform.m_Browser, pChosen->m_Address, Endpoints.m_pModern->m_aHostname, Endpoints.m_pModern->m_Pin);
	// With the scheme it was announced under: a 0.7 address that loses its
	// scheme is a 0.6 address, which is neither the server in the list nor the
	// one that would answer.
	net_addr_url_str(&pChosen->m_Address, pBuffer, BufferSize, true);
	return true;
}

bool ServerHasConnectAddress(const CServerInfo &Server, const char *pAddress)
{
	NETADDR Address;
	char aHost[128];
	const bool IsAddress = FirstConnectAddressHost(pAddress, &Address, aHost, sizeof(aHost));
	const CModernTransportInfo *pModern = nullptr;
	if(IsModernTransportUrl(pAddress))
		pModern = ConnectProtocolOf(pAddress, CConnectPlatform()) == EConnectProtocol::WEBTRANSPORT ? &Server.m_WebTransport : &Server.m_Quic;
	if(!IsAddress)
	{
		// A modern transport is listed under the name its certificate is for.
		if(pModern == nullptr || pModern->m_aHostname[0] == '\0')
			return false;
		const char *pPort = str_rchr(aHost, ':');
		if(pPort == nullptr || str_comp_nocase_num(aHost, pModern->m_aHostname, pPort - aHost) != 0 || str_length(pModern->m_aHostname) != pPort - aHost)
			return false;
		for(int i = 0; i < pModern->m_NumAddresses; i++)
		{
			if(pModern->m_aAddresses[i].port == str_toint(pPort + 1))
				return true;
		}
		return false;
	}
	if(pModern != nullptr)
	{
		for(int i = 0; i < pModern->m_NumAddresses; i++)
		{
			if(net_addr_comp(&pModern->m_aAddresses[i], &Address) == 0)
				return true;
		}
		return false;
	}
	for(int i = 0; i < Server.m_NumAddresses; i++)
	{
		if(net_addr_comp(&Server.m_aAddresses[i], &Address) == 0)
			return true;
	}
	return false;
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
	if(pStart->m_WebTransport)
		return;
	if(pStart->m_Pin.m_Trust == EModernTransportTrust::SPKI_HASH)
	{
		m_Expected = pStart->m_Pin.m_Fingerprint;
		m_Required = true;
	}
	else if(pStart->m_Pin.m_Trust == EModernTransportTrust::TOFU)
	{
		// Trusted on first use, and remembered from then on.
		m_Required = true;
		str_copy(m_aHost, pStart->m_aHost);
		m_Port = pStart->m_Address.port;
		if(const CQuicKnownHosts::CHost *pKnownHost = KnownHosts.Find(pStart->m_aHost, pStart->m_Address.port))
		{
			m_Expected = pKnownHost->m_SpkiSha256;
			m_Known = true;
			pStart->m_Pin.m_Trust = EModernTransportTrust::SPKI_HASH;
			pStart->m_Pin.m_Fingerprint = pKnownHost->m_SpkiSha256;
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
	if(m_Known && SpkiSha256 != m_Expected)
		return EResult::CHANGED;
	if(!m_Remember)
		return EResult::OK;
	if(!pKnownHosts->Add(m_aHost, m_Port, SpkiSha256))
		return EResult::NOT_STORED;
	m_Expected = SpkiSha256;
	m_Known = true;
	m_Remember = false;
	return EResult::STORED;
}
