/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
#define ENGINE_CLIENT_DEMO_RENDER_CLIENT_H

#include "demo_client_base.h"

#include <chrono>

/**
 * The client of the demo render tool: it opens the one demo the command line
 * named and encodes every frame of it into a video file, as fast as the
 * machine manages and with nobody watching.
 *
 * What it is made of is `CDemoClientBase`; what it adds is the surface without
 * a window, the loop that runs the export to the end and the progress it
 * writes to the log while it does.
 */
class CDemoRenderClient : public CDemoClientBase
{
	int m_ExitCode = 0;
	std::chrono::nanoseconds m_LastProgressLog{0};

	void OnExportFrame() override;

public:
	/**
	 * Takes over what the command line asked for.
	 *
	 * @return `false` when the output file cannot be written, which has already
	 * been logged.
	 */
	bool Configure(const CCommandLineVideoExport &Export);
	void Run();
	int ExitCode() const { return m_ExitCode; }
};

#endif // ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
