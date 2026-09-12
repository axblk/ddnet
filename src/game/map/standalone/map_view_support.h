#ifndef GAME_MAP_STANDALONE_MAP_VIEW_SUPPORT_H
#define GAME_MAP_STANDALONE_MAP_VIEW_SUPPORT_H

#include <base/log.h>
#include <base/str.h>

#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/map.h>
#include <engine/shared/jobs.h>
#include <engine/storage.h>

#include <game/map/envelope_manager.h>
#include <game/map/render_interfaces.h>
#include <game/map/render_map.h>
#include <game/mapitems.h>

#include <algorithm>
#include <chrono>
#include <memory>

/**
 * The pieces a program that only draws a map needs where the client would bring
 * a whole engine: a job pool, an envelope source that reads the map itself, and
 * the map's own images. None of them knows anything about a game.
 */
namespace MapViewSupport
{

	// A program that draws one map has nothing to gain from more; the number is
	// here rather than at the call site because the asset loader asks for it.
	static constexpr size_t JOB_THREADS = 2;

	class CMinimalEngine final : public IEngine
	{
	public:
		CJobPool m_JobPool;

		void Init() override {}
		size_t JobThreadCount() const override { return JOB_THREADS; }
		void AddJob(std::shared_ptr<IJob> pJob) override
		{
			m_JobPool.Add(std::move(pJob));
		}
		void ShutdownJobs() override
		{
			m_JobPool.Shutdown();
		}
		void SetAdditionalLogger(std::shared_ptr<ILogger> &&pLogger) override {}
	};

	class CMapRenderEnvelopeEval final : public IEnvelopeEval
	{
	public:
		CMapRenderEnvelopeEval(IMap *pMap, int TimeOffsetMillis) :
			m_pMap(pMap), m_TimeOffsetMillis(TimeOffsetMillis)
		{
			m_pEnvelopePoints = std::make_shared<CMapBasedEnvelopePointAccess>(pMap);
		}

		// Where a picture is taken at one moment, a view moves on, so the moment is
		// not fixed at construction.
		void SetTimeOffset(int TimeOffsetMillis) { m_TimeOffsetMillis = TimeOffsetMillis; }

		void EnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const override
		{
			int EnvelopeStart, EnvelopeNum;
			m_pMap->GetType(MAPITEMTYPE_ENVELOPE, &EnvelopeStart, &EnvelopeNum);
			if(EnvelopeIndex < 0 || EnvelopeIndex >= EnvelopeNum)
				return;

			const CMapItemEnvelope *pItem = (CMapItemEnvelope *)m_pMap->GetItem(EnvelopeStart + EnvelopeIndex);
			if(pItem->m_Channels <= 0)
				return;
			Channels = std::min({Channels, (size_t)pItem->m_Channels, (size_t)CEnvPoint::MAX_CHANNELS});

			m_pEnvelopePoints->SetPointsRange(pItem->m_StartPoint, pItem->m_NumPoints);
			if(m_pEnvelopePoints->NumPoints() == 0)
				return;
			CRenderMap::RenderEvalEnvelope(m_pEnvelopePoints.get(), std::chrono::milliseconds(TimeOffsetMillis) + std::chrono::milliseconds(m_TimeOffsetMillis), Result, Channels);
		}

	private:
		IMap *m_pMap;
		std::shared_ptr<CMapBasedEnvelopePointAccess> m_pEnvelopePoints;
		int m_TimeOffsetMillis;
	};

	class CToolMapImages final : public IMapImages
	{
		IGraphics *m_pGraphics;
		IMap *m_pMap;
		const char *m_pLogContext;
		IGraphics::CTextureHandle m_aTextures[MAX_MAPIMAGES];
		int m_Count;
		IGraphics::CTextureHandle m_EntitiesTexture;
		bool m_EntitiesTried = false;

	public:
		CToolMapImages(IGraphics *pGraphics, IMap *pMap, const char *pLogContext) :
			m_pGraphics(pGraphics),
			m_pMap(pMap),
			m_pLogContext(pLogContext),
			m_Count(0)
		{
			std::fill(std::begin(m_aTextures), std::end(m_aTextures), IGraphics::CTextureHandle());
			LoadMapImages();
		}

		// A program that shows one map after another would otherwise leave the
		// images of every map it has left behind on the graphics card.
		~CToolMapImages() override
		{
			for(int i = 0; i < m_Count; i++)
			{
				if(m_aTextures[i].IsValid())
					m_pGraphics->UnloadTexture(&m_aTextures[i]);
			}
			if(m_EntitiesTexture.IsValid())
				m_pGraphics->UnloadTexture(&m_EntitiesTexture);
		}

		// The picture of what a map does rather than of what it looks like.
		// It is the one thing here that comes out of `data/`, so it is only
		// fetched once somebody asks to see the entity overlay, and a program
		// that never does still needs nothing but the map.
		//
		// Asked for before drawing starts, never while it is going on: what
		// comes back is a handle the drawing then uses.
		void EnsureEntities()
		{
			if(m_EntitiesTried)
				return;
			m_EntitiesTried = true;
			// One picture for every kind of map, unmasked. The client picks
			// the one the server it is on belongs to and blanks out the tiles
			// that kind of server does not have; a viewer has no server to ask
			// and nobody to mislead about what a tile does, so it draws what
			// the map put there.
			const char *pPath = "editor/entities_clear/ddnet.png";
			m_EntitiesTexture = m_pGraphics->LoadTexture(pPath, IStorage::TYPE_ALL, IGraphics::TEXLOAD_LAYERED);
			if(!m_EntitiesTexture.IsValid())
				log_warn(m_pLogContext, "Failed to load '%s', the entity overlay stays empty.", pPath);
		}

		// Six tilesets were drawn again for Teeworlds 0.7 with the tiles in
		// other places, and a 0.7 map means the tiles of the 0.7 one. A map
		// that names an image the client would go looking for under
		// `mapres/<name>_0.7.png`, see `CMapRenderImages::Load`; loading the
		// 0.6 file instead is what leaves holes where the map has trees.
		static bool IsTranslatedImageName(const char *pName)
		{
			return !str_comp(pName, "grass_doodads") ||
			       !str_comp(pName, "grass_main") ||
			       !str_comp(pName, "winter_main") ||
			       !str_comp(pName, "generic_shadows") ||
			       !str_comp(pName, "generic_unhookable") ||
			       !str_comp(pName, "easter");
		}

		// Whether this map was written by Teeworlds 0.7, which a viewer has to
		// work out from the map itself: the client is told by the server it is
		// on, and nobody here has one. The second version of an image item is
		// 0.7's, which added the format field this fork calls `m_MustBe1`; 0.6
		// and DDNet write the first, and so does `map_convert_07`, whose 0.7
		// maps carry the pictures of the 0.6 tilesets inside them and need no
		// translating.
		bool WrittenByTeeworlds07(int Start, int Count) const
		{
			for(int i = 0; i < Count; i++)
			{
				const CMapItemImage_v2 *pImg = static_cast<const CMapItemImage_v2 *>(m_pMap->GetItem(Start + i));
				if(pImg->m_Version > 1 && pImg->m_MustBe1 == 1)
					return true;
			}
			return false;
		}

		// The client works out per image whether a tile layer, a quad layer or
		// both use it, and loads only what is sampled. The tool loads every
		// image both ways: it renders one picture and never has to care what
		// the extra copy costs.
		void LoadMapImages()
		{
			int Start;
			m_pMap->GetType(MAPITEMTYPE_IMAGE, &Start, &m_Count);
			m_Count = std::clamp<int>(m_Count, 0, MAX_MAPIMAGES);
			const bool Sixup = WrittenByTeeworlds07(Start, m_Count);

			constexpr LOG_COLOR WarningLogColor = LOG_COLOR{255, 255, 0};

			for(int i = 0; i < m_Count; i++)
			{
				const CMapItemImage_v2 *pImg = static_cast<const CMapItemImage_v2 *>(m_pMap->GetItem(Start + i));

				const char *pName = m_pMap->GetDataString(pImg->m_ImageName);
				if(pName == nullptr || pName[0] == '\0')
				{
					if(pImg->m_External)
					{
						log_warn_color(WarningLogColor, m_pLogContext, "Failed to load map image %d: failed to load name.", i);
						if(pImg->m_ImageName >= 0)
							m_pMap->UnloadData(pImg->m_ImageName);
						continue;
					}
					pName = "(error)";
				}

				if(pImg->m_Version > 1 && pImg->m_MustBe1 != 1)
				{
					log_warn_color(WarningLogColor, m_pLogContext, "Failed to load map image %d '%s': invalid map image type.", i, pName);
					if(pImg->m_ImageName >= 0)
						m_pMap->UnloadData(pImg->m_ImageName);
					continue;
				}

				if(pImg->m_External)
				{
					char aPath[IO_MAX_PATH_LENGTH];
					str_format(aPath, sizeof(aPath), "mapres/%s%s.png", pName, Sixup && IsTranslatedImageName(pName) ? "_0.7" : "");
					m_aTextures[i] = m_pGraphics->LoadTexture(aPath, IStorage::TYPE_ALL, IGraphics::TEXLOAD_LAYERED);
				}
				else
				{
					CImageInfo ImageInfo;
					ImageInfo.m_Width = pImg->m_Width;
					ImageInfo.m_Height = pImg->m_Height;
					ImageInfo.m_Format = CImageInfo::FORMAT_RGBA;
					ImageInfo.m_pData = static_cast<uint8_t *>(m_pMap->GetData(pImg->m_ImageData));
					if(ImageInfo.m_pData && (size_t)m_pMap->GetDataSize(pImg->m_ImageData) >= ImageInfo.DataSize() && pImg->m_Height > 0 && pImg->m_Width > 0)
					{
						char aTexName[IO_MAX_PATH_LENGTH];
						str_format(aTexName, sizeof(aTexName), "embedded: %s", pName);
						m_aTextures[i] = m_pGraphics->LoadTextureRaw(ImageInfo, IGraphics::TEXLOAD_LAYERED, aTexName);
						m_pMap->UnloadData(pImg->m_ImageData);
					}
					else
					{
						log_warn_color(WarningLogColor, m_pLogContext, "Failed to load map image %d '%s': failed to load data.", i, pName);
						m_pMap->UnloadData(pImg->m_ImageData);
						if(pImg->m_ImageName >= 0)
							m_pMap->UnloadData(pImg->m_ImageName);
						continue;
					}
				}
				if(pImg->m_ImageName >= 0)
					m_pMap->UnloadData(pImg->m_ImageName);
			}
		}

		IGraphics::CTextureHandle Get(int Index) const override
		{
			if(Index >= 0 && Index < m_Count)
				return m_aTextures[Index];
			return IGraphics::CTextureHandle();
		}

		int Num() const override { return m_Count; }

		// Every entity layer is drawn out of the same picture. The client
		// keeps one per layer kind so that a game layer cannot show a switch
		// tile, but a layer only ever holds the tiles that belong in it, so
		// there is nothing here for that to prevent.
		IGraphics::CTextureHandle GetEntities(EMapImageEntityLayerType EntityLayerType) override
		{
			(void)EntityLayerType;
			return m_EntitiesTexture;
		}

		IGraphics::CTextureHandle GetSpeedupArrow() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetTuneColors() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetOverlayBottom() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetOverlayTop() override { return IGraphics::CTextureHandle(); }
		IGraphics::CTextureHandle GetOverlayCenter() override { return IGraphics::CTextureHandle(); }
	};

} // namespace MapViewSupport

#endif // GAME_MAP_STANDALONE_MAP_VIEW_SUPPORT_H
