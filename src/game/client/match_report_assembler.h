#ifndef GAME_CLIENT_MATCH_REPORT_ASSEMBLER_H
#define GAME_CLIENT_MATCH_REPORT_ASSEMBLER_H

#include "match_journal.h"

#include <engine/client/session.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

bool ShouldPersistMatchReport(ESessionSourceType SourceType, bool SavingEnabled, bool HasLocalParticipant);
bool ShouldReplaceObservedMatch(const std::optional<CStoredMatch> &CurrentMatch, const CStoredMatch &ServerMatch);

class CMatchReportAssembler
{
	CUuid m_MatchId = UUID_ZEROED;
	int m_ReportSchemaVersion = 0;
	size_t m_TotalSize = 0;
	size_t m_ReceivedSize = 0;
	std::vector<std::string> m_vChunks;
	std::vector<bool> m_vReceived;
	std::optional<CUuid> m_LocalParticipantMatchId;
	std::optional<int> m_LocalParticipantId;

	void ResetAssembly();
	bool Fail(std::string *pError, const char *pMessage);

public:
	void Reset();
	bool IsReceiving() const { return m_MatchId != UUID_ZEROED; }
	bool Start(CUuid MatchId, int ReportSchemaVersion, int TotalSize, int NumChunks, std::string *pError);
	bool AddChunk(CUuid MatchId, int ChunkIndex, const void *pData, int DataSize, std::string *pError);
	bool SetLocalParticipant(CUuid MatchId, int ParticipantId, std::string *pError);
	bool Finish(CUuid MatchId, SHA256_DIGEST PayloadSha256, CStoredMatch &Match, std::string *pError);
};

class CLiveStatsAssembler
{
	CMatchReportAssembler m_Assembler;
	CUuid m_ReceivingMatchId = UUID_ZEROED;
	int m_ReceivingRevision = -1;
	bool m_ReceivingPersistOnDisconnect = false;
	int m_LatestRevision = -1;
	bool m_LatestPersistOnDisconnect = false;
	std::optional<CStoredMatch> m_Latest;

public:
	void Cancel();
	void Reset();
	bool Start(CUuid MatchId, int Revision, int ReportSchemaVersion, int LocalParticipantId, bool PersistOnDisconnect, int TotalSize, int NumChunks, std::string *pError);
	bool AddChunk(CUuid MatchId, int Revision, int ChunkIndex, const void *pData, int DataSize, std::string *pError);
	bool Finish(CUuid MatchId, int Revision, SHA256_DIGEST PayloadSha256, const char *pOriginId, std::string *pError);
	void ClearMatch(CUuid MatchId);
	const std::optional<CStoredMatch> &Latest() const { return m_Latest; }
	bool LatestPersistOnDisconnect() const { return m_LatestPersistOnDisconnect; }
};

bool PersistLiveStatsSnapshotOnDisconnect(CMatchJournal &Journal, ESessionSourceType SourceType, bool SavingEnabled, bool IsCurrentMatch, bool HasFinalServerReport, const std::optional<CStoredMatch> &ObservedMatch, const CLiveStatsAssembler &LiveStats, std::string *pError);

#endif // GAME_CLIENT_MATCH_REPORT_ASSEMBLER_H
