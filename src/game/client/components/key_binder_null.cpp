/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "key_binder.h"

// Reading a key press to bind it belongs to the controls page, which a tool
// does not have.

bool CKeyBinder::OnInput(const IInput::CEvent &Event)
{
	return false;
}

CKeyBinder::CKeyReaderResult CKeyBinder::DoKeyReader(CButtonContainer *pReaderButton, CButtonContainer *pClearButton, CUIElement *pLabelUiElement, const CUIRect *pRect, const CBindSlot &CurrentBind, bool Activate)
{
	return CKeyReaderResult{CurrentBind, true};
}

bool CKeyBinder::IsActive() const
{
	return false;
}

bool CKeyBinder::AbortPendingKey()
{
	return false;
}

#endif
