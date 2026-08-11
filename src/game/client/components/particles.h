/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_PARTICLES_H
#define GAME_CLIENT_COMPONENTS_PARTICLES_H
#include <game/client/component.h>
#include <game/client/game_state.h>

using CParticle = CGameState::CParticle;

class CParticles : public CComponent
{
	friend class CGameClient;

public:
	enum
	{
		GROUP_PROJECTILE_TRAIL = 0,
		GROUP_TRAIL_EXTRA,
		GROUP_EXPLOSIONS,
		GROUP_EXTRA,
		GROUP_GENERAL,
		NUM_GROUPS
	};

	CParticles();
	int Sizeof() const override { return sizeof(*this); }

	void Add(CGameState &State, int Group, const CParticle &Particle, float TimePassed = 0.0f);
	void Update(CGameState &State);

	void OnInit() override;

private:
	int m_ParticleQuadContainerIndex = -1;
	int m_ExtraParticleQuadContainerIndex = -1;

	void RenderGroup(const CRenderContext &Context, int Group);
	void UpdatePhysics(CGameState::CParticleSystemState &State, float TimePassed);

	template<int TGROUP>
	class CRenderGroup : public CComponent
	{
	public:
		CParticles *m_pParts;
		int Sizeof() const override { return sizeof(*this); }
		void OnRender(const CRenderContext &Context) override { m_pParts->RenderGroup(Context, TGROUP); }
	};

	// behind players
	CRenderGroup<GROUP_PROJECTILE_TRAIL> m_RenderTrail;
	CRenderGroup<GROUP_TRAIL_EXTRA> m_RenderTrailExtra;
	// in front of players
	CRenderGroup<GROUP_EXPLOSIONS> m_RenderExplosions;
	CRenderGroup<GROUP_EXTRA> m_RenderExtra;
	CRenderGroup<GROUP_GENERAL> m_RenderGeneral;

	bool ParticleIsVisibleOnScreen(const vec2 &CurPos, float CurSize) const;
};
#endif
