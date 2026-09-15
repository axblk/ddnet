#ifndef GAME_MAP_QUAD_BUFFER_CACHE_H
#define GAME_MAP_QUAD_BUFFER_CACHE_H

#include <base/color.h>

#include <engine/graphics.h>

#include <game/map/envelope_extrema.h>
#include <game/map/render_map.h>
#include <game/mapitems.h>

#include <functional>
#include <optional>
#include <vector>

class IEnvelopeEval;

/**
 * The quads of one layer on the GPU, drawn in as few calls as the envelopes
 * allow.
 *
 * Quads that share their envelopes and offsets animate as one, so a run of
 * them is a single draw with a single render info; a run that does not is
 * drawn per quad, up to what the backend takes in one call. The envelopes
 * themselves are evaluated per frame - they move - only the geometry is kept.
 *
 * The map is read once and the editor writes to it, so the cache is told when
 * the quads changed. A change in their number or in whether the layer has an
 * image is noticed here.
 */
class CQuadBufferCache
{
public:
	/**
	 * Extrema of a position envelope, to clip a cluster against the screen.
	 * Leave it unset and nothing is clipped, which is always correct and
	 * what an editor wants while its envelopes are being edited.
	 */
	using FEnvelopeExtrema = std::function<const CEnvelopeExtrema::CEnvelopeExtremaItem &(int EnvelopeIndex)>;

	class CQuadSource
	{
	public:
		const CQuad *m_pQuads = nullptr;
		int m_NumQuads = 0;
		/**
		 * Whether the layer draws from an image. It decides the vertex layout,
		 * so changing it rebuilds the buffer.
		 */
		bool m_Textured = false;
		FEnvelopeExtrema m_Extrema;
	};

	CQuadBufferCache() = default;
	CQuadBufferCache(const CQuadBufferCache &) = delete;
	CQuadBufferCache &operator=(const CQuadBufferCache &) = delete;

	void OnInit(IGraphics *pGraphics) { m_pGraphics = pGraphics; }

	/**
	 * The quads changed. The buffer and the clusters are built again on the
	 * next render.
	 */
	void Invalidate() { m_Dirty = true; }

	/**
	 * Builds what is not built yet, so the clip is known before the first
	 * render. Rendering does this on its own.
	 */
	void Build(const CQuadSource &Source)
	{
		if(m_Dirty)
			Rebuild(Source);
	}

	/**
	 * Gives the buffer back. Returns whether anything was held.
	 */
	bool Clear();

	/**
	 * Draws the layer. Envelopes are evaluated here, for the visible clusters
	 * only, every frame.
	 */
	void Render(const CQuadSource &Source, const IEnvelopeEval *pEnvelopeEval, float Alpha);

	/**
	 * The clusters, for a renderer that draws their clip regions.
	 */
	void EachClip(const std::function<void(const CClipRegion &, int StartIndex, bool Grouped)> &Callback) const;

	/**
	 * Everything the layer covers, or nothing when no cluster could be clipped.
	 */
	const std::optional<CClipRegion> &LayerClip() const { return m_LayerClip; }

private:
	class CQuadCluster
	{
	public:
		bool m_Grouped;
		int m_StartIndex;
		int m_NumQuads;

		int m_PosEnv;
		float m_PosEnvOffset;
		int m_ColorEnv;
		float m_ColorEnvOffset;

		std::vector<SQuadRenderInfo> m_vQuadRenderInfo;
		std::optional<CClipRegion> m_ClipRegion;
	};

	void Rebuild(const CQuadSource &Source);
	void CalculateClipping(const CQuadSource &Source, CQuadCluster &Cluster);
	bool CalculateQuadClipping(const CQuadSource &Source, const CQuadCluster &Cluster, float aOffsetMin[2], float aOffsetMax[2]) const;
	bool IsVisible(const std::optional<CClipRegion> &ClipRegion) const;

	IGraphics *Graphics() const { return m_pGraphics; }

	IGraphics *m_pGraphics = nullptr;
	IGraphics::CBufferHandle m_BufferObject;
	IGraphics::EVertexLayout m_Layout = IGraphics::EVertexLayout::QUAD;
	std::vector<CQuadCluster> m_vClusters;
	std::optional<CClipRegion> m_LayerClip;
	int m_BuiltNumQuads = -1;
	bool m_BuiltTextured = false;
	bool m_Dirty = true;
};

#endif
