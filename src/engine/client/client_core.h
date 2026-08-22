/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_CORE_H
#define ENGINE_CLIENT_CLIENT_CORE_H

#include "session_source_demo.h"
#include "session_sources.h"

#include <base/hash.h>
#include <base/types.h>

#include <engine/client.h>
#include <engine/shared/demo.h>
#include <engine/warning.h>

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

class CConfig;
class IConfigManager;
class IConsole;
class IEngine;
class ILogger;
class IStorage;

/**
 * Names the file a map with these contents is stored under once it was
 * downloaded, so that a map found there is used instead of asked for again.
 */
void FormatMapDownloadFilename(const char *pName, const std::optional<SHA256_DIGEST> &Sha256, int Crc, bool Temp, char *pBuffer, int BufferSize);

/**
 * The half of a client that neither talks to a server nor draws anything:
 * sessions and their streams, ticks, snapshots and the timing around them.
 *
 * A program picks up the halves it needs from here. The game client adds the
 * network and the presentation, a demo render tool adds only the presentation,
 * and a headless client adds only the network. None of them has to answer for
 * a half it does not have.
 */
class CClientCore : public IClient, public CDemoPlayer::IListener
{
protected:
	IConfigManager *m_pConfigManager = nullptr;
	CConfig *m_pConfig = nullptr;
	IConsole *m_pConsole = nullptr;
	IEngine *m_pEngine = nullptr;
	IGameClient *m_pGameClient = nullptr;
	IStorage *m_pStorage = nullptr;

	CSessionManager m_SessionManager;
	CSessionId m_DemoSessionId;
	CDemoSessionSource *m_pDemoSessionSource = nullptr;

	std::array<std::vector<std::pair<int, int>>, 2> m_avSnapshotStaticSizes;

	int64_t m_LocalStartTime = 0;
	int64_t m_GlobalStartTime = 0;

	std::mutex m_WarningsMutex;
	std::vector<SWarning> m_vWarnings;
	std::vector<SWarning> m_vQuittingWarnings;

	std::shared_ptr<ILogger> m_pFileLogger = nullptr;
	std::shared_ptr<ILogger> m_pStdoutLogger = nullptr;

	CSessionSourceBase &SessionSource(CSessionId SessionId)
	{
		CGameSession *pSession = m_SessionManager.Find(SessionId);
		dbg_assert(pSession != nullptr, "invalid game session");
		return static_cast<CSessionSourceBase &>(pSession->Source());
	}
	const CSessionSourceBase &SessionSource(CSessionId SessionId) const
	{
		const CGameSession *pSession = m_SessionManager.Find(SessionId);
		dbg_assert(pSession != nullptr, "invalid game session");
		return static_cast<const CSessionSourceBase &>(pSession->Source());
	}
	CDemoSessionSource &DemoSource(CSessionId SessionId)
	{
		CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::DEMO, "game session is not a demo source");
		return static_cast<CDemoSessionSource &>(Source);
	}
	const CDemoSessionSource &DemoSource(CSessionId SessionId) const
	{
		const CSessionSourceBase &Source = SessionSource(SessionId);
		dbg_assert(Source.Type() == ESessionSourceType::DEMO, "game session is not a demo source");
		return static_cast<const CDemoSessionSource &>(Source);
	}
	CConnection &Connection(CSessionId SessionId, CStreamId StreamId)
	{
		CConnection *pConnection = SessionSource(SessionId).StreamConnection(StreamId);
		dbg_assert(pConnection != nullptr, "invalid stream");
		return *pConnection;
	}
	const CConnection &Connection(CSessionId SessionId, CStreamId StreamId) const
	{
		const CConnection *pConnection = SessionSource(SessionId).StreamConnection(StreamId);
		dbg_assert(pConnection != nullptr, "invalid stream");
		return *pConnection;
	}
	CConnection &Connection(CSessionId SessionId, int Conn)
	{
		return Connection(SessionId, SessionSource(SessionId).StreamIdForIndex(Conn));
	}
	const CConnection &Connection(CSessionId SessionId, int Conn) const
	{
		return Connection(SessionId, SessionSource(SessionId).StreamIdForIndex(Conn));
	}
	CDemoPlayer &DemoPlayer() { return m_pDemoSessionSource->DemoPlayer(); }
	const CDemoPlayer &DemoPlayer() const { return m_pDemoSessionSource->DemoPlayer(); }

	int MaxLatencyTicks(CSessionId SessionId) const;
	int PredictionMargin(CSessionId SessionId) const;

	/**
	 * The session a demo player belongs to, so that the callbacks it makes
	 * find their way back to the right one.
	 */
	CSessionId FindDemoSessionId(const CDemoPlayer &DemoPlayer) const;
	void UpdateDemoIntraTimers(CDemoPlayer &DemoPlayer);
	void UpdateDemoSession(CSessionId SessionId);
	/**
	 * Turns a received snapshot into the unpacked form the game reads, with
	 * every object it does not know dropped.
	 *
	 * @return The size of the unpacked snapshot, negative on failure.
	 */
	int UnpackAndValidateSnapshot(CSnapshot *pFrom, CSnapshotBuffer *pTo);

	/**
	 * Which session becomes the focused one when the client enters a state.
	 * The core knows the demo session, a program that connects knows more.
	 */
	virtual void FocusSessionForState(EClientState State)
	{
		if(State == IClient::STATE_DEMOPLAYBACK)
			m_SessionManager.SetFocused(m_DemoSessionId);
	}
	/**
	 * Called after the client changed state, for whatever a program publishes
	 * about what it is doing.
	 */
	virtual void OnStateChanged(EClientState State, EClientState OldState) {}
	/**
	 * Called before a session loads a map, for whatever a program has to put
	 * down first because it was recorded against the old one.
	 */
	virtual void OnMapLoadStarted(CSessionId SessionId) {}

	const char *LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc);

	void SetState(EClientState State);
	void SetFocusedState(EClientState State, bool ResetSession);
	void FocusSession(CSessionId SessionId);

public:
	using IClient::ConnectionProblems;
	using IClient::GameTick;
	using IClient::GameTickTime;
	using IClient::GetInput;
	using IClient::GetPredictionTick;
	using IClient::GetPredictionTime;
	using IClient::GetSmoothTick;
	using IClient::IntraGameTick;
	using IClient::IntraGameTickSincePrev;
	using IClient::PredGameTick;
	using IClient::PredIntraGameTick;
	using IClient::PrevGameTick;
	using IClient::SendMsg;
	using IClient::ServerInfo;
	using IClient::SnapFindItem;
	using IClient::SnapGetItem;
	using IClient::SnapNumItems;

	IConfigManager *ConfigManager() { return m_pConfigManager; }
	CConfig *Config() { return m_pConfig; }
	IEngine *Engine() { return m_pEngine; }
	IGameClient *GameClient() { return m_pGameClient; }
	const IGameClient *GameClient() const { return m_pGameClient; }
	IStorage *Storage() { return m_pStorage; }

	// ----- sessions and their streams -----
	CSessionId FocusedSessionId() const override { return m_SessionManager.FocusedId(); }
	CSessionId DemoSessionId() const override { return m_DemoSessionId; }
	std::vector<CSessionId> SessionIds() const override { return m_SessionManager.SessionIds(); }
	ESessionSourceType SessionType(CSessionId SessionId) const override { return SessionSource(SessionId).Type(); }
	std::vector<CStreamId> StreamIds(CSessionId SessionId) const override { return SessionSource(SessionId).StreamIds(); }
	CStreamId PrimaryStreamId(CSessionId SessionId) const override { return SessionSource(SessionId).PrimaryStreamId(); }
	CStreamId ActiveStreamId(CSessionId SessionId) const override { return SessionSource(SessionId).ActiveStreamId(); }
	CStreamId StreamId(CSessionId SessionId, int LegacyConnection) const override { return SessionSource(SessionId).StreamIdForIndex(LegacyConnection); }
	int StreamIndex(CSessionId SessionId, CStreamId StreamId) const override { return SessionSource(SessionId).IndexForStream(StreamId); }
	ESessionState SessionState(CSessionId SessionId) const override { return SessionSource(SessionId).State(); }
	const CServerInfo &ServerInfo(CSessionId SessionId) const override { return SessionSource(SessionId).ServerInfo(); }
	bool IsSixup(CSessionId SessionId) const override { return SessionSource(SessionId).IsSixup(); }
	CTranslationContext &TranslationContext(CSessionId SessionId) override { return SessionSource(SessionId).TranslationContext(); }
	const CTranslationContext &TranslationContext(CSessionId SessionId) const override { return SessionSource(SessionId).TranslationContext(); }

	// ----- demo playback -----
	bool DemoPlaybackPaused(CSessionId SessionId) const override { return DemoSource(SessionId).DemoPlayer().BaseInfo()->m_Paused; }
	float DemoPlaybackSpeed(CSessionId SessionId) const override { return DemoSource(SessionId).DemoPlayer().BaseInfo()->m_Speed; }
	int64_t DemoPlaybackTime(CSessionId SessionId) const override;
	float DemoPlaybackLocalTime(CSessionId SessionId) const override;

	// ----- time -----
	int PrevGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_PrevGameTick; }
	int GameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_CurGameTick; }
	int PredGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_PredTick; }
	float IntraGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_GameIntraTick; }
	float PredIntraGameTick(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_PredIntraTick; }
	float IntraGameTickSincePrev(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_GameIntraTickSincePrev; }
	float GameTickTime(CSessionId SessionId, CStreamId StreamId) const override { return Connection(SessionId, StreamId).m_GameTickTime; }
	int GetPredictionTime(CSessionId SessionId, CStreamId StreamId) override;
	int GetPredictionTick(CSessionId SessionId, CStreamId StreamId) override;
	void GetSmoothTick(CSessionId SessionId, CStreamId StreamId, int64_t Now, int *pSmoothTick, float *pSmoothIntraTick, float MixAmount) override;

	// ----- snapshots -----
	CSnapItem SnapGetItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Index) const override;
	const void *SnapFindItem(CSessionId SessionId, CStreamId StreamId, int SnapId, int Type, int Id) const override;
	int SnapNumItems(CSessionId SessionId, CStreamId StreamId, int SnapId) const override;
	void SnapSetStaticsize(int ItemType, int Size) override;
	void SnapSetStaticsize7(int ItemType, int Size) override;

	// ----- warnings -----
	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	void AddWarning(const SWarning &Warning) override;
	std::optional<SWarning> CurrentWarning() override;
	std::vector<SWarning> &&QuittingWarnings() { return std::move(m_vQuittingWarnings); }

	void SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger);

	// ----- demo player callbacks -----
	void OnDemoPlayerSnapshot(CDemoPlayer &DemoPlayer, void *pData, int Size) override;
	void OnDemoPlayerMessage(CDemoPlayer &DemoPlayer, void *pData, int Size) override;
};

#endif // ENGINE_CLIENT_CLIENT_CORE_H
