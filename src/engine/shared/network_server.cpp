/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "network.h"

#ifndef CONF_NETWORKING_QUIC

#include "config.h"
#include "netban.h"

#include <base/dbg.h>
#include <base/hash_ctxt.h>
#include <base/math.h>
#include <base/net.h>
#include <base/secure.h>
#include <base/time.h>

#include <engine/shared/compression.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>

#include <chrono>

const int g_DummyMapCrc = 0x6AF73DAF;
const unsigned char g_aDummyMapData[] = {
	0x44, 0x41, 0x54, 0x41, 0x04, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00, 0x00,
	0xF4, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0xAC, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x00, 0x00,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
	0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x68, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
	0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x1C, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x3C, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
	0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x05, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
	0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
	0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x78, 0x9C, 0x63, 0x64,
	0x60, 0x60, 0x60, 0x44, 0xC2, 0x00, 0x00, 0x38, 0x00, 0x05, 0x78, 0x9C,
	0x63, 0x64, 0x60, 0x60, 0x60, 0x44, 0xC2, 0x00, 0x00, 0x38, 0x00, 0x05};

bool CNetServer::Open(NETADDR BindAddr, CNetBan *pNetBan, int MaxClients, int MaxClientsPerIp)
{
	// zero out the whole structure
	this->~CNetServer();
	new(this) CNetServer{};

	// open socket
	m_Socket = net_udp_create(BindAddr);
	if(!m_Socket)
		return false;

	m_Address = BindAddr;
	m_pNetBan = pNetBan;

	m_MaxClients = std::clamp(MaxClients, 1, (int)NET_MAX_CLIENTS);
	m_MaxClientsPerIp = MaxClientsPerIp;

	m_VConnNum = 0;
	m_VConnFirst = 0;

	secure_random_fill(m_aSecurityTokenSeed, sizeof(m_aSecurityTokenSeed));

	for(auto &Slot : m_aSlots)
		Slot.m_Connection.Init(m_Socket, true);

	return true;
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
	m_pfnNewClient = pfnNewClient;
	m_pfnNewClientNoAuth = pfnNewClientNoAuth;
	m_pfnClientRejoin = pfnClientRejoin;
	m_pfnDelClient = pfnDelClient;
	m_pUser = pUser;
	return 0;
}

void CNetServer::Close()
{
	if(!m_Socket)
	{
		return;
	}
	net_udp_close(m_Socket);
	m_Socket = nullptr;
}

void CNetServer::Drop(int ClientId, const char *pReason)
{
	// TODO: insert lots of checks here

	if(m_pfnDelClient)
		m_pfnDelClient(ClientId, pReason, m_pUser);

	m_aSlots[ClientId].m_Connection.Disconnect(pReason);
}

void CNetServer::Wait(uint64_t Microseconds)
{
	net_socket_read_wait(m_Socket, std::chrono::microseconds(Microseconds));
}

void CNetServer::Update()
{
	m_NumRecvPackets = 0;

	const int64_t Now = time_get();
	if(Now > m_BudgetStart + time_freq())
	{
		m_BudgetStart = Now;
		m_NumPreConnDecompress = 0;
		m_NumBanReplies = 0;
	}

	for(int i = 0; i < MaxClients(); i++)
	{
		m_aSlots[i].m_Connection.Update();
		if(m_aSlots[i].m_Connection.State() == CNetConnection::EState::ERROR &&
			(!m_aSlots[i].m_Connection.m_TimeoutProtected ||
				!m_aSlots[i].m_Connection.m_TimeoutSituation))
		{
			Drop(i, m_aSlots[i].m_Connection.ErrorString());
		}
	}
}

void CNetServer::EndFlushBatch()
{
	m_FlushBatch = false;
	for(int ClientId = 0; ClientId < MaxClients(); ClientId++)
	{
		if(!m_aFlushPending[ClientId])
			continue;
		m_aFlushPending[ClientId] = false;
		if(m_aSlots[ClientId].m_Connection.State() == CNetConnection::EState::ONLINE)
			m_aSlots[ClientId].m_Connection.Flush();
	}
}

SECURITY_TOKEN CNetServer::GetGlobalToken()
{
	static const NETADDR NULL_ADDR = {0};
	return GetToken(NULL_ADDR);
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

void CNetServer::SendControl(NETADDR &Addr, int ControlMsg, const void *pExtra, int ExtraSize, SECURITY_TOKEN SecurityToken)
{
	CNetBase::SendControlMsg(m_Socket, &Addr, 0, ControlMsg, pExtra, ExtraSize, SecurityToken);
}

int CNetServer::NumClientsWithAddr(NETADDR Addr)
{
	int FoundAddr = 0;
	for(int i = 0; i < MaxClients(); ++i)
	{
		if(m_aSlots[i].m_Connection.State() == CNetConnection::EState::OFFLINE ||
			(m_aSlots[i].m_Connection.State() == CNetConnection::EState::ERROR &&
				(!m_aSlots[i].m_Connection.m_TimeoutProtected ||
					!m_aSlots[i].m_Connection.m_TimeoutSituation)))
			continue;

		if(!net_addr_comp_noport(&Addr, m_aSlots[i].m_Connection.PeerAddress()))
			FoundAddr++;
	}

	return FoundAddr;
}

bool CNetServer::Connlimit(NETADDR Addr)
{
	int64_t Now = time_get();
	int Oldest = 0;

	for(int i = 0; i < NET_CONNLIMIT_IPS; ++i)
	{
		if(!net_addr_comp_noport(&m_aSpamConns[i].m_Addr, &Addr))
		{
			m_aSpamConns[i].m_LastSeen = Now;
			if(m_aSpamConns[i].m_Time > Now - time_freq() * g_Config.m_SvConnlimitTime)
			{
				if(m_aSpamConns[i].m_Conns >= g_Config.m_SvConnlimit)
					return true;
			}
			else
			{
				m_aSpamConns[i].m_Time = Now;
				m_aSpamConns[i].m_Conns = 0;
			}
			m_aSpamConns[i].m_Conns++;
			return false;
		}

		if(m_aSpamConns[i].m_LastSeen < m_aSpamConns[Oldest].m_LastSeen)
			Oldest = i;
	}

	m_aSpamConns[Oldest].m_Addr = Addr;
	m_aSpamConns[Oldest].m_Time = Now;
	m_aSpamConns[Oldest].m_LastSeen = Now;
	m_aSpamConns[Oldest].m_Conns = 1;
	return false;
}

int CNetServer::TryAcceptClient(NETADDR &Addr, SECURITY_TOKEN SecurityToken, int Slot, bool VanillaAuth, bool Sixup, SECURITY_TOKEN Token)
{
	if(Sixup && !g_Config.m_SvSixup)
	{
		const char aMsg[] = "0.7 connections are not accepted at this time";
		CNetBase::SendControlMsg(m_Socket, &Addr, 0, NET_CTRLMSG_CLOSE, aMsg, sizeof(aMsg), SecurityToken, Sixup);
		return -1; // failed to add client?
	}

	const bool Reconnect = Slot != -1;
	if(Reconnect)
	{
		if(g_Config.m_Debug)
			dbg_msg("security", "client %d reconnect", Slot);
	}
	else
	{
		if(Connlimit(Addr))
		{
			const char aMsg[] = "Too many connections in a short time";
			CNetBase::SendControlMsg(m_Socket, &Addr, 0, NET_CTRLMSG_CLOSE, aMsg, sizeof(aMsg), SecurityToken, Sixup);
			return -1; // failed to add client
		}

		// check for sv_max_clients_per_ip
		if(NumClientsWithAddr(Addr) + 1 > m_MaxClientsPerIp)
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "Only %d players with the same IP are allowed", m_MaxClientsPerIp);
			CNetBase::SendControlMsg(m_Socket, &Addr, 0, NET_CTRLMSG_CLOSE, aBuf, str_length(aBuf) + 1, SecurityToken, Sixup);
			return -1; // failed to add client
		}

		Slot = -1;
		for(int i = 0; i < MaxClients(); i++)
		{
			if(m_aSlots[i].m_Connection.State() == CNetConnection::EState::OFFLINE)
			{
				Slot = i;
				break;
			}
		}

		if(Slot == -1)
		{
			const char aFullMsg[] = "This server is full";
			CNetBase::SendControlMsg(m_Socket, &Addr, 0, NET_CTRLMSG_CLOSE, aFullMsg, sizeof(aFullMsg), SecurityToken, Sixup);

			return -1; // failed to add client
		}
	}

	// init connection slot
	m_aSlots[Slot].m_Connection.DirectInit(Addr, SecurityToken, Token, Sixup);

	if(VanillaAuth)
	{
		// client sequence is unknown if the auth was done
		// connection-less
		m_aSlots[Slot].m_Connection.SetUnknownSeq();
		// correct sequence
		m_aSlots[Slot].m_Connection.SetSequence(6);
	}

	if(g_Config.m_Debug)
	{
		dbg_msg("security", "client accepted %s", m_aSlots[Slot].m_Connection.PeerAddressString(true).data());
	}

	if(Reconnect)
		m_pfnClientRejoin(Slot, m_pUser, Sixup, VanillaAuth);
	else if(VanillaAuth)
		m_pfnNewClientNoAuth(Slot, m_pUser);
	else
		m_pfnNewClient(Slot, m_pUser, Sixup);

	return Slot; // done
}

void CNetServer::SendMsgs(NETADDR &Addr, const CPacker **ppMsgs, int Num)
{
	dbg_assert(Num > 0 && Num <= NET_MAX_PACKET_CHUNKS, "Number of messages invalid: %d", Num);

	CNetPacketConstruct Construct;
	mem_zero(&Construct, sizeof(Construct));
	unsigned char *pChunkData = &Construct.m_aChunkData[Construct.m_DataSize];

	for(int i = 0; i < Num; i++)
	{
		const CPacker *pMsg = ppMsgs[i];
		CNetChunkHeader Header;
		Header.m_Flags = NET_CHUNKFLAG_VITAL;
		Header.m_Size = pMsg->Size();
		Header.m_Sequence = i + 1;
		pChunkData = Header.Pack(pChunkData);
		mem_copy(pChunkData, pMsg->Data(), pMsg->Size());
		pChunkData += pMsg->Size();
		Construct.m_NumChunks++;
	}

	Construct.m_DataSize = (int)(pChunkData - Construct.m_aChunkData);
	CNetBase::SendPacket(m_Socket, &Addr, &Construct, NET_SECURITY_TOKEN_UNSUPPORTED);
}

// connection-less msg packet without token-support
void CNetServer::OnPreConnMsg(NETADDR &Addr, CNetPacketConstruct &Packet, int Slot)
{
	bool IsCtrl = Packet.m_Flags & NET_PACKETFLAG_CONTROL;
	int CtrlMsg = Packet.m_aChunkData[0];
	const bool NewAuthOrRejoin = Slot == -1 || m_aSlots[Slot].m_Connection.SecurityToken() == NET_SECURITY_TOKEN_UNKNOWN ||
				     (IsCtrl && m_aSlots[Slot].m_Connection.SecurityToken() == NET_SECURITY_TOKEN_UNSUPPORTED);
	if(!NewAuthOrRejoin)
		return;

	if(IsCtrl && CtrlMsg == NET_CTRLMSG_CONNECT)
	{
		if(g_Config.m_SvVanillaAntiSpoof && g_Config.m_Password[0] == '\0')
		{
			const int64_t Now = time_get();
			if(Now > m_VConnFirst + time_freq())
			{
				m_VConnFirst = Now;
				m_VConnNum = 0;
			}
			m_VConnNum++;

			// The handshake below goes to an address that is not verified yet and is
			// larger than the connect asking for it.
			if(g_Config.m_SvVanConnRepliesPerSecond != 0 &&
				m_VConnNum > g_Config.m_SvVanConnRepliesPerSecond)
			{
				return;
			}

			// detect flooding
			const bool Flooding = g_Config.m_SvVanConnPerSecond != 0 &&
					      m_VConnNum > g_Config.m_SvVanConnPerSecond;
			if(g_Config.m_Debug && Flooding)
			{
				dbg_msg("security", "vanilla connection flooding detected");
			}

			if(Slot != -1)
			{
				if(g_Config.m_Debug)
					dbg_msg("security", "client %d wants to reconnect (vanilla)", Slot);

				// Invalidate token for this connection to accept token in `NETMSG_INPUT` later
				m_aSlots[Slot].m_Connection.m_SecurityToken = NET_SECURITY_TOKEN_UNKNOWN;
			}

			// simulate accept
			SendControl(Addr, NET_CTRLMSG_CONNECTACCEPT, nullptr, 0, NET_SECURITY_TOKEN_UNSUPPORTED);

			// Begin vanilla compatible token handshake
			// The idea is to pack a security token in the gametick
			// parameter of NETMSG_SNAPEMPTY. The Client then will
			// return the token/gametick in NETMSG_INPUT, allowing
			// us to validate the token.
			// https://github.com/eeeee/ddnet/commit/b8e40a244af4e242dc568aa34854c5754c75a39a

			// Before we can send NETMSG_SNAPEMPTY, the client needs
			// to load a map, otherwise it might crash. The map
			// should be as small as is possible and directly available
			// to the client. Therefore a dummy map is sent in the same
			// packet. To reduce the traffic we'll fallback to a default
			// map if there are too many connection attempts at once.

			// send mapchange + map data + con_ready + 3 x empty snap (with token)
			CPacker MapChangeMsg;
			MapChangeMsg.Reset();
			MapChangeMsg.AddInt((NETMSG_MAP_CHANGE << 1) | 1);
			if(Flooding)
			{
				// Fallback to dm1
				MapChangeMsg.AddString("dm1", 0);
				MapChangeMsg.AddInt(0xf2159e6e);
				MapChangeMsg.AddInt(5805);
			}
			else
			{
				// dummy map
				MapChangeMsg.AddString("dummy", 0);
				MapChangeMsg.AddInt(g_DummyMapCrc);
				MapChangeMsg.AddInt(sizeof(g_aDummyMapData));
			}

			CPacker MapDataMsg;
			MapDataMsg.Reset();
			MapDataMsg.AddInt((NETMSG_MAP_DATA << 1) | 1);
			if(Flooding)
			{
				// send empty map data to keep 0.6.4 support
				MapDataMsg.AddInt(1); // last chunk
				MapDataMsg.AddInt(0); // crc
				MapDataMsg.AddInt(0); // chunk index
				MapDataMsg.AddInt(0); // map size
				// no map data
			}
			else
			{
				// send dummy map data
				MapDataMsg.AddInt(1); // last chunk
				MapDataMsg.AddInt(g_DummyMapCrc); // crc
				MapDataMsg.AddInt(0); // chunk index
				MapDataMsg.AddInt(sizeof(g_aDummyMapData)); // map size
				MapDataMsg.AddRaw(g_aDummyMapData, sizeof(g_aDummyMapData)); // map data
			}

			CPacker ConReadyMsg;
			ConReadyMsg.Reset();
			ConReadyMsg.AddInt((NETMSG_CON_READY << 1) | 1);

			CPacker SnapEmptyMsg;
			SnapEmptyMsg.Reset();
			SnapEmptyMsg.AddInt((NETMSG_SNAPEMPTY << 1) | 1);
			SECURITY_TOKEN SecurityToken = GetVanillaToken(Addr);
			SnapEmptyMsg.AddInt(SecurityToken);
			SnapEmptyMsg.AddInt(SecurityToken + 1);

			// send all chunks/msgs in one packet
			const CPacker *apMsgs[] = {&MapChangeMsg, &MapDataMsg, &ConReadyMsg,
				&SnapEmptyMsg, &SnapEmptyMsg, &SnapEmptyMsg};
			SendMsgs(Addr, apMsgs, std::size(apMsgs));
		}
		else
		{
			// accept client directly
			SendControl(Addr, NET_CTRLMSG_CONNECTACCEPT, nullptr, 0, NET_SECURITY_TOKEN_UNSUPPORTED);

			TryAcceptClient(Addr, NET_SECURITY_TOKEN_UNSUPPORTED, Slot);
		}
	}
	else if(!IsCtrl && g_Config.m_SvVanillaAntiSpoof && g_Config.m_Password[0] == '\0')
	{
		// the chunk header is two bytes, three for vital chunks
		if(Packet.m_DataSize < 2)
		{
			return;
		}
		CNetChunkHeader Header;
		unsigned char *pData = Header.Unpack(Packet.m_aChunkData);
		const int Remaining = Packet.m_DataSize - (int)(pData - Packet.m_aChunkData);
		if(Remaining < 0)
		{
			return;
		}
		CUnpacker Unpacker;
		Unpacker.Reset(pData, std::min(Header.m_Size, Remaining));
		int Msg = Unpacker.GetInt() >> 1;

		if(Msg == NETMSG_INPUT)
		{
			SECURITY_TOKEN SecurityToken = Unpacker.GetInt();
			if(SecurityToken == GetVanillaToken(Addr))
			{
				if(g_Config.m_Debug)
					dbg_msg("security", "new client (vanilla handshake)");
				// try to accept client skipping auth state
				TryAcceptClient(Addr, NET_SECURITY_TOKEN_UNSUPPORTED, Slot, true);
			}
			else if(g_Config.m_Debug)
			{
				dbg_msg("security", "invalid token (vanilla handshake)");
			}
		}
		else
		{
			if(g_Config.m_Debug)
			{
				dbg_msg("security", "invalid preconn msg %d", Msg);
			}
		}
	}
}

void CNetServer::OnTokenCtrlMsg(NETADDR &Addr, int ControlMsg, const CNetPacketConstruct &Packet, int Slot)
{
	if(ControlMsg == NET_CTRLMSG_CONNECT)
	{
		// response connection request with token
		SECURITY_TOKEN Token = GetToken(Addr);
		SendControl(Addr, NET_CTRLMSG_CONNECTACCEPT, SECURITY_TOKEN_MAGIC, sizeof(SECURITY_TOKEN_MAGIC), Token);

		if(g_Config.m_Debug && Slot != -1)
			dbg_msg("security", "client %d wants to reconnect (ddnet)", Slot);
	}
	else if(ControlMsg == NET_CTRLMSG_ACCEPT)
	{
		SECURITY_TOKEN Token = ToSecurityToken(&Packet.m_aChunkData[1]);
		if(Token == GetToken(Addr))
		{
			// correct token
			// try to accept client
			if(g_Config.m_Debug)
				dbg_msg("security", "new client (ddnet token)");
			TryAcceptClient(Addr, Token, Slot);
		}
		else
		{
			// invalid token
			if(g_Config.m_Debug)
				dbg_msg("security", "invalid token");
		}
	}
}

int CNetServer::OnSixupCtrlMsg(NETADDR &Addr, CNetChunk *pChunk, int ControlMsg, const CNetPacketConstruct &Packet, SECURITY_TOKEN &ResponseToken, SECURITY_TOKEN Token, int Slot)
{
	if(Packet.m_DataSize < 1 + (int)sizeof(SECURITY_TOKEN))
		return 0; // silently ignore

	ResponseToken = ToSecurityToken(Packet.m_aChunkData + 1);

	if(ControlMsg == protocol7::NET_CTRLMSG_TOKEN)
	{
		if(g_Config.m_Debug && Slot != -1)
			dbg_msg("security", "client %d wants to reconnect (0.7)", Slot);

		if(Packet.m_DataSize >= (int)NET_TOKENREQUEST_DATASIZE)
		{
			SendTokenSixup(Addr, ResponseToken);
			return 0;
		}

		// Is this behaviour safe to rely on?
		pChunk->m_Flags = 0;
		pChunk->m_ClientId = -1;
		pChunk->m_Address = Addr;
		pChunk->m_DataSize = 0;
		return 1;
	}
	else if(ControlMsg == NET_CTRLMSG_CONNECT)
	{
		SECURITY_TOKEN MyToken = GetToken(Addr);
		unsigned char aToken[sizeof(SECURITY_TOKEN)];
		mem_copy(aToken, &MyToken, sizeof(aToken));

		CNetBase::SendControlMsg(m_Socket, &Addr, 0, NET_CTRLMSG_CONNECTACCEPT, aToken, sizeof(aToken), ResponseToken, true);
		if(Token == MyToken)
			TryAcceptClient(Addr, ResponseToken, Slot, false, true, Token);
	}

	return 0;
}

int CNetServer::GetClientSlot(const NETADDR &Addr)
{
	for(int i = 0; i < MaxClients(); i++)
	{
		if(m_aSlots[i].m_Connection.State() != CNetConnection::EState::OFFLINE &&
			m_aSlots[i].m_Connection.State() != CNetConnection::EState::ERROR &&
			net_addr_comp(m_aSlots[i].m_Connection.PeerAddress(), &Addr) == 0)
		{
			return i;
		}
	}
	return -1;
}

static bool IsDDNetControlMsg(const CNetPacketConstruct *pPacket)
{
	if(!(pPacket->m_Flags & NET_PACKETFLAG_CONTROL) || pPacket->m_DataSize < 1)
	{
		return false;
	}
	if(pPacket->m_aChunkData[0] == NET_CTRLMSG_CONNECT && pPacket->m_DataSize >= (int)(1 + sizeof(SECURITY_TOKEN_MAGIC) + sizeof(SECURITY_TOKEN)) && mem_comp(&pPacket->m_aChunkData[1], SECURITY_TOKEN_MAGIC, sizeof(SECURITY_TOKEN_MAGIC)) == 0)
	{
		// DDNet CONNECT
		return true;
	}
	if(pPacket->m_aChunkData[0] == NET_CTRLMSG_ACCEPT && pPacket->m_DataSize >= 1 + (int)sizeof(SECURITY_TOKEN))
	{
		// DDNet ACCEPT
		return true;
	}
	return false;
}

/*
	TODO: chopp up this function into smaller working parts
*/
int CNetServer::Recv(CNetChunk *pChunk, SECURITY_TOKEN *pResponseToken)
{
	while(true)
	{
		// Unpack next chunk from stored packet if available
		if(m_PacketChunkUnpacker.UnpackNextChunk(pChunk))
		{
			// Only return the pending packet if the client is
			// still available, the caller might have dropped them
			// in response to the previous chunk.
			if(m_aSlots[pChunk->m_ClientId].m_Connection.State() != CNetConnection::EState::OFFLINE)
			{
				return 1;
			}
			else
			{
				m_PacketChunkUnpacker.Reset();
			}
		}

		// Stop draining the socket once this batch's budget is used up, otherwise traffic
		// arriving faster than it can be processed keeps this loop from ever returning.
		if(g_Config.m_SvMaxPacketsPerRecv != 0 && m_NumRecvPackets >= g_Config.m_SvMaxPacketsPerRecv)
		{
			break;
		}

		// TODO: empty the recvinfo
		NETADDR Addr;
		unsigned char *pData;
		int Bytes = net_udp_recv(m_Socket, &Addr, &pData);

		// no more packets for now
		if(Bytes <= 0)
			break;
		m_NumRecvPackets++;

		// check if we just should drop the packet
		char aBuf[128];
		if(NetBan() && NetBan()->IsBanned(&Addr, aBuf, sizeof(aBuf)))
		{
			// Banned, reply with a message. Rate limited, unlimited replies would
			// make a banned flooder cost more to handle than an unbanned one.
			if(g_Config.m_SvBanRepliesPerSecond == 0 || m_NumBanReplies < g_Config.m_SvBanRepliesPerSecond)
			{
				m_NumBanReplies++;
				CNetBase::SendControlMsg(m_Socket, &Addr, 0, NET_CTRLMSG_CLOSE, aBuf, str_length(aBuf) + 1, NET_SECURITY_TOKEN_UNSUPPORTED);
			}
			continue;
		}

		// Check size and unpack packet flags early so we can determine the sixup
		// state correctly for connection-oriented packets before unpacking them.
		std::optional<int> Flags = CNetBase::UnpackPacketFlags(pData, Bytes);
		if(!Flags)
		{
			continue;
		}

		SECURITY_TOKEN Token;
		int Slot = (*Flags & NET_PACKETFLAG_CONNLESS) == 0 ? GetClientSlot(Addr) : -1;
		bool Sixup = Slot != -1 && m_aSlots[Slot].m_Connection.m_Sixup;

		// Decompressing costs far more than everything else done per packet, so only do it
		// for packets that can still turn out to be authentic. In 0.7 the security token is
		// in the packet header and is compared first. In 0.6 it is inside the payload, so
		// packets from addresses without a connection can only be attributed after decoding;
		// the vanilla anti-spoof handshake is the only legitimate one and gets a budget.
		bool AllowDecompression;
		if(Slot == -1)
		{
			AllowDecompression =
				g_Config.m_SvVanillaAntiSpoof &&
				g_Config.m_Password[0] == '\0' &&
				(g_Config.m_SvPreConnDecompressPerSecond == 0 ||
					m_NumPreConnDecompress < g_Config.m_SvPreConnDecompressPerSecond);
		}
		else if(Sixup)
		{
			AllowDecompression =
				Bytes >= NET_PACKETHEADERSIZE + (int)sizeof(SECURITY_TOKEN) &&
				ToSecurityToken(pData + NET_PACKETHEADERSIZE) == m_aSlots[Slot].m_Connection.m_Token;
		}
		else
		{
			AllowDecompression = true;
		}

		bool Decompressed = false;
		const int UnpackResult = CNetBase::UnpackPacket(pData, Bytes, &m_RecvBuffer, Sixup, AllowDecompression, &Token, pResponseToken, &Decompressed);
		if(Slot == -1 && Decompressed)
		{
			m_NumPreConnDecompress++;
		}

		if(UnpackResult != 0)
		{
			continue;
		}

		if(m_RecvBuffer.m_Flags & NET_PACKETFLAG_CONNLESS)
		{
			if(Sixup && Token != GetToken(Addr) && Token != GetGlobalToken())
			{
				continue;
			}

			pChunk->m_Flags = NETSENDFLAG_CONNLESS;
			pChunk->m_ClientId = -1;
			pChunk->m_Address = Addr;
			pChunk->m_DataSize = m_RecvBuffer.m_DataSize;
			pChunk->m_pData = m_RecvBuffer.m_aChunkData;
			if(m_RecvBuffer.m_Flags & NET_PACKETFLAG_EXTENDED)
			{
				pChunk->m_Flags |= NETSENDFLAG_EXTENDED;
				mem_copy(pChunk->m_aExtraData, m_RecvBuffer.m_aExtraData, sizeof(pChunk->m_aExtraData));
			}
			return 1;
		}
		else // connection-oriented packet
		{
			const bool Control = (m_RecvBuffer.m_Flags & NET_PACKETFLAG_CONTROL) != 0;
			if(Sixup && Control)
			{
				// got 0.7 control msg
				if(OnSixupCtrlMsg(Addr, pChunk, m_RecvBuffer.m_aChunkData[0], m_RecvBuffer, *pResponseToken, Token, Slot) == 1)
					return 1;
			}
			else if(IsDDNetControlMsg(&m_RecvBuffer) && Control)
			{
				// got ddnet control msg
				OnTokenCtrlMsg(Addr, m_RecvBuffer.m_aChunkData[0], m_RecvBuffer, Slot);
			}
			else
			{
				// got connection-less ctrl or sys msg
				OnPreConnMsg(Addr, m_RecvBuffer, Slot);
			}

			if(Slot != -1 && m_aSlots[Slot].m_Connection.Feed(&m_RecvBuffer, &Addr, Token, *pResponseToken))
			{
				if(!Control && m_RecvBuffer.m_DataSize > 0 && m_RecvBuffer.m_NumChunks > 0)
				{
					m_PacketChunkUnpacker.FeedPacket(Addr, m_RecvBuffer, &m_aSlots[Slot].m_Connection, Slot);
				}
			}
		}
	}
	return 0;
}

void CNetServer::SetMap(int MapId, const char *pName, unsigned Crc, const SHA256_DIGEST &Sha256, const void *pData, unsigned Size)
{
}

bool CNetServer::SendMap(int ClientId, int MapId)
{
	return false;
}

void CNetServer::CancelMap(int ClientId)
{
}

int CNetServer::Send(CNetChunk *pChunk)
{
	pChunk->AssertSizeSanity();

	if(pChunk->m_Flags & NETSENDFLAG_CONNLESS)
	{
		// send connectionless packet
		CNetBase::SendPacketConnless(m_Socket, &pChunk->m_Address, pChunk->m_pData, pChunk->m_DataSize,
			pChunk->m_Flags & NETSENDFLAG_EXTENDED, pChunk->m_aExtraData);
	}
	else
	{
		int Flags = 0;
		dbg_assert(
			pChunk->m_ClientId >= 0 && pChunk->m_ClientId < MaxClients(),
			"Invalid pChunk->m_ClientId: %d",
			pChunk->m_ClientId);

		if(pChunk->m_Flags & NETSENDFLAG_VITAL)
			Flags = NET_CHUNKFLAG_VITAL;

		if(m_aSlots[pChunk->m_ClientId].m_Connection.QueueChunk(Flags, pChunk->m_DataSize, pChunk->m_pData) == 0)
		{
			if(pChunk->m_Flags & NETSENDFLAG_FLUSH)
			{
				if(m_FlushBatch)
					m_aFlushPending[pChunk->m_ClientId] = true;
				else
					m_aSlots[pChunk->m_ClientId].m_Connection.Flush();
			}
		}
	}
	return 0;
}

void CNetServer::SendTokenSixup(NETADDR &Addr, SECURITY_TOKEN Token)
{
	unsigned char aRequestTokenBuf[NET_TOKENREQUEST_DATASIZE] = {};
	WriteSecurityToken(aRequestTokenBuf, GetToken(Addr));
	const int Size = Token == NET_SECURITY_TOKEN_UNKNOWN ? sizeof(aRequestTokenBuf) : sizeof(SECURITY_TOKEN);
	CNetBase::SendControlMsg(m_Socket, &Addr, 0, protocol7::NET_CTRLMSG_TOKEN, aRequestTokenBuf, Size, Token, true);
}

void CNetServer::SendConnlessSixup(const NETADDR *pAddr, const void *pData, int DataSize, SECURITY_TOKEN ResponseToken)
{
	NETADDR Addr = *pAddr;
	CNetBase::SendPacketConnlessWithToken7(m_Socket, &Addr, pData, DataSize, ResponseToken, GetToken(Addr));
}

void CNetServer::SetMaxClientsPerIp(int Max)
{
	m_MaxClientsPerIp = std::clamp<int>(Max, 1, NET_MAX_CLIENTS);
}

bool CNetServer::HasErrored(int ClientId)
{
	return m_aSlots[ClientId].m_Connection.State() == CNetConnection::EState::ERROR;
}

void CNetServer::ResumeOldConnection(int ClientId, int OrigId)
{
	m_aSlots[ClientId].m_Connection.ResumeConnection(ClientAddr(OrigId), m_aSlots[OrigId].m_Connection.SeqSequence(), m_aSlots[OrigId].m_Connection.AckSequence(), m_aSlots[OrigId].m_Connection.SecurityToken(), m_aSlots[OrigId].m_Connection.ResendBuffer(), m_aSlots[OrigId].m_Connection.m_Sixup);
	m_aSlots[OrigId].m_Connection.Reset();
}

void CNetServer::IgnoreTimeouts(int ClientId)
{
	m_aSlots[ClientId].m_Connection.m_TimeoutProtected = true;
}

void CNetServer::ResetErrorString(int ClientId)
{
	m_aSlots[ClientId].m_Connection.ResetErrorString();
}

const char *CNetServer::ErrorString(int ClientId)
{
	return m_aSlots[ClientId].m_Connection.ErrorString();
}

#else // CONF_NETWORKING_QUIC

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
		(m_aTlsCert[0] != '\0' && ddnet_net_set_tls_files(m_pNet, m_aTlsCert, str_length(m_aTlsCert), m_aTlsKey, str_length(m_aTlsKey))) ||
		ddnet_net_set_accept_connections(m_pNet, true) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_TW06, g_Config.m_SvLegacyUdp != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_TW07, g_Config.m_SvLegacyUdp != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_QUIC, g_Config.m_SvQuic != 0) ||
		ddnet_net_set_accept_protocol(m_pNet, DDNET_NET_PROTOCOL_WEBTRANSPORT, g_Config.m_SvWebtransport != 0) ||
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
			m_aPeers[ClientId].m_Quic = str_startswith(pAddr, "ddnet+quic://") != nullptr || str_startswith(pAddr, "ddnet+wt://") != nullptr;
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

#endif // CONF_NETWORKING_QUIC
