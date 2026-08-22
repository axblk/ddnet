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

struct CGameModeInfo
{
	const char *m_pId;
	const char *m_pDisplayName;
	const char *m_pGameType;
	const char *m_pTestingGameType;
	EGameModeScoreKind m_ScoreKind;
	int m_GameFlags;
	int m_ActivePlayerLimit = 0;
	bool m_UseTuneZones = false;
	CPhysicsRules m_PhysicsRules = CPhysicsRules::Vanilla();
};

class CGameModeRegistry
{
public:
	using FCreateController = std::unique_ptr<IGameController> (*)(CGameServices &Services, const CGameModeInfo &Info);

	bool Register(const CGameModeInfo &Info, FCreateController pfnCreateController);
	// By id only, so that registering a mode is never refused because another
	// one advertises the same name.
	const CGameModeInfo *Find(const char *pId) const;
	// By id or by the name the mode advertises, which is what `sv_gametype` has
	// always been set to.
	const CGameModeInfo *Resolve(const char *pId) const;
	std::unique_ptr<IGameController> Create(const char *pId, CGameServices &Services) const;

private:
	struct CEntry
	{
		CGameModeInfo m_Info;
		FCreateController m_pfnCreateController;
	};

	const CEntry *FindEntry(const char *pId) const;
	const CEntry *ResolveEntry(const char *pId) const;

	std::vector<CEntry> m_vEntries;
};

const char *GameModeScoreKindName(EGameModeScoreKind ScoreKind);

#endif // GAME_SERVER_MODE_GAME_MODE_REGISTRY_H
