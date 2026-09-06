/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_EMOTICON_H
#define GAME_CLIENT_COMPONENTS_EMOTICON_H
#include <base/vmath.h>

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/game_view.h>
#include <game/client/ui.h>

class CEmoticon : public CComponent
{
	CUi::CTouchState m_TouchState;
	CGameView::CEmoticonSelectorState &Selector();
	const CGameView::CEmoticonSelectorState &Selector() const;
	bool EyeWheelAvailable(const CRenderContext &Context) const;
	void Emote(int Emoticon, CSessionId SessionId, int Conn);
	void EyeEmote(int EyeEmote, CSessionId SessionId, int Conn);

	static void ConKeyEmoticon(IConsole::IResult *pResult, void *pUserData);
	static void ConEmote(IConsole::IResult *pResult, void *pUserData);

public:
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnConsoleInit() override;
	void UpdateController(CGameView &View, const CRenderContext &Context);
	void OnRender(const CRenderContext &Context) override;
	void OnRelease() override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	bool OnInput(const IInput::CEvent &Event) override;

	void Emote(int Emoticon);
	void EyeEmote(int EyeEmote);

	bool IsActive() const;
};

#endif
