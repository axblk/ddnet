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

	if(!pSelf->GameClient()->m_Snap.m_SpecInfo.m_Active && pSelf->Client()->State() != IClient::STATE_DEMOPLAYBACK)
		pSelf->Selector().m_Active = pResult->GetInteger(0) != 0;
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

void CEmoticon::OnRender()
{
	CGameView::CEmoticonSelectorState &State = Selector();
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
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
			Emote(State.m_SelectedEmote);
		if(State.m_WasActive && State.m_SelectedEyeEmote != -1)
			EyeEmote(State.m_SelectedEyeEmote);
		State.m_WasActive = false;
		return;
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active || !GameClient()->m_Snap.m_pLocalCharacter)
	{
		State.m_Active = false;
		State.m_WasActive = false;
		return;
	}

	State.m_WasActive = true;

	const CUIRect Screen = *Ui()->Screen();

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

	if(length(State.m_SelectorMouse) > 170.0f)
		State.m_SelectorMouse = normalize(State.m_SelectorMouse) * 170.0f;

	float SelectedAngle = angle(State.m_SelectorMouse) + 2 * pi / 24;
	if(SelectedAngle < 0)
		SelectedAngle += 2 * pi;

	State.m_SelectedEmote = -1;
	State.m_SelectedEyeEmote = -1;
	if(length(State.m_SelectorMouse) > 110.0f)
		State.m_SelectedEmote = (int)(SelectedAngle / (2 * pi) * (float)NUM_EMOTICONS);
	else if(length(State.m_SelectorMouse) > 40.0f)
		State.m_SelectedEyeEmote = (int)(SelectedAngle / (2 * pi) * (float)NUM_EMOTES);

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

	if(GameClient()->FocusedGameInfo().m_AllowEyeWheel && g_Config.m_ClEyeWheel && GameClient()->GameState(GameClient()->ActiveConnection()).LocalClientId() >= 0)
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0, 1.0, 1.0, 0.3f);
		Graphics()->DrawCircle(ScreenCenter.x, ScreenCenter.y, 100.0f, 64);
		Graphics()->QuadsEnd();

		CTeeRenderInfo TeeInfo = GameClient()->m_aClients[GameClient()->GameState(GameClient()->ActiveConnection()).LocalClientId()].m_RenderInfo;

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
	else
		State.m_SelectedEyeEmote = -1;

	RenderTools()->RenderCursor(ScreenCenter + State.m_SelectorMouse, 24.0f);
}

bool CEmoticon::IsActive() const
{
	return Selector().m_Active;
}

void CEmoticon::Emote(int Emoticon)
{
	CNetMsg_Cl_Emoticon Msg;
	Msg.m_Emoticon = Emoticon;
	Client()->SendPackMsg(Client()->ActiveConnection(), &Msg, MSGFLAG_VITAL);

	if(g_Config.m_ClDummyCopyMoves)
	{
		CMsgPacker MsgDummy(NETMSGTYPE_CL_EMOTICON, false);
		MsgDummy.AddInt(Emoticon);
		Client()->SendMsg(GameClient()->OtherConnection(), &MsgDummy, MSGFLAG_VITAL);
	}
}

void CEmoticon::EyeEmote(int Emote)
{
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
	GameClient()->m_Chat.SendChat(0, aBuf);
}
