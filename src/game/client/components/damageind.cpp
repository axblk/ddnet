/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "damageind.h"

#include <engine/graphics.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/game_state.h>
#include <game/client/game_view.h>
#include <game/client/gameclient.h>

void CDamageInd::Create(CGameState &State, vec2 Pos, vec2 Dir, int OwnerClientId, float Alpha)
{
	State.DamageIndicators().Create(Pos, Dir, OwnerClientId, Alpha, -random_angle());
}

void CDamageInd::Update(const CPresentationContext &Context)
{
	if(!Context.m_Time.m_IsGameActive)
		return;
	Context.m_State.DamageIndicators().Advance(Context.m_Time.m_PresentationTime, Context.m_Time.m_PresentationTimeFrequency, Context.m_Time.m_AnimationPlaybackSpeed);
}

void CDamageInd::OnRender(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive)
		return;

	const CGameState::CDamageIndicatorState &State = Context.m_State.DamageIndicators();
	Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteStars[0]);
	for(int i = 0; i < State.NumItems(); i++)
	{
		const CGameState::CDamageIndicatorState::CItem &Item = State.Item(i);
		vec2 Pos = mix(Item.m_Pos + Item.m_Dir * 75.0f, Item.m_Pos, std::clamp((Item.m_RemainingLife - 0.60f) / 0.15f, 0.0f, 1.0f));
		const float ObserverAlpha = Context.AlphaForOwner(Item.m_OwnerClientId, g_Config.m_ClShowOthersAlpha / 100.0f);
		const float LifeAlpha = Item.m_RemainingLife / 0.1f;
		Graphics()->SetColor(Item.m_Color.WithMultipliedAlpha(LifeAlpha * ObserverAlpha));
		Graphics()->QuadsSetRotation(Item.m_StartAngle + Item.m_RemainingLife * 2.0f);
		Graphics()->RenderQuadContainerAsSprite(m_DmgIndQuadContainerIndex, 0, Pos.x, Pos.y);
	}

	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);
}

void CDamageInd::OnInit()
{
	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	m_DmgIndQuadContainerIndex = Graphics()->CreateQuadContainer(false);
	float ScaleX, ScaleY;
	Graphics()->GetSpriteScale(SPRITE_STAR1, ScaleX, ScaleY);
	Graphics()->QuadsSetSubset(0, 0, 1, 1);
	Graphics()->QuadContainerAddSprite(m_DmgIndQuadContainerIndex, 48.f * ScaleX, 48.f * ScaleY);
	Graphics()->QuadContainerUpload(m_DmgIndQuadContainerIndex);
}
