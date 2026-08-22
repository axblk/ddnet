#ifndef GAME_SERVER_INTERACTIONS_H
#define GAME_SERVER_INTERACTIONS_H

#include <engine/shared/protocol.h>

class CGameContext;

class CInteractions
{
	int m_OwnerId = -1;
	uint32_t m_UniqueOwnerId = 0;
	int m_DDRaceTeam = 0;
	bool m_Solo = false;
	bool m_NoHitOthers = false;
	bool m_NoHitSelf = false;
	bool m_RestrictToDDRaceTeam = false;

public:
	void Init(int OwnerId, uint32_t UniqueOwnerId);
	void FillOwnerConnected(
		int DDRaceTeam,
		bool Solo,
		bool NoHitOthers,
		bool NoHitSelf,
		bool RestrictToDDRaceTeam = false);
	void FillOwnerDisconnected();
	int OwnerId() const { return m_OwnerId; }
	uint32_t UniqueOwnerId() const { return m_UniqueOwnerId; }
	int DDRaceTeam() const { return m_DDRaceTeam; }
	bool IsSolo() const { return m_Solo; }
	bool NoHitOthers() const { return m_NoHitOthers; }
	bool NoHitSelf() const { return m_NoHitSelf; }
	bool RestrictToDDRaceTeam() const { return m_RestrictToDDRaceTeam; }
	bool CanSee(const CGameContext *pGameServer, int ClientId) const;
	bool CanHit(const CGameContext *pGameServer, int ClientId) const;
	CClientMask CanSeeMask(const CGameContext *pGameServer) const;
};

#endif
