#include "match_report_assembler.h"

bool CMatchReportAssembler::Start(CUuid MatchId, bool Live, bool PersistOnDisconnect, int LocalParticipantId, int Size)
{
	Reset();
	if(MatchId == UUID_ZEROED || LocalParticipantId < 0 || Size <= 0 || Size > MatchReportLimits::MAX_PAYLOAD_SIZE)
		return false;
	m_MatchId = MatchId;
	m_Live = Live;
	m_PersistOnDisconnect = PersistOnDisconnect;
	m_LocalParticipantId = LocalParticipantId;
	m_Size = Size;
	m_Payload.reserve(Size);
	return true;
}

bool CMatchReportAssembler::AddChunk(CUuid MatchId, int ChunkIndex, const void *pData, int Size)
{
	if(m_MatchId == UUID_ZEROED || MatchId != m_MatchId || ChunkIndex != m_NextChunk || Size <= 0 || m_Payload.size() + Size > m_Size)
	{
		Reset();
		return false;
	}
	m_Payload.append((const char *)pData, Size);
	m_NextChunk++;
	return true;
}

bool CMatchReportAssembler::Finish(CStoredMatch &Match, std::string *pError)
{
	CStoredMatch Received;
	const bool Success = IsComplete() && MatchReportFromPacked(m_Payload.data(), m_Payload.size(), Received.m_Report, pError);
	if(Success && (Received.m_Report.m_MatchId != m_MatchId || !Received.m_Report.Participant(m_LocalParticipantId)))
	{
		if(pError)
			*pError = "match report does not fit its announcement";
		Reset();
		return false;
	}
	Received.m_Source = m_Live ? EMatchReportSource::SERVER_SNAPSHOT : EMatchReportSource::SERVER_REPORT;
	// a round that was restarted or left with the map is no complete match either
	Received.m_Completeness = !m_Live && Received.m_Report.m_Termination == EMatchTermination::COMPLETED ? EMatchCompleteness::COMPLETE : EMatchCompleteness::ABORTED;
	Received.m_LocalParticipantId = m_LocalParticipantId;
	Reset();
	if(Success)
		Match = std::move(Received);
	return Success;
}
