#include "game_view.h"

#include <algorithm>

CGameViewId CGameViewManager::Create(CGameStateId StateId)
{
	if(!StateId.IsValid())
		return {};
	const CGameViewId Id(m_NextId++);
	m_vpViews.push_back(std::make_unique<CGameView>(Id, StateId));
	return Id;
}

CGameView *CGameViewManager::Find(CGameViewId Id)
{
	const auto Found = std::find_if(m_vpViews.begin(), m_vpViews.end(), [Id](const auto &pView) { return pView->Id() == Id; });
	return Found == m_vpViews.end() ? nullptr : Found->get();
}

const CGameView *CGameViewManager::Find(CGameViewId Id) const
{
	const auto Found = std::find_if(m_vpViews.begin(), m_vpViews.end(), [Id](const auto &pView) { return pView->Id() == Id; });
	return Found == m_vpViews.end() ? nullptr : Found->get();
}

bool CGameViewManager::Destroy(CGameViewId Id)
{
	const auto Found = std::find_if(m_vpViews.begin(), m_vpViews.end(), [Id](const auto &pView) { return pView->Id() == Id; });
	if(Found == m_vpViews.end())
		return false;
	m_vpViews.erase(Found);
	return true;
}

void CGameStateRenderer::Render(const CRenderContext &Context, CRenderOutput &Output) const
{
	Output.BeginView(Context.m_View.Viewport(), Context.m_View.CameraPosition(), Context.m_View.Zoom());
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CGameState::CClientSnapshot &SnapshotClient = Context.m_State.Client(ClientId);
		if(!SnapshotClient.m_HasCharacter)
			continue;

		vec2 Position(SnapshotClient.m_Character.m_X, SnapshotClient.m_Character.m_Y);
		const CGameState::CPredictedClient &PredictedClient = Context.m_State.PredictedClient(ClientId);
		if(PredictedClient.m_HasCurrent)
			Position = PredictedClient.m_Current.m_Pos;
		Output.DrawCharacter(ClientId, Position, ClientId == Context.m_State.LocalClientId());
	}
	Output.EndView();
}
