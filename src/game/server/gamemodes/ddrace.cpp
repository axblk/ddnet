#include "ddrace.h"

#include "ddrace_map_entities.h"

#include <base/dbg.h>
#include <base/log.h>
#include <base/math.h>
#include <base/net.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/server.h>
#include <engine/shared/config.h>

#include <generated/protocol7.h>

#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/interactions.h>
#include <game/server/player.h>
#include <game/server/save.h>
#include <game/server/score.h>
#include <game/server/teams.h>

#include <algorithm>
#include <array>
#include <memory>

namespace
{
	CGameControllerDDRace &RaceController(CGameContext *pGameServer)
	{
		return *static_cast<CGameControllerDDRace *>(pGameServer->GameHost().Controller());
	}

	CGameTeams *RaceTeams(CGameContext *pGameServer)
	{
		return &RaceController(pGameServer).RaceTeams();
	}

	CScore *RaceScore(CGameContext *pGameServer)
	{
		return &RaceController(pGameServer).RaceScore();
	}

	CCharacterDDRace *DDRaceCharacter(CCharacter *pCharacter)
	{
		// The covariant factory return type requires all DDRace-derived modes to keep this invariant.
		return static_cast<CCharacterDDRace *>(pCharacter);
	}

	class CDDRaceMapReloadState final : public IGameModeMapReloadState
	{
		std::array<std::unique_ptr<CSaveTeam>, MAX_CLIENTS> m_apSavedTeams;
		std::array<std::unique_ptr<CSaveHotReloadTee>, MAX_CLIENTS> m_apSavedTees;
		std::array<int, MAX_CLIENTS> m_aTeamMapping;

	public:
		explicit CDDRaceMapReloadState(CGameContext *pGameServer)
		{
			m_aTeamMapping.fill(-1);
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
			{
				CCharacterDDRace *pCharacter = DDRaceCharacter(pGameServer->GetPlayerChar(ClientId));
				if(!pCharacter)
					continue;

				m_apSavedTees[ClientId] = std::make_unique<CSaveHotReloadTee>();
				m_apSavedTees[ClientId]->Save(pCharacter, false);

				int Team = RaceTeams(pGameServer)->m_Core.Team(ClientId);
				if(Team == TEAM_SUPER)
					Team = pCharacter->TeamBeforeSuper();
				m_aTeamMapping[ClientId] = Team;

				if(!m_apSavedTeams[Team])
				{
					m_apSavedTeams[Team] = std::make_unique<CSaveTeam>();
					m_apSavedTeams[Team]->Save(pGameServer, RaceTeams(pGameServer), Team, true, true);
				}
			}
		}

		void RestoreCharacter(CGameContext *pGameServer, CCharacterDDRace *pCharacter)
		{
			const int ClientId = pCharacter->GetPlayer()->GetCid();
			const int Team = m_aTeamMapping[ClientId];
			if(Team == -1)
				return;

			RaceTeams(pGameServer)->SetForceCharacterTeam(ClientId, Team);
			m_aTeamMapping[ClientId] = -1;

			if(m_apSavedTeams[Team])
			{
				m_apSavedTeams[Team]->Load(pGameServer, RaceTeams(pGameServer), Team, true, true);
				m_apSavedTeams[Team].reset();
			}

			if(m_apSavedTees[ClientId])
			{
				m_apSavedTees[ClientId]->Load(pCharacter, Team);
				m_apSavedTees[ClientId].reset();
			}
		}

		void DiscardClient(int ClientId) override
		{
			const int Team = m_aTeamMapping[ClientId];
			m_aTeamMapping[ClientId] = -1;
			m_apSavedTees[ClientId].reset();

			if(Team < 0 || Team >= MAX_CLIENTS)
				return;
			for(const int MappedTeam : m_aTeamMapping)
			{
				if(MappedTeam == Team)
					return;
			}
			m_apSavedTeams[Team].reset();
		}
	};

	struct CCommandRegistration
	{
		const char *m_pName;
		const char *m_pParams;
		int m_Flags;
		IConsole::FCommandCallback m_pfnCallback;
		const char *m_pHelp;
	};

	void ConTeamTop5(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvHideScore)
		{
			log_info("chatresp", "Showing the team top 5 is not allowed on this server.");
			return;
		}

		if(pResult->NumArguments() == 0)
		{
			RaceScore(pGameServer)->ShowTeamTop5(pResult->m_ClientId, 1);
		}
		else if(pResult->NumArguments() == 1)
		{
			if(pResult->GetInteger(0) != 0)
			{
				RaceScore(pGameServer)->ShowTeamTop5(pResult->m_ClientId, pResult->GetInteger(0));
			}
			else
			{
				const char *pRequestedName = str_comp_nocase(pResult->GetString(0), "me") == 0 ?
					pGameServer->Server()->ClientName(pResult->m_ClientId) :
					pResult->GetString(0);
				RaceScore(pGameServer)->ShowPlayerTeamTop5(pResult->m_ClientId, pRequestedName, 0);
			}
		}
		else if(pResult->NumArguments() == 2 && pResult->GetInteger(1) != 0)
		{
			const char *pRequestedName = str_comp_nocase(pResult->GetString(0), "me") == 0 ?
				pGameServer->Server()->ClientName(pResult->m_ClientId) :
				pResult->GetString(0);
			RaceScore(pGameServer)->ShowPlayerTeamTop5(pResult->m_ClientId, pRequestedName, pResult->GetInteger(1));
		}
		else
		{
			log_info("chatresp", "/top5team needs 0, 1 or 2 parameter. 1. = name, 2. = start number");
			log_info("chatresp", "Example: /top5team, /top5team me, /top5team Hans, /top5team \"Papa Smurf\" 5");
			log_info("chatresp", "Bad: /top5team Papa Smurf 5 # Good: /top5team \"Papa Smurf\" 5 ");
		}
	}

	void ConTop(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvHideScore)
		{
			log_info("chatresp", "Showing the top is not allowed on this server.");
			return;
		}

		if(pResult->NumArguments() > 0)
			RaceScore(pGameServer)->ShowTop(pResult->m_ClientId, pResult->GetInteger(0));
		else
			RaceScore(pGameServer)->ShowTop(pResult->m_ClientId);
	}

	void ConTimes(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		int Offset = 1;
		const char *pRequestedName = nullptr;

		if(pResult->NumArguments() == 1)
		{
			if(pResult->GetInteger(0) != 0)
				Offset = pResult->GetInteger(0);
			else
				pRequestedName = pResult->GetString(0);
		}
		else if(pResult->NumArguments() == 2)
		{
			pRequestedName = pResult->GetString(0);
			Offset = pResult->GetInteger(1);
		}
		else if(pResult->NumArguments() > 2)
		{
			log_info("chatresp", "/times needs 0, 1 or 2 parameter. 1. = name, 2. = start number");
			log_info("chatresp", "Example: /times, /times me, /times Hans, /times \"Papa Smurf\" 5");
			log_info("chatresp", "Bad: /times Papa Smurf 5 # Good: /times \"Papa Smurf\" 5 ");
			return;
		}

		if(g_Config.m_SvHideScore)
		{
			if(pRequestedName && str_comp_nocase(pRequestedName, "me") != 0 && str_comp_nocase(pRequestedName, pGameServer->Server()->ClientName(pResult->m_ClientId)) != 0)
			{
				log_info("chatresp", "Showing the times of others is not allowed on this server.");
				return;
			}
			pRequestedName = pGameServer->Server()->ClientName(pResult->m_ClientId);
			RaceScore(pGameServer)->ShowTimes(pResult->m_ClientId, pRequestedName, Offset);
		}
		else if(!pRequestedName)
		{
			RaceScore(pGameServer)->ShowTimes(pResult->m_ClientId, Offset);
		}
		else
		{
			if(str_comp_nocase(pRequestedName, "me") == 0)
				pRequestedName = pGameServer->Server()->ClientName(pResult->m_ClientId);
			RaceScore(pGameServer)->ShowTimes(pResult->m_ClientId, pRequestedName, Offset);
		}
	}

	void ConTeamRank(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(pResult->NumArguments() > 0)
		{
			if(!g_Config.m_SvHideScore)
				RaceScore(pGameServer)->ShowTeamRank(pResult->m_ClientId, pResult->GetString(0));
			else
				log_info("chatresp", "Showing the team rank of other players is not allowed on this server.");
		}
		else
		{
			RaceScore(pGameServer)->ShowTeamRank(pResult->m_ClientId, pGameServer->Server()->ClientName(pResult->m_ClientId));
		}
	}

	void ConRank(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(pResult->NumArguments() > 0)
		{
			if(!g_Config.m_SvHideScore)
				RaceScore(pGameServer)->ShowRank(pResult->m_ClientId, pResult->GetString(0));
			else
				log_info("chatresp", "Showing the rank of other players is not allowed on this server.");
		}
		else
		{
			RaceScore(pGameServer)->ShowRank(pResult->m_ClientId, pGameServer->Server()->ClientName(pResult->m_ClientId));
		}
	}

	void ConPoints(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(pResult->NumArguments() > 0)
		{
			if(!g_Config.m_SvHideScore)
				RaceScore(pGameServer)->ShowPoints(pResult->m_ClientId, pResult->GetString(0));
			else
				log_info("chatresp", "Showing the global points of other players is not allowed on this server.");
		}
		else
		{
			RaceScore(pGameServer)->ShowPoints(pResult->m_ClientId, pGameServer->Server()->ClientName(pResult->m_ClientId));
		}
	}

	void ConTopPoints(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvHideScore)
		{
			log_info("chatresp", "Showing the global top points is not allowed on this server.");
			return;
		}

		if(pResult->NumArguments() > 0)
			RaceScore(pGameServer)->ShowTopPoints(pResult->m_ClientId, pResult->GetInteger(0));
		else
			RaceScore(pGameServer)->ShowTopPoints(pResult->m_ClientId);
	}

	void ConTimeCp(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvHideScore)
		{
			log_info("chatresp", "Showing the checkpoint times is not allowed on this server.");
			return;
		}

		if(!pGameServer->m_apPlayers[pResult->m_ClientId])
			return;

		RaceScore(pGameServer)->LoadPlayerTimeCp(pResult->m_ClientId, pResult->GetString(0));
	}

	void ConSettings(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);

		if(pResult->NumArguments() == 0)
		{
			log_info("chatresp", "to check a server setting say /settings and setting's name, setting names are:");
			log_info("chatresp", "teams, cheats, collision, hooking, endlesshooking,");
			log_info("chatresp", "hitting, oldlaser, timeout, votes, pause and scores");
			return;
		}

		const char *pArg = pResult->GetString(0);
		char aBuf[256];
		float ColTemp;
		float HookTemp;
		pGameServer->GlobalTuning()->Get("player_collision", &ColTemp);
		pGameServer->GlobalTuning()->Get("player_hooking", &HookTemp);
		if(str_comp_nocase(pArg, "teams") == 0)
		{
			str_format(aBuf, sizeof(aBuf), "%s %s",
				g_Config.m_SvTeam == SV_TEAM_ALLOWED ?
					"Teams are available on this server" :
				(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO) ?
					"Teams are not available on this server" :
					"You have to be in a team to play on this server",
				"and all of your team will die if the team is locked");
			log_info("chatresp", "%s", aBuf);
		}
		else if(str_comp_nocase(pArg, "cheats") == 0)
		{
			log_info("chatresp", g_Config.m_SvTestingCommands ? "Cheats are enabled on this server" : "Cheats are disabled on this server");
		}
		else if(str_comp_nocase(pArg, "collision") == 0)
		{
			log_info("chatresp", ColTemp ? "Players can collide on this server" : "Players can't collide on this server");
		}
		else if(str_comp_nocase(pArg, "hooking") == 0)
		{
			log_info("chatresp", HookTemp ? "Players can hook each other on this server" : "Players can't hook each other on this server");
		}
		else if(str_comp_nocase(pArg, "endlesshooking") == 0)
		{
			log_info("chatresp", g_Config.m_SvEndlessDrag ? "Players hook time is unlimited" : "Players hook time is limited");
		}
		else if(str_comp_nocase(pArg, "hitting") == 0)
		{
			log_info("chatresp", g_Config.m_SvHit ? "Players weapons affect others" : "Players weapons has no affect on others");
		}
		else if(str_comp_nocase(pArg, "oldlaser") == 0)
		{
			log_info("chatresp", g_Config.m_SvOldLaser ?
						     "Lasers can hit you if you shot them and they pull you towards the bounce origin (Like DDRace Beta)" :
						     "Lasers can't hit you if you shot them, and they pull others towards the shooter");
		}
		else if(str_comp_nocase(pArg, "timeout") == 0)
		{
			str_format(aBuf, sizeof(aBuf), "The Server Timeout is currently set to %d seconds", g_Config.m_ConnTimeout);
			log_info("chatresp", "%s", aBuf);
		}
		else if(str_comp_nocase(pArg, "votes") == 0)
		{
			log_info("chatresp", g_Config.m_SvVoteKick ? "Players can use Callvote menu tab to kick offenders" : "Players can't use the Callvote menu tab to kick offenders");
			if(g_Config.m_SvVoteKick)
			{
				str_format(aBuf, sizeof(aBuf), "Players are banned for %d minute(s) if they get voted off", g_Config.m_SvVoteKickBantime);
				log_info("chatresp", "%s", g_Config.m_SvVoteKickBantime ? aBuf : "Players are just kicked and not banned if they get voted off");
			}
		}
		else if(str_comp_nocase(pArg, "pause") == 0)
		{
			log_info("chatresp", g_Config.m_SvPauseable ? "/spec will pause you and your tee will vanish" : "/spec will pause you but your tee will not vanish");
		}
		else if(str_comp_nocase(pArg, "scores") == 0)
		{
			log_info("chatresp", g_Config.m_SvHideScore ? "Scores are private on this server" : "Scores are public on this server");
		}
		else
		{
			log_info("chatresp", "no matching settings found, type /settings to view them");
		}
	}

	void ToggleSpecPause(IConsole::IResult *pResult, void *pUserData, int PauseType)
	{
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		IServer *pServer = pGameServer->Server();
		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		int PauseState = pPlayer->IsPaused();
		if(PauseState > 0)
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "You are force-paused for %d seconds.", (PauseState - pServer->Tick()) / pServer->TickSpeed());
			log_info("chatresp", "%s", aBuf);
		}
		else if(pResult->NumArguments() > 0)
		{
			if(-PauseState == PauseType && pPlayer->SpectatorId() != pResult->m_ClientId && pServer->ClientIngame(pPlayer->SpectatorId()) && !str_comp(pServer->ClientName(pPlayer->SpectatorId()), pResult->GetString(0)))
			{
				pPlayer->Pause(CPlayer::PAUSE_NONE, false);
			}
			else
			{
				pPlayer->Pause(PauseType, false);
				pPlayer->SpectatePlayerName(pResult->GetString(0));
			}
		}
		else if(-PauseState != CPlayer::PAUSE_NONE && PauseType != CPlayer::PAUSE_NONE)
		{
			pPlayer->Pause(CPlayer::PAUSE_NONE, false);
		}
		else if(-PauseState != PauseType)
		{
			pPlayer->Pause(PauseType, false);
		}
	}

	void ToggleSpecPauseVoted(IConsole::IResult *pResult, void *pUserData, int PauseType)
	{
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		int PauseState = pPlayer->IsPaused();
		if(PauseState > 0)
		{
			IServer *pServer = pGameServer->Server();
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), "You are force-paused for %d seconds.", (PauseState - pServer->Tick()) / pServer->TickSpeed());
			log_info("chatresp", "%s", aBuf);
			return;
		}

		bool IsPlayerBeingVoted = pGameServer->m_VoteCloseTime &&
			(pGameServer->IsKickVote() || pGameServer->IsSpecVote()) &&
			pResult->m_ClientId != pGameServer->m_VoteVictim;
		if((!IsPlayerBeingVoted && -PauseState == PauseType) ||
			(IsPlayerBeingVoted && PauseState && pPlayer->SpectatorId() == pGameServer->m_VoteVictim))
		{
			pPlayer->Pause(CPlayer::PAUSE_NONE, false);
		}
		else
		{
			pPlayer->Pause(PauseType, false);
			if(IsPlayerBeingVoted)
				pPlayer->SetSpectatorId(pGameServer->m_VoteVictim);
		}
	}

	void ConToggleSpec(IConsole::IResult *pResult, void *pUserData)
	{
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		int PauseType = g_Config.m_SvPauseable ? CPlayer::PAUSE_SPEC : CPlayer::PAUSE_PAUSED;
		if(pPlayer->GetCharacter())
		{
			CGameTeams &Teams = *RaceTeams(pGameServer);
			if(Teams.IsPractice(Teams.m_Core.Team(pResult->m_ClientId)))
				PauseType = CPlayer::PAUSE_SPEC;
		}

		ToggleSpecPause(pResult, pUserData, PauseType);
	}

	void ConToggleSpecVoted(IConsole::IResult *pResult, void *pUserData)
	{
		ToggleSpecPauseVoted(pResult, pUserData, g_Config.m_SvPauseable ? CPlayer::PAUSE_SPEC : CPlayer::PAUSE_PAUSED);
	}

	void ConTogglePause(IConsole::IResult *pResult, void *pUserData)
	{
		ToggleSpecPause(pResult, pUserData, CPlayer::PAUSE_PAUSED);
	}

	void ConTogglePauseVoted(IConsole::IResult *pResult, void *pUserData)
	{
		ToggleSpecPauseVoted(pResult, pUserData, CPlayer::PAUSE_PAUSED);
	}

	void ConNinjaJetpack(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;
		if(pResult->NumArguments())
			pPlayer->m_NinjaJetpack = pResult->GetInteger(0);
		else
			pPlayer->m_NinjaJetpack = !pPlayer->m_NinjaJetpack;
	}

	void ConShowOthers(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;
		auto &ShowOthers = RaceTeams(pGameServer)->PlayerState(pResult->m_ClientId).m_ShowOthers;
		if(g_Config.m_SvShowOthers)
		{
			if(pResult->NumArguments())
				ShowOthers = pResult->GetInteger(0);
			else
				ShowOthers = !ShowOthers;
		}
		else
			log_info("chatresp", "Showing players from other teams is disabled");
	}

	void ConSpecTeam(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;
		auto &SpecTeam = RaceTeams(pGameServer)->PlayerState(pResult->m_ClientId).m_SpecTeam;

		if(pResult->NumArguments())
			SpecTeam = pResult->GetInteger(0);
		else
			SpecTeam = !SpecTeam;
	}

	void ConSayTime(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		int ClientId;
		char aBufName[MAX_NAME_LENGTH];
		if(pResult->NumArguments() > 0)
		{
			ClientId = pGameServer->FindClientIdByName(pResult->GetString(0)).value_or(-1);
			if(ClientId == -1)
				return;

			str_format(aBufName, sizeof(aBufName), "%s's", pGameServer->Server()->ClientName(ClientId));
		}
		else
		{
			str_copy(aBufName, "Your");
			ClientId = pResult->m_ClientId;
		}

		CPlayer *pPlayer = pGameServer->m_apPlayers[ClientId];
		if(!pPlayer)
			return;
		CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
		if(!pCharacter || pCharacter->m_DDRaceState != ERaceState::STARTED)
			return;

		char aBufTime[32];
		char aBuf[64];
		int64_t Time = (int64_t)100 * (float)(pGameServer->Server()->Tick() - pCharacter->m_StartTime) / ((float)pGameServer->Server()->TickSpeed());
		str_time(Time, ETimeFormat::HOURS, aBufTime, sizeof(aBufTime));
		str_format(aBuf, sizeof(aBuf), "%s current race time is %s", aBufName, aBufTime);
		log_info("chatresp", "%s", aBuf);
	}

	void ConSayTimeAll(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;
		CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
		if(!pCharacter || pCharacter->m_DDRaceState != ERaceState::STARTED)
			return;

		char aBufTime[32];
		char aBuf[64];
		int64_t Time = (int64_t)100 * (float)(pGameServer->Server()->Tick() - pCharacter->m_StartTime) / ((float)pGameServer->Server()->TickSpeed());
		const char *pName = pGameServer->Server()->ClientName(pResult->m_ClientId);
		str_time(Time, ETimeFormat::HOURS, aBufTime, sizeof(aBufTime));
		str_format(aBuf, sizeof(aBuf), "%s's current race time is %s", pName, aBufTime);
		pGameServer->SendChat(-1, TEAM_ALL, aBuf, pResult->m_ClientId);
	}

	void ConTime(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;
		CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
		if(!pCharacter)
			return;

		char aBufTime[32];
		char aBuf[64];
		int64_t Time = (int64_t)100 * (float)(pGameServer->Server()->Tick() - pCharacter->m_StartTime) / ((float)pGameServer->Server()->TickSpeed());
		str_time(Time, ETimeFormat::HOURS, aBufTime, sizeof(aBufTime));
		str_format(aBuf, sizeof(aBuf), "Your time is %s", aBufTime);
		pGameServer->SendBroadcast(aBuf, pResult->m_ClientId);
	}

	static const char s_aaTimerTypeMessage[4][128] = {"game/round timer.", "broadcast.", "both game/round timer and broadcast.", "racetime."};

	void ConSetTimerType(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		char aBuf[128];
		if(pResult->NumArguments() > 0)
		{
			int OldType = pPlayer->m_TimerType;
			bool Result = false;

			if(str_comp_nocase(pResult->GetString(0), "default") == 0)
				Result = pPlayer->SetTimerType(CPlayer::TIMERTYPE_DEFAULT);
			else if(str_comp_nocase(pResult->GetString(0), "gametimer") == 0)
				Result = pPlayer->SetTimerType(CPlayer::TIMERTYPE_GAMETIMER);
			else if(str_comp_nocase(pResult->GetString(0), "broadcast") == 0)
				Result = pPlayer->SetTimerType(CPlayer::TIMERTYPE_BROADCAST);
			else if(str_comp_nocase(pResult->GetString(0), "both") == 0)
				Result = pPlayer->SetTimerType(CPlayer::TIMERTYPE_GAMETIMER_AND_BROADCAST);
			else if(str_comp_nocase(pResult->GetString(0), "none") == 0)
				Result = pPlayer->SetTimerType(CPlayer::TIMERTYPE_NONE);
			else
			{
				log_info("chatresp", "Unknown parameter. Accepted values: default, gametimer, broadcast, both, none");
				return;
			}

			if(!Result)
			{
				log_info("chatresp", "Selected timertype is not supported by your client");
				return;
			}

			if((OldType == CPlayer::TIMERTYPE_BROADCAST || OldType == CPlayer::TIMERTYPE_GAMETIMER_AND_BROADCAST) && (pPlayer->m_TimerType == CPlayer::TIMERTYPE_GAMETIMER || pPlayer->m_TimerType == CPlayer::TIMERTYPE_NONE))
				pGameServer->SendBroadcast("", pResult->m_ClientId);
		}

		if(pPlayer->m_TimerType <= CPlayer::TIMERTYPE_SIXUP && pPlayer->m_TimerType >= CPlayer::TIMERTYPE_GAMETIMER)
			str_format(aBuf, sizeof(aBuf), "Timer is displayed in %s", s_aaTimerTypeMessage[pPlayer->m_TimerType]);
		else if(pPlayer->m_TimerType == CPlayer::TIMERTYPE_NONE)
			str_copy(aBuf, "Timer isn't displayed.");

		log_info("chatresp", "%s", aBuf);
	}

	void ConProtectedKill(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;
		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;
		CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
		if(!pCharacter)
			return;

		int CurrTime = (pGameServer->Server()->Tick() - pCharacter->m_StartTime) / pGameServer->Server()->TickSpeed();
		if(g_Config.m_SvKillProtection != 0 && CurrTime >= (60 * g_Config.m_SvKillProtection) && pCharacter->m_DDRaceState == ERaceState::STARTED)
		{
			pPlayer->KillCharacter(WEAPON_SELF);
			pPlayer->Respawn();
		}
	}

	void UnlockTeam(CGameContext *pGameServer, int ClientId, int Team)
	{
		RaceTeams(pGameServer)->SetTeamLock(Team, false);

		char aBuf[512];
		str_format(aBuf, sizeof(aBuf), "'%s' unlocked your team.", pGameServer->Server()->ClientName(ClientId));
		pGameServer->SendChatTeam(Team, aBuf);
	}

	void AttemptJoinTeam(CGameContext *pGameServer, int ClientId, int Team)
	{
		CPlayer *pPlayer = pGameServer->m_apPlayers[ClientId];
		if(!pPlayer)
			return;

		if(pGameServer->IsRunningKickOrSpecVote(ClientId))
		{
			log_info("chatresp", "You are running a vote, please try again after the vote is done!");
			return;
		}
		else if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		{
			log_info("chatresp", "Teams are disabled");
			return;
		}
		else if(g_Config.m_SvTeam == SV_TEAM_MANDATORY && Team == 0)
		{
			CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
			if(pCharacter && pCharacter->TryStartWarning())
				log_info("chatresp", "You must join a team and play with somebody or else you can't play");
		}

		if(!RaceTeams(pGameServer)->IsValidTeamNumber(Team))
		{
			auto EmptyTeam = RaceTeams(pGameServer)->GetFirstEmptyTeam();
			if(!EmptyTeam.has_value())
			{
				log_info("chatresp", "No empty team left.");
				return;
			}
			Team = EmptyTeam.value();
		}

		char aError[512];
		auto &PlayerState = RaceTeams(pGameServer)->PlayerState(ClientId);
		if(PlayerState.m_LastTeamChange.has_value() && PlayerState.m_LastTeamChange.value() + (int64_t)pGameServer->Server()->TickSpeed() * g_Config.m_SvTeamChangeDelay > pGameServer->Server()->Tick())
		{
			log_info("chatresp", "You can't change teams that fast!");
		}
		else if(Team != TEAM_FLOCK && RaceTeams(pGameServer)->TeamLocked(Team) && !RaceTeams(pGameServer)->IsInvited(Team, ClientId))
		{
			log_info("chatresp", g_Config.m_SvInvite ?
						     "This team is locked using /lock. Only members of the team can unlock it using /lock." :
						     "This team is locked using /lock. Only members of the team can invite you or unlock it using /lock.");
		}
		else if(Team != TEAM_FLOCK && RaceTeams(pGameServer)->TeamSize(Team) >= g_Config.m_SvMaxTeamSize && !RaceTeams(pGameServer)->TeamFlock(Team) && !RaceTeams(pGameServer)->IsPractice(Team))
		{
			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "This team already has the maximum allowed size of %d players", g_Config.m_SvMaxTeamSize);
			log_info("chatresp", "%s", aBuf);
		}
		else if(!RaceTeams(pGameServer)->SetCharacterTeam(pPlayer->GetCid(), Team, aError, sizeof(aError)))
		{
			log_info("chatresp", "%s", aError);
		}
		else
		{
			if(RaceTeams(pGameServer)->PracticeByDefault())
			{
				// joined an empty team
				if(RaceTeams(pGameServer)->TeamSize(Team) == 1)
					RaceTeams(pGameServer)->SetPractice(Team, true);
			}

			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "'%s' joined team %d",
				pGameServer->Server()->ClientName(pPlayer->GetCid()),
				Team);
			pGameServer->SendChat(-1, TEAM_ALL, aBuf);
			PlayerState.m_LastTeamChange = pGameServer->Server()->Tick();

			if(RaceTeams(pGameServer)->IsPractice(Team))
				pGameServer->SendChatTarget(pPlayer->GetCid(), "Practice mode enabled for your team, happy practicing!");

			if(RaceTeams(pGameServer)->TeamFlock(Team))
				pGameServer->SendChatTarget(pPlayer->GetCid(), "Team 0 mode enabled for your team. This will make your team behave like team 0.");
		}
	}

	void ConSwap(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		const char *pName = pResult->GetString(0);

		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		if(!g_Config.m_SvSwap)
		{
			log_info("chatresp", "Swap is disabled on this server.");
			return;
		}

		if(g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		{
			log_info("chatresp", "Swap is not available on forced solo servers.");
			return;
		}

		CGameTeams &Teams = *RaceTeams(pGameServer);
		int Team = Teams.m_Core.Team(pResult->m_ClientId);

		if(Team == TEAM_SUPER)
		{
			log_info("chatresp", "Turn off super to use swap feature, which means you can swap positions with each other.");
			return;
		}

		int TargetClientId = -1;
		if(pResult->NumArguments() == 1)
		{
			TargetClientId = pGameServer->FindClientIdByName(pName).value_or(-1);
		}
		else
		{
			int TeamSize = 1;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(pGameServer->m_apPlayers[i] && Teams.m_Core.Team(i) == Team && i != pResult->m_ClientId)
				{
					TargetClientId = i;
					TeamSize++;
				}
			}
			if(TeamSize != 2)
				TargetClientId = -1;
		}

		if(TargetClientId < 0)
		{
			log_info("chatresp", "Player not found");
			return;
		}

		if(TargetClientId == pResult->m_ClientId)
		{
			log_info("chatresp", "Can't swap with yourself");
			return;
		}

		int TargetTeam = Teams.m_Core.Team(TargetClientId);
		if(TargetTeam != Team)
		{
			log_info("chatresp", "Player is on a different team");
			return;
		}

		CPlayer *pSwapPlayer = pGameServer->m_apPlayers[TargetClientId];
		if(Team == TEAM_FLOCK || Teams.TeamFlock(Team))
		{
			CCharacterDDRace *pChr = DDRaceCharacter(pPlayer->GetCharacter());
			CCharacterDDRace *pSwapChr = DDRaceCharacter(pSwapPlayer->GetCharacter());
			if(!pChr || !pSwapChr || pChr->m_DDRaceState != ERaceState::STARTED || pSwapChr->m_DDRaceState != ERaceState::STARTED)
			{
				log_info("chatresp", "You and other player need to have started the map");
				return;
			}
		}
		else if(!Teams.IsStarted(Team) && !Teams.TeamFlock(Team))
		{
			log_info("chatresp", "Need to have started the map to swap with a player.");
			return;
		}
		if(pGameServer->m_World.m_Core.m_apCharacters[pResult->m_ClientId] == nullptr || pGameServer->m_World.m_Core.m_apCharacters[TargetClientId] == nullptr)
		{
			log_info("chatresp", "You and the other player must not be paused.");
			return;
		}

		bool SwapPending = Teams.PlayerState(TargetClientId).m_SwapTargetClientId != pResult->m_ClientId;
		if(SwapPending)
		{
			if(pGameServer->ProcessSpamProtection(pResult->m_ClientId))
				return;

			Teams.RequestTeamSwap(pPlayer, pSwapPlayer, Team);
			return;
		}

		Teams.SwapTeamCharacters(DDRaceCharacter(pPlayer->GetCharacter()), DDRaceCharacter(pSwapPlayer->GetCharacter()), Team);
	}

	void ConCancelSwap(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		if(!g_Config.m_SvSwap)
		{
			log_info("chatresp", "Swap is disabled on this server.");
			return;
		}

		if(g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		{
			log_info("chatresp", "Swap is not available on forced solo servers.");
			return;
		}

		CGameTeams &Teams = *RaceTeams(pGameServer);
		int Team = Teams.m_Core.Team(pResult->m_ClientId);

		const int SwapTargetClientId = Teams.PlayerState(pResult->m_ClientId).m_SwapTargetClientId;
		bool SwapPending = SwapTargetClientId != -1 && !pGameServer->Server()->ClientSlotEmpty(SwapTargetClientId);

		if(!SwapPending)
		{
			log_info("chatresp", "You do not have a pending swap request.");
			return;
		}

		Teams.CancelTeamSwap(pPlayer, Team);
	}

	void ConSave(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(!g_Config.m_SvSaveGames)
		{
			pGameServer->SendChatTarget(pResult->m_ClientId, "Save-function is disabled on this server");
			return;
		}

		const char *pCode = "";
		if(pResult->NumArguments() > 0)
			pCode = pResult->GetString(0);

		RaceScore(pGameServer)->SaveTeam(pResult->m_ClientId, pCode, g_Config.m_SvSqlServerName);
	}

	void ConLoad(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(!g_Config.m_SvSaveGames)
		{
			pGameServer->SendChatTarget(pResult->m_ClientId, "Save-function is disabled on this server");
			return;
		}

		if(pResult->NumArguments() > 0)
			RaceScore(pGameServer)->LoadTeam(pResult->GetString(0), pResult->m_ClientId);
		else
			RaceScore(pGameServer)->GetSaves(pResult->m_ClientId);
	}

	void ConLock(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		{
			log_info("chatresp", "Teams are disabled");
			return;
		}

		int Team = RaceTeams(pGameServer)->m_Core.Team(pResult->m_ClientId);
		bool Lock = RaceTeams(pGameServer)->TeamLocked(Team);

		if(pResult->NumArguments() > 0)
			Lock = !pResult->GetInteger(0);

		if(Team == TEAM_FLOCK || !RaceTeams(pGameServer)->IsValidTeamNumber(Team))
		{
			log_info("chatresp", "This team can't be locked");
			return;
		}

		if(pGameServer->ProcessSpamProtection(pResult->m_ClientId, false))
			return;

		char aBuf[512];
		if(Lock)
		{
			UnlockTeam(pGameServer, pResult->m_ClientId, Team);
		}
		else
		{
			RaceTeams(pGameServer)->SetTeamLock(Team, true);

			if(RaceTeams(pGameServer)->TeamFlock(Team))
				str_format(aBuf, sizeof(aBuf), "'%s' locked your team.", pGameServer->Server()->ClientName(pResult->m_ClientId));
			else
				str_format(aBuf, sizeof(aBuf), "'%s' locked your team. After the race starts, killing will kill everyone in your team.", pGameServer->Server()->ClientName(pResult->m_ClientId));
			pGameServer->SendChatTeam(Team, aBuf);
		}
	}

	void ConUnlock(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		{
			log_info("chatresp", "Teams are disabled");
			return;
		}

		int Team = RaceTeams(pGameServer)->m_Core.Team(pResult->m_ClientId);

		if(Team == TEAM_FLOCK || !RaceTeams(pGameServer)->IsValidTeamNumber(Team))
			return;

		if(pGameServer->ProcessSpamProtection(pResult->m_ClientId, false))
			return;

		UnlockTeam(pGameServer, pResult->m_ClientId, Team);
	}

	void ConInvite(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		const char *pName = pResult->GetString(0);

		if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		{
			log_info("chatresp", "Teams are disabled");
			return;
		}

		if(!g_Config.m_SvInvite)
		{
			log_info("chatresp", "Invites are disabled");
			return;
		}

		int Team = RaceTeams(pGameServer)->m_Core.Team(pResult->m_ClientId);
		if(Team != TEAM_FLOCK && RaceTeams(pGameServer)->IsValidTeamNumber(Team))
		{
			int Target = pGameServer->FindClientIdByName(pName).value_or(-1);
			if(Target == -1)
			{
				log_info("chatresp", "Player not found");
				return;
			}

			if(RaceTeams(pGameServer)->IsInvited(Team, Target))
			{
				log_info("chatresp", "Player already invited");
				return;
			}

			auto &PlayerState = RaceTeams(pGameServer)->PlayerState(pResult->m_ClientId);
			if(pGameServer->m_apPlayers[pResult->m_ClientId] && PlayerState.m_LastInvited + g_Config.m_SvInviteFrequency * pGameServer->Server()->TickSpeed() > pGameServer->Server()->Tick())
			{
				log_info("chatresp", "Can't invite this quickly");
				return;
			}

			RaceTeams(pGameServer)->SetClientInvited(Team, Target, true);
			PlayerState.m_LastInvited = pGameServer->Server()->Tick();

			char aBuf[512];
			str_format(aBuf, sizeof(aBuf), "'%s' invited you to team %d. Use /team %d to join.", pGameServer->Server()->ClientName(pResult->m_ClientId), Team, Team);
			pGameServer->SendChatTarget(Target, aBuf);

			str_format(aBuf, sizeof(aBuf), "'%s' invited '%s' to your team.", pGameServer->Server()->ClientName(pResult->m_ClientId), pGameServer->Server()->ClientName(Target));
			pGameServer->SendChatTeam(Team, aBuf);
		}
		else
			log_info("chatresp", "Can't invite players to this team");
	}

	void ConTeam0Mode(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		if(g_Config.m_SvTeam == SV_TEAM_FORBIDDEN || g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO || g_Config.m_SvTeam == SV_TEAM_MANDATORY)
		{
			log_info("chatresp", "Team mode change disabled");
			return;
		}

		if(!g_Config.m_SvTeam0Mode)
		{
			log_info("chatresp", "Team mode change is disabled on this server.");
			return;
		}

		int Team = RaceTeams(pGameServer)->m_Core.Team(pResult->m_ClientId);
		bool Mode = RaceTeams(pGameServer)->TeamFlock(Team);

		if(Team == TEAM_FLOCK || !RaceTeams(pGameServer)->IsValidTeamNumber(Team))
		{
			log_info("chatresp", "This team can't have the mode changed");
			return;
		}

		if(RaceTeams(pGameServer)->GetTeamState(Team) != ETeamState::OPEN)
		{
			pGameServer->SendChatTarget(pResult->m_ClientId, "Team mode can't be changed while racing");
			return;
		}

		if(pResult->NumArguments() > 0)
			Mode = !pResult->GetInteger(0);

		if(pGameServer->ProcessSpamProtection(pResult->m_ClientId, false))
			return;

		char aBuf[512];
		if(Mode)
		{
			if(RaceTeams(pGameServer)->TeamSize(Team) > g_Config.m_SvMaxTeamSize)
			{
				str_format(aBuf, sizeof(aBuf), "Can't disable team 0 mode. This team exceeds the maximum allowed size of %d players for regular team", g_Config.m_SvMaxTeamSize);
				pGameServer->SendChatTarget(pResult->m_ClientId, aBuf);
			}
			else
			{
				RaceTeams(pGameServer)->SetTeamFlock(Team, false);

				str_format(aBuf, sizeof(aBuf), "'%s' disabled team 0 mode.", pGameServer->Server()->ClientName(pResult->m_ClientId));
				pGameServer->SendChatTeam(Team, aBuf);
			}
		}
		else
		{
			if(RaceTeams(pGameServer)->IsPractice(Team))
			{
				pGameServer->SendChatTarget(pResult->m_ClientId, "Can't enable team 0 mode with practice mode on.");
			}
			else
			{
				RaceTeams(pGameServer)->SetTeamFlock(Team, true);

				str_format(aBuf, sizeof(aBuf), "'%s' enabled team 0 mode. This will make your team behave like team 0.", pGameServer->Server()->ClientName(pResult->m_ClientId));
				pGameServer->SendChatTeam(Team, aBuf);
			}
		}
	}

	void ConTeam(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		CPlayer *pPlayer = pGameServer->m_apPlayers[pResult->m_ClientId];
		if(!pPlayer)
			return;

		if(pResult->NumArguments() > 0)
		{
			AttemptJoinTeam(pGameServer, pResult->m_ClientId, pResult->GetInteger(0));
		}
		else
		{
			char aBuf[512];
			if(!pPlayer->IsPlaying())
			{
				log_info("chatresp", "You can't check your team while you are dead/a spectator.");
			}
			else
			{
				int TeamSize = 0;
				const int PlayerTeam = RaceTeams(pGameServer)->m_Core.Team(pResult->m_ClientId);

				// Count players in team
				for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
				{
					const CPlayer *pOtherPlayer = pGameServer->m_apPlayers[ClientId];
					if(!pOtherPlayer || !pOtherPlayer->IsPlaying())
						continue;

					if(RaceTeams(pGameServer)->m_Core.Team(ClientId) == PlayerTeam)
						TeamSize++;
				}

				str_format(aBuf, sizeof(aBuf), "You are in team %d having %d %s", PlayerTeam, TeamSize, TeamSize > 1 ? "players" : "player");
				log_info("chatresp", "%s", aBuf);
			}
		}
	}

	void ConJoin(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);
		if(!CheckClientId(pResult->m_ClientId))
			return;

		const char *pName = pResult->GetString(0);
		int Target = pGameServer->FindClientIdByName(pName).value_or(-1);
		if(Target == -1)
		{
			log_info("chatresp", "Player not found");
			return;
		}

		int Team = RaceTeams(pGameServer)->m_Core.Team(Target);
		if(pGameServer->ProcessSpamProtection(pResult->m_ClientId, false))
			return;

		AttemptJoinTeam(pGameServer, pResult->m_ClientId, Team);
	}

	void ConRandomMap(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);

		const int ClientId = pResult->m_ClientId == -1 ? pGameServer->m_VoteCreator : pResult->m_ClientId;
		int MinStars = pResult->NumArguments() > 0 ? pResult->GetInteger(0) : -1;
		int MaxStars = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : MinStars;

		if(!in_range(MinStars, -1, 5) || !in_range(MaxStars, -1, 5))
			return;

		RaceScore(pGameServer)->RandomMap(ClientId, MinStars, MaxStars);
	}

	void ConRandomUnfinishedMap(IConsole::IResult *pResult, void *pUserData)
	{
		CGameContext *pGameServer = static_cast<CGameContext *>(pUserData);

		const int ClientId = pResult->m_ClientId == -1 ? pGameServer->m_VoteCreator : pResult->m_ClientId;
		int MinStars = pResult->NumArguments() > 0 ? pResult->GetInteger(0) : -1;
		int MaxStars = pResult->NumArguments() > 1 ? pResult->GetInteger(1) : MinStars;

		if(!in_range(MinStars, -1, 5) || !in_range(MaxStars, -1, 5))
			return;

		RaceScore(pGameServer)->RandomUnfinishedMap(ClientId, MinStars, MaxStars);
	}
}

void CGameControllerDDRace::ApplyMapSettings()
{
	for(int Index = 0; Index < GameServer()->Collision()->GetWidth() * GameServer()->Collision()->GetHeight(); Index++)
	{
		for(int Layer = 0; Layer < 2; Layer++)
		{
			const CTile *pTiles = Layer == 0 ? GameServer()->Collision()->GameLayer() : GameServer()->Collision()->FrontLayer();
			if(!pTiles)
				continue;

			const char *pLogCategory = Layer == 0 ? "game_layer" : "front_layer";
			switch(pTiles[Index].m_Index)
			{
			case TILE_OLDLASER:
				g_Config.m_SvOldLaser = 1;
				dbg_msg(pLogCategory, "found old laser tile");
				break;
			case TILE_NPC:
				GameServer()->GlobalTuning()->Set("player_collision", 0);
				dbg_msg(pLogCategory, "found no collision tile");
				break;
			case TILE_EHOOK:
				g_Config.m_SvEndlessDrag = 1;
				dbg_msg(pLogCategory, "found unlimited hook time tile");
				break;
			case TILE_NOHIT:
				g_Config.m_SvHit = 0;
				dbg_msg(pLogCategory, "found no weapons hitting others tile");
				break;
			case TILE_NPH:
				GameServer()->GlobalTuning()->Set("player_hooking", 0);
				dbg_msg(pLogCategory, "found no player hooking tile");
				break;
			}
		}
	}
}

void CGameControllerDDRace::InitGameSettings()
{
	IGameController::InitGameSettings();
	ApplyMapSettings();
}

void CGameControllerDDRace::OnReset()
{
	IGameController::OnReset();
	ApplyMapSettings();
}

CGameControllerDDRace::CGameControllerDDRace(CGameServices &Services, const CGameModeInfo &GameModeInfo) :
	IGameController(Services, GameModeInfo)
{
	m_pRaceTeams = std::make_unique<CGameTeams>(GameServer(), TeamsCore());
	RegisterMapEntityFactory(CreateRaceMapEntity);
	IgnoreMapEntityRange(ENTITY_LASER_SHORT, ENTITY_LASER_O_FAST);
}

CGameControllerDDRace::~CGameControllerDDRace() = default;

void CGameControllerDDRace::Init(CDbConnectionPool *pDbPool)
{
	dbg_assert(pDbPool, "DDRace score service requires a database pool");
	m_pRaceScore = std::make_unique<CScore>(GameServer(), pDbPool, &RaceTeams());
	RaceTeams().SetScore(&RaceScore());
	IGameController::Init(pDbPool);
	RaceScore().LoadMapInfo();
	RaceTeams().Reset();
}

CGameTeams &CGameControllerDDRace::RaceTeams()
{
	return *m_pRaceTeams;
}

const CGameTeams &CGameControllerDDRace::RaceTeams() const
{
	return *m_pRaceTeams;
}

CScore &CGameControllerDDRace::RaceScore()
{
	dbg_assert(m_pRaceScore, "DDRace score service is not initialized");
	return *m_pRaceScore;
}

const CScore &CGameControllerDDRace::RaceScore() const
{
	dbg_assert(m_pRaceScore, "DDRace score service is not initialized");
	return *m_pRaceScore;
}

CCharacterDDRace *CGameControllerDDRace::CreateCharacter(CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	return new(ClientId) CCharacterDDRace(&Services().World(), Services().LastPlayerInput(ClientId));
}

bool CGameControllerDDRace::CanCharacterHitCharacter(CCharacter *pAttacker, CCharacter *pTarget) const
{
	return pTarget->IsAlive() && pAttacker->CanCollide(pTarget->GetPlayer()->GetCid());
}

CGamePickupResult CGameControllerDDRace::OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position)
{
	bool Sound = false;
	switch(Type)
	{
	case POWERUP_HEALTH:
		if(pCharacter->Freeze())
			GameServer()->CreateSound(Position, SOUND_PICKUP_HEALTH, pCharacter->TeamMask());
		break;
	case POWERUP_ARMOR:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		for(int Weapon = WEAPON_SHOTGUN; Weapon < NUM_WEAPONS; Weapon++)
		{
			if(pCharacter->GetWeaponGot(Weapon))
			{
				pCharacter->SetWeaponGot(Weapon, false);
				pCharacter->SetWeaponAmmo(Weapon, 0);
				Sound = true;
			}
		}
		pCharacter->SetNinjaActivationDir(vec2(0, 0));
		pCharacter->SetNinjaActivationTick(-500);
		pCharacter->SetNinjaCurrentMoveTime(0);
		if(Sound)
		{
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() >= WEAPON_SHOTGUN)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_ARMOR_SHOTGUN:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		if(pCharacter->GetWeaponGot(WEAPON_SHOTGUN))
		{
			pCharacter->SetWeaponGot(WEAPON_SHOTGUN, false);
			pCharacter->SetWeaponAmmo(WEAPON_SHOTGUN, 0);
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() == WEAPON_SHOTGUN)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_ARMOR_GRENADE:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		if(pCharacter->GetWeaponGot(WEAPON_GRENADE))
		{
			pCharacter->SetWeaponGot(WEAPON_GRENADE, false);
			pCharacter->SetWeaponAmmo(WEAPON_GRENADE, 0);
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() == WEAPON_GRENADE)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_ARMOR_NINJA:
		if(pCharacter->Team() != TEAM_SUPER)
		{
			pCharacter->SetNinjaActivationDir(vec2(0, 0));
			pCharacter->SetNinjaActivationTick(-500);
			pCharacter->SetNinjaCurrentMoveTime(0);
		}
		break;
	case POWERUP_ARMOR_LASER:
		if(pCharacter->Team() == TEAM_SUPER)
			break;
		if(pCharacter->GetWeaponGot(WEAPON_LASER))
		{
			pCharacter->SetWeaponGot(WEAPON_LASER, false);
			pCharacter->SetWeaponAmmo(WEAPON_LASER, 0);
			pCharacter->SetLastWeapon(WEAPON_GUN);
			GameServer()->CreateSound(Position, SOUND_PICKUP_ARMOR, pCharacter->TeamMask());
		}
		if(pCharacter->GetActiveWeapon() == WEAPON_LASER)
			pCharacter->SetActiveWeapon(WEAPON_HAMMER);
		break;
	case POWERUP_WEAPON:
		if(Subtype >= 0 && Subtype < NUM_WEAPONS && (!pCharacter->GetWeaponGot(Subtype) || pCharacter->GetWeaponAmmo(Subtype) != -1))
		{
			pCharacter->GiveWeapon(Subtype);
			if(Subtype == WEAPON_GRENADE)
				GameServer()->CreateSound(Position, SOUND_PICKUP_GRENADE, pCharacter->TeamMask());
			else if(Subtype == WEAPON_SHOTGUN || Subtype == WEAPON_LASER)
				GameServer()->CreateSound(Position, SOUND_PICKUP_SHOTGUN, pCharacter->TeamMask());
			if(pCharacter->GetPlayer())
				GameServer()->SendWeaponPickup(pCharacter->GetPlayer()->GetCid(), Subtype);
		}
		break;
	case POWERUP_NINJA:
		pCharacter->GiveNinja();
		break;
	default:
		break;
	}
	return {};
}

CGameProjectileRules CGameControllerDDRace::ProjectileRules(const CGameProjectileContext &Context) const
{
	const EProjectileOwnerLossAction OwnerLossAction = Context.m_Weapon != WEAPON_GRENADE || g_Config.m_SvDestroyBulletsOnDeath || Context.m_BelongsToPracticeTeam ? EProjectileOwnerLossAction::DESTROY : EProjectileOwnerLossAction::KEEP;
	return {
		Context.m_pOwner ? !Context.m_pOwner->GrenadeHitDisabled() : g_Config.m_SvHit != 0,
		true,
		0.0f,
		OwnerLossAction,
	};
}

void CGameControllerDDRace::RegisterCommands()
{
	RegisterAdminCommands();
	RegisterPracticeCommands();

	static const CCommandRegistration s_aPlayerCommands[] = {
		{"settings", "?s[configname]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConSettings, "Shows gameplay information for this server"},
		{"pause", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTogglePause, "Toggles pause"},
		{"spec", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConToggleSpec, "Toggles spec (if not available behaves as /pause)"},
		{"pausevoted", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTogglePauseVoted, "Toggles pause on the currently voted player"},
		{"specvoted", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConToggleSpecVoted, "Toggles spec on the currently voted player"},
		{"showothers", "?i['0'|'1'|'2']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConShowOthers, "Whether to show players from other teams or not (off by default), optional i = 0 for off, i = 1 for on, i = 2 for own team only"},
		{"specteam", "?i['0'|'1']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConSpecTeam, "Whether to show players from other teams when spectating (on by default), optional i = 0 for off else for on"},
		{"ninjajetpack", "?i['0'|'1']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConNinjaJetpack, "Whether to use ninja jetpack or not. Makes jetpack look more awesome"},
		{"saytime", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConSayTime, "Privately messages someone's current time in this current running race (your time by default)"},
		{"saytimeall", "", CFGFLAG_CHAT | CFGFLAG_SERVER | CFGFLAG_NONTEEHISTORIC, ConSayTimeAll, "Publicly messages everyone your current time in this current running race"},
		{"time", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTime, "Privately shows you your current time in this current running race in the broadcast message"},
		{"timer", "?s['gametimer'|'broadcast'|'both'|'none'|'cycle']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConSetTimerType, "Personal Setting of showing time in either broadcast or game/round timer, timer s, where s = broadcast for broadcast, gametimer for game/round timer, cycle for cycle, both for both, none for no timer and nothing to show current status"},
		{"kill", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConProtectedKill, "Kill yourself when kill-protected during a long game (use f1, kill for regular kill)"},
	};

	for(const CCommandRegistration &Command : s_aPlayerCommands)
	{
		dbg_assert(GameServer()->Console()->RegisterOwned(Command.m_pName, Command.m_pParams, Command.m_Flags, Command.m_pfnCallback, GameServer(), Command.m_pHelp, this), "duplicate mode command '%s'", Command.m_pName);
	}

	static const CCommandRegistration s_aScoreCommands[] = {
		{"random_map", "?i[stars] ?i[max stars]", CFGFLAG_SERVER | CFGFLAG_STORE, ConRandomMap, "Random map"},
		{"random_unfinished_map", "?i[stars] ?i[max stars]", CFGFLAG_SERVER | CFGFLAG_STORE, ConRandomUnfinishedMap, "Random unfinished map"},
		{"rankteam", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeamRank, "Shows the team rank of player with name r (your team rank by default)"},
		{"teamrank", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeamRank, "Shows the team rank of player with name r (your team rank by default)"},
		{"rank", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConRank, "Shows the rank of player with name r (your rank by default)"},
		{"top5team", "?s[player name] ?i[rank to start with]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeamTop5, "Shows five team ranks of the ladder or of a player beginning with rank i (1 by default, -1 for worst)"},
		{"teamtop5", "?s[player name] ?i[rank to start with]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeamTop5, "Shows five team ranks of the ladder or of a player beginning with rank i (1 by default, -1 for worst)"},
		{"top", "?i[rank to start with]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTop, "Shows the top ranks of the global and regional ladder beginning with rank i (1 by default, -1 for worst)"},
		{"top5", "?i[rank to start with]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTop, "Shows the top ranks of the global and regional ladder beginning with rank i (1 by default, -1 for worst)"},
		{"times", "?s[player name] ?i[number of times to skip]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTimes, "/times ?s?i shows last 5 times of the server or of a player beginning with name s starting with time i (i = 1 by default, -1 for first)"},
		{"points", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConPoints, "Shows the global points of a player beginning with name r (your rank by default)"},
		{"top5points", "?i[number]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTopPoints, "Shows five points of the global point ladder beginning with rank i (1 by default)"},
		{"timecp", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTimeCp, "Set your checkpoints based on another player"},
	};

	for(const CCommandRegistration &Command : s_aScoreCommands)
	{
		dbg_assert(GameServer()->Console()->RegisterOwned(Command.m_pName, Command.m_pParams, Command.m_Flags, Command.m_pfnCallback, GameServer(), Command.m_pHelp, this), "duplicate mode command '%s'", Command.m_pName);
	}

	static const CCommandRegistration s_aTeamCommands[] = {
		{"swap", "?r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConSwap, "Request to swap your tee with another team member"},
		{"cancelswap", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConCancelSwap, "Cancel your swap request"},
		{"save", "?r[code]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConSave, "Save team with code r."},
		{"load", "?r[code]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConLoad, "Load with code r. /load to check your existing saves"},
		{"team", "?i[id]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeam, "Lets you join team i (shows your team if left blank)"},
		{"lock", "?i['0'|'1']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConLock, "Toggle team lock so no one else can join and so the team restarts when a player dies. /lock 0 to unlock, /lock 1 to lock"},
		{"unlock", "", CFGFLAG_CHAT | CFGFLAG_SERVER, ConUnlock, "Unlock a team"},
		{"invite", "r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConInvite, "Invite a person to a locked team"},
		{"join", "r[player name]", CFGFLAG_CHAT | CFGFLAG_SERVER, ConJoin, "Join the team of the specified player"},
		{"team0mode", "?i['0'|'1']", CFGFLAG_CHAT | CFGFLAG_SERVER, ConTeam0Mode, "Toggle team between team 0 and team mode. This mode will make your team behave like team 0."},
	};

	for(const CCommandRegistration &Command : s_aTeamCommands)
	{
		dbg_assert(GameServer()->Console()->RegisterOwned(Command.m_pName, Command.m_pParams, Command.m_Flags, Command.m_pfnCallback, GameServer(), Command.m_pHelp, this), "duplicate mode command '%s'", Command.m_pName);
	}
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

void CGameControllerDDRace::OnCharacterDeath(const CGameCharacterDeathContext &Context)
{
	CCharacter *pVictim = Context.m_pVictim;
	const int VictimId = pVictim->GetPlayer()->GetCid();
	const int Team = pVictim->Team();
	const bool SendKillMessage = Context.m_SendKillMessage &&
		(Team == TEAM_FLOCK || RaceTeams().TeamFlock(Team) || RaceTeams().TeamSize(Team) == 1 || RaceTeams().GetTeamState(Team) == ETeamState::OPEN || !RaceTeams().TeamLocked(Team));

	CGameCharacterDeathContext RaceContext = Context;
	RaceContext.m_SendKillMessage = SendKillMessage;
	FinalizeCharacterDeath(RaceContext);
	RaceTeams().OnCharacterDeath(VictimId, Context.m_Weapon);

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		if(GameServer()->m_apPlayers[ClientId] && RaceTeams().PlayerState(ClientId).m_SwapTargetClientId == VictimId)
			RaceTeams().PlayerState(ClientId).m_SwapTargetClientId = -1;
	}
	RaceTeams().PlayerState(VictimId).m_SwapTargetClientId = -1;
}

void CGameControllerDDRace::OnCharacterSpawn(CCharacter *pCharacter)
{
	CCharacterDDRace *pRaceCharacter = DDRaceCharacter(pCharacter);
	dbg_assert(pRaceCharacter, "DDRace controller requires a DDRace character");
	pRaceCharacter->SetRaceTeams(&RaceTeams());
	pRaceCharacter->SetRaceScore(m_pRaceScore.get());
	RaceTeams().OnCharacterSpawn(pCharacter->GetPlayer()->GetCid());
	IGameController::OnCharacterSpawn(pCharacter);
	pRaceCharacter->DDRaceInit();
	if(g_Config.m_SvTeam == SV_TEAM_FORCED_SOLO)
		pRaceCharacter->SetSolo(true);
}

void CGameControllerDDRace::TickCharacterPreCore(CCharacter *pCharacter)
{
	DDRaceCharacter(pCharacter)->DDRaceTick();
}

void CGameControllerDDRace::TickCharacterPostCore(CCharacter *pCharacter)
{
	DDRaceCharacter(pCharacter)->DDRacePostCoreTick();
}

int CGameControllerDDRace::PlayerAutoRespawnTick(const CPlayer *pPlayer) const
{
	return std::max(pPlayer->m_DieTick, pPlayer->m_PreviousDieTick + Server()->TickSpeed() * 3) + 2;
}

std::unique_ptr<IGameModeMapReloadState> CGameControllerDDRace::SaveStateForMapReload()
{
	return std::make_unique<CDDRaceMapReloadState>(GameServer());
}

void CGameControllerDDRace::RestoreCharacterAfterMapReload(CCharacter *pCharacter)
{
	auto *pState = dynamic_cast<CDDRaceMapReloadState *>(MapReloadState());
	if(!pState)
	{
		DiscardMapReloadState(pCharacter->GetPlayer()->GetCid());
		return;
	}
	pState->RestoreCharacter(GameServer(), DDRaceCharacter(pCharacter));
}

bool CGameControllerDDRace::CreateRaceMapEntity(IGameController &Controller, const CMapEntityContext &Context)
{
	auto &RaceController = static_cast<CGameControllerDDRace &>(Controller);
	return CreateDDRaceMapEntity(RaceController.GameServer(), Context.m_Index, Context.m_X, Context.m_Y, Context.m_Layer, Context.m_Flags, Context.m_Number);
}

void CGameControllerDDRace::OnPlayerConnect(CPlayer *pPlayer)
{
	RaceTeams().ResetPlayer(pPlayer->GetCid());
	IGameController::OnPlayerConnect(pPlayer);
	RaceScore().ResetPlayer(pPlayer->GetCid());
	RaceTeams().PlayerState(pPlayer->GetCid()).m_ShowOthers = g_Config.m_SvShowOthersDefault;
	if(!Server()->ClientPrevIngame(pPlayer->GetCid()) && g_Config.m_SvShowOthers && g_Config.m_SvShowOthersDefault > SHOW_OTHERS_OFF)
		GameServer()->SendChatTarget(pPlayer->GetCid(), "You can see other players. To disable this use DDNet client and type /showothers");
}

void CGameControllerDDRace::OnPlayerEnter(CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	RaceScore().BeginFinishEligibilityCheck(ClientId);
	RaceScore().SendMapInfoMessage(ClientId);
}

void CGameControllerDDRace::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	IGameController::OnPlayerDisconnect(pPlayer, pReason);
	RaceScore().ResetPlayer(pPlayer->GetCid());
	RaceTeams().ResetPlayer(pPlayer->GetCid());
}

bool CGameControllerDDRace::OnPlayerChatMessage(int ClientId, const char *pMessage, int Team)
{
	if(Team != 0 || str_comp(pMessage, "xd sure chillerbot.png is lyfe") != 0 || !RaceScore().FinishEligibilityCheckActive(ClientId))
		return false;

	RaceScore().SetNotEligibleForFinish(ClientId);
	dbg_msg("hack", "bot detected, cid=%d", ClientId);
	return true;
}

void CGameControllerDDRace::OnPlayerNameChanged(int ClientId)
{
	RaceScore().ResetPlayer(ClientId);
	Server()->SetClientScore(ClientId, std::nullopt);
	RaceScore().LoadPlayerData(ClientId);
}

void CGameControllerDDRace::OnPlayerDDNetVersionKnown(int ClientId)
{
	RaceTeams().SendTeamsState(ClientId);
	RaceScore().SendRecord(ClientId);
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

	CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
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

	CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
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
	if((Interaction.RestrictToDDRaceTeam() || Interaction.DDRaceTeam()) && RaceTeams().m_Core.Team(ClientId) != Interaction.DDRaceTeam())
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
	const CCharacterDDRace *pCharacter = DDRaceCharacter(GameServer()->GetPlayerChar(ClientId));
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

void CGameControllerDDRace::SnapCharacterMode(CCharacter *pCharacter, int SnappingClient, int TranslatedId)
{
	DDRaceCharacter(pCharacter)->SnapDDRace(SnappingClient, TranslatedId);
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

	CCharacterDDRace *pCharacter = DDRaceCharacter(pPlayer->GetCharacter());
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
		ShowSpec = ShowSpec && (RaceTeams().m_Core.Team(pPlayer->GetCid()) == RaceTeams().m_Core.Team(SnappingClient) || RaceTeams().PlayerState(SnappingClient).m_ShowOthers == SHOW_OTHERS_ON || pSnappingPlayer->GetTeam() == TEAM_SPECTATORS || pSnappingPlayer->IsPaused());
	}
	if(ShowSpec)
	{
		CNetObj_SpecChar SpecChar = {};
		SpecChar.m_X = pCharacter->Core()->m_Pos.x;
		SpecChar.m_Y = pCharacter->Core()->m_Pos.y;
		Server()->SnapNewItem(TranslatedId, SpecChar);
	}
}

void CGameControllerDDRace::Tick()
{
	IGameController::Tick();
	RaceTeams().ProcessSaveTeam();
	RaceTeams().Tick();
	RaceScore().Tick();
}

bool CGameControllerDDRace::IsTeamPractice(int Team) const
{
	return RaceTeams().IsPractice(Team);
}
