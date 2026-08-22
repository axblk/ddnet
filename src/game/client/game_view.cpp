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

CPresentationContext::CPresentationContext(const CGameSessionContext &Session, CGameState &State, CGameTickInfo Time, std::span<const CVisibleWorldRect> vVisibleWorldRects, EPresentationPlayback Playback, EPresentationAudio Audio) :
	m_Session(Session),
	m_State(State),
	m_Time(Time),
	m_vVisibleWorldRects(vVisibleWorldRects),
	m_Playback(Playback),
	m_Audio(Audio)
{
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

CRenderContext::CRenderContext(const CGameSessionContext &Session, const CGameState &State, const CGameView &View, CGameTickInfo Time, CVisibleWorldRect VisibleWorldRect, bool IsVideoOutput, CVideoExportSettings VideoSettings) :
	m_Session(Session),
	m_State(State),
	m_View(View),
	m_Time(Time),
	m_VisibleWorldRect(VisibleWorldRect),
	m_IsVideoOutput(IsVideoOutput),
	m_VideoSettings(VideoSettings)
{
	dbg_assert(Session.Id() == View.SessionId() && State.m_Conn == View.Conn(), "render context state does not match view");
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
