#ifndef GAME_CLIENT_COMPONENTS_STATBOARD_H
#define GAME_CLIENT_COMPONENTS_STATBOARD_H

#include <engine/console.h>

#include <game/client/component.h>

#include <string>

class CGameState;
class CSessionStatsState;

class CStatboard : public CComponent
{
private:
	bool m_Active;
	bool m_ScreenshotTaken;
	int64_t m_ScreenshotTime;
	static void ConKeyStats(IConsole::IResult *pResult, void *pUserData);
	void RenderGlobalStats(const CRenderContext &Context);
	void AutoStatScreenshot();
	void AutoStatCSV();

	std::string ReplaceCommata(const char *pStr);
	void FormatStats(char *pDest, size_t DestSize);

public:
	CStatboard();
	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnConsoleInit() override;
	void UpdateController();
	void HandleMessage(CSessionStatsState &Stats, const CGameState &State, bool SuppressEvents, int MsgType, void *pRawMsg);
	void OnRender(const CRenderContext &Context) override;
	void OnRelease() override;
	bool IsActive() const;
	bool IsRenderable() const;
	bool IsRenderable(const CRenderContext &Context) const;
};

#endif // GAME_CLIENT_COMPONENTS_STATBOARD_H
