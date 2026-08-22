/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "particles.h"

#include <base/dbg.h>
#include <base/math.h>

#include <engine/graphics.h>

#include <generated/client_data.h>

#include <game/client/game_view.h>
#include <game/client/gameclient.h>
#include <game/collision.h>

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

void CParticles::UpdatePhysics(CGameState::CParticleSystemState &State, const CCollision &Collision, float TimePassed)
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
			// One lookup for the whole body. Every field access used to index the
			// container again, and in a debug build that is an out-of-line checked
			// call each time - about twenty of them per particle per frame.
			CGameState::CParticle &Particle = State.m_vParticles[i];
			int Next = Particle.m_NextPart;
			Particle.m_Vel.y += Particle.m_Gravity * TimePassed;

			for(int f = 0; f < FrictionCount; f++) // apply friction
				Particle.m_Vel *= Particle.m_Friction;

			// move the point
			vec2 Vel = Particle.m_Vel * TimePassed;
			if(Particle.m_Collides)
			{
				Collision.MovePoint(&Particle.m_Pos, &Vel, random_float(0.1f, 1.0f), nullptr);
			}
			else
			{
				Particle.m_Pos += Vel;
			}
			Particle.m_Vel = Vel * (1.0f / TimePassed);

			Particle.m_Life += TimePassed;
			Particle.m_Rot += TimePassed * Particle.m_Rotspeed;

			// check particle death
			if(Particle.m_Life > Particle.m_LifeSpan)
			{
				// remove it from the group list
				if(Particle.m_PrevPart != -1)
					State.m_vParticles[Particle.m_PrevPart].m_NextPart = Particle.m_NextPart;
				else
					FirstPart = Particle.m_NextPart;

				if(Particle.m_NextPart != -1)
					State.m_vParticles[Particle.m_NextPart].m_PrevPart = Particle.m_PrevPart;

				// insert to the free list
				if(State.m_FirstFree != -1)
					State.m_vParticles[State.m_FirstFree].m_PrevPart = i;
				Particle.m_PrevPart = -1;
				Particle.m_NextPart = State.m_FirstFree;
				State.m_FirstFree = i;
				State.m_NumParticles--;
			}

			i = Next;
		}
	}
}

void CParticles::Update(const CPresentationContext &Context)
{
	if(!Context.m_Time.m_IsGameActive)
		return;

	CGameState::CParticleSystemState &State = Context.m_State.Particles();
	const CGameTickInfo &Time = Context.m_Time;
	if(State.m_TimeInitialized && Time.m_PresentationTime >= State.m_LastRenderTime && Time.m_PresentationTimeFrequency > 0)
	{
		const float TimePassed = (float)((Time.m_PresentationTime - State.m_LastRenderTime) / (double)Time.m_PresentationTimeFrequency) * Time.m_AnimationPlaybackSpeed;
		UpdatePhysics(State, *Context.m_Session.MapContext().Collision(), TimePassed);
	}
	State.m_LastRenderTime = Time.m_PresentationTime;
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

bool CParticles::ParticleIsVisibleOnScreen(const CRenderContext &Context, const vec2 &CurPos, float CurSize) const
{
	CScreenRect ScreenRect(Context.m_VisibleWorldRect.m_TopLeft, Context.m_VisibleWorldRect.m_BottomRight);

	// for simplicity assume the worst case rotation, that increases the bounding box around the particle by its diagonal
	const float SqrtOf2 = std::sqrt(2);
	CurSize = SqrtOf2 * CurSize;

	// always uses the mid of the particle
	float SizeHalf = CurSize / 2;
	ScreenRect.Expand(SizeHalf);

	return ScreenRect.Inside(CurPos);
}

void CParticles::RenderGroup(const CRenderContext &Context, int Group)
{
	const CGameState::CParticleSystemState &State = Context.m_State.Particles();
	auto ParticleAlpha = [&Context](const CParticle &Particle, float LifeFraction) {
		float Alpha = Particle.m_UseAlphaFading ? mix(Particle.m_StartAlpha, Particle.m_EndAlpha, LifeFraction) : Particle.m_Color.a;
		return Alpha * Context.AlphaForOwner(Particle.m_OwnerClientId, g_Config.m_ClShowOthersAlpha / 100.0f);
	};
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
			const CGameState::CParticle &Particle = State.m_vParticles[i];
			const float LifeFraction = Particle.m_Life / Particle.m_LifeSpan;
			const float Alpha = ParticleAlpha(Particle, LifeFraction);
			LastColor.r = Particle.m_Color.r;
			LastColor.g = Particle.m_Color.g;
			LastColor.b = Particle.m_Color.b;
			LastColor.a = Alpha;

			Graphics()->SetColor(
				Particle.m_Color.r,
				Particle.m_Color.g,
				Particle.m_Color.b,
				Alpha);

			LastQuadOffset = Particle.m_Spr;
		}

		while(i != -1)
		{
			const CGameState::CParticle &Particle = State.m_vParticles[i];
			int QuadOffset = Particle.m_Spr;
			float a = Particle.m_Life / Particle.m_LifeSpan;
			vec2 p = Particle.m_Pos;
			float Size = mix(Particle.m_StartSize, Particle.m_EndSize, a);
			const float Alpha = ParticleAlpha(Particle, a);

			// the current position, respecting the size, is inside the viewport, render it, else ignore
			if(ParticleIsVisibleOnScreen(Context, p, Size))
			{
				if((size_t)CurParticleRenderCount == GRAPHICS_MAX_PARTICLES_RENDER_COUNT || LastColor.r != Particle.m_Color.r || LastColor.g != Particle.m_Color.g || LastColor.b != Particle.m_Color.b || LastColor.a != Alpha || LastQuadOffset != QuadOffset)
				{
					dbg_assert(LastQuadOffset >= FirstParticleOffset, "Invalid particle offsets: %d < %d", LastQuadOffset, FirstParticleOffset);
					Graphics()->TextureSet(aParticles[LastQuadOffset - FirstParticleOffset]);
					Graphics()->RenderQuadContainerAsSpriteMultiple(ParticleQuadContainerIndex, LastQuadOffset - FirstParticleOffset, CurParticleRenderCount, s_aParticleRenderInfo);
					CurParticleRenderCount = 0;
					LastQuadOffset = QuadOffset;

					Graphics()->SetColor(
						Particle.m_Color.r,
						Particle.m_Color.g,
						Particle.m_Color.b,
						Alpha);

					LastColor.r = Particle.m_Color.r;
					LastColor.g = Particle.m_Color.g;
					LastColor.b = Particle.m_Color.b;
					LastColor.a = Alpha;
				}

				s_aParticleRenderInfo[CurParticleRenderCount].m_Pos[0] = p.x;
				s_aParticleRenderInfo[CurParticleRenderCount].m_Pos[1] = p.y;
				s_aParticleRenderInfo[CurParticleRenderCount].m_Scale = Size;
				s_aParticleRenderInfo[CurParticleRenderCount].m_Rotation = Particle.m_Rot;

				++CurParticleRenderCount;
			}

			i = Particle.m_NextPart;
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
			const CGameState::CParticle &Particle = State.m_vParticles[i];
			float a = Particle.m_Life / Particle.m_LifeSpan;
			vec2 p = Particle.m_Pos;
			float Size = mix(Particle.m_StartSize, Particle.m_EndSize, a);
			const float Alpha = ParticleAlpha(Particle, a);

			// the current position, respecting the size, is inside the viewport, render it, else ignore
			if(ParticleIsVisibleOnScreen(Context, p, Size))
			{
				Graphics()->TextureSet(aParticles[Particle.m_Spr - FirstParticleOffset]);
				Graphics()->QuadsBegin();

				Graphics()->QuadsSetRotation(Particle.m_Rot);

				Graphics()->SetColor(
					Particle.m_Color.r,
					Particle.m_Color.g,
					Particle.m_Color.b,
					Alpha);

				IGraphics::CQuadItem QuadItem(p.x, p.y, Size, Size);
				Graphics()->QuadsDraw(&QuadItem, 1);
				Graphics()->QuadsEnd();
			}

			i = Particle.m_NextPart;
		}
		Graphics()->WrapNormal();
	}
}
