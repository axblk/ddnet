#ifndef GAME_MAP_MAP_RENDERER_H
#define GAME_MAP_MAP_RENDERER_H

#include <engine/engine.h>
#include <engine/map.h>

#include <game/layers.h>
#include <game/map/render_component.h>
#include <game/map/render_layer.h>

typedef std::function<void(int GroupId, int NumGroups, int LayerId, int NumLayers)> FCallbackMapRendererInit;

class CMapRenderer : public CRenderComponent
{
public:
	CMapRenderer() = default;

	void Clear();
	/**
	 * Makes a render layer for every layer of the map that this kind of
	 * rendering draws, and lets them read what they need out of the map.
	 *
	 * @param Type Which half of the map to draw: everything, what is behind
	 * the game layer, or what is in front of it.
	 * @param pLayers The groups and layers of the map, already read.
	 * @param pMapImages The pictures the layers are drawn with.
	 * @param pEnvelopeEval Where a layer asks what its envelopes say at the
	 * moment it is drawn.
	 * @param CallbackMapRendererInitOptional Told how far this has got, for a
	 * program that draws a loading bar, or `std::nullopt`.
	 * @param pEngine The job pool the layers are unpacked on, or `nullptr` to
	 * unpack them one after another on this thread. What is unpacked is only
	 * what the layers would read on their own a moment later, so a program
	 * without a job pool gets the same picture, more slowly.
	 */
	void Load(ERenderType Type, CLayers *pLayers, IMapImages *pMapImages, const IEnvelopeEval *pEnvelopeEval, std::optional<FCallbackMapRendererInit> CallbackMapRendererInitOptional, IEngine *pEngine = nullptr);
	void Render(const CRenderLayerParams &Params);

private:
	int GetLayerType(const CMapItemLayer *pLayer) const;
	int GetLayerDataIndex(const CMapItemLayer *pLayer) const;

	/**
	 * What the unpacking ahead leaves for the layers that are made after it.
	 *
	 * A plain tile layer is read into stretches on the thread that unpacked
	 * it, so that the map's copy is given back there instead of being held
	 * until the layer itself gets around to it.
	 */
	class CUnpackedLayers
	{
	public:
		/**
		 * Gives the layer the stretches that were read for `DataIndex`, if
		 * there are any. Each set is given out once.
		 */
		void HandTo(int DataIndex, CRenderLayerTile &RenderLayer);

		/**
		 * The data item each set of stretches was read from, `-1` where there
		 * are none or they have been given out.
		 */
		std::vector<int> m_vDataIndices;
		std::vector<CTileRunStore> m_vRunStores;
	};

	void UnpackLayersAhead(ERenderType Type, CLayers *pLayers, IEngine *pEngine, CUnpackedLayers &Unpacked) const;

	std::vector<std::unique_ptr<CRenderLayer>> m_vpRenderLayers;
};

#endif
