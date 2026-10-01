/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_CLIENT_CORE_H
#define ENGINE_CLIENT_CLIENT_CORE_H

#include "session_runtime.h"

#include <base/hash.h>

#include <engine/client.h>
#include <engine/client/asset_loader.h>
#include <engine/warning.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
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
	 * How looking for the map of a session ended, or that it goes on.
	 */
	enum class EMapSearch
	{
		LOADED,
		/**
		 * The map is fetched, see `LoadMapSearch`.
		 */
		FETCHING,
		NOT_FOUND,
	};

	/**
	 * Opens a demo in a session and loads its map, without starting it. The
	 * map the demo carries is loaded from the demo; only a demo without one
	 * has its map looked for, see `LoadMapSearch`.
	 *
	 * @return An error message, or `nullptr` on success, which may leave the
	 * map fetching (see `IsMapSearchPending`).
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

	/**
	 * Loads a map file into a session.
	 *
	 * @return An error message, or `nullptr` on success.
	 */
	const char *LoadMap(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	/**
	 * Looks for the map a session needs, in the maps folder, the downloaded
	 * maps and the folders below the maps folder, and loads the first that
	 * is the one wanted. A map the storage fetches rather than reads (see
	 * `IStorage::FetchUrl`) is fetched through the asset loader without
	 * waiting for it, and only when the digests the storage knows of it
	 * match; the search then goes on in `UpdateMapSearches`, and
	 * `OnMapSearchDone` hears how it ended.
	 *
	 * @return How the search ended, or `EMapSearch::FETCHING`.
	 */
	EMapSearch LoadMapSearch(CSessionId SessionId, const char *pMapName, const std::optional<SHA256_DIGEST> &WantedSha256, int WantedCrc);
	/**
	 * Goes on with the searches whose map was fetched. Called every frame.
	 */
	void UpdateMapSearches();
	/**
	 * Ends the search of a session without hearing of it, when the session
	 * stops or needs another map.
	 */
	void CancelMapSearch(CSessionId SessionId);
	bool IsMapSearchPending(CSessionId SessionId) const;
	/**
	 * Called when a search that fetched ended.
	 *
	 * @param SessionId The session the map was looked for.
	 * @param Loaded Whether the map was loaded; if not, it was not found.
	 */
	virtual void OnMapSearchDone(CSessionId SessionId, bool Loaded) {}
	/**
	 * The error a search for a map that was not found ends with.
	 */
	static const char *MapNotFoundError(const char *pMapName);

private:
	// A search for the map of a session, while its map is fetched.
	class CMapSearch
	{
	public:
		CSessionId m_SessionId;
		std::string m_Name;
		std::optional<SHA256_DIGEST> m_WantedSha256;
		unsigned m_WantedCrc = 0;
		// Where in the order the search goes on if the fetched map is not it.
		int m_NextStep = 0;
		std::string m_Path;
		CTypedAssetResource<CFileAssetJob> m_Resource;
	};
	std::vector<CMapSearch> m_vMapSearches;

	/**
	 * Readies a session for its new map. Plays the map another session has
	 * loaded already.
	 *
	 * @return Whether it plays a map another session loaded.
	 */
	bool BeginMapLoad(CSessionId SessionId, const char *pName, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	/**
	 * Loads a map that was read or fetched already, and checks that it is
	 * the one wanted.
	 */
	const char *LoadMapFile(CSessionId SessionId, const char *pName, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *LoadMapData(CSessionId SessionId, const char *pName, const char *pFilename, std::vector<uint8_t> &&vData, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	const char *CheckLoadedMap(CSessionId SessionId, const char *pFilename, const std::optional<SHA256_DIGEST> &WantedSha256, unsigned WantedCrc);
	/**
	 * Tries the places of the search from a step on, see `LoadMapSearch`.
	 */
	EMapSearch ContinueMapSearch(CMapSearch &Search);

public:
	IKernel *Kernel() { return IClient::Kernel(); }

	// A demo that is not exported runs on the clock of the client.
	float DemoPlaybackLocalTime(CSessionId SessionId) const override;

	EClientState State() const override;
	bool IsOnline() const override;
	bool IsDemoPlayback() const override;

	void AddWarning(const SWarning &Warning) override;
	std::optional<SWarning> CurrentWarning() override;
	std::vector<SWarning> &&QuittingWarnings() { return std::move(m_vQuittingWarnings); }

	void SetLoggers(std::shared_ptr<ILogger> &&pFileLogger, std::shared_ptr<ILogger> &&pStdoutLogger);
};

#endif
