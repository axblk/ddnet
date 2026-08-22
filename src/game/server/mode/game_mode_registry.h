#ifndef GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
#define GAME_SERVER_MODE_GAME_MODE_REGISTRY_H

#include <game/gamecore.h>
#include <game/match_report.h>

#include <memory>
#include <string>
#include <vector>

class CGameServices;
class IGameController;

enum class EGameModeScoreKind
{
	POINTS,
	TIME,
};

enum class EGameModeMetricCategory
{
	OVERVIEW,
	COMBAT,
	OBJECTIVES,
};

enum class EGameModeMetricUnit
{
	COUNT,
	DAMAGE,
	SCORE,
	TICKS,
};

// Stable report schema metadata. Mode and metric IDs are namespaced as
// "mode@owner" and "mode@owner/metric"; bump the schema version for incompatible changes.
class CGameModeMetricInfo
{
public:
	std::string m_Id;
	std::string m_DisplayName;
	EGameModeMetricCategory m_Category;
	EGameModeMetricUnit m_Unit;
	// Defines how history views combine the metric across compatible reports.
	EMatchMetricAggregation m_Aggregation;
	int m_DisplayOrder;
};

class CGameModeReportInfo
{
public:
	// Schema versions are scoped to this namespaced mode ID.
	std::string m_ModeId;
	int m_SchemaVersion = 0;
	std::vector<CGameModeMetricInfo> m_vMetrics;
};

struct CGameModeInfo
{
	const char *m_pId;
	const char *m_pDisplayName;
	const char *m_pGameType;
	const char *m_pTestingGameType;
	EGameModeScoreKind m_ScoreKind;
	int m_GameFlags;
	int m_ActivePlayerLimit = 0;
	bool m_UseTuneZones = false;
	CPhysicsRules m_PhysicsRules = CPhysicsRules::Vanilla();
	CGameModeReportInfo m_Report;
	// Optional schema for modes such as Race that expose live status without producing match reports.
	CGameModeReportInfo m_LiveStats;
};

class CGameModeRegistry
{
public:
	using FCreateController = std::unique_ptr<IGameController> (*)(CGameServices &Services, const CGameModeInfo &Info);

	bool Register(const CGameModeInfo &Info, FCreateController pfnCreateController);
	// By id only, so that registering a mode is never refused because another
	// one advertises the same name.
	const CGameModeInfo *Find(const char *pId) const;
	std::unique_ptr<IGameController> Create(const char *pId, CGameServices &Services) const;

private:
	struct CEntry
	{
		CGameModeInfo m_Info;
		FCreateController m_pfnCreateController;
	};

	const CEntry *FindEntry(const char *pId) const;
	const CEntry *ResolveEntry(const char *pId) const;

	std::vector<CEntry> m_vEntries;
};

const char *GameModeScoreKindName(EGameModeScoreKind ScoreKind);
CGameModeReportInfo CompetitiveGameModeReport(const char *pModeId, bool HasObjectives);
CGameModeReportInfo RaceLiveStatsReport(const char *pModeId);

#endif // GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
