#include <base/io.h>
#include <base/time.h>

#include <engine/graphics.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/storage.h>
#include <engine/textrender.h>

#include <generated/client_data.h>

#include <game/client/animstate.h>
#include <game/client/components/motd.h>
#include <game/client/components/statboard.h>
#include <game/client/gameclient.h>
#include <game/client/match_report_view.h>
#include <game/localization.h>

#include <algorithm>
#include <cinttypes>
#include <limits>
#include <optional>
#include <vector>

CStatboard::CStatboard()
{
	m_Active = false;
	m_ScreenshotTaken = false;
	m_ScreenshotTime = -1;
}

void CStatboard::OnReset()
{
	m_Active = false;
	m_ScreenshotTaken = false;
	m_ScreenshotTime = -1;
}

void CStatboard::OnRelease()
{
	m_Active = false;
}

void CStatboard::ConKeyStats(IConsole::IResult *pResult, void *pUserData)
{
	((CStatboard *)pUserData)->m_Active = pResult->GetInteger(0) != 0;
}

void CStatboard::OnConsoleInit()
{
	Console()->Register("+statboard", "", CFGFLAG_CLIENT, ConKeyStats, this, "Show stats");
}

bool CStatboard::IsActive() const
{
	return m_Active;
}

bool CStatboard::IsRenderable() const
{
	if(!IsActive())
		return false;
	const CSessionStatsState &Stats = GameClient()->SessionContext().Stats();

	int NumPlayers = 0;
	for(const auto *pInfo : GameClient()->Snap().m_apInfoByScore)
	{
		if(!pInfo || !Stats.Client(pInfo->m_ClientId).IsActive())
			continue;
		if(GameClient()->m_aClients[pInfo->m_ClientId].m_Team == TEAM_RED ||
			(GameClient()->IsTeamPlay() && GameClient()->m_aClients[pInfo->m_ClientId].m_Team == TEAM_BLUE))
			NumPlayers++;
	}
	return NumPlayers <= 32;
}

bool CStatboard::IsRenderable(const CRenderContext &Context) const
{
	if(!IsActive())
		return false;
	const CSessionStatsState &Stats = Context.m_Session.Stats();
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> *pClientsByScore = Presentation.ClientsByScore(Context.m_State.Id());
	if(pClientsByScore == nullptr)
		return false;
	const bool TeamPlay = Context.m_State.HasGameInfo() && (Context.m_State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	int NumPlayers = 0;
	for(const int ClientId : *pClientsByScore)
	{
		if(ClientId < 0)
			break;
		const CClientPresentation *pClient = Presentation.Client(Context.m_State.Id(), ClientId);
		if(pClient == nullptr || !Stats.Client(ClientId).IsActive())
			continue;
		if(pClient->m_Team == TEAM_RED || (TeamPlay && pClient->m_Team == TEAM_BLUE))
			++NumPlayers;
	}
	return NumPlayers <= 32;
}

void CStatboard::UpdateController()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if((g_Config.m_ClAutoStatboardScreenshot || g_Config.m_ClAutoCSV) && Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		if(m_ScreenshotTime < 0 && GameClient()->Snap().m_pGameInfoObj && GameClient()->Snap().m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_GAMEOVER)
			m_ScreenshotTime = time_get() + time_freq() * 3;
		if(m_ScreenshotTime > -1 && m_ScreenshotTime < time_get())
			m_Active = true;
		if(!m_ScreenshotTaken && m_ScreenshotTime > -1 && m_ScreenshotTime + time_freq() / 5 < time_get())
		{
			if(g_Config.m_ClAutoStatboardScreenshot)
				AutoStatScreenshot();
			if(g_Config.m_ClAutoCSV)
				AutoStatCSV();
			m_ScreenshotTaken = true;
		}
	}
}

void CStatboard::OnRender(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive)
		return;

	if(!IsRenderable(Context))
		return;

	// Either or, not both: while a server reports the running match, its numbers
	// are the authoritative version of everything the table below estimates, so
	// the live panel takes the place of the table instead of sitting on top of
	// it. This is where a live match belongs: in the game, not in a menu tab.
	if(const CStoredMatch *pLive = GameClient()->LiveStats(Context.m_Session.Id()))
		RenderLiveMatch(Context, *pLive);
	else
		RenderGlobalStats(Context);
}

void CStatboard::RenderLiveMatch(const CRenderContext &Context, const CStoredMatch &Live)
{
	const float StatboardWidth = 400 * 3.0f * Context.AspectRatio(Graphics()->ScreenAspect());
	const float StatboardHeight = 400 * 3.0f;
	Graphics()->MapScreenToSize(StatboardWidth, StatboardHeight);

	const float PanelWidth = 760.0f;
	const float PanelHeight = LiveMatchPanelHeight(Live);
	const float X = StatboardWidth / 2.0f - PanelWidth / 2.0f;
	const float Y = 200.0f;
	GameClient()->m_Menus.RenderBackdropRegion({X, Y, PanelWidth, PanelHeight});
	Graphics()->DrawRect(X, Y, PanelWidth, PanelHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 17.0f);
	RenderLiveMatchPanel(Live, X + 10.0f, Y + 10.0f, PanelWidth - 20.0f);
}

void CStatboard::RenderGlobalStats(const CRenderContext &Context)
{
	const CSessionStatsState &Stats = Context.m_Session.Stats();
	const CGameState &State = Context.m_State;
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(Context.m_Session.Id());
	const std::array<int, MAX_CLIENTS> *pClientsByScore = Presentation.ClientsByScore(State.Id());
	if(pClientsByScore == nullptr)
		return;
	const float StatboardWidth = 400 * 3.0f * Context.AspectRatio(Graphics()->ScreenAspect());
	const float StatboardHeight = 400 * 3.0f;
	float StatboardContentWidth = 260.0f;
	float StatboardContentHeight = 750.0f;

	std::array<int, MAX_CLIENTS> aPlayers;
	aPlayers.fill(-1);
	int NumPlayers = 0;
	const bool TeamPlay = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;

	// sort red or dm players by score
	for(const int ClientId : *pClientsByScore)
	{
		if(ClientId < 0)
			break;
		const CClientPresentation *pClient = Presentation.Client(State.Id(), ClientId);
		if(pClient == nullptr || !Stats.Client(ClientId).IsActive() || pClient->m_Team != TEAM_RED)
			continue;
		aPlayers[NumPlayers++] = ClientId;
	}

	// sort blue players by score after
	if(TeamPlay)
	{
		for(const int ClientId : *pClientsByScore)
		{
			if(ClientId < 0)
				break;
			const CClientPresentation *pClient = Presentation.Client(State.Id(), ClientId);
			if(pClient == nullptr || !Stats.Client(ClientId).IsActive() || pClient->m_Team != TEAM_BLUE)
				continue;
			aPlayers[NumPlayers++] = ClientId;
		}
	}

	// Dirty hack. Do not show scoreboard if there are more than 32 players
	// remove as soon as support of more than 32 players is required
	if(NumPlayers > 32)
		return;

	const bool GameWithFlags = State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_FLAGS) != 0;

	StatboardContentWidth += 7 * 85 + 95; // Suicides 95; other labels 85

	if(GameWithFlags)
		StatboardContentWidth += 150; // Grabs & Flags

	bool aDisplayWeapon[NUM_WEAPONS] = {false};
	for(int i = 0; i < NumPlayers; i++)
	{
		const CSessionClientStats *pStats = &Stats.Client(aPlayers[i]);
		for(int j = 0; j < NUM_WEAPONS; j++)
			aDisplayWeapon[j] = aDisplayWeapon[j] || pStats->m_aFragsWith[j] || pStats->m_aDeathsFrom[j];
	}
	for(bool DisplayWeapon : aDisplayWeapon)
		if(DisplayWeapon)
			StatboardContentWidth += 80;

	float x = StatboardWidth / 2 - StatboardContentWidth / 2;
	float y = 200.0f;

	Graphics()->MapScreenToSize(StatboardWidth, StatboardHeight);

	GameClient()->m_Menus.RenderBackdropRegion({x - 10.f, y - 10.f, StatboardContentWidth, StatboardContentHeight});
	Graphics()->DrawRect(x - 10.f, y - 10.f, StatboardContentWidth, StatboardContentHeight, ColorRGBA(0.0f, 0.0f, 0.0f, 0.5f), IGraphics::CORNER_ALL, 17.0f);

	int px = 325;

	TextRender()->Text(x + 10, y - 5, 22.0f, Localize("Name"), -1.0f);
	const char *apHeaders[] = {
		Localize("Frags"), Localize("Deaths"), Localize("Suicides"),
		Localize("Ratio"), Localize("Net"), Localize("FPM"),
		Localize("Spree"), Localize("Best"), Localize("Grabs")};
	for(int i = 0; i < 9; i++)
	{
		if(i == 2)
			px += 10.0f; // Suicides
		if(i == 8 && !GameWithFlags) // Don't draw "Grabs" in game with no flag
			continue;
		const float TextWidth = TextRender()->TextWidth(22.0f, apHeaders[i], -1, -1.0f);
		TextRender()->Text(x + px - TextWidth, y - 5, 22.0f, apHeaders[i], -1.0f);
		px += 85;
	}

	px -= 40;
	for(int i = 0; i < NUM_WEAPONS; i++)
	{
		if(!aDisplayWeapon[i])
			continue;
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(g_pData->m_Weapons.m_aId[i].m_pSpriteBody, ScaleX, ScaleY);
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_aSpriteWeapons[i]);
		Graphics()->QuadsBegin();
		if(i == 0)
			Graphics()->DrawSprite(x + px, y + 10, g_pData->m_Weapons.m_aId[i].m_VisualSize * 0.8f * ScaleX, g_pData->m_Weapons.m_aId[i].m_VisualSize * 0.8f * ScaleY);
		else
			Graphics()->DrawSprite(x + px, y + 10, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleX, g_pData->m_Weapons.m_aId[i].m_VisualSize * ScaleY);
		px += 80;
		Graphics()->QuadsEnd();
	}

	if(GameWithFlags)
	{
		Graphics()->TextureSet(GameClient()->m_GameSkin.m_SpriteFlagRed);
		float ScaleX, ScaleY;
		Graphics()->GetSpriteScale(SPRITE_FLAG_RED, ScaleX, ScaleY);
		Graphics()->QuadsBegin();
		Graphics()->QuadsSetRotation(0.78f);
		Graphics()->DrawSprite(x + px, y + 15, 48 * ScaleX, 48 * ScaleY);
		Graphics()->QuadsEnd();
	}

	y += 29.0f;

	float FontSize = 24.0f;
	float LineHeight = 50.0f;
	float TeeSizemod = 0.8f;
	float ContentLineOffset = LineHeight * 0.05f;

	if(NumPlayers > 16)
	{
		FontSize = 20.0f;
		LineHeight = 22.0f;
		TeeSizemod = 0.34f;
		ContentLineOffset = 0;
	}
	else if(NumPlayers > 14)
	{
		FontSize = 24.0f;
		LineHeight = 40.0f;
		TeeSizemod = 0.7f;
	}

	for(int j = 0; j < NumPlayers; j++)
	{
		const int ClientId = aPlayers[j];
		const CSessionClientStats *pStats = &Stats.Client(ClientId);
		const CClientPresentation *pClient = Presentation.Client(State.Id(), ClientId);
		dbg_assert(pClient != nullptr, "statboard client presentation missing");

		if(State.LocalClientId() == ClientId || (Context.m_View.IsSpectating() && ClientId == Context.m_View.SpectatorId()))
		{
			// background so it's easy to find the local player
			Graphics()->DrawRect(x - 10, y + ContentLineOffset / 2, StatboardContentWidth, LineHeight - ContentLineOffset, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), IGraphics::CORNER_NONE, 0.0f);
		}

		CTeeRenderInfo Teeinfo = pClient->m_BaseRenderInfo;
		Teeinfo.m_Size *= TeeSizemod;

		const CAnimState *pIdleState = CAnimState::GetIdle();
		vec2 OffsetToMid;
		CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &Teeinfo, OffsetToMid);
		vec2 TeeRenderPos(x + Teeinfo.m_Size / 2, y + LineHeight / 2.0f + OffsetToMid.y);

		RenderTools()->RenderTee(pIdleState, &Teeinfo, EMOTE_NORMAL, vec2(1, 0), TeeRenderPos);

		char aBuf[128];
		CTextCursor Cursor;
		Cursor.SetPosition(vec2(x + 64, y + (LineHeight * 0.95f - FontSize) / 2.f));
		Cursor.m_FontSize = FontSize;
		Cursor.m_Flags |= TEXTFLAG_STOP_AT_END;
		Cursor.m_LineWidth = 220;
		TextRender()->TextEx(&Cursor, pClient->m_aName, -1);

		px = 325;

		// FRAGS
		{
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_Frags);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// DEATHS
		{
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_Deaths);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// SUICIDES
		{
			px += 10;
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_Suicides);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// RATIO
		{
			if(pStats->m_Deaths == 0)
				str_copy(aBuf, "--");
			else
				str_format(aBuf, sizeof(aBuf), "%.2f", (float)(pStats->m_Frags) / pStats->m_Deaths);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// NET
		{
			str_format(aBuf, sizeof(aBuf), "%+d", pStats->m_Frags - pStats->m_Deaths);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// FPM
		{
			const float Fpm = pStats->GetFPM(Context.m_Time.m_GameTick, Context.m_Time.m_GameTickSpeed);
			str_format(aBuf, sizeof(aBuf), "%.1f", Fpm);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// SPREE
		{
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_CurrentSpree);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// BEST SPREE
		{
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_BestSpree);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// GRABS
		if(GameWithFlags)
		{
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_FlagGrabs);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 85;
		}
		// WEAPONS
		px -= 40;
		for(int i = 0; i < NUM_WEAPONS; i++)
		{
			if(!aDisplayWeapon[i])
				continue;

			str_format(aBuf, sizeof(aBuf), "%d/%d", pStats->m_aFragsWith[i], pStats->m_aDeathsFrom[i]);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x + px - TextWidth / 2, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
			px += 80;
		}
		// FLAGS
		if(GameWithFlags)
		{
			str_format(aBuf, sizeof(aBuf), "%d", pStats->m_FlagCaptures);
			const float TextWidth = TextRender()->TextWidth(FontSize, aBuf, -1, -1.0f);
			TextRender()->Text(x - TextWidth + px, y + (LineHeight * 0.95f - FontSize) / 2.f, FontSize, aBuf, -1.0f);
		}
		y += LineHeight;
	}
}

float CStatboard::LiveMatchPanelHeight(const CStoredMatch &Live) const
{
	const int Rows = std::min<int>(Live.m_Report.m_vParticipants.size(), MAX_LIVE_ROWS);
	return 34.0f + 26.0f + Rows * 24.0f + 10.0f;
}

void CStatboard::RenderLiveMatchPanel(const CStoredMatch &Live, float X, float Y, float Width)
{
	const CMatchReport &Report = Live.m_Report;

	char aDuration[64];
	FormatMatchDuration(Report.m_DurationTicks, Report.m_TickRate, aDuration, sizeof(aDuration));
	char aTitle[320];
	str_format(aTitle, sizeof(aTitle), "%s  ·  %s  ·  %s", Report.m_MapName.c_str(), Report.m_ModeId.c_str(), aDuration);
	TextRender()->Text(X, Y, 22.0f, aTitle, Width);
	// Says where these numbers come from: the server, not this client's count.
	TextRender()->TextColor(ColorRGBA(0.30f, 0.62f, 1.0f, 1.0f));
	const char *pSource = Localize("Live from server");
	TextRender()->Text(X + Width - TextRender()->TextWidth(18.0f, pSource, -1, -1.0f), Y + 2.0f, 18.0f, pSource, -1.0f);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	Y += 34.0f;

	// Rank, name and clan on the left, the numbers right-aligned in fixed
	// columns so that they line up down the panel.
	const float NameWidth = Width - 60.0f - 4 * 110.0f;
	const auto Row = [&](float RowY, const char *pRank, const char *pName, const char *const *ppValues, float FontSize) {
		TextRender()->Text(X + 4.0f, RowY, FontSize, pRank, 56.0f);
		TextRender()->Text(X + 60.0f, RowY, FontSize, pName, NameWidth);
		for(int Column = 0; Column < 4; ++Column)
		{
			const float Right = X + 60.0f + NameWidth + (Column + 1) * 110.0f;
			TextRender()->Text(Right - TextRender()->TextWidth(FontSize, ppValues[Column], -1, -1.0f), RowY, FontSize, ppValues[Column], -1.0f);
		}
	};

	const char *apHeaders[4] = {Localize("Score"), Localize("Frags"), Localize("Deaths"), Localize("Accuracy")};
	TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, 0.6f));
	Row(Y, Localize("#"), Localize("Name"), apHeaders, 18.0f);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
	Y += 26.0f;

	std::vector<const CMatchParticipant *> vpRanked;
	vpRanked.reserve(Report.m_vParticipants.size());
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
		vpRanked.push_back(&Participant);
	std::stable_sort(vpRanked.begin(), vpRanked.end(), [&](const CMatchParticipant *pLeft, const CMatchParticipant *pRight) {
		const CMatchStanding *pLeftStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, pLeft->m_ParticipantId);
		const CMatchStanding *pRightStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, pRight->m_ParticipantId);
		const int LeftRank = pLeftStanding == nullptr ? std::numeric_limits<int>::max() : pLeftStanding->m_Rank;
		const int RightRank = pRightStanding == nullptr ? std::numeric_limits<int>::max() : pRightStanding->m_Rank;
		if(LeftRank != RightRank)
			return LeftRank < RightRank;
		return ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, pLeft->m_ParticipantId, "score").value_or(0) > ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, pRight->m_ParticipantId, "score").value_or(0);
	});

	const int Rows = std::min<int>(vpRanked.size(), MAX_LIVE_ROWS);
	for(int Index = 0; Index < Rows; ++Index)
	{
		const CMatchParticipant &Participant = *vpRanked[Index];
		const bool Local = Live.m_LocalParticipantId.has_value() && *Live.m_LocalParticipantId == Participant.m_ParticipantId;
		if(Local)
			Graphics()->DrawRect(X - 4.0f, Y - 2.0f, Width + 8.0f, 24.0f, ColorRGBA(0.30f, 0.62f, 1.0f, 0.18f), IGraphics::CORNER_ALL, 3.0f);

		char aRank[16] = "-";
		if(const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId))
			str_format(aRank, sizeof(aRank), "%d", pStanding->m_Rank);
		char aaValues[4][32];
		const char *apValues[4];
		for(int Column = 0; Column < 4; ++Column)
			apValues[Column] = aaValues[Column];
		static const char *s_apSuffixes[3] = {"score", "kills", "deaths"};
		for(int Column = 0; Column < 3; ++Column)
		{
			const std::optional<int64_t> Value = ReportMetric(Report, EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId, s_apSuffixes[Column]);
			if(Value.has_value())
				str_format(aaValues[Column], sizeof(aaValues[Column]), "%" PRId64, *Value);
			else
				str_copy(aaValues[Column], "-");
		}
		const CMatchCombatStats Combat = BuildMatchCombatStats(Report, Participant.m_ParticipantId);
		FormatMatchAccuracy(Combat.m_Total.m_Hits, Combat.m_Total.m_Shots, aaValues[3], sizeof(aaValues[3]));

		std::string Name = Participant.m_DisplayName;
		if(!Participant.m_Clan.empty())
			Name += "  " + Participant.m_Clan;
		Row(Y, aRank, Name.c_str(), apValues, 20.0f);
		Y += 24.0f;
	}
}

void CStatboard::AutoStatScreenshot()
{
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
		Client()->AutoStatScreenshot_Start();
}

void CStatboard::AutoStatCSV()
{
	if(Client()->State() != IClient::STATE_DEMOPLAYBACK)
	{
		char aDate[20], aFilename[IO_MAX_PATH_LENGTH];
		str_timestamp(aDate, sizeof(aDate));
		str_format(aFilename, sizeof(aFilename), "screenshots/auto/stats_%s.csv", aDate);
		IOHANDLE File = Storage()->OpenFile(aFilename, IOFLAG_WRITE, IStorage::TYPE_SAVE);

		if(File)
		{
			char aStats[1024 * (VANILLA_MAX_CLIENTS + 1)];
			FormatStats(aStats, sizeof(aStats));
			io_write(File, aStats, str_length(aStats));
			io_close(File);
		}

		Client()->AutoCSV_Start();
	}
}

std::string CStatboard::ReplaceCommata(const char *pStr)
{
	if(!str_find(pStr, ","))
		return pStr;

	char aOutbuf[256] = "";
	for(int i = 0, Skip = 0; i < 64; i++)
	{
		if(pStr[i] == ',')
		{
			aOutbuf[i + Skip++] = '%';
			aOutbuf[i + Skip++] = '2';
			aOutbuf[i + Skip] = 'C';
		}
		else
			aOutbuf[i + Skip] = pStr[i];
	}
	return aOutbuf;
}

void CStatboard::FormatStats(char *pDest, size_t DestSize)
{
	const CSessionId SessionId = GameClient()->SessionContext().Id();
	const CSessionStatsState &Stats = GameClient()->SessionContext().Stats();
	// server stats
	const CServerInfo &CurrentServerInfo = Client()->ServerInfo(SessionId);

	char aServerStats[1024];
	str_format(aServerStats, sizeof(aServerStats), "Servername,Game-type,Map\n%s,%s,%s", ReplaceCommata(CurrentServerInfo.m_aName).c_str(), ReplaceCommata(CurrentServerInfo.m_aGameType).c_str(), ReplaceCommata(CurrentServerInfo.m_aMap).c_str());

	// player stats

	// sort players
	const CNetObj_PlayerInfo *apPlayers[MAX_CLIENTS] = {nullptr};
	int NumPlayers = 0;

	// sort red or dm players by score
	for(const auto *pInfo : GameClient()->Snap().m_apInfoByScore)
	{
		if(!pInfo || !Stats.Client(pInfo->m_ClientId).IsActive() || GameClient()->m_aClients[pInfo->m_ClientId].m_Team != TEAM_RED)
			continue;
		apPlayers[NumPlayers] = pInfo;
		NumPlayers++;
	}

	// sort blue players by score after
	if(GameClient()->IsTeamPlay())
	{
		for(const auto *pInfo : GameClient()->Snap().m_apInfoByScore)
		{
			if(!pInfo || !Stats.Client(pInfo->m_ClientId).IsActive() || GameClient()->m_aClients[pInfo->m_ClientId].m_Team != TEAM_BLUE)
				continue;
			apPlayers[NumPlayers] = pInfo;
			NumPlayers++;
		}
	}

	char aPlayerStats[1024 * VANILLA_MAX_CLIENTS] = "Local-player,Team,Name,Clan,Score,Frags,Deaths,Suicides,F/D-ratio,Net,FPM,Spree,Best,Hammer-F/D,Gun-F/D,Shotgun-F/D,Grenade-F/D,Laser-F/D,Ninja-F/D,GameWithFlags,Flag-grabs,Flag-captures\n";
	for(int i = 0; i < NumPlayers; i++)
	{
		const CNetObj_PlayerInfo *pInfo = apPlayers[i];
		const CSessionClientStats *pStats = &Stats.Client(pInfo->m_ClientId);

		// Pre-formatting

		// Weapons frags/deaths
		char aWeaponFD[64 * NUM_WEAPONS];
		for(int j = 0; j < NUM_WEAPONS; j++)
		{
			if(j == 0)
				str_format(aWeaponFD, sizeof(aWeaponFD), "%d/%d", pStats->m_aFragsWith[j], pStats->m_aDeathsFrom[j]);
			else
				str_format(aWeaponFD, sizeof(aWeaponFD), "%s,%d/%d", aWeaponFD, pStats->m_aFragsWith[j], pStats->m_aDeathsFrom[j]);
		}

		// Frag/Death ratio
		float KillRatio = 0.0f;
		if(pStats->m_Deaths != 0)
			KillRatio = (float)(pStats->m_Frags) / pStats->m_Deaths;

		// Local player
		bool LocalPlayer = (GameClient()->Snap().m_LocalClientId == pInfo->m_ClientId || (GameClient()->Snap().m_SpecInfo.m_Active && pInfo->m_ClientId == GameClient()->Snap().m_SpecInfo.m_SpectatorId));

		// Game with flags
		bool GameWithFlags = (GameClient()->Snap().m_pGameInfoObj && GameClient()->Snap().m_pGameInfoObj->m_GameFlags & GAMEFLAG_FLAGS);

		char aBuf[1024];
		str_format(aBuf, sizeof(aBuf), "%d,%d,%s,%s,%d,%d,%d,%d,%.2f,%i,%.1f,%d,%d,%s,%d,%d,%d\n",
			LocalPlayer ? 1 : 0, // Local player
			GameClient()->m_aClients[pInfo->m_ClientId].m_Team, // Team
			ReplaceCommata(GameClient()->m_aClients[pInfo->m_ClientId].m_aName).c_str(), // Name
			ReplaceCommata(GameClient()->m_aClients[pInfo->m_ClientId].m_aClan).c_str(), // Clan
			std::clamp(pInfo->m_Score, -999, 999), // Score
			pStats->m_Frags, // Frags
			pStats->m_Deaths, // Deaths
			pStats->m_Suicides, // Suicides
			KillRatio, // Kill ratio
			pStats->m_Frags - pStats->m_Deaths, // Net
			pStats->GetFPM(Client()->GameTick(SessionId, GameClient()->ActiveConnection()), Client()->GameTickSpeed()), // FPM
			pStats->m_CurrentSpree, // CurSpree
			pStats->m_BestSpree, // BestSpree
			aWeaponFD, // WeaponFD
			GameWithFlags ? 1 : 0, // GameWithFlags
			pStats->m_FlagGrabs, // Flag grabs
			pStats->m_FlagCaptures); // Flag captures

		str_append(aPlayerStats, aBuf);
	}

	str_format(pDest, DestSize, "%s\n\n%s", aServerStats, aPlayerStats);
}
