#ifndef GAME_MAP_TILE_RUN_STORE_H
#define GAME_MAP_TILE_RUN_STORE_H

#include <cstdint>
#include <vector>

/**
 * A tile layer kept as stretches of the same tile instead of one entry per
 * tile.
 *
 * A layer is read once, when it is loaded, and after that only to build the
 * geometry of a chunk. Keeping it as four bytes per tile costs what the map
 * covers rather than what it holds: the tile layers of Abyss are 177.5 M tile
 * places and 677 MiB unpacked, out of a 9.5 MiB file. Almost all of that is
 * the same tile over and over, so it is kept as the stretches it consists of:
 * the same layers are 1.9 M stretches, under 10 MiB, and nothing has to be
 * unpacked again later.
 *
 * The stretches are grouped by the same chunks the geometry is built in and
 * never cross a row, so building one chunk reads one run of bytes - and they
 * are already what the builder wants, which is where the merging of equal
 * tiles into one quad starts.
 *
 * Air is not stored at all. On a large map most of it is air.
 */
class CTileRunStore
{
public:
	/**
	 * Edge length of the group a stretch belongs to, in tiles. The same as
	 * CTileChunkCache::CHUNK_SIZE, which the cache asserts.
	 */
	static constexpr int CHUNK_SIZE = 64;

	/**
	 * One stretch of the same tile, inside one row of one chunk.
	 */
	class CRun
	{
	public:
		unsigned char m_Index = 0;
		unsigned char m_Flags = 0;
		/**
		 * Where the stretch starts inside its chunk, and how far it reaches.
		 */
		unsigned char m_X = 0;
		unsigned char m_Y = 0;
		unsigned char m_Length = 0;
	};

	/**
	 * The smallest rectangle in tiles that holds everything the layer draws.
	 * Empty when it draws nothing at all.
	 */
	class CBounds
	{
	public:
		int m_MinX = 0;
		int m_MinY = 0;
		int m_MaxX = -1;
		int m_MaxY = -1;

		bool IsEmpty() const { return m_MaxX < m_MinX || m_MaxY < m_MinY; }
	};

	/**
	 * Reads a whole layer into the store. Whatever it held is given up first.
	 *
	 * ReadTile is called as ReadTile(x, y) for every tile of the layer, once,
	 * and answers its index and its flags packed into one 16 bit value. It is
	 * a template and not a std::function because that would be an indirect
	 * call per tile, and the tile layers of Abyss are 177.5 M of them. It
	 * answers rather than writes through a pointer for the same reason: a
	 * write through an unsigned char * may touch anything, so the layer's own
	 * pointer would have to be loaded again after every tile.
	 */
	template<class FReadTile>
	void Build(int Width, int Height, FReadTile &&ReadTile)
	{
		Clear();
		if(Width <= 0 || Height <= 0)
			return;

		m_Width = Width;
		m_Height = Height;
		m_Columns = (Width + CHUNK_SIZE - 1) / CHUNK_SIZE;
		m_Rows = (Height + CHUNK_SIZE - 1) / CHUNK_SIZE;
		m_vChunkStart.resize((size_t)m_Columns * m_Rows + 1);

		// The layer is read row by row from left to right, which is how it
		// lies in memory, and every stretch is put aside under the chunk it
		// belongs to. Reading it chunk by chunk instead would jump a whole
		// layer width every 64 tiles; on Abyss that alone was over a second.
		std::vector<std::vector<CRun>> vvColumns(m_Columns);
		for(int ChunkY = 0; ChunkY < m_Rows; ++ChunkY)
		{
			for(std::vector<CRun> &vColumn : vvColumns)
				vColumn.clear();
			const int Y0 = ChunkY * CHUNK_SIZE;
			const int Y1 = Y0 + CHUNK_SIZE < m_Height ? Y0 + CHUNK_SIZE : m_Height;
			for(int y = Y0; y < Y1; ++y)
			{
				int x = 0;
				while(x < m_Width)
				{
					// A stretch stops at the edge of its chunk, so that a
					// chunk is built from its own stretches alone.
					const int ChunkX = x / CHUNK_SIZE;
					const int X1 = (ChunkX + 1) * CHUNK_SIZE < m_Width ? (ChunkX + 1) * CHUNK_SIZE : m_Width;
					const uint16_t Tile = ReadTile(x, y);
					if((Tile & 0xFF) == 0)
					{
						++x;
						continue;
					}
					int Length = 1;
					while(x + Length < X1 && ReadTile(x + Length, y) == Tile)
						++Length;
					CRun Run;
					Run.m_Index = (unsigned char)(Tile & 0xFF);
					Run.m_Flags = (unsigned char)(Tile >> 8);
					Run.m_X = (unsigned char)(x - ChunkX * CHUNK_SIZE);
					Run.m_Y = (unsigned char)(y - Y0);
					Run.m_Length = (unsigned char)Length;
					vvColumns[ChunkX].push_back(Run);
					Grow(x, y, Length);
					x += Length;
				}
			}
			for(int ChunkX = 0; ChunkX < m_Columns; ++ChunkX)
			{
				m_vChunkStart[(size_t)ChunkY * m_Columns + ChunkX] = m_vRuns.size();
				m_vRuns.insert(m_vRuns.end(), vvColumns[ChunkX].begin(), vvColumns[ChunkX].end());
			}
		}
		m_vChunkStart[(size_t)m_Columns * m_Rows] = m_vRuns.size();
		m_vRuns.shrink_to_fit();
	}

	/**
	 * Gives up everything, including the memory.
	 */
	void Clear();

	bool IsBuilt() const { return m_Width > 0 && m_Height > 0; }
	int Width() const { return m_Width; }
	int Height() const { return m_Height; }
	int Columns() const { return m_Columns; }
	int Rows() const { return m_Rows; }

	/**
	 * What the store costs, for anybody who wants to say so.
	 */
	uint64_t Bytes() const;

	/**
	 * The stretches of one chunk, ordered by row and then by column.
	 *
	 * The range is empty for a chunk that is nothing but air, and for one that
	 * is not on the layer at all.
	 */
	const CRun *Begin(int ChunkX, int ChunkY) const;
	const CRun *End(int ChunkX, int ChunkY) const;

	/**
	 * One tile, for the few places that want a single one. This walks the
	 * stretches of the chunk, so it is meant for a handful of tiles and not
	 * for a layer.
	 */
	void ReadTile(int x, int y, unsigned char *pIndex, unsigned char *pFlags) const;

	/**
	 * What the layer covers, worked out while it was read.
	 */
	const CBounds &Bounds() const { return m_Bounds; }

private:
	int m_Width = 0;
	int m_Height = 0;
	int m_Columns = 0;
	int m_Rows = 0;
	// Widens what the layer covers by one stretch.
	void Grow(int x, int y, int Length);

	CBounds m_Bounds;
	std::vector<CRun> m_vRuns;
	// One entry per chunk plus one past the end, so that a chunk's stretches
	// are a subtraction instead of a search.
	std::vector<uint32_t> m_vChunkStart;
};

#endif // GAME_MAP_TILE_RUN_STORE_H
