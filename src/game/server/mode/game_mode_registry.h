#ifndef GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
#define GAME_SERVER_MODE_GAME_MODE_REGISTRY_H

#include <memory>
#include <vector>

class CGameContext;
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
};

class CGameModeRegistry
{
public:
	using FCreateController = std::unique_ptr<IGameController> (*)(CGameContext *pGameServer, const CGameModeInfo &Info);

	bool Register(const CGameModeInfo &Info, FCreateController pfnCreateController);
	const CGameModeInfo *Find(const char *pId) const;
	std::unique_ptr<IGameController> Create(const char *pId, CGameContext *pGameServer) const;

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
