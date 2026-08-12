/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_TEAMS_H
#define GAME_SERVER_TEAMS_H

#include <engine/shared/protocol.h>

#include <game/race_state.h>
#include <game/server/gamecontext.h>
#include <game/server/save.h>
#include <game/team_state.h>
#include <game/teamscore.h>

#include <memory>
#include <optional>

class CCharacter;
class CCharacterDDRace;
class CPlayer;
class CScore;
struct CScoreSaveResult;

class CGameTeams
{
public:
	class CPlayerState
	{
	public:
		bool m_TeeStarted = false;
		bool m_TeeFinished = false;
		int m_LastChat = 0;
		uint64_t m_LastSwap = 0;
		int m_LastInvited = 0;
		std::optional<int64_t> m_LastTeamChange;
		bool m_VotedForPractice = false;
		int m_SwapTargetClientId = -1;
		int m_RescueMode = RESCUEMODE_AUTO;
		std::optional<CSaveTee> m_LastTeleTee;
		std::optional<CSaveTee> m_LastDeath;
		int m_ShowOthers = SHOW_OTHERS_ON;
		bool m_SpecTeam = false;
	};

private:
	// `m_TeeStarted` is used to keep track whether a given tee has hit the
	// start of the map yet. If a tee that leaves hasn't hit the start line
	// yet, the team will be marked as "not allowed to finish"
	// (`ETeamState::STARTED_UNFINISHABLE`). If this were not the case, tees
	// could go around the startline on a map, leave one tee behind at
	// start, go to the finish line, let the tee start and kill, allowing
	// the team to finish instantly.
	CPlayerState m_aPlayerState[MAX_CLIENTS];

	ETeamState m_aTeamState[NUM_DDRACE_TEAMS];
	bool m_aTeamLocked[NUM_DDRACE_TEAMS];
	bool m_aTeamFlock[NUM_DDRACE_TEAMS];
	CClientMask m_aInvited[NUM_DDRACE_TEAMS];
	bool m_aPractice[NUM_DDRACE_TEAMS];
	std::shared_ptr<CScoreSaveResult> m_apSaveTeamResult[NUM_DDRACE_TEAMS];
	bool m_aTeamSentStartWarning[NUM_DDRACE_TEAMS];
	// `m_aTeamUnfinishableKillTick` is -1 by default and gets set when a
	// team becomes unfinishable. If the team hasn't entered practice mode
	// by that time, it'll get killed to prevent people not understanding
	// the message from playing for a long time in an unfinishable team.
	int m_aTeamUnfinishableKillTick[NUM_DDRACE_TEAMS];

	// Team numbers as they are sent to clients before VERSION_DDNET_128_TEAMS, see UpdateLegacyTeamMap
	int m_aLegacyTeamMap[NUM_DDRACE_TEAMS];
	void UpdateLegacyTeamMap();

	CGameContext *m_pGameContext;
	CScore *m_pScore = nullptr;
	CScore &Score() const;

	/**
	 * Kill the whole team.
	 * @param Team The team id to kill
	 * @param NewStrongId The player with that id will get strong hook on everyone else, -1 will set the normal spawning order
	 * @param ExceptId The player that should not get killed
	 */
	void KillTeam(int Team, int NewStrongId, int ExceptId = -1);
	bool TeamFinished(int Team);
	void OnTeamFinish(int Team, CPlayer **Players, unsigned int Size, int TimeTicks, const char *pTimestamp);
	void OnFinish(CPlayer *Player, int TimeTicks, const char *pTimestamp);

public:
	CTeamsCore &m_Core;

	CGameTeams(CGameContext *pGameContext, CTeamsCore &TeamsCore);
	void SetScore(CScore *pScore) { m_pScore = pScore; }

	// helper methods
	CCharacterDDRace *Character(int ClientId);
	const CCharacterDDRace *Character(int ClientId) const;
	CPlayer *GetPlayer(int ClientId);
	CGameContext *GameServer();
	const CGameContext *GameServer() const;
	class IServer *Server();

	/**
	 * Translate a team number for a client that does not support all team numbers yet.
	 * @param Team The team id to translate
	 * @param ClientId The client the team number is sent to
	 */
	int TeamForClient(int Team, int ClientId) const;

	void OnCharacterStart(int ClientId);
	void OnCharacterFinish(int ClientId);
	void OnCharacterSpawn(int ClientId);
	void OnCharacterDeath(int ClientId, int Weapon);
	void Tick();

	// sets pError to an empty string on success (true)
	// and sets pError if it returns false
	bool CanJoinTeam(int ClientId, int Team, char *pError, int ErrorSize) const;

	// returns true if successful. Writes error into pError on failure
	bool SetCharacterTeam(int ClientId, int Team, char *pError, int ErrorSize);
	void CheckTeamFinished(int Team);

	void ChangeTeamState(int Team, ETeamState State);

	CClientMask TeamMask(int Team, int ExceptId = -1, int Asker = -1, int VersionFlags = CGameContext::FLAG_SIX | CGameContext::FLAG_SIXUP);

	int TeamSize(int Team) const;

	// need to be very careful using this method. SERIOUSLY...
	void SetForceCharacterTeam(int ClientId, int Team);

	void Reset();
	void ResetPlayer(int ClientId);
	void ResetRoundState(int Team);
	void ResetSwitchers(int Team);
	void SaveLastTeleport(CCharacterDDRace *pCharacter);
	bool LoadLastTeleport(CCharacterDDRace *pCharacter);

	void SendTeamsState(int ClientId);
	void SetTeamLock(int Team, bool Lock);
	void SetTeamFlock(int Team, bool Mode);
	void ResetInvited(int Team);
	void SetClientInvited(int Team, int ClientId, bool Invited);

	ERaceState GetDDRaceState(const CPlayer *Player) const;
	int GetStartTime(CPlayer *Player);
	float *GetCurrentTimeCp(CPlayer *Player);
	void SetDDRaceState(CPlayer *Player, ERaceState DDRaceState);
	void SetStartTime(CPlayer *Player, int StartTime);
	void SetLastTimeCp(CPlayer *Player, int LastTimeCp);
	void KillCharacterOrTeam(int ClientId, int Team);
	void ResetSavedTeam(int ClientId, int Team);
	void RequestTeamSwap(CPlayer *pPlayer, CPlayer *pTargetPlayer, int Team);
	void SwapTeamCharacters(CCharacterDDRace *pPrimaryCharacter, CCharacterDDRace *pTargetCharacter, int Team);
	void CancelTeamSwap(CPlayer *pPlayer, int Team);
	void ProcessSaveTeam();
	void SendSaveCode(int Team, int TeamSize, int State, const char *pError, const char *pSaveRequester, const char *pServerName, const char *pGeneratedCode, const char *pCode);
	std::optional<int> GetFirstEmptyTeam() const;
	bool TeeStarted(int ClientId) const;
	bool TeeFinished(int ClientId) const;
	ETeamState GetTeamState(int Team) const;
	bool TeamLocked(int Team) const;
	bool TeamFlock(int Team) const;
	bool IsInvited(int Team, int ClientId) const;
	bool IsStarted(int Team) const;
	void SetStarted(int ClientId, bool Started);
	void SetFinished(int ClientId, bool Finished);
	void SetSaving(int TeamId, std::shared_ptr<CScoreSaveResult> &SaveResult);
	bool GetSaving(int TeamId) const;
	void SetPractice(int Team, bool Enabled);
	bool IsPractice(int Team) const;
	bool PracticeByDefault() const;
	bool IsValidTeamNumber(int Team) const;
	CPlayerState &PlayerState(int ClientId) { return m_aPlayerState[ClientId]; }
	const CPlayerState &PlayerState(int ClientId) const { return m_aPlayerState[ClientId]; }
};

#endif
