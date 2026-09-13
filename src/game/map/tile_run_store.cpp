#include "tile_run_store.h"

#include <algorithm>

void CTileRunStore::Clear()
{
	m_Width = 0;
	m_Height = 0;
	m_Columns = 0;
	m_Rows = 0;
	m_Bounds = CBounds();
	// A vector keeps what it held when it is cleared, and a layer that is given
	// up is given up.
	std::vector<CRun>().swap(m_vRuns);
	std::vector<uint32_t>().swap(m_vChunkStart);
}

void CTileRunStore::Grow(int x, int y, int Length)
{
	if(m_Bounds.IsEmpty())
	{
		m_Bounds.m_MinX = x;
		m_Bounds.m_MinY = y;
	}
	else
	{
		m_Bounds.m_MinX = std::min(m_Bounds.m_MinX, x);
		m_Bounds.m_MinY = std::min(m_Bounds.m_MinY, y);
	}
	m_Bounds.m_MaxX = std::max(m_Bounds.m_MaxX, x + Length - 1);
	m_Bounds.m_MaxY = std::max(m_Bounds.m_MaxY, y);
}

uint64_t CTileRunStore::Bytes() const
{
	return m_vRuns.capacity() * sizeof(CRun) + m_vChunkStart.capacity() * sizeof(uint32_t);
}

const CTileRunStore::CRun *CTileRunStore::Begin(int ChunkX, int ChunkY) const
{
	if(ChunkX < 0 || ChunkY < 0 || ChunkX >= m_Columns || ChunkY >= m_Rows)
		return m_vRuns.data();
	return m_vRuns.data() + m_vChunkStart[(size_t)ChunkY * m_Columns + ChunkX];
}

const CTileRunStore::CRun *CTileRunStore::End(int ChunkX, int ChunkY) const
{
	if(ChunkX < 0 || ChunkY < 0 || ChunkX >= m_Columns || ChunkY >= m_Rows)
		return m_vRuns.data();
	return m_vRuns.data() + m_vChunkStart[(size_t)ChunkY * m_Columns + ChunkX + 1];
}

void CTileRunStore::ReadTile(int x, int y, unsigned char *pIndex, unsigned char *pFlags) const
{
	if(x < 0 || y < 0 || x >= m_Width || y >= m_Height)
		return;
	const int ChunkX = x / CHUNK_SIZE;
	const int ChunkY = y / CHUNK_SIZE;
	const unsigned char LocalX = (unsigned char)(x - ChunkX * CHUNK_SIZE);
	const unsigned char LocalY = (unsigned char)(y - ChunkY * CHUNK_SIZE);
	for(const CRun *pRun = Begin(ChunkX, ChunkY); pRun != End(ChunkX, ChunkY); ++pRun)
	{
		if(pRun->m_Y < LocalY)
			continue;
		if(pRun->m_Y > LocalY || pRun->m_X > LocalX)
			break;
		if(LocalX < pRun->m_X + pRun->m_Length)
		{
			*pIndex = pRun->m_Index;
			*pFlags = pRun->m_Flags;
			return;
		}
	}
}
