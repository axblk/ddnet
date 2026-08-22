/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "scoreboard.h"

#include <base/dbg.h>
#include <base/time.h>

#include <engine/console.h>
#include <engine/demo.h>
#include <engine/font_icons.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/localization.h>
#include <engine/textrender.h>

#include <generated/client_data7.h>
#include <generated/protocol.h>

#include <game/client/animstate.h>
#include <game/client/components/countryflags.h>
#include <game/client/components/motd.h>
#include <game/client/components/statboard.h>
#include <game/client/gameclient.h>
#include <game/client/match_report_view.h>
#include <game/client/ui.h>
#include <game/localization.h>

#include <algorithm>
#include <cinttypes>
#include <limits>

namespace
{
	class CReportAction
	{
	public:
		const char *m_pLabel;
		int m_Key;
	};
	// in the order of the actions
	const CReportAction gs_aReportActions[] = {
		{Localizable("History [H]"), KEY_H},
		{Localizable("Demos [D]"), KEY_D},
		{Localizable("Export CSV [C]"), KEY_C},
		{Localizable("Screenshot [S]"), KEY_S},
		{Localizable("Continue [Enter]"), KEY_RETURN},
	};
}

// Horizontal spacing of the scoreboard contents, both to its edges and between columns
static constexpr float MARGIN = 10.0f;
static constexpr const char *SCOREBOARD_CURSOR_BIND_NAME = "toggle_scoreboard_cursor";

void CScoreboard::CScoreboardPopupContext::Bind(CScoreboard *pScoreboard, const CRenderContext &Context, int ClientId, const char *pName, const char *pClan, bool IsLocal, bool IsSpectating)
{
	m_pScoreboard = pScoreboard;
	m_Binding = Context.m_View.Binding();
	m_ClientId = ClientId;
	str_copy(m_aName, pName);
	str_copy(m_aClan, pClan);
	m_IsLocal = IsLocal;
	m_IsSpectating = IsSpectating;
}

bool CScoreboard::CInteractionLayout::Matches(const CRenderContext &Context) const
{
	return m_Binding == Context.m_View.Binding() && m_Viewport == Context.m_View.Viewport();
}

bool CScoreboard::IsMatchReportDismissed(CSessionId SessionId, CUuid MatchId) const
{
	return std::any_of(m_vDismissedMatchReports.begin(), m_vDismissedMatchReports.end(), [SessionId, MatchId](const CDismissedMatchReport &Dismissed) {
		return Dismissed.m_SessionId == SessionId && Dismissed.m_MatchId == MatchId;
	});
}

void CScoreboard::DismissMatchReport(CSessionId SessionId, CUuid MatchId)
{
	const auto It = std::find_if(m_vDismissedMatchReports.begin(), m_vDismissedMatchReports.end(), [SessionId](const CDismissedMatchReport &Dismissed) { return Dismissed.m_SessionId == SessionId; });
	if(It != m_vDismissedMatchReports.end())
		It->m_MatchId = MatchId;
	else
		m_vDismissedMatchReports.push_back({SessionId, MatchId});

	// the next round has a new match id and shows its report again
	if(m_MouseUnlocked)
		LockMouse();
}

void CScoreboard::RunReportAction(int Action, CSessionId SessionId, CUuid MatchId)
{
	switch(Action)
	{
	case REPORT_ACTION_HISTORY:
		GameClient()->m_Menus.OpenStats();
		break;
	case REPORT_ACTION_DEMOS:
		GameClient()->m_Menus.OpenDemos();
		break;
	case REPORT_ACTION_CSV:
		if(const CGameSessionContext *pSession = GameClient()->FindSessionContext(SessionId); pSession && pSession->m_Stats.LatestMatch().has_value())
			GameClient()->m_Menus.ExportMatchStats(*pSession->m_Stats.LatestMatch(), true);
		break;
	case REPORT_ACTION_SCREENSHOT:
		Console()->ExecuteLine("screenshot", IConsole::CLIENT_ID_UNSPECIFIED);
		break;
	case REPORT_ACTION_CONTINUE:
		DismissMatchReport(SessionId, MatchId);
		break;
	}
}

bool CScoreboard::IsHighlighted(const CRenderContext &Context, int ClientId) const
{
	return m_HighlightBinding == Context.m_View.Binding() && m_HighlightClientId == ClientId;
}

CScoreboard::CScoreboard()
{
	CScoreboard::OnReset();
}

void CScoreboard::SetUiMousePos(vec2 Pos)
{
	const vec2 WindowSize = vec2(Graphics()->WindowWidth(), Graphics()->WindowHeight());
	const CUIRect *pScreen = Ui()->Screen();

	const vec2 UpdatedMousePos = Ui()->UpdatedMousePos();
	Pos = Pos / vec2(pScreen->w, pScreen->h) * WindowSize;
	Ui()->OnCursorMove(Pos.x - UpdatedMousePos.x, Pos.y - UpdatedMousePos.y);
}

void CScoreboard::LockMouse()
{
	Ui()->ClosePopupMenus();
	m_MouseUnlocked = false;
	SetUiMousePos(m_LastMousePos.value());
	m_LastMousePos = Ui()->MousePos();
}

void CScoreboard::ConKeyScoreboard(IConsole::IResult *pResult, void *pUserData)
{
	CScoreboard *pSelf = static_cast<CScoreboard *>(pUserData);

	pSelf->GameClient()->m_Spectator.OnRelease();
	pSelf->GameClient()->m_Emoticon.OnRelease();

	pSelf->m_Active = pResult->GetInteger(0) != 0;

	if(!pSelf->IsActive() && pSelf->m_MouseUnlocked)
	{
		pSelf->LockMouse();
	}
}

void CScoreboard::ConToggleScoreboardCursor(IConsole::IResult *pResult, void *pUserData)
{
	CScoreboard *pSelf = static_cast<CScoreboard *>(pUserData);

	if(!pSelf->IsActive() ||
		pSelf->GameClient()->m_Menus.IsActive() ||
		pSelf->GameClient()->m_Chat.IsActive() ||
		pSelf->Client()->State() == IClient::STATE_DEMOPLAYBACK)
	{
		return;
	}

	pSelf->m_MouseUnlocked = !pSelf->m_MouseUnlocked;

	if(!pSelf->m_MouseUnlocked)
	{
		pSelf->Ui()->ClosePopupMenus();
	}

	vec2 OldMousePos = pSelf->Ui()->MousePos();

	if(pSelf->m_LastMousePos == std::nullopt)
	{
		pSelf->SetUiMousePos(pSelf->Ui()->Screen()->Center());
	}
	else
	{
		pSelf->SetUiMousePos(pSelf->m_LastMousePos.value());
	}

	// save pos, so moving the mouse in esc menu doesn't change the position
	pSelf->m_LastMousePos = OldMousePos;
}

void CScoreboard::OnConsoleInit()
{
	Console()->Register("+scoreboard", "", CFGFLAG_CLIENT, ConKeyScoreboard, this, "Show scoreboard");
	Console()->Register(SCOREBOARD_CURSOR_BIND_NAME, "", CFGFLAG_CLIENT, ConToggleScoreboardCursor, this, "Toggle scoreboard cursor");
}

void CScoreboard::OnInit()
{
	m_DeadTeeResource = GameClient()->AssetLoader().LoadImageFile(Storage(), "deadtee.png", IStorage::TYPE_ALL);
}

void CScoreboard::OnUpdate()
{
	m_DeadTeeResource.FinishTexture(Graphics(), m_DeadTeeTexture);
}

void CScoreboard::OnReset()
{
	m_Active = false;
	m_MouseUnlocked = false;
	m_LastMousePos = std::nullopt;
	m_InteractionLayout = {};
	m_pCurrentInteractionLayout = nullptr;
	m_HighlightClientId = -1;
	m_HighlightMapTitle = false;
	m_HighlightReportAction = -1;
	m_ApplicationOverlayReady = false;
}

void CScoreboard::ResetTexts()
{
	for(CPlayerElement &Player : m_aPlayers)
	{
		Player.m_Score.Reset(TextRender());
		Player.m_ScoreMillis.Reset(TextRender());
		Player.m_Name.Reset(TextRender());
		Player.m_ReadyMark.Reset(TextRender());
		Player.m_Clan.Reset(TextRender());
		Player.m_Ping.Reset(TextRender());
	}
	m_TitleScore.Reset(TextRender());
	m_TitleScoreMillis.Reset(TextRender());
	m_HeadlineScore.Reset(TextRender());
	m_HeadlineName.Reset(TextRender());
	m_HeadlineClan.Reset(TextRender());
	m_HeadlinePing.Reset(TextRender());
}

void CScoreboard::OnShutdown()
{
	m_DeadTeeResource.Reset();
	Graphics()->UnloadTexture(&m_DeadTeeTexture);
	ResetTexts();
}

void CScoreboard::OnWindowResize()
{
	ResetTexts();
}

void CScoreboard::OnRelease()
{
	m_Active = false;

	if(m_MouseUnlocked)
	{
		LockMouse();
	}
}

bool CScoreboard::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(!IsActive() || !m_MouseUnlocked)
		return false;

	Ui()->ConvertMouseMove(&x, &y, CursorType);
	Ui()->OnCursorMove(x, y);

	return true;
}

bool CScoreboard::OnInput(const IInput::CEvent &Event)
{
	if((Event.m_Flags & IInput::FLAG_PRESS) && m_InteractionLayout.m_Active && m_InteractionLayout.m_ReportMatchId != UUID_ZEROED)
	{
		const int Key = Event.m_Key == KEY_KP_ENTER ? KEY_RETURN : Event.m_Key;
		const auto *const pAction = std::find_if(std::begin(gs_aReportActions), std::end(gs_aReportActions), [Key](const CReportAction &Action) { return Action.m_Key == Key; });
		if(pAction != std::end(gs_aReportActions))
		{
			RunReportAction(pAction - std::begin(gs_aReportActions), m_InteractionLayout.m_Binding.m_SessionId, m_InteractionLayout.m_ReportMatchId);
			// the menu page this opens reads the same letters from the frame's key state, a D would reach the delete shortcut of the demo list
			Input()->ClearFrameKey(Event.m_Key);
			return true;
		}
	}
	if(m_MouseUnlocked && Event.m_Key == KEY_ESCAPE && (Event.m_Flags & IInput::FLAG_PRESS))
	{
		LockMouse();
		return true;
	}

	return IsActive() && m_MouseUnlocked;
}

void CScoreboard::RenderTitle(const CRenderContext &Context, CUIRect TitleLabel, int Team, const char *pTitle, float TitleFontSize)
{
	const bool IsMapTitle = !Context.m_State.HasGameInfo() || (Context.m_State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) == 0;
	const char *pMapDescription = Context.m_Session.m_MapMetadata.Description();
	if(IsMapTitle && pMapDescription[0] != '\0')
	{
		m_pCurrentInteractionLayout->m_MapTitleRect = TitleLabel;
		m_pCurrentInteractionLayout->m_HasMapTitleRect = true;
		if(m_HighlightMapTitle && m_HighlightBinding == Context.m_View.Binding())
			TitleLabel.Draw(ColorRGBA(0.7f, 0.7f, 0.7f, 0.3f), IGraphics::CORNER_ALL, 5.0f);
	}

	SLabelProperties Props;
	Props.m_MaxWidth = TitleLabel.w;
	Props.m_EllipsisAtEnd = true;
	Ui()->DoLabel(&TitleLabel, pTitle, TitleFontSize, Team == TEAM_RED ? TEXTALIGN_ML : TEXTALIGN_MR, Props);
}

void CScoreboard::RenderTitleScore(const CRenderContext &Context, CUIRect ScoreLabel, int Team, float TitleFontSize)
{
	// map best
	char aScore[128] = "";
	const CGameState &GameState = Context.m_State;
	const CNetObj_GameInfo *pGameInfoObj = GameState.HasGameInfo() ? &GameState.GameInfo() : nullptr;
	const bool TimeScore = GameState.CoreGameInfo().m_TimeScore;
	const bool Race7 = Context.m_Session.Protocol() == EGameProtocol::SIXUP && pGameInfoObj && pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE;
	if(GameState.m_Runtime.m_ReceivedDDNetPlayerFinishTimes || TimeScore || Race7)
	{
		const CSessionMapMetadataState &MapMetadata = Context.m_Session.m_MapMetadata;
		if(MapMetadata.BestTimeSeconds() != FinishTime::UNSET)
		{
			Ui()->RenderTime(ScoreLabel,
				TitleFontSize,
				MapMetadata.BestTimeSeconds(),
				MapMetadata.BestTimeSeconds() == FinishTime::NOT_FINISHED_MILLIS,
				MapMetadata.BestTimeMillis(),
				GameState.m_Runtime.m_ReceivedDDNetPlayerFinishTimesMillis,
				m_TitleScore, m_TitleScoreMillis, TextRender()->DefaultTextColor());
			return;
		}
	}
	else if(pGameInfoObj && (pGameInfoObj->m_GameFlags & GAMEFLAG_TEAMS) != 0) // normal score
	{
		const CNetObj_GameData *pGameDataObj = GameState.GameData();
		if(pGameDataObj)
		{
			str_format(aScore, sizeof(aScore), "%d", Team == TEAM_RED ? pGameDataObj->m_TeamscoreRed : pGameDataObj->m_TeamscoreBlue);
		}
	}
	else
	{
		const int ScoreClientId = Context.m_View.IsSpectating() && Context.m_View.SpectatorId() != SPEC_FREEVIEW ? Context.m_View.SpectatorId() : GameState.LocalClientId();
		if(ScoreClientId >= 0 && ScoreClientId < MAX_CLIENTS && GameState.Client(ScoreClientId).m_HasPlayerInfo)
		{
			str_format(aScore, sizeof(aScore), "%d", GameState.Client(ScoreClientId).m_PlayerInfo.m_Score);
		}
	}

	const float ScoreTextWidth = aScore[0] != '\0' ? TextRender()->TextWidth(TitleFontSize, aScore, -1, -1.0f, 0) : 0.0f;
	if(ScoreTextWidth != 0.0f)
	{
		Ui()->DoLabel(&ScoreLabel, aScore, TitleFontSize, Team == TEAM_RED ? TEXTALIGN_MR : TEXTALIGN_ML);
	}
}

void CScoreboard::RenderTitleBar(const CRenderContext &Context, CUIRect TitleBar, int Team, const char *pTitle)
{
	dbg_assert(Team == TEAM_RED || Team == TEAM_BLUE, "Team invalid");

	const float TitleFontSize = 20.0f;
	const float ScoreTextWidth = TextRender()->TextWidth(TitleFontSize, "00:00:00");
	const float TitleTextWidth = TextRender()->TextWidth(TitleFontSize, pTitle);

	TitleBar.VMargin(MARGIN, &TitleBar);
	CUIRect TitleLabel, ScoreLabel;
	if(Team == TEAM_RED)
	{
		TitleBar.VSplitRight(ScoreTextWidth, &TitleLabel, &ScoreLabel);
		TitleLabel.VSplitRight(5.0f, &TitleLabel, nullptr);
		TitleLabel.VSplitLeft(std::min(TitleTextWidth + 2.0f, TitleLabel.w), &TitleLabel, nullptr);
	}
	else
	{
		TitleBar.VSplitLeft(ScoreTextWidth, &ScoreLabel, &TitleLabel);
		TitleLabel.VSplitLeft(5.0f, nullptr, &TitleLabel);
		TitleLabel.VSplitRight(std::min(TitleTextWidth + 2.0f, TitleLabel.w), nullptr, &TitleLabel);
	}

	RenderTitle(Context, TitleLabel, Team, pTitle, TitleFontSize);
	RenderTitleScore(Context, ScoreLabel, Team, TitleFontSize);
}

void CScoreboard::RenderGoals(const CRenderContext &Context, CUIRect Goals)
{
	GameClient()->m_Menus.DrawSurface(Goals, ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 7.5f);
	Goals.VMargin(5.0f, &Goals);

	const float FontSize = 10.0f;
	const CNetObj_GameInfo *pGameInfoObj = Context.m_State.HasGameInfo() ? &Context.m_State.GameInfo() : nullptr;
	if(pGameInfoObj == nullptr)
		return;
	char aBuf[64];

	if(pGameInfoObj->m_ScoreLimit)
	{
		str_format(aBuf, sizeof(aBuf), "%s: %d", Localize("Score limit"), pGameInfoObj->m_ScoreLimit);
		Ui()->DoLabel(&Goals, aBuf, FontSize, TEXTALIGN_ML);
	}

	if(pGameInfoObj->m_TimeLimit)
	{
		str_format(aBuf, sizeof(aBuf), Localize("Time limit: %d min"), pGameInfoObj->m_TimeLimit);
		Ui()->DoLabel(&Goals, aBuf, FontSize, TEXTALIGN_MC);
	}

	if(pGameInfoObj->m_RoundNum && pGameInfoObj->m_RoundCurrent)
	{
		str_format(aBuf, sizeof(aBuf), Localize("Round %d/%d"), pGameInfoObj->m_RoundCurrent, pGameInfoObj->m_RoundNum);
		Ui()->DoLabel(&Goals, aBuf, FontSize, TEXTALIGN_MR);
	}
}

void CScoreboard::RenderSpectators(const CRenderContext &Context, CUIRect Spectators)
{
	const CGameState &GameState = Context.m_State;
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> *pClientsByName = Presentation.ClientsByName(GameState.m_Conn);
	if(pClientsByName == nullptr)
		return;
	GameClient()->m_Menus.DrawSurface(Spectators, ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 7.5f);
	constexpr float SpectatorCut = 5.0f;
	Spectators.Margin(SpectatorCut, &Spectators);

	CTextCursor Cursor;
	Cursor.SetPosition(Spectators.TopLeft());
	Cursor.m_FontSize = 11.0f;
	Cursor.m_LineWidth = Spectators.w;
	Cursor.m_MaxLines = round_truncate(Spectators.h / Cursor.m_FontSize);

	int RemainingSpectators = 0;
	for(const int ClientId : *pClientsByName)
	{
		if(ClientId < 0)
			break;
		if(GameState.Client(ClientId).m_PlayerInfo.m_Team != TEAM_SPECTATORS)
			continue;
		++RemainingSpectators;
	}

	TextRender()->TextEx(&Cursor, Localize("Spectators"));

	if(RemainingSpectators > 0)
	{
		TextRender()->TextEx(&Cursor, ": ");
	}

	bool CommaNeeded = false;
	for(const int ClientId : *pClientsByName)
	{
		if(ClientId < 0)
			break;
		if(GameState.Client(ClientId).m_PlayerInfo.m_Team != TEAM_SPECTATORS)
			continue;
		const CClientPresentation *pClient = Presentation.Client(GameState.m_Conn, ClientId);
		if(pClient == nullptr || !pClient->m_Active)
			continue;

		if(CommaNeeded)
		{
			TextRender()->TextEx(&Cursor, ", ");
		}

		if(Cursor.m_LineCount == Cursor.m_MaxLines && RemainingSpectators >= 2)
		{
			// This is less expensive than checking with a separate invisible
			// text cursor though we waste some space at the end of the line.
			char aRemaining[64];
			str_format(aRemaining, sizeof(aRemaining), Localize("%d others…", "Spectators"), RemainingSpectators);
			TextRender()->TextEx(&Cursor, aRemaining);
			break;
		}

		CUIRect SpectatorRect, SpectatorRectLineBreak;
		const float Margin = 1.0f;
		SpectatorRect.x = Cursor.m_X - Margin;
		SpectatorRect.y = Cursor.m_Y;

		// id, clan and name of the spectator, advanced through the given cursor
		auto RenderEntryText = [&](CTextCursor *pCursor) {
			if(g_Config.m_ClShowIds)
			{
				char aClientId[16];
				GameClient()->FormatClientId(ClientId, aClientId, EClientIdFormat::NO_INDENT);
				TextRender()->TextEx(pCursor, aClientId);
			}

			{
				const char *pClanName = pClient->m_aClan;
				if(pClanName[0] != '\0')
				{
					const CClientPresentation *pLocalClient = Presentation.Client(GameState.m_Conn, GameState.LocalClientId());
					if(pLocalClient != nullptr && str_comp(pClanName, pLocalClient->m_aClan) == 0)
					{
						TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClSameClanColor)));
					}
					else
					{
						TextRender()->TextColor(ColorRGBA(0.7f, 0.7f, 0.7f));
					}

					TextRender()->TextEx(pCursor, pClanName);
					TextRender()->TextEx(pCursor, " ");

					TextRender()->TextColor(TextRender()->DefaultTextColor());
				}
			}

			const CGameState::CClientSnapshot &SnapshotClient = GameState.Client(ClientId);
			const int AuthLevel = SnapshotClient.m_HasDDNetPlayer ? SnapshotClient.m_DDNetPlayer.m_AuthLevel : 0;
			if(AuthLevel)
			{
				TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClAuthedPlayerColor)));
			}

			TextRender()->TextEx(pCursor, pClient->m_aName);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		};

		// measure the entry's extent without rendering, so the highlight can be drawn behind the text
		CTextCursor MeasureCursor = Cursor;
		MeasureCursor.m_Flags &= ~TEXTFLAG_RENDER;
		RenderEntryText(&MeasureCursor);

		bool LineBreakDetected = false;
		SpectatorRect.h = Cursor.m_FontSize;

		// detect line breaks
		if(MeasureCursor.m_Y != SpectatorRect.y)
		{
			LineBreakDetected = true;
			SpectatorRectLineBreak.x = Spectators.x - SpectatorCut;
			SpectatorRectLineBreak.y = MeasureCursor.m_Y;
			SpectatorRectLineBreak.h = Cursor.m_FontSize;
			SpectatorRectLineBreak.w = MeasureCursor.m_X - Spectators.x + SpectatorCut + 2 * Margin;

			SpectatorRect.w = Spectators.x + Spectators.w + SpectatorCut - SpectatorRect.x;
		}
		else
		{
			SpectatorRect.w = MeasureCursor.m_X - SpectatorRect.x + 2 * Margin;
		}

		CPlayerInteraction &Interaction = m_pCurrentInteractionLayout->m_aPlayers[ClientId];
		Interaction.m_Rect = SpectatorRect;
		Interaction.m_SecondRect = SpectatorRectLineBreak;
		Interaction.m_Active = true;
		Interaction.m_HasSecondRect = LineBreakDetected;
		Interaction.m_IsSpectating = true;
		if(IsHighlighted(Context, ClientId))
		{
			// draw the highlight behind the text
			if(!LineBreakDetected)
				SpectatorRect.Draw(TextRender()->DefaultTextSelectionColor(), IGraphics::CORNER_ALL, 2.5f);
			else
			{
				SpectatorRect.Draw(TextRender()->DefaultTextSelectionColor(), IGraphics::CORNER_L, 2.5f);
				SpectatorRectLineBreak.Draw(TextRender()->DefaultTextSelectionColor(), IGraphics::CORNER_R, 2.5f);
			}
		}

		// render the entry's text on top of the highlight
		RenderEntryText(&Cursor);

		CommaNeeded = true;
		--RemainingSpectators;
	}
}

void CScoreboard::RenderScoreboard(const CRenderContext &Context, CUIRect Scoreboard, int Team, int CountStart, int CountEnd, CScoreboardRenderState &State, int NumPlayersForSize)
{
	dbg_assert(Team == TEAM_RED || Team == TEAM_BLUE, "Team invalid");

	const CGameState &GameState = Context.m_State;
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> *pClientsByDDTeamScore = Presentation.ClientsByDDTeamScore(GameState.m_Conn);
	if(pClientsByDDTeamScore == nullptr)
		return;
	const CNetObj_GameInfo *pGameInfoObj = GameState.HasGameInfo() ? &GameState.GameInfo() : nullptr;
	const CNetObj_GameData *pGameDataObj = GameState.GameData();
	const bool TimeScore = GameState.CoreGameInfo().m_TimeScore;
	const bool MillisecondScore = GameState.m_Runtime.m_ReceivedDDNetPlayerFinishTimes;
	const bool TrueMilliseconds = GameState.m_Runtime.m_ReceivedDDNetPlayerFinishTimesMillis;
	const int NumPlayers = NumPlayersForSize >= 0 ? NumPlayersForSize : (CountEnd - CountStart);
	const bool LowScoreboardWidth = Scoreboard.w < 350.0f;

	bool Race7 = Context.m_Session.Protocol() == EGameProtocol::SIXUP && pGameInfoObj && pGameInfoObj->m_GameFlags & protocol7::GAMEFLAG_RACE;

	const bool UseTime = Race7 || TimeScore || MillisecondScore;

	// calculate measurements
	float LineHeight;
	float TeeSizeMod;
	float Spacing;
	float RoundRadius;
	float FontSize;
	if(NumPlayers <= 8)
	{
		LineHeight = 30.0f;
		TeeSizeMod = 0.5f;
		Spacing = 8.0f;
		RoundRadius = 5.0f;
		FontSize = 12.0f;
	}
	else if(NumPlayers <= 12)
	{
		LineHeight = 25.0f;
		TeeSizeMod = 0.45f;
		Spacing = 2.5f;
		RoundRadius = 5.0f;
		FontSize = 12.0f;
	}
	else if(NumPlayers <= 16)
	{
		LineHeight = 20.0f;
		TeeSizeMod = 0.4f;
		Spacing = 0.0f;
		RoundRadius = 2.5f;
		FontSize = 12.0f;
	}
	else if(NumPlayers <= 24)
	{
		LineHeight = 13.5f;
		TeeSizeMod = 0.3f;
		Spacing = 0.0f;
		RoundRadius = 2.5f;
		FontSize = 10.0f;
	}
	else if(NumPlayers <= 32)
	{
		LineHeight = 10.0f;
		TeeSizeMod = 0.2f;
		Spacing = 0.0f;
		RoundRadius = 2.5f;
		FontSize = 8.0f;
	}
	else if(LowScoreboardWidth && NumPlayers <= 48)
	{
		LineHeight = 7.5f;
		TeeSizeMod = 0.125f;
		Spacing = 0.0f;
		RoundRadius = 1.0f;
		FontSize = 7.0f;
	}
	else
	{
		LineHeight = 5.0f;
		TeeSizeMod = 0.1f;
		Spacing = 0.0f;
		RoundRadius = 1.0f;
		FontSize = 5.0f;
	}

	const float ScoreOffset = Scoreboard.x + MARGIN;
	const float ScoreLength = TextRender()->TextWidth(FontSize, UseTime ? "00:00:00" : "99999");
	const float TeeOffset = ScoreOffset + ScoreLength + MARGIN;
	const float TeeLength = 60.0f * TeeSizeMod;
	const float NameOffset = TeeOffset + TeeLength;
	const float CountryLength = (LineHeight - Spacing - TeeSizeMod * 5.0f) * 2.0f;
	const float PingLength = 27.5f;
	const float PingOffset = Scoreboard.x + Scoreboard.w - PingLength - MARGIN;
	const float CountryOffset = PingOffset - CountryLength;

	float NameLength = (LowScoreboardWidth ? 90.0f : 150.0f) - TeeLength;
	const float MinMiddleGap = 5.0f; // 2.5 before and after clan
	const float AvailableMiddle = CountryOffset - NameOffset;
	if(NameLength + MinMiddleGap > AvailableMiddle)
	{
		const float Shrinkable = AvailableMiddle - MinMiddleGap;
		NameLength = std::max(0.0f, Shrinkable * 0.7f);
	}

	const float ClanOffset = NameOffset + NameLength + 2.5f;
	const float ClanLength = std::max(0.0f, CountryOffset - ClanOffset - 2.5f);

	// render headlines
	const float HeadlineFontsize = 11.0f;
	CUIRect Headline;
	Scoreboard.HSplitTop(HeadlineFontsize * 2.0f, &Headline, &Scoreboard);
	const float HeadlineY = Headline.y + Headline.h / 2.0f - HeadlineFontsize / 2.0f;
	const ColorRGBA HeadlineColor = TextRender()->DefaultTextColor();
	m_HeadlineScore.Update(TextRender(), UseTime ? Localize("Time") : Localize("Score"), HeadlineFontsize);
	m_HeadlineName.Update(TextRender(), Localize("Name"), HeadlineFontsize);
	m_HeadlineClan.Update(TextRender(), Localize("Clan"), HeadlineFontsize);
	m_HeadlinePing.Update(TextRender(), Localize("Ping"), HeadlineFontsize);
	m_HeadlineScore.Render(TextRender(), vec2(ScoreOffset + ScoreLength - m_HeadlineScore.Width(), HeadlineY), HeadlineColor);
	m_HeadlineName.Render(TextRender(), vec2(NameOffset, HeadlineY), HeadlineColor);
	m_HeadlineClan.Render(TextRender(), vec2(ClanOffset + (ClanLength - m_HeadlineClan.Width()) / 2.0f, HeadlineY), HeadlineColor);
	m_HeadlinePing.Render(TextRender(), vec2(PingOffset + PingLength - m_HeadlinePing.Width(), HeadlineY), HeadlineColor);

	// render player entries
	int CountRendered = 0;
	int PrevDDTeam = -1;
	int &CurrentDDTeamSize = State.m_CurrentDDTeamSize;

	char aBuf[64];
	// The size the server sent wins over the local setting, and it belongs to
	// the session being rendered rather than to the focused one.
	const int SentMaxTeamSize = GameState.CoreGameInfo().m_MaxTeamSize;
	const int MaxTeamSize = SentMaxTeamSize != 0 ? SentMaxTeamSize : Context.m_Session.m_MapContext.GameConfig().Values().m_SvMaxTeamSize;

	for(int RenderDead = 0; RenderDead < 2; RenderDead++)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			// make sure that we render the correct team
			const int ClientId = (*pClientsByDDTeamScore)[i];
			if(ClientId < 0)
				break;
			const CGameState::CClientSnapshot &SnapshotClient = GameState.Client(ClientId);
			const CNetObj_PlayerInfo &Info = SnapshotClient.m_PlayerInfo;
			if(Info.m_Team != Team)
				continue;
			const CClientPresentation *pClient = Presentation.Client(GameState.m_Conn, ClientId);
			if(pClient == nullptr || !pClient->m_Active)
				continue;
			const bool IsDead = Context.m_Session.Protocol() == EGameProtocol::SIXUP && (GameState.Protocol7Client(ClientId).m_PlayerFlags & protocol7::PLAYERFLAG_DEAD) != 0;
			if(!RenderDead && IsDead)
				continue;
			if(RenderDead && !IsDead)
				continue;
			if(CountRendered++ < CountStart)
				continue;

			const int DDTeam = GameState.Teams().Team(ClientId);
			int NextDDTeam = 0;

			ColorRGBA TextColor = TextRender()->DefaultTextColor();
			TextColor.a = RenderDead ? 0.5f : 1.0f;
			TextRender()->TextColor(TextColor);

			for(int j = i + 1; j < MAX_CLIENTS; j++)
			{
				const int NextClientId = (*pClientsByDDTeamScore)[j];
				if(NextClientId < 0)
					break;
				if(GameState.Client(NextClientId).m_PlayerInfo.m_Team != Team)
					continue;

				NextDDTeam = GameState.Teams().Team(NextClientId);
				break;
			}

			if(PrevDDTeam == -1)
			{
				for(int j = i - 1; j >= 0; j--)
				{
					const int PrevClientId = (*pClientsByDDTeamScore)[j];
					if(PrevClientId < 0 || GameState.Client(PrevClientId).m_PlayerInfo.m_Team != Team)
						continue;

					PrevDDTeam = GameState.Teams().Team(PrevClientId);
					break;
				}
			}

			CUIRect RowAndSpacing, Row;
			Scoreboard.HSplitTop(LineHeight + Spacing, &RowAndSpacing, &Scoreboard);
			RowAndSpacing.HSplitTop(LineHeight, &Row, nullptr);

			// team background
			if(DDTeam != TEAM_FLOCK)
			{
				const ColorRGBA Color = GameClient()->GetDDTeamColor(DDTeam).WithAlpha(0.5f);
				int TeamRectCorners = 0;
				if(PrevDDTeam != DDTeam)
				{
					TeamRectCorners |= IGraphics::CORNER_T;
					State.m_TeamStartX = Row.x;
					State.m_TeamStartY = Row.y;
				}
				if(NextDDTeam != DDTeam)
					TeamRectCorners |= IGraphics::CORNER_B;
				RowAndSpacing.Draw(Color, TeamRectCorners, RoundRadius);

				CurrentDDTeamSize++;

				if(NextDDTeam != DDTeam)
				{
					const float TeamFontSize = FontSize / 1.5f;

					if(NumPlayers > 8)
					{
						if(DDTeam == Context.m_State.Teams().TeamSuper())
							str_copy(aBuf, Localize("Super"));
						else if(CurrentDDTeamSize <= 1)
							str_format(aBuf, sizeof(aBuf), "%d", DDTeam);
						else
							str_format(aBuf, sizeof(aBuf), Localize("%d\n(%d/%d)", "Team and size"), DDTeam, CurrentDDTeamSize, MaxTeamSize);
						TextRender()->Text(State.m_TeamStartX, std::max(State.m_TeamStartY + Row.h / 2.0f - TeamFontSize, State.m_TeamStartY + 1.5f /* padding top */), TeamFontSize, aBuf);
					}
					else
					{
						if(DDTeam == Context.m_State.Teams().TeamSuper())
							str_copy(aBuf, Localize("Super"));
						else if(CurrentDDTeamSize > 1)
							str_format(aBuf, sizeof(aBuf), Localize("Team %d (%d/%d)"), DDTeam, CurrentDDTeamSize, MaxTeamSize);
						else
							str_format(aBuf, sizeof(aBuf), Localize("Team %d"), DDTeam);
						TextRender()->Text(Row.x + Row.w / 2.0f - TextRender()->TextWidth(TeamFontSize, aBuf) / 2.0f + 5.0f, Row.y + Row.h, TeamFontSize, aBuf);
					}

					CurrentDDTeamSize = 0;
				}
			}
			PrevDDTeam = DDTeam;

			// background so it's easy to find the local player or the followed one in spectator mode
			const bool Local = ClientId == GameState.LocalClientId();
			if((!Context.m_View.IsSpectating() && Local) ||
				(Context.m_View.IsSpectating() && Context.m_View.SpectatorId() == SPEC_FREEVIEW && Local) ||
				(Context.m_View.IsSpectating() && ClientId == Context.m_View.SpectatorId()))
			{
				Row.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_ALL, RoundRadius);
			}

			CPlayerElement &Player = m_aPlayers[ClientId];
			CPlayerInteraction &Interaction = m_pCurrentInteractionLayout->m_aPlayers[ClientId];
			Interaction.m_Rect = Row;
			Interaction.m_RoundRadius = RoundRadius;
			Interaction.m_Active = true;
			Interaction.m_IsSpectating = false;
			if(IsHighlighted(Context, ClientId))
				Row.Draw(ColorRGBA(0.7f, 0.7f, 0.7f, 0.7f), IGraphics::CORNER_ALL, RoundRadius);

			// score
			CUIRect ScorePosition;
			ScorePosition.x = ScoreOffset;
			ScorePosition.w = ScoreLength;
			ScorePosition.y = Row.y;
			ScorePosition.h = Row.h;

			if(Race7)
			{
				Ui()->RenderTime(ScorePosition, FontSize, Info.m_Score / 1000, Info.m_Score == protocol7::FinishTime::NOT_FINISHED, Info.m_Score % 1000, true,
					Player.m_Score, Player.m_ScoreMillis, TextColor);
			}
			else if(MillisecondScore)
			{
				const int FinishTimeSeconds = SnapshotClient.m_HasDDNetPlayer ? SnapshotClient.m_DDNetPlayer.m_FinishTimeSeconds : FinishTime::UNSET;
				const int FinishTimeMillis = SnapshotClient.m_HasDDNetPlayer ? SnapshotClient.m_DDNetPlayer.m_FinishTimeMillis : 0;
				Ui()->RenderTime(ScorePosition, FontSize, FinishTimeSeconds, FinishTimeSeconds == FinishTime::UNSET || FinishTimeSeconds == FinishTime::NOT_FINISHED_MILLIS, FinishTimeMillis, TrueMilliseconds,
					Player.m_Score, Player.m_ScoreMillis, TextColor);
			}
			else if(TimeScore)
			{
				Ui()->RenderTime(ScorePosition, FontSize, Info.m_Score, Info.m_Score == FinishTime::NOT_FINISHED_TIMESCORE, -1, false,
					Player.m_Score, Player.m_ScoreMillis, TextColor);
			}
			else
			{
				str_format(aBuf, sizeof(aBuf), "%d", std::clamp(Info.m_Score, -999, 99999));
				Player.m_Score.Update(TextRender(), aBuf, FontSize);
				Player.m_Score.Render(TextRender(), vec2(ScoreOffset + ScoreLength - Player.m_Score.Width(), ScorePosition.y + (Row.h - FontSize) / 2.0f), TextColor);
			}

			// CTF flag
			if(pGameInfoObj && (pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS) &&
				pGameDataObj && (pGameDataObj->m_FlagCarrierRed == ClientId || pGameDataObj->m_FlagCarrierBlue == ClientId))
			{
				Graphics()->TextureSet(pGameDataObj->m_FlagCarrierBlue == ClientId ? GameClient()->m_GameSkin.m_SpriteFlagBlue : GameClient()->m_GameSkin.m_SpriteFlagRed);
				Graphics()->QuadsBegin();
				Graphics()->QuadsSetSubset(1.0f, 0.0f, 0.0f, 1.0f);
				IGraphics::CQuadItem QuadItem(TeeOffset, Row.y - 2.5f - Spacing / 2.0f, Row.h / 2.0f, Row.h);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
				Graphics()->QuadsEnd();
			}

			// skin
			if(RenderDead)
			{
				Graphics()->TextureSet(m_DeadTeeTexture);
				Graphics()->QuadsBegin();
				if(pGameInfoObj && (pGameInfoObj->m_GameFlags & GAMEFLAG_TEAMS) != 0)
				{
					Graphics()->SetColor(GameClient()->m_Skins7.GetTeamColor(true, 0, pClient->m_Team, protocol7::SKINPART_BODY));
				}
				CTeeRenderInfo TeeInfo = pClient->m_BaseRenderInfo;
				TeeInfo.m_Size *= TeeSizeMod;
				IGraphics::CQuadItem QuadItem(TeeOffset, Row.y, TeeInfo.m_Size, TeeInfo.m_Size);
				Graphics()->QuadsDrawTL(&QuadItem, 1);
				Graphics()->QuadsEnd();
			}
			else
			{
				CTeeRenderInfo TeeInfo = pClient->m_BaseRenderInfo;
				TeeInfo.m_Size *= TeeSizeMod;
				vec2 OffsetToMid;
				CRenderTools::GetRenderTeeOffsetToRenderedTee(CAnimState::GetIdle(), &TeeInfo, OffsetToMid);
				const vec2 TeeRenderPos = vec2(TeeOffset + TeeLength / 2, Row.y + Row.h / 2.0f + OffsetToMid.y);
				RenderTools()->RenderTee(CAnimState::GetIdle(), &TeeInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), TeeRenderPos);

				Interaction.m_SkinRect = {TeeOffset, Row.y, TeeLength, Row.h};
				Interaction.m_HasSkinRect = true;
			}

			const float TextY = Row.y + (Row.h - FontSize) / 2.0f;

			// name
			{
				if(g_Config.m_ClShowIds)
				{
					char aClientId[16];
					GameClient()->FormatClientId(ClientId, aClientId, EClientIdFormat::INDENT_AUTO);
					str_copy(aBuf, aClientId);
					str_append(aBuf, pClient->m_aName);
				}
				else
				{
					str_copy(aBuf, pClient->m_aName);
				}
				Player.m_Name.Update(TextRender(), aBuf, FontSize, NameLength, TEXTFLAG_RENDER | TEXTFLAG_ELLIPSIS_AT_END);

				ColorRGBA NameColor = TextColor;
				const int AuthLevel = SnapshotClient.m_HasDDNetPlayer ? SnapshotClient.m_DDNetPlayer.m_AuthLevel : 0;
				if(AuthLevel)
				{
					NameColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClAuthedPlayerColor));
				}
				Player.m_Name.Render(TextRender(), vec2(NameOffset, TextY), NameColor);

				// ready / watching
				if(Context.m_Session.Protocol() == EGameProtocol::SIXUP && (Context.m_State.Protocol7Client(ClientId).m_PlayerFlags & protocol7::PLAYERFLAG_READY) != 0)
				{
					Player.m_ReadyMark.Update(TextRender(), "✓", FontSize);
					Player.m_ReadyMark.Render(TextRender(), vec2(NameOffset + Player.m_Name.Width(), TextY), ColorRGBA(0.1f, 1.0f, 0.1f, TextColor.a));
				}
			}

			// clan
			{
				ColorRGBA ClanColor = TextColor;
				const CClientPresentation *pLocalClient = Presentation.Client(GameState.m_Conn, GameState.LocalClientId());
				if(pLocalClient != nullptr && str_comp(pClient->m_aClan, pLocalClient->m_aClan) == 0)
				{
					ClanColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClSameClanColor));
				}
				Player.m_Clan.Update(TextRender(), pClient->m_aClan, FontSize, ClanLength, TEXTFLAG_RENDER | TEXTFLAG_ELLIPSIS_AT_END);
				Player.m_Clan.Render(TextRender(), vec2(ClanOffset + (ClanLength - std::min(Player.m_Clan.Width(), ClanLength)) / 2.0f, TextY), ClanColor);
			}

			// country flag
			GameClient()->m_CountryFlags.Render(pClient->m_Country, ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f),
				CountryOffset, Row.y + (Spacing + TeeSizeMod * 5.0f) / 2.0f, CountryLength, Row.h - Spacing - TeeSizeMod * 5.0f);

			// ping
			ColorRGBA PingColor = TextRender()->DefaultTextColor();
			if(g_Config.m_ClEnablePingColor)
			{
				PingColor = color_cast<ColorRGBA>(ColorHSLA((300.0f - std::clamp(Info.m_Latency, 0, 300)) / 1000.0f, 1.0f, 0.5f));
			}
			str_format(aBuf, sizeof(aBuf), "%d", std::clamp(Info.m_Latency, 0, 999));
			Player.m_Ping.Update(TextRender(), aBuf, FontSize);
			Player.m_Ping.Render(TextRender(), vec2(PingOffset + PingLength - Player.m_Ping.Width(), TextY), PingColor);
			TextRender()->TextColor(TextRender()->DefaultTextColor());

			if(CountRendered == CountEnd)
				break;
		}
		if(CountRendered == CountEnd)
			break;
	}
}

void CScoreboard::RenderMouseHint(CUIRect MouseHint)
{
	constexpr float FontSize = 11.0f;
	constexpr float PillPaddingX = 8.0f;
	constexpr float PillPaddingY = 5.0f;
	constexpr float TextKeycapGap = 5.0f;
	constexpr float KeycapPaddingX = 4.0f;
	constexpr float KeycapPaddingY = 2.0f;
	constexpr float KeycapRadius = 3.5f;

	char aKey[64];
	GameClient()->m_Binds.GetKey(SCOREBOARD_CURSOR_BIND_NAME, aKey, sizeof(aKey));

	char aHint[128];
	if(!aKey[0])
	{
		str_copy(aHint, Localize("not bound"));
	}
	else
	{
		str_copy(aHint, Localize("Show cursor"));
	}

	const bool HasKeycap = aKey[0] != '\0';
	const char *pChipText = HasKeycap ? aKey : SCOREBOARD_CURSOR_BIND_NAME;
	const float TextWidth = TextRender()->TextWidth(FontSize, aHint, -1);
	const float ChipWidth = TextRender()->TextWidth(FontSize, pChipText, -1) + 2 * KeycapPaddingX;
	const float ContentWidth = ChipWidth + TextKeycapGap + TextWidth;
	const float PillHeight = FontSize + 2 * PillPaddingY;
	const float PillWidth = std::min(ContentWidth + 2 * PillPaddingX, MouseHint.w);

	// content-sized pill, top center in the available space
	CUIRect Pill = {MouseHint.x + (MouseHint.w - PillWidth) / 2.0f, MouseHint.y, PillWidth, PillHeight};
	Pill.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 7.5f);

	CUIRect Content = Pill;
	Content.x += (Pill.w - ContentWidth) / 2.0f;
	Content.w = ContentWidth;

	CUIRect Label, Chip;
	if(HasKeycap)
	{
		// key behind the text: "Show cursor [key]"
		Content.VSplitLeft(TextWidth, &Label, &Content);
		Content.VSplitLeft(TextKeycapGap, nullptr, &Content);
		Content.VSplitLeft(ChipWidth, &Chip, nullptr);
	}
	else
	{
		// bind name in front of the text: "[bind name] not bound"
		Content.VSplitLeft(ChipWidth, &Chip, &Content);
		Content.VSplitLeft(TextKeycapGap, nullptr, &Content);
		Label = Content;
	}

	// floating chip, vertically centered in the pill
	Chip.HMargin((Chip.h - (FontSize + 2 * KeycapPaddingY)) / 2.0f, &Chip);
	Chip.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.15f), IGraphics::CORNER_ALL, KeycapRadius);

	Ui()->DoLabel(&Label, aHint, FontSize, TEXTALIGN_ML);
	Ui()->DoLabel(&Chip, pChipText, FontSize, TEXTALIGN_MC);
}

void CScoreboard::RenderRecordingNotification(float x)
{
	char aBuf[512] = "";

	const auto &&AppendRecorderInfo = [&](int Recorder, const char *pName) {
		if(GameClient()->DemoRecorder(Recorder)->IsRecording())
		{
			char aTime[32];
			str_time((int64_t)GameClient()->DemoRecorder(Recorder)->Length() * 100, ETimeFormat::HOURS, aTime, sizeof(aTime));
			str_append(aBuf, pName);
			str_append(aBuf, " ");
			str_append(aBuf, aTime);
			str_append(aBuf, "  ");
		}
	};

	AppendRecorderInfo(RECORDER_MANUAL, Localize("Manual"));
	AppendRecorderInfo(RECORDER_RACE, Localize("Race"));
	AppendRecorderInfo(RECORDER_AUTO, Localize("Auto"));
	AppendRecorderInfo(RECORDER_REPLAYS, Localize("Replay"));

	if(aBuf[0] == '\0')
		return;

	const float FontSize = 10.0f;

	CUIRect Rect = {x, 0.0f, TextRender()->TextWidth(FontSize, aBuf) + 30.0f, 25.0f};
	Rect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.4f), IGraphics::CORNER_B, 7.5f);
	Rect.VSplitLeft(10.0f, nullptr, &Rect);
	Rect.VSplitRight(5.0f, &Rect, nullptr);

	CUIRect Circle;
	Rect.VSplitLeft(10.0f, &Circle, &Rect);
	Circle.HMargin((Circle.h - Circle.w) / 2.0f, &Circle);
	Circle.Draw(ColorRGBA(1.0f, 0.0f, 0.0f, 1.0f), IGraphics::CORNER_ALL, Circle.h / 2.0f);

	Rect.VSplitLeft(5.0f, nullptr, &Rect);
	Ui()->DoLabel(&Rect, aBuf, FontSize, TEXTALIGN_ML);
}

void CScoreboard::RenderMatchReport(const CStoredMatch &Stored, CUIRect Screen)
{
	const CMatchReport &Report = Stored.m_Report;
	m_ReportRanking.Update(Report);
	const int NumParticipants = m_ReportRanking.Rows().size();
	const int NumColumns = NumParticipants <= 16 ? 1 : NumParticipants <= 32 ? 2 :
						   NumParticipants <= 72         ? 3 :
										   4;
	const int RowsPerColumn = (NumParticipants + NumColumns - 1) / NumColumns;
	const float Width = std::min(Screen.w - 20.0f, NumColumns * 360.0f);
	const float RowHeight = RowsPerColumn <= 16 ? 20.0f : std::max(8.0f, 320.0f / RowsPerColumn);
	const int ActionColumns = Width < 650.0f ? 3 : NUM_REPORT_ACTIONS;
	const int ActionRows = (NUM_REPORT_ACTIONS + ActionColumns - 1) / ActionColumns;
	const float ActionsHeight = ActionRows * 24.0f - 4.0f;
	const float Height = 68.0f + (RowsPerColumn + 1) * RowHeight + 29.0f + ActionsHeight;
	CUIRect Panel = {(Screen.w - Width) / 2.0f, 45.0f, Width, std::min(Height, 545.0f)};
	GameClient()->m_Menus.DrawSurface(Panel, ColorRGBA(0.0f, 0.0f, 0.0f, 0.65f), IGraphics::CORNER_ALL, 7.5f);
	Panel.Margin(10.0f, &Panel);

	const CMatchStanding *pLocalStanding = nullptr;
	if(const CMatchParticipant *pLocal = Stored.m_LocalParticipantId ? Report.Participant(*Stored.m_LocalParticipantId) : nullptr)
	{
		pLocalStanding = Report.Standing(EMatchSubjectKind::PARTICIPANT, pLocal->m_ParticipantId);
		if(!pLocalStanding && pLocal->m_TeamId)
			pLocalStanding = Report.Standing(EMatchSubjectKind::TEAM, *pLocal->m_TeamId);
	}
	CUIRect Title, Subtitle, Rows, Summary, Actions;
	Panel.HSplitTop(30.0f, &Title, &Panel);
	Panel.HSplitTop(22.0f, &Subtitle, &Panel);
	Panel.HSplitBottom(ActionsHeight, &Panel, &Actions);
	Panel.HSplitBottom(23.0f, &Rows, &Summary);
	Ui()->DoLabel(&Title, pLocalStanding ? MatchOutcomeDisplayName(pLocalStanding->m_Outcome) : Localize("Game over"), 24.0f, TEXTALIGN_MC);

	char aDuration[64];
	FormatMatchDuration(Report.m_DurationTicks, Report.m_TickRate, aDuration, sizeof(aDuration));
	char aSubtitle[512];
	str_format(aSubtitle, sizeof(aSubtitle), "%s — %s — %s — %s / %s", Report.m_MapName.c_str(), Report.m_ModeId.c_str(), aDuration, MatchReportSourceDisplayName(Stored.m_Source), MatchCompletenessDisplayName(Stored.m_Completeness));
	if(Report.m_vTeams.size() == 2)
	{
		const std::optional<int64_t> FirstScore = Report.Metric(EMatchSubjectKind::TEAM, Report.m_vTeams[0].m_TeamId, "score");
		const std::optional<int64_t> SecondScore = Report.Metric(EMatchSubjectKind::TEAM, Report.m_vTeams[1].m_TeamId, "score");
		if(FirstScore && SecondScore)
		{
			char aTeamScore[192];
			str_format(aTeamScore, sizeof(aTeamScore), " — %s %" PRId64 " : %" PRId64 " %s", Report.m_vTeams[0].m_DisplayName.c_str(), *FirstScore, *SecondScore, Report.m_vTeams[1].m_DisplayName.c_str());
			str_append(aSubtitle, aTeamScore);
		}
	}
	Ui()->DoLabel(&Subtitle, aSubtitle, 11.0f, TEXTALIGN_MC);

	for(int Column = 0; Column < NumColumns; ++Column)
	{
		CUIRect ColumnRect;
		Rows.VSplitLeft(Rows.w / (NumColumns - Column), &ColumnRect, &Rows);
		const int First = Column * RowsPerColumn;
		const std::span<const CMatchReportRow> vRows(m_ReportRanking.Rows().data() + First, std::min(RowsPerColumn, NumParticipants - First));
		RenderMatchReportColumn(Stored, vRows, ColumnRect, RowHeight);
	}
	RenderMatchReportSummary(Stored, Summary);

	m_pCurrentInteractionLayout->m_ReportMatchId = Report.m_MatchId;
	for(int Action = 0; Action < NUM_REPORT_ACTIONS; ++Action)
	{
		const int Row = Action / ActionColumns;
		const int ActionsInRow = std::min(ActionColumns, NUM_REPORT_ACTIONS - Row * ActionColumns);
		const float ButtonWidth = (Actions.w - (ActionsInRow - 1) * 4.0f) / ActionsInRow;
		const CUIRect Button = {Actions.x + (Action % ActionColumns) * (ButtonWidth + 4.0f), Actions.y + Row * 24.0f, ButtonWidth, 20.0f};
		m_pCurrentInteractionLayout->m_aReportActionRects[Action] = Button;
		// continue is what the keyboard does by default, so it stands out
		const float Alpha = Action == REPORT_ACTION_CONTINUE ? (m_HighlightReportAction == Action ? 0.45f : 0.3f) : (m_HighlightReportAction == Action ? 0.3f : 0.18f);
		Button.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, Alpha), IGraphics::CORNER_ALL, 4.0f);
		Ui()->DoLabel(&Button, Localize(gs_aReportActions[Action].m_pLabel), 10.0f, TEXTALIGN_MC);
	}
}

void CScoreboard::RenderMatchReportColumn(const CStoredMatch &Stored, std::span<const CMatchReportRow> vRows, CUIRect Column, float RowHeight)
{
	const float FontSize = std::clamp(RowHeight - 7.0f, 8.0f, 12.0f);
	const auto SplitRow = [&](CUIRect Row, CUIRect &Rank, CUIRect &Score, CUIRect &Name, CUIRect &Clan) {
		Row.VSplitLeft(38.0f, &Rank, &Row);
		Row.VSplitLeft(55.0f, &Score, &Row);
		Row.VSplitRight(Column.w * 0.3f, &Name, &Clan);
	};
	CUIRect Header, Rank, Score, Name, Clan;
	Column.HSplitTop(RowHeight, &Header, &Column);
	Header.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.2f), IGraphics::CORNER_NONE, 0.0f);
	SplitRow(Header, Rank, Score, Name, Clan);
	Ui()->DoLabel(&Rank, Localize("Rank"), FontSize, TEXTALIGN_MC);
	Ui()->DoLabel(&Score, Localize("Score"), FontSize, TEXTALIGN_MC);
	Ui()->DoLabel(&Name, Localize("Name"), FontSize, TEXTALIGN_ML);
	Ui()->DoLabel(&Clan, Localize("Clan"), FontSize, TEXTALIGN_ML);

	for(const CMatchReportRow &Entry : vRows)
	{
		const CMatchParticipant &Participant = *Entry.m_pParticipant;
		CUIRect Row;
		Column.HSplitTop(RowHeight, &Row, &Column);
		if(Stored.m_LocalParticipantId == Participant.m_ParticipantId)
			Row.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.22f), IGraphics::CORNER_ALL, 3.0f);
		SplitRow(Row, Rank, Score, Name, Clan);
		char aValue[64] = "-";
		if(Entry.m_pStanding)
			str_format(aValue, sizeof(aValue), "%d", Entry.m_pStanding->m_Rank);
		Ui()->DoLabel(&Rank, aValue, FontSize, TEXTALIGN_MC);
		str_copy(aValue, "-");
		if(Entry.m_Score)
			str_format(aValue, sizeof(aValue), "%" PRId64, *Entry.m_Score);
		Ui()->DoLabel(&Score, aValue, FontSize, TEXTALIGN_MC);
		char aName[MatchReportLimits::MAX_DISPLAY_NAME_LENGTH + 32];
		if(Participant.m_LeftTick)
			str_format(aName, sizeof(aName), "%s (%s)", Participant.m_DisplayName.c_str(), Localize("left"));
		else
			str_copy(aName, Participant.m_DisplayName.c_str());
		SLabelProperties Properties;
		Properties.m_MaxWidth = Name.w;
		Properties.m_EllipsisAtEnd = true;
		Ui()->DoLabel(&Name, aName, FontSize, TEXTALIGN_ML, Properties);
		Properties.m_MaxWidth = Clan.w;
		Ui()->DoLabel(&Clan, Participant.m_Clan.c_str(), FontSize, TEXTALIGN_ML, Properties);
	}
}

void CScoreboard::RenderMatchReportSummary(const CStoredMatch &Stored, CUIRect Summary)
{
	const CMatchReportRow *pLocal = Stored.m_LocalParticipantId ? m_ReportRanking.Row(*Stored.m_LocalParticipantId) : nullptr;
	if(!pLocal)
		return;
	char aSummary[256] = "";
	if(pLocal->m_Combat.Value(MATCH_COMBAT_SHOTS) > 0)
	{
		char aAccuracy[32];
		FormatMatchAccuracy(pLocal->m_Combat.Value(MATCH_COMBAT_HITS), pLocal->m_Combat.Value(MATCH_COMBAT_SHOTS), aAccuracy, sizeof(aAccuracy));
		str_format(aSummary, sizeof(aSummary), "%s: %" PRId64 "  ·  %s: %" PRId64 "  ·  %s: %s  ·  %s: %" PRId64 "  ·  %s: %" PRId64,
			Localize("Shots"), pLocal->m_Combat.Value(MATCH_COMBAT_SHOTS),
			Localize("Hits"), pLocal->m_Combat.Value(MATCH_COMBAT_HITS),
			Localize("Accuracy"), aAccuracy,
			Localize("Damage done"), pLocal->m_Combat.Value(MATCH_COMBAT_DAMAGE_DONE),
			Localize("Damage taken"), pLocal->m_Combat.Value(MATCH_COMBAT_DAMAGE_TAKEN));
	}
	else
	{
		// without the counters of a server, what the client counted itself
		for(const CMatchMetric &Metric : Stored.m_Report.m_vMetrics)
		{
			if(Metric.m_SubjectKind != EMatchSubjectKind::PARTICIPANT || Metric.m_SubjectId != Stored.m_LocalParticipantId || Metric.m_MetricId == "score" || MatchMetricCategory(Metric.m_MetricId) == EMatchMetricCategory::OVERVIEW || !FindMatchMetric(Metric.m_MetricId) || Metric.m_MetricId.starts_with("weapon_"))
				continue;
			char aName[64];
			MatchMetricDisplayName(Metric.m_MetricId, aName, sizeof(aName));
			char aValue[64];
			FormatMatchMetricValue(Metric, Stored.m_Report.m_TickRate, aValue, sizeof(aValue));
			char aEntry[160];
			str_format(aEntry, sizeof(aEntry), "%s%s: %s", aSummary[0] ? "  ·  " : "", aName, aValue);
			str_append(aSummary, aEntry);
		}
	}
	SLabelProperties Properties;
	Properties.m_MaxWidth = Summary.w;
	Properties.m_EllipsisAtEnd = true;
	Ui()->DoLabel(&Summary, aSummary, 11.0f, TEXTALIGN_MC, Properties);
}

void CScoreboard::OnRender(const CRenderContext &Context)
{
	CInteractionLayout DiscardedLayout;
	CInteractionLayout &Layout = &Context.m_View == &GameClient()->LegacyGameView() ? m_InteractionLayout : DiscardedLayout;
	Layout = {};
	Layout.m_Binding = Context.m_View.Binding();
	Layout.m_Viewport = Context.m_View.Viewport();
	m_pCurrentInteractionLayout = &Layout;

	if(!Context.m_Time.m_IsGameActive)
	{
		m_pCurrentInteractionLayout = nullptr;
		return;
	}

	if(!IsActive(Context))
	{
		m_pCurrentInteractionLayout = nullptr;
		return;
	}
	Layout.m_Active = true;

	constexpr float TitleHeight = 30.0f;
	constexpr float ScoreboardSmallWidth = 375.0f + 10.0f;
	constexpr float ScoreboardSpacing = 5.0f;

	const CUIRect Screen = {0.0f, 0.0f, 600.0f * Context.AspectRatio(Graphics()->ScreenAspect()), 600.0f};
	Graphics()->MapScreenToSize(Screen.w, Screen.h);

	const CGameState &GameState = Context.m_State;
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const CNetObj_GameInfo *pGameInfoObj = GameState.HasGameInfo() ? &GameState.GameInfo() : nullptr;
	const std::optional<CStoredMatch> &LatestMatch = Context.m_Session.m_Stats.LatestMatch();
	if(pGameInfoObj != nullptr && (pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) != 0 && LatestMatch.has_value() && !LatestMatch->m_Report.m_vParticipants.empty() &&
		!IsMatchReportDismissed(Context.m_Session.Id(), LatestMatch->m_Report.m_MatchId) && LatestMatch->m_Report.m_MapSha256 == Context.m_Session.m_MapContext.Map()->Sha256())
	{
		RenderMatchReport(*LatestMatch, Screen);
		m_pCurrentInteractionLayout = nullptr;
		return;
	}
	const bool Teams = pGameInfoObj != nullptr && (pGameInfoObj->m_GameFlags & GAMEFLAG_TEAMS) != 0;
	const int NumPlayers = Teams ? std::max(Presentation.TeamSize(GameState.m_Conn, TEAM_RED), Presentation.TeamSize(GameState.m_Conn, TEAM_BLUE)) : Presentation.TeamSize(GameState.m_Conn, TEAM_RED);

	const float ScoreboardWidth = !Teams && NumPlayers <= 16 ? ScoreboardSmallWidth : 750.0f;

	CUIRect Scoreboard = {(Screen.w - ScoreboardWidth) / 2.0f, 75.0f, ScoreboardWidth, 355.0f + TitleHeight};
	CScoreboardRenderState RenderState{};

	if(Teams)
	{
		const char *pRedTeamName = GetTeamName(Context, TEAM_RED);
		const char *pBlueTeamName = GetTeamName(Context, TEAM_BLUE);

		// Game over title
		const CNetObj_GameData *pGameDataObj = GameState.GameData();
		if((pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) && pGameDataObj)
		{
			char aTitle[256];
			if(pGameDataObj->m_TeamscoreRed > pGameDataObj->m_TeamscoreBlue)
			{
				TextRender()->TextColor(ColorRGBA(0.975f, 0.17f, 0.17f, 1.0f));
				if(pRedTeamName == nullptr)
				{
					str_copy(aTitle, Localize("Red team wins!"));
				}
				else
				{
					str_format(aTitle, sizeof(aTitle), Localize("%s wins!"), pRedTeamName);
				}
			}
			else if(pGameDataObj->m_TeamscoreBlue > pGameDataObj->m_TeamscoreRed)
			{
				TextRender()->TextColor(ColorRGBA(0.17f, 0.46f, 0.975f, 1.0f));
				if(pBlueTeamName == nullptr)
				{
					str_copy(aTitle, Localize("Blue team wins!"));
				}
				else
				{
					str_format(aTitle, sizeof(aTitle), Localize("%s wins!"), pBlueTeamName);
				}
			}
			else
			{
				TextRender()->TextColor(ColorRGBA(0.91f, 0.78f, 0.33f, 1.0f));
				str_copy(aTitle, Localize("Draw!"));
			}

			const float TitleFontSize = 36.0f;
			CUIRect GameOverTitle = {Scoreboard.x, Scoreboard.y - TitleFontSize - 6.0f, Scoreboard.w, TitleFontSize};
			Ui()->DoLabel(&GameOverTitle, aTitle, TitleFontSize, TEXTALIGN_MC);
			TextRender()->TextColor(TextRender()->DefaultTextColor());
		}

		CUIRect RedScoreboard, BlueScoreboard, RedTitle, BlueTitle;
		Scoreboard.VSplitMid(&RedScoreboard, &BlueScoreboard, 7.5f);
		// Title and body are two boxes that meet in a straight line, so one
		// rounded backdrop each covers both.
		GameClient()->m_Menus.RenderBackdropRegion(RedScoreboard, IGraphics::CORNER_ALL, 7.5f);
		GameClient()->m_Menus.RenderBackdropRegion(BlueScoreboard, IGraphics::CORNER_ALL, 7.5f);
		RedScoreboard.HSplitTop(TitleHeight, &RedTitle, &RedScoreboard);
		BlueScoreboard.HSplitTop(TitleHeight, &BlueTitle, &BlueScoreboard);

		RedTitle.Draw(ColorRGBA(0.975f, 0.17f, 0.17f, 0.5f), IGraphics::CORNER_T, 7.5f);
		BlueTitle.Draw(ColorRGBA(0.17f, 0.46f, 0.975f, 0.5f), IGraphics::CORNER_T, 7.5f);
		RedScoreboard.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_B, 7.5f);
		BlueScoreboard.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_B, 7.5f);

		RenderTitleBar(Context, RedTitle, TEAM_RED, pRedTeamName == nullptr ? Localize("Red team") : pRedTeamName);
		RenderTitleBar(Context, BlueTitle, TEAM_BLUE, pBlueTeamName == nullptr ? Localize("Blue team") : pBlueTeamName);

		auto RenderTeamScoreboard = [&](CUIRect TeamScoreboard, int Team, int TeamSize) {
			if(TeamSize <= 64)
			{
				RenderScoreboard(Context, TeamScoreboard, Team, 0, TeamSize, RenderState);
			}
			else
			{
				const int FirstColumnSize = 64;
				CUIRect LeftColumn, RightColumn;
				TeamScoreboard.VSplitMid(&LeftColumn, &RightColumn, 2.5f);

				RenderScoreboard(Context, LeftColumn, Team, 0, FirstColumnSize, RenderState, FirstColumnSize);
				RenderScoreboard(Context, RightColumn, Team, FirstColumnSize, TeamSize, RenderState, FirstColumnSize);
			}
		};

		RenderTeamScoreboard(RedScoreboard, TEAM_RED, Presentation.TeamSize(GameState.m_Conn, TEAM_RED));
		RenderTeamScoreboard(BlueScoreboard, TEAM_BLUE, Presentation.TeamSize(GameState.m_Conn, TEAM_BLUE));
	}
	else
	{
		GameClient()->m_Menus.DrawSurface(Scoreboard, ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 7.5f);

		const char *pTitle;
		if(pGameInfoObj && (pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER))
		{
			pTitle = Localize("Game over");
		}
		else
		{
			pTitle = Context.m_Session.MapName();
		}

		CUIRect Title;
		Scoreboard.HSplitTop(TitleHeight, &Title, &Scoreboard);
		RenderTitleBar(Context, Title, TEAM_GAME, pTitle);

		if(NumPlayers <= 16)
		{
			RenderScoreboard(Context, Scoreboard, TEAM_GAME, 0, NumPlayers, RenderState);
		}
		else if(NumPlayers <= 64)
		{
			int PlayersPerSide;
			if(NumPlayers <= 24)
				PlayersPerSide = 12;
			else if(NumPlayers <= 32)
				PlayersPerSide = 16;
			else if(NumPlayers <= 48)
				PlayersPerSide = 24;
			else
				PlayersPerSide = 32;

			CUIRect LeftScoreboard, RightScoreboard;
			Scoreboard.VSplitMid(&LeftScoreboard, &RightScoreboard);
			RenderScoreboard(Context, LeftScoreboard, TEAM_GAME, 0, PlayersPerSide, RenderState);
			RenderScoreboard(Context, RightScoreboard, TEAM_GAME, PlayersPerSide, 2 * PlayersPerSide, RenderState);
		}
		else
		{
			const int NumColumns = 3;
			const int PlayersPerColumn = std::ceil(128.0f / NumColumns);
			CUIRect RemainingScoreboard = Scoreboard;
			for(int i = 0; i < NumColumns; ++i)
			{
				CUIRect Column;
				RemainingScoreboard.VSplitLeft(Scoreboard.w / NumColumns, &Column, &RemainingScoreboard);
				RenderScoreboard(Context, Column, TEAM_GAME, i * PlayersPerColumn, (i + 1) * PlayersPerColumn, RenderState);
			}
		}
	}

	CUIRect Spectators = {(Screen.w - ScoreboardSmallWidth) / 2.0f, Scoreboard.y + Scoreboard.h + 5.0f, ScoreboardSmallWidth, 100.0f};
	if(pGameInfoObj && (pGameInfoObj->m_ScoreLimit || pGameInfoObj->m_TimeLimit || (pGameInfoObj->m_RoundNum && pGameInfoObj->m_RoundCurrent)))
	{
		CUIRect Goals;
		Spectators.HSplitTop(25.0f, &Goals, &Spectators);
		Spectators.HSplitTop(ScoreboardSpacing, nullptr, &Spectators);
		RenderGoals(Context, Goals);
	}
	RenderSpectators(Context, Spectators);

	if(!m_MouseUnlocked)
	{
		constexpr float MouseHintSize = 26.0f;
		CUIRect MouseHint = {Spectators.x, Spectators.y + Spectators.h + ScoreboardSpacing, ScoreboardSmallWidth, MouseHintSize};
		RenderMouseHint(MouseHint);
	}

	m_pCurrentInteractionLayout = nullptr;
}

bool CScoreboard::UpdateApplicationOverlay(const CRenderContext &Context)
{
	if(!m_InteractionLayout.Matches(Context) || !m_InteractionLayout.m_Active)
		return false;
	CInteractionLayout *pLayout = &m_InteractionLayout;

	Ui()->StartCheck();
	Ui()->Update();
	m_HighlightBinding = Context.m_View.Binding();
	m_HighlightClientId = -1;
	m_HighlightMapTitle = false;
	m_HighlightReportAction = -1;

	if(m_MouseUnlocked && pLayout->m_HasMapTitleRect)
	{
		const int ButtonResult = Ui()->DoButtonLogic(&m_MapTitleButtonId, 0, &pLayout->m_MapTitleRect, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT);
		if(ButtonResult != 0)
		{
			m_MapTitlePopupContext.m_pScoreboard = this;
			str_copy(m_MapTitlePopupContext.m_aDescription, Context.m_Session.m_MapMetadata.Description());
			m_MapTitlePopupContext.m_FontSize = 12.0f;
			constexpr float MaxWidth = 300.0f;
			constexpr float Margin = 5.0f;
			const float TextWidth = std::min(std::ceil(TextRender()->TextWidth(m_MapTitlePopupContext.m_FontSize, m_MapTitlePopupContext.m_aDescription) + 0.5f), MaxWidth);
			float TextHeight = 0.0f;
			STextSizeProperties TextSizeProps{};
			TextSizeProps.m_pHeight = &TextHeight;
			TextRender()->TextWidth(m_MapTitlePopupContext.m_FontSize, m_MapTitlePopupContext.m_aDescription, -1, TextWidth, 0, TextSizeProps);
			Ui()->DoPopupMenu(&m_MapTitlePopupContext, Ui()->MouseX(), Ui()->MouseY(), TextWidth + Margin * 2, TextHeight + Margin * 2, &m_MapTitlePopupContext, CMapTitlePopupContext::Render);
		}
		m_HighlightMapTitle = Ui()->HotItem() == &m_MapTitleButtonId;
	}
	for(int Action = 0; m_MouseUnlocked && pLayout->m_ReportMatchId != UUID_ZEROED && Action < NUM_REPORT_ACTIONS; ++Action)
	{
		if(Ui()->DoButtonLogic(&m_aReportActionButtonIds[Action], 0, &pLayout->m_aReportActionRects[Action], BUTTONFLAG_LEFT))
			RunReportAction(Action, pLayout->m_Binding.m_SessionId, pLayout->m_ReportMatchId);
		if(Ui()->HotItem() == &m_aReportActionButtonIds[Action])
			m_HighlightReportAction = Action;
	}

	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayerElement &Player = m_aPlayers[ClientId];
		CPlayerInteraction &Interaction = pLayout->m_aPlayers[ClientId];
		if(!Interaction.m_Active)
			continue;
		const CClientPresentation *pClient = Presentation.Client(Context.m_State.m_Conn, ClientId);
		if(pClient == nullptr || !pClient->m_Active)
			continue;

		int ButtonResult = 0;
		if(m_MouseUnlocked)
		{
			ButtonResult = Ui()->DoButtonLogic(&Player.m_PlayerButtonId, 0, &Interaction.m_Rect, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT);
			if(Interaction.m_HasSecondRect && ButtonResult == 0)
				ButtonResult = Ui()->DoButtonLogic(&Player.m_SpectatorSecondLineButtonId, 0, &Interaction.m_SecondRect, BUTTONFLAG_LEFT | BUTTONFLAG_RIGHT);
		}
		if(ButtonResult != 0)
		{
			const bool IsLocal = std::any_of(Context.m_Session.GameStates().begin(), Context.m_Session.GameStates().end(), [ClientId](const CGameState &OtherState) { return OtherState.LocalClientId() == ClientId; });
			m_ScoreboardPopupContext.Bind(this, Context, ClientId, pClient->m_aName, pClient->m_aClan, IsLocal, Interaction.m_IsSpectating);
			const float PopupHeight = m_ScoreboardPopupContext.m_IsLocal ? (Interaction.m_IsSpectating ? 30.0f : 58.5f) : (Interaction.m_IsSpectating ? 60.0f : 87.5f);
			Ui()->DoPopupMenu(&m_ScoreboardPopupContext, Ui()->MouseX(), Ui()->MouseY(), 110.0f, PopupHeight, &m_ScoreboardPopupContext, CScoreboardPopupContext::Render);
		}
		const bool Highlighted = m_MouseUnlocked && (Ui()->HotItem() == &Player.m_PlayerButtonId || (Interaction.m_HasSecondRect && Ui()->HotItem() == &Player.m_SpectatorSecondLineButtonId) || (Ui()->IsPopupOpen(&m_ScoreboardPopupContext) && m_ScoreboardPopupContext.m_ClientId == ClientId));
		if(Highlighted)
			m_HighlightClientId = ClientId;
		if(m_MouseUnlocked && Interaction.m_HasSkinRect)
			GameClient()->m_Tooltips.DoToolTip(&Player.m_PlayerButtonId, &Interaction.m_SkinRect, pClient->m_aSkinName);
	}
	return true;
}

void CScoreboard::PrepareApplicationOverlay(const CRenderContext &Context)
{
	m_ApplicationOverlayReady = false;
	m_HighlightClientId = -1;
	m_HighlightMapTitle = false;
	m_HighlightReportAction = -1;
	if(!Context.m_Time.m_IsGameActive || !IsActive(Context))
	{
		if(m_MouseUnlocked)
			LockMouse();
		if(Ui()->IsPopupOpen(&m_ScoreboardPopupContext) || Ui()->IsPopupOpen(&m_MapTitlePopupContext))
			Ui()->ClosePopupMenus();
		return;
	}
	if(GameClient()->m_Menus.IsActive() || GameClient()->m_Chat.IsActive())
		return;
	m_ApplicationOverlayReady = UpdateApplicationOverlay(Context);
	if(!m_ApplicationOverlayReady && (Ui()->IsPopupOpen(&m_ScoreboardPopupContext) || Ui()->IsPopupOpen(&m_MapTitlePopupContext)))
		Ui()->ClosePopupMenus();
}

void CScoreboard::BeginRenderFrame()
{
	m_InteractionLayout = {};
	m_pCurrentInteractionLayout = nullptr;
}

void CScoreboard::RenderApplicationOverlay(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive || !IsActive(Context))
		return;

	const float ScreenWidth = 600.0f * Context.AspectRatio(Graphics()->ScreenAspect());
	Graphics()->MapScreenToSize(ScreenWidth, 600.0f);
	RenderRecordingNotification((ScreenWidth / 7) * 4 + 10);
	if(!m_ApplicationOverlayReady)
		return;
	Ui()->RenderPopupMenus();
	if(m_MouseUnlocked)
		RenderTools()->RenderCursor(Ui()->MousePos(), 24.0f);
	Ui()->FinishCheck();
}

bool CScoreboard::IsActive() const
{
	const CGameView &View = GameClient()->LegacyGameView();
	return IsActive(GameClient()->SessionContext(View.SessionId()).GameState(View.Conn()), View);
}

bool CScoreboard::IsActive(const CGameState &State, const CGameView &View) const
{
	if(GameClient()->m_Statboard.IsActive())
		return false;
	if(m_Active)
		return true;

	const CNetObj_GameInfo *pGameInfoObj = State.HasGameInfo() ? &State.GameInfo() : nullptr;
	const int LocalClientId = State.LocalClientId();
	if(LocalClientId >= 0 && LocalClientId < MAX_CLIENTS && State.Client(LocalClientId).m_HasPlayerInfo && !View.IsSpectating())
	{
		const CGameState::CClientSnapshot &LocalClient = State.Client(LocalClientId);
		if((!LocalClient.m_HasCharacter || !LocalClient.m_HasPrevCharacter) && g_Config.m_ClScoreboardOnDeath &&
			!(pGameInfoObj && (pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED) != 0))
			return true;
	}
	if(pGameInfoObj == nullptr || (pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) == 0)
		return false;
	const std::optional<CStoredMatch> &LatestMatch = GameClient()->SessionContext(View.SessionId()).m_Stats.LatestMatch();
	return !LatestMatch.has_value() || !IsMatchReportDismissed(View.SessionId(), LatestMatch->m_Report.m_MatchId);
}

const char *CScoreboard::GetTeamName(const CRenderContext &Context, int Team) const
{
	dbg_assert(Team == TEAM_RED || Team == TEAM_BLUE, "Team invalid");

	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> *pClientsByScore = Presentation.ClientsByScore(Context.m_State.m_Conn);
	if(pClientsByScore == nullptr)
		return nullptr;
	int ClanPlayers = 0;
	const char *pClanName = nullptr;
	for(const int ClientId : *pClientsByScore)
	{
		if(ClientId < 0)
			break;
		if(Context.m_State.Client(ClientId).m_PlayerInfo.m_Team != Team)
			continue;
		const CClientPresentation *pClient = Presentation.Client(Context.m_State.m_Conn, ClientId);
		if(pClient == nullptr)
			continue;

		if(!pClanName)
		{
			pClanName = pClient->m_aClan;
			ClanPlayers++;
		}
		else
		{
			if(str_comp(pClient->m_aClan, pClanName) == 0)
				ClanPlayers++;
			else
				return nullptr;
		}
	}

	if(ClanPlayers > 1 && pClanName[0] != '\0')
		return pClanName;
	else
		return nullptr;
}

CUi::EPopupMenuFunctionResult CScoreboard::CScoreboardPopupContext::Render(void *pContext, CUIRect View, bool Active)
{
	CScoreboardPopupContext *pPopupContext = static_cast<CScoreboardPopupContext *>(pContext);
	CScoreboard *pScoreboard = pPopupContext->m_pScoreboard;
	CUi *pUi = pPopupContext->m_pScoreboard->Ui();

	CGameClient *pGameClient = pScoreboard->GameClient();
	CGameView &OriginView = pGameClient->LegacyGameView();
	if(OriginView.Binding() != pPopupContext->m_Binding)
		return CUi::POPUP_CLOSE_CURRENT;
	CSessionPresentation &Presentation = pGameClient->SessionPresentation(pPopupContext->m_Binding.m_SessionId);
	const CClientPresentation *pClient = Presentation.Client(pPopupContext->m_Binding.m_Conn, pPopupContext->m_ClientId);
	if(!pClient->m_Active || str_comp(pClient->m_aName, pPopupContext->m_aName) != 0 || str_comp(pClient->m_aClan, pPopupContext->m_aClan) != 0)
		return CUi::POPUP_CLOSE_CURRENT;

	const float Margin = 5.0f;
	View.Margin(Margin, &View);

	CUIRect Label, Container, Action;
	const float ItemSpacing = 2.0f;
	const float FontSize = 12.0f;

	View.HSplitTop(FontSize, &Label, &View);
	pUi->DoLabel(&Label, pClient->m_aName, FontSize, TEXTALIGN_ML);

	if(!pPopupContext->m_IsLocal)
	{
		const int ActionsNum = 3;
		const float ActionSize = 25.0f;
		const float ActionSpacing = (View.w - (ActionsNum * ActionSize)) / 2;
		int ActionCorners = IGraphics::CORNER_ALL;

		View.HSplitTop(ItemSpacing * 2, nullptr, &View);
		View.HSplitTop(ActionSize, &Container, &View);

		Container.VSplitLeft(ActionSize, &Action, &Container);

		ColorRGBA FriendActionColor = pClient->m_Friend ? ColorRGBA(0.95f, 0.3f, 0.3f, 0.85f * pUi->ButtonColorMul(&pPopupContext->m_FriendAction)) :
								  ColorRGBA(1.0f, 1.0f, 1.0f, 0.5f * pUi->ButtonColorMul(&pPopupContext->m_FriendAction));
		const char *pFriendActionIcon = pUi->HotItem() == &pPopupContext->m_FriendAction && pClient->m_Friend ? FontIcon::HEART_CRACK : FontIcon::HEART;
		if(pUi->DoButton_FontIcon(&pPopupContext->m_FriendAction, pFriendActionIcon, pClient->m_Friend, &Action, BUTTONFLAG_LEFT, ActionCorners, true, FriendActionColor))
		{
			if(pClient->m_Friend)
			{
				pGameClient->Friends()->RemoveFriend(pClient->m_aName, pClient->m_aClan);
			}
			else
			{
				pGameClient->Friends()->AddFriend(pClient->m_aName, pClient->m_aClan);
			}
		}

		pGameClient->m_Tooltips.DoToolTip(&pPopupContext->m_FriendAction, &Action, pClient->m_Friend ? Localize("Remove friend") : Localize("Add friend"));

		Container.VSplitLeft(ActionSpacing, nullptr, &Container);
		Container.VSplitLeft(ActionSize, &Action, &Container);

		bool ChatIgnored = Presentation.ChatIgnored(pPopupContext->m_ClientId);
		if(pUi->DoButton_FontIcon(&pPopupContext->m_MuteAction, FontIcon::BAN, ChatIgnored, &Action, BUTTONFLAG_LEFT, ActionCorners))
		{
			Presentation.ToggleChatIgnored(pPopupContext->m_ClientId);
			ChatIgnored = !ChatIgnored;
		}
		pGameClient->m_Tooltips.DoToolTip(&pPopupContext->m_MuteAction, &Action, ChatIgnored ? Localize("Unmute") : Localize("Mute"));

		Container.VSplitLeft(ActionSpacing, nullptr, &Container);
		Container.VSplitLeft(ActionSize, &Action, &Container);

		bool EmoticonIgnored = Presentation.EmoticonIgnored(pPopupContext->m_ClientId);
		const char *EmoticonActionIcon = EmoticonIgnored ? FontIcon::COMMENT_SLASH : FontIcon::COMMENT;
		if(pUi->DoButton_FontIcon(&pPopupContext->m_EmoticonAction, EmoticonActionIcon, EmoticonIgnored, &Action, BUTTONFLAG_LEFT, ActionCorners))
		{
			Presentation.ToggleEmoticonIgnored(pPopupContext->m_ClientId);
			EmoticonIgnored = !EmoticonIgnored;
		}
		pGameClient->m_Tooltips.DoToolTip(&pPopupContext->m_EmoticonAction, &Action, EmoticonIgnored ? Localize("Unmute emoticons") : Localize("Mute emoticons"));
	}

	const float ButtonSize = 17.5f;
	View.HSplitTop(ItemSpacing * 2, nullptr, &View);
	View.HSplitTop(ButtonSize, &Container, &View);

	const bool IsSpectating = OriginView.IsSpectating() && OriginView.SpectatorMode() == pPopupContext->m_ClientId;
	ColorRGBA SpectateButtonColor = ColorRGBA(1.0f, 1.0f, 1.0f, (IsSpectating ? 0.25f : 0.5f) * pUi->ButtonColorMul(&pPopupContext->m_SpectateButton));
	if(!pPopupContext->m_IsSpectating)
	{
		if(pUi->DoButton_PopupMenu(&pPopupContext->m_SpectateButton, Localize("Spectate"), &Container, FontSize, TEXTALIGN_MC, 0.0f, false, true, SpectateButtonColor))
		{
			if(IsSpectating)
			{
				pGameClient->m_Spectator.Spectate(SPEC_FREEVIEW);
				pScoreboard->Console()->ExecuteLine("say /spec", IConsole::CLIENT_ID_UNSPECIFIED);
			}
			else
			{
				if(OriginView.IsSpectating())
				{
					pGameClient->m_Spectator.Spectate(pPopupContext->m_ClientId);
				}
				else
				{
					// escape the name
					char aEscapedCommand[2 * MAX_NAME_LENGTH + 32];
					str_copy(aEscapedCommand, "say /spec \"");
					char *pDst = aEscapedCommand + str_length(aEscapedCommand);
					str_escape(&pDst, pClient->m_aName, aEscapedCommand + sizeof(aEscapedCommand));
					str_append(aEscapedCommand, "\"");

					pScoreboard->Console()->ExecuteLine(aEscapedCommand, IConsole::CLIENT_ID_UNSPECIFIED);
				}
			}
		}
	}

	return CUi::POPUP_KEEP_OPEN;
}

CUi::EPopupMenuFunctionResult CScoreboard::CMapTitlePopupContext::Render(void *pContext, CUIRect View, bool Active)
{
	CMapTitlePopupContext *pPopupContext = static_cast<CMapTitlePopupContext *>(pContext);
	CScoreboard *pScoreboard = pPopupContext->m_pScoreboard;

	pScoreboard->TextRender()->Text(View.x, View.y, pPopupContext->m_FontSize, pPopupContext->m_aDescription, View.w);

	return CUi::POPUP_KEEP_OPEN;
}
