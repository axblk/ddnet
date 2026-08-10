#ifndef GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H
#define GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H

#include <game/server/gamecontroller.h>

#include <array>

class CGameControllerVanillaPvP : public IGameController
{
protected:
	std::array<int, MAX_CLIENTS> m_aScores{};
	std::array<int, MAX_CLIENTS> m_aEarliestRespawnTicks{};
	std::array<int, MAX_CLIENTS> m_aLastNoAmmoSoundTicks{};

public:
	enum class EMatchResult
	{
		RUNNING,
		SUDDEN_DEATH,
		END_ROUND,
	};

	CGameControllerVanillaPvP(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo);

	static CTuningParams DefaultTuning();
	static void ApplyDamage(int Damage, bool SelfDamage, int &Health, int &Armor);
	static void ApplyDeathScore(std::array<int, MAX_CLIENTS> &aScores, int VictimId, int KillerId, int Weapon, bool TeamKill = false);
	static EMatchResult EvaluateMatch(int NumTopScores, bool LimitReached, bool SuddenDeath);
	static vec2 ShotgunDirection(vec2 Direction, int Pellet, float SpeedDifference);
	void ResetTuning() override;
	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam) override;
	bool CanCharacterHitCharacter(CCharacter *pAttacker, CCharacter *pTarget) const override;
	CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context) override;
	CGamePickupResult OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position) override;
	int PickupInitialSpawnDelaySeconds(int Type, int Subtype) const override;
	CGameProjectileRules ProjectileRules(const CGameProjectileContext &Context) const override;
	void OnCharacterSpawn(CCharacter *pChr) override;
	void OnPlayerConnect(CPlayer *pPlayer) override;
	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override;
	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override;
	void StartRound() override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;

protected:
	void DetachProjectiles(int ClientId);
	void InitGameSettings() override;
	int GameInfoFlags(int SnappingClient) const override;
	int GameInfoFlags2(int SnappingClient) const override;
	int ScoreLimit() const override;
	int TimeLimit() const override;
};

#endif // GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H
