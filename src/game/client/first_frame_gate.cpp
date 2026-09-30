#include "first_frame_gate.h"

#include <base/log.h>
#include <base/time.h>

#include <engine/client/asset_loader.h>

#include <chrono>

using namespace std::chrono_literals;

namespace
{
	// Long enough for what a slow page fetches after the first frame, short
	// enough that the report still belongs to it.
	constexpr auto LATE_ASSETS_WINDOW = 15s;
} // namespace

const char *CFirstFrameGate::SceneName(EScene Scene)
{
	switch(Scene)
	{
	case EScene::MENUS: return "menus";
	case EScene::GAME: return "game";
	}
	return "?";
}

void CFirstFrameGate::Begin(EScene Scene, CSessionId SessionId, int64_t StartTime, CAssetLoader &Loader)
{
	Loader.EndLateReport();
	m_Scene = Scene;
	m_SessionId = SessionId;
	m_StartTime = StartTime;
}

void CFirstFrameGate::OnDrawn(EScene Scene, CSessionId SessionId, CAssetLoader &Loader)
{
	if(m_Scene != Scene || (Scene == EScene::GAME && SessionId != m_SessionId))
		return;
	m_Scene.reset();
	log_info("gameclient", "The %s was first drawn %.0f ms after it began to load", SceneName(Scene), (time_get() - m_StartTime) * 1000.0 / time_freq());
	Loader.ReportLateAssets(SceneName(Scene), LATE_ASSETS_WINDOW);
}
