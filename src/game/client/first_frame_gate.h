#ifndef GAME_CLIENT_FIRST_FRAME_GATE_H
#define GAME_CLIENT_FIRST_FRAME_GATE_H

#include <engine/client/session.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

class CAssetLoader;
class CAssetResource;

/**
 * What a scene cannot be drawn without and is still loading, collected from
 * the components before its first frame (see
 * `CComponent::OnCollectCriticalAssets`).
 */
class CCriticalAssets
{
	std::vector<std::pair<const char *, size_t>> m_vEntries;
	std::vector<const CAssetResource *> m_vpPrioritized;

public:
	/**
	 * @param pWhat What loads, a string that outlives the collection.
	 * @param Count How many of it.
	 */
	void Add(const char *pWhat, size_t Count = 1);
	/**
	 * Names a load of what the scene waits for, which the loader then makes
	 * urgent, see `CAssetLoader::Prioritize`: it was started early, when
	 * nobody waited for it yet. Counted by `Add`, not here.
	 *
	 * @param Resource The load, which outlives the collection.
	 */
	void Prioritize(const CAssetResource &Resource);
	/**
	 * Makes the loads named by `Prioritize` urgent.
	 */
	void PrioritizeIn(CAssetLoader &Loader) const;
	bool Empty() const { return m_vEntries.empty(); }
	/**
	 * Lists what loads, for the loading screen and the log.
	 */
	void Describe(char *pBuffer, size_t BufferSize) const;
};

/**
 * The moments at which the game begins to show something new: the menus
 * once the client has started, and the game once a server was joined or a
 * demo was opened. Holds each on the loading screen until what it cannot be
 * drawn without is there, so that nothing pops in, and gives up after
 * `TIMEOUT`, which is logged with what is missing. Notes when each began to
 * load and when it was first drawn, and has the asset loader report what
 * arrived after that.
 */
class CFirstFrameGate
{
public:
	enum class EScene
	{
		MENUS,
		GAME,
	};

	/**
	 * How long a scene waits at most, from when it could first be drawn.
	 */
	static constexpr std::chrono::seconds TIMEOUT{10};

	/**
	 * A scene begins to load. Replaces one that was not drawn yet.
	 *
	 * @param Scene What begins to load.
	 * @param SessionId The session the game is of, invalid for the menus.
	 * @param StartTime When it began, see `time_get`.
	 * @param Loader The loader, whose report of what arrived late after the
	 * scene before ends here.
	 */
	void Begin(EScene Scene, CSessionId SessionId, int64_t StartTime, CAssetLoader &Loader);
	/**
	 * Whether a scene still waits for what it cannot be drawn without. Only
	 * the scene that began waits, and for `TIMEOUT` at most, counted from the
	 * first time it is asked.
	 *
	 * @param Scene The scene.
	 * @param SessionId The session the game is of, invalid for the menus.
	 * @param Pending What the scene cannot be drawn without and still loads.
	 */
	bool Waits(EScene Scene, CSessionId SessionId, const CCriticalAssets &Pending);
	/**
	 * Called for every frame that would draw a scene: whether to draw the
	 * loading screen instead, see `Waits`. The first frame that is drawn is
	 * logged, and the loader reports the assets that finish after it.
	 *
	 * @param Scene The scene.
	 * @param SessionId The session the game is of, invalid for the menus.
	 * @param Pending What the scene cannot be drawn without and still loads.
	 * @param Loader The loader whose assets the scene draws.
	 */
	bool Hold(EScene Scene, CSessionId SessionId, const CCriticalAssets &Pending, CAssetLoader &Loader);

	/**
	 * Whether a scene began and was not drawn yet.
	 */
	bool IsScene(EScene Scene, CSessionId SessionId) const;

	static const char *SceneName(EScene Scene);

private:
	std::optional<EScene> m_Scene;
	CSessionId m_SessionId;
	int64_t m_StartTime = 0;
	// When the scene was first asked for, `TIMEOUT` counts from there.
	std::optional<int64_t> m_WaitStart;
	// What the scene waited for when it was first asked, for the log.
	char m_aWaitedFor[256] = "";
	bool m_TimedOut = false;
};

#endif
