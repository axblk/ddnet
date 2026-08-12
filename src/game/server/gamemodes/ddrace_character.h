#ifndef GAME_SERVER_GAMEMODES_DDRACE_CHARACTER_H
#define GAME_SERVER_GAMEMODES_DDRACE_CHARACTER_H

#include <game/race_state.h>
#include <game/server/entities/character.h>
#include <game/server/save.h>

class CGameTeams;
class CScore;

class CCharacterDDRace : public CCharacter
{
	friend class CSaveTee;

private:
	void ForceSetRescue(int RescueMode);
	static bool IsSwitchActiveCb(unsigned char Number, void *pUser);
	void SetTimeCheckpoint(int TimeCheckpoint);
	void HandleTiles(int Index);
	void HandleSkippableTiles(int Index);
	void HandleBroadcast();
	void HandleTuneLayer();
	void SendZoneMsgs();

	float m_Time;
	int m_LastBroadcast;
	int m_TileIndex;
	int m_TileFIndex;
	bool m_LastRefillJumps;
	bool m_LastBonus;
	bool m_SetSavePos[NUM_RESCUEMODES];
	CSaveTee m_RescueTee[NUM_RESCUEMODES];
	int64_t m_LastRescue = 0;
	int m_TeamBeforeSuper;
	CGameTeams *m_pRaceTeams = nullptr;
	CScore *m_pRaceScore = nullptr;
	int64_t m_LastStartWarning = -1;

public:
	using CCharacter::CCharacter;

	ERaceState m_DDRaceState = ERaceState::NONE;
	int m_StartTime = 0;
	int m_TeleCheckpoint = 0;
	int m_TimeCpBroadcastEndTick = 0;
	int m_LastTimeCp = -1;
	int m_LastTimeCpBroadcasted = -1;
	float m_aCurrentTimeCp[MAX_CHECKPOINTS] = {};
	int m_TuneZoneOld = TuneZone::OVERRIDE_NONE;

	void PreTick() override;
	void Die(int Killer, int Weapon, bool SendKillMsg = true) override;
	void StopRecording() override;
	void SetInvincible(bool Invincible);
	void SetSuper(bool Super);
	void SetRaceTeams(CGameTeams *pTeams);
	void SetRaceScore(CScore *pScore) { m_pRaceScore = pScore; }
	CGameTeams *RaceTeams() { return m_pRaceTeams; }
	CScore *RaceScore() { return m_pRaceScore; }
	bool HasRaceTeams() const { return m_pRaceTeams != nullptr; }
	int TeamBeforeSuper() const { return m_TeamBeforeSuper; }
	bool Rescue();
	bool TrySetRescue(int RescueMode);
	bool TryStartWarning();
	void SendStartWarning(const char *pMessage);
	CTuningParams *GetTuning(int Zone) { return &TuningList()[Zone]; }
	CSaveTee &GetLastRescueTeeRef(int Mode = RESCUEMODE_AUTO) { return m_RescueTee[Mode]; }
	void DDRaceInit();
	void DDRaceTick();
	void DDRacePostCoreTick();
	void SnapDDRace(int SnappingClient, int TranslatedId);
};

#endif // GAME_SERVER_GAMEMODES_DDRACE_CHARACTER_H
