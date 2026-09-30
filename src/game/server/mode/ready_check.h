#ifndef GAME_SERVER_MODE_READY_CHECK_H
#define GAME_SERVER_MODE_READY_CHECK_H

#include <engine/shared/protocol.h>

#include <array>

/**
 * Who is ready while the game waits for its players, as sv_player_ready_mode
 * does in Teeworlds 0.7.
 *
 * A game waits for its players before a match that starts once everybody is
 * ready, and in a pause without an end, which also begins when a player says
 * they are not ready any more. Each wait begins with nobody ready. Outside a
 * wait everybody counts as ready, which is what 0.7 tells its clients.
 *
 * This only keeps the state. CReadyMode decides who takes part, when
 * a wait begins, and what happens once everybody is ready.
 */
class CReadyCheck
{
public:
	enum class EWait
	{
		NONE,
		// for a match to start
		START,
		// for a pause to end
		RESUME,
	};

	// a player can change their mind once a second, as in 0.7
	static constexpr int CHANGE_INTERVAL_SECONDS = 1;

	EWait Wait() const { return m_Wait; }
	bool IsWaiting() const { return m_Wait != EWait::NONE; }
	// the tick the wait began, meaningless without one
	int WaitStartTick() const { return m_WaitStartTick; }

	// nobody is ready when a wait begins
	void Begin(EWait Wait, int Tick);
	void End();

	bool IsReady(int ClientId) const;
	void SetReady(int ClientId, bool Ready);
	void SetAllReady();
	// who of the participants is not ready yet
	CClientMask NotReady(const CClientMask &Participants) const;

	/**
	 * Whether a change of the ready state of a player counts now.
	 *
	 * @return false if the player changed their mind less than CHANGE_INTERVAL_SECONDS ago, else the change counts from Tick on.
	 */
	bool TakeChange(int ClientId, int Tick, int TickSpeed);
	// the player left the server
	void Forget(int ClientId);

private:
	EWait m_Wait = EWait::NONE;
	int m_WaitStartTick = 0;
	CClientMask m_Ready;
	std::array<int, MAX_CLIENTS> m_aLastChangeTick{};
};

#endif // GAME_SERVER_MODE_READY_CHECK_H
