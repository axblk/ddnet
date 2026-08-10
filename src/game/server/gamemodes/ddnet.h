/* (c) Shereef Marzouk. See "licence DDRace.txt" and the readme.txt in the root of the distribution for more information. */
#ifndef GAME_SERVER_GAMEMODES_DDNET_H
#define GAME_SERVER_GAMEMODES_DDNET_H

#include "ddrace.h"

class CScore;

class CGameControllerDDNet : public CGameControllerDDRace
{
public:
	CGameControllerDDNet(class CGameContext *pGameServer, const CGameModeInfo &GameModeInfo);
	~CGameControllerDDNet() override;
	static CTuningParams DefaultTuning();
	void ResetTuning() override;

	CScore *Score();

	void HandleCharacterTiles(class CCharacter *pChr, int MapIndex) override;
	void SetArmorProgress(CCharacter *pCharacter, int Progress) override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	CFinishTime SnapPlayerTime(int SnappingClient, CPlayer *pPlayer) override;
	CFinishTime SnapMapBestTime(int SnappingClient) override;

	void OnPlayerConnect(class CPlayer *pPlayer) override;
	void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason) override;
	void OnReset() override;

	void Tick() override;

	void DoTeamChange(class CPlayer *pPlayer, int Team, bool DoChatMsg) override;

protected:
	void InitGameSettings() override;
	void UpdateGameInfo(CNetObj_GameInfo &GameInfo, int SnappingClient) override;
	int GameInfoFlags(int SnappingClient) const override;
	int GameInfoFlags2(int SnappingClient) const override;
	void SnapMode(int SnappingClient) override;
};
#endif // GAME_SERVER_GAMEMODES_DDNET_H
