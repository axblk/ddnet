#ifndef ENGINE_CLIENT_LOCAL_SEATS_H
#define ENGINE_CLIENT_LOCAL_SEATS_H

#include "session.h"

/**
 * The seats the local players take on the server of the network session: the
 * player in seat 0, the dummy in seat 1. Each seat plays in a session of its
 * own; which one gets the input follows the dummy switch while the server is
 * in focus, and is the focused session otherwise.
 */
class CLocalSeats
{
	CSessionId m_NetworkSessionId;
	CSessionId m_DummySessionId;

public:
	enum
	{
		SEAT_PLAYER = 0,
		SEAT_DUMMY = 1,
		NUM_SEATS = 2,
		NO_SEAT = -1,
	};

	CLocalSeats() = default;
	CLocalSeats(CSessionId NetworkSessionId, CSessionId DummySessionId) :
		m_NetworkSessionId(NetworkSessionId), m_DummySessionId(DummySessionId) {}

	CSessionId NetworkSessionId() const { return m_NetworkSessionId; }
	CSessionId DummySessionId() const { return m_DummySessionId; }

	/**
	 * The session played in a seat.
	 */
	CSessionId SessionOf(int Seat) const { return Seat == SEAT_DUMMY ? m_DummySessionId : m_NetworkSessionId; }

	/**
	 * The seat a session is played in, `NO_SEAT` for one that is not played
	 * on the server, like a demo.
	 */
	int SeatOf(CSessionId SessionId) const
	{
		if(!SessionId.IsValid())
			return NO_SEAT;
		if(SessionId == m_NetworkSessionId)
			return SEAT_PLAYER;
		if(SessionId == m_DummySessionId)
			return SEAT_DUMMY;
		return NO_SEAT;
	}

	/**
	 * The session that gets the input and is predicted.
	 *
	 * @param FocusedSessionId The session in focus.
	 * @param ActiveSeat The seat the dummy switch selects.
	 */
	CSessionId InputSessionId(CSessionId FocusedSessionId, int ActiveSeat) const
	{
		if(FocusedSessionId.IsValid() && FocusedSessionId == m_NetworkSessionId)
			return SessionOf(ActiveSeat);
		return FocusedSessionId;
	}
};

#endif
