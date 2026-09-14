#ifndef GAME_MAP_DOCUMENT_TILE_STORE_H
#define GAME_MAP_DOCUMENT_TILE_STORE_H

#include <base/dbg.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

/**
 * The tiles of one layer, held so that a copy is cheap and a change is small.
 *
 * The editor's undo is a list of versions of the whole map, and a version is
 * only affordable if the parts that did not change are the same parts. So the
 * tiles are kept in squares of 64 by 64, each square a block nobody ever
 * writes to: a tile is changed by making a new block and putting it in place
 * of the old one, and every other block of that layer - and every other layer
 * - stays exactly where it was, shared between the two versions.
 *
 * The squares are the ones the renderer builds its geometry in
 * (`CTileChunkCache::CHUNK_SIZE`), so a block that is replaced is a piece of
 * geometry that has to be built again, and no more than that.
 *
 * A layer that holds nothing holds nothing: a square of air is no block at
 * all, which is what makes a large map affordable - most of Abyss is air.
 *
 * Copying a store copies one pointer. The blocks and the table of blocks are
 * taken apart again the moment somebody writes to a store that shares them,
 * which is safe because a document belongs to one thread.
 */
template<typename TTile>
class CTileStore
{
public:
	/** Edge length of one block, in tiles. */
	static constexpr int CHUNK_SIZE = 64;
	static constexpr int TILES_PER_CHUNK = CHUNK_SIZE * CHUNK_SIZE;

	CTileStore() = default;
	CTileStore(int Width, int Height) { Reset(Width, Height); }

	int Width() const { return m_Width; }
	int Height() const { return m_Height; }
	int ChunksAcross() const { return m_ChunksAcross; }
	int ChunksDown() const { return m_ChunksDown; }

	/**
	 * Gives the store a size and nothing in it.
	 */
	void Reset(int Width, int Height)
	{
		dbg_assert(Width >= 0 && Height >= 0, "A layer cannot be smaller than nothing: %dx%d", Width, Height);
		m_Width = Width;
		m_Height = Height;
		m_ChunksAcross = (Width + CHUNK_SIZE - 1) / CHUNK_SIZE;
		m_ChunksDown = (Height + CHUNK_SIZE - 1) / CHUNK_SIZE;
		m_pTable = nullptr;
	}

	/**
	 * One tile. Anything outside the layer is air, so that a brush hanging
	 * over the edge is a question that can be asked rather than one that has
	 * to be avoided.
	 */
	TTile Get(int x, int y) const
	{
		if(x < 0 || y < 0 || x >= m_Width || y >= m_Height)
			return TTile{};
		const std::shared_ptr<const CChunk> &pChunk = ChunkAt(x / CHUNK_SIZE, y / CHUNK_SIZE);
		if(pChunk == nullptr)
			return TTile{};
		return pChunk->m_aTiles[(y % CHUNK_SIZE) * CHUNK_SIZE + (x % CHUNK_SIZE)];
	}

	/**
	 * Puts one tile in place. Outside the layer it does nothing, for the same
	 * reason `Get` answers air there.
	 */
	void Set(int x, int y, const TTile &Tile)
	{
		if(x < 0 || y < 0 || x >= m_Width || y >= m_Height)
			return;
		const int ChunkX = x / CHUNK_SIZE;
		const int ChunkY = y / CHUNK_SIZE;
		const int Offset = (y % CHUNK_SIZE) * CHUNK_SIZE + (x % CHUNK_SIZE);
		// Air written into air is not a change, and a layer that is mostly
		// air is asked to write air into it all the time: a brush of five
		// tiles carries the three corners it does not paint.
		const std::shared_ptr<const CChunk> &pChunk = ChunkAt(ChunkX, ChunkY);
		if(pChunk == nullptr && IsAir(Tile))
			return;
		if(pChunk != nullptr && Same(pChunk->m_aTiles[Offset], Tile))
			return;
		MutableChunk(ChunkX, ChunkY)->m_aTiles[Offset] = Tile;
	}

	/**
	 * The block a piece of the layer lies in, to be read straight out of, or
	 * `nullptr` where that piece is nothing but air.
	 *
	 * The tiles of a block are `CHUNK_SIZE` wide whatever the layer is, so a
	 * layer whose width is not a multiple of that has tiles in its last
	 * column of blocks that are not on the layer. They are air and stay air.
	 */
	const TTile *Chunk(int ChunkX, int ChunkY) const
	{
		const std::shared_ptr<const CChunk> &pChunk = ChunkAt(ChunkX, ChunkY);
		return pChunk == nullptr ? nullptr : pChunk->m_aTiles;
	}

	/**
	 * What a block is, rather than what is in it: two versions of a layer
	 * whose blocks answer the same here hold the same tiles, and the one
	 * that changed is the one that does not.
	 *
	 * This is what the renderer and the writer are meant to compare, because
	 * comparing the tiles themselves is the work they are trying to avoid.
	 */
	const void *ChunkId(int ChunkX, int ChunkY) const { return ChunkAt(ChunkX, ChunkY).get(); }

	/**
	 * Whether two stores hold the same tiles. Blocks that are the same block
	 * are not looked into, so this is cheap between two versions of one
	 * layer and dear between two layers that were built separately.
	 */
	bool operator==(const CTileStore &Other) const
	{
		if(m_Width != Other.m_Width || m_Height != Other.m_Height)
			return false;
		if(m_pTable == Other.m_pTable)
			return true;
		for(int ChunkY = 0; ChunkY < m_ChunksDown; ++ChunkY)
		{
			for(int ChunkX = 0; ChunkX < m_ChunksAcross; ++ChunkX)
			{
				const std::shared_ptr<const CChunk> &pMine = ChunkAt(ChunkX, ChunkY);
				const std::shared_ptr<const CChunk> &pTheirs = Other.ChunkAt(ChunkX, ChunkY);
				if(pMine == pTheirs)
					continue;
				if(pMine == nullptr || pTheirs == nullptr)
				{
					const CChunk *pOne = pMine == nullptr ? pTheirs.get() : pMine.get();
					if(!IsAllAir(*pOne))
						return false;
					continue;
				}
				if(std::memcmp(pMine->m_aTiles, pTheirs->m_aTiles, sizeof(pMine->m_aTiles)) != 0)
					return false;
			}
		}
		return true;
	}
	bool operator!=(const CTileStore &Other) const { return !(*this == Other); }

	/**
	 * What this store holds that no other store holds with it, in bytes: the
	 * blocks it is the only owner of, and its share of the rest.
	 *
	 * A number for somebody watching the memory go up, not for a decision -
	 * the same layer answers differently depending on who else is holding
	 * onto a version of it.
	 */
	uint64_t Bytes() const
	{
		if(m_pTable == nullptr)
			return 0;
		uint64_t Total = sizeof(CChunkRef) * m_pTable->size();
		for(const CChunkRef &pChunk : *m_pTable)
		{
			if(pChunk != nullptr)
				Total += sizeof(CChunk) / (uint64_t)std::max<long>(1, pChunk.use_count());
		}
		return Total;
	}

	/** How many blocks of this store hold anything at all. */
	int UsedChunks() const
	{
		if(m_pTable == nullptr)
			return 0;
		int Used = 0;
		for(const CChunkRef &pChunk : *m_pTable)
		{
			if(pChunk != nullptr)
				++Used;
		}
		return Used;
	}

private:
	class CChunk
	{
	public:
		TTile m_aTiles[TILES_PER_CHUNK] = {};
	};
	using CChunkRef = std::shared_ptr<const CChunk>;

	static bool Same(const TTile &One, const TTile &Other)
	{
		return std::memcmp(&One, &Other, sizeof(TTile)) == 0;
	}
	static bool IsAir(const TTile &Tile)
	{
		return Same(Tile, TTile{});
	}
	static bool IsAllAir(const CChunk &Chunk)
	{
		static const CChunk s_Air;
		return std::memcmp(Chunk.m_aTiles, s_Air.m_aTiles, sizeof(s_Air.m_aTiles)) == 0;
	}

	const CChunkRef &ChunkAt(int ChunkX, int ChunkY) const
	{
		static const CChunkRef s_NoChunk;
		if(m_pTable == nullptr || ChunkX < 0 || ChunkY < 0 || ChunkX >= m_ChunksAcross || ChunkY >= m_ChunksDown)
			return s_NoChunk;
		return (*m_pTable)[(size_t)ChunkY * m_ChunksAcross + ChunkX];
	}

	/**
	 * The block at these coordinates, to be written to.
	 *
	 * Whoever else holds this store holds it as it was, so both the table and
	 * the block are taken apart from theirs before the first write reaches
	 * them - once each, however many tiles follow.
	 */
	CChunk *MutableChunk(int ChunkX, int ChunkY)
	{
		if(m_pTable == nullptr)
		{
			m_pTable = std::make_shared<std::vector<CChunkRef>>((size_t)m_ChunksAcross * m_ChunksDown);
		}
		else if(m_pTable.use_count() > 1)
		{
			m_pTable = std::make_shared<std::vector<CChunkRef>>(*m_pTable);
		}
		CChunkRef &pChunk = (*m_pTable)[(size_t)ChunkY * m_ChunksAcross + ChunkX];
		if(pChunk == nullptr)
		{
			pChunk = std::make_shared<CChunk>();
		}
		else if(pChunk.use_count() > 1)
		{
			pChunk = std::make_shared<CChunk>(*pChunk);
		}
		// The only owner of the block is this table, and this table is only
		// ours, so what is written here reaches nobody else.
		return const_cast<CChunk *>(pChunk.get());
	}

	int m_Width = 0;
	int m_Height = 0;
	int m_ChunksAcross = 0;
	int m_ChunksDown = 0;
	// Left empty for a layer that holds nothing, which is what a layer is
	// until somebody paints on it.
	std::shared_ptr<std::vector<CChunkRef>> m_pTable;
};

#endif // GAME_MAP_DOCUMENT_TILE_STORE_H
