#ifndef GAME_CLIENT_FIRST_FRAME_GATE_H
#define GAME_CLIENT_FIRST_FRAME_GATE_H

#include <engine/client/session.h>

#include <cstdint>
#include <optional>

class CAssetLoader;

/**
 * The moments at which the game begins to show something new: the menus
 * once the client has started, and the game once a server was joined or a
 * demo was opened. Notes when each began to load and when it was first
 * drawn, and has the asset loader report what arrived after that, which is
 * what can pop in.
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
	 * Called for every frame that draws a scene. The first one after `Begin`
	 * is logged, and the loader reports the assets that finish after it.
	 *
	 * @param Scene What was drawn.
	 * @param SessionId The session the game is of, invalid for the menus.
	 * @param Loader The loader whose assets the scene draws.
	 */
	void OnDrawn(EScene Scene, CSessionId SessionId, CAssetLoader &Loader);

	static const char *SceneName(EScene Scene);

private:
	std::optional<EScene> m_Scene;
	CSessionId m_SessionId;
	int64_t m_StartTime = 0;
};

#endif
