/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_GAMEMODES_DDRACE_H
#define GAME_SERVER_GAMEMODES_DDRACE_H

#include "ddrace_character.h"

#include <game/server/gamecontroller.h>

class CDbConnectionPool;
class CGameTeams;
class CScore;

class CGameControllerDDRace : public IGameController
{
public:
	CGameControllerDDRace(CGameServices &Services, const CGameModeInfo &GameModeInfo);
	~CGameControllerDDRace() override;

	void Init(CDbConnectionPool *pDbPool) override;
	CGameTeams &RaceTeams();
	const CGameTeams &RaceTeams() const;
	CScore &RaceScore();
	const CScore &RaceScore() const;
	CCharacterDDRace *CreateCharacter(CPlayer *pPlayer) override;
	bool CanCharacterHitCharacter(CCharacter *pAttacker, CCharacter *pTarget) const override;
	CGamePickupResult OnCharacterPickup(CCharacter *pCharacter, int Type, int Subtype, vec2 Position) override;
	CGameProjectileRules ProjectileRules(const CGameProjectileContext &Context) const override;
	void OnExplosion(const CGameExplosionContext &Context) override;
	void OnCharacterDeath(const CGameCharacterDeathContext &Context) override;
	void OnCharacterSpawn(CCharacter *pCharacter) override;
	void TickCharacterPreCore(CCharacter *pCharacter) override;
	void TickCharacterPostCore(CCharacter *pCharacter) override;
	virtual void HandleRaceTiles(CCharacterDDRace *pCharacter, int MapIndex) {}
	virtual void SetArmorProgress(CCharacterDDRace *pCharacter, int Progress) {}
	int PlayerAutoRespawnTick(const CPlayer *pPlayer) const override;
	std::unique_ptr<IGameModeMapReloadState> SaveStateForMapReload() override;
	void RestoreCharacterAfterMapReload(CCharacter *pCharacter) override;
	void OnPlayerConnect(CPlayer *pPlayer) override;
	void OnPlayerEnter(CPlayer *pPlayer) override;
	void OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason) override;
	bool OnPlayerChatMessage(int ClientId, const char *pMessage, int Team) override;
	void OnPlayerNameChanged(int ClientId) override;
	void OnPlayerDDNetVersionKnown(int ClientId) override;
	void OnPlayerMappingChanged(int ClientId) override;
	void OnReset() override;
	void OnPlayerSetTeam(int ClientId, int Team) override;
	void OnPlayerKill(int ClientId) override;
	bool CanSeeInteraction(const CInteractions &Interaction, int ClientId) const override;
	bool CanHitInteraction(const CInteractions &Interaction, int ClientId) const override;
	void OnPlayerCallKickVote(int ClientId, int TargetId, const char *pReason) override;
	void OnPlayerCallSpectateVote(int ClientId, int TargetId, const char *pReason) override;
	bool CanPlayerVoteOnTargetVote(int VoteCreatorId, int VoterId) const override;
	int PlayerVetoActivityStartTick(int ClientId) const override;
	int PlayerTeamGroup(int ClientId) const override;
	bool CanPlayerReceivePreInput(int SenderId, int ReceiverId) const override;
	CClientMask GetMaskForPlayerWorldEvent(int Asker, int ExceptId = -1) override;
	void OnPlayerShowOthers(int ClientId, int Show) override;
	bool CanSnapCharacter(CCharacter *pCharacter, int SnappingClient) const override;
	void SnapCharacterMode(CCharacter *pCharacter, int SnappingClient, int TranslatedId) override;
	void SnapPlayerMode(CPlayer *pPlayer, int SnappingClient, int TranslatedId) override;
	void Tick() override;
	bool UseDDNetEntityNetObjs() const override { return true; }
	bool IsTeamPractice(int Team) const override;

protected:
	CGameContext *GameServer() const { return IGameController::GameServer(); }
	void ApplyMapSettings();
	void InitGameSettings() override;
	void RegisterCommands() override;
	void RegisterAdminCommands();
	void RegisterPracticeCommands();

private:
	static bool CreateRaceMapEntity(IGameController &Controller, const CMapEntityContext &Context);
	std::unique_ptr<CGameTeams> m_pRaceTeams;
	std::unique_ptr<CScore> m_pRaceScore;
};

#endif // GAME_SERVER_GAMEMODES_DDRACE_H
