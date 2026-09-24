/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
#define ENGINE_CLIENT_DEMO_RENDER_CLIENT_H

#include "demo_client_base.h"

#include <chrono>
#include <string>
#include <vector>

/**
 * The client of the demo render tool: it encodes every frame of the demos the
 * command line named into video files, one after the other, as fast as the
 * machine manages, into a surface without a window.
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

public:
	std::optional<int> ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments) override;
	bool Configure() override;
	int Run() override;
};

#endif // ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
