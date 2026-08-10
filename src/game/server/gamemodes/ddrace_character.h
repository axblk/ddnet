#ifndef GAME_SERVER_GAMEMODES_DDRACE_CHARACTER_H
#define GAME_SERVER_GAMEMODES_DDRACE_CHARACTER_H

#include <game/server/entities/character.h>
#include <game/server/save.h>

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

public:
	using CCharacter::CCharacter;

	void Die(int Killer, int Weapon, bool SendKillMsg = true) override;
	void SetInvincible(bool Invincible);
	void SetSuper(bool Super);
	int TeamBeforeSuper() const { return m_TeamBeforeSuper; }
	bool Rescue();
	bool TrySetRescue(int RescueMode);
	CSaveTee &GetLastRescueTeeRef(int Mode = RESCUEMODE_AUTO) { return m_RescueTee[Mode]; }
	void DDRaceInit();
	void DDRaceTick();
	void DDRacePostCoreTick();
	void SnapDDRace(int SnappingClient, int TranslatedId);
};

#endif // GAME_SERVER_GAMEMODES_DDRACE_CHARACTER_H
