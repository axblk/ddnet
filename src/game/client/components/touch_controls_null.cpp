/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "touch_controls.h"

// On-screen controls are drawn for a finger to press, and a tool has neither.

void CTouchControls::OnInit() {}
void CTouchControls::OnReset() {}
void CTouchControls::OnWindowResize() {}
void CTouchControls::OnRender(const CRenderContext &Context) {}

bool CTouchControls::UpdateController(const CTouchControllerContext &Context, std::span<const IInput::CTouchFingerState> vTouchFingerStates, bool AcceptInput)
{
	return false;
}

void CTouchControls::RenderApplicationOverlay() {}

// Nothing here makes a button, but this is the first virtual of the behavior
// class that is neither pure nor inline, so it is what decides where the class
// puts its vtable and its type information. Left out, they are in no object
// file at all, and a compiler that refers to them - clang does under the
// sanitizers - has nothing to link against.
void CTouchControls::CTouchButtonBehavior::Init(CTouchButton *pTouchButton)
{
	m_pTouchButton = pTouchButton;
	m_pTouchControls = pTouchButton->m_pTouchControls;
}

#endif
