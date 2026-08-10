/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "particles.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/time.h>

#include <engine/demo.h>
#include <engine/graphics.h>

#include <generated/client_data.h>

#include <game/client/gameclient.h>

CParticles::CParticles()
{
	static_assert(NUM_GROUPS == CGameState::CParticleSystemState::NUM_GROUPS);
	m_RenderTrail.m_pParts = this;
	m_RenderTrailExtra.m_pParts = this;
	m_RenderExplosions.m_pParts = this;
	m_RenderExtra.m_pParts = this;
	m_RenderGeneral.m_pParts = this;
}

void CParticles::Add(CGameState &State, int Group, const CParticle &Particle, float TimePassed)
{
	if(State.Runtime().m_GameOver || State.Runtime().m_GamePaused || GameClient()->IsDemoPlaybackPaused())
		return;
	State.Particles().Add(Group, Particle, TimePassed);
}

void CParticles::UpdatePhysics(CGameState::CParticleSystemState &State, float TimePassed)
{
	if(TimePassed <= 0.0f)
		return;
	if(TimePassed > 2.0f)
	{
		State.Reset();
		return;
	}

	State.m_FrictionFraction += TimePassed;

	if(State.m_FrictionFraction > 2.0f) // safety measure
		State.m_FrictionFraction = 0;

	int FrictionCount = 0;
	while(State.m_FrictionFraction > 0.05f)
	{
		FrictionCount++;
		State.m_FrictionFraction -= 0.05f;
	}

	for(int &FirstPart : State.m_aFirstPart)
	{
		int i = FirstPart;
		while(i != -1)
		{
			int Next = State.m_vParticles[i].m_NextPart;
			State.m_vParticles[i].m_Vel.y += State.m_vParticles[i].m_Gravity * TimePassed;

			for(int f = 0; f < FrictionCount; f++) // apply friction
				State.m_vParticles[i].m_Vel *= State.m_vParticles[i].m_Friction;

			// move the point
			vec2 Vel = State.m_vParticles[i].m_Vel * TimePassed;
			if(State.m_vParticles[i].m_Collides)
			{
				Collision()->MovePoint(&State.m_vParticles[i].m_Pos, &Vel, random_float(0.1f, 1.0f), nullptr);
			}
			else
			{
				State.m_vParticles[i].m_Pos += Vel;
			}
			State.m_vParticles[i].m_Vel = Vel * (1.0f / TimePassed);

			State.m_vParticles[i].m_Life += TimePassed;
			State.m_vParticles[i].m_Rot += TimePassed * State.m_vParticles[i].m_Rotspeed;

			// check particle death
			if(State.m_vParticles[i].m_Life > State.m_vParticles[i].m_LifeSpan)
			{
				// remove it from the group list
				if(State.m_vParticles[i].m_PrevPart != -1)
					State.m_vParticles[State.m_vParticles[i].m_PrevPart].m_NextPart = State.m_vParticles[i].m_NextPart;
				else
					FirstPart = State.m_vParticles[i].m_NextPart;

				if(State.m_vParticles[i].m_NextPart != -1)
					State.m_vParticles[State.m_vParticles[i].m_NextPart].m_PrevPart = State.m_vParticles[i].m_PrevPart;

				// insert to the free list
				if(State.m_FirstFree != -1)
					State.m_vParticles[State.m_FirstFree].m_PrevPart = i;
				State.m_vParticles[i].m_PrevPart = -1;
				State.m_vParticles[i].m_NextPart = State.m_FirstFree;
				State.m_FirstFree = i;
				State.m_NumParticles--;
			}

			i = Next;
		}
	}
}

void CParticles::Update(CGameState &GameState)
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	set_new_tick();
	CGameState::CParticleSystemState &State = GameState.Particles();
	const int64_t Now = time();
	if(State.m_TimeInitialized)
		UpdatePhysics(State, (float)((Now - State.m_LastRenderTime) / (double)time_freq()) * GameClient()->GetAnimationPlaybackSpeed());
	State.m_LastRenderTime = Now;
	State.m_TimeInitialized = true;
}

void CParticles::OnInit()
{
	Graphics()->QuadsSetRotation(0);
	Graphics()->SetColor(1.f, 1.f, 1.f, 1.f);

	m_ParticleQuadContainerIndex = Graphics()->CreateQuadContainer(false);

	for(int i = 0; i <= (SPRITE_PART9 - SPRITE_PART_SLICE); ++i)
	{
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->QuadContainerAddSprite(m_ParticleQuadContainerIndex, 1.f);
	}
	Graphics()->QuadContainerUpload(m_ParticleQuadContainerIndex);

	m_ExtraParticleQuadContainerIndex = Graphics()->CreateQuadContainer(false);

	for(int i = 0; i <= (SPRITE_PART_SPARKLE - SPRITE_PART_SNOWFLAKE); ++i)
	{
		Graphics()->QuadsSetSubset(0, 0, 1, 1);
		Graphics()->QuadContainerAddSprite(m_ExtraParticleQuadContainerIndex, 1.f);
	}

	Graphics()->QuadContainerUpload(m_ExtraParticleQuadContainerIndex);
}

bool CParticles::ParticleIsVisibleOnScreen(const vec2 &CurPos, float CurSize) const
{
	CScreenRect ScreenRect = Graphics()->GetScreen();

	// for simplicity assume the worst case rotation, that increases the bounding box around the particle by its diagonal
	const float SqrtOf2 = std::sqrt(2);
	CurSize = SqrtOf2 * CurSize;

	// always uses the mid of the particle
	float SizeHalf = CurSize / 2;
	ScreenRect.Expand(SizeHalf);

	return ScreenRect.Inside(CurPos);
}

void CParticles::RenderGroup(int Group)
{
	const CGameState::CParticleSystemState &State = GameClient()->GameState(GameClient()->ActiveConnection()).Particles();
	IGraphics::CTextureHandle *aParticles = GameClient()->m_ParticlesSkin.m_aSpriteParticles;
	int FirstParticleOffset = SPRITE_PART_SLICE;
	int ParticleQuadContainerIndex = m_ParticleQuadContainerIndex;
	if(Group == GROUP_EXTRA || Group == GROUP_TRAIL_EXTRA)
	{
		aParticles = GameClient()->m_ExtrasSkin.m_aSpriteParticles;
		FirstParticleOffset = SPRITE_PART_SNOWFLAKE;
		ParticleQuadContainerIndex = m_ExtraParticleQuadContainerIndex;
	}

	// don't use the buffer methods here, else the old renderer gets many draw calls
	if(Graphics()->IsQuadContainerBufferingEnabled())
	{
		int i = State.m_aFirstPart[Group];

		static IGraphics::SRenderSpriteInfo s_aParticleRenderInfo[CGameState::CParticleSystemState::MAX_PARTICLES];

		int CurParticleRenderCount = 0;

		// batching makes sense for stuff like ninja particles
		ColorRGBA LastColor;
		int LastQuadOffset = 0;

		if(i != -1)
		{
			float Alpha = State.m_vParticles[i].m_Color.a;
			if(State.m_vParticles[i].m_UseAlphaFading)
			{
				float a = State.m_vParticles[i].m_Life / State.m_vParticles[i].m_LifeSpan;
				Alpha = mix(State.m_vParticles[i].m_StartAlpha, State.m_vParticles[i].m_EndAlpha, a);
			}
			LastColor.r = State.m_vParticles[i].m_Color.r;
			LastColor.g = State.m_vParticles[i].m_Color.g;
			LastColor.b = State.m_vParticles[i].m_Color.b;
			LastColor.a = Alpha;

			Graphics()->SetColor(
				State.m_vParticles[i].m_Color.r,
				State.m_vParticles[i].m_Color.g,
				State.m_vParticles[i].m_Color.b,
				Alpha);

			LastQuadOffset = State.m_vParticles[i].m_Spr;
		}

		while(i != -1)
		{
			int QuadOffset = State.m_vParticles[i].m_Spr;
			float a = State.m_vParticles[i].m_Life / State.m_vParticles[i].m_LifeSpan;
			vec2 p = State.m_vParticles[i].m_Pos;
			float Size = mix(State.m_vParticles[i].m_StartSize, State.m_vParticles[i].m_EndSize, a);
			float Alpha = State.m_vParticles[i].m_Color.a;
			if(State.m_vParticles[i].m_UseAlphaFading)
			{
				Alpha = mix(State.m_vParticles[i].m_StartAlpha, State.m_vParticles[i].m_EndAlpha, a);
			}

			// the current position, respecting the size, is inside the viewport, render it, else ignore
			if(ParticleIsVisibleOnScreen(p, Size))
			{
				if((size_t)CurParticleRenderCount == GRAPHICS_MAX_PARTICLES_RENDER_COUNT || LastColor.r != State.m_vParticles[i].m_Color.r || LastColor.g != State.m_vParticles[i].m_Color.g || LastColor.b != State.m_vParticles[i].m_Color.b || LastColor.a != Alpha || LastQuadOffset != QuadOffset)
				{
					dbg_assert(LastQuadOffset >= FirstParticleOffset, "Invalid particle offsets: %d < %d", LastQuadOffset, FirstParticleOffset);
					Graphics()->TextureSet(aParticles[LastQuadOffset - FirstParticleOffset]);
					Graphics()->RenderQuadContainerAsSpriteMultiple(ParticleQuadContainerIndex, LastQuadOffset - FirstParticleOffset, CurParticleRenderCount, s_aParticleRenderInfo);
					CurParticleRenderCount = 0;
					LastQuadOffset = QuadOffset;

					Graphics()->SetColor(
						State.m_vParticles[i].m_Color.r,
						State.m_vParticles[i].m_Color.g,
						State.m_vParticles[i].m_Color.b,
						Alpha);

					LastColor.r = State.m_vParticles[i].m_Color.r;
					LastColor.g = State.m_vParticles[i].m_Color.g;
					LastColor.b = State.m_vParticles[i].m_Color.b;
					LastColor.a = Alpha;
				}

				s_aParticleRenderInfo[CurParticleRenderCount].m_Pos[0] = p.x;
				s_aParticleRenderInfo[CurParticleRenderCount].m_Pos[1] = p.y;
				s_aParticleRenderInfo[CurParticleRenderCount].m_Scale = Size;
				s_aParticleRenderInfo[CurParticleRenderCount].m_Rotation = State.m_vParticles[i].m_Rot;

				++CurParticleRenderCount;
			}

			i = State.m_vParticles[i].m_NextPart;
		}

		if(CurParticleRenderCount > 0)
		{
			dbg_assert(LastQuadOffset >= FirstParticleOffset, "Invalid particle offsets: %d < %d", LastQuadOffset, FirstParticleOffset);
			Graphics()->TextureSet(aParticles[LastQuadOffset - FirstParticleOffset]);
			Graphics()->RenderQuadContainerAsSpriteMultiple(ParticleQuadContainerIndex, LastQuadOffset - FirstParticleOffset, CurParticleRenderCount, s_aParticleRenderInfo);
		}
	}
	else
	{
		int i = State.m_aFirstPart[Group];

		Graphics()->WrapClamp();

		while(i != -1)
		{
			float a = State.m_vParticles[i].m_Life / State.m_vParticles[i].m_LifeSpan;
			vec2 p = State.m_vParticles[i].m_Pos;
			float Size = mix(State.m_vParticles[i].m_StartSize, State.m_vParticles[i].m_EndSize, a);
			float Alpha = State.m_vParticles[i].m_Color.a;
			if(State.m_vParticles[i].m_UseAlphaFading)
			{
				Alpha = mix(State.m_vParticles[i].m_StartAlpha, State.m_vParticles[i].m_EndAlpha, a);
			}

			// the current position, respecting the size, is inside the viewport, render it, else ignore
			if(ParticleIsVisibleOnScreen(p, Size))
			{
				Graphics()->TextureSet(aParticles[State.m_vParticles[i].m_Spr - FirstParticleOffset]);
				Graphics()->QuadsBegin();

				Graphics()->QuadsSetRotation(State.m_vParticles[i].m_Rot);

				Graphics()->SetColor(
					State.m_vParticles[i].m_Color.r,
					State.m_vParticles[i].m_Color.g,
					State.m_vParticles[i].m_Color.b,
					Alpha);

				IGraphics::CQuadItem QuadItem(p.x, p.y, Size, Size);
				Graphics()->QuadsDraw(&QuadItem, 1);
				Graphics()->QuadsEnd();
			}

			i = State.m_vParticles[i].m_NextPart;
		}
		Graphics()->WrapNormal();
	}
}
