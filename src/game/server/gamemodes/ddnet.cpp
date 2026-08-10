/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
/* Based on Race mod stuff and tweaked by GreYFoX@GTi and others to fit our DDRace needs. */
#include "ddnet.h"

#include <base/time.h>

#include <engine/server.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>
#include <engine/shared/protocol7.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>
#include <game/server/score.h>
#include <game/server/teams.h>
#include <game/version.h>

#include <algorithm>

CGameControllerDDNet::CGameControllerDDNet(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	CGameControllerDDRace(Services, GameModeInfo)
{
}

CGameControllerDDNet::~CGameControllerDDNet() = default;

CTuningParams CGameControllerDDNet::DefaultTuning()
{
	CTuningParams Tuning = CTuningParams::DEFAULT;
	Tuning.Set("gun_speed", 1400);
	Tuning.Set("gun_curvature", 0);
	Tuning.Set("shotgun_speed", 500);
	Tuning.Set("shotgun_speeddiff", 0);
	Tuning.Set("shotgun_curvature", 0);
	return Tuning;
}

void CGameControllerDDNet::ResetTuning()
{
	*GameServer()->GlobalTuning() = DefaultTuning();
	GameServer()->SendTuningParams(-1);
}

void CGameControllerDDNet::InitGameSettings()
{
	const CTuningParams Tuning = DefaultTuning();
	for(int i = 0; i < TuneZone::NUM; i++)
	{
		GameServer()->TuningList()[i] = Tuning;
		GameServer()->m_aaZoneEnterMsg[i][0] = 0;
		GameServer()->m_aaZoneLeaveMsg[i][0] = 0;
	}
	if(g_Config.m_SvTuneReset)
		ResetTuning();

	if(g_Config.m_SvDDRaceTuneReset)
	{
		g_Config.m_SvHit = 1;
		g_Config.m_SvEndlessDrag = 0;
		g_Config.m_SvOldLaser = 0;
		g_Config.m_SvOldTeleportHook = 0;
		g_Config.m_SvOldTeleportWeapons = 0;
		g_Config.m_SvTeleportHoldHook = 0;
		g_Config.m_SvTeam = SV_TEAM_ALLOWED;
		g_Config.m_SvShowOthersDefault = SHOW_OTHERS_OFF;

		for(auto &Switcher : GameServer()->Switchers())
			Switcher.m_Initial = true;
	}

	LoadGameSettings();

	if(g_Config.m_SvSoloServer)
	{
		g_Config.m_SvTeam = SV_TEAM_FORCED_SOLO;
		g_Config.m_SvShowOthersDefault = SHOW_OTHERS_ON;

		GameServer()->GlobalTuning()->Set("player_collision", 0);
		GameServer()->GlobalTuning()->Set("player_hooking", 0);

		for(int i = 0; i < TuneZone::NUM; i++)
		{
			GameServer()->TuningList()[i].Set("player_collision", 0);
			GameServer()->TuningList()[i].Set("player_hooking", 0);
		}
	}
}

void CGameControllerDDNet::UpdateGameInfo(CNetObj_GameInfo &GameInfo, int SnappingClient)
{
	CPlayer *pPlayer = SnappingClient != SERVER_DEMO_CLIENT ? GameServer()->m_apPlayers[SnappingClient] : nullptr;
	if(!pPlayer || (pPlayer->m_TimerType != CPlayer::TIMERTYPE_GAMETIMER && pPlayer->m_TimerType != CPlayer::TIMERTYPE_GAMETIMER_AND_BROADCAST) || pPlayer->GetClientVersion() < VERSION_DDNET_GAMETICK)
		return;

	CCharacter *pChr = nullptr;
	if((pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused()) && pPlayer->SpectatorId() != SPEC_FREEVIEW)
	{
		CPlayer *pSpectatedPlayer = GameServer()->m_apPlayers[pPlayer->SpectatorId()];
		if(pSpectatedPlayer)
			pChr = pSpectatedPlayer->GetCharacter();
	}
	else
	{
		pChr = pPlayer->GetCharacter();
	}

	if(pChr && pChr->m_DDRaceState == ERaceState::STARTED)
	{
		GameInfo.m_WarmupTimer = -pChr->m_StartTime;
		GameInfo.m_GameStateFlags |= GAMESTATEFLAG_RACETIME;
	}
}

int CGameControllerDDNet::GameInfoFlags(int SnappingClient) const
{
	return GAMEINFOFLAG_TIMESCORE |
	       GAMEINFOFLAG_GAMETYPE_RACE |
	       GAMEINFOFLAG_GAMETYPE_DDRACE |
	       GAMEINFOFLAG_GAMETYPE_DDNET |
	       GAMEINFOFLAG_UNLIMITED_AMMO |
	       GAMEINFOFLAG_RACE_RECORD_MESSAGE |
	       GAMEINFOFLAG_ALLOW_EYE_WHEEL |
	       GAMEINFOFLAG_ALLOW_HOOK_COLL |
	       GAMEINFOFLAG_ALLOW_ZOOM |
	       GAMEINFOFLAG_BUG_DDRACE_GHOST |
	       GAMEINFOFLAG_BUG_DDRACE_INPUT |
	       GAMEINFOFLAG_PREDICT_DDRACE |
	       GAMEINFOFLAG_PREDICT_DDRACE_TILES |
	       GAMEINFOFLAG_ENTITIES_DDNET |
	       GAMEINFOFLAG_ENTITIES_DDRACE |
	       GAMEINFOFLAG_ENTITIES_RACE |
	       GAMEINFOFLAG_RACE;
}

int CGameControllerDDNet::GameInfoFlags2(int SnappingClient) const
{
	int Flags = GAMEINFOFLAG2_HUD_DDRACE | GAMEINFOFLAG2_DDRACE_TEAM | GAMEINFOFLAG2_PREDICT_EVENTS | GAMEINFOFLAG2_SUPPORTS_128_TEAMS;
	if(g_Config.m_SvNoWeakHook)
		Flags |= GAMEINFOFLAG2_NO_WEAK_HOOK;
	return Flags;
}

void CGameControllerDDNet::SnapMode(int SnappingClient)
{
	if(Server()->IsSixup(SnappingClient))
	{
		protocol7::CNetObj_GameDataRace RaceData = {};
		CFinishTime MapTime = SnapMapBestTime(SnappingClient);
		const int BestTime = MapTime.m_Seconds > 0 ? MapTime.m_Seconds * 1000 + MapTime.m_Milliseconds : -1;

		RaceData.m_BestTime = BestTime;
		RaceData.m_Precision = 2;
		RaceData.m_RaceFlags = protocol7::RACEFLAG_KEEP_WANTED_WEAPON;
		Server()->SnapNewItem(0, RaceData);
	}

	GameServer()->SnapSwitchers(SnappingClient);

	if(!Server()->IsSixup(SnappingClient))
	{
		CFinishTime MapTime = SnapMapBestTime(SnappingClient);
		if(MapTime.m_Seconds != FinishTime::UNSET)
		{
			CNetObj_MapBestTime MapBestTime = {};
			MapBestTime.m_MapBestTimeSeconds = MapTime.m_Seconds;
			MapBestTime.m_MapBestTimeMillis = MapTime.m_Milliseconds;
			Server()->SnapNewItem(0, MapBestTime);
		}
	}
}

CScore *CGameControllerDDNet::RaceScore()
{
	return GameServer()->RaceScore();
}

void CGameControllerDDNet::HandleCharacterTiles(CCharacter *pChr, int MapIndex)
{
	CPlayer *pPlayer = pChr->GetPlayer();
	const int ClientId = pPlayer->GetCid();

	int TileIndex = GameServer()->Collision()->GetTileIndex(MapIndex);
	int TileFIndex = GameServer()->Collision()->GetFrontTileIndex(MapIndex);

	//Sensitivity
	int S1 = GameServer()->Collision()->GetPureMapIndex(vec2(pChr->GetPos().x + pChr->GetProximityRadius() / 3.f, pChr->GetPos().y - pChr->GetProximityRadius() / 3.f));
	int S2 = GameServer()->Collision()->GetPureMapIndex(vec2(pChr->GetPos().x + pChr->GetProximityRadius() / 3.f, pChr->GetPos().y + pChr->GetProximityRadius() / 3.f));
	int S3 = GameServer()->Collision()->GetPureMapIndex(vec2(pChr->GetPos().x - pChr->GetProximityRadius() / 3.f, pChr->GetPos().y - pChr->GetProximityRadius() / 3.f));
	int S4 = GameServer()->Collision()->GetPureMapIndex(vec2(pChr->GetPos().x - pChr->GetProximityRadius() / 3.f, pChr->GetPos().y + pChr->GetProximityRadius() / 3.f));
	int Tile1 = GameServer()->Collision()->GetTileIndex(S1);
	int Tile2 = GameServer()->Collision()->GetTileIndex(S2);
	int Tile3 = GameServer()->Collision()->GetTileIndex(S3);
	int Tile4 = GameServer()->Collision()->GetTileIndex(S4);
	int FTile1 = GameServer()->Collision()->GetFrontTileIndex(S1);
	int FTile2 = GameServer()->Collision()->GetFrontTileIndex(S2);
	int FTile3 = GameServer()->Collision()->GetFrontTileIndex(S3);
	int FTile4 = GameServer()->Collision()->GetFrontTileIndex(S4);

	const ERaceState PlayerDDRaceState = pChr->m_DDRaceState;
	bool IsOnStartTile = (TileIndex == TILE_START) || (TileFIndex == TILE_START) || FTile1 == TILE_START || FTile2 == TILE_START || FTile3 == TILE_START || FTile4 == TILE_START || Tile1 == TILE_START || Tile2 == TILE_START || Tile3 == TILE_START || Tile4 == TILE_START;
	// start
	if(IsOnStartTile && PlayerDDRaceState != ERaceState::CHEATED)
	{
		const int Team = GameServer()->GetDDRaceTeam(ClientId);
		if(RaceTeams().GetSaving(Team))
		{
			GameServer()->SendStartWarning(ClientId, "You can't start while loading/saving of team is in progress");
			pChr->Die(ClientId, WEAPON_WORLD);
			return;
		}
		if(g_Config.m_SvTeam == SV_TEAM_MANDATORY && (Team == TEAM_FLOCK || RaceTeams().TeamSize(Team) <= 1))
		{
			GameServer()->SendStartWarning(ClientId, "You have to be in a team with other tees to start");
			pChr->Die(ClientId, WEAPON_WORLD);
			return;
		}
		if(g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO && Team != TEAM_FLOCK && RaceTeams().IsValidTeamNumber(Team) && RaceTeams().TeamSize(Team) < g_Config.m_SvMinTeamSize && !RaceTeams().TeamFlock(Team))
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "Your team has fewer than %d players, so your team rank won't count", g_Config.m_SvMinTeamSize);
			GameServer()->SendStartWarning(ClientId, aBuf);
		}
		if(g_Config.m_SvResetPickups)
		{
			pChr->ResetPickups();
		}

		RaceTeams().OnCharacterStart(ClientId);
		pChr->m_LastTimeCp = -1;
		pChr->m_LastTimeCpBroadcasted = -1;
		for(float &CurrentTimeCp : pChr->m_aCurrentTimeCp)
		{
			CurrentTimeCp = 0.0f;
		}
	}

	// finish
	if(((TileIndex == TILE_FINISH) || (TileFIndex == TILE_FINISH) || FTile1 == TILE_FINISH || FTile2 == TILE_FINISH || FTile3 == TILE_FINISH || FTile4 == TILE_FINISH || Tile1 == TILE_FINISH || Tile2 == TILE_FINISH || Tile3 == TILE_FINISH || Tile4 == TILE_FINISH) && PlayerDDRaceState == ERaceState::STARTED)
		RaceTeams().OnCharacterFinish(ClientId);

	// unlock team
	else if(((TileIndex == TILE_UNLOCK_TEAM) || (TileFIndex == TILE_UNLOCK_TEAM)) && RaceTeams().TeamLocked(GameServer()->GetDDRaceTeam(ClientId)))
	{
		RaceTeams().SetTeamLock(GameServer()->GetDDRaceTeam(ClientId), false);
		GameServer()->SendChatTeam(GameServer()->GetDDRaceTeam(ClientId), "Your team was unlocked by an unlock team tile");
	}

	// solo part
	if(((TileIndex == TILE_SOLO_ENABLE) || (TileFIndex == TILE_SOLO_ENABLE)) && !TeamsCore().GetSolo(ClientId))
	{
		GameServer()->SendChatTarget(ClientId, "You are now in a solo part");
		pChr->SetSolo(true);
	}
	else if(((TileIndex == TILE_SOLO_DISABLE) || (TileFIndex == TILE_SOLO_DISABLE)) && TeamsCore().GetSolo(ClientId))
	{
		GameServer()->SendChatTarget(ClientId, "You are now out of the solo part");
		pChr->SetSolo(false);
	}
}

void CGameControllerDDNet::SetArmorProgress(CCharacter *pCharacter, int Progress)
{
	pCharacter->SetArmor(std::clamp(10 - (Progress / 15), 0, 10));
}

int CGameControllerDDNet::SnapPlayerScore(int SnappingClient, CPlayer *pPlayer)
{
	bool HideScore = g_Config.m_SvHideScore && SnappingClient != pPlayer->GetCid();
	std::optional<float> Score = GameServer()->RaceScore()->PlayerData(pPlayer->GetCid())->m_BestTime;

	if(Server()->IsSixup(SnappingClient))
	{
		if(!Score.has_value() || HideScore)
			return protocol7::FinishTime::NOT_FINISHED;

		// Times are in milliseconds for 0.7
		return Score.value() * 1000.0f;
	}

	// This is the time sent to the player while ingame (do not confuse to the one reported to the master server).
	// Due to clients expecting this as a negative value, we have to make sure it's negative.
	// Special numbers:
	// -9999 or FinishTime::NOT_FINISHED_TIMESCORE: means no time and isn't displayed in the scoreboard.
	if(!Score.has_value() || HideScore)
		return FinishTime::NOT_FINISHED_TIMESCORE;

	// Times are in seconds for 0.6
	int ScoreSeconds = Score.value();

	// shift the time by a second if the player actually took 9999
	// seconds to finish the map.
	if(-ScoreSeconds == FinishTime::NOT_FINISHED_TIMESCORE)
		return -ScoreSeconds - 1;
	return -ScoreSeconds;
}

IGameController::CFinishTime CGameControllerDDNet::SnapPlayerTime(int SnappingClient, CPlayer *pPlayer)
{
	std::optional<float> BestTime = GameServer()->RaceScore()->PlayerData(pPlayer->GetCid())->m_BestTime;
	if(BestTime.has_value() && (!g_Config.m_SvHideScore || SnappingClient == pPlayer->GetCid()))
	{
		// same as in str_time_float
		int64_t TimeMilliseconds = time_milliseconds_from_seconds(BestTime.value());
		int Seconds = static_cast<int>(TimeMilliseconds / 1000);
		int Millis = static_cast<int>(TimeMilliseconds % 1000);
		return CFinishTime(Seconds, Millis);
	}
	return CFinishTime::NotFinished();
}

IGameController::CFinishTime CGameControllerDDNet::SnapMapBestTime(int SnappingClient)
{
	const std::optional<float> &CurrentRecord = RaceScore()->CurrentRecord();
	if(CurrentRecord.has_value() && !g_Config.m_SvHideScore)
	{
		// same as in str_time_float
		int64_t TimeMilliseconds = time_milliseconds_from_seconds(CurrentRecord.value());
		int Seconds = static_cast<int>(TimeMilliseconds / 1000);
		int Millis = static_cast<int>(TimeMilliseconds % 1000);
		return CFinishTime(Seconds, Millis);
	}
	return CFinishTime::NotFinished();
}

void CGameControllerDDNet::OnPlayerConnect(CPlayer *pPlayer)
{
	CGameControllerDDRace::OnPlayerConnect(pPlayer);
	int ClientId = pPlayer->GetCid();

	// Can't set score here as LoadScore() is threaded, run it in
	// LoadScoreThreaded() instead
	RaceScore()->LoadPlayerData(ClientId);

	if(!Server()->ClientPrevIngame(ClientId))
	{
		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' entered and joined the %s", Server()->ClientName(ClientId), GetTeamName(pPlayer->GetTeam()));
		GameServer()->SendChat(-1, TEAM_ALL, aBuf, -1);

		GameServer()->SendChatTarget(ClientId, "DDraceNetwork Mod. Version: " GAME_VERSION);
		GameServer()->SendChatTarget(ClientId, "please visit DDNet.org or say /info and make sure to read our /rules");
	}
}

void CGameControllerDDNet::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	int ClientId = pPlayer->GetCid();
	bool WasModerator = pPlayer->m_Moderating && Server()->ClientIngame(ClientId);

	CGameControllerDDRace::OnPlayerDisconnect(pPlayer, pReason);

	if(!GameServer()->PlayerModerating() && WasModerator)
		GameServer()->SendChat(-1, TEAM_ALL, "Server kick/spec votes are no longer actively moderated.");

	if(g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO)
		RaceTeams().SetForceCharacterTeam(ClientId, TEAM_FLOCK);

	for(int Team = TEAM_FLOCK + 1; Team < TEAM_SUPER; Team++)
		if(RaceTeams().IsInvited(Team, ClientId))
			RaceTeams().SetClientInvited(Team, ClientId, false);
}

void CGameControllerDDNet::OnReset()
{
	IGameController::OnReset();
	RaceTeams().Reset();
}

void CGameControllerDDNet::Tick()
{
	IGameController::Tick();
	RaceTeams().ProcessSaveTeam();
	RaceTeams().Tick();
}

void CGameControllerDDNet::DoTeamChange(class CPlayer *pPlayer, int Team, bool DoChatMsg)
{
	if(!IsValidTeam(Team))
		return;

	if(Team == pPlayer->GetTeam())
		return;

	CCharacter *pCharacter = pPlayer->GetCharacter();

	if(Team == TEAM_SPECTATORS)
	{
		if(g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO && pCharacter)
		{
			RaceTeams().OnCharacterDeath(pPlayer->GetCid(), WEAPON_GAME);
			// Joining spectators should not kill a locked team, but should still
			// check if the team finished by you leaving it.
			int DDRTeam = pCharacter->Team();
			RaceTeams().SetForceCharacterTeam(pPlayer->GetCid(), TEAM_FLOCK);
			RaceTeams().CheckTeamFinished(DDRTeam);
		}
	}

	IGameController::DoTeamChange(pPlayer, Team, /* Suppress chat message */ false);
}
