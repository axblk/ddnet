/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_FRONTEND_H
#define GAME_CLIENT_COMPONENTS_FRONTEND_H

#include <engine/client.h>

#include <game/client/component.h>
#include <game/client/components/console.h>
#include <game/client/components/key_binder.h>
#include <game/client/components/local_server.h>
#include <game/client/components/menu_background.h>
#include <game/client/components/menus.h>
#include <game/client/frontend.h>
#include <game/client/match_journal.h>

#include <array>

/**
 * The front end of the client: the menus, the consoles, the key binder, the
 * theme behind the menus, the local server and the match history the stats
 * page shows. It sits on top of the game and reaches into it freely; the game
 * only sees `IGameFrontend`.
 */
class CGameFrontend : public IGameFrontend, public CComponent
{
	IClientFrontend *m_pClientFrontend = nullptr;
	std::vector<CComponentInfo> m_vComponents;
	std::array<std::vector<CComponent *>, (size_t)ESlot::NUM> m_avpSlots;

	static void ConchainMenuMap(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData);

public:
	CMenus m_Menus;
	CMenuBackground m_MenuBackground;
	CKeyBinder m_KeyBinder;
	CGameConsole m_GameConsole;
	CLocalServer m_LocalServer;
	CMatchJournal m_MatchJournal;

	CGameFrontend();
	int Sizeof() const override { return sizeof(*this); }

	/**
	 * What only the front end asks of the client.
	 */
	IClientFrontend *ClientFrontend() const { return m_pClientFrontend; }

	void OnInterfacesInit(CGameClient *pClient) override;
	void OnConsoleInit() override;
	void OnInit() override;
	void OnUpdate() override;
	void OnShutdown() override;

	const std::vector<CComponentInfo> &Components() const override { return m_vComponents; }
	const std::vector<CComponent *> &Slot(ESlot Slot) const override { return m_avpSlots[(size_t)Slot]; }

	bool MenuActive() const override { return m_Menus.IsActive(); }
	void SetMenuActive(bool Active) override { m_Menus.SetActive(Active); }
	bool ConsoleActive() const override { return m_GameConsole.IsActive(); }

	bool LoadingScreenReady() const override { return m_Menus.IsInit(); }
	bool StartupAssetsLoaded() const override { return m_Menus.StartupAssetsLoaded(); }
	void RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter, bool UpdateAndSwap) override;
	void RenderLoadingDirect(const char *pCaption, const char *pContent, std::optional<float> Progress, bool UpdateAndSwap) override;
	void FinishLoading() override { m_Menus.FinishLoading(); }
	void RenderSceneBackground() override { m_Menus.RenderSceneBackground(); }
#if defined(CONF_VIDEORECORDER)
	bool RenderVideoProgress(bool Overlay) override
	{
		return m_Menus.RenderVideoProgress(Overlay);
	}
#endif

	bool CanDisplayWarning() const override { return m_Menus.CanDisplayWarning(); }
	void PopupWarning(const char *pTopic, const char *pBody, const char *pButton, std::chrono::nanoseconds Duration) override;

	void OpenStats() override { m_Menus.OpenStats(); }
	void OpenDemos() override { m_Menus.OpenDemos(); }
	void ExportMatchStats(const CStoredMatch &Stored, bool Csv) override { m_Menus.ExportMatchStats(Stored, Csv); }
	void StoreMatch(const CStoredMatch &Match, const CStoredMatch *pReplacedObserved) override;

	void OnSessionConnected(CSessionId SessionId) override;
	void OnRconType(bool UsernameReq) override { m_GameConsole.RequireUsername(UsernameReq); }
	void OnRconLine(const char *pLine) override { m_GameConsole.PrintLine(CGameConsole::CONSOLETYPE_REMOTE, pLine); }
	void OnRconCommandsChanged() override { m_GameConsole.ForceUpdateRemoteCompletionSuggestions(); }
};

#endif
