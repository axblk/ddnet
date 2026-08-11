/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_EFFECTS_H
#define GAME_CLIENT_COMPONENTS_EFFECTS_H

#include <base/vmath.h>

#include <game/client/component.h>

class CGameState;

class CEffects : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void Update(CGameState &State, float GameTickTime, float PredIntraTick);

	void BulletTrail(CGameState &State, vec2 Pos, float Alpha, float TimePassed);
	void SmokeTrail(CGameState &State, vec2 Pos, vec2 Vel, float Alpha, float TimePassed);
	void SkidTrail(CGameState &State, vec2 Pos, vec2 Vel, int Direction, float Alpha, float Volume, bool PlaySound);
	void Explosion(CGameState &State, vec2 Pos, float Alpha);
	void HammerHit(CGameState &State, vec2 Pos, float Alpha, float Volume);
	void AirJump(CGameState &State, vec2 Pos, float Alpha, float Volume);
	void DamageIndicator(CGameState &State, vec2 Pos, vec2 Dir, float Alpha);
	void PlayerSpawn(CGameState &State, vec2 Pos, float Alpha, float Volume);
	void PlayerDeath(CGameState &State, vec2 Pos, int ClientId, float Alpha);
	void PowerupShine(CGameState &State, vec2 Pos, vec2 Size, float Alpha);
	void FreezingFlakes(CGameState &State, vec2 Pos, vec2 Size, float Alpha);
	void SparkleTrail(CGameState &State, vec2 Pos, float Alpha);
	void Confetti(CGameState &State, vec2 Pos, float Alpha);
};
#endif
