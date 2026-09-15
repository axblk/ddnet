#ifndef GAME_MAP_DOCUMENT_TILE_STORE_H
#define GAME_MAP_DOCUMENT_TILE_STORE_H

#include <base/dbg.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_set>
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
 * The list of blocks is itself kept in pieces, and for the same reason. A
 * layer the size of Abyss has some eight thousand blocks, so a flat list of
 * them is 128 KiB of pointers - and a version that copied that list would
 * cost 128 KiB whatever it changed, eight times the block it actually
 * painted. Measured on such a layer, one stroke cost 140 KiB and 2,3 ms.
 * With the list in pages of `CHUNKS_PER_PAGE` blocks, a stroke copies one
 * page and the short list of pages instead, and costs what it touched.
 *
 * Copying a store copies one pointer. The blocks, the pages and the list of
 * pages are taken apart again the moment somebody writes to a store that
 * shares them, which is safe because a document belongs to one thread.
 */
template<typename TTile>
class CTileStore
{
public:
	/** Edge length of one block, in tiles. */
	static constexpr int CHUNK_SIZE = 64;
	static constexpr int TILES_PER_CHUNK = CHUNK_SIZE * CHUNK_SIZE;
	/**
	 * How many blocks are listed on one page of the block list.
	 *
	 * A write copies one page and the list of pages, so this is a trade of
	 * one against the other: 64 is 1 KiB of page against 16 bytes of list
	 * entry per page, which keeps both far below the 16 KiB of the block
	 * that is copied with them.
	 */
	static constexpr int CHUNKS_PER_PAGE = 64;

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
		m_pDirectory = nullptr;
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
		if(m_pDirectory == Other.m_pDirectory)
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
	 * What this store holds, in bytes, counting everything it points at.
	 *
	 * Two versions of a layer both answer with the whole layer even where
	 * they share every block of it - what a *history* of versions costs is a
	 * different question, and `BytesOnce` is the one that answers it.
	 */
	uint64_t Bytes() const
	{
		std::unordered_set<const void *> Seen;
		return BytesOnce(Seen);
	}

	/**
	 * The same, but counting nothing that is already in `Seen`, and putting
	 * everything it counts in there.
	 *
	 * Walked over a whole history this counts every block once, whoever
	 * shares it, which is exactly what a memory limit on the history has to
	 * be held against. It is also why the walk is cheap: a version that
	 * shares a page of the block list with the version before it is a page
	 * that was seen, and the blocks on it are not looked at again.
	 */
	uint64_t BytesOnce(std::unordered_set<const void *> &Seen) const
	{
		if(m_pDirectory == nullptr || !Seen.insert(m_pDirectory.get()).second)
			return 0;
		uint64_t Total = sizeof(CPageRef) * m_pDirectory->size();
		for(const CPageRef &pPage : *m_pDirectory)
		{
			if(pPage == nullptr || !Seen.insert(pPage.get()).second)
				continue;
			Total += sizeof(CPage);
			for(const CChunkRef &pChunk : pPage->m_apChunks)
			{
				if(pChunk != nullptr && Seen.insert(pChunk.get()).second)
					Total += sizeof(CChunk);
			}
		}
		return Total;
	}

	/** How many blocks of this store hold anything at all. */
	int UsedChunks() const
	{
		if(m_pDirectory == nullptr)
			return 0;
		int Used = 0;
		for(const CPageRef &pPage : *m_pDirectory)
		{
			if(pPage == nullptr)
				continue;
			for(const CChunkRef &pChunk : pPage->m_apChunks)
			{
				if(pChunk != nullptr)
					++Used;
			}
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

	class CPage
	{
	public:
		CChunkRef m_apChunks[CHUNKS_PER_PAGE];
	};
	using CPageRef = std::shared_ptr<const CPage>;

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

	size_t ChunkIndex(int ChunkX, int ChunkY) const { return (size_t)ChunkY * m_ChunksAcross + ChunkX; }

	const CChunkRef &ChunkAt(int ChunkX, int ChunkY) const
	{
		static const CChunkRef s_NoChunk;
		if(m_pDirectory == nullptr || ChunkX < 0 || ChunkY < 0 || ChunkX >= m_ChunksAcross || ChunkY >= m_ChunksDown)
			return s_NoChunk;
		const size_t Index = ChunkIndex(ChunkX, ChunkY);
		const CPageRef &pPage = (*m_pDirectory)[Index / CHUNKS_PER_PAGE];
		if(pPage == nullptr)
			return s_NoChunk;
		return pPage->m_apChunks[Index % CHUNKS_PER_PAGE];
	}

	/**
	 * The block at these coordinates, to be written to.
	 *
	 * Whoever else holds this store holds it as it was, so the list of pages,
	 * the page and the block are taken apart from theirs before the first
	 * write reaches them - once each, however many tiles follow.
	 */
	CChunk *MutableChunk(int ChunkX, int ChunkY)
	{
		const size_t Index = ChunkIndex(ChunkX, ChunkY);
		const size_t Pages = ((size_t)m_ChunksAcross * m_ChunksDown + CHUNKS_PER_PAGE - 1) / CHUNKS_PER_PAGE;
		if(m_pDirectory == nullptr)
		{
			m_pDirectory = std::make_shared<std::vector<CPageRef>>(Pages);
		}
		else if(m_pDirectory.use_count() > 1)
		{
			m_pDirectory = std::make_shared<std::vector<CPageRef>>(*m_pDirectory);
		}
		CPageRef &pPage = (*m_pDirectory)[Index / CHUNKS_PER_PAGE];
		if(pPage == nullptr)
		{
			pPage = std::make_shared<CPage>();
		}
		else if(pPage.use_count() > 1)
		{
			pPage = std::make_shared<CPage>(*pPage);
		}
		CChunkRef &pChunk = const_cast<CPage *>(pPage.get())->m_apChunks[Index % CHUNKS_PER_PAGE];
		if(pChunk == nullptr)
		{
			pChunk = std::make_shared<CChunk>();
		}
		else if(pChunk.use_count() > 1)
		{
			pChunk = std::make_shared<CChunk>(*pChunk);
		}
		// The only owner of the block is this page, the page's only owner is
		// this directory, and the directory is only ours, so what is written
		// here reaches nobody else.
		return const_cast<CChunk *>(pChunk.get());
	}

	int m_Width = 0;
	int m_Height = 0;
	int m_ChunksAcross = 0;
	int m_ChunksDown = 0;
	// Left empty for a layer that holds nothing, which is what a layer is
	// until somebody paints on it.
	std::shared_ptr<std::vector<CPageRef>> m_pDirectory;
};

#endif // GAME_MAP_DOCUMENT_TILE_STORE_H
