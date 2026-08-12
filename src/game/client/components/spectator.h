/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SPECTATOR_H
#define GAME_CLIENT_COMPONENTS_SPECTATOR_H
#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/game_view.h>
#include <game/client/ui.h>

class CSpectator : public CComponent
{
	static constexpr int MULTI_VIEW = CGameView::CSpectatorSelectorState::MULTI_VIEW;
	static constexpr int NO_SELECTION = CGameView::CSpectatorSelectorState::NO_SELECTION;

	CUi::CTouchState m_TouchState;
	CGameView::CSpectatorSelectorState &Selector();
	const CGameView::CSpectatorSelectorState &Selector() const;
	void Spectate(CGameView &View, const CGameView::CSpectatorSelectorState &Selector, int SpectatorId);
	void ResetMultiView(CGameView &View);
	void ApplySelection(CGameView &View, CGameView::CSpectatorSelectorState &Selector, int SpectatorId, int DDTeam, bool Toggle, float Now);

	bool CanChangeSpectatorId();
	void SpectateNext(bool Reverse);

	static void ConKeySpectator(IConsole::IResult *pResult, void *pUserData);
	static void ConSpectate(IConsole::IResult *pResult, void *pUserData);
	static void ConSpectateNext(IConsole::IResult *pResult, void *pUserData);
	static void ConSpectatePrevious(IConsole::IResult *pResult, void *pUserData);
	static void ConSpectateClosest(IConsole::IResult *pResult, void *pUserData);
	static void ConMultiView(IConsole::IResult *pResult, void *pUserData);

public:
	int Sizeof() const override { return sizeof(*this); }

	void OnConsoleInit() override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	bool OnInput(const IInput::CEvent &Event) override;
	void UpdateController(CGameView &View, const CRenderContext &Context, float LocalTime);
	void CommitController(CGameView &View, const CRenderContext &Context, float LocalTime);
	void OnRender(const CRenderContext &Context) override;
	void OnRelease() override;
	void OnReset() override;

	void Spectate(int SpectatorId);
	void SpectateClosest();

	bool IsActive() const;
};

#endif
