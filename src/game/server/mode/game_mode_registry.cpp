#include "game_mode_registry.h"

#include <base/str.h>

#include <game/server/gamecontroller.h>

bool CGameModeRegistry::Register(const CGameModeInfo &Info, FCreateController pfnCreateController)
{
	if(!Info.m_pId || !Info.m_pId[0] || !Info.m_pDisplayName || !Info.m_pGameType || !Info.m_pTestingGameType || !pfnCreateController || Find(Info.m_pId))
		return false;
	const auto ValidReportInfo = [](const CGameModeReportInfo &Report) {
		if(Report.m_ModeId.empty())
			return Report.m_SchemaVersion == 0 && Report.m_vMetrics.empty();
		if(Report.m_SchemaVersion <= 0 || !IsValidMatchReportModeId(Report.m_ModeId))
			return false;
		for(size_t i = 0; i < Report.m_vMetrics.size(); ++i)
		{
			const CGameModeMetricInfo &Metric = Report.m_vMetrics[i];
			if(!IsValidMatchReportMetricId(Metric.m_Id, Report.m_ModeId) || Metric.m_DisplayName.empty() || Metric.m_DisplayName.size() > MatchReportLimits::MAX_DISPLAY_NAME_LENGTH || str_comp(MatchMetricAggregationName(Metric.m_Aggregation), "invalid") == 0)
				return false;
			for(size_t j = 0; j < i; ++j)
				if(Report.m_vMetrics[j].m_Id == Metric.m_Id)
					return false;
		}
		return true;
	};
	if(!ValidReportInfo(Info.m_Report) || !ValidReportInfo(Info.m_LiveStats))
		return false;
	if(!Info.m_Report.m_ModeId.empty() && !Info.m_LiveStats.m_ModeId.empty() && Info.m_Report.m_ModeId == Info.m_LiveStats.m_ModeId && Info.m_Report.m_SchemaVersion != Info.m_LiveStats.m_SchemaVersion)
		return false;

	// A client works out the physics it predicts from a single bit in the game
	// info and cannot be told anything finer, so a mode running rules that are
	// neither plain vanilla nor plain DDNet would be mispredicted by every
	// client with nothing to show for it. Refusing it here says so at startup
	// instead of leaving players to find out by rubber-banding.
	if(!(Info.m_PhysicsRules == (Info.m_PhysicsRules.m_DDNetMovement ? CPhysicsRules::DDNet() : CPhysicsRules::Vanilla())))
		return false;

	m_vEntries.push_back({Info, pfnCreateController});
	return true;
}

CGameModeReportInfo RaceLiveStatsReport(const char *pModeId)
{
	CGameModeReportInfo Report;
	Report.m_ModeId = pModeId;
	Report.m_SchemaVersion = 1;
	auto Add = [&](const char *pSuffix, const char *pDisplayName, EGameModeMetricUnit Unit, int DisplayOrder) {
		Report.m_vMetrics.push_back({Report.m_ModeId + "/" + pSuffix, pDisplayName, EGameModeMetricCategory::OVERVIEW, Unit, EMatchMetricAggregation::MATCH_ONLY, DisplayOrder});
	};
	Add("personal_best_ticks", "Personal best", EGameModeMetricUnit::TICKS, 0);
	Add("map_best_ticks", "Map best", EGameModeMetricUnit::TICKS, 1);
	Add("map_rank", "Map rank", EGameModeMetricUnit::COUNT, 2);
	Add("map_finishes", "Stored map finishes", EGameModeMetricUnit::COUNT, 3);
	Add("session_finishes", "Session finishes", EGameModeMetricUnit::COUNT, 4);
	Add("last_finish_ticks", "Last finish", EGameModeMetricUnit::TICKS, 5);
	Add("current_run_ticks", "Current run", EGameModeMetricUnit::TICKS, 6);
	Add("current_checkpoint", "Current checkpoint", EGameModeMetricUnit::COUNT, 7);
	return Report;
}

CGameModeReportInfo CompetitiveGameModeReport(const char *pModeId, bool HasObjectives)
{
	CGameModeReportInfo Report;
	Report.m_ModeId = pModeId;
	Report.m_SchemaVersion = 1;
	auto Add = [&](const char *pSuffix, const char *pDisplayName, EGameModeMetricCategory Category, EGameModeMetricUnit Unit, EMatchMetricAggregation Aggregation) {
		Report.m_vMetrics.push_back({Report.m_ModeId + "/" + pSuffix, pDisplayName, Category, Unit, Aggregation, static_cast<int>(Report.m_vMetrics.size())});
	};
	Add("score", "Score", EGameModeMetricCategory::OVERVIEW, EGameModeMetricUnit::SCORE, EMatchMetricAggregation::SUM);
	Add("playtime_ticks", "Play time", EGameModeMetricCategory::OVERVIEW, EGameModeMetricUnit::TICKS, EMatchMetricAggregation::SUM);
	Add("sudden_death", "Sudden death", EGameModeMetricCategory::OVERVIEW, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::MATCH_ONLY);
	Add("kills", "Kills", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
	Add("deaths", "Deaths", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
	Add("suicides", "Suicides", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
	Add("shots", "Shots", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
	Add("hits", "Hits", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
	Add("damage_done", "Damage done", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::DAMAGE, EMatchMetricAggregation::SUM);
	Add("damage_taken", "Damage taken", EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::DAMAGE, EMatchMetricAggregation::SUM);
	const char *apWeaponNames[NUM_WEAPONS] = {"Hammer", "Gun", "Shotgun", "Grenade", "Laser", "Ninja"};
	for(int Weapon = 0; Weapon < NUM_WEAPONS; ++Weapon)
	{
		const std::string Prefix = "weapon_" + std::to_string(Weapon) + "_";
		const std::string DisplayPrefix = std::string(apWeaponNames[Weapon]) + " ";
		Add((Prefix + "kills").c_str(), (DisplayPrefix + "kills").c_str(), EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
		Add((Prefix + "deaths").c_str(), (DisplayPrefix + "deaths").c_str(), EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
		Add((Prefix + "shots").c_str(), (DisplayPrefix + "shots").c_str(), EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
		Add((Prefix + "hits").c_str(), (DisplayPrefix + "hits").c_str(), EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
		Add((Prefix + "damage_done").c_str(), (DisplayPrefix + "damage done").c_str(), EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::DAMAGE, EMatchMetricAggregation::SUM);
		Add((Prefix + "damage_taken").c_str(), (DisplayPrefix + "damage taken").c_str(), EGameModeMetricCategory::COMBAT, EGameModeMetricUnit::DAMAGE, EMatchMetricAggregation::SUM);
	}
	if(HasObjectives)
	{
		Add("flag_grabs", "Flag grabs", EGameModeMetricCategory::OBJECTIVES, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
		Add("flag_returns", "Flag returns", EGameModeMetricCategory::OBJECTIVES, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
		Add("flag_captures", "Flag captures", EGameModeMetricCategory::OBJECTIVES, EGameModeMetricUnit::COUNT, EMatchMetricAggregation::SUM);
	}
	return Report;
}

const CGameModeRegistry::CEntry *CGameModeRegistry::FindEntry(const char *pId) const
{
	if(!pId)
		return nullptr;

	for(const CEntry &Entry : m_vEntries)
	{
		if(str_comp(Entry.m_Info.m_pId, pId) == 0)
			return &Entry;
	}
	return nullptr;
}

const CGameModeRegistry::CEntry *CGameModeRegistry::ResolveEntry(const char *pId) const
{
	if(const CEntry *pExact = FindEntry(pId))
		return pExact;
	if(!pId)
		return nullptr;

	// `sv_gametype ctf` is how a server has been configured since long before
	// the modes had ids of their own, so the name a mode advertises is an alias
	// for it. A mod may register another mode under a name that is already
	// taken; the alias then means the one that was registered first, and the
	// mod's own id still selects it unambiguously.
	for(const CEntry &Entry : m_vEntries)
	{
		if(str_comp_nocase(Entry.m_Info.m_pGameType, pId) == 0)
			return &Entry;
	}
	return nullptr;
}

const CGameModeInfo *CGameModeRegistry::Find(const char *pId) const
{
	const CEntry *pEntry = FindEntry(pId);
	return pEntry == nullptr ? nullptr : &pEntry->m_Info;
}

std::unique_ptr<IGameController> CGameModeRegistry::Create(const char *pId, CGameServices &Services) const
{
	const CEntry *pEntry = ResolveEntry(pId);
	return pEntry == nullptr ? nullptr : pEntry->m_pfnCreateController(Services, pEntry->m_Info);
}

const char *GameModeScoreKindName(EGameModeScoreKind ScoreKind)
{
	switch(ScoreKind)
	{
	case EGameModeScoreKind::POINTS:
		return "points";
	case EGameModeScoreKind::TIME:
		return "time";
	}
	return "points";
}
