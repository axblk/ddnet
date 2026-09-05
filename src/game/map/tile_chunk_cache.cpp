#include "tile_chunk_cache.h"

#include <base/dbg.h>

#include <game/map/render_layer.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

uint64_t CTileChunkCache::s_CachedBytes = 0;
uint64_t CTileChunkCache::s_Tick = 0;
std::vector<CTileChunkCache *> CTileChunkCache::s_vpCaches;

CTileChunkCache::CTileChunkCache()
{
	s_vpCaches.push_back(this);
}

CTileChunkCache::~CTileChunkCache()
{
	Clear();
	std::erase(s_vpCaches, this);
}

void CTileChunkCache::OnInit(IGraphics *pGraphics)
{
	m_pGraphics = pGraphics;
}

bool CTileChunkCache::Clear()
{
	bool AllReleased = true;
	for(CChunk &Chunk : m_vChunks)
	{
		dbg_assert(s_CachedBytes >= Chunk.m_Bytes, "tile chunk cache memory accounting underflow");
		s_CachedBytes -= Chunk.m_Bytes;
		Chunk.m_Bytes = 0;
		if(m_pGraphics != nullptr)
			AllReleased = DeleteTileBuffer(m_pGraphics, Chunk.m_BufferObject) && AllReleased;
	}
	m_vChunks.clear();
	m_Columns = 0;
	m_Rows = 0;
	return AllReleased;
}

void CTileChunkCache::Ensure(int Width, int Height, bool Textured)
{
	const int Columns = (Width + CHUNK_SIZE - 1) / CHUNK_SIZE;
	const int Rows = (Height + CHUNK_SIZE - 1) / CHUNK_SIZE;
	if(Columns == m_Columns && Rows == m_Rows && Textured == m_Textured)
	{
		// A size that changed without changing the chunk grid still moves every
		// tile, because a tile's place in the layer is read off the width.
		if(Width != m_Width || Height != m_Height)
			Invalidate();
		m_Width = Width;
		m_Height = Height;
		return;
	}

	(void)Clear();
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

	// The chunk is walked twice, once for each pass, but the layer is read only
	// once: m_ReadTile is a std::function that reaches into whichever layer type
	// the caller has, and it used to be called for every tile of both passes.
	struct STile
	{
		unsigned char m_Index;
		unsigned char m_Flags;
		int m_AngleRotate;
	};
	std::vector<STile> vReadTiles(Capacity);
	for(int y = Y0; y < Y1; ++y)
	{
		for(int x = X0; x < X1; ++x)
		{
			STile &Tile = vReadTiles[(size_t)(y - Y0) * Chunk.m_Width + x - X0];
			Tile = {0, 0, -1};
			Source.m_ReadTile(x, y, &Tile.m_Index, &Tile.m_Flags, &Tile.m_AngleRotate);
		}
	}

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
				const STile &Tile = vReadTiles[LocalIndex];
				if(((Tile.m_Flags & TILEFLAG_OPAQUE) != 0) == Opaque)
					AddTileToBuffer(vTiles, vTextureCoords, Tile.m_Index, Tile.m_Flags, x, y, Source.m_Textured, Source.m_FillSpeedup, Tile.m_AngleRotate);
			}
		}
		vOffsets[Capacity] = (uint16_t)(vTiles.size() - FirstTile);
		if(Opaque)
			Chunk.m_OpaqueTiles = vTiles.size();
	}
	Chunk.m_TransparentTiles = vTiles.size() - Chunk.m_OpaqueTiles;
	dbg_assert(s_CachedBytes >= Chunk.m_Bytes, "tile chunk cache memory accounting underflow");
	s_CachedBytes -= Chunk.m_Bytes;
	Chunk.m_Bytes = 0;
	if(!UploadTileBuffer(m_pGraphics, vTiles, vTextureCoords, Chunk.m_BufferObject))
		return false;
	Chunk.m_Bytes = vTiles.size() * sizeof(CGraphicTile) + vTextureCoords.size() * sizeof(CGraphicTileTextureCoords);
	s_CachedBytes += Chunk.m_Bytes;
	Chunk.m_SourceDigest = SourceDigest(Source, ChunkX, ChunkY);
	Chunk.m_Dirty = false;
	return true;
}

void CTileChunkCache::ReleaseChunk(CChunk &Chunk)
{
	dbg_assert(s_CachedBytes >= Chunk.m_Bytes, "tile chunk cache memory accounting underflow");
	s_CachedBytes -= Chunk.m_Bytes;
	Chunk.m_Bytes = 0;
	Chunk.m_LastUsedTick = 0;
	// A buffer the backend refuses to give up stays allocated until it shuts
	// down. The chunk is rebuilt into a new one either way, so the handle has
	// to go regardless of what the destroy answered.
	(void)DeleteTileBuffer(m_pGraphics, Chunk.m_BufferObject);
	Chunk.m_Dirty = true;
}

void CTileChunkCache::EvictOverBudget(uint64_t CurrentTick)
{
	if(s_CachedBytes <= MEMORY_BUDGET)
		return;
	// One list over every cache: the chunk that should go is the least
	// recently drawn one of the whole map, whichever layer holds it. A layer
	// that left the screen keeps nothing back that the visible ones need.
	std::vector<CChunkUsage> vUsage;
	std::vector<std::pair<CTileChunkCache *, size_t>> vOwners;
	for(CTileChunkCache *pCache : s_vpCaches)
	{
		for(size_t Index = 0; Index < pCache->m_vChunks.size(); ++Index)
		{
			const CChunk &Chunk = pCache->m_vChunks[Index];
			if(Chunk.m_Bytes == 0)
				continue;
			vUsage.push_back({Chunk.m_Bytes, Chunk.m_LastUsedTick});
			vOwners.emplace_back(pCache, Index);
		}
	}
	for(const size_t Index : ChunksToEvict(vUsage, s_CachedBytes, CurrentTick))
	{
		CTileChunkCache *pCache = vOwners[Index].first;
		pCache->ReleaseChunk(pCache->m_vChunks[vOwners[Index].second]);
	}
}

uint32_t CTileChunkCache::SourceDigest(const CLayerSource &Source, int ChunkX, int ChunkY) const
{
	// FNV-1a over what a chunk draws from. Two tiles that differ in any of the
	// three values the renderer reads give a different digest; nothing else about
	// a tile reaches the buffer.
	uint32_t Digest = 2166136261u;
	const auto Feed = [&Digest](uint32_t Value) {
		for(int Byte = 0; Byte < 4; ++Byte)
		{
			Digest ^= (Value >> (Byte * 8)) & 0xFFu;
			Digest *= 16777619u;
		}
	};
	const int X0 = ChunkX * CHUNK_SIZE;
	const int Y0 = ChunkY * CHUNK_SIZE;
	const int X1 = std::min(X0 + CHUNK_SIZE, m_Width);
	const int Y1 = std::min(Y0 + CHUNK_SIZE, m_Height);
	for(int y = Y0; y < Y1; ++y)
	{
		for(int x = X0; x < X1; ++x)
		{
			unsigned char Index = 0;
			unsigned char Flags = 0;
			int AngleRotate = -1;
			Source.m_ReadTile(x, y, &Index, &Flags, &AngleRotate);
			Feed(Index);
			Feed(Flags);
			Feed(static_cast<uint32_t>(AngleRotate));
		}
	}
	return Digest;
}

void CTileChunkCache::VerifyOneChunk(const CLayerSource &Source, const CChunkRange &Visible)
{
#if defined(CONF_DEBUG)
	const size_t VisibleColumns = (size_t)(Visible.m_LastX - Visible.m_FirstX + 1);
	const size_t VisibleRows = (size_t)(Visible.m_LastY - Visible.m_FirstY + 1);
	const size_t VisibleCount = VisibleColumns * VisibleRows;
	if(VisibleCount == 0)
		return;
	m_VerifyCursor = (m_VerifyCursor + 1) % VisibleCount;
	const int ChunkX = Visible.m_FirstX + (int)(m_VerifyCursor % VisibleColumns);
	const int ChunkY = Visible.m_FirstY + (int)(m_VerifyCursor / VisibleColumns);
	const CChunk &Chunk = m_vChunks[ChunkY * m_Columns + ChunkX];
	if(Chunk.m_Dirty)
		return;
	dbg_assert(Chunk.m_SourceDigest == SourceDigest(Source, ChunkX, ChunkY),
		"tile chunk cache is stale: the layer changed without an invalidation");
#else
	(void)Source;
	(void)Visible;
#endif
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

	// Every chunk of a cache is built the same way, so the layout is the cache's
	// and not the chunk's. Ensure() throws the chunks away when it changes.
	const IGraphics::EVertexLayout Layout = m_Textured ? IGraphics::EVertexLayout::TILE_TEXTURED : IGraphics::EVertexLayout::TILE;

	const CChunkRange Visible = ChunkRange(X0, Y0, X1 - X0, Y1 - Y0, m_Width, m_Height);
	if(!Visible.IsEmpty())
		VerifyOneChunk(Source, Visible);
	const uint64_t CurrentTick = ++s_Tick;
	for(int ChunkY = Visible.m_FirstY; ChunkY <= Visible.m_LastY; ++ChunkY)
	{
		for(int ChunkX = Visible.m_FirstX; ChunkX <= Visible.m_LastX; ++ChunkX)
		{
			CChunk &Chunk = m_vChunks[ChunkY * m_Columns + ChunkX];
			if(Chunk.m_Dirty && !Rebuild(Source, ChunkX, ChunkY))
				continue;
			// Marked before the ranges are worked out: a chunk that is on
			// screen was wanted, whether or not this pass draws anything of it.
			Chunk.m_LastUsedTick = CurrentTick;
			if(!Chunk.m_BufferObject.IsValid())
				continue;

			const int ChunkTileX = ChunkX * CHUNK_SIZE;
			const int ChunkTileY = ChunkY * CHUNK_SIZE;
			const int LocalX0 = std::max(X0 - ChunkTileX, 0);
			const int LocalY0 = std::max(Y0 - ChunkTileY, 0);
			const int LocalX1 = std::min(X1 - ChunkTileX, Chunk.m_Width);
			const int LocalY1 = std::min(Y1 - ChunkTileY, Chunk.m_Height);
			const size_t FirstLocalTile = (size_t)LocalY0 * Chunk.m_Width + LocalX0;
			const size_t LastLocalTile = (size_t)(LocalY1 - 1) * Chunk.m_Width + LocalX1;

			uint32_t aFirstIndices[2];
			uint32_t aIndexCounts[2];
			size_t RangeCount = 0;
			const auto AddRange = [&](const std::vector<uint16_t> &vOffsets, unsigned int BaseTile) {
				const unsigned int StartTile = BaseTile + vOffsets[FirstLocalTile];
				const unsigned int TileCount = vOffsets[LastLocalTile] - vOffsets[FirstLocalTile];
				if(TileCount == 0)
					return;
				aFirstIndices[RangeCount] = StartTile * 6;
				aIndexCounts[RangeCount] = TileCount * 6;
				++RangeCount;
			};
			if(AllTransparent || !TransparentPass)
				AddRange(Chunk.m_vOpaqueTileOffsets, 0);
			if(AllTransparent || TransparentPass)
				AddRange(Chunk.m_vTransparentTileOffsets, Chunk.m_OpaqueTiles);
			if(RangeCount == 0)
				continue;
			m_pGraphics->RenderTileLayer(Chunk.m_BufferObject, Layout, Color, aFirstIndices, aIndexCounts, RangeCount);
		}
	}

	EvictOverBudget(CurrentTick);
}
