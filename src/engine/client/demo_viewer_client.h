/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_VIEWER_CLIENT_H
#define ENGINE_CLIENT_DEMO_VIEWER_CLIENT_H

#include "demo_client_base.h"

#include <chrono>

class IEngineInput;

/**
 * The client of the demo viewer: it shows one demo in a window, in real time.
 * Space pauses, the left and right arrows seek, up and down change the speed,
 * Home starts over and Escape closes the window.
 */
class CDemoViewerClient : public CDemoClientBase
{
	IEngineInput *m_pInput = nullptr;
	bool m_Surfaceless = false;
	std::chrono::nanoseconds m_NextFrameTime{0};

	/**
	 * Answers the keys that steer the playback.
	 *
	 * @return `false` when the window was closed.
	 */
	bool HandleInput();
	void RenderWindowFrame();

public:
	std::optional<int> ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments) override;
	bool Configure() override;
	int Run() override;

	void UpdateAndSwap() override;

	// What the controls of a page around the canvas call in the browser.
	void SetPaused(bool Paused);
	void SeekPercent(float Percent);
	void SeekTime(float Seconds);
	void SetSpeed(float Speed);
	bool Paused() const;
	float Progress() const;
	float Speed() const;
	float Length() const;
};

#endif // ENGINE_CLIENT_DEMO_VIEWER_CLIENT_H
