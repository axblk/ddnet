#ifndef GAME_CLIENT_COMPONENTS_STATBOARD_H
#define GAME_CLIENT_COMPONENTS_STATBOARD_H

#include <engine/console.h>

#include <game/client/component.h>
#include <game/client/ui.h>

#include <array>
#include <string>

class CGameState;

class CStatboard : public CComponent
{
private:
	bool m_Active;
	bool m_ScreenshotTaken;
	int64_t m_ScreenshotTime;
	class CPlayerText
	{
	public:
		CCachedText m_Name;
		std::array<CCachedText, 8> m_aStats;
		CCachedText m_Grabs;
		std::array<CCachedText, NUM_WEAPONS> m_aWeapons;
		CCachedText m_Captures;
	};
	std::array<CCachedText, 10> m_aHeaderTexts;
	std::array<CPlayerText, MAX_CLIENTS> m_aPlayerTexts;

	static void ConKeyStats(IConsole::IResult *pResult, void *pUserData);
	void ResetTexts();
	void RenderGlobalStats(const CRenderContext &Context);
	void RenderLiveMatch(const CRenderContext &Context, const class CStoredMatch &Live);
	// Bound on the rows the live panel draws, so a full server cannot push the
	// statboard off the screen.
	static constexpr int MAX_LIVE_ROWS = 16;
	float LiveMatchPanelHeight(const class CStoredMatch &Live) const;
	void RenderLiveMatchPanel(const class CStoredMatch &Live, float X, float Y, float Width);
	void AutoStatScreenshot();
	void AutoStatCSV();

	std::string ReplaceCommata(const char *pStr);
	void FormatStats(char *pDest, size_t DestSize);

public:
	CStatboard();
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnConsoleInit() override;
	void OnShutdown() override;
	void OnWindowResize() override;
	void UpdateController();
	void OnRender(const CRenderContext &Context) override;
	void OnRelease() override;
	bool IsActive() const;
	bool IsRenderable() const;
	bool IsRenderable(const CRenderContext &Context) const;
};

#endif // GAME_CLIENT_COMPONENTS_STATBOARD_H
