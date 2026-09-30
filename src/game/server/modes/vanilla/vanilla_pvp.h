#ifndef GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H
#define GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H

#include "player.h"

#include <game/server/gamecontroller.h>

class CGameControllerVanillaPvP : public IGameController
{
public:
	CGameControllerVanillaPvP(CGameServices &Services, const CGameModeInfo &GameModeInfo);

	CPlayer *CreatePlayer(uint32_t UniqueClientId, int ClientId, int Team) override;
	bool OnEntity(const CMapEntityContext &Context) override;
	bool OnCharacterTakeDamage(CCharacter *pVictim, vec2 Force, int Damage, int From, int Weapon, bool CanDamage, int AttackerTeam) override;
	CWeaponFireResult OnCharacterFireWeapon(const CWeaponFireContext &Context) override;
	CGamePickupResult OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position) override;
	int PickupInitialSpawnDelaySeconds(int Type, int Subtype) const override;
	void OnCharacterSpawn(CCharacter *pChr) override;
	void OnPlayerConnect(CPlayer *pPlayer) override;
	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override;
	bool IsSilentPlayer(const CPlayer *pPlayer) const override;
	void DoTeamChange(CPlayer *pPlayer, int Team, bool DoChatMsg) override;
	void StartRound() override;
	void Tick() override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;

	// as in 0.7: a restart after a warmup, right away for 0, and once everybody is ready for -1
	void RestartAfterWarmup(int Seconds) override;
	// as in 0.7: pausing without an end toggles, and a pause ends with a countdown
	void TogglePause() override;
	void DoPause(int Seconds) override;
	bool IsPausedWithoutEnd() const override;
	bool IsTeamChangeAllowed() const override;

protected:
	/**
	 * The world stands still for as long as sv_countdown says, 3 seconds in a
	 * survival mode for 0, before the game starts or goes on after a pause.
	 *
	 * @param Start Whether a match or a round starts, players can join it during the countdown.
	 */
	void StartCountdown(bool Start);
	bool IsCountdown() const { return m_PauseState == EPauseState::COUNTDOWN; }
	bool IsStartCountdown() const { return IsCountdown() && m_StartCountdown; }
	// the game goes on after the countdown, or right away without one
	virtual void OnCountdownEnd(bool Start) {}
	// what 0.6 clients are told while the countdown runs
	virtual void FormatCountdown(char *pBuf, int BufSize, int Seconds, bool Start) const;
	// once a match is started and not waiting for anything
	virtual void BeginMatch() { StartCountdown(true); }
	void UpdateGameDataSixup(protocol7::CNetObj_GameData &GameData, int SnappingClient) override;

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

private:
	enum class EPauseState
	{
		NONE,
		// by the pause command
		PAUSED,
		COUNTDOWN,
	};
	EPauseState m_PauseState = EPauseState::NONE;
	// until the pause or the countdown ends, -1 for a pause without an end
	int m_PauseTicks = 0;
	bool m_StartCountdown = false;

	void EndPause();
	void EndCountdown();
};

#endif // GAME_SERVER_MODES_VANILLA_VANILLA_PVP_H
