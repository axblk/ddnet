#include "game_view.h"

#include "session_context.h"

#include <base/dbg.h>
#include <base/math.h>

#include <algorithm>

void CGameView::CEmoticonSelectorState::UpdateSelection(int NumEmoticons, int NumEyeEmotes, bool AllowEyeWheel)
{
	if(length(m_SelectorMouse) > 170.0f)
		m_SelectorMouse = normalize(m_SelectorMouse) * 170.0f;

	float SelectedAngle = angle(m_SelectorMouse) + 2 * pi / 24;
	if(SelectedAngle < 0)
		SelectedAngle += 2 * pi;

	m_SelectedEmote = -1;
	m_SelectedEyeEmote = -1;
	if(length(m_SelectorMouse) > 110.0f)
		m_SelectedEmote = static_cast<int>(SelectedAngle / (2 * pi) * NumEmoticons);
	else if(AllowEyeWheel && length(m_SelectorMouse) > 40.0f)
		m_SelectedEyeEmote = static_cast<int>(SelectedAngle / (2 * pi) * NumEyeEmotes);
}

void CGameView::CSpectatorSelectorState::UpdateSelection(float ObjWidth, float LineHeight, int PerLine, const std::array<int, MAX_CLIENTS> &aClients, int NumClients, bool AllowFollow)
{
	m_SelectedSpectatorId = NO_SELECTION;
	if(m_SelectorMouse.x >= -(ObjWidth - 20.0f) && m_SelectorMouse.x <= -(ObjWidth - 20.0f) + ObjWidth * 2.0f / 3.0f - 40.0f && m_SelectorMouse.y >= -280.0f && m_SelectorMouse.y <= -220.0f)
		m_SelectedSpectatorId = SPEC_FREEVIEW;
	else if(m_SelectorMouse.x >= -(ObjWidth - 20.0f) + ObjWidth * 2.0f / 3.0f && m_SelectorMouse.x <= -(ObjWidth - 20.0f) + ObjWidth * 4.0f / 3.0f - 40.0f && m_SelectorMouse.y >= -280.0f && m_SelectorMouse.y <= -220.0f)
		m_SelectedSpectatorId = MULTI_VIEW;
	else if(AllowFollow && m_SelectorMouse.x >= -(ObjWidth - 20.0f) + ObjWidth * 4.0f / 3.0f && m_SelectorMouse.x <= -(ObjWidth - 20.0f) + ObjWidth * 2.0f - 40.0f && m_SelectorMouse.y >= -280.0f && m_SelectorMouse.y <= -220.0f)
		m_SelectedSpectatorId = SPEC_FOLLOW;

	float x = -(ObjWidth - 35.0f);
	float y = -190.0f;
	for(int Index = 0; m_SelectedSpectatorId == NO_SELECTION && Index < NumClients; ++Index)
	{
		const int Count = Index + 1;
		if(Count > PerLine && (Count - 1) % PerLine == 0)
		{
			x += 290.0f;
			y = -190.0f;
		}
		if(m_SelectorMouse.x >= x - 10.0f && m_SelectorMouse.x < x + 260.0f && m_SelectorMouse.y >= y - LineHeight / 6.0f && m_SelectorMouse.y < y + LineHeight * 5.0f / 6.0f)
			m_SelectedSpectatorId = aClients[Index];
		y += LineHeight;
	}
}

bool CVisibleWorldRect::Inside(vec2 Position, vec2 Margin) const
{
	return in_range(Position.x, m_TopLeft.x - Margin.x, m_BottomRight.x + Margin.x) &&
	       in_range(Position.y, m_TopLeft.y - Margin.y, m_BottomRight.y + Margin.y);
}

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

CPresentationContext::CPresentationContext(const CGameSessionContext &Session, CGameState &State, CGameTickInfo Time, std::span<const CVisibleWorldRect> vVisibleWorldRects, EPresentationPlayback Playback, EPresentationAudio Audio) :
	m_Session(Session),
	m_State(State),
	m_Time(Time),
	m_vVisibleWorldRects(vVisibleWorldRects),
	m_Playback(Playback),
	m_Audio(Audio)
{
	dbg_assert(Session.GameStates().Find(State.Id()) == &State, "presentation context state does not belong to session");
	dbg_assert(Time.m_GameTickSpeed > 0, "presentation context tick speed must be positive");
}

bool CPresentationContext::IsVisible(vec2 Position, vec2 Margin) const
{
	return std::any_of(m_vVisibleWorldRects.begin(), m_vVisibleWorldRects.end(), [Position, Margin](const CVisibleWorldRect &VisibleWorldRect) {
		return VisibleWorldRect.Inside(Position, Margin);
	});
}

bool CPresentationContext::IsOtherTeamFromLocalPlayer(int ClientId) const
{
	return m_State.IsOtherTeamFromLocalPlayer(ClientId);
}

CRenderContext::CRenderContext(const CGameSessionContext &Session, const CGameState &State, const CGameView &View, CGameTickInfo Time, CVisibleWorldRect VisibleWorldRect, uint64_t OutputCacheKey, bool IsVideoOutput) :
	m_Session(Session),
	m_State(State),
	m_View(View),
	m_Time(Time),
	m_VisibleWorldRect(VisibleWorldRect),
	m_OutputCacheKey(OutputCacheKey),
	m_IsVideoOutput(IsVideoOutput)
{
	dbg_assert(Session.Id() == View.SessionId(), "render context session does not match view");
	dbg_assert(State.Id() == View.StateId(), "render context state does not match view");
	dbg_assert(Session.GameStates().Find(State.Id()) == &State, "render context state does not belong to session");
	dbg_assert(Time.m_GameTickSpeed > 0, "render context tick speed must be positive");
}

float CRenderContext::AspectRatio(float DefaultAspectRatio) const
{
	const CViewport &Viewport = m_View.Viewport();
	return Viewport.m_Width > 0 && Viewport.m_Height > 0 ? Viewport.m_Width / (float)Viewport.m_Height : DefaultAspectRatio;
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

float CRenderContext::AlphaForOwner(int OwnerClientId, float OtherTeamAlpha) const
{
	return OwnerClientId >= 0 && IsOtherTeam(OwnerClientId) ? OtherTeamAlpha : 1.0f;
}

CGameRenderRequest::CGameRenderRequest(const CGameSessionContext &Session, CGameState &State, const CGameView &View, CGameTickInfo Time, CVisibleWorldRect VisibleWorldRect, EPresentationPlayback Playback, EPresentationAudio Audio, CRenderOutput &Output) :
	m_Session(Session),
	m_State(State),
	m_View(View),
	m_Time(Time),
	m_VisibleWorldRect(VisibleWorldRect),
	m_Playback(Playback),
	m_Audio(Audio),
	m_Output(Output)
{
	dbg_assert(Session.Id() == View.SessionId(), "render request session does not match view");
	dbg_assert(State.Id() == View.StateId(), "render request state does not match view");
	dbg_assert(Session.GameStates().Find(State.Id()) == &State, "render request state does not belong to session");
	dbg_assert(Time.m_GameTickSpeed > 0, "render request tick speed must be positive");
}

const CGameRenderRequest *FindAudibleRenderRequest(std::span<const CGameRenderRequest> vRequests)
{
	const CGameRenderRequest *pAudibleRequest = nullptr;
	for(const CGameRenderRequest &Request : vRequests)
	{
		if(Request.m_Audio != EPresentationAudio::AUDIBLE || !Request.m_Time.m_IsGameActive)
			continue;
		dbg_assert(pAudibleRequest == nullptr, "only one active render request can be audible");
		pAudibleRequest = &Request;
	}
	return pAudibleRequest;
}

void CGameRenderScheduler::Run(std::span<const CGameRenderRequest> vRequests, const FUpdatePresentation &UpdatePresentation, const FRenderView &RenderView) const
{
	struct CStateGroup
	{
		const CGameSessionContext *m_pSession;
		CGameState *m_pState;
		CGameTickInfo m_Time;
		EPresentationPlayback m_Playback;
		EPresentationAudio m_Audio;
		std::vector<CVisibleWorldRect> m_vVisibleWorldRects;
	};

	auto SameTime = [](const CGameTickInfo &Left, const CGameTickInfo &Right) {
		return Left.m_PrevGameTick == Right.m_PrevGameTick &&
		       Left.m_GameTick == Right.m_GameTick &&
		       Left.m_PredGameTick == Right.m_PredGameTick &&
		       Left.m_PredictionTick == Right.m_PredictionTick &&
		       Left.m_IntraGameTick == Right.m_IntraGameTick &&
		       Left.m_IntraGameTickSincePrev == Right.m_IntraGameTickSincePrev &&
		       Left.m_PredIntraGameTick == Right.m_PredIntraGameTick &&
		       Left.m_GameTickTime == Right.m_GameTickTime &&
		       Left.m_FrameTimeAverage == Right.m_FrameTimeAverage &&
		       Left.m_GameTickSpeed == Right.m_GameTickSpeed &&
		       Left.m_PredictionTime == Right.m_PredictionTime &&
		       Left.m_PresentationTime == Right.m_PresentationTime &&
		       Left.m_PresentationTimeFrequency == Right.m_PresentationTimeFrequency &&
		       Left.m_AnimationPlaybackSpeed == Right.m_AnimationPlaybackSpeed &&
		       Left.m_IsGameActive == Right.m_IsGameActive &&
		       Left.m_IsDemoPlayback == Right.m_IsDemoPlayback &&
		       Left.m_IsDemoPlaybackPaused == Right.m_IsDemoPlaybackPaused &&
		       Left.m_ConnectionProblems == Right.m_ConnectionProblems;
	};

	std::vector<CStateGroup> vGroups;
	std::vector<size_t> vRequestGroups;
	vGroups.reserve(vRequests.size());
	vRequestGroups.reserve(vRequests.size());
	for(const CGameRenderRequest &Request : vRequests)
	{
		const auto Found = std::find_if(vGroups.begin(), vGroups.end(), [&Request](const CStateGroup &Group) {
			return Group.m_pSession == &Request.m_Session && Group.m_pState == &Request.m_State;
		});
		if(Found == vGroups.end())
		{
			vGroups.push_back({&Request.m_Session, &Request.m_State, Request.m_Time, Request.m_Playback, Request.m_Audio, {Request.m_VisibleWorldRect}});
			vRequestGroups.push_back(vGroups.size() - 1);
			continue;
		}

		dbg_assert(SameTime(Found->m_Time, Request.m_Time), "views of one state must use one frozen time");
		dbg_assert(Found->m_Playback == Request.m_Playback, "views of one state must use one playback policy");
		if(Request.m_Audio == EPresentationAudio::AUDIBLE)
			Found->m_Audio = EPresentationAudio::AUDIBLE;
		Found->m_vVisibleWorldRects.push_back(Request.m_VisibleWorldRect);
		vRequestGroups.push_back(Found - vGroups.begin());
	}

	for(CStateGroup &Group : vGroups)
	{
		UpdatePresentation(CPresentationContext(*Group.m_pSession, *Group.m_pState, Group.m_Time, Group.m_vVisibleWorldRects, Group.m_Playback, Group.m_Audio));
	}

	for(size_t i = 0; i < vRequests.size(); i++)
	{
		const CGameRenderRequest &Request = vRequests[i];
		const CStateGroup &Group = vGroups[vRequestGroups[i]];
		RenderView(CRenderContext(Request.m_Session, Request.m_State, Request.m_View, Group.m_Time, Request.m_VisibleWorldRect, Request.m_Output.PresentationCacheKey(), Request.m_Output.IsVideoOutput()), Request.m_Output);
	}
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
