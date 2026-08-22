#ifndef GAME_SERVER_MODE_MATCH_RECORDER_H
#define GAME_SERVER_MODE_MATCH_RECORDER_H

#include <engine/shared/protocol.h>

#include <generated/protocol.h>

#include <game/match_report.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class IServer;

// Counts what happens in one round and turns it into a report.
class CMatchRecorder
{
public:
	class CParticipant
	{
	public:
		uint32_t m_UniqueClientId;
		CMatchParticipant m_Info;
		// -1 while spectating or gone
		int m_PlayingSinceTick = -1;
		int64_t m_PlaytimeTicks = 0;
		int m_Score = 0;
		int64_t m_Suicides = 0;
		std::array<int64_t, NUM_MATCH_COMBAT_STATS> m_aCombat{};
		std::array<std::array<int64_t, NUM_MATCH_COMBAT_STATS>, NUM_WEAPONS> m_aaWeaponCombat{};
		// what a mode counts on top, flag captures for example; the ids are static strings
		std::vector<std::pair<const char *, int64_t>> m_vExtras;
	};

private:
	// the report without its results, set while a round is recorded
	std::optional<CMatchReport> m_Header;
	std::vector<CParticipant> m_vParticipants;
	std::optional<std::array<int, 2>> m_TeamScores;
	bool m_Overflow = false;

public:
	void Start(CMatchReport Header);
	void Stop() { m_Header.reset(); }
	bool IsRunning() const { return m_Header.has_value(); }
	CUuid MatchId() const { return m_Header ? m_Header->m_MatchId : UUID_ZEROED; }

	CParticipant *Participant(uint32_t UniqueClientId);
	// a new participant, nullptr when the report is full
	CParticipant *Join(uint32_t UniqueClientId, int Tick);
	void SetPlaying(CParticipant &Participant, bool Playing, int Tick);
	void SetTeamScores(int Red, int Blue) { m_TeamScores = {Red, Blue}; }

	static void AddCombat(CParticipant &Participant, EMatchCombatStat Stat, int Weapon, int64_t Value);
	static void AddExtra(CParticipant &Participant, const char *pMetricId, int64_t Value);

	/**
	 * The report of the round as it stands.
	 *
	 * The final report and the live one sent while the round runs come from here.
	 */
	CMatchReport Report(int Tick, EMatchTermination Termination, bool SuddenDeath) const;
};

// Hands reports to clients, a few chunks per tick so that the resend buffer of a connection does not overflow.
class CMatchReportSender
{
	class CSend
	{
	public:
		int m_ClientId;
		bool m_Live;
		CUuid m_MatchId;
		std::shared_ptr<const std::string> m_pPayload;
		// the tick the send started in, which already had its chunks
		int m_Tick;
		int m_NextChunk = 0;
	};
	std::vector<CSend> m_vSends;
	std::array<int, MAX_CLIENTS> m_aLastLiveRequestTick;

	// true once the send is done
	static bool SendChunks(IServer *pServer, CSend &Send);

public:
	CMatchReportSender() { m_aLastLiveRequestTick.fill(-1); }

	// what is still being sent to the client is dropped, except a final report for a live one
	void Send(IServer *pServer, int ClientId, const CMatchReport &Report, std::shared_ptr<const std::string> pPayload, bool Live, bool PersistOnDisconnect, int LocalParticipantId);
	void Tick(IServer *pServer);
	// false while the client asks more often than every two seconds
	bool AcceptLiveRequest(int ClientId, int Tick, int TickSpeed);
};

#endif // GAME_SERVER_MODE_MATCH_RECORDER_H
