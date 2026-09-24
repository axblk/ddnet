/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_CORE_H
#define ENGINE_CLIENT_CLIENT_CORE_H

#include "session_sources.h"

#include <base/hash.h>

#include <engine/client.h>
#include <engine/sessions.h>
#include <engine/shared/demo.h>
#include <engine/warning.h>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class IConsole;
class ILogger;
class IStorage;

void FormatMapDownloadFilename(const char *pName, const std::optional<SHA256_DIGEST> &Sha256, int Crc, bool Temp, char *pBuffer, int BufferSize);

/**
 * The part of a client that neither connects nor draws: sessions, ticks,
 * snapshots, demo playback and loading the map of a session. The game client
 * and the programs that only show a demo are built on it.
 */
class CClientCore : public IClient, public ISessions
{
protected:
	// Hands what the demo player of one session reads to the client, together
	// with the session it belongs to.
	class CDemoListener : public CDemoPlayer::IListener
	{
		CClientCore *m_pClient = nullptr;
		CSessionId m_SessionId;

	public:
		CDemoListener() = default;
		CDemoListener(CClientCore *pClient, CSessionId SessionId) :
			m_pClient(pClient), m_SessionId(SessionId) {}
		void OnDemoPlayerSnapshot(void *pData, int Size) override { m_pClient->OnDemoSnapshot(m_SessionId, pData, Size); }
		void OnDemoPlayerMessage(void *pData, int Size) override { m_pClient->OnDemoMessage(m_SessionId, pData, Size); }
	};

	IConsole *m_pConsole = nullptr;
	IGameClient *m_pGameClient = nullptr;
	IStorage *m_pStorage = nullptr;

	CSessionManager m_SessionManager;
	CSessionId m_NetworkSessionId;
	CSessionId m_DemoSessionId;
	CDemoSessionSource *m_pDemoSessionSource = nullptr;
	CDemoListener m_DemoListener;

	int64_t m_LocalStartTime = 0;
	int64_t m_GlobalStartTime = 0;

	std::mutex m_WarningsMutex;
	std::vector<SWarning> m_vWarnings;
	std::vector<SWarning> m_vQuittingWarnings;

	std::shared_ptr<ILogger> m_pFileLogger = nullptr;
	std::shared_ptr<ILogger> m_pStdoutLogger = nullptr;

	CSessionSourceBase &SessionSource(CSessionId SessionId)
	{
		CSessionSource *pSource = m_SessionManager.Find(SessionId);
		dbg_assert(pSource != nullptr, "invalid game session");
		return static_cast<CSessionSourceBase &>(*pSource);
	}
	const CSessionSourceBase &SessionSource(CSessionId SessionId) const
	{
		return const_cast<CClientCore *>(this)->SessionSource(SessionId);
	}
	CDemoSessionSource &DemoSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::DEMO, "game session is not a demo");
		return static_cast<CDemoSessionSource &>(Source);
	}
	const CDemoSessionSource &DemoSource(CSessionId SessionId) const
	{
		return const_cast<CClientCore *>(this)->DemoSource(SessionId);
	}
	CConnection &Connection(CSessionId SessionId, int Conn)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		if(Source.Type() == ESessionSourceType::NETWORK)
			return static_cast<CNetworkSessionSource &>(Source).m_aConnections[Conn];
		dbg_assert(Conn == CONN_MAIN, "a demo has only one connection");
		return static_cast<CDemoSessionSource &>(Source).m_Connection;
	}
	const CConnection &Connection(CSessionId SessionId, int Conn) const
	{
		return const_cast<CClientCore *>(this)->Connection(SessionId, Conn);
	}
	CDemoPlayer &DemoPlayer() { return m_pDemoSessionSource->m_DemoPlayer; }
	const CDemoPlayer &DemoPlayer() const { return m_pDemoSessionSource->m_DemoPlayer; }

	void UpdateDemoIntraTimers(CSessionId SessionId);
	/**
	 * Advances the demo player of a session.
	 *
	 * @return `false` when the demo is no longer playing.
	 */
	bool UpdateDemoPlayer(CSessionId SessionId);
	/**
	 * Opens a demo in a session and loads its map, without starting it.
	 *
	 * @return An error message, or `nullptr` on success.
	 */
	const char *LoadDemo(CSessionId SessionId, const char *pFilename, int StorageType);
	int UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo);

	void SetState(EClientState State);
	void SetFocusedState(EClientState State, bool ResetSession);
	void FocusSession(CSessionId SessionId);
	/**
	 * Called after the state of the focused session changed.
	 */
	virtual void OnStateChanged(EClientState State, EClientState OldState) {}

	const char *LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc);

public:
	IKernel *Kernel() { return IClient::Kernel(); }
	IGameClient *GameClient() { return m_pGameClient; }
	const IGameClient *GameClient() const { return m_pGameClient; }
	IStorage *Storage() { return m_pStorage; }

	CSessionId FocusedSessionId() const override { return m_SessionManager.FocusedId(); }
	CSessionId NetworkSessionId() const override { return m_NetworkSessionId; }
	CSessionId DemoSessionId() const override { return m_DemoSessionId; }
	ESessionSourceType SessionType(CSessionId SessionId) const override { return SessionSource(SessionId).Type(); }
	ESessionState SessionState(CSessionId SessionId) const override { return SessionSource(SessionId).State(); }
	using IClient::ActiveConnection;
	int ActiveConnection(CSessionId SessionId) const override { return SessionType(SessionId) == ESessionSourceType::DEMO ? CONN_MAIN : m_ActiveConnection; }
	bool DemoPlaybackPaused(CSessionId SessionId) const override { return DemoSource(SessionId).m_DemoPlayer.BaseInfo()->m_Paused; }
	float DemoPlaybackSpeed(CSessionId SessionId) const override { return DemoSource(SessionId).m_DemoPlayer.BaseInfo()->m_Speed; }
	int64_t DemoPlaybackTime(CSessionId SessionId) const override;
	float DemoPlaybackLocalTime(CSessionId SessionId) const override;
	int PrevGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_PrevGameTick; }
	int GameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_CurGameTick; }
	int PredGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_PredTick; }
	float IntraGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_GameIntraTick; }
	float PredIntraGameTick(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_PredIntraTick; }
	float IntraGameTickSincePrev(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_GameIntraTickSincePrev; }
	float GameTickTime(CSessionId SessionId, int Conn) const override { return Connection(SessionId, Conn).m_GameTickTime; }

	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	const CServerInfo &ServerInfo(CSessionId SessionId) const override { return SessionSource(SessionId).m_ServerInfo; }
	bool IsSixup(CSessionId SessionId) const override { return SessionSource(SessionId).m_Sixup; }
	CTranslationContext &TranslationContext(CSessionId SessionId) override { return SessionSource(SessionId).m_TranslationContext; }
	const CTranslationContext &TranslationContext(CSessionId SessionId) const override { return SessionSource(SessionId).m_TranslationContext; }

	int *GetInput(CSessionId SessionId, int Conn, int Tick) const override;
	int GetPredictionTime(CSessionId SessionId, int Conn) override;
	int GetPredictionTick(CSessionId SessionId, int Conn) override;
	void GetSmoothTick(CSessionId SessionId, int Conn, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) override;

	CSnapItem SnapGetItem(CSessionId SessionId, int Conn, int SnapId, int Index) const override;
	const void *SnapFindItem(CSessionId SessionId, int Conn, int SnapId, int Type, int Id) const override;
	int SnapNumItems(CSessionId SessionId, int Conn, int SnapId) const override;
	void SnapSetStaticsize(int ItemType, int Size) override;
	void SnapSetStaticsize7(int ItemType, int Size) override;

	void OnDemoSnapshot(CSessionId SessionId, void *pData, int Size);
	void OnDemoMessage(CSessionId SessionId, void *pData, int Size);

	void AddWarning(const SWarning &Warning) override;
	std::optional<SWarning> CurrentWarning() override;
	std::vector<SWarning> &&QuittingWarnings() { return std::move(m_vQuittingWarnings); }

	void SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger);
};

#endif
