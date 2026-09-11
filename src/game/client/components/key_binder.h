/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_KEY_BINDER_H
#define GAME_CLIENT_COMPONENTS_KEY_BINDER_H

#include <game/client/component.h>
#include <game/client/components/binds.h>
#include <game/client/ui.h>

// component to fetch keypresses, override all other input
class CKeyBinder : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	bool OnInput(const IInput::CEvent &Event) override;

	class CKeyReaderResult
	{
	public:
		CBindSlot m_Bind;
		bool m_Aborted;
	};
	CKeyReaderResult DoKeyReader(CButtonContainer *pReaderButton, CButtonContainer *pClearButton, CUIElement *pLabelUiElement, const CUIRect *pRect, const CBindSlot &CurrentBind, bool Activate);
	bool IsActive() const;
	bool HasPendingKeyReader() const { return m_pKeyReaderId != nullptr; }
	bool AbortPendingKey();

private:
	const CButtonContainer *m_pKeyReaderId = nullptr;
	// Reading a key press belongs to the controls page, which neither of the
	// two programs that play a demo back has; both stand this component in -
	// see `key_binder_null.cpp`. A private field nobody reads is an error
	// where warnings are.
#if !defined(CONF_DEMO_RENDER_TOOL) && !defined(CONF_DEMO_VIEWER_TOOL)
	bool m_TakeKey = false;
#endif
	std::optional<CBindSlot> m_Key;
};

#endif
