/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_IMPORTANT_ALERT_H
#define GAME_CLIENT_COMPONENTS_IMPORTANT_ALERT_H

#include <engine/textrender.h>

#include <game/client/component.h>

#include <cstdint>
#include <optional>

class CImportantAlert : public CComponent
{
	bool m_Active = false;
	float m_ActiveSince;
	float m_FadeOutSince;
	char m_aTitleText[128];
	char m_aMessageText[1024];
	STextContainerIndex m_TitleTextContainerIndex;
	STextContainerIndex m_MessageTextContainerIndex;
	STextContainerIndex m_CloseHintTextContainerIndex;
	float m_TextContainerWidth = -1.0f;
	uint64_t m_TextContainerOutputKey = 0;

	void DeleteTextContainers();
	void RenderImportantAlert(const CRenderContext &Context);
	void DoImportantAlert(const char *pTitle, const char *pLogGroup, const char *pMessage);
	float SecondsActive() const;

public:
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnUpdate() override;
	void OnWindowResize() override;
	void OnRender(const CRenderContext &Context) override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnInput(const IInput::CEvent &Event) override;

	bool IsActive() const;
};

#endif
