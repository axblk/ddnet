#include "connect_target.h"

#include "network.h"

#include <base/log.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <iterator>

// Splits the next address off a comma-separated list. A comma inside a
// fragment, between the certificate hashes a browser takes, does not end the
// address: what follows it there is a hash or another fragment key, never an
// address.
static const char *NextConnectAddress(const char *pList, char *pBuffer, int BufferSize)
{
	if(pList == nullptr || pList[0] == '\0')
	{
		return nullptr;
	}
	const char *pEnd = pList;
	bool InFragment = false;
	for(; *pEnd != '\0'; pEnd++)
	{
		if(*pEnd == '#')
		{
			InFragment = true;
		}
		else if(*pEnd == ',')
		{
			if(!InFragment)
			{
				break;
			}
			const char *pNext = pEnd + 1;
			int HexDigits = 0;
			while(('0' <= pNext[HexDigits] && pNext[HexDigits] <= '9') || ('a' <= pNext[HexDigits] && pNext[HexDigits] <= 'f') || ('A' <= pNext[HexDigits] && pNext[HexDigits] <= 'F'))
			{
				HexDigits++;
			}
			if(HexDigits != 64 && str_startswith(pNext, "cert-sha256=") == nullptr && str_startswith(pNext, "identity-sha256=") == nullptr && str_startswith(pNext, "webpki") == nullptr)
			{
				break;
			}
		}
	}
	str_truncate(pBuffer, BufferSize, pList, pEnd - pList);
	return *pEnd == ',' ? pEnd + 1 : pEnd;
}

bool CConnectTarget::Parse(const char *pAddresses, int NetType)
{
	m_NumAddrs = 0;
	mem_zero(m_aAddrs, sizeof(m_aAddrs));
	m_aHost[0] = '\0';
	m_aFragment[0] = '\0';
	const char *pNextAddr = pAddresses;
	// One address with its host name and fragment.
	char aBuffer[NETADDR_URL_MAXSTRSIZE + 128 + 1 + 160];
	bool OnlySixup = true;
	while((pNextAddr = NextConnectAddress(pNextAddr, aBuffer, sizeof(aBuffer))) != nullptr)
	{
		if(aBuffer[0] == '\0')
		{
			continue;
		}
		NETADDR NextAddr;
		char aHost[128];
		const int UrlParseResult = net_addr_from_url(&NextAddr, aBuffer, aHost, sizeof(aHost));
		// The lookup below starts the address over, so the flags are kept aside.
		bool Sixup = NextAddr.type & NETTYPE_TW7;
		const bool Quic = NextAddr.type & NETTYPE_QUIC;
		const bool WebTransport = NextAddr.type & NETTYPE_WEBTRANSPORT;
		const bool WebSocket = NextAddr.type & NETTYPE_WEBSOCKET;
		const bool WebSocketTls = NextAddr.type & NETTYPE_WEBSOCKET_TLS;
		if(UrlParseResult > 0)
			str_copy(aHost, aBuffer);

		if(net_host_lookup(aHost, &NextAddr, NetType) != 0)
		{
			log_error("client", "could not find address of %s", aHost);
			continue;
		}
		if(m_NumAddrs == (int)std::size(m_aAddrs))
		{
			log_warn("client", "too many connect addresses, ignoring %s", aHost);
			continue;
		}
		if(NextAddr.port == 0)
		{
			NextAddr.port = 8303;
		}
		if(Sixup)
			NextAddr.type |= NETTYPE_TW7;
		else
			OnlySixup = false;
		if(Quic || WebSocket)
		{
			if(Quic)
				NextAddr.type |= NETTYPE_QUIC | (WebTransport ? NETTYPE_WEBTRANSPORT : 0);
			if(WebSocket)
				NextAddr.type |= NETTYPE_WEBSOCKET | (WebSocketTls ? NETTYPE_WEBSOCKET_TLS : 0);
			// The library reads the fragment: the identity as the masterserver
			// lists it, the certificate hashes for a browser.
			const char *pFragment = str_find(aBuffer, "#");
			if(pFragment != nullptr)
			{
				str_copy(m_aFragment, pFragment + 1);
			}
			if(UrlParseResult < 0 && m_NumAddrs == 0)
			{
				// A name, with its port still on it; an IP address
				// would have parsed above.
				const char *pPort = str_rchr(aHost, ':');
				str_truncate(m_aHost, sizeof(m_aHost), aHost, pPort != nullptr ? pPort - aHost : str_length(aHost));
			}
		}

		char aNextAddr[NETADDR_MAXSTRSIZE];
		net_addr_str(&NextAddr, aNextAddr, sizeof(aNextAddr), true);
		log_debug("client", "resolved connect address '%s' to %s", aBuffer, aNextAddr);

		m_aAddrs[m_NumAddrs] = NextAddr;
		m_NumAddrs += 1;
	}
	m_Sixup = OnlySixup;
	return m_NumAddrs > 0;
}

void CConnectTarget::SameServer(const CNetClient &Connection, bool Sixup)
{
	m_aAddrs[0] = *Connection.ServerAddress();
	m_NumAddrs = 1;
	m_Sixup = Sixup;
	str_copy(m_aHost, Connection.ConnectHost());
	m_aFragment[0] = '\0';
	if(Connection.ServerIdentity()[0] != '\0')
	{
		str_format(m_aFragment, sizeof(m_aFragment), "identity-sha256=%s", Connection.ServerIdentity());
	}
}

void CConnectTarget::Start(CNetClient &NetClient) const
{
	NetClient.SetConnectTarget(m_aHost, m_aFragment);
	if(m_Sixup)
	{
		NetClient.Connect7(m_aAddrs, m_NumAddrs);
	}
	else
	{
		NetClient.Connect(m_aAddrs, m_NumAddrs);
	}
}
