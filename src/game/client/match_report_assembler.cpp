#include "match_report_assembler.h"

#include <base/hash.h>

#include <algorithm>
#include <utility>

bool ShouldPersistMatchReport(ESessionSourceType SourceType, bool SavingEnabled, bool HasLocalParticipant)
{
	return SourceType == ESessionSourceType::NETWORK && SavingEnabled && HasLocalParticipant;
}

bool ShouldReplaceObservedMatch(const std::optional<CStoredMatch> &CurrentMatch, const CStoredMatch &ServerMatch)
{
	return CurrentMatch.has_value() && CurrentMatch->m_Source == EMatchReportSource::CLIENT_OBSERVED &&
	       CurrentMatch->m_OriginId == ServerMatch.m_OriginId && CurrentMatch->m_Report.m_MapSha256 == ServerMatch.m_Report.m_MapSha256 &&
	       CurrentMatch->m_Report.m_RoundStartTick == ServerMatch.m_Report.m_RoundStartTick;
}

void CMatchReportAssembler::ResetAssembly()
{
	m_MatchId = UUID_ZEROED;
	m_ReportSchemaVersion = 0;
	m_TotalSize = 0;
	m_ReceivedSize = 0;
	m_vChunks.clear();
	m_vReceived.clear();
}

bool CMatchReportAssembler::Fail(std::string *pError, const char *pMessage)
{
	ResetAssembly();
	if(pError != nullptr)
		*pError = pMessage;
	return false;
}

void CMatchReportAssembler::Reset()
{
	ResetAssembly();
	m_LocalParticipantMatchId.reset();
	m_LocalParticipantId.reset();
}

bool CMatchReportAssembler::Start(CUuid MatchId, int ReportSchemaVersion, int TotalSize, int NumChunks, std::string *pError)
{
	ResetAssembly();
	if(MatchId == UUID_ZEROED || ReportSchemaVersion != 1 || TotalSize <= 0 || TotalSize > MatchReportLimits::MAX_PAYLOAD_SIZE ||
		NumChunks <= 0 || NumChunks > MatchReportTransportLimits::MAX_CHUNKS || NumChunks > TotalSize || TotalSize > NumChunks * MatchReportTransportLimits::MAX_CHUNK_SIZE)
		return Fail(pError, "invalid match report start");
	if(m_LocalParticipantMatchId.has_value() && *m_LocalParticipantMatchId != MatchId)
	{
		m_LocalParticipantMatchId.reset();
		m_LocalParticipantId.reset();
	}
	m_MatchId = MatchId;
	m_ReportSchemaVersion = ReportSchemaVersion;
	m_TotalSize = TotalSize;
	m_vChunks.resize(NumChunks);
	m_vReceived.assign(NumChunks, false);
	return true;
}

bool CMatchReportAssembler::AddChunk(CUuid MatchId, int ChunkIndex, const void *pData, int DataSize, std::string *pError)
{
	if(!IsReceiving() || MatchId != m_MatchId || ChunkIndex < 0 || ChunkIndex >= static_cast<int>(m_vChunks.size()) || pData == nullptr || DataSize <= 0 || DataSize > MatchReportTransportLimits::MAX_CHUNK_SIZE)
		return Fail(pError, "invalid match report chunk");
	if(m_vReceived[ChunkIndex])
		return Fail(pError, "duplicate match report chunk");
	if(m_ReceivedSize + static_cast<size_t>(DataSize) > m_TotalSize)
		return Fail(pError, "match report chunks exceed declared size");
	m_vChunks[ChunkIndex].assign(static_cast<const char *>(pData), DataSize);
	m_vReceived[ChunkIndex] = true;
	m_ReceivedSize += DataSize;
	return true;
}

bool CMatchReportAssembler::SetLocalParticipant(CUuid MatchId, int ParticipantId, std::string *pError)
{
	if(MatchId == UUID_ZEROED || ParticipantId < 0)
	{
		if(pError != nullptr)
			*pError = "invalid local match participant";
		return false;
	}
	m_LocalParticipantMatchId = MatchId;
	m_LocalParticipantId = ParticipantId;
	return true;
}

bool CMatchReportAssembler::Finish(CUuid MatchId, SHA256_DIGEST PayloadSha256, CStoredMatch &Match, std::string *pError)
{
	if(!IsReceiving() || MatchId != m_MatchId)
		return Fail(pError, "match report end without matching start");
	if(m_ReceivedSize != m_TotalSize || std::find(m_vReceived.begin(), m_vReceived.end(), false) != m_vReceived.end())
		return Fail(pError, "incomplete match report");

	std::string Payload;
	Payload.reserve(m_TotalSize);
	for(const std::string &Chunk : m_vChunks)
		Payload.append(Chunk);
	if(Payload.size() != m_TotalSize || sha256(Payload.data(), Payload.size()) != PayloadSha256)
		return Fail(pError, "match report digest mismatch");

	CMatchReport Report;
	// The wire carries the packed form, the journal stores JSON, so what was
	// received is turned into the journal format right here.
	std::string RawReport;
	if(!MatchReportFromPacked(Payload.data(), Payload.size(), Report, pError) || !MatchReportToJson(Report, RawReport, pError))
	{
		ResetAssembly();
		return false;
	}
	if(Report.m_MatchId != MatchId || Report.m_ReportSchemaVersion != m_ReportSchemaVersion)
		return Fail(pError, "match report envelope mismatch");

	std::optional<int> LocalParticipantId;
	if(m_LocalParticipantMatchId.has_value() && *m_LocalParticipantMatchId == MatchId)
	{
		const auto Participant = std::find_if(Report.m_vParticipants.begin(), Report.m_vParticipants.end(), [this](const CMatchParticipant &Item) { return Item.m_ParticipantId == *m_LocalParticipantId; });
		if(Participant == Report.m_vParticipants.end())
			return Fail(pError, "local participant does not exist in match report");
		LocalParticipantId = m_LocalParticipantId;
	}

	Match = {};
	Match.m_Source = EMatchReportSource::SERVER_REPORT;
	Match.m_Completeness = EMatchCompleteness::COMPLETE;
	Match.m_LocalParticipantId = LocalParticipantId;
	Match.m_Report = std::move(Report);
	Match.m_RawReport = std::move(RawReport);
	Reset();
	return true;
}

void CLiveStatsAssembler::Cancel()
{
	m_Assembler.Reset();
	m_ReceivingMatchId = UUID_ZEROED;
	m_ReceivingRevision = -1;
	m_ReceivingPersistOnDisconnect = false;
}

void CLiveStatsAssembler::Reset()
{
	Cancel();
	m_LatestRevision = -1;
	m_LatestPersistOnDisconnect = false;
	m_Latest.reset();
}

bool CLiveStatsAssembler::Start(CUuid MatchId, int Revision, int ReportSchemaVersion, int LocalParticipantId, bool PersistOnDisconnect, int TotalSize, int NumChunks, std::string *pError)
{
	Cancel();
	if(Revision < 0 || LocalParticipantId < 0 || (m_Latest.has_value() && m_Latest->m_Report.m_MatchId == MatchId && Revision <= m_LatestRevision))
	{
		if(pError != nullptr)
			*pError = "stale live stats start";
		return false;
	}
	if(!m_Assembler.Start(MatchId, ReportSchemaVersion, TotalSize, NumChunks, pError) || !m_Assembler.SetLocalParticipant(MatchId, LocalParticipantId, pError))
		return false;
	m_ReceivingMatchId = MatchId;
	m_ReceivingRevision = Revision;
	m_ReceivingPersistOnDisconnect = PersistOnDisconnect;
	return true;
}

bool CLiveStatsAssembler::AddChunk(CUuid MatchId, int Revision, int ChunkIndex, const void *pData, int DataSize, std::string *pError)
{
	if(MatchId != m_ReceivingMatchId || Revision != m_ReceivingRevision)
	{
		Cancel();
		if(pError != nullptr)
			*pError = "live stats chunk envelope mismatch";
		return false;
	}
	return m_Assembler.AddChunk(MatchId, ChunkIndex, pData, DataSize, pError);
}

bool CLiveStatsAssembler::Finish(CUuid MatchId, int Revision, SHA256_DIGEST PayloadSha256, const char *pOriginId, std::string *pError)
{
	if(MatchId != m_ReceivingMatchId || Revision != m_ReceivingRevision || pOriginId == nullptr || pOriginId[0] == '\0')
	{
		Cancel();
		if(pError != nullptr)
			*pError = "live stats end envelope mismatch";
		return false;
	}
	CStoredMatch Match;
	if(!m_Assembler.Finish(MatchId, PayloadSha256, Match, pError))
	{
		Cancel();
		return false;
	}
	Match.m_OriginId = pOriginId;
	Match.m_Source = EMatchReportSource::SERVER_SNAPSHOT;
	Match.m_Completeness = EMatchCompleteness::ABORTED;
	m_Latest = std::move(Match);
	m_LatestRevision = Revision;
	m_LatestPersistOnDisconnect = m_ReceivingPersistOnDisconnect;
	m_ReceivingMatchId = UUID_ZEROED;
	m_ReceivingRevision = -1;
	m_ReceivingPersistOnDisconnect = false;
	return true;
}

void CLiveStatsAssembler::ClearMatch(CUuid MatchId)
{
	if(m_Latest.has_value() && m_Latest->m_Report.m_MatchId == MatchId)
	{
		m_Latest.reset();
		m_LatestRevision = -1;
		m_LatestPersistOnDisconnect = false;
	}
}
