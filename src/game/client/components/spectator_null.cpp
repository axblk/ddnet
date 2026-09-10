/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "spectator.h"

// The spectator menu is opened by a key, and a tool has no keyboard. Which tee
// a demo follows is what the demo says, not what somebody picks here.

void CSpectator::OnConsoleInit() {}

bool CSpectator::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	return false;
}

bool CSpectator::OnInput(const IInput::CEvent &Event)
{
	return false;
}

void CSpectator::UpdateController(CGameView &View, const CRenderContext &Context, float LocalTime) {}
void CSpectator::CommitController(CGameView &View, CSessionId SessionId, CGameStateId StateId, float LocalTime) {}
void CSpectator::OnRender(const CRenderContext &Context) {}
void CSpectator::OnRelease() {}
void CSpectator::OnReset() {}
void CSpectator::OnShutdown() {}
void CSpectator::OnWindowResize() {}

void CSpectator::Spectate(int SpectatorId) {}
void CSpectator::SpectateClosest() {}

bool CSpectator::IsActive() const
{
	return false;
}

#endif
