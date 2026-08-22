#ifndef GAME_CLIENT_MATCH_COLLECTOR_H
#define GAME_CLIENT_MATCH_COLLECTOR_H

#include "match_journal.h"

#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>

class CGameState;

class CObservedMatchMetadata
{
public:
	std::string m_OriginId;
	std::string m_ModeId;
	std::string m_MapName;
	SHA256_DIGEST m_MapSha256 = {};
	int64_t m_EndTimeUtc = 0;
	int m_TickRate = 0;
	EMatchTermination m_Termination = EMatchTermination::COMPLETED;
};

class CSessionClientStats
{
	int64_t m_IngameTicks = 0;
	int m_JoinTick = 0;
	int m_FirstJoinTick = 0;
	bool m_HasJoined = false;
	bool m_Present = false;
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
	bool IsPresent() const { return m_Present; }
	bool IsActive() const { return m_Active; }
	bool HasJoined() const { return m_HasJoined; }
	int FirstJoinTick() const { return m_FirstJoinTick; }
	void JoinGame(int Tick);
	void JoinSpec(int Tick);
	void Leave(int Tick);
	int64_t GetIngameTicks(int Tick) const;
	float GetFPM(int Tick, int TickSpeed) const;
};

class CSessionStatsState
{
	std::array<CSessionClientStats, MAX_CLIENTS> m_aClients;
	bool m_HasGameInfo = false;
	int m_RoundStartTick = 0;
	bool m_GameOver = false;
	bool m_GamePaused = false;
	bool m_SawRunningSnapshot = false;
	bool m_Complete = false;
	bool m_Finalized = false;
	int m_ServerReportRoundStartTick = -1;
	int m_LastFlagCarrierRed = FLAG_MISSING;
	int m_LastFlagCarrierBlue = FLAG_MISSING;
	std::optional<CStoredMatch> m_LatestMatch;
	std::optional<CStoredMatch> m_PreviousObservedMatch;

	void ResetClients();
	void StartMatch(int RoundStartTick, bool Complete);

public:
	void Reset();
	bool UpdateSnapshot(const CGameState &State, int Tick);
	void HandleMessage(const CGameState &State, bool SuppressEvents, int MsgType, void *pRawMsg);
	bool FinalizeObservedMatch(const CObservedMatchMetadata &Metadata, const CGameState &State, int Tick, std::string *pError);
	bool IsCurrentServerMatch(const CMatchReport &Report) const;
	void SetLatestServerMatch(CStoredMatch Match);
	void ClearPreviousObservedMatch() { m_PreviousObservedMatch.reset(); }

	CSessionClientStats &Client(int ClientId) { return m_aClients[ClientId]; }
	const CSessionClientStats &Client(int ClientId) const { return m_aClients[ClientId]; }
	const std::optional<CStoredMatch> &LatestMatch() const { return m_LatestMatch; }
	const std::optional<CStoredMatch> &ObservedMatchForReplacement() const { return m_LatestMatch.has_value() && m_LatestMatch->m_Source == EMatchReportSource::CLIENT_OBSERVED ? m_LatestMatch : m_PreviousObservedMatch; }
};

#endif // GAME_CLIENT_MATCH_COLLECTOR_H
