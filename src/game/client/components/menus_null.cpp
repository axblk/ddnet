/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "menus.h"

#include <engine/shared/config.h>
#include <engine/shared/video.h>

// The menus of a program that has none, linked instead of the menu pages: the
// programs that only show a demo never open a page, and the game still asks
// the menus whether they are open and has them draw its loading screen.

CMenus::CMenus()
{
	m_MenuActive = false;
	m_Popup = POPUP_NONE;
	m_NeedSendinfo = false;
	m_NeedSendDummyinfo = false;
}

void CMenus::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
}

void CMenus::OnInit()
{
	m_IsInit = true;
}

void CMenus::OnUpdate() {}
void CMenus::OnStateChange(int NewState, int OldState) {}
void CMenus::OnWindowResize() {}
void CMenus::OnShutdown() {}
void CMenus::OnRenderApplicationOverlay() {}
bool CMenus::OnInput(const IInput::CEvent &Event) { return false; }
bool CMenus::OnCursorMove(float x, float y, IInput::ECursorType CursorType) { return false; }

void CMenus::SetActive(bool Active) {}
bool CMenus::StartupAssetsLoaded() const { return true; }
void CMenus::RenderLoadingDirect(const char *pCaption, const char *pContent, std::optional<float> Progress, bool UpdateAndSwap) {}
void CMenus::RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter, bool UpdateAndSwap) {}

void CMenus::FinishLoading()
{
	m_LoadingState.m_Current = 0;
	m_LoadingState.m_Total = 0;
}

#if defined(CONF_VIDEORECORDER)
bool CMenus::RenderVideoProgress(bool Overlay)
{
	return false;
}
#endif

// Without the menus there is no blurred picture behind the boxes of the game.
bool CMenus::BeginMenuBackdrop(ColorRGBA ClearColor) { return false; }
void CMenus::FinishMenuBackdrop() {}
bool CMenus::CaptureMenuBackdrop() { return false; }
void CMenus::PresentMenuBackdrop() {}
void CMenus::RenderBackdropRegion(const CUIRect &Rect, int Corners, float Rounding) {}

void CMenus::DrawSurface(const CUIRect &Rect, ColorRGBA Color, int Corners, float Rounding)
{
	Rect.Draw(Color, Corners, Rounding);
}

void CMenus::GhostlistPopulate() {}
CMenus::CGhostItem *CMenus::GetOwnGhost() { return nullptr; }
void CMenus::UpdateOwnGhost(CGhostItem Item) {}
void CMenus::OnGhostLoadFailed(int Slot) {}
bool CMenus::CanDisplayWarning() const { return false; }
void CMenus::PopupWarning(const char *pTopic, const char *pBody, const char *pButton, std::chrono::nanoseconds Duration) {}
void CMenus::DemoSeekTick(IDemoPlayer::ETickOffset TickOffset) {}
void CMenus::OpenDemos() {}
void CMenus::OpenStats() {}
void CMenus::ExportMatchStats(const CStoredMatch &Stored, bool Csv) {}
void CMenus::ForceRefreshLanPage() {}
void CMenus::Connect(const char *pAddress) {}

// The two members of the menus whose bodies are not in a header.
CMenusIngameTouchControls::CBehaviorElements::~CBehaviorElements() = default;

void CMenusSettingsControls::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
}
