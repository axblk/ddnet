#include "tile_chunk_cache.h"

#include <base/dbg.h>
#include <base/log.h>

#include <game/map/render_layer.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

uint64_t CTileChunkCache::ms_CachedBytes = 0;
uint64_t CTileChunkCache::ms_Tick = 0;
bool CTileChunkCache::ms_ReportedUploadFailure = false;
std::vector<CTileChunkCache *> CTileChunkCache::ms_vpCaches;

CTileChunkCache::CTileChunkCache()
{
	ms_vpCaches.push_back(this);
}

CTileChunkCache::~CTileChunkCache()
{
	Clear();
	std::erase(ms_vpCaches, this);
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
		dbg_assert(ms_CachedBytes >= Chunk.m_Bytes, "tile chunk cache memory accounting underflow");
		ms_CachedBytes -= Chunk.m_Bytes;
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
	// AddTileToBuffer emits one quad per tile that is not air, so counting
	// them here gives the exact size of both buffers instead of a guess at
	// the chunk's capacity - and zero says the chunk draws nothing at all.
	size_t Drawn = 0;
	for(int y = Y0; y < Y1; ++y)
	{
		for(int x = X0; x < X1; ++x)
		{
			STile &Tile = vReadTiles[(size_t)(y - Y0) * Chunk.m_Width + x - X0];
			Tile = {0, 0, -1};
			Source.m_ReadTile(x, y, &Tile.m_Index, &Tile.m_Flags, &Tile.m_AngleRotate);
			if(Tile.m_Index != 0)
				++Drawn;
		}
	}

	dbg_assert(ms_CachedBytes >= Chunk.m_Bytes, "tile chunk cache memory accounting underflow");
	ms_CachedBytes -= Chunk.m_Bytes;
	Chunk.m_Bytes = 0;
	Chunk.m_OpaqueQuads = 0;
	Chunk.m_TransparentQuads = 0;
	if(Drawn == 0)
	{
		// Nothing but air. Such a chunk needs no buffer at all, and on a large
		// map most of them are this: of the 44836 chunks of Abyss, 36469 draw
		// nothing.
		(void)DeleteTileBuffer(m_pGraphics, Chunk.m_BufferObject);
		Chunk.m_SourceDigest = SourceDigest(Source, ChunkX, ChunkY);
		Chunk.m_Dirty = false;
		return true;
	}

	// Tiles that are the same and lie next to each other become one quad. The
	// tile texture is an array with one layer per tile and it wraps, so a
	// rectangle of the same tile is that tile repeated over it - which is what
	// the border tiles have always done, only there the repeat count is a
	// uniform instead of the quad's own texture coordinates.
	//
	// Rows are cut into runs of the same tile and a run that matches the one
	// above it keeps growing downwards; when it no longer does, it is written
	// out as one quad. That is the usual greedy meshing, and it costs one walk
	// over the chunk per pass, the same as writing every tile did. Over the
	// tile layers of Abyss it turns 14.5 M drawn tiles into 1.17 M quads,
	// 8.1 % of the geometry.
	struct SRun
	{
		int m_X;
		int m_Width;
		int m_FirstY;
		size_t m_Tile;
	};
	const auto SameTile = [](const STile &Left, const STile &Right) {
		return Left.m_Index == Right.m_Index && Left.m_Flags == Right.m_Flags && Left.m_AngleRotate == Right.m_AngleRotate;
	};
	std::vector<SRun> vGrowing, vRuns;

	// Opaque tiles first, transparent ones after, so that either pass is one
	// contiguous range of the same buffer.
	for(int Pass = 0; Pass < 2; ++Pass)
	{
		const bool Opaque = Pass == 0;
		vGrowing.clear();
		// One row past the bottom, so that whatever is still growing there is
		// written out by the same code as everything else.
		for(int y = Y0; y <= Y1; ++y)
		{
			vRuns.clear();
			for(int x = X0; y < Y1 && x < X1;)
			{
				const size_t First = (size_t)(y - Y0) * Chunk.m_Width + x - X0;
				const STile &Tile = vReadTiles[First];
				if(Tile.m_Index == 0 || ((Tile.m_Flags & TILEFLAG_OPAQUE) != 0) != Opaque)
				{
					++x;
					continue;
				}
				int Width = 1;
				while(x + Width < X1 && SameTile(vReadTiles[First + Width], Tile))
					++Width;
				vRuns.push_back(SRun{x, Width, y, First});
				x += Width;
			}

			// Both lists are ordered by column, so this is one walk over the
			// two of them.
			size_t Above = 0;
			size_t Here = 0;
			while(Above < vGrowing.size() || Here < vRuns.size())
			{
				const bool HaveAbove = Above < vGrowing.size();
				const bool HaveHere = Here < vRuns.size();
				if(HaveAbove && HaveHere && vGrowing[Above].m_X == vRuns[Here].m_X && vGrowing[Above].m_Width == vRuns[Here].m_Width &&
					SameTile(vReadTiles[vGrowing[Above].m_Tile], vReadTiles[vRuns[Here].m_Tile]))
				{
					vRuns[Here].m_FirstY = vGrowing[Above].m_FirstY;
					vRuns[Here].m_Tile = vGrowing[Above].m_Tile;
					++Above;
					++Here;
				}
				else if(!HaveHere || (HaveAbove && vGrowing[Above].m_X <= vRuns[Here].m_X))
				{
					const SRun &Run = vGrowing[Above];
					const STile &Tile = vReadTiles[Run.m_Tile];
					AddTileToBuffer(vTiles, vTextureCoords, Tile.m_Index, Tile.m_Flags, Run.m_X, Run.m_FirstY, Run.m_Width, y - Run.m_FirstY,
						Source.m_Textured, Source.m_FillSpeedup, Tile.m_AngleRotate);
					++Above;
				}
				else
					++Here;
			}
			vGrowing.swap(vRuns);
		}
		if(Opaque)
			Chunk.m_OpaqueQuads = vTiles.size();
	}
	Chunk.m_TransparentQuads = vTiles.size() - Chunk.m_OpaqueQuads;
	if(!UploadTileBuffer(m_pGraphics, vTiles, vTextureCoords, Chunk.m_BufferObject))
	{
		// The chunk stays dirty and is retried every frame, so this is said once
		// rather than once per frame.
		if(!ms_ReportedUploadFailure)
		{
			ms_ReportedUploadFailure = true;
			log_error("tile_chunk_cache", "failed to upload chunk %d,%d with %d quads", ChunkX, ChunkY, (int)vTiles.size());
		}
		return false;
	}
	Chunk.m_Bytes = vTiles.size() * sizeof(CGraphicTile) + vTextureCoords.size() * sizeof(CGraphicTileTextureCoords);
	ms_CachedBytes += Chunk.m_Bytes;
	Chunk.m_SourceDigest = SourceDigest(Source, ChunkX, ChunkY);
	Chunk.m_Dirty = false;
	return true;
}

void CTileChunkCache::ReleaseChunk(CChunk &Chunk)
{
	dbg_assert(ms_CachedBytes >= Chunk.m_Bytes, "tile chunk cache memory accounting underflow");
	ms_CachedBytes -= Chunk.m_Bytes;
	Chunk.m_Bytes = 0;
	Chunk.m_LastUsedTick = 0;
	Chunk.m_OpaqueQuads = 0;
	Chunk.m_TransparentQuads = 0;
	// A buffer the backend refuses to give up stays allocated until it shuts
	// down. The chunk is rebuilt into a new one either way, so the handle has
	// to go regardless of what the destroy answered.
	(void)DeleteTileBuffer(m_pGraphics, Chunk.m_BufferObject);
	Chunk.m_Dirty = true;
}

void CTileChunkCache::EvictOverBudget(uint64_t CurrentTick)
{
	if(ms_CachedBytes <= MEMORY_BUDGET)
		return;
	// One list over every cache: the chunk that should go is the least
	// recently drawn one of the whole map, whichever layer holds it. A layer
	// that left the screen keeps nothing back that the visible ones need.
	std::vector<CChunkUsage> vUsage;
	std::vector<std::pair<CTileChunkCache *, size_t>> vOwners;
	for(CTileChunkCache *pCache : ms_vpCaches)
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
	for(const size_t Index : ChunksToEvict(vUsage, ms_CachedBytes, CurrentTick))
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
	const size_t VisibleColumns = static_cast<size_t>(Visible.m_LastX) - static_cast<size_t>(Visible.m_FirstX) + 1;
	const size_t VisibleRows = static_cast<size_t>(Visible.m_LastY) - static_cast<size_t>(Visible.m_FirstY) + 1;
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
	const uint64_t CurrentTick = ++ms_Tick;
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
			// A chunk of nothing but air has no buffer, and neither has one
			// whose upload failed.
			if(!Chunk.m_BufferObject.IsValid())
				continue;

			// A quad can cover many rows, so the part of a chunk that is on
			// screen is no longer a range of its buffer. The whole chunk is
			// drawn instead, which after the merging is a hundred or so quads
			// rather than the four thousand tiles it used to be.
			uint32_t aFirstIndices[2];
			uint32_t aIndexCounts[2];
			size_t RangeCount = 0;
			const auto AddRange = [&](unsigned int FirstQuad, unsigned int QuadCount) {
				if(QuadCount == 0)
					return;
				aFirstIndices[RangeCount] = FirstQuad * 6;
				aIndexCounts[RangeCount] = QuadCount * 6;
				++RangeCount;
			};
			if(AllTransparent || !TransparentPass)
				AddRange(0, Chunk.m_OpaqueQuads);
			if(AllTransparent || TransparentPass)
				AddRange(Chunk.m_OpaqueQuads, Chunk.m_TransparentQuads);
			if(RangeCount == 0)
				continue;
			m_pGraphics->RenderTileLayer(Chunk.m_BufferObject, Layout, Color, aFirstIndices, aIndexCounts, RangeCount);
		}
	}

	EvictOverBudget(CurrentTick);
}
