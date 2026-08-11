/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "players.h"

#include <engine/shared/config.h>

#include <game/client/animstate.h>
#include <game/client/game_view.h>

void CPlayers::RenderSpectatorCharacters(const CRenderContext &Context, const CScreenRect &ScreenRect) const
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CGameState::CClientSnapshot &Client = Context.m_State.Client(ClientId);
		if(!Client.m_HasSpecChar)
			continue;
		const vec2 Position(Client.m_SpecChar.m_X, Client.m_SpecChar.m_Y);
		if(!ScreenRect.Inside(Position))
			continue;
		const float Alpha = Context.IsOtherTeam(ClientId) ? g_Config.m_ClShowOthersAlpha / 100.0f : 1.0f;
		RenderTools()->RenderTee(CAnimState::GetIdle(), &SpectatorTeeRenderInfo()->TeeRenderInfo(), EMOTE_BLINK, vec2(1.0f, 0.0f), Position, Alpha);
	}
}
