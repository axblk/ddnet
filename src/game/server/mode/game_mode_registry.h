#ifndef GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
#define GAME_SERVER_MODE_GAME_MODE_REGISTRY_H

#include <game/gamecore.h>

#include <memory>
#include <vector>

class CGameServices;
class IGameController;

enum class EGameModeScoreKind
{
	POINTS,
	TIME,
};

enum EGameModeProtocol
{
	GAME_MODE_PROTOCOL_SIX = 1 << 0,
	GAME_MODE_PROTOCOL_SEVEN = 1 << 1,
};

struct CGameModeInfo
{
	const char *m_pId;
	const char *m_pDisplayName;
	const char *m_pGameType;
	const char *m_pTestingGameType;
	EGameModeScoreKind m_ScoreKind;
	int m_GameFlags;
	int m_Protocols;
	int m_ActivePlayerLimit = 0;
	bool m_UseTuneZones = false;
	EPhysicsRuleset m_PhysicsRuleset = EPhysicsRuleset::VANILLA;
};

class CGameModeRegistry
{
public:
	using FCreateController = std::unique_ptr<IGameController> (*)(CGameServices &Services, const CGameModeInfo &Info);

	bool Register(const CGameModeInfo &Info, FCreateController pfnCreateController);
	const CGameModeInfo *Find(const char *pId) const;
	std::unique_ptr<IGameController> Create(const char *pId, CGameServices &Services) const;

private:
	struct CEntry
	{
		CGameModeInfo m_Info;
		FCreateController m_pfnCreateController;
	};

	std::vector<CEntry> m_vEntries;
};

const char *GameModeScoreKindName(EGameModeScoreKind ScoreKind);

#endif // GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
