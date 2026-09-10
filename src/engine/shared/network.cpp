/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "network.h"

#include "config.h"
#include "huffman.h"

#include <base/bytes.h>
#include <base/dbg.h>
#include <base/log.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>
#include <base/types.h>

#include <engine/console.h>

#include <net/net.h>

#include <algorithm>
#include <climits>

SECURITY_TOKEN ToSecurityToken(const unsigned char *pData)
{
	return bytes_be_to_uint(pData);
}

void CNetChunk::AssertSizeSanity() const
{
	if(m_Flags & NETSENDFLAG_CONNLESS)
	{
		dbg_assert(m_DataSize <= NET_MAX_CONNLESS_PAYLOAD, "connless packet too large, size=%d", m_DataSize);
	}
	else
	{
		dbg_assert(m_DataSize <= NET_MAX_CHUNK_SIZE, "chunk too large, size=%d", m_DataSize);
	}
}

CHuffman CNetBase::ms_Huffman;

int CNetBase::Compress(const void *pData, int DataSize, void *pOutput, int OutputSize)
{
	return ms_Huffman.Compress(pData, DataSize, pOutput, OutputSize);
}

int CNetBase::Decompress(const void *pData, int DataSize, void *pOutput, int OutputSize)
{
	return ms_Huffman.Decompress(pData, DataSize, pOutput, OutputSize);
}

// Forwards the log output of the network library into our logging system.
static void NetLogger(int Level, const char *pSystem, size_t SystemLen, const char *pMessage, size_t MessageLen)
{
	char aSystem[64];
	str_truncate(aSystem, sizeof(aSystem), pSystem, SystemLen);
	log_log((LEVEL)Level, aSystem, "%.*s", (int)MessageLen, pMessage);
}

void CNetBase::Init()
{
	ms_Huffman.Init();
	ddnet_net_set_logger(NetLogger);
	UpdateLogLevel();
}

void CNetBase::UpdateLogLevel()
{
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
	bool Failed;
	if(pChunk->m_Flags & NETSENDFLAG_EXTENDED)
	{
		Failed = ddnet_net_send_connless_chunk_extended(pNet, aUrl, str_length(aUrl), &pChunk->m_aExtraData, (const unsigned char *)pChunk->m_pData, pChunk->m_DataSize);
	}
	else
	{
		Failed = ddnet_net_send_connless_chunk(pNet, aUrl, str_length(aUrl), (const unsigned char *)pChunk->m_pData, pChunk->m_DataSize);
	}
	if(Failed)
	{
		log_error("net", "ddnet_net_send_connless_chunk: %s", ddnet_net_error(pNet));
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
