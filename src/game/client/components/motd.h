/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MOTD_H
#define GAME_CLIENT_COMPONENTS_MOTD_H

#include <engine/client/session.h>
#include <engine/textrender.h>

#include <game/client/component.h>

#include <cstdint>

class CGameSessionContext;

class CMotd : public CComponent
{
	int m_RectQuadContainer = -1;
	STextContainerIndex m_TextContainerIndex;
	CSessionId m_RenderedSessionId;
	uint64_t m_RenderedRevision = 0;
	uint64_t m_RenderedViewId = 0;
	int m_RenderedViewportWidth = 0;
	int m_RenderedViewportHeight = 0;

	void InvalidateRenderCache();
	bool IsActive(const CRenderContext &Context) const;

public:
	int Sizeof() const override { return sizeof(*this); }

	const char *ServerMotd() const;
	uint64_t ServerMotdRevision() const;
	void Clear();
	void DoMotd(CGameSessionContext &Session, const char *pText, bool Show);
	bool IsActive() const;

	void OnUpdate() override;
	void OnRender(const CRenderContext &Context) override;
	void OnStateChange(int NewState, int OldState) override;
	void OnWindowResize() override;
	bool OnInput(const IInput::CEvent &Event) override;
};

#endif
