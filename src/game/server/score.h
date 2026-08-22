#ifndef GAME_SERVER_SCORE_H
#define GAME_SERVER_SCORE_H

#include "scoreworker.h"

#include <game/prng.h>

class CDbConnectionPool;
class CGameContext;
class CGameTeams;
class IDbConnection;
class IServer;
struct ISqlData;

class CScore
{
	class CPlayerState
	{
	public:
		uint32_t m_UniqueClientId = 0;
		int64_t m_LastSqlQuery = 0;
		std::shared_ptr<CScorePlayerResult> m_pQueryResult;
		std::shared_ptr<CScorePlayerResult> m_pFinishResult;
		bool m_NotEligibleForFinish = false;
		std::optional<int64_t> m_FinishEligibilityCheck;
		bool m_BirthdayAnnounced = false;
	};

	CPlayerData m_aPlayerData[MAX_CLIENTS];
	CPlayerState m_aPlayerStates[MAX_CLIENTS];
	CDbConnectionPool *m_pPool;
	CGameTeams *m_pTeams;

	CGameContext *GameServer() const { return m_pGameServer; }
	IServer *Server() const { return m_pServer; }
	CGameContext *m_pGameServer;
	IServer *m_pServer;

	std::vector<std::string> m_vWordlist;
	CPrng m_Prng;
	std::optional<float> m_CurrentRecord;
	std::shared_ptr<CScoreLoadBestTimeResult> m_pLoadBestTimeResult;
	std::shared_ptr<CScoreRandomMapResult> m_pRandomMapResult;
	std::shared_ptr<CScorePlayerResult> m_pLoadMapInfoResult;
	char m_aMapInfoMessage[512] = {};
	void GeneratePassphrase(char *pBuf, int BufSize);
	CPlayerState *PlayerState(int ClientId);
	void ProcessPlayerResult(int ClientId, CScorePlayerResult &Result);

	// returns new SqlResult bound to the player, if no current Thread is active for this player
	std::shared_ptr<CScorePlayerResult> NewSqlPlayerResult(int ClientId);
	// Creates for player database requests
	void ExecPlayerThread(
		bool (*pFuncPtr)(IDbConnection *, const ISqlData *, char *pError, int ErrorSize),
		const char *pThreadName,
		int ClientId,
		const char *pName,
		int Offset);

	// returns true if the player should be rate limited
	bool RateLimitPlayer(int ClientId);

public:
	CScore(CGameContext *pGameServer, CDbConnectionPool *pPool, CGameTeams *pTeams);

	CPlayerData *PlayerData(int Id) { return &m_aPlayerData[Id]; }
	const std::optional<float> &CurrentRecord() const { return m_CurrentRecord; }
	void SetCurrentRecord(float Time) { m_CurrentRecord = Time; }
	const char *MapInfoMessage() const { return m_aMapInfoMessage; }
	void Tick();
	void ResetPlayer(int ClientId);
	void BeginFinishEligibilityCheck(int ClientId);
	bool FinishEligibilityCheckActive(int ClientId);
	bool NotEligibleForFinish(int ClientId);
	void SetNotEligibleForFinish(int ClientId);
	void SendRecord(int ClientId);
	void SendFinish(int ClientId, float Time, std::optional<float> PreviousBestTime);
	void SendMapInfoMessage(int ClientId) const;

	void LoadBestTime();
	void LoadMapInfo();
	void MapInfo(int ClientId, const char *pMapName);
	void MapVote(int ClientId, const char *pMapName);
	void LoadPlayerData(int ClientId, const char *pName = "");
	void LoadPlayerTimeCp(int ClientId, const char *pName = "");
	void SaveScore(int ClientId, int TimeTicks, const char *pTimestamp, const float aTimeCp[NUM_CHECKPOINTS], bool NotEligible);

	void SaveTeamScore(int Team, int *pClientIds, unsigned int Size, int TimeTicks, const char *pTimestamp);

	void ShowTop(int ClientId, int Offset = 1);
	void ShowRank(int ClientId, const char *pName);

	void ShowTeamTop5(int ClientId, int Offset = 1);
	void ShowPlayerTeamTop5(int ClientId, const char *pName, int Offset = 1);
	void ShowTeamRank(int ClientId, const char *pName);

	void ShowTopPoints(int ClientId, int Offset = 1);
	void ShowPoints(int ClientId, const char *pName);

	void ShowTimes(int ClientId, const char *pName, int Offset = 1);
	void ShowTimes(int ClientId, int Offset = 1);

	void RandomMap(int ClientId, int MinStars, int MaxStars);
	void RandomUnfinishedMap(int ClientId, int MinStars, int MaxStars);

	void SaveTeam(int ClientId, const char *pCode, const char *pServer);
	void LoadTeam(const char *pCode, int ClientId);
	void GetSaves(int ClientId);
};

#endif // GAME_SERVER_SCORE_H
