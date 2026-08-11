/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "motd.h"

#include <base/color.h>
#include <base/log.h>
#include <base/log_color.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <game/client/components/important_alert.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/statboard.h>
#include <game/client/gameclient.h>

#include <string>

const char *CMotd::ServerMotd() const
{
	return GameClient()->SessionContext().Motd().Text();
}

uint64_t CMotd::ServerMotdRevision() const
{
	return GameClient()->SessionContext().Motd().Revision();
}

void CMotd::Clear()
{
	GameClient()->LegacyGameView().Motd().Dismiss();
	InvalidateRenderCache();
}

void CMotd::InvalidateRenderCache()
{
	Graphics()->DeleteQuadContainer(m_RectQuadContainer);
	TextRender()->DeleteTextContainer(m_TextContainerIndex);
	m_RenderedSessionId = CSessionId();
	m_RenderedViewId = 0;
	m_RenderedViewportWidth = 0;
	m_RenderedViewportHeight = 0;
}

bool CMotd::IsActive() const
{
	const CGameSessionContext &Session = GameClient()->SessionContext();
	return GameClient()->LegacyGameView().Motd().IsActive(Session.Id(), Session.Motd().Revision(), time());
}

bool CMotd::IsActive(const CRenderContext &Context) const
{
	return Context.m_View.Motd().IsActive(Context.m_Session.Id(), Context.m_Session.Motd().Revision(), time());
}

void CMotd::OnStateChange(int NewState, int OldState)
{
	if(OldState == IClient::STATE_ONLINE || OldState == IClient::STATE_OFFLINE)
		Clear();
}

void CMotd::OnWindowResize()
{
	InvalidateRenderCache();
}

void CMotd::OnUpdate()
{
	if(IsActive() && (GameClient()->m_ImportantAlert.IsActive() || GameClient()->m_Scoreboard.IsActive() || GameClient()->m_Statboard.IsRenderable()))
		Clear();
}

void CMotd::OnRender(const CRenderContext &Context)
{
	if(!IsActive(Context))
		return;

	const CGameSessionContext &Session = Context.m_Session;
	const CViewport &Viewport = Context.m_View.Viewport();
	if(m_RenderedSessionId != Session.Id() || m_RenderedRevision != Session.Motd().Revision() ||
		m_RenderedViewId != Context.m_View.Id().Value() ||
		m_RenderedViewportWidth != Viewport.m_Width || m_RenderedViewportHeight != Viewport.m_Height)
	{
		InvalidateRenderCache();
		m_RenderedSessionId = Session.Id();
		m_RenderedRevision = Session.Motd().Revision();
		m_RenderedViewId = Context.m_View.Id().Value();
		m_RenderedViewportWidth = Viewport.m_Width;
		m_RenderedViewportHeight = Viewport.m_Height;
	}

	if(GameClient()->m_ImportantAlert.IsActive())
		return;

	const int MaxLines = 24;
	const float FontSize = 32.0f; // also the size of the margin and rect rounding
	const float ScreenHeight = 40.0f * FontSize; // multiple of the font size to get perfect alignment
	const float ScreenWidth = ScreenHeight * Context.AspectRatio(Graphics()->ScreenAspect());
	Graphics()->MapScreenToSize(ScreenWidth, ScreenHeight);

	const float RectHeight = (MaxLines + 2) * FontSize;
	const float RectWidth = 630.0f + 2.0f * FontSize;
	const float RectX = ScreenWidth / 2.0f - RectWidth / 2.0f;
	const float RectY = 160.0f;

	if(m_RectQuadContainer == -1)
	{
		Graphics()->SetColor(0.0f, 0.0f, 0.0f, 0.5f);
		m_RectQuadContainer = Graphics()->CreateRectQuadContainer(RectX, RectY, RectWidth, RectHeight, FontSize, IGraphics::CORNER_ALL);
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
	}

	if(m_RectQuadContainer != -1)
	{
		Graphics()->TextureClear();
		Graphics()->RenderQuadContainer(m_RectQuadContainer, -1);
	}

	if(!m_TextContainerIndex.Valid())
	{
		CTextCursor Cursor;
		Cursor.SetPosition(vec2(RectX + FontSize, RectY + FontSize));
		// TODO: Set TEXTFLAG_ELLIPSIS_AT_END when https://github.com/ddnet/ddnet/issues/11419 is fixed
		// Cursor.m_Flags |= TEXTFLAG_ELLIPSIS_AT_END;
		Cursor.m_FontSize = FontSize;
		Cursor.m_LineWidth = RectWidth - 2.0f * FontSize;
		Cursor.m_MaxLines = MaxLines;
		TextRender()->CreateTextContainer(m_TextContainerIndex, &Cursor, Session.Motd().Text());
	}

	if(m_TextContainerIndex.Valid())
		TextRender()->RenderTextContainer(m_TextContainerIndex, TextRender()->DefaultTextColor(), TextRender()->DefaultTextOutlineColor());
}

void CMotd::DoMotd(const char *pText)
{
	CGameSessionContext &Session = GameClient()->SessionContext();
	Session.Motd().Apply(pText);
	const int64_t Now = time();
	const int64_t VisibleUntil = Session.Motd().Text()[0] && g_Config.m_ClMotdTime ? Now + time_freq() * g_Config.m_ClMotdTime : 0;
	GameClient()->LegacyGameView().Motd().Show(Session.Id(), Session.Motd().Revision(), VisibleUntil);
	InvalidateRenderCache();

	if(g_Config.m_ClPrintMotd)
	{
		const LOG_COLOR LogColor = color_cast<LOG_COLOR>(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageHighlightColor)));
		const char *pLineStart = Session.Motd().Text();
		for(const char *pCursor = pLineStart;; ++pCursor)
		{
			if(*pCursor != '\n' && *pCursor != '\0')
				continue;

			if(*pCursor == '\n' || pCursor != pLineStart)
			{
				const std::string Line(pLineStart, pCursor);
				log_info_color(LogColor, "motd", "%s", Line.c_str());
			}

			if(*pCursor == '\0')
				break;
			pLineStart = pCursor + 1;
		}
	}
}

bool CMotd::OnInput(const IInput::CEvent &Event)
{
	if(IsActive() && Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		Clear();
		return true;
	}
	return false;
}
