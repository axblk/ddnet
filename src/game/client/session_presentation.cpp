#include "session_presentation.h"

#include <base/dbg.h>

#include <game/client/game_view.h>
#include <game/client/session_context.h>

#include <algorithm>

CSessionPresentation::CSessionPresentation(CSessionId SessionId, CMapImages &SharedMapImages) :
	m_SessionId(SessionId),
	m_MapImages(SharedMapImages)
{
}

void CSessionPresentation::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
	m_MapImages.OnInterfacesInit(pClient);
	m_MapLayersBackground.OnInterfacesInit(pClient);
	m_MapLayersForeground.OnInterfacesInit(pClient);
	m_MapLayersBackgroundForce.OnInterfacesInit(pClient);
	m_MapSounds.OnInterfacesInit(pClient);
}

void CSessionPresentation::Load(CGameSessionContext &Session)
{
	dbg_assert(Session.Id() == m_SessionId, "session presentation loaded for wrong session");
	Unload();

	CMapContext &MapContext = Session.MapContext();
	m_MapImages.Load(MapContext.Layers(), MapContext.Map(), Session.Protocol() == EGameProtocol::SIXUP);
	m_MapLayersBackground.Load(MapContext.Layers(), &m_MapImages);
	m_MapLayersForeground.Load(MapContext.Layers(), &m_MapImages);
	m_MapLayersBackgroundForce.Load(MapContext.Layers(), &m_MapImages);
	m_MapSounds.Load(MapContext.Map(), MapContext.Layers());
	m_Loaded = true;
}

void CSessionPresentation::Unload()
{
	m_MapSounds.Unload();
	m_MapLayersBackground.Unload();
	m_MapLayersForeground.Unload();
	m_MapLayersBackgroundForce.Unload();
	m_MapImages.Unload();
	m_Loaded = false;
}

void CSessionPresentation::PrepareRender(const CRenderContext &Context, bool UsePredictedTime)
{
	dbg_assert(Context.m_Session.Id() == m_SessionId, "render context does not match session presentation");
	dbg_assert(m_Loaded, "session presentation must be loaded before rendering");
	m_MapImages.SetGameInfo(Context.m_State.CoreGameInfo());
	m_MapLayersBackground.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
	m_MapLayersForeground.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
	m_MapLayersBackgroundForce.EnvEvaluator().SetOnlineTime(Context.m_State, Context.m_Time, UsePredictedTime);
}

void CSessionPresentation::UpdateMapSounds(const CGameState &State, const CGameTickInfo &Time, vec2 ListenerPosition, bool UsePredictedTime)
{
	dbg_assert(m_Loaded, "session presentation must be loaded before updating map sounds");
	m_MapLayersBackground.EnvEvaluator().SetOnlineTime(State, Time, UsePredictedTime);
	m_MapSounds.Update(State, Time, ListenerPosition, Time.m_IsDemoPlaybackPaused, m_MapLayersBackground.EnvEvaluator());
}

CSessionPresentationManager::CSessionPresentationManager(CMapImages &SharedMapImages) :
	m_SharedMapImages(SharedMapImages)
{
}

void CSessionPresentationManager::OnInterfacesInit(CGameClient *pClient)
{
	m_pGameClient = pClient;
	for(auto &pPresentation : m_vpPresentations)
		pPresentation->OnInterfacesInit(pClient);
}

CSessionPresentation *CSessionPresentationManager::Create(CSessionId SessionId)
{
	if(!SessionId.IsValid() || Find(SessionId) != nullptr)
		return nullptr;

	auto pPresentation = std::make_unique<CSessionPresentation>(SessionId, m_SharedMapImages);
	if(m_pGameClient != nullptr)
		pPresentation->OnInterfacesInit(m_pGameClient);
	m_vpPresentations.push_back(std::move(pPresentation));
	return m_vpPresentations.back().get();
}

CSessionPresentation *CSessionPresentationManager::Find(CSessionId SessionId)
{
	const auto It = std::find_if(m_vpPresentations.begin(), m_vpPresentations.end(), [SessionId](const auto &pPresentation) { return pPresentation->SessionId() == SessionId; });
	return It == m_vpPresentations.end() ? nullptr : It->get();
}

const CSessionPresentation *CSessionPresentationManager::Find(CSessionId SessionId) const
{
	const auto It = std::find_if(m_vpPresentations.begin(), m_vpPresentations.end(), [SessionId](const auto &pPresentation) { return pPresentation->SessionId() == SessionId; });
	return It == m_vpPresentations.end() ? nullptr : It->get();
}

void CSessionPresentationManager::SetAudible(CSessionId SessionId)
{
	if(m_AudibleSessionId == SessionId)
		return;
	CSessionPresentation *pPrevious = Find(m_AudibleSessionId);
	if(pPrevious != nullptr)
		pPrevious->MapSounds().SetAudible(false);
	m_AudibleSessionId = SessionId;
	CSessionPresentation *pCurrent = Find(m_AudibleSessionId);
	if(pCurrent != nullptr)
		pCurrent->MapSounds().SetAudible(true);
}

void CSessionPresentationManager::Unload(CSessionId SessionId)
{
	CSessionPresentation *pPresentation = Find(SessionId);
	if(pPresentation == nullptr)
		return;
	if(m_AudibleSessionId == SessionId)
		m_AudibleSessionId = CSessionId();
	pPresentation->Unload();
}

void CSessionPresentationManager::UnloadAll()
{
	m_AudibleSessionId = CSessionId();
	for(auto &pPresentation : m_vpPresentations)
		pPresentation->Unload();
}
