#include "match_recorder.h"

#include <base/log.h>
#include <base/time.h>

#include <engine/message.h>
#include <engine/server.h>
#include <engine/shared/protocol_ex.h>

#include <algorithm>

void CMatchRecorder::Start(CMatchReport Header)
{
	m_Header = std::move(Header);
	m_vParticipants.clear();
	m_TeamScores.reset();
	m_Overflow = false;
}

CMatchRecorder::CParticipant *CMatchRecorder::Participant(uint32_t UniqueClientId)
{
	if(!IsRunning())
		return nullptr;
	const auto It = std::find_if(m_vParticipants.begin(), m_vParticipants.end(), [UniqueClientId](const CParticipant &Participant) { return Participant.m_UniqueClientId == UniqueClientId; });
	return It == m_vParticipants.end() ? nullptr : &*It;
}

CMatchRecorder::CParticipant *CMatchRecorder::Join(uint32_t UniqueClientId, int Tick)
{
	if(!IsRunning())
		return nullptr;
	if(m_vParticipants.size() >= (size_t)MatchReportLimits::MAX_PARTICIPANTS)
	{
		// everyone who got in keeps their numbers, whoever comes later is not counted
		if(!m_Overflow)
			log_warn("game", "match report participant limit reached, later players are not counted");
		m_Overflow = true;
		return nullptr;
	}
	CParticipant &Participant = m_vParticipants.emplace_back();
	Participant.m_UniqueClientId = UniqueClientId;
	Participant.m_Info.m_ParticipantId = (int)m_vParticipants.size() - 1;
	Participant.m_Info.m_JoinedTick = std::max(0, Tick - m_Header->m_RoundStartTick);
	return &Participant;
}

void CMatchRecorder::SetPlaying(CParticipant &Participant, bool Playing, int Tick)
{
	if(Playing && Participant.m_PlayingSinceTick < 0)
	{
		Participant.m_PlayingSinceTick = Tick;
		Participant.m_Info.m_LeftTick.reset();
	}
	else if(!Playing && Participant.m_PlayingSinceTick >= 0)
	{
		Participant.m_PlaytimeTicks += Tick - Participant.m_PlayingSinceTick;
		Participant.m_PlayingSinceTick = -1;
		Participant.m_Info.m_LeftTick = std::max(0, Tick - m_Header->m_RoundStartTick);
	}
}

void CMatchRecorder::AddCombat(CParticipant &Participant, EMatchCombatStat Stat, int Weapon, int64_t Value)
{
	Participant.m_aCombat[Stat] += Value;
	if(Weapon >= 0 && Weapon < NUM_WEAPONS)
		Participant.m_aaWeaponCombat[Weapon][Stat] += Value;
}

void CMatchRecorder::AddExtra(CParticipant &Participant, const char *pMetricId, int64_t Value)
{
	const auto It = std::find_if(Participant.m_vExtras.begin(), Participant.m_vExtras.end(), [pMetricId](const auto &Extra) { return str_comp(Extra.first, pMetricId) == 0; });
	if(It != Participant.m_vExtras.end())
		It->second += Value;
	else
		Participant.m_vExtras.emplace_back(pMetricId, Value);
}

CMatchReport CMatchRecorder::Report(int Tick, EMatchTermination Termination, bool SuddenDeath) const
{
	dbg_assert(IsRunning(), "no match is recorded");
	CMatchReport Report = *m_Header;
	Report.m_EndTimeUtc = time_timestamp();
	Report.m_DurationTicks = std::max(0, Tick - Report.m_RoundStartTick);
	Report.m_Termination = Termination;

	const auto AddMetric = [&](int ParticipantId, const char *pMetricId, int64_t Value) {
		const CMatchMetricInfo *pInfo = FindMatchMetric(pMetricId);
		Report.m_vMetrics.push_back({EMatchSubjectKind::PARTICIPANT, ParticipantId, pMetricId, std::clamp(Value, -MatchReportLimits::MAX_METRIC_VALUE, MatchReportLimits::MAX_METRIC_VALUE), pInfo ? pInfo->m_Aggregation : EMatchMetricAggregation::SUM});
	};
	for(const CParticipant &Participant : m_vParticipants)
	{
		CMatchParticipant Info = Participant.m_Info;
		Info.m_JoinedTick = std::min(Info.m_JoinedTick, Report.m_DurationTicks);
		if(Info.m_LeftTick)
			Info.m_LeftTick = std::clamp(*Info.m_LeftTick, Info.m_JoinedTick, Report.m_DurationTicks);
		Report.m_vParticipants.push_back(std::move(Info));

		const int Id = Participant.m_Info.m_ParticipantId;
		AddMetric(Id, "score", Participant.m_Score);
		AddMetric(Id, "playtime_ticks", Participant.m_PlaytimeTicks + (Participant.m_PlayingSinceTick >= 0 ? Tick - Participant.m_PlayingSinceTick : 0));
		if(Participant.m_Suicides)
			AddMetric(Id, "suicides", Participant.m_Suicides);
		for(int Stat = 0; Stat < NUM_MATCH_COMBAT_STATS; Stat++)
		{
			if(Participant.m_aCombat[Stat])
				AddMetric(Id, MatchCombatStatInfo(Stat).m_pId, Participant.m_aCombat[Stat]);
		}
		for(int Weapon = 0; Weapon < NUM_WEAPONS; Weapon++)
		{
			for(int Stat = 0; Stat < NUM_MATCH_COMBAT_STATS; Stat++)
			{
				if(!Participant.m_aaWeaponCombat[Weapon][Stat])
					continue;
				char aMetricId[32];
				str_format(aMetricId, sizeof(aMetricId), "weapon_%d_%s", Weapon, MatchCombatStatInfo(Stat).m_pId);
				AddMetric(Id, aMetricId, Participant.m_aaWeaponCombat[Weapon][Stat]);
			}
		}
		for(const auto &[pMetricId, Value] : Participant.m_vExtras)
			AddMetric(Id, pMetricId, Value);
	}
	if(SuddenDeath)
		Report.m_vMetrics.push_back({EMatchSubjectKind::MATCH, std::nullopt, "sudden_death", 1, EMatchMetricAggregation::MATCH_ONLY});

	const auto Outcome = [&](bool Won, bool Draw) {
		if(Termination != EMatchTermination::COMPLETED)
			return EMatchOutcome::DNF;
		return Draw ? EMatchOutcome::DRAW : Won ? EMatchOutcome::WIN :
							  EMatchOutcome::LOSS;
	};
	if(m_TeamScores)
	{
		const auto [Red, Blue] = *m_TeamScores;
		for(int Team = TEAM_RED; Team <= TEAM_BLUE; Team++)
		{
			const bool Won = (Team == TEAM_RED ? Red - Blue : Blue - Red) >= 0;
			Report.m_vStandings.push_back({EMatchSubjectKind::TEAM, Team, Won ? 1 : 2, Outcome(Won, Red == Blue)});
			Report.m_vMetrics.push_back({EMatchSubjectKind::TEAM, Team, "score", Team == TEAM_RED ? Red : Blue, EMatchMetricAggregation::SUM});
		}
		for(const CMatchParticipant &Participant : Report.m_vParticipants)
		{
			if(!Participant.m_TeamId)
				continue;
			const CMatchStanding &TeamStanding = Report.m_vStandings[*Participant.m_TeamId - TEAM_RED];
			Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId, TeamStanding.m_Rank, TeamStanding.m_Outcome});
		}
	}
	else
	{
		std::vector<const CParticipant *> vpRanked;
		vpRanked.reserve(m_vParticipants.size());
		for(const CParticipant &Participant : m_vParticipants)
			vpRanked.push_back(&Participant);
		std::stable_sort(vpRanked.begin(), vpRanked.end(), [](const CParticipant *pLeft, const CParticipant *pRight) { return pLeft->m_Score > pRight->m_Score; });
		const bool TopTied = vpRanked.size() > 1 && vpRanked[0]->m_Score == vpRanked[1]->m_Score;
		int Rank = 0;
		for(size_t i = 0; i < vpRanked.size(); i++)
		{
			if(i == 0 || vpRanked[i]->m_Score != vpRanked[i - 1]->m_Score)
				Rank = (int)i + 1;
			Report.m_vStandings.push_back({EMatchSubjectKind::PARTICIPANT, vpRanked[i]->m_Info.m_ParticipantId, Rank, Outcome(Rank == 1, Rank == 1 && TopTied)});
		}
	}
	return Report;
}

void CMatchReportSender::Send(IServer *pServer, int ClientId, const CMatchReport &Report, std::shared_ptr<const std::string> pPayload, bool Live, bool PersistOnDisconnect, int LocalParticipantId)
{
	const auto It = std::find_if(m_vSends.begin(), m_vSends.end(), [ClientId](const CSend &Send) { return Send.m_ClientId == ClientId; });
	if(It != m_vSends.end())
	{
		if(Live && !It->m_Live)
			return;
		m_vSends.erase(It);
	}

	CMsgPacker Start(NETMSG_MATCH_REPORT_START, false);
	Start.AddRaw(&Report.m_MatchId, sizeof(Report.m_MatchId));
	Start.AddInt(Live);
	Start.AddInt(PersistOnDisconnect);
	Start.AddInt(LocalParticipantId);
	Start.AddInt(pPayload->size());
	// only the participant can store its report, and no demo needs a copy of it
	if(pServer->SendMsg(&Start, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId) < 0)
		return;
	// the first chunks go out right away, so that a report that fits reaches the client even if the round ends with the map
	CSend Send{ClientId, Live, Report.m_MatchId, std::move(pPayload), pServer->Tick()};
	if(!SendChunks(pServer, Send))
		m_vSends.push_back(std::move(Send));
}

bool CMatchReportSender::SendChunks(IServer *pServer, CSend &Send)
{
	// 8 chunks are roughly 7 KB, which leaves the resend buffer of a connection several ticks to drain
	static constexpr int CHUNKS_PER_TICK = 8;
	const int Size = Send.m_pPayload->size();
	for(int i = 0; i < CHUNKS_PER_TICK; i++, Send.m_NextChunk++)
	{
		const int Offset = Send.m_NextChunk * MatchReportLimits::MAX_CHUNK_SIZE;
		const int ChunkSize = std::min(MatchReportLimits::MAX_CHUNK_SIZE, Size - Offset);
		CMsgPacker Chunk(NETMSG_MATCH_REPORT_CHUNK, false);
		Chunk.AddRaw(&Send.m_MatchId, sizeof(Send.m_MatchId));
		Chunk.AddInt(Send.m_NextChunk);
		Chunk.AddInt(ChunkSize);
		Chunk.AddRaw(Send.m_pPayload->data() + Offset, ChunkSize);
		if(pServer->SendMsg(&Chunk, MSGFLAG_VITAL | MSGFLAG_NORECORD, Send.m_ClientId) < 0)
		{
			log_error("game", "failed to send match report chunk to client %d", Send.m_ClientId);
			return true;
		}
		if(Offset + ChunkSize >= Size)
			return true;
	}
	return false;
}

void CMatchReportSender::Tick(IServer *pServer)
{
	for(auto It = m_vSends.begin(); It != m_vSends.end();)
	{
		if(It->m_Tick == pServer->Tick())
			++It;
		else if(!pServer->ClientIngame(It->m_ClientId) || SendChunks(pServer, *It))
			It = m_vSends.erase(It);
		else
			++It;
	}
}

bool CMatchReportSender::AcceptLiveRequest(int ClientId, int Tick, int TickSpeed)
{
	if(m_aLastLiveRequestTick[ClientId] >= 0 && Tick - m_aLastLiveRequestTick[ClientId] < TickSpeed * 2)
		return false;
	m_aLastLiveRequestTick[ClientId] = Tick;
	return true;
}
