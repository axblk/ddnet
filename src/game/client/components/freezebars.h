#ifndef GAME_CLIENT_COMPONENTS_FREEZEBARS_H
#define GAME_CLIENT_COMPONENTS_FREEZEBARS_H
#include <game/client/component.h>

class CGameState;

class CFreezeBars : public CComponent
{
	void RenderFreezeBar(const CRenderContext &Context, int ClientId);
	void RenderFreezeBarPos(float x, float y, float Width, float Height, float Progress, float Alpha = 1.0f);
	bool IsPlayerInfoAvailable(const CGameState &State, int ClientId) const;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender(const CRenderContext &Context) override;
};

#endif
