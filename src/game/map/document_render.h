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

	/** What a group puts on the screen before its layers are drawn. */
	bool UseGroup(const map_document::CGroup &Group, const CParams &Params);
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
	size_t m_InvalidatedChunks = 0;
};

#endif // GAME_MAP_DOCUMENT_RENDER_H
