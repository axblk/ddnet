/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "frontend.h"

#include <base/log.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>

CGameFrontend::CGameFrontend()
{
	// In the order they had among the components of the game: the menus
	// right after the game's own, the theme behind them last.
	m_vComponents = {
		{&m_Menus, "ui/menus", "menus"},
		{&m_KeyBinder, "ui/key_binder", nullptr},
		{&m_GameConsole, "ui/console", "console"},
		{&m_MenuBackground, "ui/menu_background", nullptr},
		{this, "ui/frontend", nullptr},
	};
	m_avpSlots[(size_t)ESlot::INPUT_FIRST] = {&m_KeyBinder}; // this will take over all input when we want to bind a key
	m_avpSlots[(size_t)ESlot::INPUT_BEFORE_CHAT] = {&m_GameConsole};
	m_avpSlots[(size_t)ESlot::INPUT_BEFORE_CONTROLS] = {&m_Menus};
	m_avpSlots[(size_t)ESlot::OVERLAY_BELOW_TOOLTIPS] = {&m_Menus};
	m_avpSlots[(size_t)ESlot::OVERLAY_ABOVE_TOOLTIPS] = {&m_GameConsole};
}

CGameFrontend *CComponentInterfaces::Frontend() const
{
	return static_cast<CGameFrontend *>(GameClient()->Frontend());
}

IClientFrontend *CComponentInterfaces::ClientFrontend() const
{
	return Frontend()->ClientFrontend();
}

void CGameFrontend::OnInterfacesInit(CGameClient *pClient)
{
	CComponent::OnInterfacesInit(pClient);
	m_pClientFrontend = CComponent::Kernel()->RequestInterface<IClientFrontend>();
	m_LocalServer.OnInterfacesInit(pClient);
}

void CGameFrontend::OnConsoleInit()
{
	Console()->Chain("cl_menu_map", ConchainMenuMap, this);
}

void CGameFrontend::OnInit()
{
	std::string Error;
	if(!m_MatchJournal.Open(Storage(), &Error))
		log_error("match-journal", "%s", Error.c_str());
}

void CGameFrontend::StoreMatch(const CStoredMatch &Match, const CStoredMatch *pReplacedObserved)
{
	if(!g_Config.m_ClSaveMatchStats || !m_MatchJournal.IsOpen())
		return;
	std::string Error;
	if(m_MatchJournal.Insert(Match, pReplacedObserved, &Error) == CMatchJournal::EInsertResult::ERROR)
		log_error("match-journal", "%s", Error.c_str());
}

void CGameFrontend::OnUpdate()
{
	m_LocalServer.Update();
}

void CGameFrontend::OnShutdown()
{
	m_LocalServer.KillServer();
}

void CGameFrontend::RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter, bool UpdateAndSwap)
{
	m_Menus.RenderLoading(pCaption, pContent, IncreaseCounter, UpdateAndSwap);
}

void CGameFrontend::RenderLoadingDirect(const char *pCaption, const char *pContent, std::optional<float> Progress, bool UpdateAndSwap)
{
	m_Menus.RenderLoadingDirect(pCaption, pContent, Progress, UpdateAndSwap);
}

void CGameFrontend::PopupWarning(const char *pTopic, const char *pBody, const char *pButton, std::chrono::nanoseconds Duration)
{
	m_Menus.PopupWarning(pTopic, pBody, pButton, Duration);
}

void CGameFrontend::OnSessionConnected(CSessionId SessionId)
{
	if(SessionId == Sessions()->NetworkSessionId())
		m_LocalServer.RconAuthIfPossible();
}

void CGameFrontend::ConchainMenuMap(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	CGameFrontend *pSelf = static_cast<CGameFrontend *>(pUserData);
	if(pResult->NumArguments())
	{
		if(str_comp(g_Config.m_ClMenuMap, pResult->GetString(0)) != 0)
		{
			str_copy(g_Config.m_ClMenuMap, pResult->GetString(0));
			pSelf->m_MenuBackground.LoadMenuBackground();
		}
	}
	else
	{
		pfnCallback(pResult, pCallbackUserData);
	}
}

IGameFrontend *CreateGameFrontend()
{
	return new CGameFrontend();
}
