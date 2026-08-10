/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "broadcast.h"

#include <base/color.h>
#include <base/log.h>
#include <base/log_color.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/protocol.h>

#include <game/client/components/important_alert.h>
#include <game/client/components/motd.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>

void CBroadcast::OnReset()
{
	InvalidateRenderCache();
}

void CBroadcast::OnWindowResize()
{
	InvalidateRenderCache();
}

void CBroadcast::InvalidateRenderCache()
{
	m_BroadcastRenderOffset = -1.0f;
	TextRender()->DeleteTextContainer(m_TextContainerIndex);
	m_RenderedSessionId = CSessionId();
}

void CBroadcast::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	RenderServerBroadcast();
}

void CBroadcast::RenderServerBroadcast()
{
	const CGameSessionContext &Session = GameClient()->SessionContext();
	const CSessionBroadcastState &Broadcast = Session.Broadcast();
	if(m_RenderedSessionId != Session.Id() || m_RenderedRevision != Broadcast.Revision())
	{
		InvalidateRenderCache();
		m_RenderedSessionId = Session.Id();
		m_RenderedRevision = Broadcast.Revision();
	}

	if(GameClient()->m_Scoreboard.IsActive() ||
		GameClient()->m_Motd.IsActive() ||
		GameClient()->m_ImportantAlert.IsActive() ||
		!g_Config.m_ClShowBroadcasts)
	{
		return;
	}

	const int GameTick = Client()->GameTick(GameClient()->ActiveConnection());
	if(!Broadcast.IsActiveAt(GameTick))
	{
		TextRender()->DeleteTextContainer(m_TextContainerIndex);
		return;
	}
	const float SecondsRemaining = (Broadcast.ExpireTick() - GameTick) / (float)Client()->GameTickSpeed();

	const float Height = 300.0f;
	const float Width = Height * Graphics()->ScreenAspect();
	Graphics()->MapScreenToSize(Width, Height);

	if(m_BroadcastRenderOffset < 0.0f)
		m_BroadcastRenderOffset = Width / 2.0f - TextRender()->TextWidth(12.0f, Broadcast.Text(), -1, Width) / 2.0f;

	if(!m_TextContainerIndex.Valid())
	{
		CTextCursor Cursor;
		Cursor.SetPosition(vec2(m_BroadcastRenderOffset, 40.0f));
		Cursor.m_FontSize = 12.0f;
		Cursor.m_LineWidth = Width;
		TextRender()->CreateTextContainer(m_TextContainerIndex, &Cursor, Broadcast.Text());
	}
	if(m_TextContainerIndex.Valid())
	{
		const float Alpha = SecondsRemaining >= 1.0f ? 1.0f : SecondsRemaining;
		ColorRGBA TextColor = TextRender()->DefaultTextColor();
		TextColor.a *= Alpha;
		ColorRGBA OutlineColor = TextRender()->DefaultTextOutlineColor();
		OutlineColor.a *= Alpha;
		TextRender()->RenderTextContainer(m_TextContainerIndex, TextColor, OutlineColor);
	}
}

void CBroadcast::DoBroadcast(const char *pText, int Conn)
{
	GameClient()->SessionContext().Broadcast().Apply(pText, Client()->GameTick(Conn), Client()->GameTickSpeed());
	InvalidateRenderCache();

	if(g_Config.m_ClPrintBroadcasts)
	{
		const LOG_COLOR LogColor = color_cast<LOG_COLOR>(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageHighlightColor)));
		char aLine[CSessionBroadcastState::MAX_TEXT_LENGTH + 1];
		while((pText = str_next_token(pText, "\n", aLine, sizeof(aLine))))
		{
			if(aLine[0] != '\0')
			{
				log_info_color(LogColor, "broadcast", "%s", aLine);
			}
		}
	}
}
