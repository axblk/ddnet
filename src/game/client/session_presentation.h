#ifndef GAME_CLIENT_SESSION_PRESENTATION_H
#define GAME_CLIENT_SESSION_PRESENTATION_H

#include <engine/client/session.h>

#include <game/client/component.h>
#include <game/client/components/mapimages.h>
#include <game/client/components/maplayers.h>
#include <game/client/components/mapsounds.h>

#include <memory>
#include <vector>

class CGameSessionContext;

class CSessionPresentation : public CComponentInterfaces
{
	CSessionId m_SessionId;
	CMapRenderImages m_MapImages;
	CMapLayers m_MapLayersBackground{ERenderType::RENDERTYPE_BACKGROUND};
	CMapLayers m_MapLayersForeground{ERenderType::RENDERTYPE_FOREGROUND};
	CMapLayers m_MapLayersBackgroundForce{ERenderType::RENDERTYPE_BACKGROUND_FORCE};
	CMapSounds m_MapSounds;
	bool m_Loaded = false;

public:
	CSessionPresentation(CSessionId SessionId, CMapImages &SharedMapImages);

	void OnInterfacesInit(CGameClient *pClient) override;
	void Load(CGameSessionContext &Session);
	void Unload();
	void PrepareRender(const CRenderContext &Context, bool UsePredictedTime);
	void UpdateMapSounds(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool UsePredictedTime);

	CSessionId SessionId() const { return m_SessionId; }
	bool IsLoaded() const { return m_Loaded; }
	CMapLayers &MapLayersBackground() { return m_MapLayersBackground; }
	CMapLayers &MapLayersForeground() { return m_MapLayersForeground; }
	CMapLayers &MapLayersBackgroundForce() { return m_MapLayersBackgroundForce; }
	CMapSounds &MapSounds() { return m_MapSounds; }
};

class CSessionPresentationManager
{
	CMapImages &m_SharedMapImages;
	CGameClient *m_pGameClient = nullptr;
	std::vector<std::unique_ptr<CSessionPresentation>> m_vpPresentations;
	CSessionId m_AudibleSessionId;

public:
	explicit CSessionPresentationManager(CMapImages &SharedMapImages);

	void OnInterfacesInit(CGameClient *pClient);
	CSessionPresentation *Create(CSessionId SessionId);
	CSessionPresentation *Find(CSessionId SessionId);
	const CSessionPresentation *Find(CSessionId SessionId) const;
	void SetAudible(CSessionId SessionId);
	void Unload(CSessionId SessionId);
	void UnloadAll();
};

#endif // GAME_CLIENT_SESSION_PRESENTATION_H
