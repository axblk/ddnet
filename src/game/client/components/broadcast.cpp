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

void CBroadcast::ClearLayout(CLayout &Layout)
{
	Layout.m_RenderOffset = -1.0f;
	TextRender()->DeleteTextContainer(Layout.m_TextContainerIndex);
	Layout.m_Revision = 0;
}

void CBroadcast::InvalidateRenderCache()
{
	m_Layouts.ClearAll([this](CLayout &Layout) { ClearLayout(Layout); });
}

void CBroadcast::OnRender(const CRenderContext &Context)
{
	RenderServerBroadcast(Context);
}

void CBroadcast::RenderServerBroadcast(const CRenderContext &Context)
{
	const CGameSessionContext &Session = Context.m_Session;
	const CSessionBroadcastState &Broadcast = Session.Broadcast();
	CLayout &Layout = m_Layouts.Find(Context.LayoutKey(false), [this](CLayout &Old) { ClearLayout(Old); });
	if(Layout.m_Revision != Broadcast.Revision())
	{
		ClearLayout(Layout);
		Layout.m_Revision = Broadcast.Revision();
	}

	if(GameClient()->m_Scoreboard.IsActive() ||
		Context.m_View.Motd().IsActive(Session.Id(), Session.Motd().Revision(), time()) ||
		GameClient()->m_ImportantAlert.IsActive() ||
		!g_Config.m_ClShowBroadcasts)
	{
		return;
	}

	const int GameTick = Context.m_Time.m_GameTick;
	if(!Broadcast.IsActiveAt(GameTick))
	{
		TextRender()->DeleteTextContainer(Layout.m_TextContainerIndex);
		return;
	}
	const float SecondsRemaining = (Broadcast.ExpireTick() - GameTick) / (float)Context.m_Time.m_GameTickSpeed;

	const float Height = 300.0f;
	const float Width = Height * Context.AspectRatio(Graphics()->ScreenAspect());
	Graphics()->MapScreenToSize(Width, Height);

	if(Layout.m_RenderOffset < 0.0f)
		Layout.m_RenderOffset = Width / 2.0f - TextRender()->TextWidth(12.0f, Broadcast.Text(), -1, Width) / 2.0f;

	if(!Layout.m_TextContainerIndex.Valid())
	{
		CTextCursor Cursor;
		Cursor.SetPosition(vec2(Layout.m_RenderOffset, 40.0f));
		Cursor.m_FontSize = 12.0f;
		Cursor.m_LineWidth = Width;
		TextRender()->CreateTextContainer(Layout.m_TextContainerIndex, &Cursor, Broadcast.Text());
	}
	if(Layout.m_TextContainerIndex.Valid())
	{
		const float Alpha = SecondsRemaining >= 1.0f ? 1.0f : SecondsRemaining;
		ColorRGBA TextColor = TextRender()->DefaultTextColor();
		TextColor.a *= Alpha;
		ColorRGBA OutlineColor = TextRender()->DefaultTextOutlineColor();
		OutlineColor.a *= Alpha;
		TextRender()->RenderTextContainer(Layout.m_TextContainerIndex, TextColor, OutlineColor);
	}
}

void CBroadcast::DoBroadcast(CSessionBroadcastState &Broadcast, const char *pText, int GameTick, int GameTickSpeed)
{
	Broadcast.Apply(pText, GameTick, GameTickSpeed);
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
