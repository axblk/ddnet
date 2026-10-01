#include "first_frame_gate.h"

#include <base/log.h>
#include <base/str.h>
#include <base/time.h>

#include <engine/client/asset_loader.h>

using namespace std::chrono_literals;

namespace
{
	// Long enough for what a slow page fetches after the first frame, short
	// enough that the report still belongs to it.
	constexpr auto LATE_ASSETS_WINDOW = 15s;
} // namespace

void CCriticalAssets::Add(const char *pWhat, size_t Count)
{
	if(Count > 0)
		m_vEntries.emplace_back(pWhat, Count);
}

void CCriticalAssets::Prioritize(const CAssetResource &Resource)
{
	m_vpPrioritized.push_back(&Resource);
}

void CCriticalAssets::PrioritizeIn(CAssetLoader &Loader) const
{
	for(const CAssetResource *pResource : m_vpPrioritized)
		Loader.Prioritize(*pResource);
}

void CCriticalAssets::Describe(char *pBuffer, size_t BufferSize) const
{
	pBuffer[0] = '\0';
	for(const auto &[pWhat, Count] : m_vEntries)
	{
		char aEntry[128];
		str_format(aEntry, sizeof(aEntry), "%s%s (%d)", pBuffer[0] == '\0' ? "" : ", ", pWhat, (int)Count);
		str_append(pBuffer, aEntry, BufferSize);
	}
}

const char *CFirstFrameGate::SceneName(EScene Scene)
{
	switch(Scene)
	{
	case EScene::MENUS: return "menus";
	case EScene::GAME: return "game";
	}
	return "?";
}

bool CFirstFrameGate::IsScene(EScene Scene, CSessionId SessionId) const
{
	return m_Scene == Scene && (Scene != EScene::GAME || SessionId == m_SessionId);
}

void CFirstFrameGate::Begin(EScene Scene, CSessionId SessionId, int64_t StartTime, CAssetLoader &Loader)
{
	Loader.EndLateReport();
	m_Scene = Scene;
	m_SessionId = SessionId;
	m_StartTime = StartTime;
	m_WaitStart.reset();
	m_aWaitedFor[0] = '\0';
	m_TimedOut = false;
}

bool CFirstFrameGate::Waits(EScene Scene, CSessionId SessionId, const CCriticalAssets &Pending)
{
	if(!IsScene(Scene, SessionId) || m_TimedOut || Pending.Empty())
		return false;
	const int64_t Now = time_get();
	if(!m_WaitStart.has_value())
	{
		m_WaitStart = Now;
		Pending.Describe(m_aWaitedFor, sizeof(m_aWaitedFor));
	}
	if(Now - m_WaitStart.value() < TIMEOUT.count() * time_freq())
		return true;
	char aPending[512];
	Pending.Describe(aPending, sizeof(aPending));
	log_warn("gameclient", "The %s waited %d s for what it draws and goes on without: %s", SceneName(Scene), (int)TIMEOUT.count(), aPending);
	m_TimedOut = true;
	return false;
}

bool CFirstFrameGate::Hold(EScene Scene, CSessionId SessionId, const CCriticalAssets &Pending, CAssetLoader &Loader)
{
	if(!IsScene(Scene, SessionId))
		return false;
	if(Waits(Scene, SessionId, Pending))
		return true;
	m_Scene.reset();
	const int64_t Now = time_get();
	if(m_WaitStart.has_value())
		log_info("gameclient", "The %s was first drawn %.0f ms after it began to load, after waiting %.0f ms for: %s", SceneName(Scene), (Now - m_StartTime) * 1000.0 / time_freq(), (Now - m_WaitStart.value()) * 1000.0 / time_freq(), m_aWaitedFor);
	else
		log_info("gameclient", "The %s was first drawn %.0f ms after it began to load", SceneName(Scene), (Now - m_StartTime) * 1000.0 / time_freq());
	Loader.ReportLateAssets(SceneName(Scene), LATE_ASSETS_WINDOW);
	return false;
}
