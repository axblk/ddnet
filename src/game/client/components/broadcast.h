/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_BROADCAST_H
#define GAME_CLIENT_COMPONENTS_BROADCAST_H

#include <engine/client/session.h>
#include <engine/textrender.h>

#include <game/client/component.h>

#include <cstdint>

class CSessionBroadcastState;

class CBroadcast : public CComponent
{
	float m_BroadcastRenderOffset;
	STextContainerIndex m_TextContainerIndex;
	CSessionId m_RenderedSessionId;
	uint64_t m_RenderedRevision = 0;
	uint64_t m_RenderedViewId = 0;
	int m_RenderedViewportWidth = 0;
	int m_RenderedViewportHeight = 0;

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
