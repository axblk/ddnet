/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_BROADCAST_H
#define GAME_CLIENT_COMPONENTS_BROADCAST_H

#include <engine/client/session.h>
#include <engine/textrender.h>

#include <game/client/component.h>
#include <game/client/game_view.h>

#include <cstdint>

class CSessionBroadcastState;

class CBroadcast : public CComponent
{
	class CLayout
	{
	public:
		float m_RenderOffset = -1.0f;
		STextContainerIndex m_TextContainerIndex;
		uint64_t m_Revision = 0;
	};
	CLayoutCache<CLayout> m_Layouts;

	void ClearLayout(CLayout &Layout);
	void InvalidateRenderCache();
	void RenderServerBroadcast(const CRenderContext &Context);

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnWindowResize() override;
	void OnRender(const CRenderContext &Context) override;

	void DoBroadcast(CSessionBroadcastState &Broadcast, const char *pText, int GameTick, int GameTickSpeed);
};

#endif
