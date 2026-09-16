#include <base/math.h>

#include <engine/shared/config.h>

#include <game/map/document_render.h>
#include <game/map/render_layer.h>
#include <game/map/render_map.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>
#include <variant>

using namespace map_document;

namespace
{
	/**
	 * The points of one envelope of the version, for the sum that the map
	 * renderer already knows how to do.
	 */
	class CDocumentEnvelopePoints final : public IEnvelopePointAccess
	{
	public:
		explicit CDocumentEnvelopePoints(const CEnvelope &Envelope) :
			m_pPoints(&Envelope.m_Points) {}

		int NumPoints() const override { return (int)m_pPoints->Size(); }
		const CEnvPoint *GetPoint(int Index) const override
		{
			if(Index < 0 || (size_t)Index >= m_pPoints->Size())
				return nullptr;
			return &(*m_pPoints)[Index];
		}
		const CEnvPointBezier *GetBezier(int Index) const override
		{
			if(Index < 0 || (size_t)Index >= m_pPoints->Size())
				return nullptr;
			return &(*m_pPoints)[Index].m_Bezier;
		}

	private:
		const CSharedList<CEnvPoint_runtime> *m_pPoints;
	};

	/** The colour a layer is drawn with, before the picture is sampled. */
	ColorRGBA LayerColor(const CTileLayer &Layer, int EntityOverlayVal)
	{
		if(Layer.m_Kind != ETileLayerKind::TILES)
		{
			// A physics layer is the overlay: it is drawn out of the entities
			// sheet at whatever strength the overlay is turned up to.
			return ColorRGBA(1.0f, 1.0f, 1.0f, EntityOverlayVal / 100.0f);
		}
		ColorRGBA Color(Layer.m_Color.r / 255.0f, Layer.m_Color.g / 255.0f, Layer.m_Color.b / 255.0f, Layer.m_Color.a / 255.0f);
		if(EntityOverlayVal > 0)
			Color.a *= (100 - EntityOverlayVal) / 100.0f;
		return Color;
	}

	bool IsDrawn(const CTileLayer &Layer, const CDocumentRenderer::CParams &Params)
	{
		if(Layer.m_Detail && !Params.m_HighDetail)
			return false;
		if(Layer.m_Kind == ETileLayerKind::TILES)
			return Params.m_EntityOverlayVal < 100;
		return Params.m_EntityOverlayVal > 0;
	}
} // namespace

void CDocumentRenderer::OnInit(IGraphics *pGraphics, IMapImages *pImages)
{
	m_pGraphics = pGraphics;
	m_pImages = pImages;
}

void CDocumentRenderer::Clear()
{
	m_vpCaches.clear();
	m_pMap = nullptr;
	m_InvalidatedChunks = 0;
}

size_t CDocumentRenderer::Invalidate(CLayerCache &Cache, const CTileLayer &Older, const CTileLayer &Newer)
{
	size_t Changed = 0;
	const auto Mark = [&](int ChunkX, int ChunkY) {
		++Changed;
		Cache.m_pTiles->InvalidateArea(ChunkX * CTileChunkCache::CHUNK_SIZE, ChunkY * CTileChunkCache::CHUNK_SIZE,
			CTileChunkCache::CHUNK_SIZE, CTileChunkCache::CHUNK_SIZE);
	};
	Newer.m_Tiles.ForEachChangedChunk(Older.m_Tiles, Mark);
	// A physics layer is drawn out of its second plane, so that is the one
	// whose blocks decide what has to be built again.
	std::visit([&](const auto &Extra) {
		using TStore = std::decay_t<decltype(Extra)>;
		if constexpr(!std::is_same_v<TStore, std::monostate>)
		{
			if(std::holds_alternative<TStore>(Older.m_ExtraTiles))
				Extra.ForEachChangedChunk(std::get<TStore>(Older.m_ExtraTiles), Mark);
			else
				Cache.m_pTiles->Invalidate();
		}
	},
		Newer.m_ExtraTiles);
	return Changed;
}

void CDocumentRenderer::UseLayer(CLayerCache &Cache, const std::shared_ptr<const CLayer> &pLayer)
{
	Cache.m_pLayer = pLayer;
	if(std::holds_alternative<CTileLayer>(*pLayer))
	{
		// The source holds the version it reads, so it is made anew for every
		// version - what it costs is a handful of pointers.
		Cache.m_TileSource = DocumentLayerSource(pLayer);
		return;
	}
	const CQuadLayer &Quads = std::get<CQuadLayer>(*pLayer);
	Cache.m_QuadSource.m_pQuads = Quads.m_Quads.Empty() ? nullptr : Quads.m_Quads.All().data();
	Cache.m_QuadSource.m_NumQuads = (int)Quads.m_Quads.Size();
	Cache.m_QuadSource.m_Textured = Quads.m_Image >= 0;
}

void CDocumentRenderer::Use(std::shared_ptr<const CMapState> pMap)
{
	dbg_assert(m_pGraphics != nullptr, "the document renderer was not initialized");
	m_InvalidatedChunks = 0;
	std::vector<std::unique_ptr<CLayerCache>> vpNext;
	if(pMap != nullptr)
	{
		for(size_t Group = 0; Group < pMap->NumGroups(); ++Group)
		{
			const std::shared_ptr<const CGroup> &pGroup = pMap->m_vpGroups[Group];
			for(size_t Layer = 0; Layer < pGroup->m_vpLayers.size(); ++Layer)
			{
				const std::shared_ptr<const CLayer> &pLayer = pGroup->m_vpLayers[Layer];
				// A sound layer is heard rather than drawn, and there is
				// nothing on the graphics card that belongs to it.
				if(std::holds_alternative<CSoundLayer>(*pLayer))
					continue;
				const bool Tiles = std::holds_alternative<CTileLayer>(*pLayer);

				// The same node is the same geometry, wherever in the map it
				// has ended up: a group that was moved has not changed a tile.
				auto Held = std::find_if(m_vpCaches.begin(), m_vpCaches.end(), [&pLayer](const std::unique_ptr<CLayerCache> &pCache) {
					return pCache != nullptr && pCache->m_pLayer == pLayer;
				});
				// Failing that, whatever was in this place before: a layer
				// that was painted is a new node in the place of the old one,
				// and then only the blocks that changed are built again.
				if(Held == m_vpCaches.end())
				{
					Held = std::find_if(m_vpCaches.begin(), m_vpCaches.end(), [Group, Layer](const std::unique_ptr<CLayerCache> &pCache) {
						return pCache != nullptr && pCache->m_Group == Group && pCache->m_Layer == Layer;
					});
					// Only if it is the same kind of layer: what is in the
					// place of a deleted tile layer is a layer, not that one.
					if(Held != m_vpCaches.end() && ((*Held)->m_pTiles != nullptr) != Tiles)
						Held = m_vpCaches.end();
					if(Held != m_vpCaches.end())
					{
						if(Tiles)
							m_InvalidatedChunks += Invalidate(**Held, std::get<CTileLayer>(*(*Held)->m_pLayer), std::get<CTileLayer>(*pLayer));
						else if(std::get<CQuadLayer>(*(*Held)->m_pLayer).m_Quads.Id() != std::get<CQuadLayer>(*pLayer).m_Quads.Id())
							(*Held)->m_pQuads->Invalidate();
					}
				}

				std::unique_ptr<CLayerCache> pCache;
				if(Held != m_vpCaches.end())
				{
					pCache = std::move(*Held);
					*Held = nullptr;
				}
				else
				{
					pCache = std::make_unique<CLayerCache>();
					if(Tiles)
					{
						pCache->m_pTiles = std::make_unique<CTileChunkCache>();
						pCache->m_pTiles->OnInit(m_pGraphics);
					}
					else
					{
						pCache->m_pQuads = std::make_unique<CQuadBufferCache>();
						pCache->m_pQuads->OnInit(m_pGraphics);
					}
				}
				pCache->m_Group = Group;
				pCache->m_Layer = Layer;
				UseLayer(*pCache, pLayer);
				vpNext.push_back(std::move(pCache));
			}
		}
	}
	// What no layer took over is geometry of a layer that is gone.
	m_vpCaches = std::move(vpNext);
	m_pMap = std::move(pMap);
	m_Envelopes.Use(m_pMap.get());
}

bool CDocumentRenderer::UseGroup(const CGroup &Group, const CParams &Params)
{
	const int ParallaxX = Group.m_ParallaxX;
	const int ParallaxY = Group.m_ParallaxY;
	const int ParallaxZoom = std::clamp(std::max(ParallaxX, ParallaxY), 0, 100);
	const bool OwnView = Params.m_ViewSize.x > 0.0f && Params.m_ViewSize.y > 0.0f;
	const float Aspect = OwnView ? Params.m_ViewSize.x / Params.m_ViewSize.y : m_pGraphics->ScreenAspect();
	const float Scale = CalcGroupViewScale(Aspect, g_Config.m_ClViewMaxAspect / 100.0f, std::max(ParallaxX, ParallaxY));

	m_pGraphics->ClipDisable();
	if(Group.m_UseClipping)
	{
		// The clip is worked out in the view the group is drawn in, which for
		// a program that decides the view itself is not the view the game
		// would have - see `CRenderLayerGroup::DoRender`, where the same sum
		// is done for a map that came out of a file.
		const CScreenRect WorldRect = OwnView ?
						      m_pGraphics->MapViewToWorld(Params.m_ViewSize * Params.m_Zoom, Params.m_Center.x, Params.m_Center.y,
							      100.0f, 100.0f, 100.0f, 0.0f, 0.0f, Params.m_Zoom) :
						      m_pGraphics->MapScreenToWorld(Params.m_Center.x, Params.m_Center.y, 100.0f, 100.0f, 100.0f,
							      0.0f, 0.0f, m_pGraphics->ScreenAspect(), Params.m_Zoom);
		const CScreenRect ScreenRect = CRenderLayerGroup::Windowed(CRenderLayerGroup::Scaled(WorldRect, Scale), Params.m_Window);
		const float ScreenWidth = ScreenRect.Width();
		const float ScreenHeight = ScreenRect.Height();
		const float Left = Group.m_ClipX - ScreenRect.m_TopLeft.x;
		const float Top = Group.m_ClipY - ScreenRect.m_TopLeft.y;
		const float Right = Group.m_ClipX + Group.m_ClipW - ScreenRect.m_TopLeft.x;
		const float Bottom = Group.m_ClipY + Group.m_ClipH - ScreenRect.m_TopLeft.y;
		if(Right < 0.0f || Left > ScreenWidth || Bottom < 0.0f || Top > ScreenHeight)
			return false;

		const int ClipX = (int)std::round(Left * m_pGraphics->ScreenWidth() / ScreenWidth);
		const int ClipY = (int)std::round(Top * m_pGraphics->ScreenHeight() / ScreenHeight);
		m_pGraphics->ClipEnable(ClipX, ClipY,
			(int)std::round(Right * m_pGraphics->ScreenWidth() / ScreenWidth) - ClipX,
			(int)std::round(Bottom * m_pGraphics->ScreenHeight() / ScreenHeight) - ClipY);
	}

	const CScreenRect World = OwnView ?
					  m_pGraphics->MapViewToWorld(Params.m_ViewSize * Params.m_Zoom, Params.m_Center.x, Params.m_Center.y,
						  ParallaxX, ParallaxY, (float)ParallaxZoom, Group.m_OffsetX, Group.m_OffsetY, Params.m_Zoom) :
					  m_pGraphics->MapScreenToWorld(Params.m_Center.x, Params.m_Center.y,
						  ParallaxX, ParallaxY, (float)ParallaxZoom, Group.m_OffsetX, Group.m_OffsetY, m_pGraphics->ScreenAspect(), Params.m_Zoom);
	m_pGraphics->MapScreen(CRenderLayerGroup::Windowed(CRenderLayerGroup::Scaled(World, Scale), Params.m_Window));
	return true;
}

void CDocumentRenderer::CEnvelopes::EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const
{
	if(m_pMap == nullptr || EnvelopeIndex < 0 || (size_t)EnvelopeIndex >= m_pMap->NumEnvelopes())
		return;
	const CEnvelope &Envelope = *m_pMap->Envelope(EnvelopeIndex);
	if(Envelope.m_Channels <= 0 || Envelope.m_Points.Empty())
		return;
	const CDocumentEnvelopePoints Points(Envelope);
	CRenderMap::RenderEvalEnvelope(&Points,
		std::chrono::milliseconds(TimeOffsetMillis) + std::chrono::milliseconds(m_TimeOffsetMillis),
		Result, std::min({Channels, (size_t)Envelope.m_Channels, (size_t)CEnvPoint::MAX_CHANNELS}));
}

void CDocumentRenderer::RenderTileLayer(const CTileLayer &Layer, CLayerCache &Cache, const CParams &Params)
{
	const bool Physics = Layer.m_Kind != ETileLayerKind::TILES;
	IGraphics::CTextureHandle Texture;
	if(Physics)
	{
		Texture = m_pImages->GetEntities(Layer.m_Kind == ETileLayerKind::SWITCH ?
							 MAP_IMAGE_ENTITY_LAYER_TYPE_SWITCH :
							 MAP_IMAGE_ENTITY_LAYER_TYPE_ALL_EXCEPT_SWITCH);
	}
	else if(Layer.m_Image >= 0 && Layer.m_Image < m_pImages->Num())
	{
		Texture = m_pImages->Get(Layer.m_Image);
	}
	if(Texture.IsValid())
		m_pGraphics->TextureSet(Texture);
	else
		m_pGraphics->TextureClear();
	// Same as for quads: the tiles went up with a textured layout if the layer
	// has a picture, so until that picture is there the pipeline would be
	// handed texture coordinates with nothing bound. The graphics refuse that
	// draw, and rightly - what would come out is a rectangle of flat colour
	// where a wall belongs. In an editor the picture may still be on its way,
	// so this is the ordinary case for the first few frames rather than a
	// fault.
	if(Cache.m_TileSource.m_Textured && !Texture.IsValid())
		return;

	ColorRGBA Color = LayerColor(Layer, Params.m_EntityOverlayVal);
	if(!Physics)
	{
		ColorRGBA Envelope(1.0f, 1.0f, 1.0f, 1.0f);
		m_Envelopes.EnvelopeEval(Layer.m_ColorEnvelopeOffset, Layer.m_ColorEnvelope, Envelope, 4);
		Color = Color.Multiply(Envelope);
	}
	// Opaque tiles cover the largest area, so they are worth taking out of the
	// blended pass; tiles within a layer never overlap, so splitting the layer
	// into two passes cannot change what is drawn. A physics layer is drawn
	// over the design and is never opaque.
	const bool ForceTransparent = Physics || Color.a <= 254.0f / 255.0f;
	if(!ForceTransparent)
	{
		m_pGraphics->BlendNone();
		Cache.m_pTiles->Render(Cache.m_TileSource, Color, false, false);
		m_pGraphics->BlendNormal();
	}
	Cache.m_pTiles->Render(Cache.m_TileSource, Color, true, ForceTransparent);
}

void CDocumentRenderer::RenderQuadLayer(const CQuadLayer &Layer, CLayerCache &Cache, const CParams &Params)
{
	IGraphics::CTextureHandle Texture;
	if(Layer.m_Image >= 0 && Layer.m_Image < m_pImages->Num())
		Texture = m_pImages->Get(Layer.m_Image);
	if(Texture.IsValid())
		m_pGraphics->TextureSet(Texture);
	else
		m_pGraphics->TextureClear();
	// The quads went up with a textured layout if the layer has an image, so
	// until that image is there the pipeline would draw with nothing bound -
	// and in an editor the picture may still be on its way.
	if(Cache.m_QuadSource.m_Textured && !Texture.IsValid())
		return;
	Cache.m_pQuads->Render(Cache.m_QuadSource, &m_Envelopes, (100 - Params.m_EntityOverlayVal) / 100.0f);
}

void CDocumentRenderer::Render(const CParams &Params)
{
	if(m_pMap == nullptr)
		return;
	m_Envelopes.SetTimeOffset(Params.m_TimeOffsetMillis);
	size_t Next = 0;
	for(size_t Group = 0; Group < m_pMap->NumGroups(); ++Group)
	{
		const CGroup &TheGroup = *m_pMap->m_vpGroups[Group];
		const bool Visible = UseGroup(TheGroup, Params);
		for(size_t Layer = 0; Layer < TheGroup.m_vpLayers.size(); ++Layer)
		{
			const CLayer &TheLayer = *TheGroup.m_vpLayers[Layer];
			if(std::holds_alternative<CSoundLayer>(TheLayer))
				continue;
			// The caches are in the order the layers are walked in, which is
			// the order they were made in - so this walks with them rather
			// than looking each one up.
			dbg_assert(Next < m_vpCaches.size(), "The renderer was not given the version it is drawing");
			CLayerCache &Cache = *m_vpCaches[Next++];
			dbg_assert(Cache.m_Group == Group && Cache.m_Layer == Layer, "The renderer was not given the version it is drawing");
			if(!Visible)
				continue;
			if(std::holds_alternative<CTileLayer>(TheLayer))
			{
				const CTileLayer &Tiles = std::get<CTileLayer>(TheLayer);
				if(IsDrawn(Tiles, Params))
					RenderTileLayer(Tiles, Cache, Params);
			}
			else
			{
				const CQuadLayer &Quads = std::get<CQuadLayer>(TheLayer);
				if((!Quads.m_Detail || Params.m_HighDetail) && Params.m_EntityOverlayVal < 100 && Quads.m_Quads.Size() > 0)
					RenderQuadLayer(Quads, Cache, Params);
			}
		}
	}
	m_pGraphics->ClipDisable();
}
