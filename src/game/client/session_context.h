#ifndef GAME_CLIENT_SESSION_CONTEXT_H
#define GAME_CLIENT_SESSION_CONTEXT_H

#include "game_state.h"
#include "input_policy.h"
#include "local_player_profile.h"
#include "map_context.h"

#include <base/dbg.h>
#include <base/str.h>

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
#include <memory>
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
		Stored = std::move(Message);
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
	static constexpr int MAX_LINE_LENGTH = 256;
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
		CStreamId m_StreamId;
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
		Stored = std::move(Line);
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
	bool Enqueue(CStreamId StreamId, int Team, const char *pText)
	{
		if(m_PendingMessages.size() >= MAX_PENDING)
			return false;
		m_PendingMessages.push_back({StreamId, Team, pText});
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

class CSessionClientStats
{
	int m_IngameTicks = 0;
	int m_JoinTick = 0;
	bool m_Active = false;

public:
	std::array<int, NUM_WEAPONS> m_aFragsWith = {};
	std::array<int, NUM_WEAPONS> m_aDeathsFrom = {};
	int m_Frags = 0;
	int m_Deaths = 0;
	int m_Suicides = 0;
	int m_BestSpree = 0;
	int m_CurrentSpree = 0;
	int m_FlagGrabs = 0;
	int m_FlagCaptures = 0;

	void Reset() { *this = {}; }
	bool IsActive() const { return m_Active; }
	void JoinGame(int Tick)
	{
		m_Active = true;
		m_JoinTick = Tick;
	}
	void JoinSpec(int Tick)
	{
		m_Active = false;
		m_IngameTicks += Tick - m_JoinTick;
	}
	int GetIngameTicks(int Tick) const { return m_IngameTicks + Tick - m_JoinTick; }
	float GetFPM(int Tick, int TickSpeed) const { return static_cast<float>(m_Frags * TickSpeed * 60) / GetIngameTicks(Tick); }
};

class CSessionStatsState
{
	std::array<CSessionClientStats, MAX_CLIENTS> m_aClients;
	bool m_HasGameInfo = false;
	int m_LastRoundStartTick = 0;
	bool m_GameOver = false;
	bool m_GamePaused = false;
	int m_LastFlagCarrierRed = FLAG_MISSING;
	int m_LastFlagCarrierBlue = FLAG_MISSING;

	void ResetClients()
	{
		for(CSessionClientStats &Client : m_aClients)
			Client.Reset();
	}

public:
	void Reset()
	{
		ResetClients();
		m_HasGameInfo = false;
		m_LastRoundStartTick = 0;
		m_GameOver = false;
		m_GamePaused = false;
		m_LastFlagCarrierRed = FLAG_MISSING;
		m_LastFlagCarrierBlue = FLAG_MISSING;
	}
	void UpdateSnapshot(const CGameState &State, int Tick)
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
			const bool Present = SnapshotClient.m_HasPlayerInfo && SnapshotClient.m_PlayerInfo.m_ClientId == ClientId;
			CSessionClientStats &Stats = m_aClients[ClientId];
			if(!Present)
				Stats.Reset();
			else if(SnapshotClient.m_PlayerInfo.m_Team != TEAM_SPECTATORS && !Stats.IsActive())
				Stats.JoinGame(Tick);
			else if(SnapshotClient.m_PlayerInfo.m_Team == TEAM_SPECTATORS && Stats.IsActive())
				Stats.JoinSpec(Tick);
		}

		if(State.HasGameInfo())
		{
			const CNetObj_GameInfo &GameInfo = State.GameInfo();
			const bool GameOver = (GameInfo.m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) != 0;
			const bool GamePaused = (GameInfo.m_GameStateFlags & GAMESTATEFLAG_PAUSED) != 0;
			if(m_HasGameInfo && ((m_GameOver && !GameOver) || (GameInfo.m_RoundStartTick != m_LastRoundStartTick && !(GameOver || GamePaused || m_GamePaused))))
				ResetClients();
			m_HasGameInfo = true;
			m_LastRoundStartTick = GameInfo.m_RoundStartTick;
			m_GameOver = GameOver;
			m_GamePaused = GamePaused;
		}

		if(const CNetObj_GameData *pGameData = State.GameData())
		{
			if(m_LastFlagCarrierRed == FLAG_ATSTAND && pGameData->m_FlagCarrierRed >= 0)
				m_aClients[pGameData->m_FlagCarrierRed].m_FlagGrabs++;
			else if(m_LastFlagCarrierBlue == FLAG_ATSTAND && pGameData->m_FlagCarrierBlue >= 0)
				m_aClients[pGameData->m_FlagCarrierBlue].m_FlagGrabs++;
			m_LastFlagCarrierRed = pGameData->m_FlagCarrierRed;
			m_LastFlagCarrierBlue = pGameData->m_FlagCarrierBlue;
		}
	}
	CSessionClientStats &Client(int ClientId) { return m_aClients[ClientId]; }
	const CSessionClientStats &Client(int ClientId) const { return m_aClients[ClientId]; }
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

class CGameSessionContext
{
	CSessionId m_Id;
	std::string m_MapName;
	EGameProtocol m_Protocol;
	bool m_ServerCapAnyPlayerFlag = false;
	CMapContext m_MapContext;
	CGameStateManager m_GameStates;
	CStreamInputRouter m_InputRouter;
	CLocalPlayerProfileBindings m_LocalPlayerProfiles;
	CSessionBroadcastState m_Broadcast;
	CSessionMotdState m_Motd;
	CSessionMapMetadataState m_MapMetadata;
	CSessionInfoMessageState m_InfoMessages;
	CSessionChatState m_Chat;
	CSessionStatsState m_Stats;
	CSessionVoteState m_Vote;

public:
	CGameSessionContext(CSessionId Id, const char *pMapName, EGameProtocol Protocol, std::initializer_list<CStreamId> StreamIds) :
		m_Id(Id),
		m_MapName(pMapName),
		m_Protocol(Protocol)
	{
		for(CStreamId StreamId : StreamIds)
			m_GameStates.Create(StreamId);
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
	CMapContext &MapContext() { return m_MapContext; }
	const CMapContext &MapContext() const { return m_MapContext; }
	CGameStateManager &GameStates() { return m_GameStates; }
	const CGameStateManager &GameStates() const { return m_GameStates; }
	CStreamInputRouter &InputRouter() { return m_InputRouter; }
	const CStreamInputRouter &InputRouter() const { return m_InputRouter; }
	CLocalPlayerProfileBindings &LocalPlayerProfiles() { return m_LocalPlayerProfiles; }
	CSessionBroadcastState &Broadcast() { return m_Broadcast; }
	const CSessionBroadcastState &Broadcast() const { return m_Broadcast; }
	CSessionMotdState &Motd() { return m_Motd; }
	const CSessionMotdState &Motd() const { return m_Motd; }
	CSessionMapMetadataState &MapMetadata() { return m_MapMetadata; }
	const CSessionMapMetadataState &MapMetadata() const { return m_MapMetadata; }
	CSessionInfoMessageState &InfoMessages() { return m_InfoMessages; }
	const CSessionInfoMessageState &InfoMessages() const { return m_InfoMessages; }
	CSessionChatState &Chat() { return m_Chat; }
	const CSessionChatState &Chat() const { return m_Chat; }
	CSessionStatsState &Stats() { return m_Stats; }
	const CSessionStatsState &Stats() const { return m_Stats; }
	CSessionVoteState &Vote() { return m_Vote; }
	const CSessionVoteState &Vote() const { return m_Vote; }
};

class CGameSessionContextManager
{
	std::vector<std::unique_ptr<CGameSessionContext>> m_vpContexts;

public:
	CGameSessionContext *Create(CSessionId Id, const char *pMapName, EGameProtocol Protocol, std::initializer_list<CStreamId> StreamIds)
	{
		if(!Id.IsValid() || Find(Id))
			return nullptr;
		m_vpContexts.push_back(std::make_unique<CGameSessionContext>(Id, pMapName, Protocol, StreamIds));
		return m_vpContexts.back().get();
	}

	CGameSessionContext *Find(CSessionId Id)
	{
		const auto It = std::find_if(m_vpContexts.begin(), m_vpContexts.end(), [Id](const auto &pContext) { return pContext->Id() == Id; });
		return It == m_vpContexts.end() ? nullptr : It->get();
	}

	const CGameSessionContext *Find(CSessionId Id) const
	{
		const auto It = std::find_if(m_vpContexts.begin(), m_vpContexts.end(), [Id](const auto &pContext) { return pContext->Id() == Id; });
		return It == m_vpContexts.end() ? nullptr : It->get();
	}

	const std::vector<std::unique_ptr<CGameSessionContext>> &Contexts() const { return m_vpContexts; }
};

#endif // GAME_CLIENT_SESSION_CONTEXT_H
