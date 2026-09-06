#ifndef GAME_SERVER_MODES_VANILLA_CTF_H
#define GAME_SERVER_MODES_VANILLA_CTF_H

#include "teamplay.h"

#include <array>

class CFlag;

class CGameControllerVanillaCTF : public CGameControllerVanillaTeamplay
{
public:
	CGameControllerVanillaCTF(CGameServices &Services, const CGameModeInfo &GameModeInfo);

	void OnCharacterDeath(const CGameCharacterDeathContext &Context) override;
	void Tick() override;
	CFlag *Flag(int Team) const;

protected:
	void SnapMode(int SnappingClient) override;
	bool CanBeMovedOnBalance(const CPlayer *pPlayer) const override;

private:
	static bool CreateFlagMapEntity(IGameController &Controller, const CMapEntityContext &Context);
	void ProcessFlags();
	void FlagGrab(CFlag *pFlag, CCharacter *pCarrier);
	void FlagReturn(CFlag *pFlag, CCharacter *pPlayer);
	void FlagCapture(CFlag *pFlag);
	int FlagCarrierState(const CFlag *pFlag, int SnappingClient) const;

	std::array<CFlag *, NUM_TEAMS> m_apFlags{};
};

#endif // GAME_SERVER_MODES_VANILLA_CTF_H
