/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "gamecontroller.h"

#include "entities/character.h"
#include "entities/laser.h"
#include "entities/pickup.h"
#include "entities/projectile.h"
#include "gamecontext.h"
#include "mode/game_services.h"
#include "player.h"

#include <base/log.h>
#include <base/net.h>
#include <base/time.h>

#include <engine/antibot.h>
#include <engine/config.h>
#include <engine/message.h>
#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol_ex.h>
#include <engine/shared/protocolglue.h>

#include <generated/protocol.h>
#include <generated/server_data.h>

#include <game/mapitems.h>
#include <game/teamscore.h>

#include <algorithm>
#include <limits>

IGameController::IGameController(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	m_GameModeInfo(GameModeInfo),
	m_MatchLifecycle(Services.Server()->Tick())
{
	RegisterMapEntityFactory(CreateCommonMapEntity);
	m_pServices = &Services;
	m_pServer = Services.Server();
	m_pGameType = g_Config.m_SvTestingCommands ? m_GameModeInfo.m_pTestingGameType : m_GameModeInfo.m_pGameType;
	m_aLastLiveStatsRequestTick.fill(std::numeric_limits<int>::min());
}

CGameContext *IGameController::GameServer() const
{
	return m_pServices->GameServer();
}

CTeamsCore &IGameController::TeamsCore()
{
	return *GameServer()->m_World.Teams();
}

const CTeamsCore &IGameController::TeamsCore() const
{
	return *GameServer()->m_World.Teams();
}

IGameController::~IGameController()
{
	GameServer()->Console()->DeregisterOwner(this);
}

IGameController::CMatchParticipantState *IGameController::MatchParticipant(const CPlayer *pPlayer)
{
	for(CMatchParticipantState &State : m_vMatchParticipants)
		if(State.m_UniqueClientId == pPlayer->GetUniqueCid())
			return &State;
	return nullptr;
}

IGameController::CMatchParticipantState *IGameController::EnsureMatchParticipant(CPlayer *pPlayer)
{
	if(!m_pMatchReportBuilder || pPlayer->GetTeam() == TEAM_SPECTATORS)
		return nullptr;
	if(CMatchParticipantState *pState = MatchParticipant(pPlayer))
		return pState;
	if(m_vMatchParticipants.size() >= MatchReportLimits::MAX_PARTICIPANTS)
	{
		// The round outlived more players than a report can hold. Everyone who
		// got in keeps their statistics; whoever comes after this is not
		// counted, which is a far better answer than throwing the round away.
		if(!m_MatchReportOverflow)
			log_warn("game", "match report participant limit reached, later players are not counted");
		m_MatchReportOverflow = true;
		return nullptr;
	}

	CMatchParticipant Participant;
	Participant.m_ParticipantId = m_NextMatchParticipantId++;
	Participant.m_DisplayName = Server()->ClientName(pPlayer->GetCid());
	Participant.m_Clan = Server()->ClientClan(pPlayer->GetCid());
	Participant.m_JoinedTick = std::max(0, Server()->Tick() - m_MatchReportStartTick);
	if(IsTeamPlay())
		Participant.m_TeamId = pPlayer->GetTeam();
	m_vMatchParticipants.push_back({pPlayer->GetUniqueCid(), std::move(Participant), Server()->Tick()});
	return &m_vMatchParticipants.back();
}

void IGameController::UpdateMatchParticipant(CPlayer *pPlayer, bool Leaving)
{
	CMatchParticipantState *pState = MatchParticipant(pPlayer);
	if(!pState && !Leaving)
		pState = EnsureMatchParticipant(pPlayer);
	if(!pState)
		return;

	pState->m_Participant.m_DisplayName = Server()->ClientName(pPlayer->GetCid());
	pState->m_Participant.m_Clan = Server()->ClientClan(pPlayer->GetCid());
	pState->m_Score = SnapPlayerScore(SERVER_DEMO_CLIENT, pPlayer);
	if(IsTeamPlay() && pPlayer->GetTeam() != TEAM_SPECTATORS)
		pState->m_Participant.m_TeamId = pPlayer->GetTeam();
	if(!Leaving && pState->m_ActiveSinceTick < 0)
	{
		pState->m_ActiveSinceTick = Server()->Tick();
		pState->m_Participant.m_LeftTick.reset();
	}
	if(Leaving && pState->m_ActiveSinceTick >= 0)
	{
		pState->m_PlaytimeTicks += Server()->Tick() - pState->m_ActiveSinceTick;
		pState->m_ActiveSinceTick = -1;
		pState->m_Participant.m_LeftTick = std::max(0, Server()->Tick() - m_MatchReportStartTick);
	}
}

void IGameController::StartMatchReport()
{
	if(Info().m_Report.m_ModeId.empty() || Match().IsWarmup())
		return;

	CMatchReport Report;
	Report.m_MatchId = RandomUuid();
	Report.m_GameUuid = GameServer()->GameUuid();
	Report.m_ModeId = Info().m_Report.m_ModeId;
	Report.m_ModeSchemaVersion = Info().m_Report.m_SchemaVersion;
	Report.m_MapName = GameServer()->Map()->FullName();
	Report.m_MapSha256 = GameServer()->Map()->Sha256();
	Report.m_StartTimeUtc = time_timestamp();
	Report.m_TickRate = Server()->TickSpeed();
	Report.m_RoundStartTick = Match().RoundStartTick();
	Report.m_Ranked = false;
	Report.m_UnrankedReason = "server_not_ranked";
	if(IsTeamPlay())
	{
		Report.m_vTeams.push_back({TEAM_RED, "Red"});
		Report.m_vTeams.push_back({TEAM_BLUE, "Blue"});
	}
	m_MatchReportStartTick = Match().RoundStartTick();
	m_NextMatchParticipantId = 0;
	m_MatchReportOverflow = false;
	m_vMatchParticipants.clear();
	m_pMatchReportBuilder = std::make_unique<CMatchReportBuilder>(std::move(Report));
	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
		if(pPlayer)
			EnsureMatchParticipant(pPlayer);
}

EMatchMetricAggregation IGameController::MatchMetricAggregation(const char *pMetricId) const
{
	for(const CGameModeMetricInfo &Metric : Info().m_Report.m_vMetrics)
		if(Metric.m_Id == pMetricId)
			return Metric.m_Aggregation;
	return EMatchMetricAggregation::INVALID;
}

const CGameModeReportInfo &IGameController::LiveStatsInfo() const
{
	return Info().m_LiveStats.m_ModeId.empty() ? Info().m_Report : Info().m_LiveStats;
}

bool IGameController::InitializeLiveStatsReport(CMatchReport &Report) const
{
	const CGameModeReportInfo &LiveInfo = LiveStatsInfo();
	if(LiveInfo.m_ModeId.empty() || m_LiveStatsInstanceId == UUID_ZEROED)
		return false;
	Report = {};
	Report.m_MatchId = m_LiveStatsInstanceId;
	Report.m_GameUuid = GameServer()->GameUuid();
	Report.m_ModeId = LiveInfo.m_ModeId;
	Report.m_ModeSchemaVersion = LiveInfo.m_SchemaVersion;
	Report.m_MapName = GameServer()->Map()->FullName();
	Report.m_MapSha256 = GameServer()->Map()->Sha256();
	Report.m_StartTimeUtc = m_LiveStatsStartTimeUtc;
	Report.m_EndTimeUtc = time_timestamp();
	Report.m_DurationTicks = std::max(0, Server()->Tick() - m_LiveStatsStartTick);
	Report.m_TickRate = Server()->TickSpeed();
	Report.m_RoundStartTick = m_LiveStatsStartTick;
	Report.m_Termination = EMatchTermination::ABORTED;
	Report.m_Ranked = false;
	Report.m_UnrankedReason = "live_snapshot";
	return true;
}

bool IGameController::BuildLiveStatsReport(int ClientId, CMatchReport &Report, int &LocalParticipantId, std::string &Payload)
{
	if(!m_pMatchReportBuilder || ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return false;
	if(pPlayer->GetTeam() != TEAM_SPECTATORS)
		UpdateMatchParticipant(pPlayer, false);
	CMatchParticipantState *pState = MatchParticipant(pPlayer);
	if(!pState)
		return false;

	Report = m_pMatchReportBuilder->Report();
	Report.m_EndTimeUtc = time_timestamp();
	Report.m_DurationTicks = std::max(0, Server()->Tick() - m_MatchReportStartTick);
	Report.m_Termination = EMatchTermination::ABORTED;
	Report.m_vParticipants.clear();
	Report.m_vStandings.clear();
	Report.m_vMetrics.erase(std::remove_if(Report.m_vMetrics.begin(), Report.m_vMetrics.end(), [pState](const CMatchMetric &Metric) {
		return Metric.m_SubjectKind != EMatchSubjectKind::MATCH &&
		       (Metric.m_SubjectKind != EMatchSubjectKind::PARTICIPANT || Metric.m_SubjectId != pState->m_Participant.m_ParticipantId);
	}),
		Report.m_vMetrics.end());

	CMatchParticipant Participant = pState->m_Participant;
	if(pPlayer->GetTeam() != TEAM_SPECTATORS)
		Participant.m_LeftTick.reset();
	Participant.m_JoinedTick = std::clamp<int64_t>(Participant.m_JoinedTick, 0, Report.m_DurationTicks);
	CMatchReportBuilder Builder(std::move(Report));
	if(!Builder.AddParticipant(std::move(Participant)))
		return false;
	const std::string ScoreMetricId = Info().m_Report.m_ModeId + "/score";
	const std::string PlaytimeMetricId = Info().m_Report.m_ModeId + "/playtime_ticks";
	const int64_t PlaytimeTicks = pState->m_PlaytimeTicks + (pState->m_ActiveSinceTick >= 0 ? Server()->Tick() - pState->m_ActiveSinceTick : 0);
	if(!Builder.SetMetricValue(EMatchSubjectKind::PARTICIPANT, pState->m_Participant.m_ParticipantId, ScoreMetricId.c_str(), pState->m_Score, MatchMetricAggregation(ScoreMetricId.c_str())) ||
		!Builder.SetMetricValue(EMatchSubjectKind::PARTICIPANT, pState->m_Participant.m_ParticipantId, PlaytimeMetricId.c_str(), PlaytimeTicks, MatchMetricAggregation(PlaytimeMetricId.c_str())))
		return false;
	if(Match().IsSuddenDeath())
	{
		const std::string MetricId = Info().m_Report.m_ModeId + "/sudden_death";
		if(!Builder.SetMetricValue(EMatchSubjectKind::MATCH, std::nullopt, MetricId.c_str(), 1, MatchMetricAggregation(MetricId.c_str())))
			return false;
	}
	std::string Error;
	if(!Builder.Finalize(&Error) || !MatchReportToPacked(Builder.Report(), Payload, &Error))
	{
		log_error("game", "failed to build live stats: %s", Error.c_str());
		return false;
	}
	Report = Builder.Report();
	LocalParticipantId = pState->m_Participant.m_ParticipantId;
	return true;
}

bool IGameController::AddParticipantMatchMetric(CPlayer *pPlayer, const char *pSuffix, int64_t Value)
{
	CMatchParticipantState *pState = EnsureMatchParticipant(pPlayer);
	if(!pState)
		return false;
	// Every shot, every hit and every death comes through here, so the id is
	// spelled into a buffer on the stack rather than into a fresh string.
	char aMetricId[MatchReportLimits::MAX_METRIC_ID_LENGTH + 1];
	str_format(aMetricId, sizeof(aMetricId), "%s/%s", Info().m_Report.m_ModeId.c_str(), pSuffix);
	return m_pMatchReportBuilder->AddMetricValue(EMatchSubjectKind::PARTICIPANT, pState->m_Participant.m_ParticipantId, aMetricId, Value, MatchMetricAggregation(aMetricId));
}

void IGameController::AddParticipantWeaponMatchMetric(CPlayer *pPlayer, int Weapon, const char *pSuffix, int64_t Value)
{
	// A death can come from the world or from the player itself, which are
	// negative weapon numbers and name no weapon to count this against.
	if(Weapon < WEAPON_HAMMER || Weapon >= NUM_WEAPONS)
		return;
	char aSuffix[64];
	str_format(aSuffix, sizeof(aSuffix), "weapon_%d_%s", Weapon, pSuffix);
	AddParticipantMatchMetric(pPlayer, aSuffix, Value);
}

void IGameController::AddCharacterDamageMatchMetrics(CPlayer *pAttacker, CPlayer *pVictim, int Weapon, int Damage)
{
	if(!m_pMatchReportBuilder || !pVictim || pAttacker == pVictim || Weapon < WEAPON_HAMMER || Weapon >= NUM_WEAPONS || Damage <= 0)
		return;

	if(pAttacker)
	{
		AddParticipantMatchMetric(pAttacker, "hits", 1);
		AddParticipantWeaponMatchMetric(pAttacker, Weapon, "hits", 1);
		AddParticipantMatchMetric(pAttacker, "damage_done", Damage);
		AddParticipantWeaponMatchMetric(pAttacker, Weapon, "damage_done", Damage);
	}
	AddParticipantMatchMetric(pVictim, "damage_taken", Damage);
	AddParticipantWeaponMatchMetric(pVictim, Weapon, "damage_taken", Damage);
}

void IGameController::AddMatchReportStandings()
{
	std::vector<CMatchParticipantState *> vStandings;
	vStandings.reserve(m_vMatchParticipants.size());
	for(CMatchParticipantState &State : m_vMatchParticipants)
		vStandings.push_back(&State);
	std::stable_sort(vStandings.begin(), vStandings.end(), [](const CMatchParticipantState *pLeft, const CMatchParticipantState *pRight) {
		return pLeft->m_Score > pRight->m_Score;
	});

	const bool TopTied = vStandings.size() > 1 && vStandings[0]->m_Score == vStandings[1]->m_Score;
	int Rank = 0;
	for(size_t i = 0; i < vStandings.size(); ++i)
	{
		if(i == 0 || vStandings[i]->m_Score != vStandings[i - 1]->m_Score)
			Rank = static_cast<int>(i) + 1;
		const EMatchOutcome Outcome = Rank == 1 ? (TopTied ? EMatchOutcome::DRAW : EMatchOutcome::WIN) : EMatchOutcome::LOSS;
		m_pMatchReportBuilder->AddStanding({EMatchSubjectKind::PARTICIPANT, vStandings[i]->m_Participant.m_ParticipantId, Rank, Outcome});
	}
}

void IGameController::AddTeamMatchReportStandings(int RedScore, int BlueScore)
{
	const std::string ScoreMetricId = Info().m_Report.m_ModeId + "/score";
	const bool Draw = RedScore == BlueScore;
	for(int Team = TEAM_RED; Team <= TEAM_BLUE; ++Team)
	{
		const int Score = Team == TEAM_RED ? RedScore : BlueScore;
		const bool Won = Draw || (Team == TEAM_RED ? RedScore > BlueScore : BlueScore > RedScore);
		m_pMatchReportBuilder->AddStanding({EMatchSubjectKind::TEAM, Team, Won ? 1 : 2, Draw ? EMatchOutcome::DRAW : (Won ? EMatchOutcome::WIN : EMatchOutcome::LOSS)});
		m_pMatchReportBuilder->SetMetricValue(EMatchSubjectKind::TEAM, Team, ScoreMetricId.c_str(), Score, MatchMetricAggregation(ScoreMetricId.c_str()));
	}
	for(const CMatchParticipantState &State : m_vMatchParticipants)
	{
		if(!State.m_Participant.m_TeamId.has_value())
			continue;
		const int Team = *State.m_Participant.m_TeamId;
		const bool Won = RedScore == BlueScore || (Team == TEAM_RED ? RedScore > BlueScore : BlueScore > RedScore);
		m_pMatchReportBuilder->AddStanding({EMatchSubjectKind::PARTICIPANT, State.m_Participant.m_ParticipantId, Won ? 1 : 2, RedScore == BlueScore ? EMatchOutcome::DRAW : (Won ? EMatchOutcome::WIN : EMatchOutcome::LOSS)});
	}
}

void IGameController::FinalizeMatchReport(EMatchTermination Termination)
{
	if(!m_pMatchReportBuilder)
		return;

	CMatchReport &Report = m_pMatchReportBuilder->Report();
	Report.m_EndTimeUtc = time_timestamp();
	Report.m_DurationTicks = std::max(0, Server()->Tick() - m_MatchReportStartTick);
	Report.m_Termination = Termination;
	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
		if(pPlayer)
			UpdateMatchParticipant(pPlayer, pPlayer->GetTeam() == TEAM_SPECTATORS);
	for(CMatchParticipantState &State : m_vMatchParticipants)
	{
		const int64_t PlaytimeTicks = State.m_PlaytimeTicks + (State.m_ActiveSinceTick >= 0 ? Server()->Tick() - State.m_ActiveSinceTick : 0);
		m_pMatchReportBuilder->AddParticipant(State.m_Participant);
		const std::string ScoreMetricId = Info().m_Report.m_ModeId + "/score";
		m_pMatchReportBuilder->SetMetricValue(EMatchSubjectKind::PARTICIPANT, State.m_Participant.m_ParticipantId, ScoreMetricId.c_str(), State.m_Score, MatchMetricAggregation(ScoreMetricId.c_str()));
		const std::string PlaytimeMetricId = Info().m_Report.m_ModeId + "/playtime_ticks";
		m_pMatchReportBuilder->SetMetricValue(EMatchSubjectKind::PARTICIPANT, State.m_Participant.m_ParticipantId, PlaytimeMetricId.c_str(), PlaytimeTicks, MatchMetricAggregation(PlaytimeMetricId.c_str()));
	}
	AddMatchReportStandings();
	if(Termination != EMatchTermination::COMPLETED)
	{
		for(CMatchStanding &Standing : Report.m_vStandings)
			Standing.m_Outcome = EMatchOutcome::DNF;
	}

	std::string Error;
	std::string Payload;
	if(m_pMatchReportBuilder->Finalize(&Error) && MatchReportToPacked(m_pMatchReportBuilder->Report(), Payload, &Error))
	{
		m_pLatestMatchReport = std::make_unique<CMatchReport>(m_pMatchReportBuilder->Report());
		SendMatchReport(*m_pLatestMatchReport, Payload);
	}
	else
		log_error("game", "failed to finalize match report: %s", Error.c_str());
	m_pMatchReportBuilder.reset();
}

void IGameController::SendMatchReport(const CMatchReport &Report, const std::string &Payload)
{
	const int PayloadSize = static_cast<int>(Payload.size());
	const int NumChunks = (PayloadSize + MatchReportTransportLimits::MAX_CHUNK_SIZE - 1) / MatchReportTransportLimits::MAX_CHUNK_SIZE;
	const SHA256_DIGEST PayloadSha256 = sha256(Payload.data(), Payload.size());
	// Every participant gets the same bytes, and they are handed out over the
	// next ticks, so the payload outlives this call once.
	const auto pSharedPayload = std::make_shared<const std::string>(Payload);
	// Only a participant can store this report, and no demo needs a copy of it,
	// so it does not go to everyone and it is not recorded. The live stats path
	// has always done it this way.
	const int Flags = MSGFLAG_VITAL | MSGFLAG_NORECORD;

	for(CPlayer *pPlayer : GameServer()->m_apPlayers)
	{
		if(pPlayer == nullptr || !Server()->ClientIngame(pPlayer->GetCid()))
			continue;
		const CMatchParticipantState *pParticipant = MatchParticipant(pPlayer);
		if(pParticipant == nullptr)
			continue;
		const int ClientId = pPlayer->GetCid();

		CMsgPacker Start(NETMSG_MATCH_REPORT_START, false);
		Start.AddRaw(&Report.m_MatchId, sizeof(Report.m_MatchId));
		Start.AddInt(Report.m_ReportSchemaVersion);
		Start.AddInt(PayloadSize);
		Start.AddInt(NumChunks);
		if(Start.Error() || Server()->SendMsg(&Start, Flags, ClientId) < 0)
		{
			log_error("game", "failed to send match report start to client %d", ClientId);
			continue;
		}

		CMsgPacker LocalParticipant(NETMSG_MATCH_REPORT_LOCAL_PARTICIPANT, false);
		LocalParticipant.AddRaw(&Report.m_MatchId, sizeof(Report.m_MatchId));
		LocalParticipant.AddInt(pParticipant->m_Participant.m_ParticipantId);
		if(LocalParticipant.Error() || Server()->SendMsg(&LocalParticipant, Flags, ClientId) < 0)
			log_error("game", "failed to send local match participant to client %d", ClientId);

		CPendingReportSend Send;
		Send.m_ClientId = ClientId;
		Send.m_LiveStats = false;
		Send.m_MatchId = Report.m_MatchId;
		Send.m_pPayload = pSharedPayload;
		Send.m_PayloadSha256 = PayloadSha256;
		Send.m_NumChunks = NumChunks;
		m_vPendingReportSends.push_back(Send);
	}
}

void IGameController::QueueReportChunks(const CPendingReportSend &Send)
{
	m_vPendingReportSends.push_back(Send);
}

void IGameController::TickReportSends()
{
	const int Flags = MSGFLAG_VITAL | MSGFLAG_NORECORD;
	for(size_t Index = 0; Index < m_vPendingReportSends.size();)
	{
		CPendingReportSend &Send = m_vPendingReportSends[Index];
		if(!Server()->ClientIngame(Send.m_ClientId))
		{
			m_vPendingReportSends.erase(m_vPendingReportSends.begin() + Index);
			continue;
		}

		const std::string &Payload = *Send.m_pPayload;
		const int PayloadSize = static_cast<int>(Payload.size());
		bool Failed = false;
		const int LastChunk = std::min(Send.m_NumChunks, Send.m_NextChunk + REPORT_CHUNKS_PER_TICK);
		for(; Send.m_NextChunk < LastChunk; ++Send.m_NextChunk)
		{
			const int Offset = Send.m_NextChunk * MatchReportTransportLimits::MAX_CHUNK_SIZE;
			const int ChunkSize = std::min(MatchReportTransportLimits::MAX_CHUNK_SIZE, PayloadSize - Offset);
			CMsgPacker Chunk(Send.m_LiveStats ? NETMSG_LIVE_STATS_CHUNK : NETMSG_MATCH_REPORT_CHUNK, false);
			Chunk.AddRaw(&Send.m_MatchId, sizeof(Send.m_MatchId));
			if(Send.m_LiveStats)
				Chunk.AddInt(Send.m_Revision);
			Chunk.AddInt(Send.m_NextChunk);
			Chunk.AddInt(ChunkSize);
			Chunk.AddRaw(Payload.data() + Offset, ChunkSize);
			if(Chunk.Error() || Server()->SendMsg(&Chunk, Flags, Send.m_ClientId) < 0)
			{
				log_error("game", "failed to send report chunk %d to client %d", Send.m_NextChunk, Send.m_ClientId);
				Failed = true;
				break;
			}
		}

		if(!Failed && Send.m_NextChunk == Send.m_NumChunks)
		{
			CMsgPacker End(Send.m_LiveStats ? NETMSG_LIVE_STATS_END : NETMSG_MATCH_REPORT_END, false);
			End.AddRaw(&Send.m_MatchId, sizeof(Send.m_MatchId));
			if(Send.m_LiveStats)
				End.AddInt(Send.m_Revision);
			End.AddRaw(&Send.m_PayloadSha256, sizeof(Send.m_PayloadSha256));
			if(End.Error() || Server()->SendMsg(&End, Flags, Send.m_ClientId) < 0)
				log_error("game", "failed to send report end to client %d", Send.m_ClientId);
			Failed = true;
		}

		if(Failed)
			m_vPendingReportSends.erase(m_vPendingReportSends.begin() + Index);
		else
			++Index;
	}
}

void IGameController::SendLiveStatsReport(int ClientId, int Revision, const CMatchReport &Report, int LocalParticipantId, bool PersistOnDisconnect, const std::string &Payload)
{
	const int PayloadSize = static_cast<int>(Payload.size());
	const int NumChunks = (PayloadSize + MatchReportTransportLimits::MAX_CHUNK_SIZE - 1) / MatchReportTransportLimits::MAX_CHUNK_SIZE;
	const SHA256_DIGEST PayloadSha256 = sha256(Payload.data(), Payload.size());
	const int Flags = MSGFLAG_VITAL | MSGFLAG_NORECORD;

	CMsgPacker Start(NETMSG_LIVE_STATS_START, false);
	Start.AddRaw(&Report.m_MatchId, sizeof(Report.m_MatchId));
	Start.AddInt(Revision);
	Start.AddInt(Report.m_ReportSchemaVersion);
	Start.AddInt(LocalParticipantId);
	Start.AddInt(PersistOnDisconnect);
	Start.AddInt(PayloadSize);
	Start.AddInt(NumChunks);
	if(Start.Error() || Server()->SendMsg(&Start, Flags, ClientId) < 0)
		return;

	// A client asks for live stats repeatedly, so an older answer that is still
	// on its way is dropped rather than sent alongside the new one.
	std::erase_if(m_vPendingReportSends, [ClientId](const CPendingReportSend &Send) {
		return Send.m_LiveStats && Send.m_ClientId == ClientId;
	});

	CPendingReportSend Send;
	Send.m_ClientId = ClientId;
	Send.m_LiveStats = true;
	Send.m_Revision = Revision;
	Send.m_MatchId = Report.m_MatchId;
	Send.m_pPayload = std::make_shared<const std::string>(Payload);
	Send.m_PayloadSha256 = PayloadSha256;
	Send.m_NumChunks = NumChunks;
	QueueReportChunks(Send);
}

void IGameController::SendLiveStats(int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || !Server()->ClientIngame(ClientId))
		return;
	const int Revision = Server()->Tick();
	const int LastRequestTick = m_aLastLiveStatsRequestTick[ClientId];
	if(LastRequestTick != std::numeric_limits<int>::min() && Revision - LastRequestTick < Server()->TickSpeed() * 2)
		return;
	m_aLastLiveStatsRequestTick[ClientId] = Revision;

	CMatchReport Report;
	int LocalParticipantId = -1;
	std::string Payload;
	if(!BuildLiveStatsReport(ClientId, Report, LocalParticipantId, Payload) || Payload.empty() || Payload.size() > MatchReportLimits::MAX_PAYLOAD_SIZE)
		return;
	const bool PersistOnDisconnect = !Info().m_Report.m_ModeId.empty() && Report.m_ModeId == Info().m_Report.m_ModeId;
	SendLiveStatsReport(ClientId, Revision, Report, LocalParticipantId, PersistOnDisconnect, Payload);
}

void IGameController::FinalizeMatchReportForRestart()
{
	FinalizeMatchReport(EMatchTermination::ADMIN_ENDED);
}

void IGameController::FinalizeMatchReportForShutdown()
{
	FinalizeMatchReport(EMatchTermination::ADMIN_ENDED);
}

void IGameController::Init(CDbConnectionPool *)
{
	Services().World().SetModePhysicsRules(Info().m_PhysicsRules);
	RegisterCommands();
	InitGameSettings();
	m_pGameType = g_Config.m_SvTestingCommands ? m_GameModeInfo.m_pTestingGameType : m_GameModeInfo.m_pGameType;
	m_LiveStatsInstanceId = RandomUuid();
	m_LiveStatsStartTick = Server()->Tick();
	m_LiveStatsStartTimeUtc = time_timestamp();
	DoWarmup(g_Config.m_SvWarmup);
	TeamsCore().Reset();
	StartMatchReport();
}

int IGameController::TuningZoneAt(vec2 Position) const
{
	if(!Info().m_UseTuneZones)
		return 0;
	const int MapIndex = GameServer()->Collision()->GetMapIndex(Position);
	return GameServer()->Collision()->IsTune(MapIndex);
}

CPlayer *IGameController::CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team)
{
	return new CPlayer(Services(), UniqueClientId, ClientId, Team);
}

CCharacter *IGameController::CreateCharacter(CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	return new CCharacter(&Services().World(), Services().LastPlayerInput(ClientId));
}

void IGameController::LoadGameSettings()
{
	GameServer()->ConfigManager()->SetGameSettingsReadOnly(false);
	GameServer()->ConfigManager()->SetReadOnly("sv_gametype", true);
	GameServer()->Console()->ExecuteFile(g_Config.m_SvResetFile, IConsole::CLIENT_ID_UNSPECIFIED);
	GameServer()->LoadMapSettings();
	GameServer()->ConfigManager()->SetReadOnly("sv_gametype", false);
	GameServer()->ConfigManager()->SetGameSettingsReadOnly(true);
}

void IGameController::InitGameSettings()
{
	for(int i = 0; i < TuneZone::NUM; i++)
	{
		GameServer()->TuningList()[i] = CTuningParams::DEFAULT;
		GameServer()->m_aaZoneEnterMsg[i][0] = 0;
		GameServer()->m_aaZoneLeaveMsg[i][0] = 0;
	}
	if(g_Config.m_SvTuneReset)
		ResetTuning();

	LoadGameSettings();
}

void IGameController::ResetTuning()
{
	*GameServer()->GlobalTuning() = CTuningParams::DEFAULT;
	GameServer()->SendTuningParams(-1);
}

void IGameController::DoActivityCheck()
{
	if(g_Config.m_SvInactiveKickTime == 0)
		return;

	for(int i = 0; i < MAX_CLIENTS; ++i)
	{
		if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetTeam() != TEAM_SPECTATORS && !Server()->IsRconAuthed(i))
		{
			if(Server()->Tick() > GameServer()->m_apPlayers[i]->m_LastActionTick + g_Config.m_SvInactiveKickTime * Server()->TickSpeed() * 60)
			{
				switch(g_Config.m_SvInactiveKick)
				{
				case 0:
				{
					// move player to spectator
					DoTeamChange(GameServer()->m_apPlayers[i], TEAM_SPECTATORS, true);
				}
				break;
				case 1:
				{
					// move player to spectator if the reserved slots aren't filled yet, kick them otherwise
					int Spectators = 0;
					for(auto &pPlayer : GameServer()->m_apPlayers)
						if(pPlayer && pPlayer->GetTeam() == TEAM_SPECTATORS)
							++Spectators;
					if(Spectators >= g_Config.m_SvSpectatorSlots)
						Server()->Kick(i, "Kicked for inactivity");
					else
						DoTeamChange(GameServer()->m_apPlayers[i], TEAM_SPECTATORS, true);
				}
				break;
				case 2:
				{
					// kick the player
					Server()->Kick(i, "Kicked for inactivity");
				}
				}
			}
		}
	}
}

void IGameController::OnPlayerSetTeam(int ClientId, int Team)
{
	if(IsGamePaused())
		return;

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->GetTeam() == Team)
		return;
	if(g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > Server()->Tick())
		return;

	if(pPlayer->m_TeamChangeTick > Server()->Tick())
	{
		pPlayer->m_LastSetTeam = Server()->Tick();
		const int TimeLeft = (pPlayer->m_TeamChangeTick - Server()->Tick()) / Server()->TickSpeed();
		char aTime[32];
		str_time((int64_t)TimeLeft * 100, ETimeFormat::HOURS, aTime, sizeof(aTime));
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "Time to wait before changing team: %s", aTime);
		GameServer()->SendBroadcast(aBuf, ClientId);
		return;
	}

	char aTeamJoinError[512];
	if(CanJoinTeam(Team, ClientId, aTeamJoinError, sizeof(aTeamJoinError)))
	{
		if(pPlayer->GetTeam() == TEAM_SPECTATORS || Team == TEAM_SPECTATORS)
			GameServer()->m_VoteUpdate = true;
		DoTeamChange(pPlayer, Team, true);
		pPlayer->m_TeamChangeTick = Server()->Tick();
	}
	else
		GameServer()->SendBroadcast(aTeamJoinError, ClientId);
}

void IGameController::OnPlayerKill(int ClientId)
{
	if(IsGamePaused())
		return;

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->m_LastKill && pPlayer->m_LastKill + Server()->TickSpeed() * g_Config.m_SvKillDelay > Server()->Tick())
		return;
	if(pPlayer->IsPaused() || !pPlayer->GetCharacter())
		return;

	pPlayer->m_LastKill = Server()->Tick();
	pPlayer->KillCharacter(WEAPON_SELF);
	pPlayer->Respawn();
}

void IGameController::OnPlayerCallKickVote(int ClientId, int TargetId, const char *pReason)
{
	if(g_Config.m_SvVoteKickMin)
	{
		int NumPlayers = 0;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!GameServer()->m_apPlayers[i] || GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS)
				continue;

			++NumPlayers;
			for(int j = 0; j < i; ++j)
			{
				if(GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->GetTeam() != TEAM_SPECTATORS &&
					!net_addr_comp_noport(Server()->ClientAddr(i), Server()->ClientAddr(j)))
				{
					--NumPlayers;
					break;
				}
			}
		}

		if(NumPlayers < g_Config.m_SvVoteKickMin)
		{
			char aMessage[128];
			str_format(aMessage, sizeof(aMessage), "Kick voting requires %d players", g_Config.m_SvVoteKickMin);
			GameServer()->SendChatTarget(ClientId, aMessage);
			return;
		}
	}

	char aChatMessage[512];
	str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to kick '%s' (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
	char aSixupDescription[VOTE_DESC_LENGTH];
	str_format(aSixupDescription, sizeof(aSixupDescription), "%2d: %s", TargetId, Server()->ClientName(TargetId));
	char aCommand[VOTE_CMD_LENGTH];
	char aDescription[VOTE_DESC_LENGTH];
	if(!g_Config.m_SvVoteKickBantime)
	{
		str_format(aCommand, sizeof(aCommand), "kick %d Kicked by vote", TargetId);
		str_format(aDescription, sizeof(aDescription), "Kick '%s'", Server()->ClientName(TargetId));
	}
	else
	{
		str_format(aCommand, sizeof(aCommand), "ban %s %d Banned by vote", Server()->ClientAddrString(TargetId, false), g_Config.m_SvVoteKickBantime);
		str_format(aDescription, sizeof(aDescription), "Ban '%s'", Server()->ClientName(TargetId));
	}

	GameServer()->m_apPlayers[ClientId]->m_LastKickVote = time_get();
	GameServer()->m_VoteType = CGameContext::VOTE_TYPE_KICK;
	GameServer()->m_VoteVictim = TargetId;
	GameServer()->CallVote(ClientId, aDescription, aCommand, pReason, aChatMessage, aSixupDescription);
}

void IGameController::OnPlayerCallSpectateVote(int ClientId, int TargetId, const char *pReason)
{
	char aChatMessage[512];
	str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to move '%s' to spectators (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
	char aDescription[VOTE_DESC_LENGTH];
	str_format(aDescription, sizeof(aDescription), "Move '%s' to spectators", Server()->ClientName(TargetId));
	char aSixupDescription[VOTE_DESC_LENGTH];
	str_format(aSixupDescription, sizeof(aSixupDescription), "%2d: %s", TargetId, Server()->ClientName(TargetId));
	char aCommand[VOTE_CMD_LENGTH];
	str_format(aCommand, sizeof(aCommand), "set_team %d -1 %d", TargetId, g_Config.m_SvVoteSpectateRejoindelay);

	GameServer()->m_VoteType = CGameContext::VOTE_TYPE_SPECTATE;
	GameServer()->m_VoteVictim = TargetId;
	GameServer()->CallVote(ClientId, aDescription, aCommand, pReason, aChatMessage, aSixupDescription);
}

bool IGameController::CanPlayerVoteOnTargetVote(int, int VoterId) const
{
	return GameServer()->m_apPlayers[VoterId]->GetTeam() != TEAM_SPECTATORS;
}

int IGameController::PlayerVetoActivityStartTick(int ClientId) const
{
	return GameServer()->m_apPlayers[ClientId]->m_JoinTick;
}

int IGameController::PlayerTeamGroup(int ClientId) const
{
	return GameServer()->m_apPlayers[ClientId]->GetTeam();
}

bool IGameController::CanPlayerReceivePreInput(int, int) const
{
	return true;
}

float IGameController::EvaluateSpawnPos(CSpawnEval *pEval, vec2 Pos, int ClientId)
{
	float Score = 0.0f;
	CCharacter *pC = static_cast<CCharacter *>(GameServer()->m_World.FindFirst(CGameWorld::ENTTYPE_CHARACTER));
	for(; pC; pC = (CCharacter *)pC->TypeNext())
	{
		if(!pC->CanCollide(ClientId))
			continue;

		const float TeamWeight = pEval->m_FriendlyTeam != -1 && pC->GetPlayer()->GetTeam() == pEval->m_FriendlyTeam ? 0.5f : 1.0f;
		const float d = distance(Pos, pC->m_Pos);
		Score += TeamWeight * (d == 0 ? 1000000000.0f : 1.0f / d);
	}

	return Score;
}

void IGameController::EvaluateSpawnType(CSpawnEval *pEval, ESpawnType SpawnType, int ClientId)
{
	const bool PlayerCollision = GameServer()->GlobalTuning()->m_PlayerCollision;

	bool PlayerCollisionDisabled = false;
	CCharacter *pPlayerCharacter = GameServer()->GetPlayerChar(ClientId);
	if(pPlayerCharacter)
		PlayerCollisionDisabled = pPlayerCharacter->GetCore().m_CollisionDisabled;

	// make sure players keep spawning at the same tile
	// on race maps no matter what
	if(!PlayerCollision && pEval->m_Got)
		return;

	// j == 0: Find an empty slot, j == 1: Take any slot if no empty one found
	for(int j = 0; j < 2; j++)
	{
		// get spawn point
		for(const vec2 &SpawnPoint : m_avSpawnPoints[SpawnType])
		{
			vec2 P = SpawnPoint;
			if(j == 0)
			{
				// check if the position is occupado
				CEntity *apEnts[MAX_CLIENTS];
				int Num = GameServer()->m_World.FindEntities(SpawnPoint, 64, apEnts, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
				vec2 aPositions[5] = {vec2(0.0f, 0.0f), vec2(-32.0f, 0.0f), vec2(0.0f, -32.0f), vec2(32.0f, 0.0f), vec2(0.0f, 32.0f)}; // start, left, up, right, down
				int Result = -1;
				for(int Index = 0; Index < 5 && Result == -1; ++Index)
				{
					Result = Index;
					if(!PlayerCollision || PlayerCollisionDisabled)
						break;
					for(int c = 0; c < Num; ++c)
					{
						CCharacter *pChr = static_cast<CCharacter *>(apEnts[c]);
						const bool CanCollide = pChr->CanCollide(ClientId) && !pChr->GetCore().m_CollisionDisabled;

						if(GameServer()->Collision()->CheckPoint(SpawnPoint + aPositions[Index]) ||
							(CanCollide && distance(pChr->m_Pos, SpawnPoint + aPositions[Index]) <= pChr->GetProximityRadius()))
						{
							Result = -1;
							break;
						}
					}
				}
				if(Result == -1)
					continue; // try next spawn point

				P += aPositions[Result];
			}

			float S = EvaluateSpawnPos(pEval, P, ClientId);
			if(!pEval->m_Got || (j == 0 && pEval->m_Score > S))
			{
				pEval->m_Got = true;
				pEval->m_Score = S;
				pEval->m_Pos = P;
			}
		}
	}
}

bool IGameController::CanSpawn(int Team, vec2 *pOutPos, int ClientId)
{
	// spectators can't spawn
	if(Team == TEAM_SPECTATORS)
		return false;

	CSpawnEval Eval;
	EvaluateSpawnType(&Eval, SPAWNTYPE_DEFAULT, ClientId);
	EvaluateSpawnType(&Eval, SPAWNTYPE_RED, ClientId);
	EvaluateSpawnType(&Eval, SPAWNTYPE_BLUE, ClientId);

	*pOutPos = Eval.m_Pos;
	return Eval.m_Got;
}

bool IGameController::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	dbg_assert(Index >= 0, "Invalid entity index");
	return m_EntityRegistry.CreateMapEntity(*this, {Index, x, y, Layer, Flags, Initial, Number});
}

bool IGameController::CreateCommonMapEntity(IGameController &Controller, const CMapEntityContext &Context)
{
	const vec2 Pos(Context.m_X * 32.0f + 16.0f, Context.m_Y * 32.0f + 16.0f);

	if(Context.m_Index >= ENTITY_SPAWN && Context.m_Index <= ENTITY_SPAWN_BLUE)
	{
		if(Context.m_Initial)
		{
			const int SpawnType = Context.m_Index - ENTITY_SPAWN;
			Controller.m_avSpawnPoints[SpawnType].push_back(Pos);
		}
		return true;
	}

	int Type = -1;
	int SubType = 0;

	if(Context.m_Index == ENTITY_ARMOR_1)
		Type = POWERUP_ARMOR;
	else if(Context.m_Index == ENTITY_ARMOR_SHOTGUN)
		Type = POWERUP_ARMOR_SHOTGUN;
	else if(Context.m_Index == ENTITY_ARMOR_GRENADE)
		Type = POWERUP_ARMOR_GRENADE;
	else if(Context.m_Index == ENTITY_ARMOR_NINJA)
		Type = POWERUP_ARMOR_NINJA;
	else if(Context.m_Index == ENTITY_ARMOR_LASER)
		Type = POWERUP_ARMOR_LASER;
	else if(Context.m_Index == ENTITY_HEALTH_1)
		Type = POWERUP_HEALTH;
	else if(Context.m_Index == ENTITY_WEAPON_SHOTGUN)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_SHOTGUN;
	}
	else if(Context.m_Index == ENTITY_WEAPON_GRENADE)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_GRENADE;
	}
	else if(Context.m_Index == ENTITY_WEAPON_LASER)
	{
		Type = POWERUP_WEAPON;
		SubType = WEAPON_LASER;
	}
	else if(Context.m_Index == ENTITY_POWERUP_NINJA)
	{
		Type = POWERUP_NINJA;
		SubType = WEAPON_NINJA;
	}
	if(Type != -1) // NOLINT(clang-analyzer-unix.Malloc)
	{
		const int PickupFlags = Controller.TileFlagsToPickupFlags(Context.m_Flags);
		CPickup *pPickup = new CPickup(&Controller.GameServer()->m_World, Type, SubType, Context.m_Layer, Context.m_Number, PickupFlags);
		pPickup->m_Pos = Pos;
		return true; // NOLINT(clang-analyzer-unix.Malloc)
	}

	return false;
}

void IGameController::OnPlayerConnect(CPlayer *pPlayer)
{
	int ClientId = pPlayer->GetCid();
	pPlayer->Respawn();
	EnsureMatchParticipant(pPlayer);

	if(!Server()->ClientPrevIngame(ClientId))
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' team=%d", ClientId, Server()->ClientName(ClientId), pPlayer->GetTeam());
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	if(Server()->IsSixup(ClientId))
	{
		SendGameInfoSixup(ClientId);

		// Override Sixup's built-in /team only when the active mode provides its own command.
		if(GameServer()->Console()->GetCommandInfo("team", CFGFLAG_CHAT, false))
		{
			protocol7::CNetMsg_Sv_CommandInfoRemove Msg;
			Msg.m_pName = "team";
			Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
		}
	}
}

void IGameController::OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason)
{
	UpdateMatchParticipant(pPlayer, true);
	pPlayer->OnDisconnect();
	int ClientId = pPlayer->GetCid();
	if(Server()->ClientIngame(ClientId))
	{
		char aBuf[512];
		if(pReason && *pReason)
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game (%s)", Server()->ClientName(ClientId), pReason);
		else
			str_format(aBuf, sizeof(aBuf), "'%s' has left the game", Server()->ClientName(ClientId));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf, -1);

		str_format(aBuf, sizeof(aBuf), "leave player='%d:%s'", ClientId, Server()->ClientName(ClientId));
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "game", aBuf);
	}
}

int IGameController::PlayerAutoRespawnTick(const CPlayer *pPlayer) const
{
	return pPlayer->m_DieTick + 2;
}

IGameModeMapReloadState *IGameController::MapReloadState() const
{
	return GameServer()->GameHost().MapReloadState();
}

void IGameController::DiscardMapReloadState(int ClientId)
{
	GameServer()->GameHost().DiscardMapReloadState(ClientId);
}

void IGameController::RestoreCharacterAfterMapReload(CCharacter *pCharacter)
{
	DiscardMapReloadState(pCharacter->GetPlayer()->GetCid());
}

void IGameController::EndRound()
{
	const bool SuddenDeath = Match().IsSuddenDeath();
	if(!Match().EndRound(Server()->Tick()))
		return;
	if(SuddenDeath && m_pMatchReportBuilder)
	{
		const std::string MetricId = Info().m_Report.m_ModeId + "/sudden_death";
		m_pMatchReportBuilder->SetMetricValue(EMatchSubjectKind::MATCH, std::nullopt, MetricId.c_str(), 1, MatchMetricAggregation(MetricId.c_str()));
	}
	FinalizeMatchReport(EMatchTermination::COMPLETED);

	SetGamePaused(true);
	log_info("game", "end round type='%s'", m_pGameType);
}

void IGameController::ResetGame()
{
	GameServer()->m_World.m_ResetRequested = true;
}

bool IGameController::IsValidTeam(int Team)
{
	return Team == TEAM_SPECTATORS || Team == TEAM_GAME;
}

const char *IGameController::GetTeamName(int Team)
{
	switch(Team)
	{
	case TEAM_SPECTATORS:
		return "spectators";
	case TEAM_GAME:
		return "game";
	default:
		dbg_assert_failed("Invalid Team: %d", Team);
	}
}

void IGameController::SetGamePaused(bool Paused)
{
	// Cannot unpause the game while gameover is active
	if(Match().IsGameOver() && !Paused)
	{
		return;
	}
	GameServer()->m_World.m_Paused = Paused;
}

bool IGameController::IsGamePaused() const
{
	return GameServer()->m_World.m_Paused;
}

void IGameController::StartRound()
{
	FinalizeMatchReportForRestart();
	ResetGame();

	Match().StartRound(Server()->Tick());
	StartMatchReport();
	SetGamePaused(false);
	Server()->DemoRecorder_HandleAutoStart();
	log_info("game", "start round type='%s' teamplay='%d'", m_pGameType, Info().m_GameFlags & GAMEFLAG_TEAMS);
}

void IGameController::ChangeMap(const char *pToMap)
{
	FinalizeMatchReportForRestart();
	Server()->ChangeMap(pToMap);
}

void IGameController::OnReset()
{
	for(auto &pPlayer : GameServer()->m_apPlayers)
		if(pPlayer)
			pPlayer->Respawn();
}

void IGameController::FinalizeCharacterDeath(const CGameCharacterDeathContext &Context, int ModeSpecial)
{
	Context.m_pVictim->FinalizeDeath(Context.m_Killer, Context.m_Weapon, Context.m_SendKillMessage, ModeSpecial);
	if(Context.m_Weapon == WEAPON_GAME)
		return;
	CPlayer *pVictim = Context.m_pVictim->GetPlayer();
	AddParticipantMatchMetric(pVictim, "deaths", 1);
	AddParticipantWeaponMatchMetric(pVictim, Context.m_Weapon, "deaths", 1);
	if(Context.m_pKiller == pVictim)
		AddParticipantMatchMetric(pVictim, "suicides", 1);
	else if(Context.m_pKiller)
	{
		AddParticipantMatchMetric(Context.m_pKiller, "kills", 1);
		AddParticipantWeaponMatchMetric(Context.m_pKiller, Context.m_Weapon, "kills", 1);
	}
}

void IGameController::OnCharacterDeath(const CGameCharacterDeathContext &Context)
{
	FinalizeCharacterDeath(Context);
}

bool IGameController::OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam)
{
	if(Damage)
		pVictim->SetEmote(EMOTE_PAIN, Server()->Tick() + 500 * Server()->TickSpeed() / 1000);

	pVictim->AddVelocity(Force);
	return true;
}

void IGameController::OnCharacterFiredWeapon(CCharacter *pCharacter, int Weapon)
{
	if(!m_pMatchReportBuilder || !pCharacter || Weapon < WEAPON_HAMMER || Weapon >= NUM_WEAPONS)
		return;
	AddParticipantMatchMetric(pCharacter->GetPlayer(), "shots", 1);
	AddParticipantWeaponMatchMetric(pCharacter->GetPlayer(), Weapon, "shots", 1);
}

bool IGameController::CanCharacterHitCharacter(CCharacter *, CCharacter *pTarget) const
{
	return pTarget->IsAlive();
}

CWeaponFireResult IGameController::OnCharacterFireWeapon(const CWeaponFireContext &Context)
{
	CCharacter *pCharacter = Context.m_pCharacter;
	if(!pCharacter || Context.m_Weapon < 0 || Context.m_Weapon >= NUM_WEAPONS || !pCharacter->GetWeaponAmmo(Context.m_Weapon))
		return {};

	CWeaponFireResult Result;
	Result.m_Fired = true;
	const int Owner = pCharacter->GetPlayer()->GetCid();

	switch(Context.m_Weapon)
	{
	case WEAPON_HAMMER:
	{
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_HAMMER_FIRE, pCharacter->TeamMask());
		GameServer()->Antibot()->OnHammerFire(Owner);
		if(pCharacter->HammerHitDisabled())
			break;

		CEntity *apEntities[MAX_CLIENTS];
		const int NumEntities = GameServer()->m_World.FindEntities(
			Context.m_ProjectileStartPosition,
			pCharacter->GetProximityRadius() * 0.5f,
			apEntities,
			MAX_CLIENTS,
			CGameWorld::ENTTYPE_CHARACTER);
		int Hits = 0;
		for(int i = 0; i < NumEntities; i++)
		{
			auto *pTarget = static_cast<CCharacter *>(apEntities[i]);
			if(pTarget == pCharacter || !CanCharacterHitCharacter(pCharacter, pTarget))
				continue;

			if(length(pTarget->m_Pos - Context.m_ProjectileStartPosition) > 0.0f)
				GameServer()->CreateHammerHit(pTarget->m_Pos - normalize(pTarget->m_Pos - Context.m_ProjectileStartPosition) * pCharacter->GetProximityRadius() * 0.5f, pCharacter->TeamMask());
			else
				GameServer()->CreateHammerHit(Context.m_ProjectileStartPosition, pCharacter->TeamMask());

			const vec2 Direction = length(pTarget->m_Pos - pCharacter->m_Pos) > 0.0f ? normalize(pTarget->m_Pos - pCharacter->m_Pos) : vec2(0.0f, -1.0f);
			const vec2 VelocityDelta = pTarget->VelocityDeltaAfterClamping(normalize(Direction + vec2(0.0f, -1.1f)) * 10.0f);
			pTarget->TakeDamage(
				(vec2(0.0f, -1.0f) + VelocityDelta) * Context.m_pTuning->m_HammerStrength,
				g_pData->m_Weapons.m_Hammer.m_pBase->m_Damage,
				Owner,
				Context.m_Weapon);
			pTarget->Unfreeze();
			GameServer()->Antibot()->OnHammerHit(Owner, pTarget->GetPlayer()->GetCid());
			Hits++;
		}

		if(Hits)
			Result.m_ReloadTicks = Context.m_pTuning->m_HammerHitFireDelay * Server()->TickSpeed() / 1000;
		break;
	}
	case WEAPON_GUN:
		if(!pCharacter->Core()->m_Jetpack || !pCharacter->GetPlayer()->m_NinjaJetpack || pCharacter->HasTelegunGun())
		{
			new CProjectile(
				pCharacter->GameWorld(),
				WEAPON_GUN,
				Owner,
				Context.m_ProjectileStartPosition,
				Context.m_Direction,
				Server()->TickSpeed() * Context.m_pTuning->m_GunLifetime,
				false,
				false,
				-1,
				Context.m_MouseTarget);
			GameServer()->CreateSound(pCharacter->m_Pos, SOUND_GUN_FIRE, pCharacter->TeamMask()); // NOLINT(clang-analyzer-unix.Malloc)
		}
		break;
	case WEAPON_SHOTGUN:
		new CLaser(pCharacter->GameWorld(), pCharacter->m_Pos, Context.m_Direction, Context.m_pTuning->m_LaserReach, Owner, WEAPON_SHOTGUN);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_SHOTGUN_FIRE, pCharacter->TeamMask()); // NOLINT(clang-analyzer-unix.Malloc)
		break;
	case WEAPON_GRENADE:
		new CProjectile(
			pCharacter->GameWorld(),
			WEAPON_GRENADE,
			Owner,
			Context.m_ProjectileStartPosition,
			Context.m_Direction,
			Server()->TickSpeed() * Context.m_pTuning->m_GrenadeLifetime,
			false,
			true,
			SOUND_GRENADE_EXPLODE,
			Context.m_MouseTarget);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_GRENADE_FIRE, pCharacter->TeamMask()); // NOLINT(clang-analyzer-unix.Malloc)
		break;
	case WEAPON_LASER:
		new CLaser(pCharacter->GameWorld(), pCharacter->m_Pos, Context.m_Direction, Context.m_pTuning->m_LaserReach, Owner, WEAPON_LASER);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_LASER_FIRE, pCharacter->TeamMask()); // NOLINT(clang-analyzer-unix.Malloc)
		break;
	case WEAPON_NINJA:
		pCharacter->ActivateNinja(Context.m_Direction);
		GameServer()->CreateSound(pCharacter->m_Pos, SOUND_NINJA_FIRE, pCharacter->TeamMask());
		break;
	default:
		return {};
	}

	return Result;
}

CGamePickupResult IGameController::OnCharacterPickup(CCharacter *, int, int, vec2)
{
	return {};
}

CGameProjectileRules IGameController::ProjectileRules(const CGameProjectileContext &Context) const
{
	return {true, false, 0.001f, Context.m_OwnerConnected ? EProjectileOwnerLossAction::KEEP : EProjectileOwnerLossAction::DETACH};
}

void IGameController::OnExplosion(const CGameExplosionContext &Context)
{
	GameServer()->CreateExplosionEvent(Context.m_Position, Context.m_Mask);
	if(Context.m_NoDamage)
		return;

	CEntity *apEntities[MAX_CLIENTS];
	constexpr float Radius = 135.0f;
	constexpr float InnerRadius = 48.0f;
	const int Num = GameServer()->m_World.FindEntities(Context.m_Position, Radius, apEntities, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	for(int i = 0; i < Num; i++)
	{
		auto *pCharacter = static_cast<CCharacter *>(apEntities[i]);
		const vec2 Difference = pCharacter->m_Pos - Context.m_Position;
		const float Distance = length(Difference);
		const vec2 ForceDirection = Distance > 0.0f ? normalize(Difference) : vec2(0.0f, 1.0f);
		const float Falloff = 1.0f - std::clamp((Distance - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		const float Damage = GameServer()->GlobalTuning()->m_ExplosionStrength * Falloff;
		if((int)Damage == 0)
			continue;
		pCharacter->TakeDamage(ForceDirection * Damage * 2.0f, (int)Damage, Context.m_Owner, Context.m_Weapon, true, Context.m_AttackerTeam);
	}
}

void IGameController::OnCharacterSpawn(class CCharacter *pChr)
{
	// default health
	pChr->IncreaseHealth(10);

	// give default weapons
	pChr->GiveWeapon(WEAPON_HAMMER);
	pChr->GiveWeapon(WEAPON_GUN);
}

void IGameController::TickCharacterPostCore(CCharacter *pCharacter)
{
	if(pCharacter->GameLayerClipped(pCharacter->m_Pos) || pCharacter->IsOnDeathTile())
		pCharacter->Die(pCharacter->GetPlayer()->GetCid(), WEAPON_WORLD);
}

void IGameController::DoWarmup(int Seconds)
{
	Match().SetWarmupTicks(Seconds < 0 ? 0 : Seconds * Server()->TickSpeed());
}

void IGameController::SendGameInfoSixup(int ClientId)
{
	protocol7::CNetMsg_Sv_GameInfo Msg;
	Msg.m_GameFlags = Info().m_GameFlags;
	Msg.m_MatchCurrent = 1;
	Msg.m_MatchNum = 0;
	Msg.m_ScoreLimit = ScoreLimit();
	Msg.m_TimeLimit = TimeLimit();
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
}

void IGameController::Tick()
{
	// The 0.6 protocol carries the limits in every snapshot, the 0.7 one only in
	// a message. A server that switches modes or has its limits set from the
	// console would leave those clients on whatever was true when they joined.
	if(ScoreLimit() != m_SixupScoreLimit || TimeLimit() != m_SixupTimeLimit)
	{
		m_SixupScoreLimit = ScoreLimit();
		m_SixupTimeLimit = TimeLimit();
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
		{
			if(Server()->ClientIngame(ClientId) && Server()->IsSixup(ClientId))
				SendGameInfoSixup(ClientId);
		}
	}
	TickReportSends();

	if(Match().TickWarmup())
		StartRound();

	if(Match().ShouldRestartRound(Server()->Tick(), Server()->TickSpeed() * 10))
	{
		StartRound();
		Match().AdvanceRound();
	}

	DoActivityCheck();
}

void IGameController::Snap(int SnappingClient)
{
	CNetObj_GameInfo GameInfo = {};

	GameInfo.m_GameFlags = GameFlags_ClampToSix(Info().m_GameFlags);
	GameInfo.m_GameStateFlags = 0;
	if(Match().IsGameOver())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_GAMEOVER;
	if(Match().IsSuddenDeath())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_SUDDENDEATH;
	if(IsGamePaused())
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_PAUSED;
	GameInfo.m_RoundStartTick = Match().RoundStartTick();
	GameInfo.m_WarmupTimer = Match().WarmupTicks();
	// A mode that ends on a score or a clock has to say so, or the client counts
	// up towards nothing and the scoreboard has no target to show.
	GameInfo.m_ScoreLimit = ScoreLimit();
	GameInfo.m_TimeLimit = TimeLimit();

	GameInfo.m_RoundNum = 0;
	GameInfo.m_RoundCurrent = Match().RoundCount() + 1;
	UpdateGameInfo(GameInfo, SnappingClient);
	Server()->SnapNewItem(0, GameInfo);

	CNetObj_GameInfoEx GameInfoEx = {};
	GameInfoEx.m_Flags = GameInfoFlags(SnappingClient);
	GameInfoEx.m_Flags2 = GameInfoFlags2(SnappingClient);
	GameInfoEx.m_Version = GAMEINFO_CURVERSION;
	GameInfoEx.m_MinTeamSize = g_Config.m_SvMinTeamSize;
	GameInfoEx.m_MaxTeamSize = g_Config.m_SvMaxTeamSize;
	Server()->SnapNewItem(0, GameInfoEx);

	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_GameData GameData = {};
		GameData.m_GameStartTick = Match().RoundStartTick();
		GameData.m_GameStateFlags = 0;
		if(Match().IsGameOver())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_GAMEOVER;
		if(Match().IsSuddenDeath())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_SUDDENDEATH;
		if(IsGamePaused())
			GameData.m_GameStateFlags |= protocol7::GAMESTATEFLAG_PAUSED;
		GameData.m_GameStateEndTick = TimeLimit() > 0 ? Match().RoundStartTick() + TimeLimit() * Server()->TickSpeed() * 60 : 0;
		Server()->SnapNewItem(0, GameData);
	}

	SnapMode(SnappingClient);
}

int IGameController::GetAutoTeam(int NotThisId)
{
	int Team = TEAM_GAME;

	if(CanJoinTeam(Team, NotThisId, nullptr, 0))
		return Team;
	return TEAM_SPECTATORS;
}

int IGameController::ActivePlayerSlots() const
{
	const int ConfiguredSlots = std::max(0, Server()->MaxClients() - g_Config.m_SvSpectatorSlots);
	if(m_GameModeInfo.m_ActivePlayerLimit <= 0)
		return ConfiguredSlots;
	return std::min(ConfiguredSlots, m_GameModeInfo.m_ActivePlayerLimit);
}

bool IGameController::CanJoinTeam(int Team, int NotThisId, char *pErrorReason, int ErrorReasonSize)
{
	const CPlayer *pPlayer = GameServer()->m_apPlayers[NotThisId];
	if(pPlayer && pPlayer->IsPaused())
	{
		if(pErrorReason)
			str_copy(pErrorReason, "Use /pause first then you can kill", ErrorReasonSize);
		return false;
	}
	if(Team == TEAM_SPECTATORS || (pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS))
		return true;

	int aNumplayers[2] = {0, 0};
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(GameServer()->m_apPlayers[i] && i != NotThisId)
		{
			if(GameServer()->m_apPlayers[i]->GetTeam() >= TEAM_RED && GameServer()->m_apPlayers[i]->GetTeam() <= TEAM_BLUE)
				aNumplayers[GameServer()->m_apPlayers[i]->GetTeam()]++;
		}
	}

	if((aNumplayers[0] + aNumplayers[1]) < ActivePlayerSlots())
		return true;

	if(pErrorReason)
		str_format(pErrorReason, ErrorReasonSize, "Only %d active players are allowed", ActivePlayerSlots());
	return false;
}

CClientMask IGameController::GetMaskForPlayerWorldEvent(int, int ExceptId)
{
	CClientMask Mask = CClientMask().set();
	if(ExceptId != -1)
		Mask.reset(ExceptId);
	return Mask;
}

void IGameController::DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(!IsValidTeam(Team))
		return;

	if(Team == pPlayer->GetTeam())
		return;

	pPlayer->SetTeam(Team);
	UpdateMatchParticipant(pPlayer, Team == TEAM_SPECTATORS);
	int ClientId = pPlayer->GetCid();

	char aBuf[128];
	if(DoChatMsg)
	{
		str_format(aBuf, sizeof(aBuf), "'%s' joined the %s", Server()->ClientName(ClientId), GetTeamName(Team));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf);
	}

	str_format(aBuf, sizeof(aBuf), "team_join player='%d:%s' m_Team=%d", ClientId, Server()->ClientName(ClientId), Team);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// OnPlayerInfoChange(pPlayer);
}

int IGameController::TileFlagsToPickupFlags(int TileFlags) const
{
	int PickupFlags = 0;
	if(TileFlags & TILEFLAG_XFLIP)
		PickupFlags |= PICKUPFLAG_XFLIP;
	if(TileFlags & TILEFLAG_YFLIP)
		PickupFlags |= PICKUPFLAG_YFLIP;
	if(TileFlags & TILEFLAG_ROTATE)
		PickupFlags |= PICKUPFLAG_ROTATE;
	return PickupFlags;
}
