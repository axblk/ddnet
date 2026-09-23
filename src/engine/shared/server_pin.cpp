#include "server_pin.h"

#include <base/net.h>
#include <base/str.h>

// QUIC and `wss://` show the identity to the client; WebTransport shows
// browsers a certificate of its own, plain WebSockets show none.
static bool PinsIdentity(const NETADDR &Addr)
{
	return (Addr.type & NETTYPE_WEBTRANSPORT) == 0 && (Addr.type & (NETTYPE_QUIC | NETTYPE_WEBSOCKET_TLS)) != 0;
}

void CServerPin::Reset()
{
	m_aIdentity[0] = '\0';
	m_aWebTransport[0] = '\0';
}

void CServerPin::AddFragment(const NETADDR &Addr, const char *pFragment)
{
	if((Addr.type & NETTYPE_WEBTRANSPORT) != 0)
	{
		if(m_aWebTransport[0] == '\0' && (str_startswith(pFragment, "cert-sha256=") != nullptr || str_comp(pFragment, "webpki") == 0))
		{
			str_copy(m_aWebTransport, pFragment);
		}
	}
	else if(PinsIdentity(Addr) && m_aIdentity[0] == '\0')
	{
		const char *pHex = str_startswith(pFragment, "spki-sha256=");
		if(pHex != nullptr && str_length(pHex) == 64 && str_isallnum_hex(pHex))
		{
			str_copy(m_aIdentity, pHex);
		}
	}
}

void CServerPin::Merge(const CServerPin &Other)
{
	if(m_aIdentity[0] == '\0')
	{
		str_copy(m_aIdentity, Other.m_aIdentity);
	}
	if(m_aWebTransport[0] == '\0')
	{
		str_copy(m_aWebTransport, Other.m_aWebTransport);
	}
}

void CServerPin::Fragment(const NETADDR &Addr, char *pBuffer, int BufferSize) const
{
	pBuffer[0] = '\0';
	if((Addr.type & NETTYPE_WEBTRANSPORT) != 0)
	{
		str_copy(pBuffer, m_aWebTransport, BufferSize);
	}
	else if(PinsIdentity(Addr) && m_aIdentity[0] != '\0')
	{
		str_format(pBuffer, BufferSize, "spki-sha256=%s", m_aIdentity);
	}
}

void CServerPin::AddressUrl(const NETADDR &Addr, const char *pHost, char *pBuffer, int BufferSize) const
{
	net_addr_url_str(&Addr, pBuffer, BufferSize, true);
	if(pHost != nullptr && pHost[0] != '\0')
	{
		// The scheme stays, the name takes the place of the IP address.
		const char *pSchemeEnd = str_find(pBuffer, "://");
		const int Start = pSchemeEnd != nullptr ? pSchemeEnd - pBuffer + 3 : 0;
		str_format(pBuffer + Start, BufferSize - Start, "%s:%d", pHost, Addr.port);
	}
	char aFragment[sizeof(m_aWebTransport)];
	Fragment(Addr, aFragment, sizeof(aFragment));
	if(aFragment[0] != '\0')
	{
		str_append(pBuffer, "#", BufferSize);
		str_append(pBuffer, aFragment, BufferSize);
	}
}

bool CServerPin::SignedForName(const NETADDR &Addr) const
{
	return (Addr.type & NETTYPE_WEBSOCKET_TLS) != 0 || ((Addr.type & NETTYPE_WEBTRANSPORT) != 0 && str_comp(m_aWebTransport, "webpki") == 0);
}
