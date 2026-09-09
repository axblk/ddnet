/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "network.h"

#ifdef CONF_NETWORKING_QUIC

#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <curl/curl.h>
#include <net/net.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

// The library reports errors instead of ending the process. A call that
// failed is logged and skipped, and one made without a library counts as
// failed; whether the library as a whole is broken and has to be reopened is
// checked where receiving happens, which every frame does.
#define NET_CALL(function, net, ...) \
	((net) == nullptr || CheckNetCall((net), function((net), __VA_ARGS__), #function))

static bool CheckNetCall(CNet *pNet, bool Failed, const char *pFunction)
{
	if(Failed)
	{
		log_error("net", "%s: %s", pFunction, ddnet_net_error(pNet));
	}
	return Failed;
}

static bool AddrFromUrl(const char *pUrl, NETADDR *pAddr)
{
	// TODO: maybe parse URL by ourselves
	CURLU *pHandle = curl_url();
	char *pHostname;
	char *pPort;
	bool Error = false ||
		     curl_url_set(pHandle, CURLUPART_URL, pUrl, CURLU_NON_SUPPORT_SCHEME) ||
		     curl_url_get(pHandle, CURLUPART_HOST, &pHostname, 0) ||
		     curl_url_get(pHandle, CURLUPART_PORT, &pPort, 0);
	curl_url_cleanup(pHandle);
	if(Error)
	{
		return false;
	}
	char aBuf[64];
	str_format(aBuf, sizeof(aBuf), "%s:%s", pHostname, pPort);
	return net_addr_from_str(pAddr, aBuf) == 0;
}

CNetClient::~CNetClient()
{
	Close();
}

bool CNetClient::Open(NETADDR BindAddr)
{
	Close();

	m_BindAddr = BindAddr;
	if(!OpenLibrary())
	{
		return false;
	}

	m_pStun = new CStun(SendRaw, this);

	m_State = NETSTATE_OFFLINE;
	m_PeerId = -1;
	m_NumConnectAddrs = 0;
	m_aErrorString[0] = '\0';
	return true;
}

bool CNetClient::OpenLibrary()
{
	char aBindAddr[NETADDR_MAXSTRSIZE];
	BindAddrStr(m_BindAddr, aBindAddr, sizeof(aBindAddr));

	ddnet_net_ev_new(&m_pNetEvent);
	if(false ||
		ddnet_net_new(&m_pNet) ||
		ddnet_net_set_bindaddr(m_pNet, aBindAddr, str_length(aBindAddr)) ||
		ddnet_net_open(m_pNet))
	{
		log_error("net", "couldn't open net client: %s", ddnet_net_error(m_pNet));
		CloseLibrary();
		return false;
	}
	return true;
}

void CNetClient::CloseLibrary()
{
	if(m_pNet)
	{
		ddnet_net_free(m_pNet);
		m_pNet = nullptr;
	}
	if(m_pNetEvent)
	{
		ddnet_net_ev_free(m_pNetEvent);
		m_pNetEvent = nullptr;
	}
}

void CNetClient::Reopen()
{
	// The library is gone for good, and with it the connection: that ends
	// like any other disconnect, with the library's error as the reason, and
	// the socket is opened anew for the next attempt.
	if(m_pNet != nullptr)
	{
		log_error("net", "reopening the network library: %s", ddnet_net_error(m_pNet));
		if(m_State != NETSTATE_OFFLINE)
		{
			str_format(m_aErrorString, sizeof(m_aErrorString), "Network error: %s", ddnet_net_error(m_pNet));
		}
	}
	m_PeerId = -1;
	m_State = NETSTATE_OFFLINE;
	CloseLibrary();
	if(!OpenLibrary())
	{
		log_error("net", "couldn't reopen the network library, trying again later");
	}
}

void CNetClient::Close()
{
	CloseLibrary();
	if(m_pStun)
	{
		delete m_pStun;
		m_pStun = nullptr;
	}
	m_State = NETSTATE_OFFLINE;
	m_PeerId = -1;
}

void CNetClient::Disconnect(const char *pReason)
{
	if(m_PeerId != -1)
	{
		if(!pReason)
		{
			pReason = "";
		}
		NET_CALL(ddnet_net_close, m_pNet, m_PeerId, pReason, str_length(pReason));
		str_copy(m_aErrorString, pReason);
		m_PeerId = -1;
		m_State = NETSTATE_OFFLINE;
	}
}

void CNetClient::Connect(const NETADDR *pAddr, int NumAddrs)
{
	ConnectImpl(pAddr, NumAddrs, false);
}

void CNetClient::Connect7(const NETADDR *pAddr, int NumAddrs)
{
	ConnectImpl(pAddr, NumAddrs, true);
}

void CNetClient::SetConnectIdentity(const char *pIdentity)
{
	str_copy(m_aConnectIdentity, pIdentity);
}

void CNetClient::ConnectImpl(const NETADDR *pAddr, int NumAddrs, bool Sixup)
{
	Disconnect(nullptr);

	m_NumConnectAddrs = std::min(NumAddrs, (int)std::size(m_aConnectAddrs));
	for(int i = 0; i < m_NumConnectAddrs; i++)
	{
		m_aConnectAddrs[i] = pAddr[i];
	}

	NETADDR Addr = pAddr[0];
	Addr.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	char aAddr[NETADDR_MAXSTRSIZE];
	net_addr_str(&Addr, aAddr, sizeof(aAddr), true);
	char aUrl[192];
	if(pAddr[0].type & NETTYPE_QUIC)
	{
		if(Sixup)
		{
			str_copy(m_aErrorString, "0.7 over QUIC is not supported yet");
			return;
		}
		// The fragment pins the server's identity.
		str_format(aUrl, sizeof(aUrl), "ddnet+quic://%s%s%s", aAddr, m_aConnectIdentity[0] != '\0' ? "#" : "", m_aConnectIdentity);
	}
	else
	{
		str_format(aUrl, sizeof(aUrl), "%s://%s", Sixup ? "tw-0.7+udp" : "tw-0.6+udp", aAddr);
	}
	m_aServerIdentity[0] = '\0';
	uint64_t PeerId;
	if(NET_CALL(ddnet_net_connect, m_pNet, aUrl, str_length(aUrl), &PeerId))
	{
		str_format(m_aErrorString, sizeof(m_aErrorString), "Network error: %s", m_pNet != nullptr ? ddnet_net_error(m_pNet) : "network library not open");
		return;
	}
	m_PeerId = PeerId;
	m_State = NETSTATE_CONNECTING;
	m_aErrorString[0] = '\0';
}

void CNetClient::Update()
{
	// TODO: call timeout stuff
	if(m_pStun)
	{
		m_pStun->Update();
	}
}

void CNetClient::Wait(uint64_t Microseconds)
{
	// Without a library there is nothing to wake up for; sleeping keeps the
	// loop from spinning until Recv has reopened it.
	if(NET_CALL(ddnet_net_wait_timeout, m_pNet, Microseconds * 1000))
	{
		std::this_thread::sleep_for(std::chrono::microseconds(Microseconds));
	}
}

int CNetClient::Flush()
{
	if(m_PeerId == -1)
	{
		return 0;
	}
	NET_CALL(ddnet_net_flush, m_pNet, m_PeerId);
	return 0;
}

void CNetClient::ResetErrorString()
{
	dbg_assert(m_State == NETSTATE_OFFLINE, "can only reset error string while having one");
	m_aErrorString[0] = '\0';
}

int CNetClient::Recv(CNetChunk *pChunk, SECURITY_TOKEN *pResponseToken, bool Sixup)
{
	*pResponseToken = NET_SECURITY_TOKEN_UNKNOWN;

	if(m_pNet == nullptr || ddnet_net_is_broken(m_pNet))
	{
		Reopen();
		if(m_pNet == nullptr)
		{
			return 0;
		}
	}
	while(true)
	{
		// Keep space for null termination.
		if(NET_CALL(ddnet_net_recv, m_pNet, m_aBuffer, sizeof(m_aBuffer) - 1, m_pNetEvent))
		{
			return 0;
		}
		switch(ddnet_net_ev_kind(m_pNetEvent))
		{
		case DDNET_NET_EV_NONE:
			return 0;
		case DDNET_NET_EV_CONNECT:
		{
			const uint64_t PeerId = ddnet_net_ev_connect_peer_index(m_pNetEvent);
			if((int64_t)PeerId != m_PeerId)
			{
				continue;
			}
			const char *pAddr;
			size_t AddrLen;
			ddnet_net_ev_connect_addr(m_pNetEvent, &pAddr, &AddrLen);
			NETADDR Addr;
			if(!AddrFromUrl(pAddr, &Addr))
			{
				static const char UNRECOGNIZED_ADDR[] = "Unrecognized address";
				NET_CALL(ddnet_net_close, m_pNet, PeerId, UNRECOGNIZED_ADDR, sizeof(UNRECOGNIZED_ADDR) - 1);
				continue;
			}
			if(str_startswith(pAddr, "tw-0.7+udp://"))
			{
				Addr.type |= NETTYPE_TW7;
			}
			else if(str_startswith(pAddr, "ddnet+quic://"))
			{
				Addr.type |= NETTYPE_QUIC;
				// The identity the server showed, to connect the dummy with.
				const char *pFragment = str_find(pAddr, "#");
				str_copy(m_aServerIdentity, pFragment != nullptr ? pFragment + 1 : "");
			}
			m_ServerAddress = Addr;
			m_State = NETSTATE_ONLINE;
		}
		break;
		case DDNET_NET_EV_DISCONNECT:
		{
			const uint64_t PeerId = ddnet_net_ev_disconnect_peer_index(m_pNetEvent);
			if((int64_t)PeerId != m_PeerId)
			{
				continue;
			}
			m_PeerId = -1;
			m_State = NETSTATE_OFFLINE;
			const size_t ReasonLen = std::min(ddnet_net_ev_disconnect_reason_len(m_pNetEvent), sizeof(m_aErrorString) - 1);
			mem_copy(m_aErrorString, m_aBuffer, ReasonLen);
			m_aErrorString[ReasonLen] = '\0';
		}
		break;
		case DDNET_NET_EV_CHUNK:
		{
			const uint64_t PeerId = ddnet_net_ev_chunk_peer_index(m_pNetEvent);
			if((int64_t)PeerId != m_PeerId)
			{
				continue;
			}
			mem_zero(pChunk, sizeof(*pChunk));
			pChunk->m_ClientId = 0;
			pChunk->m_Address = m_ServerAddress;
			pChunk->m_Flags = 0;
			if(!ddnet_net_ev_chunk_is_unreliable(m_pNetEvent))
			{
				pChunk->m_Flags |= NET_CHUNKFLAG_VITAL;
			}
			pChunk->m_DataSize = ddnet_net_ev_chunk_len(m_pNetEvent);
			pChunk->m_pData = m_aBuffer;
		}
			return 1;
		case DDNET_NET_EV_CONNLESS_CHUNK:
		{
			const char *pAddr;
			size_t AddrLen;
			ddnet_net_ev_connless_chunk_addr(m_pNetEvent, &pAddr, &AddrLen);
			NETADDR Addr;
			const ENetConnless Kind = NetConnlessAddr(pAddr, &Addr);
			if(Kind == ENetConnless::RAW)
			{
				if(m_pStun)
				{
					m_pStun->OnPacket(Addr, m_aBuffer, ddnet_net_ev_connless_chunk_len(m_pNetEvent));
				}
				continue;
			}
			if(Kind == ENetConnless::NONE)
			{
				continue;
			}
			const bool ConnlessSixup = Kind == ENetConnless::TW07;
			mem_zero(pChunk, sizeof(*pChunk));
			pChunk->m_ClientId = -1;
			pChunk->m_Address = Addr;
			pChunk->m_Flags = NETSENDFLAG_CONNLESS;
			pChunk->m_DataSize = ddnet_net_ev_connless_chunk_len(m_pNetEvent);
			pChunk->m_pData = m_aBuffer;
			if(ddnet_net_ev_connless_chunk_extra(m_pNetEvent, &pChunk->m_aExtraData))
			{
				pChunk->m_Flags |= NETSENDFLAG_EXTENDED;
			}
			uint32_t Token;
			if(ConnlessSixup && ddnet_net_ev_connless_chunk_token7(m_pNetEvent, &Token))
			{
				*pResponseToken = Token;
			}
		}
			return 1;
		}
	}
}

int CNetClient::Send(CNetChunk *pChunk)
{
	pChunk->AssertSizeSanity();

	if(pChunk->m_Flags & NETSENDFLAG_CONNLESS)
	{
		NetSendConnless(m_pNet, pChunk);
		return 0;
	}

	if(m_PeerId == -1)
	{
		return -1;
	}
	dbg_assert(pChunk->m_ClientId == 0, "erroneous client id");
	NET_CALL(ddnet_net_send_chunk, m_pNet, m_PeerId, (const unsigned char *)pChunk->m_pData, pChunk->m_DataSize, (pChunk->m_Flags & NETSENDFLAG_VITAL) == 0);
	if((pChunk->m_Flags & NETSENDFLAG_FLUSH) != 0)
	{
		NET_CALL(ddnet_net_flush, m_pNet, m_PeerId);
	}
	return 0;
}

int CNetClient::State() const
{
	return m_State;
}

bool CNetClient::GotProblems(int64_t MaxLatency) const
{
	// TODO: the network library does not report the last receive time yet
	return false;
}

const char *CNetClient::ErrorString() const
{
	if(m_State == NETSTATE_OFFLINE)
	{
		return m_aErrorString;
	}
	return "";
}

bool CNetClient::SendRaw(void *pUser, const NETADDR *pAddr, const void *pData, int Size)
{
	CNetClient *pThis = (CNetClient *)pUser;
	if(pThis->m_pNet == nullptr)
	{
		return false;
	}
	NETADDR Addr = *pAddr;
	Addr.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	char aHost[NETADDR_MAXSTRSIZE];
	net_addr_str(&Addr, aHost, sizeof(aHost), true);
	char aUrl[128];
	str_format(aUrl, sizeof(aUrl), "udp://%s", aHost);
	return !NET_CALL(ddnet_net_send_connless_chunk, pThis->m_pNet, aUrl, str_length(aUrl), (const unsigned char *)pData, Size);
}

void CNetClient::FeedStunServer(NETADDR StunServer)
{
	if(m_pStun)
	{
		m_pStun->FeedStunServer(StunServer);
	}
}

void CNetClient::RefreshStun()
{
	if(m_pStun)
	{
		m_pStun->Refresh();
	}
}

CONNECTIVITY CNetClient::GetConnectivity(int NetType, NETADDR *pGlobalAddr)
{
	if(!m_pStun)
	{
		return CONNECTIVITY::UNKNOWN;
	}
	return m_pStun->GetConnectivity(NetType, pGlobalAddr);
}

int CNetClient::NetType()
{
	// unimplemented
	return NETTYPE_IPV4 | NETTYPE_IPV6;
}

bool CNetClient::SocketIsBroken() const
{
	// the network library reports errors through ErrorString()
	return false;
}

const NETADDR *CNetClient::ServerAddress() const
{
	return &m_ServerAddress;
}

void CNetClient::ConnectAddresses(const NETADDR **ppAddrs, int *pNumAddrs) const
{
	*ppAddrs = m_aConnectAddrs;
	*pNumAddrs = m_NumConnectAddrs;
}

#endif // CONF_NETWORKING_QUIC
