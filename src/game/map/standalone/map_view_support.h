#ifndef GAME_MAP_STANDALONE_MAP_VIEW_SUPPORT_H
#define GAME_MAP_STANDALONE_MAP_VIEW_SUPPORT_H

#include <base/log.h>
#include <base/str.h>

#include <engine/client/asset_loader.h>
#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/image.h>
#include <engine/map.h>
#include <engine/shared/datafile.h>
#include <engine/shared/jobs.h>
#include <engine/storage.h>

#include <game/layers.h>
#include <game/map/envelope_manager.h>
#include <game/map/render_interfaces.h>
#include <game/map/render_map.h>
#include <game/mapitems.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

/**
 * The pieces a program that only draws a map needs where the client would bring
 * a whole engine: a job pool, an envelope source that reads the map itself, and
 * the map's own images. None of them knows anything about a game.
 */
namespace MapViewSupport
{

	// Two was enough while the pool only fetched the map's images. Unpacking
	// the layers is what it mostly does now, and that is as wide as the map
	// has layers: four is what the client asks for on the same platforms, and
	// what the browser's pool of ten threads has room for beside everything
	// else a viewer holds. The number is here rather than at the call site
	// because the asset loader asks for it.
	static constexpr size_t JOB_THREADS = 4;

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
		IStorage *m_pStorage;
		CAssetLoader *m_pAssetLoader;
		IMap *m_pMap;
		CLayers *m_pLayers;
		const char *m_pLogContext;
		IGraphics::CTextureHandle m_aTextures[MAX_MAPIMAGES];
		int m_Count;
		IGraphics::CTextureHandle m_EntitiesTexture;
		bool m_EntitiesTried = false;

		// One map's worth of images is one owner and one generation: nothing
		// here loads a second map over the first, it makes a new one of these.
		static constexpr int ASSET_OWNER = 0;
		static constexpr uint64_t ASSET_GENERATION = 1;

	public:
		CToolMapImages(IGraphics *pGraphics, IStorage *pStorage, CAssetLoader *pAssetLoader, IMap *pMap, CLayers *pLayers, const char *pLogContext) :
			m_pGraphics(pGraphics),
			m_pStorage(pStorage),
			m_pAssetLoader(pAssetLoader),
			m_pMap(pMap),
			m_pLayers(pLayers),
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

		// Which of the map's images a tile layer samples, which a quad layer
		// samples, and which nobody does: 1 for tiles, 2 for quads. A tileset
		// is cut into an array of sixteen for the tile shader, a quad's
		// picture is one plain texture, and an image that is used both ways
		// needs both - so asking first is what keeps every image from being
		// uploaded twice, and an image nobody uses from being uploaded at all.
		void UsageOfImages(unsigned char *pUsage) const
		{
			for(int GroupId = 0; GroupId < m_pLayers->NumGroups(); ++GroupId)
			{
				const CMapItemGroup *pGroup = m_pLayers->GetGroup(GroupId);
				if(pGroup == nullptr)
					continue;
				for(int LayerId = 0; LayerId < pGroup->m_NumLayers; ++LayerId)
				{
					const CMapItemLayer *pLayer = m_pLayers->GetLayer(pGroup->m_StartLayer + LayerId);
					if(pLayer == nullptr)
						continue;
					if(pLayer->m_Type == LAYERTYPE_TILES)
					{
						const int Image = reinterpret_cast<const CMapItemLayerTilemap *>(pLayer)->m_Image;
						if(Image >= 0 && Image < m_Count)
							pUsage[Image] |= 1;
					}
					else if(pLayer->m_Type == LAYERTYPE_QUADS)
					{
						const int Image = reinterpret_cast<const CMapItemLayerQuads *>(pLayer)->m_Image;
						if(Image >= 0 && Image < m_Count)
							pUsage[Image] |= 2;
					}
				}
			}
		}

		// Every image is loaded the way the layers that sample it need it, as
		// the client does, and one that nothing samples is not loaded at all.
		//
		// Every image is asked for before any of them is waited for, and the
		// asset loader decodes them on its job threads - a PNG off the disk
		// and an embedded image out of the map are both work that has nothing
		// to do with the graphics card until the pixels are there. Only the
		// upload happens here, as each one arrives. Together with loading only
		// what is sampled, this took a large map from 6,4 to 4,7 seconds to
		// open in a browser.
		void LoadMapImages()
		{
			int Start;
			m_pMap->GetType(MAPITEMTYPE_IMAGE, &Start, &m_Count);
			m_Count = std::clamp<int>(m_Count, 0, MAX_MAPIMAGES);
			const bool Sixup = WrittenByTeeworlds07(Start, m_Count);
			unsigned char aUsage[MAX_MAPIMAGES] = {0};
			UsageOfImages(aUsage);

			constexpr LOG_COLOR WarningLogColor = LOG_COLOR{255, 255, 0};

			struct SImageLoad
			{
				int m_Index;
				int m_LoadFlags;
				CImageResource m_Resource;
			};
			std::vector<SImageLoad> vLoads;
			vLoads.reserve(m_Count);

			for(int i = 0; i < m_Count; i++)
			{
				if(aUsage[i] == 0)
					continue;
				const int LoadFlags = ((aUsage[i] & 1) != 0 ? IGraphics::TEXLOAD_LAYERED : 0) | ((aUsage[i] & 2) != 0 ? 0 : IGraphics::TEXLOAD_NO_2D_TEXTURE);
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
					vLoads.push_back({i, LoadFlags, m_pAssetLoader->LoadImageFile(m_pStorage, aPath, IStorage::TYPE_ALL, ASSET_OWNER, ASSET_GENERATION)});
				}
				else if(pImg->m_Width <= 0 || pImg->m_Height <= 0)
				{
					log_warn_color(WarningLogColor, m_pLogContext, "Failed to load map image %d '%s': invalid image dimensions.", i, pName);
				}
				else
				{
					// The compressed bytes go over as they are and are
					// uncompressed on a job thread, so the map never holds an
					// uncompressed copy of the picture at all.
					const size_t DataSize = (size_t)pImg->m_Width * pImg->m_Height * CImageInfo::PixelSize(CImageInfo::FORMAT_RGBA);
					CDataFileRawData RawData;
					if(!m_pMap->GetRawData(pImg->m_ImageData, RawData) || RawData.UncompressedSize() < DataSize)
					{
						log_warn_color(WarningLogColor, m_pLogContext, "Failed to load map image %d '%s': failed to load data.", i, pName);
					}
					else
					{
						char aTexName[IO_MAX_PATH_LENGTH];
						str_format(aTexName, sizeof(aTexName), "embedded: %s", pName);
						vLoads.push_back({i, LoadFlags, m_pAssetLoader->LoadImageRawData(std::move(RawData), pImg->m_Width, pImg->m_Height, CImageInfo::FORMAT_RGBA, aTexName, ASSET_OWNER, ASSET_GENERATION)});
					}
				}
				if(pImg->m_ImageName >= 0)
					m_pMap->UnloadData(pImg->m_ImageName);
			}

			// Nothing here can draw before its pictures are on the graphics
			// card, so this waits - but it waits for all of them at once
			// rather than for one at a time, and uploads each as it lands.
			while(!vLoads.empty())
			{
				m_pAssetLoader->Update();
				bool Landed = false;
				for(auto It = vLoads.begin(); It != vLoads.end();)
				{
					if(!It->m_Resource.IsFinished())
					{
						++It;
						continue;
					}
					Landed = true;
					if(It->m_Resource.IsReady(ASSET_GENERATION))
					{
						CImageInfo Image = It->m_Resource.TakeImage();
						m_aTextures[It->m_Index] = m_pGraphics->LoadTextureRawMove(Image, It->m_LoadFlags, It->m_Resource.Path());
					}
					else
					{
						log_warn_color(WarningLogColor, m_pLogContext, "Failed to load map image '%s'.", It->m_Resource.Path());
					}
					It = vLoads.erase(It);
				}
				if(!Landed)
				{
					std::this_thread::yield();
				}
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
