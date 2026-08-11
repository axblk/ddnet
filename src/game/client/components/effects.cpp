/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "effects.h"

#include <engine/shared/config.h>

#include <generated/client_data.h>

#include <game/client/components/damageind.h>
#include <game/client/components/particles.h>
#include <game/client/components/sounds.h>
#include <game/client/game_view.h>
#include <game/client/gameclient.h>

void CEffects::AirJump(CGameState &State, vec2 Pos, int OwnerClientId, float Alpha, float Volume)
{
	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_AIRJUMP;
	p.m_Pos = Pos + vec2(-6.0f, 16.0f);
	p.m_Vel = vec2(0.0f, -200.0f);
	p.m_LifeSpan = 0.5f;
	p.m_StartSize = 48.0f;
	p.m_EndSize = 0.0f;
	p.m_Rot = random_angle();
	p.m_Rotspeed = pi * 2.0f;
	p.m_Gravity = 500.0f;
	p.m_Friction = 0.7f;
	p.m_Color.a = Alpha;
	p.m_StartAlpha = Alpha;
	p.m_OwnerClientId = OwnerClientId;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);

	p.m_Pos = Pos + vec2(6.0f, 16.0f);
	GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);

	if(g_Config.m_SndGame)
		GameClient()->m_Sounds.PlayAt(CSounds::CHN_WORLD, SOUND_PLAYER_AIRJUMP, Volume, Pos);
}

void CEffects::DamageIndicator(CGameState &State, vec2 Pos, vec2 Dir, int OwnerClientId, float Alpha)
{
	GameClient()->m_DamageInd.Create(State, Pos, Dir, OwnerClientId, Alpha);
}

void CEffects::PowerupShine(CGameState &State, vec2 Pos, vec2 Size, int OwnerClientId, float Alpha)
{
	if(!State.EffectClock().m_Add50hz)
		return;

	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_SLICE;
	p.m_Pos = Pos + vec2(random_float(-0.5f, 0.5f), random_float(-0.5f, 0.5f)) * Size;
	p.m_Vel = vec2(0.0f, 0.0f);
	p.m_LifeSpan = 0.5f;
	p.m_StartSize = 16.0f;
	p.m_EndSize = 0.0f;
	p.m_Rot = random_angle();
	p.m_Rotspeed = pi * 2.0f;
	p.m_Gravity = 500.0f;
	p.m_Friction = 0.9f;
	p.m_Color.a = Alpha;
	p.m_StartAlpha = Alpha;
	p.m_OwnerClientId = OwnerClientId;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
}

void CEffects::FreezingFlakes(CGameState &State, vec2 Pos, vec2 Size, int OwnerClientId, float Alpha)
{
	if(!State.EffectClock().m_Add5hz)
		return;

	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_SNOWFLAKE;
	p.m_Pos = Pos + vec2(random_float(-0.5f, 0.5f), random_float(-0.5f, 0.5f)) * Size;
	p.m_Vel = vec2(0.0f, 0.0f);
	p.m_LifeSpan = 1.5f;
	p.m_StartSize = random_float(0.5f, 1.5f) * 16.0f;
	p.m_EndSize = p.m_StartSize * 0.5f;
	p.m_UseAlphaFading = true;
	p.m_StartAlpha = 1.0f;
	p.m_EndAlpha = 0.0f;
	p.m_Rot = random_angle();
	p.m_Rotspeed = pi;
	p.m_Gravity = random_float(250.0f);
	p.m_Friction = 0.9f;
	p.m_Collides = false;
	p.m_Color.a = Alpha;
	p.m_StartAlpha = Alpha;
	p.m_OwnerClientId = OwnerClientId;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_EXTRA, p);
}

void CEffects::SparkleTrail(CGameState &State, vec2 Pos, int OwnerClientId, float Alpha)
{
	if(!State.EffectClock().m_Add50hz)
		return;

	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_SPARKLE;
	p.m_Pos = Pos + random_direction() * random_float(40.0f);
	p.m_Vel = vec2(0.0f, 0.0f);
	p.m_LifeSpan = 0.5f;
	p.m_StartSize = 0.0f;
	p.m_EndSize = random_float(20.0f, 30.0f);
	p.m_UseAlphaFading = true;
	p.m_StartAlpha = Alpha;
	p.m_EndAlpha = std::min(0.2f, Alpha);
	p.m_Collides = false;
	p.m_OwnerClientId = OwnerClientId;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_TRAIL_EXTRA, p);
}

void CEffects::SmokeTrail(CGameState &State, vec2 Pos, vec2 Vel, int OwnerClientId, float Alpha, float TimePassed)
{
	if(!State.EffectClock().m_Add50hz && TimePassed < 0.001f)
		return;

	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_SMOKE;
	p.m_Pos = Pos;
	p.m_Vel = Vel + random_direction() * 50.0f;
	p.m_LifeSpan = random_float(0.5f, 1.0f);
	p.m_StartSize = random_float(12.0f, 20.0f);
	p.m_EndSize = 0.0f;
	p.m_Friction = 0.7f;
	p.m_Gravity = random_float(-500.0f);
	p.m_Color.a = Alpha;
	p.m_StartAlpha = Alpha;
	p.m_OwnerClientId = OwnerClientId;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_PROJECTILE_TRAIL, p, TimePassed);
}

void CEffects::SkidTrail(CGameState &State, const CGameTickInfo &Time, vec2 Pos, vec2 Vel, int Direction, int OwnerClientId, float Alpha, float Volume, bool PlaySound)
{
	CGameState::CEffectClockState &EffectClock = State.EffectClock();
	if(EffectClock.m_Add100hz)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SMOKE;
		p.m_Pos = Pos + vec2(-Direction * 6.0f, 12.0f);
		p.m_Vel = vec2(-Direction * 100.0f * length(Vel), -50.0f) + random_direction() * 50.0f;
		p.m_LifeSpan = random_float(0.5f, 1.0f);
		p.m_StartSize = random_float(24.0f, 36.0f);
		p.m_EndSize = 0.0f;
		p.m_Friction = 0.7f;
		p.m_Gravity = random_float(-500.0f);
		p.m_Color = ColorRGBA(0.75f, 0.75f, 0.75f, Alpha);
		p.m_StartAlpha = Alpha;
		p.m_OwnerClientId = OwnerClientId;
		GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
	}
	if(PlaySound && g_Config.m_SndGame)
	{
		if(EffectClock.TrySkidSound(Time.m_PresentationTime, Time.m_PresentationTimeFrequency))
			GameClient()->m_Sounds.PlayAt(CSounds::CHN_WORLD, SOUND_PLAYER_SKID, Volume, Pos);
	}
}

void CEffects::BulletTrail(CGameState &State, vec2 Pos, int OwnerClientId, float Alpha, float TimePassed)
{
	if(!State.EffectClock().m_Add100hz && TimePassed < 0.001f)
		return;

	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_BALL;
	p.m_Pos = Pos;
	p.m_LifeSpan = random_float(0.25f, 0.5f);
	p.m_StartSize = 8.0f;
	p.m_EndSize = 0.0f;
	p.m_Friction = 0.7f;
	p.m_Color.a *= Alpha;
	p.m_StartAlpha = Alpha;
	p.m_OwnerClientId = OwnerClientId;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_PROJECTILE_TRAIL, p, TimePassed);
}

void CEffects::PlayerSpawn(CGameState &State, vec2 Pos, float Alpha, float Volume)
{
	for(int i = 0; i < 32; i++)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SHELL;
		p.m_Pos = Pos;
		p.m_Vel = random_direction() * (std::pow(random_float(), 3) * 600.0f);
		p.m_LifeSpan = random_float(0.3f, 0.6f);
		p.m_StartSize = random_float(64.0f, 96.0f);
		p.m_EndSize = 0.0f;
		p.m_Rot = random_angle();
		p.m_Rotspeed = random_float();
		p.m_Gravity = random_float(-400.0f);
		p.m_Friction = 0.7f;
		p.m_Color = ColorRGBA(0xb5 / 255.0f, 0x50 / 255.0f, 0xcb / 255.0f, Alpha);
		p.m_StartAlpha = Alpha;
		GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
	}
	if(g_Config.m_SndGame)
		GameClient()->m_Sounds.PlayAt(CSounds::CHN_WORLD, SOUND_PLAYER_SPAWN, Volume, Pos);
}

void CEffects::PlayerDeath(CGameState &State, vec2 Pos, int ClientId, float Alpha)
{
	ColorRGBA BloodColor(1.0f, 1.0f, 1.0f);

	if(ClientId >= 0)
	{
		// Use m_RenderInfo.m_CustomColoredSkin instead of m_UseCustomColor
		// m_UseCustomColor says if the player's skin has a custom color (value sent from the client side)

		// m_RenderInfo.m_CustomColoredSkin Defines if in the context of the game the color is being customized,
		// Using this value if the game is teams (red and blue), this value will be true even if the skin is with the normal color.
		// And will use the team body color to create player death effect instead of tee color
		if(GameClient()->Client()->IsSixup())
		{
			if(GameClient()->m_aClients[ClientId].m_RenderInfo.m_Sixup.m_aUseCustomColors[protocol7::SKINPART_BODY])
			{
				BloodColor = GameClient()->m_aClients[ClientId].m_RenderInfo.m_Sixup.m_aColors[protocol7::SKINPART_BODY];
			}
			else
			{
				BloodColor = GameClient()->m_aClients[ClientId].m_RenderInfo.m_Sixup.m_BloodColor;
			}
		}
		else
		{
			if(GameClient()->m_aClients[ClientId].m_RenderInfo.m_CustomColoredSkin)
			{
				BloodColor = GameClient()->m_aClients[ClientId].m_RenderInfo.m_ColorBody;
			}
			else
			{
				BloodColor = GameClient()->m_aClients[ClientId].m_RenderInfo.m_BloodColor;
			}
		}
	}

	for(int i = 0; i < 64; i++)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SPLAT01 + (rand() % 3);
		p.m_Pos = Pos;
		p.m_Vel = random_direction() * (random_float(0.1f, 1.1f) * 900.0f);
		p.m_LifeSpan = random_float(0.3f, 0.6f);
		p.m_StartSize = random_float(24.0f, 40.0f);
		p.m_EndSize = 0.0f;
		p.m_Rot = random_angle();
		p.m_Rotspeed = random_float(-0.5f, 0.5f) * pi;
		p.m_Gravity = 800.0f;
		p.m_Friction = 0.8f;
		p.m_Color = BloodColor.Multiply(random_float(0.75f, 1.0f)).WithAlpha(0.75f * Alpha);
		p.m_StartAlpha = Alpha;
		p.m_OwnerClientId = ClientId;
		GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
	}
}

void CEffects::Confetti(CGameState &State, vec2 Pos, float Alpha)
{
	ColorRGBA Red(1.0f, 0.4f, 0.4f);
	ColorRGBA Green(0.4f, 1.0f, 0.4f);
	ColorRGBA Blue(0.4f, 0.4f, 1.0f);
	ColorRGBA Yellow(1.0f, 1.0f, 0.4f);
	ColorRGBA Cyan(0.4f, 1.0f, 1.0f);
	ColorRGBA Magenta(1.0f, 0.4f, 1.0f);

	ColorRGBA aConfettiColors[] = {Red, Green, Blue, Yellow, Cyan, Magenta};

	// powerful confettis
	for(int i = 0; i < 32; i++)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SPLAT01 + (rand() % 3);
		p.m_Pos = Pos;
		p.m_Vel = direction(-0.5f * pi + random_float(-0.2f, 0.2f)) * random_float(0.01f, 1.0f) * 2000.0f;
		p.m_LifeSpan = random_float(1.0f, 1.2f);
		p.m_StartSize = random_float(12.0f, 24.0f);
		p.m_EndSize = 0.0f;
		p.m_Rot = random_angle();
		p.m_Rotspeed = random_float(-0.5f, 0.5f) * pi;
		p.m_Gravity = -700.0f;
		p.m_Friction = 0.6f;
		ColorRGBA c = aConfettiColors[(rand() % std::size(aConfettiColors))];
		p.m_Color = c.WithMultipliedAlpha(0.75f * Alpha);
		p.m_StartAlpha = Alpha;
		GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
	}

	// broader confettis
	for(int i = 0; i < 32; i++)
	{
		CParticle p;
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SPLAT01 + (rand() % 3);
		p.m_Pos = Pos;
		p.m_Vel = direction(-0.5f * pi + random_float(-0.8f, 0.8f)) * random_float(0.01f, 1.0f) * 1500.0f;
		p.m_LifeSpan = random_float(0.8f, 1.0f);
		p.m_StartSize = random_float(12.0f, 24.0f);
		p.m_EndSize = 0.0f;
		p.m_Rot = random_angle();
		p.m_Rotspeed = random_float(-0.5f, 0.5f) * pi;
		p.m_Gravity = -700.0f;
		p.m_Friction = 0.6f;
		ColorRGBA c = aConfettiColors[(rand() % std::size(aConfettiColors))];
		p.m_Color = c.WithMultipliedAlpha(0.75f * Alpha);
		p.m_StartAlpha = Alpha;
		GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
	}
}

void CEffects::Explosion(CGameState &State, vec2 Pos, float Alpha)
{
	// add the explosion
	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_EXPL01;
	p.m_Pos = Pos;
	p.m_LifeSpan = 0.4f;
	p.m_StartSize = 150.0f;
	p.m_EndSize = 0.0f;
	p.m_Rot = random_angle();
	p.m_Color.a = Alpha;
	p.m_StartAlpha = Alpha;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_EXPLOSIONS, p);

	// Nudge position slightly to edge of closest tile so the
	// smoke doesn't get stuck inside the tile.
	if(Collision()->CheckPoint(Pos))
	{
		const vec2 DistanceToTopLeft = Pos - vec2(round_truncate(Pos.x / 32), round_truncate(Pos.y / 32)) * 32;

		vec2 CheckOffset;
		CheckOffset.x = (DistanceToTopLeft.x > 16.0f ? 32.0f : -1.0f);
		CheckOffset.y = (DistanceToTopLeft.y > 16.0f ? 32.0f : -1.0f);
		CheckOffset -= DistanceToTopLeft;

		for(vec2 Mask : {vec2(1.0f, 0.0f), vec2(0.0f, 1.0f), vec2(1.0f, 1.0f)})
		{
			const vec2 NewPos = Pos + CheckOffset * Mask;
			if(!Collision()->CheckPoint(NewPos))
			{
				Pos = NewPos;
				break;
			}
		}
	}

	// add the smoke
	for(int i = 0; i < 24; i++)
	{
		p.SetDefault();
		p.m_Spr = SPRITE_PART_SMOKE;
		p.m_Pos = Pos;
		p.m_Vel = random_direction() * (random_float(1.0f, 1.2f) * 1000.0f);
		p.m_LifeSpan = random_float(0.5f, 0.9f);
		p.m_StartSize = random_float(32.0f, 40.0f);
		p.m_EndSize = 0.0f;
		p.m_Gravity = random_float(-800.0f);
		p.m_Friction = 0.4f;
		p.m_Color = ColorRGBA(1.0f, 1.0f, 1.0f).Multiply(random_float(0.5f, 0.75f)).WithAlpha(Alpha);
		p.m_StartAlpha = p.m_Color.a;
		GameClient()->m_Particles.Add(State, CParticles::GROUP_GENERAL, p);
	}
}

void CEffects::HammerHit(CGameState &State, vec2 Pos, float Alpha, float Volume)
{
	// add the explosion
	CParticle p;
	p.SetDefault();
	p.m_Spr = SPRITE_PART_HIT01;
	p.m_Pos = Pos;
	p.m_LifeSpan = 0.3f;
	p.m_StartSize = 120.0f;
	p.m_EndSize = 0.0f;
	p.m_Rot = random_angle();
	p.m_Color.a = Alpha;
	p.m_StartAlpha = Alpha;
	GameClient()->m_Particles.Add(State, CParticles::GROUP_EXPLOSIONS, p);
	if(g_Config.m_SndGame)
		GameClient()->m_Sounds.PlayAt(CSounds::CHN_WORLD, SOUND_HAMMER_HIT, Volume, Pos);
}

void CEffects::Update(const CPresentationContext &Context)
{
	CGameState &State = Context.m_State;
	const CGameTickInfo &Time = Context.m_Time;
	State.EffectClock().Update(Time.m_PresentationTime, Time.m_PresentationTimeFrequency, Time.m_AnimationPlaybackSpeed);
	if(Time.m_IsGameActive)
		State.SceneClock().Update(Time.m_PresentationTime, Time.m_PresentationTimeFrequency, Time.m_AnimationPlaybackSpeed, Time.m_GameTickTime, Time.m_PredIntraGameTick);
}
