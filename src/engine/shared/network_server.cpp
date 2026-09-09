/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "network.h"

#ifdef CONF_NETWORKING_QUIC

#include "config.h"
#include "netban.h"

#include <base/dbg.h>
#include <base/hash_ctxt.h>
#include <base/log.h>
#include <base/math.h>
#include <base/net.h>
#include <base/secure.h>
#include <base/str.h>

#include <curl/curl.h>
#include <net/net.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

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

// The library listens on one socket. Bound to IPv6 it takes IPv4 as well, so
// an address that is not pinned to one family becomes the IPv6 wildcard.
void BindAddrStr(const NETADDR &BindAddr, char *pBuffer, size_t BufferSize)
{
	if((BindAddr.type & NETTYPE_IPV4) && (BindAddr.type & NETTYPE_IPV6))
	{
		str_format(pBuffer, BufferSize, "[::]:%d", BindAddr.port);
		return;
	}
	NETADDR Addr = BindAddr;
	Addr.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	net_addr_str(&Addr, pBuffer, BufferSize, true);
}

// The address of a connect event names the protocol in its scheme.
static bool UrlIsSixup(const char *pUrl)
{
	return str_startswith(pUrl, "tw-0.7+udp://") != nullptr;
}

void CNetServer::CPeer::Reset()
{
	m_State = STATE_NONE;
	m_Id = -1;
	m_TimeoutProtected = false;
	mem_zero(&m_Address, sizeof(m_Address));
	m_aAddressStr[0] = '\0';
	m_aAddressStrNoPort[0] = '\0';
}

void CNetServer::CPeer::SetAddress(const NETADDR &Addr)
{
	m_Address = Addr;
	net_addr_str(&m_Address, m_aAddressStr.data(), m_aAddressStr.size(), true);
	net_addr_str(&m_Address, m_aAddressStrNoPort.data(), m_aAddressStrNoPort.size(), false);
}

CNetServer::~CNetServer()
{
	Close();
}

void CNetServer::SetIdentity(const unsigned char (&aSeed)[32])
{
	mem_copy(m_aIdentity, aSeed, sizeof(m_aIdentity));
	m_HasIdentity = true;
}

bool CNetServer::Open(NETADDR BindAddr, CNetBan *pNetBan, int MaxClients, int MaxClientsPerIp)
{
	m_pNetBan = pNetBan;
	m_Address = BindAddr;
	m_MaxClients = std::clamp(MaxClients, 1, (int)NET_MAX_CLIENTS);
	m_MaxClientsPerIp = std::clamp(MaxClientsPerIp, 1, (int)NET_MAX_CLIENTS);

	secure_random_fill(m_aSecurityTokenSeed, sizeof(m_aSecurityTokenSeed));

	for(auto &Peer : m_aPeers)
	{
		Peer.Reset();
	}
	return OpenLibrary();
}

bool CNetServer::OpenLibrary()
{
	char aBindAddr[NETADDR_MAXSTRSIZE];
	BindAddrStr(m_Address, aBindAddr, sizeof(aBindAddr));

	ddnet_net_ev_new(&m_pNetEvent);
	if(false ||
		ddnet_net_new(&m_pNet) ||
		ddnet_net_set_bindaddr(m_pNet, aBindAddr, str_length(aBindAddr)) ||
		(m_HasIdentity && ddnet_net_set_identity(m_pNet, &m_aIdentity)) ||
		ddnet_net_set_accept_connections(m_pNet, true) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_TW06, g_Config.m_SvLegacyUdp != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_TW07, g_Config.m_SvLegacyUdp != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_QUIC, g_Config.m_SvQuic != 0) ||
		ddnet_net_open(m_pNet))
	{
		log_error("net", "couldn't open net server: %s", ddnet_net_error(m_pNet));
		Close();
		return false;
	}
	return true;
}

// The client slot of a peer, or -1 if it has none.
int CNetServer::PeerClientId(uint64_t PeerId)
{
	void *pUserdata;
	if(NET_CALL(ddnet_net_userdata, m_pNet, PeerId, &pUserdata) || (uintptr_t)pUserdata == (uintptr_t)-1)
	{
		return -1;
	}
	const int ClientId = (uintptr_t)pUserdata;
	dbg_assert(m_aPeers[ClientId].m_Id == PeerId, "invalid peer mapping");
	return ClientId;
}

void CNetServer::Reopen()
{
	// The library is gone for good, and so are its connections: the game
	// hears of each of them the way it hears of a timeout, then the socket
	// is opened anew on the same address.
	if(m_pNet != nullptr)
	{
		log_error("net", "reopening the network library: %s", ddnet_net_error(m_pNet));
	}
	for(int ClientId = 0; ClientId < MaxClients(); ClientId++)
	{
		if(m_aPeers[ClientId].m_State == CPeer::STATE_NONE)
		{
			continue;
		}
		m_aPeers[ClientId].Reset();
		m_aFlushPending[ClientId] = false;
		if(m_pfnDelClient)
		{
			m_pfnDelClient(ClientId, "Network error, please reconnect", m_pUser);
		}
	}
	Close();
	if(!OpenLibrary())
	{
		log_error("net", "couldn't reopen the network library, trying again later");
	}
}

int CNetServer::SetCallbacks(NETFUNC_NEWCLIENT pfnNewClient, NETFUNC_DELCLIENT pfnDelClient, void *pUser)
{
	m_pfnNewClient = pfnNewClient;
	m_pfnDelClient = pfnDelClient;
	m_pUser = pUser;
	return 0;
}

int CNetServer::SetCallbacks(NETFUNC_NEWCLIENT pfnNewClient, NETFUNC_NEWCLIENT_NOAUTH pfnNewClientNoAuth, NETFUNC_CLIENTREJOIN pfnClientRejoin, NETFUNC_DELCLIENT pfnDelClient, void *pUser)
{
	m_pfnNewClientNoAuth = pfnNewClientNoAuth;
	m_pfnClientRejoin = pfnClientRejoin;
	return SetCallbacks(pfnNewClient, pfnDelClient, pUser);
}

void CNetServer::Close()
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

void CNetServer::Drop(int ClientId, const char *pReason)
{
	const uint64_t PeerId = m_aPeers[ClientId].m_Id;
	if(PeerId == (uint64_t)-1)
	{
		return;
	}

	if(m_pfnDelClient)
	{
		m_pfnDelClient(ClientId, pReason, m_pUser);
	}

	// Reset peer mapping.
	m_aPeers[ClientId].Reset();
	NET_CALL(ddnet_net_set_userdata, m_pNet, PeerId, (void *)(uintptr_t)-1);

	// Close the connection.
	NET_CALL(ddnet_net_close, m_pNet, PeerId, pReason, str_length(pReason));
}

void CNetServer::Update()
{
	// TODO: detect timeouts and honor timeout protection
}

void CNetServer::Wait(uint64_t Microseconds)
{
	// Without a library there is nothing to wake up for; sleeping keeps the
	// loop from spinning until Recv has reopened it.
	if(NET_CALL(ddnet_net_wait_timeout, m_pNet, Microseconds * 1000))
	{
		std::this_thread::sleep_for(std::chrono::microseconds(Microseconds));
	}
}

void CNetServer::Flush(int ClientId)
{
	if(m_aPeers[ClientId].m_Id == (uint64_t)-1)
	{
		return;
	}
	NET_CALL(ddnet_net_flush, m_pNet, m_aPeers[ClientId].m_Id);
}

void CNetServer::EndFlushBatch()
{
	m_FlushBatch = false;
	for(int ClientId = 0; ClientId < MaxClients(); ClientId++)
	{
		if(!m_aFlushPending[ClientId])
			continue;
		m_aFlushPending[ClientId] = false;
		// The client may have been dropped while the batch was open.
		if(m_aPeers[ClientId].m_Id != (uint64_t)-1)
			Flush(ClientId);
	}
}

int CNetServer::Recv(CNetChunk *pChunk, SECURITY_TOKEN *pResponseToken)
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

			char aBanReason[256];
			if(NetBan() && NetBan()->IsBanned(&Addr, aBanReason, sizeof(aBanReason)))
			{
				NET_CALL(ddnet_net_close, m_pNet, PeerId, aBanReason, str_length(aBanReason));
				continue;
			}

			const bool Sixup = UrlIsSixup(pAddr);
			if(Sixup && !g_Config.m_SvSixup)
			{
				static const char NO_SIXUP[] = "0.7 connections are not accepted at this time";
				NET_CALL(ddnet_net_close, m_pNet, PeerId, NO_SIXUP, sizeof(NO_SIXUP) - 1);
				continue;
			}

			uint32_t NumConnected = 0;
			NET_CALL(ddnet_net_num_peers_in_bucket, m_pNet, pAddr, AddrLen, &NumConnected);
			if((int)NumConnected > m_MaxClientsPerIp)
			{
				char aBuf[128];
				str_format(aBuf, sizeof(aBuf), "Only %d players with the same IP are allowed", m_MaxClientsPerIp);
				NET_CALL(ddnet_net_close, m_pNet, PeerId, aBuf, str_length(aBuf));
				continue;
			}

			int ClientId = -1;
			for(int i = 0; i < MaxClients(); i++)
			{
				if(m_aPeers[m_NextClientId].m_Id == (uint64_t)-1)
				{
					ClientId = m_NextClientId;
					m_NextClientId = (m_NextClientId + 1) % MaxClients();
					break;
				}
				m_NextClientId = (m_NextClientId + 1) % MaxClients();
			}
			if(ClientId == -1)
			{
				static const char FULL[] = "This server is full";
				NET_CALL(ddnet_net_close, m_pNet, PeerId, FULL, sizeof(FULL) - 1);
				continue;
			}

			m_aPeers[ClientId].m_State = CPeer::STATE_CONNECTED;
			m_aPeers[ClientId].m_Id = PeerId;
			m_aPeers[ClientId].SetAddress(Addr);
			NET_CALL(ddnet_net_set_userdata, m_pNet, PeerId, (void *)(uintptr_t)ClientId);
			if(m_pfnNewClient)
			{
				m_pfnNewClient(ClientId, m_pUser, Sixup);
			}
		}
		break;
		case DDNET_NET_EV_DISCONNECT:
		{
			const uint64_t PeerId = ddnet_net_ev_disconnect_peer_index(m_pNetEvent);
			const int ClientId = PeerClientId(PeerId);
			if(ClientId < 0)
			{
				continue;
			}

			// The peer mapping has to be cleared before the callback, sends from
			// within the callback would otherwise go to a closed connection.
			m_aPeers[ClientId].m_Id = -1;
			m_aPeers[ClientId].m_State = CPeer::STATE_NONE;
			m_aFlushPending[ClientId] = false;

			if(m_pfnDelClient)
			{
				m_aBuffer[ddnet_net_ev_disconnect_reason_len(m_pNetEvent)] = 0;
				const char *pReason = ddnet_net_ev_disconnect_is_remote(m_pNetEvent) ? "" : (char *)m_aBuffer;
				m_pfnDelClient(ClientId, pReason, m_pUser);
			}
		}
		break;
		case DDNET_NET_EV_CHUNK:
		{
			const uint64_t PeerId = ddnet_net_ev_chunk_peer_index(m_pNetEvent);
			const int ClientId = PeerClientId(PeerId);
			if(ClientId < 0)
			{
				continue;
			}
			mem_zero(pChunk, sizeof(*pChunk));
			pChunk->m_ClientId = ClientId;
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
			if(Kind != ENetConnless::TW06 && Kind != ENetConnless::TW07)
			{
				continue;
			}
			const bool Sixup = Kind == ENetConnless::TW07;
			char aBanReason[256];
			if(NetBan() && NetBan()->IsBanned(&Addr, aBanReason, sizeof(aBanReason)))
			{
				continue;
			}
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
			if(Sixup && ddnet_net_ev_connless_chunk_token7(m_pNetEvent, &Token))
			{
				*pResponseToken = Token;
			}
		}
			return 1;
		}
	}
}

int CNetServer::Send(CNetChunk *pChunk)
{
	pChunk->AssertSizeSanity();

	if(pChunk->m_Flags & NETSENDFLAG_CONNLESS)
	{
		NetSendConnless(m_pNet, pChunk);
		return 0;
	}

	dbg_assert(
		pChunk->m_ClientId >= 0 && pChunk->m_ClientId < MaxClients(),
		"Invalid pChunk->m_ClientId: %d",
		pChunk->m_ClientId);

	const uint64_t PeerId = m_aPeers[pChunk->m_ClientId].m_Id;
	if(PeerId == (uint64_t)-1)
	{
		// The client is not connected (anymore), drop the chunk.
		return -1;
	}
	NET_CALL(ddnet_net_send_chunk, m_pNet, PeerId, (const unsigned char *)pChunk->m_pData, pChunk->m_DataSize, (pChunk->m_Flags & NETSENDFLAG_VITAL) == 0);
	if((pChunk->m_Flags & NETSENDFLAG_FLUSH) != 0)
	{
		if(m_FlushBatch)
			m_aFlushPending[pChunk->m_ClientId] = true;
		else
			Flush(pChunk->m_ClientId);
	}
	return 0;
}

void CNetServer::SendConnlessSixup(const NETADDR *pAddr, const void *pData, int DataSize, SECURITY_TOKEN ResponseToken)
{
	// The library remembered the token when the packet came in.
	(void)ResponseToken;
	CNetChunk Chunk;
	Chunk.m_ClientId = -1;
	Chunk.m_Address = *pAddr;
	Chunk.m_Address.type |= NETTYPE_TW7;
	Chunk.m_Flags = NETSENDFLAG_CONNLESS;
	Chunk.m_DataSize = DataSize;
	Chunk.m_pData = pData;
	NetSendConnless(m_pNet, &Chunk);
}

void CNetServer::SetMaxClientsPerIp(int Max)
{
	m_MaxClientsPerIp = std::clamp<int>(Max, 1, NET_MAX_CLIENTS);
}

bool CNetServer::HasErrored(int ClientId)
{
	return m_aPeers[ClientId].m_State == CPeer::STATE_TIMEOUT ||
	       m_aPeers[ClientId].m_State == CPeer::STATE_TIMEOUT_CLEARED;
}

void CNetServer::ResumeOldConnection(int ClientId, int OrigId)
{
	dbg_assert(HasErrored(ClientId), "client did not time out");
	dbg_assert(m_aPeers[ClientId].m_Id == (uint64_t)-1, "invalid peer id");
	m_aPeers[ClientId] = m_aPeers[OrigId];
	m_aPeers[OrigId].Reset();
	NET_CALL(ddnet_net_set_userdata, m_pNet, m_aPeers[ClientId].m_Id, (void *)(uintptr_t)ClientId);
}

void CNetServer::IgnoreTimeouts(int ClientId)
{
	dbg_assert(m_aPeers[ClientId].m_State != CPeer::STATE_NONE, "invalid client id");
	m_aPeers[ClientId].m_TimeoutProtected = true;
}

void CNetServer::ResetErrorString(int ClientId)
{
	dbg_assert(m_aPeers[ClientId].m_State == CPeer::STATE_TIMEOUT, "invalid client state");
	m_aPeers[ClientId].m_State = CPeer::STATE_TIMEOUT_CLEARED;
}

const char *CNetServer::ErrorString(int ClientId)
{
	if(m_aPeers[ClientId].m_State == CPeer::STATE_TIMEOUT)
	{
		return "timeout";
	}
	return "";
}

const NETADDR *CNetServer::ClientAddr(int ClientId) const
{
	dbg_assert(m_aPeers[ClientId].m_State != CPeer::STATE_NONE, "invalid client id");
	return &m_aPeers[ClientId].m_Address;
}

const std::array<char, NETADDR_MAXSTRSIZE> &CNetServer::ClientAddrString(int ClientId, bool IncludePort) const
{
	dbg_assert(m_aPeers[ClientId].m_State != CPeer::STATE_NONE, "invalid client id");
	return IncludePort ? m_aPeers[ClientId].m_aAddressStr : m_aPeers[ClientId].m_aAddressStrNoPort;
}

bool CNetServer::HasSecurityToken(int ClientId) const
{
	// unimplemented
	return true;
}

NETSOCKET CNetServer::Socket() const
{
	// unimplemented
	return nullptr;
}

int CNetServer::NetType() const
{
	// unimplemented
	return NETTYPE_IPV4 | NETTYPE_IPV6;
}
SECURITY_TOKEN CNetServer::GetGlobalToken()
{
	// The library hands out the 0.7 tokens, so the one the masterserver
	// challenges with has to be the library's. It changes when the library is
	// reopened after an error, until the next registration.
	uint32_t Token;
	if(NET_CALL(ddnet_net_global_token7, m_pNet, &Token))
	{
		return 1;
	}
	return Token;
}

SECURITY_TOKEN CNetServer::GetToken(const NETADDR &Addr)
{
	SHA256_CTX Sha256;
	sha256_init(&Sha256);
	sha256_update(&Sha256, (unsigned char *)m_aSecurityTokenSeed, sizeof(m_aSecurityTokenSeed));
	sha256_update(&Sha256, (unsigned char *)&Addr, 20); // omit port, bad idea!

	SECURITY_TOKEN SecurityToken = ToSecurityToken(sha256_finish(&Sha256).data);

	if(SecurityToken == NET_SECURITY_TOKEN_UNKNOWN ||
		SecurityToken == NET_SECURITY_TOKEN_UNSUPPORTED)
		SecurityToken = 1;

	return SecurityToken;
}

SECURITY_TOKEN CNetServer::GetVanillaToken(const NETADDR &Addr)
{
	// vanilla token/gametick shouldn't be negative
	return absolute(GetToken(Addr));
}

#endif // CONF_NETWORKING_QUIC
