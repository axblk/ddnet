#include "match_collector.h"

#include "game_state.h"

#include <base/str.h>

#include <game/gamecore.h>

#include <algorithm>
#include <limits>

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
		if(SnapshotClient.m_PlayerInfo.m_Team != TEAM_SPECTATORS)
			Stats.JoinGame(Tick);
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
	const CGameState::CClientIdentityState &Identity = State.ClientIdentity(LocalClientId);
	if(!Identity.m_Active)
		return false;

	char aName[MAX_NAME_LENGTH];
	char aClan[MAX_CLAN_LENGTH];
	if(!IntsToStr(Identity.m_ClientInfo.m_aName, std::size(Identity.m_ClientInfo.m_aName), aName, sizeof(aName)))
		return false;
	if(!IntsToStr(Identity.m_ClientInfo.m_aClan, std::size(Identity.m_ClientInfo.m_aClan), aClan, sizeof(aClan)))
		aClan[0] = '\0';

	const CSessionClientStats &Stats = m_aClients[LocalClientId];
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
	const CGameState::CClientSnapshot &LocalSnapshot = State.Client(LocalClientId);
	const std::optional<int> TeamId = LocalSnapshot.m_HasPlayerInfo && LocalSnapshot.m_PlayerInfo.m_Team >= TEAM_RED ? std::optional<int>(LocalSnapshot.m_PlayerInfo.m_Team) : std::nullopt;
	if(TeamId.has_value())
		Report.m_vTeams.push_back({*TeamId, *TeamId == TEAM_RED ? "Red" : "Blue"});
	Report.m_vParticipants.push_back({0, TeamId, aName, aClan, std::clamp<int64_t>(Stats.FirstJoinTick() - m_RoundStartTick, 0, DurationTicks), std::nullopt, false});

	int Rank = 1;
	EMatchOutcome Outcome = EMatchOutcome::DRAW;
	const int LocalScore = LocalSnapshot.m_HasPlayerInfo ? LocalSnapshot.m_PlayerInfo.m_Score : 0;
	if((State.GameInfo().m_GameFlags & GAMEFLAG_TEAMS) != 0 && State.GameData() != nullptr && TeamId.has_value())
	{
		const int OwnScore = *TeamId == TEAM_RED ? State.GameData()->m_TeamscoreRed : State.GameData()->m_TeamscoreBlue;
		const int OtherScore = *TeamId == TEAM_RED ? State.GameData()->m_TeamscoreBlue : State.GameData()->m_TeamscoreRed;
		Rank = OwnScore >= OtherScore ? 1 : 2;
		Outcome = OwnScore > OtherScore ? EMatchOutcome::WIN : OwnScore < OtherScore ? EMatchOutcome::LOSS :
											       EMatchOutcome::DRAW;
	}
	else
	{
		int NumEqual = 0;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			const CGameState::CClientSnapshot &Client = State.Client(ClientId);
			if(!Client.m_HasPlayerInfo || Client.m_PlayerInfo.m_Team == TEAM_SPECTATORS)
				continue;
			Rank += Client.m_PlayerInfo.m_Score > LocalScore ? 1 : 0;
			NumEqual += Client.m_PlayerInfo.m_Score == LocalScore ? 1 : 0;
		}
		Outcome = Rank == 1 && NumEqual == 1 ? EMatchOutcome::WIN : Rank == 1 ? EMatchOutcome::DRAW :
											EMatchOutcome::LOSS;
	}
	if(Metadata.m_Termination != EMatchTermination::COMPLETED)
	{
		Rank = 1;
		Outcome = EMatchOutcome::DNF;
	}
	Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, 0, Rank, Outcome});
	const std::string MetricPrefix = Metadata.m_ModeId + "/";
	Report.m_vMetrics = {
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "score", LocalScore},
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "kills", Stats.m_Frags},
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "deaths", Stats.m_Deaths},
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "suicides", Stats.m_Suicides},
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "best_spree", Stats.m_BestSpree, EMatchMetricAggregation::MAXIMUM},
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "flag_grabs", Stats.m_FlagGrabs},
		{EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "playtime_ticks", Stats.GetIngameTicks(Tick)}};
	for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
	{
		if(Stats.m_aFragsWith[Weapon] != 0)
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "weapon_" + std::to_string(Weapon) + "_kills", Stats.m_aFragsWith[Weapon]});
		if(Stats.m_aDeathsFrom[Weapon] != 0)
			Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, 0, MetricPrefix + "weapon_" + std::to_string(Weapon) + "_deaths", Stats.m_aDeathsFrom[Weapon]});
	}

	CStoredMatch Stored;
	Stored.m_OriginId = Metadata.m_OriginId;
	Stored.m_Source = EMatchReportSource::CLIENT_OBSERVED;
	Stored.m_Completeness = Metadata.m_Termination == EMatchTermination::COMPLETED ? (m_Complete ? EMatchCompleteness::COMPLETE : EMatchCompleteness::PARTIAL_SINCE_JOIN) : EMatchCompleteness::ABORTED;
	Stored.m_LocalParticipantId = 0;
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
