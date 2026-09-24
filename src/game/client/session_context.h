#ifndef GAME_CLIENT_SESSION_CONTEXT_H
#define GAME_CLIENT_SESSION_CONTEXT_H

#include "game_state.h"
#include "input_policy.h"
#include "map_context.h"
#include "match_collector.h"
#include "match_report_assembler.h"

#include <base/dbg.h>
#include <base/str.h>

#include <engine/client/enums.h>
#include <engine/client/session.h>
#include <engine/shared/protocol.h>

#include <game/voting.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <list>
#include <span>
#include <string>
#include <utility>
#include <vector>

enum class EGameProtocol
{
	SIX,
	SIXUP,
};

class CSessionBroadcastState
{
	std::string m_Text;
	int m_ExpireTick = 0;
	uint64_t m_Revision = 0;

public:
	static constexpr int MAX_TEXT_LENGTH = 1023;

	const char *Text() const { return m_Text.c_str(); }
	int ExpireTick() const { return m_ExpireTick; }
	uint64_t Revision() const { return m_Revision; }
	bool IsActiveAt(int GameTick) const { return GameTick < m_ExpireTick; }

	void Apply(const char *pText, int GameTick, int GameTickSpeed)
	{
		m_Text = pText;
		if(m_Text.size() > static_cast<std::string::size_type>(MAX_TEXT_LENGTH))
			m_Text.resize(MAX_TEXT_LENGTH);
		m_ExpireTick = GameTick + GameTickSpeed * 10;
		++m_Revision;
	}

	void Reset()
	{
		m_Text.clear();
		m_ExpireTick = 0;
		++m_Revision;
	}
};

class CSessionMotdState
{
	std::string m_Text;
	uint64_t m_Revision = 0;

public:
	static constexpr int MAX_TEXT_LENGTH = 899;

	const char *Text() const { return m_Text.c_str(); }
	uint64_t Revision() const { return m_Revision; }

	void Apply(const char *pText)
	{
		m_Text.clear();
		for(size_t i = 0; pText[i] != '\0' && m_Text.size() < static_cast<std::string::size_type>(MAX_TEXT_LENGTH); ++i)
		{
			if(pText[i] == '\\' && pText[i + 1] == 'n')
			{
				m_Text.push_back('\n');
				++i;
			}
			else
			{
				m_Text.push_back(pText[i]);
			}
		}
		++m_Revision;
	}

	void Reset()
	{
		m_Text.clear();
		++m_Revision;
	}
};

class CSessionMapMetadataState
{
	int m_BestTimeSeconds = FinishTime::UNSET;
	int m_BestTimeMillis = 0;
	std::string m_Description;

public:
	static constexpr int MAX_DESCRIPTION_LENGTH = 511;

	int BestTimeSeconds() const { return m_BestTimeSeconds; }
	int BestTimeMillis() const { return m_BestTimeMillis; }
	const char *Description() const { return m_Description.c_str(); }

	void ApplyBestTime(int Seconds, int Millis)
	{
		m_BestTimeSeconds = Seconds;
		m_BestTimeMillis = Millis;
	}

	void ApplyRecordBestTime(int Centiseconds)
	{
		// Some PvP mods based on DDNet accidentally send zero despite having no finished races.
		if(Centiseconds <= 0)
			return;
		m_BestTimeSeconds = Centiseconds / 100;
		m_BestTimeMillis = (Centiseconds % 100) * 10;
	}

	void SetDescription(const char *pDescription)
	{
		m_Description = pDescription;
		if(m_Description.size() > static_cast<std::string::size_type>(MAX_DESCRIPTION_LENGTH))
			m_Description.resize(MAX_DESCRIPTION_LENGTH);
		m_Description.resize(str_utf8_fix_truncation(m_Description.data()));
	}

	void Reset()
	{
		m_BestTimeSeconds = FinishTime::UNSET;
		m_BestTimeMillis = 0;
		m_Description.clear();
	}
};

class CSessionInfoMessageState
{
public:
	static constexpr int MAX_MESSAGES = 5;
	static constexpr int MAX_TEAM_MEMBERS = 4;

	enum class EType
	{
		KILL,
		FINISH,
	};

	struct CMessage
	{
		EType m_Type = EType::KILL;
		uint64_t m_Id = 0;
		int m_Tick = -1;
		std::array<int, MAX_TEAM_MEMBERS> m_aVictimIds = {-1, -1, -1, -1};
		int m_VictimDDTeam = 0;
		char m_aVictimName[64] = {};
		int m_KillerId = -1;
		char m_aKillerName[64] = {};
		int m_Weapon = -1;
		int m_ModeSpecial = 0;
		int m_FlagCarrierBlue = -1;
		int m_TeamSize = 0;
		int m_Diff = 0;
		char m_aTimeText[32] = {};
		char m_aDiffText[32] = {};
		bool m_RecordPersonal = false;
	};

private:
	std::array<CMessage, MAX_MESSAGES> m_aMessages;
	uint64_t m_LastId = 0;
	int m_Count = 0;

public:
	const CMessage &Add(CMessage Message)
	{
		Message.m_Id = ++m_LastId;
		CMessage &Stored = m_aMessages[Message.m_Id % MAX_MESSAGES];
		Stored = Message;
		m_Count = std::min(m_Count + 1, MAX_MESSAGES);
		return Stored;
	}

	int Count() const { return m_Count; }
	const CMessage &Message(int Index) const
	{
		dbg_assert(Index >= 0 && Index < m_Count, "info message index invalid");
		const uint64_t Id = m_LastId - m_Count + 1 + Index;
		const CMessage &Message = m_aMessages[Id % MAX_MESSAGES];
		dbg_assert(Message.m_Id == Id, "info message ring corrupted");
		return Message;
	}
	void Reset()
	{
		m_aMessages = {};
		m_LastId = 0;
		m_Count = 0;
	}
};

class CSessionChatState
{
public:
	static constexpr int MAX_LINES = 64;
	static constexpr int MAX_LINE_LENGTH = MAX_CHAT_LENGTH;
	static constexpr int MAX_PENDING = 3;

	struct CLine
	{
		uint64_t m_Id = 0;
		uint64_t m_Revision = 0;
		int64_t m_Time = 0;
		int m_ClientId = -1;
		int m_TeamNumber = 0;
		bool m_Team = false;
		bool m_Whisper = false;
		int m_NameColor = -2;
		int m_DDTeam = 0;
		int m_CustomColor = -1;
		char m_aName[64] = {};
		char m_aText[MAX_LINE_LENGTH] = {};
		bool m_Friend = false;
		bool m_Highlighted = false;
		int m_TimesRepeated = 0;
	};

	struct CCommand
	{
		std::string m_Name;
		std::string m_Params;
		std::string m_HelpText;

		bool operator<(const CCommand &Other) const { return str_comp(m_Name.c_str(), Other.m_Name.c_str()) < 0; }
	};

	struct CPendingMessage
	{
		// The session of the local player that says it.
		CSessionId m_SessionId;
		int m_Team = 0;
		std::string m_Text;
	};

private:
	std::array<CLine, MAX_LINES> m_aLines;
	std::vector<CCommand> m_vCommands;
	std::deque<CPendingMessage> m_PendingMessages;
	uint64_t m_LastId = 0;
	int64_t m_LastSend = 0;
	int m_Count = 0;
	bool m_ServerSupportsCommandInfo = false;
	bool m_CommandsNeedSorting = false;
	bool m_SixupTeamLocked = false;

public:
	const CLine &Add(CLine Line)
	{
		if(m_Count > 0)
		{
			CLine &Previous = m_aLines[m_LastId % MAX_LINES];
			if(Previous.m_TeamNumber == Line.m_TeamNumber && Previous.m_ClientId == Line.m_ClientId && Previous.m_CustomColor == Line.m_CustomColor && str_comp(Previous.m_aText, Line.m_aText) == 0)
			{
				Previous.m_Time = Line.m_Time;
				++Previous.m_Revision;
				++Previous.m_TimesRepeated;
				return Previous;
			}
		}

		Line.m_Id = ++m_LastId;
		Line.m_Revision = 1;
		CLine &Stored = m_aLines[Line.m_Id % MAX_LINES];
		Stored = Line;
		m_Count = std::min(m_Count + 1, MAX_LINES);
		return Stored;
	}

	int Count() const { return m_Count; }
	uint64_t LastId() const { return m_LastId; }
	const CLine &Line(int Index) const
	{
		dbg_assert(Index >= 0 && Index < m_Count, "chat line index invalid");
		const uint64_t Id = m_LastId - m_Count + 1 + Index;
		const CLine &Stored = m_aLines[Id % MAX_LINES];
		dbg_assert(Stored.m_Id == Id, "chat line ring corrupted");
		return Stored;
	}
	const CLine &LineFromNewest(int Index) const { return Line(m_Count - Index - 1); }

	void BeginCommandInfo()
	{
		if(m_ServerSupportsCommandInfo)
			return;
		m_vCommands.clear();
		m_ServerSupportsCommandInfo = true;
	}
	void RegisterCommand(const char *pName, const char *pParams, const char *pHelpText)
	{
		if(std::any_of(m_vCommands.begin(), m_vCommands.end(), [pName](const CCommand &Command) { return str_comp(Command.m_Name.c_str(), pName) == 0; }))
			return;
		m_vCommands.push_back({pName, pParams, pHelpText});
		m_CommandsNeedSorting = true;
	}
	void UnregisterCommand(const char *pName)
	{
		m_vCommands.erase(std::remove_if(m_vCommands.begin(), m_vCommands.end(), [pName](const CCommand &Command) { return str_comp(Command.m_Name.c_str(), pName) == 0; }), m_vCommands.end());
	}
	const std::vector<CCommand> &SortedCommands()
	{
		if(m_CommandsNeedSorting)
		{
			std::sort(m_vCommands.begin(), m_vCommands.end());
			m_CommandsNeedSorting = false;
		}
		return m_vCommands;
	}
	const std::vector<CCommand> &Commands() const { return m_vCommands; }
	bool Enqueue(CSessionId SessionId, int Team, const char *pText)
	{
		if(m_PendingMessages.size() >= MAX_PENDING)
			return false;
		m_PendingMessages.push_back({SessionId, Team, pText});
		return true;
	}
	bool HasPending() const { return !m_PendingMessages.empty(); }
	const CPendingMessage &Pending() const { return m_PendingMessages.front(); }
	void PopPending() { m_PendingMessages.pop_front(); }
	int PendingCount() const { return m_PendingMessages.size(); }
	int64_t LastSend() const { return m_LastSend; }
	void SetLastSend(int64_t LastSend) { m_LastSend = LastSend; }
	bool UpdateSixupTeamLocked(bool Locked)
	{
		if(m_SixupTeamLocked == Locked)
			return false;
		m_SixupTeamLocked = Locked;
		return true;
	}

	void ClearLines()
	{
		m_aLines = {};
		m_LastId = 0;
		m_Count = 0;
	}
	void Reset()
	{
		ClearLines();
		m_vCommands.clear();
		m_PendingMessages.clear();
		m_LastSend = 0;
		m_ServerSupportsCommandInfo = false;
		m_CommandsNeedSorting = false;
		m_SixupTeamLocked = false;
	}
};

class CSessionVoteState
{
	int64_t m_OpenTime = 0;
	int64_t m_CloseTime = 0;
	std::string m_Description;
	std::string m_Reason;
	int m_Voted = 0;
	int m_Yes = 0;
	int m_No = 0;
	int m_Pass = 0;
	int m_Total = 0;
	bool m_ReceivingOptions = false;
	std::list<std::string> m_Options;

	static void AssignTruncated(std::string &Target, const char *pText, size_t MaxLength)
	{
		Target = pText;
		if(Target.size() > MaxLength)
			Target.resize(MaxLength);
		Target.resize(str_utf8_fix_truncation(Target.data()));
	}

public:
	int64_t OpenTime() const { return m_OpenTime; }
	int64_t CloseTime() const { return m_CloseTime; }
	const char *Description() const { return m_Description.c_str(); }
	const char *Reason() const { return m_Reason.c_str(); }
	int Voted() const { return m_Voted; }
	int Yes() const { return m_Yes; }
	int No() const { return m_No; }
	int Pass() const { return m_Pass; }
	int Total() const { return m_Total; }
	bool IsVoting() const { return m_CloseTime != 0; }
	bool IsReceivingOptions() const { return m_ReceivingOptions; }
	int NumOptions() const { return static_cast<int>(m_Options.size()); }
	const std::list<std::string> &Options() const { return m_Options; }

	int SecondsLeft(int64_t Now, int64_t Frequency) const
	{
		return static_cast<int>((m_CloseTime - Now) / Frequency);
	}
	void Expire(int64_t Now, int64_t Frequency)
	{
		if(IsVoting() && SecondsLeft(Now, Frequency) < 0)
			ResetVote();
	}

	bool ApplyVoteSet(int Timeout, const char *pDescription, const char *pReason, int64_t Now, int64_t Frequency)
	{
		ResetVote();
		if(Timeout == 0)
			return false;
		AssignTruncated(m_Description, pDescription, VOTE_DESC_LENGTH - 1);
		AssignTruncated(m_Reason, pReason, VOTE_REASON_LENGTH - 1);
		m_OpenTime = Now;
		m_CloseTime = Now + Frequency * Timeout;
		return true;
	}

	void ApplyStatus(int Yes, int No, int Pass, int Total)
	{
		m_Yes = Yes;
		m_No = No;
		m_Pass = Pass;
		m_Total = Total;
	}

	void SetVoted(int Voted) { m_Voted = Voted; }
	void SetReceivingOptions(bool ReceivingOptions) { m_ReceivingOptions = ReceivingOptions; }

	void AddOption(const char *pDescription)
	{
		if(NumOptions() == MAX_VOTE_OPTIONS)
			return;
		std::string Description;
		AssignTruncated(Description, pDescription, VOTE_DESC_LENGTH - 1);
		m_Options.push_back(std::move(Description));
	}

	void RemoveOption(const char *pDescription)
	{
		const auto It = std::find(m_Options.begin(), m_Options.end(), pDescription);
		if(It != m_Options.end())
			m_Options.erase(It);
	}

	const std::string *Option(int Index) const
	{
		if(Index < 0 || Index >= NumOptions())
			return nullptr;
		auto It = m_Options.begin();
		while(Index-- > 0)
			++It;
		return &*It;
	}

	void ClearOptions() { m_Options.clear(); }

	void ResetVote()
	{
		m_OpenTime = 0;
		m_CloseTime = 0;
		m_Description.clear();
		m_Reason.clear();
		m_Voted = 0;
		m_Yes = 0;
		m_No = 0;
		m_Pass = 0;
		m_Total = 0;
		m_ReceivingOptions = false;
	}

	void Reset()
	{
		ResetVote();
		ClearOptions();
	}
};

/**
 * What the game keeps of one server or demo. The state of a server holds the
 * game states of both seats played on it, the player and the dummy, each in
 * its own session; what belongs to the server, like the map, the chat and the
 * votes, is kept once for both.
 */
class CGameSessionContext
{
	CSessionId m_Id;
	std::string m_MapName;
	EGameProtocol m_Protocol;
	bool m_ServerCapAnyPlayerFlag = false;
	// One per seat: player and dummy of a server, the only one of a demo.
	std::array<CGameState, NUM_DUMMIES> m_aGameStates;
	int m_NumGameStates;

public:
	CMapContext m_MapContext;
	CSessionBroadcastState m_Broadcast;
	CSessionMotdState m_Motd;
	CSessionMapMetadataState m_MapMetadata;
	CSessionInfoMessageState m_InfoMessages;
	CSessionChatState m_Chat;
	CSessionStatsState m_Stats;
	CMatchReportAssembler m_MatchReportAssembler;
	int64_t m_LastLiveStatsRequest = 0;
	CSessionVoteState m_Vote;
	// Indexed by seat.
	std::array<CInputRoute, NUM_DUMMIES> m_aInputRoutes;

	/**
	 * @param Id The session of the server or demo, played in seat 0.
	 * @param DummyId The session of the dummy on the server, invalid for a
	 *                demo.
	 */
	explicit CGameSessionContext(CSessionId Id, CSessionId DummyId = CSessionId()) :
		m_Id(Id),
		m_Protocol(EGameProtocol::SIX),
		m_NumGameStates(DummyId.IsValid() ? NUM_DUMMIES : 1)
	{
		for(int Seat = 0; Seat < NUM_DUMMIES; Seat++)
			m_aGameStates[Seat].m_Seat = Seat;
		m_aGameStates[0].m_SessionId = Id;
		m_aGameStates[1].m_SessionId = DummyId;
	}

	CSessionId Id() const { return m_Id; }
	const char *MapName() const { return m_MapName.c_str(); }
	EGameProtocol Protocol() const { return m_Protocol; }
	bool ServerCapAnyPlayerFlag() const { return m_ServerCapAnyPlayerFlag; }
	void SetServerCapAnyPlayerFlag(bool Value) { m_ServerCapAnyPlayerFlag = Value; }
	void SetDescriptor(const char *pMapName, EGameProtocol Protocol)
	{
		m_MapName = pMapName;
		m_Protocol = Protocol;
	}
	std::span<CGameState> GameStates() { return {m_aGameStates.data(), static_cast<size_t>(m_NumGameStates)}; }
	std::span<const CGameState> GameStates() const { return {m_aGameStates.data(), static_cast<size_t>(m_NumGameStates)}; }
	/**
	 * The game state of a session played here, `nullptr` if the session is
	 * not one of them.
	 */
	CGameState *FindGameState(CSessionId SessionId)
	{
		if(!SessionId.IsValid())
			return nullptr;
		for(CGameState &State : GameStates())
		{
			if(State.m_SessionId == SessionId)
				return &State;
		}
		return nullptr;
	}
	const CGameState *FindGameState(CSessionId SessionId) const { return const_cast<CGameSessionContext *>(this)->FindGameState(SessionId); }
	CGameState &GameState(CSessionId SessionId)
	{
		CGameState *pState = FindGameState(SessionId);
		dbg_assert(pState != nullptr, "session is not played here");
		return *pState;
	}
	const CGameState &GameState(CSessionId SessionId) const { return const_cast<CGameSessionContext *>(this)->GameState(SessionId); }
	CGameState &SeatState(int Seat)
	{
		dbg_assert(Seat >= 0 && Seat < m_NumGameStates, "invalid seat");
		return m_aGameStates[Seat];
	}
	const CGameState &SeatState(int Seat) const { return const_cast<CGameSessionContext *>(this)->SeatState(Seat); }
	/**
	 * Whether a session is played here: the server or demo, or the dummy on
	 * the server.
	 */
	bool Contains(CSessionId SessionId) const { return FindGameState(SessionId) != nullptr; }
};

#endif // GAME_CLIENT_SESSION_CONTEXT_H
