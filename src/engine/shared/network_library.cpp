#include "config.h"
#include "network.h"

#include <base/log.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>
#include <base/types.h>

#include <engine/console.h>

#include <net/net.h>

#include <algorithm>
#include <climits>

// Forwards the log output of the network library into our logging system.
static void NetLogger(int Level, const char *pSystem, size_t SystemLen, const char *pMessage, size_t MessageLen)
{
	char aSystem[64];
	str_truncate(aSystem, sizeof(aSystem), pSystem, SystemLen);
	log_log((LEVEL)Level, aSystem, "%.*s", (int)MessageLen, pMessage);
}

bool CheckNetCall(CNet *pNet, bool Failed, const char *pFunction)
{
	if(Failed)
	{
		log_error("net", "%s: %s", pFunction, ddnet_net_error(pNet));
	}
	return Failed;
}

void CNetBase::UpdateLogLevel()
{
	static bool s_LoggerSet = false;
	if(!s_LoggerSet)
	{
		ddnet_net_set_logger(NetLogger);
		s_LoggerSet = true;
	}
	static int s_Level = INT_MIN;
	// The most any of the loggers wants; each still filters for itself.
	const int Level = std::max({g_Config.m_Loglevel, g_Config.m_StdoutOutputLevel, g_Config.m_ConsoleOutputLevel, g_Config.m_EcOutputLevel});
	if(Level == s_Level)
	{
		return;
	}
	s_Level = Level;
	ddnet_net_set_log_level(IConsole::ToLogLevelFilter(Level));
}

bool NetDecodeMapHeader(const void *pData, int Size, CNetMapHeader *pHeader)
{
	uint64_t MapSize;
	uint32_t Crc;
	const uint8_t *pName;
	size_t NameLen;
	if(Size < 0 ||
		!ddnet_net_decode_map_header((const uint8_t *)pData, Size, &MapSize, &Crc, &pHeader->m_Sha256.data, &pName, &NameLen) ||
		NameLen >= sizeof(pHeader->m_aName))
	{
		return false;
	}
	mem_copy(pHeader->m_aName, pName, NameLen);
	pHeader->m_aName[NameLen] = '\0';
	pHeader->m_Crc = Crc;
	pHeader->m_Size = MapSize;
	return true;
}

ENetConnless NetConnlessAddr(const char *pUrl, NETADDR *pAddr)
{
	// A raw datagram and a 0.6 packet both come as a plain address; only
	// the scheme tells them apart.
	const bool Raw = str_startswith(pUrl, "udp://") != nullptr;
	if(net_addr_from_url(pAddr, pUrl, nullptr, 0) != 0)
	{
		return ENetConnless::NONE;
	}
	if(pAddr->type & (NETTYPE_QUIC | NETTYPE_WEBSOCKET))
	{
		return ENetConnless::NONE;
	}
	if(Raw)
	{
		return ENetConnless::RAW;
	}
	return pAddr->type & NETTYPE_TW7 ? ENetConnless::TW07 : ENetConnless::TW06;
}

static void NetSendConnlessTo(CNet *pNet, const char *pScheme, const char *pHost, const CNetChunk *pChunk)
{
	char aUrl[128];
	str_format(aUrl, sizeof(aUrl), "%s://%s", pScheme, pHost);
	if(pChunk->m_Flags & NETSENDFLAG_EXTENDED)
	{
		NET_CALL(ddnet_net_send_connless_chunk_extended, pNet, aUrl, str_length(aUrl), &pChunk->m_aExtraData, (const unsigned char *)pChunk->m_pData, pChunk->m_DataSize);
	}
	else
	{
		NET_CALL(ddnet_net_send_connless_chunk, pNet, aUrl, str_length(aUrl), (const unsigned char *)pChunk->m_pData, pChunk->m_DataSize);
	}
}

void NetSendConnless(CNet *pNet, const CNetChunk *pChunk)
{
	if(pNet == nullptr)
	{
		return;
	}
	const char *pScheme = (pChunk->m_Address.type & NETTYPE_TW7) ? "tw-0.7+udp" : "tw-0.6+udp";
	if(pChunk->m_Address.type & NETTYPE_LINK_BROADCAST)
	{
		// Everyone on the link, in each family the address asks for.
		char aHost[64];
		if(pChunk->m_Address.type & NETTYPE_IPV4)
		{
			str_format(aHost, sizeof(aHost), "255.255.255.255:%d", pChunk->m_Address.port);
			NetSendConnlessTo(pNet, pScheme, aHost, pChunk);
		}
		if(pChunk->m_Address.type & NETTYPE_IPV6)
		{
			str_format(aHost, sizeof(aHost), "[ff02::1]:%d", pChunk->m_Address.port);
			NetSendConnlessTo(pNet, pScheme, aHost, pChunk);
		}
		return;
	}
	NETADDR Addr = pChunk->m_Address;
	Addr.type &= NETTYPE_IPV4 | NETTYPE_IPV6;
	char aHost[NETADDR_MAXSTRSIZE];
	net_addr_str(&Addr, aHost, sizeof(aHost), true);
	NetSendConnlessTo(pNet, pScheme, aHost, pChunk);
}
