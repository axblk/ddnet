/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "match_journal.h"
#include "match_report_assembler.h"

// The match journal for a program that plays nothing. A tool that turns one
// demo into one video file joins no match, so the journal and the SQLite it
// would be kept in stay out of its build. The journal never opens, and every
// caller already asks IsOpen first, so nothing here is ever reached with a
// match to store.

bool CMatchJournal::Open(IStorage *, std::string *pError)
{
	if(pError != nullptr)
	{
		*pError = "This program does not keep a match journal";
	}
	return false;
}

CMatchJournal::EInsertResult CMatchJournal::Insert(const CStoredMatch &, std::string *)
{
	return EInsertResult::ERROR;
}

CMatchJournal::EInsertResult CMatchJournal::InsertReplacingObserved(const CStoredMatch &, const char *, CUuid, std::string *)
{
	return EInsertResult::ERROR;
}

bool CMatchJournal::ListMatches(const CMatchHistoryFilter &, std::vector<CMatchHistoryEntry> &, std::string *) const
{
	return false;
}

bool CMatchJournal::LoadMatch(const char *, CUuid, CStoredMatch &, std::string *) const
{
	return false;
}

bool CMatchJournal::QueryProfile(const CMatchProfileFilter &, CMatchProfile &, std::string *) const
{
	return false;
}

bool CMatchJournal::DeleteMatch(const char *, CUuid, std::string *)
{
	return false;
}

bool CMatchJournal::DeleteAll(std::string *)
{
	return false;
}

bool CMatchJournal::Info(CMatchJournalInfo &, std::string *) const
{
	return false;
}

bool PersistLiveStatsSnapshotOnDisconnect(CMatchJournal &, ESessionSourceType, bool, bool, bool, const std::optional<CStoredMatch> &, const CLiveStatsAssembler &, std::string *)
{
	// Nothing to persist, which is what the real one reports when the journal
	// is closed.
	return true;
}

bool GenerateSampleMatches(CMatchJournal &, int, const char *, std::string *)
{
	return false;
}

#endif
