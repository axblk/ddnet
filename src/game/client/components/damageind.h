/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_DAMAGEIND_H
#define GAME_CLIENT_COMPONENTS_DAMAGEIND_H
#include <base/vmath.h>

#include <game/client/component.h>

class CGameState;
class CPresentationContext;

class CDamageInd : public CComponent
{
	int m_DmgIndQuadContainerIndex = -1;

public:
	int Sizeof() const override { return sizeof(*this); }

	void Create(CGameState &State, vec2 Pos, vec2 Dir, int OwnerClientId, float Alpha);
	void Update(const CPresentationContext &Context);
	void OnRender(const CRenderContext &Context) override;
	void OnInit() override;
};
#endif
