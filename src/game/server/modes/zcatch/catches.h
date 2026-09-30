#ifndef GAME_SERVER_MODES_ZCATCH_CATCHES_H
#define GAME_SERVER_MODES_ZCATCH_CATCHES_H

#include <engine/shared/protocol.h>

#include <array>
#include <vector>

/**
 * Who caught whom in zCatch, by client id.
 *
 * A catcher keeps the ones caught in order, the kill key lets the last one
 * go first. The kills that count towards winning go up with each catch that
 * was a kill and down with each release; a player who joined and was caught
 * by the leader is not one.
 */
class CCatches
{
public:
	static constexpr int NONE = -1;

private:
	class CState
	{
	public:
		int m_CatcherId = NONE;
		std::vector<int> m_vVictimIds;
		int m_KillsThatCount = 0;
	};
	std::array<CState, MAX_CLIENTS> m_aStates;

public:
	int CatcherId(int ClientId) const { return m_aStates[ClientId].m_CatcherId; }
	bool IsCaught(int ClientId) const { return CatcherId(ClientId) != NONE; }
	const std::vector<int> &VictimIds(int ClientId) const { return m_aStates[ClientId].m_vVictimIds; }
	int KillsThatCount(int ClientId) const { return m_aStates[ClientId].m_KillsThatCount; }
	// the one with the most kills that count, the lowest id of those; NONE if nobody has one
	int LeaderId() const;

	/**
	 * A catcher catches a victim, who is out until the catcher dies.
	 *
	 * @param VictimId The one who is caught.
	 * @param CatcherId The one who caught them.
	 * @param Counts Whether it was a kill, which counts towards winning.
	 */
	void Catch(int VictimId, int CatcherId, bool Counts);
	/**
	 * The kill key of a catcher: the one caught last goes free, and one kill
	 * less counts. Once none counts, all others go free too.
	 *
	 * @return The ones who go free, the last caught first.
	 */
	std::vector<int> ReleaseLast(int CatcherId);
	/**
	 * A catcher died: the ones caught go free and no kill counts any more.
	 *
	 * @return The ones who go free.
	 */
	std::vector<int> ReleaseAll(int CatcherId);
	// a player left the game: nobody holds them any more, and whom they held goes free
	std::vector<int> Leave(int ClientId);
	void Clear();
};

#endif // GAME_SERVER_MODES_ZCATCH_CATCHES_H
