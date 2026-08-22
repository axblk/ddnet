/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "menus.h"

#include <base/color.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/client.h>
#include <engine/graphics.h>
#include <engine/shared/video.h>

#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/ui.h>

#include <algorithm>

// The menus for a program that has none. A tool that turns one demo into one
// video file never opens a page, so the pages, their popups and everything they
// drag along stay out of its build and the rest of the client talks to these
// stand-ins instead. The backdrop the scoreboard and the message of the day
// draw over is in menus_backdrop.cpp, which both builds share.

CMenus::CMenus() = default;

void CMenus::OnInterfacesInit(CGameClient *pClient)
{
	CComponent::OnInterfacesInit(pClient);
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

bool CMenus::OnInput(const IInput::CEvent &Event)
{
	return false;
}

bool CMenus::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	return false;
}

void CMenus::SetActive(bool Active) {}
void CMenus::RenderMenuBackground() {}

void CMenus::RenderLoadingDirect(const char *pCaption, const char *pContent, std::optional<float> Progress, bool UpdateAndSwap) {}
void CMenus::RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter, bool UpdateAndSwap) {}

void CMenus::FinishLoading()
{
	m_LoadingState.m_Current = 0;
	m_LoadingState.m_Total = 0;
}

bool CMenus::StartupAssetsLoaded() const
{
	return true;
}

#if defined(CONF_VIDEORECORDER)
bool CMenus::RenderVideoProgress(bool Overlay)
{
	CVideoExportStatus Status;
	float Progress;
	float Elapsed;
	if(!VideoProgress(Status, Progress, Elapsed))
		return false;

	// No buttons: the tool has no input device, so the only way out is the
	// interrupt signal the entry point listens for.
	Graphics()->Clear(0.03f, 0.03f, 0.04f);
	Graphics()->TextureClear();
	Ui()->MapScreen();
	CUIRect Box, Row;
	Ui()->Screen()->Margin(160.0f, &Box);
	Box.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.65f), IGraphics::CORNER_ALL, 15.0f);
	Box.Margin(20.0f, &Box);
	Box.HSplitTop(30.0f, &Row, &Box);
	Ui()->DoLabel(&Row, "Rendering demo", 24.0f, TEXTALIGN_MC);
	Box.HSplitTop(20.0f, nullptr, &Box);
	Box.HSplitTop(25.0f, &Row, &Box);
	Ui()->RenderProgressBar(Row, Progress);
	Box.HSplitTop(15.0f, nullptr, &Box);
	Box.HSplitTop(22.0f, &Row, &Box);

	char aElapsed[32];
	str_time_float(Elapsed, ETimeFormat::HOURS, aElapsed, sizeof(aElapsed));
	char aStatus[128];
	if(Elapsed >= 1.0f && Progress > 0.01f)
	{
		char aEta[32];
		str_time_float(Elapsed * (1.0f - Progress) / Progress, ETimeFormat::HOURS, aEta, sizeof(aEta));
		str_format(aStatus, sizeof(aStatus), "%.1f%% — %llu frames — %s / %s", Progress * 100.0f, static_cast<unsigned long long>(Status.m_EncodedFrames), aElapsed, aEta);
	}
	else
	{
		str_format(aStatus, sizeof(aStatus), "%.1f%% — %llu frames — %s", Progress * 100.0f, static_cast<unsigned long long>(Status.m_EncodedFrames), aElapsed);
	}
	Ui()->DoLabel(&Row, aStatus, 14.0f, TEXTALIGN_MC);
	return false;
}
#endif

void CMenus::GhostlistPopulate() {}

CMenus::CGhostItem *CMenus::GetOwnGhost()
{
	return nullptr;
}

void CMenus::UpdateOwnGhost(CGhostItem Item) {}

void CMenus::OnGhostLoadFailed(int Slot) {}

bool CMenus::CanDisplayWarning() const
{
	return false;
}

void CMenus::PopupWarning(const char *pTopic, const char *pBody, const char *pButton, std::chrono::nanoseconds Duration) {}

void CMenus::DemoSeekTick(IDemoPlayer::ETickOffset TickOffset) {}

void CMenus::OpenDemos() {}
void CMenus::OpenStats() {}
void CMenus::ExportMatchStats(const CStoredMatch &Stored, bool Csv) {}
void CMenus::ForceRefreshLanPage() {}

// The two members of the menus whose bodies do not come with a header. Without
// them the compiler cannot destroy the touch control editor or lay out the
// controls page, even though neither is ever used here.
CMenusIngameTouchControls::CBehaviorElements::~CBehaviorElements() = default;

void CMenusSettingsControls::OnInterfacesInit(CGameClient *pClient)
{
	CComponentInterfaces::OnInterfacesInit(pClient);
}

#endif
