/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
#define ENGINE_CLIENT_DEMO_RENDER_CLIENT_H

#include "demo_client_base.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

class CConfig;

/**
 * The client of the demo render tool: it encodes every frame of the demos the
 * command line named into video files, one after the other, as fast as the
 * machine manages, into a surface without a window.
 *
 * In a browser it can instead wait for a page to hand it demos, render each
 * one it is given and wait for the next, so that the page starts it once for
 * any number of them: see `--render-queue` and the `DemoRenderer*` functions.
 */
class CDemoRenderClient : public CDemoClientBase
{
	/**
	 * One demo and the video it goes into.
	 */
	struct SJob
	{
		std::string m_DemoPath;
		std::string m_VideoPath;
	};

	std::vector<SJob> m_vJobs;
	std::chrono::nanoseconds m_LastProgressLog{0};
	// Who the command line asked to follow, as it was written: a client id or
	// a name. Which of the two is settled once the demo is open.
	char m_aFollow[MAX_NAME_LENGTH] = "";
	// Set by an interrupt, which ends the demo being rendered and the rest.
	bool m_Interrupted = false;

	void OnExportFrame() override;
	/**
	 * Renders one demo, and leaves the program as it found it for the next.
	 *
	 * @return `false` when the video could not be made, which has been
	 * logged.
	 */
	bool RenderDemo(const SJob &Job);
	/**
	 * Closes the demo and the video of the last render and forgets what it
	 * left behind: its error and the sound still playing.
	 */
	void EndDemo();
	// Set when the page cancelled the demo being rendered.
	bool m_Cancelled = false;
	// Why the last render failed, empty when it did not.
	std::string m_LastError;

#if defined(CONF_WEB_PLATFORM)
public:
	/**
	 * What a page asks of a renderer it keeps: to render a demo, or to say
	 * what it is.
	 */
	struct SPageJob
	{
		int m_Id = 0;
		bool m_Render = true;
		std::string m_DemoPath;
		std::string m_VideoPath;
		// Whom to follow, as `--follow` takes it.
		std::string m_Follow;
		// Console commands, one per line, run on the configuration the
		// program started with.
		std::string m_Commands;
	};

private:
	bool m_Queue = false;
	std::vector<SPageJob> m_vPageJobs;
	int m_PageJobId = -1;
	// The configuration after the command line, which every job starts from.
	std::unique_ptr<CConfig> m_pBaseConfig;

	/**
	 * Takes what the page asked for since the last call: new jobs, and
	 * cancelling the one that runs or one that waits.
	 */
	void TakePageRequests();
	/**
	 * Waits for jobs from the page and does them, until the program quits.
	 */
	void RunQueue();
	void RunPageJob(const SPageJob &Job);

public:
	/**
	 * Queues a job of the page. Only from the program's thread.
	 */
	void AddPageJob(SPageJob &&Job) { m_vPageJobs.push_back(std::move(Job)); }
	/**
	 * Cancels a job of the page, running or waiting. Only from the program's
	 * thread.
	 */
	void CancelPageJob(int Id);
#endif

public:
	std::optional<int> ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments) override;
	bool Configure() override;
	int Run() override;
};

#endif // ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
