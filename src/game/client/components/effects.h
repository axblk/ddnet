/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_EFFECTS_H
#define GAME_CLIENT_COMPONENTS_EFFECTS_H

#include <base/vmath.h>

#include <game/client/component.h>

class CCollision;
class CGameState;
class CGameTickInfo;
class CPresentationContext;
class CSessionId;

class CEffects : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }

	void Update(const CPresentationContext &Context);

	void BulletTrail(CGameState &State, vec2 Pos, int OwnerClientId, float Alpha, float TimePassed);
	void SmokeTrail(CGameState &State, vec2 Pos, vec2 Vel, int OwnerClientId, float Alpha, float TimePassed);
	void SkidTrail(CSessionId SessionId, CGameState &State, const CGameTickInfo &Time, vec2 Pos, vec2 Vel, int Direction, int OwnerClientId, float Alpha, float Volume, bool PlaySound);
	void Explosion(CGameState &State, const CCollision &Collision, vec2 Pos, float Alpha);
	void HammerHit(CSessionId SessionId, CGameState &State, vec2 Pos, float Alpha, float Volume);
	void AirJump(CSessionId SessionId, CGameState &State, vec2 Pos, int OwnerClientId, float Alpha, float Volume);
	void DamageIndicator(CGameState &State, vec2 Pos, vec2 Dir, int OwnerClientId, float Alpha);
	void PlayerSpawn(CSessionId SessionId, CGameState &State, vec2 Pos, float Alpha, float Volume);
	void PlayerDeath(CSessionId SessionId, CGameState &State, vec2 Pos, int ClientId, float Alpha);
	void PowerupShine(CGameState &State, vec2 Pos, vec2 Size, int OwnerClientId, float Alpha);
	void FreezingFlakes(CGameState &State, vec2 Pos, vec2 Size, int OwnerClientId, float Alpha);
	void SparkleTrail(CGameState &State, vec2 Pos, int OwnerClientId, float Alpha);
	void Confetti(CGameState &State, vec2 Pos, float Alpha);
};
#endif
