/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_CORE_H
#define ENGINE_CLIENT_CLIENT_CORE_H

#include "session_runtime.h"

#include <base/hash.h>

#include <engine/client.h>
#include <engine/warning.h>

#include <memory>
#include <mutex>
#include <optional>
#include <vector>

class ILogger;

void FormatMapDownloadFilename(const char *pName, const std::optional<SHA256_DIGEST> &Sha256, int Crc, bool Temp, char *pBuffer, int BufferSize);

/**
 * The part of a client that neither connects nor draws: the state of the
 * client over the sessions it runs, loading the map and the demo of a session,
 * and the warnings. The game client and the programs that only show a demo
 * are built on it.
 */
class CClientCore : public IClient, public CSessionRuntime
{
protected:
	// The client state is that of the focused session, except once the client
	// quits or restarts. The game is told of every change; this is what it was
	// told last.
	std::optional<EClientState> m_ExitState;
	EClientState m_AnnouncedState = IClient::STATE_OFFLINE;

	int64_t m_LocalStartTime = 0;
	int64_t m_GlobalStartTime = 0;

	std::mutex m_WarningsMutex;
	std::vector<SWarning> m_vWarnings;
	std::vector<SWarning> m_vQuittingWarnings;

	std::shared_ptr<ILogger> m_pFileLogger = nullptr;
	std::shared_ptr<ILogger> m_pStdoutLogger = nullptr;

	/**
	 * Opens a demo in a session and loads its map, without starting it.
	 *
	 * @return An error message, or `nullptr` on success.
	 */
	const char *LoadDemo(CSessionId SessionId, const char *pFilename, int StorageType);

	/**
	 * Focuses the session a state belongs to and puts it into that state.
	 * Quitting and restarting are for good.
	 */
	void SetState(EClientState State);
	/**
	 * Puts the focused session into a state.
	 *
	 * @param State The state the focused session is put into.
	 * @param ResetSession Whether the game drops what it has of the session
	 * when the state goes back below online.
	 */
	void SetFocusedState(EClientState State, bool ResetSession);
	void FocusSession(CSessionId SessionId);
	/**
	 * Tells the game when `State()` has changed since it was told last.
	 */
	void AnnounceState();
	/**
	 * Called after the state of the focused session changed.
	 */
	virtual void OnStateChanged(EClientState State, EClientState OldState) {}

	const char *LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc);

public:
	IKernel *Kernel() { return IClient::Kernel(); }

	// A demo that is not exported runs on the clock of the client.
	float DemoPlaybackLocalTime(CSessionId SessionId) const override;
	using IClient::ActiveConnection;
	int ActiveConnection(CSessionId SessionId) const override { return SessionType(SessionId) == ESessionSourceType::DEMO ? CONN_MAIN : m_ActiveConnection; }

	EClientState State() const override;
	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	void AddWarning(const SWarning &Warning) override;
	std::optional<SWarning> CurrentWarning() override;
	std::vector<SWarning> &&QuittingWarnings() { return std::move(m_vQuittingWarnings); }

	void SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger);
};

#endif
