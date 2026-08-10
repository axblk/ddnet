#ifndef GAME_SERVER_MODES_VANILLA_DM_H
#define GAME_SERVER_MODES_VANILLA_DM_H

#include <game/server/gamecontroller.h>

#include <array>

class CGameControllerVanillaDM : public IGameController
{
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

	CGameControllerVanillaDM(CGameContext *pGameServer, const CGameModeInfo &GameModeInfo);

	static CTuningParams DefaultTuning();
	static void ApplyDamage(int Damage, bool SelfDamage, int &Health, int &Armor);
	static void ApplyDeathScore(std::array<int, MAX_CLIENTS> &aScores, int VictimId, int KillerId, int Weapon);
	static EMatchResult EvaluateMatch(int NumTopScores, bool LimitReached, bool SuddenDeath);
	static vec2 ShotgunDirection(vec2 Direction, int Pellet, float SpeedDifference);
	void ResetTuning() override;
	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage) override;
	bool CanCharacterHitCharacter(CCharacter *pAttacker, CCharacter *pTarget) const override;
	CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context) override;
	CGamePickupResult OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position) override;
	int PickupInitialSpawnDelaySeconds(int Type, int Subtype) const override;
	CGameProjectileRules ProjectileRules(const CGameProjectileContext &Context) const override;
	int OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon) override;
	void OnCharacterSpawn(CCharacter *pChr) override;
	void OnPlayerConnect(CPlayer *pPlayer) override;
	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override;
	void StartRound() override;
	void Tick() override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	bool CanSpawn(int Team, vec2 *pOutPos, int ClientId) override;

protected:
	void InitGameSettings() override;
	int GameInfoFlags(int SnappingClient) const override;
	int GameInfoFlags2(int SnappingClient) const override;
	int ScoreLimit() const override;
	int TimeLimit() const override;
};

#endif // GAME_SERVER_MODES_VANILLA_DM_H
