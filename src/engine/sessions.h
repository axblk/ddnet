/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SESSIONS_H
#define ENGINE_SESSIONS_H

#include "kernel.h"

#include <engine/client/session.h>
#include <engine/shared/protocol.h>
#include <engine/shared/translation_context.h>

#include <cstdint>

class CServerInfo;

/**
 * The game sessions the game shows: which there are, which one is in focus,
 * their game time and their snapshots. A session is fed by a connection to a
 * server or by a demo, and has one of each; the dummy is a session of its own.
 * The game reads all of them the same way.
 */
class ISessions : public IInterface
{
	MACRO_INTERFACE("sessions")
public:
	class CSnapItem
	{
	public:
		int m_Type;
		int m_Id;
		const void *m_pData;
		int m_DataSize;
	};

	enum
	{
		SNAP_CURRENT = 0,
		SNAP_PREV = 1,
		NUM_SNAPSHOT_TYPES = 2,
	};

	virtual CSessionId FocusedSessionId() const = 0;
	virtual CSessionId DemoSessionId() const = 0;
#if defined(CONF_VIDEORECORDER)
	/**
	 * The demo session queued video exports are rendered in, next to the one
	 * a demo is watched in.
	 */
	virtual CSessionId VideoExportSessionId() const = 0;
#endif
	virtual ESessionSourceType SessionType(CSessionId SessionId) const = 0;
	virtual ESessionState SessionState(CSessionId SessionId) const = 0;
	virtual bool DemoPlaybackPaused(CSessionId SessionId) const = 0;
	virtual float DemoPlaybackSpeed(CSessionId SessionId) const = 0;
	virtual int64_t DemoPlaybackTime(CSessionId SessionId) const = 0;
	virtual float DemoPlaybackLocalTime(CSessionId SessionId) const = 0;

	// Game time.
	//
	// There are 50 ticks per second, by default we only send snapshot on
	// every second tick.

	/**
	 * Tick of the second to most recently received snapshot (usually 2
	 * less than `GameTick`).
	 */
	virtual int PrevGameTick(CSessionId SessionId) const = 0;
	/**
	 * Tick of most recently received snapshot.
	 */
	virtual int GameTick(CSessionId SessionId) const = 0;
	/**
	 * The tick we should predict to. Comes from a magic black box called
	 * "smooth time".
	 */
	virtual int PredGameTick(CSessionId SessionId) const = 0;
	/**
	 * Linear interpolation parameter between `PrevGameTick` (0) and
	 * `GameTick` (1). Can be outside the interval [0, 1].
	 */
	virtual float IntraGameTick(CSessionId SessionId) const = 0;
	/**
	 * Linear interpolation parameter between `PredGameTick - 1` (0) and
	 * `PredGameTick` (1). Can be outside the interval [0, 1].
	 */
	virtual float PredIntraGameTick(CSessionId SessionId) const = 0;
	/**
	 * (Fractional) ticks since `PrevGameTick`.
	 */
	virtual float IntraGameTickSincePrev(CSessionId SessionId) const = 0;
	/**
	 * Time in seconds since the second to most recently received snapshot.
	 */
	virtual float GameTickTime(CSessionId SessionId) const = 0;
	/**
	 * 50
	 */
	int GameTickSpeed() const { return SERVER_TICK_SPEED; }

	virtual int GetPredictionTime(CSessionId SessionId) = 0;
	virtual int GetPredictionTick(CSessionId SessionId) = 0;
	virtual void GetSmoothTick(CSessionId SessionId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) = 0;

	// input
	virtual int *GetInput(CSessionId SessionId, int Tick) const = 0;

	// server info; the dummy session answers with that of the server it plays on
	virtual const CServerInfo &ServerInfo(CSessionId SessionId) const = 0;
	virtual bool IsSixup(CSessionId SessionId) const = 0;
	virtual CTranslationContext &TranslationContext(CSessionId SessionId) = 0;
	virtual const CTranslationContext &TranslationContext(CSessionId SessionId) const = 0;

	// snapshot interface

	// TODO: Refactor: should redo this a bit i think, too many virtual calls
	virtual int SnapNumItems(CSessionId SessionId, int SnapId) const = 0;
	virtual const void *SnapFindItem(CSessionId SessionId, int SnapId, int Type, int Id) const = 0;
	virtual CSnapItem SnapGetItem(CSessionId SessionId, int SnapId, int Index) const = 0;

	virtual void SnapSetStaticsize(int ItemType, int Size) = 0;
	virtual void SnapSetStaticsize7(int ItemType, int Size) = 0;
};

#endif
