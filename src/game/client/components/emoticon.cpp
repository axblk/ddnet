/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "emoticon.h"

#include "chat.h"

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/ui.h>

CGameView::CEmoticonSelectorState &CEmoticon::Selector()
{
	return GameClient()->LegacyGameView().EmoticonSelector();
}

const CGameView::CEmoticonSelectorState &CEmoticon::Selector() const
{
	return GameClient()->LegacyGameView().EmoticonSelector();
}

void CEmoticon::ConKeyEmoticon(IConsole::IResult *pResult, void *pUserData)
{
	CEmoticon *pSelf = (CEmoticon *)pUserData;

	if(pSelf->GameClient()->m_Scoreboard.IsActive())
		return;

	CGameView &View = pSelf->GameClient()->LegacyGameView();
	CGameView::CEmoticonSelectorState &Selector = View.EmoticonSelector();
	if(pResult->GetInteger(0) == 0)
	{
		Selector.m_Active = false;
		return;
	}

	CGameSessionContext &Session = pSelf->GameClient()->SessionContext();
	CGameState *pState = Session.GameStates().Find(View.StateId());
	if(Session.Id() != View.SessionId() || Session.Id() != pSelf->Client()->NetworkSessionId() || View.IsSpectating() || !pState)
		return;

	const int Conn = static_cast<int>(pState->StreamId().Value()) - 1;
	if(Conn < IClient::CONN_MAIN || Conn >= IClient::NUM_CONNS)
		return;
	Selector.m_OriginSessionId = Session.Id();
	Selector.m_OriginConnection = Conn;
	Selector.m_Active = true;
}

void CEmoticon::ConEmote(IConsole::IResult *pResult, void *pUserData)
{
	((CEmoticon *)pUserData)->Emote(pResult->GetInteger(0));
}

void CEmoticon::OnConsoleInit()
{
	Console()->Register("+emote", "", CFGFLAG_CLIENT, ConKeyEmoticon, this, "Open emote selector");
	Console()->Register("emote", "i[emote-id]", CFGFLAG_CLIENT, ConEmote, this, "Use emote");
}

void CEmoticon::OnReset()
{
	Selector().Reset();
	m_TouchState = {};
}

void CEmoticon::OnRelease()
{
	Selector().m_Active = false;
}

bool CEmoticon::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(!Selector().m_Active)
		return false;

	Ui()->ConvertMouseMove(&x, &y, CursorType);
	Selector().m_SelectorMouse += vec2(x, y);
	return true;
}

bool CEmoticon::OnInput(const IInput::CEvent &Event)
{
	if(IsActive() && Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		OnRelease();
		return true;
	}
	return false;
}

bool CEmoticon::EyeWheelAvailable(const CRenderContext &Context) const
{
	const int LocalClientId = Context.m_State.LocalClientId();
	return Context.m_State.CoreGameInfo().m_AllowEyeWheel && g_Config.m_ClEyeWheel && LocalClientId >= 0 &&
	       GameClient()->SessionPresentation(Context.m_Session.Id()).Client(Context.m_State.Id(), LocalClientId);
}

void CEmoticon::UpdateController(CGameView &View, const CRenderContext &Context)
{
	CGameView::CEmoticonSelectorState &State = View.EmoticonSelector();
	if(!Context.m_Time.m_IsGameActive)
		return;

	if(!State.m_Active)
	{
		if(State.m_TouchPressedOutside)
		{
			State.m_SelectedEmote = -1;
			State.m_SelectedEyeEmote = -1;
			State.m_TouchPressedOutside = false;
		}

		if(State.m_WasActive && State.m_SelectedEmote != -1)
			Emote(State.m_SelectedEmote, State.m_OriginSessionId, State.m_OriginConnection);
		if(State.m_WasActive && State.m_SelectedEyeEmote != -1)
			EyeEmote(State.m_SelectedEyeEmote, State.m_OriginSessionId, State.m_OriginConnection);
		State.m_WasActive = false;
		return;
	}

	const int LocalClientId = Context.m_State.LocalClientId();
	if(View.IsSpectating() || LocalClientId < 0 || !Context.m_State.Client(LocalClientId).m_HasCharacter)
	{
		State.m_Active = false;
		State.m_WasActive = false;
		return;
	}

	State.m_WasActive = true;
	const CUIRect Screen = {0.0f, 0.0f, 600.0f * Context.AspectRatio(Graphics()->ScreenAspect()), 600.0f};

	const bool WasTouchPressed = m_TouchState.m_AnyPressed;
	Ui()->UpdateTouchState(m_TouchState);
	if(m_TouchState.m_AnyPressed)
	{
		const vec2 TouchPos = (m_TouchState.m_PrimaryPosition - vec2(0.5f, 0.5f)) * Screen.Size();
		const float TouchCenterDistance = length(TouchPos);
		if(TouchCenterDistance <= 170.0f)
		{
			State.m_SelectorMouse = TouchPos;
		}
		else if(TouchCenterDistance > 190.0f)
		{
			State.m_TouchPressedOutside = true;
		}
	}
	else if(WasTouchPressed)
	{
		State.m_Active = false;
		return;
	}
	State.UpdateSelection(NUM_EMOTICONS, NUM_EMOTES, EyeWheelAvailable(Context));
}

void CEmoticon::OnRender(const CRenderContext &Context)
{
	const CGameView::CEmoticonSelectorState &State = Context.m_View.EmoticonSelector();
	if(!Context.m_Time.m_IsGameActive || !State.m_Active)
		return;

	const CUIRect Screen = {0.0f, 0.0f, 600.0f * Context.AspectRatio(Graphics()->ScreenAspect()), 600.0f};
	const vec2 ScreenCenter = Screen.Center();

	Ui()->MapScreen();

	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	Graphics()->SetColor(0, 0, 0, 0.3f);
	Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, 190.0f, 64);
	Graphics()->QuadsEnd();

	Graphics()->WrapClamp();
	for(int Emote = 0; Emote < NUM_EMOTICONS; Emote++)
	{
		float Angle = 2 * pi * Emote / (float)NUM_EMOTICONS;
		if(Angle > pi)
			Angle -= 2 * pi;

		Graphics()->TextureSet(GameClient()->m_EmoticonsSkin.m_aSpriteEmoticons[Emote]);
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->QuadsBegin();
		const vec2 Nudge = direction(Angle) * 150.0f;
		const float Size = State.m_SelectedEmote == Emote ? 80.0f : 50.0f;
		IGraphics::CQuadItem QuadItem(ScreenCenter.x + Nudge.x, ScreenCenter.y + Nudge.y, Size, Size);
		Graphics()->QuadsDraw(&QuadItem, 1);
		Graphics()->QuadsEnd();
	}
	Graphics()->WrapNormal();

	const int LocalClientId = Context.m_State.LocalClientId();
	const CClientPresentation *pClient = LocalClientId < 0 ? nullptr : GameClient()->SessionPresentation(Context.m_Session.Id()).Client(Context.m_State.Id(), LocalClientId);
	if(EyeWheelAvailable(Context) && pClient)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0, 1.0, 1.0, 0.3f);
		Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, 100.0f, 64);
		Graphics()->QuadsEnd();

		CTeeRenderInfo TeeInfo = pClient->m_RenderInfo;

		for(int Emote = 0; Emote < NUM_EMOTES; Emote++)
		{
			float Angle = 2 * pi * Emote / (float)NUM_EMOTES;
			if(Angle > pi)
				Angle -= 2 * pi;

			const vec2 Nudge = direction(Angle) * 70.0f;
			TeeInfo.m_Size = State.m_SelectedEyeEmote == Emote ? 64.0f : 48.0f;
			RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, Emote, vec2(-1, 0), ScreenCenter + Nudge);
		}

		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0, 0, 0, 0.3f);
		Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, 30.0f, 64);
		Graphics()->QuadsEnd();
	}
	RenderTools()->RenderCursor(ScreenCenter + State.m_SelectorMouse, 24.0f);
}

bool CEmoticon::IsActive() const
{
	return Selector().m_Active;
}

void CEmoticon::Emote(int Emoticon)
{
	Emote(Emoticon, Client()->NetworkSessionId(), Client()->ActiveConnection());
}

void CEmoticon::Emote(int Emoticon, CSessionId SessionId, int Conn)
{
	if(SessionId != Client()->NetworkSessionId() || SessionId != Client()->FocusedSessionId() || Conn < IClient::CONN_MAIN || Conn >= IClient::NUM_CONNS)
		return;

	CNetMsg_Cl_Emoticon Msg;
	Msg.m_Emoticon = Emoticon;
	Client()->SendPackMsg(Conn, &Msg, MSGFLAG_VITAL);

	if(g_Config.m_ClDummyCopyMoves)
	{
		CMsgPacker MsgDummy(NETMSGTYPE_CL_EMOTICON, false);
		MsgDummy.AddInt(Emoticon);
		Client()->SendMsg(Conn == IClient::CONN_MAIN ? IClient::CONN_DUMMY : IClient::CONN_MAIN, &MsgDummy, MSGFLAG_VITAL);
	}
}

void CEmoticon::EyeEmote(int Emote)
{
	EyeEmote(Emote, Client()->NetworkSessionId(), Client()->ActiveConnection());
}

void CEmoticon::EyeEmote(int Emote, CSessionId SessionId, int Conn)
{
	if(SessionId != Client()->NetworkSessionId() || SessionId != Client()->FocusedSessionId() || Conn < IClient::CONN_MAIN || Conn >= IClient::NUM_CONNS)
		return;

	char aBuf[32];
	switch(Emote)
	{
	case EMOTE_NORMAL:
		str_format(aBuf, sizeof(aBuf), "/emote normal %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_PAIN:
		str_format(aBuf, sizeof(aBuf), "/emote pain %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_HAPPY:
		str_format(aBuf, sizeof(aBuf), "/emote happy %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_SURPRISE:
		str_format(aBuf, sizeof(aBuf), "/emote surprise %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_ANGRY:
		str_format(aBuf, sizeof(aBuf), "/emote angry %d", g_Config.m_ClEyeDuration);
		break;
	case EMOTE_BLINK:
		str_format(aBuf, sizeof(aBuf), "/emote blink %d", g_Config.m_ClEyeDuration);
		break;
	}
	GameClient()->m_Chat.SendChat(0, aBuf, SessionId, CStreamId(Conn + 1));
}
