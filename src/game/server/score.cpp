#include "score.h"

#include "player.h"
#include "save.h"
#include "scoreworker.h"
#include "teams.h"

#include <base/dbg.h>
#include <base/io.h>
#include <base/math.h>
#include <base/secure.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/server.h>
#include <engine/server/databases/connection_pool.h>
#include <engine/shared/config.h>
#include <engine/shared/console.h>
#include <engine/shared/linereader.h>
#include <engine/shared/protocol.h>
#include <engine/storage.h>

#include <generated/protocol.h>
#include <generated/wordlist.h>

#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/gamemodes/ddnet.h>
#include <game/team_state.h>

#include <memory>

class IDbConnection;

CScore::CPlayerState *CScore::PlayerState(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(pPlayer == nullptr)
		return nullptr;

	CPlayerState &State = m_aPlayerStates[ClientId];
	if(State.m_UniqueClientId != pPlayer->GetUniqueCid())
	{
		State = {};
		State.m_UniqueClientId = pPlayer->GetUniqueCid();
		m_aPlayerData[ClientId].Reset();
	}
	return &State;
}

std::shared_ptr<CScorePlayerResult> CScore::NewSqlPlayerResult(int ClientId)
{
	CPlayerState *pState = PlayerState(ClientId);
	if(pState == nullptr || pState->m_pQueryResult != nullptr) // TODO: send player a message: "too many requests"
		return nullptr;
	pState->m_pQueryResult = std::make_shared<CScorePlayerResult>();
	return pState->m_pQueryResult;
}

void CScore::ExecPlayerThread(
	bool (*pFuncPtr)(IDbConnection *, const ISqlData *, char *pError, int ErrorSize),
	const char *pThreadName,
	int ClientId,
	const char *pName,
	int Offset)
{
	auto pResult = NewSqlPlayerResult(ClientId);
	if(pResult == nullptr)
		return;
	auto Tmp = std::make_unique<CSqlPlayerRequest>(pResult);
	str_copy(Tmp->m_aName, pName);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName());
	str_copy(Tmp->m_aServer, g_Config.m_SvSqlServerName);
	str_copy(Tmp->m_aRequestingPlayer, Server()->ClientName(ClientId));
	Tmp->m_Offset = Offset;

	m_pPool->Execute(pFuncPtr, std::move(Tmp), pThreadName);
}

bool CScore::RateLimitPlayer(int ClientId)
{
	CPlayerState *pState = PlayerState(ClientId);
	if(pState == nullptr)
		return true;
	if(pState->m_LastSqlQuery + (int64_t)g_Config.m_SvSqlQueriesDelay * Server()->TickSpeed() >= Server()->Tick())
		return true;
	pState->m_LastSqlQuery = Server()->Tick();
	return false;
}

void CScore::GeneratePassphrase(char *pBuf, int BufSize)
{
	for(int i = 0; i < 3; i++)
	{
		if(i != 0)
			str_append(pBuf, " ", BufSize);
		// TODO: decide if the slight bias towards lower numbers is ok
		int Rand = m_Prng.RandomBits() % m_vWordlist.size();
		str_append(pBuf, m_vWordlist[Rand].c_str(), BufSize);
	}
}

CScore::CScore(CGameContext *pGameServer, CDbConnectionPool *pPool) :
	m_pPool(pPool),
	m_pGameServer(pGameServer),
	m_pServer(pGameServer->Server())
{
	LoadBestTime();

	uint64_t aSeed[2];
	secure_random_fill(aSeed, sizeof(aSeed));
	m_Prng.Seed(aSeed);

	CLineReader LineReader;
	if(LineReader.OpenFile(GameServer()->Storage()->OpenFile("wordlist.txt", IOFLAG_READ, IStorage::TYPE_ALL)))
	{
		while(const char *pLine = LineReader.Get())
		{
			char aWord[32] = {0};
			sscanf(pLine, "%*s %31s", aWord);
			aWord[31] = 0;
			m_vWordlist.emplace_back(aWord);
		}
	}
	else
	{
		dbg_msg("sql", "failed to open wordlist, using fallback");
		m_vWordlist.assign(std::begin(g_aFallbackWordlist), std::end(g_aFallbackWordlist));
	}

	if(m_vWordlist.size() < 1000)
	{
		dbg_msg("sql", "too few words in wordlist");
		Server()->SetErrorShutdown("sql too few words in wordlist");
		return;
	}
}

void CScore::Tick()
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CPlayerState *pState = PlayerState(ClientId);
		if(pState == nullptr)
		{
			m_aPlayerStates[ClientId] = {};
			continue;
		}

		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(pState->m_pQueryResult != nullptr && pState->m_pQueryResult->m_Completed && pPlayer->m_SentSnaps >= 3)
		{
			ProcessPlayerResult(ClientId, *pState->m_pQueryResult);
			pState->m_pQueryResult = nullptr;
		}
		if(pState->m_pFinishResult != nullptr && pState->m_pFinishResult->m_Completed)
		{
			ProcessPlayerResult(ClientId, *pState->m_pFinishResult);
			pState->m_pFinishResult = nullptr;
		}
	}

	if(m_pLoadBestTimeResult != nullptr && m_pLoadBestTimeResult->m_Completed)
	{
		if(m_pLoadBestTimeResult->m_Success)
		{
			m_CurrentRecord = m_pLoadBestTimeResult->m_CurrentRecord;
			for(int i = 0; i < MAX_CLIENTS; i++)
			{
				if(GameServer()->m_apPlayers[i] && GameServer()->m_apPlayers[i]->GetClientVersion() >= VERSION_DDRACE)
					SendRecord(i);
			}
		}
		m_pLoadBestTimeResult = nullptr;
	}

	if(m_pRandomMapResult != nullptr && m_pRandomMapResult->m_Completed)
	{
		if(m_pRandomMapResult->m_Success)
		{
			if(m_pRandomMapResult->m_ClientId != -1 && GameServer()->m_apPlayers[m_pRandomMapResult->m_ClientId] && m_pRandomMapResult->m_aMessage[0] != '\0')
				GameServer()->SendChat(-1, TEAM_ALL, m_pRandomMapResult->m_aMessage);
			if(m_pRandomMapResult->m_aMap[0] != '\0')
				Server()->ChangeMap(m_pRandomMapResult->m_aMap);
			else
				GameServer()->m_LastMapVote = 0;
		}
		m_pRandomMapResult = nullptr;
	}

	if(m_pLoadMapInfoResult != nullptr && m_pLoadMapInfoResult->m_Completed)
	{
		if(m_pLoadMapInfoResult->m_Success && m_pLoadMapInfoResult->m_Data.m_aaMessages[0][0] != '\0')
		{
			str_copy(m_aMapInfoMessage, m_pLoadMapInfoResult->m_Data.m_aaMessages[0]);
			SendMapInfoMessage(-1);
		}
		m_pLoadMapInfoResult = nullptr;
	}
}

void CScore::ResetPlayer(int ClientId)
{
	m_aPlayerData[ClientId].Reset();
	m_aPlayerStates[ClientId] = {};
	if(GameServer()->m_apPlayers[ClientId] != nullptr)
		m_aPlayerStates[ClientId].m_UniqueClientId = GameServer()->m_apPlayers[ClientId]->GetUniqueCid();
}

void CScore::BeginFinishEligibilityCheck(int ClientId)
{
	CPlayerState *pState = PlayerState(ClientId);
	dbg_assert(pState != nullptr, "finish eligibility requires an active player");
	pState->m_FinishEligibilityCheck = time_get();
}

bool CScore::FinishEligibilityCheckActive(int ClientId)
{
	CPlayerState *pState = PlayerState(ClientId);
	dbg_assert(pState != nullptr, "finish eligibility requires an active player");
	return !pState->m_NotEligibleForFinish && pState->m_FinishEligibilityCheck.has_value() && pState->m_FinishEligibilityCheck.value() + 10 * time_freq() >= time_get();
}

bool CScore::NotEligibleForFinish(int ClientId)
{
	CPlayerState *pState = PlayerState(ClientId);
	dbg_assert(pState != nullptr, "finish eligibility requires an active player");
	return pState->m_NotEligibleForFinish;
}

void CScore::SetNotEligibleForFinish(int ClientId)
{
	CPlayerState *pState = PlayerState(ClientId);
	dbg_assert(pState != nullptr, "finish eligibility requires an active player");
	pState->m_NotEligibleForFinish = true;
}

void CScore::ProcessPlayerResult(int ClientId, CScorePlayerResult &Result)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	dbg_assert(pPlayer != nullptr, "score result requires an active player");
	CPlayerState *pState = PlayerState(ClientId);
	dbg_assert(pState != nullptr, "score result requires player state");

	if(!Result.m_Success)
		return;

	switch(Result.m_MessageKind)
	{
	case CScorePlayerResult::DIRECT:
		for(auto &aMessage : Result.m_Data.m_aaMessages)
		{
			if(aMessage[0] == 0)
				break;
			GameServer()->SendChatTarget(ClientId, aMessage);
		}
		break;
	case CScorePlayerResult::ALL:
	{
		bool PrimaryMessage = true;
		for(auto &aMessage : Result.m_Data.m_aaMessages)
		{
			if(aMessage[0] == 0)
				break;

			if(GameServer()->ProcessSpamProtection(ClientId) && PrimaryMessage)
				break;

			GameServer()->SendChat(-1, TEAM_ALL, aMessage, -1);
			PrimaryMessage = false;
		}
		break;
	}
	case CScorePlayerResult::BROADCAST:
		if(Result.m_Data.m_aBroadcast[0] != 0)
			GameServer()->SendBroadcast(Result.m_Data.m_aBroadcast, -1);
		break;
	case CScorePlayerResult::MAP_VOTE:
		GameServer()->m_VoteType = CGameContext::VOTE_TYPE_OPTION;
		GameServer()->m_LastMapVote = time_get();

		char aCmd[256];
		str_format(aCmd, sizeof(aCmd),
			"sv_reset_file types/%s/flexreset.cfg; change_map \"%s\"",
			Result.m_Data.m_MapVote.m_aServer, Result.m_Data.m_MapVote.m_aMap);

		char aChatmsg[512];
		str_format(aChatmsg, sizeof(aChatmsg), "'%s' called vote to change server option '%s' (%s)",
			Server()->ClientName(ClientId), Result.m_Data.m_MapVote.m_aMap, "/map");

		GameServer()->CallVote(ClientId, Result.m_Data.m_MapVote.m_aMap, aCmd, "/map", aChatmsg);
		break;
	case CScorePlayerResult::PLAYER_INFO:
	{
		if(Result.m_Data.m_Info.m_Time.has_value())
		{
			PlayerData(ClientId)->Set(Result.m_Data.m_Info.m_Time.value(), Result.m_Data.m_Info.m_aTimeCp);
			Server()->SetClientScore(ClientId, Result.m_Data.m_Info.m_Time.value());
			if(!CurrentRecord().has_value() || Result.m_Data.m_Info.m_Time.value() < CurrentRecord().value())
				LoadBestTime();
		}
		Server()->ExpireServerInfo();
		int Birthday = Result.m_Data.m_Info.m_Birthday;
		if(Birthday != 0 && !pState->m_BirthdayAnnounced && pPlayer->GetCharacter())
		{
			char aBuf[512];
			str_format(aBuf, sizeof(aBuf),
				"Happy DDNet birthday to %s for finishing their first map %d year%s ago!",
				Server()->ClientName(ClientId), Birthday, Birthday > 1 ? "s" : "");
			GameServer()->SendChat(-1, TEAM_ALL, aBuf, ClientId);
			str_format(aBuf, sizeof(aBuf),
				"Happy DDNet birthday, %s!\nYou have finished your first map exactly %d year%s ago!",
				Server()->ClientName(ClientId), Birthday, Birthday > 1 ? "s" : "");
			GameServer()->SendBroadcast(aBuf, ClientId);
			pState->m_BirthdayAnnounced = true;

			GameServer()->CreateBirthdayEffect(pPlayer->GetCharacter()->m_Pos, pPlayer->GetCharacter()->TeamMask());
		}
		SendRecord(ClientId);
		break;
	}
	case CScorePlayerResult::PLAYER_TIMECP:
		PlayerData(ClientId)->SetBestTimeCp(Result.m_Data.m_Info.m_aTimeCp);
		char aBuf[128], aTime[32];
		str_time_float(Result.m_Data.m_Info.m_Time.value(), ETimeFormat::HOURS_CENTISECS, aTime, sizeof(aTime));
		str_format(aBuf, sizeof(aBuf), "Showing the checkpoint times for '%s' with a race time of %s", Result.m_Data.m_Info.m_aRequestedPlayer, aTime);
		GameServer()->SendChatTarget(ClientId, aBuf);
		break;
	}
}

void CScore::SendMapInfoMessage(int ClientId) const
{
	if(m_aMapInfoMessage[0] == '\0')
		return;
	CNetMsg_Sv_MapInfo Msg;
	Msg.m_pDescription = m_aMapInfoMessage;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL | MSGFLAG_NORECORD, ClientId);
}

void CScore::SendRecord(int ClientId)
{
	if(Server()->IsSixup(ClientId) || GameServer()->GetClientVersion(ClientId) >= VERSION_DDNET_MAP_BESTTIME)
		return;

	CNetMsg_Sv_Record Msg;
	CNetMsg_Sv_RecordLegacy MsgLegacy;
	MsgLegacy.m_PlayerTimeBest = Msg.m_PlayerTimeBest = round_to_int(PlayerData(ClientId)->m_BestTime.value_or(0.0f) * 100.0f);
	const std::optional<float> &CurrentRecord = this->CurrentRecord();
	MsgLegacy.m_ServerTimeBest = Msg.m_ServerTimeBest = CurrentRecord.has_value() && !g_Config.m_SvHideScore ? round_to_int(CurrentRecord.value() * 100.0f) : 0;
	Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
	if(GameServer()->GetClientVersion(ClientId) < VERSION_DDNET_MSG_LEGACY)
	{
		Server()->SendPackMsg(&MsgLegacy, MSGFLAG_VITAL, ClientId);
	}
}

void CScore::SendFinish(int ClientId, float Time, std::optional<float> PreviousBestTime)
{
	int ClientVersion = GameServer()->m_apPlayers[ClientId]->GetClientVersion();

	if(!Server()->IsSixup(ClientId))
	{
		CNetMsg_Sv_DDRaceTime Msg;
		CNetMsg_Sv_DDRaceTimeLegacy MsgLegacy;
		MsgLegacy.m_Time = Msg.m_Time = (int)(Time * 100.0f);
		MsgLegacy.m_Check = Msg.m_Check = 0;
		MsgLegacy.m_Finish = Msg.m_Finish = 1;

		if(PreviousBestTime.has_value())
		{
			float Diff100 = (Time - PreviousBestTime.value()) * 100;
			MsgLegacy.m_Check = Msg.m_Check = (int)Diff100;
		}
		if(VERSION_DDRACE <= ClientVersion)
		{
			if(ClientVersion < VERSION_DDNET_MSG_LEGACY)
			{
				Server()->SendPackMsg(&Msg, MSGFLAG_VITAL, ClientId);
			}
			else
			{
				Server()->SendPackMsg(&MsgLegacy, MSGFLAG_VITAL, ClientId);
			}
		}
	}

	CNetMsg_Sv_RaceFinish RaceFinishMsg;
	RaceFinishMsg.m_ClientId = ClientId;
	RaceFinishMsg.m_Time = Time * 1000;
	RaceFinishMsg.m_Diff = 0;
	if(PreviousBestTime.has_value())
	{
		float Diff = absolute(Time - PreviousBestTime.value());
		RaceFinishMsg.m_Diff = Diff * 1000 * (Time < PreviousBestTime.value() ? -1 : 1);
	}
	RaceFinishMsg.m_RecordPersonal = (!PreviousBestTime.has_value() || Time < PreviousBestTime.value());
	RaceFinishMsg.m_RecordServer = Time < CurrentRecord();
	Server()->SendPackMsg(&RaceFinishMsg, MSGFLAG_VITAL | MSGFLAG_NORECORD, g_Config.m_SvHideScore ? ClientId : -1);
}

void CScore::LoadBestTime()
{
	if(m_pLoadBestTimeResult)
		return; // already in progress

	auto LoadBestTimeResult = std::make_shared<CScoreLoadBestTimeResult>();
	m_pLoadBestTimeResult = LoadBestTimeResult;

	auto Tmp = std::make_unique<CSqlLoadBestTimeRequest>(LoadBestTimeResult);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName());
	m_pPool->Execute(CScoreWorker::LoadBestTime, std::move(Tmp), "load best time");
}

void CScore::LoadMapInfo()
{
	if(m_pLoadMapInfoResult)
		return; // already in progress

	auto pResult = std::make_shared<CScorePlayerResult>();
	m_pLoadMapInfoResult = pResult;

	auto Tmp = std::make_unique<CSqlPlayerRequest>(pResult);
	str_copy(Tmp->m_aName, GameServer()->Map()->BaseName());
	Tmp->m_aRequestingPlayer[0] = '\0'; // no player, so no "your time" in result
	m_pPool->Execute(CScoreWorker::MapInfo, std::move(Tmp), "load map info");
}

void CScore::LoadPlayerData(int ClientId, const char *pName)
{
	ExecPlayerThread(CScoreWorker::LoadPlayerData, "load player data", ClientId, pName, 0);
}

void CScore::LoadPlayerTimeCp(int ClientId, const char *pName)
{
	ExecPlayerThread(CScoreWorker::LoadPlayerTimeCp, "load player timecp", ClientId, pName, 0);
}

void CScore::MapVote(int ClientId, const char *pMapName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::MapVote, "map vote", ClientId, pMapName, 0);
}

void CScore::MapInfo(int ClientId, const char *pMapName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::MapInfo, "map info", ClientId, pMapName, 0);
}

void CScore::SaveScore(int ClientId, int TimeTicks, const char *pTimestamp, const float aTimeCp[NUM_CHECKPOINTS], bool NotEligible)
{
	CConsole *pCon = (CConsole *)GameServer()->Console();
	if(pCon->Cheated() || NotEligible)
		return;

	GameServer()->TeehistorianRecordPlayerFinish(ClientId, TimeTicks);

	CPlayerState *pState = PlayerState(ClientId);
	dbg_assert(pState != nullptr, "saving a score requires an active player");
	if(pState->m_pFinishResult != nullptr)
		dbg_msg("sql", "WARNING: previous save score result didn't complete, overwriting it now");
	pState->m_pFinishResult = std::make_shared<CScorePlayerResult>();
	auto Tmp = std::make_unique<CSqlScoreData>(pState->m_pFinishResult);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName());
	FormatUuid(GameServer()->GameUuid(), Tmp->m_aGameUuid, sizeof(Tmp->m_aGameUuid));
	Tmp->m_ClientId = ClientId;
	str_copy(Tmp->m_aName, Server()->ClientName(ClientId));
	Tmp->m_Time = (float)(TimeTicks) / (float)Server()->TickSpeed();
	str_copy(Tmp->m_aTimestamp, pTimestamp);
	for(int i = 0; i < NUM_CHECKPOINTS; i++)
		Tmp->m_aCurrentTimeCp[i] = aTimeCp[i];

	m_pPool->ExecuteWrite(CScoreWorker::SaveScore, std::move(Tmp), "save score");
}

void CScore::SaveTeamScore(int Team, int *pClientIds, unsigned int Size, int TimeTicks, const char *pTimestamp)
{
	CConsole *pCon = (CConsole *)GameServer()->Console();
	if(pCon->Cheated())
		return;
	for(unsigned int i = 0; i < Size; i++)
	{
		if(NotEligibleForFinish(pClientIds[i]))
			return;
	}

	GameServer()->TeehistorianRecordTeamFinish(Team, TimeTicks);

	auto Tmp = std::make_unique<CSqlTeamScoreData>();
	for(unsigned int i = 0; i < Size; i++)
		str_copy(Tmp->m_aaNames[i], Server()->ClientName(pClientIds[i]));
	Tmp->m_Size = Size;
	Tmp->m_Time = (float)TimeTicks / (float)Server()->TickSpeed();
	str_copy(Tmp->m_aTimestamp, pTimestamp);
	FormatUuid(GameServer()->GameUuid(), Tmp->m_aGameUuid, sizeof(Tmp->m_aGameUuid));
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName());
	Tmp->m_TeamrankUuid = RandomUuid();

	m_pPool->ExecuteWrite(CScoreWorker::SaveTeamScore, std::move(Tmp), "save team score");
}

void CScore::ShowRank(int ClientId, const char *pName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowRank, "show rank", ClientId, pName, 0);
}

void CScore::ShowTeamRank(int ClientId, const char *pName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTeamRank, "show team rank", ClientId, pName, 0);
}

void CScore::ShowTop(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTop, "show top5", ClientId, "", Offset);
}

void CScore::ShowTeamTop5(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTeamTop5, "show team top5", ClientId, "", Offset);
}

void CScore::ShowPlayerTeamTop5(int ClientId, const char *pName, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowPlayerTeamTop5, "show team top5 player", ClientId, pName, Offset);
}

void CScore::ShowTimes(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTimes, "show times", ClientId, "", Offset);
}

void CScore::ShowTimes(int ClientId, const char *pName, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTimes, "show times", ClientId, pName, Offset);
}

void CScore::ShowPoints(int ClientId, const char *pName)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowPoints, "show points", ClientId, pName, 0);
}

void CScore::ShowTopPoints(int ClientId, int Offset)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::ShowTopPoints, "show top points", ClientId, "", Offset);
}

void CScore::RandomMap(int ClientId, int MinStars, int MaxStars)
{
	auto pResult = std::make_shared<CScoreRandomMapResult>(ClientId);
	m_pRandomMapResult = pResult;

	auto Tmp = std::make_unique<CSqlRandomMapRequest>(pResult);
	Tmp->m_MinStars = MinStars;
	Tmp->m_MaxStars = MaxStars;
	str_copy(Tmp->m_aCurrentMap, GameServer()->Map()->BaseName());
	str_copy(Tmp->m_aServerType, g_Config.m_SvServerType);
	str_copy(Tmp->m_aRequestingPlayer, ClientId == -1 ? "nameless tee" : GameServer()->Server()->ClientName(ClientId));

	m_pPool->Execute(CScoreWorker::RandomMap, std::move(Tmp), "random map");
}

void CScore::RandomUnfinishedMap(int ClientId, int MinStars, int MaxStars)
{
	auto pResult = std::make_shared<CScoreRandomMapResult>(ClientId);
	m_pRandomMapResult = pResult;

	auto Tmp = std::make_unique<CSqlRandomMapRequest>(pResult);
	Tmp->m_MinStars = MinStars;
	Tmp->m_MaxStars = MaxStars;
	str_copy(Tmp->m_aCurrentMap, GameServer()->Map()->BaseName());
	str_copy(Tmp->m_aServerType, g_Config.m_SvServerType);
	str_copy(Tmp->m_aRequestingPlayer, ClientId == -1 ? "nameless tee" : GameServer()->Server()->ClientName(ClientId));

	m_pPool->Execute(CScoreWorker::RandomUnfinishedMap, std::move(Tmp), "random unfinished map");
}

void CScore::SaveTeam(int ClientId, const char *pCode, const char *pServer)
{
	if(RateLimitPlayer(ClientId))
		return;
	CGameTeams *pTeams = GameServer()->RaceTeams();
	int Team = pTeams->m_Core.Team(ClientId);
	if(pTeams->GetSaving(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team save already in progress");
		return;
	}
	if(pTeams->IsPractice(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team save disabled for teams in practice mode");
		return;
	}

	auto SaveResult = std::make_shared<CScoreSaveResult>(ClientId, Server()->ClientName(ClientId), pServer);
	SaveResult->m_SaveId = RandomUuid();
	ESaveResult Result = SaveResult->m_SavedTeam.Save(GameServer(), Team);
	if(CSaveTeam::HandleSaveError(Result, ClientId, GameServer()))
		return;
	pTeams->SetSaving(Team, SaveResult);

	auto Tmp = std::make_unique<CSqlTeamSaveData>(SaveResult);
	str_copy(Tmp->m_aCode, pCode);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName());
	str_copy(Tmp->m_aServer, pServer);
	str_copy(Tmp->m_aClientName, this->Server()->ClientName(ClientId));
	Tmp->m_aGeneratedCode[0] = '\0';
	GeneratePassphrase(Tmp->m_aGeneratedCode, sizeof(Tmp->m_aGeneratedCode));

	pTeams->KillCharacterOrTeam(ClientId, Team);

	pTeams->SendSaveCode(
		Team,
		SaveResult->m_SavedTeam.GetMembersCount(),
		SAVESTATE_PENDING,
		"",
		SaveResult->m_aRequestingPlayer,
		Tmp->m_aServer,
		Tmp->m_aGeneratedCode,
		Tmp->m_aCode);

	m_pPool->ExecuteWrite(CScoreWorker::SaveTeam, std::move(Tmp), "save team");
}

void CScore::LoadTeam(const char *pCode, int ClientId)
{
	if(RateLimitPlayer(ClientId))
		return;
	CGameTeams *pTeams = GameServer()->RaceTeams();
	int Team = pTeams->m_Core.Team(ClientId);
	if(pTeams->GetSaving(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team load already in progress");
		return;
	}
	if(!pTeams->IsValidTeamNumber(Team) || (g_Config.m_SvTeam != SV_TEAM_FORCED_SOLO && Team == TEAM_FLOCK))
	{
		GameServer()->SendChatTarget(ClientId, "You have to be in a team (from 1-63)");
		return;
	}
	if(pTeams->GetTeamState(Team) != ETeamState::OPEN)
	{
		GameServer()->SendChatTarget(ClientId, "Team can't be loaded while racing");
		return;
	}
	if(pTeams->TeamFlock(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team can't be loaded while in team 0 mode");
		return;
	}
	if(pTeams->IsPractice(Team))
	{
		GameServer()->SendChatTarget(ClientId, "Team can't be loaded while practice is enabled");
		return;
	}
	auto SaveResult = std::make_shared<CScoreSaveResult>(ClientId, Server()->ClientName(ClientId), g_Config.m_SvSqlServerName);
	SaveResult->m_Status = CScoreSaveResult::LOAD_FAILED;
	pTeams->SetSaving(Team, SaveResult);
	auto Tmp = std::make_unique<CSqlTeamLoadRequest>(SaveResult);
	str_copy(Tmp->m_aCode, pCode);
	str_copy(Tmp->m_aMap, GameServer()->Map()->BaseName());
	str_copy(Tmp->m_aRequestingPlayer, Server()->ClientName(ClientId));
	Tmp->m_NumPlayer = 0;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(pTeams->m_Core.Team(i) == Team)
		{
			// put all names at the beginning of the array
			str_copy(Tmp->m_aClientNames[Tmp->m_NumPlayer], Server()->ClientName(i));
			Tmp->m_aClientId[Tmp->m_NumPlayer] = i;
			Tmp->m_NumPlayer++;
		}
	}
	m_pPool->ExecuteWrite(CScoreWorker::LoadTeam, std::move(Tmp), "load team");
}

void CScore::GetSaves(int ClientId)
{
	if(RateLimitPlayer(ClientId))
		return;
	ExecPlayerThread(CScoreWorker::GetSaves, "get saves", ClientId, "", 0);
}
