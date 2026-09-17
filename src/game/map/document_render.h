#ifndef GAME_MAP_DOCUMENT_RENDER_H
#define GAME_MAP_DOCUMENT_RENDER_H

#include <base/vmath.h>

#include <engine/graphics.h>

#include <game/map/document/map_state.h>
#include <game/map/document_source.h>
#include <game/map/quad_buffer_cache.h>
#include <game/map/render_interfaces.h>
#include <game/map/tile_chunk_cache.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

/**
 * Draws a version of a map document.
 *
 * The renderer that draws a map read from a file works from the file: its
 * layers hold pointers into the map's data, one flat array per layer, and they
 * are built once when the map is loaded. A document is the other way round -
 * it changes while it is being drawn, and what it holds is blocks that
 * versions share - so this is the piece between the two: one chunk cache per
 * tile layer, fed by `DocumentLayerSource`.
 *
 * What makes that worth having is what happens when the version changes.
 * `Use` is given the new one, and every layer that is the same node as before
 * keeps its geometry untouched; a layer that was painted keeps it too, except
 * for the blocks that actually changed - which the two versions can be asked
 * for without looking at a tile (`CTileStore::ForEachChangedChunk`). A stroke
 * on a large map therefore rebuilds the one chunk under the brush, not the
 * layer and not the map.
 */
class CDocumentRenderer
{
public:
	/**
	 * Where the view looks and what of the map is drawn.
	 *
	 * The same parameters the standalone map view takes, because they say the
	 * same thing: an editor is a window onto a map like any other.
	 */
	class CParams
	{
	public:
		vec2 m_Center = vec2(0.0f, 0.0f);
		float m_Zoom = 1.0f;
		/**
		 * How much of the world the view shows before the zoom, in world
		 * units, or zero for the one that fits the surface.
		 */
		vec2 m_ViewSize = vec2(0.0f, 0.0f);
		/** Which part of the view to draw, for a picture drawn in pieces. */
		CScreenRect m_Window = CScreenRect(0.0f, 0.0f, 1.0f, 1.0f);
		/** Whether the layers the map marks as detail are drawn. */
		bool m_HighDetail = true;
		/**
		 * The moment of the envelopes that is drawn. An editor that is not
		 * animating leaves this where it is, and then a colour envelope shows
		 * what it does at that moment rather than nothing at all.
		 */
		int m_TimeOffsetMillis = 0;
		/**
		 * How strongly the physics layers are drawn over the design, from 0
		 * to 100. At 100 the design is left out and what is left is what the
		 * map does rather than what it looks like.
		 */
		int m_EntityOverlayVal = 0;
		/**
		 * Which layers not to draw, as the number of the group and the number
		 * of the layer in it. An editor hides a layer to look under it;
		 * nothing else that draws a map has a reason to.
		 */
		const std::vector<std::pair<size_t, size_t>> *m_pHidden = nullptr;
		/**
		 * How many tiles apart the lines of the grid are, or 0 for no grid.
		 *
		 * The grid is drawn by the renderer rather than by whatever is around
		 * it because it lies in the world: it follows the parallax and the
		 * offset of the group it belongs to, so that its lines sit on that
		 * group's tiles at every zoom.
		 */
		int m_Grid = 0;
		/** Which group the grid follows - the one being worked in. */
		size_t m_GridGroup = 0;
		/**
		 * A rectangle of tiles to mark, for a gesture that is about an area
		 * rather than about a tile: taking a piece of a layer into the brush,
		 * filling it, rubbing it out. Nothing is marked while it is empty.
		 *
		 * It is drawn here for the same reason the grid is - it lies in the
		 * world, in the tiles of one group.
		 */
		class CMarked
		{
		public:
			size_t m_Group = 0;
			int m_X = 0;
			int m_Y = 0;
			int m_Width = 0;
			int m_Height = 0;

			bool Empty() const { return m_Width <= 0 || m_Height <= 0; }
		};
		CMarked m_Marked;
		/**
		 * The quad whose corners are shown, for somebody dragging them.
		 *
		 * Drawn here for the same reason the grid and the mark are: a quad
		 * lies in the world, in the coordinates of the group it is in, and
		 * whatever is around the renderer would have to do the parallax sum
		 * again to put a handle on one.
		 */
		class CShownQuad
		{
		public:
			size_t m_Group = 0;
			size_t m_Layer = 0;
			size_t m_Quad = 0;
			bool m_Shown = false;
		};
		CShownQuad m_ShownQuad;
		/**
		 * What the brush would do at the place the pointer is over, before
		 * the button goes down: the brush itself drawn faintly where a stamp
		 * would put it, the rectangle a fill would cover with the brush
		 * repeated over it, the rectangle a rubber would clear, or just an
		 * outline around the tile that is under the pointer.
		 *
		 * Drawn here for the same reason the mark is - it lies in the world,
		 * in the tiles of one group, and it has to follow that group's
		 * parallax and offset to sit where the stamp will land.
		 */
		class CGhost
		{
		public:
			enum EKind
			{
				NONE = 0,
				/** The brush, once, at the top left corner. */
				STAMP,
				/** The rectangle, covered with what a fill would put there. */
				FILL,
				/** The rectangle, marked as what a rubber would clear. */
				ERASE,
				/** Only the outline of the rectangle. */
				SPOT,
			};
			int m_Kind = NONE;
			size_t m_Group = 0;
			int m_X = 0;
			int m_Y = 0;
			int m_Width = 0;
			int m_Height = 0;

			bool Shown() const { return m_Kind != NONE; }
		};
		CGhost m_Ghost;
	};

	/**
	 * @param pGraphics Where it is drawn, and what outlives the caches.
	 * @param pImages The pictures the layers are drawn with.
	 */
	void OnInit(IGraphics *pGraphics, IMapImages *pImages);

	/**
	 * The version to draw from now on.
	 *
	 * Holds on to it, because the geometry on the graphics card is geometry of
	 * that version: it is read again whenever a chunk is rebuilt, which
	 * happens long after this returns.
	 */
	void Use(std::shared_ptr<const map_document::CMapState> pMap);

	/**
	 * The tiles the ghost is drawn with: the brush, or for a fill the brush
	 * already repeated over the rectangle. Held on to like the version is,
	 * because its geometry is built lazily; the same pointer twice costs
	 * nothing, another one is built again on the next frame. `nullptr` or
	 * anything but a tile layer leaves the ghost as an outline.
	 */
	void UseGhost(const std::shared_ptr<const map_document::CLayer> &pTiles);

	/** Draws the version that was last handed over. */
	void Render(const CParams &Params);

	/** Gives up every chunk, and the version with it. */
	void Clear();

	/**
	 * How many chunks the last `Use` marked for rebuilding.
	 *
	 * This is the number the whole design is for, so it is worth being able to
	 * ask for it: painting one tile has to answer one, whatever the map.
	 */
	size_t InvalidatedChunks() const { return m_InvalidatedChunks; }

private:
	/**
	 * One layer as it is drawn: the version of it that the geometry was built
	 * from, and the geometry. A layer holds one or the other of the two
	 * caches, whichever its kind draws through.
	 */
	class CLayerCache
	{
	public:
		std::shared_ptr<const map_document::CLayer> m_pLayer;
		size_t m_Group = 0;
		size_t m_Layer = 0;
		std::unique_ptr<CTileChunkCache> m_pTiles;
		CTileChunkCache::CLayerSource m_TileSource;
		std::unique_ptr<CQuadBufferCache> m_pQuads;
		CQuadBufferCache::CQuadSource m_QuadSource;
	};

	/**
	 * What a group puts on the screen before its layers are drawn.
	 *
	 * @param Group The group that is about to be drawn.
	 * @param Params Where the view looks.
	 * @param pWorld Where the piece of the world the group shows is put, for
	 * whoever has to draw in those coordinates afterwards.
	 *
	 * @return Whether anything of the group is on the screen at all.
	 */
	bool UseGroup(const map_document::CGroup &Group, const CParams &Params, CScreenRect *pWorld = nullptr);

public:
	/**
	 * The piece of the world a group shows, for the view these parameters
	 * describe.
	 *
	 * Out here because it is the only sum that turns a place on the surface
	 * into a place in a group - what a pointer over a quad needs - and having
	 * it twice would mean two answers that slowly stop agreeing.
	 *
	 * @param Group The group to look through.
	 * @param Params Where the view looks.
	 *
	 * @return The rectangle of that group's world that fills the surface.
	 */
	CScreenRect GroupScreen(const map_document::CGroup &Group, const CParams &Params) const;

private:
	/**
	 * Draws the grid over the group that was last put on the screen.
	 *
	 * @param World The piece of the world that group shows.
	 * @param Spacing How many tiles apart the lines are.
	 */
	void RenderGrid(const CScreenRect &World, int Spacing);
	/**
	 * Draws the marked rectangle over the group that was last put on the
	 * screen.
	 *
	 * @param Marked Which tiles are marked.
	 */
	void RenderMarked(const CParams::CMarked &Marked);
	/**
	 * Draws the ghost over the group that was last put on the screen.
	 *
	 * @param Ghost What the brush would do and where.
	 * @param World The piece of the world that group shows.
	 */
	void RenderGhost(const CParams::CGhost &Ghost, const CScreenRect &World);
	/**
	 * Draws the outline of one quad and a handle on each of its five points,
	 * over the group that was last put on the screen.
	 *
	 * @param Quad The quad to draw handles on.
	 */
	void RenderQuadHandles(const CQuad &Quad);
	void RenderTileLayer(const map_document::CTileLayer &Layer, CLayerCache &Cache, const CParams &Params);
	void RenderQuadLayer(const map_document::CQuadLayer &Layer, CLayerCache &Cache, const CParams &Params);
	/** Points a cache at the layer it draws, and says what changed about it. */
	void UseLayer(CLayerCache &Cache, const std::shared_ptr<const map_document::CLayer> &pLayer);
	/**
	 * What the envelopes of the version say, for whoever draws with them.
	 *
	 * The sum itself is the map renderer's - what a document brings is where
	 * the points are: in the version, as a list that a change copies rather
	 * than a range of one array that the file holds.
	 */
	class CEnvelopes final : public IEnvelopeEval
	{
	public:
		void Use(const map_document::CMapState *pMap) { m_pMap = pMap; }
		void SetTimeOffset(int TimeOffsetMillis) { m_TimeOffsetMillis = TimeOffsetMillis; }
		void EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const override;

	private:
		const map_document::CMapState *m_pMap = nullptr;
		int m_TimeOffsetMillis = 0;
	};

	CEnvelopes m_Envelopes;
	/**
	 * Marks the blocks in which the two versions of one layer differ, and says
	 * how many that was.
	 */
	size_t Invalidate(CLayerCache &Cache, const map_document::CTileLayer &Older, const map_document::CTileLayer &Newer);

	IGraphics *m_pGraphics = nullptr;
	IMapImages *m_pImages = nullptr;
	std::shared_ptr<const map_document::CMapState> m_pMap;
	std::vector<std::unique_ptr<CLayerCache>> m_vpCaches;
	std::unique_ptr<CLayerCache> m_pGhost;
	size_t m_InvalidatedChunks = 0;
};

#endif // GAME_MAP_DOCUMENT_RENDER_H
