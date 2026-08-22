#ifndef GAME_CLIENT_MATCH_REPORT_ASSEMBLER_H
#define GAME_CLIENT_MATCH_REPORT_ASSEMBLER_H

#include "match_journal.h"

#include <engine/shared/uuid_manager.h>

#include <string>

// Puts a report together from the chunks the server sends in order over the vital channel.
class CMatchReportAssembler
{
	CUuid m_MatchId = UUID_ZEROED;
	bool m_Live = false;
	bool m_PersistOnDisconnect = false;
	int m_LocalParticipantId = -1;
	size_t m_Size = 0;
	int m_NextChunk = 0;
	std::string m_Payload;

public:
	void Reset() { *this = {}; }
	bool Start(CUuid MatchId, bool Live, bool PersistOnDisconnect, int LocalParticipantId, int Size);
	bool AddChunk(CUuid MatchId, int ChunkIndex, const void *pData, int Size);
	bool IsComplete() const { return m_MatchId != UUID_ZEROED && m_Payload.size() == m_Size; }
	bool IsLive() const { return m_Live; }
	bool PersistOnDisconnect() const { return m_PersistOnDisconnect; }
	// unpacks the complete report and resets the assembler
	bool Finish(CStoredMatch &Match, std::string *pError);
};

#endif // GAME_CLIENT_MATCH_REPORT_ASSEMBLER_H
