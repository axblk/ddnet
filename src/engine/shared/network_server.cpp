/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "config.h"
#include "netban.h"
#include "network.h"

#include <base/dbg.h>
#include <base/hash_ctxt.h>
#include <base/log.h>
#include <base/math.h>
#include <base/net.h>
#include <base/secure.h>
#include <base/str.h>

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

// The plain address of a peer's URL, without the scheme's flags: bans and
// the address shown for the client do not care about the protocol.
static bool AddrFromUrl(const char *pUrl, NETADDR *pAddr)
{
	if(net_addr_from_url(pAddr, pUrl, nullptr, 0) != 0)
	{
		return false;
	}
	pAddr->type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	return true;
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

// Whether the library lost the peer to silence rather than to a close or
// an error of its own; the reasons are the library's.
static bool IsTimeoutReason(const char *pReason)
{
	return str_comp(pReason, "Timeout") == 0 || str_startswith(pReason, "Too weak connection") != nullptr;
}

void CNetServer::CPeer::Reset()
{
	m_State = STATE_NONE;
	m_Id = -1;
	m_TimeoutProtected = false;
	m_TimeoutAt = 0;
	m_Quic = false;
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

void CNetServer::SetTlsFiles(const char *pCert, const char *pKey)
{
	str_copy(m_aTlsCert, pCert);
	str_copy(m_aTlsKey, pKey);
}

bool CNetServer::CertificateSha256(bool Next, SHA256_DIGEST *pSha256)
{
	return m_pNet != nullptr && ddnet_net_certificate_sha256(m_pNet, Next, &pSha256->data);
}

bool CNetServer::Identity(unsigned char (&aIdentity)[32])
{
	return m_pNet != nullptr && ddnet_net_identity(m_pNet, &aIdentity);
}

bool CNetServer::AcceptsWebsockets()
{
	bool Accepts = false;
	return m_pNet != nullptr && !ddnet_net_accepts_protocol(m_pNet, DDNET_NET_PROTOCOL_WEBSOCKET, &Accepts) && Accepts;
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
		ddnet_net_set_timeout(m_pNet, g_Config.m_ConnTimeout) ||
		ddnet_net_set_key_log(m_pNet, g_Config.m_DbgTlsKeyLog != 0) ||
		(m_aTlsCert[0] != '\0' && ddnet_net_set_tls_files(m_pNet, m_aTlsCert, str_length(m_aTlsCert), m_aTlsKey, str_length(m_aTlsKey))) ||
		ddnet_net_set_accept_connections(m_pNet, true) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_TW06, g_Config.m_SvLegacyUdp != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_TW07, g_Config.m_SvLegacyUdp != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_QUIC, g_Config.m_SvQuic != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_WEBTRANSPORT, g_Config.m_SvWebtransport != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_WEBSOCKET, g_Config.m_SvWebsocket != 0) ||
		ApplyLimits() ||
		ddnet_net_open(m_pNet))
	{
		log_error("net", "couldn't open net server: %s", ddnet_net_error(m_pNet));
		Close();
		return false;
	}
	if(!std::ranges::all_of(m_Maps, [this](const auto &Entry) { return SetMapImpl(Entry.first, Entry.second); }))
	{
		Close();
		return false;
	}
	return true;
}

bool CNetServer::SetMapImpl(int MapId, const CMap &Map)
{
	if(ddnet_net_set_map(m_pNet, MapId, (const uint8_t *)Map.m_aName, str_length(Map.m_aName), Map.m_Crc, &Map.m_Sha256.data, Map.m_vData.data(), Map.m_vData.size()))
	{
		log_error("net", "couldn't set map %d: %s", MapId, ddnet_net_error(m_pNet));
		return false;
	}
	return true;
}

void CNetServer::SetMap(int MapId, const char *pName, unsigned Crc, const SHA256_DIGEST &Sha256, const void *pData, unsigned Size)
{
	CMap &Map = m_Maps[MapId];
	str_copy(Map.m_aName, pName);
	Map.m_Crc = Crc;
	Map.m_Sha256 = Sha256;
	Map.m_vData.assign((const unsigned char *)pData, (const unsigned char *)pData + Size);
	if(m_pNet != nullptr)
	{
		SetMapImpl(MapId, Map);
	}
}

bool CNetServer::SendMap(int ClientId, int MapId)
{
	if(m_pNet == nullptr || m_aPeers[ClientId].m_State == CPeer::STATE_NONE || !m_aPeers[ClientId].m_Quic)
	{
		return false;
	}
	return !NET_CALL(ddnet_net_send_map, m_pNet, m_aPeers[ClientId].m_Id, MapId);
}

void CNetServer::CancelMap(int ClientId)
{
	if(m_pNet == nullptr || m_aPeers[ClientId].m_State == CPeer::STATE_NONE || !m_aPeers[ClientId].m_Quic)
	{
		return;
	}
	NET_CALL(ddnet_net_cancel_map, m_pNet, m_aPeers[ClientId].m_Id);
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
	m_Limits = CLimits();
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
		// A slot waiting for its timeout code has no peer in the library.
		if(HasErrored(ClientId))
		{
			if(m_pfnDelClient)
			{
				m_pfnDelClient(ClientId, pReason, m_pUser);
			}
			m_aPeers[ClientId].Reset();
		}
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

bool CNetServer::ApplyLimits()
{
	if(m_Limits.m_Connlimit != g_Config.m_SvConnlimit || m_Limits.m_ConnlimitTime != g_Config.m_SvConnlimitTime)
	{
		if(ddnet_net_set_connlimit(m_pNet, g_Config.m_SvConnlimit, g_Config.m_SvConnlimitTime))
		{
			return true;
		}
		m_Limits.m_Connlimit = g_Config.m_SvConnlimit;
		m_Limits.m_ConnlimitTime = g_Config.m_SvConnlimitTime;
	}
	if(m_Limits.m_MaxPacketsPerRecv != g_Config.m_SvMaxPacketsPerRecv)
	{
		if(ddnet_net_set_max_packets_per_recv(m_pNet, g_Config.m_SvMaxPacketsPerRecv))
		{
			return true;
		}
		m_Limits.m_MaxPacketsPerRecv = g_Config.m_SvMaxPacketsPerRecv;
	}
	if(m_Limits.m_ResendRequestsPerSecond != g_Config.m_ConnResendRequestsPerSecond)
	{
		if(ddnet_net_set_resend_requests_per_second(m_pNet, g_Config.m_ConnResendRequestsPerSecond))
		{
			return true;
		}
		m_Limits.m_ResendRequestsPerSecond = g_Config.m_ConnResendRequestsPerSecond;
	}
	return false;
}

void CNetServer::CloseBanned(uint64_t PeerId, const char *pReason)
{
	if(g_Config.m_SvBanRepliesPerSecond != 0 && m_NumBanReplies >= g_Config.m_SvBanRepliesPerSecond)
	{
		pReason = "";
	}
	else
	{
		m_NumBanReplies++;
	}
	NET_CALL(ddnet_net_close, m_pNet, PeerId, pReason, str_length(pReason));
}

void CNetServer::Update()
{
	CNetBase::UpdateLogLevel();
	if(m_pNet != nullptr && ApplyLimits())
	{
		log_error("net", "applying the limits: %s", ddnet_net_error(m_pNet));
	}
	const int64_t Now = time_get();
	if(Now > m_BanRepliesStart + time_freq())
	{
		m_BanRepliesStart = Now;
		m_NumBanReplies = 0;
	}
	// A protected slot outlives its timeout by `conn_timeout_protection`
	// after the last packet, which came `conn_timeout` before the timeout.
	const int64_t Protection = time_freq() * std::max(0, g_Config.m_ConnTimeoutProtection - g_Config.m_ConnTimeout);
	for(int i = 0; i < MaxClients(); i++)
	{
		if(HasErrored(i) && Now - m_aPeers[i].m_TimeoutAt > Protection)
		{
			Drop(i, "Timeout Protection over");
		}
	}
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
				CloseBanned(PeerId, aBanReason);
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
			// Peers over the game's own wire protocol take the map on a stream.
			m_aPeers[ClientId].m_Quic = str_startswith(pAddr, "ddnet+") != nullptr;
			m_aPeers[ClientId].SetAddress(Addr);
			NET_CALL(ddnet_net_set_userdata, m_pNet, PeerId, (void *)(uintptr_t)ClientId);
			if(m_pfnNewClient)
			{
				m_pfnNewClient(ClientId, m_pUser, Sixup);
			}
		}
		break;
		case DDNET_NET_EV_MOVED:
		{
			const uint64_t PeerId = ddnet_net_ev_moved_peer_index(m_pNetEvent);
			void *pUserdata;
			if(NET_CALL(ddnet_net_userdata, m_pNet, PeerId, &pUserdata) || (uintptr_t)pUserdata == (uintptr_t)-1)
			{
				continue;
			}
			const int ClientId = (uintptr_t)pUserdata;
			dbg_assert(m_aPeers[ClientId].m_Id == PeerId, "invalid peer mapping");

			const char *pAddr;
			size_t AddrLen;
			ddnet_net_ev_moved_addr(m_pNetEvent, &pAddr, &AddrLen);
			NETADDR Addr;
			if(!AddrFromUrl(pAddr, &Addr))
			{
				static const char UNRECOGNIZED_ADDR[] = "Unrecognized address";
				NET_CALL(ddnet_net_close, m_pNet, PeerId, UNRECOGNIZED_ADDR, sizeof(UNRECOGNIZED_ADDR) - 1);
				continue;
			}

			// The ban list is asked again, the new address may be on it.
			char aBanReason[256];
			if(NetBan() && NetBan()->IsBanned(&Addr, aBanReason, sizeof(aBanReason)))
			{
				CloseBanned(PeerId, aBanReason);
				continue;
			}

			char aOldAddr[NETADDR_MAXSTRSIZE];
			str_copy(aOldAddr, m_aPeers[ClientId].m_aAddressStr.data());
			m_aPeers[ClientId].SetAddress(Addr);
			log_info("net", "client %d moved from %s to %s", ClientId, aOldAddr, m_aPeers[ClientId].m_aAddressStr.data());
		}
		break;
		case DDNET_NET_EV_DISCONNECT:
		{
			const uint64_t PeerId = ddnet_net_ev_disconnect_peer_index(m_pNetEvent);
			void *pUserdata;
			if(NET_CALL(ddnet_net_userdata, m_pNet, PeerId, &pUserdata) || (uintptr_t)pUserdata == (uintptr_t)-1)
			{
				continue;
			}
			const int ClientId = (uintptr_t)pUserdata;
			dbg_assert(m_aPeers[ClientId].m_Id == PeerId, "invalid peer mapping");

			// The peer mapping has to be cleared before the callback, sends from
			// within the callback would otherwise go to a closed connection.
			m_aPeers[ClientId].m_Id = -1;
			m_aFlushPending[ClientId] = false;
			m_aBuffer[ddnet_net_ev_disconnect_reason_len(m_pNetEvent)] = 0;
			const char *pReason = ddnet_net_ev_disconnect_is_remote(m_pNetEvent) ? "" : (char *)m_aBuffer;

			if(m_aPeers[ClientId].m_TimeoutProtected && IsTimeoutReason(pReason))
			{
				// The library is done with the peer; the slot waits for the
				// client to come back with its timeout code, see SetTimedOut.
				m_aPeers[ClientId].m_State = CPeer::STATE_TIMEOUT;
				m_aPeers[ClientId].m_TimeoutAt = time_get();
				log_info("net", "client %d timed out, keeping the slot for its timeout code", ClientId);
				continue;
			}
			m_aPeers[ClientId].m_State = CPeer::STATE_NONE;

			if(m_pfnDelClient)
			{
				m_pfnDelClient(ClientId, pReason, m_pUser);
			}
			m_aPeers[ClientId].Reset();
		}
		break;
		case DDNET_NET_EV_CHUNK:
		{
			const uint64_t PeerId = ddnet_net_ev_chunk_peer_index(m_pNetEvent);
			void *pUserdata;
			if(NET_CALL(ddnet_net_userdata, m_pNet, PeerId, &pUserdata) || (uintptr_t)pUserdata == (uintptr_t)-1)
			{
				continue;
			}
			const int ClientId = (uintptr_t)pUserdata;
			dbg_assert(m_aPeers[ClientId].m_Id == PeerId, "invalid peer mapping");
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
		case DDNET_NET_EV_MAP:
			// Only a server sends maps.
			continue;
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
