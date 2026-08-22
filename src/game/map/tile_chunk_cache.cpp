#include "tile_chunk_cache.h"

#include <base/dbg.h>

#include <game/map/render_layer.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

CTileChunkCache::~CTileChunkCache()
{
	Clear();
}

void CTileChunkCache::OnInit(IGraphics *pGraphics)
{
	m_pGraphics = pGraphics;
}

void CTileChunkCache::Clear()
{
	if(m_pGraphics != nullptr)
	{
		for(CChunk &Chunk : m_vChunks)
			DeleteTileBuffer(m_pGraphics, Chunk.m_BufferObject, Chunk.m_BufferContainer);
	}
	m_vChunks.clear();
	m_Columns = 0;
	m_Rows = 0;
}

void CTileChunkCache::Ensure(int Width, int Height, bool Textured)
{
	const int Columns = (Width + CHUNK_SIZE - 1) / CHUNK_SIZE;
	const int Rows = (Height + CHUNK_SIZE - 1) / CHUNK_SIZE;
	if(Columns == m_Columns && Rows == m_Rows && Textured == m_Textured)
	{
		m_Width = Width;
		m_Height = Height;
		return;
	}

	Clear();
	m_Columns = Columns;
	m_Rows = Rows;
	m_Width = Width;
	m_Height = Height;
	m_Textured = Textured;
	m_vChunks.resize((size_t)Columns * Rows);
}

void CTileChunkCache::Invalidate()
{
	for(CChunk &Chunk : m_vChunks)
		Chunk.m_Dirty = true;
}

void CTileChunkCache::InvalidateArea(int x, int y, int w, int h)
{
	if(m_vChunks.empty())
		return;

	const CChunkRange Range = ChunkRange(x, y, w, h, m_Width, m_Height);
	if(Range.IsEmpty())
		return;

	for(int ChunkY = Range.m_FirstY; ChunkY <= Range.m_LastY; ++ChunkY)
		for(int ChunkX = Range.m_FirstX; ChunkX <= Range.m_LastX; ++ChunkX)
			m_vChunks[ChunkY * m_Columns + ChunkX].m_Dirty = true;
}

CTileChunkCache::CLayerSource TileLayerSource(const CTile *pTiles, int Width, int Height, bool Textured)
{
	CTileChunkCache::CLayerSource Source;
	Source.m_ReadTile = [pTiles, Width](int x, int y, unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate) {
		const CTile &Tile = pTiles[(size_t)y * Width + x];
		*pIndex = Tile.m_Index;
		*pFlags = Tile.m_Flags;
	};
	Source.m_Width = Width;
	Source.m_Height = Height;
	Source.m_Textured = Textured;
	return Source;
}

bool CTileChunkCache::Rebuild(const CLayerSource &Source, int ChunkX, int ChunkY)
{
	CChunk &Chunk = m_vChunks[ChunkY * m_Columns + ChunkX];
	std::vector<CGraphicTile> vTiles;
	std::vector<CGraphicTileTextureCoords> vTextureCoords;
	const int X0 = ChunkX * CHUNK_SIZE;
	const int Y0 = ChunkY * CHUNK_SIZE;
	const int X1 = std::min(X0 + CHUNK_SIZE, m_Width);
	const int Y1 = std::min(Y0 + CHUNK_SIZE, m_Height);
	Chunk.m_Width = X1 - X0;
	Chunk.m_Height = Y1 - Y0;
	const size_t Capacity = (size_t)Chunk.m_Width * Chunk.m_Height;
	vTiles.reserve(Capacity);
	if(Source.m_Textured)
		vTextureCoords.reserve(Capacity);

	// Opaque tiles first, transparent ones after, so that either pass is one
	// contiguous range of the same buffer.
	for(int Pass = 0; Pass < 2; ++Pass)
	{
		const bool Opaque = Pass == 0;
		std::vector<uint16_t> &vOffsets = Opaque ? Chunk.m_vOpaqueTileOffsets : Chunk.m_vTransparentTileOffsets;
		vOffsets.resize(Capacity + 1);
		const unsigned int FirstTile = vTiles.size();
		for(int y = Y0; y < Y1; ++y)
		{
			for(int x = X0; x < X1; ++x)
			{
				const size_t LocalIndex = (size_t)(y - Y0) * Chunk.m_Width + x - X0;
				vOffsets[LocalIndex] = (uint16_t)(vTiles.size() - FirstTile);
				unsigned char Index = 0;
				unsigned char Flags = 0;
				int AngleRotate = -1;
				Source.m_ReadTile(x, y, &Index, &Flags, &AngleRotate);
				if(((Flags & TILEFLAG_OPAQUE) != 0) == Opaque)
					AddTileToBuffer(vTiles, vTextureCoords, Index, Flags, x, y, Source.m_Textured, Source.m_FillSpeedup, AngleRotate);
			}
		}
		vOffsets[Capacity] = (uint16_t)(vTiles.size() - FirstTile);
		if(Opaque)
			Chunk.m_OpaqueTiles = vTiles.size();
	}
	Chunk.m_TransparentTiles = vTiles.size() - Chunk.m_OpaqueTiles;
	if(!UploadTileBuffer(m_pGraphics, vTiles, vTextureCoords, Chunk.m_BufferObject, Chunk.m_BufferContainer))
		return false;
	Chunk.m_Dirty = false;
	return true;
}

void CTileChunkCache::Render(const CLayerSource &Source, const ColorRGBA &Color, bool TransparentPass, bool ForceTransparent)
{
	dbg_assert(m_pGraphics != nullptr, "tile chunk cache was not initialized");
	Ensure(Source.m_Width, Source.m_Height, Source.m_Textured);
	const CScreenRect ScreenRect = m_pGraphics->GetScreen();
	const int X0 = std::clamp((int)std::floor(ScreenRect.m_TopLeft.x / 32.0f), 0, m_Width);
	const int Y0 = std::clamp((int)std::floor(ScreenRect.m_TopLeft.y / 32.0f), 0, m_Height);
	const int X1 = std::clamp((int)std::ceil(ScreenRect.m_BottomRight.x / 32.0f), 0, m_Width);
	const int Y1 = std::clamp((int)std::ceil(ScreenRect.m_BottomRight.y / 32.0f), 0, m_Height);
	if(X0 >= X1 || Y0 >= Y1)
		return;

	const bool AllTransparent = ForceTransparent || Color.a <= 254.0f / 255.0f;
	if(AllTransparent && !TransparentPass)
		return;

	const CChunkRange Visible = ChunkRange(X0, Y0, X1 - X0, Y1 - Y0, m_Width, m_Height);
	for(int ChunkY = Visible.m_FirstY; ChunkY <= Visible.m_LastY; ++ChunkY)
	{
		for(int ChunkX = Visible.m_FirstX; ChunkX <= Visible.m_LastX; ++ChunkX)
		{
			CChunk &Chunk = m_vChunks[ChunkY * m_Columns + ChunkX];
			if(Chunk.m_Dirty && !Rebuild(Source, ChunkX, ChunkY))
				continue;
			if(!Chunk.m_BufferContainer.IsValid())
				continue;

			const int ChunkTileX = ChunkX * CHUNK_SIZE;
			const int ChunkTileY = ChunkY * CHUNK_SIZE;
			const int LocalX0 = std::max(X0 - ChunkTileX, 0);
			const int LocalY0 = std::max(Y0 - ChunkTileY, 0);
			const int LocalX1 = std::min(X1 - ChunkTileX, Chunk.m_Width);
			const int LocalY1 = std::min(Y1 - ChunkTileY, Chunk.m_Height);
			const size_t FirstLocalTile = (size_t)LocalY0 * Chunk.m_Width + LocalX0;
			const size_t LastLocalTile = (size_t)(LocalY1 - 1) * Chunk.m_Width + LocalX1;

			offset_ptr_size apByteOffsets[2];
			unsigned int aIndexCounts[2];
			size_t RangeCount = 0;
			const auto AddRange = [&](const std::vector<uint16_t> &vOffsets, unsigned int BaseTile) {
				const unsigned int StartTile = BaseTile + vOffsets[FirstLocalTile];
				const unsigned int TileCount = vOffsets[LastLocalTile] - vOffsets[FirstLocalTile];
				if(TileCount == 0)
					return;
				apByteOffsets[RangeCount] = (offset_ptr_size)((offset_ptr)StartTile * 6 * sizeof(uint32_t));
				aIndexCounts[RangeCount] = TileCount * 6;
				++RangeCount;
			};
			if(AllTransparent || !TransparentPass)
				AddRange(Chunk.m_vOpaqueTileOffsets, 0);
			if(AllTransparent || TransparentPass)
				AddRange(Chunk.m_vTransparentTileOffsets, Chunk.m_OpaqueTiles);
			if(RangeCount == 0)
				continue;
			m_pGraphics->RenderTileLayer(Chunk.m_BufferContainer, Color, apByteOffsets, aIndexCounts, RangeCount);
		}
	}
}
