/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#if defined(CONF_DEMO_RENDER_TOOL)

#include "local_server.h"

// A tool that reads one demo from disk never starts a server of its own.

bool CLocalServer::RunServer(const std::vector<const char *> &vpArguments)
{
	return false;
}

void CLocalServer::KillServer() {}

bool CLocalServer::IsServerRunning()
{
	return false;
}

void CLocalServer::RconAuthIfPossible() {}

#endif
