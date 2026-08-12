/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "spectator.h"

#include "camera.h"

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

#include <limits>

namespace
{
	struct CSpectatorMenuLayout
	{
		float m_Width;
		float m_Height = 1200.0f;
		float m_ObjWidth = 300.0f;
		float m_FontSize = 20.0f;
		float m_LineHeight = 60.0f;
		float m_TeeSizeMod = 1.0f;
		float m_RoundRadius = 30.0f;
		int m_PerLine = 8;
		float m_BoxMove = -10.0f;
		float m_BoxOffset = 0.0f;

		CSpectatorMenuLayout(float AspectRatio, int TotalPlayers) :
			m_Width(1200.0f * AspectRatio)
		{
			if(TotalPlayers > 64)
			{
				m_FontSize = 12.0f;
				m_LineHeight = 15.0f;
				m_TeeSizeMod = 0.3f;
				m_PerLine = 32;
				m_RoundRadius = 5.0f;
				m_BoxMove = 3.0f;
				m_BoxOffset = 6.0f;
			}
			else if(TotalPlayers > 32)
			{
				m_FontSize = 18.0f;
				m_LineHeight = 30.0f;
				m_TeeSizeMod = 0.7f;
				m_PerLine = 16;
				m_RoundRadius = 10.0f;
				m_BoxMove = 3.0f;
				m_BoxOffset = 6.0f;
			}
			if(TotalPlayers > 16)
				m_ObjWidth = 600.0f;
		}
	};

	std::array<int, MAX_CLIENTS> SpectatorClients(const CGameState &State, const std::array<int, MAX_CLIENTS> *pClientsByDDTeamName, int &TotalPlayers)
	{
		std::array<int, MAX_CLIENTS> aResult;
		aResult.fill(-1);
		TotalPlayers = 0;
		if(!pClientsByDDTeamName)
			return aResult;
		for(int ClientId : *pClientsByDDTeamName)
		{
			if(ClientId < 0)
				break;
			const CGameState::CClientSnapshot &Client = State.Client(ClientId);
			if(Client.m_HasPlayerInfo && Client.m_PlayerInfo.m_Team != TEAM_SPECTATORS)
				aResult[TotalPlayers++] = ClientId;
		}
		return aResult;
	}

}

CGameView::CSpectatorSelectorState &CSpectator::Selector()
{
	return GameClient()->LegacyGameView().SpectatorSelector();
}

const CGameView::CSpectatorSelectorState &CSpectator::Selector() const
{
	return GameClient()->LegacyGameView().SpectatorSelector();
}

bool CSpectator::CanChangeSpectatorId()
{
	// don't change SpectatorId when not spectating
	if(!GameClient()->m_Snap.m_SpecInfo.m_Active)
		return false;

	// stop follow mode from changing SpectatorId
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK && GameClient()->m_DemoSpecId == SPEC_FOLLOW)
		return false;

	return true;
}

void CSpectator::SpectateNext(bool Reverse)
{
	int CurIndex = -1;
	const CNetObj_PlayerInfo **paPlayerInfos = GameClient()->m_Snap.m_apInfoByDDTeamName;

	// m_SpectatorId may be uninitialized if m_Active is false
	if(GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(paPlayerInfos[i] && paPlayerInfos[i]->m_ClientId == GameClient()->m_Snap.m_SpecInfo.m_SpectatorId)
			{
				CurIndex = i;
				break;
			}
		}
	}

	int Start;
	if(CurIndex != -1)
	{
		if(Reverse)
			Start = CurIndex - 1;
		else
			Start = CurIndex + 1;
	}
	else
	{
		if(Reverse)
			Start = -1;
		else
			Start = 0;
	}

	int Increment = Reverse ? -1 : 1;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		int PlayerIndex = (Start + i * Increment) % MAX_CLIENTS;
		// % in C++ takes the sign of the dividend, not divisor
		if(PlayerIndex < 0)
			PlayerIndex += MAX_CLIENTS;

		const CNetObj_PlayerInfo *pPlayerInfo = paPlayerInfos[PlayerIndex];
		if(pPlayerInfo && pPlayerInfo->m_Team != TEAM_SPECTATORS)
		{
			Spectate(pPlayerInfo->m_ClientId);
			break;
		}
	}
}

void CSpectator::ConKeySpectator(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;

	if(pSelf->GameClient()->m_Scoreboard.IsActive())
		return;

	CGameView &View = pSelf->GameClient()->LegacyGameView();
	CGameView::CSpectatorSelectorState &Selector = View.SpectatorSelector();
	if(pResult->GetInteger(0) == 0)
	{
		Selector.m_Active = false;
		return;
	}

	CGameSessionContext &Session = pSelf->GameClient()->SessionContext();
	CGameState *pState = Session.GameStates().Find(View.StateId());
	const bool Demo = Session.Id() == pSelf->Client()->DemoSessionId();
	if(Session.Id() != View.SessionId() || (!View.IsSpectating() && !Demo) || !pState)
	{
		Selector.m_Active = false;
		return;
	}

	const int Conn = static_cast<int>(pState->StreamId().Value()) - 1;
	if(!Demo && (Session.Id() != pSelf->Client()->NetworkSessionId() || Conn < IClient::CONN_MAIN || Conn >= IClient::NUM_CONNS))
		return;
	Selector.m_OriginSessionId = Session.Id();
	Selector.m_OriginStateId = pState->Id();
	Selector.m_OriginConnection = Conn;
	Selector.m_OriginSixup = Session.Protocol() == EGameProtocol::SIXUP;
	Selector.m_OriginDemo = Demo;
	Selector.m_Active = true;
}

void CSpectator::ConSpectate(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	if(!pSelf->CanChangeSpectatorId())
		return;

	pSelf->Spectate(pResult->GetInteger(0));
}

void CSpectator::ConSpectateNext(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	if(!pSelf->CanChangeSpectatorId())
		return;

	pSelf->SpectateNext(false);
}

void CSpectator::ConSpectatePrevious(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	if(!pSelf->CanChangeSpectatorId())
		return;

	pSelf->SpectateNext(true);
}

void CSpectator::ConSpectateClosest(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	pSelf->SpectateClosest();
}

void CSpectator::ConMultiView(IConsole::IResult *pResult, void *pUserData)
{
	CSpectator *pSelf = (CSpectator *)pUserData;
	int Input = pResult->GetInteger(0);

	if(Input == -1)
		std::fill(std::begin(pSelf->GameClient()->MultiView().m_aSelected), std::end(pSelf->GameClient()->MultiView().m_aSelected), false); // remove everyone from multiview
	else if(Input < MAX_CLIENTS && Input >= 0)
		pSelf->GameClient()->MultiView().m_aSelected[Input] = !pSelf->GameClient()->MultiView().m_aSelected[Input]; // activate or deactivate one player from multiview
}

void CSpectator::OnConsoleInit()
{
	Console()->Register("+spectate", "", CFGFLAG_CLIENT, ConKeySpectator, this, "Open spectator mode selector");
	Console()->Register("spectate", "i[spectator-id]", CFGFLAG_CLIENT, ConSpectate, this, "Switch spectator mode");
	Console()->Register("spectate_next", "", CFGFLAG_CLIENT, ConSpectateNext, this, "Spectate the next player");
	Console()->Register("spectate_previous", "", CFGFLAG_CLIENT, ConSpectatePrevious, this, "Spectate the previous player");
	Console()->Register("spectate_closest", "", CFGFLAG_CLIENT, ConSpectateClosest, this, "Spectate the closest player");
	Console()->Register("spectate_multiview", "i[id]", CFGFLAG_CLIENT, ConMultiView, this, "Add/remove Client-IDs to spectate them exclusively (-1 to reset)");
}

bool CSpectator::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(!Selector().m_Active)
		return false;

	Ui()->ConvertMouseMove(&x, &y, CursorType);
	Selector().m_SelectorMouse += vec2(x, y);
	return true;
}

bool CSpectator::OnInput(const IInput::CEvent &Event)
{
	if(IsActive() && Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		OnRelease();
		return true;
	}

	if(g_Config.m_ClSpectatorMouseclicks)
	{
		if(GameClient()->m_Snap.m_SpecInfo.m_Active && !IsActive() && !GameClient()->MultiView().m_Active &&
			!Ui()->IsPopupOpen() && !GameClient()->m_GameConsole.IsActive() && !GameClient()->m_Menus.IsActive())
		{
			if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_MOUSE_1)
			{
				if(GameClient()->m_Snap.m_SpecInfo.m_SpectatorId != SPEC_FREEVIEW)
					Spectate(SPEC_FREEVIEW);
				else
					SpectateClosest();
				return true;
			}
		}
	}

	if(GameClient()->m_Camera.SpectatingPlayer() && GameClient()->m_Camera.CanUseAutoSpecCamera())
	{
		if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_MOUSE_2)
		{
			GameClient()->m_Camera.ResetAutoSpecCamera();
			return true;
		}
	}

	return false;
}

void CSpectator::OnRelease()
{
	OnReset();
}

void CSpectator::ResetMultiView(CGameView &View)
{
	GameClient()->m_Camera.SetZoom(CCamera::ZoomStepsToValue(g_Config.m_ClDefaultZoom - 10), g_Config.m_ClSmoothZoomTime, true);
	CGameView::CMultiViewState &MultiView = View.MultiView();
	MultiView.m_PersonalZoom = 0.0f;
	MultiView.m_Active = false;
	MultiView.m_Solo = false;
	MultiView.m_IsInit = false;
	MultiView.m_Teleported = false;
	MultiView.m_OldCameraDistance = 0.0f;
}

void CSpectator::ApplySelection(CGameView &View, CGameView::CSpectatorSelectorState &Selector, int SpectatorId, int DDTeam, bool Toggle, float Now)
{
	CGameView::CMultiViewState &MultiView = View.MultiView();
	if(SpectatorId == MULTI_VIEW)
	{
		MultiView.m_Active = true;
		return;
	}
	if(SpectatorId == SPEC_FREEVIEW || SpectatorId == SPEC_FOLLOW)
	{
		MultiView.m_Active = false;
		Spectate(View, Selector, SpectatorId);
		return;
	}
	if(!MultiView.m_Active)
	{
		Spectate(View, Selector, SpectatorId);
		return;
	}
	if(MultiView.m_Team != DDTeam)
	{
		ResetMultiView(View);
		Spectate(View, Selector, SpectatorId);
		Selector.m_MultiViewActivateTime = Now + 0.3f;
		return;
	}
	if(!Toggle)
		return;

	MultiView.m_aSelected[SpectatorId] = !MultiView.m_aSelected[SpectatorId];
	if(!in_range(View.SpectatorMode(), MAX_CLIENTS - 1) || MultiView.m_aSelected[View.SpectatorMode()])
		return;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!MultiView.m_aSelected[ClientId] || MultiView.m_aVanish[ClientId])
			continue;
		MultiView.m_aSelected[ClientId] = false;
		MultiView.m_aLastFreeze[ClientId] = 0.0f;
		MultiView.m_aVanish[ClientId] = false;
		MultiView.m_aSelected[ClientId] = true;
		Spectate(View, Selector, ClientId);
		break;
	}
}

void CSpectator::UpdateController(CGameView &View, const CRenderContext &Context, float LocalTime)
{
	CGameView::CSpectatorSelectorState &State = View.SpectatorSelector();
	if(!Context.m_Time.m_IsGameActive)
		return;
	if(!View.MultiView().m_Active && State.m_MultiViewActivateTime != 0.0f && State.m_MultiViewActivateTime <= LocalTime)
	{
		State.m_MultiViewActivateTime = 0.0f;
		View.MultiView().m_Active = true;
	}

	if(!State.m_Active)
	{
		if(State.m_WasActive && State.m_SelectedSpectatorId != NO_SELECTION)
			ApplySelection(View, State, State.m_SelectedSpectatorId, State.m_SelectedDDTeam, false, LocalTime);
		State.m_WasActive = false;
		return;
	}
	if(State.m_OriginSessionId != Context.m_Session.Id() || State.m_OriginStateId != Context.m_State.Id())
	{
		State.Reset();
		m_TouchState = {};
		return;
	}
	if(!View.IsSpectating() && !State.m_OriginDemo)
	{
		State.m_Active = false;
		State.m_WasActive = false;
		return;
	}

	int TotalPlayers;
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> aClients = SpectatorClients(Context.m_State, Presentation.ClientsByDDTeamName(Context.m_State.Id()), TotalPlayers);
	const CSpectatorMenuLayout Layout(Context.AspectRatio(Graphics()->ScreenAspect()), TotalPlayers);
	const vec2 ScreenSize(Layout.m_Width, Layout.m_Height);
	const vec2 ScreenCenter = ScreenSize / 2.0f;
	CUIRect SpectatorRect = {Layout.m_Width / 2.0f - Layout.m_ObjWidth, Layout.m_Height / 2.0f - 300.0f, Layout.m_ObjWidth * 2.0f, 600.0f};
	CUIRect SpectatorMouseRect;
	SpectatorRect.Margin(20.0f, &SpectatorMouseRect);

	const bool WasTouchPressed = m_TouchState.m_AnyPressed;
	Ui()->UpdateTouchState(m_TouchState);
	if(m_TouchState.m_AnyPressed)
	{
		const vec2 TouchPos = (m_TouchState.m_PrimaryPosition - vec2(0.5f, 0.5f)) * ScreenSize;
		if(SpectatorMouseRect.Inside(ScreenCenter + TouchPos))
			State.m_SelectorMouse = TouchPos;
	}
	else if(WasTouchPressed)
	{
		const vec2 TouchPos = (m_TouchState.m_PrimaryPosition - vec2(0.5f, 0.5f)) * ScreenSize;
		if(!SpectatorRect.Inside(ScreenCenter + TouchPos))
		{
			State.Reset();
			m_TouchState = {};
			return;
		}
	}

	State.m_SelectorMouse.x = std::clamp(State.m_SelectorMouse.x, -(Layout.m_ObjWidth - 20.0f), Layout.m_ObjWidth - 20.0f);
	State.m_SelectorMouse.y = std::clamp(State.m_SelectorMouse.y, -280.0f, 280.0f);
	State.UpdateSelection(Layout.m_ObjWidth, Layout.m_LineHeight, Layout.m_PerLine, aClients, TotalPlayers, State.m_OriginDemo && Context.m_State.LocalClientId() >= 0);
	const int SelectedId = State.m_SelectedSpectatorId;
	State.m_SelectedDDTeam = in_range(SelectedId, MAX_CLIENTS - 1) ? Context.m_State.Teams().Team(SelectedId) : 0;
	if(Input()->KeyPress(KEY_MOUSE_1) || m_TouchState.m_PrimaryPressed)
	{
		State.m_PendingSpectatorId = State.m_SelectedSpectatorId;
		State.m_PendingDDTeam = State.m_SelectedDDTeam;
	}
	State.m_WasActive = true;
}

void CSpectator::CommitController(CGameView &View, const CRenderContext &Context, float LocalTime)
{
	CGameView::CSpectatorSelectorState &State = View.SpectatorSelector();
	if(State.m_OriginSessionId != Context.m_Session.Id() || State.m_OriginStateId != Context.m_State.Id())
	{
		State.Reset();
		return;
	}
	if(State.m_PendingSpectatorId == NO_SELECTION)
		return;
	const int SpectatorId = State.m_PendingSpectatorId;
	const int DDTeam = State.m_PendingDDTeam;
	State.m_PendingSpectatorId = NO_SELECTION;
	ApplySelection(View, State, SpectatorId, DDTeam, true, LocalTime);
}

void CSpectator::OnRender(const CRenderContext &Context)
{
	const CGameView::CSpectatorSelectorState &State = Context.m_View.SpectatorSelector();
	if(!Context.m_Time.m_IsGameActive || !State.m_Active || State.m_OriginSessionId != Context.m_Session.Id() || State.m_OriginStateId != Context.m_State.Id())
		return;

	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	int TotalPlayers;
	const std::array<int, MAX_CLIENTS> aClients = SpectatorClients(Context.m_State, Presentation.ClientsByDDTeamName(Context.m_State.Id()), TotalPlayers);
	const CSpectatorMenuLayout Layout(Context.AspectRatio(Graphics()->ScreenAspect()), TotalPlayers);
	const float Width = Layout.m_Width;
	const float Height = Layout.m_Height;
	const float ObjWidth = Layout.m_ObjWidth;
	const float FontSize = Layout.m_FontSize;
	const float BigFontSize = 20.0f;
	const float StartY = -190.0f;
	const float LineHeight = Layout.m_LineHeight;
	const float TeeSizeMod = Layout.m_TeeSizeMod;
	const float RoundRadius = Layout.m_RoundRadius;
	const float BoxMove = Layout.m_BoxMove;
	const float BoxOffset = Layout.m_BoxOffset;
	int HighestClientId = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(Context.m_State.Client(ClientId).m_HasPlayerInfo)
			HighestClientId = ClientId;
	}
	const vec2 ScreenSize(Width, Height);
	const vec2 ScreenCenter = ScreenSize / 2.0f;
	CUIRect SpectatorRect = {Width / 2.0f - ObjWidth, Height / 2.0f - 300.0f, ObjWidth * 2.0f, 600.0f};

	Graphics()->MapScreenToSize(Width, Height);

	SpectatorRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.3f), IGraphics::CORNER_ALL, 20.0f);

	// draw selections
	if(Context.m_View.SpectatorMode() == SPEC_FREEVIEW)
	{
		Graphics()->DrawRect(Width / 2.0f - (ObjWidth - 20.0f), Height / 2.0f - 280.0f, ((ObjWidth * 2.0f) / 3.0f) - 40.0f, 60.0f, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_ALL, 20.0f);
	}

	if(Context.m_View.MultiView().m_Active)
	{
		Graphics()->DrawRect(Width / 2.0f - (ObjWidth - 20.0f) + (ObjWidth * 2.0f / 3.0f), Height / 2.0f - 280.0f, ((ObjWidth * 2.0f) / 3.0f) - 40.0f, 60.0f, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_ALL, 20.0f);
	}

	if(State.m_OriginDemo && Context.m_State.LocalClientId() >= 0 && Context.m_View.SpectatorMode() == SPEC_FOLLOW)
	{
		Graphics()->DrawRect(Width / 2.0f - (ObjWidth - 20.0f) + (ObjWidth * 2.0f * 2.0f / 3.0f), Height / 2.0f - 280.0f, ((ObjWidth * 2.0f) / 3.0f) - 40.0f, 60.0f, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_ALL, 20.0f);
	}

	const bool FreeViewSelected = State.m_SelectedSpectatorId == SPEC_FREEVIEW;
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, FreeViewSelected ? 1.0f : 0.5f);
	TextRender()->Text(Width / 2.0f - (ObjWidth - 40.0f), Height / 2.0f - 280.f + (60.f - BigFontSize) / 2.f, BigFontSize, Localize("Free-View"), -1.0f);

	const bool MultiViewSelected = State.m_SelectedSpectatorId == MULTI_VIEW;
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, MultiViewSelected ? 1.0f : 0.5f);
	TextRender()->Text(Width / 2.0f - (ObjWidth - 40.0f) + (ObjWidth * 2.0f / 3.0f), Height / 2.0f - 280.f + (60.f - BigFontSize) / 2.f, BigFontSize, Localize("Multi-View"), -1.0f);

	if(State.m_OriginDemo && Context.m_State.LocalClientId() >= 0)
	{
		const bool FollowSelected = State.m_SelectedSpectatorId == SPEC_FOLLOW;
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, FollowSelected ? 1.0f : 0.5f);
		TextRender()->Text(Width / 2.0f - (ObjWidth - 40.0f) + (ObjWidth * 2.0f * 2.0f / 3.0f), Height / 2.0f - 280.0f + (60.f - BigFontSize) / 2.f, BigFontSize, Localize("Follow"), -1.0f);
	}

	float x = -(ObjWidth - 35.0f), y = StartY;

	int OldDDTeam = -1;

	for(int Index = 0; Index < TotalPlayers; ++Index)
	{
		const int Count = Index + 1;
		if(Count > Layout.m_PerLine && (Count - 1) % Layout.m_PerLine == 0)
		{
			x += 290.0f;
			y = StartY;
		}

		const int ClientId = aClients[Index];
		const CGameState::CClientSnapshot &SnapshotClient = Context.m_State.Client(ClientId);
		const CClientPresentation *pClient = Presentation.Client(Context.m_State.Id(), ClientId);
		if(!pClient)
			continue;
		const int DDTeam = Context.m_State.Teams().Team(ClientId);
		const int NextDDTeam = Index + 1 < TotalPlayers ? Context.m_State.Teams().Team(aClients[Index + 1]) : 0;

		if(DDTeam != TEAM_FLOCK)
		{
			const ColorRGBA Color = GameClient()->GetDDTeamColor(DDTeam).WithAlpha(0.5f);
			int Corners = 0;
			if(OldDDTeam != DDTeam)
				Corners |= IGraphics::CORNER_TL | IGraphics::CORNER_TR;
			if(NextDDTeam != DDTeam)
				Corners |= IGraphics::CORNER_BL | IGraphics::CORNER_BR;
			Graphics()->DrawRect(Width / 2.0f + x - 10.0f + BoxOffset, Height / 2.0f + y + BoxMove, 270.0f - BoxOffset, LineHeight, Color, Corners, RoundRadius);
		}

		OldDDTeam = DDTeam;

		if(Context.m_View.SpectatorMode() == ClientId)
		{
			Graphics()->DrawRect(Width / 2.0f + x - 10.0f + BoxOffset, Height / 2.0f + y + BoxMove, 270.0f - BoxOffset, LineHeight, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_ALL, RoundRadius);
		}

		const bool PlayerSelected = State.m_SelectedSpectatorId == ClientId;
		float TeeAlpha;
		if(State.m_OriginDemo && (!SnapshotClient.m_HasCharacter || !SnapshotClient.m_HasPrevCharacter))
		{
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.25f);
			TeeAlpha = 0.5f;
		}
		else
		{
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, PlayerSelected ? 1.0f : 0.5f);
			TeeAlpha = 1.0f;
		}
		CTextCursor NameCursor;
		NameCursor.SetPosition(vec2(Width / 2.0f + x + 50.0f, Height / 2.0f + y + BoxMove + (LineHeight - FontSize) / 2.f));
		NameCursor.m_FontSize = FontSize;
		NameCursor.m_Flags |= TEXTFLAG_ELLIPSIS_AT_END;
		NameCursor.m_LineWidth = 180.0f;
		if(g_Config.m_ClShowIds)
		{
			char aClientId[16];
			GameClient()->FormatClientId(ClientId, aClientId, HighestClientId);
			TextRender()->TextEx(&NameCursor, aClientId);
		}
		TextRender()->TextEx(&NameCursor, pClient->m_aName);

		if(Context.m_View.MultiView().m_Active)
		{
			if(Context.m_View.MultiView().m_aSelected[ClientId])
			{
				TextRender()->TextColor(0.1f, 1.0f, 0.1f, PlayerSelected ? 1.0f : 0.5f);
				TextRender()->Text(Width / 2.0f + x + 50.0f + 180.0f, Height / 2.0f + y + BoxMove + (LineHeight - FontSize) / 2.f, FontSize - 3, "⬤", 220.0f);
			}
			else if(Context.m_View.MultiView().m_Team == DDTeam)
			{
				TextRender()->TextColor(1.0f, 0.1f, 0.1f, PlayerSelected ? 1.0f : 0.5f);
				TextRender()->Text(Width / 2.0f + x + 50.0f + 180.0f, Height / 2.0f + y + BoxMove + (LineHeight - FontSize) / 2.f, FontSize - 3, "◯", 220.0f);
			}
		}

		// flag
		const CNetObj_GameData *pGameData = Context.m_State.GameData();
		if(Context.m_State.HasGameInfo() && (Context.m_State.GameInfo().m_GameFlags & GAMEFLAG_FLAGS) && pGameData && (pGameData->m_FlagCarrierRed == ClientId || pGameData->m_FlagCarrierBlue == ClientId))
		{
			if(pGameData->m_FlagCarrierBlue == ClientId)
				Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagBlue);
			else
				Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagRed);

			Graphics()->QuadsBegin();
			Graphics()->QuadsSetSubset(1, 0, 0, 1);

			float Size = LineHeight;
			IGraphics::CQuadItem QuadItem(Width / 2.0f + x - LineHeight / 5.0f, Height / 2.0f + y - LineHeight / 3.0f, Size / 2.0f, Size);
			Graphics()->QuadsDrawTL(&QuadItem, 1);
			Graphics()->QuadsEnd();
		}

		CTeeRenderInfo TeeInfo = pClient->m_RenderInfo;
		TeeInfo.m_Size *= TeeSizeMod;

		const CAnimState *pIdleState = CAnimState::GetIdle();
		vec2 OffsetToMid;
		CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeInfo, OffsetToMid);
		vec2 TeeRenderPos(Width / 2.0f + x + 20.0f, Height / 2.0f + y + BoxMove + LineHeight / 2.0f + OffsetToMid.y);

		RenderTools()->RenderTee(pIdleState, &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos, TeeAlpha);

		if(pClient->m_Friend)
		{
			TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageFriendColor)));
			TextRender()->Text(Width / 2.0f + x - TeeInfo.m_Size / 2.0f, Height / 2.0f + y + BoxMove + (LineHeight - FontSize) / 2.f, FontSize, "♥", 220.0f);
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);
		}

		y += LineHeight;
	}
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

	RenderTools()->RenderCursor(ScreenCenter + State.m_SelectorMouse, 48.0f);
}

void CSpectator::OnReset()
{
	Selector().Reset();
	m_TouchState = {};
}

bool CSpectator::IsActive() const
{
	return Selector().m_Active;
}

void CSpectator::Spectate(CGameView &View, const CGameView::CSpectatorSelectorState &Selector, int SpectatorId)
{
	if(Selector.m_OriginDemo)
	{
		if(Selector.m_OriginSessionId != Client()->DemoSessionId())
			return;
		GameClient()->m_DemoSpecId = std::clamp(SpectatorId, (int)SPEC_FOLLOW, MAX_CLIENTS - 1);
		View.SetSpectatorMode(GameClient()->m_DemoSpecId);
		// The tick must be rendered for the spectator mode to be updated, so we do it manually when demo playback is paused
		// TODO: https://github.com/ddnet/ddnet/issues/11681
		if(DemoPlayer()->BaseInfo()->m_Paused)
			GameClient()->m_Menus.DemoSeekTick(IDemoPlayer::TICK_CURRENT);
		return;
	}

	if(Selector.m_OriginSessionId != Client()->NetworkSessionId() || Selector.m_OriginConnection < IClient::CONN_MAIN || Selector.m_OriginConnection >= IClient::NUM_CONNS || View.SpectatorMode() == SpectatorId)
		return;

	if(Selector.m_OriginSixup)
	{
		protocol7::CNetMsg_Cl_SetSpectatorMode Msg;
		if(SpectatorId == SPEC_FREEVIEW)
		{
			Msg.m_SpecMode = protocol7::SPEC_FREEVIEW;
			Msg.m_SpectatorId = -1;
		}
		else
		{
			Msg.m_SpecMode = protocol7::SPEC_PLAYER;
			Msg.m_SpectatorId = SpectatorId;
		}
		Client()->SendPackMsg(Selector.m_OriginConnection, &Msg, MSGFLAG_VITAL, true);
		return;
	}
	CNetMsg_Cl_SetSpectatorMode Msg;
	Msg.m_SpectatorId = SpectatorId;
	Client()->SendPackMsg(Selector.m_OriginConnection, &Msg, MSGFLAG_VITAL);
}

void CSpectator::Spectate(int SpectatorId)
{
	CGameView::CSpectatorSelectorState Target;
	Target.m_OriginDemo = Client()->State() == IClient::STATE_DEMOPLAYBACK;
	Target.m_OriginSessionId = Target.m_OriginDemo ? Client()->DemoSessionId() : Client()->NetworkSessionId();
	Target.m_OriginConnection = Client()->ActiveConnection();
	Target.m_OriginSixup = Client()->IsSixup(Target.m_OriginSessionId);
	Spectate(GameClient()->LegacyGameView(), Target, SpectatorId);
}

void CSpectator::SpectateClosest()
{
	if(!CanChangeSpectatorId())
		return;

	const CGameClient::CSnapState &Snap = GameClient()->m_Snap;
	int SpectatorId = Snap.m_SpecInfo.m_SpectatorId;

	int NewSpectatorId = -1;

	vec2 CurPosition = GameClient()->m_Camera.Center();
	if(SpectatorId != SPEC_FREEVIEW)
	{
		const CNetObj_Character &CurCharacter = Snap.m_aCharacters[SpectatorId].m_Cur;
		CurPosition = vec2(CurCharacter.m_X, CurCharacter.m_Y);
	}

	int ClosestDistance = std::numeric_limits<int>::max();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(ClientId == SpectatorId || !Snap.m_aCharacters[ClientId].m_Active || !Snap.m_apPlayerInfos[ClientId] || Snap.m_apPlayerInfos[ClientId]->m_Team == TEAM_SPECTATORS)
			continue;

		if(Client()->State() != IClient::STATE_DEMOPLAYBACK && ClientId == Snap.m_LocalClientId)
			continue;

		const CNetObj_Character &MaybeClosestCharacter = Snap.m_aCharacters[ClientId].m_Cur;
		int Distance = distance(CurPosition, vec2(MaybeClosestCharacter.m_X, MaybeClosestCharacter.m_Y));
		if(NewSpectatorId == -1 || Distance < ClosestDistance)
		{
			NewSpectatorId = ClientId;
			ClosestDistance = Distance;
		}
	}
	if(NewSpectatorId > -1)
		Spectate(NewSpectatorId);
}
