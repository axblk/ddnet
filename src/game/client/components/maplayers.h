/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MAPLAYERS_H
#define GAME_CLIENT_COMPONENTS_MAPLAYERS_H

#include <game/client/component.h>
#include <game/client/components/envelope_state.h>
#include <game/map/map_renderer.h>

class CLayers;
class ColorRGBA;
class IMapImages;

class CMapLayers : public CComponent
{
	friend class CBackground;
	friend class CMenuBackground;

	CLayers *m_pLayers;
	IMapImages *m_pImages;
	ERenderType m_Type;
	bool m_OnlineOnly;

public:
	CMapLayers(ERenderType Type, bool OnlineOnly = true);
	int Sizeof() const override { return sizeof(*this); }
	void OnInit() override;
	void OnRender(const CRenderContext &Context) override;
	void OnMapLoad() override;
	void Load(CLayers *pLayers, IMapImages *pImages);
	void Unload();

	CEnvelopeState &EnvEvaluator() { return m_EnvEvaluator; }

protected:
	void Render(vec2 Center, float Zoom);

private:
	CRenderLayerParams m_Params;
	CMapRenderer m_MapRenderer;
	CEnvelopeState m_EnvEvaluator;
};

#endif
