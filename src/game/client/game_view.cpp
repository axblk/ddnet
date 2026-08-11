#include "game_view.h"

#include "session_context.h"

#include <base/dbg.h>
#include <base/math.h>

#include <algorithm>

CGameViewId CGameViewManager::Create(CSessionId SessionId, CGameStateId StateId)
{
	if(!SessionId.IsValid() || !StateId.IsValid())
		return {};
	const CGameViewId Id(m_NextId++);
	m_vpViews.push_back(std::make_unique<CGameView>(Id, SessionId, StateId));
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

CRenderContext::CRenderContext(const CGameSessionContext &Session, const CGameState &State, const CGameView &View, CGameTickInfo Time) :
	m_Session(Session),
	m_State(State),
	m_View(View),
	m_Time(Time)
{
	dbg_assert(Session.Id() == View.SessionId(), "render context session does not match view");
	dbg_assert(State.Id() == View.StateId(), "render context state does not match view");
	dbg_assert(Session.GameStates().Find(State.Id()) == &State, "render context state does not belong to session");
	dbg_assert(Time.m_GameTickSpeed > 0, "render context tick speed must be positive");
}

bool CRenderContext::IsOtherTeam(int ClientId) const
{
	const int LocalClientId = m_State.LocalClientId();
	if(!in_range(ClientId, MAX_CLIENTS - 1) || !in_range(LocalClientId, MAX_CLIENTS - 1))
		return false;
	if(m_View.IsSpectating())
	{
		const int SpectatorId = m_View.SpectatorId();
		if(!in_range(SpectatorId, MAX_CLIENTS - 1))
			return false;
		if(m_State.Teams().Team(ClientId) == TEAM_SUPER || m_State.Teams().Team(SpectatorId) == TEAM_SUPER)
			return false;
		return m_State.Teams().Team(ClientId) != m_State.Teams().Team(SpectatorId);
	}

	const CNetObj_DDNetCharacter *pLocalExtended = m_State.ExtendedCharacter(LocalClientId);
	const CNetObj_DDNetCharacter *pClientExtended = m_State.ExtendedCharacter(ClientId);
	const bool LocalSolo = pLocalExtended != nullptr && (pLocalExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
	const bool ClientSolo = pClientExtended != nullptr && (pClientExtended->m_Flags & CHARACTERFLAG_SOLO) != 0;
	if((LocalSolo || ClientSolo) && ClientId != LocalClientId)
		return true;
	if(m_State.Teams().Team(ClientId) == TEAM_SUPER || m_State.Teams().Team(LocalClientId) == TEAM_SUPER)
		return false;
	return m_State.Teams().Team(ClientId) != m_State.Teams().Team(LocalClientId);
}

void CGameStateRenderer::Render(const CRenderContext &Context, CRenderOutput &Output) const
{
	Output.BeginView(Context.m_View.Viewport(), Context.m_View.CameraPosition(), Context.m_View.Zoom());
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const CGameState::CClientSnapshot &SnapshotClient = Context.m_State.Client(ClientId);
		if(SnapshotClient.m_HasCharacter)
		{
			vec2 Position(SnapshotClient.m_Character.m_X, SnapshotClient.m_Character.m_Y);
			const CGameState::CPredictedClient &PredictedClient = Context.m_State.PredictedClient(ClientId);
			if(PredictedClient.m_HasCurrent)
				Position = PredictedClient.m_Current.m_Pos;
			Output.DrawCharacter(ClientId, Position, ClientId == Context.m_State.LocalClientId());
		}
		if(SnapshotClient.m_HasSpecChar)
		{
			Output.DrawSpectatorCharacter(ClientId, vec2(SnapshotClient.m_SpecChar.m_X, SnapshotClient.m_SpecChar.m_Y), Context.IsOtherTeam(ClientId));
		}
	}
	Output.EndView();
}
