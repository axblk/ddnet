/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
#define ENGINE_CLIENT_DEMO_RENDER_CLIENT_H

#include "demo_client_base.h"

#include <chrono>

/**
 * The client of the demo render tool: it encodes every frame of the demo the
 * command line named into a video file, as fast as the machine manages, into a
 * surface without a window.
 */
class CDemoRenderClient : public CDemoClientBase
{
	std::chrono::nanoseconds m_LastProgressLog{0};

	void OnExportFrame() override;

public:
	std::optional<int> ParseArguments(int &ArgumentCount, const char **&ppArguments, std::vector<const char *> &vArguments) override;
	bool Configure() override;
	int Run() override;
};

#endif // ENGINE_CLIENT_DEMO_RENDER_CLIENT_H
