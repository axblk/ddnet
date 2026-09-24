/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_FRONTEND_H
#define GAME_CLIENT_FRONTEND_H

#include <engine/client/session.h>
#include <engine/kernel.h>

#include <game/client/ui_rect.h>

#include <chrono>
#include <optional>
#include <vector>

class CComponent;
class CStoredMatch;

/**
 * What the game knows of the front end the client puts on top of it: the
 * menus with the server browser, the settings, the start and the demo pages,
 * the consoles, the key binder and the local server. A program that only
 * shows the game has none, and the game runs the same without one.
 */
class IGameFrontend : public IInterface
{
	MACRO_INTERFACE("gamefrontend")
public:
	/**
	 * Where the components of the front end go among the game's own.
	 */
	enum class ESlot
	{
		// Takes input before anything else: the key binder.
		INPUT_FIRST,
		// Takes input before the chat: the console.
		INPUT_BEFORE_CHAT,
		// Takes input before the controls of the game: the menus.
		INPUT_BEFORE_CONTROLS,
		// Drawn last, under the tooltips: the menus.
		OVERLAY_BELOW_TOOLTIPS,
		// Drawn last, over the tooltips: the console.
		OVERLAY_ABOVE_TOOLTIPS,
		NUM,
	};

	class CComponentInfo
	{
	public:
		CComponent *m_pComponent;
		// What the render trace calls it, and its GPU zone if it has one.
		const char *m_pTraceName;
		const char *m_pGpuZone;
	};

	/**
	 * All components of the front end. The game adds them after its own, so
	 * they get the events of the game after it and are initialized first.
	 */
	virtual const std::vector<CComponentInfo> &Components() const = 0;
	virtual const std::vector<CComponent *> &Slot(ESlot Slot) const = 0;

	/**
	 * Whether the menus cover the game. The game leaves the keyboard and the
	 * mouse to them then.
	 */
	virtual bool MenuActive() const = 0;
	/**
	 * Opens or closes the menus.
	 */
	virtual void SetMenuActive(bool Active) = 0;
	/**
	 * Whether a console is open over the game or the menus.
	 */
	virtual bool ConsoleActive() const = 0;

	/**
	 * Whether the loading screen can be drawn yet.
	 */
	virtual bool LoadingScreenReady() const = 0;
	virtual bool StartupAssetsLoaded() const = 0;
	virtual void RenderLoading(const char *pCaption, const char *pContent, int IncreaseCounter, bool UpdateAndSwap) = 0;
	virtual void RenderLoadingDirect(const char *pCaption, const char *pContent, std::optional<float> Progress, bool UpdateAndSwap) = 0;
	virtual void FinishLoading() = 0;
	/**
	 * Draws what the menus show when there is no game, into the scene, so it
	 * gets blurred behind the menus like the game would.
	 */
	virtual void RenderSceneBackground() = 0;
	/**
	 * Where the menus want a demo that plays out of sight drawn over them, in
	 * interface units. Empty when they do not; taken once per frame.
	 */
	virtual CUIRect TakeDemoPreview() = 0;
#if defined(CONF_VIDEORECORDER)
	virtual bool RenderVideoProgress(bool Overlay) = 0;
#endif

	virtual bool CanDisplayWarning() const = 0;
	virtual void PopupWarning(const char *pTopic, const char *pBody, const char *pButton, std::chrono::nanoseconds Duration) = 0;

	// Where the buttons of the match report lead.
	virtual void OpenStats() = 0;
	virtual void OpenDemos() = 0;
	virtual void ExportMatchStats(const CStoredMatch &Stored, bool Csv) = 0;
	/**
	 * Keeps a finished match of the network session in the match history
	 * that the stats page shows.
	 *
	 * @param Match The match, it must have a local participant.
	 * @param pReplacedObserved What the client observed of the same round,
	 * dropped in favour of the server's report.
	 */
	virtual void StoreMatch(const CStoredMatch &Match, const CStoredMatch *pReplacedObserved) = 0;

	/**
	 * The game has entered a session.
	 */
	virtual void OnSessionConnected(CSessionId SessionId) = 0;
	// What the server's remote console sends.
	virtual void OnRconType(bool UsernameReq) = 0;
	virtual void OnRconLine(const char *pLine) = 0;
	virtual void OnRconCommandsChanged() = 0;
};

/**
 * Creates the front end of the client. The game finds it in the kernel.
 */
IGameFrontend *CreateGameFrontend();

#endif
