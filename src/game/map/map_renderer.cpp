#include "map_renderer.h"

#include <base/dbg.h>
#include <base/log.h>

#include <engine/graphics.h>
#include <engine/shared/jobs.h>

#include <game/map/envelope_manager.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

const int LAYER_DEFAULT_TILESET = -1;

namespace
{
	// A layer is unpacked the first time it is read, and on a large map that is
	// most of what loading it costs: abyss is 42 layers and 710 MB of tiles out of
	// 9,9 MB of zlib, and unpacking them one after another takes 1,8 of the 2,2
	// seconds the map takes to open. They do not depend on each other, and the
	// reader keeps a slot of its own per data item over a file it only reads, so
	// they can be unpacked side by side.
	// One layer to unpack, and whether it is one that can be given back again
	// right away.
	class CLayerToUnpack
	{
	public:
		int m_DataIndex = -1;
		// The size of a plain tile layer, whose tiles are read into stretches
		// on the spot. Zero for everything else: a physics layer keeps the
		// map's tiles because the collision holds the same pointer, and a quad
		// layer has no tiles to read.
		int m_Width = 0;
		int m_Height = 0;
	};

	class CUnpackLayersJob final : public IJob
	{
		IMap *m_pMap;
		const std::vector<CLayerToUnpack> *m_pvLayers;
		std::vector<CTileRunStore> *m_pvRunStores;
		std::atomic<size_t> *m_pNext;

		void Run() override
		{
			UnpackLayers(m_pMap, *m_pvLayers, *m_pvRunStores, *m_pNext);
		}

	public:
		CUnpackLayersJob(IMap *pMap, const std::vector<CLayerToUnpack> *pvLayers, std::vector<CTileRunStore> *pvRunStores, std::atomic<size_t> *pNext) :
			m_pMap(pMap), m_pvLayers(pvLayers), m_pvRunStores(pvRunStores), m_pNext(pNext)
		{
		}

		// Whoever runs this takes the next layer that nobody has taken, so one
		// layer that is ten times the size of the others does not decide when
		// everybody is finished.
		//
		// A plain tile layer is read into stretches and given back before the
		// next one is taken. Unpacking all of them first and reading them
		// afterwards would hold every layer at once, which for abyss is
		// 677 MiB; this holds one layer per thread and keeps the stretches,
		// which are under 10 MiB for the same map. It matters most in the
		// browser, where the heap never shrinks again and the peak is what
		// stays.
		static void UnpackLayers(IMap *pMap, const std::vector<CLayerToUnpack> &vLayers, std::vector<CTileRunStore> &vRunStores, std::atomic<size_t> &Next)
		{
			for(size_t Index = Next++; Index < vLayers.size(); Index = Next++)
			{
				const CLayerToUnpack &Layer = vLayers[Index];
				const void *pData = pMap->GetData(Layer.m_DataIndex);
				if(pData == nullptr || Layer.m_Width <= 0 || Layer.m_Height <= 0)
				{
					continue;
				}
				const CTile *pTiles = static_cast<const CTile *>(pData);
				const int Width = Layer.m_Width;
				vRunStores[Index].Build(Width, Layer.m_Height, [pTiles, Width](int x, int y) -> uint16_t {
					const CTile &Tile = pTiles[(size_t)y * Width + x];
					return (uint16_t)Tile.m_Index | (uint16_t)((uint16_t)Tile.m_Flags << 8);
				});
				pMap->UnloadData(Layer.m_DataIndex);
			}
		}
	};
} // namespace

void CMapRenderer::Clear()
{
	for(auto &pLayer : m_vpRenderLayers)
		pLayer->Unload();
	m_vpRenderLayers.clear();
}

void CMapRenderer::Load(ERenderType Type, CLayers *pLayers, IMapImages *pMapImages, const IEnvelopeEval *pEnvelopeEval, std::optional<FCallbackMapRendererInit> CallbackMapRendererInitOptional, IEngine *pEngine)
{
	Clear();
	CUnpackedLayers Unpacked;
	UnpackLayersAhead(Type, pLayers, pEngine, Unpacked);

	std::shared_ptr<CEnvelopeManager> pEnvelopeManager = std::make_shared<CEnvelopeManager>(pEnvelopeEval, pLayers->Map());
	bool PassedGameLayer = false;

	for(int GroupId = 0; GroupId < pLayers->NumGroups(); GroupId++)
	{
		CMapItemGroup *pGroup = pLayers->GetGroup(GroupId);
		std::unique_ptr<CRenderLayer> pRenderLayerGroup = std::make_unique<CRenderLayerGroup>(GroupId, pGroup);

		std::optional<FCallbackLayerInit> CallbackLayerInitOptional;
		if(CallbackMapRendererInitOptional.has_value())
		{
			CallbackLayerInitOptional = [&](int LayerGroupId, int LayerId) {
				(*CallbackMapRendererInitOptional)(LayerGroupId, pLayers->NumGroups(), LayerId, pGroup->m_NumLayers);
			};
		}

		pRenderLayerGroup->OnInit(Graphics(), TextRender(), RenderMap(), pEnvelopeManager, pLayers->Map(), pMapImages, CallbackLayerInitOptional);
		if(!pRenderLayerGroup->IsValid())
		{
			log_error("map_renderer", "error group was null, group number = %d, total groups = %d", GroupId, pLayers->NumGroups());
			log_error("map_renderer", "this is here to prevent a crash but the source of this is unknown, please report this for it to get fixed");
			log_error("map_renderer", "we need mapname and crc and the map that caused this if possible, and anymore info you think is relevant");
			continue;
		}

		for(int LayerId = 0; LayerId < pGroup->m_NumLayers; LayerId++)
		{
			CMapItemLayer *pLayer = pLayers->GetLayer(pGroup->m_StartLayer + LayerId);
			int LayerType = GetLayerType(pLayer);
			PassedGameLayer |= LayerType == LAYER_GAME;

			if(Type == ERenderType::RENDERTYPE_BACKGROUND_FORCE || Type == ERenderType::RENDERTYPE_BACKGROUND)
			{
				if(PassedGameLayer)
					return;
			}
			else if(Type == ERenderType::RENDERTYPE_FOREGROUND)
			{
				if(!PassedGameLayer)
					continue;
			}

			if(pRenderLayerGroup)
				m_vpRenderLayers.push_back(std::move(pRenderLayerGroup));

			std::unique_ptr<CRenderLayer> pRenderLayer;

			if(pLayer->m_Type == LAYERTYPE_TILES)
			{
				CMapItemLayerTilemap *pTileLayer = (CMapItemLayerTilemap *)pLayer;

				switch(LayerType)
				{
				case LAYER_DEFAULT_TILESET:
				{
					std::unique_ptr<CRenderLayerTile> pRenderLayerTile = std::make_unique<CRenderLayerTile>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					Unpacked.HandTo(pTileLayer->m_Data, *pRenderLayerTile);
					pRenderLayer = std::move(pRenderLayerTile);
					break;
				}
				case LAYER_GAME:
					pRenderLayer = std::make_unique<CRenderLayerEntityGame>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					break;
				case LAYER_FRONT:
					pRenderLayer = std::make_unique<CRenderLayerEntityFront>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					break;
				case LAYER_TELE:
					pRenderLayer = std::make_unique<CRenderLayerEntityTele>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					break;
				case LAYER_SPEEDUP:
					pRenderLayer = std::make_unique<CRenderLayerEntitySpeedup>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					break;
				case LAYER_SWITCH:
					pRenderLayer = std::make_unique<CRenderLayerEntitySwitch>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					break;
				case LAYER_TUNE:
					pRenderLayer = std::make_unique<CRenderLayerEntityTune>(
						GroupId,
						LayerId,
						pLayer->m_Flags,
						pTileLayer);
					break;
				default:
					dbg_assert_failed("Unknown LayerType %d", LayerType);
				}
			}
			else if(pLayer->m_Type == LAYERTYPE_QUADS)
			{
				CMapItemLayerQuads *pQLayer = (CMapItemLayerQuads *)pLayer;

				pRenderLayer = std::make_unique<CRenderLayerQuads>(
					GroupId,
					LayerId,
					pLayer->m_Flags,
					pQLayer);
			}

			// just ignore invalid layers from rendering
			if(pRenderLayer)
			{
				pRenderLayer->OnInit(Graphics(), TextRender(), RenderMap(), pEnvelopeManager, pLayers->Map(), pMapImages, CallbackLayerInitOptional);
				if(pRenderLayer->IsValid())
				{
					pRenderLayer->Init();
					m_vpRenderLayers.push_back(std::move(pRenderLayer));
				}
			}
		}
	}
}

void CMapRenderer::Render(const CRenderLayerParams &Params)
{
	CScreenRect ScreenRect = Graphics()->GetScreen();

	bool DoRenderGroup = true;
	for(auto &pRenderLayer : m_vpRenderLayers)
	{
		if(pRenderLayer->IsGroup())
			DoRenderGroup = pRenderLayer->DoRender(Params);

		if(!DoRenderGroup)
			continue;

		if(pRenderLayer->DoRender(Params))
			pRenderLayer->Render(Params);
	}

	// Reset clip from last group
	Graphics()->ClipDisable();

	// don't reset screen on background
	if(Params.m_RenderType != ERenderType::RENDERTYPE_BACKGROUND && Params.m_RenderType != ERenderType::RENDERTYPE_BACKGROUND_FORCE)
	{
		// reset the screen like it was before
		Graphics()->MapScreen(ScreenRect);
	}
	else
	{
		// reset the screen to the default interface
		Graphics()->MapScreenToInterface(Params.m_Center.x, Params.m_Center.y, Params.m_Zoom);
	}
}

// The data item every render layer reads in its InitTileData, before it has
// been made - see the GetDataIndex of each of them, which this mirrors.
int CMapRenderer::GetLayerDataIndex(const CMapItemLayer *pLayer) const
{
	if(pLayer->m_Type == LAYERTYPE_QUADS)
	{
		return reinterpret_cast<const CMapItemLayerQuads *>(pLayer)->m_Data;
	}
	if(pLayer->m_Type != LAYERTYPE_TILES)
	{
		return -1;
	}
	const CMapItemLayerTilemap *pTilemap = reinterpret_cast<const CMapItemLayerTilemap *>(pLayer);
	switch(GetLayerType(pLayer))
	{
	case LAYER_FRONT: return pTilemap->m_Front;
	case LAYER_TELE: return pTilemap->m_Tele;
	case LAYER_SPEEDUP: return pTilemap->m_Speedup;
	case LAYER_SWITCH: return pTilemap->m_Switch;
	case LAYER_TUNE: return pTilemap->m_Tune;
	default: return pTilemap->m_Data;
	}
}

void CMapRenderer::CUnpackedLayers::HandTo(int DataIndex, CRenderLayerTile &RenderLayer)
{
	for(size_t Index = 0; Index < m_vDataIndices.size(); ++Index)
	{
		if(m_vDataIndices[Index] != DataIndex)
		{
			continue;
		}
		// Taken off the list, so that two layers over the same data item do not
		// both get it: the second reads the map itself, as it always did.
		m_vDataIndices[Index] = -1;
		RenderLayer.UseRuns(std::move(m_vRunStores[Index]));
		return;
	}
}

void CMapRenderer::UnpackLayersAhead(ERenderType Type, CLayers *pLayers, IEngine *pEngine, CUnpackedLayers &Unpacked) const
{
	IMap *pMap = pLayers->Map();
	if(pMap == nullptr)
	{
		return;
	}
	// Exactly the layers the loop below goes on to make, by the same rules:
	// a background stops at the game layer and a foreground starts there, and
	// unpacking what neither of them reads would cost the time this is meant
	// to save and hold on to the memory besides.
	std::vector<CLayerToUnpack> vLayers;
	bool PassedGameLayer = false;
	for(int GroupId = 0; GroupId < pLayers->NumGroups() && !(PassedGameLayer && (Type == ERenderType::RENDERTYPE_BACKGROUND_FORCE || Type == ERenderType::RENDERTYPE_BACKGROUND)); ++GroupId)
	{
		const CMapItemGroup *pGroup = pLayers->GetGroup(GroupId);
		if(pGroup == nullptr)
		{
			continue;
		}
		for(int LayerId = 0; LayerId < pGroup->m_NumLayers; ++LayerId)
		{
			const CMapItemLayer *pLayer = pLayers->GetLayer(pGroup->m_StartLayer + LayerId);
			if(pLayer == nullptr)
			{
				continue;
			}
			PassedGameLayer |= GetLayerType(pLayer) == LAYER_GAME;
			if(Type == ERenderType::RENDERTYPE_BACKGROUND_FORCE || Type == ERenderType::RENDERTYPE_BACKGROUND)
			{
				if(PassedGameLayer)
					break;
			}
			else if(Type == ERenderType::RENDERTYPE_FOREGROUND)
			{
				if(!PassedGameLayer)
					continue;
			}
			const int DataIndex = GetLayerDataIndex(pLayer);
			if(DataIndex < 0)
			{
				continue;
			}
			// The same data item twice would have two threads unpacking it and
			// one of them giving it back under the other.
			if(std::any_of(vLayers.begin(), vLayers.end(), [DataIndex](const CLayerToUnpack &Layer) { return Layer.m_DataIndex == DataIndex; }))
			{
				continue;
			}
			CLayerToUnpack Layer;
			Layer.m_DataIndex = DataIndex;
			if(pLayer->m_Type == LAYERTYPE_TILES && GetLayerType(pLayer) == LAYER_DEFAULT_TILESET)
			{
				const CMapItemLayerTilemap *pTilemap = reinterpret_cast<const CMapItemLayerTilemap *>(pLayer);
				Layer.m_Width = pTilemap->m_Width;
				Layer.m_Height = pTilemap->m_Height;
			}
			vLayers.push_back(Layer);
		}
	}
	if(vLayers.empty())
	{
		return;
	}
	Unpacked.m_vDataIndices.reserve(vLayers.size());
	for(const CLayerToUnpack &Layer : vLayers)
	{
		Unpacked.m_vDataIndices.push_back(Layer.m_Width > 0 ? Layer.m_DataIndex : -1);
	}
	Unpacked.m_vRunStores.resize(vLayers.size());

	// One helper less than the pool has threads would leave one idle; one more
	// than there are layers would have nothing to take. This thread is one of
	// the workers, so it takes layers as well as waiting for the others.
	std::atomic<size_t> Next(0);
	std::vector<std::shared_ptr<CUnpackLayersJob>> vpJobs;
	const size_t Helpers = pEngine == nullptr ? 0 : std::min(pEngine->JobThreadCount(), vLayers.size() - 1);
	vpJobs.reserve(Helpers);
	for(size_t Helper = 0; Helper < Helpers; ++Helper)
	{
		vpJobs.push_back(std::make_shared<CUnpackLayersJob>(pMap, &vLayers, &Unpacked.m_vRunStores, &Next));
		pEngine->AddJob(vpJobs.back());
	}
	CUnpackLayersJob::UnpackLayers(pMap, vLayers, Unpacked.m_vRunStores, Next);
	// The jobs are not abortable, so the pool runs every one of them to the
	// end even while it is shutting down, and waiting for them cannot hang.
	// It has to be waited for: what comes next reads the same slots.
	for(const std::shared_ptr<CUnpackLayersJob> &pJob : vpJobs)
	{
		while(!pJob->Done())
		{
			std::this_thread::yield();
		}
	}
}

int CMapRenderer::GetLayerType(const CMapItemLayer *pLayer) const
{
	if(pLayer->m_Type != LAYERTYPE_TILES)
		return LAYER_DEFAULT_TILESET;

	// Physics layers must be determined by their flags instead of by comparing them with the
	// layers of CLayers, which only knows the last physics layer of each type, as design tiles
	// layers use the data index which is neither used nor validated for physics layers.
	const int Flags = reinterpret_cast<const CMapItemLayerTilemap *>(pLayer)->m_Flags;
	if(Flags & TILESLAYERFLAG_GAME)
		return LAYER_GAME;
	else if(Flags & TILESLAYERFLAG_FRONT)
		return LAYER_FRONT;
	else if(Flags & TILESLAYERFLAG_SWITCH)
		return LAYER_SWITCH;
	else if(Flags & TILESLAYERFLAG_TELE)
		return LAYER_TELE;
	else if(Flags & TILESLAYERFLAG_SPEEDUP)
		return LAYER_SPEEDUP;
	else if(Flags & TILESLAYERFLAG_TUNE)
		return LAYER_TUNE;
	return LAYER_DEFAULT_TILESET;
}
