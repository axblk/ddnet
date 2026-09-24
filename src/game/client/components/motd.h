/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_MOTD_H
#define GAME_CLIENT_COMPONENTS_MOTD_H

#include <engine/client/session.h>
#include <engine/textrender.h>

#include <game/client/component.h>
#include <game/client/game_view.h>
#include <game/client/ui_rect.h>

#include <chrono>
#include <cstdint>
#include <optional>

class CGameSessionContext;
class CGameView;

class CMotd : public CComponent
{
	int m_RectQuadContainer = -1;
	STextContainerIndex m_TextContainerIndex;
	// A single layout, because the message is only ever shown in the view
	// that takes input.
	CLayoutKey m_RenderedLayoutKey;
	uint64_t m_RenderedRevision = 0;
	std::chrono::nanoseconds m_ShownSince{0}; // when the MOTD last became visible
	std::optional<CUIRect> m_TouchRect; // in normalized screen coordinates like the touch finger positions
	std::optional<IInput::CTouchFinger> m_DismissTouchFinger; // the finger that dismissed the MOTD, ignored until released

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
	bool OnTouchState(std::vector<IInput::CTouchFingerState> &vTouchFingerStates) override;
};

#endif
