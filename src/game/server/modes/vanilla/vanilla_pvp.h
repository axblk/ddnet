#ifndef GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H
#define GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H

#include "player.h"

#include <game/server/gamecontroller.h>

class CGameControllerVanillaPvP : public IGameController
{
public:
	CGameControllerVanillaPvP(CGameServices &Services, const CGameModeInfo &GameModeInfo);

	CPlayer *CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team) override;
	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam) override;
	CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context) override;
	CGamePickupResult OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position) override;
	int PickupInitialSpawnDelaySeconds(int Type, int Subtype) const override;
	void OnCharacterSpawn(CCharacter *pChr) override;
	void OnPlayerConnect(CPlayer *pPlayer) override;
	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override;
	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override;
	void StartRound() override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;

protected:
	static int DeathScoreDelta(int VictimId, int KillerId, int Weapon, bool TeamKill = false);
	CPlayerVanilla *VanillaPlayer(int ClientId) const;
	void SetRespawnDelay(int VictimId, int Weapon);
	void DetachProjectiles(int ClientId);
	void CheckMatchEnd(int TopScore, bool Tied);
	CTuningParams DefaultTuning() const override;
	int GameInfoFlags(int SnappingClient) const override;
	int GameInfoFlags2(int SnappingClient) const override;
	int ScoreLimit() const override;
	int TimeLimit() const override;
};

#endif // GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H
