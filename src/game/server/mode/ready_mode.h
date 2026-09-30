#ifndef GAME_SERVER_MODE_READY_MODE_H
#define GAME_SERVER_MODE_READY_MODE_H

#include <engine/console.h>
#include <engine/shared/protocol.h>

#include <game/server/mode/ready_check.h>

class CGameServices;

/**
 * The ready mode of Teeworlds 0.7 (sv_player_ready_mode), a part of every
 * game controller that plays matches.
 *
 * A match that waits for its players to be ready and a pause without an end
 * both wait for everybody in the game, and a player who is not ready any more
 * pauses a running game. This decides when a wait begins and ends, takes the
 * players' changes of mind, and tells the players whom the game waits for:
 * 0.7 clients from PLAYERFLAG_READY (see IsReady), DDNet clients that asked for
 * it from the ready state objects, and all others in broadcasts. Who is ready
 * is kept by CReadyCheck.
 *
 * What waiting, starting, pausing and resuming mean is up to the game it is a
 * part of, see IGame.
 */
class CReadyMode
{
public:
	/**
	 * What the ready mode needs of the game it waits in. The game controller
	 * implements it with its match lifecycle and the pause of its mode.
	 */
	class IGame
	{
	public:
		// who has to be ready: the players in the game, not the spectators and not the dead
		virtual bool IsReadyParticipant(int ClientId) const = 0;
		// a match waits for everybody to be ready before it starts (restart -1, sv_warmup -1)
		virtual bool IsMatchWaitingForReady() const = 0;
		// a running game that a player who is not ready any more pauses
		virtual bool IsRunningUnpaused() const = 0;
		// a pause that only a command or the players' ready state ends
		virtual bool IsPausedUntilReady() const = 0;
		// the match that waited for everybody starts, with the countdown of the mode
		virtual void StartMatch() = 0;
		// a pause without an end, or the end of one with the countdown of the mode
		virtual void PauseUntilReady(bool Pause) = 0;

	protected:
		~IGame() = default;
	};

	CReadyMode(CGameServices &Services, IGame &Game);
	~CReadyMode();
	CReadyMode(const CReadyMode &) = delete;
	CReadyMode &operator=(const CReadyMode &) = delete;

	/**
	 * Gives the game a ready mode, which sv_player_ready_mode switches on and
	 * off while it runs. Registers the chat commands /ready and /pause.
	 */
	void Init();
	// whether the game waits for its players to be ready
	bool IsOn() const;

	void Tick();
	void Snap(int SnappingClient);

	/**
	 * A player says that they are ready, or not any more, as the ready change of Teeworlds 0.7 does.
	 *
	 * A player who is not ready any more pauses a running game until
	 * everybody is ready again, and a game that waits for its players goes on
	 * once all of them are.
	 *
	 * @param ClientId The player.
	 *
	 * @return why nothing changed, for a player who asked in chat; nullptr if it changed or a change came too soon after the last one.
	 */
	const char *OnPlayerReadyChange(int ClientId);
	// the force_ready command: the player, or everybody for -1, counts as ready
	void ForceReady(int ClientId);
	// the player left the server and is not ready when coming back
	void OnPlayerLeave(int ClientId);
	// the client of a player started or stopped showing the ready state itself, see ShowsReadyState
	void OnPlayerShowsReadyStateChanged(int ClientId);

	// whether a player counts as ready, which everybody does while the game waits for nobody
	bool IsReady(int ClientId) const { return m_ReadyCheck.IsReady(ClientId); }
	// what the game waits for, NONE without ready mode
	CReadyCheck::EWait Wait() const { return m_ReadyCheck.Wait(); }

private:
	CGameServices &m_Services;
	IGame &m_Game;
	// whether the game has a ready mode at all, see Init
	bool m_Available = false;
	CReadyCheck m_ReadyCheck;
	int m_NextBroadcastTick = 0;
	bool m_BroadcastSent = false;
	// whom the last broadcast named
	CClientMask m_BroadcastNotReady;

	CClientMask Participants() const;
	// why a player cannot change their ready state now, nullptr if they can
	const char *ChangeRefusal(int ClientId) const;
	// begins or ends the wait with the state of the game
	void UpdateWait();
	void FinishWait();
	// the tick the game goes on anyway with sv_force_ready_all, 0 for never
	int ForceReadyTick() const;
	void SendBroadcasts(bool Clear);
	// DDNet clients that asked for it and server demos get the ready state as objects
	bool ShowsReadyState(int SnappingClient) const;

	static void ConReady(IConsole::IResult *pResult, void *pUserData);
};

#endif // GAME_SERVER_MODE_READY_MODE_H
