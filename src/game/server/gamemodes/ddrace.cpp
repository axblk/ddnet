#include "ddrace.h"

#include "ddrace_map_entities.h"

#include <base/math.h>
#include <base/net.h>
#include <base/time.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <generated/protocol7.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/interactions.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/server/teams.h>

#include <algorithm>

CGameControllerDDRace::CGameControllerDDRace(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	IGameController(Services, GameModeInfo)
{
}

void CGameControllerDDRace::RegisterCommands()
{
	GameServer()->RegisterDDRaceAdminCommands(this);
	GameServer()->RegisterDDRacePlayerCommands(this);
	GameServer()->RegisterDDRacePracticeCommands(this);
	GameServer()->RegisterDDRaceScoreCommands(this);
	GameServer()->RegisterDDRaceTeamCommands(this);
}

void CGameControllerDDRace::OnExplosion(const CGameExplosionContext &Context)
{
	GameServer()->CreateExplosionEvent(Context.m_Position, Context.m_Mask);

	CEntity *apEntities[MAX_CLIENTS];
	constexpr float Radius = 135.0f;
	constexpr float InnerRadius = 48.0f;
	const int Num = GameServer()->m_World.FindEntities(Context.m_Position, Radius, apEntities, MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);
	CClientMask TeamMask = CClientMask().set();
	for(int i = 0; i < Num; i++)
	{
		auto *pCharacter = static_cast<CCharacter *>(apEntities[i]);
		const vec2 Difference = pCharacter->m_Pos - Context.m_Position;
		const float Distance = length(Difference);
		const vec2 ForceDirection = Distance > 0.0f ? normalize(Difference) : vec2(0.0f, 1.0f);
		const float Falloff = 1.0f - std::clamp((Distance - InnerRadius) / (Radius - InnerRadius), 0.0f, 1.0f);
		const float Strength = Context.m_Owner == -1 || !GameServer()->m_apPlayers[Context.m_Owner] || !GameServer()->m_apPlayers[Context.m_Owner]->m_TuneZone ?
					       GameServer()->GlobalTuning()->m_ExplosionStrength :
					       GameServer()->TuningList()[GameServer()->m_apPlayers[Context.m_Owner]->m_TuneZone].m_ExplosionStrength;
		const float Damage = Strength * Falloff;
		if((int)Damage == 0)
			continue;

		if((GameServer()->GetPlayerChar(Context.m_Owner) ? !GameServer()->GetPlayerChar(Context.m_Owner)->GrenadeHitDisabled() : g_Config.m_SvHit) || Context.m_NoDamage || Context.m_Owner == pCharacter->GetPlayer()->GetCid())
		{
			if(Context.m_Owner != -1 && pCharacter->IsAlive() && !pCharacter->CanCollide(Context.m_Owner))
				continue;
			if(Context.m_Owner == -1 && Context.m_ActivatedTeam != -1 && pCharacter->IsAlive() && pCharacter->Team() != Context.m_ActivatedTeam)
				continue;

			// Explode at most once per team.
			const int PlayerTeam = pCharacter->Team();
			if((GameServer()->GetPlayerChar(Context.m_Owner) ? GameServer()->GetPlayerChar(Context.m_Owner)->GrenadeHitDisabled() : !g_Config.m_SvHit) || Context.m_NoDamage)
			{
				if(PlayerTeam == TEAM_SUPER)
					continue;
				if(!TeamMask.test(PlayerTeam))
					continue;
				TeamMask.reset(PlayerTeam);
			}

			pCharacter->TakeDamage(ForceDirection * Damage * 2.0f, (int)Damage, Context.m_Owner, Context.m_Weapon, !Context.m_NoDamage, Context.m_AttackerTeam);
		}
	}
}

void CGameControllerDDRace::OnCharacterSpawn(CCharacter *pCharacter)
{
	pCharacter->SetRaceTeams(&RaceTeams());
	RaceTeams().OnCharacterSpawn(pCharacter->GetPlayer()->GetCid());
	IGameController::OnCharacterSpawn(pCharacter);
	pCharacter->DDRaceInit();
}

void CGameControllerDDRace::TickCharacterPreCore(CCharacter *pCharacter)
{
	pCharacter->DDRaceTick();
}

void CGameControllerDDRace::TickCharacterPostCore(CCharacter *pCharacter)
{
	pCharacter->DDRacePostCoreTick();
}

int CGameControllerDDRace::PlayerAutoRespawnTick(const CPlayer *pPlayer) const
{
	return std::max(pPlayer->m_DieTick, pPlayer->m_PreviousDieTick + Server()->TickSpeed() * 3) + 2;
}

bool CGameControllerDDRace::SaveStateForHotReload()
{
	GameServer()->SaveDDRaceStateForHotReload();
	return true;
}

void CGameControllerDDRace::RestoreCharacterAfterHotReload(CCharacter *pCharacter)
{
	GameServer()->RestoreDDRaceCharacterAfterHotReload(pCharacter);
}

bool CGameControllerDDRace::OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number)
{
	return IGameController::OnEntity(Index, x, y, Layer, Flags, Initial, Number) ||
	       CreateDDRaceMapEntity(GameServer(), Index, x, y, Layer, Flags, Number);
}

void CGameControllerDDRace::OnPlayerConnect(CPlayer *pPlayer)
{
	RaceTeams().ResetPlayer(pPlayer->GetCid());
	IGameController::OnPlayerConnect(pPlayer);
	GameServer()->RaceScore()->ResetPlayer(pPlayer->GetCid());
	RaceTeams().PlayerState(pPlayer->GetCid()).m_ShowOthers = g_Config.m_SvShowOthersDefault;
	if(!Server()->ClientPrevIngame(pPlayer->GetCid()) && g_Config.m_SvShowOthers && g_Config.m_SvShowOthersDefault > SHOW_OTHERS_OFF)
		GameServer()->SendChatTarget(pPlayer->GetCid(), "You can see other players. To disable this use DDNet client and type /showothers");
}

void CGameControllerDDRace::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	IGameController::OnPlayerDisconnect(pPlayer, pReason);
	GameServer()->RaceScore()->ResetPlayer(pPlayer->GetCid());
	RaceTeams().ResetPlayer(pPlayer->GetCid());
}

void CGameControllerDDRace::OnPlayerNameChanged(int ClientId)
{
	GameServer()->RaceScore()->ResetPlayer(ClientId);
	Server()->SetClientScore(ClientId, std::nullopt);
	GameServer()->RaceScore()->LoadPlayerData(ClientId);
}

void CGameControllerDDRace::OnPlayerDDNetVersionKnown(int ClientId)
{
	RaceTeams().SendTeamsState(ClientId);
	GameServer()->SendRecord(ClientId);
}

void CGameControllerDDRace::OnPlayerSetTeam(int ClientId, int Team)
{
	// Keep the cheap common rejection checks ahead of the DDRace-specific policy.
	// The neutral implementation repeats them before applying the operation.
	if(IsGamePaused())
		return;

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->GetTeam() == Team)
		return;
	if(g_Config.m_SvSpamprotection && pPlayer->m_LastSetTeam && pPlayer->m_LastSetTeam + Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > Server()->Tick())
		return;

	CCharacter *pCharacter = pPlayer->GetCharacter();
	if(pCharacter)
	{
		const int CurrentTime = (Server()->Tick() - pCharacter->m_StartTime) / Server()->TickSpeed();
		if(g_Config.m_SvKillProtection != 0 && CurrentTime >= 60 * g_Config.m_SvKillProtection && pCharacter->m_DDRaceState == ERaceState::STARTED)
		{
			GameServer()->SendChatTarget(ClientId, "Kill Protection enabled. If you really want to join the spectators, first type /kill");
			return;
		}
	}

	IGameController::OnPlayerSetTeam(ClientId, Team);
}

void CGameControllerDDRace::OnPlayerKill(int ClientId)
{
	// Keep the cheap common rejection checks ahead of the DDRace-specific policy.
	// The neutral implementation repeats them before applying the operation.
	if(IsGamePaused())
		return;

	if(GameServer()->IsRunningKickOrSpecVote(ClientId) && RaceTeams().m_Core.Team(ClientId))
	{
		GameServer()->SendChatTarget(ClientId, "You are running a vote please try again after the vote is done!");
		return;
	}

	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer->m_LastKill && pPlayer->m_LastKill + Server()->TickSpeed() * g_Config.m_SvKillDelay > Server()->Tick())
		return;
	if(pPlayer->IsPaused())
		return;

	CCharacter *pCharacter = pPlayer->GetCharacter();
	if(!pCharacter)
		return;

	const int CurrentTime = (Server()->Tick() - pCharacter->m_StartTime) / Server()->TickSpeed();
	if(g_Config.m_SvKillProtection != 0 && CurrentTime >= 60 * g_Config.m_SvKillProtection && pCharacter->m_DDRaceState == ERaceState::STARTED)
	{
		GameServer()->SendChatTarget(ClientId, "Kill Protection enabled. If you really want to kill, type /kill");
		return;
	}

	IGameController::OnPlayerKill(ClientId);
}

bool CGameControllerDDRace::CanSeeInteraction(const CInteractions &Interaction, int ClientId) const
{
	const CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	const auto &PlayerState = RaceTeams().PlayerState(ClientId);
	auto IsDifferentTeam = [this, &Interaction](int OtherClientId) {
		const int Team = RaceTeams().m_Core.Team(OtherClientId);
		return Team != Interaction.DDRaceTeam() && Team != TEAM_SUPER;
	};
	auto IsSolo = [this](int OtherClientId) {
		const CCharacter *pCharacter = GameServer()->GetPlayerChar(OtherClientId);
		return pCharacter && pCharacter->Core()->m_Solo;
	};

	if(!(pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused()))
	{
		if(ClientId == Interaction.OwnerId())
			return true;
		if(PlayerState.m_ShowOthers == SHOW_OTHERS_ONLY_TEAM)
			return !IsDifferentTeam(ClientId);
		if(PlayerState.m_ShowOthers == SHOW_OTHERS_OFF)
			return !Interaction.IsSolo() && !IsSolo(ClientId) && !IsDifferentTeam(ClientId);
	}
	else if(pPlayer->SpectatorId() != SPEC_FREEVIEW)
	{
		const int SpectatorId = pPlayer->SpectatorId();
		if(SpectatorId == Interaction.OwnerId())
			return true;
		if(!GameServer()->GetPlayerChar(SpectatorId))
			return false;
		if(PlayerState.m_ShowOthers == SHOW_OTHERS_ONLY_TEAM)
			return !IsDifferentTeam(SpectatorId);
		if(PlayerState.m_ShowOthers == SHOW_OTHERS_OFF)
			return !Interaction.IsSolo() && !IsSolo(SpectatorId) && !IsDifferentTeam(SpectatorId);
	}
	else if(PlayerState.m_SpecTeam)
	{
		return !IsDifferentTeam(ClientId);
	}

	return true;
}

bool CGameControllerDDRace::CanHitInteraction(const CInteractions &Interaction, int ClientId) const
{
	const CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(Interaction.DDRaceTeam() && RaceTeams().m_Core.Team(ClientId) != Interaction.DDRaceTeam())
		return false;
	if(Interaction.IsSolo() && Interaction.UniqueOwnerId() != pPlayer->GetUniqueCid())
		return false;
	if(Interaction.NoHitOthers() && Interaction.UniqueOwnerId() != pPlayer->GetUniqueCid())
		return false;
	if(Interaction.NoHitSelf() && Interaction.UniqueOwnerId() == pPlayer->GetUniqueCid())
		return false;

	return true;
}

void CGameControllerDDRace::OnPlayerCallKickVote(int ClientId, int TargetId, const char *pReason)
{
	const int Team = RaceTeams().m_Core.Team(ClientId);
	if(g_Config.m_SvVoteKickMin && Team == TEAM_FLOCK)
	{
		const NETADDR *apAddresses[MAX_CLIENTS];
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(GameServer()->m_apPlayers[i])
				apAddresses[i] = Server()->ClientAddr(i);
		}

		int NumPlayers = 0;
		for(int i = 0; i < MAX_CLIENTS; ++i)
		{
			if(!GameServer()->m_apPlayers[i] || GameServer()->m_apPlayers[i]->GetTeam() == TEAM_SPECTATORS || RaceTeams().m_Core.Team(i) != TEAM_FLOCK)
				continue;

			++NumPlayers;
			for(int j = 0; j < i; ++j)
			{
				if(GameServer()->m_apPlayers[j] && GameServer()->m_apPlayers[j]->GetTeam() != TEAM_SPECTATORS && RaceTeams().m_Core.Team(j) == TEAM_FLOCK &&
					!net_addr_comp_noport(apAddresses[i], apAddresses[j]))
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

	if(!GameServer()->GetPlayerChar(ClientId) || !GameServer()->GetPlayerChar(TargetId))
	{
		GameServer()->SendChatTarget(ClientId, "You can kick only your team member");
		return;
	}

	const int TargetTeam = RaceTeams().m_Core.Team(TargetId);
	if(Team == TEAM_FLOCK && TargetTeam == TEAM_FLOCK)
	{
		IGameController::OnPlayerCallKickVote(ClientId, TargetId, pReason);
		return;
	}

	char aChatMessage[512];
	char aDescription[VOTE_DESC_LENGTH];
	char aCommand[VOTE_CMD_LENGTH];
	if(Team != TargetTeam)
	{
		if(g_Config.m_SvVoteKickMuteTime)
		{
			GameServer()->SendChatTarget(ClientId, "You can kick only your team member");
			return;
		}
		str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to mute '%s' (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
		str_format(aCommand, sizeof(aCommand), "muteid %d %d Muted by vote", TargetId, g_Config.m_SvVoteKickMuteTime);
		str_format(aDescription, sizeof(aDescription), "Mute '%s'", Server()->ClientName(TargetId));
	}
	else
	{
		str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to kick '%s' (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
		str_format(aCommand, sizeof(aCommand), "uninvite %d %d; set_team_ddr %d 0", TargetId, TargetTeam, TargetId);
		str_format(aDescription, sizeof(aDescription), "Move '%s' to team 0", Server()->ClientName(TargetId));
	}
	char aSixupDescription[VOTE_DESC_LENGTH];
	str_format(aSixupDescription, sizeof(aSixupDescription), "%2d: %s", TargetId, Server()->ClientName(TargetId));

	GameServer()->m_apPlayers[ClientId]->m_LastKickVote = time_get();
	GameServer()->m_VoteType = CGameContext::VOTE_TYPE_KICK;
	GameServer()->m_VoteVictim = TargetId;
	GameServer()->CallVote(ClientId, aDescription, aCommand, pReason, aChatMessage, aSixupDescription);
}

void CGameControllerDDRace::OnPlayerCallSpectateVote(int ClientId, int TargetId, const char *pReason)
{
	if(!GameServer()->GetPlayerChar(ClientId) || !GameServer()->GetPlayerChar(TargetId) || RaceTeams().m_Core.Team(ClientId) != RaceTeams().m_Core.Team(TargetId))
	{
		GameServer()->SendChatTarget(ClientId, "You can only move your team member to spectators");
		return;
	}

	char aChatMessage[512];
	char aDescription[VOTE_DESC_LENGTH];
	char aCommand[VOTE_CMD_LENGTH];
	const int TargetTeam = RaceTeams().m_Core.Team(TargetId);
	if(g_Config.m_SvPauseable && g_Config.m_SvVotePause)
	{
		str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to pause '%s' for %d seconds (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), g_Config.m_SvVotePauseTime, pReason);
		str_format(aDescription, sizeof(aDescription), "Pause '%s' (%ds)", Server()->ClientName(TargetId), g_Config.m_SvVotePauseTime);
		str_format(aCommand, sizeof(aCommand), "uninvite %d %d; force_pause %d %d", TargetId, TargetTeam, TargetId, g_Config.m_SvVotePauseTime);
	}
	else
	{
		str_format(aChatMessage, sizeof(aChatMessage), "'%s' called for vote to move '%s' to spectators (%s)", Server()->ClientName(ClientId), Server()->ClientName(TargetId), pReason);
		str_format(aDescription, sizeof(aDescription), "Move '%s' to spectators", Server()->ClientName(TargetId));
		str_format(aCommand, sizeof(aCommand), "uninvite %d %d; set_team %d -1 %d", TargetId, TargetTeam, TargetId, g_Config.m_SvVoteSpectateRejoindelay);
	}
	char aSixupDescription[VOTE_DESC_LENGTH];
	str_format(aSixupDescription, sizeof(aSixupDescription), "%2d: %s", TargetId, Server()->ClientName(TargetId));

	GameServer()->m_VoteType = CGameContext::VOTE_TYPE_SPECTATE;
	GameServer()->m_VoteVictim = TargetId;
	GameServer()->CallVote(ClientId, aDescription, aCommand, pReason, aChatMessage, aSixupDescription);
}

bool CGameControllerDDRace::CanPlayerVoteOnTargetVote(int VoteCreatorId, int VoterId) const
{
	if(!IGameController::CanPlayerVoteOnTargetVote(VoteCreatorId, VoterId))
		return false;

	const CCharacter *pCreator = GameServer()->GetPlayerChar(VoteCreatorId);
	const CCharacter *pVoter = GameServer()->GetPlayerChar(VoterId);
	return !pCreator || !pVoter || RaceTeams().m_Core.Team(VoteCreatorId) == RaceTeams().m_Core.Team(VoterId);
}

int CGameControllerDDRace::PlayerVetoActivityStartTick(int ClientId) const
{
	int StartTick = IGameController::PlayerVetoActivityStartTick(ClientId);
	const CCharacter *pCharacter = GameServer()->GetPlayerChar(ClientId);
	if(pCharacter && pCharacter->m_DDRaceState == ERaceState::STARTED)
		StartTick = std::min(StartTick, pCharacter->m_StartTime);
	return StartTick;
}

int CGameControllerDDRace::PlayerTeamGroup(int ClientId) const
{
	return RaceTeams().m_Core.Team(ClientId);
}

bool CGameControllerDDRace::CanPlayerReceivePreInput(int SenderId, int ReceiverId) const
{
	return RaceTeams().m_Core.Team(SenderId) == RaceTeams().m_Core.Team(ReceiverId);
}

CClientMask CGameControllerDDRace::GetMaskForPlayerWorldEvent(int Asker, int ExceptId)
{
	if(Asker == -1)
		return IGameController::GetMaskForPlayerWorldEvent(Asker, ExceptId);
	return RaceTeams().TeamMask(RaceTeams().m_Core.Team(Asker), ExceptId, Asker);
}

void CGameControllerDDRace::OnPlayerShowOthers(int ClientId, int Show)
{
	if(g_Config.m_SvShowOthers && !g_Config.m_SvShowOthersDefault)
		RaceTeams().PlayerState(ClientId).m_ShowOthers = Show;
}

bool CGameControllerDDRace::CanSnapCharacter(CCharacter *pCharacter, int SnappingClient) const
{
	if(SnappingClient == SERVER_DEMO_CLIENT)
		return true;

	CCharacter *pSnappingCharacter = GameServer()->GetPlayerChar(SnappingClient);
	CPlayer *pSnappingPlayer = GameServer()->m_apPlayers[SnappingClient];
	const auto &PlayerState = RaceTeams().PlayerState(SnappingClient);
	if(pSnappingPlayer->GetTeam() == TEAM_SPECTATORS || pSnappingPlayer->IsPaused())
	{
		if(pSnappingPlayer->SpectatorId() != SPEC_FREEVIEW && !pCharacter->CanCollide(pSnappingPlayer->SpectatorId()) && (PlayerState.m_ShowOthers == SHOW_OTHERS_OFF || (PlayerState.m_ShowOthers == SHOW_OTHERS_ONLY_TEAM && !pCharacter->SameTeam(pSnappingPlayer->SpectatorId()))))
			return false;
		if(pSnappingPlayer->SpectatorId() == SPEC_FREEVIEW && !pCharacter->CanCollide(SnappingClient) && PlayerState.m_SpecTeam && !pCharacter->SameTeam(SnappingClient))
			return false;
	}
	else if(pSnappingCharacter && !pSnappingCharacter->Core()->m_Super && !pCharacter->CanCollide(SnappingClient) && (PlayerState.m_ShowOthers == SHOW_OTHERS_OFF || (PlayerState.m_ShowOthers == SHOW_OTHERS_ONLY_TEAM && !pCharacter->SameTeam(SnappingClient))))
		return false;

	return true;
}

void CGameControllerDDRace::SnapCharacterMode(CCharacter *pCharacter, int, int TranslatedId)
{
	pCharacter->SnapDDRace(TranslatedId);
}

void CGameControllerDDRace::SnapPlayerMode(CPlayer *pPlayer, int SnappingClient, int TranslatedId)
{
	if(pPlayer->GetCid() == SnappingClient)
	{
		// Send extended spectator info even when playing, so demos record the local camera settings.
		const int SpectatingClient = ((pPlayer->GetTeam() != TEAM_SPECTATORS && !pPlayer->IsPaused()) || pPlayer->SpectatorId() < 0 || pPlayer->SpectatorId() >= MAX_CLIENTS) ? TranslatedId : pPlayer->SpectatorId();
		const CPlayer *pSpectatedPlayer = GameServer()->m_apPlayers[SpectatingClient];
		if(pSpectatedPlayer)
		{
			CNetObj_DDNetSpectatorInfo DDNetSpectatorInfo = {};
			DDNetSpectatorInfo.m_HasCameraInfo = pSpectatedPlayer->m_CameraInfo.HasInfo();
			DDNetSpectatorInfo.m_Zoom = pSpectatedPlayer->m_CameraInfo.Zoom() * 1000.0f;
			DDNetSpectatorInfo.m_Deadzone = pSpectatedPlayer->m_CameraInfo.Deadzone();
			DDNetSpectatorInfo.m_FollowFactor = pSpectatedPlayer->m_CameraInfo.FollowFactor();

			if(pSpectatedPlayer->m_EnableSpectatorCount && SpectatingClient == TranslatedId && SnappingClient != SERVER_DEMO_CLIENT && pPlayer->GetTeam() != TEAM_SPECTATORS && !pPlayer->IsPaused())
			{
				int SpectatorCount = 0;
				for(const CPlayer *pOtherPlayer : GameServer()->m_apPlayers)
				{
					if(!pOtherPlayer || !pOtherPlayer->m_EnableSpectatorCount || pOtherPlayer->GetCid() == TranslatedId || pOtherPlayer->IsAfk() ||
						(Server()->IsRconAuthed(pOtherPlayer->GetCid()) && Server()->HasAuthHidden(pOtherPlayer->GetCid())) ||
						!(pOtherPlayer->IsPaused() || pOtherPlayer->GetTeam() == TEAM_SPECTATORS))
						continue;

					if(pOtherPlayer->SpectatorId() == TranslatedId)
						SpectatorCount++;
					else if(GameServer()->m_apPlayers[TranslatedId]->GetCharacter())
					{
						const vec2 CheckPos = GameServer()->m_apPlayers[TranslatedId]->GetCharacter()->GetPos();
						const float Dx = pOtherPlayer->m_ViewPos.x - CheckPos.x;
						const float Dy = pOtherPlayer->m_ViewPos.y - CheckPos.y;
						if(absolute(Dx) < pOtherPlayer->m_ShowDistance.x / 2.5f && absolute(Dy) < pOtherPlayer->m_ShowDistance.y / 2.3f)
							SpectatorCount++;
					}
				}
				DDNetSpectatorInfo.m_SpectatorCount = SpectatorCount;
				CNetObj_SpectatorCount SpectatorCountObj = {};
				SpectatorCountObj.m_NumSpectators = SpectatorCount;
				Server()->SnapNewItem(0, SpectatorCountObj);
			}
			Server()->SnapNewItem(TranslatedId, DDNetSpectatorInfo);
		}
	}

	CNetObj_DDNetPlayer DDNetPlayer = {};
	if((SnappingClient >= 0 && Server()->IsRconAuthed(SnappingClient)) || !Server()->HasAuthHidden(pPlayer->GetCid()))
		DDNetPlayer.m_AuthLevel = Server()->GetAuthedState(pPlayer->GetCid());
	else
		DDNetPlayer.m_AuthLevel = AUTHED_NO;

	if(pPlayer->IsAfk())
		DDNetPlayer.m_Flags |= EXPLAYERFLAG_AFK;
	if(pPlayer->IsPaused() == CPlayer::PAUSE_SPEC)
		DDNetPlayer.m_Flags |= EXPLAYERFLAG_SPEC;
	if(pPlayer->IsPaused() == CPlayer::PAUSE_PAUSED)
		DDNetPlayer.m_Flags |= EXPLAYERFLAG_PAUSED;

	const CFinishTime PlayerTime = SnapPlayerTime(SnappingClient, pPlayer);
	DDNetPlayer.m_FinishTimeSeconds = PlayerTime.m_Seconds;
	DDNetPlayer.m_FinishTimeMillis = PlayerTime.m_Milliseconds;
	Server()->SnapNewItem(TranslatedId, DDNetPlayer);

	CCharacter *pCharacter = pPlayer->GetCharacter();
	if(Server()->IsSixup(SnappingClient) && pCharacter && pCharacter->m_DDRaceState == ERaceState::STARTED && GameServer()->m_apPlayers[SnappingClient]->m_TimerType == CPlayer::TIMERTYPE_SIXUP)
	{
		protocol7::CNetObj_PlayerInfoRace RaceInfo = {};
		RaceInfo.m_RaceStartTick = pCharacter->m_StartTime;
		Server()->SnapNewItem(TranslatedId, RaceInfo);
	}

	bool ShowSpec = pCharacter && pCharacter->IsPaused() && pCharacter->CanSnapCharacter(SnappingClient);
	if(SnappingClient != SERVER_DEMO_CLIENT)
	{
		CPlayer *pSnappingPlayer = GameServer()->m_apPlayers[SnappingClient];
		ShowSpec = ShowSpec && (GameServer()->GetDDRaceTeam(pPlayer->GetCid()) == GameServer()->GetDDRaceTeam(SnappingClient) || RaceTeams().PlayerState(SnappingClient).m_ShowOthers == SHOW_OTHERS_ON || pSnappingPlayer->GetTeam() == TEAM_SPECTATORS || pSnappingPlayer->IsPaused());
	}
	if(ShowSpec)
	{
		CNetObj_SpecChar SpecChar = {};
		SpecChar.m_X = pCharacter->Core()->m_Pos.x;
		SpecChar.m_Y = pCharacter->Core()->m_Pos.y;
		Server()->SnapNewItem(TranslatedId, SpecChar);
	}
}
