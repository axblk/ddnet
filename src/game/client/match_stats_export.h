#ifndef GAME_CLIENT_MATCH_STATS_EXPORT_H
#define GAME_CLIENT_MATCH_STATS_EXPORT_H

#include "match_journal.h"

#include <base/types.h>

class CJsonWriter;

// the stored match with its report as one JSON object
void MatchStatsExportJson(CJsonWriter &Writer, const CStoredMatch &Stored);
// one row per metric
void MatchStatsExportCsv(IOHANDLE File, const CStoredMatch &Stored);

#endif // GAME_CLIENT_MATCH_STATS_EXPORT_H
