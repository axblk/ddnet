/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */

#include "chat.h"

#include <base/color.h>
#include <base/io.h>
#include <base/log.h>
#include <base/log_color.h>
#include <base/time.h>

#include <engine/editor.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/shared/config.h>
#include <engine/shared/csv.h>
#include <engine/textrender.h>

#include <generated/protocol.h>
#include <generated/protocol7.h>

#include <game/client/animstate.h>
#include <game/client/components/censor.h>
#include <game/client/components/scoreboard.h>
#include <game/client/components/skins.h>
#include <game/client/components/sounds.h>
#include <game/client/gameclient.h>
#include <game/localization.h>

char CChat::ms_aDisplayText[MAX_LINE_LENGTH] = "";

CChat::CCachedLine::CCachedLine()
{
	m_TextContainerIndex.Reset();
}

void CChat::CCachedLine::Invalidate(CChat &This)
{
	This.TextRender()->DeleteTextContainer(m_TextContainerIndex);
	This.Graphics()->DeleteQuadContainer(m_QuadContainerIndex);
	m_Revision = 0;
	m_StateId = CGameStateId();
	m_ViewId = CGameViewId();
	m_Viewport = {};
	m_OutputCacheKey = 0;
	m_ScoreboardOpen = false;
	m_ShowLargeArea = false;
	m_YOffset = -1.0f;
	m_TextYOffset = 0.0f;
}

void CChat::CCachedLine::Reset(CChat &This)
{
	Invalidate(This);
	m_LineId = 0;
	m_pManagedTeeRenderInfo = nullptr;
}

CChat::CChat()
{
	m_Mode = MODE_NONE;

	m_Input.SetCalculateOffsetCallback([this]() { return m_IsInputCensored; });
	m_Input.SetDisplayTextCallback([this](char *pStr, size_t NumChars) {
		m_IsInputCensored = false;
		if(
			g_Config.m_ClStreamerMode &&
			(str_startswith(pStr, "/login ") ||
				str_startswith(pStr, "/register ") ||
				str_startswith(pStr, "/code ") ||
				str_startswith(pStr, "/timeout ") ||
				str_startswith(pStr, "/save ") ||
				str_startswith(pStr, "/load ")))
		{
			bool Censor = false;
			const size_t NumLetters = std::min(NumChars, sizeof(ms_aDisplayText) - 1);
			for(size_t i = 0; i < NumLetters; ++i)
			{
				if(Censor)
					ms_aDisplayText[i] = '*';
				else
					ms_aDisplayText[i] = pStr[i];
				if(pStr[i] == ' ')
				{
					Censor = true;
					m_IsInputCensored = true;
				}
			}
			ms_aDisplayText[NumLetters] = '\0';
			return ms_aDisplayText;
		}
		return pStr;
	});
}

CChat::CSessionCache &CChat::SessionCache(CSessionId SessionId)
{
	const auto It = std::find_if(m_vSessionCaches.begin(), m_vSessionCaches.end(), [SessionId](const CSessionCache &Cache) { return Cache.m_SessionId == SessionId; });
	if(It != m_vSessionCaches.end())
		return *It;
	m_vSessionCaches.emplace_back();
	m_vSessionCaches.back().m_SessionId = SessionId;
	return m_vSessionCaches.back();
}

CChat::CCachedLine &CChat::CachedLine(CSessionId SessionId, uint64_t LineId)
{
	CCachedLine &Cached = SessionCache(SessionId).m_aLines[LineId % CSessionChatState::MAX_LINES];
	if(Cached.m_LineId != LineId)
	{
		Cached.Reset(*this);
		Cached.m_LineId = LineId;
	}
	return Cached;
}

void CChat::RebuildChat()
{
	for(CSessionCache &Session : m_vSessionCaches)
		for(CCachedLine &Line : Session.m_aLines)
			Line.Invalidate(*this);
}

void CChat::ClearLines(CSessionId SessionId)
{
	CGameSessionContext *pSession = GameClient()->FindSessionContext(SessionId);
	if(pSession != nullptr)
		pSession->Chat().ClearLines();
	const auto It = std::find_if(m_vSessionCaches.begin(), m_vSessionCaches.end(), [SessionId](const CSessionCache &Cache) { return Cache.m_SessionId == SessionId; });
	if(It == m_vSessionCaches.end())
		return;
	for(CCachedLine &Line : It->m_aLines)
		Line.Reset(*this);
}

void CChat::OnWindowResize()
{
	RebuildChat();
}

void CChat::Reset()
{
	for(CSessionCache &Session : m_vSessionCaches)
		for(CCachedLine &Line : Session.m_aLines)
			Line.Reset(*this);
	m_vSessionCaches.clear();

	m_Show = false;
	m_ShowViewId = CGameViewId();
	m_ShowSessionId = CSessionId();
	m_ShowStateId = CGameStateId();
	m_CompletionUsed = false;
	m_CompletionChosen = -1;
	m_aCompletionBuffer[0] = 0;
	m_PlaceholderOffset = 0;
	m_PlaceholderLength = 0;
	m_pHistoryEntry = nullptr;
	m_IsInputCensored = false;
	m_EditingNewLine = true;
	m_aCurrentInputText[0] = '\0';
	DisableMode();

	for(int64_t &LastSoundPlayed : m_aLastSoundPlayed)
		LastSoundPlayed = 0;
}

void CChat::ResetSession(CSessionId SessionId)
{
	if(m_InputSessionId == SessionId)
	{
		DisableMode();
	}
	if(m_ShowSessionId == SessionId)
	{
		m_Show = false;
		m_ShowViewId = CGameViewId();
		m_ShowSessionId = CSessionId();
		m_ShowStateId = CGameStateId();
	}
	CGameSessionContext *pSession = GameClient()->FindSessionContext(SessionId);
	if(pSession != nullptr)
		pSession->Chat().Reset();
	const auto It = std::find_if(m_vSessionCaches.begin(), m_vSessionCaches.end(), [SessionId](const CSessionCache &Cache) { return Cache.m_SessionId == SessionId; });
	if(It == m_vSessionCaches.end())
		return;
	for(CCachedLine &Line : It->m_aLines)
		Line.Reset(*this);
	m_vSessionCaches.erase(It);
}

void CChat::OnRelease()
{
	m_Show = false;
}

void CChat::OnStateChange(int NewState, int OldState)
{
	if(OldState <= IClient::STATE_CONNECTING)
	{
		m_Show = false;
		DisableMode();
	}
}

void CChat::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->SendChat(0, pResult->GetString(0));
}

void CChat::ConSayTeam(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->SendChat(1, pResult->GetString(0));
}

void CChat::ConChat(IConsole::IResult *pResult, void *pUserData)
{
	const char *pMode = pResult->GetString(0);
	if(str_comp(pMode, "all") == 0)
		((CChat *)pUserData)->EnableMode(0);
	else if(str_comp(pMode, "team") == 0)
		((CChat *)pUserData)->EnableMode(1);
	else
		log_error("chat", "expected all or team as mode");

	if(pResult->GetString(1)[0] || g_Config.m_ClChatReset)
		((CChat *)pUserData)->m_Input.Set(pResult->GetString(1));
}

void CChat::ConShowChat(IConsole::IResult *pResult, void *pUserData)
{
	CChat *pChat = (CChat *)pUserData;
	pChat->m_Show = pResult->GetInteger(0) != 0;
	if(pChat->m_Show)
	{
		const CGameView &View = pChat->GameClient()->LegacyGameView();
		pChat->m_ShowViewId = View.Id();
		pChat->m_ShowSessionId = View.SessionId();
		pChat->m_ShowStateId = View.StateId();
	}
}

void CChat::ConEcho(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->Echo(pResult->GetString(0));
}

void CChat::ConClearChat(IConsole::IResult *pResult, void *pUserData)
{
	CChat *pChat = (CChat *)pUserData;
	pChat->ClearLines(pChat->GameClient()->SessionContext().Id());
}

void CChat::ConchainChatOld(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	((CChat *)pUserData)->RebuildChat();
}

void CChat::ConchainChatFontSize(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	CChat *pChat = (CChat *)pUserData;
	pChat->EnsureCoherentWidth();
	pChat->RebuildChat();
}

void CChat::ConchainChatWidth(IConsole::IResult *pResult, void *pUserData, IConsole::FCommandCallback pfnCallback, void *pCallbackUserData)
{
	pfnCallback(pResult, pCallbackUserData);
	CChat *pChat = (CChat *)pUserData;
	pChat->EnsureCoherentFontSize();
	pChat->RebuildChat();
}

void CChat::Echo(const char *pString)
{
	CGameSessionContext &Session = GameClient()->SessionContext();
	AddLine(Session, GameClient()->GameState(Client()->ActiveConnection()), time(), Client()->IsDemoPlayback(), true, CLIENT_MSG, 0, pString);
}

void CChat::OnConsoleInit()
{
	Console()->Register("say", "r[message]", CFGFLAG_CLIENT, ConSay, this, "Say in chat");
	Console()->Register("say_team", "r[message]", CFGFLAG_CLIENT, ConSayTeam, this, "Say in team chat");
	Console()->Register("chat", "s['team'|'all'] ?r[message]", CFGFLAG_CLIENT, ConChat, this, "Enable chat with all/team mode");
	Console()->Register("+show_chat", "", CFGFLAG_CLIENT, ConShowChat, this, "Show chat");
	Console()->Register("echo", "r[message]", CFGFLAG_CLIENT | CFGFLAG_STORE, ConEcho, this, "Echo the text in chat window");
	Console()->Register("clear_chat", "", CFGFLAG_CLIENT | CFGFLAG_STORE, ConClearChat, this, "Clear chat messages");
}

void CChat::OnInit()
{
	Reset();
	Console()->Chain("cl_chat_old", ConchainChatOld, this);
	Console()->Chain("cl_chat_size", ConchainChatFontSize, this);
	Console()->Chain("cl_chat_width", ConchainChatWidth, this);
}

bool CChat::OnInput(const IInput::CEvent &Event)
{
	if(m_Mode == MODE_NONE)
		return false;
	CGameSessionContext *pInputSession = GameClient()->FindSessionContext(m_InputSessionId);
	CGameState *pInputState = pInputSession != nullptr ? pInputSession->GameStates().Find(m_InputStateId) : nullptr;
	if(pInputState == nullptr)
	{
		DisableMode();
		return true;
	}
	const CSessionPresentation &Presentation = GameClient()->SessionPresentation(m_InputSessionId);
	const std::vector<CSessionChatState::CCommand> &vServerCommands = pInputSession->Chat().SortedCommands();

	if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		DisableMode();
		GameClient()->OnRelease();
		if(g_Config.m_ClChatReset)
		{
			m_Input.Clear();
			m_pHistoryEntry = nullptr;
		}
	}
	else if(Event.m_Flags & IInput::FLAG_PRESS && (Event.m_Key == KEY_RETURN || Event.m_Key == KEY_KP_ENTER))
	{
		SendChatQueued(m_Input.GetString());
		m_pHistoryEntry = nullptr;
		DisableMode();
		GameClient()->OnRelease();
		m_Input.Clear();
	}
	if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_TAB)
	{
		const bool ShiftPressed = Input()->ShiftIsPressed();

		// fill the completion buffer
		if(!m_CompletionUsed)
		{
			const char *pCursor = m_Input.GetString() + m_Input.GetCursorOffset();
			for(size_t Count = 0; Count < m_Input.GetCursorOffset() && *(pCursor - 1) != ' '; --pCursor, ++Count)
				;
			m_PlaceholderOffset = pCursor - m_Input.GetString();

			for(m_PlaceholderLength = 0; *pCursor && *pCursor != ' '; ++pCursor)
				++m_PlaceholderLength;

			str_truncate(m_aCompletionBuffer, sizeof(m_aCompletionBuffer), m_Input.GetString() + m_PlaceholderOffset, m_PlaceholderLength);
		}

		if(!m_CompletionUsed && m_aCompletionBuffer[0] != '/')
		{
			// Create the completion list of player names through which the player can iterate
			const char *PlayerName, *FoundInput;
			m_PlayerCompletionListLength = 0;
			const std::array<int, MAX_CLIENTS> *pClientsByName = Presentation.ClientsByName(m_InputStateId);
			if(pClientsByName != nullptr)
			{
				for(int ClientId : *pClientsByName)
				{
					if(ClientId < 0)
						break;
					const CClientPresentation *pClient = Presentation.Client(m_InputStateId, ClientId);
					if(pClient == nullptr || !pClient->m_Active)
						continue;
					PlayerName = pClient->m_aName;
					FoundInput = str_utf8_find_nocase(PlayerName, m_aCompletionBuffer);
					if(FoundInput != nullptr)
					{
						m_aPlayerCompletionList[m_PlayerCompletionListLength].m_ClientId = ClientId;
						// The score for suggesting a player name is determined by the distance of the search input to the beginning of the player name
						m_aPlayerCompletionList[m_PlayerCompletionListLength].m_Score = (int)(FoundInput - PlayerName);
						m_PlayerCompletionListLength++;
					}
				}
			}
			std::stable_sort(m_aPlayerCompletionList, m_aPlayerCompletionList + m_PlayerCompletionListLength,
				[](const CRateablePlayer &Player1, const CRateablePlayer &Player2) -> bool {
					return Player1.m_Score < Player2.m_Score;
				});
		}

		if(m_aCompletionBuffer[0] == '/' && !vServerCommands.empty())
		{
			const CSessionChatState::CCommand *pCompletionCommand = nullptr;

			const size_t NumCommands = vServerCommands.size();

			if(ShiftPressed && m_CompletionUsed)
				m_CompletionChosen--;
			else if(!ShiftPressed)
				m_CompletionChosen++;
			m_CompletionChosen = (m_CompletionChosen + 2 * NumCommands) % (2 * NumCommands);

			m_CompletionUsed = true;

			const char *pCommandStart = m_aCompletionBuffer + 1;
			for(size_t i = 0; i < 2 * NumCommands; ++i)
			{
				int SearchType;
				int Index;

				if(ShiftPressed)
				{
					SearchType = ((m_CompletionChosen - i + 2 * NumCommands) % (2 * NumCommands)) / NumCommands;
					Index = (m_CompletionChosen - i + NumCommands) % NumCommands;
				}
				else
				{
					SearchType = ((m_CompletionChosen + i) % (2 * NumCommands)) / NumCommands;
					Index = (m_CompletionChosen + i) % NumCommands;
				}

				const CSessionChatState::CCommand &Command = vServerCommands[Index];

				if(str_startswith_nocase(Command.m_Name.c_str(), pCommandStart))
				{
					pCompletionCommand = &Command;
					m_CompletionChosen = Index + SearchType * NumCommands;
					break;
				}
			}

			// insert the command
			if(pCompletionCommand)
			{
				char aBuf[MAX_LINE_LENGTH];
				// add part before the name
				str_truncate(aBuf, sizeof(aBuf), m_Input.GetString(), m_PlaceholderOffset);

				// add the command
				str_append(aBuf, "/");
				str_append(aBuf, pCompletionCommand->m_Name.c_str());

				// add separator
				const char *pSeparator = pCompletionCommand->m_Params.empty() ? "" : " ";
				str_append(aBuf, pSeparator);

				// add part after the name
				str_append(aBuf, m_Input.GetString() + m_PlaceholderOffset + m_PlaceholderLength);

				m_PlaceholderLength = str_length(pSeparator) + pCompletionCommand->m_Name.length() + 1;
				m_Input.Set(aBuf);
				m_Input.SetCursorOffset(m_PlaceholderOffset + m_PlaceholderLength);
			}
		}
		else
		{
			// find next possible name
			const char *pCompletionString = nullptr;
			if(m_PlayerCompletionListLength > 0)
			{
				// We do this in a loop, if a player left the game during the repeated pressing of Tab, they are skipped
				for(int i = 0; i < m_PlayerCompletionListLength; ++i)
				{
					if(ShiftPressed && m_CompletionUsed)
					{
						m_CompletionChosen--;
					}
					else if(!ShiftPressed)
					{
						m_CompletionChosen++;
					}
					if(m_CompletionChosen < 0)
					{
						m_CompletionChosen += m_PlayerCompletionListLength;
					}
					m_CompletionChosen %= m_PlayerCompletionListLength;
					m_CompletionUsed = true;

					const CClientPresentation *pCompletionClient = Presentation.Client(m_InputStateId, m_aPlayerCompletionList[m_CompletionChosen].m_ClientId);
					if(pCompletionClient == nullptr || !pCompletionClient->m_Active)
					{
						continue;
					}

					pCompletionString = pCompletionClient->m_aName;
					break;
				}
			}

			// insert the name
			if(pCompletionString)
			{
				char aBuf[MAX_LINE_LENGTH];
				// add part before the name
				str_truncate(aBuf, sizeof(aBuf), m_Input.GetString(), m_PlaceholderOffset);

				// quote the name
				char aQuoted[128];
				if(m_Input.GetString()[0] == '/' && (str_find(pCompletionString, " ") || str_find(pCompletionString, "\"")))
				{
					// escape the name
					str_copy(aQuoted, "\"");
					char *pDst = aQuoted + str_length(aQuoted);
					str_escape(&pDst, pCompletionString, aQuoted + sizeof(aQuoted));
					str_append(aQuoted, "\"");

					pCompletionString = aQuoted;
				}

				// add the name
				str_append(aBuf, pCompletionString);

				// add separator
				const char *pSeparator = "";
				if(*(m_Input.GetString() + m_PlaceholderOffset + m_PlaceholderLength) != ' ')
					pSeparator = m_PlaceholderOffset == 0 ? ": " : " ";
				else if(m_PlaceholderOffset == 0)
					pSeparator = ":";
				if(*pSeparator)
					str_append(aBuf, pSeparator);

				// add part after the name
				str_append(aBuf, m_Input.GetString() + m_PlaceholderOffset + m_PlaceholderLength);

				m_PlaceholderLength = str_length(pSeparator) + str_length(pCompletionString);
				m_Input.Set(aBuf);
				m_Input.SetCursorOffset(m_PlaceholderOffset + m_PlaceholderLength);
			}
		}
	}
	else
	{
		// reset name completion process
		if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key != KEY_TAB && Event.m_Key != KEY_LSHIFT && Event.m_Key != KEY_RSHIFT)
		{
			m_CompletionChosen = -1;
			m_CompletionUsed = false;
		}

		m_Input.ProcessInput(Event);
	}

	if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_UP)
	{
		if(m_EditingNewLine)
		{
			str_copy(m_aCurrentInputText, m_Input.GetString());
			m_EditingNewLine = false;
		}

		if(m_pHistoryEntry)
		{
			CHistoryEntry *pTest = m_History.Prev(m_pHistoryEntry);

			if(pTest)
				m_pHistoryEntry = pTest;
		}
		else
		{
			m_pHistoryEntry = m_History.Last();
		}

		if(m_pHistoryEntry)
			m_Input.Set(m_pHistoryEntry->m_aText);
	}
	else if(Event.m_Flags & IInput::FLAG_PRESS && Event.m_Key == KEY_DOWN)
	{
		if(m_pHistoryEntry)
			m_pHistoryEntry = m_History.Next(m_pHistoryEntry);

		if(m_pHistoryEntry)
		{
			m_Input.Set(m_pHistoryEntry->m_aText);
		}
		else if(!m_EditingNewLine)
		{
			m_Input.Set(m_aCurrentInputText);
			m_EditingNewLine = true;
		}
	}

	return true;
}

void CChat::EnableMode(int Team)
{
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return;

	if(m_Mode == MODE_NONE)
	{
		const CGameView &View = GameClient()->LegacyGameView();
		CGameSessionContext *pSession = GameClient()->FindSessionContext(View.SessionId());
		CGameState *pState = pSession != nullptr ? pSession->GameStates().Find(View.StateId()) : nullptr;
		if(pSession == nullptr || pState == nullptr || pSession->Id() != Client()->NetworkSessionId())
			return;
		m_InputViewId = View.Id();
		m_InputSessionId = pSession->Id();
		m_InputStateId = pState->Id();
		m_InputStreamId = pState->StreamId();
		if(Team)
			m_Mode = MODE_TEAM;
		else
			m_Mode = MODE_ALL;

		m_CompletionChosen = -1;
		m_CompletionUsed = false;
		m_Input.Activate(EInputPriority::CHAT);
	}
}

void CChat::DisableMode()
{
	if(m_Mode != MODE_NONE)
	{
		m_Mode = MODE_NONE;
		m_Input.Deactivate();
		m_InputViewId = CGameViewId();
		m_InputSessionId = CSessionId();
		m_InputStateId = CGameStateId();
		m_InputStreamId = CStreamId();
	}
}

void CChat::HandleMessage(CGameSessionContext &Session, const CGameState &State, int64_t Now, bool SuppressEvents, bool IsDemoPlayback, bool ApplicationEffects, int MsgType, void *pRawMsg)
{
	if(SuppressEvents)
		return;

	if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;

		/*
		if(g_Config.m_ClCensorChat)
		{
			char aMessage[MAX_LINE_LENGTH];
			str_copy(aMessage, pMsg->m_pMessage);
			GameClient()->m_Censor.CensorMessage(aMessage);
			AddLine(pMsg->m_ClientId, pMsg->m_Team, aMessage);
		}
		else
			AddLine(pMsg->m_ClientId, pMsg->m_Team, pMsg->m_pMessage);
		*/

		AddLine(Session, State, Now, IsDemoPlayback, ApplicationEffects, pMsg->m_ClientId, pMsg->m_Team, pMsg->m_pMessage);

		if(!IsDemoPlayback && pMsg->m_ClientId == SERVER_MSG)
		{
			StoreSave(pMsg->m_pMessage, Session);
		}
	}
	else if(MsgType == NETMSGTYPE_SV_COMMANDINFO)
	{
		CNetMsg_Sv_CommandInfo *pMsg = (CNetMsg_Sv_CommandInfo *)pRawMsg;
		Session.Chat().BeginCommandInfo();
		Session.Chat().RegisterCommand(pMsg->m_pName, pMsg->m_pArgsFormat, pMsg->m_pHelpText);
	}
	else if(MsgType == NETMSGTYPE_SV_COMMANDINFOREMOVE)
	{
		CNetMsg_Sv_CommandInfoRemove *pMsg = (CNetMsg_Sv_CommandInfoRemove *)pRawMsg;
		Session.Chat().UnregisterCommand(pMsg->m_pName);
	}
}

bool CChat::LineShouldHighlight(const char *pLine, const char *pName)
{
	const char *pHit = str_utf8_find_nocase(pLine, pName);

	while(pHit)
	{
		int Length = str_length(pName);

		if(Length > 0 && (pLine == pHit || pHit[-1] == ' ') && (pHit[Length] == 0 || pHit[Length] == ' ' || pHit[Length] == '.' || pHit[Length] == '!' || pHit[Length] == ',' || pHit[Length] == '?' || pHit[Length] == ':'))
			return true;

		pHit = str_utf8_find_nocase(pHit + 1, pName);
	}

	return false;
}

static constexpr const char *SAVES_HEADER[] = {
	"Time",
	"Player",
	"Map",
	"Code",
};

// TODO: remove this in a few releases (in 2027 or later)
//       it got deprecated by CGameClient::StoreSave
void CChat::StoreSave(const char *pText, const CGameSessionContext &Session)
{
	const char *pStart = str_find(pText, "Team successfully saved by ");
	const char *pMid = str_find(pText, ". Use '/load ");
	const char *pOn = str_find(pText, "' on ");
	const char *pEnd = str_find(pText, pOn ? " to continue" : "' to continue");

	if(!pStart || !pMid || !pEnd || pMid < pStart || pEnd < pMid || (pOn && (pOn < pMid || pEnd < pOn)))
		return;

	char aName[16];
	str_truncate(aName, sizeof(aName), pStart + 27, pMid - pStart - 27);

	char aSaveCode[64];

	str_truncate(aSaveCode, sizeof(aSaveCode), pMid + 13, (pOn ? pOn : pEnd) - pMid - 13);

	char aTimestamp[20];
	str_timestamp_format(aTimestamp, sizeof(aTimestamp), TimestampFormat::SPACE);

	const bool SavesFileExists = Storage()->FileExists(SAVES_FILE, IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(SAVES_FILE, IOFLAG_APPEND, IStorage::TYPE_SAVE);
	if(!File)
		return;

	const char *apColumns[4] = {
		aTimestamp,
		aName,
		Session.MapName(),
		aSaveCode,
	};

	if(!SavesFileExists)
	{
		CsvWrite(File, 4, SAVES_HEADER);
	}
	CsvWrite(File, 4, apColumns);
	io_close(File);
}

void CChat::CacheAppearance(CSessionId SessionId, const CGameState &State, const CSessionChatState::CLine &Line)
{
	CCachedLine &Cached = CachedLine(SessionId, Line.m_Id);
	Cached.Invalidate(*this);
	if(Line.m_ClientId >= 0)
		Cached.m_pManagedTeeRenderInfo = GameClient()->SessionPresentation(SessionId).CreateClientTee(State, Line.m_ClientId);
}

void CChat::AddLine(CGameSessionContext &Session, const CGameState &State, int64_t Now, bool IsDemoPlayback, bool ApplicationEffects, int ClientId, int Team, const char *pLine)
{
	CSessionChatState::CLine NewLine;
	str_utf8_copy_num(NewLine.m_aText, pLine, sizeof(NewLine.m_aText), MAX_LINE_LENGTH - 1);
	str_utf8_trim_right(NewLine.m_aText);
	if(NewLine.m_aText[0] == '\0' || (ClientId == SERVER_MSG && !g_Config.m_ClShowChatSystem))
		return;

	char aAuthorName[64] = {};
	char aAuthorClan[MAX_CLAN_LENGTH] = {};
	bool AuthorActive = false;
	bool AuthorLocal = false;
	bool AuthorFriend = false;
	bool AuthorFoe = false;
	if(ClientId >= 0)
	{
		if(!in_range(ClientId, MAX_CLIENTS - 1))
			return;
		const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
		AuthorActive = Identity.m_Active && IntsToStr(Identity.m_ClientInfo.m_aName, std::size(Identity.m_ClientInfo.m_aName), aAuthorName, std::size(aAuthorName));
		if(!AuthorActive || aAuthorName[0] == '\0' || GameClient()->SessionPresentation(Session.Id()).ChatIgnored(ClientId))
			return;
		IntsToStr(Identity.m_ClientInfo.m_aClan, std::size(Identity.m_ClientInfo.m_aClan), aAuthorClan, std::size(aAuthorClan));
		AuthorLocal = std::any_of(Session.GameStates().States().begin(), Session.GameStates().States().end(), [ClientId](const auto &pState) { return pState->LocalClientId() == ClientId; });
		AuthorFriend = !AuthorLocal && GameClient()->Friends()->IsFriend(aAuthorName, aAuthorClan, true);
		AuthorFoe = !AuthorLocal && GameClient()->Foes()->IsFriend(aAuthorName, aAuthorClan, true);
		const int LocalClientId = State.LocalClientId();
		const bool OtherTeam = State.IsOtherTeamFromLocalPlayer(ClientId);
		if(!AuthorLocal && ((g_Config.m_ClShowChatFriends && !AuthorFriend) ||
					   (g_Config.m_ClShowChatTeamMembersOnly && OtherTeam && in_range(LocalClientId, MAX_CLIENTS - 1) && State.Teams().Team(LocalClientId) != TEAM_FLOCK) || AuthorFoe))
			return;
	}

	NewLine.m_Time = Now;
	NewLine.m_ClientId = ClientId;
	NewLine.m_TeamNumber = Team;
	NewLine.m_Team = Team == 1;
	NewLine.m_Whisper = Team >= 2;
	NewLine.m_NameColor = -2;
	NewLine.m_DDTeam = ClientId >= 0 ? State.Teams().Team(ClientId) : 0;
	NewLine.m_CustomColor = ClientId == CLIENT_MSG ? g_Config.m_ClMessageClientColor : -1;
	NewLine.m_Friend = AuthorFriend;

	bool Highlighted = false;
	if(ClientId >= 0 && !AuthorLocal)
	{
		for(const auto &pSessionState : Session.GameStates().States())
		{
			const int LocalId = pSessionState->LocalClientId();
			if(!in_range(LocalId, MAX_CLIENTS - 1))
				continue;
			const CGameState::CClientIdentityState &LocalIdentity = pSessionState->ClientIdentity(LocalId);
			char aLocalName[64];
			if(LocalIdentity.m_Active && IntsToStr(LocalIdentity.m_ClientInfo.m_aName, std::size(LocalIdentity.m_ClientInfo.m_aName), aLocalName, std::size(aLocalName)))
				Highlighted |= LineShouldHighlight(NewLine.m_aText, aLocalName);
		}
	}
	NewLine.m_Highlighted = Highlighted;

	if(ClientId == SERVER_MSG)
	{
		str_copy(NewLine.m_aName, "*** ");
	}
	else if(ClientId == CLIENT_MSG)
	{
		str_copy(NewLine.m_aName, "— ");
	}
	else
	{
		const CGameState::CClientSnapshot &Author = State.Client(ClientId);
		if(Author.m_HasPlayerInfo)
		{
			if(Author.m_PlayerInfo.m_Team == TEAM_SPECTATORS)
				NewLine.m_NameColor = TEAM_SPECTATORS;
			if(State.HasGameInfo() && (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0)
			{
				if(Author.m_PlayerInfo.m_Team == TEAM_RED)
					NewLine.m_NameColor = TEAM_RED;
				else if(Author.m_PlayerInfo.m_Team == TEAM_BLUE)
					NewLine.m_NameColor = TEAM_BLUE;
			}
		}

		if(Team == TEAM_WHISPER_SEND)
		{
			str_copy(NewLine.m_aName, "→ ");
			str_append(NewLine.m_aName, aAuthorName);
			NewLine.m_NameColor = TEAM_BLUE;
			NewLine.m_Highlighted = false;
			Highlighted = false;
		}
		else if(Team == TEAM_WHISPER_RECV)
		{
			str_copy(NewLine.m_aName, "← ");
			str_append(NewLine.m_aName, aAuthorName);
			NewLine.m_NameColor = TEAM_RED;
			NewLine.m_Highlighted = true;
			Highlighted = true;
		}
		else
		{
			str_copy(NewLine.m_aName, aAuthorName);
		}
	}

	const uint64_t PreviousLastId = Session.Chat().LastId();
	const CSessionChatState::CLine &StoredLine = Session.Chat().Add(std::move(NewLine));
	if(StoredLine.m_Id != PreviousLastId)
		CacheAppearance(Session.Id(), State, StoredLine);
	else
		CachedLine(Session.Id(), StoredLine.m_Id).Invalidate(*this);

	auto &&FChatMsgCheckAndPrint = [](const CSessionChatState::CLine &Line) {
		ColorRGBA ChatLogColor = ColorRGBA(1.0f, 1.0f, 1.0f, 1.0f);
		if(Line.m_Highlighted)
		{
			ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageHighlightColor));
		}
		else
		{
			if(Line.m_Friend && g_Config.m_ClMessageFriend)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageFriendColor));
			else if(Line.m_Team)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageTeamColor));
			else if(Line.m_ClientId == SERVER_MSG)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
			else if(Line.m_CustomColor >= 0)
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(Line.m_CustomColor));
			else // regular message
				ChatLogColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageColor));
		}

		const char *pFrom;
		if(Line.m_Whisper)
			pFrom = "chat/whisper";
		else if(Line.m_Team)
			pFrom = "chat/team";
		else if(Line.m_ClientId == SERVER_MSG)
			pFrom = "chat/server";
		else if(Line.m_ClientId == CLIENT_MSG)
			pFrom = "chat/client";
		else
			pFrom = "chat/all";

		log_info_color(color_cast<LOG_COLOR>(ChatLogColor), pFrom, "%s%s%s", Line.m_aName, Line.m_ClientId >= 0 ? ": " : "", Line.m_aText);
	};

	FChatMsgCheckAndPrint(StoredLine);

	// play sound
	if(!ApplicationEffects)
		return;
	if(ClientId == SERVER_MSG)
	{
		if(Now - m_aLastSoundPlayed[CHAT_SERVER] >= time_freq() * 3 / 10)
		{
			if(g_Config.m_SndServerMessage)
			{
				GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_SERVER, 1.0f);
				m_aLastSoundPlayed[CHAT_SERVER] = Now;
			}
		}
	}
	else if(ClientId == CLIENT_MSG)
	{
		// No sound yet
	}
	else if(Highlighted && !IsDemoPlayback)
	{
		if(Now - m_aLastSoundPlayed[CHAT_HIGHLIGHT] >= time_freq() * 3 / 10)
		{
			char aBuf[1024];
			str_format(aBuf, sizeof(aBuf), "%s: %s", StoredLine.m_aName, StoredLine.m_aText);
			Client()->Notify("DDNet Chat", aBuf);
			if(g_Config.m_SndHighlight)
			{
				GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_HIGHLIGHT, 1.0f);
				m_aLastSoundPlayed[CHAT_HIGHLIGHT] = Now;
			}

			if(g_Config.m_ClEditor)
			{
				GameClient()->Editor()->UpdateMentions();
			}
		}
	}
	else if(Team != TEAM_WHISPER_SEND)
	{
		if(Now - m_aLastSoundPlayed[CHAT_CLIENT] >= time_freq() * 3 / 10)
		{
			bool PlaySound = StoredLine.m_Team ? g_Config.m_SndTeamChat : g_Config.m_SndChat;
#if defined(CONF_VIDEORECORDER)
			if(IVideo::Current())
			{
				PlaySound &= (bool)g_Config.m_ClVideoShowChat;
			}
#endif
			if(PlaySound)
			{
				GameClient()->m_Sounds.Play(CSounds::CHN_GUI, SOUND_CHAT_CLIENT, 1.0f);
				m_aLastSoundPlayed[CHAT_CLIENT] = Now;
			}
		}
	}
}

bool CChat::InputMatches(const CRenderContext &Context) const
{
	return m_Mode != MODE_NONE && Context.m_View.MatchesBinding(m_InputViewId, m_InputSessionId, m_InputStateId);
}

bool CChat::ShowMatches(const CRenderContext &Context) const
{
	return m_Show && Context.m_View.MatchesBinding(m_ShowViewId, m_ShowSessionId, m_ShowStateId);
}

void CChat::OnPrepareLines(const CRenderContext &Context, float y)
{
	float x = 5.0f;
	float FontSize = this->FontSize();

	const bool IsScoreBoardOpen = GameClient()->m_Scoreboard.IsActive(Context) && Context.AspectRatio(Graphics()->ScreenAspect()) > 1.7f;
	const bool ShowLargeArea = ShowMatches(Context) || (InputMatches(Context) && g_Config.m_ClShowChat == 1) || g_Config.m_ClShowChat == 2;
	const CSessionChatState &Chat = Context.m_Session.Chat();

	const int TeeSize = MessageTeeSize();
	float RealMsgPaddingX = MessagePaddingX();
	float RealMsgPaddingY = MessagePaddingY();
	float RealMsgPaddingTee = TeeSize + MESSAGE_TEE_PADDING_RIGHT;

	if(g_Config.m_ClChatOld)
	{
		RealMsgPaddingX = 0;
		RealMsgPaddingY = 0;
		RealMsgPaddingTee = 0;
	}

	const int64_t Now = Context.m_Time.m_PresentationTime;
	float LineWidth = (IsScoreBoardOpen ? std::max(85.0f, FontSize * 85.0f / 6.0f) : g_Config.m_ClChatWidth) - (RealMsgPaddingX * 1.5f) - RealMsgPaddingTee;

	float HeightLimit = IsScoreBoardOpen ? 180.0f : (ShowLargeArea ? 50.0f : 200.0f);
	float Begin = x;
	float TextBegin = Begin + RealMsgPaddingX / 2.0f;

	for(int i = 0; i < Chat.Count(); i++)
	{
		const CSessionChatState::CLine &Line = Chat.LineFromNewest(i);
		CCachedLine &Cached = CachedLine(Context.m_Session.Id(), Line.m_Id);
		if(Now > Line.m_Time + 16 * Context.m_Time.m_PresentationTimeFrequency && !ShowLargeArea)
			break;

		const CViewport &Viewport = Context.m_View.Viewport();
		const bool CacheMatches = Cached.m_Revision == Line.m_Revision && Cached.m_StateId == Context.m_State.Id() && Cached.m_ViewId == Context.m_View.Id() && Cached.m_Viewport == Viewport && Cached.m_OutputCacheKey == Context.m_OutputCacheKey && Cached.m_ScoreboardOpen == IsScoreBoardOpen && Cached.m_ShowLargeArea == ShowLargeArea;
		if(!CacheMatches)
		{
			Cached.Invalidate(*this);
			Cached.m_Revision = Line.m_Revision;
			Cached.m_StateId = Context.m_State.Id();
			Cached.m_ViewId = Context.m_View.Id();
			Cached.m_Viewport = Viewport;
			Cached.m_OutputCacheKey = Context.m_OutputCacheKey;
			Cached.m_ScoreboardOpen = IsScoreBoardOpen;
			Cached.m_ShowLargeArea = ShowLargeArea;
		}
		if(Cached.m_TextContainerIndex.Valid())
			continue;

		char aClientId[16] = "";
		if(g_Config.m_ClShowIds && Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
		{
			GameClient()->FormatClientId(Line.m_ClientId, aClientId, EClientIdFormat::INDENT_AUTO);
		}

		char aCount[12];
		if(Line.m_ClientId < 0)
			str_format(aCount, sizeof(aCount), "[%d] ", Line.m_TimesRepeated + 1);
		else
			str_format(aCount, sizeof(aCount), " [%d]", Line.m_TimesRepeated + 1);

		const char *pText = Line.m_aText;
		if(Config()->m_ClStreamerMode && Line.m_ClientId == SERVER_MSG)
		{
			if(str_startswith(Line.m_aText, "Team save in progress. You'll be able to load with '/load ") && str_endswith(Line.m_aText, "'"))
			{
				pText = "Team save in progress. You'll be able to load with '/load *** *** ***'";
			}
			else if(str_startswith(Line.m_aText, "Team save in progress. You'll be able to load with '/load") && str_endswith(Line.m_aText, "if it fails"))
			{
				pText = "Team save in progress. You'll be able to load with '/load *** *** ***' if save is successful or with '/load *** *** ***' if it fails";
			}
			else if(str_startswith(Line.m_aText, "Team successfully saved by ") && str_endswith(Line.m_aText, " to continue"))
			{
				pText = "Team successfully saved by ***. Use '/load *** *** ***' to continue";
			}
		}

		// get the y offset (calculate it if we haven't done that yet)
		if(Cached.m_YOffset < 0.0f)
		{
			CTextCursor MeasureCursor;
			MeasureCursor.SetPosition(vec2(TextBegin, 0.0f));
			MeasureCursor.m_FontSize = FontSize;
			MeasureCursor.m_Flags = 0;
			MeasureCursor.m_LineWidth = LineWidth;

			if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
			{
				MeasureCursor.m_X += RealMsgPaddingTee;

				if(Line.m_Friend && g_Config.m_ClMessageFriend)
				{
					TextRender()->TextEx(&MeasureCursor, "♥ ");
				}
			}

			TextRender()->TextEx(&MeasureCursor, aClientId);
			TextRender()->TextEx(&MeasureCursor, Line.m_aName);
			if(Line.m_TimesRepeated > 0)
				TextRender()->TextEx(&MeasureCursor, aCount);

			if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
			{
				TextRender()->TextEx(&MeasureCursor, ": ");
			}

			CTextCursor AppendCursor = MeasureCursor;
			AppendCursor.m_LongestLineWidth = 0.0f;
			if(!IsScoreBoardOpen && !g_Config.m_ClChatOld)
			{
				AppendCursor.m_StartX = MeasureCursor.m_X;
				AppendCursor.m_LineWidth -= MeasureCursor.m_LongestLineWidth;
			}

			TextRender()->TextEx(&AppendCursor, pText);

			Cached.m_YOffset = AppendCursor.Height() + RealMsgPaddingY;
		}

		y -= Cached.m_YOffset;

		// cut off if msgs waste too much space
		if(y < HeightLimit)
			break;

		// the position the text was created
		Cached.m_TextYOffset = y + RealMsgPaddingY / 2.0f;

		int CurRenderFlags = TextRender()->GetRenderFlags();
		TextRender()->SetRenderFlags(CurRenderFlags | ETextRenderFlags::TEXT_RENDER_FLAG_NO_AUTOMATIC_QUAD_UPLOAD);

		// reset the cursor
		CTextCursor LineCursor;
		LineCursor.SetPosition(vec2(TextBegin, Cached.m_TextYOffset));
		LineCursor.m_FontSize = FontSize;
		LineCursor.m_LineWidth = LineWidth;

		// Message is from valid player
		if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
		{
			LineCursor.m_X += RealMsgPaddingTee;

			if(Line.m_Friend && g_Config.m_ClMessageFriend)
			{
				TextRender()->TextColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageFriendColor)).WithAlpha(1.0f));
				TextRender()->CreateOrAppendTextContainer(Cached.m_TextContainerIndex, &LineCursor, "♥ ");
			}
		}

		// render name
		ColorRGBA NameColor;
		if(Line.m_CustomColor >= 0)
			NameColor = color_cast<ColorRGBA>(ColorHSLA(Line.m_CustomColor));
		else if(Line.m_ClientId == SERVER_MSG)
			NameColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
		else if(Line.m_Team)
			NameColor = CalculateNameColor(ColorHSLA(g_Config.m_ClMessageTeamColor));
		else if(Line.m_NameColor == TEAM_RED)
			NameColor = ColorRGBA(1.0f, 0.5f, 0.5f, 1.0f);
		else if(Line.m_NameColor == TEAM_BLUE)
			NameColor = ColorRGBA(0.7f, 0.7f, 1.0f, 1.0f);
		else if(Line.m_NameColor == TEAM_SPECTATORS)
			NameColor = ColorRGBA(0.75f, 0.5f, 0.75f, 1.0f);
		else if(Line.m_ClientId >= 0 && g_Config.m_ClChatTeamColors && Line.m_DDTeam != TEAM_FLOCK)
			NameColor = GameClient()->GetDDTeamColor(Line.m_DDTeam, 0.75f);
		else
			NameColor = ColorRGBA(0.8f, 0.8f, 0.8f, 1.0f);

		TextRender()->TextColor(NameColor);
		TextRender()->CreateOrAppendTextContainer(Cached.m_TextContainerIndex, &LineCursor, aClientId);
		TextRender()->CreateOrAppendTextContainer(Cached.m_TextContainerIndex, &LineCursor, Line.m_aName);

		if(Line.m_TimesRepeated > 0)
		{
			TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.3f);
			TextRender()->CreateOrAppendTextContainer(Cached.m_TextContainerIndex, &LineCursor, aCount);
		}

		if(Line.m_ClientId >= 0 && Line.m_aName[0] != '\0')
		{
			TextRender()->TextColor(NameColor);
			TextRender()->CreateOrAppendTextContainer(Cached.m_TextContainerIndex, &LineCursor, ": ");
		}

		ColorRGBA Color;
		if(Line.m_CustomColor >= 0)
			Color = color_cast<ColorRGBA>(ColorHSLA(Line.m_CustomColor));
		else if(Line.m_ClientId == SERVER_MSG)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageSystemColor));
		else if(Line.m_Highlighted)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageHighlightColor));
		else if(Line.m_Team)
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageTeamColor));
		else // regular message
			Color = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClMessageColor));
		TextRender()->TextColor(Color);

		CTextCursor AppendCursor = LineCursor;
		AppendCursor.m_LongestLineWidth = 0.0f;
		if(!IsScoreBoardOpen && !g_Config.m_ClChatOld)
		{
			AppendCursor.m_StartX = LineCursor.m_X;
			AppendCursor.m_LineWidth -= LineCursor.m_LongestLineWidth;
		}

		TextRender()->CreateOrAppendTextContainer(Cached.m_TextContainerIndex, &AppendCursor, pText);

		if(!g_Config.m_ClChatOld && (Line.m_aText[0] != '\0' || Line.m_aName[0] != '\0'))
		{
			float FullWidth = RealMsgPaddingX * 1.5f;
			if(!IsScoreBoardOpen && !g_Config.m_ClChatOld)
			{
				FullWidth += LineCursor.m_LongestLineWidth + AppendCursor.m_LongestLineWidth;
			}
			else
			{
				FullWidth += std::max(LineCursor.m_LongestLineWidth, AppendCursor.m_LongestLineWidth);
			}
			Graphics()->SetColor(1, 1, 1, 1);
			Cached.m_QuadContainerIndex = Graphics()->CreateRectQuadContainer(Begin, y, FullWidth, Cached.m_YOffset, MessageRounding(), IGraphics::CORNER_ALL);
		}

		TextRender()->SetRenderFlags(CurRenderFlags);
		if(Cached.m_TextContainerIndex.Valid())
			TextRender()->UploadTextContainer(Cached.m_TextContainerIndex);
	}

	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CChat::UpdateController(const CRenderContext &Context)
{
	if(m_Mode != MODE_NONE && !InputMatches(Context))
		DisableMode();
	if(!Context.m_Time.m_IsGameActive)
		return;
	CGameSessionContext *pSession = GameClient()->FindSessionContext(Client()->NetworkSessionId());
	const int64_t Now = time();
	if(pSession == nullptr || !pSession->Chat().HasPending() || pSession->Chat().LastSend() + time_freq() >= Now)
		return;
	const CSessionChatState::CPendingMessage Pending = pSession->Chat().Pending();
	SendChat(Pending.m_Team, Pending.m_Text.c_str(), pSession->Id(), Pending.m_StreamId);
	pSession->Chat().PopPending();
}

void CChat::RenderApplicationOverlay(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive || !InputMatches(Context))
		return;
	const float Height = 300.0f;
	const float Width = Height * Context.AspectRatio(Graphics()->ScreenAspect());
	Graphics()->MapScreenToSize(Width, Height);
	const float x = 5.0f;
	const float y = 300.0f - 20.0f * FontSize() / 6.0f;
	const float ScaledFontSize = FontSize() * (8.0f / 6.0f);
	CTextCursor InputCursor;
	InputCursor.SetPosition(vec2(x, y));
	InputCursor.m_FontSize = ScaledFontSize;
	InputCursor.m_LineWidth = Width - 190.0f;
	TextRender()->TextEx(&InputCursor, m_Mode == MODE_ALL ? Localize("All") : m_Mode == MODE_TEAM ? Localize("Team") :
													Localize("Chat"));
	TextRender()->TextEx(&InputCursor, ": ");

	const float MessageMaxWidth = InputCursor.m_LineWidth - (InputCursor.m_X - InputCursor.m_StartX);
	const CUIRect ClippingRect = {InputCursor.m_X, InputCursor.m_Y, MessageMaxWidth, 2.25f * InputCursor.m_FontSize};
	const CViewport &Viewport = Context.m_View.Viewport();
	const float ViewportWidth = Viewport.m_Width > 0 ? Viewport.m_Width : Graphics()->ScreenWidth();
	const float ViewportHeight = Viewport.m_Height > 0 ? Viewport.m_Height : Graphics()->ScreenHeight();
	const int ViewportX = Viewport.m_Width > 0 ? Viewport.m_X : 0;
	const int ViewportY = Viewport.m_Height > 0 ? Viewport.m_Y : 0;
	const float XScale = ViewportWidth / Width;
	const float YScale = ViewportHeight / Height;
	Graphics()->ClipEnable(ViewportX + (int)(ClippingRect.x * XScale), ViewportY + (int)(ClippingRect.y * YScale), (int)(ClippingRect.w * XScale), (int)(ClippingRect.h * YScale));

	float ScrollOffset = m_Input.GetScrollOffset();
	float ScrollOffsetChange = m_Input.GetScrollOffsetChange();
	m_Input.Activate(EInputPriority::CHAT);
	const CUIRect InputCursorRect = {InputCursor.m_X, InputCursor.m_Y - ScrollOffset, 0.0f, 0.0f};
	const bool Changed = m_Input.WasChanged() || m_Input.WasCursorChanged();
	const STextBoundingBox BoundingBox = m_Input.Render(&InputCursorRect, InputCursor.m_FontSize, TEXTALIGN_TL, Changed, MessageMaxWidth, 0.0f);
	Graphics()->ClipDisable();

	const float CaretPositionY = m_Input.GetCaretPosition().y - ScrollOffsetChange;
	if(CaretPositionY < ClippingRect.y)
		ScrollOffsetChange -= ClippingRect.y - CaretPositionY;
	else if(CaretPositionY + InputCursor.m_FontSize > ClippingRect.y + ClippingRect.h)
		ScrollOffsetChange += CaretPositionY + InputCursor.m_FontSize - (ClippingRect.y + ClippingRect.h);
	Ui()->DoSmoothScrollLogic(&ScrollOffset, &ScrollOffsetChange, ClippingRect.h, BoundingBox.m_H);
	m_Input.SetScrollOffset(ScrollOffset);
	m_Input.SetScrollOffsetChange(ScrollOffsetChange);

	const CGameSessionContext *pSession = GameClient()->FindSessionContext(m_InputSessionId);
	if(pSession == nullptr || m_Input.GetString()[0] != '/' || m_Input.GetString()[1] == '\0')
		return;
	for(const CSessionChatState::CCommand &Command : pSession->Chat().Commands())
	{
		if(!str_startswith_nocase(Command.m_Name.c_str(), m_Input.GetString() + 1))
			continue;
		InputCursor.m_X += TextRender()->TextWidth(InputCursor.m_FontSize, m_Input.GetString(), -1, InputCursor.m_LineWidth);
		InputCursor.m_Y = m_Input.GetCaretPosition().y;
		TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.5f);
		TextRender()->TextEx(&InputCursor, Command.m_Name.c_str() + str_length(m_Input.GetString() + 1));
		TextRender()->TextColor(TextRender()->DefaultTextColor());
		break;
	}
}

void CChat::RenderLines(const CRenderContext &Context, float y)
{
	const bool IsScoreBoardOpen = GameClient()->m_Scoreboard.IsActive(Context) && Context.AspectRatio(Graphics()->ScreenAspect()) > 1.7f;
	const bool ShowLargeArea = ShowMatches(Context) || (InputMatches(Context) && g_Config.m_ClShowChat == 1) || g_Config.m_ClShowChat == 2;
	const CSessionChatState &Chat = Context.m_Session.Chat();
	const int64_t Now = Context.m_Time.m_PresentationTime;
	const int64_t Frequency = Context.m_Time.m_PresentationTimeFrequency;
	const float HeightLimit = IsScoreBoardOpen ? 180.0f : (ShowLargeArea ? 50.0f : 200.0f);
	const float x = 5.0f;
	float RealMsgPaddingX = MessagePaddingX();
	float RealMsgPaddingY = MessagePaddingY();
	if(g_Config.m_ClChatOld)
	{
		RealMsgPaddingX = 0;
		RealMsgPaddingY = 0;
	}

	for(int i = 0; i < Chat.Count(); ++i)
	{
		const CSessionChatState::CLine &Line = Chat.LineFromNewest(i);
		CCachedLine &Cached = CachedLine(Context.m_Session.Id(), Line.m_Id);
		if(Now > Line.m_Time + 16 * Frequency && !ShowLargeArea)
			break;
		y -= Cached.m_YOffset;
		if(y < HeightLimit)
			break;
		const float Blend = Now > Line.m_Time + 14 * Frequency && !ShowLargeArea ? 1.0f - (Now - Line.m_Time - 14 * Frequency) / (2.0f * Frequency) : 1.0f;
		if(!g_Config.m_ClChatOld)
		{
			Graphics()->TextureClear();
			if(Cached.m_QuadContainerIndex != -1)
			{
				Graphics()->SetColor(color_cast<ColorRGBA>(ColorHSLA(g_Config.m_ClChatBackgroundColor, true)).WithMultipliedAlpha(Blend));
				Graphics()->RenderQuadContainerEx(Cached.m_QuadContainerIndex, 0, -1, 0, (y + RealMsgPaddingY / 2.0f) - Cached.m_TextYOffset);
			}
		}
		if(!Cached.m_TextContainerIndex.Valid())
			continue;
		if(!g_Config.m_ClChatOld && Cached.m_pManagedTeeRenderInfo != nullptr)
		{
			CTeeRenderInfo &TeeRenderInfo = Cached.m_pManagedTeeRenderInfo->TeeRenderInfo();
			const int TeeSize = MessageTeeSize();
			TeeRenderInfo.m_Size = TeeSize;
			const float RowHeight = FontSize() + RealMsgPaddingY;
			const CAnimState *pIdleState = CAnimState::GetIdle();
			vec2 OffsetToMid;
			CRenderTools::GetRenderTeeOffsetToRenderedTee(pIdleState, &TeeRenderInfo, OffsetToMid);
			const vec2 TeeRenderPos(x + (RealMsgPaddingX + TeeSize) / 2.0f, y + TeeSize / 2.0f + (RowHeight - TeeSize) / 2.0f + OffsetToMid.y);
			RenderTools()->RenderTee(pIdleState, &TeeRenderInfo, EMOTE_NORMAL, vec2(1, 0.1f), TeeRenderPos, Blend);
		}
		const ColorRGBA TextColor = TextRender()->DefaultTextColor().WithMultipliedAlpha(Blend);
		const ColorRGBA TextOutlineColor = TextRender()->DefaultTextOutlineColor().WithMultipliedAlpha(Blend);
		TextRender()->RenderTextContainer(Cached.m_TextContainerIndex, TextColor, TextOutlineColor, 0, (y + RealMsgPaddingY / 2.0f) - Cached.m_TextYOffset);
	}
}

void CChat::OnRender(const CRenderContext &Context)
{
	if(!Context.m_Time.m_IsGameActive || !((g_Config.m_ClShowChat && !Context.m_IsVideoOutput) || (g_Config.m_ClVideoShowChat && Context.m_IsVideoOutput)))
		return;
	const float Height = 300.0f;
	const float Width = Height * Context.AspectRatio(Graphics()->ScreenAspect());
	Graphics()->MapScreenToSize(Width, Height);
	const float y = 300.0f - 20.0f * FontSize() / 6.0f - FontSize() * (8.0f / 6.0f);
	OnPrepareLines(Context, y);
	RenderLines(Context, y);
}

void CChat::EnsureCoherentFontSize() const
{
	// Adjust font size based on width
	if(g_Config.m_ClChatWidth / (float)g_Config.m_ClChatFontSize >= CHAT_FONTSIZE_WIDTH_RATIO)
		return;

	// We want to keep a ration between font size and font width so that we don't have a weird rendering
	g_Config.m_ClChatFontSize = g_Config.m_ClChatWidth / CHAT_FONTSIZE_WIDTH_RATIO;
}

void CChat::EnsureCoherentWidth() const
{
	// Adjust width based on font size
	if(g_Config.m_ClChatWidth / (float)g_Config.m_ClChatFontSize >= CHAT_FONTSIZE_WIDTH_RATIO)
		return;

	// We want to keep a ration between font size and font width so that we don't have a weird rendering
	g_Config.m_ClChatWidth = CHAT_FONTSIZE_WIDTH_RATIO * g_Config.m_ClChatFontSize;
}

// ----- send functions -----

void CChat::SendChat(int Team, const char *pLine)
{
	const CGameView &View = GameClient()->LegacyGameView();
	const CGameSessionContext *pSession = GameClient()->FindSessionContext(View.SessionId());
	const CGameState *pState = pSession != nullptr ? pSession->GameStates().Find(View.StateId()) : nullptr;
	if(pState != nullptr)
		SendChat(Team, pLine, pSession->Id(), pState->StreamId());
}

void CChat::SendChat(int Team, const char *pLine, CSessionId SessionId, CStreamId StreamId)
{
	CGameSessionContext *pSession = GameClient()->FindSessionContext(SessionId);
	const int Conn = static_cast<int>(StreamId.Value()) - 1;
	if(*str_utf8_skip_whitespaces(pLine) == '\0' || pSession == nullptr || SessionId != Client()->NetworkSessionId() || pSession->GameStates().FindByStream(StreamId) == nullptr || Conn < IClient::CONN_MAIN || Conn >= IClient::NUM_CONNS)
		return;

	pSession->Chat().SetLastSend(time());

	if(pSession->Protocol() == EGameProtocol::SIXUP)
	{
		protocol7::CNetMsg_Cl_Say Msg7;
		Msg7.m_Mode = Team == 1 ? protocol7::CHAT_TEAM : protocol7::CHAT_ALL;
		Msg7.m_Target = -1;
		Msg7.m_pMessage = pLine;
		Client()->SendPackMsg(Conn, &Msg7, MSGFLAG_VITAL, true);
		return;
	}

	// send chat message
	CNetMsg_Cl_Say Msg;
	Msg.m_Team = Team;
	Msg.m_pMessage = pLine;
	Client()->SendPackMsg(Conn, &Msg, MSGFLAG_VITAL);
}

void CChat::SendChatQueued(const char *pLine)
{
	if(!pLine || str_length(pLine) < 1)
		return;

	bool AddEntry = false;
	CGameSessionContext *pSession = GameClient()->FindSessionContext(m_InputSessionId);
	if(pSession == nullptr)
		return;
	if(!pSession->Chat().HasPending() && pSession->Chat().LastSend() + time_freq() < time())
	{
		SendChat(m_Mode == MODE_ALL ? 0 : 1, pLine, m_InputSessionId, m_InputStreamId);
		AddEntry = true;
	}
	else
	{
		AddEntry = pSession->Chat().Enqueue(m_InputStreamId, m_Mode == MODE_ALL ? 0 : 1, pLine);
	}

	if(AddEntry)
	{
		const int Length = str_length(pLine);
		CHistoryEntry *pEntry = m_History.Allocate(sizeof(CHistoryEntry) + Length);
		pEntry->m_Team = m_Mode == MODE_ALL ? 0 : 1;
		str_copy(pEntry->m_aText, pLine, Length + 1);
	}
}
