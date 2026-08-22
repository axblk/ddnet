/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_SCOREBOARD_H
#define GAME_CLIENT_COMPONENTS_SCOREBOARD_H

#include <engine/console.h>
#include <engine/graphics.h>

#include <game/client/component.h>
#include <game/client/game_view.h>
#include <game/client/ui.h>
#include <game/client/ui_rect.h>

class CGameState;
class CRenderContext;

class CScoreboard : public CComponent
{
	struct CScoreboardRenderState
	{
		float m_TeamStartX;
		float m_TeamStartY;
		int m_CurrentDDTeamSize;

		CScoreboardRenderState() :
			m_TeamStartX(0), m_TeamStartY(0), m_CurrentDDTeamSize(0) {}
	};

	void RenderTitleScore(const CRenderContext &Context, CUIRect ScoreLabel, int Team, float TitleFontSize);
	void RenderTitle(const CRenderContext &Context, CUIRect TitleLabel, int Team, const char *pTitle, float TitleFontSize);
	void RenderTitleBar(const CRenderContext &Context, CUIRect TitleBar, int Team, const char *pTitle);
	void RenderGoals(const CRenderContext &Context, CUIRect Goals);
	void RenderSpectators(const CRenderContext &Context, CUIRect Spectators);
	void RenderScoreboard(const CRenderContext &Context, CUIRect Scoreboard, int Team, int CountStart, int CountEnd, CScoreboardRenderState &State, int NumPlayersForSize = -1);
	void RenderRecordingNotification(float x);
	bool UpdateApplicationOverlay(const CRenderContext &Context);

	static void ConKeyScoreboard(IConsole::IResult *pResult, void *pUserData);
	static void ConToggleScoreboardCursor(IConsole::IResult *pResult, void *pUserData);

	const char *GetTeamName(const CRenderContext &Context, int Team) const;

	bool m_Active;

	IGraphics::CTextureHandle m_DeadTeeTexture;

	std::optional<vec2> m_LastMousePos;
	bool m_MouseUnlocked = false;

	void SetUiMousePos(vec2 Pos);
	void LockMouse();

	class CScoreboardPopupContext : public SPopupMenuId
	{
	public:
		CScoreboard *m_pScoreboard = nullptr;
		CButtonContainer m_FriendAction;
		CButtonContainer m_MuteAction;
		CButtonContainer m_EmoticonAction;

		CButtonContainer m_SpectateButton;

		CSessionId m_SessionId;
		CGameStateId m_StateId;
		CGameViewId m_ViewId;
		int m_ClientId = -1;
		char m_aName[MAX_NAME_LENGTH] = {};
		char m_aClan[MAX_CLAN_LENGTH] = {};
		bool m_IsLocal = false;
		bool m_IsSpectating = false;

		void Bind(CScoreboard *pScoreboard, const CRenderContext &Context, int ClientId, const char *pName, const char *pClan, bool IsLocal, bool IsSpectating);

		static CUi::EPopupMenuFunctionResult Render(void *pContext, CUIRect View, bool Active);
	} m_ScoreboardPopupContext;

	class CMapTitlePopupContext : public SPopupMenuId
	{
	public:
		CScoreboard *m_pScoreboard = nullptr;

		float m_FontSize;
		char m_aDescription[512] = {};

		static CUi::EPopupMenuFunctionResult Render(void *pContext, CUIRect View, bool Active);
	} m_MapTitlePopupContext;
	char m_MapTitleButtonId;

	class CPlayerElement
	{
	public:
		char m_PlayerButtonId;
		char m_SpectatorSecondLineButtonId;

		CCachedText m_Score;
		CCachedText m_ScoreMillis;
		CCachedText m_Name;
		CCachedText m_ReadyMark;
		CCachedText m_Clan;
		CCachedText m_Ping;
	};
	CPlayerElement m_aPlayers[MAX_CLIENTS];

	struct CPlayerInteraction
	{
		CUIRect m_Rect;
		CUIRect m_SecondRect;
		CUIRect m_SkinRect;
		float m_RoundRadius = 0.0f;
		bool m_Active = false;
		bool m_HasSecondRect = false;
		bool m_HasSkinRect = false;
		bool m_IsSpectating = false;
	};

	struct CInteractionLayout
	{
		CSessionId m_SessionId;
		CGameStateId m_StateId;
		CGameViewId m_ViewId;
		std::array<CPlayerInteraction, MAX_CLIENTS> m_aPlayers;
		CUIRect m_MapTitleRect;
		bool m_Active = false;
		bool m_HasMapTitleRect = false;

		bool Matches(const CRenderContext &Context) const;
	};
	std::vector<CInteractionLayout> m_vInteractionLayouts;
	CInteractionLayout *m_pCurrentInteractionLayout = nullptr;
	CInteractionLayout *InteractionLayout(const CRenderContext &Context);
	CSessionId m_HighlightSessionId;
	CGameStateId m_HighlightStateId;
	CGameViewId m_HighlightViewId;
	int m_HighlightClientId = -1;
	bool m_HighlightMapTitle = false;
	bool m_ApplicationOverlayReady = false;
	bool IsHighlighted(const CRenderContext &Context, int ClientId) const;

	CCachedText m_TitleScore;
	CCachedText m_TitleScoreMillis;
	CCachedText m_HeadlineScore;
	CCachedText m_HeadlineName;
	CCachedText m_HeadlineClan;
	CCachedText m_HeadlinePing;

	void ResetTexts();

public:
	CScoreboard();
	int Sizeof() const override { return sizeof(*this); }
	void OnConsoleInit() override;
	void OnInit() override;
	void OnReset() override;
	void OnShutdown() override;
	void OnWindowResize() override;
	void OnRender(const CRenderContext &Context) override;
	void BeginRenderFrame();
	void PrepareApplicationOverlay(const CRenderContext &Context);
	void RenderApplicationOverlay(const CRenderContext &Context);
	void OnRelease() override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	bool OnInput(const IInput::CEvent &Event) override;

	bool IsActive() const;
	bool IsActive(const CRenderContext &Context) const;
};

#endif
