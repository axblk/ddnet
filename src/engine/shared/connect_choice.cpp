#include "connect_choice.h"

#include <base/net.h>
#include <base/str.h>

#include <engine/serverbrowser.h>

#include <utility>

EConnectProtocol AddressConnectProtocol(const NETADDR &Address)
{
	if((Address.type & NETTYPE_WEBTRANSPORT) != 0)
		return EConnectProtocol::WEBTRANSPORT;
	if((Address.type & NETTYPE_QUIC) != 0)
		return EConnectProtocol::QUIC;
	if((Address.type & NETTYPE_WEBSOCKET) != 0)
		return EConnectProtocol::WEBSOCKET;
	return EConnectProtocol::LEGACY;
}

bool ConnectProtocolAvailable(EConnectProtocol Protocol)
{
#if defined(CONF_PLATFORM_EMSCRIPTEN)
	// A browser has WebTransport and WebSockets, but no UDP.
	return Protocol == EConnectProtocol::WEBTRANSPORT || Protocol == EConnectProtocol::WEBSOCKET;
#else
	return Protocol == EConnectProtocol::LEGACY || Protocol == EConnectProtocol::QUIC;
#endif
}

bool ConnectEndpointUsable(const CServerInfo &Server, const NETADDR &Address)
{
	if(!ConnectProtocolAvailable(AddressConnectProtocol(Address)))
		return false;
	if((Address.type & NETTYPE_TW7) == 0)
		return true;
	// 0.7 only where the server has no DDNet endpoint for this client.
	for(int i = 0; i < Server.m_NumAddresses; ++i)
	{
		const NETADDR &Other = Server.m_aAddresses[i];
		if((Other.type & NETTYPE_TW7) == 0 && ConnectProtocolAvailable(AddressConnectProtocol(Other)))
			return false;
	}
	return true;
}

// What an address without a scheme connects with: UDP, as it always could,
// and WebTransport where there is no UDP.
static EConnectProtocol PlainAddressProtocol()
{
	return ConnectProtocolAvailable(EConnectProtocol::LEGACY) ? EConnectProtocol::LEGACY : EConnectProtocol::WEBTRANSPORT;
}

static EConnectAddressFamily AddressFamily(const NETADDR &Address)
{
	return (Address.type & NETTYPE_IPV6) != 0 ? EConnectAddressFamily::IPV6 : EConnectAddressFamily::IPV4;
}

CConnectChoices::CConnectChoices(const CServerInfo *pServer, const char *pAddress)
{
	const auto AddProtocol = [&](EConnectProtocol Protocol) {
		for(int i = 0; i < m_NumProtocols; ++i)
			if(m_aProtocols[i] == Protocol)
				return i;
		m_aProtocols[m_NumProtocols] = Protocol;
		return m_NumProtocols++;
	};
	const auto AddFamily = [&](EConnectAddressFamily Family) {
		for(int i = 0; i < m_NumFamilies; ++i)
			if(m_aFamilies[i] == Family)
				return i;
		m_aFamilies[m_NumFamilies] = Family;
		return m_NumFamilies++;
	};

	if(pServer != nullptr)
	{
		// What the server has for this client, best first: the top entry
		// is taken when nothing was picked yet.
		for(const EConnectProtocol Protocol : {EConnectProtocol::QUIC, EConnectProtocol::WEBTRANSPORT, EConnectProtocol::WEBSOCKET, EConnectProtocol::LEGACY})
		{
			for(int i = 0; i < pServer->m_NumAddresses; ++i)
			{
				if(ConnectEndpointUsable(*pServer, pServer->m_aAddresses[i]) && AddressConnectProtocol(pServer->m_aAddresses[i]) == Protocol)
				{
					AddProtocol(Protocol);
					break;
				}
			}
		}
		for(int i = 0; i < pServer->m_NumAddresses; ++i)
			if(ConnectEndpointUsable(*pServer, pServer->m_aAddresses[i]))
				AddFamily(AddressFamily(pServer->m_aAddresses[i]));
		// Preference order, not discovery order.
		if(m_NumFamilies == 2 && m_aFamilies[0] == EConnectAddressFamily::IPV4)
			std::swap(m_aFamilies[0], m_aFamilies[1]);
	}

	// The address connects the way it reads: a link by its scheme, an
	// address without one the way such an address always connects. That is
	// what is shown, also where it is not one of the server's choices.
	if(pAddress != nullptr && pAddress[0] != '\0')
	{
		NETADDR Address;
		const int UrlParseResult = net_addr_from_url(&Address, pAddress, nullptr, 0);
		m_CurrentProtocol = AddProtocol(UrlParseResult <= 0 ? AddressConnectProtocol(Address) : PlainAddressProtocol());
		if(UrlParseResult == 0 || (UrlParseResult > 0 && net_addr_from_str(&Address, pAddress) == 0))
			m_CurrentFamily = AddFamily(AddressFamily(Address));
	}
	if(m_NumProtocols == 0)
		AddProtocol(PlainAddressProtocol());
	// A host name is left to the resolver, which prefers IPv6 and falls back.
	if(m_NumFamilies == 0)
		AddFamily(EConnectAddressFamily::IPV6);
}

int CConnectChoices::ProtocolIndex(int Picked) const
{
	if(m_CurrentProtocol >= 0)
		return m_CurrentProtocol;
	for(int i = 0; i < m_NumProtocols; ++i)
		if((int)m_aProtocols[i] == Picked)
			return i;
	return 0;
}

int CConnectChoices::FamilyIndex(int Picked) const
{
	if(m_CurrentFamily >= 0)
		return m_CurrentFamily;
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

bool ServerReachable(const CServerInfo &Server)
{
	for(int i = 0; i < Server.m_NumAddresses; ++i)
	{
		if(ConnectEndpointUsable(Server, Server.m_aAddresses[i]))
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

bool ConnectAddressFor(const CServerInfo &Server, int PickedProtocol, int PickedFamily, EConnectPrecedence Precedence, char *pBuffer, int BufferSize)
{
	const CConnectChoices Choices(&Server, nullptr);
	// The first endpoint of the transport and the family, -1 standing for
	// any.
	const auto Find = [&](int Protocol, int Family) {
		for(int i = 0; i < Server.m_NumAddresses; ++i)
		{
			const NETADDR &Address = Server.m_aAddresses[i];
			if(ConnectEndpointUsable(Server, Address) &&
				(Protocol < 0 || (int)AddressConnectProtocol(Address) == Protocol) &&
				(Family < 0 || (int)AddressFamily(Address) == Family))
				return i;
		}
		return -1;
	};
	// The pick where the server has it, the best it has otherwise.
	const auto ProtocolIn = [&](int Family) {
		if(PickedProtocol >= 0 && Find(PickedProtocol, Family) >= 0)
			return PickedProtocol;
		for(int i = 0; i < Choices.m_NumProtocols; ++i)
			if(Find((int)Choices.m_aProtocols[i], Family) >= 0)
				return (int)Choices.m_aProtocols[i];
		return -1;
	};
	const auto FamilyIn = [&](int Protocol) {
		if(PickedFamily >= 0 && Find(Protocol, PickedFamily) >= 0)
			return PickedFamily;
		for(int i = 0; i < Choices.m_NumFamilies; ++i)
			if(Find(Protocol, (int)Choices.m_aFamilies[i]) >= 0)
				return (int)Choices.m_aFamilies[i];
		return -1;
	};
	int Chosen;
	if(Precedence == EConnectPrecedence::PROTOCOL)
	{
		const int Protocol = ProtocolIn(-1);
		Chosen = Find(Protocol, FamilyIn(Protocol));
	}
	else
	{
		const int Family = FamilyIn(-1);
		Chosen = Find(ProtocolIn(Family), Family);
	}
	if(Chosen < 0)
		return false;
	const NETADDR &Address = Server.m_aAddresses[Chosen];
	const bool ByName = Server.m_Pin.SignedForName(Address) && Server.m_aHostname[0] != '\0';
	Server.m_Pin.AddressUrl(Address, ByName ? Server.m_aHostname : nullptr, pBuffer, BufferSize);
	return true;
}
