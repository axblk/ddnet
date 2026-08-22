#include "match_collector.h"

#include "game_state.h"

#include <base/str.h>

#include <game/gamecore.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace
{
	/**
	 * Counts the weapon a dying client was holding.
	 *
	 * The kill message says which weapon did the killing, never which one the
	 * victim had drawn, so this reads it from the snapshot instead. That
	 * snapshot is the last one before the death, which makes the count
	 * approximate: a victim who switched weapons in the fraction of a second
	 * before dying is counted with the weapon they switched away from.
	 */
	void CountDeathHolding(const CGameState &State, CSessionClientStats &Stats, int ClientId)
	{
		const CGameState::CClientSnapshot &Snapshot = State.Client(ClientId);
		if(Snapshot.m_HasCharacter && Snapshot.m_Character.m_Weapon >= 0 && Snapshot.m_Character.m_Weapon < NUM_WEAPONS)
			Stats.m_aDeathsHolding[Snapshot.m_Character.m_Weapon]++;
	}
}

void CSessionClientStats::JoinGame(int Tick)
{
	if(!m_HasJoined)
	{
		m_HasJoined = true;
		m_FirstJoinTick = Tick;
	}
	m_Present = true;
	if(!m_Active)
	{
		m_Active = true;
		m_JoinTick = Tick;
	}
}

void CSessionClientStats::JoinSpec(int Tick)
{
	m_Present = true;
	if(m_Active)
	{
		m_IngameTicks += std::max(0, Tick - m_JoinTick);
		m_Active = false;
		m_LeaveTick = Tick;
	}
}

void CSessionClientStats::Leave(int Tick)
{
	JoinSpec(Tick);
	m_Present = false;
}

int64_t CSessionClientStats::GetIngameTicks(int Tick) const
{
	return m_IngameTicks + (m_Active ? std::max(0, Tick - m_JoinTick) : 0);
}

float CSessionClientStats::GetFPM(int Tick, int TickSpeed) const
{
	const int64_t Ticks = GetIngameTicks(Tick);
	return Ticks > 0 ? static_cast<float>(m_Frags * TickSpeed * 60) / Ticks : 0.0f;
}

void CSessionStatsState::ResetClients()
{
	for(CSessionClientStats &Client : m_aClients)
		Client.Reset();
}

void CSessionStatsState::StartMatch(int RoundStartTick, bool Complete)
{
	ResetClients();
	m_RoundStartTick = RoundStartTick;
	m_SawRunningSnapshot = false;
	m_Complete = Complete;
	m_Finalized = false;
	m_ServerReportRoundStartTick = -1;
	if(m_LatestMatch.has_value() && m_LatestMatch->m_Source == EMatchReportSource::CLIENT_OBSERVED)
		m_PreviousObservedMatch = std::move(m_LatestMatch);
	else
		m_PreviousObservedMatch.reset();
	m_LatestMatch.reset();
	m_LastFlagCarrierRed = FLAG_MISSING;
	m_LastFlagCarrierBlue = FLAG_MISSING;
}

void CSessionStatsState::Reset()
{
	ResetClients();
	m_HasGameInfo = false;
	m_RoundStartTick = 0;
	m_GameOver = false;
	m_GamePaused = false;
	m_SawRunningSnapshot = false;
	m_Complete = false;
	m_Finalized = false;
	m_ServerReportRoundStartTick = -1;
	m_LastFlagCarrierRed = FLAG_MISSING;
	m_LastFlagCarrierBlue = FLAG_MISSING;
	m_LatestMatch.reset();
	m_PreviousObservedMatch.reset();
}

bool CSessionStatsState::UpdateSnapshot(const CGameState &State, int Tick)
{
	if(!State.HasGameInfo())
		return false;
	const CNetObj_GameInfo &GameInfo = State.GameInfo();
	const bool GameOver = (GameInfo.m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) != 0;
	const bool GamePaused = (GameInfo.m_GameStateFlags & GAMESTATEFLAG_PAUSED) != 0;
	const bool EnteredGameOver = m_HasGameInfo && !m_GameOver && GameOver && m_SawRunningSnapshot && !m_Finalized;
	const bool NewRound = m_HasGameInfo && !GameOver && (m_GameOver || (GameInfo.m_RoundStartTick != m_RoundStartTick && !(GamePaused || m_GamePaused)));
	if(!m_HasGameInfo)
		StartMatch(GameInfo.m_RoundStartTick, Tick <= GameInfo.m_RoundStartTick + 1);
	else if(NewRound)
		StartMatch(GameInfo.m_RoundStartTick, true);

	for(int ClientId = 0; !NewRound && ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CGameState::CClientSnapshot &SnapshotClient = State.Client(ClientId);
		const bool Present = SnapshotClient.m_HasPlayerInfo && SnapshotClient.m_PlayerInfo.m_ClientId == ClientId;
		CSessionClientStats &Stats = m_aClients[ClientId];
		if(!Present)
		{
			if(Stats.IsPresent())
				Stats.Leave(Tick);
			continue;
		}
		if(!Stats.IsPresent() && Stats.HasJoined())
			Stats.Reset();
		const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
		if(Identity.m_Active)
		{
			Stats.m_ClientInfo = Identity.m_ClientInfo;
			Stats.m_HasClientInfo = true;
		}
		Stats.m_LastScore = SnapshotClient.m_PlayerInfo.m_Score;
		if(SnapshotClient.m_PlayerInfo.m_Team != TEAM_SPECTATORS)
		{
			Stats.m_LastTeam = SnapshotClient.m_PlayerInfo.m_Team;
			Stats.JoinGame(Tick);
		}
		else
			Stats.JoinSpec(Tick);
	}

	if(!GameOver && !GamePaused)
		m_SawRunningSnapshot = true;
	if(const CNetObj_GameData *pGameData = State.GameData())
	{
		if(m_LastFlagCarrierRed == FLAG_ATSTAND && pGameData->m_FlagCarrierRed >= 0 && pGameData->m_FlagCarrierRed < MAX_CLIENTS)
			m_aClients[pGameData->m_FlagCarrierRed].m_FlagGrabs++;
		if(m_LastFlagCarrierBlue == FLAG_ATSTAND && pGameData->m_FlagCarrierBlue >= 0 && pGameData->m_FlagCarrierBlue < MAX_CLIENTS)
			m_aClients[pGameData->m_FlagCarrierBlue].m_FlagGrabs++;
		m_LastFlagCarrierRed = pGameData->m_FlagCarrierRed;
		m_LastFlagCarrierBlue = pGameData->m_FlagCarrierBlue;
	}

	m_HasGameInfo = true;
	m_GameOver = GameOver;
	m_GamePaused = GamePaused;
	return EnteredGameOver;
}

void CSessionStatsState::HandleMessage(const CGameState &State, bool SuppressEvents, int MsgType, void *pRawMsg)
{
	if(SuppressEvents)
		return;
	if(MsgType == NETMSGTYPE_SV_KILLMSG)
	{
		const CNetMsg_Sv_KillMsg *pMsg = static_cast<CNetMsg_Sv_KillMsg *>(pRawMsg);
		if(pMsg->m_Victim < 0 || pMsg->m_Victim >= MAX_CLIENTS || pMsg->m_Killer < 0 || pMsg->m_Killer >= MAX_CLIENTS)
			return;
		CSessionClientStats &VictimStats = Client(pMsg->m_Victim);
		VictimStats.m_Deaths++;
		VictimStats.m_CurrentSpree = 0;
		CountDeathHolding(State, VictimStats, pMsg->m_Victim);
		if(pMsg->m_Weapon >= 0 && pMsg->m_Weapon < NUM_WEAPONS)
			VictimStats.m_aDeathsFrom[pMsg->m_Weapon]++;
		if(pMsg->m_Victim == pMsg->m_Killer)
		{
			VictimStats.m_Suicides++;
			return;
		}
		CSessionClientStats &KillerStats = Client(pMsg->m_Killer);
		KillerStats.m_Frags++;
		KillerStats.m_CurrentSpree++;
		KillerStats.m_BestSpree = std::max(KillerStats.m_BestSpree, KillerStats.m_CurrentSpree);
		if(pMsg->m_Weapon >= 0 && pMsg->m_Weapon < NUM_WEAPONS)
			KillerStats.m_aFragsWith[pMsg->m_Weapon]++;
	}
	else if(MsgType == NETMSGTYPE_SV_KILLMSGTEAM)
	{
		const CNetMsg_Sv_KillMsgTeam *pMsg = static_cast<CNetMsg_Sv_KillMsgTeam *>(pRawMsg);
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(State.Teams().Team(ClientId) == pMsg->m_Team)
			{
				Client(ClientId).m_Deaths++;
				Client(ClientId).m_Suicides++;
				CountDeathHolding(State, Client(ClientId), ClientId);
			}
		}
	}
	else if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		const CNetMsg_Sv_Chat *pMsg = static_cast<CNetMsg_Sv_Chat *>(pRawMsg);
		if(pMsg->m_ClientId >= 0)
			return;
		const char *pCapture = str_find(pMsg->m_pMessage, "flag was captured by '");
		if(pCapture == nullptr)
			return;
		pCapture += str_length("flag was captured by '");
		const char *pEnd = str_rchr(pCapture, '\'');
		if(pEnd == nullptr || pEnd <= pCapture)
			return;
		char aName[MAX_NAME_LENGTH];
		str_truncate(aName, sizeof(aName), pCapture, pEnd - pCapture);
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			const CGameState::CClientIdentityState &Identity = State.ClientIdentity(ClientId);
			char aClientName[MAX_NAME_LENGTH];
			if(Identity.m_Active && IntsToStr(Identity.m_ClientInfo.m_aName, std::size(Identity.m_ClientInfo.m_aName), aClientName, sizeof(aClientName)) && str_comp(aClientName, aName) == 0)
			{
				Client(ClientId).m_FlagCaptures++;
				break;
			}
		}
	}
}

bool CSessionStatsState::FinalizeObservedMatch(const CObservedMatchMetadata &Metadata, const CGameState &State, int Tick, std::string *pError)
{
	if(m_Finalized || !m_SawRunningSnapshot || !State.HasGameInfo() || (Metadata.m_Termination == EMatchTermination::COMPLETED && (State.GameInfo().m_GameStateFlags & GAMESTATEFLAG_GAMEOVER) == 0) || Metadata.m_TickRate <= 0)
		return false;
	const int LocalClientId = State.LocalClientId();
	if(LocalClientId < 0 || LocalClientId >= MAX_CLIENTS || !m_aClients[LocalClientId].HasJoined())
		return false;

	const int64_t DurationTicks = std::max<int64_t>(0, Tick - m_RoundStartTick);
	CMatchReport Report;
	Report.m_MatchId = RandomUuid();
	Report.m_ModeId = Metadata.m_ModeId;
	Report.m_MapName = Metadata.m_MapName;
	Report.m_MapSha256 = Metadata.m_MapSha256;
	Report.m_EndTimeUtc = Metadata.m_EndTimeUtc;
	Report.m_StartTimeUtc = Metadata.m_EndTimeUtc - DurationTicks / Metadata.m_TickRate;
	Report.m_DurationTicks = DurationTicks;
	Report.m_TickRate = Metadata.m_TickRate;
	Report.m_RoundStartTick = m_RoundStartTick;
	Report.m_Termination = Metadata.m_Termination;
	Report.m_Ranked = false;
	Report.m_UnrankedReason = "client_observed";
	const bool TeamPlay = (State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0;
	if(TeamPlay)
	{
		Report.m_vTeams.push_back({TEAM_RED, "Red"});
		Report.m_vTeams.push_back({TEAM_BLUE, "Blue"});
	}

	// Client ids double as participant ids. They are unique for as long as a
	// client is connected and a report only asks its ids to be distinct, so this
	// saves carrying a second numbering next to the one every lookup here already
	// uses. Exceeding the report limits would only show up when the finished
	// report is rejected at the end of the match, when the counts it holds are
	// already gone, so the bounds that rule that out are a build error instead.
	constexpr int MAX_OBSERVED_METRICS_PER_PARTICIPANT = 7 + 3 * NUM_WEAPONS;
	static_assert(MAX_CLIENTS <= MatchReportLimits::MAX_PARTICIPANTS);
	static_assert(MAX_OBSERVED_METRICS_PER_PARTICIPANT <= MatchReportLimits::MAX_METRICS_PER_PARTICIPANT);
	static_assert(MAX_CLIENTS * MAX_OBSERVED_METRICS_PER_PARTICIPANT + 2 <= MatchReportLimits::MAX_METRICS);
	const std::string MetricPrefix = Metadata.m_ModeId + "/";
	bool HasLocalParticipant = false;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		const CSessionClientStats &Stats = m_aClients[ClientId];
		char aName[MAX_NAME_LENGTH];
		char aClan[MAX_CLAN_LENGTH];
		// A client that never played has nothing to report, and one whose name
		// never arrived cannot be named: an empty name would make the whole
		// report invalid and cost every other client their statistics too.
		if(!Stats.HasJoined() || !Stats.m_HasClientInfo || !IntsToStr(Stats.m_ClientInfo.m_aName, std::size(Stats.m_ClientInfo.m_aName), aName, sizeof(aName)) || aName[0] == '\0')
			continue;
		if(!IntsToStr(Stats.m_ClientInfo.m_aClan, std::size(Stats.m_ClientInfo.m_aClan), aClan, sizeof(aClan)))
			aClan[0] = '\0';
		HasLocalParticipant = HasLocalParticipant || ClientId == LocalClientId;

		CMatchParticipant Participant;
		Participant.m_ParticipantId = ClientId;
		if(TeamPlay && (Stats.m_LastTeam == TEAM_RED || Stats.m_LastTeam == TEAM_BLUE))
			Participant.m_TeamId = Stats.m_LastTeam;
		Participant.m_DisplayName = aName;
		Participant.m_Clan = aClan;
		Participant.m_JoinedTick = std::clamp<int64_t>(Stats.FirstJoinTick() - m_RoundStartTick, 0, DurationTicks);
		// Only a client that is still playing has an open end. One that went to
		// the spectators or disconnected stopped taking part when it did so,
		// which is also the moment its playtime stopped counting.
		if(!Stats.IsActive())
			Participant.m_LeftTick = std::clamp<int64_t>(Stats.LeaveTick() - m_RoundStartTick, Participant.m_JoinedTick, DurationTicks);
		// Nothing the client receives tells a bot or a dummy apart from a human,
		// so claiming either way would put a guess into the journal.
		Report.m_vParticipants.push_back(std::move(Participant));

		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "score", Stats.m_LastScore});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "kills", Stats.m_Frags});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "deaths", Stats.m_Deaths});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "suicides", Stats.m_Suicides});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "best_spree", Stats.m_BestSpree, EMatchMetricAggregation::MAXIMUM});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "flag_grabs", Stats.m_FlagGrabs});
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, MetricPrefix + "playtime_ticks", Stats.GetIngameTicks(Tick)});
		for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
		{
			const std::string WeaponPrefix = MetricPrefix + "weapon_" + std::to_string(Weapon) + "_";
			if(Stats.m_aFragsWith[Weapon] != 0)
				Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, WeaponPrefix + "kills", Stats.m_aFragsWith[Weapon]});
			if(Stats.m_aDeathsFrom[Weapon] != 0)
				Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, WeaponPrefix + "deaths", Stats.m_aDeathsFrom[Weapon]});
			if(Stats.m_aDeathsHolding[Weapon] != 0)
				Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ClientId, WeaponPrefix + "deaths_holding", Stats.m_aDeathsHolding[Weapon]});
		}
	}
	// The report is the local player's own record of the match, so it is worth
	// nothing without them in it.
	if(!HasLocalParticipant)
		return false;

	// Both ways of ranking follow what the server writes into its own reports, so
	// that a match seen by the client places its players like the same match
	// reported by a server that knows the mode.
	if(TeamPlay && State.GameData() != nullptr)
	{
		const int RedScore = State.GameData()->m_TeamscoreRed;
		const int BlueScore = State.GameData()->m_TeamscoreBlue;
		const bool Draw = RedScore == BlueScore;
		for(int Team = TEAM_RED; Team <= TEAM_BLUE; ++Team)
		{
			const bool Won = Draw || (Team == TEAM_RED ? RedScore > BlueScore : BlueScore > RedScore);
			Report.m_vStandings.push_back({EMatchSubjectKind::TEAM, Team, Won ? 1 : 2, Draw ? EMatchOutcome::DRAW : (Won ? EMatchOutcome::WIN : EMatchOutcome::LOSS)});
			Report.m_vMetrics.push_back({EMatchSubjectKind::TEAM, Team, MetricPrefix + "score", Team == TEAM_RED ? RedScore : BlueScore});
		}
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
		{
			if(!Participant.m_TeamId.has_value())
				continue;
			const bool Won = Draw || (*Participant.m_TeamId == TEAM_RED ? RedScore > BlueScore : BlueScore > RedScore);
			Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId, Won ? 1 : 2, Draw ? EMatchOutcome::DRAW : (Won ? EMatchOutcome::WIN : EMatchOutcome::LOSS)});
		}
	}
	else
	{
		std::vector<int> vRankedIds;
		vRankedIds.reserve(Report.m_vParticipants.size());
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
			vRankedIds.push_back(Participant.m_ParticipantId);
		std::stable_sort(vRankedIds.begin(), vRankedIds.end(), [this](int Left, int Right) { return m_aClients[Left].m_LastScore > m_aClients[Right].m_LastScore; });
		const bool TopTied = vRankedIds.size() > 1 && m_aClients[vRankedIds[0]].m_LastScore == m_aClients[vRankedIds[1]].m_LastScore;
		int Rank = 0;
		for(size_t Index = 0; Index < vRankedIds.size(); ++Index)
		{
			// Everyone on the same score shares the rank of the first of them, and
			// the ranks after them keep the gap that the tie leaves behind.
			if(Index == 0 || m_aClients[vRankedIds[Index]].m_LastScore != m_aClients[vRankedIds[Index - 1]].m_LastScore)
				Rank = static_cast<int>(Index) + 1;
			Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, vRankedIds[Index], Rank, Rank == 1 ? (TopTied ? EMatchOutcome::DRAW : EMatchOutcome::WIN) : EMatchOutcome::LOSS});
		}
	}
	if(Metadata.m_Termination != EMatchTermination::COMPLETED)
	{
		for(CMatchStanding &Standing : Report.m_vStandings)
			Standing.m_Outcome = EMatchOutcome::DNF;
	}

	CStoredMatch Stored;
	Stored.m_OriginId = Metadata.m_OriginId;
	Stored.m_Source = EMatchReportSource::CLIENT_OBSERVED;
	Stored.m_Completeness = Metadata.m_Termination == EMatchTermination::COMPLETED ? (m_Complete ? EMatchCompleteness::COMPLETE : EMatchCompleteness::PARTIAL_SINCE_JOIN) : EMatchCompleteness::ABORTED;
	Stored.m_LocalParticipantId = LocalClientId;
	Stored.m_Report = std::move(Report);
	if(!MatchReportToJson(Stored.m_Report, Stored.m_RawReport, pError))
		return false;
	m_LatestMatch = std::move(Stored);
	m_Finalized = true;
	return true;
}

bool CSessionStatsState::IsCurrentServerMatch(const CMatchReport &Report) const
{
	return m_HasGameInfo && Report.m_RoundStartTick == m_RoundStartTick && m_ServerReportRoundStartTick != m_RoundStartTick;
}

void CSessionStatsState::SetLatestServerMatch(CStoredMatch Match)
{
	dbg_assert(IsCurrentServerMatch(Match.m_Report), "server report does not match current round");
	m_LatestMatch = std::move(Match);
	m_Finalized = true;
	m_ServerReportRoundStartTick = m_RoundStartTick;
}
