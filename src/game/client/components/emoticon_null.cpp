/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL) || defined(CONF_DEMO_VIEWER_TOOL)

#include "emoticon.h"

// The emoticon wheel is there to send one, which only somebody playing on a
// server can do. Watching a demo there is nobody to send it as.

void CEmoticon::OnReset() {}
void CEmoticon::OnConsoleInit() {}
void CEmoticon::UpdateController(CGameView &View, const CRenderContext &Context) {}
void CEmoticon::OnRender(const CRenderContext &Context) {}
void CEmoticon::OnRelease() {}

bool CEmoticon::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	return false;
}

bool CEmoticon::OnInput(const IInput::CEvent &Event)
{
	return false;
}

void CEmoticon::Emote(int Emoticon) {}
void CEmoticon::EyeEmote(int EyeEmote) {}

bool CEmoticon::IsActive() const
{
	return false;
}

#endif
