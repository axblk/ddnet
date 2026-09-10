#ifndef GAME_CLIENT_COMPONENTS_LOCAL_SERVER_H
#define GAME_CLIENT_COMPONENTS_LOCAL_SERVER_H

#include <base/types.h>

#include <engine/shared/config.h>

#include <game/client/component.h>

class CLocalServer : public CComponentInterfaces
{
public:
	bool RunServer(const std::vector<const char *> &vpArguments);
	void KillServer();
	bool IsServerRunning();
	void RconAuthIfPossible();

private:
	// What it takes to talk to a server this program started. A program that
	// never starts one has nothing to remember about it, and a private field
	// nobody reads is an error where warnings are - see `local_server_null.cpp`.
#if !defined(CONF_DEMO_RENDER_TOOL) && !defined(CONF_DEMO_VIEWER_TOOL)
	char m_aRconPassword[sizeof(g_Config.m_SvRconPassword)] = "";

#if !defined(CONF_PLATFORM_ANDROID)
	PROCESS m_Process = INVALID_PROCESS;
#endif
#endif
};

#endif
