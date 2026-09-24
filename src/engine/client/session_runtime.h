/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_SESSION_RUNTIME_H
#define ENGINE_CLIENT_SESSION_RUNTIME_H

#include "session_sources.h"

#include <engine/client.h>
#include <engine/sessions.h>
#include <engine/shared/demo.h>

class IConsole;
class IStorage;

/**
 * Runs the game sessions and answers `ISessions` for them: the session
 * sources in the session manager, their connections with the snapshots and
 * the game time, and the demo player of the demo session. Whoever drives the
 * sessions derives from it: the client, and the programs that only show a
 * demo.
 */
class CSessionRuntime : public ISessions
{
protected:
	// Hands what the demo player of one session reads to the runtime, together
	// with the session it belongs to.
	class CDemoListener : public CDemoPlayer::IListener
	{
		CSessionRuntime *m_pRuntime = nullptr;
		CSessionId m_SessionId;

	public:
		CDemoListener() = default;
		CDemoListener(CSessionRuntime *pRuntime, CSessionId SessionId) :
			m_pRuntime(pRuntime), m_SessionId(SessionId) {}
		void OnDemoPlayerSnapshot(void *pData, int Size) override { m_pRuntime->OnDemoSnapshot(m_SessionId, pData, Size); }
		void OnDemoPlayerMessage(void *pData, int Size) override { m_pRuntime->OnDemoMessage(m_SessionId, pData, Size); }
	};

	IConsole *m_pConsole = nullptr;
	IGameClient *m_pGameClient = nullptr;
	IStorage *m_pStorage = nullptr;

	CSessionManager m_SessionManager;
	CSessionId m_NetworkSessionId;
	CSessionId m_DemoSessionId;
	CDemoSessionSource *m_pDemoSessionSource = nullptr;
	CDemoListener m_DemoListener;

	CSessionSourceBase &SessionSource(CSessionId SessionId)
	{
		CSessionSource *pSource = m_SessionManager.Find(SessionId);
		dbg_assert(pSource != nullptr, "invalid game session");
		return static_cast<CSessionSourceBase &>(*pSource);
	}
	const CSessionSourceBase &SessionSource(CSessionId SessionId) const
	{
		return const_cast<CSessionRuntime *>(this)->SessionSource(SessionId);
	}
	CDemoSessionSource &DemoSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::DEMO, "game session is not a demo");
		return static_cast<CDemoSessionSource &>(Source);
	}
	const CDemoSessionSource &DemoSource(CSessionId SessionId) const
	{
		return const_cast<CSessionRuntime *>(this)->DemoSource(SessionId);
	}
	CSessionSourceBase &ServerSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		return Source.m_pServerSource ? *Source.m_pServerSource : Source;
	}
	const CSessionSourceBase &ServerSource(CSessionId SessionId) const
	{
		return const_cast<CSessionRuntime *>(this)->ServerSource(SessionId);
	}
	CConnection &Connection(CSessionId SessionId) { return SessionSource(SessionId).m_Connection; }
	const CConnection &Connection(CSessionId SessionId) const { return SessionSource(SessionId).m_Connection; }
	CDemoPlayer &DemoPlayer() { return m_pDemoSessionSource->m_DemoPlayer; }
	const CDemoPlayer &DemoPlayer() const { return m_pDemoSessionSource->m_DemoPlayer; }

	void UpdateDemoIntraTimers(CSessionId SessionId);
	/**
	 * Advances the demo player of a session.
	 *
	 * @return `false` when the demo is no longer playing.
	 */
	bool UpdateDemoPlayer(CSessionId SessionId);
	int UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo);

public:
	IGameClient *GameClient() { return m_pGameClient; }
	const IGameClient *GameClient() const { return m_pGameClient; }
	IStorage *Storage() { return m_pStorage; }

	CSessionId FocusedSessionId() const override { return m_SessionManager.FocusedId(); }
	CSessionId DemoSessionId() const override { return m_DemoSessionId; }
	ESessionSourceType SessionType(CSessionId SessionId) const override { return SessionSource(SessionId).Type(); }
	ESessionState SessionState(CSessionId SessionId) const override { return SessionSource(SessionId).State(); }
	bool DemoPlaybackPaused(CSessionId SessionId) const override { return DemoSource(SessionId).m_DemoPlayer.BaseInfo()->m_Paused; }
	float DemoPlaybackSpeed(CSessionId SessionId) const override { return DemoSource(SessionId).m_DemoPlayer.BaseInfo()->m_Speed; }
	int64_t DemoPlaybackTime(CSessionId SessionId) const override;
	int PrevGameTick(CSessionId SessionId) const override { return Connection(SessionId).m_PrevGameTick; }
	int GameTick(CSessionId SessionId) const override { return Connection(SessionId).m_CurGameTick; }
	int PredGameTick(CSessionId SessionId) const override { return Connection(SessionId).m_PredTick; }
	float IntraGameTick(CSessionId SessionId) const override { return Connection(SessionId).m_GameIntraTick; }
	float PredIntraGameTick(CSessionId SessionId) const override { return Connection(SessionId).m_PredIntraTick; }
	float IntraGameTickSincePrev(CSessionId SessionId) const override { return Connection(SessionId).m_GameIntraTickSincePrev; }
	float GameTickTime(CSessionId SessionId) const override { return Connection(SessionId).m_GameTickTime; }

	const CServerInfo &ServerInfo(CSessionId SessionId) const override { return ServerSource(SessionId).m_ServerInfo; }
	bool IsSixup(CSessionId SessionId) const override { return ServerSource(SessionId).m_Sixup; }
	CTranslationContext &TranslationContext(CSessionId SessionId) override { return ServerSource(SessionId).m_TranslationContext; }
	const CTranslationContext &TranslationContext(CSessionId SessionId) const override { return ServerSource(SessionId).m_TranslationContext; }

	int *GetInput(CSessionId SessionId, int Tick) const override;
	int GetPredictionTime(CSessionId SessionId) override;
	int GetPredictionTick(CSessionId SessionId) override;
	void GetSmoothTick(CSessionId SessionId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) override;

	CSnapItem SnapGetItem(CSessionId SessionId, int SnapId, int Index) const override;
	const void *SnapFindItem(CSessionId SessionId, int SnapId, int Type, int Id) const override;
	int SnapNumItems(CSessionId SessionId, int SnapId) const override;
	void SnapSetStaticsize(int ItemType, int Size) override;
	void SnapSetStaticsize7(int ItemType, int Size) override;

	void OnDemoSnapshot(CSessionId SessionId, void *pData, int Size);
	void OnDemoMessage(CSessionId SessionId, void *pData, int Size);
};

#endif
