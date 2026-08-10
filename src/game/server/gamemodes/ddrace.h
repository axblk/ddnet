/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_GAMEMODES_DDRACE_H
#define GAME_SERVER_GAMEMODES_DDRACE_H

#include <game/server/gamecontroller.h>

class CGameControllerDDRace : public IGameController
{
public:
	CGameControllerDDRace(class CGameContext *pGameServer, const CGameModeInfo &GameModeInfo);

	void OnCharacterSpawn(CCharacter *pCharacter) override;
	void TickCharacterPreCore(CCharacter *pCharacter) override;
	void TickCharacterPostCore(CCharacter *pCharacter) override;
	int PlayerAutoRespawnTick(const CPlayer *pPlayer) const override;
	bool SaveStateForHotReload() override;
	void RestoreCharacterAfterHotReload(CCharacter *pCharacter) override;
	bool OnEntity(int Index, int x, int y, int Layer, int Flags, bool Initial, int Number) override;
	void OnPlayerSetTeam(int ClientId, int Team) override;

protected:
	void RegisterCommands() override;
};

#endif // GAME_SERVER_GAMEMODES_DDRACE_H
