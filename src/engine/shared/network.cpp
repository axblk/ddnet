/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "network.h"

#include "huffman.h"

#include <base/dbg.h>

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

void CNetBase::Init()
{
	ms_Huffman.Init();
}
