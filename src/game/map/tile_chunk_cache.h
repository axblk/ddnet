#ifndef GAME_MAP_TILE_CHUNK_CACHE_H
#define GAME_MAP_TILE_CHUNK_CACHE_H

#include <base/color.h>

#include <engine/graphics.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <vector>

class CTile;

/**
 * Buffered tile rendering in fixed-size chunks.
 *
 * A tile layer is cut into square chunks; each one keeps its own GPU buffer and
 * is rebuilt only after the tiles it covers changed. Rendering walks the chunks
 * the screen actually touches and draws one index range per blend pass, so an
 * edit costs one chunk instead of the whole layer and a zoomed-in view costs
 * what is visible instead of the whole map.
 *
 * The cache owns no tiles. The caller passes a source on every render, which is
 * what lets the editor hand over an array it reallocates on resize and the
 * client hand over layers whose tiles are not `CTile` at all.
 */
class CTileChunkCache
{
public:
	/**
	 * Reads one tile of the layer.
	 *
	 * Called only while a chunk is being built, never per frame. The outputs
	 * are initialized before the call, so a source only has to write what it
	 * knows about.
	 *
	 * @param x Tile column.
	 * @param y Tile row.
	 * @param pIndex Tile index, `0` for a tile that is not drawn.
	 * @param pFlags Tile flags, `TILEFLAG_OPAQUE` decides the blend pass.
	 * @param pAngleRotate Rotation in degrees, only read for speedup tiles.
	 */
	typedef std::function<void(int x, int y, unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate)> FReadTile;

	/**
	 * Everything the cache needs to build the geometry of one tile layer.
	 */
	class CLayerSource
	{
	public:
		FReadTile m_ReadTile;
		int m_Width = 0;
		int m_Height = 0;
		/**
		 * Whether the layer has a texture, which decides whether texture
		 * coordinates are part of the buffers.
		 */
		bool m_Textured = false;
		/**
		 * Whether the tiles are speedup arrows, which are rotated by their
		 * angle instead of drawn from their index.
		 */
		bool m_FillSpeedup = false;
	};

	/**
	 * Edge length of one chunk in tiles.
	 *
	 * Large enough that a full-map view does not issue thousands of draws,
	 * small enough that painting a tile does not rebuild megabytes.
	 */
	static constexpr int CHUNK_SIZE = 64;

	CTileChunkCache() = default;
	// The chunks own GPU buffers, so a copy would free them twice
	CTileChunkCache(const CTileChunkCache &Other) = delete;
	CTileChunkCache &operator=(const CTileChunkCache &Other) = delete;
	~CTileChunkCache();

	/**
	 * @param pGraphics Graphics interface used for all buffers, must outlive
	 * the cache.
	 */
	void OnInit(IGraphics *pGraphics);

	/**
	 * Marks every chunk for rebuild, for changes whose extent is not known.
	 */
	void Invalidate();

	/**
	 * The chunks a tile rectangle touches, as an inclusive range.
	 *
	 * Empty when the rectangle does not reach the layer at all, which is what
	 * `m_FirstX > m_LastX` says.
	 */
	class CChunkRange
	{
	public:
		int m_FirstX = 0;
		int m_FirstY = 0;
		int m_LastX = -1;
		int m_LastY = -1;

		bool IsEmpty() const { return m_FirstX > m_LastX || m_FirstY > m_LastY; }
	};

	/**
	 * Maps a tile rectangle onto the chunk grid of a layer.
	 *
	 * @param x Left tile of the rectangle, may lie outside the layer.
	 * @param y Top tile of the rectangle, may lie outside the layer.
	 * @param w Width of the rectangle in tiles.
	 * @param h Height of the rectangle in tiles.
	 * @param Width Layer width in tiles.
	 * @param Height Layer height in tiles.
	 */
	static CChunkRange ChunkRange(int x, int y, int w, int h, int Width, int Height)
	{
		CChunkRange Range;
		if(Width <= 0 || Height <= 0 || w <= 0 || h <= 0)
			return Range;

		const int X0 = std::clamp(x, 0, Width);
		const int Y0 = std::clamp(y, 0, Height);
		const int X1 = std::clamp(x + w, 0, Width);
		const int Y1 = std::clamp(y + h, 0, Height);
		if(X0 >= X1 || Y0 >= Y1)
			return Range;

		Range.m_FirstX = X0 / CHUNK_SIZE;
		Range.m_FirstY = Y0 / CHUNK_SIZE;
		Range.m_LastX = (X1 - 1) / CHUNK_SIZE;
		Range.m_LastY = (Y1 - 1) / CHUNK_SIZE;
		return Range;
	}

	/**
	 * Marks the chunks that a tile rectangle touches for rebuild.
	 *
	 * @param x Left tile of the changed rectangle.
	 * @param y Top tile of the changed rectangle.
	 * @param w Width of the changed rectangle in tiles.
	 * @param h Height of the changed rectangle in tiles.
	 */
	void InvalidateArea(int x, int y, int w, int h);

	/**
	 * Releases all GPU buffers. The next render rebuilds what it needs.
	 */
	void Clear();

	/**
	 * Draws the visible part of one blend pass of a tile layer.
	 *
	 * @param Source The layer to draw.
	 * @param Color Color the layer is drawn with.
	 * @param TransparentPass Whether this is the transparent pass.
	 * @param ForceTransparent Whether opaque tiles are drawn in the
	 * transparent pass too, as entity layers need.
	 */
	void Render(const CLayerSource &Source, const ColorRGBA &Color, bool TransparentPass, bool ForceTransparent);

private:
	class CChunk
	{
	public:
		IGraphics::CBufferHandle m_BufferObject;
		IGraphics::CBufferContainerHandle m_BufferContainer;
		unsigned int m_OpaqueTiles = 0;
		unsigned int m_TransparentTiles = 0;
		int m_Width = 0;
		int m_Height = 0;
		// Tile index -> first quad of that tile, one entry past the end so
		// that a range of tiles is a subtraction instead of a search. A chunk
		// holds at most CHUNK_SIZE * CHUNK_SIZE quads, so 16 bit are enough and
		// the two tables together cost no more than one 32 bit table per tile.
		std::vector<uint16_t> m_vOpaqueTileOffsets;
		std::vector<uint16_t> m_vTransparentTileOffsets;
		bool m_Dirty = true;
	};

	void Ensure(int Width, int Height, bool Textured);
	bool Rebuild(const CLayerSource &Source, int ChunkX, int ChunkY);

	IGraphics *m_pGraphics = nullptr;
	std::vector<CChunk> m_vChunks;
	int m_Columns = 0;
	int m_Rows = 0;
	int m_Width = 0;
	int m_Height = 0;
	bool m_Textured = false;
};

/**
 * Builds a source for a plain tile layer.
 *
 * @param pTiles Tiles of the layer, `Width * Height` entries, must outlive the
 * source.
 * @param Width Layer width in tiles.
 * @param Height Layer height in tiles.
 * @param Textured Whether the layer has a texture.
 *
 * @return The source, to be passed to `CTileChunkCache::Render`.
 */
CTileChunkCache::CLayerSource TileLayerSource(const CTile *pTiles, int Width, int Height, bool Textured);

#endif // GAME_MAP_TILE_CHUNK_CACHE_H
