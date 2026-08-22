#include "match_detail_view.h"

#include <base/str.h>

#include <algorithm>
#include <cinttypes>
#include <string_view>

namespace
{

	bool IsMatchObjectiveSuffix(std::string_view Suffix)
	{
		// Whatever a mode does with flags belongs on the objectives page, so the
		// whole `flag_` family counts and not only the three the game itself knows.
		return Suffix.starts_with("flag_") || Suffix == "catches" || Suffix == "health_picked_up" || Suffix == "armor_picked_up";
	}

	bool IsMatchRunSuffix(std::string_view Suffix)
	{
		return Suffix == "personal_best_ticks" || Suffix == "map_best_ticks" || Suffix == "map_rank" || Suffix == "map_finishes" || Suffix == "session_finishes" || Suffix == "last_finish_ticks" || Suffix == "current_run_ticks" || Suffix == "current_checkpoint";
	}

	void AddWeaponStats(CMatchWeaponStats &Target, const CMatchWeaponStats &Source)
	{
		AddMatchCombatValue(Target.m_Kills, Source.m_Kills);
		AddMatchCombatValue(Target.m_Deaths, Source.m_Deaths);
		AddMatchCombatValue(Target.m_DeathsHolding, Source.m_DeathsHolding);
		AddMatchCombatValue(Target.m_Shots, Source.m_Shots);
		AddMatchCombatValue(Target.m_Hits, Source.m_Hits);
		AddMatchCombatValue(Target.m_DamageDone, Source.m_DamageDone);
		AddMatchCombatValue(Target.m_DamageTaken, Source.m_DamageTaken);
	}

	int64_t StatOf(const CMatchWeaponStats &Stats, EMatchDetailValue Value)
	{
		switch(Value)
		{
		case EMatchDetailValue::KILLS: return Stats.m_Kills;
		case EMatchDetailValue::DEATHS: return Stats.m_Deaths;
		case EMatchDetailValue::DEATHS_HOLDING: return Stats.m_DeathsHolding;
		case EMatchDetailValue::SHOTS: return Stats.m_Shots;
		case EMatchDetailValue::HITS: return Stats.m_Hits;
		case EMatchDetailValue::DAMAGE_DONE: return Stats.m_DamageDone;
		case EMatchDetailValue::DAMAGE_TAKEN: return Stats.m_DamageTaken;
		case EMatchDetailValue::METRIC:
		case EMatchDetailValue::ACCURACY: break;
		}
		return 0;
	}

	/**
	 * @return The stat of one weapon, or of all of them together for weapon -1.
	 *
	 * A mode may report the same number twice, once for the match and once per
	 * weapon, or only one of the two. The total the mode gave wins, because it also
	 * counts what happened with a weapon the mode does not break down.
	 */
	int64_t WeaponStat(const CMatchCombatStats &Combat, int Weapon, EMatchDetailValue Value)
	{
		if(Weapon >= 0)
			return Weapon < static_cast<int>(Combat.m_vWeapons.size()) ? StatOf(Combat.m_vWeapons[Weapon], Value) : 0;
		if(StatOf(Combat.m_Total, Value) != 0)
			return StatOf(Combat.m_Total, Value);
		int64_t Sum = 0;
		for(const CMatchWeaponStats &Stats : Combat.m_vWeapons)
			AddMatchCombatValue(Sum, StatOf(Stats, Value));
		return Sum;
	}

	/**
	 * Copies the metrics the pages ask for by name out of the indexed ones, so that
	 * both a participant and a team summary get them the same way.
	 */
	void CacheNamedMetrics(CMatchDetailRow &Row, const std::vector<std::string> &vMetricIds)
	{
		for(size_t Metric = 0; Metric < vMetricIds.size() && Metric < Row.m_vMetrics.size(); ++Metric)
		{
			const std::string_view Suffix = MatchMetricSuffix(vMetricIds[Metric]);
			const int64_t Value = Row.m_vMetrics[Metric].value_or(0);
			if(Suffix == "score")
				Row.m_Score = Value;
			else if(Suffix == "suicides")
				Row.m_Suicides = Value;
			else if(Suffix == "assists")
				Row.m_Assists = Value;
			else if(Suffix == "playtime_ticks")
				Row.m_PlaytimeTicks = Value;
			else if(Suffix == "flag_grabs")
				Row.m_FlagGrabs = Value;
			else if(Suffix == "flag_returns")
				Row.m_FlagReturns = Value;
			else if(Suffix == "flag_captures")
				Row.m_FlagCaptures = Value;
			else if(Suffix == "catches")
				Row.m_Catches = Value;
			else if(Suffix == "health_picked_up")
				Row.m_HealthPickedUp = Value;
			else if(Suffix == "armor_picked_up")
				Row.m_ArmorPickedUp = Value;
		}
	}

} // namespace

const char *StatsMatchTabDisplayName(EStatsMatchTab Tab)
{
	switch(Tab)
	{
	case EStatsMatchTab::SCORE: return Localize("Score");
	case EStatsMatchTab::FRAGS: return Localize("Frags");
	case EStatsMatchTab::DEATHS: return Localize("Deaths");
	case EStatsMatchTab::ACCURACY: return Localize("Accuracy");
	case EStatsMatchTab::DAMAGE: return Localize("Damage");
	case EStatsMatchTab::SHOTS: return Localize("Shots");
	case EStatsMatchTab::OBJECTIVES: return Localize("Objectives");
	case EStatsMatchTab::RUN: return Localize("Run");
	case EStatsMatchTab::COUNT: break;
	}
	return "";
}

std::optional<int64_t> MatchDetailCell(const CMatchDetailRow &Row, const CMatchDetailColumn &Column)
{
	if(Column.m_Value == EMatchDetailValue::METRIC)
	{
		if(Column.m_Metric < 0 || Column.m_Metric >= static_cast<int>(Row.m_vMetrics.size()))
			return std::nullopt;
		return Row.m_vMetrics[Column.m_Metric];
	}
	if(Column.m_Value == EMatchDetailValue::ACCURACY)
	{
		const int64_t Shots = WeaponStat(Row.m_Combat, Column.m_Weapon, EMatchDetailValue::SHOTS);
		if(Shots <= 0)
			return std::nullopt;
		// Per mille, so that the order of two cells matches the one decimal the
		// table prints for them.
		return WeaponStat(Row.m_Combat, Column.m_Weapon, EMatchDetailValue::HITS) * 1000 / Shots;
	}
	return WeaponStat(Row.m_Combat, Column.m_Weapon, Column.m_Value);
}

void FormatMatchDetailCell(const CMatchDetailRow &Row, const CMatchDetailColumn &Column, int TickRate, char *pBuffer, int BufferSize)
{
	if(Column.m_Format == EMatchDetailFormat::ACCURACY)
	{
		FormatMatchAccuracy(WeaponStat(Row.m_Combat, Column.m_Weapon, EMatchDetailValue::HITS), WeaponStat(Row.m_Combat, Column.m_Weapon, EMatchDetailValue::SHOTS), pBuffer, BufferSize);
		return;
	}
	const std::optional<int64_t> Value = MatchDetailCell(Row, Column);
	if(!Value.has_value())
	{
		str_copy(pBuffer, "-", BufferSize);
		return;
	}
	switch(Column.m_Format)
	{
	case EMatchDetailFormat::DURATION:
		FormatMatchDuration(*Value, TickRate, pBuffer, BufferSize);
		return;
	case EMatchDetailFormat::RANK:
		if(*Value > 0)
			str_format(pBuffer, BufferSize, "#%" PRId64, *Value);
		else
			str_copy(pBuffer, "-", BufferSize);
		return;
	case EMatchDetailFormat::COUNT:
	case EMatchDetailFormat::ACCURACY:
		break;
	}
	str_format(pBuffer, BufferSize, "%" PRId64, *Value);
}

void BuildMatchDetailView(const CStoredMatch &Stored, CMatchDetailView &View)
{
	const CMatchReport &Report = Stored.m_Report;
	View = CMatchDetailView();
	View.m_TickRate = Report.m_TickRate;
	View.m_ModeSchemaVersion = Report.m_ModeSchemaVersion;
	View.m_HasTeams = !Report.m_vTeams.empty();

	// One pass over the metrics decides both which pages can be filled and which
	// columns exist at all, so that the drawing code never looks at them again.
	bool HasWeaponKills = false;
	bool HasWeaponDeaths = false;
	bool HasWeaponShots = false;
	bool HasWeaponDamage = false;
	bool HasObjectives = false;
	bool HasRun = false;
	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		const std::string_view Suffix = MatchMetricSuffix(Metric.m_MetricId);
		int Weapon = -1;
		const std::string_view WeaponStatName = MatchWeaponMetricStat(Suffix, &Weapon);
		HasWeaponKills = HasWeaponKills || WeaponStatName == "kills";
		HasWeaponDeaths = HasWeaponDeaths || WeaponStatName == "deaths" || WeaponStatName == "deaths_holding";
		HasWeaponShots = HasWeaponShots || WeaponStatName == "shots";
		HasWeaponDamage = HasWeaponDamage || WeaponStatName == "damage_done" || WeaponStatName == "damage_taken";
		HasObjectives = HasObjectives || IsMatchObjectiveSuffix(Suffix);
		HasRun = HasRun || IsMatchRunSuffix(Suffix);
		// Anything the combat statistics do not swallow needs a column of its
		// own, including a metric of a mode this build has never heard of.
		if(Metric.m_SubjectKind == EMatchSubjectKind::PARTICIPANT && !IsMatchCombatStatMetric(Metric.m_MetricId, Report.m_ModeSchemaVersion))
			View.m_vMetricIds.push_back(Metric.m_MetricId);
	}
	std::sort(View.m_vMetricIds.begin(), View.m_vMetricIds.end());
	View.m_vMetricIds.erase(std::unique(View.m_vMetricIds.begin(), View.m_vMetricIds.end()), View.m_vMetricIds.end());

	const auto Offer = [&View](EStatsMatchTab Tab, bool Available) {
		if(Available)
			View.m_TabMask |= 1u << static_cast<int>(Tab);
	};
	Offer(EStatsMatchTab::SCORE, true);
	// A race has no opponents to shoot at, so a mode that measures runs gets the
	// run page instead of five weapon pages that would all stay empty.
	Offer(EStatsMatchTab::FRAGS, HasWeaponKills && !HasRun);
	Offer(EStatsMatchTab::DEATHS, HasWeaponDeaths && !HasRun);
	Offer(EStatsMatchTab::ACCURACY, HasWeaponShots && !HasRun);
	Offer(EStatsMatchTab::DAMAGE, HasWeaponDamage && !HasRun);
	Offer(EStatsMatchTab::SHOTS, HasWeaponShots && !HasRun);
	Offer(EStatsMatchTab::OBJECTIVES, HasObjectives);
	Offer(EStatsMatchTab::RUN, HasRun);

	// Participant ids are sparse, a client that never played leaves a hole, so
	// the metrics are routed to their row through a lookup instead of a search.
	std::vector<int> vRowOfParticipant;
	View.m_vRows.reserve(Report.m_vParticipants.size());
	for(const CMatchParticipant &Participant : Report.m_vParticipants)
	{
		CMatchDetailRow Row;
		Row.m_pParticipant = &Participant;
		Row.m_vMetrics.resize(View.m_vMetricIds.size());
		Row.m_Local = Stored.m_LocalParticipantId.has_value() && *Stored.m_LocalParticipantId == Participant.m_ParticipantId;
		Row.m_Left = Participant.m_LeftTick.has_value();
		if(const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::PARTICIPANT, Participant.m_ParticipantId))
		{
			Row.m_Rank = pStanding->m_Rank;
			Row.m_Outcome = pStanding->m_Outcome;
		}
		if(Participant.m_ParticipantId >= 0)
		{
			vRowOfParticipant.resize(std::max<size_t>(vRowOfParticipant.size(), Participant.m_ParticipantId + 1), -1);
			vRowOfParticipant[Participant.m_ParticipantId] = static_cast<int>(View.m_vRows.size());
		}
		View.m_vRows.push_back(std::move(Row));
	}

	for(const CMatchMetric &Metric : Report.m_vMetrics)
	{
		if(Metric.m_SubjectKind != EMatchSubjectKind::PARTICIPANT || !Metric.m_SubjectId.has_value())
			continue;
		if(*Metric.m_SubjectId < 0 || *Metric.m_SubjectId >= static_cast<int>(vRowOfParticipant.size()) || vRowOfParticipant[*Metric.m_SubjectId] < 0)
			continue;
		CMatchDetailRow &Row = View.m_vRows[vRowOfParticipant[*Metric.m_SubjectId]];
		if(AddMatchCombatMetric(Row.m_Combat, Metric.m_MetricId, Report.m_ModeSchemaVersion, Metric.m_Value))
			continue;
		const auto Found = std::lower_bound(View.m_vMetricIds.begin(), View.m_vMetricIds.end(), Metric.m_MetricId);
		if(Found == View.m_vMetricIds.end() || *Found != Metric.m_MetricId)
			continue;
		Row.m_vMetrics[Found - View.m_vMetricIds.begin()] = Metric.m_Value;
	}
	for(CMatchDetailRow &Row : View.m_vRows)
		CacheNamedMetrics(Row, View.m_vMetricIds);

	for(int Weapon = 0; Weapon < MAX_MATCH_WEAPONS; ++Weapon)
	{
		const auto Used = [Weapon](const CMatchDetailRow &Row) {
			return Weapon < static_cast<int>(Row.m_Combat.m_vWeapons.size()) && Row.m_Combat.m_vWeapons[Weapon].HasData();
		};
		if(std::any_of(View.m_vRows.begin(), View.m_vRows.end(), Used))
			View.m_vWeapons.push_back(Weapon);
	}

	// Without teams every participant still needs a block to live in, otherwise
	// the page would need two ways of walking the same table.
	if(View.m_HasTeams)
	{
		for(const CMatchTeam &Team : Report.m_vTeams)
		{
			CMatchDetailBlock Block;
			Block.m_TeamId = Team.m_TeamId;
			if(Team.m_DisplayName.empty())
			{
				char aName[64];
				str_format(aName, sizeof(aName), Localize("Team %d"), Team.m_TeamId);
				Block.m_DisplayName = aName;
			}
			else
				Block.m_DisplayName = Team.m_DisplayName;
			if(const CMatchStanding *pStanding = ReportStanding(Report, EMatchSubjectKind::TEAM, Team.m_TeamId))
			{
				Block.m_Outcome = pStanding->m_Outcome;
				Block.m_Summary.m_Rank = pStanding->m_Rank;
				Block.m_Summary.m_Outcome = pStanding->m_Outcome;
			}
			View.m_vBlocks.push_back(std::move(Block));
		}
	}
	else
	{
		CMatchDetailBlock Block;
		Block.m_DisplayName = Localize("Participants");
		View.m_vBlocks.push_back(std::move(Block));
	}

	std::vector<int> vBlockOfRow(View.m_vRows.size(), -1);
	bool HasUnassigned = false;
	for(size_t Index = 0; Index < View.m_vRows.size(); ++Index)
	{
		if(!View.m_HasTeams)
		{
			vBlockOfRow[Index] = 0;
			continue;
		}
		for(size_t Block = 0; Block < View.m_vBlocks.size(); ++Block)
			if(View.m_vBlocks[Block].m_TeamId == View.m_vRows[Index].m_pParticipant->m_TeamId)
				vBlockOfRow[Index] = static_cast<int>(Block);
		HasUnassigned = HasUnassigned || vBlockOfRow[Index] < 0;
	}
	// A participant the mode put in no team of the report would otherwise
	// silently vanish from the table.
	if(HasUnassigned)
	{
		CMatchDetailBlock Block;
		Block.m_DisplayName = Localize("Without a team");
		View.m_vBlocks.push_back(std::move(Block));
		for(int &Assigned : vBlockOfRow)
			if(Assigned < 0)
				Assigned = static_cast<int>(View.m_vBlocks.size()) - 1;
	}

	for(CMatchDetailBlock &Block : View.m_vBlocks)
		Block.m_Summary.m_vMetrics.resize(View.m_vMetricIds.size());
	for(size_t Index = 0; Index < View.m_vRows.size(); ++Index)
	{
		const CMatchDetailRow &Row = View.m_vRows[Index];
		CMatchDetailBlock &Block = View.m_vBlocks[vBlockOfRow[Index]];
		(Row.m_Left ? Block.m_vLeft : Block.m_vPlaying).push_back(static_cast<int>(Index));
		Block.m_Summary.m_Local = Block.m_Summary.m_Local || Row.m_Local;
		AddWeaponStats(Block.m_Summary.m_Combat.m_Total, Row.m_Combat.m_Total);
		for(const CMatchWeaponStats &Weapon : Row.m_Combat.m_vWeapons)
			if(CMatchWeaponStats *pTarget = Block.m_Summary.m_Combat.Weapon(Weapon.m_Weapon))
				AddWeaponStats(*pTarget, Weapon);
		for(size_t Metric = 0; Metric < View.m_vMetricIds.size(); ++Metric)
		{
			if(!Row.m_vMetrics[Metric].has_value())
				continue;
			int64_t Value = Block.m_Summary.m_vMetrics[Metric].value_or(0);
			AddMatchCombatValue(Value, *Row.m_vMetrics[Metric]);
			Block.m_Summary.m_vMetrics[Metric] = Value;
		}
	}

	// What the mode says about a team beats the sum over its players: a capture
	// counts for the team once, not once per player who touched the flag.
	for(CMatchDetailBlock &Block : View.m_vBlocks)
	{
		for(size_t Metric = 0; Block.m_TeamId.has_value() && Metric < View.m_vMetricIds.size(); ++Metric)
		{
			const std::string Suffix(MatchMetricSuffix(View.m_vMetricIds[Metric]));
			if(const std::optional<int64_t> Value = ReportMetric(Report, EMatchSubjectKind::TEAM, *Block.m_TeamId, Suffix.c_str()))
				Block.m_Summary.m_vMetrics[Metric] = *Value;
		}
		CacheNamedMetrics(Block.m_Summary, View.m_vMetricIds);
	}

	const auto ByRank = [&View](int Left, int Right) {
		const CMatchDetailRow &RowLeft = View.m_vRows[Left];
		const CMatchDetailRow &RowRight = View.m_vRows[Right];
		// A participant the mode gave no standing goes last instead of first.
		const int RankLeft = RowLeft.m_Rank > 0 ? RowLeft.m_Rank : MatchReportLimits::MAX_STANDINGS + 1;
		const int RankRight = RowRight.m_Rank > 0 ? RowRight.m_Rank : MatchReportLimits::MAX_STANDINGS + 1;
		if(RankLeft != RankRight)
			return RankLeft < RankRight;
		return RowLeft.m_pParticipant->m_ParticipantId < RowRight.m_pParticipant->m_ParticipantId;
	};
	for(CMatchDetailBlock &Block : View.m_vBlocks)
	{
		std::sort(Block.m_vPlaying.begin(), Block.m_vPlaying.end(), ByRank);
		std::sort(Block.m_vLeft.begin(), Block.m_vLeft.end(), ByRank);
	}

	for(int Tab = 0; Tab < static_cast<int>(EStatsMatchTab::COUNT); ++Tab)
	{
		if(View.HasTab(static_cast<EStatsMatchTab>(Tab)))
		{
			UpdateMatchDetailBest(View, static_cast<EStatsMatchTab>(Tab));
			break;
		}
	}
}

void UpdateMatchDetailBest(CMatchDetailView &View, EStatsMatchTab Tab)
{
	View.m_Tab = Tab;
	View.m_vColumns.clear();
	if(!View.HasTab(Tab))
		return;

	const auto AddWeaponColumns = [&View](EMatchDetailValue Value, EMatchDetailFormat Format) {
		for(const int Weapon : View.m_vWeapons)
		{
			CMatchDetailColumn Column;
			char aWeaponName[64];
			MatchWeaponDisplayName(Weapon, aWeaponName, sizeof(aWeaponName));
			Column.m_Label = aWeaponName;
			Column.m_Value = Value;
			Column.m_Format = Format;
			Column.m_Weapon = Weapon;
			View.m_vColumns.push_back(std::move(Column));
		}
	};
	const auto AddTotalColumn = [&View](const char *pLabel, EMatchDetailValue Value, EMatchDetailFormat Format) {
		CMatchDetailColumn Column;
		Column.m_Label = pLabel;
		Column.m_Value = Value;
		Column.m_Format = Format;
		View.m_vColumns.push_back(std::move(Column));
	};
	const auto AddMetricColumn = [&View](size_t Metric) {
		const std::string_view Suffix = MatchMetricSuffix(View.m_vMetricIds[Metric]);
		CMatchDetailColumn Column;
		char aLabel[64];
		MatchMetricDisplayName(View.m_vMetricIds[Metric], View.m_ModeSchemaVersion, aLabel, sizeof(aLabel));
		Column.m_Label = aLabel;
		Column.m_Metric = static_cast<int>(Metric);
		if(Suffix == "map_rank")
			Column.m_Format = EMatchDetailFormat::RANK;
		else if(Suffix.ends_with("_ticks"))
			Column.m_Format = EMatchDetailFormat::DURATION;
		// A rank is better the lower it is, and so is a run, but a play time is
		// not a competition.
		Column.m_LowerIsBetter = Column.m_Format == EMatchDetailFormat::RANK || (Column.m_Format == EMatchDetailFormat::DURATION && IsMatchRunSuffix(Suffix));
		View.m_vColumns.push_back(std::move(Column));
	};
	const auto AnyRowHas = [&View](EMatchDetailValue Value) {
		CMatchDetailColumn Probe;
		Probe.m_Value = Value;
		return std::any_of(View.m_vRows.begin(), View.m_vRows.end(), [&Probe](const CMatchDetailRow &Row) { return MatchDetailCell(Row, Probe).value_or(0) != 0; });
	};

	switch(Tab)
	{
	case EStatsMatchTab::SCORE:
	{
		// The score page also collects everything the other pages do not claim,
		// so a metric this build has never seen still ends up on screen.
		for(size_t Metric = 0; Metric < View.m_vMetricIds.size(); ++Metric)
			if(MatchMetricSuffix(View.m_vMetricIds[Metric]) == "score")
				AddMetricColumn(Metric);
		if(AnyRowHas(EMatchDetailValue::KILLS))
			AddTotalColumn(Localize("Kills"), EMatchDetailValue::KILLS, EMatchDetailFormat::COUNT);
		if(AnyRowHas(EMatchDetailValue::DEATHS))
			AddTotalColumn(Localize("Deaths"), EMatchDetailValue::DEATHS, EMatchDetailFormat::COUNT);
		for(size_t Metric = 0; Metric < View.m_vMetricIds.size(); ++Metric)
		{
			const std::string_view Suffix = MatchMetricSuffix(View.m_vMetricIds[Metric]);
			if(Suffix != "score" && !IsMatchObjectiveSuffix(Suffix) && !IsMatchRunSuffix(Suffix))
				AddMetricColumn(Metric);
		}
		break;
	}
	case EStatsMatchTab::FRAGS:
		AddWeaponColumns(EMatchDetailValue::KILLS, EMatchDetailFormat::COUNT);
		AddTotalColumn(Localize("Total"), EMatchDetailValue::KILLS, EMatchDetailFormat::COUNT);
		break;
	case EStatsMatchTab::DEATHS:
		AddWeaponColumns(EMatchDetailValue::DEATHS, EMatchDetailFormat::COUNT);
		AddTotalColumn(Localize("Total"), EMatchDetailValue::DEATHS, EMatchDetailFormat::COUNT);
		// Dying while holding a weapon is a different number from being killed
		// by one, and only some modes report it.
		if(AnyRowHas(EMatchDetailValue::DEATHS_HOLDING))
			AddTotalColumn(Localize("Died holding"), EMatchDetailValue::DEATHS_HOLDING, EMatchDetailFormat::COUNT);
		break;
	case EStatsMatchTab::ACCURACY:
		AddWeaponColumns(EMatchDetailValue::ACCURACY, EMatchDetailFormat::ACCURACY);
		AddTotalColumn(Localize("Total"), EMatchDetailValue::ACCURACY, EMatchDetailFormat::ACCURACY);
		break;
	case EStatsMatchTab::DAMAGE:
		AddWeaponColumns(EMatchDetailValue::DAMAGE_DONE, EMatchDetailFormat::COUNT);
		AddTotalColumn(Localize("Damage done"), EMatchDetailValue::DAMAGE_DONE, EMatchDetailFormat::COUNT);
		AddTotalColumn(Localize("Damage taken"), EMatchDetailValue::DAMAGE_TAKEN, EMatchDetailFormat::COUNT);
		break;
	case EStatsMatchTab::SHOTS:
		AddWeaponColumns(EMatchDetailValue::SHOTS, EMatchDetailFormat::COUNT);
		AddTotalColumn(Localize("Total"), EMatchDetailValue::SHOTS, EMatchDetailFormat::COUNT);
		AddTotalColumn(Localize("Hits"), EMatchDetailValue::HITS, EMatchDetailFormat::COUNT);
		break;
	case EStatsMatchTab::OBJECTIVES:
		for(size_t Metric = 0; Metric < View.m_vMetricIds.size(); ++Metric)
			if(IsMatchObjectiveSuffix(MatchMetricSuffix(View.m_vMetricIds[Metric])))
				AddMetricColumn(Metric);
		break;
	case EStatsMatchTab::RUN:
		for(size_t Metric = 0; Metric < View.m_vMetricIds.size(); ++Metric)
			if(IsMatchRunSuffix(MatchMetricSuffix(View.m_vMetricIds[Metric])))
				AddMetricColumn(Metric);
		break;
	case EStatsMatchTab::COUNT:
		break;
	}

	// The team summaries stay out of this: a team beating every one of its
	// players at their own column would emphasise nothing worth seeing.
	for(CMatchDetailColumn &Column : View.m_vColumns)
	{
		// Leading a column of deaths or of damage taken is not an achievement,
		// and pointing at it would read as one.
		if(Column.m_Value == EMatchDetailValue::DEATHS || Column.m_Value == EMatchDetailValue::DEATHS_HOLDING || Column.m_Value == EMatchDetailValue::DAMAGE_TAKEN)
			continue;
		if(Column.m_Metric >= 0 && MatchMetricSuffix(View.m_vMetricIds[Column.m_Metric]) == "suicides")
			continue;
		for(const CMatchDetailRow &Row : View.m_vRows)
		{
			const std::optional<int64_t> Value = MatchDetailCell(Row, Column);
			// A rank or a run of zero means the participant has none, not that
			// they were the fastest of the match.
			if(!Value.has_value() || (Column.m_LowerIsBetter && *Value <= 0))
				continue;
			if(!Column.m_Best.has_value() || (Column.m_LowerIsBetter ? *Value < *Column.m_Best : *Value > *Column.m_Best))
				Column.m_Best = *Value;
		}
	}
}
