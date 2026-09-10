#include "connect_choice.h"

#include <base/net.h>
#include <base/str.h>

#include <engine/serverbrowser.h>

#include <utility>

// Whether an address is one of the transport, by the flags its scheme set.
static bool AddressIsProtocol(const NETADDR &Address, EConnectProtocol Protocol)
{
	switch(Protocol)
	{
	case EConnectProtocol::QUIC: return (Address.type & NETTYPE_QUIC) != 0 && (Address.type & NETTYPE_WEBTRANSPORT) == 0;
	case EConnectProtocol::WEBTRANSPORT: return (Address.type & NETTYPE_WEBTRANSPORT) != 0;
	case EConnectProtocol::WEBSOCKET: return (Address.type & NETTYPE_WEBSOCKET) != 0;
	default: return (Address.type & (NETTYPE_QUIC | NETTYPE_WEBSOCKET)) == 0;
	}
}

// Whether this client can speak the transport at all.
static bool ProtocolCompiledIn(EConnectProtocol Protocol)
{
	switch(Protocol)
	{
	case EConnectProtocol::QUIC:
	case EConnectProtocol::WEBTRANSPORT:
#if defined(CONF_NETWORKING_QUIC)
		return true;
#else
		return false;
#endif
	case EConnectProtocol::WEBSOCKET:
#if defined(CONF_WEBSOCKETS)
		return true;
#else
		return false;
#endif
	default: return true;
	}
}

CConnectChoices::CConnectChoices(const CServerInfo *pServer, const char *pAddress)
{
	const auto AddProtocol = [&](EConnectProtocol Protocol) {
		for(int i = 0; i < m_NumProtocols; ++i)
			if(m_aProtocols[i] == Protocol)
				return;
		m_aProtocols[m_NumProtocols++] = Protocol;
	};
	const auto AddFamily = [&](EConnectAddressFamily Family) {
		for(int i = 0; i < m_NumFamilies; ++i)
			if(m_aFamilies[i] == Family)
				return;
		m_aFamilies[m_NumFamilies++] = Family;
	};

	// A link says what it wants, so there is nothing left to choose.
	if(pAddress != nullptr && (str_startswith(pAddress, "ddnet+quic://") || str_startswith(pAddress, "tw-0.7+quic://")))
	{
		AddProtocol(EConnectProtocol::QUIC);
	}
	else if(pAddress != nullptr && (str_startswith(pAddress, "ddnet+wt://") || str_startswith(pAddress, "tw-0.7+wt://")))
	{
		AddProtocol(EConnectProtocol::WEBTRANSPORT);
	}
	else if(pAddress != nullptr && (str_startswith(pAddress, "ddnet+ws://") || str_startswith(pAddress, "ddnet+wss://")))
	{
		AddProtocol(EConnectProtocol::WEBSOCKET);
	}
	else if(pServer == nullptr)
	{
		// Typed by hand and not in the list: connect the way it always could.
		AddProtocol(EConnectProtocol::LEGACY);
	}
	else
	{
		// Best first, the top entry is taken when nothing was picked yet.
		for(const EConnectProtocol Protocol : {EConnectProtocol::QUIC, EConnectProtocol::WEBTRANSPORT, EConnectProtocol::WEBSOCKET, EConnectProtocol::LEGACY})
		{
			if(!ProtocolCompiledIn(Protocol))
				continue;
			for(int i = 0; i < pServer->m_NumAddresses; ++i)
			{
				if(AddressIsProtocol(pServer->m_aAddresses[i], Protocol))
				{
					AddProtocol(Protocol);
					break;
				}
			}
		}
		if(m_NumProtocols == 0)
			AddProtocol(EConnectProtocol::LEGACY);
	}

	// The family follows the addresses the server has; only an address of no
	// known server brings its own.
	NETADDR Literal;
	if(pServer == nullptr)
	{
		if(pAddress != nullptr && (net_addr_from_url(&Literal, pAddress, nullptr, 0) == 0 || net_addr_from_str(&Literal, pAddress) == 0))
			AddFamily((Literal.type & NETTYPE_IPV6) != 0 ? EConnectAddressFamily::IPV6 : EConnectAddressFamily::IPV4);
		else
			// A hostname is left to the resolver, which prefers IPv6 and falls back.
			AddFamily(EConnectAddressFamily::IPV6);
	}
	else
	{
		for(int i = 0; i < pServer->m_NumAddresses; ++i)
			AddFamily((pServer->m_aAddresses[i].type & NETTYPE_IPV6) != 0 ? EConnectAddressFamily::IPV6 : EConnectAddressFamily::IPV4);
		if(m_NumFamilies == 0)
			AddFamily(EConnectAddressFamily::IPV6);
		// Preference order, not discovery order.
		if(m_NumFamilies == 2 && m_aFamilies[0] == EConnectAddressFamily::IPV4)
			std::swap(m_aFamilies[0], m_aFamilies[1]);
	}
}

int CConnectChoices::ProtocolIndex(int Picked) const
{
	for(int i = 0; i < m_NumProtocols; ++i)
		if((int)m_aProtocols[i] == Picked)
			return i;
	return 0;
}

int CConnectChoices::FamilyIndex(int Picked) const
{
	for(int i = 0; i < m_NumFamilies; ++i)
		if((int)m_aFamilies[i] == Picked)
			return i;
	return 0;
}

const char *ConnectProtocolShortName(EConnectProtocol Protocol, const char *pAddress)
{
	switch(Protocol)
	{
	case EConnectProtocol::QUIC: return "QUIC";
	case EConnectProtocol::WEBSOCKET: return pAddress != nullptr && str_find_nocase(pAddress, "wss://") != nullptr ? "WSS" : "WS";
	case EConnectProtocol::WEBTRANSPORT: return "WT";
	default: return "UDP";
	}
}

bool ServerHasAddress(const CServerInfo &Server, const char *pAddress)
{
	NETADDR Address;
	// A host name is the server listed under it, with the port.
	char aHost[128];
	const int UrlParseResult = net_addr_from_url(&Address, pAddress, aHost, sizeof(aHost));
	if(UrlParseResult < 0)
	{
		if(Server.m_aHostname[0] == '\0')
			return false;
		const char *pPort = str_rchr(aHost, ':');
		if(pPort == nullptr)
			return false;
		const int Port = str_toint(pPort + 1);
		char aName[sizeof(aHost)];
		str_truncate(aName, sizeof(aName), aHost, pPort - aHost);
		if(str_comp_nocase(aName, Server.m_aHostname) != 0)
			return false;
		for(int i = 0; i < Server.m_NumAddresses; ++i)
		{
			if(Server.m_aAddresses[i].port == Port)
				return true;
		}
		return false;
	}
	if(UrlParseResult != 0 && net_addr_from_str(&Address, pAddress) != 0)
		return false;
	for(int i = 0; i < Server.m_NumAddresses; ++i)
	{
		if(net_addr_comp(&Server.m_aAddresses[i], &Address) == 0)
			return true;
	}
	return false;
}

const CServerInfo *FindListedServer(IServerBrowser &Browser, const char *pAddresses)
{
	char aFirstAddress[NETADDR_URL_MAXSTRSIZE + 128];
	str_copy(aFirstAddress, pAddresses);
	if(char *pSeparator = (char *)str_find(aFirstAddress, ","))
		*pSeparator = '\0';
	NETADDR Address;
	const int UrlParseResult = net_addr_from_url(&Address, aFirstAddress, nullptr, 0);
	if(UrlParseResult == 0 || net_addr_from_str(&Address, aFirstAddress) == 0)
	{
		const IServerBrowser::CServerEntry *pEntry = Browser.Find(Address);
		return pEntry != nullptr ? &pEntry->m_Info : nullptr;
	}
	if(UrlParseResult < 0)
	{
		for(int i = 0; i < Browser.NumServers(); ++i)
		{
			if(ServerHasAddress(*Browser.Get(i), aFirstAddress))
				return Browser.Get(i);
		}
	}
	return nullptr;
}

bool ConnectAddressFor(const CServerInfo &Server, int PickedProtocol, int PickedFamily, char *pBuffer, int BufferSize)
{
	if(Server.m_NumAddresses <= 0)
		return false;
	const CConnectChoices Choices(&Server, nullptr);
	const EConnectProtocol Protocol = Choices.m_aProtocols[Choices.ProtocolIndex(PickedProtocol)];
	const bool WantsIpv6 = (EConnectAddressFamily)PickedFamily != EConnectAddressFamily::IPV4;
	int Chosen = 0;
	int Best = -1;
	for(int i = 0; i < Server.m_NumAddresses; ++i)
	{
		const NETADDR &Address = Server.m_aAddresses[i];
		const bool FamilyMatches = ((Address.type & NETTYPE_IPV6) != 0) == WantsIpv6;
		const int Score = (AddressIsProtocol(Address, Protocol) ? 4 : 0) + ((Address.type & NETTYPE_TW7) != 0 ? 0 : 2) + (FamilyMatches ? 1 : 0);
		if(Score > Best)
		{
			Best = Score;
			Chosen = i;
		}
	}
	const NETADDR &Address = Server.m_aAddresses[Chosen];
	char aFragment[sizeof(Server.m_aWebTransportFragment)];
	CServerInfo::AddressFragment(aFragment, sizeof(aFragment), Server, Address);
	const bool SignedForName = (Address.type & NETTYPE_WEBSOCKET_TLS) != 0 || ((Address.type & NETTYPE_WEBTRANSPORT) != 0 && str_comp(aFragment, "webpki") == 0);
	if(SignedForName && Server.m_aHostname[0] != '\0')
	{
		net_addr_url_str(&Address, pBuffer, BufferSize, false);
		const int SchemeLength = str_find(pBuffer, "://") - pBuffer + 3;
		str_format(pBuffer + SchemeLength, BufferSize - SchemeLength, "%s:%d", Server.m_aHostname, Address.port);
	}
	else
	{
		net_addr_url_str(&Address, pBuffer, BufferSize, true);
	}
	if(aFragment[0] != '\0')
	{
		str_append(pBuffer, "#", BufferSize);
		str_append(pBuffer, aFragment, BufferSize);
	}
	return true;
}
